#include <stdlib.h>
#include <string.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public swapchain API + lifetime tracking (Phase 5).
 *
 * A swapchain borrows its device and surface, so liveness of both is
 * verified against the internal lists, the surface must belong to the
 * given device (cross-device Vulkan misuse is rejected), and at most
 * one live swapchain may exist per surface. Dependent swapchains die
 * before their surface/device via the for_surface/for_device/for_window
 * hooks wired into lc_surface_destroy(), lc_device_destroy(),
 * lc_window_destroy(), and lc_shutdown() (swapchains first).
 *
 * Frame recording/presentation itself lives in frame.c and
 * vulkan_frame.c; destruction here retires images first, then frame
 * objects (the image teardown waits idle, so nothing is pending when
 * the pool, semaphores, and fences go away).
 */

static void lc_swapchain_list_add(lc_swapchain *swapchain) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || swapchain == NULL) {
        return;
    }
    swapchain->next = state->swapchains;
    swapchain->prev = NULL;
    if (state->swapchains != NULL) {
        state->swapchains->prev = swapchain;
    }
    state->swapchains = swapchain;
}

static void lc_swapchain_list_remove(lc_swapchain *swapchain) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || swapchain == NULL) {
        return;
    }
    if (swapchain->prev != NULL) {
        swapchain->prev->next = swapchain->next;
    } else if (state->swapchains == swapchain) {
        state->swapchains = swapchain->next;
    }
    if (swapchain->next != NULL) {
        swapchain->next->prev = swapchain->prev;
    }
    swapchain->next = NULL;
    swapchain->prev = NULL;
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

static int lc_is_live_surface(const lc_surface *surface) {
    lc_state *state = lc_get_internal_state();
    const lc_surface *it;

    if (state == NULL || surface == NULL) {
        return 0;
    }
    for (it = state->surfaces; it != NULL; it = it->next) {
        if (it == surface) {
            return 1;
        }
    }
    return 0;
}

static int lc_is_live_swapchain(const lc_swapchain *swapchain) {
    lc_state *state = lc_get_internal_state();
    const lc_swapchain *it;

    if (state == NULL || swapchain == NULL) {
        return 0;
    }
    for (it = state->swapchains; it != NULL; it = it->next) {
        if (it == swapchain) {
            return 1;
        }
    }
    return 0;
}

/* At most one live swapchain per surface (public rule for Phase 5). */
static int lc_surface_has_swapchain(const lc_surface *surface) {
    lc_state *state = lc_get_internal_state();
    const lc_swapchain *it;

    if (state == NULL || surface == NULL) {
        return 0;
    }
    for (it = state->swapchains; it != NULL; it = it->next) {
        if (it->surface == surface) {
            return 1;
        }
    }
    return 0;
}

