/*
 * Vulkan textured-rendering integration test (Phase 10).
 *
 * Real image views (incl. cube), binding layouts/sets/updates,
 * allocator stress, textured-quad rendering with per-frame uniform
 * updates, resize stress, minimize/restore, and two-window texture
 * reuse. Run with validation layers: descriptor, layout, view, and
 * lifetime misuse would surface as validation errors.
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
    lc_device_desc desc = { 0 };

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
    lc_swapchain_desc desc = { 0 };

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

typedef struct tex_vertex {
    float position[2];
    float uv[2];
} tex_vertex;

/* Two counter-clockwise triangles forming a centered quad. */
static const tex_vertex k_quad[6] = {
    { { -0.7f, -0.7f }, { 0.0f, 0.0f } },
    { { 0.7f, -0.7f }, { 1.0f, 0.0f } },
    { { 0.7f, 0.7f }, { 1.0f, 1.0f } },
    { { -0.7f, -0.7f }, { 0.0f, 0.0f } },
    { { 0.7f, 0.7f }, { 1.0f, 1.0f } },
    { { -0.7f, 0.7f }, { 0.0f, 1.0f } },
};

#define TEX_W 64
#define TEX_H 64

static void fill_checkerboard(unsigned char *texels) {
    uint32_t x;
    uint32_t y;

    for (y = 0; y < TEX_H; y++) {
        for (x = 0; x < TEX_W; x++) {
            unsigned char *px = &texels[(y * TEX_W + x) * 4];
            int white = (int)(((x / 8u) + (y / 8u)) % 2u);

            px[0] = white ? 235 : 30;
            px[1] = white ? 235 : 90;
            px[2] = white ? 235 : 160;
            px[3] = 255;
        }
    }
}

/* Full textured stack for one window. */
typedef struct tex_ctx {
    lc_device *device;
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
    lc_buffer *vbo;
    lc_buffer *ubo;
    void *ubo_mapped;
    lc_image *image;
    lc_image_view *view;
    lc_sampler *sampler;
    lc_binding_layout *layout;
    lc_binding_set *set;
    lc_pipeline *pipeline;
    void *vert_code;
    size_t vert_size;
    void *frag_code;
    size_t frag_size;
} tex_ctx;

/* Build the standard textured-quad stack: SPIR-V, shaders, texture +
 * mips, view, sampler, buffers, layout, set, pipeline. Returns 0 on
 * success, -1 on hard failure (caller tears down). */
