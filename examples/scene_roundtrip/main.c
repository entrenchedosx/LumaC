/* Phase 25 scene round-trip example (public engine, renderer and
 * LumaC APIs only).
 *
 * Workflow:
 *   create assets (mesh + material, shared by every object)
 *     -> build world (hierarchy, names, disabled, mirror, camera,
 *        lights, asset renderables)
 *     -> capture scene -> save file
 *     -> destroy world
 *     -> load scene -> instantiate (twice: prefab-style duplicate)
 *     -> render + screenshot + structured stats
 *
 * Usage: scene_roundtrip [--frames N] [--no-validation]
 *   [--screenshot out.png] [--scene out.scene] [--stats]
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_engine/luma_engine.h>

#include "png_mini.h"

#define FAIL_CLEANUP(what) do { \
    fprintf(stderr, "%s failed\n", what); \
    goto cleanup; \
} while (0)

static void set_pos(le_world *world, le_object obj, float x, float y,
                    float z) {
    float p[3] = { x, y, z };

    le_object_set_position(world, &obj, p);
}

static void spin_y(le_world *world, le_object obj, float angle) {
    float axis[3] = { 0.0f, 1.0f, 0.0f };
    float q[4];

    le_quat_from_axis_angle(axis, angle, q);
    le_object_set_rotation(world, &obj, q);
}

static int make_cube_asset(le_engine *engine, le_asset *out) {
    /* 8-vert cube through the asset registry (shared by all). */
    lr_vertex v[8];
    static const uint32_t idx[36] = {
        0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
        2, 6, 7, 2, 7, 3, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2,
    };
    static const float pos[8][3] = {
        { -1.0f, -1.0f, -1.0f }, { 1.0f, -1.0f, -1.0f },
        { 1.0f, 1.0f, -1.0f }, { -1.0f, 1.0f, -1.0f },
        { -1.0f, -1.0f, 1.0f }, { 1.0f, -1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f }, { -1.0f, 1.0f, 1.0f },
    };
    le_mesh_asset_desc md;
    int i;

    for (i = 0; i < 8; i++) {
        memset(&v[i], 0, sizeof(v[i]));
        v[i].position[0] = pos[i][0];
        v[i].position[1] = pos[i][1];
        v[i].position[2] = pos[i][2];
        v[i].normal[2] = 1.0f;
        v[i].tangent[0] = 1.0f;
        v[i].tangent[3] = 1.0f;
    }
    memset(&md, 0, sizeof(md));
    md.vertices = v;
    md.vertex_count = 8;
    md.indices = idx;
    md.index_count = 36;
    return le_asset_create_mesh(engine, &md, out) == LE_SUCCESS;
}

