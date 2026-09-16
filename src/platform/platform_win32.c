/*
 * Win32 platform backend (Phase 2).
 *
 * - One process-global window class, registered on first window creation
 *   and never unregistered (OS reclaims it at process exit). This keeps
 *   repeated lc_init()/lc_shutdown() cycles and multi-window use safe.
 * - Instance comes from GetModuleHandleW(NULL); the app never provides it.
 * - Titles are UTF-8 in the public API and converted to UTF-16 for the
 *   Unicode (W) Win32 API. Invalid UTF-8 falls back to "LumaC".
 * - WM_CLOSE only sets should_close; the app owns destruction via
 *   lc_window_destroy(). No exit() is ever called from the window proc.
 */

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
#include <stdlib.h>
#include <string.h>

#include "platform/platform.h"

static const wchar_t kClassName[] = L"LumaCWindowClass";
static int s_class_registered = 0;
static HINSTANCE s_hinstance = NULL;

/* Phase 27: translate a Win32 virtual key + scan-code lparam to a
 * backend-neutral physical lc_keycode. Left/right modifiers use bit
 * 24 of lparam (extended key): Shift is ambiguous in VK (report
 * LEFT; the engine sees either side through the mods mask too). */
static lc_keycode lc_win32_translate_key(unsigned int vk,
                                         unsigned int lparam) {
    unsigned int sc;
    int extended;

    if (vk >= (unsigned int)'A' && vk <= (unsigned int)'Z') {
        return (lc_keycode)((unsigned int)LC_KEY_A + (vk - 'A'));
    }
    if (vk >= (unsigned int)'0' && vk <= (unsigned int)'9') {
        return (lc_keycode)((unsigned int)LC_KEY_0 + (vk - '0'));
    }
    switch (vk) {
    case VK_ESCAPE: return LC_KEY_ESCAPE;
    case VK_RETURN: return LC_KEY_ENTER;
    case VK_TAB: return LC_KEY_TAB;
    case VK_SPACE: return LC_KEY_SPACE;
    case VK_BACK: return LC_KEY_BACKSPACE;
    case VK_LEFT: return LC_KEY_LEFT;
    case VK_RIGHT: return LC_KEY_RIGHT;
    case VK_UP: return LC_KEY_UP;
    case VK_DOWN: return LC_KEY_DOWN;
    case VK_INSERT: return LC_KEY_INSERT;
    case VK_DELETE: return LC_KEY_DELETE;
    case VK_HOME: return LC_KEY_HOME;
    case VK_END: return LC_KEY_END;
    case VK_PRIOR: return LC_KEY_PAGE_UP;
    case VK_NEXT: return LC_KEY_PAGE_DOWN;
    case VK_SHIFT: return LC_KEY_LEFT_SHIFT;
    case VK_CONTROL: return LC_KEY_LEFT_CONTROL;
    case VK_MENU: return LC_KEY_LEFT_ALT;
    case VK_LWIN: return LC_KEY_LEFT_SUPER;
    case VK_RWIN: return LC_KEY_RIGHT_SUPER;
    case VK_CAPITAL: return LC_KEY_CAPS_LOCK;
    case VK_OEM_MINUS: return LC_KEY_MINUS;
    case VK_OEM_PLUS: return LC_KEY_EQUAL;
    case VK_OEM_4: return LC_KEY_LEFT_BRACKET;
    case VK_OEM_6: return LC_KEY_RIGHT_BRACKET;
    case VK_OEM_5: return LC_KEY_BACKSLASH;
    case VK_OEM_1: return LC_KEY_SEMICOLON;
    case VK_OEM_7: return LC_KEY_APOSTROPHE;
    case VK_OEM_3: return LC_KEY_GRAVE;
    case VK_OEM_COMMA: return LC_KEY_COMMA;
    case VK_OEM_PERIOD: return LC_KEY_PERIOD;
    case VK_OEM_2: return LC_KEY_SLASH;
    default: break;
    }
    if (vk >= VK_F1 && vk <= VK_F12) {
        return (lc_keycode)((unsigned int)LC_KEY_F1 +
                            (vk - VK_F1));
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        return (lc_keycode)((unsigned int)LC_KEY_NUMPAD_0 +
                            (vk - VK_NUMPAD0));
    }
    switch (vk) {
    case VK_DECIMAL: return LC_KEY_NUMPAD_DECIMAL;
    case VK_DIVIDE: return LC_KEY_NUMPAD_DIVIDE;
    case VK_MULTIPLY: return LC_KEY_NUMPAD_MULTIPLY;
    case VK_SUBTRACT: return LC_KEY_NUMPAD_SUBTRACT;
    case VK_ADD: return LC_KEY_NUMPAD_ADD;
    case VK_SEPARATOR: return LC_KEY_NUMPAD_DECIMAL;
    default: break;
    }
    /* Distinguish numpad Enter (extended) from main Enter. */
    if (vk == VK_RETURN) {
        extended = ((lparam >> 24) & 1u) != 0;
        return extended ? LC_KEY_NUMPAD_ENTER : LC_KEY_ENTER;
    }
    /* Left/right Control/Alt via scan code (extended bit): fall
     * back to the generic VK mapping above when unknown. */
    sc = (lparam >> 16) & 0xFFu;
    extended = ((lparam >> 24) & 1u) != 0;
    if (vk == VK_SHIFT) {
        /* MapShiftEx resolves L/R Shift from the scan code. */
        {
            unsigned int any =
                MapVirtualKeyW(sc, MAPVK_VSC_TO_VK_EX);
            if (any == VK_RSHIFT) {
                return LC_KEY_RIGHT_SHIFT;
            }
        }
        return LC_KEY_LEFT_SHIFT;
    }
    if (vk == VK_CONTROL) {
        return extended ? LC_KEY_RIGHT_CONTROL
                        : LC_KEY_LEFT_CONTROL;
    }
    if (vk == VK_MENU) {
        return extended ? LC_KEY_RIGHT_ALT : LC_KEY_LEFT_ALT;
    }
    (void)sc;
    return LC_KEY_UNKNOWN;
}

