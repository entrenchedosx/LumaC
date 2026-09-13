/*
 * Vulkan render-target integration test (Phase 12).
 *
 * Live validation (target/pipeline/encoder misuse), MRT rendering
 * with exact pixel readback, two-pass render-to-texture sampling with
 * readback proof (plus a LOAD-preserve re-sample), equivalent-target
 * pipeline sharing, incompatible-target rejection, swapchain-resize
 * independence, multi-window sharing, and a 120-frame two-pass cube
 * run. Validation layers stay enabled throughout.
 *
 * If the environment cannot provide a window or Vulkan setup, SKIP and
 * exit 0. Any other failure is a hard FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <lumac/lumac.h>

#include "graphics/graphics_internal.h"

#ifndef LC_TRIANGLE_SPV_DIR
#define LC_TRIANGLE_SPV_DIR "."
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
        return 1;
    default:
        return -1;
    }
}

static int make_window_titled(lc_window **out, const char *title) {
    lc_window_desc desc;

    desc.title = title;
    desc.width = 800;
    desc.height = 600;
    *out = NULL;
    switch (lc_window_create(&desc, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_PLATFORM:
    case LC_ERROR_WINDOW_CREATION_FAILED:
        return 1;
    default:
        return -1;
    }
}

static int make_surface(lc_device *device, lc_window *window,
                        lc_surface **out) {
    *out = NULL;
    switch (lc_surface_create(device, window, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_SURFACE_UNSUPPORTED:
        return 1;
    default:
        return -1;
    }
}

static int make_swapchain(lc_device *device, lc_surface *surface,
                          lc_swapchain **out) {
    lc_swapchain_desc desc;

    desc.width = 800;
    desc.height = 600;
    desc.image_count = 0;
    desc.vsync = 1;
    *out = NULL;
    switch (lc_swapchain_create(device, surface, &desc, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_SWAPCHAIN_UNSUPPORTED:
    case LC_ERROR_ZERO_EXTENT:
        return 1;
    default:
        return -1;
    }
}

#define SKIP_ENV(what) do { \
    printf("SKIP: environment cannot provide %s\n", what); \
    lc_shutdown(); \
    return 0; \
} while (0)

#define FAIL_SUMMARY() do { \
    printf("TESTS FAILED\n"); \
    lc_shutdown(); \
    return 1; \
} while (0)

static int load_spv_file(const char *name, void **out_code,
                         size_t *out_size) {
    char path[512];
    FILE *file = NULL;
    long length = 0;
    void *code = NULL;
    size_t got = 0;
    int written = snprintf(path, sizeof(path), "%s/%s", LC_TRIANGLE_SPV_DIR,
                           name);

    if (written < 0 || (size_t)written >= sizeof(path)) {
        return 0;
    }
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

static int make_shader(lc_device *device, lc_shader_stage stage,
                       const char *file, lc_shader **out) {
    lc_shader_desc sdesc;
    void *code = NULL;
    size_t size = 0;

    if (!load_spv_file(file, &code, &size)) {
        return -1;
    }
    sdesc.stage = stage;
    sdesc.code = code;
    sdesc.code_size = size;
    sdesc.entry_point = NULL;
    if (lc_shader_create(device, &sdesc, out) != LC_SUCCESS) {
        free(code);
        return -1;
    }
    free(code);
    return 0;
}

/* Test-only GPU idle (white-box): frames submit asynchronously, so
 * readback after end_frame must wait for rendering to finish. Coarse
 * and rare (never per-frame in real code). */
static void test_wait_idle(lc_device *device) {
    if (device != NULL && device->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->device);
    }
}

/* Test-only MVP math (mirrors examples/cube_3d): Vulkan perspective
 * with Y flip, fixed orbit. Identity matrices mirror the authored
 * winding (all faces culled); the Y-flipped projection is required
 * for BACK/CCW to show front faces. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void rt_mat4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void rt_mat4_multiply(float *out, const float *a, const float *b) {
    float tmp[16];
    int col;
    int row;

    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            tmp[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] +
                                 a[1 * 4 + row] * b[col * 4 + 1] +
                                 a[2 * 4 + row] * b[col * 4 + 2] +
                                 a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

static void rt_mat4_perspective(float *m, float fov_y, float aspect,
                                float near_z, float far_z) {
    float f = 1.0f / tanf(fov_y * 0.5f);

    /* Same contract as the examples: w = -z, near -> 0, far -> 1. */
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = -f;
    m[10] = -far_z / (far_z - near_z);
    m[11] = -1.0f;
    m[14] = -(far_z * near_z) / (far_z - near_z);
}

static void rt_mat4_rotate_y(float *m, float angle) {
    float c = cosf(angle);
    float s = sinf(angle);

    memset(m, 0, 16 * sizeof(float));
    m[0] = c;
    m[2] = -s;
    m[5] = 1.0f;
    m[8] = s;
    m[10] = c;
    m[15] = 1.0f;
}

static void rt_cube_mvp(float *mvp, float angle) {
    float proj[16];
    float view[16];
    float rot[16];
    float view_model[16];

    rt_mat4_perspective(proj, (float)(45.0 * M_PI / 180.0), 1.0f, 0.1f,
                        100.0f);
    rt_mat4_identity(view);
    view[14] = -3.0f;
    rt_mat4_rotate_y(rot, angle);
    rt_mat4_multiply(view_model, view, rot);
    rt_mat4_multiply(mvp, proj, view_model);
}

/* Read one 2D RGBA8 mip-0 layer back to malloc'd host memory (caller
 * frees). Returns NULL on any failure. Image must carry TRANSFER_SRC;
 * it is left sampled-readable (copy-helper contract). */
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
                                       staging->vk_buffer, 0) != LC_SUCCESS) {
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

static int make_color_image(lc_device *device, lc_format format, uint32_t w,
                            uint32_t h, int with_transfer_src,
                            lc_image **out_image, lc_image_view **out_view) {
    lc_image_desc idesc;
    lc_image_view_desc vdesc;

    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = format;
    idesc.width = w;
    idesc.height = h;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    /* Render-to-texture attachments must be sampled later: the pass
     * ends them in SHADER_READ, which requires SAMPLED usage.
     * TRANSFER_DST lets tests prime them sampled-readable before the
     * first bind (passes CLEAR anyway, discarding the primer). */
    idesc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                  LC_IMAGE_USAGE_TRANSFER_DST |
                  (with_transfer_src ? LC_IMAGE_USAGE_TRANSFER_SRC : 0);
    idesc.flags = LC_IMAGE_FLAG_NONE;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &idesc, out_image) != LC_SUCCESS) {
        return -1;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.format = LC_FORMAT_UNDEFINED;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.base_mip_level = 0;
    vdesc.mip_level_count = 1;
    vdesc.base_array_layer = 0;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(*out_image, &vdesc, out_view) != LC_SUCCESS) {
        lc_image_destroy(*out_image);
        *out_image = NULL;
        return -1;
    }
    return 0;
}

static int make_depth_image(lc_device *device, lc_format format, uint32_t w,
                            uint32_t h, lc_image **out_image,
                            lc_image_view **out_view) {
    lc_image_desc idesc;
    lc_image_view_desc vdesc;

    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = format;
    idesc.width = w;
    idesc.height = h;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL;
    idesc.flags = LC_IMAGE_FLAG_NONE;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &idesc, out_image) != LC_SUCCESS) {
        return -1;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.format = LC_FORMAT_UNDEFINED;
    vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
    vdesc.base_mip_level = 0;
    vdesc.mip_level_count = 1;
    vdesc.base_array_layer = 0;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(*out_image, &vdesc, out_view) != LC_SUCCESS) {
        lc_image_destroy(*out_image);
        *out_image = NULL;
        return -1;
    }
    return 0;
}

/* Present one trivial swapchain pass so every test frame ends with a
 * defined image (empty command buffers would present UNDEFINED). */
static int present_clear(lc_swapchain *swapchain, lc_command_encoder *enc) {    lc_render_swapchain_pass_desc spass;

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
    if (lc_encoder_begin_swapchain_pass(enc, swapchain, &spass) !=
        LC_SUCCESS) {
        return -1;
    }
    if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        return -1;
    }
    return 0;
}

