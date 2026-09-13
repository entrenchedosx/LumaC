#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>

/*
 * Phase 7 example: first LumaC triangle. Loads SPIR-V vertex/fragment
 * shaders, builds a graphics pipeline, and draws one vertex-indexed
 * triangle per frame over a dark background. Resize recreates the
 * swapchain; minimize defers; close exits. No vertex buffers yet.
 */

#ifndef LC_TRIANGLE_SPV_DIR
#define LC_TRIANGLE_SPV_DIR "."
#endif

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

int main(void) {
    lc_window *window = NULL;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lc_shader *vertex_shader = NULL;
    lc_shader *fragment_shader = NULL;
    lc_pipeline *pipeline = NULL;
    lc_window_desc window_desc;
    lc_device_desc device_desc = { 0 };
    lc_swapchain_desc swapchain_desc;
    lc_shader_desc shader_desc;
    lc_graphics_pipeline_desc pipeline_desc = { 0 };
    lc_result res;
    void *vert_code = NULL;
    void *frag_code = NULL;
    size_t vert_size = 0;
    size_t frag_size = 0;

    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    window_desc.title = "LumaC Triangle";
    window_desc.width = 800;
    window_desc.height = 600;
    res = lc_window_create(&window_desc, &window);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_window_create failed (%d)\n", res);
        lc_shutdown();
        return 1;
    }

    device_desc.backend = LC_BACKEND_VULKAN;
    device_desc.enable_validation = 1; /* best-effort; falls back without layers */
    res = lc_device_create(&device_desc, &device);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_device_create failed (%d)\n", res);
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }

    res = lc_surface_create(device, window, &surface);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_surface_create failed (%d)\n", res);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }

    swapchain_desc.width = lc_window_get_width(window);
    swapchain_desc.height = lc_window_get_height(window);
    swapchain_desc.image_count = 0; /* automatic */
    swapchain_desc.vsync = 1;
    res = lc_swapchain_create(device, surface, &swapchain_desc, &swapchain);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_swapchain_create failed (%d)\n", res);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }

    if (!load_shader_file("triangle.vert.spv", &vert_code, &vert_size) ||
        !load_shader_file("triangle.frag.spv", &frag_code, &frag_size)) {
        fprintf(stderr, "failed to load triangle SPIR-V\n");
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }

    shader_desc.stage = LC_SHADER_STAGE_VERTEX;
    shader_desc.code = vert_code;
    shader_desc.code_size = vert_size;
    shader_desc.entry_point = NULL; /* "main" */
    res = lc_shader_create(device, &shader_desc, &vertex_shader);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "vertex lc_shader_create failed (%d)\n", res);
        free(vert_code);
        free(frag_code);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }

    shader_desc.stage = LC_SHADER_STAGE_FRAGMENT;
    shader_desc.code = frag_code;
    shader_desc.code_size = frag_size;
    shader_desc.entry_point = NULL;
    res = lc_shader_create(device, &shader_desc, &fragment_shader);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "fragment lc_shader_create failed (%d)\n", res);
        lc_shader_destroy(vertex_shader);
        free(vert_code);
        free(frag_code);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }
    /* Host copies no longer needed; modules live on the device. */
    free(vert_code);
    free(frag_code);
    vert_code = NULL;
    frag_code = NULL;

    pipeline_desc.vertex_shader = vertex_shader;
    pipeline_desc.fragment_shader = fragment_shader;
    /* Presentation-compatible signature, built deliberately (Phase 13:
     * pipeline creation takes no swapchain). */
    res = lc_swapchain_get_render_target_desc(swapchain,
                                              &pipeline_desc.render_target);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_swapchain_get_render_target_desc failed (%d)\n",
                res);
        lc_shader_destroy(fragment_shader);
        lc_shader_destroy(vertex_shader);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }
    res = lc_graphics_pipeline_create(device, &pipeline_desc, &pipeline);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_graphics_pipeline_create failed (%d)\n", res);
        lc_shader_destroy(fragment_shader);
        lc_shader_destroy(vertex_shader);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }
    /* Shaders may die once the pipeline exists. */
    lc_shader_destroy(fragment_shader);
    lc_shader_destroy(vertex_shader);
    fragment_shader = NULL;
    vertex_shader = NULL;

    printf("LumaC %s\n", lc_get_version_string());
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("Rendering triangle. Close the window to exit.\n");

    while (!lc_window_should_close(window)) {
        uint32_t w;
        uint32_t h;

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
                break;
            }
        }

        res = lc_begin_frame(swapchain);
        if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            res = lc_swapchain_recreate(swapchain, w, h);
            if (res != LC_SUCCESS && res != LC_ERROR_ZERO_EXTENT) {
                fprintf(stderr, "lc_swapchain_recreate failed (%d)\n", res);
                break;
            }
            continue;
        }
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_begin_frame failed (%d)\n", res);
            break;
        }

        res = lc_clear_color(swapchain, 0.08f, 0.10f, 0.16f, 1.0f);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_clear_color failed (%d)\n", res);
            break;
        }
        res = lc_bind_pipeline(swapchain, pipeline);
        if (res == LC_ERROR_PIPELINE_INCOMPATIBLE) {
            fprintf(stderr, "pipeline incompatible after recreate\n");
            break;
        }
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_bind_pipeline failed (%d)\n", res);
            break;
        }
        res = lc_draw(swapchain, 3, 0);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_draw failed (%d)\n", res);
            break;
        }

        res = lc_end_frame(swapchain);
        if (res == LC_SUBOPTIMAL ||
            res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            res = lc_swapchain_recreate(swapchain, w, h);
            if (res != LC_SUCCESS && res != LC_ERROR_ZERO_EXTENT) {
                fprintf(stderr, "lc_swapchain_recreate failed (%d)\n", res);
                break;
            }
            continue;
        }
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_end_frame failed (%d)\n", res);
            break;
        }
    }

    lc_pipeline_destroy(pipeline);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return 0;
}
