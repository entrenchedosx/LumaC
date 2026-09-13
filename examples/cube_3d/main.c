#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <lumac/lumac.h>

/*
 * Phase 11 example: indexed textured cube with depth, push constants,
 * and instancing. Two cube instances (per-instance offsets) share one
 * indexed vertex buffer; a 64-byte push-constant MVP rotates them every
 * frame; the swapchain-owned depth buffer resolves occlusion; back-face
 * culling is on. Resize recreates the swapchain; minimize defers.
 */

#ifndef LC_TRIANGLE_SPV_DIR
#define LC_TRIANGLE_SPV_DIR "."
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TEX_W 64
#define TEX_H 64
#define CUBE_INSTANCES 2

typedef struct cube_vertex {
    float position[3];
    float uv[2];
} cube_vertex;

/* 24 vertices: 4 per face, CCW outward (front face winding verified by
 * (b-a)x(c-a) == outward normal), full-texture UVs per face. */
static const cube_vertex k_cube_vertices[24] = {
    /* Front (+Z) */
    { { -0.5f, -0.5f, 0.5f }, { 0.0f, 0.0f } },
    { { 0.5f, -0.5f, 0.5f }, { 1.0f, 0.0f } },
    { { 0.5f, 0.5f, 0.5f }, { 1.0f, 1.0f } },
    { { -0.5f, 0.5f, 0.5f }, { 0.0f, 1.0f } },
    /* Back (-Z) */
    { { 0.5f, -0.5f, -0.5f }, { 0.0f, 0.0f } },
    { { -0.5f, -0.5f, -0.5f }, { 1.0f, 0.0f } },
    { { -0.5f, 0.5f, -0.5f }, { 1.0f, 1.0f } },
    { { 0.5f, 0.5f, -0.5f }, { 0.0f, 1.0f } },
    /* Right (+X) */
    { { 0.5f, -0.5f, 0.5f }, { 0.0f, 0.0f } },
    { { 0.5f, -0.5f, -0.5f }, { 1.0f, 0.0f } },
    { { 0.5f, 0.5f, -0.5f }, { 1.0f, 1.0f } },
    { { 0.5f, 0.5f, 0.5f }, { 0.0f, 1.0f } },
    /* Left (-X) */
    { { -0.5f, -0.5f, -0.5f }, { 0.0f, 0.0f } },
    { { -0.5f, -0.5f, 0.5f }, { 1.0f, 0.0f } },
    { { -0.5f, 0.5f, 0.5f }, { 1.0f, 1.0f } },
    { { -0.5f, 0.5f, -0.5f }, { 0.0f, 1.0f } },
    /* Top (+Y) */
    { { -0.5f, 0.5f, 0.5f }, { 0.0f, 0.0f } },
    { { 0.5f, 0.5f, 0.5f }, { 1.0f, 0.0f } },
    { { 0.5f, 0.5f, -0.5f }, { 1.0f, 1.0f } },
    { { -0.5f, 0.5f, -0.5f }, { 0.0f, 1.0f } },
    /* Bottom (-Y) */
    { { -0.5f, -0.5f, -0.5f }, { 0.0f, 0.0f } },
    { { 0.5f, -0.5f, -0.5f }, { 1.0f, 0.0f } },
    { { 0.5f, -0.5f, 0.5f }, { 1.0f, 1.0f } },
    { { -0.5f, -0.5f, 0.5f }, { 0.0f, 1.0f } },
};

static const uint16_t k_cube_indices[36] = {
    0, 1, 2, 0, 2, 3, /* front */
    4, 5, 6, 4, 6, 7, /* back */
    8, 9, 10, 8, 10, 11, /* right */
    12, 13, 14, 12, 14, 15, /* left */
    16, 17, 18, 16, 18, 19, /* top */
    20, 21, 22, 20, 22, 23, /* bottom */
};

/* Per-instance world offsets: two cubes side by side. */
static const float k_instance_offsets[CUBE_INSTANCES][3] = {
    { -0.75f, 0.0f, 0.0f },
    { 0.75f, 0.0f, 0.0f },
};

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

/* Column-major mat4 helpers (GLSL layout: m[col*4+row]). */
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

    /* out = a * b (column-major). */
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
    m[5] = -f; /* Vulkan Y flip */
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

