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

lc_result lc_bind_pipeline(lc_swapchain *swapchain, lc_pipeline *pipeline) {
    lc_state *state = lc_get_internal_state();

    if (swapchain == NULL || pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_is_live_swapchain(swapchain) ||
        !lc_pipeline_is_live(pipeline)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* The pipeline must belong to this frame's device... */
    if (pipeline->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* ...and match the swapchain's current color format. A format
     * drifted by recreation needs a recreated pipeline. */
    if (pipeline->format != swapchain->format) {
        return LC_ERROR_PIPELINE_INCOMPATIBLE;
    }
    return lc_vulkan_frame_bind(swapchain, pipeline);
}

static int lc_is_live_buffer(const lc_buffer *buffer) {
    lc_state *state = lc_get_internal_state();
    const lc_buffer *it;

    if (state == NULL || buffer == NULL) {
        return 0;
    }
    for (it = state->buffers; it != NULL; it = it->next) {
        if (it == buffer) {
            return 1;
        }
    }
    return 0;
}

lc_result lc_bind_vertex_buffer(lc_swapchain *swapchain, uint32_t binding,
                                lc_buffer *buffer, uint64_t offset) {
    lc_state *state = lc_get_internal_state();

    if (swapchain == NULL || buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_is_live_swapchain(swapchain) || !lc_is_live_buffer(buffer)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Same device, vertex-capable buffer, offset inside the buffer. */
    if (buffer->device != swapchain->device ||
        (buffer->usage & LC_BUFFER_USAGE_VERTEX) == 0 ||
        offset >= buffer->size) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_frame_bind_vertex(swapchain, binding, buffer, offset);
}

lc_result lc_draw(lc_swapchain *swapchain, uint32_t vertex_count,
                  uint32_t first_vertex) {
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
    /* A pipeline destroyed after binding must not be recorded. */
    if (swapchain->bound_pipeline == NULL ||
        !lc_pipeline_is_live(swapchain->bound_pipeline)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_frame_draw(swapchain, vertex_count, first_vertex);
}