int main(void) {
    printf("Running LumaC render-target integration test...\n");

    lc_shutdown();
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }

    /* ---- 1. live target/pipeline/encoder validation ---- */
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_image *img = NULL;
        lc_image_view *view = NULL;
        lc_image *depth_img = NULL;
        lc_image_view *depth_view = NULL;
        lc_render_target *target = NULL;
        lc_command_encoder *enc = NULL;
        int env;

        env = make_device(&device);
        if (env != 0) {
            if (env == 1) {
                SKIP_ENV("a Vulkan device");
            }
            printf("device creation failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        env = make_window_titled(&window, "LumaC RT Test");
        if (env != 0) {
            lc_device_destroy(device);
            if (env == 1) {
                SKIP_ENV("a native window");
            }
            printf("window creation failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        env = make_surface(device, window, &surface);
        if (env != 0) {
            lc_device_destroy(device);
            lc_window_destroy(window);
            if (env == 1) {
                SKIP_ENV("a presentation surface");
            }
            printf("surface creation failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        env = make_swapchain(device, surface, &swapchain);
        if (env != 0) {
            lc_surface_destroy(surface);
            lc_device_destroy(device);
            lc_window_destroy(window);
            if (env == 1) {
                SKIP_ENV("a working swapchain path");
            }
            printf("swapchain creation failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }

        /* Encoder needs an open frame. */
        TEST_CHECK(lc_swapchain_get_encoder(swapchain, &enc) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "get_encoder with no frame rejected");
        {
            lc_command_encoder *dead = (lc_command_encoder *)0x1;

            TEST_CHECK(lc_encoder_end_render_pass(dead) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "end pass(dead enc) rejected");
            TEST_CHECK(lc_encoder_draw(dead, 3, 0) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "draw(dead enc) rejected");
        }

        TEST_CHECK(make_color_image(device, LC_FORMAT_RGBA8_UNORM, 64, 64,
                                    1, &img, &view) == 0,
                   "validation color image+view created");
        TEST_CHECK(make_depth_image(device, LC_FORMAT_D16_UNORM, 64, 64,
                                    &depth_img, &depth_view) == 0,
                   "validation depth image+view created");

        /* Target shape misuse. */
        {
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment atts[2];
            lc_render_target *bad = NULL;

            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = 0;
            tdesc.height = 64;
            atts[0].view = view;
            tdesc.color_attachments = atts;
            tdesc.color_attachment_count = 1;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &bad) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "zero-width target rejected");
            TEST_CHECK(bad == NULL, "out cleared on zero extent");
            tdesc.width = 64;
            tdesc.color_attachment_count = 0;
            tdesc.depth_stencil_attachment = NULL;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &bad) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "attachment-less target rejected");
            tdesc.color_attachment_count = 1;
            tdesc.depth_stencil_attachment = depth_view;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &bad) ==
                       LC_SUCCESS,
                       "color+depth target created");
            TEST_CHECK(bad != NULL, "target handle non-null");
            TEST_CHECK(lc_render_target_get_width(bad) == 64,
                       "target width getter");
            TEST_CHECK(lc_render_target_get_height(bad) == 64,
                       "target height getter");
            TEST_CHECK(lc_render_target_get_color_count(bad) == 1,
                       "target color count getter");
            TEST_CHECK(lc_render_target_get_color_format(bad, 0) ==
                           LC_FORMAT_RGBA8_UNORM,
                       "target color format getter");
            TEST_CHECK(lc_render_target_get_color_format(bad, 5) ==
                           LC_FORMAT_UNDEFINED,
                       "target color format OOB -> UNDEFINED");
            TEST_CHECK(lc_render_target_get_depth_format(bad) ==
                           LC_FORMAT_D16_UNORM,
                       "target depth format getter");
            TEST_CHECK(lc_render_target_get_samples(bad) ==
                           LC_SAMPLE_COUNT_1,
                       "target samples getter");
            target = bad;
        }

        /* Swapchain target snapshot. */
        {
            lc_render_target *starget =
                lc_swapchain_get_render_target(swapchain);

            TEST_CHECK(starget != NULL, "swapchain target non-null");
            TEST_CHECK(lc_render_target_get_color_count(starget) == 1,
                       "swapchain target has 1 color");
            TEST_CHECK(lc_render_target_get_color_format(starget, 0) ==
                           lc_swapchain_get_format(swapchain),
                       "swapchain target color matches swapchain");
            TEST_CHECK(lc_render_target_get_depth_format(starget) ==
                           lc_swapchain_get_depth_format(swapchain),
                       "swapchain target depth matches swapchain");
        }

        /* Mismatched-dimension target rejected. */
        {
            lc_image *big = NULL;
            lc_image_view *big_view = NULL;
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment atts[2];
            lc_render_target *bad = NULL;

            TEST_CHECK(make_color_image(device, LC_FORMAT_RGBA8_UNORM, 32,
                                        32, 0, &big, &big_view) == 0,
                       "odd-size image created");
            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = 64;
            tdesc.height = 64;
            atts[0].view = view;
            atts[1].view = big_view;
            tdesc.color_attachments = atts;
            tdesc.color_attachment_count = 2;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &bad) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "dimension-mismatched target rejected");
            TEST_CHECK(bad == NULL, "out cleared on dimension mismatch");
            lc_image_view_destroy(big_view);
            lc_image_destroy(big);
        }

        /* Missing COLOR_ATTACHMENT usage rejected. */
        {
            lc_image_desc idesc;
            lc_image *plain = NULL;
            lc_image_view_desc vdesc;
            lc_image_view *plain_view = NULL;
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment att;
            lc_render_target *bad = NULL;

            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = 64;
            idesc.height = 64;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                          LC_IMAGE_USAGE_TRANSFER_DST;
            idesc.flags = LC_IMAGE_FLAG_NONE;
            idesc.samples = LC_SAMPLE_COUNT_1;
            TEST_CHECK(lc_image_create(device, &idesc, &plain) == LC_SUCCESS,
                       "sampled-only image created");
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.format = LC_FORMAT_UNDEFINED;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.base_mip_level = 0;
            vdesc.mip_level_count = 1;
            vdesc.base_array_layer = 0;
            vdesc.array_layer_count = 1;
            TEST_CHECK(lc_image_view_create(plain, &vdesc, &plain_view) ==
                           LC_SUCCESS,
                       "sampled-only view created");
            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = 64;
            tdesc.height = 64;
            att.view = plain_view;
            tdesc.color_attachments = &att;
            tdesc.color_attachment_count = 1;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &bad) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "non-attachment image rejected as target");
            lc_image_view_destroy(plain_view);
            lc_image_destroy(plain);
        }

        /* Encoder state machine on a live frame. */
        {
            lc_render_color_attachment catt;
            lc_render_pass_desc pdesc;
            lc_render_swapchain_pass_desc spass;

            TEST_CHECK(lc_begin_frame(swapchain) == LC_SUCCESS,
                       "frame begins for encoder validation");
            TEST_CHECK(lc_swapchain_get_encoder(swapchain, &enc) ==
                           LC_SUCCESS,
                       "encoder borrowed");
            TEST_CHECK(enc != NULL, "encoder non-null");
            TEST_CHECK(lc_encoder_end_render_pass(enc) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "end without begin rejected");
            TEST_CHECK(lc_encoder_draw(enc, 3, 0) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "draw outside pass rejected");
            TEST_CHECK(lc_encoder_bind_pipeline(enc, NULL) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "bind NULL pipeline rejected");

            /* Nested begins rejected; coherent begin works. */
            memset(&catt, 0, sizeof(catt));
            catt.view = view;
            catt.load_op = LC_LOAD_OP_CLEAR;
            catt.store_op = LC_STORE_OP_STORE;
            catt.clear_color[0] = 0.1f;
            catt.clear_color[1] = 0.1f;
            catt.clear_color[2] = 0.1f;
            catt.clear_color[3] = 1.0f;
            memset(&pdesc, 0, sizeof(pdesc));
            pdesc.color_attachments = &catt;
            pdesc.color_attachment_count = 1;
            pdesc.depth_attachment = NULL;
            pdesc.width = 64;
            pdesc.height = 64;
            /* NOTE: `target` carries depth, so this depthless desc
             * mismatches it and must fail (PART X exactness). */
            TEST_CHECK(lc_encoder_begin_render_pass(enc, &pdesc) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "depth-dropping begin on depth target rejected");
            {
                /* Depthless target for the positive begin below.
                 * NOTE: it must survive until after submit (its
                 * framebuffer is referenced by the unsubmitted command
                 * buffer), so it is destroyed after end_frame. */
                lc_render_target_create_desc tdesc;
                lc_render_target_attachment att;
                lc_render_target *flat = NULL;
                lc_result end_res = LC_SUCCESS;
                int ok = 0;

                memset(&tdesc, 0, sizeof(tdesc));
                tdesc.width = 64;
                tdesc.height = 64;
                att.view = view;
                tdesc.color_attachments = &att;
                tdesc.color_attachment_count = 1;
                TEST_CHECK(lc_render_target_create(device, &tdesc, &flat) ==
                               LC_SUCCESS,
                           "depthless target created");
                TEST_CHECK(lc_encoder_begin_render_pass(enc, &pdesc) ==
                               LC_SUCCESS,
                           "offscreen begin succeeds");
                TEST_CHECK(lc_encoder_begin_render_pass(enc, &pdesc) ==
                               LC_ERROR_INVALID_ARGUMENT,
                           "nested begin rejected");
                /* Legacy recording is locked out mid-pass. */
                TEST_CHECK(lc_clear_color(swapchain, 0, 0, 0, 1) ==
                               LC_ERROR_INVALID_ARGUMENT,
                           "legacy clear inside explicit pass rejected");
                TEST_CHECK(lc_encoder_end_render_pass(enc) == LC_SUCCESS,
                           "explicit end succeeds");
                TEST_CHECK(lc_encoder_end_render_pass(enc) ==
                               LC_ERROR_INVALID_ARGUMENT,
                           "double end rejected");
                /* Swapchain pass after offscreen: multi-pass frame. */
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
                TEST_CHECK(lc_encoder_begin_swapchain_pass(
                               enc, swapchain, &spass) == LC_SUCCESS,
                           "swapchain pass after offscreen succeeds");
                /* Swapchain LOAD must fail (UNDEFINED start). */
                TEST_CHECK(lc_encoder_end_render_pass(enc) == LC_SUCCESS,
                           "swapchain end succeeds");
                spass.color_load_op = LC_LOAD_OP_LOAD;
                TEST_CHECK(lc_encoder_begin_swapchain_pass(
                               enc, swapchain, &spass) ==
                               LC_ERROR_INVALID_ARGUMENT,
                           "swapchain LOAD rejected");
                spass.color_load_op = LC_LOAD_OP_CLEAR;
                end_res = lc_end_frame(swapchain);
                ok = (end_res == LC_SUCCESS || end_res == LC_SUBOPTIMAL ||
                      end_res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE);
                TEST_CHECK(ok, "validation frame ends clean");
                if (end_res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                    uint32_t w = lc_window_get_width(window);
                    uint32_t h = lc_window_get_height(window);

                    if (w != 0 && h != 0) {
                        lc_swapchain_recreate(swapchain, w, h);
                    }
                }
                /* Now submitted: the borrowed framebuffer is safe to
                 * release with the target. */
                lc_render_target_destroy(flat);
            }
        }

        /* End-while-open keeps the frame alive for recovery. */
        {
            lc_render_swapchain_pass_desc spass;

            TEST_CHECK(lc_begin_frame(swapchain) == LC_SUCCESS,
                       "frame begins for open-end test");
            TEST_CHECK(lc_swapchain_get_encoder(swapchain, &enc) ==
                           LC_SUCCESS,
                       "encoder re-borrowed");
            memset(&spass, 0, sizeof(spass));
            spass.color_load_op = LC_LOAD_OP_CLEAR;
            spass.color_store_op = LC_STORE_OP_STORE;
            spass.depth_load_op = LC_LOAD_OP_DONT_CARE;
            spass.depth_store_op = LC_STORE_OP_DONT_CARE;
            TEST_CHECK(lc_encoder_begin_swapchain_pass(enc, swapchain,
                                                       &spass) == LC_SUCCESS,
                       "swapchain pass opens");
            TEST_CHECK(lc_end_frame(swapchain) == LC_ERROR_INVALID_ARGUMENT,
                       "end with open pass rejected");
            TEST_CHECK(lc_encoder_end_render_pass(enc) == LC_SUCCESS,
                       "pass ends after rejected end");
            {
                lc_result res = lc_end_frame(swapchain);
                int ok = (res == LC_SUCCESS || res == LC_SUBOPTIMAL ||
                          res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE);

                TEST_CHECK(ok, "recovered frame ends clean");
                if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                    uint32_t w = lc_window_get_width(window);
                    uint32_t h = lc_window_get_height(window);

                    if (w != 0 && h != 0) {
                        lc_swapchain_recreate(swapchain, w, h);
                    }
                }
            }
        }

        /* Device MRT limit sanity (PART U exposure). */
        {
            lc_device_limits limits;

            lc_device_get_limits(device, &limits);
            TEST_CHECK(limits.max_color_attachments >= 1 &&
                           limits.max_color_attachments <=
                               LC_MAX_COLOR_ATTACHMENTS,
                       "max_color_attachments within structural bounds");
        }

        /* View destruction invalidates dependent targets (PART X):
         * passes referencing the dead view are rejected by liveness,
         * never dereferenced. */
        {
            lc_image *ximg = NULL;
            lc_image_view *xview = NULL;
            lc_render_target *xtarget = NULL;
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment att;

            TEST_CHECK(make_color_image(device, LC_FORMAT_RGBA8_UNORM, 32,
                                        32, 0, &ximg, &xview) == 0,
                       "invalidation image+view");
            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = 32;
            tdesc.height = 32;
            att.view = xview;
            tdesc.color_attachments = &att;
            tdesc.color_attachment_count = 1;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &xtarget) ==
                           LC_SUCCESS,
                       "invalidation target created");
            /* Destroying the view takes the borrowing target with it
             * (hook); the image itself stays live. */
            {
                lc_image_view *dead = xview;

                lc_image_view_destroy(xview);
                xview = NULL;
                xtarget = NULL;
                {
                    lc_render_color_attachment catt;
                    lc_render_pass_desc pdesc;

                    TEST_CHECK(lc_begin_frame(swapchain) == LC_SUCCESS,
                               "frame begins for dead-view test");
                    TEST_CHECK(lc_swapchain_get_encoder(swapchain, &enc) ==
                                   LC_SUCCESS,
                               "encoder for dead-view test");
                    memset(&catt, 0, sizeof(catt));
                    /* Dangling view address used by comparison only;
                     * validation rejects it without dereferencing. */
                    catt.view = dead;
                catt.load_op = LC_LOAD_OP_CLEAR;
                catt.store_op = LC_STORE_OP_DONT_CARE;
                memset(&pdesc, 0, sizeof(pdesc));
                pdesc.color_attachments = &catt;
                pdesc.color_attachment_count = 1;
                pdesc.width = 32;
                pdesc.height = 32;
                TEST_CHECK(lc_encoder_begin_render_pass(enc, &pdesc) ==
                               LC_ERROR_INVALID_ARGUMENT,
                           "begin with dead view rejected");
                {
                    lc_result res = lc_end_frame(swapchain);
                    int ok = (res == LC_SUCCESS || res == LC_SUBOPTIMAL ||
                              res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE);

                    TEST_CHECK(ok, "dead-view frame ends clean");
                    if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                        uint32_t w = lc_window_get_width(window);
                        uint32_t h = lc_window_get_height(window);

                        if (w != 0 && h != 0) {
                            lc_swapchain_recreate(swapchain, w, h);
                        }
                    }
                }
                }
            }
            lc_image_destroy(ximg);
        }

        lc_render_target_destroy(target);
        lc_image_view_destroy(view);
        lc_image_destroy(img);
        lc_image_view_destroy(depth_view);
        lc_image_destroy(depth_img);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "validation teardown clean");
    }

    /* ---- 2. MRT: two constant attachments, exact readback ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_command_encoder *enc = NULL;
        lc_image *mrt0 = NULL;
        lc_image *mrt1 = NULL;
        lc_image *mdepth = NULL;
        lc_image_view *mrt0_view = NULL;
        lc_image_view *mrt1_view = NULL;
        lc_image_view *mdepth_view = NULL;
        lc_render_target *mrt_target = NULL;
        lc_shader *vs = NULL;
        lc_shader *fs = NULL;
        lc_shader *fs1 = NULL;
        lc_pipeline *mrt_pipeline = NULL;
        lc_pipeline *solo_pipeline = NULL;
        int env;
        int presented = 0;
        int guard = 0;

        env = make_device(&device);
        TEST_CHECK(env == 0, "device for MRT test");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC RT Test") == 0,
                   "window for MRT test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for MRT test");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for MRT test");
        TEST_CHECK(make_color_image(device, LC_FORMAT_RGBA8_UNORM, 64, 64,
                                    1, &mrt0, &mrt0_view) == 0,
                   "MRT attachment 0 created");
        TEST_CHECK(make_color_image(device, LC_FORMAT_RGBA8_UNORM, 64, 64,
                                    1, &mrt1, &mrt1_view) == 0,
                   "MRT attachment 1 created");
        TEST_CHECK(make_depth_image(device, LC_FORMAT_D16_UNORM, 64, 64,
                                    &mdepth, &mdepth_view) == 0,
                   "MRT depth created");
        {
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment atts[2];

            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = 64;
            tdesc.height = 64;
            atts[0].view = mrt0_view;
            atts[1].view = mrt1_view;
            tdesc.color_attachments = atts;
            tdesc.color_attachment_count = 2;
            tdesc.depth_stencil_attachment = mdepth_view;
            TEST_CHECK(lc_render_target_create(device, &tdesc,
                                               &mrt_target) == LC_SUCCESS,
                       "2-attachment MRT target created");
        }
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_VERTEX, "quad.vert.spv",
                               &vs) == 0,
                   "MRT vertex shader loaded");
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_FRAGMENT, "mrt.frag.spv",
                               &fs) == 0,
                   "MRT fragment shader loaded");
        /* Solo sibling: single-output, descriptor-free shader so the
         * 1-color pipeline is itself valid (only the bind mismatches). */
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_FRAGMENT,
                               "checker.frag.spv", &fs1) == 0,
                   "solo fragment shader loaded");
        {
            /* MRT pipeline (2 colors + depth, depth unused) and an
             * incompatible 1-color sibling sharing the shaders. */
            lc_graphics_pipeline_desc pdesc = { 0 };

            pdesc.vertex_shader = vs;
            pdesc.fragment_shader = fs;
            pdesc.render_target.color_attachment_count = 2;
            pdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
            pdesc.render_target.color_formats[1] = LC_FORMAT_RGBA8_UNORM;
            pdesc.render_target.depth_stencil_format = LC_FORMAT_D16_UNORM;
            pdesc.render_target.samples = LC_SAMPLE_COUNT_1;
            TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc,
                                                   &mrt_pipeline) ==
                           LC_SUCCESS,
                       "MRT pipeline created");
            pdesc.fragment_shader = fs1;
            pdesc.render_target.color_attachment_count = 1;
            pdesc.render_target.depth_stencil_format = LC_FORMAT_UNDEFINED;
            TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc,
                                                   &solo_pipeline) ==
                           LC_SUCCESS,
                       "solo pipeline created");
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           mrt_target, mrt_pipeline) != 0,
                       "MRT target compatible with MRT pipeline");
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           mrt_target, solo_pipeline) == 0,
                       "MRT target incompatible with solo pipeline");
        }
        while (presented < 1) {
            lc_render_color_attachment catts[2];
            lc_render_depth_attachment datt;
            lc_render_pass_desc pdesc;
            lc_result res;

            if (++guard > 60) {
                break;
            }
            lc_poll_events();
            {
                uint32_t w = lc_window_get_width(window);
                uint32_t h = lc_window_get_height(window);

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
                        break;
                    }
                }
            }
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
                lc_swapchain_get_encoder(swapchain, &enc) != LC_SUCCESS) {
                break;
            }
            memset(&catts, 0, sizeof(catts));
            catts[0].view = mrt0_view;
            catts[0].load_op = LC_LOAD_OP_CLEAR;
            catts[0].store_op = LC_STORE_OP_STORE;
            catts[1].view = mrt1_view;
            catts[1].load_op = LC_LOAD_OP_CLEAR;
            catts[1].store_op = LC_STORE_OP_STORE;
            memset(&datt, 0, sizeof(datt));
            datt.view = mdepth_view;
            datt.depth_load_op = LC_LOAD_OP_CLEAR;
            datt.depth_store_op = LC_STORE_OP_DONT_CARE;
            datt.clear_depth = 1.0f;
            datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
            datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
            memset(&pdesc, 0, sizeof(pdesc));
            pdesc.color_attachments = catts;
            pdesc.color_attachment_count = 2;
            pdesc.depth_attachment = &datt;
            pdesc.width = 64;
            pdesc.height = 64;
            if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
                break;
            }
            /* Incompatible bind rejected structurally (not pointer). */
            if (lc_encoder_bind_pipeline(enc, solo_pipeline) !=
                LC_ERROR_PIPELINE_INCOMPATIBLE) {
                printf("solo bind in MRT pass not rejected: FAIL\n");
                lc_encoder_end_render_pass(enc);
                break;
            }
            if (lc_encoder_bind_pipeline(enc, mrt_pipeline) != LC_SUCCESS ||
                lc_encoder_draw(enc, 3, 0) != LC_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS ||
                present_clear(swapchain, enc) != 0) {
                break;
            }
            res = lc_end_frame(swapchain);
            if (res == LC_SUCCESS || res == LC_SUBOPTIMAL) {
                presented++;
            } else if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                uint32_t w = lc_window_get_width(window);
                uint32_t h = lc_window_get_height(window);

                if (w != 0 && h != 0) {
                    lc_swapchain_recreate(swapchain, w, h);
                }
            } else {
                break;
            }
        }
        TEST_CHECK(presented == 1, "MRT frame presented");
        {
            unsigned char *px0 =
                readback_rgba8(device, mrt0, 64, 64);
            unsigned char *px1 =
                readback_rgba8(device, mrt1, 64, 64);
            int ok0 = 0;
            int ok1 = 0;

            /* Center of the fullscreen triangle must carry the
             * constant outputs (overscan corners excluded). */
            if (px0 != NULL) {
                unsigned char *c = &px0[(32u * 64u + 16u) * 4u];

                ok0 = (c[0] == 255 && c[1] == 0 && c[2] == 0 && c[3] == 255);
            }
            if (px1 != NULL) {
                unsigned char *c = &px1[(32u * 64u + 16u) * 4u];

                ok1 = (c[0] == 0 && c[1] == 255 && c[2] == 0 && c[3] == 255);
            }
            TEST_CHECK(ok0, "MRT attachment 0 is solid red");
            TEST_CHECK(ok1, "MRT attachment 1 is solid green");
            free(px0);
            free(px1);
        }
        lc_pipeline_destroy(solo_pipeline);
        lc_pipeline_destroy(mrt_pipeline);
        lc_shader_destroy(fs1);
        lc_shader_destroy(fs);
        lc_shader_destroy(vs);
        lc_render_target_destroy(mrt_target);
        lc_image_view_destroy(mrt0_view);
        lc_image_view_destroy(mrt1_view);
        lc_image_view_destroy(mdepth_view);
        lc_image_destroy(mrt0);
        lc_image_destroy(mrt1);
        lc_image_destroy(mdepth);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "MRT teardown clean");
    }

    /* ---- 3. two-pass checker: procedural -> texA -> sampled -> texB ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_command_encoder *enc = NULL;
        lc_image *img_a = NULL;
        lc_image *img_b = NULL;
        lc_image_view *view_a = NULL;
        lc_image_view *view_b = NULL;
        lc_render_target *target_a = NULL;
        lc_render_target *target_b = NULL;
        lc_sampler *samp = NULL;
        lc_shader *vs = NULL;
        lc_shader *fs_check = NULL;
        lc_shader *fs_sample = NULL;
        lc_binding_layout *layout = NULL;
        lc_binding_set *set = NULL;
        lc_pipeline *pipe_check = NULL;
        lc_pipeline *pipe_sample = NULL;
        int env;
        int presented = 0;
        int guard = 0;

        env = make_device(&device);
        TEST_CHECK(env == 0, "device for two-pass test");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC RT Test") == 0,
                   "window for two-pass test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for two-pass test");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for two-pass test");
        TEST_CHECK(make_color_image(device, LC_FORMAT_RGBA8_UNORM, 64, 64,
                                    1, &img_a, &view_a) == 0,
                   "texA created");
        TEST_CHECK(make_color_image(device, LC_FORMAT_RGBA8_UNORM, 64, 64,
                                    1, &img_b, &view_b) == 0,
                   "texB created");
        /* Prime both targets sampled-readable before any set points at
         * them (binding validates sampled state; the first CLEAR pass
         * discards the primer). */
        {
            lc_image_upload_desc primer;
            static unsigned char zeros[64 * 64 * 4];

            memset(&primer, 0, sizeof(primer));
            primer.mip_level = 0;
            primer.array_layer = 0;
            primer.width = 64;
            primer.height = 64;
            primer.depth = 1;
            primer.data = zeros;
            primer.data_size = sizeof(zeros);
            TEST_CHECK(lc_image_write(img_a, &primer) == LC_SUCCESS,
                       "texA primed sampled-readable");
            TEST_CHECK(lc_image_write(img_b, &primer) == LC_SUCCESS,
                       "texB primed sampled-readable");
        }
        {
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment att;

            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = 64;
            tdesc.height = 64;
            att.view = view_a;
            tdesc.color_attachments = &att;
            tdesc.color_attachment_count = 1;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &target_a) ==
                           LC_SUCCESS,
                       "depthless target A created");
            att.view = view_b;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &target_b) ==
                           LC_SUCCESS,
                       "depthless target B created");
        }
        {
            lc_sampler_desc smdesc;

            memset(&smdesc, 0, sizeof(smdesc));
            smdesc.min_filter = LC_FILTER_NEAREST;
            smdesc.mag_filter = LC_FILTER_NEAREST;
            smdesc.mipmap_mode = LC_MIPMAP_MODE_NEAREST;
            smdesc.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
            smdesc.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
            smdesc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
            smdesc.max_anisotropy = 1.0f;
            TEST_CHECK(lc_sampler_create(device, &smdesc, &samp) ==
                           LC_SUCCESS,
                       "nearest sampler created");
        }
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_VERTEX, "quad.vert.spv",
                               &vs) == 0,
                   "quad vertex loaded");
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_FRAGMENT,
                               "checker.frag.spv", &fs_check) == 0,
                   "checker fragment loaded");
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_FRAGMENT,
                               "quad.frag.spv", &fs_sample) == 0,
                   "sample fragment loaded");
        {
            lc_graphics_pipeline_desc pdesc = { 0 };

            pdesc.vertex_shader = vs;
            pdesc.fragment_shader = fs_check;
            pdesc.render_target.color_attachment_count = 1;
            pdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
            pdesc.render_target.samples = LC_SAMPLE_COUNT_1;
            TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc,
                                                   &pipe_check) == LC_SUCCESS,
                       "checker pipeline created");
        }
        {
            lc_binding_desc slots[2];
            lc_binding_layout_desc ldesc;
            lc_binding_write writes[2];
            lc_graphics_pipeline_desc pdesc = { 0 };

            slots[0].binding = 0;
            slots[0].type = LC_BINDING_SAMPLED_IMAGE;
            slots[0].count = 1;
            slots[0].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
            slots[1].binding = 1;
            slots[1].type = LC_BINDING_SAMPLER;
            slots[1].count = 1;
            slots[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
            ldesc.bindings = slots;
            ldesc.binding_count = 2;
            TEST_CHECK(lc_binding_layout_create(device, &ldesc, &layout) ==
                           LC_SUCCESS,
                       "sample layout created");
            pdesc.vertex_shader = vs;
            pdesc.fragment_shader = fs_sample;
            {
                const lc_binding_layout *sl[1];

                sl[0] = layout;
                pdesc.binding_layouts = sl;
                pdesc.binding_layout_count = 1;
                pdesc.render_target.color_attachment_count = 1;
                pdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
                pdesc.render_target.samples = LC_SAMPLE_COUNT_1;
                TEST_CHECK(lc_graphics_pipeline_create(
                               device, &pdesc, &pipe_sample) ==
                               LC_SUCCESS,
                           "sample pipeline created");
            }
            TEST_CHECK(lc_binding_set_create(layout, &set) == LC_SUCCESS,
                       "sample set created");
            writes[0].binding = 0;
            writes[0].array_element = 0;
            writes[0].type = LC_BINDING_SAMPLED_IMAGE;
            writes[0].u.image.view = view_a;
            writes[1].binding = 1;
            writes[1].array_element = 0;
            writes[1].type = LC_BINDING_SAMPLER;
            writes[1].u.sampler.sampler = samp;
            TEST_CHECK(lc_binding_set_update(set, writes, 2) == LC_SUCCESS,
                       "sample set points at texA");
            /* Equivalent targets share the pipeline (content match). */
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           target_a, pipe_sample) != 0,
                       "target A compatible with sample pipeline");
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           target_b, pipe_sample) != 0,
                       "identical target B shares the pipeline");
        }
        /* One two-pass frame: checker -> A, sample A -> B, present. */
        while (presented < 1) {
            lc_render_color_attachment catt;
            lc_render_pass_desc pdesc;
            lc_result res;

            if (++guard > 60) {
                break;
            }
            lc_poll_events();
            {
                uint32_t w = lc_window_get_width(window);
                uint32_t h = lc_window_get_height(window);

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
                        break;
                    }
                }
            }
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
                lc_swapchain_get_encoder(swapchain, &enc) != LC_SUCCESS) {
                break;
            }
            memset(&catt, 0, sizeof(catt));
            catt.view = view_a;
            catt.load_op = LC_LOAD_OP_CLEAR;
            catt.store_op = LC_STORE_OP_STORE;
            memset(&pdesc, 0, sizeof(pdesc));
            pdesc.color_attachments = &catt;
            pdesc.color_attachment_count = 1;
            pdesc.width = 64;
            pdesc.height = 64;
            if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS ||
                lc_encoder_bind_pipeline(enc, pipe_check) != LC_SUCCESS ||
                lc_encoder_draw(enc, 3, 0) != LC_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                break;
            }
            catt.view = view_b;
            if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS ||
                lc_encoder_bind_pipeline(enc, pipe_sample) != LC_SUCCESS ||
                lc_encoder_bind_binding_set(enc, pipe_sample, 0, set) !=
                    LC_SUCCESS ||
                lc_encoder_draw(enc, 3, 0) != LC_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS ||
                present_clear(swapchain, enc) != 0) {
                break;
            }
            res = lc_end_frame(swapchain);
            if (res == LC_SUCCESS || res == LC_SUBOPTIMAL) {
                presented++;
            } else if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                uint32_t w = lc_window_get_width(window);
                uint32_t h = lc_window_get_height(window);

                if (w != 0 && h != 0) {
                    lc_swapchain_recreate(swapchain, w, h);
                }
            } else {
                break;
            }
        }
        TEST_CHECK(presented == 1, "two-pass frame presented");
        {
            /* texB must hold texA's checkerboard (double-sampled):
             * white cells exact, dark cells near 13, alpha opaque. */
            unsigned char *px = readback_rgba8(device, img_b, 64, 64);
            int ok = 0;

            if (px != NULL) {
                unsigned char *w00 = &px[(8u * 64u + 8u) * 4u];
                unsigned char *d10 = &px[(8u * 64u + 24u) * 4u];
                unsigned char *d01 = &px[(24u * 64u + 8u) * 4u];
                unsigned char *w11 = &px[(24u * 64u + 24u) * 4u];
                unsigned char *w22 = &px[(40u * 64u + 40u) * 4u];

                ok = (w00[0] == 255 && w00[1] == 255 && w00[2] == 255 &&
                      w00[3] == 255 && w11[0] == 255 && w22[0] == 255 &&
                      d10[3] == 255 && d01[3] == 255 &&
                      d10[0] <= 16 && d10[1] <= 16 && d10[2] <= 16 &&
                      d01[0] <= 16 && d01[1] <= 16 && d01[2] <= 16);
            }
            TEST_CHECK(ok, "texB carries the checkerboard via sampling");
            free(px);
        }
        /* LOAD-preserve: empty LOAD pass on A, re-sample to B, same. */
        {
            int ok = 0;
            int done = 0;

            guard = 0;
            while (!done) {
                lc_render_color_attachment catt;
                lc_render_pass_desc pdesc;
                lc_result res;

                if (++guard > 60) {
                    break;
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
                    lc_swapchain_get_encoder(swapchain, &enc) != LC_SUCCESS) {
                    break;
                }
                memset(&catt, 0, sizeof(catt));
                catt.view = view_a;
                catt.load_op = LC_LOAD_OP_LOAD;
                catt.store_op = LC_STORE_OP_STORE;
                memset(&pdesc, 0, sizeof(pdesc));
                pdesc.color_attachments = &catt;
                pdesc.color_attachment_count = 1;
                pdesc.width = 64;
                pdesc.height = 64;
                if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS ||
                    lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                    break;
                }
                catt.view = view_b;
                catt.load_op = LC_LOAD_OP_CLEAR;
                if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS ||
                    lc_encoder_bind_pipeline(enc, pipe_sample) !=
                        LC_SUCCESS ||
                    lc_encoder_bind_binding_set(enc, pipe_sample, 0, set) !=
                        LC_SUCCESS ||
                    lc_encoder_draw(enc, 3, 0) != LC_SUCCESS ||
                    lc_encoder_end_render_pass(enc) != LC_SUCCESS ||
                    present_clear(swapchain, enc) != 0) {
                    break;
                }
                res = lc_end_frame(swapchain);
                if (res == LC_SUCCESS || res == LC_SUBOPTIMAL) {
                    done = 1;
                } else if (res != LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                    break;
                }
            }
            TEST_CHECK(done, "LOAD-preserve frame presented");
            if (done) {
                unsigned char *px = readback_rgba8(device, img_b, 64, 64);

                if (px != NULL) {
                    unsigned char *w00 = &px[(8u * 64u + 8u) * 4u];
                    unsigned char *d10 = &px[(8u * 64u + 24u) * 4u];

                    ok = (w00[0] == 255 && w00[1] == 255 && w00[2] == 255 &&
                          d10[0] <= 16 && d10[3] == 255);
                }
                free(px);
            }
            TEST_CHECK(ok, "LOAD preserved texA across passes");
        }
        lc_pipeline_destroy(pipe_sample);
        lc_pipeline_destroy(pipe_check);
        lc_binding_set_destroy(set);
        lc_binding_layout_destroy(layout);
        lc_shader_destroy(fs_sample);
        lc_shader_destroy(fs_check);
        lc_shader_destroy(vs);
        lc_sampler_destroy(samp);
        lc_render_target_destroy(target_b);
        lc_render_target_destroy(target_a);
        lc_image_view_destroy(view_b);
        lc_image_view_destroy(view_a);
        lc_image_destroy(img_b);
        lc_image_destroy(img_a);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "two-pass teardown clean");
    }

    /* ---- 4. sharing matrix + depth-only targets ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_command_encoder *enc = NULL;
        lc_image *img_a = NULL;
        lc_image *img_b = NULL;
        lc_image *img_c = NULL;
        lc_image *img_d = NULL;
        lc_image *img_e = NULL;
        lc_image_view *view_a = NULL;
        lc_image_view *view_b = NULL;
        lc_image_view *view_c = NULL;
        lc_image_view *view_d = NULL;
        lc_image_view *view_e = NULL;
        lc_render_target *target_a = NULL;
        lc_render_target *target_b = NULL;
        lc_render_target *target_c = NULL;
        lc_render_target *target_d = NULL;
        lc_render_target *target_e = NULL;
        lc_shader *vs = NULL;
        lc_shader *fs = NULL;
        lc_pipeline *pipeline = NULL;
        int env;

        env = make_device(&device);
        TEST_CHECK(env == 0, "device for sharing test");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC RT Test") == 0,
                   "window for sharing test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for sharing test");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for sharing test");

        /* A/B identical, C different color, D depthless, E samples-2. */
        TEST_CHECK(make_color_image(device, LC_FORMAT_RGBA8_UNORM, 32, 32,
                                    0, &img_a, &view_a) == 0,
                   "share image A");
        TEST_CHECK(make_color_image(device, LC_FORMAT_RGBA8_UNORM, 32, 32,
                                    0, &img_b, &view_b) == 0,
                   "share image B");
        TEST_CHECK(make_color_image(device, LC_FORMAT_BGRA8_UNORM, 32, 32,
                                    0, &img_c, &view_c) == 0,
                   "share image C (BGRA)");
        TEST_CHECK(make_color_image(device, LC_FORMAT_RGBA8_UNORM, 32, 32,
                                    0, &img_d, &view_d) == 0,
                   "share image D");
        {
            lc_image_desc idesc;
            lc_image_view_desc vdesc;

            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = 32;
            idesc.height = 32;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                          LC_IMAGE_USAGE_COLOR_ATTACHMENT;
            idesc.flags = LC_IMAGE_FLAG_NONE;
            idesc.samples = LC_SAMPLE_COUNT_2;
            TEST_CHECK(lc_image_create(device, &idesc, &img_e) == LC_SUCCESS,
                       "share image E (2 samples)");
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.format = LC_FORMAT_UNDEFINED;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.base_mip_level = 0;
            vdesc.mip_level_count = 1;
            vdesc.base_array_layer = 0;
            vdesc.array_layer_count = 1;
            TEST_CHECK(lc_image_view_create(img_e, &vdesc, &view_e) ==
                           LC_SUCCESS,
                       "share view E");
        }
        {
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment att;

            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = 32;
            tdesc.height = 32;
            att.view = view_a;
            tdesc.color_attachments = &att;
            tdesc.color_attachment_count = 1;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &target_a) ==
                           LC_SUCCESS,
                       "target A created");
            att.view = view_b;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &target_b) ==
                           LC_SUCCESS,
                       "identical target B created");
            att.view = view_c;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &target_c) ==
                           LC_SUCCESS,
                       "BGRA target C created");
            att.view = view_d;
            /* D stays depthless like A: identical on purpose would
             * trivialize; instead D gets no... keep D identical too
             * and rely on C/E for mismatch (D tests double-create). */
            TEST_CHECK(lc_render_target_create(device, &tdesc, &target_d) ==
                           LC_SUCCESS,
                       "target D created");
            att.view = view_e;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &target_e) ==
                           LC_SUCCESS,
                       "samples-2 target E created");
        }
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_VERTEX, "quad.vert.spv",
                               &vs) == 0,
                   "share vertex shader");
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_FRAGMENT,
                               "checker.frag.spv", &fs) == 0,
                   "share fragment shader");
        {
            lc_graphics_pipeline_desc pdesc = { 0 };

            pdesc.vertex_shader = vs;
            pdesc.fragment_shader = fs;
            pdesc.render_target.color_attachment_count = 1;
            pdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
            pdesc.render_target.samples = LC_SAMPLE_COUNT_1;
            TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc,
                                                   &pipeline) == LC_SUCCESS,
                       "A-signature pipeline created");
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           target_a, pipeline) != 0,
                       "A compatible");
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           target_b, pipeline) != 0,
                       "identical B shares the pipeline");
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           target_d, pipeline) != 0,
                       "identical D shares the pipeline");
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           target_c, pipeline) == 0,
                       "BGRA C rejected (format)");
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           target_e, pipeline) == 0,
                       "samples-2 E rejected (samples)");
        }
        /* Recording-time rejection on each mismatched target. */
        {
            lc_render_color_attachment catt;
            lc_render_pass_desc pdesc;

            TEST_CHECK(lc_begin_frame(swapchain) == LC_SUCCESS,
                       "frame begins for bind matrix");
            TEST_CHECK(lc_swapchain_get_encoder(swapchain, &enc) ==
                           LC_SUCCESS,
                       "encoder for bind matrix");
            memset(&catt, 0, sizeof(catt));
            catt.load_op = LC_LOAD_OP_CLEAR;
            catt.store_op = LC_STORE_OP_DONT_CARE;
            memset(&pdesc, 0, sizeof(pdesc));
            pdesc.color_attachments = &catt;
            pdesc.color_attachment_count = 1;
            pdesc.width = 32;
            pdesc.height = 32;
            catt.view = view_b;
            TEST_CHECK(lc_encoder_begin_render_pass(enc, &pdesc) ==
                           LC_SUCCESS,
                       "begin on identical B");
            TEST_CHECK(lc_encoder_bind_pipeline(enc, pipeline) == LC_SUCCESS,
                       "bind on identical B succeeds");
            TEST_CHECK(lc_encoder_end_render_pass(enc) == LC_SUCCESS,
                       "end B");
            catt.view = view_c;
            TEST_CHECK(lc_encoder_begin_render_pass(enc, &pdesc) ==
                           LC_SUCCESS,
                       "begin on BGRA C (pass itself valid)");
            TEST_CHECK(lc_encoder_bind_pipeline(enc, pipeline) ==
                           LC_ERROR_PIPELINE_INCOMPATIBLE,
                       "bind on C rejected (format)");
            TEST_CHECK(lc_encoder_end_render_pass(enc) == LC_SUCCESS,
                       "end C");
            catt.view = view_e;
            TEST_CHECK(lc_encoder_begin_render_pass(enc, &pdesc) ==
                           LC_SUCCESS,
                       "begin on samples-2 E");
            TEST_CHECK(lc_encoder_bind_pipeline(enc, pipeline) ==
                           LC_ERROR_PIPELINE_INCOMPATIBLE,
                       "bind on E rejected (samples)");
            TEST_CHECK(lc_encoder_end_render_pass(enc) == LC_SUCCESS,
                       "end E");
            TEST_CHECK(present_clear(swapchain, enc) == 0,
                       "matrix frame presents");
            {
                lc_result res = lc_end_frame(swapchain);
                int ok = (res == LC_SUCCESS || res == LC_SUBOPTIMAL ||
                          res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE);

                TEST_CHECK(ok, "matrix frame ends clean");
                if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                    uint32_t w = lc_window_get_width(window);
                    uint32_t h = lc_window_get_height(window);

                    if (w != 0 && h != 0) {
                        lc_swapchain_recreate(swapchain, w, h);
                    }
                }
            }
        }
        /* Depth-only target: creation + getters + compat rejections
         * (no draw: all local fragment shaders write color, which a
         * zero-color subpass must not consume). */
        {
            lc_image *dimg = NULL;
            lc_image_view *dview = NULL;
            lc_image *dimg32 = NULL;
            lc_image_view *dview32 = NULL;
            lc_render_target *donly = NULL;
            lc_render_target *donly32 = NULL;
            lc_render_target_create_desc tdesc;

            TEST_CHECK(make_depth_image(device, LC_FORMAT_D32_FLOAT, 32, 32,
                                        &dimg, &dview) == 0,
                       "depth-only image+view");
            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = 32;
            tdesc.height = 32;
            tdesc.depth_stencil_attachment = dview;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &donly) ==
                           LC_SUCCESS,
                       "depth-only target created");
            TEST_CHECK(lc_render_target_get_color_count(donly) == 0,
                       "depth-only has 0 colors");
            TEST_CHECK(lc_render_target_get_depth_format(donly) ==
                           LC_FORMAT_D32_FLOAT,
                       "depth-only format getter");
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           donly, pipeline) == 0,
                       "depth-only target rejects color pipeline");
            /* Same colors, different depth format -> incompatible. */
            TEST_CHECK(make_depth_image(device, LC_FORMAT_D16_UNORM, 32, 32,
                                        &dimg32, &dview32) == 0,
                       "D16 depth image+view");
            {
                lc_render_target_create_desc t2;
                lc_render_target_attachment att;

                memset(&t2, 0, sizeof(t2));
                t2.width = 32;
                t2.height = 32;
                att.view = view_a;
                t2.color_attachments = &att;
                t2.color_attachment_count = 1;
                t2.depth_stencil_attachment = dview32;
                TEST_CHECK(lc_render_target_create(device, &t2, &donly32) ==
                               LC_SUCCESS,
                           "D16-depth variant target created");
            }
            {
                /* Pipeline carrying D32 depth vs D16 target. */
                lc_graphics_pipeline_desc pd = { 0 };
                lc_pipeline *dp = NULL;

                pd.vertex_shader = vs;
                pd.fragment_shader = fs;
                pd.depth_test_enable = 1;
                pd.depth_write_enable = 1;
                pd.render_target.color_attachment_count = 1;
                pd.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
                pd.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
                pd.render_target.samples = LC_SAMPLE_COUNT_1;
                TEST_CHECK(lc_graphics_pipeline_create(device, &pd, &dp) ==
                               LC_SUCCESS,
                           "D32-depth pipeline created");
                TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                               donly32, dp) == 0,
                           "D16 target rejects D32 pipeline (depth)");
                lc_pipeline_destroy(dp);
            }
            lc_render_target_destroy(donly32);
            lc_render_target_destroy(donly);
            lc_image_view_destroy(dview32);
            lc_image_destroy(dimg32);
            lc_image_view_destroy(dview);
            lc_image_destroy(dimg);
        }
        lc_pipeline_destroy(pipeline);
        lc_shader_destroy(fs);
        lc_shader_destroy(vs);
        lc_render_target_destroy(target_e);
        lc_render_target_destroy(target_d);
        lc_render_target_destroy(target_c);
        lc_render_target_destroy(target_b);
        lc_render_target_destroy(target_a);
        lc_image_view_destroy(view_e);
        lc_image_view_destroy(view_d);
        lc_image_view_destroy(view_c);
        lc_image_view_destroy(view_b);
        lc_image_view_destroy(view_a);
        lc_image_destroy(img_e);
        lc_image_destroy(img_d);
        lc_image_destroy(img_c);
        lc_image_destroy(img_b);
        lc_image_destroy(img_a);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "sharing teardown clean");
    }

    /* ---- 5. cube two-pass endurance: 120 frames, resize, sharing ----
     * Offscreen textured cube (indexed, depth, push identity MVP) +
     * swapchain sampling quad. Fixed 256x256 offscreen target must
     * survive every swapchain recreate; two windows share one target
     * and both pipelines. */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        typedef struct cube_vertex {
            float position[3];
            float uv[2];
        } cube_vertex;

        static const cube_vertex k_cube[24] = {
            { { -0.5f, -0.5f, 0.5f }, { 0.0f, 0.0f } },
            { { 0.5f, -0.5f, 0.5f }, { 1.0f, 0.0f } },
            { { 0.5f, 0.5f, 0.5f }, { 1.0f, 1.0f } },
            { { -0.5f, 0.5f, 0.5f }, { 0.0f, 1.0f } },
            { { 0.5f, -0.5f, -0.5f }, { 0.0f, 0.0f } },
            { { -0.5f, -0.5f, -0.5f }, { 1.0f, 0.0f } },
            { { -0.5f, 0.5f, -0.5f }, { 1.0f, 1.0f } },
            { { 0.5f, 0.5f, -0.5f }, { 0.0f, 1.0f } },
            { { 0.5f, -0.5f, 0.5f }, { 0.0f, 0.0f } },
            { { 0.5f, -0.5f, -0.5f }, { 1.0f, 0.0f } },
            { { 0.5f, 0.5f, -0.5f }, { 1.0f, 1.0f } },
            { { 0.5f, 0.5f, 0.5f }, { 0.0f, 1.0f } },
            { { -0.5f, -0.5f, -0.5f }, { 0.0f, 0.0f } },
            { { -0.5f, -0.5f, 0.5f }, { 1.0f, 0.0f } },
            { { -0.5f, 0.5f, 0.5f }, { 1.0f, 1.0f } },
            { { -0.5f, 0.5f, -0.5f }, { 0.0f, 1.0f } },
            { { -0.5f, 0.5f, 0.5f }, { 0.0f, 0.0f } },
            { { 0.5f, 0.5f, 0.5f }, { 1.0f, 0.0f } },
            { { 0.5f, 0.5f, -0.5f }, { 1.0f, 1.0f } },
            { { -0.5f, 0.5f, -0.5f }, { 0.0f, 1.0f } },
            { { -0.5f, -0.5f, -0.5f }, { 0.0f, 0.0f } },
            { { 0.5f, -0.5f, -0.5f }, { 1.0f, 0.0f } },
            { { 0.5f, -0.5f, 0.5f }, { 1.0f, 1.0f } },
            { { -0.5f, -0.5f, 0.5f }, { 0.0f, 1.0f } },
        };
        static const uint16_t k_idx[36] = {
            0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11,
            12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22,
            20, 22, 23,
        };

        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_command_encoder *enc = NULL;
        lc_buffer *vbo = NULL;
        lc_buffer *ibo = NULL;
        lc_buffer *inst = NULL;
        lc_image *tex = NULL;
        lc_image_view *tex_view = NULL;
        lc_sampler *tex_samp = NULL;
        lc_image *off_c = NULL;
        lc_image *off_d = NULL;
        lc_image_view *off_c_view = NULL;
        lc_image_view *off_d_view = NULL;
        lc_render_target *offscreen = NULL;
        lc_sampler *off_samp = NULL;
        lc_shader *cvs = NULL;
        lc_shader *cfs = NULL;
        lc_shader *qvs = NULL;
        lc_shader *qfs = NULL;
        lc_binding_layout *tex_layout = NULL;
        lc_binding_layout *off_layout = NULL;
        lc_binding_set *tex_set = NULL;
        lc_binding_set *off_set = NULL;
        lc_pipeline *cube_pipe = NULL;
        lc_pipeline *quad_pipe = NULL;
        float mvp[16];
        int env;
        int k;
        int presented = 0;
        int guard = 0;

        (void)k;

        env = make_device(&device);
        TEST_CHECK(env == 0, "device for endurance test");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC RT Test") == 0,
                   "window for endurance test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for endurance test");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for endurance test");

        /* Mesh + 16x16 mipmapped checker texture. */
        {
            lc_buffer_desc bdesc;
            unsigned char cells[16 * 16 * 4];
            lc_image_desc idesc;
            lc_image_upload_desc up;
            lc_image_view_desc vdesc;
            lc_sampler_desc smdesc;

            memset(&bdesc, 0, sizeof(bdesc));
            bdesc.size = sizeof(k_cube);
            bdesc.usage = LC_BUFFER_USAGE_VERTEX;
            bdesc.memory = LC_MEMORY_GPU_ONLY;
            if (lc_buffer_create(device, &bdesc, &vbo) != LC_SUCCESS ||
                lc_buffer_write(vbo, 0, k_cube, sizeof(k_cube)) !=
                    LC_SUCCESS) {
                printf("endurance vbo failed: FAIL\n");
                FAIL_SUMMARY();
            }
            bdesc.size = sizeof(k_idx);
            bdesc.usage = LC_BUFFER_USAGE_INDEX;
            if (lc_buffer_create(device, &bdesc, &ibo) != LC_SUCCESS ||
                lc_buffer_write(ibo, 0, k_idx, sizeof(k_idx)) != LC_SUCCESS) {
                printf("endurance ibo failed: FAIL\n");
                FAIL_SUMMARY();
            }
            for (k = 0; k < 16 * 16; k++) {
                int x = k % 16;
                int y = k / 16;
                int white = (((x / 4) + (y / 4)) % 2);

                cells[k * 4 + 0] = white ? 230 : 40;
                cells[k * 4 + 1] = white ? 230 : 80;
                cells[k * 4 + 2] = white ? 230 : 140;
                cells[k * 4 + 3] = 255;
            }
            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = 16;
            idesc.height = 16;
            idesc.depth = 1;
            idesc.mip_levels = 0;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                          LC_IMAGE_USAGE_TRANSFER_SRC |
                          LC_IMAGE_USAGE_TRANSFER_DST;
            idesc.samples = LC_SAMPLE_COUNT_1;
            if (lc_image_create(device, &idesc, &tex) != LC_SUCCESS) {
                printf("endurance texture failed: FAIL\n");
                FAIL_SUMMARY();
            }
            memset(&up, 0, sizeof(up));
            up.mip_level = 0;
            up.array_layer = 0;
            up.width = 16;
            up.height = 16;
            up.depth = 1;
            up.data = cells;
            up.data_size = sizeof(cells);
            if (lc_image_write(tex, &up) != LC_SUCCESS ||
                lc_image_generate_mipmaps(tex) != LC_SUCCESS) {
                printf("endurance texture upload failed: FAIL\n");
                FAIL_SUMMARY();
            }
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.mip_level_count = lc_image_get_mip_levels(tex);
            vdesc.array_layer_count = 1;
            if (lc_image_view_create(tex, &vdesc, &tex_view) != LC_SUCCESS) {
                printf("endurance texture view failed: FAIL\n");
                FAIL_SUMMARY();
            }
            memset(&smdesc, 0, sizeof(smdesc));
            smdesc.min_filter = LC_FILTER_LINEAR;
            smdesc.mag_filter = LC_FILTER_LINEAR;
            smdesc.mipmap_mode = LC_MIPMAP_MODE_LINEAR;
            smdesc.address_u = LC_ADDRESS_REPEAT;
            smdesc.address_v = LC_ADDRESS_REPEAT;
            smdesc.max_anisotropy = 1.0f;
            if (lc_sampler_create(device, &smdesc, &tex_samp) != LC_SUCCESS) {
                printf("endurance sampler failed: FAIL\n");
                FAIL_SUMMARY();
            }
        }

        /* 256x256 offscreen target (fixed for the whole section). */
        TEST_CHECK(make_color_image(device, LC_FORMAT_RGBA8_UNORM, 256, 256,
                                    1, &off_c, &off_c_view) == 0,
                   "endurance offscreen color");
        TEST_CHECK(make_depth_image(device, LC_FORMAT_D32_FLOAT, 256, 256,
                                    &off_d, &off_d_view) == 0,
                   "endurance offscreen depth");
        {
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment att;
            lc_sampler_desc smdesc;
            unsigned char zeros[16 * 16 * 4];
            lc_image_upload_desc primer;

            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = 256;
            tdesc.height = 256;
            att.view = off_c_view;
            tdesc.color_attachments = &att;
            tdesc.color_attachment_count = 1;
            tdesc.depth_stencil_attachment = off_d_view;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &offscreen) ==
                           LC_SUCCESS,
                       "endurance offscreen target");
            memset(zeros, 0, sizeof(zeros));
            memset(&primer, 0, sizeof(primer));
            primer.width = 16;
            primer.height = 16;
            primer.depth = 1;
            primer.data = zeros;
            primer.data_size = sizeof(zeros);
            /* Primer keeps tex sampled-readable for its set; offscreen
             * color primes the same way (CLEAR discards it later). */
            memset(&smdesc, 0, sizeof(smdesc));
            smdesc.min_filter = LC_FILTER_LINEAR;
            smdesc.mag_filter = LC_FILTER_LINEAR;
            smdesc.mipmap_mode = LC_MIPMAP_MODE_NEAREST;
            smdesc.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
            smdesc.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
            smdesc.max_anisotropy = 1.0f;
            TEST_CHECK(lc_sampler_create(device, &smdesc, &off_samp) ==
                           LC_SUCCESS,
                       "endurance offscreen sampler");
        }
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_VERTEX, "cube.vert.spv",
                               &cvs) == 0,
                   "endurance cube vertex shader");
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_FRAGMENT,
                               "cube.frag.spv", &cfs) == 0,
                   "endurance cube fragment shader");
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_VERTEX, "quad.vert.spv",
                               &qvs) == 0,
                   "endurance quad vertex shader");
        TEST_CHECK(make_shader(device, LC_SHADER_STAGE_FRAGMENT,
                               "quad.frag.spv", &qfs) == 0,
                   "endurance quad fragment shader");
        {
            /* Cube needs an instance offset attribute (location 2):
             * one zero offset (centered cube), uploaded once. */
            lc_buffer_desc bdesc;
            static const float zero[3] = { 0.0f, 0.0f, 0.0f };
            lc_binding_desc slots[2];
            lc_binding_layout_desc ldesc;
            lc_binding_write writes[2];
            lc_vertex_binding_desc vb[2];
            lc_vertex_attribute_desc va[3];
            lc_push_constant_range push;
            lc_graphics_pipeline_desc pd = { 0 };

            memset(&bdesc, 0, sizeof(bdesc));
            bdesc.size = sizeof(zero);
            bdesc.usage = LC_BUFFER_USAGE_VERTEX;
            bdesc.memory = LC_MEMORY_GPU_ONLY;
            if (lc_buffer_create(device, &bdesc, &inst) != LC_SUCCESS ||
                lc_buffer_write(inst, 0, zero, sizeof(zero)) != LC_SUCCESS) {
                printf("endurance instance buffer failed: FAIL\n");
                FAIL_SUMMARY();
            }

            slots[0].binding = 0;
            slots[0].type = LC_BINDING_SAMPLED_IMAGE;
            slots[0].count = 1;
            slots[0].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
            slots[1].binding = 1;
            slots[1].type = LC_BINDING_SAMPLER;
            slots[1].count = 1;
            slots[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
            ldesc.bindings = slots;
            ldesc.binding_count = 2;
            if (lc_binding_layout_create(device, &ldesc, &tex_layout) !=
                    LC_SUCCESS ||
                lc_binding_layout_create(device, &ldesc, &off_layout) !=
                    LC_SUCCESS) {
                printf("endurance layouts failed: FAIL\n");
                FAIL_SUMMARY();
            }
            vb[0].binding = 0;
            vb[0].stride = sizeof(cube_vertex);
            vb[0].input_rate = LC_VERTEX_INPUT_PER_VERTEX;
            vb[1].binding = 1;
            vb[1].stride = sizeof(float) * 3u;
            vb[1].input_rate = LC_VERTEX_INPUT_PER_INSTANCE;
            va[0].location = 0;
            va[0].binding = 0;
            va[0].format = LC_FORMAT_RGB32_FLOAT;
            va[0].offset = 0;
            va[1].location = 1;
            va[1].binding = 0;
            va[1].format = LC_FORMAT_RG32_FLOAT;
            va[1].offset = sizeof(float) * 3u;
            va[2].location = 2;
            va[2].binding = 1;
            va[2].format = LC_FORMAT_RGB32_FLOAT;
            va[2].offset = 0;
            push.visibility = LC_SHADER_VISIBILITY_VERTEX;
            push.offset = 0;
            push.size = 64;
            pd.vertex_shader = cvs;
            pd.fragment_shader = cfs;
            pd.vertex_bindings = vb;
            pd.vertex_binding_count = 2;
            pd.vertex_attributes = va;
            pd.vertex_attribute_count = 3;
            {
                const lc_binding_layout *sl[1];

                sl[0] = tex_layout;
                pd.binding_layouts = sl;
                pd.binding_layout_count = 1;
                pd.cull_mode = LC_CULL_BACK;
                pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
                pd.depth_test_enable = 1;
                pd.depth_write_enable = 1;
                pd.push_constant_ranges = &push;
                pd.push_constant_range_count = 1;
                pd.render_target.color_attachment_count = 1;
                pd.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
                pd.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
                pd.render_target.samples = LC_SAMPLE_COUNT_1;
                if (lc_graphics_pipeline_create(device, &pd,
                                                &cube_pipe) != LC_SUCCESS) {
                    printf("endurance cube pipeline failed: FAIL\n");
                    FAIL_SUMMARY();
                }
            }
            if (lc_binding_set_create(tex_layout, &tex_set) != LC_SUCCESS) {
                printf("endurance tex set failed: FAIL\n");
                FAIL_SUMMARY();
            }
            writes[0].binding = 0;
            writes[0].array_element = 0;
            writes[0].type = LC_BINDING_SAMPLED_IMAGE;
            writes[0].u.image.view = tex_view;
            writes[1].binding = 1;
            writes[1].array_element = 0;
            writes[1].type = LC_BINDING_SAMPLER;
            writes[1].u.sampler.sampler = tex_samp;
            if (lc_binding_set_update(tex_set, writes, 2) != LC_SUCCESS) {
                printf("endurance tex set update failed: FAIL\n");
                FAIL_SUMMARY();
            }
            memset(&pd, 0, sizeof(pd));
            pd.vertex_shader = qvs;
            pd.fragment_shader = qfs;
            {
                const lc_binding_layout *sl[1];

                sl[0] = off_layout;
                pd.binding_layouts = sl;
                pd.binding_layout_count = 1;
                pd.render_target.color_attachment_count = 1;
                pd.render_target.color_formats[0] =
                    lc_swapchain_get_format(swapchain);
                pd.render_target.depth_stencil_format =
                    lc_swapchain_get_depth_format(swapchain);
                pd.render_target.samples = LC_SAMPLE_COUNT_1;
                if (lc_graphics_pipeline_create(device, &pd,
                                                &quad_pipe) != LC_SUCCESS) {
                    printf("endurance quad pipeline failed: FAIL\n");
                    FAIL_SUMMARY();
                }
            }
            if (lc_binding_set_create(off_layout, &off_set) != LC_SUCCESS) {
                printf("endurance off set failed: FAIL\n");
                FAIL_SUMMARY();
            }
            /* Prime offscreen color sampled-readable for its set (the
             * first CLEAR pass discards the primer). */
            {
                lc_image_upload_desc primer;
                void *zeros =
                    calloc(1, (size_t)256 * 256 * 4u);

                if (zeros == NULL) {
                    printf("endurance primer alloc failed: FAIL\n");
                    FAIL_SUMMARY();
                }
                memset(&primer, 0, sizeof(primer));
                primer.width = 256;
                primer.height = 256;
                primer.depth = 1;
                primer.data = zeros;
                primer.data_size = (uint64_t)256 * 256 * 4u;
                if (lc_image_write(off_c, &primer) != LC_SUCCESS) {
                    free(zeros);
                    printf("endurance primer failed: FAIL\n");
                    FAIL_SUMMARY();
                }
                free(zeros);
            }
            writes[0].u.image.view = off_c_view;
            writes[1].u.sampler.sampler = off_samp;
            if (lc_binding_set_update(off_set, writes, 2) != LC_SUCCESS) {
                printf("endurance off set update failed: FAIL\n");
                FAIL_SUMMARY();
            }
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           offscreen, cube_pipe) != 0,
                       "offscreen compatible with cube pipeline");
            TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                           lc_swapchain_get_render_target(swapchain),
                           quad_pipe) != 0,
                       "swapchain target compatible with quad pipeline");
        }

        /* One two-pass frame on `swapchain`: cube -> offscreen (CLEAR),
         * sample -> swapchain (CLEAR). Returns 1 presented, 0 retried
         * (recreated), -1 fatal. */
        {
            int round;
            int ok120 = 1;

            for (round = 0; round < 120; round++) {
                lc_render_color_attachment catt;
                lc_render_depth_attachment datt;
                lc_render_pass_desc pdesc;
                lc_render_swapchain_pass_desc spass;
                lc_result res;
                int settled = 0;

                while (!settled) {
                    if (++guard > 120 * 25 + 600) {
                        ok120 = 0;
                        break;
                    }
                    lc_poll_events();
                    {
                        uint32_t w = lc_window_get_width(window);
                        uint32_t h = lc_window_get_height(window);

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
                                ok120 = 0;
                                break;
                            }
                        }
                    }
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
                        ok120 = 0;
                        break;
                    }
                    memset(&catt, 0, sizeof(catt));
                    catt.view = off_c_view;
                    catt.load_op = LC_LOAD_OP_CLEAR;
                    catt.store_op = LC_STORE_OP_STORE;
                    catt.clear_color[0] = 0.04f;
                    catt.clear_color[1] = 0.05f;
                    catt.clear_color[2] = 0.09f;
                    catt.clear_color[3] = 1.0f;
                    memset(&datt, 0, sizeof(datt));
                    datt.view = off_d_view;
                    datt.depth_load_op = LC_LOAD_OP_CLEAR;
                    datt.depth_store_op = LC_STORE_OP_STORE;
                    datt.clear_depth = 1.0f;
                    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
                    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
                    memset(&pdesc, 0, sizeof(pdesc));
                    pdesc.color_attachments = &catt;
                    pdesc.color_attachment_count = 1;
                    pdesc.depth_attachment = &datt;
                    pdesc.width = 256;
                    pdesc.height = 256;
                    if (lc_encoder_begin_render_pass(enc, &pdesc) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_pipeline(enc, cube_pipe) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_binding_set(enc, cube_pipe, 0,
                                                    tex_set) != LC_SUCCESS ||
                        lc_encoder_bind_vertex_buffer(enc, 0, vbo, 0) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_vertex_buffer(enc, 1, inst, 0) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_index_buffer(enc, ibo, 0,
                                                     LC_INDEX_UINT16) !=
                            LC_SUCCESS ||
                        lc_encoder_push_constants(
                            enc, cube_pipe, LC_SHADER_VISIBILITY_VERTEX, 0,
                            sizeof(mvp), (rt_cube_mvp(mvp, 0.7f), mvp)) != LC_SUCCESS ||
                        lc_encoder_draw_indexed(enc, 36, 1, 0, 0, 0) !=
                            LC_SUCCESS ||
                        lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                        ok120 = 0;
                        break;
                    }
                    memset(&spass, 0, sizeof(spass));
                    spass.color_load_op = LC_LOAD_OP_CLEAR;
                    spass.color_store_op = LC_STORE_OP_STORE;
                    spass.depth_load_op = LC_LOAD_OP_CLEAR;
                    spass.depth_store_op = LC_STORE_OP_DONT_CARE;
                    spass.clear_depth = 1.0f;
                    if (lc_encoder_begin_swapchain_pass(enc, swapchain,
                                                        &spass) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_pipeline(enc, quad_pipe) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_binding_set(enc, quad_pipe, 0,
                                                    off_set) != LC_SUCCESS ||
                        lc_encoder_draw(enc, 3, 0) != LC_SUCCESS ||
                        lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                        ok120 = 0;
                        break;
                    }
                    res = lc_end_frame(swapchain);
                    if (res == LC_SUCCESS || res == LC_SUBOPTIMAL) {
                        presented++;
                        settled = 1;
                    } else if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                        uint32_t w = lc_window_get_width(window);
                        uint32_t h = lc_window_get_height(window);

                        if (w != 0 && h != 0) {
                            lc_swapchain_recreate(swapchain, w, h);
                        }
                    } else {
                        ok120 = 0;
                        break;
                    }
                }
                if (!ok120) {
                    break;
                }
            }
            TEST_CHECK(ok120 && presented == 120,
                       "120 two-pass cube frames presented");
        }

        /* Screenshot hook (test-only): dump the offscreen cube scene
         * to PPM when LC_SAVE_PPM is set. White-box readback of the
         * last presented frame's scene texture. */
        {
            const char *ppm_path = getenv("LC_SAVE_PPM");

            if (ppm_path != NULL && ppm_path[0] != '\0') {
                unsigned char *px = readback_rgba8(device, off_c, 256, 256);

                if (px != NULL) {
                    FILE *f = fopen(ppm_path, "wb");

                    if (f != NULL) {
                        fprintf(f, "P6\n256 256\n255\n");
                        {
                            /* PPM is top-row-first; Vulkan images are
                             * bottom-row-first: flip vertically. */
                            int y;

                            for (y = 255; y >= 0; y--) {
                                int x;

                                for (x = 0; x < 256; x++) {
                                    unsigned char *p =
                                        &px[((uint32_t)y * 256u +
                                             (uint32_t)x) *
                                            4u];

                                    fputc(p[0], f);
                                    fputc(p[1], f);
                                    fputc(p[2], f);
                                }
                            }
                        }
                        fclose(f);
                        printf("[INFO] screenshot -> %s\n", ppm_path);
                    }
                    free(px);
                }
            }
        }

        /* Resize independence: fixed offscreen target survives. */
        {
            static const unsigned targets[][2] = {
                { 1024, 768 }, { 640, 480 }, { 800, 600 },
            };
            size_t step;
            int resize_ok = 1;

            for (step = 0;
                 resize_ok &&
                 step < sizeof(targets) / sizeof(targets[0]);
                 step++) {
                unsigned w = targets[step][0];
                unsigned h = targets[step][1];
                int n = 0;
                int inner_guard = 0;

                if (lc_swapchain_recreate(swapchain, w, h) != LC_SUCCESS) {
                    resize_ok = 0;
                    break;
                }
                TEST_CHECK(lc_render_target_get_width(offscreen) == 256 &&
                               lc_render_target_get_height(offscreen) == 256,
                           "offscreen extent untouched by swapchain resize");
                TEST_CHECK(lc_render_target_is_compatible_with_pipeline(
                               lc_swapchain_get_render_target(swapchain),
                               quad_pipe) != 0,
                           "quad pipeline still matches after resize");
                while (n < 4) {
                    lc_render_color_attachment catt;
                    lc_render_depth_attachment datt;
                    lc_render_pass_desc pdesc;
                    lc_render_swapchain_pass_desc spass;
                    lc_result res;

                    if (++inner_guard > 200) {
                        resize_ok = 0;
                        break;
                    }
                    lc_poll_events();
                    res = lc_begin_frame(swapchain);
                    if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                        if (lc_swapchain_recreate(swapchain, w, h) !=
                            LC_SUCCESS) {
                            resize_ok = 0;
                            break;
                        }
                        continue;
                    }
                    if (res != LC_SUCCESS ||
                        lc_swapchain_get_encoder(swapchain, &enc) !=
                            LC_SUCCESS) {
                        resize_ok = 0;
                        break;
                    }
                    memset(&catt, 0, sizeof(catt));
                    catt.view = off_c_view;
                    catt.load_op = LC_LOAD_OP_CLEAR;
                    catt.store_op = LC_STORE_OP_STORE;
                    memset(&datt, 0, sizeof(datt));
                    datt.view = off_d_view;
                    datt.depth_load_op = LC_LOAD_OP_CLEAR;
                    datt.depth_store_op = LC_STORE_OP_STORE;
                    datt.clear_depth = 1.0f;
                    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
                    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
                    memset(&pdesc, 0, sizeof(pdesc));
                    pdesc.color_attachments = &catt;
                    pdesc.color_attachment_count = 1;
                    pdesc.depth_attachment = &datt;
                    pdesc.width = 256;
                    pdesc.height = 256;
                    if (lc_encoder_begin_render_pass(enc, &pdesc) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_pipeline(enc, cube_pipe) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_binding_set(enc, cube_pipe, 0,
                                                    tex_set) != LC_SUCCESS ||
                        lc_encoder_bind_vertex_buffer(enc, 0, vbo, 0) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_vertex_buffer(enc, 1, inst, 0) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_index_buffer(enc, ibo, 0,
                                                     LC_INDEX_UINT16) !=
                            LC_SUCCESS ||
                        lc_encoder_push_constants(
                            enc, cube_pipe, LC_SHADER_VISIBILITY_VERTEX, 0,
                            sizeof(mvp), (rt_cube_mvp(mvp, 0.7f), mvp)) != LC_SUCCESS ||
                        lc_encoder_draw_indexed(enc, 36, 1, 0, 0, 0) !=
                            LC_SUCCESS ||
                        lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                        resize_ok = 0;
                        break;
                    }
                    memset(&spass, 0, sizeof(spass));
                    spass.color_load_op = LC_LOAD_OP_CLEAR;
                    spass.color_store_op = LC_STORE_OP_STORE;
                    spass.depth_load_op = LC_LOAD_OP_DONT_CARE;
                    spass.depth_store_op = LC_STORE_OP_DONT_CARE;
                    if (lc_encoder_begin_swapchain_pass(enc, swapchain,
                                                        &spass) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_pipeline(enc, quad_pipe) !=
                            LC_SUCCESS ||
                        lc_encoder_bind_binding_set(enc, quad_pipe, 0,
                                                    off_set) != LC_SUCCESS ||
                        lc_encoder_draw(enc, 3, 0) != LC_SUCCESS ||
                        lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                        resize_ok = 0;
                        break;
                    }
                    res = lc_end_frame(swapchain);
                    if (res == LC_SUCCESS || res == LC_SUBOPTIMAL) {
                        n++;
                    } else if (res != LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                        resize_ok = 0;
                        break;
                    }
                }
                {
                    char msg[96];

                    snprintf(msg, sizeof(msg),
                             "4 two-pass frames at %ux%u", w, h);
                    TEST_CHECK(resize_ok && n == 4, msg);
                }
            }
            TEST_CHECK(resize_ok, "resize stress kept offscreen intact");
        }

        /* Multi-window sharing: second swapchain samples the same
         * offscreen target through its own quad pipeline. */
        {
            lc_window *window_b = NULL;
            lc_surface *surface_b = NULL;
            lc_swapchain *swapchain_b = NULL;
            lc_pipeline *quad_b = NULL;
            int i;
            int ok = 1;

            TEST_CHECK(make_window_titled(&window_b, "LumaC RT Test B") == 0,
                       "window B created");
            if (window_b != NULL) {
                if (make_surface(device, window_b, &surface_b) != 0 ||
                    make_swapchain(device, surface_b, &swapchain_b) != 0) {
                    ok = 0;
                    TEST_CHECK(0, "surface/swapchain B created");
                } else {
                    lc_graphics_pipeline_desc pd = { 0 };
                    const lc_binding_layout *sl[1];

                    sl[0] = off_layout;
                    pd.vertex_shader = qvs;
                    pd.fragment_shader = qfs;
                    pd.binding_layouts = sl;
                    pd.binding_layout_count = 1;
                    pd.render_target.color_attachment_count = 1;
                    pd.render_target.color_formats[0] =
                        lc_swapchain_get_format(swapchain_b);
                    pd.render_target.depth_stencil_format =
                        lc_swapchain_get_depth_format(swapchain_b);
                    pd.render_target.samples = LC_SAMPLE_COUNT_1;
                    TEST_CHECK(lc_graphics_pipeline_create(
                                   device, &pd, &quad_b) ==
                                   LC_SUCCESS,
                               "quad pipeline for window B");
                }
            }
            for (i = 0; ok && i < 6; i++) {
                lc_swapchain *sc = (i % 2 == 0) ? swapchain : swapchain_b;
                lc_pipeline *qp = (i % 2 == 0) ? quad_pipe : quad_b;
                lc_window *win = (i % 2 == 0) ? window : window_b;
                lc_render_color_attachment catt;
                lc_render_depth_attachment datt;
                lc_render_pass_desc pdesc;
                lc_render_swapchain_pass_desc spass;
                lc_result res;

                (void)win;
                lc_poll_events();
                res = lc_begin_frame(sc);
                if (res != LC_SUCCESS ||
                    lc_swapchain_get_encoder(sc, &enc) != LC_SUCCESS) {
                    ok = 0;
                    break;
                }
                memset(&catt, 0, sizeof(catt));
                catt.view = off_c_view;
                catt.load_op = LC_LOAD_OP_CLEAR;
                catt.store_op = LC_STORE_OP_STORE;
                memset(&datt, 0, sizeof(datt));
                datt.view = off_d_view;
                datt.depth_load_op = LC_LOAD_OP_CLEAR;
                datt.depth_store_op = LC_STORE_OP_STORE;
                datt.clear_depth = 1.0f;
                datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
                datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
                memset(&pdesc, 0, sizeof(pdesc));
                pdesc.color_attachments = &catt;
                pdesc.color_attachment_count = 1;
                pdesc.depth_attachment = &datt;
                pdesc.width = 256;
                pdesc.height = 256;
                if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS ||
                    lc_encoder_bind_pipeline(enc, cube_pipe) != LC_SUCCESS ||
                    lc_encoder_bind_binding_set(enc, cube_pipe, 0, tex_set) !=
                        LC_SUCCESS ||
                    lc_encoder_bind_vertex_buffer(enc, 0, vbo, 0) !=
                        LC_SUCCESS ||
                    lc_encoder_bind_vertex_buffer(enc, 1, inst, 0) !=
                        LC_SUCCESS ||
                    lc_encoder_bind_index_buffer(enc, ibo, 0,
                                                 LC_INDEX_UINT16) !=
                        LC_SUCCESS ||
                    lc_encoder_push_constants(enc, cube_pipe,
                                              LC_SHADER_VISIBILITY_VERTEX, 0,
                                              sizeof(mvp),
                                              (rt_cube_mvp(mvp, 0.7f),
                                               mvp)) != LC_SUCCESS ||
                    lc_encoder_draw_indexed(enc, 36, 1, 0, 0, 0) !=
                        LC_SUCCESS ||
                    lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                    ok = 0;
                    break;
                }
                memset(&spass, 0, sizeof(spass));
                spass.color_load_op = LC_LOAD_OP_CLEAR;
                spass.color_store_op = LC_STORE_OP_STORE;
                spass.depth_load_op = LC_LOAD_OP_DONT_CARE;
                spass.depth_store_op = LC_STORE_OP_DONT_CARE;
                if (lc_encoder_begin_swapchain_pass(enc, sc, &spass) !=
                        LC_SUCCESS ||
                    lc_encoder_bind_pipeline(enc, qp) != LC_SUCCESS ||
                    lc_encoder_bind_binding_set(enc, qp, 0, off_set) !=
                        LC_SUCCESS ||
                    lc_encoder_draw(enc, 3, 0) != LC_SUCCESS ||
                    lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                    ok = 0;
                    break;
                }
                res = lc_end_frame(sc);
                if (res != LC_SUCCESS && res != LC_SUBOPTIMAL) {
                    ok = 0;
                    break;
                }
            }
            TEST_CHECK(ok, "6 alternating shared-target frames");
            lc_pipeline_destroy(quad_b);
            if (swapchain_b != NULL) {
                lc_swapchain_destroy(swapchain_b);
            }
            if (surface_b != NULL) {
                lc_surface_destroy(surface_b);
            }
            if (window_b != NULL) {
                lc_window_destroy(window_b);
            }
        }

        lc_pipeline_destroy(quad_pipe);
        lc_pipeline_destroy(cube_pipe);
        lc_binding_set_destroy(off_set);
        lc_binding_set_destroy(tex_set);
        lc_binding_layout_destroy(off_layout);
        lc_binding_layout_destroy(tex_layout);
        lc_shader_destroy(qfs);
        lc_shader_destroy(qvs);
        lc_shader_destroy(cfs);
        lc_shader_destroy(cvs);
        lc_sampler_destroy(off_samp);
        lc_sampler_destroy(tex_samp);
        lc_render_target_destroy(offscreen);
        lc_image_view_destroy(off_d_view);
        lc_image_view_destroy(off_c_view);
        lc_image_destroy(off_d);
        lc_image_destroy(off_c);
        lc_image_view_destroy(tex_view);
        lc_image_destroy(tex);
        lc_buffer_destroy(inst);
        lc_buffer_destroy(ibo);
        lc_buffer_destroy(vbo);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "endurance teardown clean");
    }

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