/* MVP for a rotating cube: proj * view * model. View is a fixed
 * translate(0,0,-3); model spins around Y with a slight X tilt. */
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
    lc_image *image = NULL;
    lc_image_view *view = NULL;
    lc_sampler *sampler = NULL;
    lc_shader *vertex_shader = NULL;
    lc_shader *fragment_shader = NULL;
    lc_binding_layout *layout = NULL;
    lc_binding_set *set = NULL;
    lc_pipeline *pipeline = NULL;
    void *vert_code = NULL;
    void *frag_code = NULL;
    size_t vert_size = 0;
    size_t frag_size = 0;
    lc_window_desc window_desc;
    lc_device_desc device_desc = { 0 };
    lc_swapchain_desc swapchain_desc;
    lc_result res = LC_SUCCESS;
    unsigned long frame = 0;
    int exit_code = 1;

    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    window_desc.title = "LumaC Textured Cube (3D)";
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

    /* Indexed vertex data: GPU-only buffers via staging uploads. */
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

        buffer_desc.size = sizeof(k_instance_offsets);
        buffer_desc.usage = LC_BUFFER_USAGE_VERTEX;
        buffer_desc.memory = LC_MEMORY_GPU_ONLY;
        res = lc_buffer_create(device, &buffer_desc, &instance_buffer);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_create(instance)", res);
        }
        res = lc_buffer_write(instance_buffer, 0, k_instance_offsets,
                              sizeof(k_instance_offsets));
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_write(instance)", res);
        }
    }

    /* Texture: checkerboard upload plus generated mipmaps. */
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
        res = lc_image_create(device, &image_desc, &image);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_create", res);
        }

        upload.mip_level = 0;
        upload.array_layer = 0;
        upload.width = TEX_W;
        upload.height = TEX_H;
        upload.depth = 1;
        upload.data = s_texels;
        upload.data_size = sizeof(s_texels);
        res = lc_image_write(image, &upload);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_write", res);
        }
        res = lc_image_generate_mipmaps(image);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_generate_mipmaps", res);
        }

        view_desc.type = LC_IMAGE_VIEW_2D;
        view_desc.format = LC_FORMAT_UNDEFINED;
        view_desc.aspect = LC_IMAGE_ASPECT_COLOR;
        view_desc.base_mip_level = 0;
        view_desc.mip_level_count = lc_image_get_mip_levels(image);
        view_desc.base_array_layer = 0;
        view_desc.array_layer_count = 1;
        res = lc_image_view_create(image, &view_desc, &view);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_image_view_create", res);
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
        res = lc_sampler_create(device, &sampler_desc, &sampler);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_sampler_create", res);
        }
    }

    /* Shaders + 3D pipeline: culling, depth, push-constant MVP. */
    if (!load_shader_file("cube.vert.spv", &vert_code, &vert_size) ||
        !load_shader_file("cube.frag.spv", &frag_code, &frag_size)) {
        fprintf(stderr, "failed to load cube SPIR-V\n");
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

        shader_desc.stage = LC_SHADER_STAGE_VERTEX;
        shader_desc.code = vert_code;
        shader_desc.code_size = vert_size;
        shader_desc.entry_point = NULL;
        res = lc_shader_create(device, &shader_desc, &vertex_shader);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("vertex lc_shader_create", res);
        }
        shader_desc.stage = LC_SHADER_STAGE_FRAGMENT;
        shader_desc.code = frag_code;
        shader_desc.code_size = frag_size;
        res = lc_shader_create(device, &shader_desc, &fragment_shader);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("fragment lc_shader_create", res);
        }
        free(vert_code);
        free(frag_code);
        vert_code = NULL;
        frag_code = NULL;

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
        res = lc_binding_layout_create(device, &layout_desc, &layout);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_binding_layout_create", res);
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
        push_range.size = 64; /* mat4 */

        pipeline_desc.vertex_shader = vertex_shader;
        pipeline_desc.fragment_shader = fragment_shader;
        pipeline_desc.vertex_bindings = vbindings;
        pipeline_desc.vertex_binding_count = 2;
        pipeline_desc.vertex_attributes = vattrs;
        pipeline_desc.vertex_attribute_count = 3;
        {
            const lc_binding_layout *slots[1];

            slots[0] = layout;
            pipeline_desc.binding_layouts = slots;
            pipeline_desc.binding_layout_count = 1;
            pipeline_desc.cull_mode = LC_CULL_BACK;
            pipeline_desc.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
            pipeline_desc.depth_test_enable = 1;
            pipeline_desc.depth_write_enable = 1;
            pipeline_desc.push_constant_ranges = &push_range;
            pipeline_desc.push_constant_range_count = 1;
            res = lc_swapchain_get_render_target_desc(
                swapchain, &pipeline_desc.render_target);
            if (res != LC_SUCCESS) {
                FAIL_CLEANUP("lc_swapchain_get_render_target_desc", res);
            }
            res = lc_graphics_pipeline_create(device, &pipeline_desc,
                                              &pipeline);
            if (res != LC_SUCCESS) {
                FAIL_CLEANUP("lc_graphics_pipeline_create", res);
            }
        }
        lc_shader_destroy(fragment_shader);
        lc_shader_destroy(vertex_shader);
        fragment_shader = NULL;
        vertex_shader = NULL;

        res = lc_binding_set_create(layout, &set);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_binding_set_create", res);
        }
        writes[0].binding = 0;
        writes[0].array_element = 0;
        writes[0].type = LC_BINDING_SAMPLED_IMAGE;
        writes[0].u.image.view = view;
        writes[1].binding = 1;
        writes[1].array_element = 0;
        writes[1].type = LC_BINDING_SAMPLER;
        writes[1].u.sampler.sampler = sampler;
        res = lc_binding_set_update(set, writes, 2);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_binding_set_update", res);
        }
    }

    printf("LumaC %s\n", lc_get_version_string());
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("Depth format: %d\n", (int)lc_swapchain_get_depth_format(swapchain));
    printf("Rendering indexed textured cubes (instanced x%d). Close to exit.\n",
           CUBE_INSTANCES);

    while (!lc_window_should_close(window)) {
        uint32_t w;
        uint32_t h;
        float mvp[16];
        float aspect;
        float angle;

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

        aspect = (h != 0) ? ((float)w / (float)h) : 1.0f;
        angle = (float)frame * 0.01f;
        write_cube_mvp(mvp, aspect, angle);

        res = lc_clear_color(swapchain, 0.06f, 0.07f, 0.10f, 1.0f);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_clear_color failed (%d)\n", res);
            goto cleanup;
        }
        res = lc_clear_depth(swapchain, 1.0f);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_clear_depth failed (%d)\n", res);
            goto cleanup;
        }
        res = lc_bind_pipeline(swapchain, pipeline);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_bind_pipeline failed (%d)\n", res);
            goto cleanup;
        }
        res = lc_bind_binding_set(swapchain, pipeline, 0, set);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_bind_binding_set failed (%d)\n", res);
            goto cleanup;
        }
        res = lc_bind_vertex_buffer(swapchain, 0, vertex_buffer, 0);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_bind_vertex_buffer(0) failed (%d)\n", res);
            goto cleanup;
        }
        res = lc_bind_vertex_buffer(swapchain, 1, instance_buffer, 0);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_bind_vertex_buffer(1) failed (%d)\n", res);
            goto cleanup;
        }
        res = lc_bind_index_buffer(swapchain, index_buffer, 0,
                                   LC_INDEX_UINT16);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_bind_index_buffer failed (%d)\n", res);
            goto cleanup;
        }
        res = lc_push_constants(swapchain, pipeline,
                                LC_SHADER_VISIBILITY_VERTEX, 0, sizeof(mvp),
                                mvp);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_push_constants failed (%d)\n", res);
            goto cleanup;
        }
        res = lc_draw_indexed(swapchain, 36, CUBE_INSTANCES, 0, 0, 0);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_draw_indexed failed (%d)\n", res);
            goto cleanup;
        }

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
    free(vert_code);
    free(frag_code);
    lc_pipeline_destroy(pipeline);
    lc_binding_set_destroy(set);
    lc_binding_layout_destroy(layout);
    lc_shader_destroy(fragment_shader);
    lc_shader_destroy(vertex_shader);
    lc_sampler_destroy(sampler);
    lc_image_view_destroy(view);
    lc_image_destroy(image);
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
