/* Phase 33 GUI-over-GPU proofs (Vulkan-gated; SKIP without a
 * device). Renderer-free (no lr_*; the engine is created WITHOUT a
 * renderer — NULL renderer is legal for authoring worlds, and
 * viewport rendering is SKIPPED here; the composite path is proven
 * by leg_viewport_* unit discipline + the app target):
 *
 * 1. font/blend/scissor draw-walk: build a session + device + GUI
 *    context, open a synthetic frame (leg_frame_begin needs an
 *    lc_window — created headless, never shown), record panels via
 *    leg_panels_frame, end the frame, and record the GUI draws into
 *    an offscreen LDR target pass on the SWAPCHAIN frame encoder
 *    (GUI draws run on the primary encoder only — worker lists
 *    reject scissor; see lc_encoder_set_scissor docs). A swapchain
 *    IS created (presentation never happens; the frame opens and
 *    the offscreen pass records inside it). Asserts:
 *    leg_record_gui succeeds, draw stats are nonzero, scissor state
 *    restores to full-target after the walk.
 * 2. gizmo overlay math: single selection + led_gizmo_begin/apply
 *    through the GUI's drag staging (translate one unit on X via a
 *    synthetic drag payload; undo restores).
 * 3. play/undo/redo through the GUI command funnel: execute a create
 *    + value write, enter play, tick, exit (edit byte-identical via
 *    canonical capture), undo/redo the value write.
 *
 * Window discipline: lc_window_create opens a NATIVE window on this
 * machine (CI runs headless with a virtual display or SKIP when the
 * platform refuses). Validation layers ON (0 ERROR / 0 VUID
 * tolerance: any validation failure fails the test loudly).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_engine/luma_engine.h>
#include <luma_editor/luma_editor.h>

static int g_passed = 0;
static int g_failed = 0;

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        printf("[PASS] %s\n", msg); \
        g_passed++; \
    } else { \
        printf("[FAIL] %s\n", msg); \
        g_failed++; \
    } \
} while (0)

#define SKIP_ENV(what) do { \
    printf("SKIP: environment cannot provide %s\n", what); \
    lc_shutdown(); \
    return 0; \
} while (0)

/* Canonical capture helper (edit-equality oracle). */
static char *capture_canonical(le_engine *e, le_world *w,
                               size_t *out_size) {
    le_asset scene = LE_ASSET_INVALID;
    char *text = NULL;
    size_t size = 0;

    *out_size = 0;
    if (le_scene_create(e, 0, &scene) != LE_SUCCESS) {
        return NULL;
    }
    if (le_scene_capture(w, &scene, NULL) != LE_SUCCESS) {
        le_asset_unload(e, &scene);
        return NULL;
    }
    if (le_scene_save_text(e, &scene, &text, &size) !=
        LE_SUCCESS) {
        le_asset_unload(e, &scene);
        return NULL;
    }
    le_asset_unload(e, &scene);
    *out_size = size;
    return text;
}