/* Snapshot the async modifier state into lc_key_mod bits. */
static uint32_t lc_win32_query_mods(void) {
    uint32_t mods = (uint32_t)LC_MOD_NONE;

    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0) {
        mods |= (uint32_t)LC_MOD_SHIFT;
    }
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0) {
        mods |= (uint32_t)LC_MOD_CONTROL;
    }
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0) {
        mods |= (uint32_t)LC_MOD_ALT;
    }
    if (((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0) ||
        ((GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)) {
        mods |= (uint32_t)LC_MOD_SUPER;
    }
    return mods;
}

static lc_mouse_button lc_win32_translate_button(unsigned int msg,
                                                 unsigned int wparam) {
    (void)wparam;
    switch (msg) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        return LC_MOUSE_LEFT;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        return LC_MOUSE_RIGHT;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        return LC_MOUSE_MIDDLE;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        /* HIWORD(wparam): 1 = XBUTTON1 (side back), 2 = XBUTTON2. */
        if (HIWORD(wparam) == 2) {
            return LC_MOUSE_5;
        }
        return LC_MOUSE_4;
    default:
        return LC_MOUSE_LEFT;
    }
}

static wchar_t *lc_win32_utf8_to_wide(const char *utf8) {
    int needed;
    wchar_t *out;

    if (utf8 == NULL) {
        return NULL;
    }
    needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (needed <= 0) {
        return NULL;
    }
    out = (wchar_t *)malloc((size_t)needed * sizeof(wchar_t));
    if (out == NULL) {
        return NULL;
    }
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, out, needed) <= 0) {
        free(out);
        return NULL;
    }
    return out;
}

