#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public frame API (Phase 6: acquire, clear, submit, present).
 *
 * Thin validation over the Vulkan backend in vulkan_frame.c: NULL and
 * init checks plus liveness of the swapchain handle (rejecting
 * use-after-destroy pointers without a handle-validation framework).
 * Frame-open rules (one open frame max, clear/end require an open
 * frame) are enforced here on live state so misuse never reaches
 * Vulkan recording.
 */

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

lc_result lc_begin_frame(lc_swapchain *swapchain) {
    lc_state *state = lc_get_internal_state();

    if (swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_is_live_swapchain(swapchain)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_frame_begin(swapchain);
}

lc_result lc_clear_color(lc_swapchain *swapchain, float r, float g, float b,
                         float a) {
    lc_state *state = lc_get_internal_state();

    if (swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_is_live_swapchain(swapchain)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_frame_clear(swapchain, r, g, b, a);
}

lc_result lc_end_frame(lc_swapchain *swapchain) {
    lc_state *state = lc_get_internal_state();

    if (swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_is_live_swapchain(swapchain)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_frame_end(swapchain);
}
