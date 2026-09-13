/*
 * Vulkan synchronization integration test (Phase 19, PARTs D–J).
 *
 * Semantic states, explicit transitions, and write->read
 * visibility, all through public LumaC on a windowed frame loop
 * (skips cleanly without display/GPU):
 * - PART G: A write -> B sample ping-pong, 500 iterations with
 *   deterministic clear colors, final pixels exact.
 * - PART H: depth write -> sampled-readable + raw readback.
 * - PART I: upload -> sample, render -> readback, copy -> vertex.
 * - PART D: mixed mip states (mip0 SHADER_READ while mip1 is
 *   TRANSFER_DST), plus the transition error matrix.
 * - PART J: the G loop performs ~1000 explicit transitions plus
 *   ~1000 pass-boundary transitions; result deterministic.
 *
 * Run with validation layers: missing barriers, wrong layouts, and
 * invisible writes all surface as validation errors.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <lumac/lumac.h>

#ifndef LC_SYNC_SPV_DIR
#define LC_SYNC_SPV_DIR "."
#endif

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

#define FAIL_CLEANUP(msg) do { \
    printf("FAIL: %s\n", msg); \
    goto cleanup; \
} while (0)

/* 0 = ready, 1 = environmental SKIP, -1 = hard failure */
static int make_device(lc_device **out) {
    lc_device_desc desc = { 0 };

    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 1;
    *out = NULL;
    switch (lc_device_create(&desc, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_BACKEND_UNAVAILABLE:
    case LC_ERROR_NO_SUPPORTED_DEVICE:
    case LC_ERROR_SURFACE_UNSUPPORTED:
        return 1;
    default:
        return -1;
    }
}

static int load_spv(const char *name, void **out_code, size_t *out_size) {
    char path[512];
    FILE *file = NULL;
    long length = 0;
    void *code = NULL;
    size_t got = 0;

    snprintf(path, sizeof(path), "%s/%s", LC_SYNC_SPV_DIR, name);
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    length = ftell(file);
    if (length <= 0 || (length % 4) != 0) {
        fclose(file);
        return 0;
    }
    rewind(file);
    code = malloc((size_t)length);
    if (code == NULL) {
        fclose(file);
        return 0;
    }
    got = fread(code, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) {
        free(code);
        return 0;
    }
    *out_code = code;
    *out_size = (size_t)length;
    return 1;
}

static lc_image *make_color(lc_device *device, uint32_t w, uint32_t h,
                            uint32_t mips) {
    lc_image_desc desc;
    lc_image *image = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.type = LC_IMAGE_TYPE_2D;
    desc.format = LC_FORMAT_RGBA8_UNORM;
    desc.width = w;
    desc.height = h;
    desc.depth = 1;
    desc.mip_levels = mips;
    desc.array_layers = 1;
    desc.usage = LC_IMAGE_USAGE_SAMPLED |
                 LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                 LC_IMAGE_USAGE_TRANSFER_SRC |
                 LC_IMAGE_USAGE_TRANSFER_DST;
    desc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &desc, &image) != LC_SUCCESS) {
        return NULL;
    }
    return image;
}

static lc_image_view *make_view(lc_image *image, uint32_t base_mip,
                                uint32_t mip_count) {
    lc_image_view_desc desc;
    lc_image_view *view = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.type = LC_IMAGE_VIEW_2D;
    desc.aspect = LC_IMAGE_ASPECT_COLOR;
    desc.base_mip_level = base_mip;
    desc.mip_level_count = mip_count;
    desc.base_array_layer = 0;
    desc.array_layer_count = 1;
    if (lc_image_view_create(image, &desc, &view) != LC_SUCCESS) {
        return NULL;
    }
    return view;
}

static lc_render_target *make_target(lc_device *device, lc_image_view *view,
                                     uint32_t w, uint32_t h) {
    lc_render_target_create_desc desc;
    lc_render_target_attachment att;
    lc_render_target *target = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.width = w;
    desc.height = h;
    att.view = view;
    desc.color_attachments = &att;
    desc.color_attachment_count = 1;
    desc.depth_stencil_attachment = NULL;
    if (lc_render_target_create(device, &desc, &target) != LC_SUCCESS) {
        return NULL;
    }
    return target;
}

/* Begin one output pass (CLEAR/STORE, no depth). */
static int begin_pass(lc_command_encoder *enc, lc_image_view *view,
                      uint32_t w, uint32_t h, const float clear[4]) {
    lc_render_pass_desc desc;
    lc_render_color_attachment catt;

    memset(&catt, 0, sizeof(catt));
    catt.view = view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    memcpy(catt.clear_color, clear, sizeof(catt.clear_color));
    memset(&desc, 0, sizeof(desc));
    desc.color_attachments = &catt;
    desc.color_attachment_count = 1;
    desc.depth_attachment = NULL;
    desc.width = w;
    desc.height = h;
    return (lc_encoder_begin_render_pass(enc, &desc) == LC_SUCCESS)
               ? 0
               : -1;
}

