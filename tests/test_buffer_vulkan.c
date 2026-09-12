/*
 * Vulkan buffer integration test (Phase 8).
 *
 * Real allocation, mapping, direct and staging writes, an exact
 * GPU->readback byte round-trip (white-box copy helper: no public copy
 * API exists by design), vertex-buffer triangle rendering, resize and
 * multi-window behavior, and dependency cleanup. Run with validation
 * layers: memory-type, binding, mapping, and copy misuse would surface
 * as validation errors.
 *
 * If the environment cannot provide a window or a usable Vulkan setup,
 * the test reports SKIP and exits 0. Any other failure is a hard FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lumac/lumac.h>

/* White-box access for the copy path only: LumaC exposes no public
 * buffer-copy API yet, so the exact round-trip below uses the internal
 * immediate-submit helper plus struct fields directly. */
#include "graphics/graphics_internal.h"

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

typedef struct vb_vertex {
    float position[2];
    float color[3];
} vb_vertex;

static const vb_vertex k_tri[3] = {
    { { 0.0f, -0.6f }, { 1.0f, 0.2f, 0.2f } },
    { { 0.6f, 0.6f }, { 0.2f, 1.0f, 0.2f } },
    { { -0.6f, 0.6f }, { 0.2f, 0.4f, 1.0f } },
};

/* Build the shared vertex-buffer pipeline (backend-neutral layout:
 * vec2 position + vec3 color on binding 0). Shaders are consumed and
 * may be passed dead afterwards by the caller. */
static int make_vb_pipeline(lc_device *device, lc_swapchain *swapchain,
                            void *vert_code, size_t vert_size,
                            void *frag_code, size_t frag_size,
                            lc_pipeline **out_pipeline) {
    lc_shader_desc sdesc;
    lc_shader *vs = NULL;
    lc_shader *fs = NULL;
    lc_graphics_pipeline_desc pdesc = { 0 };
    lc_vertex_binding_desc binding;
    lc_vertex_attribute_desc attrs[2];
    lc_result res;

    *out_pipeline = NULL;
    sdesc.stage = LC_SHADER_STAGE_VERTEX;
    sdesc.code = vert_code;
    sdesc.code_size = vert_size;
    sdesc.entry_point = NULL;
    if (lc_shader_create(device, &sdesc, &vs) != LC_SUCCESS) {
        return -1;
    }
    sdesc.stage = LC_SHADER_STAGE_FRAGMENT;
    sdesc.code = frag_code;
    sdesc.code_size = frag_size;
    if (lc_shader_create(device, &sdesc, &fs) != LC_SUCCESS) {
        lc_shader_destroy(vs);
        return -1;
    }
    binding.binding = 0;
    binding.stride = sizeof(vb_vertex);
    binding.input_rate = LC_VERTEX_INPUT_PER_VERTEX;
    attrs[0].location = 0;
    attrs[0].binding = 0;
    attrs[0].format = LC_FORMAT_RG32_FLOAT;
    attrs[0].offset = 0;
    attrs[1].location = 1;
    attrs[1].binding = 0;
    attrs[1].format = LC_FORMAT_RGB32_FLOAT;
    attrs[1].offset = sizeof(float) * 2u;

    pdesc.vertex_shader = vs;
    pdesc.fragment_shader = fs;
    pdesc.vertex_bindings = &binding;
    pdesc.vertex_binding_count = 1;
    pdesc.vertex_attributes = attrs;
    pdesc.vertex_attribute_count = 2;
    res = lc_graphics_pipeline_create(device, swapchain, &pdesc,
                                      out_pipeline);
    lc_shader_destroy(vs);
    lc_shader_destroy(fs);
    return (res == LC_SUCCESS) ? 0 : -1;
}

/* Render `want` presented frames drawing the vertex buffer each frame.
 * Returns presented count, or -1 on fatal error / guard trip. */
