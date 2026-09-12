#ifndef LUMAC_PLATFORM_H
#define LUMAC_PLATFORM_H

/*
 * Internal platform interface (Phase 2).
 *
 * Platform-neutral logic (argument validation, init checks, allocation,
 * window-list tracking, getters) lives in src/window.c.
 * Only native OS work lives in platform_win32.c / platform_x11.c.
 *
 * Each backend implements the three lc_platform_* functions below.
 * Native headers are included here only (never in the public header).
 */

#include "internal/lumac_internal.h"

#if defined(_WIN32) || defined(_WIN64)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <stdint.h>
#endif

#if defined(_WIN32) || defined(_WIN64)

/* Win32 window. Class is registered once per process; see platform_win32.c. */
struct lc_window {
    uint32_t width;   /* client-area width in pixels */
    uint32_t height;  /* client-area height in pixels */
    int should_close; /* set by WM_CLOSE */
    lc_window *next;
    lc_window *prev;
    HWND hwnd;
};

#else /* Linux/X11 - platform_x11.c defines native behavior */

/* Forward-declare X11 types without including Xlib here so that
 * src/window.c stays free of X11 headers. The backend .c file includes
 * <X11/Xlib.h> and treats xwindow as an opaque native id. */
struct lc_window {
    uint32_t width;   /* drawable-area width in pixels */
    uint32_t height;  /* drawable-area height in pixels */
    int should_close; /* set by WM_DELETE_WINDOW / DestroyNotify */
    lc_window *next;
    lc_window *prev;
    unsigned long xwindow; /* Window id (0 = none) */
};

#endif

/*
 * Opaque native window handles for backend use (e.g. Vulkan surface
 * creation). Native types never leave this header: on X11 the display
 * is carried as void* so translation units without Xlib stay clean;
 * backend .c files cast it back to Display*.
 */
typedef struct lc_native_window_handle {
#if defined(_WIN32) || defined(_WIN64)
    HINSTANCE hinstance;
    HWND hwnd;
#else
    void *display;        /* X11 Display* owned by the platform layer */
    unsigned long window; /* X11 Window id */
#endif
} lc_native_window_handle;

/*
 * Create the native OS window for an already-allocated lc_window.
 * title is never NULL (window.c substitutes LC_DEFAULT_TITLE).
 * width/height are non-zero client-area dimensions.
 * On success the backend sets native handles and may refine
 * width/height to the actual client area.
 */
lc_result lc_platform_create(lc_window *window, const char *title,
                             uint32_t width, uint32_t height);

/*
 * Fill out with the native handles of a fully-created window.
 * Fails safely for NULL arguments or windows without native handles.
 */
lc_result lc_platform_get_native_handle(const lc_window *window,
                                        lc_native_window_handle *out);

/* Destroy the native OS window. Safe with partially-created windows. */
void lc_platform_destroy(lc_window *window);

/* Pump pending OS events for all windows (non-blocking). */
void lc_platform_poll(void);

#endif /* LUMAC_PLATFORM_H */
