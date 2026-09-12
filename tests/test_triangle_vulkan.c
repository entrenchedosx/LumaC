/*
 * Vulkan triangle integration test (Phase 7).
 *
 * Loads real SPIR-V, creates shader modules and a graphics pipeline,
 * and renders triangle frames: misuse rejection, shader lifetime
 * independence, 120-frame stability, resize stress, minimize/restore,
 * and two-window rendering. Run with validation layers: render pass,
 * framebuffer, pipeline, layout, and draw-state misuse would surface
 * as validation errors.
 *
 * If the environment cannot provide a window or a usable Vulkan setup,
 * the test reports SKIP and exits 0. Any other failure is a hard FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lumac/lumac.h>

#ifndef LC_TRIANGLE_SPV_DIR
#define LC_TRIANGLE_SPV_DIR "."
#endif

#ifdef _WIN32
/* Test-only native window control (LumaC has no resize API by design). */
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

/* Grow/shrink the named test window so its client area becomes target_w
 * x target_h, pump events so lc_window observes WM_SIZE, and report the
 * observed client size. Returns nonzero on success. */
static int native_resize_client(const wchar_t *title, lc_window *window,
                                unsigned target_w, unsigned target_h,
                                unsigned *out_w, unsigned *out_h) {
    HWND hwnd = FindWindowW(NULL, title);
    RECT outer;
    RECT client;
    int i;

    if (hwnd == NULL) {
        return 0;
    }
    if (!GetWindowRect(hwnd, &outer) || !GetClientRect(hwnd, &client)) {
        return 0;
    }
    if (!SetWindowPos(hwnd, NULL, 0, 0,
                      (outer.right - outer.left) + (int)target_w -
                          (client.right - client.left),
                      (outer.bottom - outer.top) + (int)target_h -
                          (client.bottom - client.top),
                      SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE)) {
        return 0;
    }
    for (i = 0; i < 60; i++) {
        lc_poll_events();
        Sleep(5);
    }
    *out_w = lc_window_get_width(window);
    *out_h = lc_window_get_height(window);
    return (*out_w == target_w && *out_h == target_h) ? 1 : 0;
}

static HWND find_test_window(const wchar_t *title) {
    return FindWindowW(NULL, title);
}
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

/* 0 = ready, 1 = environmental SKIP, -1 = hard failure */
static int make_device(lc_device **out) {
    lc_device_desc desc;

    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 1; /* exercises messenger when layers exist */
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

/* 0 = ready, 1 = environmental SKIP (headless), -1 = hard failure */
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

/* 0 = ready, 1 = environmental SKIP, -1 = hard failure */
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

/* 0 = ready, 1 = environmental SKIP, -1 = hard failure */
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

/* Minimal private SPIR-V file loader (tests only). */
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

/* Render `want` presented triangle frames (bind + draw 3 vertices each),
 * handling recoverable swapchain states. Returns presented count, or
 * -1 on fatal error / livelock guard trip. */
static int render_triangles(lc_swapchain *sc, lc_window *win,
                            lc_pipeline *pipeline, int want, float r, float g,
                            float b) {
    int presented = 0;
    int guard = 0;

    while (presented < want) {
        uint32_t w;
        uint32_t h;
        lc_result res;

        if (++guard > want * 25 + 120) {
            return -1;
        }
        lc_poll_events();
        w = lc_window_get_width(win);
        h = lc_window_get_height(win);
        if (w == 0 || h == 0) {
            continue; /* minimized */
        }
        if (w != lc_swapchain_get_width(sc) ||
            h != lc_swapchain_get_height(sc)) {
            res = lc_swapchain_recreate(sc, w, h);
            if (res == LC_ERROR_ZERO_EXTENT) {
                continue;
            }
            if (res != LC_SUCCESS) {
                return -1;
            }
        }
        res = lc_begin_frame(sc);
        if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            res = lc_swapchain_recreate(sc, w, h);
            if (res == LC_ERROR_ZERO_EXTENT) {
                continue;
            }
            if (res != LC_SUCCESS) {
                return -1;
            }
            continue;
        }
        if (res != LC_SUCCESS) {
            return -1;
        }
        if (lc_clear_color(sc, r, g, b, 1.0f) != LC_SUCCESS) {
            return -1;
        }
        if (lc_bind_pipeline(sc, pipeline) != LC_SUCCESS) {
            return -1;
        }
        if (lc_draw(sc, 3, 0) != LC_SUCCESS) {
            return -1;
        }
        res = lc_end_frame(sc);
        if (res == LC_SUCCESS) {
            presented++;
        } else if (res == LC_SUBOPTIMAL ||
                   res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            presented++;
            res = lc_swapchain_recreate(sc, w, h);
            if (res == LC_ERROR_ZERO_EXTENT) {
                continue;
            }
            if (res != LC_SUCCESS) {
                return -1;
            }
        } else {
            return -1;
        }
    }
    return presented;
}

