#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <lumac/lumac.h>

/*
 * Phase 12 example: render-to-texture in two passes through one
 * command encoder.
 *
 * Pass 1 renders a rotating textured cube (indexed, push-constant
 * MVP, depth-tested) into a fixed 512x512 offscreen target.
 *
 * Pass 2 samples that offscreen color image on a fullscreen triangle
 * presented to the swapchain — the editor-viewport primitive: scene
 * into a texture, texture into UI.
 *
 * The offscreen target never touches swapchain recreation: resizing
 * the window rebuilds only presentation resources.
 */

#ifndef LC_TRIANGLE_SPV_DIR
#define LC_TRIANGLE_SPV_DIR "."
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TEX_W 64
#define TEX_H 64
#define OFF_W 512
#define OFF_H 512

typedef struct cube_vertex {
    float position[3];
    float uv[2];
} cube_vertex;

static const cube_vertex k_cube_vertices[24] = {
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

static const uint16_t k_cube_indices[36] = {
    0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11,
    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23,
};

static const float k_zero_offset[3] = { 0.0f, 0.0f, 0.0f };

static unsigned char s_texels[TEX_W * TEX_H * 4];

static void fill_checkerboard(void) {
    uint32_t x;
    uint32_t y;

    for (y = 0; y < TEX_H; y++) {
        for (x = 0; x < TEX_W; x++) {
            unsigned char *px = &s_texels[(y * TEX_W + x) * 4];
            int white = (int)(((x / 8u) + (y / 8u)) % 2u);

            px[0] = white ? 235 : 30;
            px[1] = white ? 235 : 90;
            px[2] = white ? 235 : 160;
            px[3] = 255;
        }
    }
}

static int load_spv(const char *path, void **out_code, size_t *out_size) {
    FILE *file = NULL;
    long length = 0;
    void *code = NULL;
    size_t got = 0;

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

static int load_shader_file(const char *name, void **out_code,
                            size_t *out_size) {
    char path[260];
    int written = snprintf(path, sizeof(path), "%s/%s", LC_TRIANGLE_SPV_DIR,
                           name);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return 0;
    }
    return load_spv(path, out_code, out_size);
}

static void mat4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void mat4_multiply(float *out, const float *a, const float *b) {
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

static void mat4_perspective_vulkan(float *m, float fov_y_rad, float aspect,
                                    float near_z, float far_z) {
    float f = 1.0f / tanf(fov_y_rad * 0.5f);

    /* Camera looks along -Z (view space z < 0 in front). Clip w must
     * equal -z (positive in front), so the W row is (0,0,-1,0); the Z
     * row maps near -> 0 and far -> 1 for Vulkan depth. Y is flipped
     * for the Vulkan framebuffer convention. */
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = -f;
    m[10] = -far_z / (far_z - near_z);
    m[11] = -1.0f;
    m[14] = -(far_z * near_z) / (far_z - near_z);
}

static void mat4_translate(float *m, float x, float y, float z) {
    mat4_identity(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

static void mat4_rotate_y(float *m, float angle) {
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

static void mat4_rotate_x(float *m, float angle) {
    float c = cosf(angle);
    float s = sinf(angle);

    memset(m, 0, 16 * sizeof(float));
    m[0] = 1.0f;
    m[5] = c;
    m[6] = s;
    m[9] = -s;
    m[10] = c;
    m[15] = 1.0f;
}

static void write_cube_mvp(float *mvp, float aspect, float angle) {
    float proj[16];
    float view[16];
    float rot_y[16];
    float rot_x[16];
    float model[16];
    float view_model[16];

    mat4_perspective_vulkan(proj, (float)(45.0 * M_PI / 180.0), aspect, 0.1f,
                            100.0f);
    mat4_translate(view, 0.0f, 0.0f, -3.0f);
    mat4_rotate_y(rot_y, angle);
    mat4_rotate_x(rot_x, angle * 0.5f);
    mat4_multiply(model, rot_y, rot_x);
    mat4_multiply(view_model, view, model);
    mat4_multiply(mvp, proj, view_model);
}

#define FAIL_CLEANUP(msg, res)                                            \
    do {                                                                  \
        fprintf(stderr, "%s failed (%d)\n", msg, res);                    \
        goto cleanup;                                                     \
    } while (0)

int main(void) {
    lc_window *window = NULL;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lc_buffer *vertex_buffer = NULL;
    lc_buffer *index_buffer = NULL;
    lc_buffer *instance_buffer = NULL;
    lc_image *texture = NULL;
    lc_image_view *texture_view = NULL;
    lc_sampler *texture_sampler = NULL;
    lc_image *off_color = NULL;
    lc_image *off_depth = NULL;
    lc_image_view *off_color_view = NULL;
    lc_image_view *off_depth_view = NULL;
    lc_render_target *offscreen = NULL;
    lc_sampler *off_sampler = NULL;
    lc_shader *cube_vs = NULL;
    lc_shader *cube_fs = NULL;
    lc_shader *quad_vs = NULL;
    lc_shader *quad_fs = NULL;
    lc_binding_layout *tex_layout = NULL;
    lc_binding_layout *off_layout = NULL;
    lc_binding_set *tex_set = NULL;
    lc_binding_set *off_set = NULL;
    lc_pipeline *cube_pipeline = NULL;
    lc_pipeline *quad_pipeline = NULL;
    void *code = NULL;
    size_t code_size = 0;
    lc_window_desc window_desc;
    lc_device_desc device_desc = { 0 };
    lc_swapchain_desc swapchain_desc = { 0 };
    lc_result res = LC_SUCCESS;
    unsigned long frame = 0;
    int exit_code = 1;

    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    window_desc.title = "LumaC Render To Texture";
    window_desc.width = 800;
    window_desc.height = 600;
    if (lc_window_create(&window_desc, &window) != LC_SUCCESS) {
        fprintf(stderr, "lc_window_create failed\n");
        lc_shutdown();
        return 1;
    }

    device_desc.backend = LC_BACKEND_VULKAN;
    device_desc.enable_validation = 1;
    if (lc_device_create(&device_desc, &device) != LC_SUCCESS) {
        fprintf(stderr, "lc_device_create failed\n");
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }

    if (lc_surface_create(device, window, &surface) != LC_SUCCESS) {
        fprintf(stderr, "lc_surface_create failed\n");
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }

    swapchain_desc.width = lc_window_get_width(window);
    swapchain_desc.height = lc_window_get_height(window);
    swapchain_desc.image_count = 0;
    swapchain_desc.vsync = 1;
    if (lc_swapchain_create(device, surface, &swapchain_desc, &swapchain) !=
        LC_SUCCESS) {
        fprintf(stderr, "lc_swapchain_create failed\n");
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }

    /* Cube mesh + centered instance buffer (GPU-only staging). */
    {
        lc_buffer_desc buffer_desc;

        buffer_desc.size = sizeof(k_cube_vertices);
        buffer_desc.usage = LC_BUFFER_USAGE_VERTEX;
        buffer_desc.memory = LC_MEMORY_GPU_ONLY;
        res = lc_buffer_create(device, &buffer_desc, &vertex_buffer);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_create(vertex)", res);
        }
        res = lc_buffer_write(vertex_buffer, 0, k_cube_vertices,
                              sizeof(k_cube_vertices));
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_write(vertex)", res);
        }

        buffer_desc.size = sizeof(k_cube_indices);
        buffer_desc.usage = LC_BUFFER_USAGE_INDEX;
        buffer_desc.memory = LC_MEMORY_GPU_ONLY;
        res = lc_buffer_create(device, &buffer_desc, &index_buffer);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_create(index)", res);
        }
        res = lc_buffer_write(index_buffer, 0, k_cube_indices,
                              sizeof(k_cube_indices));
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_write(index)", res);
        }

        buffer_desc.size = sizeof(k_zero_offset);
        buffer_desc.usage = LC_BUFFER_USAGE_VERTEX;
        buffer_desc.memory = LC_MEMORY_GPU_ONLY;
        res = lc_buffer_create(device, &buffer_desc, &instance_buffer);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_create(instance)", res);
        }
        res = lc_buffer_write(instance_buffer, 0, k_zero_offset,
                              sizeof(k_zero_offset));
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_write(instance)", res);
        }
    }

    /* Cube texture (checkerboard + mipmaps). */
    fill_checkerboard();
    {
        lc_image_desc image_desc;
        lc_image_upload_desc upload;
        lc_image_view_desc view_desc;
        lc_sampler_desc sampler_desc;
        lc_device_limits limits;

        image_desc.type = LC_IMAGE_TYPE_2D;
        image_desc.format = LC_FORMAT_RGBA8_UNORM;
        image_desc.width = TEX_W;
        image_desc.height = TEX_H;
        image_desc.depth = 1;
        image_desc.mip_levels = 0;
        image_desc.array_layers = 1;
        image_desc.usage = LC_IMAGE_USAGE_SAMPLED |
                           LC_IMAGE_USAGE_TRANSFER_SRC |
                           LC_IMAGE_USAGE_TRANSFER_DST;
        image_desc.flags = LC_IMAGE_FLAG_NONE;
        image_desc.samples = LC_SAMPLE_COUNT_1;
        res = lc_image_create(device, &image_desc, &texture);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_create(texture)", res);
        }
        upload.mip_level = 0;
        upload.array_layer = 0;
        upload.width = TEX_W;
        upload.height = TEX_H;
        upload.depth = 1;
        upload.data = s_texels;
        upload.data_size = sizeof(s_texels);
        res = lc_image_write(texture, &upload);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_write(texture)", res);
        }
        res = lc_image_generate_mipmaps(texture);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_generate_mipmaps", res);
        }
        view_desc.type = LC_IMAGE_VIEW_2D;
        view_desc.format = LC_FORMAT_UNDEFINED;
        view_desc.aspect = LC_IMAGE_ASPECT_COLOR;
        view_desc.base_mip_level = 0;
        view_desc.mip_level_count = lc_image_get_mip_levels(texture);
        view_desc.base_array_layer = 0;
        view_desc.array_layer_count = 1;
        res = lc_image_view_create(texture, &view_desc, &texture_view);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_view_create(texture)", res);
        }
        lc_device_get_limits(device, &limits);
        sampler_desc.min_filter = LC_FILTER_LINEAR;
        sampler_desc.mag_filter = LC_FILTER_LINEAR;
        sampler_desc.mipmap_mode = LC_MIPMAP_MODE_LINEAR;
        sampler_desc.address_u = LC_ADDRESS_REPEAT;
        sampler_desc.address_v = LC_ADDRESS_REPEAT;
        sampler_desc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
        sampler_desc.mip_lod_bias = 0.0f;
        sampler_desc.min_lod = 0.0f;
        sampler_desc.max_lod = 8.0f;
        sampler_desc.max_anisotropy = limits.max_sampler_anisotropy;
        res = lc_sampler_create(device, &sampler_desc, &texture_sampler);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_sampler_create(texture)", res);
        }
    }

    /* Fixed-size offscreen target (survives swapchain recreation). */
    {
        lc_image_desc image_desc;
        lc_image_view_desc view_desc;
        lc_render_target_create_desc target_desc;
        lc_render_target_attachment color_att;
        lc_sampler_desc sampler_desc;

        image_desc.type = LC_IMAGE_TYPE_2D;
        image_desc.format = LC_FORMAT_RGBA8_UNORM;
        image_desc.width = OFF_W;
        image_desc.height = OFF_H;
        image_desc.depth = 1;
        image_desc.mip_levels = 1;
        image_desc.array_layers = 1;
        image_desc.usage = LC_IMAGE_USAGE_SAMPLED |
                           LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                           LC_IMAGE_USAGE_TRANSFER_SRC |
                           LC_IMAGE_USAGE_TRANSFER_DST;
        image_desc.flags = LC_IMAGE_FLAG_NONE;
        image_desc.samples = LC_SAMPLE_COUNT_1;
        res = lc_image_create(device, &image_desc, &off_color);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_create(offscreen color)", res);
        }
        image_desc.format = LC_FORMAT_D32_FLOAT;
        image_desc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL;
        res = lc_image_create(device, &image_desc, &off_depth);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_create(offscreen depth)", res);
        }
        view_desc.type = LC_IMAGE_VIEW_2D;
        view_desc.format = LC_FORMAT_UNDEFINED;
        view_desc.aspect = LC_IMAGE_ASPECT_COLOR;
        view_desc.base_mip_level = 0;
        view_desc.mip_level_count = 1;
        view_desc.base_array_layer = 0;
        view_desc.array_layer_count = 1;
        res = lc_image_view_create(off_color, &view_desc, &off_color_view);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_view_create(offscreen color)", res);
        }
        view_desc.aspect = LC_IMAGE_ASPECT_DEPTH;
        res = lc_image_view_create(off_depth, &view_desc, &off_depth_view);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_view_create(offscreen depth)", res);
        }
        /* Prime the color image sampled-readable before the sampling
         * set points at it (binding validates sampled state; the
         * first CLEAR pass discards the primer). */
        {
            lc_image_upload_desc primer;
            void *zeros = calloc(1, (size_t)OFF_W * OFF_H * 4u);

            if (zeros == NULL) {
                FAIL_CLEANUP("primer alloc", LC_ERROR_OUT_OF_MEMORY);
            }
            primer.mip_level = 0;
            primer.array_layer = 0;
            primer.width = OFF_W;
            primer.height = OFF_H;
            primer.depth = 1;
            primer.data = zeros;
            primer.data_size = (uint64_t)OFF_W * OFF_H * 4u;
            res = lc_image_write(off_color, &primer);
            free(zeros);
            if (res != LC_SUCCESS) {
                FAIL_CLEANUP("lc_image_write(offscreen primer)", res);
            }
        }
        color_att.view = off_color_view;
        target_desc.width = OFF_W;
        target_desc.height = OFF_H;
        target_desc.color_attachments = &color_att;
        target_desc.color_attachment_count = 1;
        target_desc.depth_stencil_attachment = off_depth_view;
        res = lc_render_target_create(device, &target_desc, &offscreen);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_render_target_create", res);
        }
        sampler_desc.min_filter = LC_FILTER_LINEAR;
        sampler_desc.mag_filter = LC_FILTER_LINEAR;
        sampler_desc.mipmap_mode = LC_MIPMAP_MODE_NEAREST;
        sampler_desc.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
        sampler_desc.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
        sampler_desc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
        sampler_desc.mip_lod_bias = 0.0f;
        sampler_desc.min_lod = 0.0f;
        sampler_desc.max_lod = 0.0f;
        sampler_desc.max_anisotropy = 1.0f;
        res = lc_sampler_create(device, &sampler_desc, &off_sampler);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_sampler_create(offscreen)", res);
        }
    }

    /* Pass 1 pipeline: cube into the offscreen signature. */
    if (!load_shader_file("cube.vert.spv", &code, &code_size)) {
        fprintf(stderr, "failed to load cube.vert.spv\n");
        goto cleanup;
    }
    {
        lc_shader_desc shader_desc;

        shader_desc.stage = LC_SHADER_STAGE_VERTEX;
        shader_desc.code = code;
        shader_desc.code_size = code_size;
        shader_desc.entry_point = NULL;
        res = lc_shader_create(device, &shader_desc, &cube_vs);
        free(code);
        code = NULL;
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("cube vertex lc_shader_create", res);
        }
    }
    if (!load_shader_file("cube.frag.spv", &code, &code_size)) {
        fprintf(stderr, "failed to load cube.frag.spv\n");
        goto cleanup;
    }
    {
        lc_shader_desc shader_desc;
        lc_graphics_pipeline_desc pipeline_desc = { 0 };
        lc_binding_desc bindings[2];
        lc_binding_layout_desc layout_desc;
        lc_binding_write writes[2];
        lc_vertex_binding_desc vbindings[2];
        lc_vertex_attribute_desc vattrs[3];
        lc_push_constant_range push_range;

        shader_desc.stage = LC_SHADER_STAGE_FRAGMENT;
        shader_desc.code = code;
        shader_desc.code_size = code_size;
        shader_desc.entry_point = NULL;
        res = lc_shader_create(device, &shader_desc, &cube_fs);
        free(code);
        code = NULL;
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("cube fragment lc_shader_create", res);
        }

        bindings[0].binding = 0;
        bindings[0].type = LC_BINDING_SAMPLED_IMAGE;
        bindings[0].count = 1;
        bindings[0].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        bindings[1].binding = 1;
        bindings[1].type = LC_BINDING_SAMPLER;
        bindings[1].count = 1;
        bindings[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        layout_desc.bindings = bindings;
        layout_desc.binding_count = 2;
        res = lc_binding_layout_create(device, &layout_desc, &tex_layout);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_binding_layout_create(cube)", res);
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
        push_range.visibility = LC_SHADER_VISIBILITY_VERTEX;
        push_range.offset = 0;
        push_range.size = 64;

        pipeline_desc.vertex_shader = cube_vs;
        pipeline_desc.fragment_shader = cube_fs;
        pipeline_desc.vertex_bindings = vbindings;
        pipeline_desc.vertex_binding_count = 2;
        pipeline_desc.vertex_attributes = vattrs;
        pipeline_desc.vertex_attribute_count = 3;
        {
            const lc_binding_layout *slots[1];

            slots[0] = tex_layout;
            pipeline_desc.binding_layouts = slots;
            pipeline_desc.binding_layout_count = 1;
            pipeline_desc.cull_mode = LC_CULL_BACK;
            pipeline_desc.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
            pipeline_desc.depth_test_enable = 1;
            pipeline_desc.depth_write_enable = 1;
            pipeline_desc.push_constant_ranges = &push_range;
            pipeline_desc.push_constant_range_count = 1;
            pipeline_desc.render_target.color_attachment_count = 1;
            pipeline_desc.render_target.color_formats[0] =
                LC_FORMAT_RGBA8_UNORM;
            pipeline_desc.render_target.depth_stencil_format =
                LC_FORMAT_D32_FLOAT;
            pipeline_desc.render_target.samples = LC_SAMPLE_COUNT_1;
            res = lc_graphics_pipeline_create(device, &pipeline_desc,
                                              &cube_pipeline);
            if (res != LC_SUCCESS) {
                FAIL_CLEANUP("lc_graphics_pipeline_create(cube)", res);
            }
        }
        lc_shader_destroy(cube_fs);
        lc_shader_destroy(cube_vs);
        cube_fs = NULL;
        cube_vs = NULL;

        res = lc_binding_set_create(tex_layout, &tex_set);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_binding_set_create(cube)", res);
        }
        writes[0].binding = 0;
        writes[0].array_element = 0;
        writes[0].type = LC_BINDING_SAMPLED_IMAGE;
        writes[0].u.image.view = texture_view;
        writes[1].binding = 1;
        writes[1].array_element = 0;
        writes[1].type = LC_BINDING_SAMPLER;
        writes[1].u.sampler.sampler = texture_sampler;
        res = lc_binding_set_update(tex_set, writes, 2);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_binding_set_update(cube)", res);
        }
    }

    /* Pass 2 pipeline: fullscreen sampling quad to the swapchain. */
    if (!load_shader_file("quad.vert.spv", &code, &code_size)) {
        fprintf(stderr, "failed to load quad.vert.spv\n");
        goto cleanup;
    }
    {
        lc_shader_desc shader_desc;

        shader_desc.stage = LC_SHADER_STAGE_VERTEX;
        shader_desc.code = code;
        shader_desc.code_size = code_size;
        shader_desc.entry_point = NULL;
        res = lc_shader_create(device, &shader_desc, &quad_vs);
        free(code);
        code = NULL;
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("quad vertex lc_shader_create", res);
        }
    }
    if (!load_shader_file("quad.frag.spv", &code, &code_size)) {
        fprintf(stderr, "failed to load quad.frag.spv\n");
        goto cleanup;
    }
    {
        lc_shader_desc shader_desc;
        lc_graphics_pipeline_desc pipeline_desc = { 0 };
        lc_binding_desc bindings[2];
        lc_binding_layout_desc layout_desc;
        lc_binding_write writes[2];

        shader_desc.stage = LC_SHADER_STAGE_FRAGMENT;
        shader_desc.code = code;
        shader_desc.code_size = code_size;
        shader_desc.entry_point = NULL;
        res = lc_shader_create(device, &shader_desc, &quad_fs);
        free(code);
        code = NULL;
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("quad fragment lc_shader_create", res);
        }

        bindings[0].binding = 0;
        bindings[0].type = LC_BINDING_SAMPLED_IMAGE;
        bindings[0].count = 1;
        bindings[0].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        bindings[1].binding = 1;
        bindings[1].type = LC_BINDING_SAMPLER;
        bindings[1].count = 1;
        bindings[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        layout_desc.bindings = bindings;
        layout_desc.binding_count = 2;
        res = lc_binding_layout_create(device, &layout_desc, &off_layout);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_binding_layout_create(quad)", res);
        }

        pipeline_desc.vertex_shader = quad_vs;
        pipeline_desc.fragment_shader = quad_fs;
        {
            const lc_binding_layout *slots[1];

            slots[0] = off_layout;
            pipeline_desc.binding_layouts = slots;
            pipeline_desc.binding_layout_count = 1;
            /* No culling, no depth test: 2D composite. The target
             * still carries depth (swapchain recipe); the attachment
             * exists but this pipeline ignores it. */
            pipeline_desc.render_target.color_attachment_count = 1;
            pipeline_desc.render_target.color_formats[0] =
                lc_swapchain_get_format(swapchain);
            pipeline_desc.render_target.depth_stencil_format =
                lc_swapchain_get_depth_format(swapchain);
            pipeline_desc.render_target.samples = LC_SAMPLE_COUNT_1;
            res = lc_graphics_pipeline_create(device, &pipeline_desc,
                                              &quad_pipeline);
            if (res != LC_SUCCESS) {
                FAIL_CLEANUP("lc_graphics_pipeline_create(quad)", res);
            }
        }
        lc_shader_destroy(quad_fs);
        lc_shader_destroy(quad_vs);
        quad_fs = NULL;
        quad_vs = NULL;

        res = lc_binding_set_create(off_layout, &off_set);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_binding_set_create(quad)", res);
        }
        writes[0].binding = 0;
        writes[0].array_element = 0;
        writes[0].type = LC_BINDING_SAMPLED_IMAGE;
        writes[0].u.image.view = off_color_view;
        writes[1].binding = 1;
        writes[1].array_element = 0;
        writes[1].type = LC_BINDING_SAMPLER;
        writes[1].u.sampler.sampler = off_sampler;
        res = lc_binding_set_update(off_set, writes, 2);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_binding_set_update(quad)", res);
        }
    }

    printf("LumaC %s\n", lc_get_version_string());
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("Offscreen: %ux%u (swapchain-independent); ",
           lc_render_target_get_width(offscreen),
           lc_render_target_get_height(offscreen));
    printf("swapchain target compatible: %d\n",
           lc_render_target_is_compatible_with_pipeline(
               lc_swapchain_get_render_target(swapchain), quad_pipeline));
    printf("Two-pass render-to-texture. Close the window to exit.\n");

    while (!lc_window_should_close(window)) {
        uint32_t w;
        uint32_t h;
        float mvp[16];
        float angle;
        lc_command_encoder *enc = NULL;
        lc_render_color_attachment off_color_att;
        lc_render_depth_attachment off_depth_att;
        lc_render_pass_desc off_pass;
        lc_render_swapchain_pass_desc swap_pass;

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
            /* Offscreen target intentionally untouched. */
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
        res = lc_swapchain_get_encoder(swapchain, &enc);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_swapchain_get_encoder failed (%d)\n", res);
            goto cleanup;
        }

        angle = (float)frame * 0.01f;
        write_cube_mvp(mvp, 1.0f, angle);

        /* Pass 1: cube into the offscreen target. */
        off_color_att.view = off_color_view;
        off_color_att.load_op = LC_LOAD_OP_CLEAR;
        off_color_att.store_op = LC_STORE_OP_STORE;
        off_color_att.clear_color[0] = 0.04f;
        off_color_att.clear_color[1] = 0.05f;
        off_color_att.clear_color[2] = 0.09f;
        off_color_att.clear_color[3] = 1.0f;
        off_depth_att.view = off_depth_view;
        off_depth_att.depth_load_op = LC_LOAD_OP_CLEAR;
        off_depth_att.depth_store_op = LC_STORE_OP_STORE;
        off_depth_att.clear_depth = 1.0f;
        off_depth_att.stencil_load_op = LC_LOAD_OP_DONT_CARE;
        off_depth_att.stencil_store_op = LC_STORE_OP_DONT_CARE;
        off_depth_att.clear_stencil = 0;
        off_pass.color_attachments = &off_color_att;
        off_pass.color_attachment_count = 1;
        off_pass.depth_attachment = &off_depth_att;
        off_pass.width = OFF_W;
        off_pass.height = OFF_H;