lc_result lc_swapchain_create(lc_device *device, lc_surface *surface,
                              const lc_swapchain_desc *desc,
                              lc_swapchain **out_swapchain) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;
    lc_result res;

    if (device == NULL || surface == NULL || desc == NULL ||
        out_swapchain == NULL) {
        if (out_swapchain != NULL) {
            *out_swapchain = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_swapchain = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    /* Liveness before any dereference of caller handles. */
    if (!lc_is_live_device(device) || !lc_is_live_surface(surface)) {
        *out_swapchain = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* The surface must belong to this device; cross-device Vulkan
     * misuse is rejected rather than attempted. */
    if (surface->device != device) {
        *out_swapchain = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0) {
        *out_swapchain = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (lc_surface_has_swapchain(surface)) {
        *out_swapchain = NULL;
        return LC_ERROR_ALREADY_INITIALIZED;
    }

    swapchain = (lc_swapchain *)calloc(1, sizeof(lc_swapchain));
    if (swapchain == NULL) {
        *out_swapchain = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    swapchain->device = device;
    swapchain->surface = surface;
    swapchain->vsync = (desc->vsync != 0) ? 1 : 0;
    swapchain->preferred_image_count = desc->image_count;
    /* Flight slots (PART AE): 0 selects the default; clamp the rest
     * (never reject: uninitialized-descriptor callers must still get
     * a working swapchain). */
    if (desc->max_frames_in_flight == 0 ||
        desc->max_frames_in_flight > 8) {
        swapchain->max_flights = (desc->max_frames_in_flight == 0)
                                     ? LC_MAX_FRAMES_IN_FLIGHT
                                     : 8;
    } else {
        swapchain->max_flights = desc->max_frames_in_flight;
    }

    /* Fresh build: transactional helper either fills the struct or
     * leaves it empty ( VK_NULL_HANDLE / NULL / 0 ). */
    res = lc_vulkan_swapchain_rebuild(swapchain, desc->width, desc->height,
                                      VK_NULL_HANDLE);
    if (res != LC_SUCCESS) {
        *out_swapchain = NULL;
        free(swapchain);
        return res;
    }

    lc_swapchain_list_add(swapchain);
    *out_swapchain = swapchain;
    return LC_SUCCESS;
}

void lc_swapchain_destroy(lc_swapchain *swapchain) {
    if (swapchain == NULL) {
        return;
    }
    /* Phase 12: pipelines are device children with structural target
     * signatures (never anchored here), so nothing pipeline-related
     * dies with the swapchain. */
    lc_swapchain_list_remove(swapchain);
    /* Image teardown waits idle, so no submission is pending when the
     * pool, semaphores, and fences below are destroyed - even if the
     * caller never ended an open frame. */
    lc_vulkan_swapchain_teardown(swapchain);
    lc_vulkan_frame_teardown(swapchain);
    free(swapchain);
}

lc_result lc_swapchain_recreate(lc_swapchain *swapchain, uint32_t width,
                                uint32_t height) {
    if (swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (width == 0 || height == 0) {
        /* Minimized or invalid target: recoverable, caller retries
         * after resize. Existing swapchain untouched. */
        return LC_ERROR_ZERO_EXTENT;
    }
    if (!lc_is_live_swapchain(swapchain)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (swapchain->frame_active) {
        /* Recreating mid-frame would orphan the open recording; end
         * the frame first. */
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Transactional: success replaces the old state, failure leaves
     * the existing swapchain intact (see vulkan_swapchain.c). */
    return lc_vulkan_swapchain_rebuild(swapchain, width, height,
                                       swapchain->vk_swapchain);
}

void lc_swapchain_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->swapchains != NULL) {
        lc_swapchain_destroy(state->swapchains);
    }
}

void lc_swapchain_destroy_for_surface(const lc_surface *surface) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *it;
    lc_swapchain *next;

    if (state == NULL || surface == NULL) {
        return;
    }
    for (it = state->swapchains; it != NULL; it = next) {
        next = it->next;
        if (it->surface == surface) {
            lc_swapchain_destroy(it);
        }
    }
}

void lc_swapchain_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *it;
    lc_swapchain *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->swapchains; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_swapchain_destroy(it);
        }
    }
}

void lc_swapchain_destroy_for_window(const lc_window *window) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *it;
    lc_swapchain *next;

    if (state == NULL || window == NULL) {
        return;
    }
    for (it = state->swapchains; it != NULL; it = next) {
        next = it->next;
        if (it->surface != NULL && it->surface->window == window) {
            lc_swapchain_destroy(it);
        }
    }
}

uint32_t lc_swapchain_get_width(const lc_swapchain *swapchain) {
    if (swapchain == NULL) {
        return 0;
    }
    return swapchain->extent.width;
}

uint32_t lc_swapchain_get_height(const lc_swapchain *swapchain) {
    if (swapchain == NULL) {
        return 0;
    }
    return swapchain->extent.height;
}

uint32_t lc_swapchain_get_image_count(const lc_swapchain *swapchain) {
    if (swapchain == NULL) {
        return 0;
    }
    return swapchain->image_count;
}

lc_format lc_swapchain_get_format(const lc_swapchain *swapchain) {
    if (swapchain == NULL) {
        return LC_FORMAT_UNDEFINED;
    }
    return lc_vulkan_untranslate_format(swapchain->format);
}

lc_format lc_swapchain_get_depth_format(const lc_swapchain *swapchain) {
    if (swapchain == NULL) {
        return LC_FORMAT_UNDEFINED;
    }
    return lc_vulkan_untranslate_format(swapchain->depth_format);
}

lc_result lc_swapchain_get_render_target_desc(
    const lc_swapchain *swapchain, lc_render_target_desc *out_desc) {
    lc_state *state = lc_get_internal_state();
    const lc_swapchain *it;
    uint32_t i;

    if (swapchain == NULL || out_desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    for (it = state->swapchains; it != NULL; it = it->next) {
        if (it == swapchain) {
            break;
        }
    }
    if (it == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(out_desc, 0, sizeof(*out_desc));
    out_desc->width = swapchain->extent.width;
    out_desc->height = swapchain->extent.height;
    out_desc->color_attachment_count = 1;
    for (i = 0; i < LC_MAX_COLOR_ATTACHMENTS; i++) {
        out_desc->color_formats[i] = LC_FORMAT_UNDEFINED;
    }
    out_desc->color_formats[0] =
        lc_vulkan_untranslate_format(swapchain->format);
    out_desc->depth_stencil_format =
        lc_vulkan_untranslate_format(swapchain->depth_format);
    out_desc->samples = LC_SAMPLE_COUNT_1;
    return LC_SUCCESS;
}
