#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_assets/luma_assets.h>

/*
 * Phase 14 example: load a glTF 2.0 model through Luma Assets and
 * render it through Luma Renderer. The whole frame body stays three
 * lines — begin, submit the model, render — because hierarchy
 * composition and material binding live inside la_model_submit.
 *
 * Usage: model_viewer [path/to/model.glb]
 * Defaults to the committed example asset (LA_MODEL_PATH).
 */

#ifndef LA_MODEL_PATH
#define LA_MODEL_PATH "assets/BoxTextured.glb"
#endif

#define FAIL_CLEANUP(msg)                                                 \
    do {                                                                  \
        fprintf(stderr, "%s failed\n", msg);                              \
        goto cleanup;                                                     \
    } while (0)

#define CHECK_LA(expr, what)                                              \
    do {                                                                  \
        if ((expr) != LA_SUCCESS) {                                       \
            fprintf(stderr, "%s failed\n", what);                         \
            goto cleanup;                                                 \
        }                                                                 \
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
            fprintf(stderr, "%s failed (%d)\n", what, _r);                \
            goto cleanup;                                                 \
        }                                                                 \
    } while (0)

int main(int argc, char **argv) {
    const char *model_path = (argc > 1) ? argv[1] : LA_MODEL_PATH;
    lc_window *window = NULL;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lr_renderer *renderer = NULL;
    la_asset_manager *assets = NULL;
    la_model *model = NULL;
    lr_camera camera;
    lc_window_desc window_desc;
    lc_device_desc device_desc = { 0 };
    lc_swapchain_desc swapchain_desc;
    la_asset_manager_desc asset_desc;
    unsigned long frame = 0;
    int exit_code = 1;
    static const float y_axis[3] = { 0.0f, 1.0f, 0.0f };

    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    window_desc.title = "Luma Model Viewer";
    window_desc.width = 800;
    window_desc.height = 600;
    if (lc_window_create(&window_desc, &window) != LC_SUCCESS) {
        fprintf(stderr, "lc_window_create failed\n");
        lc_shutdown();
        return 1;
    }

    device_desc.backend = LC_BACKEND_VULKAN;
    device_desc.enable_validation = 1;
    if (lc_device_create(&device_desc, &device) != LC_SUCCESS) {
        fprintf(stderr, "lc_device_create failed\n");
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }

    if (lc_surface_create(device, window, &surface) != LC_SUCCESS) {
        fprintf(stderr, "lc_surface_create failed\n");
        FAIL_CLEANUP("lc_surface_create");
    }

    swapchain_desc.width = lc_window_get_width(window);
    swapchain_desc.height = lc_window_get_height(window);
    swapchain_desc.image_count = 0;
    swapchain_desc.vsync = 1;
    if (lc_swapchain_create(device, surface, &swapchain_desc, &swapchain) !=
        LC_SUCCESS) {
        fprintf(stderr, "lc_swapchain_create failed\n");
        FAIL_CLEANUP("lc_swapchain_create");
    }

    /* Renderer over the presentation signature. */
    {
        lr_renderer_desc rdesc;

        memset(&rdesc, 0, sizeof(rdesc));
        rdesc.device = device;
        if (lc_swapchain_get_render_target_desc(
                swapchain, &rdesc.render_target) != LC_SUCCESS) {
            FAIL_CLEANUP("lc_swapchain_get_render_target_desc");
        }
        rdesc.max_objects = 128;
        CHECK_LR(lr_renderer_create(&rdesc, &renderer), "lr_renderer_create");
    }

    /* Camera: slightly above, looking at the scene heart. */
    {
        static const float eye[3] = { 0.0f, 2.4f, 6.0f };
        static const float center[3] = { 0.0f, 0.4f, 0.0f };
        static const float up[3] = { 0.0f, 1.0f, 0.0f };

        lr_camera_init(&camera);
        CHECK_LR(lr_camera_set_perspective(&camera, 0.7853982f,
                                           800.0f / 600.0f, 0.1f, 100.0f),
                 "lr_camera_set_perspective");
        CHECK_LR(lr_camera_look_at(&camera, eye, center, up),
                 "lr_camera_look_at");
    }

    /* Assets + model: parse, validate, upload once; submit per frame. */
    memset(&asset_desc, 0, sizeof(asset_desc));
    asset_desc.renderer = renderer;
    CHECK_LA(la_asset_manager_create(&asset_desc, &assets),
             "la_asset_manager_create");
    if (la_model_load(assets, model_path, &model) != LA_SUCCESS) {
        fprintf(stderr, "la_model_load('%s') failed: %s\n", model_path,
                la_asset_manager_get_last_error(assets));
        goto cleanup;
    }

    {
        lr_bounds bounds;

        la_model_get_bounds(model, &bounds);
        printf("LumaC %s\n", lc_get_version_string());
        printf("GPU: %s\n", lc_device_get_name(device));
        printf("Model '%s': nodes=%u meshes=%u materials=%u "
               "textures=%u samplers=%u instances=%u\n",
               model_path, la_model_get_node_count(model),
               la_model_get_mesh_count(model),
               la_model_get_material_count(model),
               la_model_get_texture_count(model),
               la_model_get_sampler_count(model),
               la_model_get_instance_count(model));
        printf("Bounds: min=(%.3f %.3f %.3f) max=(%.3f %.3f %.3f) "
               "radius=%.3f. Close to exit.\n",
               bounds.min[0], bounds.min[1], bounds.min[2], bounds.max[0],
               bounds.max[1], bounds.max[2], bounds.radius);
    }

    while (!lc_window_should_close(window)) {
        uint32_t w;
        uint32_t h;
        lc_command_encoder *enc = NULL;
        lc_render_swapchain_pass_desc spass;
        lr_transform root;
        lc_result res;
        float t;

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

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.clear_color[0] = 0.05f;
        spass.clear_color[1] = 0.06f;
        spass.clear_color[2] = 0.09f;
        spass.clear_color[3] = 1.0f;
        spass.depth_load_op = LC_LOAD_OP_CLEAR;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        CHECK_LC(lc_encoder_begin_swapchain_pass(enc, swapchain, &spass),
                 "swapchain pass begin");

        /* Three-line frame body: begin, submit lights + model, render. */
        if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
            fprintf(stderr, "lr_renderer_begin failed\n");
            goto cleanup;
        }
        {
            /* Key light (travel direction: above-front, toward scene)
             * plus a cool rim from behind-left. */
            lr_light key;
            lr_light rim;

            memset(&key, 0, sizeof(key));
            key.type = LR_LIGHT_DIRECTIONAL;
            key.color[0] = 1.0f;
            key.color[1] = 0.96f;
            key.color[2] = 0.90f;
            key.intensity = 2.5f;
            key.direction[0] = 0.35f;
            key.direction[1] = -1.0f;
            key.direction[2] = 0.25f;
            if (lr_renderer_submit_light(renderer, &key) != LR_SUCCESS) {
                fprintf(stderr, "lr_renderer_submit_light failed\n");
                goto cleanup;
            }
            memset(&rim, 0, sizeof(rim));
            rim.type = LR_LIGHT_DIRECTIONAL;
            rim.color[0] = 0.55f;
            rim.color[1] = 0.65f;
            rim.color[2] = 1.0f;
            rim.intensity = 0.8f;
            rim.direction[0] = -0.6f;
            rim.direction[1] = -0.25f;
            rim.direction[2] = -0.75f;
            if (lr_renderer_submit_light(renderer, &rim) != LR_SUCCESS) {
                fprintf(stderr, "lr_renderer_submit_light failed\n");
                goto cleanup;
            }
        }
        lr_transform_identity(&root);
        lr_quat_from_axis_angle(y_axis, t, root.rotation);
        if (la_model_submit(model, renderer, &root) != LA_SUCCESS) {
            fprintf(stderr, "la_model_submit failed\n");
            goto cleanup;
        }
        if (lr_renderer_render(renderer, enc,
                               lc_swapchain_get_render_target(swapchain)) !=
            LR_SUCCESS) {
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
            printf("frame %lu: submitted=%u visible=%u draws=%u tris=%u "
                   "pipes=%u mats=%u\n",
                   frame, stats.submitted_objects, stats.visible_objects,
                   stats.draw_calls, stats.triangles, stats.pipeline_binds,
                   stats.material_binds);
        }
        frame++;
    }

    exit_code = 0;

cleanup:
    /* Dependents first: model, manager, renderer, then LumaC objects. */
    la_model_destroy(model);
    la_asset_manager_destroy(assets);
    lr_renderer_destroy(renderer);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return exit_code;
}
