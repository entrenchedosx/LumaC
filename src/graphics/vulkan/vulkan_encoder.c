/*
 * Vulkan encoder backend (Phase 12: explicit offscreen + swapchain
 * passes sharing the frame command buffer with the legacy implicit
 * pass, mutually exclusive per frame).
 *
 * Offscreen passes use the device render-pass cache (structure +
 * policy + sampled/present finals) plus one lazily created
 * framebuffer per target. Swapchain passes reuse the swapchain's
 * legacy per-image framebuffers with cache passes ending PRESENT.
 * Attachment image tracking is updated on end (STORE) so second-pass
 * sampling validates without extra barriers; CLEAR passes start from
 * UNDEFINED and need no pre-transition.
 */

#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

static lc_vk_flight *lc_enc_flight(lc_swapchain *swapchain) {
    return &swapchain->flights[swapchain->current_frame];
}

static int lc_enc_frame_ready(const lc_swapchain *swapchain) {
    return swapchain != NULL && swapchain->device != NULL &&
           swapchain->surface != NULL &&
           swapchain->device->device != VK_NULL_HANDLE &&
           swapchain->vk_swapchain != VK_NULL_HANDLE &&
           swapchain->cmd_pool != VK_NULL_HANDLE &&
           swapchain->current_frame < swapchain->max_flights &&
           swapchain->frame_active &&
           swapchain->current_image < swapchain->image_count;
}

/* Legacy implicit-pass state must be idle before an explicit begin
 * (no mixing paths inside one frame). */
static int lc_enc_legacy_idle(const lc_swapchain *swapchain) {
    return swapchain->rp_open == 0 && swapchain->clear_pending == 0 &&
           swapchain->depth_clear_pending == 0 &&
           swapchain->bound_pipeline == NULL;
}

static float lc_enc_clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

static lc_resource_state lc_enc_tracked_at(const lc_image *image,
                                              const lc_image_view *view) {
    size_t idx;
    lc_resource_state st;

    if (image == NULL || image->states == NULL || view == NULL) {
        return LC_RESOURCE_STATE_UNDEFINED;
    }
    idx = (size_t)view->base_array_layer * image->mip_levels +
          view->base_mip_level;
    lc_device_lock_state(image->device);
    st = image->states[idx];
    lc_device_unlock_state(image->device);
    return st;
}

/* Shared offscreen pass lookup + framebuffer ensure (explicit
 * passes and worker-list inheritance). Keyed identically so both
 * see the same VkRenderPass for equal recipes. */
lc_result lc_vulkan_offscreen_pass(lc_device *device,
                                   lc_render_target *target,
                                   const lc_render_pass_desc *desc,
                                   VkRenderPass *out_pass) {
    lc_vk_pass_key key;
    uint32_t i;
    lc_result res;

    if (device == NULL || target == NULL || desc == NULL ||
        out_pass == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&key, 0, sizeof(key));
    key.color_count = target->color_count;
    for (i = 0; i < key.color_count; i++) {
        key.color_formats[i] =
            lc_vulkan_translate_format(target->color_formats[i]);
        key.color_loads[i] = lc_vulkan_translate_load(
            desc->color_attachments[i].load_op);
        key.color_stores[i] = lc_vulkan_translate_store(
            desc->color_attachments[i].store_op);
    }
    key.depth_format = (target->depth_view != NULL)
                           ? lc_vulkan_translate_format(target->depth_format)
                           : VK_FORMAT_UNDEFINED;
    /* Sampled-usage depth selects the sampled-readable final layout
     * (must agree with pass creation + end-of-pass adoption). The
     * STORE op joins the key: a discarded (DONT_CARE) depth ends
     * attachment-optimal/UNDEFINED like plain depth even when the
     * image carries SAMPLED usage (Phase 23: Hi-Z stores depth only
     * while it needs the previous frame; legacy frames keep the
     * exact historical behavior). */
    key.depth_sampled =
        (target->depth_view != NULL && target->depth_view->image != NULL &&
         (target->depth_view->image->usage & LC_IMAGE_USAGE_SAMPLED) !=
             0 &&
         desc->depth_attachment != NULL &&
         desc->depth_attachment->depth_store_op == LC_STORE_OP_STORE)
            ? 1
            : 0;
    if (desc->depth_attachment != NULL) {
        key.depth_load =
            lc_vulkan_translate_load(desc->depth_attachment->depth_load_op);
        key.depth_store =
            lc_vulkan_translate_store(desc->depth_attachment->depth_store_op);
    } else {
        key.depth_load = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        key.depth_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }
    key.samples = lc_vulkan_translate_samples(target->samples);
    key.present = 0;

    res = lc_vulkan_pass_cache_get(device, &key, out_pass);
    if (res != LC_SUCCESS) {
        return res;
    }
    /* Multiple workers may first-touch the same target concurrently. */
    lc_device_lock_cache(device);
    res = lc_vulkan_target_ensure_framebuffer(target, *out_pass);
    lc_device_unlock_cache(device);
    return res;
}