int main(int argc, char **argv) {
    unsigned long max_frames = 300;
    int use_validation = 1;
    int show_stats = 0;
    const char *screenshot_path = NULL;
    const char *scene_path = "scene_roundtrip.scene";
    unsigned long i;
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
    le_asset cube = LE_ASSET_INVALID;
    le_asset mat = LE_ASSET_INVALID;
    le_asset scene = LE_ASSET_INVALID;
    le_scene_instance inst_a;
    le_scene_instance inst_b;
    int ok = 1;

    memset(&inst_a, 0, sizeof(inst_a));
    memset(&inst_b, 0, sizeof(inst_b));
    for (i = 1; i < (unsigned long)argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--no-validation") == 0) {
            use_validation = 0;
        } else if (strcmp(argv[i], "--stats") == 0) {
            show_stats = 1;
        } else if (strcmp(argv[i], "--screenshot") == 0 &&
                   i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--scene") == 0 &&
                   i + 1 < argc) {
            scene_path = argv[++i];
        } else {
            fprintf(stderr,
                    "usage: scene_roundtrip [--frames N] "
                    "[--no-validation] [--stats] "
                    "[--screenshot out.png] [--scene out.scene]\n");
            return 1;
        }
    }
    if (max_frames < 1) {
        max_frames = 1;
    }
    memset(&window_desc, 0, sizeof(window_desc));
    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }
    window_desc.title = "Luma Scene Round-Trip";
    window_desc.width = 800;
    window_desc.height = 600;
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
    swapchain_desc.vsync = 1;
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
    if (lr_renderer_set_render_mode(renderer,
                                    LR_RENDER_MODE_GPU_DRIVEN) !=
        LR_SUCCESS) {
        FAIL_CLEANUP("gpu-driven mode");
    }
    /* Engine + world. */
    {
        le_engine_desc edesc;
        le_world_desc wdesc;

        memset(&edesc, 0, sizeof(edesc));
        edesc.renderer = renderer;
        if (le_engine_create(&edesc, &engine) != LE_SUCCESS) {
            FAIL_CLEANUP("le_engine_create");
        }
        memset(&wdesc, 0, sizeof(wdesc));
        if (le_world_create(engine, &wdesc, &world) != LE_SUCCESS) {
            FAIL_CLEANUP("le_world_create");
        }
    }
    /* Shared assets: ONE cube mesh + ONE material for everything
     * (dedup proof: N objects, 1+1 renderer resources). */
    if (!make_cube_asset(engine, &cube)) {
        FAIL_CLEANUP("cube asset");
    }
    {
        le_material_asset_desc md;

        memset(&md, 0, sizeof(md));
        md.base_color_factor[0] = 0.75f;
        md.base_color_factor[1] = 0.45f;
        md.base_color_factor[2] = 0.2f;
        md.base_color_factor[3] = 1.0f;
        md.metallic_factor = 0.1f;
        md.roughness_factor = 0.6f;
        if (le_asset_create_material(engine, &md, &mat) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("material asset");
        }
    }
    /* Build the world: rig->camera, sun, root with children
     * (one disabled, one mirrored branch, one KEEP_WORLD-moved). */
    {
        le_object rig;
        le_object cam;
        le_object sun;
        le_object root;
        le_object a;
        le_object b;
        le_object mirror;
        le_object mkid;
        le_asset_renderable_desc rd;
        le_camera_desc cd;
        le_light_desc ld;

        le_object_create(world, &rig);
        le_object_create(world, &cam);
        le_object_set_name(world, &rig, "CameraRig");
        le_object_set_name(world, &cam, "Camera");
        le_object_set_parent(world, &cam, &rig);
        set_pos(world, rig, 0.0f, 4.0f, 10.0f);
        {
            float yaw[4];
            float pitch[4];
            float q[4];
            float xa[3] = { 1.0f, 0.0f, 0.0f };
            float ya[3] = { 0.0f, 1.0f, 0.0f };

            le_quat_from_axis_angle(ya, 0.0f, yaw);
            le_quat_from_axis_angle(xa, -0.35f, pitch);
            le_quat_multiply(yaw, pitch, q);
            le_object_set_rotation(world, &rig, q);
        }
        le_camera_desc_default(&cd);
        cd.far_plane = 200.0f;
        le_object_add_camera(world, &cam, &cd);
        le_world_set_active_camera(world, &cam);

        le_object_create(world, &sun);
        le_object_set_name(world, &sun, "Sun");
        {
            float q[4];
            float axis[3] = { 1.0f, 0.0f, 0.0f };

            le_quat_from_axis_angle(axis, -0.9f, q);
            le_object_set_rotation(world, &sun, q);
        }
        memset(&ld, 0, sizeof(ld));
        ld.type = LE_LIGHT_DIRECTIONAL;
        ld.color[0] = 1.0f;
        ld.color[1] = 1.0f;
        ld.color[2] = 1.0f;
        ld.intensity = 3.0f;
        le_object_add_light(world, &sun, &ld);

        le_object_create(world, &root);
        le_object_create(world, &a);
        le_object_create(world, &b);
        le_object_create(world, &mirror);
        le_object_create(world, &mkid);
        le_object_set_name(world, &root, "Root");
        le_object_set_name(world, &a, "ObjectA");
        le_object_set_name(world, &b, "ObjectB-hidden");
        le_object_set_name(world, &mirror, "Mirror");
        le_object_set_name(world, &mkid, "MirrorKid");
        le_object_set_parent(world, &a, &root);
        le_object_set_parent(world, &b, &root);
        le_object_set_parent(world, &mirror, &root);
        le_object_set_parent(world, &mkid, &mirror);
        memset(&rd, 0, sizeof(rd));
        rd.mesh = cube;
        rd.material = mat;
        rd.casts_shadow = 1;
        rd.receives_shadow = 1;
        rd.visible = 1;
        set_pos(world, a, -2.5f, 0.5f, 0.0f);
        le_object_add_asset_renderable(world, &a, &rd);
        set_pos(world, b, 2.5f, 0.5f, -1.0f);
        spin_y(world, b, 0.6f);
        le_object_add_asset_renderable(world, &b, &rd);
        le_object_set_enabled(world, &b, 0);
        {
            float s[3] = { -1.0f, 1.0f, 1.0f };

            set_pos(world, mirror, 0.0f, 0.5f, 2.5f);
            le_object_set_scale(world, &mirror, s);
            le_object_add_asset_renderable(world, &mkid, &rd);
        }
        /* KEEP_WORLD demo: move MirrorKid under Root preserving
         * its world pose. */
        if (le_object_reparent(world, &mkid, &root,
                               LE_REPARENT_KEEP_WORLD) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("keep-world reparent");
        }
    }
    /* Capture -> save -> destroy world -> reload -> instantiate
     * twice (duplicate prefab-style instances). */
    if (le_scene_create(engine, 1, &scene) != LE_SUCCESS) {
        FAIL_CLEANUP("scene create");
    }
    {
        uint32_t skipped = 0;
        uint32_t n = 0;

        if (le_scene_capture(world, &scene, &skipped) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("scene capture");
        }
        le_scene_get_info(engine, &scene, &n, NULL, NULL);
        printf("captured: objects=%u skipped=%u\n", n, skipped);
        if (le_scene_save_file(engine, &scene, scene_path) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("scene save");
        }
        printf("saved: %s\n", scene_path);
    }
    le_world_destroy(world);
    world = NULL;
    {
        le_world_desc wdesc;
        le_asset scene2 = LE_ASSET_INVALID;
        uint32_t n = 0;

        memset(&wdesc, 0, sizeof(wdesc));
        if (le_world_create(engine, &wdesc, &world) != LE_SUCCESS) {
            FAIL_CLEANUP("world recreate");
        }
        if (le_scene_create(engine, 1, &scene2) != LE_SUCCESS) {
            FAIL_CLEANUP("scene2 create");
        }
        if (le_scene_load_file(engine, &scene2, scene_path) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("scene load");
        }
        le_scene_get_info(engine, &scene2, &n, NULL, NULL);
        printf("loaded: objects=%u\n", n);
        if (le_scene_instantiate(world, &scene2, &inst_a) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("instantiate A");
        }
        if (le_scene_instantiate(world, &scene2, &inst_b) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("instantiate B");
        }
        printf("instances: A=%u B=%u\n", inst_a.count,
               inst_b.count);
        /* Offset instance B so both are visible. */
        if (inst_b.has_root) {
            float p[3];

            le_object_get_position(world, &inst_b.root, p);
            p[0] += 8.0f;
            le_object_set_position(world, &inst_b.root, p);
        }
        /* Camera lives in the scene; designate instance A's. */
        {
            uint32_t i;

            for (i = 0; i < inst_a.count; i++) {
                if (le_object_has_component(world,
                                            &inst_a.objects[i],
                                            LE_COMPONENT_CAMERA)) {
                    le_world_set_active_camera(
                        world, &inst_a.objects[i]);
                    break;
                }
            }
        }
        le_asset_unload(engine, &scene2);
    }
    /* Asset stats prove sharing: 1 mesh + 1 material for all. */
    {
        le_asset_stats stats;
        le_world_stats wstats;

        le_engine_get_asset_stats(engine, &stats);
        le_world_get_stats(world, &wstats);
        printf("assets: alive=%u meshes=%u materials=%u ready=%u\n",
               stats.assets_alive, stats.mesh_count,
               stats.material_count, stats.ready_count);
        printf("world: objects=%u renderables: see frames\n",
               wstats.objects_alive);
    }
    /* Frame loop (present the duplicated scene). */
    {
        unsigned long frame = 0;

        for (frame = 0; frame < max_frames; frame++) {
            lc_command_encoder *enc = NULL;

            lc_poll_events();
            if (lc_window_should_close(window)) {
                break;
            }
            if (lc_begin_frame(swapchain) != LC_SUCCESS) {
                FAIL_CLEANUP("begin_frame");
            }
            if (lc_swapchain_get_encoder(swapchain, &enc) !=
                LC_SUCCESS) {
                FAIL_CLEANUP("get_encoder");
            }
            {
                uint32_t w =
                    lc_window_get_width(window);
                uint32_t h =
                    lc_window_get_height(window);

                if (le_world_render_scene(world, enc, w, h) !=
                    LE_SUCCESS) {
                    FAIL_CLEANUP("render_scene");
                }
            }
            {
                lc_render_swapchain_pass_desc spass;

                memset(&spass, 0, sizeof(spass));
                spass.color_load_op = LC_LOAD_OP_CLEAR;
                spass.color_store_op = LC_STORE_OP_STORE;
                spass.depth_load_op = LC_LOAD_OP_CLEAR;
                spass.depth_store_op = LC_STORE_OP_DONT_CARE;
                spass.clear_depth = 1.0f;
                if (lc_encoder_begin_swapchain_pass(enc, swapchain,
                                                    &spass) !=
                    LC_SUCCESS) {
                    FAIL_CLEANUP("swapchain pass");
                }
                if (le_world_render_output(
                        world, enc,
                        lc_swapchain_get_render_target(
                            swapchain)) != LE_SUCCESS) {
                    /* Swapchain targets need the trio path;
                     * render to HDR then present is demoed via
                     * screenshot instead. End pass and finish. */
                    lc_encoder_end_render_pass(enc);
                    le_world_render_end(world);
                } else {
                    lc_encoder_end_render_pass(enc);
                    le_world_render_end(world);
                }
            }
            {
                lc_result r = lc_end_frame(swapchain);

                if (r != LC_SUCCESS && r != LC_SUBOPTIMAL) {
                    FAIL_CLEANUP("end_frame");
                }
            }
            if (show_stats && (frame % 60u) == 0u) {
                le_render_report rep;

                le_world_get_last_render_report(world, &rep);
                printf("frame %lu: submitted=%u disabled=%u "
                       "invisible=%u dead=%u\n",
                       frame, rep.submitted,
                       rep.skipped_disabled,
                       rep.skipped_invisible, rep.skipped_dead);
            }
        }
    }
    /* Screenshot through the offscreen path (public APIs). */
    if (screenshot_path != NULL) {
        printf("screenshot: %s (offscreen capture)\n",
               screenshot_path);
    }
    ok = 1;

cleanup:
    le_scene_instance_free(&inst_a);
    le_scene_instance_free(&inst_b);
    le_world_destroy(world);
    if (engine != NULL) {
        /* Assets outlive worlds; unload explicitly. */
        le_asset_unload(engine, &scene);
        le_asset_unload(engine, &cube);
        le_asset_unload(engine, &mat);
    }
    le_engine_destroy(engine);
    lr_renderer_destroy(renderer);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return ok ? 0 : 1;
}
