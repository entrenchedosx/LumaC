/* Phase 33 Luma Editor desktop app (real windowed application).
 *
 * Real windowed app over the PUBLIC stacks only:
 *   GUI (leg_* in luma_editor.h, C++ ImGui behind the ABI)
 *   -> EditorCore (led_*) -> Engine (le_*) -> Renderer (lr_*)
 *   -> LumaC (lc_*).
 * The host owns every lifetime (window/device/surface/swapchain/
 * renderer/engine/world/session/GUI context); leg_* borrows them.
 *
 * Frame (matches engine_scene/main.c + the GPU proof discipline):
 *   lc_poll_events -> leg_frame_begin (drains the lc queue ONCE into
 *     ImGui io; the engine has NO attached window so nothing
 *     double-consumes — gameplay input during Play is injected, see
 *     below) -> leg_panels_frame (menu/toolbar/viewport/hierarchy/
 *     inspector/assets/console/status) -> leg_frame_end ->
 *     lc_begin_frame -> scene composite into the viewport target
 *     (leg_viewport_render: engine trio on the frame encoder, NO open
 *     pass) -> swapchain pass: scene blit? No — the viewport target
 *     is sampled BY the panels (ImGui::Image over the registered
 *     TexID, one frame latency); the swapchain pass records ONLY the
 *     GUI walk (leg_record_gui: blended pipeline, per-draw scissor)
 *     -> lc_end_frame (presents).
 *
 * Play input routing (no leaks): the engine NEVER attaches the OS
 * window (le_engine_attach_window is NOT called). While playing, the
 * viewport hover + raw ImGui key/mouse state drive
 * le_input_inject_* (keys WASD/arrows/space, mouse deltas, scroll)
 * into the engine before led_play_tick — text fields never leak
 * keystrokes into gameplay (leg_wants_keyboard gates injection),
 * and the GUI queue stays the single drainer.
 *
 * Authoring workflow (spec): open project -> browse assets ->
 * open/create scene -> create/select -> edit properties -> TRS
 * gizmos -> drag assets -> create/use prefabs -> undo/redo -> save
 * -> play -> interact -> stop -> continue editing unchanged.
 * Every step is a GUI panel over led_* (see editor/src/gui/).
 *
 * Usage: luma_editor [--project DIR] [--scene FILE] [--frames N]
 *   [--no-validation] [--no-vsync] [--screenshot out.png]
 *   [--import-all]
 *   --frames 0 (default) runs until the window closes; N > 0 runs
 *   N frames (CI/screenshot discipline, like engine_scene).
 *   --import-all imports every UNIMPORTED record via led_import_asset
 *   (the exact function the Assets panel Import button calls) after
 *   project open, before scene open: batch/CI project warmup so a
 *   fresh process can open mesh-bearing scenes without manual
 *   per-asset clicks. Interactive sessions keep the lazy contract
 *   (open never uploads GPU resources unless --import-all is given).
 * No absolute paths are stored: project/scene CLI values resolve at
 * startup only; the session remembers the scene path as given.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_engine/luma_engine.h>
#include <luma_editor/luma_editor.h>

#include "png_mini.h"

#define FAIL_CLEANUP(what) do { \
    fprintf(stderr, "%s failed\n", what); \
    goto cleanup; \
} while (0)

/* Seconds between lc_clock ticks (monotonic ns). */
static float app_delta(uint64_t *last) {
    uint64_t now = lc_clock_now();
    uint64_t freq = lc_clock_frequency();
    double dt = 0.0;

    if (freq == 0) {
        freq = 1000000000ull;
    }
    if (*last != 0) {
        dt = (double)(now - *last) / (double)freq;
    }
    *last = now;
    if (!(dt > 0.0)) {
        dt = 1.0 / 60.0;
    }
    if (dt > 0.25) {
        dt = 0.25;
    }
    return (float)dt;
}

/* Inject viewport play input into the engine (called while playing,
 * before led_play_tick; skipped when the GUI wants the keyboard).
 * Phase 34A: real per-key injection through leg_consume_play_input
 * (ImGui key state -> le_input_inject_* -> led_play_tick folds the
 * pending list WITHOUT the OS pump — deterministic, same state
 * machine scripts read). The engine never attaches the OS window;
 * the GUI queue stays the single drainer. */
