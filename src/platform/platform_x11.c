/*
 * X11 platform backend (Phase 2, Linux only).
 *
 * - One shared Display opened on first window creation, closed when the
 *   last window is destroyed. Owned entirely by this file (statics below).
 * - WM_DELETE_WINDOW protocol is used so the close button sets
 *   should_close instead of killing the process.
 * - Title uses XStoreName (locale encoding); full UTF-8 is NOT guaranteed.
 *   Documented as a current limitation.
 * - Main-thread use only.
 */

#include <stddef.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include "platform/platform.h"

/* Shared display state. s_window_count tracks live native windows so the
 * display is closed exactly when the last one goes away (including
 * lc_shutdown() auto-cleanup, which funnels through lc_platform_destroy). */
static Display *s_display = NULL;
static Atom s_wm_delete = None;
static int s_window_count = 0;

static lc_window *lc_x11_find(unsigned long xwindow) {
    lc_state *state = lc_get_internal_state();
    lc_window *it;

    if (state == NULL || xwindow == 0) {
        return NULL;
    }
    for (it = state->windows; it != NULL; it = it->next) {
        if (it->xwindow == xwindow) {
            return it;
        }
    }
    return NULL;
}

lc_result lc_platform_create(lc_window *window, const char *title,
                             uint32_t width, uint32_t height) {
    int screen;
    Window root;
    Window xw;
    const char *name;

    if (window == NULL || title == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (width == 0 || height == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    if (s_display == NULL) {
        s_display = XOpenDisplay(NULL);
        if (s_display == NULL) {
            return LC_ERROR_PLATFORM;
        }
        s_wm_delete = XInternAtom(s_display, "WM_DELETE_WINDOW", False);
    }

    screen = DefaultScreen(s_display);
    root = RootWindow(s_display, screen);

    xw = XCreateSimpleWindow(s_display, root,
                             0, 0,
                             (unsigned int)width, (unsigned int)height,
                             1,
                             BlackPixel(s_display, screen),
                             WhitePixel(s_display, screen));
    if (xw == 0) {
        if (s_window_count == 0) {
            XCloseDisplay(s_display);
            s_display = NULL;
            s_wm_delete = None;
        }
        return LC_ERROR_WINDOW_CREATION_FAILED;
    }

    name = (title != NULL) ? title : "LumaC";
    XStoreName(s_display, xw, name);
    XSelectInput(s_display, xw, ExposureMask | StructureNotifyMask);
    XSetWMProtocols(s_display, xw, &s_wm_delete, 1);
    XMapWindow(s_display, xw);
    XFlush(s_display);

    window->xwindow = (unsigned long)xw;
    s_window_count++;
    return LC_SUCCESS;
}

lc_result lc_platform_get_native_handle(const lc_window *window,
                                            lc_native_window_handle *out) {
    if (window == NULL || out == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Reuses the platform layer's shared display connection; no separate
     * Display is opened for graphics use. */
    if (window->xwindow == 0 || s_display == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    out->display = (void *)s_display;
    out->window = window->xwindow;
    return LC_SUCCESS;
}

void lc_platform_destroy(lc_window *window) {
    Window xw;

    if (window == NULL) {
        return;
    }
    if (window->xwindow == 0 || s_display == NULL) {
        window->xwindow = 0;
        return;
    }

    xw = (Window)window->xwindow;
    window->xwindow = 0;
    XDestroyWindow(s_display, xw);
    XFlush(s_display);

    s_window_count--;
    if (s_window_count <= 0) {
        s_window_count = 0;
        XCloseDisplay(s_display);
        s_display = NULL;
        s_wm_delete = None;
    }
}

void lc_platform_poll(void) {
    if (s_display == NULL) {
        return;
    }

    while (XPending(s_display) > 0) {
        XEvent ev;
        unsigned long evw = 0;
        lc_window *target;

        XNextEvent(s_display, &ev);

        switch (ev.type) {
        case ClientMessage:
            evw = (unsigned long)ev.xclient.window;
            break;
        case ConfigureNotify:
            evw = (unsigned long)ev.xconfigure.window;
            break;
        case DestroyNotify:
            evw = (unsigned long)ev.xdestroywindow.window;
            break;
        case MapNotify:
            evw = (unsigned long)ev.xmap.window;
            break;
        case Expose:
            evw = (unsigned long)ev.xexpose.window;
            break;
        default:
            continue;
        }

        target = lc_x11_find(evw);
        if (target == NULL) {
            continue;
        }

        if (ev.type == ClientMessage) {
            if ((Atom)ev.xclient.data.l[0] == s_wm_delete) {
                target->should_close = 1;
            }
        } else if (ev.type == ConfigureNotify) {
            if (ev.xconfigure.width > 0) {
                target->width = (uint32_t)ev.xconfigure.width;
            }
            if (ev.xconfigure.height > 0) {
                target->height = (uint32_t)ev.xconfigure.height;
            }
        } else if (ev.type == DestroyNotify) {
            /* Window was destroyed outside our API; flag close and forget
             * the id so lc_platform_destroy() will not double-destroy. */
            target->should_close = 1;
            target->xwindow = 0;
        }
    }
}
