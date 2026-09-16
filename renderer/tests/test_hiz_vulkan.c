/* Phase 23 Hi-Z pyramid test.
 *
 * Proves the dedicated R32F depth pyramid against an independent
 * CPU MAX-reduction reference on exact fixtures: 8x8 diagnostic
 * (near/far blocks + gradients), 7x5 odd, 1x8, 8x1, 1x1. Every
 * generated mip is compared texel-for-texel via public readback
 * (D32->R32F is bit-exact; MAX over identical floats is exact, so
 * the tolerance is zero by construction). Mip-count rules and
 * per-mip extent math are pinned directly. Validation layers stay
 * enabled throughout.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>

#include "graphics/graphics_internal.h"
#include "internal/renderer_internal.h"

static int g_passed;
static int g_failed;

#define CHECK(c, m) do {                                                \
    if (c) { printf("[PASS] %s\n", m); g_passed++; }                    \
    else { printf("[FAIL] %s\n", m); g_failed++; }                     \
} while (0)

#define SKIP_ENV(what) do {                                             \
    printf("SKIP: environment cannot provide %s\n", what);              \
    lc_shutdown();                                                      \
    return 0;                                                           \
} while (0)

typedef struct hiz_env {
    lc_device *device;
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
    lr_renderer *renderer;
} hiz_env;

static void fill_diagnostic(float *px, uint32_t w, uint32_t h) {
    /* Quadrants: near block, far block, horizontal gradient,
     * vertical gradient; odd edge texels get distinct values so a
     * dropped last row/column fails loudly. */
    uint32_t x;
    uint32_t y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            float v;

            if (x < w / 2 && y < h / 2) {
                v = 0.1f;
            } else if (x >= w / 2 && y < h / 2) {
                v = 0.9f;
            } else if (y >= h / 2 && x < w / 2) {
                v = (w > 1) ? (float)x / (float)(w - 1) : 0.5f;
            } else {
                v = (h > 1) ? (float)y / (float)(h - 1) : 0.5f;
            }
            if (x == w - 1 || y == h - 1) {
                v = 0.05f + 0.9f * (float)(x + y) /
                                  (float)(w + h);
            }
            px[(size_t)y * w + x] = v;
        }
    }
}

static void fill_blocks(float *px, uint32_t w, uint32_t h) {
    uint32_t x;
    uint32_t y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            px[(size_t)y * w + x] =
                (x < (w + 1u) / 2u) ? 0.2f : 0.8f;
        }
    }
}

/* One fixture: upload pattern as D32 depth, generate the pyramid,
 * compare every mip against the CPU reference exactly. */
