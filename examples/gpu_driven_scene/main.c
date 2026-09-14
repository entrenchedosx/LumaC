/*
 * Phase 21 example: GPU-driven scene with frustum culling.
 *
 * Thousands of PBR cubes render through one camera with per-frame
 * CPU submission and GPU visibility:
 *
 *   submit all instances (CPU, measured)
 *   -> async instance upload (no stall)
 *   -> compute frustum culling + compaction (GPU)
 *   -> indexed indirect draws, one per mesh/material group (GPU)
 *
 * Usage:
 *   gpu_driven_scene [--frames N] [--instances N]
 *                    [--cpu-culling | --gpu-culling]
 *                    [--screenshot out.png]
 *
 * --frames 0 (default) runs until the window closes; N > 0 runs
 * exactly N frames (benchmark/CI mode). --instances defaults to
 * 20000 (100000 for the AAA stress point). --screenshot renders
 * frame 0 to an offscreen target and writes a PNG through the
 * public readback path.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
            fprintf(stderr, "%s failed\n", what);                          \
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

/* Deterministic 0..1 hash (no rand(): runs are reproducible). */
static float hash01(uint32_t n) {
    uint32_t x = n * 2654435761u;

    x ^= x >> 15u;
    x *= 2246822519u;
    x ^= x >> 13u;
    return (float)(x % 10000u) / 10000.0f;
}

