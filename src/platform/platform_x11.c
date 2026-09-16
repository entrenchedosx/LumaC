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
#include <string.h>

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>

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
    XSelectInput(s_display, xw,
                 ExposureMask | StructureNotifyMask |
                 KeyPressMask | KeyReleaseMask |
                 ButtonPressMask | ButtonReleaseMask |
                 PointerMotionMask | FocusChangeMask |
                 EnterWindowMask | LeaveWindowMask);
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

/* Phase 27: translate an X11 KeySym to a backend-neutral physical
 * lc_keycode. Letters/digits arrive layout-mapped (XK_a..XK_z);
 * treat lowercase+uppercase identically (physical identity). */
static lc_keycode lc_x11_translate_key(unsigned long keysym) {
    if (keysym >= XK_a && keysym <= XK_z) {
        return (lc_keycode)((unsigned int)LC_KEY_A +
                            (keysym - XK_a));
    }
    if (keysym >= XK_A && keysym <= XK_Z) {
        return (lc_keycode)((unsigned int)LC_KEY_A +
                            (keysym - XK_A));
    }
    if (keysym >= XK_0 && keysym <= XK_9) {
        return (lc_keycode)((unsigned int)LC_KEY_0 +
                            (keysym - XK_0));
    }
    if (keysym >= XK_F1 && keysym <= XK_F12) {
        return (lc_keycode)((unsigned int)LC_KEY_F1 +
                            (keysym - XK_F1));
    }
    if (keysym >= XK_KP_0 && keysym <= XK_KP_9) {
        return (lc_keycode)((unsigned int)LC_KEY_NUMPAD_0 +
                            (keysym - XK_KP_0));
    }
    switch (keysym) {
    case XK_Escape: return LC_KEY_ESCAPE;
    case XK_Return:
    case XK_KP_Enter:
        return (keysym == XK_KP_Enter) ? LC_KEY_NUMPAD_ENTER
                                       : LC_KEY_ENTER;
    case XK_Tab:
    case XK_KP_Tab:
        return LC_KEY_TAB;
    case XK_space: return LC_KEY_SPACE;
    case XK_BackSpace: return LC_KEY_BACKSPACE;
    case XK_Shift_L: return LC_KEY_LEFT_SHIFT;
    case XK_Shift_R: return LC_KEY_RIGHT_SHIFT;
    case XK_Control_L: return LC_KEY_LEFT_CONTROL;
    case XK_Control_R: return LC_KEY_RIGHT_CONTROL;
    case XK_Alt_L:
    case XK_Meta_L:
        return LC_KEY_LEFT_ALT;
    case XK_Alt_R:
    case XK_Meta_R:
        return LC_KEY_RIGHT_ALT;
    case XK_Super_L: return LC_KEY_LEFT_SUPER;
    case XK_Super_R: return LC_KEY_RIGHT_SUPER;
    case XK_Left: return LC_KEY_LEFT;
    case XK_Right: return LC_KEY_RIGHT;
    case XK_Up: return LC_KEY_UP;
    case XK_Down: return LC_KEY_DOWN;
    case XK_Insert:
    case XK_KP_Insert:
        return LC_KEY_INSERT;
    case XK_Delete:
    case XK_KP_Delete:
        return LC_KEY_DELETE;
    case XK_Home:
    case XK_KP_Home:
        return LC_KEY_HOME;
    case XK_End:
    case XK_KP_End:
        return LC_KEY_END;
    case XK_Page_Up:
    case XK_KP_Page_Up:
        return LC_KEY_PAGE_UP;
    case XK_Page_Down:
    case XK_KP_Page_Down:
        return LC_KEY_PAGE_DOWN;
    case XK_KP_Decimal: return LC_KEY_NUMPAD_DECIMAL;
    case XK_KP_Divide: return LC_KEY_NUMPAD_DIVIDE;
    case XK_KP_Multiply: return LC_KEY_NUMPAD_MULTIPLY;
    case XK_KP_Subtract: return LC_KEY_NUMPAD_SUBTRACT;
    case XK_KP_Add: return LC_KEY_NUMPAD_ADD;
    case XK_KP_Equal: return LC_KEY_NUMPAD_EQUAL;
    case XK_minus: return LC_KEY_MINUS;
    case XK_equal: return LC_KEY_EQUAL;
    case XK_bracketleft: return LC_KEY_LEFT_BRACKET;
    case XK_bracketright: return LC_KEY_RIGHT_BRACKET;
    case XK_backslash: return LC_KEY_BACKSLASH;
    case XK_semicolon: return LC_KEY_SEMICOLON;
    case XK_apostrophe: return LC_KEY_APOSTROPHE;
    case XK_grave: return LC_KEY_GRAVE;
    case XK_comma:
    case XK_KP_Separator:
        return LC_KEY_COMMA;
    case XK_period: return LC_KEY_PERIOD;
    case XK_slash: return LC_KEY_SLASH;
    case XK_Caps_Lock: return LC_KEY_CAPS_LOCK;
    default: break;
    }
    return LC_KEY_UNKNOWN;
}