/* Public readback of one RGBA8 pixel. */
static int read_pixel(lc_image *image, uint32_t x, uint32_t y,
                      unsigned char out[4]) {
    lc_image_readback_desc rd;
    lc_image_readback_info info;
    unsigned char *bytes = NULL;
    int ok = 0;

    memset(&rd, 0, sizeof(rd));
    memset(&info, 0, sizeof(info));
    if (lc_image_query_readback(image, &rd, &info) != LC_SUCCESS ||
        info.size == 0) {
        return 0;
    }
    bytes = (unsigned char *)malloc(info.size);
    if (bytes == NULL) {
        return 0;
    }
    if (lc_image_readback(image, &rd, bytes, info.size, NULL) ==
        LC_SUCCESS) {
        const unsigned char *p =
            bytes + ((size_t)y * info.width + x) * 4u;

        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
        out[3] = p[3];
        ok = 1;
    }
    free(bytes);
    return ok;
}

int main(void) {
    lc_device *device = NULL;
    lc_window *window = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    int rc;
    int exit_code = 1;
    /* Chain resources (PART G/J). */
    lc_image *imgA = NULL;
    lc_image *imgB = NULL;
    lc_image_view *viewA = NULL;
    lc_image_view *viewB = NULL;
    lc_render_target *targetA = NULL;
    lc_render_target *targetB = NULL;
    lc_sampler *samp = NULL;
    lc_binding_layout *layout = NULL;
    lc_binding_set *setA = NULL; /* samples A */
    lc_binding_set *setB = NULL; /* samples B */
    lc_shader *copy_vs = NULL;
    lc_shader *copy_fs = NULL;
    lc_pipeline *copy_pipe = NULL;
    static const uint32_t CW = 64;
    static const uint32_t CH = 64;

    memset(&window, 0, sizeof(window));
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (lc_init() != LC_SUCCESS) {
        printf("SKIP: lc_init failed\n");
        return 0;
    }
    rc = make_device(&device);
    if (rc != 0) {
        if (rc > 0) {
            SKIP_ENV("a Vulkan device");
        }
        printf("FAIL: lc_device_create\n");
        lc_shutdown();
        return 1;
    }
    {
        lc_window_desc desc;

        memset(&desc, 0, sizeof(desc));
        desc.title = "Luma Sync Test";
        desc.width = 400;
        desc.height = 300;
        if (lc_window_create(&desc, &window) != LC_SUCCESS) {
            printf("SKIP: no display server\n");
            lc_device_destroy(device);
            lc_shutdown();
            return 0;
        }
    }
    if (lc_surface_create(device, window, &surface) != LC_SUCCESS) {
        FAIL_CLEANUP("lc_surface_create");
    }
    {
        lc_swapchain_desc desc;

        memset(&desc, 0, sizeof(desc));
        desc.width = lc_window_get_width(window);
        desc.height = lc_window_get_height(window);
        desc.image_count = 0;
        desc.vsync = 1;
        if (lc_swapchain_create(device, surface, &desc, &swapchain) !=
            LC_SUCCESS) {
            FAIL_CLEANUP("lc_swapchain_create");
        }
    }

    /* Chain images, views, targets. */
    imgA = make_color(device, CW, CH, 1);
    imgB = make_color(device, CW, CH, 1);
    TEST_CHECK(imgA != NULL && imgB != NULL, "chain: images create");
    if (imgA == NULL || imgB == NULL) {
        FAIL_CLEANUP("chain images");
    }
    viewA = make_view(imgA, 0, 1);
    viewB = make_view(imgB, 0, 1);
    targetA = make_target(device, viewA, CW, CH);
    targetB = make_target(device, viewB, CW, CH);
    TEST_CHECK(viewA != NULL && viewB != NULL && targetA != NULL &&
                   targetB != NULL,
               "chain: views and targets create");
    if (viewA == NULL || viewB == NULL || targetA == NULL ||
        targetB == NULL) {
        FAIL_CLEANUP("chain views/targets");
    }
    {
        lc_sampler_desc sdesc;
        lc_binding_desc slots[2];
        lc_binding_layout_desc ldesc;

        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.min_filter = LC_FILTER_LINEAR;
        sdesc.mag_filter = LC_FILTER_LINEAR;
        sdesc.mipmap_mode = LC_MIPMAP_MODE_LINEAR;
        sdesc.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
        sdesc.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
        sdesc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
        sdesc.max_anisotropy = 1.0f;
        if (lc_sampler_create(device, &sdesc, &samp) != LC_SUCCESS) {
            FAIL_CLEANUP("chain sampler");
        }
        memset(slots, 0, sizeof(slots));
        slots[0].binding = 0;
        slots[0].type = LC_BINDING_SAMPLED_IMAGE;
        slots[0].count = 1;
        slots[0].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        slots[1].binding = 1;
        slots[1].type = LC_BINDING_SAMPLER;
        slots[1].count = 1;
        slots[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        memset(&ldesc, 0, sizeof(ldesc));
        ldesc.bindings = slots;
        ldesc.binding_count = 2;
        if (lc_binding_layout_create(device, &ldesc, &layout) !=
            LC_SUCCESS) {
            FAIL_CLEANUP("chain layout");
        }
        /* Write-once sets (never updated between binds). Sources
         * start UNDEFINED, so the writes land after the first
         * clears (see the loop); binding validation requires
         * sampled-readable state. */
        if (lc_binding_set_create(layout, &setA) != LC_SUCCESS ||
            lc_binding_set_create(layout, &setB) != LC_SUCCESS) {
            FAIL_CLEANUP("chain sets");
        }
    }
    {
        /* Defer set writes until sources are sampled-readable. */
        void *vcode = NULL;
        void *fcode = NULL;
        size_t vsize = 0;
        size_t fsize = 0;
        lc_shader_desc sdesc;
        lc_graphics_pipeline_desc pd;
        lc_render_target_desc sig;
        const lc_binding_layout *layouts[1];

        if (!load_spv("chain_copy.vert.spv", &vcode, &vsize) ||
            !load_spv("chain_copy.frag.spv", &fcode, &fsize)) {
            printf("SKIP: chain SPIR-V not staged\n");
            free(vcode);
            free(fcode);
            goto cleanup;
        }
        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.stage = LC_SHADER_STAGE_VERTEX;
        sdesc.code = vcode;
        sdesc.code_size = vsize;
        if (lc_shader_create(device, &sdesc, &copy_vs) != LC_SUCCESS) {
            free(vcode);
            free(fcode);
            FAIL_CLEANUP("chain vertex shader");
        }
        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.stage = LC_SHADER_STAGE_FRAGMENT;
        sdesc.code = fcode;
        sdesc.code_size = fsize;
        if (lc_shader_create(device, &sdesc, &copy_fs) != LC_SUCCESS) {
            free(vcode);
            free(fcode);
            FAIL_CLEANUP("chain fragment shader");
        }
        free(vcode);
        free(fcode);
        memset(&sig, 0, sizeof(sig));
        sig.color_attachment_count = 1;
        sig.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
        sig.depth_stencil_format = LC_FORMAT_UNDEFINED;
        sig.samples = LC_SAMPLE_COUNT_1;
        memset(&pd, 0, sizeof(pd));
        pd.vertex_shader = copy_vs;
        pd.fragment_shader = copy_fs;
        layouts[0] = layout;
        pd.binding_layouts = layouts;
        pd.binding_layout_count = 1;
        pd.cull_mode = LC_CULL_NONE;
        pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
        pd.render_target = sig;
        if (lc_graphics_pipeline_create(device, &pd, &copy_pipe) !=
            LC_SUCCESS) {
            FAIL_CLEANUP("chain pipeline");
        }
        TEST_CHECK(1, "chain: pipeline creates");
    }

    /* ---- PART G/J: ping-pong visibility, ~1000 transitions ----
     * Even k: clear A to color_k, copy A -> B. Odd k: clear B,
     * copy B -> A. 25 iterations per frame; readback every 25th
     * (post-end_frame only). Final pixels exact. */
    {
        lc_command_encoder *enc = NULL;
        static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        int k;
        int frames_ok = 0;
        int copies_ok = 0;
        int frame_open = 0;
        int setA_written = 0;
        int setB_written = 0;
        unsigned char expect[4] = { 0, 0, 0, 255 };
        lc_image_subresource_range full;

        memset(&full, 0, sizeof(full));
        full.level_count = 1;
        full.layer_count = 1;
        for (k = 0; k < 500; k++) {
            float clear[4];
            lc_image *src;
            lc_image *dst;
            lc_image_view *dst_view;
            lc_binding_set *src_set;
            int even = ((k % 2) == 0);

            if ((k % 25) == 0) {
                lc_poll_events();
                lc_device_wait_idle(device);
                if (lc_begin_frame(swapchain) != LC_SUCCESS) {
                    break;
                }
                if (lc_swapchain_get_encoder(swapchain, &enc) !=
                    LC_SUCCESS) {
                    break;
                }
                frame_open = 1;
                frames_ok++;
            }
            clear[0] = (float)(((k * 37) + 11) % 256) / 255.0f;
            clear[1] = (float)(((k * 91) + 43) % 256) / 255.0f;
            clear[2] = (float)(((k * 53) + 97) % 256) / 255.0f;
            clear[3] = 1.0f;
            expect[0] = (unsigned char)(((k * 37) + 11) % 256);
            expect[1] = (unsigned char)(((k * 91) + 43) % 256);
            expect[2] = (unsigned char)(((k * 53) + 97) % 256);
            /* Quantization check: 8-bit UNORM round-trips the
             * byte pattern we built the float from. */
            src = even ? imgA : imgB;
            dst = even ? imgB : imgA;
            dst_view = even ? viewB : viewA;
            src_set = even ? setA : setB;
            /* (1) clear src to color_k. */
            if (begin_pass(enc, even ? viewA : viewB, CW, CH, clear) !=
                    0 ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                break;
            }
            /* First clears make the sources sampled-readable: land
             * the write-once set writes now (validation requires
             * SHADER_READ state). */
            if ((even && !setA_written) || (!even && !setB_written)) {
                lc_binding_write writes[2];

                memset(writes, 0, sizeof(writes));
                writes[0].binding = 0;
                writes[0].type = LC_BINDING_SAMPLED_IMAGE;
                writes[0].u.image.view = even ? viewA : viewB;
                writes[1].binding = 1;
                writes[1].type = LC_BINDING_SAMPLER;
                writes[1].u.sampler.sampler = samp;
                if (lc_binding_set_update(even ? setA : setB, writes,
                                          2) != LC_SUCCESS) {
                    break;
                }
                if (even) {
                    setA_written = 1;
                } else {
                    setB_written = 1;
                }
            }
            /* (2) explicit transition src -> SHADER_READ (already
             * there via STORE finals; validates the explicit path
             * on every iteration). */
            if (lc_encoder_transition_image(enc, src, &full,
                                            LC_RESOURCE_STATE_SHADER_READ) !=
                LC_SUCCESS) {
                break;
            }
            /* (3) copy src -> dst (dst cleared first; fullscreen
             * triangle covers everything deterministically). */
            if (begin_pass(enc, dst_view, CW, CH, black) != 0) {
                break;
            }
            if (lc_encoder_bind_pipeline(enc, copy_pipe) !=
                    LC_SUCCESS ||
                lc_encoder_bind_binding_set(enc, copy_pipe, 0,
                                            src_set) != LC_SUCCESS ||
                lc_encoder_draw(enc, 3, 0) != LC_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                break;
            }
            if (((k + 1) % 25) == 0 || k == 499) {
                unsigned char got[4] = { 0, 0, 0, 0 };
                lc_render_swapchain_pass_desc spass;
                lc_result er;

                /* Swapchain leg (clear-only present): presenting an
                 * untouched swapchain image is a PRESENT_SRC VUID,
                 * so every frame presents a cleared image. */
                memset(&spass, 0, sizeof(spass));
                spass.color_load_op = LC_LOAD_OP_CLEAR;
                spass.color_store_op = LC_STORE_OP_STORE;
                spass.depth_load_op = LC_LOAD_OP_CLEAR;
                spass.depth_store_op = LC_STORE_OP_DONT_CARE;
                spass.clear_depth = 1.0f;
                if (lc_encoder_begin_swapchain_pass(enc, swapchain,
                                                   &spass) !=
                        LC_SUCCESS ||
                    lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                    break;
                }
                er = lc_end_frame(swapchain);
                frame_open = 0;
                if (er != LC_SUCCESS && er != LC_SUBOPTIMAL) {
                    break;
                }
                if (!read_pixel(dst, CW / 2, CH / 2, got) ||
                    got[0] != expect[0] || got[1] != expect[1] ||
                    got[2] != expect[2] || got[3] != 255) {
                    printf("[info] k=%d expect=(%u,%u,%u) "
                           "got=(%u,%u,%u)\n",
                           k, expect[0], expect[1], expect[2], got[0],
                           got[1], got[2]);
                    break;
                }
                copies_ok++;
                enc = NULL;
                frame_open = 0;
            }
        }
        /* Balance an open frame if the loop broke mid-frame. */
        if (frame_open) {
            lc_end_frame(swapchain);
            frame_open = 0;
        }
        enc = NULL;
        TEST_CHECK(k == 500, "chain: 500 iterations record");
        TEST_CHECK(copies_ok == 20, "chain: 20 sampled copies exact");
        TEST_CHECK(frames_ok == 20, "chain: 20 frames present");
    }

    /* ---- PART H: depth write -> sampled-readable + raw read ----
     * Depth-only clear pass, then the depth view must bind as a
     * sampled image (tracked SHADER_READ via SAMPLED usage), then
     * raw depth readback equals the clear value. Covers the shadow
     * depth-write -> sample class at LumaC level (renderer shadow
     * tests cover the PBR sampling itself). */
    {
        lc_image *dimg = NULL;
        lc_image_view *dview = NULL;
        lc_render_target *dtarget = NULL;
        lc_image_desc idesc;
        lc_image_view_desc vdesc;
        lc_render_target_create_desc tdesc;
        lc_command_encoder *enc = NULL;
        lc_render_pass_desc pdesc;
        lc_render_depth_attachment datt;
        lc_binding_write write;
        float got = -1.0f;

        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_D32_FLOAT;
        idesc.width = CW;
        idesc.height = CH;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                      LC_IMAGE_USAGE_DEPTH_STENCIL |
                      LC_IMAGE_USAGE_TRANSFER_SRC;
        idesc.samples = LC_SAMPLE_COUNT_1;
        if (lc_image_create(device, &idesc, &dimg) != LC_SUCCESS) {
            TEST_CHECK(0, "depth: image creates");
        } else {
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
            vdesc.mip_level_count = 1;
            vdesc.array_layer_count = 1;
            if (lc_image_view_create(dimg, &vdesc, &dview) !=
                LC_SUCCESS) {
                TEST_CHECK(0, "depth: view creates");
                dview = NULL;
            } else {
                lc_render_target_create_desc dd;

                memset(&dd, 0, sizeof(dd));
                dd.width = CW;
                dd.height = CH;
                dd.color_attachments = NULL;
                dd.color_attachment_count = 0;
                dd.depth_stencil_attachment = dview;
                if (lc_render_target_create(device, &dd, &dtarget) !=
                    LC_SUCCESS) {
                    TEST_CHECK(0, "depth: target creates");
                    dtarget = NULL;
                }
            }
            if (dtarget != NULL) {
                int opened = 0;

                lc_poll_events();
                lc_device_wait_idle(device);
                if (lc_begin_frame(swapchain) == LC_SUCCESS &&
                    lc_swapchain_get_encoder(swapchain, &enc) ==
                        LC_SUCCESS) {
                    opened = 1;
                    memset(&datt, 0, sizeof(datt));
                    datt.view = dview;
                    datt.depth_load_op = LC_LOAD_OP_CLEAR;
                    datt.depth_store_op = LC_STORE_OP_STORE;
                    datt.clear_depth = 0.25f;
                    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
                    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
                    memset(&pdesc, 0, sizeof(pdesc));
                    pdesc.color_attachments = NULL;
                    pdesc.color_attachment_count = 0;
                    pdesc.depth_attachment = &datt;
                    pdesc.width = CW;
                    pdesc.height = CH;
                    if (lc_encoder_begin_render_pass(enc, &pdesc) !=
                            LC_SUCCESS ||
                        lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                        TEST_CHECK(0, "depth: clear pass records");
                        opened = -1;
                    }
                } else {
                    TEST_CHECK(0, "depth: frame opens");
                }
                if (opened != 0) {
                    lc_result er = LC_ERROR_UNKNOWN;

                    /* Swapchain leg: clear-only present (mirrors
                     * the renderer test harness), then present. */
                    if (opened > 0) {
                        lc_render_swapchain_pass_desc spass;

                        memset(&spass, 0, sizeof(spass));
                        spass.color_load_op = LC_LOAD_OP_CLEAR;
                        spass.color_store_op = LC_STORE_OP_STORE;
                        spass.depth_load_op = LC_LOAD_OP_CLEAR;
                        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
                        spass.clear_depth = 1.0f;
                        if (lc_encoder_begin_swapchain_pass(
                                enc, swapchain, &spass) == LC_SUCCESS &&
                            lc_encoder_end_render_pass(enc) ==
                                LC_SUCCESS) {
                            er = lc_end_frame(swapchain);
                        }
                    }
                    TEST_CHECK(opened > 0 &&
                                   (er == LC_SUCCESS ||
                                    er == LC_SUBOPTIMAL),
                               "depth: clear pass presents");
                    enc = NULL;
                }
                /* Sampled bind must accept the post-STORE depth
                 * view (tracked SHADER_READ). Reuse the chain
                 * layout via a scratch set. */
                {
                    lc_binding_set *dset = NULL;

                    memset(&write, 0, sizeof(write));
                    if (lc_binding_set_create(layout, &dset) ==
                            LC_SUCCESS) {
                        write.binding = 0;
                        write.type = LC_BINDING_SAMPLED_IMAGE;
                        write.u.image.view = dview;
                        TEST_CHECK(lc_binding_set_update(dset, &write,
                                                         1) ==
                                       LC_SUCCESS,
                                   "depth: post-write view binds "
                                   "sampled");
                        lc_binding_set_destroy(dset);
                    } else {
                        TEST_CHECK(0, "depth: scratch set creates");
                    }
                }
                /* Raw depth readback equals the clear value. */
                {
                    lc_image_readback_desc rd;
                    float *vals = NULL;

                    memset(&rd, 0, sizeof(rd));
                    vals = (float *)malloc((size_t)CW * CH *
                                           sizeof(float));
                    if (vals != NULL &&
                        lc_image_readback(dimg, &rd, vals,
                                          (size_t)CW * CH *
                                              sizeof(float),
                                          NULL) == LC_SUCCESS) {
                        got = vals[(CH / 2) * CW + (CW / 2)];
                    }
                    free(vals);
                    TEST_CHECK(got == 0.25f,
                               "depth: clear value reads back raw");
                }
            }
        }
        /* Drain the depth frame before tearing down its objects. */
        lc_device_wait_idle(device);
        lc_render_target_destroy(dtarget);
        lc_image_view_destroy(dview);
        lc_image_destroy(dimg);
    }

    /* ---- PART I: upload -> sample, copy -> vertex ----
     * Upload a 4x4 pattern, sample it through the copy pipeline
     * into B, and prove the pattern survives (transfer visibility
     * into sampling). Then write a GPU-only vertex buffer via
     * lc_buffer_write (staging copy internally), draw it with a
     * vertex-fetch pipeline, and prove the staged bytes shade. */
    {
        lc_image *imgU = NULL;
        lc_image_view *viewU = NULL;
        lc_binding_set *setU = NULL;
        unsigned char pattern[4 * 4 * 4];
        unsigned char got[4] = { 0, 0, 0, 0 };
        lc_command_encoder *enc = NULL;
        static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        uint32_t i;

        for (i = 0; i < 4 * 4; i++) {
            pattern[i * 4 + 0] = (unsigned char)(i * 16);
            pattern[i * 4 + 1] = (unsigned char)(255 - i * 16);
            pattern[i * 4 + 2] = 128;
            pattern[i * 4 + 3] = 255;
        }
        imgU = make_color(device, 4, 4, 1);
        viewU = (imgU != NULL) ? make_view(imgU, 0, 1) : NULL;
        if (imgU == NULL || viewU == NULL) {
            TEST_CHECK(0, "upload-sample: image creates");
        } else {
            lc_image_upload_desc up;
            lc_binding_write writes[2];

            memset(&up, 0, sizeof(up));
            up.width = 4;
            up.height = 4;
            up.depth = 1;
            up.data = pattern;
            up.data_size = sizeof(pattern);
            TEST_CHECK(lc_image_write(imgU, &up) == LC_SUCCESS,
                       "upload-sample: upload works");
            memset(writes, 0, sizeof(writes));
            writes[0].binding = 0;
            writes[0].type = LC_BINDING_SAMPLED_IMAGE;
            writes[0].u.image.view = viewU;
            writes[1].binding = 1;
            writes[1].type = LC_BINDING_SAMPLER;
            writes[1].u.sampler.sampler = samp;
            TEST_CHECK(lc_binding_set_create(layout, &setU) ==
                           LC_SUCCESS &&
                           lc_binding_set_update(setU, writes, 2) ==
                               LC_SUCCESS,
                       "upload-sample: set writes");
            lc_poll_events();
            lc_device_wait_idle(device);
            if (lc_begin_frame(swapchain) == LC_SUCCESS &&
                lc_swapchain_get_encoder(swapchain, &enc) ==
                    LC_SUCCESS &&
                begin_pass(enc, viewB, CW, CH, black) == 0 &&
                lc_encoder_bind_pipeline(enc, copy_pipe) ==
                    LC_SUCCESS &&
                lc_encoder_bind_binding_set(enc, copy_pipe, 0, setU) ==
                    LC_SUCCESS &&
                lc_encoder_draw(enc, 3, 0) == LC_SUCCESS &&
                lc_encoder_end_render_pass(enc) == LC_SUCCESS) {
                lc_render_swapchain_pass_desc spass;
                lc_result er = LC_ERROR_UNKNOWN;

                memset(&spass, 0, sizeof(spass));
                spass.color_load_op = LC_LOAD_OP_CLEAR;
                spass.color_store_op = LC_STORE_OP_STORE;
                spass.depth_load_op = LC_LOAD_OP_CLEAR;
                spass.depth_store_op = LC_STORE_OP_DONT_CARE;
                spass.clear_depth = 1.0f;
                if (lc_encoder_begin_swapchain_pass(enc, swapchain,
                                                   &spass) ==
                        LC_SUCCESS &&
                    lc_encoder_end_render_pass(enc) == LC_SUCCESS) {
                    er = lc_end_frame(swapchain);
                }
                if ((er == LC_SUCCESS || er == LC_SUBOPTIMAL) &&
                    read_pixel(imgB, 0, 0, got) && got[0] == pattern[0] &&
                    got[1] == pattern[1] && got[2] == pattern[2]) {
                    TEST_CHECK(1, "upload-sample: texel survives");
                } else {
                    TEST_CHECK(0, "upload-sample: texel survives");
                }
            } else {
                /* Balance a half-open frame before failing. */
                lc_end_frame(swapchain);
                TEST_CHECK(0, "upload-sample: texel survives");
            }
            enc = NULL;
        }
        lc_device_wait_idle(device);
        lc_binding_set_destroy(setU);
        lc_image_view_destroy(viewU);
        lc_image_destroy(imgU);
    }
    {
        /* Copy -> vertex: GPU-only VBO staged through
         * lc_buffer_write, then fetched by a draw. All three
         * vertices share one color so interpolation is exact
         * (0.75 avoids UNORM .5 rounding ambiguity). */
        void *vcode = NULL;
        void *fcode = NULL;
        size_t vsize = 0;
        size_t fsize = 0;
        lc_shader *vs = NULL;
        lc_shader *fs = NULL;
        lc_pipeline *vb_pipe = NULL;
        lc_buffer *vbo = NULL;
        lc_image *imgV = NULL;
        lc_image_view *viewV = NULL;
        lc_render_target *targetV = NULL;
        static const float verts[3 * 5] = {
            0.0f, -0.6f, 0.25f, 0.75f, 1.0f,
            0.6f, 0.6f,  0.25f, 0.75f, 1.0f,
            -0.6f, 0.6f, 0.25f, 0.75f, 1.0f
        };
        unsigned char got[4] = { 0, 0, 0, 0 };
        lc_command_encoder *enc = NULL;
        static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

        if (load_spv("vb_triangle.vert.spv", &vcode, &vsize) &&
            load_spv("triangle.frag.spv", &fcode, &fsize)) {
            lc_shader_desc sdesc;
            lc_buffer_desc bdesc;
            lc_vertex_binding_desc vbinding;
            lc_vertex_attribute_desc vattrs[2];
            lc_graphics_pipeline_desc pd;
            lc_render_target_desc sig;

            memset(&sdesc, 0, sizeof(sdesc));
            sdesc.stage = LC_SHADER_STAGE_VERTEX;
            sdesc.code = vcode;
            sdesc.code_size = vsize;
            if (lc_shader_create(device, &sdesc, &vs) == LC_SUCCESS) {
                memset(&sdesc, 0, sizeof(sdesc));
                sdesc.stage = LC_SHADER_STAGE_FRAGMENT;
                sdesc.code = fcode;
                sdesc.code_size = fsize;
                if (lc_shader_create(device, &sdesc, &fs) !=
                    LC_SUCCESS) {
                    lc_shader_destroy(vs);
                    vs = NULL;
                }
            }
            memset(&bdesc, 0, sizeof(bdesc));
            bdesc.size = sizeof(verts);
            bdesc.usage = LC_BUFFER_USAGE_VERTEX;
            bdesc.memory = LC_MEMORY_GPU_ONLY;
            if (vs != NULL && fs != NULL &&
                lc_buffer_create(device, &bdesc, &vbo) ==
                    LC_SUCCESS &&
                lc_buffer_write(vbo, 0, verts, sizeof(verts)) ==
                    LC_SUCCESS) {
                memset(&vbinding, 0, sizeof(vbinding));
                vbinding.binding = 0;
                vbinding.stride = 5 * sizeof(float);
                vbinding.input_rate = LC_VERTEX_INPUT_PER_VERTEX;
                memset(vattrs, 0, sizeof(vattrs));
                vattrs[0].location = 0;
                vattrs[0].binding = 0;
                vattrs[0].format = LC_FORMAT_RG32_FLOAT;
                vattrs[0].offset = 0;
                vattrs[1].location = 1;
                vattrs[1].binding = 0;
                vattrs[1].format = LC_FORMAT_RGB32_FLOAT;
                vattrs[1].offset = 2 * sizeof(float);
                memset(&sig, 0, sizeof(sig));
                sig.color_attachment_count = 1;
                sig.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
                sig.depth_stencil_format = LC_FORMAT_UNDEFINED;
                sig.samples = LC_SAMPLE_COUNT_1;
                memset(&pd, 0, sizeof(pd));
                pd.vertex_shader = vs;
                pd.fragment_shader = fs;
                pd.vertex_bindings = &vbinding;
                pd.vertex_binding_count = 1;
                pd.vertex_attributes = vattrs;
                pd.vertex_attribute_count = 2;
                pd.cull_mode = LC_CULL_NONE;
                pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
                pd.render_target = sig;
                imgV = make_color(device, CW, CH, 1);
                viewV = (imgV != NULL) ? make_view(imgV, 0, 1) : NULL;
                targetV = (viewV != NULL)
                              ? make_target(device, viewV, CW, CH)
                              : NULL;
                lc_poll_events();
                lc_device_wait_idle(device);
                if (targetV != NULL &&
                    lc_graphics_pipeline_create(device, &pd,
                                                &vb_pipe) ==
                        LC_SUCCESS &&
                    lc_begin_frame(swapchain) == LC_SUCCESS &&
                    lc_swapchain_get_encoder(swapchain, &enc) ==
                        LC_SUCCESS &&
                    begin_pass(enc, viewV, CW, CH, black) == 0 &&
                    lc_encoder_bind_pipeline(enc, vb_pipe) ==
                        LC_SUCCESS &&
                    lc_encoder_bind_vertex_buffer(enc, 0, vbo, 0) ==
                        LC_SUCCESS &&
                    lc_encoder_draw(enc, 3, 0) == LC_SUCCESS &&
                    lc_encoder_end_render_pass(enc) == LC_SUCCESS) {
                    lc_render_swapchain_pass_desc spass2;
                    lc_result er2 = LC_ERROR_UNKNOWN;

                    memset(&spass2, 0, sizeof(spass2));
                    spass2.color_load_op = LC_LOAD_OP_CLEAR;
                    spass2.color_store_op = LC_STORE_OP_STORE;
                    spass2.depth_load_op = LC_LOAD_OP_CLEAR;
                    spass2.depth_store_op = LC_STORE_OP_DONT_CARE;
                    spass2.clear_depth = 1.0f;
                    if (lc_encoder_begin_swapchain_pass(
                            enc, swapchain, &spass2) == LC_SUCCESS &&
                        lc_encoder_end_render_pass(enc) == LC_SUCCESS) {
                        er2 = lc_end_frame(swapchain);
                    }
                    if ((er2 == LC_SUCCESS || er2 == LC_SUBOPTIMAL) &&
                        read_pixel(imgV, CW / 2, CH / 2, got) &&
                        got[0] == 64 && got[1] == 191 &&
                        got[2] == 255) {
                        TEST_CHECK(1, "copy-vertex: staged bytes shade");
                    } else {
                        printf("[info] vb got=(%u,%u,%u,%u)\n", got[0],
                               got[1], got[2], got[3]);
                        TEST_CHECK(0, "copy-vertex: staged bytes shade");
                    }
                } else {
                    lc_end_frame(swapchain);
                    TEST_CHECK(0, "copy-vertex: staged bytes shade");
                }
                enc = NULL;
            } else {
                TEST_CHECK(0, "copy-vertex: resources create");
            }
            free(vcode);
            free(fcode);
        } else {
            printf("SKIP: vb_triangle SPIR-V not staged\n");
        }
        lc_device_wait_idle(device);
        lc_pipeline_destroy(vb_pipe);
        lc_render_target_destroy(targetV);
        lc_image_view_destroy(viewV);
        lc_image_destroy(imgV);
        lc_buffer_destroy(vbo);
        lc_shader_destroy(fs);
        lc_shader_destroy(vs);
    }

    /* ---- PART D: mixed mip states + error matrix ----
     * mip0 stays SHADER_READ while mip1 goes TRANSFER_DST (range
     * transitions must not disturb siblings); binds prove it. */
    {
        lc_image *imgC = NULL;
        lc_image_view *viewC0 = NULL;
        lc_image_view *viewC1 = NULL;
        lc_binding_set *setC0 = NULL;
        lc_binding_set *setC1 = NULL;
        lc_binding_write write;
        lc_image_subresource_range full;
        lc_image_subresource_range bad;
        unsigned char white[32 * 32 * 4];

        memset(white, 0xFF, sizeof(white));
        imgC = make_color(device, 32, 32, 2);
        TEST_CHECK(imgC != NULL, "mixed: mipmapped image creates");
        if (imgC != NULL) {
            lc_image_upload_desc up;

            memset(&up, 0, sizeof(up));
            up.width = 32;
            up.height = 32;
            up.depth = 1;
            up.data = white;
            up.data_size = sizeof(white);
            TEST_CHECK(lc_image_write(imgC, &up) == LC_SUCCESS &&
                           lc_image_generate_mipmaps(imgC) ==
                               LC_SUCCESS,
                       "mixed: upload + mipmaps");
            viewC0 = make_view(imgC, 0, 1);
            /* mip1-only view needs a custom range (make_view is
             * mip0-only). */
            {
                lc_image_view_desc vd;

                memset(&vd, 0, sizeof(vd));
                vd.type = LC_IMAGE_VIEW_2D;
                vd.aspect = LC_IMAGE_ASPECT_COLOR;
                vd.base_mip_level = 1;
                vd.mip_level_count = 1;
                vd.base_array_layer = 0;
                vd.array_layer_count = 1;
                if (lc_image_view_create(imgC, &vd, &viewC1) !=
                    LC_SUCCESS) {
                    viewC1 = NULL;
                }
            }
            TEST_CHECK(viewC0 != NULL && viewC1 != NULL,
                       "mixed: per-mip views create");
            TEST_CHECK(lc_binding_set_create(layout, &setC0) ==
                           LC_SUCCESS &&
                           lc_binding_set_create(layout, &setC1) ==
                               LC_SUCCESS,
                       "mixed: sets create");
            /* Transition ONLY mip1 away (explicit range). */
            memset(&full, 0, sizeof(full));
            full.base_mip_level = 1;
            full.level_count = 1;
            full.layer_count = 1;
            /* No open frame here: use immediate-submit upload path
             * via a scratch frame below. For now only validate the
             * public call rejects without a frame. */
            TEST_CHECK(lc_encoder_transition_image(
                           NULL, imgC, &full,
                           LC_RESOURCE_STATE_TRANSFER_DST) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "mixed: NULL encoder rejected");
            memset(&bad, 0, sizeof(bad));
            bad.base_mip_level = 5;
            bad.level_count = 1;
            bad.layer_count = 1;
            /* Open a frame for the real transitions. */
            {
                lc_command_encoder *enc = NULL;

                lc_poll_events();
                lc_device_wait_idle(device);
                if (lc_begin_frame(swapchain) == LC_SUCCESS &&
                    lc_swapchain_get_encoder(swapchain, &enc) ==
                        LC_SUCCESS) {
                    TEST_CHECK(
                        lc_encoder_transition_image(
                            enc, imgC, &bad,
                            LC_RESOURCE_STATE_TRANSFER_DST) ==
                            LC_ERROR_INVALID_ARGUMENT,
                        "mixed: bad mip rejected");
                    memset(&bad, 0, sizeof(bad));
                    bad.level_count = 1;
                    bad.layer_count = 1;
                    TEST_CHECK(
                        lc_encoder_transition_image(
                            enc, imgC, &bad,
                            LC_RESOURCE_STATE_VERTEX_READ) ==
                            LC_ERROR_INVALID_ARGUMENT,
                        "mixed: buffer state rejected for images");
                    TEST_CHECK(
                        lc_encoder_transition_image(
                            enc, NULL, &bad,
                            LC_RESOURCE_STATE_TRANSFER_DST) ==
                            LC_ERROR_INVALID_ARGUMENT,
                        "mixed: NULL image rejected");
                    TEST_CHECK(
                        lc_encoder_transition_image(
                            enc, imgC, NULL,
                            LC_RESOURCE_STATE_TRANSFER_DST) ==
                            LC_ERROR_INVALID_ARGUMENT,
                        "mixed: NULL range rejected");
                    /* The real mixed-state transition. */
                    full.base_mip_level = 1;
                    full.level_count = 1;
                    full.base_array_layer = 0;
                    full.layer_count = 1;
                    TEST_CHECK(
                        lc_encoder_transition_image(
                            enc, imgC, &full,
                            LC_RESOURCE_STATE_TRANSFER_DST) ==
                            LC_SUCCESS,
                        "mixed: mip1 leaves sampled state");
                    memset(&write, 0, sizeof(write));
                    write.binding = 0;
                    write.type = LC_BINDING_SAMPLED_IMAGE;
                    write.u.image.view = viewC0;
                    TEST_CHECK(
                        lc_binding_set_update(setC0, &write, 1) ==
                            LC_SUCCESS,
                        "mixed: mip0 still binds sampled");
                    write.u.image.view = viewC1;
                    TEST_CHECK(
                        lc_binding_set_update(setC1, &write, 1) ==
                            LC_ERROR_INVALID_ARGUMENT,
                        "mixed: mip1 refuses sampled bind");
                    TEST_CHECK(
                        lc_encoder_transition_image(
                            enc, imgC, &full,
                            LC_RESOURCE_STATE_SHADER_READ) ==
                            LC_SUCCESS,
                        "mixed: mip1 returns sampled state");
                    TEST_CHECK(
                        lc_binding_set_update(setC1, &write, 1) ==
                            LC_SUCCESS,
                        "mixed: mip1 binds again");
                    lc_end_frame(swapchain);
                } else {
                    TEST_CHECK(0, "mixed: frame opens");
                }
            }
            lc_device_wait_idle(device);
            lc_binding_set_destroy(setC1);
            lc_binding_set_destroy(setC0);
            lc_image_view_destroy(viewC1);
            lc_image_view_destroy(viewC0);
            lc_image_destroy(imgC);
        }
    }

    printf("sync vulkan: %d passed, %d failed\n", g_passed, g_failed);
    exit_code = (g_failed == 0) ? 0 : 1;

cleanup:
    /* Drain in-flight frames before tearing down GPU objects. */
    lc_device_wait_idle(device);
    lc_binding_set_destroy(setB);
    lc_binding_set_destroy(setA);
    lc_binding_layout_destroy(layout);
    lc_sampler_destroy(samp);
    lc_pipeline_destroy(copy_pipe);
    lc_shader_destroy(copy_fs);
    lc_shader_destroy(copy_vs);
    lc_render_target_destroy(targetB);
    lc_render_target_destroy(targetA);
    lc_image_view_destroy(viewB);
    lc_image_view_destroy(viewA);
    lc_image_destroy(imgB);
    lc_image_destroy(imgA);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return exit_code;
}