#define PASS1(op)                                                             \
    do {                                                                      \
        res = (op);                                                           \
        if (res != LC_SUCCESS) {                                              \
            fprintf(stderr, #op " failed (%d)\n", res);                       \
            goto end_pass1;                                                   \
        }                                                                     \
    } while (0)
        PASS1(lc_encoder_begin_render_pass(enc, &off_pass));
        PASS1(lc_encoder_bind_pipeline(enc, cube_pipeline));
        PASS1(lc_encoder_bind_binding_set(enc, cube_pipeline, 0, tex_set));
        PASS1(lc_encoder_bind_vertex_buffer(enc, 0, vertex_buffer, 0));
        PASS1(lc_encoder_bind_vertex_buffer(enc, 1, instance_buffer, 0));
        PASS1(lc_encoder_bind_index_buffer(enc, index_buffer, 0,
                                           LC_INDEX_UINT16));
        PASS1(lc_encoder_push_constants(enc, cube_pipeline,
                                        LC_SHADER_VISIBILITY_VERTEX, 0,
                                        sizeof(mvp), mvp));
        PASS1(lc_encoder_draw_indexed(enc, 36, 1, 0, 0, 0));
        PASS1(lc_encoder_end_render_pass(enc));
#undef PASS1
        goto pass2;
end_pass1:
        /* Never submit half a pass: end what was opened, then fail. */
        lc_encoder_end_render_pass(enc);
        goto cleanup;

pass2:
        /* Pass 2: sample the offscreen result to the swapchain. */
        swap_pass.color_load_op = LC_LOAD_OP_CLEAR;
        swap_pass.color_store_op = LC_STORE_OP_STORE;
        swap_pass.clear_color[0] = 0.02f;
        swap_pass.clear_color[1] = 0.02f;
        swap_pass.clear_color[2] = 0.03f;
        swap_pass.clear_color[3] = 1.0f;
        swap_pass.depth_load_op = LC_LOAD_OP_CLEAR;
        swap_pass.depth_store_op = LC_STORE_OP_DONT_CARE;
        swap_pass.clear_depth = 1.0f;
#define PASS2(op)                                                             \
    do {                                                                      \
        res = (op);                                                           \
        if (res != LC_SUCCESS) {                                              \
            fprintf(stderr, #op " failed (%d)\n", res);                       \
            goto end_pass2;                                                   \
        }                                                                     \
    } while (0)
        PASS2(lc_encoder_begin_swapchain_pass(enc, swapchain, &swap_pass));
        PASS2(lc_encoder_bind_pipeline(enc, quad_pipeline));
        PASS2(lc_encoder_bind_binding_set(enc, quad_pipeline, 0, off_set));
        PASS2(lc_encoder_draw(enc, 3, 0));
        PASS2(lc_encoder_end_render_pass(enc));
#undef PASS2
        goto present;
end_pass2:
        lc_encoder_end_render_pass(enc);
        goto cleanup;

present:
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
        frame++;
    }

    exit_code = 0;

cleanup:
    free(code);
    lc_pipeline_destroy(quad_pipeline);
    lc_pipeline_destroy(cube_pipeline);
    lc_binding_set_destroy(off_set);
    lc_binding_set_destroy(tex_set);
    lc_binding_layout_destroy(off_layout);
    lc_binding_layout_destroy(tex_layout);
    lc_shader_destroy(quad_fs);
    lc_shader_destroy(quad_vs);
    lc_shader_destroy(cube_fs);
    lc_shader_destroy(cube_vs);
    lc_sampler_destroy(off_sampler);
    lc_sampler_destroy(texture_sampler);
    lc_render_target_destroy(offscreen);
    lc_image_view_destroy(off_depth_view);
    lc_image_view_destroy(off_color_view);
    lc_image_destroy(off_depth);
    lc_image_destroy(off_color);
    lc_image_view_destroy(texture_view);
    lc_image_destroy(texture);
    lc_buffer_destroy(instance_buffer);
    lc_buffer_destroy(index_buffer);
    lc_buffer_destroy(vertex_buffer);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return exit_code;
}