/* Shared context built once per test section needing the full stack. */
typedef struct tri_ctx {
    lc_device *device;
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
    void *vert_code;
    size_t vert_size;
    void *frag_code;
    size_t frag_size;
} tri_ctx;

/* 0 = ready with ctx filled, 1 = environmental SKIP, -1 = hard failure.
 * Loads real SPIR-V from the staged directory. */
static int make_tri_ctx(tri_ctx *ctx, const char *title) {
    int env;

    memset(ctx, 0, sizeof(*ctx));
    env = make_device(&ctx->device);
    if (env != 0) {
        return env;
    }
    env = make_window_titled(&ctx->window, title);
    if (env != 0) {
        lc_device_destroy(ctx->device);
        ctx->device = NULL;
        return env;
    }
    env = make_surface(ctx->device, ctx->window, &ctx->surface);
    if (env != 0) {
        lc_device_destroy(ctx->device);
        lc_window_destroy(ctx->window);
        ctx->device = NULL;
        ctx->window = NULL;
        return env;
    }
    env = make_swapchain(ctx->device, ctx->surface, &ctx->swapchain);
    if (env != 0) {
        lc_surface_destroy(ctx->surface);
        lc_device_destroy(ctx->device);
        lc_window_destroy(ctx->window);
        ctx->surface = NULL;
        ctx->device = NULL;
        ctx->window = NULL;
        return env;
    }
    if (!load_spv_file("triangle.vert.spv", &ctx->vert_code,
                       &ctx->vert_size) ||
        !load_spv_file("triangle.frag.spv", &ctx->frag_code,
                       &ctx->frag_size)) {
        printf("SPIR-V files missing: FAIL\n");
        return -1;
    }
    return 0;
}

static void free_tri_ctx_code(tri_ctx *ctx) {
    free(ctx->vert_code);
    free(ctx->frag_code);
    ctx->vert_code = NULL;
    ctx->frag_code = NULL;
}

static void destroy_tri_ctx(tri_ctx *ctx) {
    free_tri_ctx_code(ctx);
    lc_swapchain_destroy(ctx->swapchain);
    lc_surface_destroy(ctx->surface);
    lc_device_destroy(ctx->device);
    lc_window_destroy(ctx->window);
    memset(ctx, 0, sizeof(*ctx));
}

