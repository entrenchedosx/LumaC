#include <stdio.h>

#include <lumac/lumac.h>

/*
 * Phase 6 example: window + Vulkan device + surface + swapchain with a
 * real frame loop. Each frame acquires an image, clears it to a slowly
 * pulsing deep blue, and presents. Resize recreates the swapchain;
 * minimize defers work until restore. Close the window to exit.
 * No triangle, no shaders - clear only.
 */
int main(void) {
    lc_window *window = NULL;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lc_window_desc window_desc;
    lc_device_desc device_desc = { 0 };
    lc_swapchain_desc swapchain_desc;
    lc_result res;
    unsigned long frame = 0;

    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    window_desc.title = "LumaC Clear Screen";
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

    printf("LumaC %s\n", lc_get_version_string());
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("Rendering clear color. Close the window to exit.\n");

    while (!lc_window_should_close(window)) {
        uint32_t w;
        uint32_t h;
        /* Slow pulse on blue between ~0.15 and ~0.35 over 480 frames.
         * Integer triangle wave: no math library needed. */
        unsigned long phase = frame % 480ul;
        float pulse;
        float blue;

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

        pulse = (phase < 240ul) ? (float)phase / 240.0f
                                : (float)(480ul - phase) / 240.0f;
        blue = 0.15f + 0.20f * pulse;
        res = lc_clear_color(swapchain, 0.08f, 0.12f, blue, 1.0f);
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_clear_color failed (%d)\n", res);
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
        frame++;
    }

    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return 0;
}