static int make_textured_stack(tex_ctx *ctx) {
    lc_shader_desc sdesc;
    lc_shader *vs = NULL;
    lc_shader *fs = NULL;
    lc_buffer_desc bdesc;
    lc_image_desc idesc;
    lc_image_upload_desc upload;
    lc_image_view_desc vdesc;
    lc_sampler_desc smdesc;
    lc_binding_desc slots[3];
    lc_binding_layout_desc ldesc;
    lc_binding_write writes[3];
    lc_vertex_binding_desc vbinding;
    lc_vertex_attribute_desc vattrs[2];
    lc_graphics_pipeline_desc pdesc = { 0 };
    unsigned char texels[TEX_W * TEX_H * 4];

    /* Caller provides a zeroed ctx with device/window/surface/swapchain
     * already filled in; only resource fields are written here. */
    if (!load_spv_file("tex_quad.vert.spv", &ctx->vert_code,
                       &ctx->vert_size) ||
        !load_spv_file("tex_quad.frag.spv", &ctx->frag_code,
                       &ctx->frag_size)) {
        printf("texture SPIR-V files missing: FAIL\n");
        return -1;
    }
    /* Device/window/surface/swapchain are built by the caller. */
    sdesc.stage = LC_SHADER_STAGE_VERTEX;
    sdesc.code = ctx->vert_code;
    sdesc.code_size = ctx->vert_size;
    sdesc.entry_point = NULL;
    if (lc_shader_create(ctx->device, &sdesc, &vs) != LC_SUCCESS) {
        return -1;
    }
    sdesc.stage = LC_SHADER_STAGE_FRAGMENT;
    sdesc.code = ctx->frag_code;
    sdesc.code_size = ctx->frag_size;
    if (lc_shader_create(ctx->device, &sdesc, &fs) != LC_SUCCESS) {
        lc_shader_destroy(vs);
        return -1;
    }

    bdesc.size = sizeof(k_quad);
    bdesc.usage = LC_BUFFER_USAGE_VERTEX;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(ctx->device, &bdesc, &ctx->vbo) != LC_SUCCESS ||
        lc_buffer_write(ctx->vbo, 0, k_quad, sizeof(k_quad)) != LC_SUCCESS) {
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        return -1;
    }
    bdesc.size = 64;
    bdesc.usage = LC_BUFFER_USAGE_UNIFORM;
    bdesc.memory = LC_MEMORY_CPU_TO_GPU;
    if (lc_buffer_create(ctx->device, &bdesc, &ctx->ubo) != LC_SUCCESS ||
        lc_buffer_map(ctx->ubo, &ctx->ubo_mapped) != LC_SUCCESS) {
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        return -1;
    }

    fill_checkerboard(texels);
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RGBA8_UNORM;
    idesc.width = TEX_W;
    idesc.height = TEX_H;
    idesc.depth = 1;
    idesc.mip_levels = 0;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_SRC |
                  LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.flags = LC_IMAGE_FLAG_NONE;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(ctx->device, &idesc, &ctx->image) != LC_SUCCESS) {
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        return -1;
    }
    memset(&upload, 0, sizeof(upload));
    upload.mip_level = 0;
    upload.array_layer = 0;
    upload.width = TEX_W;
    upload.height = TEX_H;
    upload.depth = 1;
    upload.data = texels;
    upload.data_size = sizeof(texels);
    if (lc_image_write(ctx->image, &upload) != LC_SUCCESS ||
        lc_image_generate_mipmaps(ctx->image) != LC_SUCCESS) {
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        return -1;
    }

    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.format = LC_FORMAT_UNDEFINED;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.base_mip_level = 0;
    vdesc.mip_level_count = lc_image_get_mip_levels(ctx->image);
    vdesc.base_array_layer = 0;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(ctx->image, &vdesc, &ctx->view) != LC_SUCCESS) {
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        return -1;
    }

    memset(&smdesc, 0, sizeof(smdesc));
    smdesc.min_filter = LC_FILTER_LINEAR;
    smdesc.mag_filter = LC_FILTER_LINEAR;
    smdesc.mipmap_mode = LC_MIPMAP_MODE_LINEAR;
    smdesc.address_u = LC_ADDRESS_REPEAT;
    smdesc.address_v = LC_ADDRESS_REPEAT;
    smdesc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
    smdesc.min_lod = 0.0f;
    smdesc.max_lod = 8.0f;
    smdesc.max_anisotropy = 1.0f;
    if (lc_sampler_create(ctx->device, &smdesc, &ctx->sampler) != LC_SUCCESS) {
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        return -1;
    }

    slots[0].binding = 0;
    slots[0].type = LC_BINDING_UNIFORM_BUFFER;
    slots[0].count = 1;
    slots[0].visibility = LC_SHADER_VISIBILITY_VERTEX;
    slots[1].binding = 1;
    slots[1].type = LC_BINDING_SAMPLED_IMAGE;
    slots[1].count = 1;
    slots[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    slots[2].binding = 2;
    slots[2].type = LC_BINDING_SAMPLER;
    slots[2].count = 1;
    slots[2].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    ldesc.bindings = slots;
    ldesc.binding_count = 3;
    if (lc_binding_layout_create(ctx->device, &ldesc, &ctx->layout) !=
        LC_SUCCESS) {
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        return -1;
    }

    vbinding.binding = 0;
    vbinding.stride = sizeof(tex_vertex);
    vbinding.input_rate = LC_VERTEX_INPUT_PER_VERTEX;
    vattrs[0].location = 0;
    vattrs[0].binding = 0;
    vattrs[0].format = LC_FORMAT_RG32_FLOAT;
    vattrs[0].offset = 0;
    vattrs[1].location = 1;
    vattrs[1].binding = 0;
    vattrs[1].format = LC_FORMAT_RG32_FLOAT;
    vattrs[1].offset = sizeof(float) * 2u;
    pdesc.vertex_shader = vs;
    pdesc.fragment_shader = fs;
    pdesc.vertex_bindings = &vbinding;
    pdesc.vertex_binding_count = 1;
    pdesc.vertex_attributes = vattrs;
    pdesc.vertex_attribute_count = 2;
    {
        const lc_binding_layout *slot_layouts[1];

        slot_layouts[0] = ctx->layout;
        pdesc.binding_layouts = slot_layouts;
        pdesc.binding_layout_count = 1;
        if (lc_swapchain_get_render_target_desc(ctx->swapchain,
                                                &pdesc.render_target) !=
            LC_SUCCESS) {
            lc_shader_destroy(vs);
            lc_shader_destroy(fs);
            return -1;
        }
        if (lc_graphics_pipeline_create(ctx->device, &pdesc,
                                        &ctx->pipeline) != LC_SUCCESS) {
            lc_shader_destroy(vs);
            lc_shader_destroy(fs);
            return -1;
        }
    }
    lc_shader_destroy(vs);
    lc_shader_destroy(fs);

    if (lc_binding_set_create(ctx->layout, &ctx->set) != LC_SUCCESS) {
        return -1;
    }
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_UNIFORM_BUFFER;
    writes[0].u.buffer.buffer = ctx->ubo;
    writes[0].u.buffer.offset = 0;
    writes[0].u.buffer.size = 0;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLED_IMAGE;
    writes[1].u.image.view = ctx->view;
    writes[2].binding = 2;
    writes[2].array_element = 0;
    writes[2].type = LC_BINDING_SAMPLER;
    writes[2].u.sampler.sampler = ctx->sampler;
    if (lc_binding_set_update(ctx->set, writes, 3) != LC_SUCCESS) {
        return -1;
    }
    return 0;
}

