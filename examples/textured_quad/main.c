#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>

/*
 * Phase 10 example: textured quad. A 64x64 procedural checkerboard
 * uploads into a mipmapped image; a uniform buffer carries a sliding
 * transform updated every frame (no descriptor rebuilds); one binding
 * set (uniform + sampled image + sampler) feeds a pipeline that draws
 * two triangles. Resize recreates the swapchain; minimize defers;
 * close exits.
 */

#ifndef LC_TRIANGLE_SPV_DIR
#define LC_TRIANGLE_SPV_DIR "."
#endif

#define TEX_W 64
#define TEX_H 64

typedef struct quad_vertex {
    float position[2];
    float uv[2];
} quad_vertex;

/* Two triangles, counter-clockwise in Vulkan NDC (culling is off, so
 * winding is belt-and-braces). UV origin top-left matches the upload. */
static const quad_vertex k_quad[6] = {
    { { -0.7f, -0.7f }, { 0.0f, 0.0f } },
    { { 0.7f, -0.7f }, { 1.0f, 0.0f } },
    { { 0.7f, 0.7f }, { 1.0f, 1.0f } },
    { { -0.7f, -0.7f }, { 0.0f, 0.0f } },
    { { 0.7f, 0.7f }, { 1.0f, 1.0f } },
    { { -0.7f, 0.7f }, { 0.0f, 1.0f } },
};

static unsigned char s_texels[TEX_W * TEX_H * 4];

static void fill_checkerboard(void) {
    uint32_t x;
    uint32_t y;

    for (y = 0; y < TEX_H; y++) {
        for (x = 0; x < TEX_W; x++) {
            unsigned char *px = &s_texels[(y * TEX_W + x) * 4];
            int white = (int)(((x / 8u) + (y / 8u)) % 2u);

            px[0] = white ? 235 : 30; /* R */
            px[1] = white ? 235 : 90; /* G */
            px[2] = white ? 235 : 160; /* B */
            px[3] = 255; /* A */
        }
    }
}

/* Minimal private SPIR-V file loader (examples/tests only; the library
 * takes bytes, never paths). Returns malloc'd 4-aligned memory. */
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
    /* 260 matches Windows MAX_PATH for these short staged paths. */
    char path[260];
    int written = snprintf(path, sizeof(path), "%s/%s", LC_TRIANGLE_SPV_DIR,
                           name);
    if (written < 0 || (size_t)written >= sizeof(path)) {
        return 0;
    }
    return load_spv(path, out_code, out_size);
}

