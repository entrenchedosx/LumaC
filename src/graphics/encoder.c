#include <stdint.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public command-encoder API (Phase 12: generic recording).
 *
 * Encoders are borrowed inline from an open swapchain frame; liveness
 * is the swapchain's liveness plus address identity (no dangling use).
 * Exactly one pass (explicit or legacy implicit) may be open per
 * frame; legacy swapchain-bound recording and explicit encoder
 * recording are mutually exclusive. Pipeline compatibility is purely
 * structural (hash fast-path, structural resolve).
 */

static lc_swapchain *lc_enc_lookup(const lc_command_encoder *enc) {
    lc_state *state = lc_get_internal_state();
    const lc_swapchain *it;

    if (state == NULL || enc == NULL) {
        return NULL;
    }
    for (it = state->swapchains; it != NULL; it = it->next) {
        if (&it->encoder == enc) {
            return (lc_swapchain *)it;
        }
    }
    return NULL;
}

static int lc_is_live_pipeline(const lc_pipeline *pipeline) {
    return lc_pipeline_is_live(pipeline);
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

static int lc_is_live_set(const lc_binding_set *set) {
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

static int lc_is_live_target(const lc_render_target *target) {
    return lc_render_target_is_live(target);
}

/* Structural pipeline-vs-pass compatibility (hash reject, structural
 * resolve). Width/height excluded (extent-independent). */
static int lc_enc_compat(const lc_command_encoder *enc,
                         const lc_pipeline *pipeline) {
    lc_render_target_desc pipe_desc;

    if (enc == NULL || pipeline == NULL) {
        return 0;
    }
    if (pipeline->target_hash !=
        lc_render_target_hash(enc->pass_target.color_attachment_count,
                              enc->pass_target.color_formats,
                              enc->pass_target.depth_stencil_format,
                              enc->pass_target.samples)) {
        return 0;
    }
    pipe_desc.width = 0;
    pipe_desc.height = 0;
    pipe_desc.color_attachment_count = pipeline->target_color_count;
    {
        uint32_t i;

        for (i = 0; i < pipeline->target_color_count; i++) {
            pipe_desc.color_formats[i] = pipeline->target_color_formats[i];
        }
    }
    pipe_desc.depth_stencil_format = pipeline->target_depth_format;
    pipe_desc.samples = pipeline->target_samples;
    return lc_render_target_desc_equal(&enc->pass_target, &pipe_desc);
}

static int lc_valid_load(lc_load_op op) {
    return op == LC_LOAD_OP_LOAD || op == LC_LOAD_OP_CLEAR ||
           op == LC_LOAD_OP_DONT_CARE;
}

static int lc_valid_store(lc_store_op op) {
    return op == LC_STORE_OP_STORE || op == LC_STORE_OP_DONT_CARE;
}

lc_result lc_encoder_begin_render_pass(lc_command_encoder *enc,
                                       const lc_render_pass_desc *desc) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;
    const lc_render_target *target = NULL;
    uint32_t i;

    if (enc == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* No mixing with the legacy implicit pass. */
    if (swapchain->rp_open || swapchain->clear_pending ||
        swapchain->depth_clear_pending || swapchain->bound_pipeline != NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->color_attachment_count > LC_MAX_COLOR_ATTACHMENTS ||
        (desc->color_attachment_count == 0 &&
         desc->depth_attachment == NULL)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->color_attachment_count > 0 &&
        desc->color_attachments == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < desc->color_attachment_count; i++) {
        const lc_render_color_attachment *att = &desc->color_attachments[i];

        if (att->view == NULL || !lc_valid_load(att->load_op) ||
            !lc_valid_store(att->store_op)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    if (desc->depth_attachment != NULL) {
        const lc_render_depth_attachment *datt = desc->depth_attachment;

        if (datt->view == NULL || !lc_valid_load(datt->depth_load_op) ||
            !lc_valid_store(datt->depth_store_op) ||
            datt->stencil_load_op != LC_LOAD_OP_DONT_CARE ||
            datt->stencil_store_op != LC_STORE_OP_DONT_CARE ||
            datt->clear_stencil != 0) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    /* All views must belong to one live offscreen target with a
     * matching extent (signature equality follows from shared views;
     * re-checked structurally by the backend). */
    {
        lc_state *st = lc_get_internal_state();
        const lc_render_target *it;

        for (it = st->render_targets; it != NULL; it = it->next) {
            uint32_t k;
            int ok = 1;

            if (it->device != swapchain->device ||
                it->width != desc->width || it->height != desc->height ||
                it->color_count != desc->color_attachment_count) {
                continue;
            }
            for (k = 0; k < it->color_count; k++) {
                if (desc->color_attachments[k].view != it->color_views[k]) {
                    ok = 0;
                    break;
                }
            }
            if (!ok) {
                continue;
            }
            if (desc->depth_attachment != NULL) {
                if (it->depth_view == NULL ||
                    desc->depth_attachment->view != it->depth_view) {
                    continue;
                }
            } else if (it->depth_view != NULL) {
                continue;
            }
            target = it;
            break;
        }
    }
    if (target == NULL || !lc_is_live_target(target)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_begin_offscreen(enc, (lc_render_target *)target,
                                            desc);
}

lc_result lc_encoder_begin_swapchain_pass(
    lc_command_encoder *enc, lc_swapchain *swapchain,
    const lc_render_swapchain_pass_desc *desc) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *parent;

    if (enc == NULL || swapchain == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    parent = lc_enc_lookup(enc);
    if (parent == NULL || parent != swapchain ||
        !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (swapchain->rp_open || swapchain->clear_pending ||
        swapchain->depth_clear_pending || swapchain->bound_pipeline != NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_valid_load(desc->color_load_op) ||
        !lc_valid_store(desc->color_store_op) ||
        !lc_valid_load(desc->depth_load_op) ||
        !lc_valid_store(desc->depth_store_op)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_begin_swapchain(enc, swapchain, desc);
}

lc_result lc_encoder_end_render_pass(lc_command_encoder *enc) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (enc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_end(enc);
}

lc_result lc_encoder_bind_pipeline(lc_command_encoder *enc,
                                   lc_pipeline *pipeline) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (enc == NULL || pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (lc_vk_worker_live(enc)) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_bind(enc, pipeline);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->in_pass || !lc_is_live_pipeline(pipeline)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (pipeline->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_enc_compat(enc, pipeline)) {
        return LC_ERROR_PIPELINE_INCOMPATIBLE;
    }
    return lc_vulkan_encoder_bind(enc, pipeline);
}

lc_result lc_encoder_bind_binding_set(lc_command_encoder *enc,
                                      lc_pipeline *pipeline, uint32_t slot,
                                      lc_binding_set *set) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (enc == NULL || pipeline == NULL || set == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (lc_vk_worker_live(enc)) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_bind_set(enc, pipeline, slot, set);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->in_pass || !lc_is_live_pipeline(pipeline) ||
        !lc_is_live_set(set)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
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
    return lc_vulkan_encoder_bind_set(enc, pipeline, slot, set);
}

lc_result lc_encoder_bind_vertex_buffer(lc_command_encoder *enc,
                                        uint32_t binding, lc_buffer *buffer,
                                        uint64_t offset) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (enc == NULL || buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (lc_vk_worker_live(enc)) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_bind_vertex(enc, binding, buffer, offset);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->in_pass || !lc_is_live_buffer(buffer)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (buffer->device != swapchain->device ||
        (buffer->usage & LC_BUFFER_USAGE_VERTEX) == 0 ||
        offset >= buffer->size) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_bind_vertex(enc, binding, buffer, offset);
}

lc_result lc_encoder_bind_index_buffer(lc_command_encoder *enc,
                                       lc_buffer *buffer, uint64_t offset,
                                       lc_index_type index_type) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;
    uint64_t align;

    if (enc == NULL || buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (lc_vk_worker_live(enc)) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_bind_index(enc, buffer, offset, index_type);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->in_pass || !lc_is_live_buffer(buffer)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (index_type != LC_INDEX_UINT16 && index_type != LC_INDEX_UINT32) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    align = (index_type == LC_INDEX_UINT16) ? 2u : 4u;
    if (buffer->device != swapchain->device ||
        (buffer->usage & LC_BUFFER_USAGE_INDEX) == 0 ||
        offset >= buffer->size || (offset % align) != 0u) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_bind_index(enc, buffer, offset, index_type);
}

lc_result lc_encoder_push_constants(lc_command_encoder *enc,
                                    lc_pipeline *pipeline,
                                    uint32_t visibility, uint32_t offset,
                                    uint32_t size, const void *data) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;
    const uint32_t known =
        (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
        (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT;
    uint32_t i;
    int fits = 0;

    if (enc == NULL || pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (lc_vk_worker_live(enc)) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_push(enc, pipeline, visibility, offset, size, data);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->in_pass || !lc_is_live_pipeline(pipeline)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_pipeline != pipeline) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (pipeline->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (visibility == 0 || (visibility & ~known) != 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0 || data == NULL || (offset % 4u) != 0u ||
        (size % 4u) != 0u || offset + size < offset) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
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
    return lc_vulkan_encoder_push(enc, pipeline, visibility, offset, size,
                                  data);
}

lc_result lc_encoder_draw(lc_command_encoder *enc, uint32_t vertex_count,
                          uint32_t first_vertex) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    (void)vertex_count;
    (void)first_vertex;
    if (enc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (lc_vk_worker_live(enc)) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_draw(enc, vertex_count, first_vertex);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->in_pass || enc->bound_pipeline == NULL ||
        !lc_is_live_pipeline(enc->bound_pipeline)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_draw(enc, vertex_count, first_vertex);
}

lc_result lc_encoder_draw_indexed(
    lc_command_encoder *enc, uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;
    const lc_buffer *index_buffer;
    uint64_t index_size;
    uint64_t offset;
    uint64_t needed;

    if (enc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (lc_vk_worker_live(enc)) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_draw_indexed(enc, index_count, instance_count, first_index, vertex_offset, first_instance);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->in_pass || enc->bound_pipeline == NULL ||
        !lc_is_live_pipeline(enc->bound_pipeline)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->index_bound || enc->bound_index_buffer == NULL ||
        !lc_is_live_buffer(enc->bound_index_buffer)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (index_count == 0 || instance_count == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    index_buffer = enc->bound_index_buffer;
    if (index_buffer->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    index_size = (enc->bound_index_type == LC_INDEX_UINT16) ? 2u : 4u;
    offset = enc->bound_index_offset;
    if (first_index > (UINT64_MAX / index_size) ||
        index_count > (UINT64_MAX / index_size)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    needed = ((uint64_t)first_index + (uint64_t)index_count) * index_size;
    if (needed > index_buffer->size || offset > index_buffer->size - needed) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_draw_indexed(enc, index_count, instance_count,
                                          first_index, vertex_offset,
                                          first_instance);
}

lc_result lc_encoder_draw_instanced(lc_command_encoder *enc,
                                    uint32_t vertex_count,
                                    uint32_t instance_count,
                                    uint32_t first_vertex,
                                    uint32_t first_instance) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (enc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (lc_vk_worker_live(enc)) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_draw_instanced(enc, vertex_count, instance_count, first_vertex, first_instance);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->in_pass || enc->bound_pipeline == NULL ||
        !lc_is_live_pipeline(enc->bound_pipeline)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (vertex_count == 0 || instance_count == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_draw_instanced(enc, vertex_count, instance_count,
                                            first_vertex, first_instance);
}

static int lc_is_live_image(const lc_image *image) {
    lc_state *state = lc_get_internal_state();
    const lc_image *it;

    if (state == NULL || image == NULL) {
        return 0;
    }
    for (it = state->images; it != NULL; it = it->next) {
        if (it == image) {
            return 1;
        }
    }
    return 0;
}

lc_result lc_encoder_transition_image(
    lc_command_encoder *enc, lc_image *image,
    const lc_image_subresource_range *range, lc_resource_state new_state) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (enc == NULL || image == NULL || range == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (lc_vk_worker_live(enc)) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_transition(
            enc, image, range->base_mip_level, range->level_count,
            range->base_array_layer, range->layer_count, new_state);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_is_live_image(image)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (image->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (range->level_count == 0 || range->layer_count == 0 ||
        range->base_mip_level >= image->mip_levels ||
        range->level_count > image->mip_levels - range->base_mip_level ||
        range->base_array_layer >= image->array_layers ||
        range->layer_count >
            image->array_layers - range->base_array_layer) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_state_valid_for_image(new_state)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_transition(
        enc, image, range->base_mip_level, range->level_count,
        range->base_array_layer, range->layer_count, new_state);
}

lc_result lc_command_encoder_create(
    lc_device *device,
    const lc_command_encoder_desc *desc,
    lc_command_encoder **out_encoder) {
    lc_state *state = lc_get_internal_state();

    if (device == NULL || desc == NULL || out_encoder == NULL) {
        if (out_encoder != NULL) {
            *out_encoder = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_encoder = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    {
        lc_device *it;
        int live = 0;

        for (it = state->devices; it != NULL; it = it->next) {
            if (it == device) {
                live = 1;
                break;
            }
        }
        if (!live) {
            *out_encoder = NULL;
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    return lc_vulkan_worker_create(device, desc->queue, out_encoder);
}

void lc_command_encoder_destroy(lc_command_encoder *encoder) {
    if (!lc_vk_worker_live(encoder)) {
        return;
    }
    lc_vulkan_worker_destroy(encoder);
}

lc_result lc_command_list_begin(lc_command_encoder *encoder,
                                lc_render_target *target,
                                const lc_render_pass_desc *desc) {
    if (encoder == NULL || target == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_worker_live(encoder)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_worker_begin(encoder, target, desc);
}

lc_result lc_command_encoder_finish(lc_command_encoder *encoder,
                                    lc_command_list **out_list) {
    if (encoder == NULL || out_list == NULL) {
        if (out_list != NULL) {
            *out_list = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_worker_live(encoder)) {
        if (out_list != NULL) {
            *out_list = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_worker_finish(encoder, out_list);
}

void lc_command_list_destroy(lc_command_list *list) {
    if (list == NULL) {
        return;
    }
    lc_vulkan_worker_list_destroy(list);
}

lc_result lc_encoder_execute_lists(lc_command_encoder *primary,
                                   lc_command_list *const *lists,
                                   uint32_t list_count) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (primary == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (primary->worker_mode) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = lc_enc_lookup(primary);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!primary->in_pass) {
        /* Compute-only batches execute outside any pass (dispatch
         * is illegal inside a render-pass instance); graphics
         * batches keep the open-pass requirement. Mixed batches
         * are rejected by the backend. */
        uint32_t i;
        int all_compute = (list_count > 0) ? 1 : 0;

        for (i = 0; i < list_count; i++) {
            if (lists != NULL && lists[i] != NULL &&
                lists[i]->is_compute) {
                continue;
            }
            all_compute = 0;
            break;
        }
        if (!all_compute) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    if (list_count > 0 && lists == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    {
        lc_vk_flight *flight = NULL;

        if (swapchain->current_frame >= swapchain->max_flights) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        flight = &swapchain->flights[swapchain->current_frame];
        return lc_vulkan_worker_execute(primary, lists, list_count,
                                        flight->cmd);
    }
}

/* ------------------------------------------------------------------ */
/* Compute + indirect public recording (Phase 21).                     */
/* ------------------------------------------------------------------ */

static int lc_is_live_compute_pipeline(const lc_compute_pipeline *pipeline) {
    return lc_compute_pipeline_is_live(pipeline);
}

lc_result lc_encoder_bind_compute_pipeline(lc_command_encoder *enc,
                                           lc_compute_pipeline *pipeline) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (enc == NULL || pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (enc->worker_mode) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (!lc_is_live_compute_pipeline(pipeline)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_bind_compute_pipeline(enc, pipeline);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_is_live_compute_pipeline(pipeline)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (pipeline->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_bind_compute_pipeline(enc, pipeline);
}

lc_result lc_encoder_dispatch(lc_command_encoder *enc, uint32_t x,
                              uint32_t y, uint32_t z) {
    lc_state *state = lc_get_internal_state();

    if (enc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (enc->worker_mode) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_dispatch(enc, x, y, z);
    }
    {
        lc_swapchain *swapchain = lc_enc_lookup(enc);

        if (swapchain == NULL || !swapchain->frame_active) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_dispatch(enc, x, y, z);
    }
}

lc_result lc_encoder_bind_compute_set(lc_command_encoder *enc,
                                      lc_compute_pipeline *pipeline,
                                      uint32_t slot, lc_binding_set *set) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (enc == NULL || pipeline == NULL || set == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (enc->worker_mode) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (!lc_is_live_compute_pipeline(pipeline)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_bind_compute_set(enc, pipeline, slot,
                                                  set);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_is_live_compute_pipeline(pipeline) ||
        !lc_is_live_set(set)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
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
    return lc_vulkan_encoder_bind_compute_set(enc, pipeline, slot, set);
}

lc_result lc_encoder_push_compute_constants(
    lc_command_encoder *enc, lc_compute_pipeline *pipeline,
    uint32_t visibility, uint32_t offset, uint32_t size,
    const void *data) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;
    const uint32_t known =
        (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
        (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT |
        (uint32_t)LC_SHADER_VISIBILITY_COMPUTE;
    uint32_t i;
    int fits = 0;

    if (enc == NULL || pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (enc->worker_mode) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (!lc_is_live_compute_pipeline(pipeline)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_push_compute(enc, pipeline, visibility,
                                              offset, size, data);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_is_live_compute_pipeline(pipeline)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_compute_pipeline != pipeline) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (pipeline->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (visibility == 0 || (visibility & ~known) != 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0 || data == NULL || (offset % 4u) != 0u ||
        (size % 4u) != 0u || offset + size < offset) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
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
    return lc_vulkan_encoder_push_compute(enc, pipeline, visibility,
                                          offset, size, data);
}

lc_result lc_encoder_draw_indirect(lc_command_encoder *enc,
                                   lc_buffer *buffer, uint64_t offset,
                                   uint32_t draw_count, uint32_t stride) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (enc == NULL || buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (enc->worker_mode) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (!lc_is_live_buffer(buffer)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_draw_indirect(enc, buffer, offset,
                                               draw_count, stride);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->in_pass || !lc_is_live_buffer(buffer)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (buffer->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_draw_indirect(enc, buffer, offset,
                                           draw_count, stride);
}

lc_result lc_encoder_draw_indexed_indirect(lc_command_encoder *enc,
                                           lc_buffer *buffer,
                                           uint64_t offset,
                                           uint32_t draw_count,
                                           uint32_t stride) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (enc == NULL || buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (enc->worker_mode) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (!lc_is_live_buffer(buffer)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_draw_indexed_indirect(
            enc, buffer, offset, draw_count, stride);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!enc->in_pass || !lc_is_live_buffer(buffer)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (buffer->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_draw_indexed_indirect(enc, buffer, offset,
                                                   draw_count, stride);
}

lc_result lc_encoder_transition_buffer(lc_command_encoder *enc,
                                       lc_buffer *buffer,
                                       lc_resource_state new_state) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (enc == NULL || buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (enc->worker_mode) {
        if (!lc_vk_worker_live(enc) || enc->worker_list == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (!lc_is_live_buffer(buffer)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vulkan_encoder_transition_buffer(enc, buffer,
                                                   new_state);
    }
    swapchain = lc_enc_lookup(enc);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_is_live_buffer(buffer)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (buffer->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_encoder_transition_buffer(enc, buffer, new_state);
}

lc_result lc_command_list_begin_compute(lc_command_encoder *encoder) {
    lc_state *state = lc_get_internal_state();

    if (encoder == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!encoder->worker_mode || !lc_vk_worker_live(encoder)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_worker_begin_compute(encoder);
}

lc_result lc_encoder_get_flight_slot(lc_command_encoder *encoder,
                                     uint32_t *out_index,
                                     uint32_t *out_count) {
    lc_state *state = lc_get_internal_state();
    lc_swapchain *swapchain;

    if (encoder == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (encoder->worker_mode) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = lc_enc_lookup(encoder);
    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (out_index != NULL) {
        *out_index = swapchain->current_frame;
    }
    if (out_count != NULL) {
        *out_count = swapchain->max_flights;
    }
    return LC_SUCCESS;
}
