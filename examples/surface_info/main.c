#include <stdio.h>

#include <lumac/lumac.h>

/*
 * Phase 4 example: links one Vulkan device with one native window via a
 * presentation surface. No swapchain, no rendering - the window stays
 * blank. Close the window to exit.
 */
int main(void) {
    lc_window *window = NULL;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_window_desc window_desc;
    lc_device_desc device_desc = { 0 };
    lc_result res;

    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    window_desc.title = "LumaC Surface Example";
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

    printf("LumaC %s\n", lc_get_version_string());
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("Window: %ux%u\n", lc_window_get_width(window),
           lc_window_get_height(window));
    printf("Vulkan surface: created\n");
    printf("Presentation: %s\n",
           lc_surface_is_present_supported(surface) ? "supported"
                                                    : "unsupported");

    while (!lc_window_should_close(window)) {
        lc_poll_events();
    }

    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return 0;
}
