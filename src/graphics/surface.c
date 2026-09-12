#include <stdlib.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "platform/platform.h" /* complete lc_window for liveness walk */
#include "graphics/graphics_internal.h"

/*
 * Public surface API + lifetime tracking (Phase 4).
 *
 * A surface borrows its device and window, so liveness of both handles
 * is verified against the internal lists (rejecting use-after-destroy
 * pointers without a handle-validation framework). Dependent surfaces
 * are destroyed before their device/window via the for_device/for_window
 * hooks wired into lc_device_destroy(), lc_window_destroy(), and
 * lc_shutdown() in dependency order: surfaces, devices, windows.
 */

static void lc_surface_list_add(lc_surface *surface) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || surface == NULL) {
        return;
    }
    surface->next = state->surfaces;
    surface->prev = NULL;
    if (state->surfaces != NULL) {
        state->surfaces->prev = surface;
    }
    state->surfaces = surface;
}

static void lc_surface_list_remove(lc_surface *surface) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || surface == NULL) {
        return;
    }
    if (surface->prev != NULL) {
        surface->prev->next = surface->next;
    } else if (state->surfaces == surface) {
        state->surfaces = surface->next;
    }
    if (surface->next != NULL) {
        surface->next->prev = surface->prev;
    }
    surface->next = NULL;
    surface->prev = NULL;
}

static int lc_is_live_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    const lc_device *it;

    if (state == NULL || device == NULL) {
        return 0;
    }
    for (it = state->devices; it != NULL; it = it->next) {
        if (it == device) {
            return 1;
        }
    }
    return 0;
}

static int lc_is_live_window(const lc_window *window) {
    lc_state *state = lc_get_internal_state();
    const lc_window *it;

    if (state == NULL || window == NULL) {
        return 0;
    }
    for (it = state->windows; it != NULL; it = it->next) {
        if (it == window) {
            return 1;
        }
    }
    return 0;
}

lc_result lc_surface_create(lc_device *device, lc_window *window,
                            lc_surface **out_surface) {
    lc_state *state = lc_get_internal_state();
    lc_surface *surface;
    lc_result res;

    if (device == NULL || window == NULL || out_surface == NULL) {
        if (out_surface != NULL) {
            *out_surface = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_surface = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    /* Liveness first: never dereference a dead handle. Only live
     * objects from the internal lists may be touched below. */
    if (!lc_is_live_device(device) || !lc_is_live_window(window)) {
        *out_surface = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->backend != LC_BACKEND_VULKAN) {
        *out_surface = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }

    surface = (lc_surface *)calloc(1, sizeof(lc_surface));
    if (surface == NULL) {
        *out_surface = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }

    res = lc_vulkan_surface_create(surface, device, window);
    if (res != LC_SUCCESS) {
        *out_surface = NULL;
        free(surface);
        return res;
    }

    lc_surface_list_add(surface);
    *out_surface = surface;
    return LC_SUCCESS;
}

void lc_surface_destroy(lc_surface *surface) {
    if (surface == NULL) {
        return;
    }
    /* Dependent swapchain first: VkSwapchainKHR dies with its surface's
     * device, and validation requires it gone before vkDestroySurfaceKHR. */
    lc_swapchain_destroy_for_surface(surface);
    lc_surface_list_remove(surface);
    lc_vulkan_surface_destroy(surface);
    free(surface);
}

void lc_surface_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->surfaces != NULL) {
        lc_surface_destroy(state->surfaces);
    }
}

void lc_surface_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_surface *it;
    lc_surface *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->surfaces; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_surface_destroy(it);
        }
    }
}

void lc_surface_destroy_for_window(const lc_window *window) {
    lc_state *state = lc_get_internal_state();
    lc_surface *it;
    lc_surface *next;

    if (state == NULL || window == NULL) {
        return;
    }
    for (it = state->surfaces; it != NULL; it = next) {
        next = it->next;
        if (it->window == window) {
            lc_surface_destroy(it);
        }
    }
}

int lc_surface_is_present_supported(const lc_surface *surface) {
    if (surface == NULL) {
        return 0;
    }
    return surface->present_supported ? 1 : 0;
}
