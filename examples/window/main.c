/* Must precede every system header: exposes nanosleep under strict
 * C11, where __STRICT_ANSI__ would otherwise hide it on Linux. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stdio.h>

#include <lumac/lumac.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
static void sleep_ms(unsigned int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static void sleep_ms(unsigned int ms) {
    struct timespec ts;
    ts.tv_sec = (long)(ms / 1000u);
    ts.tv_nsec = (long)((ms % 1000u) * 1000000u);
    nanosleep(&ts, NULL);
}
#endif

int main(void) {
    lc_window *window = NULL;
    lc_window_desc desc;
    uint32_t last_w;
    uint32_t last_h;

    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    desc.title = "LumaC Window Example";
    desc.width = 800;
    desc.height = 600;

    if (lc_window_create(&desc, &window) != LC_SUCCESS) {
        fprintf(stderr, "lc_window_create failed\n");
        lc_shutdown();
        return 1;
    }

    printf("LumaC window open (800x600). Close the window to exit.\n");
    last_w = lc_window_get_width(window);
    last_h = lc_window_get_height(window);

    while (!lc_window_should_close(window)) {
        lc_poll_events();

        {
            uint32_t w = lc_window_get_width(window);
            uint32_t h = lc_window_get_height(window);
            if (w != last_w || h != last_h) {
                printf("window resized: %u x %u\n", w, h);
                last_w = w;
                last_h = h;
            }
        }

        /* No rendering yet (Phase 2): yield to avoid a busy loop. */
        sleep_ms(16);
    }

    printf("close requested, shutting down\n");
    lc_window_destroy(window);
    lc_shutdown();
    return 0;
}
