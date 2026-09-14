/*
 * Phase 17 example: image-based lighting. A procedural HDR sky
 * (warm horizon, cool zenith, one bright sun disk) lights a
 * dielectric/rough/copper sphere trio on a PBR ground plane with
 * no direct lights at all; the environment yaw drifts slowly to
 * show params-only rotation (no reprocessing — rebuilds stays 0
 * after the first frame).
 *
 * Each frame is: begin -> submits -> render_shadows -> render_scene
 * (HDR + sky) -> render_output (exposure + ACES into the swapchain)
 * -> present.
 *
 * Usage: ibl_scene [--frames N]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>

#include "../../tools/png_mini.h"

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

/* Procedural HDR equirect: cool zenith, warm horizon, dark ground,
 * one hard sun disk toward +X (u = 0.5, slightly above horizon).
 * Row 0 = v 0 = +Y pole; u = atan2(z,x)/2pi + 0.5. */
static void paint_sky(double u, double v, float out[4]) {
    double du = u - 0.5;
    double dv = (v - 0.42) * 0.5;
    double r;
    double g;
    double b;
    double t;

    if (du > 0.5) {
        du -= 1.0;
    }
    if (du < -0.5) {
        du += 1.0;
    }
    if (v < 0.5) {
        /* Zenith -> horizon. */
        t = v / 0.5;
        r = 0.25 + (1.40 - 0.25) * t;
        g = 0.45 + (0.90 - 0.45) * t;
        b = 0.90 + (0.55 - 0.90) * t;
    } else {
        /* Horizon -> nadir. */
        t = (v - 0.5) / 0.5;
        r = 1.40 + (0.12 - 1.40) * t;
        g = 0.90 + (0.09 - 0.90) * t;
        b = 0.55 + (0.07 - 0.55) * t;
    }
    if (du * du + dv * dv < 0.0012) {
        r += 8.0;
        g += 8.0;
        b += 7.5;
    }
    out[0] = (float)r;
    out[1] = (float)g;
    out[2] = (float)b;
    out[3] = 1.0f;
}

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
    static const float look_target[3] = { 0.0f, 1.2f, 0.0f };
    static const float ground_base[4] = { 0.55f, 0.55f, 0.57f, 1.0f };
    static const float diel_base[4] = { 0.9f, 0.9f, 0.92f, 1.0f };
    static const float copper_base[4] = { 0.95f, 0.45f, 0.20f, 1.0f };
    lc_window *window = NULL;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lr_renderer *renderer = NULL;
    lc_image *sky_image = NULL;
    lc_image_view *sky_view = NULL;
    lc_sampler *sky_sampler = NULL;
    lr_environment *env = NULL;
    lr_mesh *ground_mesh = NULL;
    lr_mesh *ball_mesh = NULL;
    lr_material *ground_mat = NULL;
    lr_material *diel_smooth = NULL;
    lr_material *diel_rough = NULL;
    lr_material *copper = NULL;
    lr_camera camera;
    lc_window_desc window_desc;
    lc_device_desc device_desc = { 0 };
    lc_swapchain_desc swapchain_desc = { 0 };
    /* Screenshot target (offscreen LDR, capture-capable): renders the
     * same output the swapchain shows, then reads back publicly. */
    lc_image *shot_image = NULL;
    lc_image_view *shot_view = NULL;
    lc_render_target *shot_target = NULL;
    uint32_t shot_w = 0;
    uint32_t shot_h = 0;
    unsigned long frame = 0;
    unsigned long max_frames = 0;
    const char *screenshot_path = NULL;
    int exit_code = 1;
    int i;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = (unsigned long)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else {
            fprintf(stderr, "usage: ibl_scene [--frames N] "
                            "[--screenshot out.png]\n");
            return 1;
        }
    }

    memset(&window_desc, 0, sizeof(window_desc));
    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    window_desc.title = "Luma IBL Scene";
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

    /* Procedural HDR sky upload (RGBA32F sampled source, borrowed
     * by the environment for its whole lifetime). */
    {
        static const uint32_t sw = 128;
        static const uint32_t sh = 64;
        float *pixels = (float *)malloc((size_t)sw * sh * 4u *
                                        sizeof(float));
        lc_image_desc idesc;
        lc_image_upload_desc upload;
        lc_image_view_desc vdesc;
        lc_sampler_desc sdesc;
        lr_environment_desc edesc;
        uint32_t x;
        uint32_t y;

        if (pixels == NULL) {
            FAIL_CLEANUP("sky pixels");
        }
        for (y = 0; y < sh; y++) {
            for (x = 0; x < sw; x++) {
                paint_sky(((double)x + 0.5) / (double)sw,
                          (double)y / (double)(sh - 1),
                          pixels + ((size_t)y * sw + x) * 4u);
            }
        }
        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_RGBA32_FLOAT;
        idesc.width = sw;
        idesc.height = sh;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        idesc.usage =
            LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_DST;
        idesc.samples = LC_SAMPLE_COUNT_1;
        CHECK_LC(lc_image_create(device, &idesc, &sky_image),
                 "sky image");
        memset(&upload, 0, sizeof(upload));
        upload.width = sw;
        upload.height = sh;
        upload.depth = 1;
        upload.data = pixels;
        upload.data_size = (uint64_t)sw * sh * 4u * sizeof(float);
        CHECK_LC(lc_image_write(sky_image, &upload), "sky upload");
        /* Free only after the upload consumed the bytes (the write
         * memcpys synchronously; freeing first is use-after-free). */
        free(pixels);
        pixels = NULL;
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.mip_level_count = 1;
        vdesc.array_layer_count = 1;
        CHECK_LC(lc_image_view_create(sky_image, &vdesc, &sky_view),
                 "sky view");
        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.min_filter = LC_FILTER_LINEAR;
        sdesc.mag_filter = LC_FILTER_LINEAR;
        sdesc.mipmap_mode = LC_MIPMAP_MODE_LINEAR;
        sdesc.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
        sdesc.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
        sdesc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
        sdesc.max_anisotropy = 1.0f;
        CHECK_LC(lc_sampler_create(device, &sdesc, &sky_sampler),
                 "sky sampler");
        memset(&edesc, 0, sizeof(edesc));
        edesc.environment_texture = sky_view;
        edesc.sampler = sky_sampler;
        edesc.intensity = 1.0f;
        edesc.rotation = 0.0f;
        CHECK_LR(lr_environment_create(renderer, &edesc, &env),
                 "lr_environment_create");
        CHECK_LR(lr_renderer_set_environment(renderer, env),
                 "lr_renderer_set_environment");
        CHECK_LR(lr_renderer_set_exposure(renderer, 0.0f),
                 "lr_renderer_set_exposure");
        CHECK_LR(lr_renderer_set_tonemap_operator(renderer,
                                                  LR_TONEMAP_ACES),
                 "lr_renderer_set_tonemap_operator");
    }

    CHECK_LR(lr_mesh_create_plane(renderer, 14.0f, 14.0f, &ground_mesh),
             "ground mesh");
    CHECK_LR(lr_mesh_create_sphere(renderer, 1.0f, 32, 16, &ball_mesh),
             "ball mesh");
    make_pbr(renderer, ground_base, 0.0f, 0.9f, &ground_mat);
    make_pbr(renderer, diel_base, 0.0f, 0.05f, &diel_smooth);
    make_pbr(renderer, diel_base, 0.0f, 0.6f, &diel_rough);
    make_pbr(renderer, copper_base, 1.0f, 0.08f, &copper);
    if (ground_mat == NULL || diel_smooth == NULL ||
        diel_rough == NULL || copper == NULL) {
        FAIL_CLEANUP("scene materials");
    }

    printf("LumaC %s\n", lc_get_version_string());
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("IBL scene: pure environment light, no direct lights. "
           "Close to exit.\n");

    while (!lc_window_should_close(window)) {
        uint32_t w;
        uint32_t h;
        lc_command_encoder *enc = NULL;
        lc_render_swapchain_pass_desc spass;
        lc_result res;
        float t;
        float eye[3];
        float ambient[3] = { 0.0f, 0.0f, 0.0f };
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

        t = (float)frame * 0.016f;

        eye[0] = 0.0f;
        eye[1] = 3.2f;
        eye[2] = 8.5f;
        CHECK_LR(lr_camera_look_at(&camera, eye, look_target, up),
                 "lr_camera_look_at");

        /* Slow yaw drift: runtime parameters only, the derived
         * cube/irradiance/prefilter maps are never rebuilt. */
        CHECK_LR(lr_environment_set_rotation(env, t * 0.05f),
                 "lr_environment_set_rotation");

        if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
            fprintf(stderr, "lr_renderer_begin failed\n");
            goto cleanup;
        }

        /* No direct lights: the sky is the only light source. */
        submit_prop(renderer, ground_mesh, ground_mat, 0.0f, 0.0f, 0.0f,
                    &ok);
        submit_prop(renderer, ball_mesh, diel_smooth, -2.2f, 1.0f, 0.0f,
                    &ok);
        submit_prop(renderer, ball_mesh, copper, 0.0f, 1.0f, 0.0f, &ok);
        submit_prop(renderer, ball_mesh, diel_rough, 2.2f, 1.0f, 0.0f,
                    &ok);
        if (!ok) {
            fprintf(stderr, "submit props failed\n");
            goto cleanup;
        }
        lr_renderer_set_ambient(renderer, ambient);
        if (lr_renderer_render_shadows(renderer, enc) != LR_SUCCESS) {
            fprintf(stderr, "lr_renderer_render_shadows failed\n");
            goto cleanup;
        }
        /* HDR scene (PBR + sky), sized to the swapchain image. */
        if (lr_renderer_render_scene(renderer, enc, w, h) != LR_SUCCESS) {
            fprintf(stderr, "lr_renderer_render_scene failed\n");
            goto cleanup;
        }
        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.clear_color[0] = 0.0f;
        spass.clear_color[1] = 0.0f;
        spass.clear_color[2] = 0.0f;
        spass.clear_color[3] = 1.0f;
        spass.depth_load_op = LC_LOAD_OP_CLEAR;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        CHECK_LC(lc_encoder_begin_swapchain_pass(enc, swapchain, &spass),
                 "swapchain pass begin");
        {
            lr_result opr = lr_renderer_render_output(
                renderer, enc,
                lc_swapchain_get_render_target(swapchain));

            if (opr != LR_SUCCESS) {
                fprintf(stderr, "lr_renderer_render_output failed (%d)\n",
                        (int)opr);
                goto cleanup;
            }
        }
        CHECK_LC(lc_encoder_end_render_pass(enc), "pass end");
        /* Screenshot leg (optional, own pass after the swapchain
         * pass ends — passes cannot nest). */
        if (screenshot_path != NULL && frame == 0) {
            lc_image_desc idesc;
            lc_image_view_desc vdesc;
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment att;
            lc_render_pass_desc pdesc;
            lc_render_color_attachment catt;

            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = w;
            idesc.height = h;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                          LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                          LC_IMAGE_USAGE_TRANSFER_SRC |
                          LC_IMAGE_USAGE_TRANSFER_DST;
            idesc.samples = LC_SAMPLE_COUNT_1;
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.mip_level_count = 1;
            vdesc.array_layer_count = 1;
            if (lc_image_create(device, &idesc, &shot_image) !=
                    LC_SUCCESS ||
                lc_image_view_create(shot_image, &vdesc, &shot_view) !=
                    LC_SUCCESS) {
                fprintf(stderr, "screenshot target failed\n");
                goto cleanup;
            }
            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = w;
            tdesc.height = h;
            att.view = shot_view;
            tdesc.color_attachments = &att;
            tdesc.color_attachment_count = 1;
            tdesc.depth_stencil_attachment = NULL;
            if (lc_render_target_create(device, &tdesc, &shot_target) !=
                LC_SUCCESS) {
                fprintf(stderr, "screenshot target failed\n");
                goto cleanup;
            }
            shot_w = w;
            shot_h = h;
            memset(&catt, 0, sizeof(catt));
            catt.view = shot_view;
            catt.load_op = LC_LOAD_OP_CLEAR;
            catt.store_op = LC_STORE_OP_STORE;
            memset(&pdesc, 0, sizeof(pdesc));
            pdesc.color_attachments = &catt;
            pdesc.color_attachment_count = 1;
            pdesc.width = w;
            pdesc.height = h;
            pdesc.depth_attachment = NULL;
            CHECK_LC(lc_encoder_begin_render_pass(enc, &pdesc),
                     "screenshot pass begin");
            CHECK_LR(lr_renderer_render_output(renderer, enc,
                                               shot_target),
                     "screenshot output");
            CHECK_LC(lc_encoder_end_render_pass(enc), "screenshot end");
            shot_w = w;
            shot_h = h;
        }
        lr_renderer_end(renderer);

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
        /* Screenshot readback AFTER present (never mid-recording:
         * layout tracking describes recorded passes, and the GPU
         * has not executed them until the frame submits). */
        if (screenshot_path != NULL && frame == 0 &&
            shot_image != NULL) {
            lc_image_readback_desc rd;
            lc_image_readback_info info;
            unsigned char *rows = NULL;

            memset(&rd, 0, sizeof(rd));
            memset(&info, 0, sizeof(info));
            CHECK_LC(lc_image_query_readback(shot_image, &rd, &info),
                     "screenshot query");
            rows = (unsigned char *)malloc(info.size);
            if (rows == NULL) {
                fprintf(stderr, "screenshot scratch failed\n");
                goto cleanup;
            }
            CHECK_LC(lc_image_readback(shot_image, &rd, rows,
                                       info.size, NULL),
                     "screenshot readback");
            /* RGBA -> RGB rows for the PNG writer. */
            {
                unsigned char *rgb = NULL;
                uint32_t yy;

                rgb = (unsigned char *)malloc(
                    (size_t)info.width * info.height * 3u);
                if (rgb == NULL) {
                    free(rows);
                    fprintf(stderr, "screenshot rgb failed\n");
                    goto cleanup;
                }
                for (yy = 0; yy < info.height; yy++) {
                    uint32_t xx;

                    for (xx = 0; xx < info.width; xx++) {
                        const unsigned char *src =
                            rows + ((size_t)yy * info.width + xx) * 4u;
                        unsigned char *d =
                            rgb + ((size_t)yy * info.width + xx) * 3u;

                        d[0] = src[0];
                        d[1] = src[1];
                        d[2] = src[2];
                    }
                }
                free(rows);
                if (lpng_write(screenshot_path, info.width,
                               info.height, 3, rgb,
                               (size_t)info.width * 3u) != 0) {
                    free(rgb);
                    fprintf(stderr, "screenshot write failed\n");
                    goto cleanup;
                }
                free(rgb);
                printf("screenshot: %s (%ux%u)\n", screenshot_path,
                       info.width, info.height);
            }
        }
        if ((frame % 120u) == 0u) {
            lr_render_stats stats;

            lr_renderer_get_stats(renderer, &stats);
            printf("frame %lu: draws=%u tris=%u ibl=%u sky=%u "
                   "tonemap=%u rebuilds=%u\n",
                   frame, stats.draw_calls, stats.triangles,
                   stats.ibl_enabled, stats.sky_draw_calls,
                   stats.tonemap_passes, stats.environment_rebuilds);
        }
        frame++;
    }

    exit_code = 0;

cleanup:
    /* Drain the last frame before tearing down GPU objects. */
    if (device != NULL) {
        lc_device_wait_idle(device);
    }
    if (renderer != NULL) {
        lr_renderer_set_environment(renderer, NULL);
    }
    lr_environment_destroy(env);
    lc_render_target_destroy(shot_target);
    lc_image_view_destroy(shot_view);
    lc_image_destroy(shot_image);
    lc_sampler_destroy(sky_sampler);
    lc_image_view_destroy(sky_view);
    lc_image_destroy(sky_image);
    lr_material_destroy(copper);
    lr_material_destroy(diel_rough);
    lr_material_destroy(diel_smooth);
    lr_material_destroy(ground_mat);
    lr_mesh_destroy(ball_mesh);
    lr_mesh_destroy(ground_mesh);
    lr_renderer_destroy(renderer);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return exit_code;
}