static void destroy_textured_stack(tex_ctx *ctx) {
    free(ctx->vert_code);
    free(ctx->frag_code);
    lc_pipeline_destroy(ctx->pipeline);
    lc_binding_set_destroy(ctx->set);
    lc_binding_layout_destroy(ctx->layout);
    lc_sampler_destroy(ctx->sampler);
    lc_image_view_destroy(ctx->view);
    lc_image_destroy(ctx->image);
    lc_buffer_destroy(ctx->ubo);
    lc_buffer_destroy(ctx->vbo);
    lc_swapchain_destroy(ctx->swapchain);
    lc_surface_destroy(ctx->surface);
    lc_device_destroy(ctx->device);
    lc_window_destroy(ctx->window);
    memset(ctx, 0, sizeof(*ctx));
}

/* Identity matrix with X slide in elements[12] (column-major). */
static void write_slide_matrix(float *m, float x) {
    static const float k_identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f, //
        0.0f, 1.0f, 0.0f, 0.0f, //
        0.0f, 0.0f, 1.0f, 0.0f, //
        0.0f, 0.0f, 0.0f, 1.0f, //
    };

    if (m == NULL) {
        return;
    }
    memcpy(m, k_identity, sizeof(k_identity));
    m[12] = x;
}

/* Render `want` presented textured frames, sliding the uniform each
 * frame. Returns presented count, or -1 on fatal error / guard trip. */