lc_result lc_vulkan_encoder_begin_offscreen(
    lc_command_encoder *enc, lc_render_target *target,
    const lc_render_pass_desc *desc) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkRenderPassBeginInfo begin_info;
    VkClearValue clear_values[LC_MAX_COLOR_ATTACHMENTS + 1];
    VkViewport viewport;
    VkRect2D scissor;
    uint32_t i;
    lc_result res;

    if (enc == NULL || target == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (swapchain == NULL || !lc_enc_frame_ready(swapchain) ||
        enc->in_pass || !lc_enc_legacy_idle(swapchain)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (target->device != swapchain->device || target->width == 0 ||
        target->height == 0 || target->width != desc->width ||
        target->height != desc->height) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->color_attachment_count != target->color_count ||
        desc->color_attachment_count > LC_MAX_COLOR_ATTACHMENTS) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Views must be exactly the target's attachments in order (no
     * aliasing games in Phase 12). */
    for (i = 0; i < desc->color_attachment_count; i++) {
        const lc_render_color_attachment *att = &desc->color_attachments[i];

        if (att->view != target->color_views[i]) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (att->load_op != LC_LOAD_OP_LOAD &&
            att->load_op != LC_LOAD_OP_CLEAR &&
            att->load_op != LC_LOAD_OP_DONT_CARE) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (att->store_op != LC_STORE_OP_STORE &&
            att->store_op != LC_STORE_OP_DONT_CARE) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (att->load_op == LC_LOAD_OP_LOAD) {
            const lc_image_view *view = att->view;

            if (view->image == NULL ||
                lc_enc_tracked_at(view->image, view) !=
                    LC_RESOURCE_STATE_SHADER_READ) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    if (desc->depth_attachment != NULL) {
        const lc_render_depth_attachment *datt = desc->depth_attachment;

        if (target->depth_view == NULL ||
            datt->view != target->depth_view) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (datt->depth_load_op != LC_LOAD_OP_LOAD &&
            datt->depth_load_op != LC_LOAD_OP_CLEAR &&
            datt->depth_load_op != LC_LOAD_OP_DONT_CARE) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (datt->depth_store_op != LC_STORE_OP_STORE &&
            datt->depth_store_op != LC_STORE_OP_DONT_CARE) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (datt->stencil_load_op != LC_LOAD_OP_DONT_CARE ||
            datt->stencil_store_op != LC_STORE_OP_DONT_CARE ||
            datt->clear_stencil != 0) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (datt->depth_load_op == LC_LOAD_OP_LOAD) {
            const lc_image_view *view = datt->view;

            if (view->image == NULL ||
                lc_enc_tracked_at(view->image, view) !=
                    LC_RESOURCE_STATE_DEPTH_ATTACHMENT_WRITE) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
    } else if (target->depth_view != NULL) {
        /* A target with depth must bind it: omitting depth while the
         * framebuffer carries it is a signature mismatch. */
        return LC_ERROR_INVALID_ARGUMENT;
    }

    res = lc_vulkan_offscreen_pass(target->device, target, desc, &pass);
    if (res != LC_SUCCESS) {
        return res;
    }
    if (res != LC_SUCCESS) {
        return res;
    }

    for (i = 0; i < desc->color_attachment_count; i++) {
        clear_values[i].color.float32[0] =
            lc_enc_clamp01(desc->color_attachments[i].clear_color[0]);
        clear_values[i].color.float32[1] =
            lc_enc_clamp01(desc->color_attachments[i].clear_color[1]);
        clear_values[i].color.float32[2] =
            lc_enc_clamp01(desc->color_attachments[i].clear_color[2]);
        clear_values[i].color.float32[3] =
            lc_enc_clamp01(desc->color_attachments[i].clear_color[3]);
    }
    if (desc->depth_attachment != NULL) {
        float d = desc->depth_attachment->clear_depth;

        if (d < 0.0f) {
            d = 0.0f;
        }
        if (d > 1.0f) {
            d = 1.0f;
        }
        clear_values[desc->color_attachment_count]
            .depthStencil.depth = d;
        clear_values[desc->color_attachment_count]
            .depthStencil.stencil = 0;
    }

    flight = lc_enc_flight(swapchain);
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = pass;
    begin_info.framebuffer = target->framebuffer;
    begin_info.renderArea.offset.x = 0;
    begin_info.renderArea.offset.y = 0;
    begin_info.renderArea.extent.width = target->width;
    begin_info.renderArea.extent.height = target->height;
    begin_info.clearValueCount =
        desc->color_attachment_count +
        ((desc->depth_attachment != NULL) ? 1u : 0u);
    begin_info.pClearValues = clear_values;
    vkCmdBeginRenderPass(flight->cmd, &begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);

    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)target->width;
    viewport.height = (float)target->height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(flight->cmd, 0, 1, &viewport);
    memset(&scissor, 0, sizeof(scissor));
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    scissor.extent.width = target->width;
    scissor.extent.height = target->height;
    vkCmdSetScissor(flight->cmd, 0, 1, &scissor);

    enc->in_pass = 1;
    enc->pass_is_swapchain = 0;
    enc->pass_target.width = target->width;
    enc->pass_target.height = target->height;
    enc->pass_target.color_attachment_count = target->color_count;
    for (i = 0; i < target->color_count; i++) {
        enc->pass_target.color_formats[i] = target->color_formats[i];
        enc->end_color_views[i] =
            (lc_image_view *)desc->color_attachments[i].view;
        enc->end_color_stores[i] = desc->color_attachments[i].store_op;
    }
    enc->end_color_count = target->color_count;
    enc->pass_target.depth_stencil_format = target->depth_format;
    enc->pass_target.samples = target->samples;
    enc->pass_target_obj = target;
    if (desc->depth_attachment != NULL) {
        enc->end_depth_view =
            (lc_image_view *)desc->depth_attachment->view;
        enc->end_depth_store = desc->depth_attachment->depth_store_op;
        enc->end_has_depth = 1;
    } else {
        enc->end_depth_view = NULL;
        enc->end_has_depth = 0;
    }
    enc->bound_pipeline = NULL;
    enc->bound_index_buffer = NULL;
    enc->index_bound = 0;
    /* Phase 33: pass begin resets scissor to the full target. */
    enc->scissor.offset_x = 0;
    enc->scissor.offset_y = 0;
    enc->scissor.width = target->width;
    enc->scissor.height = target->height;
    enc->scissor_active = 1;
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_begin_swapchain(
    lc_command_encoder *enc, lc_swapchain *swapchain,
    const lc_render_swapchain_pass_desc *desc) {
    lc_vk_flight *flight;
    lc_vk_pass_key key;
    VkRenderPass pass = VK_NULL_HANDLE;
    VkRenderPassBeginInfo begin_info;
    VkClearValue clear_values[2];
    VkViewport viewport;
    VkRect2D scissor;
    lc_result res;

    if (enc == NULL || swapchain == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->swapchain != swapchain || !lc_enc_frame_ready(swapchain) ||
        enc->in_pass || !lc_enc_legacy_idle(swapchain)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->color_load_op != LC_LOAD_OP_CLEAR) {
        /* Swap images start UNDEFINED every frame: only CLEAR is
         * meaningful for color. */
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->color_store_op != LC_STORE_OP_STORE &&
        desc->color_store_op != LC_STORE_OP_DONT_CARE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->depth_load_op != LC_LOAD_OP_CLEAR &&
        desc->depth_load_op != LC_LOAD_OP_DONT_CARE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->depth_store_op != LC_STORE_OP_STORE &&
        desc->depth_store_op != LC_STORE_OP_DONT_CARE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (swapchain->render_pass == VK_NULL_HANDLE ||
        swapchain->framebuffers == NULL ||
        swapchain->current_image >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }

    memset(&key, 0, sizeof(key));
    key.color_count = 1;
    key.color_formats[0] = swapchain->format;
    key.color_loads[0] = lc_vulkan_translate_load(desc->color_load_op);
    key.color_stores[0] = lc_vulkan_translate_store(desc->color_store_op);
    key.depth_format = swapchain->depth_format;
    key.depth_load = lc_vulkan_translate_load(desc->depth_load_op);
    key.depth_store = lc_vulkan_translate_store(desc->depth_store_op);
    key.samples = VK_SAMPLE_COUNT_1_BIT;
    key.present = 1;

    res = lc_vulkan_pass_cache_get(swapchain->device, &key, &pass);
    if (res != LC_SUCCESS) {
        return res;
    }

    clear_values[0].color.float32[0] = lc_enc_clamp01(desc->clear_color[0]);
    clear_values[0].color.float32[1] = lc_enc_clamp01(desc->clear_color[1]);
    clear_values[0].color.float32[2] = lc_enc_clamp01(desc->clear_color[2]);
    clear_values[0].color.float32[3] = lc_enc_clamp01(desc->clear_color[3]);
    {
        float d = desc->clear_depth;

        if (d < 0.0f) {
            d = 0.0f;
        }
        if (d > 1.0f) {
            d = 1.0f;
        }
        clear_values[1].depthStencil.depth = d;
        clear_values[1].depthStencil.stencil = 0;
    }

    flight = lc_enc_flight(swapchain);
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    /* Generic swapchain passes reuse the legacy per-image framebuffers:
     * same attachment recipe (color + depth), so compatible. */
    begin_info.renderPass = pass;
    begin_info.framebuffer =
        swapchain->framebuffers[swapchain->current_image];
    begin_info.renderArea.offset.x = 0;
    begin_info.renderArea.offset.y = 0;
    begin_info.renderArea.extent = swapchain->extent;
    begin_info.clearValueCount = 2;
    begin_info.pClearValues = clear_values;
    vkCmdBeginRenderPass(flight->cmd, &begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);

    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)swapchain->extent.width;
    viewport.height = (float)swapchain->extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(flight->cmd, 0, 1, &viewport);
    memset(&scissor, 0, sizeof(scissor));
    scissor.extent = swapchain->extent;
    vkCmdSetScissor(flight->cmd, 0, 1, &scissor);

    enc->in_pass = 1;
    enc->pass_is_swapchain = 1;
    enc->pass_target.width = swapchain->extent.width;
    enc->pass_target.height = swapchain->extent.height;
    enc->pass_target.color_attachment_count = 1;
    enc->pass_target.color_formats[0] =
        lc_vulkan_untranslate_format(swapchain->format);
    enc->pass_target.depth_stencil_format =
        lc_vulkan_untranslate_format(swapchain->depth_format);
    enc->pass_target.samples = LC_SAMPLE_COUNT_1;
    enc->pass_target_obj = NULL;
    enc->end_color_count = 0;
    enc->end_has_depth = 0;
    enc->bound_pipeline = NULL;
    enc->bound_index_buffer = NULL;
    enc->index_bound = 0;
    /* Phase 33: pass begin resets scissor to the full target. */
    enc->scissor.offset_x = 0;
    enc->scissor.offset_y = 0;
    enc->scissor.width = swapchain->extent.width;
    enc->scissor.height = swapchain->extent.height;
    enc->scissor_active = 1;
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_end(lc_command_encoder *enc) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;

    if (enc == NULL || enc->swapchain == NULL || !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain)) {
        return LC_ERROR_UNKNOWN;
    }
    flight = lc_enc_flight(swapchain);
    vkCmdEndRenderPass(flight->cmd);

    /* Attachment finals already transitioned the images: adopt the
     * tracked state so second-pass sampling validates. Swapchain
     * images carry no tracking (UNDEFINED -> PRESENT, self-contained).
     * STORE color ends sampled-readable; DONT_CARE decays to UNDEFINED
     * (a later LOAD is then correctly rejected). Depth STORE stays an
     * attachment unless the image carries SAMPLED usage (shadow maps),
     * in which case it ends sampled-readable like color.
     */
    if (!enc->pass_is_swapchain) {
        uint32_t i;

        for (i = 0; i < enc->end_color_count; i++) {
            lc_image_view *view = enc->end_color_views[i];

            if (view != NULL && view->image != NULL) {
                lc_resource_state final =
                    (enc->end_color_stores[i] == LC_STORE_OP_STORE)
                        ? LC_RESOURCE_STATE_SHADER_READ
                        : LC_RESOURCE_STATE_UNDEFINED;
                lc_vulkan_image_notify_range(
                    view->image, view->base_mip_level,
                    view->mip_level_count, view->base_array_layer,
                    view->array_layer_count, final);
            }
        }
        if (enc->end_has_depth && enc->end_depth_view != NULL &&
            enc->end_depth_view->image != NULL) {
            lc_image_view *view = enc->end_depth_view;
            lc_resource_state final =
                (enc->end_depth_store == LC_STORE_OP_STORE)
                    ? (((view->image->usage & LC_IMAGE_USAGE_SAMPLED) !=
                        0)
                           ? LC_RESOURCE_STATE_SHADER_READ
                           : LC_RESOURCE_STATE_DEPTH_ATTACHMENT_WRITE)
                    : LC_RESOURCE_STATE_UNDEFINED;
            lc_vulkan_image_notify_range(
                view->image, view->base_mip_level,
                view->mip_level_count, view->base_array_layer,
                view->array_layer_count, final);
        }
    }
    enc->in_pass = 0;
    enc->pass_is_swapchain = 0;
    enc->bound_pipeline = NULL;
    enc->bound_index_buffer = NULL;
    enc->index_bound = 0;
    enc->end_color_count = 0;
    enc->end_has_depth = 0;
    enc->scissor_active = 0;
    return LC_SUCCESS;
}

/* Phase 33 scissor backend: validate-then-record (validation lives
 * in encoder.c; re-checked here defensively). */
lc_result lc_vulkan_encoder_scissor(lc_command_encoder *enc,
                                    const lc_scissor_rect *rect) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    VkRect2D vk_rect;

    if (enc == NULL || rect == NULL || enc->swapchain == NULL ||
        !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain)) {
        return LC_ERROR_UNKNOWN;
    }
    if (rect->offset_x < 0 || rect->offset_y < 0 || rect->width == 0 ||
        rect->height == 0 || rect->width > enc->pass_target.width ||
        rect->height > enc->pass_target.height ||
        (uint32_t)rect->offset_x > enc->pass_target.width - rect->width ||
        (uint32_t)rect->offset_y >
            enc->pass_target.height - rect->height) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    memset(&vk_rect, 0, sizeof(vk_rect));
    vk_rect.offset.x = rect->offset_x;
    vk_rect.offset.y = rect->offset_y;
    vk_rect.extent.width = rect->width;
    vk_rect.extent.height = rect->height;
    vkCmdSetScissor(flight->cmd, 0, 1, &vk_rect);
    enc->scissor = *rect;
    enc->scissor_active = 1;
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_bind(lc_command_encoder *enc,
                                  const lc_pipeline *pipeline) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;

    if (enc != NULL && enc->worker_mode) {
        return lc_worker_record_bind_pipeline(enc, pipeline);
    }
    if (enc == NULL || pipeline == NULL || enc->swapchain == NULL ||
        !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) ||
        pipeline->layout == VK_NULL_HANDLE ||
        pipeline->pipeline == VK_NULL_HANDLE) {
        return LC_ERROR_UNKNOWN;
    }
    flight = lc_enc_flight(swapchain);
    vkCmdBindPipeline(flight->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipeline->pipeline);
    enc->bound_pipeline = pipeline;
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_bind_set(lc_command_encoder *enc,
                                     const lc_pipeline *pipeline,
                                     uint32_t slot,
                                     lc_binding_set *set) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;

    if (enc != NULL && enc->worker_mode) {
        return lc_worker_record_bind_set(enc, pipeline, slot, set);
    }
    if (enc == NULL || pipeline == NULL || set == NULL ||
        enc->swapchain == NULL || !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) ||
        slot >= pipeline->layout_count || set->vk_set == VK_NULL_HANDLE ||
        pipeline->layout == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    vkCmdBindDescriptorSets(flight->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline->layout, slot, 1, &set->vk_set, 0,
                            NULL);
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_bind_vertex(lc_command_encoder *enc,
                                        uint32_t binding,
                                        const lc_buffer *buffer,
                                        uint64_t offset) {
    VkPhysicalDeviceProperties props;
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    VkDeviceSize vk_offset;

    if (enc != NULL && enc->worker_mode) {
        return lc_worker_record_bind_vertex(enc, binding, buffer,
                                            offset);
    }
    if (enc == NULL || buffer == NULL || enc->swapchain == NULL ||
        !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) ||
        buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_UNKNOWN;
    }
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(swapchain->device->physical_device, &props);
    if (binding >= props.limits.maxVertexInputBindings) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    vk_offset = (VkDeviceSize)offset;
    vkCmdBindVertexBuffers(flight->cmd, binding, 1, &buffer->vk_buffer,
                           &vk_offset);
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_bind_index(lc_command_encoder *enc,
                                       const lc_buffer *buffer,
                                       uint64_t offset,
                                       lc_index_type index_type) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    VkIndexType vk_type;

    if (enc != NULL && enc->worker_mode) {
        return lc_worker_record_bind_index(enc, buffer, offset,
                                           index_type);
    }
    if (enc == NULL || buffer == NULL || enc->swapchain == NULL ||
        !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) ||
        buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_UNKNOWN;
    }
    if (index_type != LC_INDEX_UINT16 && index_type != LC_INDEX_UINT32) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    vk_type = (index_type == LC_INDEX_UINT16) ? VK_INDEX_TYPE_UINT16
                                              : VK_INDEX_TYPE_UINT32;
    flight = lc_enc_flight(swapchain);
    vkCmdBindIndexBuffer(flight->cmd, buffer->vk_buffer,
                         (VkDeviceSize)offset, vk_type);
    enc->bound_index_buffer = buffer;
    enc->bound_index_offset = offset;
    enc->bound_index_type = index_type;
    enc->index_bound = 1;
    return LC_SUCCESS;
}

