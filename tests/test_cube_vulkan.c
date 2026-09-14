/*
 * Vulkan cube integration test (Phase 11).
 *
 * Real indexed/instanced drawing, push constants, depth clears, raster
 * state, and canonical binding signatures: bad pipeline/frame/push
 * validation, signature-compatible binds across layout recreation,
 * then instanced indexed-cube rendering (depth-tested, push-constant
 * perspective MVP) with resize stress. Run with validation layers.
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

static const uint16_t k_indices[36] = {
    0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11,
    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
};

static const float k_offsets[2][3] = {
    { -0.6f, 0.0f, 0.0f },
    { 0.6f, 0.0f, 0.0f },
};

#define TEX_W 16
#define TEX_H 16

static void fill_checker(unsigned char *texels) {
    uint32_t x;
    uint32_t y;

    for (y = 0; y < TEX_H; y++) {
        for (x = 0; x < TEX_W; x++) {
            unsigned char *px = &texels[(y * TEX_W + x) * 4];
            int white = (int)(((x / 4u) + (y / 4u)) % 2u);

            px[0] = white ? 220 : 40;
            px[1] = white ? 220 : 80;
            px[2] = white ? 220 : 140;
            px[3] = 255;
        }
    }
}

typedef struct cube_ctx {
    lc_device *device;
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
    lc_buffer *vbo;
    lc_buffer *ibo;
    lc_buffer *instance_bo;
    lc_image *image;
    lc_image_view *view;
    lc_sampler *sampler;
    lc_binding_layout *layout;
    lc_binding_set *set;
    lc_pipeline *pipeline;
    lc_shader *vs;
    lc_shader *fs;
    void *vert_code;
    size_t vert_size;
    void *frag_code;
    size_t frag_size;
} cube_ctx;

static void destroy_cube_stack(cube_ctx *ctx) {
    free(ctx->vert_code);
    free(ctx->frag_code);
    lc_pipeline_destroy(ctx->pipeline);
    lc_binding_set_destroy(ctx->set);
    lc_binding_layout_destroy(ctx->layout);
    lc_shader_destroy(ctx->vs);
    lc_shader_destroy(ctx->fs);
    lc_sampler_destroy(ctx->sampler);
    lc_image_view_destroy(ctx->view);
    lc_image_destroy(ctx->image);
    lc_buffer_destroy(ctx->instance_bo);
    lc_buffer_destroy(ctx->ibo);
    lc_buffer_destroy(ctx->vbo);
    lc_swapchain_destroy(ctx->swapchain);
    lc_surface_destroy(ctx->surface);
    lc_device_destroy(ctx->device);
    lc_window_destroy(ctx->window);
    memset(ctx, 0, sizeof(*ctx));
}

static int make_cube_stack(cube_ctx *ctx) {
    lc_buffer_desc bdesc;
    lc_image_desc idesc;
    lc_image_upload_desc upload;
    lc_image_view_desc vdesc;
    lc_sampler_desc smdesc;
    lc_binding_desc slots[2];
    lc_binding_layout_desc ldesc;
    lc_binding_write writes[2];
    lc_vertex_binding_desc vbindings[2];
    lc_vertex_attribute_desc vattrs[3];
    lc_push_constant_range push;
    lc_graphics_pipeline_desc pdesc = { 0 };
    lc_shader_desc sdesc;
    unsigned char texels[TEX_W * TEX_H * 4];

    if (!load_spv_file("cube.vert.spv", &ctx->vert_code, &ctx->vert_size) ||
        !load_spv_file("cube.frag.spv", &ctx->frag_code, &ctx->frag_size)) {
        printf("cube SPIR-V files missing: FAIL\n");
        return -1;
    }
    sdesc.stage = LC_SHADER_STAGE_VERTEX;
    sdesc.code = ctx->vert_code;
    sdesc.code_size = ctx->vert_size;
    sdesc.entry_point = NULL;
    if (lc_shader_create(ctx->device, &sdesc, &ctx->vs) != LC_SUCCESS) {
        return -1;
    }
    sdesc.stage = LC_SHADER_STAGE_FRAGMENT;
    sdesc.code = ctx->frag_code;
    sdesc.code_size = ctx->frag_size;
    if (lc_shader_create(ctx->device, &sdesc, &ctx->fs) != LC_SUCCESS) {
        lc_shader_destroy(ctx->vs);
        ctx->vs = NULL;
        return -1;
    }

    bdesc.size = sizeof(k_cube);
    bdesc.usage = LC_BUFFER_USAGE_VERTEX;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(ctx->device, &bdesc, &ctx->vbo) != LC_SUCCESS ||
        lc_buffer_write(ctx->vbo, 0, k_cube, sizeof(k_cube)) != LC_SUCCESS) {
        return -1;
    }
    bdesc.size = sizeof(k_indices);
    bdesc.usage = LC_BUFFER_USAGE_INDEX;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(ctx->device, &bdesc, &ctx->ibo) != LC_SUCCESS ||
        lc_buffer_write(ctx->ibo, 0, k_indices, sizeof(k_indices)) !=
            LC_SUCCESS) {
        return -1;
    }
    bdesc.size = sizeof(k_offsets);
    bdesc.usage = LC_BUFFER_USAGE_VERTEX;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(ctx->device, &bdesc, &ctx->instance_bo) !=
            LC_SUCCESS ||
        lc_buffer_write(ctx->instance_bo, 0, k_offsets,
                        sizeof(k_offsets)) != LC_SUCCESS) {
        return -1;
    }

    fill_checker(texels);
    memset(&idesc, 0, sizeof(idesc));
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
        return -1;
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
    if (lc_binding_layout_create(ctx->device, &ldesc, &ctx->layout) !=
        LC_SUCCESS) {
        return -1;
    }

    vbindings[0].binding = 0;
    vbindings[0].stride = sizeof(cube_vertex);
    vbindings[0].input_rate = LC_VERTEX_INPUT_PER_VERTEX;
    vbindings[1].binding = 1;
    vbindings[1].stride = sizeof(float) * 3u;
    vbindings[1].input_rate = LC_VERTEX_INPUT_PER_INSTANCE;
    vattrs[0].location = 0;
    vattrs[0].binding = 0;
    vattrs[0].format = LC_FORMAT_RGB32_FLOAT;
    vattrs[0].offset = 0;
    vattrs[1].location = 1;
    vattrs[1].binding = 0;
    vattrs[1].format = LC_FORMAT_RG32_FLOAT;
    vattrs[1].offset = sizeof(float) * 3u;
    vattrs[2].location = 2;
    vattrs[2].binding = 1;
    vattrs[2].format = LC_FORMAT_RGB32_FLOAT;
    vattrs[2].offset = 0;
    push.visibility = LC_SHADER_VISIBILITY_VERTEX;
    push.offset = 0;
    push.size = 64;
    pdesc.vertex_shader = ctx->vs;
    pdesc.fragment_shader = ctx->fs;
    pdesc.vertex_bindings = vbindings;
    pdesc.vertex_binding_count = 2;
    pdesc.vertex_attributes = vattrs;
    pdesc.vertex_attribute_count = 3;
    {
        const lc_binding_layout *slot_layouts[1];

        slot_layouts[0] = ctx->layout;
        pdesc.binding_layouts = slot_layouts;
        pdesc.binding_layout_count = 1;
        pdesc.cull_mode = LC_CULL_BACK;
        pdesc.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
        pdesc.depth_test_enable = 1;
        pdesc.depth_write_enable = 1;
        pdesc.push_constant_ranges = &push;
        pdesc.push_constant_range_count = 1;
        if (lc_swapchain_get_render_target_desc(ctx->swapchain,
                                                &pdesc.render_target) !=
            LC_SUCCESS) {
            return -1;
        }
        if (lc_graphics_pipeline_create(ctx->device, &pdesc,
                                        &ctx->pipeline) != LC_SUCCESS) {
            return -1;
        }
    }
    lc_shader_destroy(ctx->vs);
    lc_shader_destroy(ctx->fs);
    ctx->vs = NULL;
    ctx->fs = NULL;

    if (lc_binding_set_create(ctx->layout, &ctx->set) != LC_SUCCESS) {
        return -1;
    }
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_SAMPLED_IMAGE;
    writes[0].u.image.view = ctx->view;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLER;
    writes[1].u.sampler.sampler = ctx->sampler;
    if (lc_binding_set_update(ctx->set, writes, 2) != LC_SUCCESS) {
        return -1;
    }
    return 0;
}

/* Perspective MVP (Phase 12 fix): identity mirrored the authored
 * winding and clips nothing usefully (Phase 11 never pixel-verified
 * it). Vulkan needs w = -z with near -> 0 / far -> 1 plus the Y flip;
 * see examples/cube_3d for the canonical form. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void write_cube_mvp(float *mvp, float aspect, float angle) {
    float f = 1.0f / tanf((float)(45.0 * M_PI / 180.0) * 0.5f);
    float c = cosf(angle);
    float s = sinf(angle);
    float proj[16];
    float rot[16];
    float tmp[16];
    int col;
    int row;

    memset(proj, 0, sizeof(proj));
    proj[0] = f / aspect;
    proj[5] = -f;
    proj[10] = -100.0f / (100.0f - 0.1f);
    proj[11] = -1.0f;
    proj[14] = -(100.0f * 0.1f) / (100.0f - 0.1f);
    memset(rot, 0, sizeof(rot));
    rot[0] = c;
    rot[2] = -s;
    rot[5] = 1.0f;
    rot[8] = s;
    rot[10] = c;
    rot[15] = 1.0f;
    /* view_model = translate(0,0,-3) * rot: rotation columns pass
     * through, translation column becomes (0,0,-3,1). */
    for (col = 0; col < 3; col++) {
        for (row = 0; row < 4; row++) {
            tmp[col * 4 + row] = rot[col * 4 + row];
        }
    }
    tmp[12] = 0.0f;
    tmp[13] = 0.0f;
    tmp[14] = -3.0f;
    tmp[15] = 1.0f;
    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            mvp[col * 4 + row] = proj[0 * 4 + row] * tmp[col * 4 + 0] +
                                 proj[1 * 4 + row] * tmp[col * 4 + 1] +
                                 proj[2 * 4 + row] * tmp[col * 4 + 2] +
                                 proj[3 * 4 + row] * tmp[col * 4 + 3];
        }
    }
}

