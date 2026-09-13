/*
 * Phase 16 example: real-time shadows on PBR. A cube and a ball cast
 * PCF-filtered shadows from one slowly orbiting directional light
 * onto a PBR ground plane; a warm unshadowed point fill proves
 * light separation.
 *
 * Each frame is a single prepared leg: begin -> submits ->
 * render_shadows -> main pass -> present. Exactly one lights upload
 * happens per frame; a second renderer leg with different prepared
 * state would clobber the persistently-mapped lights UBO before the
 * single submit executes (see SHADOW_ARCHITECTURE.md).
 *
 * Usage: shadow_scene [--frames N]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>

#define FAIL_CLEANUP(msg)                                                 \
    do {                                                                  \
        fprintf(stderr, "%s failed\n", msg);                              \
        goto cleanup;                                                     \
    } while (0)

#define CHECK_LR(expr, what)                                              \
    do {                                                                  \
        if ((expr) != LR_SUCCESS) {                                       \
            fprintf(stderr, "%s failed\n", what);                         \
            goto cleanup;                                                 \
        }                                                                 \
    } while (0)

#define CHECK_LC(expr, what)                                              \
    do {                                                                  \
        lc_result _r = (expr);                                            \
        if (_r != LC_SUCCESS) {                                           \
            fprintf(stderr, "%s failed (%d)\n", what, _r);               \
            goto cleanup;                                                 \
        }                                                                 \
    } while (0)

static void make_pbr(lr_renderer *renderer, const float base[4],
                     float metallic, float roughness, lr_material **out) {
    lr_pbr_material_desc desc;

    memset(&desc, 0, sizeof(desc));
    memcpy(desc.base_color_factor, base, sizeof(desc.base_color_factor));
    desc.metallic_factor = metallic;
    desc.roughness_factor = roughness;
    desc.normal_scale = 1.0f;
    desc.occlusion_strength = 1.0f;
    *out = NULL;
    if (lr_material_create_pbr(renderer, &desc, out) != LR_SUCCESS) {
        *out = NULL;
    }
}

static void submit_prop(lr_renderer *renderer, lr_mesh *mesh,
                        lr_material *mat, float x, float y, float z,
                        int *ok) {
    lr_draw_item item;

    memset(&item, 0, sizeof(item));
    lr_transform_identity(&item.transform);
    item.transform.position[0] = x;
    item.transform.position[1] = y;
    item.transform.position[2] = z;
    item.mesh = mesh;
    item.material = mat;
    item.casts_shadow = 1;
    item.receives_shadow = 1;
    if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
        *ok = 0;
    }
}

int main(int argc, char **argv) {
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    static const float look_target[3] = { 0.0f, 1.0f, -0.5f };
    static const float gray[4] = { 0.6f, 0.6f, 0.62f, 1.0f };
    lc_window *window = NULL;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lr_renderer *renderer = NULL;
    lr_mesh *ground_mesh = NULL;
    lr_mesh *cube_mesh = NULL;
    lr_mesh *ball_mesh = NULL;
    lr_material *ground_mat = NULL;
    lr_material *cube_mat = NULL;
    lr_material *ball_mat = NULL;
    lr_camera camera;
    lc_window_desc window_desc;
    lc_device_desc device_desc = { 0 };
    lc_swapchain_desc swapchain_desc;
    unsigned long frame = 0;
    unsigned long max_frames = 0;
    int exit_code = 1;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = (unsigned long)strtoul(argv[++i], NULL, 10);
        } else {
            fprintf(stderr, "usage: shadow_scene [--frames N]\n");
            return 1;
        }
    }

    memset(&window_desc, 0, sizeof(window_desc));
    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    window_desc.title = "Luma Shadow Scene";
    window_desc.width = 800;
    window_desc.height = 600;
    if (lc_window_create(&window_desc, &window) != LC_SUCCESS) {
        fprintf(stderr, "lc_window_create failed\n");
        lc_shutdown();
        return 1;
    }

    memset(&device_desc, 0, sizeof(device_desc));
    device_desc.backend = LC_BACKEND_VULKAN;
    device_desc.enable_validation = 1;
    if (lc_device_create(&device_desc, &device) != LC_SUCCESS) {
        fprintf(stderr, "lc_device_create failed\n");
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }

    if (lc_surface_create(device, window, &surface) != LC_SUCCESS) {
        FAIL_CLEANUP("lc_surface_create");
    }

    memset(&swapchain_desc, 0, sizeof(swapchain_desc));
    swapchain_desc.width = lc_window_get_width(window);
    swapchain_desc.height = lc_window_get_height(window);
    swapchain_desc.image_count = 0;
    swapchain_desc.vsync = 1;
    if (lc_swapchain_create(device, surface, &swapchain_desc, &swapchain) !=
        LC_SUCCESS) {
        FAIL_CLEANUP("lc_swapchain_create");
    }

    {
        lr_renderer_desc rdesc;

        memset(&rdesc, 0, sizeof(rdesc));
        rdesc.device = device;
        if (lc_swapchain_get_render_target_desc(
                swapchain, &rdesc.render_target) != LC_SUCCESS) {
            FAIL_CLEANUP("lc_swapchain_get_render_target_desc");
        }
        rdesc.max_objects = 64;
        CHECK_LR(lr_renderer_create(&rdesc, &renderer),
                 "lr_renderer_create");
    }

    lr_camera_init(&camera);
    CHECK_LR(lr_camera_set_perspective(&camera, 0.6f, 800.0f / 600.0f,
                                       0.1f, 100.0f),
             "lr_camera_set_perspective");

    CHECK_LR(lr_mesh_create_plane(renderer, 14.0f, 14.0f, &ground_mesh),
             "ground mesh");
    CHECK_LR(lr_mesh_create_cube(renderer, 2.0f, &cube_mesh), "cube mesh");
    CHECK_LR(lr_mesh_create_sphere(renderer, 1.0f, 32, 16, &ball_mesh),
             "ball mesh");
    make_pbr(renderer, gray, 0.0f, 0.8f, &ground_mat);
    make_pbr(renderer, gray, 0.0f, 0.8f, &cube_mat);
    make_pbr(renderer, gray, 0.0f, 0.6f, &ball_mat);
    if (ground_mat == NULL || cube_mat == NULL || ball_mat == NULL) {
        FAIL_CLEANUP("scene materials");
    }

    printf("LumaC %s\n", lc_get_version_string());
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("Shadow scene: cube + ball under one orbiting shadow light. "
           "Close to exit.\n");

    while (!lc_window_should_close(window)) {
        uint32_t w;
        uint32_t h;
        lc_command_encoder *enc = NULL;
        lc_render_swapchain_pass_desc spass;
        lc_result res;
        float t;
        float eye[3];
        int ok = 1;

        if (max_frames != 0 && frame >= max_frames) {
            break;
        }
        lc_poll_events();
        w = lc_window_get_width(window);
        h = lc_window_get_height(window);
        if (w == 0 || h == 0) {
            continue;
        }
        if (w != lc_swapchain_get_width(swapchain) ||
            h != lc_swapchain_get_height(swapchain)) {
            res = lc_swapchain_recreate(swapchain, w, h);
            if (res == LC_ERROR_ZERO_EXTENT) {
                continue;
            }
            if (res != LC_SUCCESS) {
                fprintf(stderr, "lc_swapchain_recreate failed (%d)\n", res);
                goto cleanup;
            }
        }

        res = lc_begin_frame(swapchain);
        if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            res = lc_swapchain_recreate(swapchain, w, h);
            if (res != LC_SUCCESS && res != LC_ERROR_ZERO_EXTENT) {
                fprintf(stderr, "lc_swapchain_recreate failed (%d)\n", res);
                goto cleanup;
            }
            continue;
        }
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_begin_frame failed (%d)\n", res);
            goto cleanup;
        }
        CHECK_LC(lc_swapchain_get_encoder(swapchain, &enc),
                 "lc_swapchain_get_encoder");

        t = (float)frame * 0.01f;

        /* Fixed camera (matches the shadow testbed view). */
        eye[0] = 0.0f;
        eye[1] = 4.0f;
        eye[2] = 9.0f;
        CHECK_LR(lr_camera_look_at(&camera, eye, look_target, up),
                 "lr_camera_look_at");

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.clear_color[0] = 0.02f;
        spass.clear_color[1] = 0.02f;
        spass.clear_color[2] = 0.03f;
        spass.clear_color[3] = 1.0f;
        spass.depth_load_op = LC_LOAD_OP_CLEAR;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;

        if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
            fprintf(stderr, "lr_renderer_begin failed\n");
            goto cleanup;
        }

        /* Hero: orbiting shadowed directional (t=0 matches the
         * testbed tilt exactly). */
        {
            lr_light key;
            lr_light fill;
            float ang = t * 0.3f;
            float ca = cosf(ang);
            float sa = sinf(ang);

            memset(&key, 0, sizeof(key));
            key.type = LR_LIGHT_DIRECTIONAL;
            key.color[0] = 1.0f;
            key.color[1] = 1.0f;
            key.color[2] = 1.0f;
            key.intensity = 3.0f;
            key.direction[0] = 0.5f * ca + 0.2f * sa;
            key.direction[1] = -1.0f;
            key.direction[2] = -0.5f * sa + 0.2f * ca;
            key.shadow.enabled = 1;
            key.shadow.resolution = 1024;
            key.shadow.depth_bias = -1.0f;
            key.shadow.normal_bias = -1.0f;
            if (lr_renderer_submit_light(renderer, &key) != LR_SUCCESS) {
                fprintf(stderr, "submit key failed\n");
                goto cleanup;
            }

            /* Unshadowed warm fill (proves light separation). */
            memset(&fill, 0, sizeof(fill));
            fill.type = LR_LIGHT_POINT;
            fill.color[0] = 1.0f;
            fill.color[1] = 0.75f;
            fill.color[2] = 0.5f;
            fill.intensity = 20.0f;
            fill.position[0] = 4.0f;
            fill.position[1] = 3.0f;
            fill.position[2] = 1.0f;
            fill.range = 15.0f;
            if (lr_renderer_submit_light(renderer, &fill) != LR_SUCCESS) {
                fprintf(stderr, "submit fill failed\n");
                goto cleanup;
            }
        }

        submit_prop(renderer, ground_mesh, ground_mat, 0.0f, 0.0f, 0.0f,
                    &ok);
        submit_prop(renderer, cube_mesh, cube_mat, 0.0f, 1.5f, 0.0f, &ok);
        submit_prop(renderer, ball_mesh, ball_mat, 0.0f, 2.0f, 0.5f, &ok);
        if (!ok) {
            fprintf(stderr, "submit props failed\n");
            goto cleanup;
        }
        {
            float ambient[3] = { 0.03f, 0.03f, 0.035f };

            lr_renderer_set_ambient(renderer, ambient);
        }
        if (lr_renderer_render_shadows(renderer, enc) != LR_SUCCESS) {
            fprintf(stderr, "lr_renderer_render_shadows failed\n");
            goto cleanup;
        }
        /* The main pass opens only after prepare (passes cannot
         * nest: the shadow depth passes run first). */
        CHECK_LC(lc_encoder_begin_swapchain_pass(enc, swapchain, &spass),
                 "swapchain pass begin");
        if (lr_renderer_render(renderer, enc,
                               lc_swapchain_get_render_target(
                                   swapchain)) != LR_SUCCESS) {
            fprintf(stderr, "lr_renderer_render failed\n");
            goto cleanup;
        }
        lr_renderer_end(renderer);
        CHECK_LC(lc_encoder_end_render_pass(enc), "pass end");

        res = lc_end_frame(swapchain);
        if (res == LC_SUBOPTIMAL ||
            res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            res = lc_swapchain_recreate(swapchain, w, h);
            if (res != LC_SUCCESS && res != LC_ERROR_ZERO_EXTENT) {
                fprintf(stderr, "lc_swapchain_recreate failed (%d)\n", res);
                goto cleanup;
            }
            continue;
        }
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_end_frame failed (%d)\n", res);
            goto cleanup;
        }
        if ((frame % 120u) == 0u) {
            lr_render_stats stats;

            lr_renderer_get_stats(renderer, &stats);
            printf("frame %lu: draws=%u tris=%u shadow_passes=%u "
                   "shadow_draws=%u lights=%u/%u\n",
                   frame, stats.draw_calls, stats.triangles,
                   stats.shadow_passes, stats.shadow_draw_calls,
                   stats.active_lights, stats.submitted_lights);
        }
        frame++;
    }

    exit_code = 0;

cleanup:
    /* Drain the last frame before tearing down GPU objects. */
    if (device != NULL) {
        lc_device_wait_idle(device);
    }
    lr_material_destroy(ball_mat);
    lr_material_destroy(cube_mat);
    lr_material_destroy(ground_mat);
    lr_mesh_destroy(ball_mesh);
    lr_mesh_destroy(cube_mesh);
    lr_mesh_destroy(ground_mesh);
    lr_renderer_destroy(renderer);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return exit_code;
}