static VkShaderStageFlags lc_enc_push_stages(uint32_t visibility) {
    VkShaderStageFlags stages = 0;

    if ((visibility & LC_SHADER_VISIBILITY_VERTEX) != 0) {
        stages |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if ((visibility & LC_SHADER_VISIBILITY_FRAGMENT) != 0) {
        stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    return stages;
}

lc_result lc_vulkan_encoder_push(lc_command_encoder *enc,
                                 const lc_pipeline *pipeline,
                                 uint32_t visibility, uint32_t offset,
                                 uint32_t size, const void *data) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    VkShaderStageFlags stages;

    if (enc != NULL && enc->worker_mode) {
        return lc_worker_record_push(enc, pipeline, visibility, offset,
                                     size, data);
    }
    if (enc == NULL || pipeline == NULL || data == NULL ||
        enc->swapchain == NULL || !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) ||
        pipeline->layout == VK_NULL_HANDLE) {
        return LC_ERROR_UNKNOWN;
    }
    if (enc->bound_pipeline != pipeline) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    stages = lc_enc_push_stages(visibility);
    if (stages == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    vkCmdPushConstants(flight->cmd, pipeline->layout, stages, offset, size,
                       data);
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_draw(lc_command_encoder *enc,
                                 uint32_t vertex_count,
                                 uint32_t first_vertex) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;

    if (enc != NULL && enc->worker_mode) {
        return lc_worker_record_draw(enc, vertex_count, first_vertex);
    }
    if (enc == NULL || enc->swapchain == NULL || !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) || enc->bound_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    vkCmdDraw(flight->cmd, vertex_count, 1, first_vertex, 0);
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_draw_indexed(
    lc_command_encoder *enc, uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;

    if (enc != NULL && enc->worker_mode) {
        return lc_worker_record_draw_indexed(enc, index_count, instance_count, first_index, vertex_offset, first_instance);
    }
    if (enc == NULL || enc->swapchain == NULL || !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) || enc->bound_pipeline == NULL ||
        !enc->index_bound || enc->bound_index_buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    vkCmdDrawIndexed(flight->cmd, index_count, instance_count, first_index,
                     vertex_offset, first_instance);
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_draw_instanced(
    lc_command_encoder *enc, uint32_t vertex_count, uint32_t instance_count,
    uint32_t first_vertex, uint32_t first_instance) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;

    if (enc != NULL && enc->worker_mode) {
        return lc_worker_record_draw_instanced(enc, vertex_count, instance_count, first_vertex, first_instance);
    }
    if (enc == NULL || enc->swapchain == NULL || !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) || enc->bound_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    vkCmdDraw(flight->cmd, vertex_count, instance_count, first_vertex,
              first_instance);
    return LC_SUCCESS;
}

/* Explicit image transition into the frame command buffer.
 * Allowed between passes (the primary use: end A, transition,
 * begin B); never inside an open pass and never outside an open
 * frame. Phase 22, PART U: explicit transitions of any kind are
 * illegal inside render-pass instances (passes carry no
 * self-dependency for vkCmdPipelineBarrier); attachment flow
 * rides render-pass begin/end semantics instead. */
lc_result lc_vulkan_encoder_transition(lc_command_encoder *enc,
                                       lc_image *image,
                                       uint32_t base_mip,
                                       uint32_t level_count,
                                       uint32_t base_layer,
                                       uint32_t layer_count,
                                       lc_resource_state new_state) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;

    if (enc == NULL || image == NULL || enc->swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->worker_mode) {
        return lc_worker_record_transition(
            enc, image, base_mip, level_count, base_layer, layer_count,
            new_state);
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->in_pass || swapchain->rp_open) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (image->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    return lc_vulkan_encoder_transition_image(
        flight->cmd, image, base_mip, level_count, base_layer,
        layer_count, new_state);
}

/* ------------------------------------------------------------------ */
/* Compute + indirect (Phase 21). Dispatch records outside render     */
/* passes only; indirect draws follow the same rules as direct draws. */
/* ------------------------------------------------------------------ */

lc_result lc_vulkan_encoder_bind_compute_pipeline(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;

    if (enc == NULL || pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->worker_mode) {
        return lc_worker_record_bind_compute_pipeline(enc, pipeline);
    }
    if (enc->swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain)) {
        return LC_ERROR_UNKNOWN;
    }
    if (pipeline->device != swapchain->device ||
        pipeline->pipeline == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    vkCmdBindPipeline(flight->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline->pipeline);
    enc->bound_compute_pipeline = pipeline;
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_dispatch(lc_command_encoder *enc, uint32_t x,
                                     uint32_t y, uint32_t z) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    const lc_device *device;

    if (enc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->worker_mode) {
        return lc_worker_record_dispatch(enc, x, y, z);
    }
    if (enc->swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    /* Dispatch outside render passes (Vulkan forbids dispatch
     * inside a pass instance) but inside an open frame. Pending
     * legacy clears compose: they realize as a pass at submit
     * time, after this dispatch, so only an OPEN pass (explicit
     * or legacy) is rejected. */
    if (!lc_enc_frame_ready(swapchain) || enc->in_pass ||
        swapchain->rp_open) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_compute_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = swapchain->device;
    if (x == 0 || y == 0 || z == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (x > device->max_workgroup_count[0] ||
        y > device->max_workgroup_count[1] ||
        z > device->max_workgroup_count[2]) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    vkCmdDispatch(flight->cmd, x, y, z);
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_push_compute(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline,
    uint32_t visibility, uint32_t offset, uint32_t size,
    const void *data) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    VkShaderStageFlags stages = 0;

    if (enc == NULL || pipeline == NULL || data == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->worker_mode) {
        return lc_worker_record_push_compute(enc, pipeline, visibility,
                                             offset, size, data);
    }
    if (enc->swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    /* Same outside-pass rule as dispatch (push writes the same
     * command stream; pending clears compose at submit). */
    if (!lc_enc_frame_ready(swapchain) || enc->in_pass ||
        swapchain->rp_open) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_compute_pipeline != pipeline ||
        pipeline->layout == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((visibility & LC_SHADER_VISIBILITY_COMPUTE) != 0) {
        stages |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    if (stages == 0 || size == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    vkCmdPushConstants(flight->cmd, pipeline->layout, stages, offset,
                       size, data);
    return LC_SUCCESS;
}

/* Indirect layouts are backend-neutral by contract; assert the
 * Vulkan equivalence once here (D3D12 maps the same fields). */
_Static_assert(sizeof(lc_indirect_draw_command) == 16,
               "lc_indirect_draw_command must be 16 bytes");
_Static_assert(sizeof(lc_indirect_draw_indexed_command) == 20,
               "lc_indirect_draw_indexed_command must be 20 bytes");
_Static_assert(sizeof(lc_indirect_draw_command) ==
                   sizeof(VkDrawIndirectCommand),
               "non-indexed indirect layout must match Vulkan");
_Static_assert(sizeof(lc_indirect_draw_indexed_command) ==
                   sizeof(VkDrawIndexedIndirectCommand),
               "indexed indirect layout must match Vulkan");

/* Validate one indirect batch (shared frame/worker rules). */
lc_result lc_vk_indirect_batch_valid(const lc_buffer *buffer,
                                     uint64_t offset, uint32_t draw_count,
                                     uint32_t stride, uint32_t elem_size) {
    uint64_t need = 0;

    if (buffer == NULL || buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((buffer->usage & LC_BUFFER_USAGE_INDIRECT) == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (draw_count == 0 || stride < elem_size ||
        (stride % 4u) != 0u || (offset % 4u) != 0u) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    need = (uint64_t)draw_count * (uint64_t)stride;
    if (need > buffer->size || offset > buffer->size - need) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_draw_indirect(lc_command_encoder *enc,
                                          lc_buffer *buffer, uint64_t offset,
                                          uint32_t draw_count,
                                          uint32_t stride) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    lc_result res;

    if (enc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->worker_mode) {
        return lc_worker_record_draw_indirect(enc, buffer, offset,
                                              draw_count, stride);
    }
    if (enc->swapchain == NULL || !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) || enc->bound_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vk_indirect_batch_valid(buffer, offset, draw_count, stride,
                                     (uint32_t)sizeof(
                                         lc_indirect_draw_command));
    if (res != LC_SUCCESS) {
        return res;
    }
    flight = lc_enc_flight(swapchain);
    /* The indirect buffer must already be INDIRECT_READ: barriers
     * are illegal inside a render pass (passes carry no
     * self-dependency), so producers transition before the pass
     * opens (renderer prepare does this). Loud reject otherwise. */
    {
        lc_resource_state st;

        lc_device_lock_transfer(swapchain->device);
        st = buffer->buffer_state;
        lc_device_unlock_transfer(swapchain->device);
        if (st != LC_RESOURCE_STATE_INDIRECT_READ) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    if (draw_count == 1) {
        vkCmdDrawIndirect(flight->cmd, buffer->vk_buffer,
                          (VkDeviceSize)offset, 1, stride);
        return LC_SUCCESS;
    }
    if (swapchain->device->multi_draw_indirect != 0) {
        vkCmdDrawIndirect(flight->cmd, buffer->vk_buffer,
                          (VkDeviceSize)offset, draw_count, stride);
        return LC_SUCCESS;
    }
    /* Compatibility loop: N single indirect draws (honest fallback,
     * counted as such in stats by the caller). */
    {
        uint32_t i;

        for (i = 0; i < draw_count; i++) {
            vkCmdDrawIndirect(flight->cmd, buffer->vk_buffer,
                              (VkDeviceSize)offset +
                                  (VkDeviceSize)i * (VkDeviceSize)stride,
                              1, stride);
        }
    }
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_draw_indexed_indirect(
    lc_command_encoder *enc, lc_buffer *buffer, uint64_t offset,
    uint32_t draw_count, uint32_t stride) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    lc_result res;

    if (enc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->worker_mode) {
        return lc_worker_record_draw_indexed_indirect(
            enc, buffer, offset, draw_count, stride);
    }
    if (enc->swapchain == NULL || !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) || enc->bound_pipeline == NULL ||
        !enc->index_bound || enc->bound_index_buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vk_indirect_batch_valid(
        buffer, offset, draw_count, stride,
        (uint32_t)sizeof(lc_indirect_draw_indexed_command));
    if (res != LC_SUCCESS) {
        return res;
    }
    flight = lc_enc_flight(swapchain);
    /* Same outside-pass rule: the producer transitions before the
     * pass opens (barriers are illegal inside passes). */
    {
        lc_resource_state st;

        lc_device_lock_transfer(swapchain->device);
        st = buffer->buffer_state;
        lc_device_unlock_transfer(swapchain->device);
        if (st != LC_RESOURCE_STATE_INDIRECT_READ) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    if (draw_count == 1 || swapchain->device->multi_draw_indirect != 0) {
        vkCmdDrawIndexedIndirect(flight->cmd, buffer->vk_buffer,
                                 (VkDeviceSize)offset, draw_count,
                                 stride);
        return LC_SUCCESS;
    }
    {
        uint32_t i;

        for (i = 0; i < draw_count; i++) {
            vkCmdDrawIndexedIndirect(flight->cmd, buffer->vk_buffer,
                                     (VkDeviceSize)offset +
                                         (VkDeviceSize)i *
                                             (VkDeviceSize)stride,
                                     1, stride);
        }
    }
    return LC_SUCCESS;
}

/* Validate a GPU-count buffer (shared frame/worker rules): the
 * Vulkan count buffer carries INDIRECT usage and one uint32. */
lc_result lc_vk_indirect_count_valid(const lc_buffer *count_buffer,
                                     uint64_t count_offset,
                                     uint32_t max_draw_count) {
    if (count_buffer == NULL ||
        count_buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((count_buffer->usage & LC_BUFFER_USAGE_INDIRECT) == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (max_draw_count == 0 || (count_offset % 4u) != 0u) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (count_offset + 4u > count_buffer->size) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return LC_SUCCESS;
}

/* Both command and count buffers must already be INDIRECT_READ
 * (same outside-pass rule as fixed-count draws). */
static lc_result lc_vk_indirect_count_ready(lc_device *device,
                                            const lc_buffer *buffer,
                                            const lc_buffer *count_buffer) {
    lc_resource_state command_state;
    lc_resource_state count_state;

    lc_device_lock_transfer(device);
    command_state = buffer->buffer_state;
    count_state = count_buffer->buffer_state;
    lc_device_unlock_transfer(device);
    if (command_state != LC_RESOURCE_STATE_INDIRECT_READ ||
        count_state != LC_RESOURCE_STATE_INDIRECT_READ) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_draw_indirect_count(
    lc_command_encoder *enc, lc_buffer *buffer, uint64_t offset,
    lc_buffer *count_buffer, uint64_t count_offset,
    uint32_t max_draw_count, uint32_t stride) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    lc_result res;

    if (enc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->worker_mode) {
        return lc_worker_record_draw_indirect_count(
            enc, buffer, offset, count_buffer, count_offset,
            max_draw_count, stride);
    }
    if (enc->swapchain == NULL || !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) || enc->bound_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vk_indirect_batch_valid(buffer, offset, max_draw_count,
                                     stride,
                                     (uint32_t)sizeof(
                                         lc_indirect_draw_command));
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vk_indirect_count_valid(count_buffer, count_offset,
                                     max_draw_count);
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vk_indirect_count_ready(swapchain->device, buffer,
                                     count_buffer);
    if (res != LC_SUCCESS) {
        return res;
    }
    flight = lc_enc_flight(swapchain);
    if (swapchain->device->indirect_count_supported != 0) {
        vkCmdDrawIndirectCount(flight->cmd, buffer->vk_buffer,
                               (VkDeviceSize)offset,
                               count_buffer->vk_buffer,
                               (VkDeviceSize)count_offset,
                               max_draw_count, stride);
        return LC_SUCCESS;
    }
    /* GPU-driven fallback (no count consumption): draw the whole
     * bound. Producers zero instance_count on unused slots, so the
     * extras are valid no-op draws and no CPU count readback is
     * ever needed. */
    {
        uint32_t i;

        for (i = 0; i < max_draw_count; i++) {
            vkCmdDrawIndirect(flight->cmd, buffer->vk_buffer,
                              (VkDeviceSize)offset +
                                  (VkDeviceSize)i * (VkDeviceSize)stride,
                              1, stride);
        }
    }
    return LC_SUCCESS;
}

lc_result lc_vulkan_encoder_draw_indexed_indirect_count(
    lc_command_encoder *enc, lc_buffer *buffer, uint64_t offset,
    lc_buffer *count_buffer, uint64_t count_offset,
    uint32_t max_draw_count, uint32_t stride) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    lc_result res;

    if (enc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->worker_mode) {
        return lc_worker_record_draw_indexed_indirect_count(
            enc, buffer, offset, count_buffer, count_offset,
            max_draw_count, stride);
    }
    if (enc->swapchain == NULL || !enc->in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain) || enc->bound_pipeline == NULL ||
        !enc->index_bound || enc->bound_index_buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vk_indirect_batch_valid(
        buffer, offset, max_draw_count, stride,
        (uint32_t)sizeof(lc_indirect_draw_indexed_command));
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vk_indirect_count_valid(count_buffer, count_offset,
                                     max_draw_count);
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vk_indirect_count_ready(swapchain->device, buffer,
                                     count_buffer);
    if (res != LC_SUCCESS) {
        return res;
    }
    flight = lc_enc_flight(swapchain);
    if (swapchain->device->indirect_count_supported != 0) {
        vkCmdDrawIndexedIndirectCount(
            flight->cmd, buffer->vk_buffer, (VkDeviceSize)offset,
            count_buffer->vk_buffer, (VkDeviceSize)count_offset,
            max_draw_count, stride);
        return LC_SUCCESS;
    }
    {
        uint32_t i;

        for (i = 0; i < max_draw_count; i++) {
            vkCmdDrawIndexedIndirect(flight->cmd, buffer->vk_buffer,
                                     (VkDeviceSize)offset +
                                         (VkDeviceSize)i *
                                             (VkDeviceSize)stride,
                                     1, stride);
        }
    }
    return LC_SUCCESS;
}

/* Explicit buffer transition into the frame command buffer. Allowed
 * between passes as well as inside passes; never outside an open
 * frame. Worker encoders log the intent for execute-time
 * reconciliation. */
lc_result lc_vulkan_encoder_transition_buffer(lc_command_encoder *enc,
                                              lc_buffer *buffer,
                                              lc_resource_state new_state) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;
    lc_result res;

    if (enc == NULL || buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (buffer->device == NULL ||
        buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_state_valid_for_buffer(new_state)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->worker_mode) {
        return lc_worker_record_transition_buffer(enc, buffer,
                                                  new_state);
    }
    if (enc->swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Buffer barriers are illegal inside a render pass (passes
     * carry no self-dependency): reject open passes. Image
     * transitions allow in-pass use (pre-existing behavior). */
    if (enc->in_pass || swapchain->rp_open) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (buffer->device != swapchain->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    lc_device_lock_transfer(swapchain->device);
    res = lc_vulkan_buffer_transition(flight->cmd, buffer, new_state);
    lc_device_unlock_transfer(swapchain->device);
    return res;
}

lc_result lc_vulkan_encoder_bind_compute_set(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline,
    uint32_t slot, lc_binding_set *set) {
    lc_swapchain *swapchain;
    lc_vk_flight *flight;

    if (enc == NULL || pipeline == NULL || set == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->worker_mode) {
        return lc_worker_record_bind_compute_set(enc, pipeline, slot,
                                                 set);
    }
    if (enc->swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    swapchain = enc->swapchain;
    if (!lc_enc_frame_ready(swapchain)) {
        return LC_ERROR_UNKNOWN;
    }
    if (slot >= pipeline->layout_count ||
        set->vk_set == VK_NULL_HANDLE ||
        pipeline->layout == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = lc_enc_flight(swapchain);
    vkCmdBindDescriptorSets(flight->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline->layout, slot, 1, &set->vk_set, 0,
                            NULL);
    return LC_SUCCESS;
}