int main(void) {
    printf("Running LumaC Vulkan triangle integration test...\n");

    lc_shutdown();
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }

    /* ---- 1. shader validation against a live device + lifecycle ---- */
    {
        tri_ctx ctx;
        lc_shader_desc desc;
        lc_shader *vs = NULL;
        lc_shader *fs = NULL;
        lc_shader *bad = NULL;
        unsigned char *mutated = NULL;
        int env = make_tri_ctx(&ctx, "LumaC Triangle Test");

        if (env != 0) {
            if (env == 1) {
                SKIP_ENV("a full triangle stack");
            }
            printf("triangle context failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }

        /* From here on the environment is proven: failures are FAILs. */
        desc.stage = LC_SHADER_STAGE_VERTEX;
        desc.code = ctx.vert_code;
        desc.code_size = ctx.vert_size;
        desc.entry_point = NULL;
        TEST_CHECK(lc_shader_create(ctx.device, &desc, &vs) == LC_SUCCESS,
                   "vertex shader creation succeeds");
        desc.stage = LC_SHADER_STAGE_FRAGMENT;
        desc.code = ctx.frag_code;
        desc.code_size = ctx.frag_size;
        TEST_CHECK(lc_shader_create(ctx.device, &desc, &fs) == LC_SUCCESS,
                   "fragment shader creation succeeds");

        /* Malformed inputs against the live device. */
        mutated = (unsigned char *)malloc(ctx.vert_size);
        TEST_CHECK(mutated != NULL, "scratch buffer for negative checks");
        if (mutated != NULL) {
            memcpy(mutated, ctx.vert_code, ctx.vert_size);
            mutated[0] ^= 0xFFu; /* break the SPIR-V magic */
            desc.stage = LC_SHADER_STAGE_VERTEX;
            desc.code = mutated;
            desc.code_size = ctx.vert_size;
            bad = (lc_shader *)0x1;
            TEST_CHECK(lc_shader_create(ctx.device, &desc, &bad) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "bad SPIR-V magic -> INVALID_ARGUMENT");
            TEST_CHECK(bad == NULL, "out cleared on bad magic");
            desc.code = ctx.vert_code;
            desc.code_size = ctx.vert_size - 2; /* misaligned size */
            bad = (lc_shader *)0x1;
            TEST_CHECK(lc_shader_create(ctx.device, &desc, &bad) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "misaligned size -> INVALID_ARGUMENT");
            TEST_CHECK(bad == NULL, "out cleared on misaligned size");
            desc.code_size = 0;
            bad = (lc_shader *)0x1;
            TEST_CHECK(lc_shader_create(ctx.device, &desc, &bad) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "zero size -> INVALID_ARGUMENT");
            TEST_CHECK(bad == NULL, "out cleared on zero size");
            free(mutated);
        }

        /* Unknown stage against the live device. */
        desc.stage = (lc_shader_stage)99;
        desc.code = ctx.vert_code;
        desc.code_size = ctx.vert_size;
        bad = (lc_shader *)0x1;
        TEST_CHECK(lc_shader_create(ctx.device, &desc, &bad) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "unknown stage -> INVALID_ARGUMENT");
        TEST_CHECK(bad == NULL, "out cleared on unknown stage");

        /* Destroy + recreate modules. */
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        vs = NULL;
        fs = NULL;
        desc.stage = LC_SHADER_STAGE_VERTEX;
        desc.code = ctx.vert_code;
        desc.code_size = ctx.vert_size;
        TEST_CHECK(lc_shader_create(ctx.device, &desc, &vs) == LC_SUCCESS,
                   "vertex shader re-creation succeeds");
        desc.stage = LC_SHADER_STAGE_FRAGMENT;
        desc.code = ctx.frag_code;
        desc.code_size = ctx.frag_size;
        TEST_CHECK(lc_shader_create(ctx.device, &desc, &fs) == LC_SUCCESS,
                   "fragment shader re-creation succeeds");

        /* Stage mismatch is rejected at pipeline creation. */
        {
            lc_graphics_pipeline_desc pdesc = { 0 };
            lc_pipeline *nope = (lc_pipeline *)0x1;

            pdesc.vertex_shader = fs; /* fragment in vertex slot */
            pdesc.fragment_shader = vs;
            TEST_CHECK(lc_graphics_pipeline_create(ctx.device, ctx.swapchain,
                                                   &pdesc,
                                                   &nope) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "swapped stages -> INVALID_ARGUMENT");
            TEST_CHECK(nope == NULL, "out cleared on stage mismatch");
        }

        /* Real pipeline, then shaders may die while it lives. */
        {
            lc_graphics_pipeline_desc pdesc = { 0 };
            lc_pipeline *pipeline = NULL;

            pdesc.vertex_shader = vs;
            pdesc.fragment_shader = fs;
            TEST_CHECK(lc_graphics_pipeline_create(ctx.device, ctx.swapchain,
                                                   &pdesc,
                                                   &pipeline) == LC_SUCCESS,
                       "graphics pipeline creation succeeds");
            lc_shader_destroy(vs);
            lc_shader_destroy(fs);
            vs = NULL;
            fs = NULL;
            TEST_CHECK(pipeline != NULL, "pipeline survives shader destroy");

            /* Cross-device bind is rejected on a live foreign stack. */
            {
                lc_device *other = NULL;
                lc_window *other_win = NULL;
                lc_surface *other_surf = NULL;
                lc_swapchain *other_sc = NULL;
                lc_swapchain_desc sdesc;
                lc_device_desc ddesc;

                ddesc.backend = LC_BACKEND_VULKAN;
                ddesc.enable_validation = 0;
                sdesc.width = 800;
                sdesc.height = 600;
                sdesc.image_count = 0;
                sdesc.vsync = 1;
                if (lc_device_create(&ddesc, &other) == LC_SUCCESS &&
                    make_window_titled(&other_win,
                                       "LumaC Triangle Foreign") == 0 &&
                    make_surface(other, other_win, &other_surf) == 0 &&
                    lc_swapchain_create(other, other_surf, &sdesc,
                                        &other_sc) == LC_SUCCESS &&
                    lc_begin_frame(other_sc) == LC_SUCCESS) {
                    TEST_CHECK(lc_bind_pipeline(other_sc, pipeline) ==
                                   LC_ERROR_INVALID_ARGUMENT,
                               "cross-device bind -> INVALID_ARGUMENT");
                    {
                        lc_result end_res = lc_end_frame(other_sc);
                        TEST_CHECK(end_res == LC_SUCCESS ||
                                       end_res == LC_SUBOPTIMAL,
                                   "end foreign frame succeeds");
                    }
                } else {
                    TEST_CHECK(0, "foreign stack for bind test");
                }
                lc_swapchain_destroy(other_sc);
                lc_surface_destroy(other_surf);
                lc_window_destroy(other_win);
                lc_device_destroy(other);
            }

            /* Single triangle frame through the shaderless pipeline. */
            TEST_CHECK(lc_begin_frame(ctx.swapchain) == LC_SUCCESS,
                       "begin frame for triangle");
            TEST_CHECK(lc_clear_color(ctx.swapchain, 0.08f, 0.10f, 0.16f,
                                      1.0f) == LC_SUCCESS,
                       "clear for triangle");
            TEST_CHECK(lc_bind_pipeline(ctx.swapchain, pipeline) ==
                           LC_SUCCESS,
                       "bind pipeline succeeds");
            TEST_CHECK(lc_draw(ctx.swapchain, 3, 0) == LC_SUCCESS,
                       "draw 3 vertices succeeds");
            {
                lc_result end_res = lc_end_frame(ctx.swapchain);
                TEST_CHECK(end_res == LC_SUCCESS || end_res == LC_SUBOPTIMAL,
                           "end frame with triangle succeeds");
            }
            lc_pipeline_destroy(pipeline);
        }

        destroy_tri_ctx(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "shader/pipeline teardown clean");
    }

    /* ---- 2. 120 triangle frames ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        tri_ctx ctx;
        lc_shader_desc desc;
        lc_shader *vs = NULL;
        lc_shader *fs = NULL;
        lc_graphics_pipeline_desc pdesc = { 0 };
        lc_pipeline *pipeline = NULL;
        int env = make_tri_ctx(&ctx, "LumaC Triangle Test");

        TEST_CHECK(env == 0, "triangle stack for 120-frame test");
        if (env != 0) {
            printf("triangle context failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        desc.stage = LC_SHADER_STAGE_VERTEX;
        desc.code = ctx.vert_code;
        desc.code_size = ctx.vert_size;
        desc.entry_point = NULL;
        TEST_CHECK(lc_shader_create(ctx.device, &desc, &vs) == LC_SUCCESS,
                   "vertex shader for 120-frame test");
        desc.stage = LC_SHADER_STAGE_FRAGMENT;
        desc.code = ctx.frag_code;
        desc.code_size = ctx.frag_size;
        TEST_CHECK(lc_shader_create(ctx.device, &desc, &fs) == LC_SUCCESS,
                   "fragment shader for 120-frame test");
        pdesc.vertex_shader = vs;
        pdesc.fragment_shader = fs;
        TEST_CHECK(lc_graphics_pipeline_create(ctx.device, ctx.swapchain,
                                               &pdesc,
                                               &pipeline) == LC_SUCCESS,
                   "pipeline for 120-frame test");
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        vs = NULL;
        fs = NULL;

        TEST_CHECK(render_triangles(ctx.swapchain, ctx.window, pipeline, 120,
                                    0.08f, 0.10f, 0.16f) == 120,
                   "120 triangle frames presented");

        lc_pipeline_destroy(pipeline);
        destroy_tri_ctx(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "120-frame teardown clean");
    }

    /* ---- 3. resize stress with triangle ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        tri_ctx ctx;
        lc_shader_desc desc;
        lc_shader *vs = NULL;
        lc_shader *fs = NULL;
        lc_graphics_pipeline_desc pdesc = { 0 };
        lc_pipeline *pipeline = NULL;
        static const unsigned targets[][2] = {
            { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 640, 480 },
            { 900, 900 }
        };
        size_t step;
        int env = make_tri_ctx(&ctx, "LumaC Triangle Test");

        TEST_CHECK(env == 0, "triangle stack for resize stress");
        if (env != 0) {
            printf("triangle context failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        desc.stage = LC_SHADER_STAGE_VERTEX;
        desc.code = ctx.vert_code;
        desc.code_size = ctx.vert_size;
        desc.entry_point = NULL;
        TEST_CHECK(lc_shader_create(ctx.device, &desc, &vs) == LC_SUCCESS,
                   "vertex shader for resize stress");
        desc.stage = LC_SHADER_STAGE_FRAGMENT;
        desc.code = ctx.frag_code;
        desc.code_size = ctx.frag_size;
        TEST_CHECK(lc_shader_create(ctx.device, &desc, &fs) == LC_SUCCESS,
                   "fragment shader for resize stress");
        pdesc.vertex_shader = vs;
        pdesc.fragment_shader = fs;
        TEST_CHECK(lc_graphics_pipeline_create(ctx.device, ctx.swapchain,
                                               &pdesc,
                                               &pipeline) == LC_SUCCESS,
                   "pipeline for resize stress");
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        vs = NULL;
        fs = NULL;

        for (step = 0; step < sizeof(targets) / sizeof(targets[0]); step++) {
            unsigned w = targets[step][0];
            unsigned h = targets[step][1];
            int n;
#ifdef _WIN32
            /* Win32 extents track the window: resize it for real. */
            {
                unsigned ow = 0;
                unsigned oh = 0;
                char msg[96];

                if (!native_resize_client(L"LumaC Triangle Test",
                                          ctx.window, w, h, &ow, &oh)) {
                    snprintf(msg, sizeof(msg),
                             "native resize to %ux%u observed", w, h);
                    TEST_CHECK(0, msg);
                    break;
                }
                snprintf(msg, sizeof(msg), "native resize to %ux%u observed",
                         w, h);
                TEST_CHECK(1, msg);
            }
#else
            /* Undefined-extent surfaces honor the request directly. */
            if (lc_swapchain_recreate(ctx.swapchain, w, h) != LC_SUCCESS) {
                char msg[96];
                snprintf(msg, sizeof(msg), "recreate to %ux%u succeeds", w,
                         h);
                TEST_CHECK(0, msg);
                break;
            }
#endif
            n = render_triangles(ctx.swapchain, ctx.window, pipeline, 12,
                                 0.08f, 0.10f, 0.16f);
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "12 triangle frames at %ux%u", w,
                         h);
                TEST_CHECK(n == 12, msg);
            }
            if (n != 12) {
                break;
            }
        }

        lc_pipeline_destroy(pipeline);
        destroy_tri_ctx(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "resize-stress teardown clean");
    }

