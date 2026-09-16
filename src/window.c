#include <stdlib.h>
#include <string.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "platform/platform.h"

/* Phase 27: bounded per-window event queue (ring buffer, geometric
 * growth to a cap; oldest dropped when full so the OS pump never
 * blocks on an unread queue). */
#define LC_EVENT_INITIAL_CAP 64u
#define LC_EVENT_MAX_CAP 4096u

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
    free(window->events);
    window->events = NULL;
    window->event_cap = 0;
    window->event_head = 0;
    window->event_count = 0;
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

void lc_window_push_event(lc_window *window,
                          const lc_window_event *event) {
    uint32_t tail;

    if (window == NULL || event == NULL) {
        return;
    }
    if (event->type == LC_EVENT_NONE) {
        return;
    }
    if (window->events == NULL || window->event_cap == 0) {
        window->events = (lc_window_event *)calloc(
            LC_EVENT_INITIAL_CAP, sizeof(lc_window_event));
        if (window->events == NULL) {
            return;
        }
        window->event_cap = LC_EVENT_INITIAL_CAP;
        window->event_head = 0;
        window->event_count = 0;
    }
    if (window->event_count >= window->event_cap) {
        if (window->event_cap < LC_EVENT_MAX_CAP) {
            uint32_t grown = window->event_cap * 2u;
            lc_window_event *fresh;
            uint32_t i;

            if (grown > LC_EVENT_MAX_CAP) {
                grown = LC_EVENT_MAX_CAP;
            }
            fresh = (lc_window_event *)calloc(
                grown, sizeof(lc_window_event));
            if (fresh != NULL) {
                for (i = 0; i < window->event_count; i++) {
                    fresh[i] = window->events[
                        (window->event_head + i) %
                        window->event_cap];
                }
                free(window->events);
                window->events = fresh;
                window->event_cap = grown;
                window->event_head = 0;
            } else {
                /* OOM: drop the oldest, keep the pump live. */
                window->event_head =
                    (window->event_head + 1u) % window->event_cap;
                window->event_count--;
            }
        } else {
            /* Full at cap: drop the oldest, keep the newest. */
            window->event_head =
                (window->event_head + 1u) % window->event_cap;
            window->event_count--;
        }
    }
    tail = (window->event_head + window->event_count) %
        window->event_cap;
    window->events[tail] = *event;
    window->events[tail].window = window;
    window->event_count++;
}

lc_result lc_window_read_event(lc_window *window,
                               lc_window_event *out) {
    if (window == NULL || out == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(out, 0, sizeof(*out));
    out->type = LC_EVENT_NONE;
    out->window = window;
    if (window->events == NULL || window->event_count == 0) {
        return LC_SUCCESS;
    }
    *out = window->events[window->event_head];
    out->window = window;
    window->event_head =
        (window->event_head + 1u) % window->event_cap;
    window->event_count--;
    return LC_SUCCESS;
}

lc_result lc_window_drain_events(lc_window *window) {
    if (window == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    window->event_head = 0;
    window->event_count = 0;
    return LC_SUCCESS;
}

uint32_t lc_window_pending_events(const lc_window *window) {
    if (window == NULL) {
        return 0;
    }
    return window->event_count;
}