int main(int argc, char **argv) {
    lc_window *window = NULL;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lr_renderer *renderer = NULL;
    lr_mesh *cube = NULL;
    lr_material *gray_mat = NULL;
    lr_material *red_mat = NULL;
    lc_command_encoder *enc = NULL;
    /* Screenshot offscreen target (frame 0 only, public path). */
    lc_image *shot_image = NULL;
    lc_image_view *shot_view = NULL;
    lc_image *shot_depth = NULL;
    lc_image_view *shot_depth_view = NULL;
    lc_render_target *shot_target = NULL;
    lr_camera camera;
    lr_draw_item item;
    lr_light light;
    lc_window_desc window_desc;
    lc_device_desc device_desc = { 0 };
    lc_swapchain_desc swapchain_desc = { 0 };
    lc_render_swapchain_pass_desc spass;
    lr_renderer_desc rdesc;
    lr_pbr_material_desc matdesc;
    unsigned long frame = 0;
    unsigned long max_frames = 0;
    unsigned long want_instances = 20000;
    int use_gpu = 1;
    int use_validation = 1;
    const char *screenshot_path = NULL;
    int exit_code = 1;
    int i;
    static const float y_axis[3] = { 0.0f, 1.0f, 0.0f };
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    static const float look[3] = { 0.0f, 0.0f, 0.0f };
    uint64_t submit_ticks = 0;
    uint64_t perf_freq = lc_clock_frequency();

    if (perf_freq == 0) {
        perf_freq = 1;
    }

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--instances") == 0 && i + 1 < argc) {
            want_instances = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--cpu-culling") == 0) {
            use_gpu = 0;
        } else if (strcmp(argv[i], "--gpu-culling") == 0) {
            use_gpu = 1;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--no-validation") == 0) {
            use_validation = 0;
        } else {
            fprintf(stderr,
                    "usage: gpu_driven_scene [--frames N] "
                    "[--instances N] [--cpu-culling | --gpu-culling] "
                    "[--no-validation] [--screenshot out.png]\n");
            return 1;
        }
    }
    if (want_instances < 1) {
        want_instances = 1;
    }
    if (want_instances > 200000) {
        want_instances = 200000;
    }

    memset(&window_desc, 0, sizeof(window_desc));
    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    window_desc.title = "Luma GPU-Driven Scene";
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
        fprintf(stderr, "lc_device_create failed\n");
        FAIL_CLEANUP("lc_device_create");
    }
    {
        lc_compute_capabilities caps;

        memset(&caps, 0, sizeof(caps));
        lc_device_get_compute_capabilities(device, &caps);
        printf("compute=%d indirect=%d multi=%d indirect_count=%d "
               "dedicated_compute=%d\n",
               caps.compute_supported, caps.indirect_draw_supported,
               caps.multi_draw_indirect, caps.indirect_count,
               caps.dedicated_compute);
        if (use_gpu &&
            (!caps.compute_supported ||
             !caps.indirect_draw_supported)) {
            fprintf(stderr, "gpu culling unavailable, failing\n");
            FAIL_CLEANUP("capabilities");
        }
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
    rdesc.max_objects = (uint32_t)want_instances + 8192u;
    rdesc.ambient_light[0] = 0.45f;
    rdesc.ambient_light[1] = 0.45f;
    rdesc.ambient_light[2] = 0.5f;
    if (lr_renderer_create(&rdesc, &renderer) != LR_SUCCESS) {
        FAIL_CLEANUP("lr_renderer_create");
    }
    if (lr_mesh_create_cube(renderer, 1.0f, &cube) != LR_SUCCESS) {
        FAIL_CLEANUP("cube");
    }
    {
        static const float gray[4] = { 0.75f, 0.75f, 0.78f, 1.0f };
        static const float red[4] = { 0.75f, 0.22f, 0.18f, 1.0f };

        memset(&matdesc, 0, sizeof(matdesc));
        memcpy(matdesc.base_color_factor, gray, sizeof(gray));
        matdesc.metallic_factor = 0.0f;
        matdesc.roughness_factor = 0.55f;
        matdesc.normal_scale = 1.0f;
        matdesc.occlusion_strength = 1.0f;
        matdesc.alpha_mode = LR_ALPHA_OPAQUE;
        if (lr_material_create_pbr(renderer, &matdesc, &gray_mat) !=
            LR_SUCCESS) {
            FAIL_CLEANUP("gray material");
        }
        memcpy(matdesc.base_color_factor, red, sizeof(red));
        matdesc.metallic_factor = 0.6f;
        matdesc.roughness_factor = 0.35f;
        if (lr_material_create_pbr(renderer, &matdesc, &red_mat) !=
            LR_SUCCESS) {
            FAIL_CLEANUP("red material");
        }
    }
    if (lr_renderer_set_render_mode(
            renderer, use_gpu ? LR_RENDER_MODE_GPU_DRIVEN
                              : LR_RENDER_MODE_CPU) != LR_SUCCESS) {
        FAIL_CLEANUP("render mode");
    }

    lr_camera_init(&camera);
    if (lr_camera_set_perspective(&camera, 1.0471976f, 4.0f / 3.0f,
                                  0.5f, 600.0f) != LR_SUCCESS) {
        FAIL_CLEANUP("camera");
    }
    memset(&light, 0, sizeof(light));
    light.type = LR_LIGHT_DIRECTIONAL;
    light.color[0] = 1.0f;
    light.color[1] = 0.96f;
    light.color[2] = 0.9f;
    light.intensity = 2.2f;
    light.direction[0] = 0.35f;
    light.direction[1] = -1.0f;
    light.direction[2] = 0.45f;

    /* Grid sized for N instances (roughly cubic). */
    {
        unsigned long n = want_instances;
        unsigned long nx = 1;
        unsigned long ny = 1;
        unsigned long nz = 1;

        while (nx * ny * nz < n) {
            if (nx <= ny && nx <= nz) {
                nx++;
            } else if (ny <= nz) {
                ny++;
            } else {
                nz++;
            }
        }
        printf("scene: %lu instances (%lux%lux%lu grid), mode=%s\n", n,
               nx, ny, nz, use_gpu ? "gpu-driven" : "cpu");
    }

    while (!lc_window_should_close(window)) {
        lc_result res;
        uint32_t w;
        uint32_t h;
        float t;
        float radius;
        float eye[3];
        unsigned long n;
        unsigned long nx = 1;
        unsigned long ny = 1;
        unsigned long nz = 1;
        unsigned long id = 0;
        unsigned long ix;
        unsigned long iy;
        unsigned long iz;
        uint64_t submit0;
        uint64_t submit1;

        if (max_frames > 0 && frame >= max_frames) {
            break;
        }
        lc_poll_events();
        w = lc_window_get_width(window);
        h = lc_window_get_height(window);
        if (w == 0 || h == 0) {
            continue;
        }
        n = want_instances;
        while (nx * ny * nz < n) {
            if (nx <= ny && nx <= nz) {
                nx++;
            } else if (ny <= nz) {
                ny++;
            } else {
                nz++;
            }
        }
        t = (float)frame * 0.008f;
        radius = (float)nx * 2.2f;
        eye[0] = cosf(t) * radius;
        eye[1] = radius * 0.45f;
        eye[2] = sinf(t) * radius;
        if (lr_camera_look_at(&camera, eye, look, up) != LR_SUCCESS) {
            FAIL_CLEANUP("look_at");
        }

        res = lc_begin_frame(swapchain);
        if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            res = lc_swapchain_recreate(swapchain, w, h);
            if (res != LC_SUCCESS && res != LC_ERROR_ZERO_EXTENT) {
                FAIL_CLEANUP("recreate");
            }
            continue;
        }
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("begin_frame");
        }
        CHECK_LC(lc_swapchain_get_encoder(swapchain, &enc),
                 "get_encoder");

        if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
            FAIL_CLEANUP("renderer_begin");
        }
        if (lr_renderer_submit_light(renderer, &light) != LR_SUCCESS) {
            FAIL_CLEANUP("submit_light");
        }
        /* Submit all instances (CPU submission time measured). */
        submit0 = lc_clock_now();
        for (iz = 0; iz < nz && id < n; iz++) {
            for (iy = 0; iy < ny && id < n; iy++) {
                for (ix = 0; ix < nx && id < n; ix++) {
                    float s = 0.6f + 0.9f * hash01((uint32_t)id);

                    memset(&item, 0, sizeof(item));
                    lr_transform_identity(&item.transform);
                    item.transform.position[0] =
                        ((float)ix - (float)nx * 0.5f) * 2.5f;
                    item.transform.position[1] =
                        ((float)iy - (float)ny * 0.5f) * 2.5f;
                    item.transform.position[2] =
                        ((float)iz - (float)nz * 0.5f) * 2.5f;
                    item.transform.scale[0] = s;
                    item.transform.scale[1] = s;
                    item.transform.scale[2] = s;
                    lr_quat_from_axis_angle(y_axis,
                                            t * (0.2f + hash01(
                                                            (uint32_t)id +
                                                            7u)),
                                            item.transform.rotation);
                    item.mesh = cube;
                    item.material =
                        (id % 5u == 0u) ? red_mat : gray_mat;
                    item.casts_shadow = 0;
                    item.receives_shadow = 1;
                    if (lr_renderer_submit(renderer, &item) !=
                        LR_SUCCESS) {
                        FAIL_CLEANUP("submit");
                    }
                    id++;
                }
            }
        }
        submit1 = lc_clock_now();
        submit_ticks += submit1 - submit0;

        /* GPU visibility runs before the pass opens (render_scene
         * would do this internally; the legacy path prepares
         * explicitly). */
        if (use_gpu &&
            lr_renderer_prepare_gpu(renderer, enc) != LR_SUCCESS) {
            FAIL_CLEANUP("prepare_gpu");
        }

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.clear_color[0] = 0.03f;
        spass.clear_color[1] = 0.04f;
        spass.clear_color[2] = 0.07f;
        spass.clear_color[3] = 1.0f;
        spass.depth_load_op = LC_LOAD_OP_CLEAR;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        CHECK_LC(lc_encoder_begin_swapchain_pass(enc, swapchain, &spass),
                 "swapchain pass begin");
        if (lr_renderer_render(
                renderer, enc,
                lc_swapchain_get_render_target(swapchain)) !=
            LR_SUCCESS) {
            FAIL_CLEANUP("render");
        }
        CHECK_LC(lc_encoder_end_render_pass(enc), "pass end");

        if (screenshot_path != NULL && frame == 0) {
            /* Second recording of the same submits into an
             * offscreen target (prepare is idempotent; groups
             * persist), then public readback + PNG. */
            lc_image_desc idesc;
            lc_image_view_desc vdesc;
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment att;
            lc_render_pass_desc pdesc;
            lc_render_color_attachment catt;
            lc_render_depth_attachment datt;
            lc_image_readback_desc rbdesc;
            lc_image_readback_info rbinfo;
            unsigned char *rows = NULL;
            unsigned char *rgb = NULL;
            size_t need = 0;
            uint32_t yy;

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
                fprintf(stderr, "screenshot color failed\n");
                FAIL_CLEANUP("screenshot color");
            }
            idesc.format = LC_FORMAT_D32_FLOAT;
            idesc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL;
            if (lc_image_create(device, &idesc, &shot_depth) !=
                    LC_SUCCESS) {
                fprintf(stderr, "screenshot depth failed\n");
                FAIL_CLEANUP("screenshot depth");
            }
            vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
            if (lc_image_view_create(shot_depth, &vdesc,
                                     &shot_depth_view) != LC_SUCCESS) {
                fprintf(stderr, "screenshot depth view failed\n");
                FAIL_CLEANUP("screenshot depth view");
            }
            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = w;
            tdesc.height = h;
            att.view = shot_view;
            tdesc.color_attachments = &att;
            tdesc.color_attachment_count = 1;
            tdesc.depth_stencil_attachment = shot_depth_view;
            {
                lc_result trr =
                    lc_render_target_create(device, &tdesc, &shot_target);

                if (trr != LC_SUCCESS) {
                    fprintf(stderr,
                            "screenshot render target failed (%d)\n",
                            trr);
                    FAIL_CLEANUP("screenshot render target");
                }
            }
            memset(&catt, 0, sizeof(catt));
            catt.view = shot_view;
            catt.load_op = LC_LOAD_OP_CLEAR;
            catt.store_op = LC_STORE_OP_STORE;
            catt.clear_color[0] = 0.03f;
            catt.clear_color[1] = 0.04f;
            catt.clear_color[2] = 0.07f;
            catt.clear_color[3] = 1.0f;
            memset(&datt, 0, sizeof(datt));
            datt.view = shot_depth_view;
            datt.depth_load_op = LC_LOAD_OP_CLEAR;
            datt.depth_store_op = LC_STORE_OP_DONT_CARE;
            datt.clear_depth = 1.0f;
            datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
            datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
            memset(&pdesc, 0, sizeof(pdesc));
            pdesc.color_attachments = &catt;
            pdesc.color_attachment_count = 1;
            pdesc.depth_attachment = &datt;
            pdesc.width = w;
            pdesc.height = h;
            CHECK_LC(lc_encoder_begin_render_pass(enc, &pdesc),
                     "shot pass begin");
            {
                lr_result srr =
                    lr_renderer_render(renderer, enc, shot_target);

                if (srr != LR_SUCCESS) {
                    fprintf(stderr, "shot render failed (%d)\n", srr);
                    FAIL_CLEANUP("shot render");
                }
            }
            CHECK_LC(lc_encoder_end_render_pass(enc), "shot pass end");
            memset(&rbdesc, 0, sizeof(rbdesc));
            if (lc_image_query_readback(shot_image, &rbdesc,
                                        &rbinfo) != LC_SUCCESS) {
                FAIL_CLEANUP("shot query");
            }
            need = rbinfo.size;
            rows = (unsigned char *)malloc(need);
            rgb = (unsigned char *)malloc((size_t)rbinfo.width *
                                          rbinfo.height * 3u);
            if (rows == NULL || rgb == NULL) {
                free(rows);
                free(rgb);
                FAIL_CLEANUP("shot alloc");
            }
            if (lc_image_readback(shot_image, &rbdesc, rows, need,
                                  NULL) != LC_SUCCESS) {
                free(rows);
                free(rgb);
                FAIL_CLEANUP("shot readback");
            }
            for (yy = 0; yy < rbinfo.height; yy++) {
                uint32_t xx;

                for (xx = 0; xx < rbinfo.width; xx++) {
                    const unsigned char *s =
                        rows + ((size_t)yy * rbinfo.width + xx) * 4u;
                    unsigned char *d =
                        rgb + ((size_t)yy * rbinfo.width + xx) * 3u;

                    d[0] = s[0];
                    d[1] = s[1];
                    d[2] = s[2];
                }
            }
            free(rows);
            if (lpng_write(screenshot_path, rbinfo.width,
                           rbinfo.height, 3, rgb,
                           (size_t)rbinfo.width * 3u) != 0) {
                free(rgb);
                FAIL_CLEANUP("screenshot write");
            }
            free(rgb);
            printf("screenshot wrote %s (%ux%u)\n", screenshot_path,
                   rbinfo.width, rbinfo.height);
        }

        /* The renderer frame spans both recordings (queue + prepared
         * groups stay valid until end). */
        lr_renderer_end(renderer);

        res = lc_end_frame(swapchain);
        if (res == LC_SUBOPTIMAL ||
            res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            res = lc_swapchain_recreate(swapchain, w, h);
            if (res != LC_SUCCESS && res != LC_ERROR_ZERO_EXTENT) {
                FAIL_CLEANUP("recreate");
            }
            continue;
        }
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("end_frame");
        }
        if ((frame % 60u) == 0u) {
            lr_render_stats stats;
            lr_gpu_driven_stats gstats;
            lc_memory_stats mstats;

            lr_renderer_get_stats(renderer, &stats);
            lr_renderer_get_gpu_driven_stats(renderer, &gstats);
            memset(&mstats, 0, sizeof(mstats));
            lc_device_get_memory_stats(device, &mstats);
            printf("frame %lu: submitted=%u visible=%u draws=%u "
                   "tris=%u dispatches=%llu indirect=%llu batches=%llu "
                   "prepare=%.3fms blocks=%llu\n",
                   frame, stats.submitted_objects, stats.visible_objects,
                   stats.draw_calls, stats.triangles,
                   (unsigned long long)gstats.compute_dispatches,
                   (unsigned long long)gstats.indirect_draw_calls,
                   (unsigned long long)gstats.gpu_driven_batches,
                   gstats.cpu_prepare_ms,
                   (unsigned long long)mstats.block_count);
        }
    /* Resource churn every 300 frames (PART 48): recreate one
     * material and swing the instance count while frames are in
     * flight; retirement + group pruning keep it safe. Swings
     * down from the base count so max_objects (base + 8192)
     * always suffices. */
    if (frame > 0 && (frame % 300u) == 0u) {
        lr_pbr_material_desc mdesc;
        static const float alt[4] = { 0.2f, 0.45f, 0.7f, 1.0f };
        static unsigned long base_instances = 0;

        if (base_instances == 0) {
            base_instances = want_instances;
        }

            lr_material_destroy(red_mat);
            red_mat = NULL;
            memset(&mdesc, 0, sizeof(mdesc));
            memcpy(mdesc.base_color_factor, alt, sizeof(alt));
            mdesc.metallic_factor = 0.6f;
            mdesc.roughness_factor = 0.35f;
            mdesc.normal_scale = 1.0f;
            mdesc.occlusion_strength = 1.0f;
            mdesc.alpha_mode = LR_ALPHA_OPAQUE;
            if (lr_material_create_pbr(renderer, &mdesc, &red_mat) !=
                LR_SUCCESS) {
                FAIL_CLEANUP("churn material");
            }
            want_instances = (want_instances >= base_instances)
                                 ? ((base_instances > 5000)
                                        ? base_instances - 5000
                                        : base_instances)
                                 : base_instances;
            printf("churn at frame %lu: instances now %lu\n", frame,
                   want_instances);
        }
        frame++;
    }

    /* Benchmark summary (visible/culled via one test-only
     * download; GPU timing NOT MEASURED — no timestamp path). */
    {
        lr_gpu_driven_stats gstats;
        lc_memory_stats mstats;
        double submit_ms;

        lr_renderer_get_gpu_driven_stats(renderer, &gstats);
        if (use_gpu &&
            lr_renderer_update_gpu_visibility_stats(renderer) ==
                LR_SUCCESS) {
            lr_renderer_get_gpu_driven_stats(renderer, &gstats);
        }
        memset(&mstats, 0, sizeof(mstats));
        lc_device_get_memory_stats(device, &mstats);
        submit_ms = (double)submit_ticks / (double)perf_freq * 1000.0 /
                    (double)(frame > 0 ? frame : 1);
        printf("summary: frames=%lu instances=%lu visible=%llu "
               "culled=%llu\n",
               frame, want_instances,
               (unsigned long long)gstats.instances_visible,
               (unsigned long long)gstats.instances_culled);
        printf("summary: dispatches=%llu indirect_draws=%llu "
               "indirect_commands=%llu batches=%llu "
               "descriptor_sets=%llu\n",
               (unsigned long long)gstats.compute_dispatches,
               (unsigned long long)gstats.indirect_draw_calls,
               (unsigned long long)gstats.indirect_commands,
               (unsigned long long)gstats.gpu_driven_batches,
               (unsigned long long)gstats.descriptor_sets_alive);
        printf("summary: cpu_submit_ms=%.3f cpu_prepare_ms=%.3f "
               "gpu_ms=NOT MEASURED\n",
               submit_ms, gstats.cpu_prepare_ms);
        printf("summary: gpu_buffer_bytes=%llu memory_blocks=%llu\n",
               (unsigned long long)(want_instances * 96u),
               (unsigned long long)mstats.block_count);
    }

    exit_code = 0;

cleanup:
    lc_render_target_destroy(shot_target);
    lc_image_view_destroy(shot_depth_view);
    lc_image_destroy(shot_depth);
    lc_image_view_destroy(shot_view);
    lc_image_destroy(shot_image);
    lr_material_destroy(red_mat);
    lr_material_destroy(gray_mat);
    lr_mesh_destroy(cube);
    lr_renderer_destroy(renderer);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return exit_code;
}