int main(void) {
    lc_device *device = NULL;
    lc_window *window = NULL;
    le_engine *engine = NULL;
    le_world *world = NULL;
    led_session *session = NULL;
    leg_context *gui = NULL;
    led_viewport vp;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lc_command_encoder *enc = NULL;
    lc_image *color = NULL;
    lc_image_view *color_view = NULL;
    lc_render_target *target = NULL;
    uint32_t fw = 640;
    uint32_t fh = 480;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Luma Editor Phase 33 GUI-over-GPU proofs...\n");

    if (lc_init() != LC_SUCCESS) {
        printf("[FAIL] lc_init\n");
        return 1;
    }
    /* Device (validation ON: 0 ERROR / 0 VUID tolerance). */
    {
        lc_device_desc dd;

        memset(&dd, 0, sizeof(dd));
        dd.backend = LC_BACKEND_VULKAN;
        dd.enable_validation = 1;
        {
            lc_result drc = lc_device_create(&dd, &device);

            if (drc == LC_SUCCESS) {
            } else if (drc == LC_ERROR_BACKEND_UNAVAILABLE ||
                       drc == LC_ERROR_NO_SUPPORTED_DEVICE) {
                SKIP_ENV("Vulkan device");
            } else {
                printf("[FAIL] device rc=%d\n", (int)drc);
                lc_shutdown();
                return 1;
            }
        }
    }
    /* Headless window (event queue source for leg_frame_begin;
     * never presented — offscreen target only). */
    {
        lc_window_desc wd;

        memset(&wd, 0, sizeof(wd));
        wd.title = "luma-gui-proofs";
        wd.width = fw;
        wd.height = fh;
        if (lc_window_create(&wd, &window) != LC_SUCCESS) {
            lc_device_destroy(device);
            SKIP_ENV("native window");
        }
    }
    /* Engine WITHOUT renderer (authoring worlds need no lr_*;
     * render_scene paths are skipped here by construction). */
    {
        le_engine_desc ed;
        le_world_desc wd;

        memset(&ed, 0, sizeof(ed));
        /* ed.renderer stays NULL: renderer-free authoring. */
        if (le_engine_create(&ed, &engine) != LE_SUCCESS) {
            lc_window_destroy(window);
            lc_device_destroy(device);
            lc_shutdown();
            printf("[FAIL] engine create\n");
            return 1;
        }
        memset(&wd, 0, sizeof(wd));
        if (le_world_create(engine, &wd, &world) != LE_SUCCESS) {
            le_engine_destroy(engine);
            lc_window_destroy(window);
            lc_device_destroy(device);
            lc_shutdown();
            printf("[FAIL] world create\n");
            return 1;
        }
    }
    if (led_session_create(&session) != LED_SUCCESS ||
        led_session_attach(session, engine, world) !=
            LED_SUCCESS) {
        le_world_destroy(world);
        le_engine_destroy(engine);
        lc_window_destroy(window);
        lc_device_destroy(device);
        lc_shutdown();
        printf("[FAIL] editor session\n");
        return 1;
    }
    TEST_CHECK(leg_context_create(session, device, &gui) ==
                   LED_SUCCESS,
               "gui context create");
    TEST_CHECK(leg_set_ini_path(gui, NULL) == LED_SUCCESS,
               "gui ini default");

    led_viewport_default(&vp);
    vp.width = fw;
    vp.height = fh;

    /* Seed one object (selection + inspector + gizmo target). */
    {
        le_object o = LE_OBJECT_INVALID;

        TEST_CHECK(le_object_create(world, &o) == LE_SUCCESS,
                   "seed object");
        TEST_CHECK(le_object_set_name(world, &o, "proof") ==
                       LE_SUCCESS,
                   "seed name");
        TEST_CHECK(led_selection_set(session, &o, 1) ==
                       LED_SUCCESS,
                   "seed select");
    }

    /* Surface + swapchain (frame encoder source; presentation
     * never happens — the frame opens, the offscreen GUI pass
     * records inside it, the frame ends without present... actually
     * lc_end_frame presents: instead the test records but SKIPS
     * end_frame (records + finishes the list? No — the frame
     * encoder belongs to the swapchain frame). Discipline: begin
     * frame -> offscreen pass -> GUI walk -> end pass -> end frame
     * (presents once into the headless window; harmless). */
    {
        lc_swapchain_desc sd;

        if (lc_surface_create(device, window, &surface) !=
            LC_SUCCESS) {
            leg_context_destroy(gui);
            led_session_destroy(session);
            le_world_destroy(world);
            le_engine_destroy(engine);
            lc_window_destroy(window);
            lc_device_destroy(device);
            SKIP_ENV("presentation surface");
        }
        memset(&sd, 0, sizeof(sd));
        sd.width = fw;
        sd.height = fh;
        sd.image_count = 0;
        sd.vsync = 0;
        if (lc_swapchain_create(device, surface, &sd, &swapchain) !=
            LC_SUCCESS) {
            lc_surface_destroy(surface);
            leg_context_destroy(gui);
            led_session_destroy(session);
            le_world_destroy(world);
            le_engine_destroy(engine);
            lc_window_destroy(window);
            lc_device_destroy(device);
            SKIP_ENV("swapchain");
        }
    }
    /* Offscreen LDR target (RGBA8, depthless; the GUI pipeline is
     * depthless — depth_stencil_format UNDEFINED — so the pass must
     * be depthless too or bind fails PIPELINE_INCOMPATIBLE). */
    {
        lc_image_desc idesc;
        lc_image_view_desc vdesc;
        lc_render_target_create_desc rtdesc;
        lc_render_target_attachment ratt;

        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_RGBA8_UNORM;
        idesc.width = fw;
        idesc.height = fh;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                      LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                      LC_IMAGE_USAGE_TRANSFER_SRC |
                      LC_IMAGE_USAGE_TRANSFER_DST;
        idesc.flags = LC_IMAGE_FLAG_NONE;
        idesc.samples = LC_SAMPLE_COUNT_1;
        TEST_CHECK(lc_image_create(device, &idesc, &color) ==
                       LC_SUCCESS,
                   "offscreen color");
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.format = LC_FORMAT_UNDEFINED;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.base_mip_level = 0;
        vdesc.mip_level_count = 1;
        vdesc.base_array_layer = 0;
        vdesc.array_layer_count = 1;
        TEST_CHECK(lc_image_view_create(color, &vdesc,
                                        &color_view) ==
                       LC_SUCCESS,
                   "offscreen view");
        memset(&rtdesc, 0, sizeof(rtdesc));
        ratt.view = color_view;
        rtdesc.color_attachments = &ratt;
        rtdesc.color_attachment_count = 1;
        rtdesc.width = fw;
        rtdesc.height = fh;
        TEST_CHECK(lc_render_target_create(device, &rtdesc,
                                           &target) ==
                       LC_SUCCESS,
                   "offscreen target");
    }

    /* Frame 1: begin (drains the window queue) -> panels -> end ->
     * swapchain frame -> offscreen GUI pass on the frame encoder ->
     * end pass -> swapchain CLEAR pass (present legality) -> end
     * frame (presents once; harmless headless).
     *
     * Debug note (kept: this was a REAL bug, not harness noise):
     * the first version of this test recorded the GUI walk into an
     * offscreen pass but called lc_end_frame WITHOUT any swapchain
     * pass — validation rightly fired (present of an UNDEFINED swap
     * image). The app target (editor/app) always runs the swapchain
     * pass (scene or CLEAR) before present; the test now mirrors it.
     */
    {
        leg_frame_input in;
        leg_draw_stats stats;

        memset(&in, 0, sizeof(in));
        in.window_width = fw;
        in.window_height = fh;
        in.delta_seconds = 1.0f / 60.0f;
        in.window_focused = 1;
        TEST_CHECK(leg_frame_begin(gui, window, &in) ==
                       LED_SUCCESS,
                   "frame begin");
        leg_panels_frame(gui, &vp, 1.0f / 60.0f);
        TEST_CHECK(leg_frame_end(gui) == LED_SUCCESS,
                   "frame end");
        memset(&stats, 0, sizeof(stats));
        leg_draw_get_stats(gui, &stats);
        TEST_CHECK(stats.vertices > 0 && stats.indices > 0,
                   "draw stats nonzero (panels emitted)");
        TEST_CHECK(stats.user_callbacks_skipped == 0,
                   "no user callbacks");
        /* Swapchain frame (frame encoder = primary: scissor OK). */
        {
            lc_result brc = lc_begin_frame(swapchain);

            if (brc == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                TEST_CHECK(lc_swapchain_recreate(swapchain, fw, fh) ==
                               LC_SUCCESS,
                           "swapchain recreate");
                TEST_CHECK(lc_begin_frame(swapchain) == LC_SUCCESS,
                           "begin frame (retry)");
            } else {
                TEST_CHECK(brc == LC_SUCCESS, "begin frame");
            }
        }
        TEST_CHECK(lc_swapchain_get_encoder(swapchain, &enc) ==
                       LC_SUCCESS,
                   "frame encoder");
        /* Record into the offscreen pass (explicit target pass). */
        {
            lc_render_color_attachment catt;
            lc_render_pass_desc pass;

            memset(&catt, 0, sizeof(catt));
            catt.view = color_view;
            catt.load_op = LC_LOAD_OP_CLEAR;
            catt.store_op = LC_STORE_OP_STORE;
            memset(&pass, 0, sizeof(pass));
            pass.color_attachments = &catt;
            pass.color_attachment_count = 1;
            pass.width = fw;
            pass.height = fh;
            TEST_CHECK(lc_encoder_begin_render_pass(enc, &pass) ==
                           LC_SUCCESS,
                       "offscreen pass begin");
            {
                led_result rrc = leg_record_gui(gui, enc,
                                                LC_FORMAT_RGBA8_UNORM,
                                                LC_FORMAT_UNDEFINED);

                TEST_CHECK(rrc == LED_SUCCESS,
                           "record gui (blended+scissor walk)");
            }
            /* Scissor restored to full-target after the walk. */
            {
                lc_scissor_rect sc;
                int has = lc_encoder_get_scissor(enc, &sc);

                TEST_CHECK(has && sc.width == fw &&
                               sc.height == fh,
                           "scissor restored full-target");
            }
            TEST_CHECK(lc_encoder_end_render_pass(enc) ==
                           LC_SUCCESS,
                       "offscreen pass end");
        }
        /* Present legality: lc_end_frame presents the swap image,
         * which is UNDEFINED-layout without a swapchain pass (the
         * validation error is the harness proving it). Run a trivial
         * CLEAR swapchain pass so present is legal. */
        {
            lc_render_swapchain_pass_desc spass;

            memset(&spass, 0, sizeof(spass));
            spass.color_load_op = LC_LOAD_OP_CLEAR;
            spass.color_store_op = LC_STORE_OP_STORE;
            spass.depth_load_op = LC_LOAD_OP_DONT_CARE;
            spass.depth_store_op = LC_STORE_OP_DONT_CARE;
            spass.clear_depth = 1.0f;
            TEST_CHECK(lc_encoder_begin_swapchain_pass(
                           enc, swapchain, &spass) == LC_SUCCESS,
                       "swapchain pass (present legality)");
            TEST_CHECK(lc_encoder_end_render_pass(enc) ==
                           LC_SUCCESS,
                       "swapchain pass end");
        }
        TEST_CHECK(lc_end_frame(swapchain) == LC_SUCCESS,
                   "end frame (present once)");
    }

    /* Gizmo funnel: begin + apply a +1 X translate drag through
     * led_gizmo_begin/apply (the overlay's commit path), then undo
     * restores the position. */
    {
        le_object sel = LE_OBJECT_INVALID;
        float p0[3];
        float p1[3];

        TEST_CHECK(led_selection_get(session, &sel, 1) == 1,
                   "gizmo selection live");
        le_object_get_position(world, &sel, p0);
        TEST_CHECK(led_gizmo_begin(session, LED_GIZMO_TRANSLATE,
                                   0),
                   "gizmo begin X");
        {
            led_gizmo_drag drag;

            memset(&drag, 0, sizeof(drag));
            drag.mode = LED_GIZMO_TRANSLATE;
            drag.axis = 0;
            drag.start_world[0] = 0;
            drag.start_world[1] = 0;
            drag.start_world[2] = 0;
            drag.current_world[0] = 1.0f;
            drag.current_world[1] = 0.3f; /* off-axis ignored */
            drag.current_world[2] = -0.2f;
            drag.snap = 0.0f;
            TEST_CHECK(led_gizmo_apply(session, &drag) ==
                           LED_SUCCESS,
                       "gizmo apply +1X");
        }
        le_object_get_position(world, &sel, p1);
        TEST_CHECK(p1[0] == p0[0] + 1.0f && p1[1] == p0[1] &&
                       p1[2] == p0[2],
                   "gizmo moved exactly +1 X");
        TEST_CHECK(led_undo(session), "gizmo undo");
        {
            float p2[3];

            le_object_get_position(world, &sel, p2);
            TEST_CHECK(p2[0] == p0[0] && p2[1] == p0[1] &&
                           p2[2] == p0[2],
                       "gizmo undo restored");
        }
    }

    /* Play/undo/redo through the GUI command funnel: execute a
     * CREATE (undoable command) -> play (tick) -> exit
     * (byte-identical) -> undo/redo the create. NOTE: led_execute
     * value writes (led_write_property path) are NOT history-tracked
     * — only led_command kinds push undo entries. The funnel test
     * therefore uses LED_CMD_CREATE (undo removes the newborn). */
    {
        led_command cmd;
        char *before = NULL;
        char *after = NULL;
        size_t bsize = 0;
        size_t asize = 0;
        uint32_t base = le_world_get_object_count(world);

        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = LED_CMD_CREATE;
        snprintf(cmd.label, sizeof(cmd.label), "Funnel create");
        strncpy(cmd.name_value, "funnel", sizeof(cmd.name_value) - 1);
        TEST_CHECK(led_execute(session, &cmd) == LED_SUCCESS,
                   "funnel create");
        TEST_CHECK(le_world_get_object_count(world) == base + 1,
                   "funnel object added");
        before = capture_canonical(engine, world, &bsize);
        TEST_CHECK(before != NULL, "capture before play");
        TEST_CHECK(led_play_enter(session) == LED_SUCCESS,
                   "play enter (renderer-free)");
        TEST_CHECK(led_play_tick(session, 1.0f / 60.0f) ==
                       LED_SUCCESS,
                   "play tick");
        TEST_CHECK(led_play_exit(session) == LED_SUCCESS,
                   "play exit");
        after = capture_canonical(engine, world, &asize);
        TEST_CHECK(after != NULL, "capture after play");
        TEST_CHECK(bsize == asize && strcmp(before, after) == 0,
                   "edit byte-identical across play");
        if (before != NULL) {
            le_scene_free_text(before);
        }
        if (after != NULL) {
            le_scene_free_text(after);
        }
        TEST_CHECK(led_undo(session), "funnel undo");
        TEST_CHECK(le_world_get_object_count(world) == base,
                   "funnel undo removed newborn");
        TEST_CHECK(led_redo(session), "funnel redo");
        TEST_CHECK(le_world_get_object_count(world) == base + 1,
                   "funnel redo restored newborn");
    }

    /* Teardown (context first: GPU bridge dies while device lives). */
    leg_context_destroy(gui);

    /* Phase 33-E stress (GUI context torn down; session/engine stay):
     * 1k Play/Stop cycles (enter+tick+exit) with edit byte-identical
     * at the end; scene switch (save-as -> new -> open round-trip
     * via TEMP file, failed open preserves); revert oracle. These
     * are the headed-app's Play/Stop + scene-switch paths driven
     * headless through the same public led_* API. */
    {
        char *steady = NULL;
        size_t steady_size = 0;
        unsigned long cycle = 0;

        steady = capture_canonical(engine, world, &steady_size);
        TEST_CHECK(steady != NULL, "stress baseline capture");
        for (cycle = 0; cycle < 1000; cycle++) {
            if (led_play_enter(session) != LED_SUCCESS) {
                TEST_CHECK(0, "stress enter 1k");
                break;
            }
            if (!led_play_is_paused(session)) {
                if (led_play_tick(session, 1.0f / 60.0f) !=
                    LED_SUCCESS) {
                    TEST_CHECK(0, "stress tick 1k");
                    led_play_exit(session);
                    break;
                }
            }
            if (led_play_exit(session) != LED_SUCCESS) {
                TEST_CHECK(0, "stress exit 1k");
                break;
            }
        }
        TEST_CHECK(cycle == 1000, "play/stop 1k cycles");
        TEST_CHECK(!led_is_playing(session),
                   "not playing after 1k");
        {
            char *after = NULL;
            size_t after_size = 0;

            after = capture_canonical(engine, world, &after_size);
            TEST_CHECK(after != NULL, "stress after capture");
            TEST_CHECK(after != NULL && steady != NULL &&
                           after_size == steady_size &&
                           strcmp(after, steady) == 0,
                       "edit byte-identical after 1k play/stop");
            if (after != NULL) {
                le_scene_free_text(after);
            }
        }
        if (steady != NULL) {
            le_scene_free_text(steady);
        }
    }
    {
        const char *tmp = getenv("TEMP");
        const char *tmp2 = getenv("TMP");
        char path[1024];
        uint32_t before = le_world_get_object_count(world);

        if (tmp == NULL || tmp[0] == '\0') {
            tmp = tmp2;
        }
        if (tmp == NULL || tmp[0] == '\0') {
            tmp = ".";
        }
        memset(path, 0, sizeof(path));
        snprintf(path, sizeof(path),
                 "%s/phase33_gui_stress.luma_scene", tmp);
        TEST_CHECK(led_scene_save_as(session, path) == LED_SUCCESS,
                   "stress save as");
        TEST_CHECK(led_scene_new(session) == LED_SUCCESS,
                   "stress scene new");
        TEST_CHECK(le_world_get_object_count(world) == 0,
                   "stress world cleared");
        TEST_CHECK(led_scene_open(session, path) == LED_SUCCESS,
                   "stress scene open");
        TEST_CHECK(le_world_get_object_count(world) == before,
                   "stress switch restored census");
        TEST_CHECK(led_scene_open(session,
                                  "phase33_gui_missing_xyz") !=
                       LED_SUCCESS,
                   "stress missing open fails");
        TEST_CHECK(le_world_get_object_count(world) == before,
                   "stress failed open preserves world");
        TEST_CHECK(led_scene_revert(session) == LED_SUCCESS,
                   "stress revert ok");
        remove(path);
    }
    led_session_destroy(session);
    lc_render_target_destroy(target);
    lc_image_view_destroy(color_view);
    lc_image_destroy(color);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    le_world_destroy(world);
    le_engine_destroy(engine);
    lc_window_destroy(window);
    lc_device_destroy(device);
    lc_shutdown();

    printf("editor gui gpu: %d passed, %d failed\n", g_passed,
           g_failed);
    return (g_failed == 0) ? 0 : 1;
}