static uint32_t lc_x11_query_mods(unsigned int xstate) {
    uint32_t mods = (uint32_t)LC_MOD_NONE;

    if ((xstate & ShiftMask) != 0) {
        mods |= (uint32_t)LC_MOD_SHIFT;
    }
    if ((xstate & ControlMask) != 0) {
        mods |= (uint32_t)LC_MOD_CONTROL;
    }
    if ((xstate & Mod1Mask) != 0) {
        mods |= (uint32_t)LC_MOD_ALT;
    }
    if ((xstate & Mod4Mask) != 0) {
        mods |= (uint32_t)LC_MOD_SUPER;
    }
    return mods;
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
        case KeyPress:
        case KeyRelease:
            evw = (unsigned long)ev.xkey.window;
            break;
        case ButtonPress:
        case ButtonRelease:
            evw = (unsigned long)ev.xbutton.window;
            break;
        case MotionNotify:
            evw = (unsigned long)ev.xmotion.window;
            break;
        case FocusIn:
        case FocusOut:
            evw = (unsigned long)ev.xfocus.window;
            break;
        case EnterNotify:
        case LeaveNotify:
            evw = (unsigned long)ev.xcrossing.window;
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
                lc_window_event qev;

                target->should_close = 1;
                memset(&qev, 0, sizeof(qev));
                qev.type = LC_EVENT_CLOSE;
                qev.window = target;
                lc_window_push_event(target, &qev);
            }
        } else if (ev.type == ConfigureNotify) {
            if (ev.xconfigure.width > 0) {
                target->width = (uint32_t)ev.xconfigure.width;
            }
            if (ev.xconfigure.height > 0) {
                target->height = (uint32_t)ev.xconfigure.height;
            }
            /* Zero-size ConfigureNotify (minimize path) still
             * reports so the engine can apply its policy. */
            {
                lc_window_event qev;

                memset(&qev, 0, sizeof(qev));
                qev.type = LC_EVENT_RESIZE;
                qev.window = target;
                qev.width = target->width;
                qev.height = target->height;
                lc_window_push_event(target, &qev);
            }
        } else if (ev.type == DestroyNotify) {
            /* Window was destroyed outside our API; flag close and forget
             * the id so lc_platform_destroy() will not double-destroy. */
            target->should_close = 1;
            target->xwindow = 0;
        } else if (ev.type == KeyPress || ev.type == KeyRelease) {
            lc_window_event qev;
            KeySym sym;
            char tmp[8];
            int n;

            memset(&qev, 0, sizeof(qev));
            qev.type = (ev.type == KeyPress) ? LC_EVENT_KEY_DOWN
                                             : LC_EVENT_KEY_UP;
            qev.window = target;
            sym = XLookupKeysym(&ev.xkey, 0);
            qev.key = lc_x11_translate_key((unsigned long)sym);
            qev.repeat = 0; /* X11 has no repeat bit; engine
                             * derives repeat from held state. */
            qev.mods = lc_x11_query_mods(ev.xkey.state);
            lc_window_push_event(target, &qev);
            /* Text: printable KeyPress also yields CHAR (ASCII
             * fast path; full IME stays deferred per plan). */
            if (ev.type == KeyPress) {
                n = XLookupString(&ev.xkey, tmp, (int)sizeof(tmp),
                                  NULL, NULL);
                if (n == 1 && tmp[0] >= 32) {
                    lc_window_event cev;

                    memset(&cev, 0, sizeof(cev));
                    cev.type = LC_EVENT_CHAR;
                    cev.window = target;
                    cev.mods = qev.mods;
                    cev.utf8[0] = tmp[0];
                    cev.utf8[1] = '\0';
                    cev.utf8_len = 1;
                    lc_window_push_event(target, &cev);
                }
            }
        } else if (ev.type == ButtonPress ||
                   ev.type == ButtonRelease) {
            /* Buttons 4-7 are wheel on X11: translate to WHEEL,
             * not button events. */
            if (ev.xbutton.button >= 4 &&
                ev.xbutton.button <= 7) {
                if (ev.type == ButtonPress) {
                    lc_window_event qev;

                    memset(&qev, 0, sizeof(qev));
                    qev.type = LC_EVENT_MOUSE_WHEEL;
                    qev.window = target;
                    if (ev.xbutton.button == 4) {
                        qev.wheel_y = 1.0f;
                    } else if (ev.xbutton.button == 5) {
                        qev.wheel_y = -1.0f;
                    } else if (ev.xbutton.button == 6) {
                        qev.wheel_x = -1.0f;
                    } else {
                        qev.wheel_x = 1.0f;
                    }
                    qev.mods =
                        lc_x11_query_mods(ev.xbutton.state);
                    lc_window_push_event(target, &qev);
                }
            } else {
                lc_window_event qev;

                memset(&qev, 0, sizeof(qev));
                qev.type = (ev.type == ButtonPress)
                               ? LC_EVENT_MOUSE_DOWN
                               : LC_EVENT_MOUSE_UP;
                qev.window = target;
                switch (ev.xbutton.button) {
                case 1: qev.button = LC_MOUSE_LEFT; break;
                case 2: qev.button = LC_MOUSE_MIDDLE; break;
                case 3: qev.button = LC_MOUSE_RIGHT; break;
                case 8: qev.button = LC_MOUSE_4; break;
                case 9: qev.button = LC_MOUSE_5; break;
                default: qev.button = LC_MOUSE_LEFT; break;
                }
                qev.mouse_x = (float)ev.xbutton.x;
                qev.mouse_y = (float)ev.xbutton.y;
                qev.mods = lc_x11_query_mods(ev.xbutton.state);
                target->last_mouse_x = ev.xbutton.x;
                target->last_mouse_y = ev.xbutton.y;
                target->have_mouse_pos = 1;
                lc_window_push_event(target, &qev);
            }
        } else if (ev.type == MotionNotify) {
            lc_window_event qev;

            memset(&qev, 0, sizeof(qev));
            qev.type = LC_EVENT_MOUSE_MOVE;
            qev.window = target;
            qev.mouse_x = (float)ev.xmotion.x;
            qev.mouse_y = (float)ev.xmotion.y;
            qev.delta_x =
                (float)(ev.xmotion.x - target->last_mouse_x);
            qev.delta_y =
                (float)(ev.xmotion.y - target->last_mouse_y);
            if (!target->have_mouse_pos) {
                qev.delta_x = 0.0f;
                qev.delta_y = 0.0f;
            }
            qev.mods = lc_x11_query_mods(ev.xmotion.state);
            target->last_mouse_x = ev.xmotion.x;
            target->last_mouse_y = ev.xmotion.y;
            target->have_mouse_pos = 1;
            lc_window_push_event(target, &qev);
        } else if (ev.type == FocusIn || ev.type == FocusOut) {
            lc_window_event qev;

            memset(&qev, 0, sizeof(qev));
            qev.type = (ev.type == FocusIn) ? LC_EVENT_FOCUS_GAINED
                                            : LC_EVENT_FOCUS_LOST;
            qev.window = target;
            lc_window_push_event(target, &qev);
        }
    }
}