static int run_fixture(hiz_env *env, const char *name, uint32_t w,
                       uint32_t h,
                       void (*fill)(float *, uint32_t, uint32_t)) {
    lc_command_encoder *enc = NULL;
    lr_camera camera;
    lc_image_desc idesc;
    lc_image *depth = NULL;
    lc_image_upload_desc upload;
    lc_image_subresource_range range;
    lc_image *saved_image;
    lc_image_view *saved_view;
    float *pattern = NULL;
    uint32_t mips;
    uint32_t m;
    int ok = 1;

    pattern = (float *)malloc((size_t)w * h * sizeof(float));
    if (pattern == NULL) {
        return 0;
    }
    fill(pattern, w, h);
    if (lc_begin_frame(env->swapchain) != LC_SUCCESS ||
        lc_swapchain_get_encoder(env->swapchain, &enc) !=
            LC_SUCCESS) {
        free(pattern);
        return 0;
    }
    lr_camera_init(&camera);
    lr_camera_set_perspective(&camera, 1.0471976f, 1.0f, 0.1f,
                              100.0f);
    {
        static const float eye[3] = { 0.0f, 0.0f, 5.0f };
        static const float center[3] = { 0.0f, 0.0f, 0.0f };
        static const float up[3] = { 0.0f, 1.0f, 0.0f };

        lr_camera_look_at(&camera, eye, center, up);
    }
    if (lr_renderer_begin(env->renderer, &camera) != LR_SUCCESS) {
        free(pattern);
        return 0;
    }
    if (lr_renderer_ensure_hdr(env->renderer, w, h) != LR_SUCCESS ||
        lr_vis_ensure_shared(env->renderer) != LR_SUCCESS ||
        lr_hiz_ensure(env->renderer, w, h) != LR_SUCCESS) {
        free(pattern);
        return 0;
    }
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_D32_FLOAT;
    idesc.width = w;
    idesc.height = h;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = (uint32_t)LC_IMAGE_USAGE_SAMPLED |
                  (uint32_t)LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(env->device, &idesc, &depth) != LC_SUCCESS) {
        free(pattern);
        return 0;
    }
    memset(&upload, 0, sizeof(upload));
    upload.mip_level = 0;
    upload.array_layer = 0;
    upload.width = w;
    upload.height = h;
    upload.depth = 1;
    upload.data = pattern;
    upload.data_size = (uint64_t)w * h * sizeof(float);
    if (lc_image_write(depth, &upload) != LC_SUCCESS) {
        lc_image_destroy(depth);
        free(pattern);
        return 0;
    }
    memset(&range, 0, sizeof(range));
    range.base_mip_level = 0;
    range.level_count = 1;
    range.base_array_layer = 0;
    range.layer_count = 1;
    if (lc_encoder_transition_image(enc, depth, &range,
                                    LC_RESOURCE_STATE_SHADER_READ) !=
        LC_SUCCESS) {
        lc_image_destroy(depth);
        free(pattern);
        return 0;
    }
    /* Borrow the fixture as the scene depth for one generation. */
    saved_image = env->renderer->hdr_depth_image;
    saved_view = env->renderer->hdr_depth_view;
    {
        lc_image_view_desc vdesc;

        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.format = LC_FORMAT_UNDEFINED;
        vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
        vdesc.base_mip_level = 0;
        vdesc.mip_level_count = 1;
        vdesc.base_array_layer = 0;
        vdesc.array_layer_count = 1;
        env->renderer->hdr_depth_image = depth;
        env->renderer->hdr_depth_view = NULL;
        if (lc_image_view_create(depth, &vdesc,
                                 &env->renderer->hdr_depth_view) !=
            LC_SUCCESS) {
            env->renderer->hdr_depth_image = saved_image;
            env->renderer->hdr_depth_view = saved_view;
            lc_image_destroy(depth);
            free(pattern);
            return 0;
        }
    }
    if (lr_hiz_generate(env->renderer, enc) != LR_SUCCESS) {
        ok = 0;
    }
    /* Trivial swapchain pass so end_frame presents a defined
     * image (validation stays quiet). */
    {
        lc_render_swapchain_pass_desc swap_pass;

        memset(&swap_pass, 0, sizeof(swap_pass));
        swap_pass.color_load_op = LC_LOAD_OP_CLEAR;
        swap_pass.color_store_op = LC_STORE_OP_STORE;
        swap_pass.clear_color[3] = 1.0f;
        swap_pass.depth_load_op = LC_LOAD_OP_CLEAR;
        swap_pass.depth_store_op = LC_STORE_OP_DONT_CARE;
        swap_pass.clear_depth = 1.0f;
        if (lc_encoder_begin_swapchain_pass(enc, env->swapchain,
                                            &swap_pass) !=
                LC_SUCCESS ||
            lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            ok = 0;
        }
    }
    {
        lc_result end_res = lc_end_frame(env->swapchain);

        if (end_res != LC_SUCCESS && end_res != LC_SUBOPTIMAL) {
            ok = 0;
        }
    }
    mips = lr_hiz_levels(env->renderer);
    if (ok) {
        char msg[128];

        snprintf(msg, sizeof(msg), "%s: %u mips for %ux%u", name,
                 mips, w, h);
        CHECK(mips == lr_hiz_mip_count(w, h), msg);
    }
    for (m = 0; m < mips && ok; m++) {
        uint32_t mw = w >> m;
        uint32_t mh = h >> m;
        float *gpu = NULL;
        float *cpu = NULL;
        uint32_t t;

        if (mw == 0) {
            mw = 1;
        }
        if (mh == 0) {
            mh = 1;
        }
        gpu = (float *)malloc((size_t)mw * mh * sizeof(float));
        cpu = (float *)malloc((size_t)mw * mh * sizeof(float));
        if (gpu == NULL || cpu == NULL) {
            free(gpu);
            free(cpu);
            ok = 0;
            break;
        }
        if (lr_hiz_read_mip(env->renderer, m, gpu) != LR_SUCCESS) {
            free(gpu);
            free(cpu);
            ok = 0;
            break;
        }
        lr_hiz_cpu_pyramid(pattern, w, h, m, cpu);
        for (t = 0; t < mw * mh; t++) {
            if (gpu[t] != cpu[t]) {
                ok = 0;
                break;
            }
        }
        {
            char msg[128];

            snprintf(msg, sizeof(msg), "%s: mip %u (%ux%u) exact",
                     name, m, mw, mh);
            CHECK(ok, msg);
        }
        free(gpu);
        free(cpu);
    }
    lc_image_view_destroy(env->renderer->hdr_depth_view);
    env->renderer->hdr_depth_image = saved_image;
    env->renderer->hdr_depth_view = saved_view;
    lc_image_destroy(depth);
    free(pattern);
    return ok;
}

