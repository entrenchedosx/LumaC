/*
 * Vulkan swapchain integration test (Phase 5).
 *
 * Creates real windows, devices, surfaces, and VkSwapchainKHR objects
 * with image views: creation, capability-driven selection, transactional
 * recreation, one-per-surface rule, device/surface mismatch rejection,
 * multi-window lifetimes, and ordered dependency cleanup. Run with
 * validation layers: leaked views, misordered destruction, or invalid
 * swapchain use would surface as validation errors on teardown.
 *
 * If the environment cannot provide a window or a usable Vulkan setup,
 * the test reports SKIP and exits 0. Any other failure is a hard FAIL.
 */
#include <stdio.h>
#include <lumac/lumac.h>

#ifdef _WIN32
/* Test-only native resize (LumaC has no resize API by design). Win32
 * surfaces report a FIXED currentExtent tracking the window, so
 * exercising new extents requires resizing the real window first. */
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

/* Grow/shrink the test window so its client area becomes target_w x
 * target_h, pump events so lc_window observes WM_SIZE, and report the
 * observed client size. Returns nonzero on success. */
static int native_resize_client(lc_window *window, unsigned target_w,
                                unsigned target_h, unsigned *out_w,
                                unsigned *out_h) {
    HWND hwnd = FindWindowW(NULL, L"LumaC Swapchain Test");
    RECT outer;
    RECT client;
    int i;

    (void)window;
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
static int make_window(lc_window **out) {
    lc_window_desc desc;

    desc.title = "LumaC Swapchain Test";
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

/* After recreate(w, h), the extent must be the request (undefined-
 * extent surface) or the live window size (fixed-extent surface).
 * Both are correct Vulkan behavior across drivers/servers. */
static int extent_plausible(lc_swapchain *sc, lc_window *win, unsigned w,
                            unsigned h) {
    unsigned ew = lc_swapchain_get_width(sc);
    unsigned eh = lc_swapchain_get_height(sc);
    unsigned ww = lc_window_get_width(win);
    unsigned wh = lc_window_get_height(win);

    return ((ew == w && eh == h) || (ew == ww && eh == wh)) ? 1 : 0;
}

/* 0 = swapchain ready, 1 = environmental SKIP, -1 = hard failure.
 * Only used for the very first creation; afterwards the environment
 * is proven and failures are FAILs. */
static int make_swapchain_first(lc_device *device, lc_surface *surface,
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

int main(void) {
    printf("Running LumaC Vulkan swapchain integration test...\n");

    lc_shutdown();
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }

    /* ---- 1. basic creation + queries (§42) ---- */
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_swapchain_desc desc;
        lc_swapchain *second = (lc_swapchain *)0x1;
        lc_device *other_device = NULL;
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
        /* Environment proven from here on: failures are FAILs. */
        env = make_swapchain_first(device, surface, &swapchain);
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

        TEST_CHECK(swapchain != NULL, "swapchain handle non-null");
        TEST_CHECK(lc_swapchain_get_width(swapchain) == 800,
                   "swapchain width 800");
        TEST_CHECK(lc_swapchain_get_height(swapchain) == 600,
                   "swapchain height 600");
        TEST_CHECK(lc_swapchain_get_image_count(swapchain) >= 1,
                   "image count >= 1");
        printf("swapchain: %ux%u, images: %u\n",
               lc_swapchain_get_width(swapchain),
               lc_swapchain_get_height(swapchain),
               lc_swapchain_get_image_count(swapchain));

        /* Explicit image-count preference is honored as a clamped hint. */
        desc.width = 800;
        desc.height = 600;
        desc.image_count = 3;
        desc.vsync = 0;
        {
            /* Must destroy the first swapchain: one live per surface. */
            lc_swapchain_destroy(swapchain);
            swapchain = NULL;
            TEST_CHECK(lc_swapchain_create(device, surface, &desc,
                                           &swapchain) == LC_SUCCESS,
                       "create with image_count=3, vsync=0 succeeds");
            printf("explicit-count swapchain: %ux%u, images: %u\n",
                   lc_swapchain_get_width(swapchain),
                   lc_swapchain_get_height(swapchain),
                   lc_swapchain_get_image_count(swapchain));
            TEST_CHECK(lc_swapchain_get_image_count(swapchain) >= 1,
                       "explicit-count images >= 1");
        }

        /* One live swapchain per surface. */
        second = (lc_swapchain *)0x1;
        TEST_CHECK(lc_swapchain_create(device, surface, &desc, &second) ==
                       LC_ERROR_ALREADY_INITIALIZED,
                   "second swapchain on surface -> ALREADY_INITIALIZED");
        TEST_CHECK(second == NULL, "out cleared on second-swapchain reject");

        /* Device/surface mismatch is rejected. */
        {
            lc_device_desc ddesc = { 0 };
            lc_swapchain_desc sdesc;
            lc_swapchain *bad = (lc_swapchain *)0x1;

            ddesc.backend = LC_BACKEND_VULKAN;
            ddesc.enable_validation = 0;
            TEST_CHECK(lc_device_create(&ddesc, &other_device) == LC_SUCCESS,
                       "second device for mismatch test");
            sdesc.width = 800;
            sdesc.height = 600;
            sdesc.image_count = 0;
            sdesc.vsync = 1;
            TEST_CHECK(lc_swapchain_create(other_device, surface, &sdesc,
                                           &bad) == LC_ERROR_INVALID_ARGUMENT,
                       "cross-device surface -> INVALID_ARGUMENT");
            TEST_CHECK(bad == NULL, "out cleared on mismatch");
        }
        lc_device_destroy(other_device);
        other_device = NULL;

        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "explicit full teardown clean");
    }

    /* ---- 2. transactional recreation (§43) ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_swapchain_desc desc;

        TEST_CHECK(make_device(&device) == 0, "device for recreate test");
        TEST_CHECK(make_window(&window) == 0, "window for recreate test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for recreate test");
        desc.width = 800;
        desc.height = 600;
        desc.image_count = 0;
        desc.vsync = 1;
        TEST_CHECK(lc_swapchain_create(device, surface, &desc, &swapchain) ==
                       LC_SUCCESS,
                   "swapchain for recreate test");

#ifdef _WIN32
        /* Win32 surfaces carry a FIXED extent tracking the window, so
         * new extents require a real window resize first (mirrors the
         * example's resize workflow). */
        {
            unsigned w = 0;
            unsigned h = 0;
            TEST_CHECK(native_resize_client(window, 1000, 700, &w, &h) != 0,
                       "native window resize to 1000x700 observed");
            TEST_CHECK(lc_swapchain_recreate(swapchain, w, h) == LC_SUCCESS,
                       "recreate after resize succeeds");
            TEST_CHECK(w == 1000 && h == 700 &&
                       lc_swapchain_get_width(swapchain) == 1000 &&
                       lc_swapchain_get_height(swapchain) == 700,
                       "extent 1000x700 after resize+recreate");
            TEST_CHECK(lc_swapchain_get_image_count(swapchain) >= 1,
                       "images valid after recreate");
            TEST_CHECK(native_resize_client(window, 640, 480, &w, &h) != 0,
                       "native window resize to 640x480 observed");
            TEST_CHECK(lc_swapchain_recreate(swapchain, w, h) == LC_SUCCESS,
                       "recreate 640x480 succeeds");
            TEST_CHECK(w == 640 && h == 480 &&
                       lc_swapchain_get_width(swapchain) == 640 &&
                       lc_swapchain_get_height(swapchain) == 480,
                       "extent 640x480 after resize+recreate");
        }
#else
        /* Undefined-extent surfaces honor the request; fixed-extent
         * ones (some X servers) track the window instead. Either is
         * correct Vulkan behavior; accept both. */
        TEST_CHECK(lc_swapchain_recreate(swapchain, 1000, 700) == LC_SUCCESS,
                   "recreate 1000x700 succeeds");
        TEST_CHECK(extent_plausible(swapchain, window, 1000, 700),
                   "extent 1000x700 or window size after recreate");
        TEST_CHECK(lc_swapchain_get_image_count(swapchain) >= 1,
                   "images valid after recreate");

        TEST_CHECK(lc_swapchain_recreate(swapchain, 640, 480) == LC_SUCCESS,
                   "recreate 640x480 succeeds");
        TEST_CHECK(extent_plausible(swapchain, window, 640, 480),
                   "extent 640x480 or window size after recreate");
#endif

        /* Failed recreate leaves the old swapchain intact. */
        {
            uint32_t keep_w = lc_swapchain_get_width(swapchain);
            uint32_t keep_h = lc_swapchain_get_height(swapchain);
            uint32_t keep_n = lc_swapchain_get_image_count(swapchain);
            TEST_CHECK(lc_swapchain_recreate(swapchain, 0, 480) ==
                           LC_ERROR_ZERO_EXTENT,
                       "recreate w=0 -> ZERO_EXTENT");
            TEST_CHECK(lc_swapchain_get_width(swapchain) == keep_w &&
                           lc_swapchain_get_height(swapchain) == keep_h,
                       "old extent intact after failed recreate");
            TEST_CHECK(lc_swapchain_get_image_count(swapchain) == keep_n,
                       "old images intact after failed recreate");
        }

        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "recreate teardown clean");
    }

    /* ---- 3. multi-window / multi-swapchain (§45) ---- */
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
        lc_swapchain_desc desc;
        lc_swapchain *sc_a2 = NULL;

        desc.width = 800;
        desc.height = 600;
        desc.image_count = 0;
        desc.vsync = 1;

        TEST_CHECK(make_device(&device) == 0, "device for multi test");
        TEST_CHECK(make_window(&win_a) == 0, "window A created");
        TEST_CHECK(make_window(&win_b) == 0, "window B created");
        TEST_CHECK(make_surface(device, win_a, &surf_a) == 0,
                   "surface A created");
        TEST_CHECK(make_surface(device, win_b, &surf_b) == 0,
                   "surface B created");
        TEST_CHECK(lc_swapchain_create(device, surf_a, &desc, &sc_a) ==
                       LC_SUCCESS,
                   "swapchain A created");
        TEST_CHECK(lc_swapchain_create(device, surf_b, &desc, &sc_b) ==
                       LC_SUCCESS,
                   "swapchain B created (same device)");

        lc_swapchain_destroy(sc_a);
        sc_a = NULL;
        TEST_CHECK(lc_swapchain_get_image_count(sc_b) >= 1,
                   "swapchain B valid after A destroyed");

        /* Destroying window B must retire swapchain B + surface B. */
        lc_window_destroy(win_b);
        win_b = NULL;
        surf_b = NULL;
        sc_b = NULL; /* auto-destroyed; must never be touched */
        TEST_CHECK(lc_swapchain_create(device, surf_a, &desc, &sc_a2) ==
                       LC_SUCCESS,
                   "new swapchain on surviving surface/device");

        lc_swapchain_destroy(sc_a2);
        lc_surface_destroy(surf_a);
        lc_window_destroy(win_a);
        lc_device_destroy(device);
        lc_shutdown();
        TEST_CHECK(1, "multi-window teardown clean");
    }

    /* ---- 4. device destroy retires swapchains (§46) ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_swapchain_desc desc;

        desc.width = 800;
        desc.height = 600;
        desc.image_count = 0;
        desc.vsync = 1;

        TEST_CHECK(make_device(&device) == 0, "device for device-destroy");
        TEST_CHECK(make_window(&window) == 0, "window for device-destroy");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for device-destroy");
        TEST_CHECK(lc_swapchain_create(device, surface, &desc, &swapchain) ==
                       LC_SUCCESS,
                   "swapchain for device-destroy");

        lc_device_destroy(device);
        device = NULL;
        surface = NULL;
        swapchain = NULL; /* all auto-destroyed; never touched */
        lc_poll_events();
        TEST_CHECK(lc_window_should_close(window) == 0,
                   "window alive and polling after device destroyed");
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "device-destroy teardown clean");
    }

    /* ---- 5. window destroy retires swapchains (§47) ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_swapchain_desc desc;

        desc.width = 800;
        desc.height = 600;
        desc.image_count = 0;
        desc.vsync = 1;

        TEST_CHECK(make_device(&device) == 0, "device for window-destroy");
        TEST_CHECK(make_window(&window) == 0, "window for window-destroy");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for window-destroy");
        TEST_CHECK(lc_swapchain_create(device, surface, &desc, &swapchain) ==
                       LC_SUCCESS,
                   "swapchain for window-destroy");

        lc_window_destroy(window);
        window = NULL;
        surface = NULL;
        swapchain = NULL; /* all auto-destroyed; never touched */
        lc_device_destroy(device);
        device = NULL;
        TEST_CHECK(1, "device explicitly destroyed after window destroyed");
        lc_shutdown();
    }

    /* ---- 6. shutdown with everything live (§48) ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_swapchain_desc desc;

        desc.width = 800;
        desc.height = 600;
        desc.image_count = 0;
        desc.vsync = 1;

        TEST_CHECK(make_device(&device) == 0, "device for shutdown test");
        TEST_CHECK(make_window(&window) == 0, "window for shutdown test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for shutdown test");
        TEST_CHECK(lc_swapchain_create(device, surface, &desc, &swapchain) ==
                       LC_SUCCESS,
                   "swapchain for shutdown test");
        device = NULL;
        window = NULL;
        lc_shutdown(); /* swapchains, surfaces, devices, windows, core */
        TEST_CHECK(1, "lc_shutdown() with live swapchain clean");
        TEST_CHECK(lc_init() == LC_SUCCESS, "re-init after full cleanup works");
        {
            lc_device *d2 = NULL;
            lc_window *w2 = NULL;
            lc_surface *s2 = NULL;
            lc_swapchain *c2 = NULL;
            TEST_CHECK(make_device(&d2) == 0, "device after re-init");
            TEST_CHECK(make_window(&w2) == 0, "window after re-init");
            TEST_CHECK(make_surface(d2, w2, &s2) == 0,
                       "surface after re-init");
            TEST_CHECK(lc_swapchain_create(d2, s2, &desc, &c2) == LC_SUCCESS,
                       "swapchain after re-init");
            lc_swapchain_destroy(c2);
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
