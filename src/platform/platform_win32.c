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

#include "platform/platform.h"

static const wchar_t kClassName[] = L"LumaCWindowClass";
static int s_class_registered = 0;
static HINSTANCE s_hinstance = NULL;

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
            window->should_close = 1;
        }
        return 0;
    case WM_SIZE:
        /* lParam holds the new client area: LOWORD=width, HIWORD=height. */
        if (window != NULL) {
            window->width = (uint32_t)(uint16_t)LOWORD(lparam);
            window->height = (uint32_t)(uint16_t)HIWORD(lparam);
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
