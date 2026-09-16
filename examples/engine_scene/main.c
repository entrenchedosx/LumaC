/* Phase 24 engine scene example (public le_* API only).
 *
 * World
 * |- CameraRig
 * |   `- Camera (active, perspective)
 * |- Sun (directional light + shadows)
 * |- EnvironmentRoot
 * |   |- ObjectA (PBR cube)
 * |   |- ObjectB (PBR cube, rotated)
 * |   `- MirroredObject (negative X scale: winding-flip proof)
 * `- MovingParent (orbiting)
 *     |- ChildA (sphere)
 *     `- ChildB (cube)
 *
 * Demonstrates: hierarchy, world transforms, active camera sync,
 * light sync, renderable submission with stable temporal IDs,
 * enable/disable filtering, and GPU-driven rendering.
 *
 * Usage: engine_scene [--frames N] [--no-validation] [--no-vsync]
 *   [--screenshot out.png] [--stats]
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

static void set_scale(le_world *world, le_object obj, float x, float y,
                      float z) {
    float s[3] = { x, y, z };

    le_object_set_scale(world, &obj, s);
}

static void spin_y(le_world *world, le_object obj, float angle) {
    float axis[3] = { 0.0f, 1.0f, 0.0f };
    float q[4];

    le_quat_from_axis_angle(axis, angle, q);
    le_object_set_rotation(world, &obj, q);
}

int main(int argc, char **argv) {
    unsigned long max_frames = 600;
    int use_validation = 1;
    int use_vsync = 1;
    int show_stats = 0;
    const char *screenshot_path = NULL;
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
    lr_mesh *cube_mesh = NULL;
    lr_mesh *sphere_mesh = NULL;
    lr_mesh *ground_mesh = NULL;
    lr_material *cube_mat = NULL;
    lr_material *gold_mat = NULL;
    lr_material *ground_mat = NULL;
    le_object camera_rig;
    le_object camera_obj;
    le_object sun;
    le_object env_root;
    le_object object_a;
    le_object object_b;
    le_object mirrored;
    le_object moving;
    le_object child_a;
    le_object child_b;
    lc_image *shot_img = NULL;
    lc_image_view *shot_view = NULL;
    lc_render_target *shot_target = NULL;
    uint32_t shot_w = 0;
    uint32_t shot_h = 0;
    unsigned long frame = 0;
    int ok = 1;

    for (i = 1; i < (unsigned long)argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--no-validation") == 0) {
            use_validation = 0;
        } else if (strcmp(argv[i], "--no-vsync") == 0) {
            use_vsync = 0;
        } else if (strcmp(argv[i], "--stats") == 0) {
            show_stats = 1;
        } else if (strcmp(argv[i], "--screenshot") == 0 &&
                   i + 1 < argc) {
            screenshot_path = argv[++i];
        } else {
            fprintf(stderr,
                    "usage: engine_scene [--frames N] [--no-validation] "
                    "[--no-vsync] [--stats] [--screenshot out.png]\n");
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
    window_desc.title = "Luma Engine Scene";
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
    /* GPU-driven path (engine submission is mode-agnostic; the
     * renderer decides CPU vs GPU internally). */
    if (lr_renderer_set_render_mode(renderer,
                                    LR_RENDER_MODE_GPU_DRIVEN) !=
        LR_SUCCESS) {
        FAIL_CLEANUP("gpu-driven mode");
    }
    if (lr_mesh_create_cube(renderer, 2.0f, &cube_mesh) != LR_SUCCESS) {
        FAIL_CLEANUP("cube mesh");
    }
    if (lr_mesh_create_sphere(renderer, 1.0f, 24, 12, &sphere_mesh) !=
        LR_SUCCESS) {
        FAIL_CLEANUP("sphere mesh");
    }
    if (lr_mesh_create_plane(renderer, 60.0f, 60.0f, &ground_mesh) !=
        LR_SUCCESS) {
        FAIL_CLEANUP("ground mesh");
    }
    {
        lr_pbr_material_desc matdesc;

        memset(&matdesc, 0, sizeof(matdesc));
        matdesc.base_color_factor[0] = 0.55f;
        matdesc.base_color_factor[1] = 0.58f;
        matdesc.base_color_factor[2] = 0.62f;
        matdesc.base_color_factor[3] = 1.0f;
        matdesc.metallic_factor = 0.0f;
        matdesc.roughness_factor = 0.7f;
        matdesc.normal_scale = 1.0f;
        matdesc.occlusion_strength = 1.0f;
        matdesc.alpha_mode = LR_ALPHA_OPAQUE;
        if (lr_material_create_pbr(renderer, &matdesc, &cube_mat) !=
            LR_SUCCESS) {
            FAIL_CLEANUP("cube material");
        }
        matdesc.base_color_factor[0] = 0.9f;
        matdesc.base_color_factor[1] = 0.7f;
        matdesc.base_color_factor[2] = 0.25f;
        matdesc.metallic_factor = 0.9f;
        matdesc.roughness_factor = 0.3f;
        if (lr_material_create_pbr(renderer, &matdesc, &gold_mat) !=
            LR_SUCCESS) {
            FAIL_CLEANUP("gold material");
        }
        matdesc.base_color_factor[0] = 0.16f;
        matdesc.base_color_factor[1] = 0.16f;
        matdesc.base_color_factor[2] = 0.18f;
        matdesc.metallic_factor = 0.0f;
        matdesc.roughness_factor = 0.9f;
        if (lr_material_create_pbr(renderer, &matdesc, &ground_mat) !=
            LR_SUCCESS) {
            FAIL_CLEANUP("ground material");
        }
    }
    /* Engine + world own the scene from here on. */
    {
        le_engine_desc edesc;

        memset(&edesc, 0, sizeof(edesc));
        edesc.renderer = renderer;
        if (le_engine_create(&edesc, &engine) != LE_SUCCESS) {
            FAIL_CLEANUP("le_engine_create");
        }
        {
            le_world_desc wdesc;

            memset(&wdesc, 0, sizeof(wdesc));
            if (le_world_create(engine, &wdesc, &world) != LE_SUCCESS) {
                FAIL_CLEANUP("le_world_create");
            }
        }
    }
    /* CameraRig -> Camera (active). */
    if (le_object_create(world, &camera_rig) != LE_SUCCESS ||
        le_object_create(world, &camera_obj) != LE_SUCCESS) {
        FAIL_CLEANUP("camera objects");
    }
    le_object_set_name(world, &camera_rig, "CameraRig");
    le_object_set_name(world, &camera_obj, "Camera");
    if (le_object_set_parent(world, &camera_obj, &camera_rig) !=
        LE_SUCCESS) {
        FAIL_CLEANUP("camera attach");
    }
    set_pos(world, camera_rig, 0.0f, 4.0f, 10.0f);
    {
        /* Aim the rig at the origin: yaw 0 + pitch down ~20 deg. */
        float yaw[4];
        float pitch[4];
        float q[4];
        float x_axis[3] = { 1.0f, 0.0f, 0.0f };
        float y_axis[3] = { 0.0f, 1.0f, 0.0f };

        le_quat_from_axis_angle(y_axis, 0.0f, yaw);
        le_quat_from_axis_angle(x_axis, -0.35f, pitch);
        le_quat_multiply(yaw, pitch, q);
        le_object_set_rotation(world, &camera_rig, q);
    }
    {
        le_camera_desc cd;

        le_camera_desc_default(&cd);
        cd.near_plane = 0.1f;
        cd.far_plane = 200.0f;
        if (le_object_add_camera(world, &camera_obj, &cd) != LE_SUCCESS) {
            FAIL_CLEANUP("camera component");
        }
        if (le_world_set_active_camera(world, &camera_obj) != LE_SUCCESS) {
            FAIL_CLEANUP("active camera");
        }
    }
    /* Sun (directional + shadows). */
    if (le_object_create(world, &sun) != LE_SUCCESS) {
        FAIL_CLEANUP("sun object");
    }
    le_object_set_name(world, &sun, "Sun");
    {
        float q[4];
        float axis[3] = { 1.0f, 0.0f, 0.0f };

        /* Tilt so the travel direction (-Z rotated) slants down. */
        le_quat_from_axis_angle(axis, -0.9f, q);
        le_object_set_rotation(world, &sun, q);
    }
    {
        le_light_desc ld;

        memset(&ld, 0, sizeof(ld));
        ld.type = LE_LIGHT_DIRECTIONAL;
        ld.color[0] = 1.0f;
        ld.color[1] = 1.0f;
        ld.color[2] = 1.0f;
        ld.intensity = 3.0f;
        ld.shadow.enabled = 1;
        ld.shadow.resolution = 1024;
        ld.shadow.depth_bias = -1.0f;
        ld.shadow.normal_bias = -1.0f;
        if (le_object_add_light(world, &sun, &ld) != LE_SUCCESS) {
            FAIL_CLEANUP("sun light");
        }
    }
    /* EnvironmentRoot + objects. */
    if (le_object_create(world, &env_root) != LE_SUCCESS ||
        le_object_create(world, &object_a) != LE_SUCCESS ||
        le_object_create(world, &object_b) != LE_SUCCESS ||
        le_object_create(world, &mirrored) != LE_SUCCESS) {
        FAIL_CLEANUP("environment objects");
    }
    le_object_set_name(world, &env_root, "EnvironmentRoot");
    le_object_set_name(world, &object_a, "ObjectA");
    le_object_set_name(world, &object_b, "ObjectB");
    le_object_set_name(world, &mirrored, "MirroredObject");
    le_object_set_parent(world, &object_a, &env_root);
    le_object_set_parent(world, &object_b, &env_root);
    le_object_set_parent(world, &mirrored, &env_root);
    {
        le_renderable_desc rd;

        memset(&rd, 0, sizeof(rd));
        rd.mesh = ground_mesh;
        rd.material = ground_mat;
        rd.receives_shadow = 1;
        rd.visible = 1;
        /* Ground slab straight under the world root. */
        {
            le_object ground;

            if (le_object_create(world, &ground) != LE_SUCCESS) {
                FAIL_CLEANUP("ground object");
            }
            le_object_set_name(world, &ground, "Ground");
            set_pos(world, ground, 0.0f, -1.0f, 0.0f);
            le_object_add_renderable(world, &ground, &rd);
        }
        rd.mesh = cube_mesh;
        rd.material = cube_mat;
        rd.casts_shadow = 1;
        set_pos(world, object_a, -2.5f, 0.5f, 0.0f);
        le_object_add_renderable(world, &object_a, &rd);
        set_pos(world, object_b, 2.5f, 0.5f, -1.0f);
        spin_y(world, object_b, 0.6f);
        le_object_add_renderable(world, &object_b, &rd);
        /* Mirrored: negative X scale exercises the winding flip
         * through hierarchy composition. */
        set_pos(world, mirrored, 0.0f, 0.5f, 2.5f);
        set_scale(world, mirrored, -1.0f, 1.0f, 1.0f);
        le_object_add_renderable(world, &mirrored, &rd);
    }
    /* MovingParent -> ChildA (sphere) + ChildB (cube). */
    if (le_object_create(world, &moving) != LE_SUCCESS ||
        le_object_create(world, &child_a) != LE_SUCCESS ||
        le_object_create(world, &child_b) != LE_SUCCESS) {
        FAIL_CLEANUP("moving objects");
    }
    le_object_set_name(world, &moving, "MovingParent");
    le_object_set_name(world, &child_a, "ChildA");
    le_object_set_name(world, &child_b, "ChildB");
    le_object_set_parent(world, &child_a, &moving);
    le_object_set_parent(world, &child_b, &moving);
    {
        le_renderable_desc rd;

        memset(&rd, 0, sizeof(rd));
        rd.casts_shadow = 1;
        rd.receives_shadow = 1;
        rd.visible = 1;
        rd.mesh = sphere_mesh;
        rd.material = gold_mat;
        set_pos(world, child_a, -1.5f, 1.0f, 0.0f);
        le_object_add_renderable(world, &child_a, &rd);
        rd.mesh = cube_mesh;
        rd.material = cube_mat;
        set_pos(world, child_b, 1.5f, 0.5f, 0.0f);
        le_object_add_renderable(world, &child_b, &rd);
    }
    {
        le_world_stats stats;

        le_world_get_stats(world, &stats);
        printf("engine scene: objects=%u roots=%u renderables=%u "
               "cameras=%u lights=%u\n",
               stats.objects_alive, stats.root_count, stats.renderables,
               stats.cameras, stats.lights);
    }
    for (frame = 0; frame < max_frames; frame++) {
        lc_command_encoder *enc = NULL;
        uint32_t w;
        uint32_t h;
        float t = (float)frame * 0.01f;

        lc_poll_events();
        /* Animate: orbit the moving parent, bob ObjectB. */
        set_pos(world, moving, 3.0f * cosf(t), 0.0f, 3.0f * sinf(t));
        spin_y(world, moving, t * 0.5f);
        {
            float p[3];

            le_object_get_position(world, &object_b, p);
            p[1] = 0.5f + 0.3f * sinf(t * 2.0f);
            le_object_set_position(world, &object_b, p);
        }
        /* Demonstrate enable/disable: hide ChildB every 240 frames
         * for 120 frames (submission filtering proof in --stats). */
        if ((frame % 240u) == 0u) {
            le_object_set_enabled(world, &child_b, 0);
        } else if ((frame % 240u) == 120u) {
            le_object_set_enabled(world, &child_b, 1);
        }
        if (lc_begin_frame(swapchain) != LC_SUCCESS ||
            lc_swapchain_get_encoder(swapchain, &enc) != LC_SUCCESS) {
            FAIL_CLEANUP("begin_frame");
        }
        w = lc_window_get_width(window);
        h = lc_window_get_height(window);
        if (w == 0 || h == 0) {
            w = 800;
            h = 600;
        }
        /* Engine scene pass (HDR) — renderer frame stays open. */
        if (le_world_render_scene(world, enc, w, h) != LE_SUCCESS) {
            FAIL_CLEANUP("render_scene");
        }
        if (screenshot_path != NULL && frame + 1u == max_frames) {
            lc_image_desc idesc;
            lc_image_view_desc svdesc;
            lc_render_target_attachment ratt;
            lc_render_target_create_desc rtdesc;
            lc_render_color_attachment satt;
            lc_render_pass_desc spass;

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
            if (lc_image_create(device, &idesc, &shot_img) != LC_SUCCESS) {
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
            if (lc_render_target_create(device, &rtdesc, &shot_target) !=
                LC_SUCCESS) {
                FAIL_CLEANUP("screenshot target");
            }
            memset(&satt, 0, sizeof(satt));
            satt.view = shot_view;
            satt.load_op = LC_LOAD_OP_CLEAR;
            satt.store_op = LC_STORE_OP_STORE;
            memset(&spass, 0, sizeof(spass));
            spass.color_attachments = &satt;
            spass.color_attachment_count = 1;
            spass.width = w;
            spass.height = h;
            if (lc_encoder_begin_render_pass(enc, &spass) != LC_SUCCESS ||
                le_world_render_output(world, enc, shot_target) !=
                    LE_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                FAIL_CLEANUP("screenshot output");
            }
            shot_w = w;
            shot_h = h;
        }
        /* Present leg: tonemap into the swapchain image. */
        {
            lc_render_swapchain_pass_desc spass;

            memset(&spass, 0, sizeof(spass));
            spass.color_load_op = LC_LOAD_OP_CLEAR;
            spass.color_store_op = LC_STORE_OP_STORE;
            spass.depth_load_op = LC_LOAD_OP_CLEAR;
            spass.depth_store_op = LC_STORE_OP_DONT_CARE;
            spass.clear_depth = 1.0f;
            if (lc_encoder_begin_swapchain_pass(enc, swapchain, &spass) !=
                    LC_SUCCESS ||
                le_world_render_output(
                    world, enc,
                    lc_swapchain_get_render_target(swapchain)) !=
                    LE_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                FAIL_CLEANUP("present");
            }
        }
        le_world_render_end(world);
        if (lc_end_frame(swapchain) != LC_SUCCESS) {
            /* suboptimal tolerated; keep running */
        }
        if (show_stats && (frame % 60u == 0u || frame + 1u == max_frames)) {
            le_render_report report;
            lr_camera rc;
            le_extraction_counts counts;

            le_world_get_last_render_report(world, &report);
            le_world_get_render_camera(world, w, h, &rc);
            le_world_get_extraction_counts(world, &counts);
            printf("frame %lu: submitted=%u disabled=%u invisible=%u "
                   "dead=%u draws=%u tris=%u cam=(%.2f,%.2f,%.2f)\n",
                   frame, report.submitted, report.skipped_disabled,
                   report.skipped_invisible, report.skipped_dead,
                   report.renderer_stats.draw_calls,
                   report.renderer_stats.triangles, rc.position[0],
                   rc.position[1], rc.position[2]);
            (void)counts;
        }
        if (screenshot_path != NULL && frame + 1u == max_frames &&
            shot_target != NULL) {
            lc_image_readback_desc rbdesc;
            lc_image_readback_info rbinfo;
            unsigned char *rgba = NULL;
            unsigned char *rgb = NULL;
            uint32_t x;
            uint32_t y;

            memset(&rbdesc, 0, sizeof(rbdesc));
            if (lc_image_query_readback(shot_img, &rbdesc, &rbinfo) !=
                LC_SUCCESS) {
                FAIL_CLEANUP("screenshot query");
            }
            rgba = (unsigned char *)malloc(rbinfo.size);
            rgb = (unsigned char *)malloc((size_t)shot_w * shot_h * 3u);
            if (rgba == NULL || rgb == NULL) {
                free(rgba);
                free(rgb);
                FAIL_CLEANUP("screenshot memory");
            }
            if (lc_image_readback(shot_img, &rbdesc, rgba, rbinfo.size,
                                  NULL) != LC_SUCCESS) {
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
    }
cleanup:
    lc_render_target_destroy(shot_target);
    lc_image_view_destroy(shot_view);
    lc_image_destroy(shot_img);
    le_world_destroy(world);
    le_engine_destroy(engine);
    lr_material_destroy(cube_mat);
    lr_material_destroy(gold_mat);
    lr_material_destroy(ground_mat);
    lr_mesh_destroy(cube_mesh);
    lr_mesh_destroy(sphere_mesh);
    lr_mesh_destroy(ground_mesh);
    if (renderer != NULL) {
        lr_renderer_destroy(renderer);
    }
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return ok ? 0 : 1;
}