static LRESULT CALLBACK lc_win32_wndproc(HWND hwnd, UINT msg,
                                         WPARAM wparam, LPARAM lparam) {
    lc_window *window = NULL;

    if (msg == WM_NCCREATE) {
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lparam;
        window = (lc_window *)cs->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)window);
        if (window != NULL) {
            window->hwnd = hwnd;
        }
    } else {
        window = (lc_window *)(LONG_PTR)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    }

    switch (msg) {
    case WM_CLOSE:
        /* User clicked close: flag it, let the app destroy the window. */
        if (window != NULL) {
            lc_window_event ev;

            window->should_close = 1;
            memset(&ev, 0, sizeof(ev));
            ev.type = LC_EVENT_CLOSE;
            ev.window = window;
            lc_window_push_event(window, &ev);
        }
        return 0;
    case WM_SIZE:
        /* lParam holds the new client area: LOWORD=width, HIWORD=height. */
        if (window != NULL) {
            lc_window_event ev;

            window->width = (uint32_t)(uint16_t)LOWORD(lparam);
            window->height = (uint32_t)(uint16_t)HIWORD(lparam);
            memset(&ev, 0, sizeof(ev));
            ev.type = LC_EVENT_RESIZE;
            ev.window = window;
            ev.width = window->width;
            ev.height = window->height;
            lc_window_push_event(window, &ev);
        }
        break;
    case WM_SETFOCUS:
        if (window != NULL) {
            lc_window_event ev;

            memset(&ev, 0, sizeof(ev));
            ev.type = LC_EVENT_FOCUS_GAINED;
            ev.window = window;
            lc_window_push_event(window, &ev);
        }
        break;
    case WM_KILLFOCUS:
        if (window != NULL) {
            lc_window_event ev;

            memset(&ev, 0, sizeof(ev));
            ev.type = LC_EVENT_FOCUS_LOST;
            ev.window = window;
            lc_window_push_event(window, &ev);
        }
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (window != NULL) {
            lc_window_event ev;
            int is_down =
                (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN);
            int was_down = (((unsigned int)lparam >> 30) & 1u) != 0;

            memset(&ev, 0, sizeof(ev));
            ev.type = is_down ? LC_EVENT_KEY_DOWN : LC_EVENT_KEY_UP;
            ev.window = window;
            ev.key = lc_win32_translate_key((unsigned int)wparam,
                                            (unsigned int)lparam);
            ev.repeat = (is_down && was_down) ? 1 : 0;
            ev.mods = lc_win32_query_mods();
            lc_window_push_event(window, &ev);
        }
        break;
    case WM_CHAR:
        if (window != NULL && wparam >= 32) {
            lc_window_event ev;

            /* wparam is UTF-16; encode BMP scalars to UTF-8.
             * Lone surrogates are dropped (never mojibake). */
            if (wparam >= 0xD800 && wparam <= 0xDFFF) {
                break;
            }
            memset(&ev, 0, sizeof(ev));
            ev.type = LC_EVENT_CHAR;
            ev.window = window;
            ev.mods = lc_win32_query_mods();
            if (wparam < 0x80) {
                ev.utf8[0] = (char)wparam;
                ev.utf8_len = 1;
            } else if (wparam < 0x800) {
                ev.utf8[0] = (char)(0xC0 | (wparam >> 6));
                ev.utf8[1] = (char)(0x80 | (wparam & 0x3F));
                ev.utf8_len = 2;
            } else {
                ev.utf8[0] = (char)(0xE0 | (wparam >> 12));
                ev.utf8[1] = (char)(0x80 | ((wparam >> 6) & 0x3F));
                ev.utf8[2] = (char)(0x80 | (wparam & 0x3F));
                ev.utf8_len = 3;
            }
            ev.utf8[ev.utf8_len] = '\0';
            lc_window_push_event(window, &ev);
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_XBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
    case WM_MBUTTONUP:
    case WM_XBUTTONUP:
        if (window != NULL) {
            lc_window_event ev;
            int is_down = (msg == WM_LBUTTONDOWN ||
                           msg == WM_RBUTTONDOWN ||
                           msg == WM_MBUTTONDOWN ||
                           msg == WM_XBUTTONDOWN);

            memset(&ev, 0, sizeof(ev));
            ev.type =
                is_down ? LC_EVENT_MOUSE_DOWN : LC_EVENT_MOUSE_UP;
            ev.window = window;
            ev.button =
                lc_win32_translate_button(msg, (unsigned int)wparam);
            ev.mouse_x = (float)(int)(int16_t)LOWORD(lparam);
            ev.mouse_y = (float)(int)(int16_t)HIWORD(lparam);
            ev.mods = lc_win32_query_mods();
            lc_window_push_event(window, &ev);
        }
        break;
    case WM_MOUSEMOVE:
        if (window != NULL) {
            lc_window_event ev;
            int x = (int)(int16_t)LOWORD(lparam);
            int y = (int)(int16_t)HIWORD(lparam);

            memset(&ev, 0, sizeof(ev));
            ev.type = LC_EVENT_MOUSE_MOVE;
            ev.window = window;
            ev.mouse_x = (float)x;
            ev.mouse_y = (float)y;
            ev.delta_x = (float)(x - window->last_mouse_x);
            ev.delta_y = (float)(y - window->last_mouse_y);
            if (!window->have_mouse_pos) {
                ev.delta_x = 0.0f;
                ev.delta_y = 0.0f;
            }
            ev.mods = lc_win32_query_mods();
            window->last_mouse_x = x;
            window->last_mouse_y = y;
            window->have_mouse_pos = 1;
            lc_window_push_event(window, &ev);
        }
        break;
    case WM_MOUSEWHEEL:
        if (window != NULL) {
            lc_window_event ev;
            short ticks = (short)HIWORD(wparam);

            memset(&ev, 0, sizeof(ev));
            ev.type = LC_EVENT_MOUSE_WHEEL;
            ev.window = window;
            ev.wheel_y = (float)ticks / (float)WHEEL_DELTA;
            ev.mods = lc_win32_query_mods();
            lc_window_push_event(window, &ev);
        }
        break;
    case WM_MOUSEHWHEEL:
        if (window != NULL) {
            lc_window_event ev;
            short ticks = (short)HIWORD(wparam);

            memset(&ev, 0, sizeof(ev));
            ev.type = LC_EVENT_MOUSE_WHEEL;
            ev.window = window;
            ev.wheel_x = (float)ticks / (float)WHEEL_DELTA;
            ev.mods = lc_win32_query_mods();
            lc_window_push_event(window, &ev);
        }
        break;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)0);
        break;
    default:
        break;
    }

    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static lc_result lc_win32_ensure_class(void) {
    WNDCLASSEXW wc;

    if (s_class_registered) {
        return LC_SUCCESS;
    }

    s_hinstance = (HINSTANCE)GetModuleHandleW(NULL);
    if (s_hinstance == NULL) {
        return LC_ERROR_PLATFORM;
    }

    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc = lc_win32_wndproc;
    wc.cbClsExtra = 0;
    wc.cbWndExtra = 0;
    wc.hInstance = s_hinstance;
    wc.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszMenuName = NULL;
    wc.lpszClassName = kClassName;
    wc.hIconSm = LoadIconW(NULL, IDI_APPLICATION);

    if (RegisterClassExW(&wc) == 0) {
        /* If another LumaC instance in this process already registered the
         * class (e.g. DLL reload edge), treat ALREADY_EXISTS as success. */
        if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return LC_ERROR_WINDOW_CREATION_FAILED;
        }
    }

    s_class_registered = 1;
    return LC_SUCCESS;
}