static void app_inject_play_input(leg_context *gui,
                                  le_engine *engine) {
    if (gui == NULL || engine == NULL) {
        return;
    }
    leg_consume_play_input(gui, engine);
}

int main(int argc, char **argv) {
    const char *project_dir = NULL;
    const char *scene_file = NULL;
    const char *screenshot_path = NULL;
    const char *select_name = NULL;
    const char *size_arg = NULL;
    int play_at_start = 0;
    int want_report = 0;
    unsigned long max_frames = 0;
    int use_validation = 1;
    int use_vsync = 1;
    int import_all = 0;
    unsigned long i = 0;
    lc_window_desc window_desc;
    lc_window *window = NULL;
    lc_device_desc device_desc;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain_desc swapchain_desc;
    lc_swapchain *swapchain = NULL;
    lr_renderer_desc rdesc;
    lr_renderer *renderer = NULL;
    le_engine *engine = NULL;
    le_world *world = NULL;
    led_session *session = NULL;
    leg_context *gui = NULL;
    led_viewport vp;
    uint64_t last_tick = 0;
    unsigned long frame = 0;
    int ok = 1;
    /* Screenshot staging (like engine_scene: offscreen target +
     * readback on the last frame). */
    lc_image *shot_img = NULL;
    lc_image_view *shot_view = NULL;
    lc_render_target *shot_target = NULL;
    uint32_t shot_w = 0;
    uint32_t shot_h = 0;

    for (i = 1; i < (unsigned long)argc; i++) {
        if (strcmp(argv[i], "--project") == 0 && i + 1 < argc) {
            project_dir = argv[++i];
        } else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            scene_file = argv[++i];
        } else if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--no-validation") == 0) {
            use_validation = 0;
        } else if (strcmp(argv[i], "--no-vsync") == 0) {
            use_vsync = 0;
        } else if (strcmp(argv[i], "--screenshot") == 0 &&
                   i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--import-all") == 0) {
            import_all = 1;
        } else if (strcmp(argv[i], "--select") == 0 &&
                   i + 1 < argc) {
            /* Automation/verification selection (same
             * led_selection_set the Hierarchy click path calls). */
            select_name = argv[++i];
        } else if (strcmp(argv[i], "--size") == 0 &&
                   i + 1 < argc) {
            /* Verification window size (e.g. 1920x1080, 1280x720). */
            size_arg = argv[++i];
        } else if (strcmp(argv[i], "--play") == 0) {
            /* Enter Play after setup (same led_play_enter the Play
             * button calls; Play/Stop isolation verified headed). */
            play_at_start = 1;
        } else if (strcmp(argv[i], "--report") == 0) {
            /* Recovery CI: print the last render submission report
             * (submitted/skipped + renderer draws/tris) on the last
             * frame. Production le_world_get_last_render_report;
             * observe-only. */
            want_report = 1;
        } else {
            fprintf(stderr,
                    "usage: luma_editor [--project DIR] "
                    "[--scene FILE] [--frames N] [--no-validation] "
                    "[--no-vsync] [--screenshot out.png] "
                    "[--import-all] [--select NAME] [--size WxH] "
                    "[--play] [--report]\n");
            return 1;
        }
    }
    memset(&window_desc, 0, sizeof(window_desc));
    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }
    window_desc.title = "Luma Editor (Phase 33)";
    window_desc.width = 1280;
    window_desc.height = 800;
    if (size_arg != NULL) {
        unsigned long sw = 0;
        unsigned long sh = 0;

        if (sscanf(size_arg, "%lux%lu", &sw, &sh) == 2 && sw >= 640 &&
            sh >= 480 && sw <= 3840 && sh <= 2160) {
            window_desc.width = (uint32_t)sw;
            window_desc.height = (uint32_t)sh;
        } else {
            fprintf(stderr, "warning: bad --size '%s' (want WxH)\n",
                    size_arg);
        }
    }
    if (lc_window_create(&window_desc, &window) != LC_SUCCESS) {
        fprintf(stderr, "lc_window_create failed\n");
        lc_shutdown();
        return 1;
    }
    memset(&device_desc, 0, sizeof(device_desc));
    device_desc.backend = LC_BACKEND_VULKAN;
    device_desc.enable_validation = use_validation;
    if (lc_device_create(&device_desc, &device) != LC_SUCCESS) {
        FAIL_CLEANUP("lc_device_create");
    }
    if (lc_surface_create(device, window, &surface) != LC_SUCCESS) {
        FAIL_CLEANUP("lc_surface_create");
    }
    memset(&swapchain_desc, 0, sizeof(swapchain_desc));
    swapchain_desc.width = lc_window_get_width(window);
    swapchain_desc.height = lc_window_get_height(window);
    swapchain_desc.image_count = 0;
    swapchain_desc.vsync = use_vsync;
    if (lc_swapchain_create(device, surface, &swapchain_desc,
                            &swapchain) != LC_SUCCESS) {
        FAIL_CLEANUP("lc_swapchain_create");
    }
    memset(&rdesc, 0, sizeof(rdesc));
    rdesc.device = device;
    rdesc.render_target.color_attachment_count = 1;
    rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
    rdesc.max_objects = 4096;
    rdesc.ambient_light[0] = 0.35f;
    rdesc.ambient_light[1] = 0.35f;
    rdesc.ambient_light[2] = 0.4f;
    if (lr_renderer_create(&rdesc, &renderer) != LR_SUCCESS) {
        FAIL_CLEANUP("lr_renderer_create");
    }
    {
        le_engine_desc edesc;
        le_world_desc wdesc;

        memset(&edesc, 0, sizeof(edesc));
        edesc.renderer = renderer;
        if (le_engine_create(&edesc, &engine) != LE_SUCCESS) {
            FAIL_CLEANUP("le_engine_create");
        }
        /* NOTE: no le_engine_attach_window — the GUI owns the lc
         * queue drain (leg_frame_begin). See app_inject_play_input
         * for the Play input contract. */
        memset(&wdesc, 0, sizeof(wdesc));
        if (le_world_create(engine, &wdesc, &world) != LE_SUCCESS) {
            FAIL_CLEANUP("le_world_create");
        }
    }
    if (led_session_create(&session) != LED_SUCCESS) {
        FAIL_CLEANUP("led_session_create");
    }
    if (led_session_attach(session, engine, world) != LED_SUCCESS) {
        FAIL_CLEANUP("led_session_attach");
    }
    if (leg_context_create(session, device, &gui) != LED_SUCCESS) {
        FAIL_CLEANUP("leg_context_create");
    }
    /* Layout INI beside the process (never beside scenes/prefabs). */
    leg_set_ini_path(gui, NULL);
    led_viewport_default(&vp);

    /* Open project + scene when requested (CLI only; no stored
     * absolute paths beyond the session's remembered scene path). */
    if (project_dir != NULL) {
        if (led_project_open(session, project_dir) != LED_SUCCESS) {
            fprintf(stderr, "warning: project open failed (%s)\n",
                    project_dir);
        } else if (import_all) {
            /* Batch warmup: import every record that is not READY
             * through the same led_import_asset the GUI Import
             * button calls (UNIMPORTED + STALE: a fresh process has
             * an empty engine registry, so STALE records — new
             * importer version, changed source — must also upload
             * before scene open; a single bad asset never aborts
             * the batch — mirrors the panel's per-click errors).
             * Recovery fix: the old code swept UNIMPORTED only, so
             * a project whose sidecars all read STALE (e.g. after
             * an importer bump) imported NOTHING and every
             * mesh-bearing scene failed to open. */
            uint32_t n = led_assetdb_count(session);
            uint32_t k = 0;
            uint32_t ok_imports = 0;

            for (k = 0; k < n; k++) {
                led_asset_record rec;

                memset(&rec, 0, sizeof(rec));
                if (!led_assetdb_get(session, k, &rec)) {
                    continue;
                }
                /* Recovery: warmup must cover UNIMPORTED too. A
                 * fresh process has an empty engine registry, so
                 * sidecar-READY records arrive with dead handles
                 * and the scan demotes them to UNIMPORTED (identity
                 * kept) — they still need uploading before scene
                 * open. STALE (drift) likewise. */
                if (rec.status != LED_IMPORT_UNIMPORTED &&
                    rec.status != LED_IMPORT_STALE &&
                    rec.status != LED_IMPORT_READY) {
                    continue;
                }
                if (led_import_asset(session, rec.source_path) ==
                    LED_SUCCESS) {
                    ok_imports++;
                } else {
                    fprintf(stderr,
                            "warning: import failed (%s)\n",
                            rec.source_path);
                }
            }
            printf("import-all: %u ok\n", ok_imports);
        }
    }
    if (scene_file != NULL) {
        if (led_scene_open(session, scene_file) != LED_SUCCESS) {
            fprintf(stderr, "warning: scene open failed (%s)\n",
                    scene_file);
        }
    } else {
        /* Seed a starter scene (camera + sun + ground cue) so the
         * first frame is never empty — all through led_execute so
         * undo works from frame one. */
        le_object rig = LE_OBJECT_INVALID;
        le_object cam = LE_OBJECT_INVALID;
        le_object sun = LE_OBJECT_INVALID;
        led_command cmd;

        if (le_object_create(world, &rig) == LE_SUCCESS) {
            le_object_set_name(world, &rig, "CameraRig");
            {
                float p[3] = { 0.0f, 4.0f, 10.0f };

                le_object_set_position(world, &rig, p);
            }
            if (le_object_create(world, &cam) == LE_SUCCESS) {
                le_camera_desc cd;

                le_object_set_name(world, &cam, "Camera");
                le_object_set_parent(world, &cam, &rig);
                le_camera_desc_default(&cd);
                le_object_add_camera(world, &cam, &cd);
                le_world_set_active_camera(world, &cam);
            }
        }
        if (le_object_create(world, &sun) == LE_SUCCESS) {
            le_light_desc ld;

            le_object_set_name(world, &sun, "Sun");
            memset(&ld, 0, sizeof(ld));
            ld.type = LE_LIGHT_DIRECTIONAL;
            ld.color[0] = ld.color[1] = ld.color[2] = 1.0f;
            ld.intensity = 3.0f;
            le_object_add_light(world, &sun, &ld);
        }
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = LED_CMD_CREATE;
        snprintf(cmd.label, sizeof(cmd.label), "Seed");
        strncpy(cmd.name_value, "Ground",
                sizeof(cmd.name_value) - 1);
        led_execute(session, &cmd);
        led_history_clear(session);
    }
    if (select_name != NULL) {
        le_object found = LE_OBJECT_INVALID;

        if (le_world_find_by_name(world, select_name, &found)) {
            led_selection_set(session, &found, 1);
            printf("selected '%s'\n", select_name);
        } else {
            fprintf(stderr, "warning: select target '%s' not found\n",
                    select_name);
        }
    }

    printf("Luma Editor (Phase 33): %s | %s\n", lc_get_version_string(),
           led_project_is_open(session) ? "project open"
                                        : "no project");
    printf("Author: open project -> browse -> scene -> create -> "
           "gizmo -> prefab -> undo -> save -> play -> stop.\n");
    if (play_at_start) {
        if (led_play_enter(session) == LED_SUCCESS) {
            printf("playing (via --play)\n");
        } else {
            fprintf(stderr, "warning: --play enter failed\n");
        }
    }

    for (frame = 0; max_frames == 0 || frame < max_frames; frame++) {
        leg_frame_input in;
        uint32_t w = 0;
        uint32_t h = 0;
        float dt = 0.0f;
        lc_command_encoder *enc = NULL;
        lc_result brc;

        if (lc_window_should_close(window)) {
            break;
        }
        lc_poll_events();
        w = lc_window_get_width(window);
        h = lc_window_get_height(window);
        if (w == 0 || h == 0) {
            continue; /* minimized: keep pumping, skip frames */
        }
        if (w != lc_swapchain_get_width(swapchain) ||
            h != lc_swapchain_get_height(swapchain)) {
            lc_result rrc = lc_swapchain_recreate(swapchain, w, h);

            if (rrc == LC_ERROR_ZERO_EXTENT) {
                continue;
            }
            if (rrc != LC_SUCCESS) {
                fprintf(stderr, "lc_swapchain_recreate failed (%d)\n",
                        (int)rrc);
                goto cleanup;
            }
        }
        dt = app_delta(&last_tick);
        /* Editor housekeeping (prunes stale selection; edit scripts
         * never run here) + console script-error mirror happens in
         * the console panel. */
        led_session_tick(session, dt);
        /* Play input + tick ordering (Phase 34A): the panels frame
         * (below) drains the lc queue into ImGui key state; per-key
         * gameplay injection runs AFTER panels and BEFORE the tick
         * so scripts observe this frame's keys. The pre-frame call
         * only covers keys held across frames (ImGui state from
         * the PREVIOUS panels frame — still correct: held stays
         * held); the post-panels call below covers fresh presses.
         * Both funnel through leg_consume_play_input (text fields
         * never leak). */
        if (led_is_playing(session)) {
            app_inject_play_input(gui, engine);
            if (!led_play_is_paused(session)) {
                if (led_play_tick(session, dt) != LED_SUCCESS) {
                    fprintf(stderr, "play tick failed; stopping\n");
                    led_play_exit(session);
                }
            }
        }
        /* Swapchain frame FIRST (the scene composite needs the frame
         * encoder with no open pass; the GUI walk needs the open
         * swapchain pass later). */
        brc = lc_begin_frame(swapchain);
        if (brc == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            if (lc_swapchain_recreate(swapchain, w, h) != LC_SUCCESS) {
                FAIL_CLEANUP("swapchain recreate");
            }
            continue;
        }
        if (brc != LC_SUCCESS) {
            FAIL_CLEANUP("lc_begin_frame");
        }
        if (lc_swapchain_get_encoder(swapchain, &enc) != LC_SUCCESS) {
            FAIL_CLEANUP("frame encoder");
        }
        /* Scene composite into the viewport target (engine trio, no
         * open pass) — BEFORE panels so the panel Image() samples a
         * composited set on the SAME frame (no one-frame latency, no
         * stale-TexID first frame). Panel extent is unknown before
         * the first panels frame, so frame 0 composites at a window
         * fraction; later frames use the panel extent the viewport
         * panel wrote into vp. */
        {
            uint32_t vpw = (vp.width > 0) ? vp.width
                          : (w > 320) ? w - 320 : 640;
            uint32_t vph = (vp.height > 0) ? vp.height
                          : (h > 120) ? h - 120 : 480;
            struct leg_viewport_target *vt =
                leg_viewport_target_for(gui);
            unsigned long long vtex = leg_viewport_composite(
                gui, vt, enc, vpw, vph);

            (void)vtex;
        }
        /* GUI frame (drains lc queue -> panels -> draw data). */
        memset(&in, 0, sizeof(in));
        in.window_width = w;
        in.window_height = h;
        in.delta_seconds = dt;
        in.window_focused = 1;
        if (leg_frame_begin(gui, window, &in) != LED_SUCCESS) {
            FAIL_CLEANUP("leg_frame_begin");
        }
        leg_panels_frame(gui, &vp, dt);
        if (leg_frame_end(gui) != LED_SUCCESS) {
            FAIL_CLEANUP("leg_frame_end");
        }
        /* Optional screenshot staging on the last frame (offscreen
         * target + readback after end_frame, like engine_scene). */
        if (screenshot_path != NULL &&
            ((max_frames > 0 && frame + 1u == max_frames) ||
             (max_frames == 0 && frame == 0))) {
            lc_image_desc idesc;
            lc_image_view_desc svdesc;
            lc_render_target_attachment ratt;
            lc_render_target_create_desc rtdesc;

            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = w;
            idesc.height = h;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                          LC_IMAGE_USAGE_SAMPLED |
                          LC_IMAGE_USAGE_TRANSFER_SRC |
                          LC_IMAGE_USAGE_TRANSFER_DST;
            idesc.samples = LC_SAMPLE_COUNT_1;
            if (lc_image_create(device, &idesc, &shot_img) !=
                LC_SUCCESS) {
                FAIL_CLEANUP("screenshot target");
            }
            memset(&svdesc, 0, sizeof(svdesc));
            svdesc.type = LC_IMAGE_VIEW_2D;
            svdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            svdesc.mip_level_count = 1;
            svdesc.array_layer_count = 1;
            if (lc_image_view_create(shot_img, &svdesc, &shot_view) !=
                LC_SUCCESS) {
                FAIL_CLEANUP("screenshot target");
            }
            memset(&rtdesc, 0, sizeof(rtdesc));
            ratt.view = shot_view;
            rtdesc.color_attachments = &ratt;
            rtdesc.color_attachment_count = 1;
            rtdesc.width = w;
            rtdesc.height = h;
            if (lc_render_target_create(device, &rtdesc,
                                        &shot_target) != LC_SUCCESS) {
                FAIL_CLEANUP("screenshot target");
            }
            shot_w = w;
            shot_h = h;
        }
        /* Swapchain pass: GUI draws only (scene is sampled by the
         * panels as a texture; the swapchain never carries the 3D
         * scene directly — viewport-composite discipline).
         * Depth: the GUI pipeline signature carries the swapchain
         * depth format, so the pass MUST name the same depth (LOAD
         * CLEAR or DONT_CARE both keep the signature; only the pass
         * SHAPE (color+depth+samples) matters for compat). DONT_CARE
         * is correct + cheapest (GUI never depth-tests). */
        {
            lc_render_swapchain_pass_desc spass;

            memset(&spass, 0, sizeof(spass));
            spass.color_load_op = LC_LOAD_OP_CLEAR;
            spass.color_store_op = LC_STORE_OP_STORE;
            spass.depth_load_op = LC_LOAD_OP_DONT_CARE;
            spass.depth_store_op = LC_STORE_OP_DONT_CARE;
            spass.clear_depth = 1.0f;
            if (lc_encoder_begin_swapchain_pass(enc, swapchain,
                                                &spass) !=
                LC_SUCCESS) {
                FAIL_CLEANUP("swapchain pass");
            }
            {
                lc_format fmt =
                    lc_swapchain_get_format(swapchain);
                lc_format dfmt =
                    lc_swapchain_get_depth_format(swapchain);
                led_result rrc = leg_record_gui(
                    gui, enc, fmt, dfmt);

                if (rrc != LED_SUCCESS) {
                    /* GUI failure keeps the frame alive: end the
                     * pass and present without GUI (spec). Step 8
                     * (bind pipeline) after a swapchain recreate =
                     * format drift: the GUI rebuilds its pipeline
                     * next frame (leg_gpu_ensure is per-format). */
                    fprintf(stderr,
                            "warning: GUI record failed "
                            "(rc %d, step %d, tex %llu, fmt %d, dfmt %d); "
                            "presenting without GUI\n",
                            (int)rrc, leg_record_step_last(),
                            leg_record_tex_last(), (int)fmt,
                            (int)dfmt);
                }
            }
            if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                FAIL_CLEANUP("swapchain pass end");
            }
        }
        /* Screenshot: re-record the SAME GUI draw data into the
         * offscreen shot target (explicit target pass on the frame
         * encoder, after the swapchain pass closes — sequential
         * passes share one encoder). The shot target is RGBA8 with
         * no depth, so the GUI rebuilds its pipeline for that
         * signature (per-format discipline). */
        if (screenshot_path != NULL && shot_target != NULL) {
            lc_render_color_attachment shot_catt;
            lc_render_pass_desc shot_pass;
            led_result shot_rc;

            memset(&shot_catt, 0, sizeof(shot_catt));
            shot_catt.view = shot_view;
            shot_catt.load_op = LC_LOAD_OP_CLEAR;
            shot_catt.store_op = LC_STORE_OP_STORE;
            memset(&shot_pass, 0, sizeof(shot_pass));
            shot_pass.color_attachments = &shot_catt;
            shot_pass.color_attachment_count = 1;
            shot_pass.width = shot_w;
            shot_pass.height = shot_h;
            if (lc_encoder_begin_render_pass(enc, &shot_pass) !=
                LC_SUCCESS) {
                FAIL_CLEANUP("screenshot pass");
            }
            shot_rc = leg_record_gui(gui, enc,
                                     LC_FORMAT_RGBA8_UNORM,
                                     LC_FORMAT_UNDEFINED);
            if (shot_rc != LED_SUCCESS) {
                fprintf(stderr,
                        "warning: screenshot record failed "
                        "(rc %d, step %d, tex %llu); shot may be blank\n",
                        (int)shot_rc, leg_record_step_last(),
                        leg_record_tex_last());
            }
            if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                FAIL_CLEANUP("screenshot pass end");
            }
        }
        if (lc_end_frame(swapchain) != LC_SUCCESS) {
            /* suboptimal tolerated; keep running */
        }
        if (screenshot_path != NULL && shot_target != NULL &&
            ((max_frames > 0 && frame + 1u == max_frames) ||
             (max_frames == 0 && frame == 0))) {
            lc_image_readback_desc rbdesc;
            lc_image_readback_info rbinfo;
            unsigned char *rgba = NULL;
            unsigned char *rgb = NULL;
            uint32_t x = 0;
            uint32_t y = 0;

            memset(&rbdesc, 0, sizeof(rbdesc));
            if (lc_image_query_readback(shot_img, &rbdesc, &rbinfo) !=
                LC_SUCCESS) {
                FAIL_CLEANUP("screenshot query");
            }
            rgba = (unsigned char *)malloc(rbinfo.size);
            rgb = (unsigned char *)malloc((size_t)shot_w * shot_h *
                                          3u);
            if (rgba == NULL || rgb == NULL) {
                free(rgba);
                free(rgb);
                FAIL_CLEANUP("screenshot memory");
            }
            if (lc_image_readback(shot_img, &rbdesc, rgba,
                                  rbinfo.size, NULL) != LC_SUCCESS) {
                free(rgba);
                free(rgb);
                FAIL_CLEANUP("screenshot readback");
            }
            for (y = 0; y < shot_h; y++) {
                for (x = 0; x < shot_w; x++) {
                    rgb[((size_t)y * shot_w + x) * 3u + 0] =
                        rgba[((size_t)y * shot_w + x) * 4u + 0];
                    rgb[((size_t)y * shot_w + x) * 3u + 1] =
                        rgba[((size_t)y * shot_w + x) * 4u + 1];
                    rgb[((size_t)y * shot_w + x) * 3u + 2] =
                        rgba[((size_t)y * shot_w + x) * 4u + 2];
                }
            }
            free(rgba);
            if (lpng_write(screenshot_path, shot_w, shot_h, 3, rgb,
                           (size_t)shot_w * 3u) != 0) {
                free(rgb);
                FAIL_CLEANUP("screenshot write");
            }
            free(rgb);
            printf("screenshot wrote %s (%ux%u)\n", screenshot_path,
                   shot_w, shot_h);
            lc_render_target_destroy(shot_target);
            lc_image_view_destroy(shot_view);
            lc_image_destroy(shot_img);
            shot_target = NULL;
            shot_view = NULL;
            shot_img = NULL;
        }
        /* Recovery CI report: what did the composite actually
         * submit this frame (edit world, or play world under
         * --play)? */
        if (want_report &&
            ((max_frames > 0 && frame + 1u == max_frames) ||
             (max_frames == 0 && frame == 0))) {
            le_world *rw = led_is_playing(session)
                               ? led_play_get_world(session)
                               : led_session_get_edit_world(session);
            le_render_report rep;

            memset(&rep, 0, sizeof(rep));
            le_world_get_last_render_report(rw, &rep);
            printf("report submitted=%u dis=%u inv=%u dead=%u "
                   "draws=%u tris=%u objects=%u\n",
                   (unsigned)rep.submitted,
                   (unsigned)rep.skipped_disabled,
                   (unsigned)rep.skipped_invisible,
                   (unsigned)rep.skipped_dead,
                   (unsigned)rep.renderer_stats.draw_calls,
                   (unsigned)rep.renderer_stats.triangles,
                   (unsigned)le_world_get_object_count(rw));
        }
    }
cleanup:
    lc_render_target_destroy(shot_target);
    lc_image_view_destroy(shot_view);
    lc_image_destroy(shot_img);
    /* Exit play first (destroys the runtime world; edit stays). */
    if (session != NULL && led_is_playing(session)) {
        led_play_exit(session);
    }
    if (gui != NULL) {
        leg_context_destroy(gui);
    }
    if (session != NULL) {
        led_session_destroy(session);
    }
    if (world != NULL) {
        le_world_destroy(world);
    }
    if (engine != NULL) {
        le_engine_destroy(engine);
    }
    if (renderer != NULL) {
        lr_renderer_destroy(renderer);
    }
    lc_swapchain_destroy(swapchain);
    if (surface != NULL) {
        lc_surface_destroy(surface);
    }
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return ok ? 0 : 1;
}