static int render_cube_frames(cube_ctx *ctx, int want) {
    int presented = 0;
    int guard = 0;
    float mvp[16];

    while (presented < want) {
        uint32_t w;
        uint32_t h;
        lc_result res;

        if (++guard > want * 25 + 120) {
            return -1;
        }
        lc_poll_events();
        w = lc_window_get_width(ctx->window);
        h = lc_window_get_height(ctx->window);
        if (w == 0 || h == 0) {
            continue;
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
        /* Perspective orbit (Phase 12: identity mirrored the authored
         * winding, rendering nothing visible). */
        write_cube_mvp(mvp, (h != 0) ? ((float)w / (float)h) : 1.0f,
                       (float)presented * 0.05f);
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
        if (lc_clear_color(ctx->swapchain, 0.06f, 0.07f, 0.10f, 1.0f) !=
                LC_SUCCESS ||
            lc_clear_depth(ctx->swapchain, 1.0f) != LC_SUCCESS ||
            lc_bind_pipeline(ctx->swapchain, ctx->pipeline) != LC_SUCCESS ||
            lc_bind_binding_set(ctx->swapchain, ctx->pipeline, 0, ctx->set) !=
                LC_SUCCESS ||
            lc_bind_vertex_buffer(ctx->swapchain, 0, ctx->vbo, 0) !=
                LC_SUCCESS ||
            lc_bind_vertex_buffer(ctx->swapchain, 1, ctx->instance_bo, 0) !=
                LC_SUCCESS ||
            lc_bind_index_buffer(ctx->swapchain, ctx->ibo, 0,
                                 LC_INDEX_UINT16) != LC_SUCCESS ||
            lc_push_constants(ctx->swapchain, ctx->pipeline,
                              LC_SHADER_VISIBILITY_VERTEX, 0, sizeof(mvp),
                              mvp) != LC_SUCCESS ||
            lc_draw_indexed(ctx->swapchain, 36, 2, 0, 0, 0) != LC_SUCCESS) {
            return -1;
        }
        res = lc_end_frame(ctx->swapchain);
        if (res == LC_SUCCESS) {
            presented++;
        } else if (res == LC_SUBOPTIMAL ||
                   res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            presented++;
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
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running LumaC cube (Phase 11) integration test...\n");

    lc_shutdown();
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }

    /* ---- 1. pipeline + frame validation on live objects ---- */
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_shader *vs = NULL;
        lc_shader *fs = NULL;
        lc_pipeline *pipeline = NULL;
        lc_buffer *vbo = NULL;
        lc_buffer *ibo = NULL;
        lc_buffer_desc bdesc;
        void *vert_code = NULL;
        void *frag_code = NULL;
        size_t vert_size = 0;
        size_t frag_size = 0;
        float mvp[16];
        int env;
        int i;

        for (i = 0; i < 16; i++) {
            mvp[i] = (i % 5 == 0) ? 1.0f : 0.0f;
        }

        env = make_device(&device);
        if (env != 0) {
            if (env == 1) {
                SKIP_ENV("a Vulkan device");
            }
            printf("device creation failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        env = make_window_titled(&window, "LumaC Cube Test");
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

        TEST_CHECK(lc_swapchain_get_depth_format(swapchain) !=
                       LC_FORMAT_UNDEFINED,
                   "swapchain owns a depth format");
        {
            /* Public-API-only depth check (shared builds hide
             * internal format helpers): depth formats are exactly
             * the D* enumerants. */
            lc_format df = lc_swapchain_get_depth_format(swapchain);

            TEST_CHECK(df == LC_FORMAT_D16_UNORM ||
                           df == LC_FORMAT_D24_UNORM_S8_UINT ||
                           df == LC_FORMAT_D32_FLOAT,
                       "depth format is a known depth format");
        }

        /* Pipeline validation uses triangle shaders (no descriptors, no
         * push usage, vertex-index generation) so layout/push mismatches
         * never reach Vulkan validation; cube shaders are exercised in
         * the rendering sections below with full descriptors. */
        if (!load_spv_file("triangle.vert.spv", &vert_code, &vert_size) ||
            !load_spv_file("triangle.frag.spv", &frag_code, &frag_size)) {
            printf("triangle SPIR-V files missing: FAIL\n");
            FAIL_SUMMARY();
        }
        {
            lc_shader_desc sdesc;

            sdesc.stage = LC_SHADER_STAGE_VERTEX;
            sdesc.code = vert_code;
            sdesc.code_size = vert_size;
            sdesc.entry_point = NULL;
            TEST_CHECK(lc_shader_create(device, &sdesc, &vs) == LC_SUCCESS,
                       "triangle vertex shader created");
            sdesc.stage = LC_SHADER_STAGE_FRAGMENT;
            sdesc.code = frag_code;
            sdesc.code_size = frag_size;
            TEST_CHECK(lc_shader_create(device, &sdesc, &fs) == LC_SUCCESS,
                       "triangle fragment shader created");
        }

        memset(&bdesc, 0, sizeof(bdesc));
        bdesc.size = sizeof(k_cube);
        bdesc.usage = LC_BUFFER_USAGE_VERTEX;
        bdesc.memory = LC_MEMORY_GPU_ONLY;
        TEST_CHECK(lc_buffer_create(device, &bdesc, &vbo) == LC_SUCCESS,
                   "cube vbo created");
        bdesc.size = sizeof(k_indices);
        bdesc.usage = LC_BUFFER_USAGE_INDEX;
        bdesc.memory = LC_MEMORY_GPU_ONLY;
        TEST_CHECK(lc_buffer_create(device, &bdesc, &ibo) == LC_SUCCESS,
                   "cube ibo created");

        /* Bad pipeline descriptors rejected (triangle shaders: empty
         * vertex input and no layouts keep Vulkan validation quiet).
         * Structural target first, so rejects come from the field
         * under test. */
        {
            lc_graphics_pipeline_desc pdesc = { 0 };
            lc_pipeline *bad = NULL;

            pdesc.vertex_shader = vs;
            pdesc.fragment_shader = fs;
            TEST_CHECK(lc_swapchain_get_render_target_desc(
                           swapchain, &pdesc.render_target) == LC_SUCCESS,
                       "swapchain target desc for validation");
            pdesc.cull_mode = (lc_cull_mode)99;
            TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc, &bad) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "bad cull mode rejected");
            TEST_CHECK(bad == NULL, "out cleared on bad cull");
            pdesc.cull_mode = LC_CULL_BACK;
            pdesc.front_face = (lc_front_face)99;
            TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc, &bad) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "bad front face rejected");
            pdesc.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
            {
                lc_push_constant_range push;

                push.visibility = 0;
                push.offset = 0;
                push.size = 64;
                pdesc.push_constant_ranges = &push;
                pdesc.push_constant_range_count = 1;
                TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc,
                                                       &bad) ==
                               LC_ERROR_INVALID_ARGUMENT,
                           "zero push visibility rejected");
                push.visibility = LC_SHADER_VISIBILITY_VERTEX;
                push.offset = 1;
                TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc,
                                                       &bad) ==
                               LC_ERROR_INVALID_ARGUMENT,
                           "misaligned push offset rejected");
                push.offset = 0;
                push.size = 0;
                TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc,
                                                       &bad) ==
                               LC_ERROR_INVALID_ARGUMENT,
                           "zero push size rejected");
                push.size = 64;
                push.visibility =
                    (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
                    (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT | 0x8u;
                TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc,
                                                       &bad) ==
                               LC_ERROR_INVALID_ARGUMENT,
                           "unknown push visibility rejected");
                pdesc.push_constant_ranges = NULL;
                pdesc.push_constant_range_count = 0;
            }
            /* Valid 3D-state pipeline on triangle shaders: culling,
             * depth, and an (unused but legal) push range. Unused push
             * ranges and depth state without depth usage are valid. */
            {
                lc_push_constant_range push;

                push.visibility = LC_SHADER_VISIBILITY_VERTEX;
                push.offset = 0;
                push.size = 64;
                pdesc.depth_test_enable = 1;
                pdesc.depth_write_enable = 1;
                pdesc.push_constant_ranges = &push;
                pdesc.push_constant_range_count = 1;
                TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc,
                                                       &pipeline) ==
                               LC_SUCCESS,
                           "valid 3D pipeline created");
            }
        }

        /* Frame misuse without an open frame. */
        TEST_CHECK(lc_bind_index_buffer(swapchain, ibo, 0,
                                        LC_INDEX_UINT16) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "bind_index with no frame rejected");
        TEST_CHECK(lc_draw_indexed(swapchain, 36, 1, 0, 0, 0) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "draw_indexed with no frame rejected");
        TEST_CHECK(lc_draw_instanced(swapchain, 3, 1, 0, 0) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "draw_instanced with no frame rejected");
        TEST_CHECK(lc_push_constants(swapchain, pipeline,
                                     LC_SHADER_VISIBILITY_VERTEX, 0, 64,
                                     mvp) == LC_ERROR_INVALID_ARGUMENT,
                   "push with no frame rejected");
        TEST_CHECK(lc_clear_depth(swapchain, 1.0f) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "clear_depth with no frame rejected");

        /* Bad index bindings rejected once a frame is open. */
        {
            lc_result res;
            lc_buffer_desc bad_desc;
            lc_buffer *novtx = NULL;

            TEST_CHECK(lc_begin_frame(swapchain) == LC_SUCCESS,
                       "frame begins for index validation");
            TEST_CHECK(lc_bind_index_buffer(swapchain, vbo, 0,
                                            LC_INDEX_UINT16) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "index bind on vertex-only buffer rejected");
            TEST_CHECK(lc_bind_index_buffer(swapchain, ibo, 1,
                                            LC_INDEX_UINT16) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "misaligned index offset rejected");
            TEST_CHECK(lc_bind_index_buffer(swapchain, ibo, 0,
                                            (lc_index_type)99) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "unknown index type rejected");
            memset(&bad_desc, 0, sizeof(bad_desc));
            bad_desc.size = 64;
            bad_desc.usage = LC_BUFFER_USAGE_UNIFORM;
            bad_desc.memory = LC_MEMORY_CPU_TO_GPU;
            if (lc_buffer_create(device, &bad_desc, &novtx) == LC_SUCCESS) {
                TEST_CHECK(lc_bind_index_buffer(swapchain, novtx, 0,
                                                LC_INDEX_UINT16) ==
                               LC_ERROR_INVALID_ARGUMENT,
                           "index bind on uniform buffer rejected");
                lc_buffer_destroy(novtx);
            }
            TEST_CHECK(lc_draw_indexed(swapchain, 36, 1, 0, 0, 0) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "indexed draw with no index bound rejected");
            TEST_CHECK(lc_draw_indexed(swapchain, 0, 1, 0, 0, 0) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "zero index count rejected");
            TEST_CHECK(lc_bind_index_buffer(swapchain, ibo, 0,
                                            LC_INDEX_UINT16) == LC_SUCCESS,
                       "valid index bind succeeds");
            /* No pipeline bound yet: draws still rejected. */
            TEST_CHECK(lc_draw_indexed(swapchain, 36, 1, 0, 0, 0) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "indexed draw with no pipeline rejected");
            TEST_CHECK(lc_draw_indexed(swapchain, 1000000u, 1, 0, 0, 0) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "oversized index range rejected at bind-draw check");
            /* Clear so the frame opens its render pass (color+depth) and
             * presents a defined image even though no draw succeeded. */
            TEST_CHECK(lc_clear_color(swapchain, 0.06f, 0.07f, 0.10f, 1.0f) ==
                           LC_SUCCESS,
                       "validation frame clears color");
            TEST_CHECK(lc_clear_depth(swapchain, 1.0f) == LC_SUCCESS,
                       "validation frame clears depth");
            res = lc_end_frame(swapchain);
            TEST_CHECK(res == LC_SUCCESS || res == LC_SUBOPTIMAL ||
                           res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE,
                       "index-validation frame ends clean");
            if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                uint32_t w = lc_window_get_width(window);
                uint32_t h = lc_window_get_height(window);
                lc_swapchain_recreate(swapchain, w, h);
            }
        }

        free(vert_code);
        free(frag_code);
        lc_pipeline_destroy(pipeline);
        lc_buffer_destroy(ibo);
        lc_buffer_destroy(vbo);
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "validation teardown clean");
    }

    /* ---- 2. canonical signatures survive layout recreation ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_binding_desc slots[2];
        lc_binding_layout_desc ldesc;
        lc_binding_layout *layout_a = NULL;
        lc_binding_layout *layout_b = NULL;
        lc_binding_set *set_b = NULL;
        lc_shader *vs = NULL;
        lc_shader *fs = NULL;
        lc_pipeline *pipeline = NULL;
        void *vert_code = NULL;
        void *frag_code = NULL;
        size_t vert_size = 0;
        size_t frag_size = 0;
        int env;

        env = make_device(&device);
        TEST_CHECK(env == 0, "device for signature test");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC Cube Test") == 0,
                   "window for signature test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for signature test");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for signature test");
        TEST_CHECK(load_spv_file("cube.vert.spv", &vert_code, &vert_size) &&
                       load_spv_file("cube.frag.spv", &frag_code, &frag_size),
                   "cube SPIR-V for signature test");

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
        TEST_CHECK(lc_binding_layout_create(device, &ldesc, &layout_a) ==
                       LC_SUCCESS,
                   "layout A created");
        {
            lc_shader_desc sdesc;
            lc_graphics_pipeline_desc pdesc = { 0 };
            const lc_binding_layout *slot_layouts[1];
            lc_vertex_binding_desc vbindings[2];
            lc_vertex_attribute_desc vattrs[3];
            lc_push_constant_range push;

            sdesc.stage = LC_SHADER_STAGE_VERTEX;
            sdesc.code = vert_code;
            sdesc.code_size = vert_size;
            sdesc.entry_point = NULL;
            TEST_CHECK(lc_shader_create(device, &sdesc, &vs) == LC_SUCCESS,
                       "sig vertex shader created");
            sdesc.stage = LC_SHADER_STAGE_FRAGMENT;
            sdesc.code = frag_code;
            sdesc.code_size = frag_size;
            TEST_CHECK(lc_shader_create(device, &sdesc, &fs) == LC_SUCCESS,
                       "sig fragment shader created");
            /* Full cube vertex layout + push so Vulkan validation sees
             * a shader-matching pipeline (bind-only frame needs no
             * vertex buffers). */
            vbindings[0].binding = 0;
            vbindings[0].stride = sizeof(cube_vertex);
            vbindings[0].input_rate = LC_VERTEX_INPUT_PER_VERTEX;
            vbindings[1].binding = 1;
            vbindings[1].stride = sizeof(float) * 3u;
            vbindings[1].input_rate = LC_VERTEX_INPUT_PER_INSTANCE;
            vattrs[0].location = 0;
            vattrs[0].binding = 0;
            vattrs[0].format = LC_FORMAT_RGB32_FLOAT;
            vattrs[0].offset = 0;
            vattrs[1].location = 1;
            vattrs[1].binding = 0;
            vattrs[1].format = LC_FORMAT_RG32_FLOAT;
            vattrs[1].offset = sizeof(float) * 3u;
            vattrs[2].location = 2;
            vattrs[2].binding = 1;
            vattrs[2].format = LC_FORMAT_RGB32_FLOAT;
            vattrs[2].offset = 0;
            push.visibility = LC_SHADER_VISIBILITY_VERTEX;
            push.offset = 0;
            push.size = 64;
            slot_layouts[0] = layout_a;
            pdesc.vertex_shader = vs;
            pdesc.fragment_shader = fs;
            pdesc.vertex_bindings = vbindings;
            pdesc.vertex_binding_count = 2;
            pdesc.vertex_attributes = vattrs;
            pdesc.vertex_attribute_count = 3;
            pdesc.binding_layouts = slot_layouts;
            pdesc.binding_layout_count = 1;
            pdesc.cull_mode = LC_CULL_BACK;
            pdesc.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
            pdesc.depth_test_enable = 1;
            pdesc.depth_write_enable = 1;
            pdesc.push_constant_ranges = &push;
            pdesc.push_constant_range_count = 1;
            TEST_CHECK(lc_swapchain_get_render_target_desc(
                           swapchain, &pdesc.render_target) == LC_SUCCESS,
                       "swapchain target desc for sig pipeline");
            TEST_CHECK(lc_graphics_pipeline_create(device, &pdesc,
                                                   &pipeline) == LC_SUCCESS,
                       "pipeline on layout A created");
        }
        /* Destroy A, recreate B with identical contents, bind B's set
         * against A's pipeline: content match must succeed. */
        lc_binding_layout_destroy(layout_a);
        layout_a = NULL;
        TEST_CHECK(lc_binding_layout_create(device, &ldesc, &layout_b) ==
                       LC_SUCCESS,
                   "layout B (same signature) created");
        TEST_CHECK(lc_binding_set_create(layout_b, &set_b) == LC_SUCCESS,
                   "set on layout B created");
        {
            lc_image_desc idesc;
            lc_image *img = NULL;
            lc_image_view_desc vdesc;
            lc_image_view *view = NULL;
            lc_sampler_desc smdesc;
            lc_sampler *samp = NULL;
            lc_image_upload_desc upload;
            unsigned char tex[4 * 4 * 4];
            lc_binding_write writes[2];
            unsigned k;

            for (k = 0; k < sizeof(tex); k++) {
                tex[k] = (unsigned char)k;
            }
            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = 4;
            idesc.height = 4;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                          LC_IMAGE_USAGE_TRANSFER_DST;
            idesc.flags = LC_IMAGE_FLAG_NONE;
            idesc.samples = LC_SAMPLE_COUNT_1;
            TEST_CHECK(lc_image_create(device, &idesc, &img) == LC_SUCCESS,
                       "sig image created");
            memset(&upload, 0, sizeof(upload));
            upload.mip_level = 0;
            upload.array_layer = 0;
            upload.width = 4;
            upload.height = 4;
            upload.depth = 1;
            upload.data = tex;
            upload.data_size = sizeof(tex);
            TEST_CHECK(lc_image_write(img, &upload) == LC_SUCCESS,
                       "sig image uploaded");
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.format = LC_FORMAT_UNDEFINED;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.base_mip_level = 0;
            vdesc.mip_level_count = 1;
            vdesc.base_array_layer = 0;
            vdesc.array_layer_count = 1;
            TEST_CHECK(lc_image_view_create(img, &vdesc, &view) == LC_SUCCESS,
                       "sig view created");
            memset(&smdesc, 0, sizeof(smdesc));
            smdesc.min_filter = LC_FILTER_NEAREST;
            smdesc.mag_filter = LC_FILTER_NEAREST;
            smdesc.mipmap_mode = LC_MIPMAP_MODE_NEAREST;
            smdesc.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
            smdesc.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
            smdesc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
            smdesc.min_lod = 0.0f;
            smdesc.max_lod = 0.0f;
            smdesc.max_anisotropy = 1.0f;
            TEST_CHECK(lc_sampler_create(device, &smdesc, &samp) ==
                           LC_SUCCESS,
                       "sig sampler created");
            writes[0].binding = 0;
            writes[0].array_element = 0;
            writes[0].type = LC_BINDING_SAMPLED_IMAGE;
            writes[0].u.image.view = view;
            writes[1].binding = 1;
            writes[1].array_element = 0;
            writes[1].type = LC_BINDING_SAMPLER;
            writes[1].u.sampler.sampler = samp;
            TEST_CHECK(lc_binding_set_update(set_b, writes, 2) == LC_SUCCESS,
                       "sig set updated");
            TEST_CHECK(lc_begin_frame(swapchain) == LC_SUCCESS,
                       "sig frame begins");
            TEST_CHECK(lc_bind_pipeline(swapchain, pipeline) == LC_SUCCESS,
                       "sig pipeline binds");
            TEST_CHECK(lc_bind_binding_set(swapchain, pipeline, 0, set_b) ==
                           LC_SUCCESS,
                       "signature-compatible bind succeeds after recreate");
            {
                lc_result res = lc_end_frame(swapchain);

                TEST_CHECK(res == LC_SUCCESS || res == LC_SUBOPTIMAL ||
                               res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE,
                           "sig frame ends clean");
                if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                    uint32_t w = lc_window_get_width(window);
                    uint32_t h = lc_window_get_height(window);
                    lc_swapchain_recreate(swapchain, w, h);
                }
            }
            lc_sampler_destroy(samp);
            lc_image_view_destroy(view);
            lc_image_destroy(img);
        }
        free(vert_code);
        free(frag_code);
        lc_binding_set_destroy(set_b);
        lc_binding_layout_destroy(layout_b);
        lc_pipeline_destroy(pipeline);
        lc_shader_destroy(vs);
        lc_shader_destroy(fs);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "signature teardown clean");
    }

    /* ---- 3. 60 instanced indexed-cube frames ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        cube_ctx ctx;
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        int env;

        memset(&ctx, 0, sizeof(ctx));
        env = make_device(&device);
        TEST_CHECK(env == 0, "device for cube rendering");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC Cube Test") == 0,
                   "window for cube rendering");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for cube rendering");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for cube rendering");
        ctx.device = device;
        ctx.window = window;
        ctx.surface = surface;
        ctx.swapchain = swapchain;
        TEST_CHECK(make_cube_stack(&ctx) == 0, "cube stack built");
        if (ctx.pipeline == NULL) {
            printf("cube stack failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(render_cube_frames(&ctx, 60) == 60,
                   "60 instanced indexed-cube frames presented");
        /* Push-constants proof: rewrite the same identity MVP and draw
         * once more; success means the push path stays valid. */
        {
            float mvp[16];
            int k;

            for (k = 0; k < 16; k++) {
                mvp[k] = (k % 5 == 0) ? 1.0f : 0.0f;
            }
            TEST_CHECK(render_cube_frames(&ctx, 1) == 1,
                       "push-constant frame after 60 succeeds");
            (void)mvp;
        }
        destroy_cube_stack(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "cube teardown clean");
    }

    /* ---- 4. resize stress with cube frames ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        cube_ctx ctx;
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        int env;
        static const unsigned targets[][2] = {
            { 800, 600 }, { 1024, 768 }, { 640, 480 },
        };
        size_t step;

        memset(&ctx, 0, sizeof(ctx));
        env = make_device(&device);
        TEST_CHECK(env == 0, "device for resize stress");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC Cube Test") == 0,
                   "window for resize stress");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for resize stress");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for resize stress");
        ctx.device = device;
        ctx.window = window;
        ctx.surface = surface;
        ctx.swapchain = swapchain;
        TEST_CHECK(make_cube_stack(&ctx) == 0,
                   "cube stack for resize stress");
        for (step = 0; step < sizeof(targets) / sizeof(targets[0]); step++) {
            unsigned w = targets[step][0];
            unsigned h = targets[step][1];
            int n;

            if (lc_swapchain_recreate(ctx.swapchain, w, h) != LC_SUCCESS) {
                char msg[96];

                snprintf(msg, sizeof(msg), "recreate to %ux%u succeeds", w,
                         h);
                TEST_CHECK(0, msg);
                break;
            }
            n = render_cube_frames(&ctx, 8);
            {
                char msg[96];

                snprintf(msg, sizeof(msg), "8 cube frames at %ux%u", w, h);
                TEST_CHECK(n == 8, msg);
            }
            if (n != 8) {
                break;
            }
        }
        destroy_cube_stack(&ctx);
        lc_shutdown();
        TEST_CHECK(1, "resize-stress teardown clean");
    }

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
