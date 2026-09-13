#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>

/*
 * Phase 13 example: a small multi-object 3D scene through Luma
 * Renderer. Ground plane, three cubes (one textured), and a sphere —
 * each independently transformed — render through one camera with a
 * three-line frame body: begin, submit all, render. No manual LumaC
 * draw commands in application code.
 */

#define TEX_W 64
#define TEX_H 64

static unsigned char s_texels[TEX_W * TEX_H * 4];

static void fill_checkerboard(void) {
    uint32_t x;
    uint32_t y;

    for (y = 0; y < TEX_H; y++) {
        for (x = 0; x < TEX_W; x++) {
            unsigned char *px = &s_texels[(y * TEX_W + x) * 4];
            int white = (int)(((x / 8u) + (y / 8u)) % 2u);

            px[0] = white ? 235 : 30;
            px[1] = white ? 235 : 90;
            px[2] = white ? 235 : 160;
            px[3] = 255;
        }
    }
}

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
            fprintf(stderr, "%s failed (%d)\n", what, _r);                \
            goto cleanup;                                                 \
        }                                                                 \
    } while (0)

int main(void) {
    lc_window *window = NULL;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lr_renderer *renderer = NULL;
    lr_mesh *ground = NULL;
    lr_mesh *cube = NULL;
    lr_mesh *ball = NULL;
    lr_material *ground_mat = NULL;
    lr_material *tex_mat = NULL;
    lr_material *red_mat = NULL;
    lr_material *blue_mat = NULL;
    lr_material *gold_mat = NULL;
    lc_image *texture = NULL;
    lc_image_view *texture_view = NULL;
    lc_sampler *texture_sampler = NULL;
    lr_camera camera;
    lc_window_desc window_desc;
    lc_device_desc device_desc = { 0 };
    lc_swapchain_desc swapchain_desc;
    unsigned long frame = 0;
    int exit_code = 1;
    static const float y_axis[3] = { 0.0f, 1.0f, 0.0f };

    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    window_desc.title = "Luma Renderer Scene";
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

    /* Meshes: shared across objects (one GPU upload each). */
    CHECK_LR(lr_mesh_create_plane(renderer, 12.0f, 12.0f, &ground),
             "lr_mesh_create_plane");
    CHECK_LR(lr_mesh_create_cube(renderer, 1.0f, &cube), "lr_mesh_create_cube");
    CHECK_LR(lr_mesh_create_sphere(renderer, 0.7f, 24, 12, &ball),
             "lr_mesh_create_sphere");

    /* Checkerboard texture for the hero cube. */
    fill_checkerboard();
    {
        lc_image_desc image_desc;
        lc_image_upload_desc upload;
        lc_image_view_desc view_desc;
        lc_sampler_desc sampler_desc;
        lc_device_limits limits;
        lc_result res;

        memset(&image_desc, 0, sizeof(image_desc));
        image_desc.type = LC_IMAGE_TYPE_2D;
        image_desc.format = LC_FORMAT_RGBA8_UNORM;
        image_desc.width = TEX_W;
        image_desc.height = TEX_H;
        image_desc.depth = 1;
        image_desc.mip_levels = 0;
        image_desc.array_layers = 1;
        image_desc.usage = LC_IMAGE_USAGE_SAMPLED |
                           LC_IMAGE_USAGE_TRANSFER_SRC |
                           LC_IMAGE_USAGE_TRANSFER_DST;
        image_desc.samples = LC_SAMPLE_COUNT_1;
        res = lc_image_create(device, &image_desc, &texture);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_create(texture)");
        }
        upload.mip_level = 0;
        upload.array_layer = 0;
        upload.width = TEX_W;
        upload.height = TEX_H;
        upload.depth = 1;
        upload.data = s_texels;
        upload.data_size = sizeof(s_texels);
        res = lc_image_write(texture, &upload);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_write(texture)");
        }
        res = lc_image_generate_mipmaps(texture);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_generate_mipmaps");
        }
        memset(&view_desc, 0, sizeof(view_desc));
        view_desc.type = LC_IMAGE_VIEW_2D;
        view_desc.aspect = LC_IMAGE_ASPECT_COLOR;
        view_desc.mip_level_count = lc_image_get_mip_levels(texture);
        view_desc.array_layer_count = 1;
        res = lc_image_view_create(texture, &view_desc, &texture_view);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_view_create(texture)");
        }
        lc_device_get_limits(device, &limits);
        memset(&sampler_desc, 0, sizeof(sampler_desc));
        sampler_desc.min_filter = LC_FILTER_LINEAR;
        sampler_desc.mag_filter = LC_FILTER_LINEAR;
        sampler_desc.mipmap_mode = LC_MIPMAP_MODE_LINEAR;
        sampler_desc.address_u = LC_ADDRESS_REPEAT;
        sampler_desc.address_v = LC_ADDRESS_REPEAT;
        sampler_desc.max_anisotropy = limits.max_sampler_anisotropy;
        res = lc_sampler_create(device, &sampler_desc, &texture_sampler);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_sampler_create(texture)");
        }
    }

    /* Materials: flat colors over the fallback texture, one textured. */
    {
        lr_unlit_material_desc mdesc;

        memset(&mdesc, 0, sizeof(mdesc));
        mdesc.color[0] = 0.25f;
        mdesc.color[1] = 0.45f;
        mdesc.color[2] = 0.25f;
        mdesc.color[3] = 1.0f;
        CHECK_LR(lr_material_create_unlit(renderer, &mdesc, &ground_mat),
                 "ground material");
        mdesc.color[0] = 1.0f;
        mdesc.color[1] = 1.0f;
        mdesc.color[2] = 1.0f;
        mdesc.base_color_texture = texture_view;
        mdesc.sampler = texture_sampler;
        CHECK_LR(lr_material_create_unlit(renderer, &mdesc, &tex_mat),
                 "textured material");
        mdesc.color[0] = 0.85f;
        mdesc.color[1] = 0.25f;
        mdesc.color[2] = 0.22f;
        mdesc.base_color_texture = NULL;
        mdesc.sampler = NULL;
        CHECK_LR(lr_material_create_unlit(renderer, &mdesc, &red_mat),
                 "red material");
        mdesc.color[0] = 0.25f;
        mdesc.color[1] = 0.45f;
        mdesc.color[2] = 0.90f;
        CHECK_LR(lr_material_create_unlit(renderer, &mdesc, &blue_mat),
                 "blue material");
        mdesc.color[0] = 0.95f;
        mdesc.color[1] = 0.75f;
        mdesc.color[2] = 0.30f;
        CHECK_LR(lr_material_create_unlit(renderer, &mdesc, &gold_mat),
                 "gold material");
    }

    printf("LumaC %s\n", lc_get_version_string());
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("Renderer scene: plane + 3 cubes + sphere. Close to exit.\n");

    while (!lc_window_should_close(window)) {
        uint32_t w;
        uint32_t h;
        lc_command_encoder *enc = NULL;
        lc_render_swapchain_pass_desc spass;
        lr_draw_item item;
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

        /* Three-line frame body: begin, submit all, render. */
        if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
            fprintf(stderr, "lr_renderer_begin failed\n");
            goto cleanup;
        }
        memset(&item, 0, sizeof(item));
        lr_transform_identity(&item.transform);
        item.transform.position[1] = -0.51f;
        item.mesh = ground;
        item.material = ground_mat;
        if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
            fprintf(stderr, "submit ground failed\n");
            goto cleanup;
        }
        item.mesh = cube;
        item.material = tex_mat;
        item.transform.position[0] = -1.7f;
        item.transform.position[1] = 0.5f;
        item.transform.position[2] = 0.0f;
        lr_quat_from_axis_angle(y_axis, t, item.transform.rotation);
        if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
            fprintf(stderr, "submit cube 1 failed\n");
            goto cleanup;
        }
        item.material = red_mat;
        item.transform.position[0] = 0.1f;
        item.transform.position[1] = 0.65f;
        item.transform.position[2] = -1.3f;
        item.transform.scale[0] = 1.3f;
        item.transform.scale[1] = 1.3f;
        item.transform.scale[2] = 1.3f;
        lr_quat_from_axis_angle(y_axis, -t * 0.7f, item.transform.rotation);
        if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
            fprintf(stderr, "submit cube 2 failed\n");
            goto cleanup;
        }
        item.material = blue_mat;
        item.transform.position[0] = 1.8f;
        item.transform.position[1] = 0.4f;
        item.transform.position[2] = 0.7f;
        item.transform.scale[0] = 0.8f;
        item.transform.scale[1] = 0.8f;
        item.transform.scale[2] = 0.8f;
        lr_quat_from_axis_angle(y_axis, t * 1.3f, item.transform.rotation);
        if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
            fprintf(stderr, "submit cube 3 failed\n");
            goto cleanup;
        }
        item.mesh = ball;
        item.material = gold_mat;
        lr_transform_identity(&item.transform);
        item.transform.position[0] = 0.0f;
        item.transform.position[1] = 1.35f;
        item.transform.position[2] = 1.9f;
        if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
            fprintf(stderr, "submit sphere failed\n");
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
    lr_material_destroy(gold_mat);
    lr_material_destroy(blue_mat);
    lr_material_destroy(red_mat);
    lr_material_destroy(tex_mat);
    lr_material_destroy(ground_mat);
    lr_mesh_destroy(ball);
    lr_mesh_destroy(cube);
    lr_mesh_destroy(ground);
    lr_renderer_destroy(renderer);
    lc_sampler_destroy(texture_sampler);
    lc_image_view_destroy(texture_view);
    lc_image_destroy(texture);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return exit_code;
}