lc_result lc_platform_create(lc_window *window, const char *title,
                             uint32_t width, uint32_t height) {
    const DWORD style = WS_OVERLAPPEDWINDOW;
    const DWORD ex_style = 0;
    RECT rect;
    int outer_w;
    int outer_h;
    wchar_t *wtitle = NULL;
    const wchar_t *wtitle_use;
    HWND hwnd;

    if (window == NULL || title == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (width == 0 || height == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    {
        lc_result res = lc_win32_ensure_class();
        if (res != LC_SUCCESS) {
            return res;
        }
    }

    /* Convert UTF-8 title; fall back to ASCII default on failure. */
    wtitle = lc_win32_utf8_to_wide(title);
    wtitle_use = (wtitle != NULL) ? (const wchar_t *)wtitle : L"LumaC";

    /* Size the outer window so the *client* area matches width x height. */
    rect.left = 0;
    rect.top = 0;
    rect.right = (LONG)width;
    rect.bottom = (LONG)height;
    if (AdjustWindowRectEx(&rect, style, FALSE, ex_style)) {
        outer_w = (int)(rect.right - rect.left);
        outer_h = (int)(rect.bottom - rect.top);
    } else {
        outer_w = (int)width;
        outer_h = (int)height;
    }

    hwnd = CreateWindowExW(ex_style, kClassName, wtitle_use, style,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           outer_w, outer_h,
                           NULL, NULL, s_hinstance, window);
    free(wtitle);

    if (hwnd == NULL) {
        return LC_ERROR_WINDOW_CREATION_FAILED;
    }

    window->hwnd = hwnd;

    /* Confirm actual client size (DPI/decoration clamping may adjust it). */
    {
        RECT client;
        if (GetClientRect(hwnd, &client)) {
            LONG cw = client.right - client.left;
            LONG ch = client.bottom - client.top;
            if (cw > 0 && ch > 0) {
                window->width = (uint32_t)cw;
                window->height = (uint32_t)ch;
            }
        }
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    return LC_SUCCESS;
}

lc_result lc_platform_get_native_handle(const lc_window *window,
                                            lc_native_window_handle *out) {
    if (window == NULL || out == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (window->hwnd == NULL || s_hinstance == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    out->hinstance = s_hinstance;
    out->hwnd = window->hwnd;
    return LC_SUCCESS;
}

void lc_platform_destroy(lc_window *window) {
    HWND hwnd;

    if (window == NULL) {
        return;
    }
    hwnd = window->hwnd;
    if (hwnd == NULL) {
        return;
    }
    window->hwnd = NULL;
    /* Clear userdata first so in-flight messages cannot touch freed memory. */
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)0);
    DestroyWindow(hwnd);
}

void lc_platform_poll(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}
