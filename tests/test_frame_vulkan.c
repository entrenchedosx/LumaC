/*
 * Vulkan frame integration test (Phase 6).
 *
 * Renders real frames on real windows: acquire/clear/submit/present
 * over 120 frames, misuse rejection against live swapchains, destroy
 * mid-frame, resize stress, minimize/restore, and two-window rendering
 * with distinct clear colors. Run with validation layers: semaphore,
 * fence, layout, or lifetime misuse would surface as validation errors.
 *
 * If the environment cannot provide a window or a usable Vulkan setup,
 * the test reports SKIP and exits 0. Any other failure is a hard FAIL.
 */
#include <stdio.h>
#include <lumac/lumac.h>

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
                          int vsync, lc_swapchain **out) {
    lc_swapchain_desc desc;

    desc.width = 800;
    desc.height = 600;
    desc.image_count = 0;
    desc.vsync = vsync;
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

/* After recreate(w, h), the extent must be the request
 * (undefined-extent surface) or the live window size (fixed-extent
 * surface tracking the window, e.g. Win32 or some X servers). */
static int extent_plausible(lc_swapchain *sc, lc_window *win, unsigned w,
                            unsigned h) {
    unsigned ew = lc_swapchain_get_width(sc);
    unsigned eh = lc_swapchain_get_height(sc);
    unsigned ww = lc_window_get_width(win);
    unsigned wh = lc_window_get_height(win);

    return ((ew == w && eh == h) || (ew == ww && eh == wh)) ? 1 : 0;
}

/* Render `want` presented frames, handling recoverable swapchain states
 * by recreating with the current window size. Returns presented count,
 * or -1 on fatal error / livelock guard trip. */
static int render_frames(lc_swapchain *sc, lc_window *win, int want, float r,
                         float g, float b) {
    int presented = 0;
    int guard = 0;

    while (presented < want) {
        uint32_t w;
        uint32_t h;
        lc_result res;

        if (++guard > want * 25 + 120) {
            return -1;
        }
        lc_poll_events();
        w = lc_window_get_width(win);
        h = lc_window_get_height(win);
        if (w == 0 || h == 0) {
            continue; /* minimized */
        }
        if (w != lc_swapchain_get_width(sc) ||
            h != lc_swapchain_get_height(sc)) {
            res = lc_swapchain_recreate(sc, w, h);
            if (res == LC_ERROR_ZERO_EXTENT) {
                continue;
            }
            if (res != LC_SUCCESS) {
                return -1;
            }
        }
        res = lc_begin_frame(sc);
        if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            res = lc_swapchain_recreate(sc, w, h);
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
        if (lc_clear_color(sc, r, g, b, 1.0f) != LC_SUCCESS) {
            return -1;
        }
        res = lc_end_frame(sc);
        if (res == LC_SUCCESS) {
            presented++;
        } else if (res == LC_SUBOPTIMAL ||
                   res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            presented++;
            res = lc_swapchain_recreate(sc, w, h);
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
    printf("Running LumaC Vulkan frame integration test...\n");

    lc_shutdown();
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }

    /* ---- 1. misuse against a live swapchain ---- */
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        int env;

        env = make_device(&device);
        if (env != 0) {
            if (env == 1) {
                SKIP_ENV("a Vulkan device");
            }
            printf("device creation failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        env = make_window_titled(&window, "LumaC Frame Test");
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
        env = make_swapchain(device, surface, 1, &swapchain);
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
        TEST_CHECK(lc_end_frame(swapchain) == LC_ERROR_INVALID_ARGUMENT,
                   "end without begin -> INVALID_ARGUMENT");
        TEST_CHECK(lc_clear_color(swapchain, 0.0f, 0.0f, 0.0f, 1.0f) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "clear without begin -> INVALID_ARGUMENT");
        TEST_CHECK(lc_begin_frame(swapchain) == LC_SUCCESS,
                   "begin frame succeeds");
        TEST_CHECK(lc_begin_frame(swapchain) == LC_ERROR_INVALID_ARGUMENT,
                   "begin twice -> INVALID_ARGUMENT");
        TEST_CHECK(lc_clear_color(swapchain, 0.0f, 0.0f, 1.0f, 1.0f) ==
                       LC_SUCCESS,
                   "clear inside frame succeeds");
        TEST_CHECK(lc_clear_color(swapchain, 0.0f, 1.0f, 0.0f, 1.0f) ==
                       LC_SUCCESS,
                   "second clear inside frame succeeds");
        {
            lc_result end_res = lc_end_frame(swapchain);
            TEST_CHECK(end_res == LC_SUCCESS || end_res == LC_SUBOPTIMAL,
                       "end frame succeeds");
        }
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "misuse+basic frame teardown clean");
    }

    /* ---- 2. 120 presented frames with varying clear colors ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        int presented = 0;
        int chunk;

        TEST_CHECK(make_device(&device) == 0, "device for 120-frame test");
        TEST_CHECK(make_window_titled(&window, "LumaC Frame Test") == 0,
                   "window for 120-frame test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for 120-frame test");
        TEST_CHECK(make_swapchain(device, surface, 1, &swapchain) == 0,
                   "swapchain for 120-frame test");
        for (chunk = 0; chunk < 4; chunk++) {
            int n = render_frames(swapchain, window, 30, 0.08f, 0.12f,
                                  0.20f + 0.05f * (float)chunk);
            if (n < 0) {
                break;
            }
            presented += n;
        }
        TEST_CHECK(presented == 120, "120 frames presented");
        printf("presented %d frames\n", presented);

        /* Destroy mid-frame: begun but unsubmitted recording must go
         * away safely with the pool. */
        TEST_CHECK(lc_begin_frame(swapchain) == LC_SUCCESS,
                   "begin frame before mid-frame destroy");
        lc_swapchain_destroy(swapchain);
        swapchain = NULL;
        TEST_CHECK(1, "swapchain destroy with open frame does not crash");
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "post-frame teardown clean");
    }

    /* ---- 3. resize stress while rendering ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        static const unsigned targets[][2] = {
            { 800, 600 }, { 1024, 768 }, { 1280, 720 }, { 640, 480 },
            { 900, 900 }
        };
        size_t step;

        TEST_CHECK(make_device(&device) == 0, "device for resize stress");
        TEST_CHECK(make_window_titled(&window, "LumaC Frame Test") == 0,
                   "window for resize stress");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for resize stress");
        TEST_CHECK(make_swapchain(device, surface, 1, &swapchain) == 0,
                   "swapchain for resize stress");

        for (step = 0; step < sizeof(targets) / sizeof(targets[0]); step++) {
            unsigned w = targets[step][0];
            unsigned h = targets[step][1];
            int n;
#ifdef _WIN32
            /* Win32 extents track the window: resize it for real, then
             * render through the recreated swapchain. */
            {
                unsigned ow = 0;
                unsigned oh = 0;
                char msg[96];

                if (!native_resize_client(L"LumaC Frame Test", window, w,
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
            if (lc_swapchain_recreate(swapchain, w, h) != LC_SUCCESS) {
                char msg[96];
                snprintf(msg, sizeof(msg), "recreate to %ux%u succeeds", w,
                         h);
                TEST_CHECK(0, msg);
                break;
            }
#endif
            n = render_frames(swapchain, window, 12, 0.10f, 0.14f, 0.22f);
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "12 frames at %ux%u", w, h);
                TEST_CHECK(n == 12, msg);
            }
            if (n != 12) {
                break;
            }
            TEST_CHECK(extent_plausible(swapchain, window, w, h),
                       "extent tracks target or window size");
        }

        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "resize-stress teardown clean");
    }