int main(void) {
    hiz_env env;
    lc_device_desc ddesc;
    lc_window_desc wdesc;
    lc_swapchain_desc sdesc;
    lr_renderer_desc rdesc;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running LumaC Hi-Z pyramid (Phase 23) test...\n");
    memset(&env, 0, sizeof(env));
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.backend = LC_BACKEND_VULKAN;
    ddesc.enable_validation = 1;
    if (lc_device_create(&ddesc, &env.device) != LC_SUCCESS) {
        SKIP_ENV("Vulkan device");
    }
    memset(&wdesc, 0, sizeof(wdesc));
    wdesc.title = "LumaC Hi-Z";
    wdesc.width = 320;
    wdesc.height = 240;
    if (lc_window_create(&wdesc, &env.window) != LC_SUCCESS ||
        lc_surface_create(env.device, env.window, &env.surface) !=
            LC_SUCCESS) {
        SKIP_ENV("windowed Vulkan");
    }
    memset(&sdesc, 0, sizeof(sdesc));
    sdesc.width = 320;
    sdesc.height = 240;
    sdesc.vsync = 1;
    if (lc_swapchain_create(env.device, env.surface, &sdesc,
                            &env.swapchain) != LC_SUCCESS) {
        SKIP_ENV("windowed Vulkan");
    }
    memset(&rdesc, 0, sizeof(rdesc));
    rdesc.device = env.device;
    rdesc.render_target.color_attachment_count = 1;
    rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
    rdesc.max_objects = 64;
    if (lr_renderer_create(&rdesc, &env.renderer) != LR_SUCCESS) {
        printf("renderer create failed: FAIL\n");
        return 1;
    }
    /* Mip-count rules (floor(log2(max)) + 1). */
    CHECK(lr_hiz_mip_count(8, 8) == 4, "8x8 -> 4 mips");
    CHECK(lr_hiz_mip_count(7, 5) == 3, "7x5 -> 3 mips");
    CHECK(lr_hiz_mip_count(1, 8) == 4, "1x8 -> 4 mips");
    CHECK(lr_hiz_mip_count(8, 1) == 4, "8x1 -> 4 mips");
    CHECK(lr_hiz_mip_count(1, 1) == 1, "1x1 -> 1 mip");
    CHECK(lr_hiz_mip_count(800, 600) == 10, "800x600 -> 10 mips");
    CHECK(lr_hiz_mip_count(321, 179) == 9, "321x179 -> 9 mips");
    CHECK(lr_hiz_mip_count(1920, 1080) == 11, "1920x1080 -> 11 mips");

    CHECK(run_fixture(&env, "8x8 diagnostic", 8, 8,
                      fill_diagnostic),
          "8x8 diagnostic pyramid");
    CHECK(run_fixture(&env, "7x5 odd", 7, 5, fill_blocks),
          "7x5 odd pyramid");
    CHECK(run_fixture(&env, "1x8 column", 1, 8, fill_diagnostic),
          "1x8 column pyramid");
    CHECK(run_fixture(&env, "8x1 row", 8, 1, fill_diagnostic),
          "8x1 row pyramid");
    CHECK(run_fixture(&env, "1x1 single", 1, 1, fill_blocks),
          "1x1 single pyramid");
    /* Resize sequence: only extent-dependent resources rebuild
     * (each fixture re-sizes the pyramid and regenerates). */
    CHECK(run_fixture(&env, "800x600 view", 800, 600, fill_blocks),
          "800x600 pyramid");
    CHECK(run_fixture(&env, "321x179 resize", 321, 179,
                      fill_diagnostic),
          "321x179 pyramid");
    CHECK(run_fixture(&env, "1920x1080 resize", 1920, 1080,
                      fill_blocks),
          "1920x1080 pyramid");
    CHECK(run_fixture(&env, "800x600 restore", 800, 600,
                      fill_diagnostic),
          "800x600 restored pyramid");

    lr_renderer_destroy(env.renderer);
    lc_swapchain_destroy(env.swapchain);
    lc_surface_destroy(env.surface);
    lc_device_destroy(env.device);
    lc_window_destroy(env.window);
    lc_shutdown();
    printf("hiz: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
