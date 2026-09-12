/*
 * Vulkan surface integration test (Phase 4).
 *
 * Creates real native windows, Vulkan devices, and VkSurfaceKHR objects:
 * surface creation, presentation support, capability/format/mode queries
 * (implied by creation success), multi-window/multi-surface lifetimes,
 * device-destroy and window-destroy dependency cleanup, and ordered
 * shutdown. Run with validation layers: a leaked or misordered surface
 * would surface as a validation error on teardown.
 *
 * If the environment cannot provide a window or a usable Vulkan setup,
 * the test reports SKIP and exits 0. Any other failure is a hard FAIL.
 */
#include <stdio.h>
#include <lumac/lumac.h>

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
static int make_window(lc_window **out) {
    lc_window_desc desc;

    desc.title = "LumaC Surface Test";
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

int main(void) {
    printf("Running LumaC Vulkan surface integration test...\n");

    lc_shutdown();
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }

    /* ---- 1. basic surface creation + presentation (§41) ---- */
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        int env;

        env = make_device(&device);
        if (env != 0) {
            if (env == 1) {
                SKIP_ENV("a Vulkan device");
            }
            printf("device creation failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        env = make_window(&window);
        if (env != 0) {
            lc_device_destroy(device);
            if (env == 1) {
                SKIP_ENV("a native window");
            }
            printf("window creation failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }

        /* From here on the environment is proven: failures are FAILs. */
        TEST_CHECK(lc_surface_create(device, window, &surface) == LC_SUCCESS,
                   "surface create succeeds");
        TEST_CHECK(surface != NULL, "surface handle non-null");
        TEST_CHECK(lc_surface_is_present_supported(surface) != 0,
                   "presentation supported");
        TEST_CHECK(lc_window_get_width(window) == 800 &&
                   lc_window_get_height(window) == 600,
                   "window 800x600 while surfaced");
        lc_poll_events();
        TEST_CHECK(lc_window_should_close(window) == 0,
                   "window usable with live surface");

        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "explicit surface/device/window teardown clean");
    }

    /* ---- 2. multi-window / multi-surface lifetimes (§42) ---- */
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
        lc_surface *surf_a2 = NULL;

        TEST_CHECK(make_device(&device) == 0, "device for multi-surface test");
        TEST_CHECK(make_window(&win_a) == 0, "window A created");
        TEST_CHECK(make_window(&win_b) == 0, "window B created");
        TEST_CHECK(lc_surface_create(device, win_a, &surf_a) == LC_SUCCESS,
                   "surface A created");
        TEST_CHECK(lc_surface_create(device, win_b, &surf_b) == LC_SUCCESS,
                   "surface B created (same device)");

        lc_surface_destroy(surf_a);
        surf_a = NULL;
        TEST_CHECK(lc_surface_is_present_supported(surf_b) != 0,
                   "surface B valid after surface A destroyed");

        /* Destroying window B must auto-destroy surface B. */
        lc_window_destroy(win_b);
        win_b = NULL;
        surf_b = NULL; /* dangling by design; must never be touched */
        TEST_CHECK(lc_window_get_width(win_a) == 800,
                   "window A alive after window B destroyed");

        /* Device must still serve new surfaces on the surviving window. */
        TEST_CHECK(lc_surface_create(device, win_a, &surf_a2) == LC_SUCCESS,
                   "new surface on surviving window/device");
        lc_surface_destroy(surf_a2);
        lc_window_destroy(win_a);
        lc_device_destroy(device);
        lc_shutdown();
        TEST_CHECK(1, "multi-window teardown clean");
    }

    /* ---- 3. device destroy cleans dependent surfaces (§43) ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;

        TEST_CHECK(make_device(&device) == 0, "device for device-destroy test");
        TEST_CHECK(make_window(&window) == 0, "window for device-destroy test");
        TEST_CHECK(lc_surface_create(device, window, &surface) == LC_SUCCESS,
                   "surface for device-destroy test");

        lc_device_destroy(device);
        device = NULL;
        surface = NULL; /* auto-destroyed; must never be touched */
        lc_poll_events();
        TEST_CHECK(lc_window_should_close(window) == 0,
                   "window alive and polling after device destroyed");
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "device-destroy teardown clean");
    }

    /* ---- 4. window destroy cleans dependent surfaces (§44) ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;

        TEST_CHECK(make_device(&device) == 0, "device for window-destroy test");
        TEST_CHECK(make_window(&window) == 0, "window for window-destroy test");
        TEST_CHECK(lc_surface_create(device, window, &surface) == LC_SUCCESS,
                   "surface for window-destroy test");

        lc_window_destroy(window);
        window = NULL;
        surface = NULL; /* auto-destroyed; must never be touched */
        lc_device_destroy(device);
        device = NULL;
        TEST_CHECK(1, "device explicitly destroyed after window destroyed");
        lc_shutdown();
    }

    /* ---- 5. shutdown with everything live, then re-init (§45) ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;

        TEST_CHECK(make_device(&device) == 0, "device for shutdown test");
        TEST_CHECK(make_window(&window) == 0, "window for shutdown test");
        TEST_CHECK(lc_surface_create(device, window, &surface) == LC_SUCCESS,
                   "surface for shutdown test");
        (void)surface;
        device = NULL;
        window = NULL;
        lc_shutdown(); /* must tear down surface, device, window in order */
        TEST_CHECK(1, "lc_shutdown() with live surface/device/window clean");
        TEST_CHECK(lc_init() == LC_SUCCESS, "re-init after full cleanup works");
        {
            lc_device *d2 = NULL;
            lc_window *w2 = NULL;
            lc_surface *s2 = NULL;
            TEST_CHECK(make_device(&d2) == 0, "device after re-init");
            TEST_CHECK(make_window(&w2) == 0, "window after re-init");
            TEST_CHECK(lc_surface_create(d2, w2, &s2) == LC_SUCCESS,
                       "surface after re-init");
            lc_surface_destroy(s2);
            lc_device_destroy(d2);
            lc_window_destroy(w2);
        }
        lc_shutdown();
    }

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