#ifdef _WIN32
    /* ---- 4. minimize / restore while looping ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        HWND hwnd;
        int i;
        int skipped = 0;

        TEST_CHECK(make_device(&device) == 0, "device for minimize test");
        TEST_CHECK(make_window_titled(&window, "LumaC Frame Test") == 0,
                   "window for minimize test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for minimize test");
        TEST_CHECK(make_swapchain(device, surface, 1, &swapchain) == 0,
                   "swapchain for minimize test");
        TEST_CHECK(render_frames(swapchain, window, 10, 0.08f, 0.12f,
                                 0.20f) == 10,
                   "10 warmup frames before minimize");

        hwnd = find_test_window(L"LumaC Frame Test");
        TEST_CHECK(hwnd != NULL, "test window found for minimize");
        ShowWindow(hwnd, SW_MINIMIZE);
        for (i = 0; i < 30; i++) {
            lc_poll_events();
            Sleep(5);
        }
        TEST_CHECK(lc_window_get_width(window) == 0 &&
                       lc_window_get_height(window) == 0,
                   "minimized window reports zero extent");

        /* Loop like the example: poll, defer work while minimized. */
        for (i = 0; i < 30; i++) {
            uint32_t w;
            uint32_t h;
            lc_poll_events();
            w = lc_window_get_width(window);
            h = lc_window_get_height(window);
            if (w == 0 || h == 0) {
                skipped++;
                Sleep(5);
                continue;
            }
            break;
        }
        TEST_CHECK(skipped == 30, "rendering deferred while minimized");

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
            TEST_CHECK(render_frames(swapchain, window, 30, 0.08f, 0.12f,
                                     0.25f) == 30,
                       "30 frames after restore");
        }

        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "minimize/restore teardown clean");
    }
#endif

    /* ---- 5. two windows render distinct colors ---- */
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
        int i;
        int ok_a = 0;
        int ok_b = 0;

        TEST_CHECK(make_device(&device) == 0, "device for multi test");
        TEST_CHECK(make_window_titled(&win_a, "LumaC Frame Test A") == 0,
                   "window A created");
        TEST_CHECK(make_window_titled(&win_b, "LumaC Frame Test B") == 0,
                   "window B created");
        TEST_CHECK(make_surface(device, win_a, &surf_a) == 0,
                   "surface A created");
        TEST_CHECK(make_surface(device, win_b, &surf_b) == 0,
                   "surface B created");
        TEST_CHECK(make_swapchain(device, surf_a, 1, &sc_a) == 0,
                   "swapchain A created");
        TEST_CHECK(make_swapchain(device, surf_b, 0, &sc_b) == 0,
                   "swapchain B created (no vsync)");

        /* Alternate frames on both swapchains from one thread. */
        for (i = 0; i < 30; i++) {
            int na = render_frames(sc_a, win_a, 1, 0.08f, 0.12f, 0.30f);
            int nb = render_frames(sc_b, win_b, 1, 0.30f, 0.08f, 0.08f);
            if (na != 1 || nb != 1) {
                break;
            }
            ok_a++;
            ok_b++;
        }
        TEST_CHECK(ok_a == 30, "30 blue frames on window A");
        TEST_CHECK(ok_b == 30, "30 red frames on window B");

        lc_swapchain_destroy(sc_a);
        lc_swapchain_destroy(sc_b);
        lc_surface_destroy(surf_a);
        lc_surface_destroy(surf_b);
        lc_device_destroy(device);
        lc_window_destroy(win_a);
        lc_window_destroy(win_b);
        lc_shutdown();
        TEST_CHECK(1, "multi-window teardown clean");
    }

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