/* Column-major identity with an X translation in elements[12]. */
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
    lc_buffer *uniform_buffer = NULL;
    lc_image *image = NULL;
    lc_image_view *view = NULL;
    lc_sampler *sampler = NULL;
    lc_shader *vertex_shader = NULL;
    lc_shader *fragment_shader = NULL;
    lc_binding_layout *layout = NULL;
    lc_binding_set *set = NULL;
    lc_pipeline *pipeline = NULL;
    void *uniform_mapped = NULL;
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

    window_desc.title = "LumaC Textured Quad";
    window_desc.width = 800;
    window_desc.height = 600;
    if (lc_window_create(&window_desc, &window) != LC_SUCCESS) {
        fprintf(stderr, "lc_window_create failed\n");
        lc_shutdown();
        return 1;
    }

    device_desc.backend = LC_BACKEND_VULKAN;
    device_desc.enable_validation = 1; /* best-effort; falls back without layers */
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
    swapchain_desc.image_count = 0; /* automatic */
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

    /* Vertex buffer: GPU-only, staged upload. */
    {
        lc_buffer_desc buffer_desc;
        lc_result res;

        buffer_desc.size = sizeof(k_quad);
        buffer_desc.usage = LC_BUFFER_USAGE_VERTEX;
        buffer_desc.memory = LC_MEMORY_GPU_ONLY;
        res = lc_buffer_create(device, &buffer_desc, &vertex_buffer);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_create(vertex)", res);
        }
        res = lc_buffer_write(vertex_buffer, 0, k_quad, sizeof(k_quad));
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_write(vertex)", res);
        }
    }

    /* Uniform buffer: CPU-visible, mapped once, rewritten per frame. */
    {
        lc_buffer_desc buffer_desc;
        lc_result res;

        buffer_desc.size = 64; /* mat4 */
        buffer_desc.usage = LC_BUFFER_USAGE_UNIFORM;
        buffer_desc.memory = LC_MEMORY_CPU_TO_GPU;
        res = lc_buffer_create(device, &buffer_desc, &uniform_buffer);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_create(uniform)", res);
        }
        res = lc_buffer_map(uniform_buffer, &uniform_mapped);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_buffer_map(uniform)", res);
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
        lc_result res;

        image_desc.type = LC_IMAGE_TYPE_2D;
        image_desc.format = LC_FORMAT_RGBA8_UNORM;
        image_desc.width = TEX_W;
        image_desc.height = TEX_H;
        image_desc.depth = 1;
        image_desc.mip_levels = 0; /* full chain */
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
        view_desc.format = LC_FORMAT_UNDEFINED; /* image format */
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

    /* Shaders + pipeline with one resource slot layout. */
    if (!load_shader_file("tex_quad.vert.spv", &vert_code, &vert_size) ||
        !load_shader_file("tex_quad.frag.spv", &frag_code, &frag_size)) {
        fprintf(stderr, "failed to load texture SPIR-V\n");
        goto cleanup;
    }
    {
        lc_shader_desc shader_desc;
        lc_graphics_pipeline_desc pipeline_desc = { 0 };
        lc_binding_desc bindings[3];
        lc_binding_layout_desc layout_desc;
        lc_binding_write writes[3];
        lc_vertex_binding_desc vbinding;
        lc_vertex_attribute_desc vattrs[2];
        lc_result res;

        shader_desc.stage = LC_SHADER_STAGE_VERTEX;
        shader_desc.code = vert_code;
        shader_desc.code_size = vert_size;
        shader_desc.entry_point = NULL; /* "main" */
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
        bindings[0].type = LC_BINDING_UNIFORM_BUFFER;
        bindings[0].count = 1;
        bindings[0].visibility = LC_SHADER_VISIBILITY_VERTEX;
        bindings[1].binding = 1;
        bindings[1].type = LC_BINDING_SAMPLED_IMAGE;
        bindings[1].count = 1;
        bindings[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        bindings[2].binding = 2;
        bindings[2].type = LC_BINDING_SAMPLER;
        bindings[2].count = 1;
        bindings[2].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        layout_desc.bindings = bindings;
        layout_desc.binding_count = 3;
        res = lc_binding_layout_create(device, &layout_desc, &layout);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_binding_layout_create", res);
        }

        vbinding.binding = 0;
        vbinding.stride = sizeof(quad_vertex);
        vbinding.input_rate = LC_VERTEX_INPUT_PER_VERTEX;
        vattrs[0].location = 0;
        vattrs[0].binding = 0;
        vattrs[0].format = LC_FORMAT_RG32_FLOAT;
        vattrs[0].offset = 0;
        vattrs[1].location = 1;
        vattrs[1].binding = 0;
        vattrs[1].format = LC_FORMAT_RG32_FLOAT;
        vattrs[1].offset = sizeof(float) * 2u;

        pipeline_desc.vertex_shader = vertex_shader;
        pipeline_desc.fragment_shader = fragment_shader;
        pipeline_desc.vertex_bindings = &vbinding;
        pipeline_desc.vertex_binding_count = 1;
        const lc_binding_layout *slots[1];

        slots[0] = layout;
        pipeline_desc.vertex_attributes = vattrs;
        pipeline_desc.vertex_attribute_count = 2;
        pipeline_desc.binding_layouts = slots;
        pipeline_desc.binding_layout_count = 1;
        res = lc_swapchain_get_render_target_desc(
            swapchain, &pipeline_desc.render_target);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_swapchain_get_render_target_desc", res);
        }
        res = lc_graphics_pipeline_create(device, &pipeline_desc, &pipeline);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_graphics_pipeline_create", res);
        }
        /* Shaders may die once the pipeline exists. */
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
        writes[0].type = LC_BINDING_UNIFORM_BUFFER;
        writes[0].u.buffer.buffer = uniform_buffer;
        writes[0].u.buffer.offset = 0;
        writes[0].u.buffer.size = 0; /* whole buffer */
        writes[1].binding = 1;
        writes[1].array_element = 0;
        writes[1].type = LC_BINDING_SAMPLED_IMAGE;
        writes[1].u.image.view = view;
        writes[2].binding = 2;
        writes[2].array_element = 0;
        writes[2].type = LC_BINDING_SAMPLER;
        writes[2].u.sampler.sampler = sampler;
        res = lc_binding_set_update(set, writes, 3);
        if (res != LC_SUCCESS) {
            FAIL_CLEANUP("lc_binding_set_update", res);
        }
    }

    printf("LumaC %s\n", lc_get_version_string());
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("Rendering textured quad. Close the window to exit.\n");

    while (!lc_window_should_close(window)) {
        uint32_t w;
        uint32_t h;
        /* Gentle horizontal slide, integer triangle wave: no math
         * library needed. */
        unsigned long phase = frame % 600ul;
        float slide;

        lc_poll_events();
        w = lc_window_get_width(window);
        h = lc_window_get_height(window);
        if (w == 0 || h == 0) {
            continue; /* minimized: nothing valid to render into */
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

        /* Uniform contents update in place every frame; the binding
         * set from setup is reused untouched. */
        slide = ((phase < 300ul) ? (float)phase : (float)(600ul - phase)) /
                300.0f * 0.25f - 0.125f;
        write_slide_matrix((float *)uniform_mapped, slide);

        res = lc_clear_color(swapchain, 0.06f, 0.07f, 0.10f, 1.0f);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_clear_color failed (%d)\n", res);
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
            fprintf(stderr, "lc_bind_vertex_buffer failed (%d)\n", res);
            goto cleanup;
        }
        res = lc_draw(swapchain, 6, 0);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_draw failed (%d)\n", res);
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
    lc_buffer_destroy(uniform_buffer);
    lc_buffer_destroy(vertex_buffer);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return exit_code;
}
