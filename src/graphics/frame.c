#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public frame API (Phase 6/11: acquire, clear, bind, draw, present).
 *
 * Thin validation over the Vulkan backend in vulkan_frame.c: NULL and
 * init checks plus liveness of handles (rejecting use-after-destroy
 * pointers without a handle-validation framework). Frame-open rules
 * (one open frame max, clear/bind/draw/end require an open frame) are
 * enforced here on live state so misuse never reaches Vulkan
 * recording. Phase 11 adds index buffers, indexed/instanced draws,
 * push constants, and depth clears with the same discipline.
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

lc_result lc_clear_depth(lc_swapchain *swapchain, float depth) {
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
    return lc_vulkan_frame_clear_depth(swapchain, depth);
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
    /* Legacy and explicit passes are mutually exclusive. */
    if (swapchain->encoder.in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* The pipeline must belong to this frame's device... */
    if (pipeline->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* ...and be structurally compatible with the swapchain target
     * (1 color + swapchain depth + 1 sample). A signature drifted by
     * recreation needs a recreated pipeline. */
    {
        lc_render_target_desc swap_desc;

        swap_desc.width = 0;
        swap_desc.height = 0;
        swap_desc.color_attachment_count = 1;
        swap_desc.color_formats[0] =
            lc_vulkan_untranslate_format(swapchain->format);
        swap_desc.depth_stencil_format =
            lc_vulkan_untranslate_format(swapchain->depth_format);
        swap_desc.samples = LC_SAMPLE_COUNT_1;
        if (pipeline->target_hash !=
            lc_render_target_hash(swap_desc.color_attachment_count,
                                  swap_desc.color_formats,
                                  swap_desc.depth_stencil_format,
                                  swap_desc.samples)) {
            return LC_ERROR_PIPELINE_INCOMPATIBLE;
        }
        {
            lc_render_target_desc pipe_desc;

            pipe_desc.width = 0;
            pipe_desc.height = 0;
            pipe_desc.color_attachment_count =
                pipeline->target_color_count;
            {
                uint32_t i;

                for (i = 0; i < pipeline->target_color_count; i++) {
                    pipe_desc.color_formats[i] =
                        pipeline->target_color_formats[i];
                }
            }
            pipe_desc.depth_stencil_format = pipeline->target_depth_format;
            pipe_desc.samples = pipeline->target_samples;
            if (!lc_render_target_desc_equal(&swap_desc, &pipe_desc)) {
                return LC_ERROR_PIPELINE_INCOMPATIBLE;
            }
        }
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

static int lc_is_live_binding_set(const lc_binding_set *set) {
    lc_state *state = lc_get_internal_state();
    const lc_binding_set *it;

    if (state == NULL || set == NULL) {
        return 0;
    }
    for (it = state->binding_sets; it != NULL; it = it->next) {
        if (it == set) {
            return 1;
        }
    }
    return 0;
}

lc_result lc_bind_binding_set(lc_swapchain *swapchain, lc_pipeline *pipeline,
                              uint32_t slot, lc_binding_set *set) {
    lc_state *state = lc_get_internal_state();

    if (swapchain == NULL || pipeline == NULL || set == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_is_live_swapchain(swapchain) ||
        !lc_pipeline_is_live(pipeline) || !lc_is_live_binding_set(set)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Same device everywhere; the set's signature must equal the
     * pipeline's canonical signature at this slot (content comparison,
     * never raw layout pointers, so destroyed layouts do not
     * invalidate matching pipelines/sets). */
    if (pipeline->device != swapchain->device ||
        set->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (slot >= pipeline->layout_count ||
        pipeline->slot_signatures == NULL ||
        pipeline->slot_signature_counts == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_binding_signature_equal(
            set->slots, set->slot_count,
            pipeline->slot_signatures[slot],
            pipeline->slot_signature_counts[slot])) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_frame_bind_set(swapchain, pipeline, slot, set);
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

lc_result lc_bind_index_buffer(lc_swapchain *swapchain, lc_buffer *buffer,
                               uint64_t offset, lc_index_type index_type) {
    lc_state *state = lc_get_internal_state();
    uint64_t align;

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
    if (index_type != LC_INDEX_UINT16 && index_type != LC_INDEX_UINT32) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Same device, index-capable buffer, offset inside with proper
     * alignment for the element width. */
    align = (index_type == LC_INDEX_UINT16) ? 2u : 4u;
    if (buffer->device != swapchain->device ||
        (buffer->usage & LC_BUFFER_USAGE_INDEX) == 0 ||
        offset >= buffer->size || (offset % align) != 0u) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_frame_bind_index(swapchain, buffer, offset, index_type);
}

lc_result lc_draw_indexed(lc_swapchain *swapchain, uint32_t index_count,
                          uint32_t instance_count, uint32_t first_index,
                          int32_t vertex_offset, uint32_t first_instance) {
    lc_state *state = lc_get_internal_state();
    const lc_buffer *index_buffer;
    uint64_t index_size;
    uint64_t offset;
    uint64_t needed;

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
    if (swapchain->bound_pipeline == NULL ||
        !lc_pipeline_is_live(swapchain->bound_pipeline)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!swapchain->index_bound || swapchain->bound_index_buffer == NULL ||
        !lc_is_live_buffer(swapchain->bound_index_buffer)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (index_count == 0 || instance_count == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    index_buffer = swapchain->bound_index_buffer;
    if (index_buffer->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    index_size =
        (swapchain->bound_index_type == LC_INDEX_UINT16) ? 2u : 4u;
    offset = swapchain->bound_index_offset;
    /* Byte range [offset + first_index*size, + index_count*size) must
     * fit the buffer; overflow fails closed. */
    if (first_index > (UINT64_MAX / index_size) ||
        index_count > (UINT64_MAX / index_size)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    needed = ((uint64_t)first_index + (uint64_t)index_count) * index_size;
    if (needed > index_buffer->size || offset > index_buffer->size - needed) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_frame_draw_indexed(swapchain, index_count,
                                        instance_count, first_index,
                                        vertex_offset, first_instance);
}

lc_result lc_draw_instanced(lc_swapchain *swapchain, uint32_t vertex_count,
                            uint32_t instance_count, uint32_t first_vertex,
                            uint32_t first_instance) {
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
    if (swapchain->bound_pipeline == NULL ||
        !lc_pipeline_is_live(swapchain->bound_pipeline)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (vertex_count == 0 || instance_count == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_frame_draw_instanced(swapchain, vertex_count,
                                          instance_count, first_vertex,
                                          first_instance);
}

lc_result lc_push_constants(lc_swapchain *swapchain, lc_pipeline *pipeline,
                            uint32_t visibility, uint32_t offset,
                            uint32_t size, const void *data) {
    lc_state *state = lc_get_internal_state();
    const uint32_t known =
        (uint32_t)LC_SHADER_VISIBILITY_VERTEX | (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT;
    uint32_t i;
    int fits = 0;

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
    /* The matching pipeline must be bound; pushes target its layout. */
    if (swapchain->bound_pipeline != pipeline) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (pipeline->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (visibility == 0 || (visibility & ~known) != 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0 || data == NULL || (offset % 4u) != 0u ||
        (size % 4u) != 0u) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (offset + size < offset) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* The window must sit inside one declared range with a compatible
     * stage mask (visibility subset of the range). */
    if (pipeline->push_ranges == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < pipeline->push_range_count; i++) {
        uint32_t r_off = pipeline->push_ranges[i].offset;
        uint32_t r_size = pipeline->push_ranges[i].size;
        uint32_t r_vis = pipeline->push_ranges[i].visibility;

        if ((visibility & ~r_vis) != 0) {
            continue;
        }
        if (offset >= r_off && offset + size <= r_off + r_size) {
            fits = 1;
            break;
        }
    }
    if (!fits) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_frame_push(swapchain, pipeline, visibility, offset,
                                size, data);
}