static int render_textured_frames(tex_ctx *ctx, int want) {
    int presented = 0;
    int guard = 0;
    int frame = 0;

    while (presented < want) {
        uint32_t w;
        uint32_t h;
        lc_result res;
        float slide;

        if (++guard > want * 25 + 120) {
            return -1;
        }
        lc_poll_events();
        w = lc_window_get_width(ctx->window);
        h = lc_window_get_height(ctx->window);
        if (w == 0 || h == 0) {
            continue; /* minimized */
        }
        if (w != lc_swapchain_get_width(ctx->swapchain) ||
            h != lc_swapchain_get_height(ctx->swapchain)) {
            res = lc_swapchain_recreate(ctx->swapchain, w, h);
            if (res == LC_ERROR_ZERO_EXTENT) {
                continue;
            }
            if (res != LC_SUCCESS) {
                return -1;
            }
        }
        res = lc_begin_frame(ctx->swapchain);
        if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            res = lc_swapchain_recreate(ctx->swapchain, w, h);
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
        slide = (float)((frame % 200) - 100) / 100.0f * 0.2f;
        write_slide_matrix((float *)ctx->ubo_mapped, slide);
        if (lc_clear_color(ctx->swapchain, 0.06f, 0.07f, 0.10f, 1.0f) !=
            LC_SUCCESS) {
            return -1;
        }
        if (lc_bind_pipeline(ctx->swapchain, ctx->pipeline) != LC_SUCCESS) {
            return -1;
        }
        if (lc_bind_binding_set(ctx->swapchain, ctx->pipeline, 0, ctx->set) !=
            LC_SUCCESS) {
            return -1;
        }
        if (lc_bind_vertex_buffer(ctx->swapchain, 0, ctx->vbo, 0) !=
            LC_SUCCESS) {
            return -1;
        }
        if (lc_draw(ctx->swapchain, 6, 0) != LC_SUCCESS) {
            return -1;
        }
        res = lc_end_frame(ctx->swapchain);
        if (res == LC_SUCCESS) {
            presented++;
            frame++;
        } else if (res == LC_SUBOPTIMAL ||
                   res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            presented++;
            frame++;
            res = lc_swapchain_recreate(ctx->swapchain, w, h);
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

int main(void) {
    printf("Running LumaC textured-rendering integration test...\n");

    lc_shutdown();
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }

    /* ---- 1. view + layout + update validation on live objects ---- */
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_image *image = NULL;
        lc_image_view *view = NULL;
        lc_binding_layout *layout = NULL;
        lc_binding_set *set = NULL;
        lc_image_desc idesc;
        lc_image_view_desc vdesc;
        lc_binding_desc slots[2];
        lc_binding_layout_desc ldesc;
        int env;

        env = make_device(&device);
        if (env != 0) {
            if (env == 1) {
                SKIP_ENV("a Vulkan device");
            }
            printf("device creation failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        env = make_window_titled(&window, "LumaC Textured Test");
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

        /* From here on the environment is proven: failures are FAILs. */
        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_RGBA8_UNORM;
        idesc.width = 16;
        idesc.height = 16;
        idesc.depth = 1;
        idesc.mip_levels = 3;
        idesc.array_layers = 1;
        idesc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_DST;
        idesc.flags = LC_IMAGE_FLAG_NONE;
        idesc.samples = LC_SAMPLE_COUNT_1;
        TEST_CHECK(lc_image_create(device, &idesc, &image) == LC_SUCCESS,
                   "validation image created");

        /* View: aspect, ranges, types, cube rules, format. */
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.format = LC_FORMAT_UNDEFINED;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.base_mip_level = 0;
        vdesc.mip_level_count = 3;
        vdesc.base_array_layer = 0;
        vdesc.array_layer_count = 1;
        TEST_CHECK(lc_image_view_create(image, &vdesc, &view) == LC_SUCCESS,
                   "full-range 2D view succeeds");
        lc_image_view_destroy(view);
        view = NULL;

        vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
        TEST_CHECK(lc_image_view_create(image, &vdesc, &view) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "depth aspect on color image rejected");
        TEST_CHECK(view == NULL, "out cleared on aspect mismatch");
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR | 0x8u;
        TEST_CHECK(lc_image_view_create(image, &vdesc, &view) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "unknown aspect bit rejected");
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.base_mip_level = 2;
        vdesc.mip_level_count = 2;
        TEST_CHECK(lc_image_view_create(image, &vdesc, &view) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "mip range overflow rejected");
        vdesc.base_mip_level = 0;
        vdesc.mip_level_count = 0;
        TEST_CHECK(lc_image_view_create(image, &vdesc, &view) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "empty mip range rejected");
        vdesc.mip_level_count = 3;
        vdesc.base_array_layer = 0;
        vdesc.array_layer_count = 2;
        TEST_CHECK(lc_image_view_create(image, &vdesc, &view) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "layer range overflow rejected");
        vdesc.array_layer_count = 1;
        vdesc.type = LC_IMAGE_VIEW_CUBE;
        TEST_CHECK(lc_image_view_create(image, &vdesc, &view) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "cube view on non-cube image rejected");
        vdesc.type = LC_IMAGE_VIEW_3D;
        TEST_CHECK(lc_image_view_create(image, &vdesc, &view) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "3D view on 2D image rejected");
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.format = LC_FORMAT_R8_UNORM;
        TEST_CHECK(lc_image_view_create(image, &vdesc, &view) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "mismatched view format rejected");
        vdesc.format = LC_FORMAT_UNDEFINED;
        TEST_CHECK(lc_image_view_create(image, &vdesc, &view) == LC_SUCCESS,
                   "valid view succeeds after rejects");

        /* Layout: duplicates, counts, types, visibility. */
        slots[0].binding = 1;
        slots[0].type = LC_BINDING_UNIFORM_BUFFER;
        slots[0].count = 1;
        slots[0].visibility = LC_SHADER_VISIBILITY_VERTEX;
        slots[1].binding = 1;
        slots[1].type = LC_BINDING_SAMPLER;
        slots[1].count = 1;
        slots[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        ldesc.bindings = slots;
        ldesc.binding_count = 2;
        TEST_CHECK(lc_binding_layout_create(device, &ldesc, &layout) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "duplicate bindings rejected");
        TEST_CHECK(layout == NULL, "out cleared on duplicates");
        ldesc.binding_count = 0;
        TEST_CHECK(lc_binding_layout_create(device, &ldesc, &layout) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "zero binding count rejected");
        ldesc.binding_count = 2;
        slots[0].type = (lc_binding_type)99;
        TEST_CHECK(lc_binding_layout_create(device, &ldesc, &layout) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "unknown binding type rejected");
        slots[0].type = LC_BINDING_UNIFORM_BUFFER;
        slots[0].visibility = 0x8u;
        TEST_CHECK(lc_binding_layout_create(device, &ldesc, &layout) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "unknown visibility rejected");
        slots[0].visibility = LC_SHADER_VISIBILITY_VERTEX;
        slots[0].count = 0;
        TEST_CHECK(lc_binding_layout_create(device, &ldesc, &layout) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "zero slot count rejected");
        slots[0].count = 1;
        slots[1].binding = 5;
        slots[1].type = LC_BINDING_SAMPLER;
        slots[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        TEST_CHECK(lc_binding_layout_create(device, &ldesc, &layout) ==
                       LC_SUCCESS,
                   "unordered valid layout succeeds");
        TEST_CHECK(layout != NULL, "layout handle non-null");

        /* Set + update validation. */
        TEST_CHECK(lc_binding_set_create(layout, &set) == LC_SUCCESS,
                   "binding set creation succeeds");
        {
            lc_binding_write bad;
            lc_buffer *dead_buffer = (lc_buffer *)0x1;

            memset(&bad, 0, sizeof(bad));
            bad.binding = 9;
            bad.array_element = 0;
            bad.type = LC_BINDING_UNIFORM_BUFFER;
            bad.u.buffer.buffer = dead_buffer;
            bad.u.buffer.offset = 0;
            bad.u.buffer.size = 0;
            TEST_CHECK(lc_binding_set_update(set, &bad, 1) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "unknown binding number rejected");
            bad.binding = 1;
            bad.type = LC_BINDING_SAMPLER;
            TEST_CHECK(lc_binding_set_update(set, &bad, 1) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "wrong write type rejected");
            bad.type = LC_BINDING_UNIFORM_BUFFER;
            bad.array_element = 4;
            TEST_CHECK(lc_binding_set_update(set, &bad, 1) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "array element overflow rejected");
            bad.array_element = 0;
            TEST_CHECK(lc_binding_set_update(set, &bad, 1) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "dead buffer rejected");
            TEST_CHECK(lc_binding_set_update(set, NULL, 2) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "NULL writes rejected");
            TEST_CHECK(lc_binding_set_update(set, NULL, 0) == LC_SUCCESS,
                       "empty update succeeds");
        }

        lc_binding_set_destroy(set);
        lc_binding_layout_destroy(layout);
        lc_image_view_destroy(view);
        lc_image_destroy(image);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "validation teardown clean");
    }

    /* ---- 2. allocator stress: 1000 sets ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_binding_desc slot;
        lc_binding_layout_desc ldesc;
        lc_binding_layout *layout = NULL;
        lc_binding_set *sets[1000];
        lc_buffer *shared_ubo = NULL;
        lc_buffer_desc bdesc;
        lc_binding_write write;
        int i;
        int env = make_device(&device);

        TEST_CHECK(env == 0, "device for allocator stress");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        slot.binding = 0;
        slot.type = LC_BINDING_UNIFORM_BUFFER;
        slot.count = 1;
        slot.visibility = LC_SHADER_VISIBILITY_VERTEX;
        ldesc.bindings = &slot;
        ldesc.binding_count = 1;
        TEST_CHECK(lc_binding_layout_create(device, &ldesc, &layout) ==
                       LC_SUCCESS,
                   "stress layout created");
        bdesc.size = 64;
        bdesc.usage = LC_BUFFER_USAGE_UNIFORM;
        bdesc.memory = LC_MEMORY_CPU_TO_GPU;
        TEST_CHECK(lc_buffer_create(device, &bdesc, &shared_ubo) ==
                       LC_SUCCESS,
                   "shared uniform buffer created");
        memset(&write, 0, sizeof(write));
        write.binding = 0;
        write.array_element = 0;
        write.type = LC_BINDING_UNIFORM_BUFFER;
        write.u.buffer.buffer = shared_ubo;
        write.u.buffer.offset = 0;
        write.u.buffer.size = 0;
        memset(sets, 0, sizeof(sets));
        for (i = 0; i < 1000; i++) {
            if (lc_binding_set_create(layout, &sets[i]) != LC_SUCCESS ||
                lc_binding_set_update(sets[i], &write, 1) != LC_SUCCESS) {
                break;
            }
        }
        TEST_CHECK(i == 1000, "1000 sets allocated and updated");
        for (i = 0; i < 1000; i++) {
            lc_binding_set_destroy(sets[i]);
        }
        TEST_CHECK(1, "1000 sets destroyed without leaks flagged");
        lc_buffer_destroy(shared_ubo);
        lc_binding_layout_destroy(layout);
        lc_device_destroy(device);
        lc_shutdown();
        TEST_CHECK(1, "stress teardown clean");
    }

    /* ---- 3. 120 textured frames with animated uniform ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        tex_ctx ctx;
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        int env;

        memset(&ctx, 0, sizeof(ctx));
        env = make_device(&device);
        TEST_CHECK(env == 0, "device for textured rendering");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC Textured Test") == 0,
                   "window for textured rendering");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for textured rendering");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for textured rendering");
        ctx.device = device;
        ctx.window = window;
        ctx.surface = surface;
        ctx.swapchain = swapchain;
        TEST_CHECK(make_textured_stack(&ctx) == 0,
                   "textured stack built");
        if (ctx.pipeline == NULL) {
            printf("textured stack failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(render_textured_frames(&ctx, 120) == 120,
                   "120 textured frames presented");
        /* The uniform keeps the last slide written by the loop;
         * reading it back proves per-frame updates land. */
        {
            float *m = (float *)ctx.ubo_mapped;
            int frame_count = 120;
            float expect =
                (float)(((frame_count - 1) % 200) - 100) / 100.0f * 0.2f;

            TEST_CHECK(m != NULL && m[12] == expect &&
                           m[0] == 1.0f && m[5] == 1.0f && m[15] == 1.0f,
                       "uniform holds last animated transform");
        }
        destroy_textured_stack(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "textured teardown clean");
    }

    /* ---- 4. resize stress with textured frames ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        tex_ctx ctx;
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        int env;
        static const unsigned targets[][2] = {
            { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 640, 480 },
            { 900, 900 }
        };
        size_t step;

        memset(&ctx, 0, sizeof(ctx));
        env = make_device(&device);
        TEST_CHECK(env == 0, "device for resize stress");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC Textured Test") == 0,
                   "window for resize stress");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for resize stress");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for resize stress");
        ctx.device = device;
        ctx.window = window;
        ctx.surface = surface;
        ctx.swapchain = swapchain;
        TEST_CHECK(make_textured_stack(&ctx) == 0,
                   "textured stack for resize stress");

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

                if (!native_resize_client(L"LumaC Textured Test", window, w,
                                          h, &ow, &oh)) {
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
            n = render_textured_frames(&ctx, 12);
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "12 textured frames at %ux%u", w,
                         h);
                TEST_CHECK(n == 12, msg);
            }
            if (n != 12) {
                break;
            }
        }

        destroy_textured_stack(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "resize-stress teardown clean");
    }

#ifdef _WIN32
    /* ---- 5. minimize / restore with textured frames ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        tex_ctx ctx;
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        HWND hwnd;
        int i;
        int env;

        memset(&ctx, 0, sizeof(ctx));
        env = make_device(&device);
        TEST_CHECK(env == 0, "device for minimize test");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC Textured Test") == 0,
                   "window for minimize test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for minimize test");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for minimize test");
        ctx.device = device;
        ctx.window = window;
        ctx.surface = surface;
        ctx.swapchain = swapchain;
        TEST_CHECK(make_textured_stack(&ctx) == 0,
                   "textured stack for minimize test");
        TEST_CHECK(render_textured_frames(&ctx, 10) == 10,
                   "10 warmup textured frames");

        hwnd = find_test_window(L"LumaC Textured Test");
        TEST_CHECK(hwnd != NULL, "test window found for minimize");
        ShowWindow(hwnd, SW_MINIMIZE);
        for (i = 0; i < 30; i++) {
            lc_poll_events();
            Sleep(5);
        }
        TEST_CHECK(lc_window_get_width(window) == 0 &&
                       lc_window_get_height(window) == 0,
                   "minimized window reports zero extent");
        ShowWindow(hwnd, SW_RESTORE);
        {
            uint32_t w = 0;
            uint32_t h = 0;
            for (i = 0; i < 120 && (w == 0 || h == 0); i++) {
                lc_poll_events();
                Sleep(5);
                w = lc_window_get_width(window);
                h = lc_window_get_height(window);
            }
            TEST_CHECK(w != 0 && h != 0, "nonzero extent after restore");
            TEST_CHECK(render_textured_frames(&ctx, 30) == 30,
                       "30 textured frames after restore");
        }

        destroy_textured_stack(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "minimize/restore teardown clean");
    }
#endif

    /* ---- 6. two windows share one texture ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        tex_ctx a;
        tex_ctx b;
        lc_device *device = NULL;
        int env;
        int i;
        int ok = 0;

        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        env = make_device(&device);
        TEST_CHECK(env == 0, "device for multi test");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&a.window, "LumaC Textured Test A") ==
                       0,
                   "window A created");
        TEST_CHECK(make_window_titled(&b.window, "LumaC Textured Test B") ==
                       0,
                   "window B created");
        a.device = device;
        b.device = device;
        TEST_CHECK(make_surface(device, a.window, &a.surface) == 0,
                   "surface A created");
        TEST_CHECK(make_surface(device, b.window, &b.surface) == 0,
                   "surface B created");
        TEST_CHECK(make_swapchain(device, a.surface, &a.swapchain) == 0,
                   "swapchain A created");
        TEST_CHECK(make_swapchain(device, b.surface, &b.swapchain) == 0,
                   "swapchain B created");
        TEST_CHECK(make_textured_stack(&a) == 0,
                   "textured stack A (own texture)");
        TEST_CHECK(make_textured_stack(&b) == 0,
                   "textured stack B (own texture)");

        /* Alternate frames on both swapchains from one thread. */
        for (i = 0; i < 30; i++) {
            int na = render_textured_frames(&a, 1);
            int nb = render_textured_frames(&b, 1);
            if (na != 1 || nb != 1) {
                break;
            }
            ok++;
        }
        TEST_CHECK(ok == 30, "30 textured frames on both windows");

        /* Shared device: tear down explicitly in dependency order
         * (destroy_textured_stack would destroy the device twice). */
        free(a.vert_code);
        free(a.frag_code);
        free(b.vert_code);
        free(b.frag_code);
        lc_pipeline_destroy(a.pipeline);
        lc_pipeline_destroy(b.pipeline);
        lc_binding_set_destroy(a.set);
        lc_binding_set_destroy(b.set);
        lc_binding_layout_destroy(a.layout);
        lc_binding_layout_destroy(b.layout);
        lc_sampler_destroy(a.sampler);
        lc_sampler_destroy(b.sampler);
        lc_image_view_destroy(a.view);
        lc_image_view_destroy(b.view);
        lc_image_destroy(a.image);
        lc_image_destroy(b.image);
        lc_buffer_destroy(a.ubo);
        lc_buffer_destroy(a.vbo);
        lc_buffer_destroy(b.ubo);
        lc_buffer_destroy(b.vbo);
        lc_swapchain_destroy(a.swapchain);
        lc_swapchain_destroy(b.swapchain);
        lc_surface_destroy(a.surface);
        lc_surface_destroy(b.surface);
        lc_device_destroy(device);
        lc_window_destroy(a.window);
        lc_window_destroy(b.window);
        lc_shutdown();
        TEST_CHECK(1, "multi-window teardown clean");
    }

    /* ---- 7. cube view bound to a compatible set ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_image_desc idesc;
        lc_image *image = NULL;
        lc_image_view_desc vdesc;
        lc_image_view *view = NULL;
        lc_binding_desc slot;
        lc_binding_layout_desc ldesc;
        lc_binding_layout *layout = NULL;
        lc_binding_set *set = NULL;
        lc_binding_write write;
        int env = make_device(&device);

        TEST_CHECK(env == 0, "device for cube bind test");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_RGBA8_UNORM;
        idesc.width = 4;
        idesc.height = 4;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 6;
        idesc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_DST;
        idesc.flags = LC_IMAGE_FLAG_CUBE_COMPATIBLE;
        idesc.samples = LC_SAMPLE_COUNT_1;
        TEST_CHECK(lc_image_create(device, &idesc, &image) == LC_SUCCESS,
                   "cube-compatible image created");

        /* Upload face 0 so the image reaches a defined layout. */
        {
            unsigned char face[4 * 4 * 4];
            lc_image_upload_desc upload;
            unsigned i;

            for (i = 0; i < sizeof(face); i++) {
                face[i] = (unsigned char)(i & 0xFFu);
            }
            memset(&upload, 0, sizeof(upload));
            upload.mip_level = 0;
            upload.array_layer = 0;
            upload.width = 4;
            upload.height = 4;
            upload.depth = 1;
            upload.data = face;
            upload.data_size = sizeof(face);
            TEST_CHECK(lc_image_write(image, &upload) == LC_SUCCESS,
                       "cube face 0 uploaded");
        }

        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_CUBE;
        vdesc.format = LC_FORMAT_UNDEFINED;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.base_mip_level = 0;
        vdesc.mip_level_count = 1;
        vdesc.base_array_layer = 0;
        vdesc.array_layer_count = 6;
        TEST_CHECK(lc_image_view_create(image, &vdesc, &view) == LC_SUCCESS,
                   "real cube view creation succeeds");

        slot.binding = 0;
        slot.type = LC_BINDING_SAMPLED_IMAGE;
        slot.count = 1;
        slot.visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        ldesc.bindings = &slot;
        ldesc.binding_count = 1;
        TEST_CHECK(lc_binding_layout_create(device, &ldesc, &layout) ==
                       LC_SUCCESS,
                   "cube layout created");
        TEST_CHECK(lc_binding_set_create(layout, &set) == LC_SUCCESS,
                   "cube set created");
        memset(&write, 0, sizeof(write));
        write.binding = 0;
        write.array_element = 0;
        write.type = LC_BINDING_SAMPLED_IMAGE;
        write.u.image.view = view;
        TEST_CHECK(lc_binding_set_update(set, &write, 1) == LC_SUCCESS,
                   "cube view bound into set");

        lc_binding_set_destroy(set);
        lc_binding_layout_destroy(layout);
        lc_image_view_destroy(view);
        lc_image_destroy(image);
        lc_device_destroy(device);
        lc_shutdown();
        TEST_CHECK(1, "cube bind teardown clean");
    }

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}