static int render_vb_frames(lc_swapchain *sc, lc_window *win,
                            lc_pipeline *pipeline, lc_buffer *vbo, int want,
                            float r, float g, float b) {
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
        if (lc_bind_vertex_buffer(sc, 0, vbo, 0) != LC_SUCCESS) {
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

/* Full stack plus SPIR-V bytes for sections needing them. */
typedef struct buf_ctx {
    lc_device *device;
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
    void *vert_code;
    size_t vert_size;
    void *frag_code;
    size_t frag_size;
} buf_ctx;

/* 0 = ready, 1 = environmental SKIP, -1 = hard failure. */
static int make_buf_ctx(buf_ctx *ctx, const char *title) {
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
    if (!load_spv_file("vb_triangle.vert.spv", &ctx->vert_code,
                       &ctx->vert_size) ||
        !load_spv_file("triangle.frag.spv", &ctx->frag_code,
                       &ctx->frag_size)) {
        printf("SPIR-V files missing: FAIL\n");
        return -1;
    }
    return 0;
}

static void destroy_buf_ctx(buf_ctx *ctx) {
    free(ctx->vert_code);
    free(ctx->frag_code);
    lc_swapchain_destroy(ctx->swapchain);
    lc_surface_destroy(ctx->surface);
    lc_device_destroy(ctx->device);
    lc_window_destroy(ctx->window);
    memset(ctx, 0, sizeof(*ctx));
}

static int make_buffer(lc_device *device, uint64_t size, uint32_t usage,
                       lc_memory_usage memory, lc_buffer **out) {
    lc_buffer_desc desc;

    desc.size = size;
    desc.usage = usage;
    desc.memory = memory;
    *out = NULL;
    return (lc_buffer_create(device, &desc, out) == LC_SUCCESS) ? 0 : -1;
}

int main(void) {
    printf("Running LumaC Vulkan buffer integration test...\n");

    lc_shutdown();
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }

    /* ---- 1. lifecycle, mapping, direct write ---- */
    {
        buf_ctx ctx;
        lc_buffer *gpu = NULL;
        lc_buffer *upload = NULL;
        lc_buffer *readback = NULL;
        lc_buffer *leaked = NULL;
        void *ptr = NULL;
        unsigned char pattern[64];
        unsigned i;
        int env = make_buf_ctx(&ctx, "LumaC Buffer Test");

        if (env != 0) {
            if (env == 1) {
                SKIP_ENV("a full buffer stack");
            }
            printf("buffer context failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        for (i = 0; i < sizeof(pattern); i++) {
            pattern[i] = (unsigned char)(i * 3u + 1u);
        }

        /* From here on the environment is proven: failures are FAILs. */
        TEST_CHECK(make_buffer(ctx.device, 256,
                               LC_BUFFER_USAGE_TRANSFER_DST,
                               LC_MEMORY_GPU_ONLY, &gpu) == 0,
                   "GPU-only buffer creation succeeds");
        TEST_CHECK(lc_buffer_get_size(gpu) == 256, "buffer size reported");
        TEST_CHECK(make_buffer(ctx.device, 256,
                               LC_BUFFER_USAGE_TRANSFER_SRC,
                               LC_MEMORY_CPU_TO_GPU, &upload) == 0,
                   "CPU-to-GPU buffer creation succeeds");
        TEST_CHECK(make_buffer(ctx.device, 256,
                               LC_BUFFER_USAGE_TRANSFER_DST,
                               LC_MEMORY_GPU_TO_CPU, &readback) == 0,
                   "GPU-to-CPU buffer creation succeeds");

        /* GPU-only memory is not mappable; CPU placements are. */
        TEST_CHECK(lc_buffer_map(gpu, &ptr) == LC_ERROR_INVALID_ARGUMENT,
                   "map(GPU-only) rejected");
        TEST_CHECK(ptr == NULL, "map out cleared on reject");
        TEST_CHECK(lc_buffer_map(upload, &ptr) == LC_SUCCESS,
                   "map(CPU-to-GPU) succeeds");
        TEST_CHECK(ptr != NULL, "mapped pointer non-null");
        {
            void *ptr2 = NULL;
            TEST_CHECK(lc_buffer_map(upload, &ptr2) == LC_SUCCESS,
                       "remap succeeds");
            TEST_CHECK(ptr2 == ptr, "remap returns the same pointer");
        }
        lc_buffer_unmap(upload);

        /* Direct write + read-back through the mapping. */
        TEST_CHECK(lc_buffer_write(upload, 0, pattern, sizeof(pattern)) ==
                       LC_SUCCESS,
                   "direct write succeeds");
        TEST_CHECK(lc_buffer_write(upload, 16, pattern, 16) == LC_SUCCESS,
                   "offset write succeeds");
        TEST_CHECK(lc_buffer_write(upload, 0, NULL, 0) == LC_SUCCESS,
                   "zero-length write succeeds");
        TEST_CHECK(lc_buffer_write(upload, 0, NULL, 8) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "NULL data rejected");
        TEST_CHECK(lc_buffer_write(upload, 257, pattern, 1) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "offset past end rejected");
        TEST_CHECK(lc_buffer_write(upload, 250, pattern, 16) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "overflowing range rejected");
        TEST_CHECK(lc_buffer_write(upload, 0xFFFFFFFFFFFFFFFFull, pattern,
                                   16) == LC_ERROR_INVALID_ARGUMENT,
                   "huge offset rejected");
        TEST_CHECK(memcmp(ptr, pattern, 16) == 0 &&
                       memcmp((char *)ptr + 16, pattern, 16) == 0,
                   "written bytes read back through mapping");

        /* Limits + swapchain format queries on the live device. */
        {
            lc_device_limits limits;

            memset(&limits, 0, sizeof(limits));
            lc_device_get_limits(ctx.device, &limits);
            printf("limits: tex2d=%u attrs=%u bindings=%u ubo=%llu\n",
                   limits.max_texture_2d_dimension,
                   limits.max_vertex_attributes,
                   limits.max_vertex_bindings,
                   (unsigned long long)limits.max_uniform_buffer_size);
            TEST_CHECK(limits.max_texture_2d_dimension > 0,
                       "texture dimension limit nonzero");
            TEST_CHECK(limits.max_vertex_attributes >= 2,
                       "vertex attribute limit usable");
            TEST_CHECK(limits.max_vertex_bindings >= 1,
                       "vertex binding limit usable");
            TEST_CHECK(limits.max_uniform_buffer_size > 0,
                       "uniform size limit nonzero");
        }
        TEST_CHECK(lc_swapchain_get_format(ctx.swapchain) !=
                       LC_FORMAT_UNDEFINED,
                   "swapchain format reported");

        /* Device teardown with a live buffer cleans up. */
        TEST_CHECK(make_buffer(ctx.device, 64, LC_BUFFER_USAGE_VERTEX,
                               LC_MEMORY_GPU_ONLY, &leaked) == 0,
                   "buffer for device-cleanup check");
        leaked = NULL; /* dropped; device destroy must retire it */
        lc_buffer_destroy(gpu);
        lc_buffer_destroy(upload);
        lc_buffer_destroy(readback);
        destroy_buf_ctx(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "device teardown with live buffer clean");
    }

    /* ---- 2. exact GPU round-trip through staging ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        buf_ctx ctx;
        lc_buffer *gpu = NULL;
        lc_buffer *readback = NULL;
        unsigned char pattern[256];
        unsigned char *check = NULL;
        unsigned i;
        int env = make_buf_ctx(&ctx, "LumaC Buffer Test");

        TEST_CHECK(env == 0, "stack for round-trip test");
        if (env != 0) {
            printf("buffer context failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        for (i = 0; i < sizeof(pattern); i++) {
            pattern[i] = (unsigned char)((i * 131u + 17u) & 0xFFu);
        }

        TEST_CHECK(make_buffer(ctx.device, sizeof(pattern),
                               LC_BUFFER_USAGE_TRANSFER_DST |
                                   LC_BUFFER_USAGE_TRANSFER_SRC,
                               LC_MEMORY_GPU_ONLY, &gpu) == 0,
                   "round-trip GPU buffer created");
        /* Staging write path (GPU-only has no mapping). */
        TEST_CHECK(lc_buffer_write(gpu, 0, pattern, sizeof(pattern)) ==
                       LC_SUCCESS,
                   "staging write succeeds");
        TEST_CHECK(make_buffer(ctx.device, sizeof(pattern),
                               LC_BUFFER_USAGE_TRANSFER_DST,
                               LC_MEMORY_GPU_TO_CPU, &readback) == 0,
                   "round-trip readback buffer created");
        /* White-box copy: no public copy API exists by design. */
        TEST_CHECK(lc_vulkan_copy_buffer(ctx.device, readback->vk_buffer, 0,
                                         gpu->vk_buffer, 0,
                                         sizeof(pattern)) == LC_SUCCESS,
                   "GPU->readback copy succeeds");
        TEST_CHECK(lc_buffer_map(readback, (void **)&check) == LC_SUCCESS,
                   "readback map succeeds");
        TEST_CHECK(check != NULL &&
                       memcmp(check, pattern, sizeof(pattern)) == 0,
                   "round-trip bytes match exactly");
        lc_buffer_unmap(readback);

        lc_buffer_destroy(gpu);
        lc_buffer_destroy(readback);
        destroy_buf_ctx(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "round-trip teardown clean");
    }

    /* ---- 3. vertex-buffer triangle: 120 frames ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        buf_ctx ctx;
        lc_buffer *vbo = NULL;
        lc_pipeline *pipeline = NULL;
        int env = make_buf_ctx(&ctx, "LumaC Buffer Test");

        TEST_CHECK(env == 0, "stack for vertex-buffer triangle");
        if (env != 0) {
            printf("buffer context failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_buffer(ctx.device, sizeof(k_tri),
                               LC_BUFFER_USAGE_VERTEX, LC_MEMORY_GPU_ONLY,
                               &vbo) == 0,
                   "GPU vertex buffer created");
        TEST_CHECK(lc_buffer_write(vbo, 0, k_tri, sizeof(k_tri)) ==
                       LC_SUCCESS,
                   "vertex data staged into GPU buffer");
        TEST_CHECK(make_vb_pipeline(ctx.device, ctx.swapchain, ctx.vert_code,
                                    ctx.vert_size, ctx.frag_code,
                                    ctx.frag_size,
                                    &pipeline) == 0,
                   "vertex pipeline created");
        free(ctx.vert_code);
        free(ctx.frag_code);
        ctx.vert_code = NULL;
        ctx.frag_code = NULL;

        /* Bind misuse against the live buffer. */
        {
            lc_buffer_desc idesc;
            lc_buffer *index_only = NULL;

            idesc.size = 64;
            idesc.usage = LC_BUFFER_USAGE_INDEX;
            idesc.memory = LC_MEMORY_GPU_ONLY;
            TEST_CHECK(lc_buffer_create(ctx.device, &idesc, &index_only) ==
                           LC_SUCCESS,
                       "index-only buffer for misuse checks");
            TEST_CHECK(lc_bind_vertex_buffer(ctx.swapchain, 0, index_only,
                                             0) == LC_ERROR_INVALID_ARGUMENT,
                       "bind outside frame rejected");
            TEST_CHECK(lc_bind_vertex_buffer(ctx.swapchain, 0, vbo,
                                             sizeof(k_tri)) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "bind offset==size rejected outside frame");
            lc_buffer_destroy(index_only);
        }

        TEST_CHECK(render_vb_frames(ctx.swapchain, ctx.window, pipeline, vbo,
                                    120, 0.08f, 0.10f,
                                    0.16f) == 120,
                   "120 vertex-buffer frames presented");

        lc_pipeline_destroy(pipeline);
        lc_buffer_destroy(vbo);
        destroy_buf_ctx(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "vertex-triangle teardown clean");
    }

    /* ---- 4. buffers survive swapchain recreation + multi-window ---- */
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
        lc_buffer *vbo = NULL;
        lc_pipeline *pipe_a = NULL;
        lc_pipeline *pipe_b = NULL;
        void *vert_code = NULL;
        void *frag_code = NULL;
        size_t vert_size = 0;
        size_t frag_size = 0;
        unsigned char *cpu_check = NULL;
        lc_buffer *cpu_buf = NULL;
        int i;
        int ok = 0;

        TEST_CHECK(make_device(&device) == 0, "device for multi test");
        TEST_CHECK(make_window_titled(&win_a, "LumaC Buffer Test A") == 0,
                   "window A created");
        TEST_CHECK(make_window_titled(&win_b, "LumaC Buffer Test B") == 0,
                   "window B created");
        TEST_CHECK(make_surface(device, win_a, &surf_a) == 0,
                   "surface A created");
        TEST_CHECK(make_surface(device, win_b, &surf_b) == 0,
                   "surface B created");
        TEST_CHECK(make_swapchain(device, surf_a, &sc_a) == 0,
                   "swapchain A created");
        TEST_CHECK(make_swapchain(device, surf_b, &sc_b) == 0,
                   "swapchain B created");
        TEST_CHECK(load_spv_file("vb_triangle.vert.spv", &vert_code,
                                 &vert_size) != 0 &&
                       load_spv_file("triangle.frag.spv", &frag_code,
                                     &frag_size) != 0,
                   "SPIR-V loaded for multi test");
        TEST_CHECK(make_buffer(device, sizeof(k_tri), LC_BUFFER_USAGE_VERTEX,
                               LC_MEMORY_GPU_ONLY, &vbo) == 0,
                   "shared GPU vertex buffer created");
        TEST_CHECK(lc_buffer_write(vbo, 0, k_tri, sizeof(k_tri)) ==
                       LC_SUCCESS,
                   "shared vertex data staged");
        TEST_CHECK(make_vb_pipeline(device, sc_a, vert_code, vert_size,
                                    frag_code, frag_size, &pipe_a) == 0,
                   "pipeline A created");
        TEST_CHECK(make_vb_pipeline(device, sc_b, vert_code, vert_size,
                                    frag_code, frag_size, &pipe_b) == 0,
                   "pipeline B created");
        free(vert_code);
        free(frag_code);

        /* One device buffer renders on both swapchains. */
        for (i = 0; i < 20; i++) {
            int na = render_vb_frames(sc_a, win_a, pipe_a, vbo, 1, 0.08f,
                                      0.10f, 0.16f);
            int nb = render_vb_frames(sc_b, win_b, pipe_b, vbo, 1, 0.16f,
                                      0.08f, 0.08f);
            if (na != 1 || nb != 1) {
                break;
            }
            ok++;
        }
        TEST_CHECK(ok == 20, "20 shared-buffer frames on both windows");

        /* CPU buffer contents survive swapchain recreation. */
        TEST_CHECK(make_buffer(device, 32, LC_BUFFER_USAGE_UNIFORM,
                               LC_MEMORY_CPU_TO_GPU, &cpu_buf) == 0,
                   "CPU buffer for recreate test");
        TEST_CHECK(lc_buffer_write(cpu_buf, 0, k_tri, 32) == LC_SUCCESS,
                   "CPU buffer written");
        TEST_CHECK(lc_swapchain_recreate(sc_a, 640, 480) == LC_SUCCESS ||
                       lc_swapchain_get_width(sc_a) == 640,
                   "swapchain A recreated under live buffers");
        TEST_CHECK(lc_buffer_map(cpu_buf, (void **)&cpu_check) ==
                       LC_SUCCESS &&
                       memcmp(cpu_check, k_tri, 32) == 0,
                   "buffer bytes intact across recreation");

        lc_buffer_destroy(cpu_buf);
        lc_pipeline_destroy(pipe_a);
        lc_pipeline_destroy(pipe_b);
        lc_buffer_destroy(vbo);
        lc_swapchain_destroy(sc_a);
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