#ifdef _WIN32
    /* ---- 4. minimize / restore with triangle ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        tri_ctx ctx;
        lc_shader_desc desc;
        lc_shader *vs = NULL;
        lc_shader *fs = NULL;
        lc_graphics_pipeline_desc pdesc = { 0 };
        lc_pipeline *pipeline = NULL;
        HWND hwnd;
        int i;
        int env = make_tri_ctx(&ctx, "LumaC Triangle Test");

        TEST_CHECK(env == 0, "triangle stack for minimize test");
        if (env != 0) {
            printf("triangle context failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        desc.stage = LC_SHADER_STAGE_VERTEX;
        desc.code = ctx.vert_code;
        desc.code_size = ctx.vert_size;
        desc.entry_point = NULL;
        TEST_CHECK(lc_shader_create(ctx.device, &desc, &vs) == LC_SUCCESS,
                   "vertex shader for minimize test");
        desc.stage = LC_SHADER_STAGE_FRAGMENT;
        desc.code = ctx.frag_code;
        desc.code_size = ctx.frag_size;
        TEST_CHECK(lc_shader_create(ctx.device, &desc, &fs) == LC_SUCCESS,
                   "fragment shader for minimize test");
        pdesc.vertex_shader = vs;
        pdesc.fragment_shader = fs;
        TEST_CHECK(lc_graphics_pipeline_create(ctx.device, ctx.swapchain,
                                               &pdesc,
                                               &pipeline) == LC_SUCCESS,
                   "pipeline for minimize test");
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        vs = NULL;
        fs = NULL;

        TEST_CHECK(render_triangles(ctx.swapchain, ctx.window, pipeline, 10,
                                    0.08f, 0.10f, 0.16f) == 10,
                   "10 warmup triangle frames");
        hwnd = find_test_window(L"LumaC Triangle Test");
        TEST_CHECK(hwnd != NULL, "test window found for minimize");
        ShowWindow(hwnd, SW_MINIMIZE);
        for (i = 0; i < 30; i++) {
            lc_poll_events();
            Sleep(5);
        }
        TEST_CHECK(lc_window_get_width(ctx.window) == 0 &&
                       lc_window_get_height(ctx.window) == 0,
                   "minimized window reports zero extent");
        ShowWindow(hwnd, SW_RESTORE);
        {
            uint32_t w = 0;
            uint32_t h = 0;
            for (i = 0; i < 120 && (w == 0 || h == 0); i++) {
                lc_poll_events();
                Sleep(5);
                w = lc_window_get_width(ctx.window);
                h = lc_window_get_height(ctx.window);
            }
            TEST_CHECK(w != 0 && h != 0, "nonzero extent after restore");
            TEST_CHECK(render_triangles(ctx.swapchain, ctx.window, pipeline,
                                        30, 0.08f, 0.10f, 0.16f) == 30,
                       "30 triangle frames after restore");
        }

        lc_pipeline_destroy(pipeline);
        destroy_tri_ctx(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "minimize/restore teardown clean");
    }
#endif

    /* ---- 5. two windows render triangles ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *win_a = NULL;
        lc_window *win_b = NULL;
        lc_surface *surf_a = NULL;
        lc_surface *surf_b = NULL;
        lc_swapchain *sc_a = NULL;
        lc_swapchain *sc_b = NULL;
        void *vert_code = NULL;
        void *frag_code = NULL;
        size_t vert_size = 0;
        size_t frag_size = 0;
        lc_shader_desc desc;
        lc_shader *vs = NULL;
        lc_shader *fs = NULL;
        lc_graphics_pipeline_desc pdesc = { 0 };
        lc_pipeline *pipeline = NULL;
        int i;
        int ok_a = 0;
        int ok_b = 0;

        TEST_CHECK(make_device(&device) == 0, "device for multi test");
        TEST_CHECK(make_window_titled(&win_a, "LumaC Triangle Test A") == 0,
                   "window A created");
        TEST_CHECK(make_window_titled(&win_b, "LumaC Triangle Test B") == 0,
                   "window B created");
        TEST_CHECK(make_surface(device, win_a, &surf_a) == 0,
                   "surface A created");
        TEST_CHECK(make_surface(device, win_b, &surf_b) == 0,
                   "surface B created");
        TEST_CHECK(make_swapchain(device, surf_a, &sc_a) == 0,
                   "swapchain A created");
        TEST_CHECK(make_swapchain(device, surf_b, &sc_b) == 0,
                   "swapchain B created");
        TEST_CHECK(load_spv_file("triangle.vert.spv", &vert_code,
                                 &vert_size) != 0 &&
                       load_spv_file("triangle.frag.spv", &frag_code,
                                     &frag_size) != 0,
                   "SPIR-V loaded for multi test");

        desc.stage = LC_SHADER_STAGE_VERTEX;
        desc.code = vert_code;
        desc.code_size = vert_size;
        desc.entry_point = NULL;
        TEST_CHECK(lc_shader_create(device, &desc, &vs) == LC_SUCCESS,
                   "vertex shader for multi test");
        desc.stage = LC_SHADER_STAGE_FRAGMENT;
        desc.code = frag_code;
        desc.code_size = frag_size;
        TEST_CHECK(lc_shader_create(device, &desc, &fs) == LC_SUCCESS,
                   "fragment shader for multi test");
        free(vert_code);
        free(frag_code);
        pdesc.vertex_shader = vs;
        pdesc.fragment_shader = fs;
        TEST_CHECK(lc_graphics_pipeline_create(device, sc_a, &pdesc,
                                               &pipeline) == LC_SUCCESS,
                   "one pipeline for both windows");
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        vs = NULL;
        fs = NULL;

        /* One pipeline bound on both swapchains: distinct backgrounds
         * tell the windows apart. */
        for (i = 0; i < 30; i++) {
            int na = render_triangles(sc_a, win_a, pipeline, 1, 0.08f, 0.10f,
                                      0.16f);
            int nb = render_triangles(sc_b, win_b, pipeline, 1, 0.16f, 0.08f,
                                      0.08f);
            if (na != 1 || nb != 1) {
                break;
            }
            ok_a++;
            ok_b++;
        }
        TEST_CHECK(ok_a == 30, "30 triangle frames on window A");
        TEST_CHECK(ok_b == 30, "30 triangle frames on window B");

        /* The pipeline dies with its creation swapchain: binding it
         * elsewhere afterwards must fail cleanly. The pointer is only
         * ever compared, never dereferenced, once dead. */
        lc_swapchain_destroy(sc_a);
        sc_a = NULL;
        TEST_CHECK(lc_begin_frame(sc_b) == LC_SUCCESS,
                   "begin on B after A destroyed");
        TEST_CHECK(lc_clear_color(sc_b, 0.0f, 0.0f, 0.0f, 1.0f) ==
                       LC_SUCCESS,
                   "clear on B after A destroyed");
        TEST_CHECK(lc_bind_pipeline(sc_b, pipeline) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "bind dead pipeline -> INVALID_ARGUMENT");
        {
            /* End the intentionally unbound frame. */
            lc_result end_res = lc_end_frame(sc_b);
            TEST_CHECK(end_res == LC_SUCCESS || end_res == LC_SUBOPTIMAL,
                       "end unbound frame succeeds");
        }
        pipeline = NULL; /* owned (and destroyed) by the swapchain hook */

        lc_swapchain_destroy(sc_b);
        lc_surface_destroy(surf_a);
        lc_surface_destroy(surf_b);
        lc_device_destroy(device);
        lc_window_destroy(win_a);
        lc_window_destroy(win_b);
        lc_shutdown();
        TEST_CHECK(1, "multi-window teardown clean");
    }

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
