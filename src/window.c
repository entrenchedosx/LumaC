#include <stdlib.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "platform/platform.h"

static void lc_window_list_add(lc_window *window) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || window == NULL) {
        return;
    }
    window->next = state->windows;
    window->prev = NULL;
    if (state->windows != NULL) {
        state->windows->prev = window;
    }
    state->windows = window;
}

static void lc_window_list_remove(lc_window *window) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || window == NULL) {
        return;
    }
    if (window->prev != NULL) {
        window->prev->next = window->next;
    } else if (state->windows == window) {
        state->windows = window->next;
    }
    if (window->next != NULL) {
        window->next->prev = window->prev;
    }
    window->next = NULL;
    window->prev = NULL;
}

lc_result lc_window_create(const lc_window_desc *desc, lc_window **out_window) {
    lc_state *state = lc_get_internal_state();
    const char *title;

    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (desc == NULL || out_window == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    title = (desc->title != NULL) ? desc->title : LC_DEFAULT_TITLE;

    {
        lc_window *window = (lc_window *)calloc(1, sizeof(lc_window));
        lc_result res;

        if (window == NULL) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
        window->width = desc->width;
        window->height = desc->height;
        window->should_close = 0;

        res = lc_platform_create(window, title, desc->width, desc->height);
        if (res != LC_SUCCESS) {
            free(window);
            return res;
        }

        lc_window_list_add(window);
        *out_window = window;
        return LC_SUCCESS;
    }
}

void lc_window_destroy(lc_window *window) {
    if (window == NULL) {
        return;
    }
    /* Dependents first: swapchains, then surfaces. Neither may outlive
     * the native window they were created from. */
    lc_swapchain_destroy_for_window(window);
    lc_surface_destroy_for_window(window);
    lc_window_list_remove(window);
    lc_platform_destroy(window);
    free(window);
}

void lc_window_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->windows != NULL) {
        lc_window_destroy(state->windows);
    }
}

void lc_poll_events(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || !state->initialized) {
        return;
    }
    lc_platform_poll();
}

int lc_window_should_close(const lc_window *window) {
    if (window == NULL) {
        return 1;
    }
    return window->should_close ? 1 : 0;
}

uint32_t lc_window_get_width(const lc_window *window) {
    if (window == NULL) {
        return 0;
    }
    return window->width;
}

uint32_t lc_window_get_height(const lc_window *window) {
    if (window == NULL) {
        return 0;
    }
    return window->height;
}
