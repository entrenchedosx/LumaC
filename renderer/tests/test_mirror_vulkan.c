/*
 * Mirrored-transform rendering test (Stage 40 audit).
 *
 * A negative-scale (determinant < 0) instance inverts triangle
 * winding. The renderer must flip the raster front face for such
 * items (per-item pipeline variant + parity-grouped GPU batches)
 * instead of globally disabling backface culling. Before the fix,
 * every triangle of a mirrored closed cube was backface-culled and
 * the mirrored half of the frame stayed clear-colored; after the
 * fix both halves show lit coverage.
 *
 * Headless-safe: SKIP (exit 0) when no window/Vulkan is available.
 * Validation layers stay enabled throughout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>

#include "graphics/graphics_internal.h"

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

static void test_wait_idle(lc_device *device) {
    if (device != NULL && device->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->device);
    }
}

static unsigned char *readback_rgba8(lc_device *device, lc_image *image,
                                     uint32_t w, uint32_t h) {
    lc_buffer *staging = NULL;
    lc_buffer_desc bdesc;
    void *mapped = NULL;
    unsigned char *out = NULL;
    uint64_t bytes = (uint64_t)w * h * 4u;

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = bytes;
    bdesc.usage = LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_TO_CPU;
    if (lc_buffer_create(device, &bdesc, &staging) != LC_SUCCESS) {
        return NULL;
    }
    test_wait_idle(device);
    if (lc_vulkan_copy_image_to_buffer(device, image, 0, 0, w, h, 1,
                                       staging->vk_buffer, 0) !=
        LC_SUCCESS) {
        lc_buffer_destroy(staging);
        return NULL;
    }
    if (lc_buffer_map(staging, &mapped) != LC_SUCCESS || mapped == NULL) {
        lc_buffer_destroy(staging);
        return NULL;
    }
    out = (unsigned char *)malloc((size_t)bytes);
    if (out != NULL) {
        memcpy(out, mapped, (size_t)bytes);
    }
    lc_buffer_unmap(staging);
    lc_buffer_destroy(staging);
    return out;
}

int main(void) {
    lc_device *device = NULL;
    lc_window *window = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lc_command_encoder *enc = NULL;
    lr_renderer *renderer = NULL;
    lr_mesh *cube = NULL;
    lr_material *red = NULL;
    lr_camera camera;
    lc_image *off_img = NULL;
    lc_image_view *off_view = NULL;
    lc_image *off_depth = NULL;
    lc_image_view *off_dview = NULL;
    lc_render_target *offscreen = NULL;
    lc_device_desc ddesc;
    lc_window_desc wdesc;
    lc_surface *tmp_surface_check = NULL;
    int presented = 0;
    int guard = 0;
    int modes_tested = 0;
    int m;

    (void)tmp_surface_check;
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.backend = LC_BACKEND_VULKAN;
    ddesc.enable_validation = 1;
    switch (lc_device_create(&ddesc, &device)) {
    case LC_SUCCESS:
        break;
    case LC_ERROR_BACKEND_UNAVAILABLE:
    case LC_ERROR_NO_SUPPORTED_DEVICE:
        SKIP_ENV("Vulkan device");
    default:
        printf("device failed: FAIL\n");
        lc_shutdown();
        return 1;
    }
    memset(&wdesc, 0, sizeof(wdesc));
    wdesc.title = "LumaC Mirror Test";
    wdesc.width = 800;
    wdesc.height = 600;
    switch (lc_window_create(&wdesc, &window)) {
    case LC_SUCCESS:
        break;
    case LC_ERROR_PLATFORM:
    case LC_ERROR_WINDOW_CREATION_FAILED:
        lc_device_destroy(device);
        SKIP_ENV("window");
    default:
        printf("window failed: FAIL\n");
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    switch (lc_surface_create(device, window, &surface)) {
    case LC_SUCCESS:
        break;
    case LC_ERROR_SURFACE_UNSUPPORTED:
        lc_window_destroy(window);
        lc_device_destroy(device);
        SKIP_ENV("surface");
    default:
        printf("surface failed: FAIL\n");
        lc_window_destroy(window);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    {
        lc_swapchain_desc sdesc;

        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.width = 800;
        sdesc.height = 600;
        sdesc.image_count = 0;
        sdesc.vsync = 1;
        switch (lc_swapchain_create(device, surface, &sdesc,
                                    &swapchain)) {
        case LC_SUCCESS:
            break;
        case LC_ERROR_SWAPCHAIN_UNSUPPORTED:
        case LC_ERROR_ZERO_EXTENT:
            lc_surface_destroy(surface);
            lc_window_destroy(window);
            lc_device_destroy(device);
            SKIP_ENV("swapchain");
        default:
            printf("swapchain failed: FAIL\n");
            lc_surface_destroy(surface);
            lc_window_destroy(window);
            lc_device_destroy(device);
            lc_shutdown();
            return 1;
        }
    }

    /* Test both the CPU draw path and the GPU-driven grouped path. */
    for (m = 0; m < 2; m++) {
        lr_renderer_desc rdesc;
        lc_image_desc idesc;
        lc_image_view_desc vdesc;
        lc_render_target_create_desc tdesc;
        lc_render_target_attachment att;
        lr_unlit_material_desc matdesc;
        lr_pbr_material_desc pbrdesc;
        unsigned char *px = NULL;
        uint32_t lit_left = 0;
        uint32_t lit_right = 0;
        uint32_t x;
        uint32_t y;
        int ok = 0;

        memset(&rdesc, 0, sizeof(rdesc));
        rdesc.device = device;
        rdesc.max_objects = 16;
        rdesc.render_target.color_attachment_count = 1;
        rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
        rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
        rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
        if (lr_renderer_create(&rdesc, &renderer) != LR_SUCCESS) {
            printf("renderer create failed: FAIL\n");
            goto fail;
        }
        if (lr_mesh_create_cube(renderer, 2.0f, &cube) != LR_SUCCESS) {
            printf("cube failed: FAIL\n");
            goto fail;
        }
        memset(&matdesc, 0, sizeof(matdesc));
        matdesc.color[0] = 0.9f;
        matdesc.color[1] = 0.2f;
        matdesc.color[2] = 0.2f;
        matdesc.color[3] = 1.0f;
        memset(&pbrdesc, 0, sizeof(pbrdesc));
        pbrdesc.base_color_factor[0] = 0.9f;
        pbrdesc.base_color_factor[1] = 0.2f;
        pbrdesc.base_color_factor[2] = 0.2f;
        pbrdesc.base_color_factor[3] = 1.0f;
        pbrdesc.metallic_factor = 0.0f;
        pbrdesc.roughness_factor = 0.7f;
        pbrdesc.normal_scale = 1.0f;
        pbrdesc.occlusion_strength = 1.0f;
        pbrdesc.alpha_mode = LR_ALPHA_OPAQUE;
        if (m == 1) {
            /* GPU leg renders PBR so items flow through the
             * parity-grouped indirect path (unlit would fall back
             * to CPU draws even in GPU mode). */
            if (lr_material_create_pbr(renderer, &pbrdesc, &red) !=
                LR_SUCCESS) {
                printf("material failed: FAIL\n");
                goto fail;
            }
            if (lr_renderer_set_render_mode(
                    renderer, LR_RENDER_MODE_GPU_DRIVEN) !=
                LR_SUCCESS) {
                printf("gpu-driven mode failed: FAIL\n");
                goto fail;
            }
        } else {
            if (lr_material_create_unlit(renderer, &matdesc, &red) !=
                LR_SUCCESS) {
                printf("material failed: FAIL\n");
                goto fail;
            }
        }
        lr_camera_init(&camera);
        {
            static const float eye[3] = { 0.0f, 0.0f, 7.0f };
            static const float center[3] = { 0.0f, 0.0f, 0.0f };
            static const float up[3] = { 0.0f, 1.0f, 0.0f };

            if (lr_camera_set_perspective(&camera, 0.7853982f, 1.0f,
                                          0.1f, 100.0f) !=
                    LR_SUCCESS ||
                lr_camera_look_at(&camera, eye, center, up) !=
                    LR_SUCCESS) {
                printf("camera failed: FAIL\n");
                goto fail;
            }
        }
        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_RGBA8_UNORM;
        idesc.width = 512;
        idesc.height = 512;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                      LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                      LC_IMAGE_USAGE_TRANSFER_SRC |
                      LC_IMAGE_USAGE_TRANSFER_DST;
        idesc.samples = LC_SAMPLE_COUNT_1;
        if (lc_image_create(device, &idesc, &off_img) != LC_SUCCESS) {
            printf("offscreen image failed: FAIL\n");
            goto fail;
        }
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.mip_level_count = 1;
        vdesc.array_layer_count = 1;
        if (lc_image_view_create(off_img, &vdesc, &off_view) !=
            LC_SUCCESS) {
            printf("offscreen view failed: FAIL\n");
            goto fail;
        }
        idesc.format = LC_FORMAT_D32_FLOAT;
        idesc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL;
        if (lc_image_create(device, &idesc, &off_depth) !=
            LC_SUCCESS) {
            printf("depth image failed: FAIL\n");
            goto fail;
        }
        vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
        if (lc_image_view_create(off_depth, &vdesc, &off_dview) !=
            LC_SUCCESS) {
            printf("depth view failed: FAIL\n");
            goto fail;
        }
        memset(&tdesc, 0, sizeof(tdesc));
        tdesc.width = 512;
        tdesc.height = 512;
        att.view = off_view;
        tdesc.color_attachments = &att;
        tdesc.color_attachment_count = 1;
        tdesc.depth_stencil_attachment = off_dview;
        if (lc_render_target_create(device, &tdesc, &offscreen) !=
            LC_SUCCESS) {
            printf("offscreen target failed: FAIL\n");
            goto fail;
        }

        presented = 0;
        guard = 0;
        while (!presented) {
            lc_render_color_attachment catt;
            lc_render_depth_attachment datt;
            lc_render_pass_desc pdesc;
            lc_render_swapchain_pass_desc spass;
            lc_result res;

            if (++guard > 120) {
                printf("frame loop guard tripped: FAIL\n");
                goto fail;
            }
            lc_poll_events();
            res = lc_begin_frame(swapchain);
            if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                uint32_t w = lc_window_get_width(window);
                uint32_t h = lc_window_get_height(window);

                if (w != 0 && h != 0) {
                    lc_swapchain_recreate(swapchain, w, h);
                }
                continue;
            }
            if (res != LC_SUCCESS ||
                lc_swapchain_get_encoder(swapchain, &enc) !=
                    LC_SUCCESS) {
                printf("begin_frame failed: FAIL\n");
                goto fail;
            }
            memset(&catt, 0, sizeof(catt));
            catt.view = off_view;
            catt.load_op = LC_LOAD_OP_CLEAR;
            catt.store_op = LC_STORE_OP_STORE;
            catt.clear_color[0] = 0.02f;
            catt.clear_color[1] = 0.02f;
            catt.clear_color[2] = 0.03f;
            catt.clear_color[3] = 1.0f;
            memset(&datt, 0, sizeof(datt));
            datt.view = off_dview;
            datt.depth_load_op = LC_LOAD_OP_CLEAR;
            datt.depth_store_op = LC_STORE_OP_STORE;
            datt.clear_depth = 1.0f;
            datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
            datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
            memset(&pdesc, 0, sizeof(pdesc));
            pdesc.color_attachments = &catt;
            pdesc.color_attachment_count = 1;
            pdesc.depth_attachment = &datt;
            pdesc.width = 512;
            pdesc.height = 512;
            /* NOTE: the offscreen pass opens AFTER submits (+ GPU
             * prepare for m==1): compute dispatches and buffer
             * barriers are illegal inside a pass. */
            if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
                printf("renderer begin failed: FAIL\n");
                goto fail;
            }
            {
                /* Normal cube left, mirrored cube right (negative
                 * X scale inverts winding). PBR leg gets a facing
                 * key light so lit pixels prove coverage. */
                lr_draw_item item;
                lr_light key;

                if (m == 1) {
                    static const float amb[3] = { 0.25f, 0.25f,
                                                  0.25f };

                    memset(&key, 0, sizeof(key));
                    key.type = LR_LIGHT_DIRECTIONAL;
                    key.color[0] = 1.0f;
                    key.color[1] = 1.0f;
                    key.color[2] = 1.0f;
                    key.intensity = 2.5f;
                    key.direction[0] = 0.0f;
                    key.direction[1] = 0.0f;
                    key.direction[2] = -1.0f;
                    lr_renderer_set_ambient(renderer, amb);
                    if (lr_renderer_submit_light(renderer, &key) !=
                            LR_SUCCESS) {
                        printf("submit light failed: FAIL\n");
                        goto fail;
                    }
                }
                memset(&item, 0, sizeof(item));
                lr_transform_identity(&item.transform);
                item.transform.position[0] = -1.4f;
                item.mesh = cube;
                item.material = red;
                if (lr_renderer_submit(renderer, &item) !=
                    LR_SUCCESS) {
                    printf("submit normal failed: FAIL\n");
                    goto fail;
                }
                memset(&item, 0, sizeof(item));
                lr_transform_identity(&item.transform);
                item.transform.position[0] = 1.4f;
                item.transform.scale[0] = -1.0f;
                item.mesh = cube;
                item.material = red;
                if (lr_renderer_submit(renderer, &item) !=
                    LR_SUCCESS) {
                    printf("submit mirrored failed: FAIL\n");
                    goto fail;
                }
            }
            if (m == 1) {
                /* GPU-driven path via the public prepare hook
                 * (parity-grouped indirect draws with per-group
                 * front-face variants). Runs after begin+submits
                 * with NO open pass; the legacy render call below
                 * draws the prepared groups inside the pass. */
                lr_result pr = lr_renderer_prepare_gpu(renderer, enc);

                if (pr != LR_SUCCESS) {
                    printf("gpu prepare failed: FAIL (code=%d)\n",
                           (int)pr);
                    goto fail;
                }
            }
            if (lc_encoder_begin_render_pass(enc, &pdesc) !=
                LC_SUCCESS) {
                printf("offscreen begin failed: FAIL\n");
                goto fail;
            }
            if (lr_renderer_render(renderer, enc, offscreen) !=
                LR_SUCCESS) {
                lc_encoder_end_render_pass(enc);
                printf("renderer render failed: FAIL\n");
                goto fail;
            }
            if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                printf("offscreen end failed: FAIL\n");
                goto fail;
            }
            lr_renderer_end(renderer);
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
            if (lc_encoder_begin_swapchain_pass(enc, swapchain,
                                                &spass) != LC_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                printf("present pass failed: FAIL\n");
                goto fail;
            }
            res = lc_end_frame(swapchain);
            if (res == LC_SUCCESS || res == LC_SUBOPTIMAL) {
                presented = 1;
            } else if (res != LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                printf("end_frame failed: FAIL\n");
                goto fail;
            }
        }
        px = readback_rgba8(device, off_img, 512, 512);
        if (px == NULL) {
            printf("readback failed: FAIL\n");
            goto fail;
        }
        for (y = 0; y < 512u; y++) {
            for (x = 0; x < 512u; x++) {
                unsigned char *p = &px[(y * 512u + x) * 4u];

                /* Lit red cube pixels: strong red, weak blue. */
                if (p[0] > 90 && p[1] < 90 && p[2] < 90) {
                    if (x < 256u) {
                        lit_left++;
                    } else {
                        lit_right++;
                    }
                }
            }
        }
        free(px);
        px = NULL;
        /* Both cubes are 2 units wide at the same depth: each half
         * must show thousands of red pixels. The mirrored half near
         * zero means backface culling ate the mirrored winding. */
        ok = (lit_left > 3000u && lit_right > 3000u);
        TEST_CHECK(ok, (m == 0) ? "mirrored cube visible (CPU path)"
                                : "mirrored cube visible (GPU path)");
        if (!ok) {
            printf("  coverage left=%u right=%u\n", lit_left,
                   lit_right);
        }
        test_wait_idle(device);
        lr_material_destroy(red);
        red = NULL;
        lr_mesh_destroy(cube);
        cube = NULL;
        lc_render_target_destroy(offscreen);
        offscreen = NULL;
        lc_image_view_destroy(off_dview);
        off_dview = NULL;
        lc_image_view_destroy(off_view);
        off_view = NULL;
        lc_image_destroy(off_depth);
        off_depth = NULL;
        lc_image_destroy(off_img);
        off_img = NULL;
        lr_renderer_destroy(renderer);
        renderer = NULL;
        modes_tested++;
        continue;
    fail:
        test_wait_idle(device);
        free(px);
        if (red != NULL) {
            lr_material_destroy(red);
            red = NULL;
        }
        if (cube != NULL) {
            lr_mesh_destroy(cube);
            cube = NULL;
        }
        if (offscreen != NULL) {
            lc_render_target_destroy(offscreen);
            offscreen = NULL;
        }
        if (off_dview != NULL) {
            lc_image_view_destroy(off_dview);
            off_dview = NULL;
        }
        if (off_view != NULL) {
            lc_image_view_destroy(off_view);
            off_view = NULL;
        }
        if (off_depth != NULL) {
            lc_image_destroy(off_depth);
            off_depth = NULL;
        }
        if (off_img != NULL) {
            lc_image_destroy(off_img);
            off_img = NULL;
        }
        if (renderer != NULL) {
            lr_renderer_destroy(renderer);
            renderer = NULL;
        }
        printf("TESTS FAILED\n");
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_window_destroy(window);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    TEST_CHECK(modes_tested == 2, "both CPU and GPU paths tested");
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_window_destroy(window);
    lc_device_destroy(device);
    lc_shutdown();
    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
