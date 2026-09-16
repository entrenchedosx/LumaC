/*
 * Vulkan synchronization layer (Phase 19: AAA foundation).
 *
 * Backend-neutral semantic states (lc_resource_state) drive every
 * image barrier; Vulkan layouts/stages/access masks are derived
 * centrally and never leak into callers. Subresource granularity:
 * each mip/layer tracks its own state, so mixed usage (mip 0
 * sampled while mip 1 renders) stays representable.
 *
 * Two emission contexts:
 * - immediate (device upload context, submit + wait): transfers,
 *   uploads, readbacks, mip generation;
 * - recorded (frame command buffer): lc_encoder_transition_image.
 * Tracking updates immediately in both (recorded-not-executed
 * discipline, same as render-pass finals).
 *
 * Queue note (PARTs AM/AN): all barriers use IGNORED queue
 * families (single graphics queue today). Multi-queue scheduling
 * will add ownership transfers here without changing the state
 * model or callers.
 */

#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

static size_t lc_vk_state_index(const lc_image *image, uint32_t mip,
                                uint32_t layer) {
    return (size_t)layer * image->mip_levels + mip;
}

/* Total mapper for image-applicable states. Returns 1 with layout /
 * stage / access filled, or 0 when the state never applies to
 * images (buffer-only states, PRESENT-as-endpoint excepted below).
 * `for_read` selects the access flavor for attachment states used
 * as barrier sources (writes) vs destinations. */
static int lc_vk_state_map(lc_resource_state state,
                           VkImageLayout *layout,
                           VkPipelineStageFlags *stage,
                           VkAccessFlags *access) {
    switch (state) {
    case LC_RESOURCE_STATE_UNDEFINED:
        /* UNDEFINED is source-only (discard); never a destination. */
        *layout = VK_IMAGE_LAYOUT_UNDEFINED;
        *stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        *access = 0;
        return 1;
    case LC_RESOURCE_STATE_COLOR_ATTACHMENT_WRITE:
        *layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        *stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        *access = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        return 1;
    case LC_RESOURCE_STATE_DEPTH_ATTACHMENT_WRITE:
        *layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        *stage = (VkPipelineStageFlags)(
            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT);
        *access = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        return 1;
    case LC_RESOURCE_STATE_SHADER_READ:
        *layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        *stage = (VkPipelineStageFlags)(
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        *access = VK_ACCESS_SHADER_READ_BIT;
        return 1;
    case LC_RESOURCE_STATE_SHADER_READ_WRITE:
        *layout = VK_IMAGE_LAYOUT_GENERAL;
        *stage = (VkPipelineStageFlags)(
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        *access = (VkAccessFlags)(VK_ACCESS_SHADER_READ_BIT |
                                  VK_ACCESS_SHADER_WRITE_BIT);
        return 1;
    case LC_RESOURCE_STATE_TRANSFER_SRC:
        *layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_READ_BIT;
        return 1;
    case LC_RESOURCE_STATE_TRANSFER_DST:
        *layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        *stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *access = VK_ACCESS_TRANSFER_WRITE_BIT;
        return 1;
    case LC_RESOURCE_STATE_PRESENT:
        *layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        *stage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
        *access = 0;
        return 1;
    default:
        /* Buffer-only states never apply to images. */
        return 0;
    }
}

int lc_vk_sync_state_valid_for_image(lc_resource_state state) {
    VkImageLayout layout;
    VkPipelineStageFlags stage = 0;
    VkAccessFlags access = 0;

    if (state == LC_RESOURCE_STATE_UNDEFINED ||
        state == LC_RESOURCE_STATE_PRESENT) {
        return 0;
    }
    return lc_vk_state_map(state, &layout, &stage, &access);
}

/* ------------------------------------------------------------------ */
/* Buffer states (Phase 21, PART J). Whole-resource tracking for      */
/* compute/indirect ordering. UNDEFINED is source-only (first use).   */
/* ------------------------------------------------------------------ */

int lc_vk_sync_state_valid_for_buffer(lc_resource_state state) {
    switch (state) {
    case LC_RESOURCE_STATE_TRANSFER_SRC:
    case LC_RESOURCE_STATE_TRANSFER_DST:
    case LC_RESOURCE_STATE_VERTEX_READ:
    case LC_RESOURCE_STATE_INDEX_READ:
    case LC_RESOURCE_STATE_UNIFORM_READ:
    case LC_RESOURCE_STATE_STORAGE_READ:
    case LC_RESOURCE_STATE_STORAGE_WRITE:
    case LC_RESOURCE_STATE_STORAGE_READ_WRITE:
    case LC_RESOURCE_STATE_INDIRECT_READ:
        return 1;
    default:
        return 0;
    }
}

int lc_vk_sync_buffer_barrier_params(lc_resource_state state,
                                     VkPipelineStageFlags *out_stage,
                                     VkAccessFlags *out_access) {
    VkPipelineStageFlags shader_read;

    if (out_stage == NULL || out_access == NULL) {
        return 0;
    }
    shader_read = (VkPipelineStageFlags)(
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    switch (state) {
    case LC_RESOURCE_STATE_UNDEFINED:
        *out_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        *out_access = 0;
        return 1;
    case LC_RESOURCE_STATE_TRANSFER_SRC:
        *out_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *out_access = VK_ACCESS_TRANSFER_READ_BIT;
        return 1;
    case LC_RESOURCE_STATE_TRANSFER_DST:
        *out_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        *out_access = VK_ACCESS_TRANSFER_WRITE_BIT;
        return 1;
    case LC_RESOURCE_STATE_VERTEX_READ:
        *out_stage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        *out_access = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
        return 1;
    case LC_RESOURCE_STATE_INDEX_READ:
        *out_stage = VK_PIPELINE_STAGE_VERTEX_INPUT_BIT;
        *out_access = VK_ACCESS_INDEX_READ_BIT;
        return 1;
    case LC_RESOURCE_STATE_UNIFORM_READ:
        *out_stage = shader_read;
        *out_access = VK_ACCESS_UNIFORM_READ_BIT;
        return 1;
    case LC_RESOURCE_STATE_STORAGE_READ:
        *out_stage = shader_read;
        *out_access = VK_ACCESS_SHADER_READ_BIT;
        return 1;
    case LC_RESOURCE_STATE_STORAGE_WRITE:
        *out_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        *out_access = VK_ACCESS_SHADER_WRITE_BIT;
        return 1;
    case LC_RESOURCE_STATE_STORAGE_READ_WRITE:
        /* Phase 23: read-modify-write storage (e.g. LOD history):
         * compute-stage ordered with both read and write access
         * (the D3D12 UAV-barrier equivalent). */
        *out_stage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        *out_access = (VkAccessFlags)(
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        return 1;
    case LC_RESOURCE_STATE_INDIRECT_READ:
        *out_stage = VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT;
        *out_access = VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
        return 1;
    default:
        return 0;
    }
}

void lc_vk_sync_record_buffer(VkCommandBuffer cmd, const lc_buffer *buffer,
                              uint64_t offset, uint64_t size,
                              VkPipelineStageFlags src_stage,
                              VkAccessFlags src_access,
                              VkPipelineStageFlags dst_stage,
                              VkAccessFlags dst_access,
                              uint32_t src_family, uint32_t dst_family) {
    VkBufferMemoryBarrier barrier;

    if (cmd == VK_NULL_HANDLE || buffer == NULL ||
        buffer->vk_buffer == VK_NULL_HANDLE) {
        return;
    }
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.srcQueueFamilyIndex = src_family;
    barrier.dstQueueFamilyIndex = dst_family;
    barrier.buffer = buffer->vk_buffer;
    barrier.offset = (VkDeviceSize)offset;
    barrier.size = (VkDeviceSize)size;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 1,
                         &barrier, 0, NULL);
}

void lc_vk_sync_mark_buffer(lc_buffer *buffer, lc_resource_state state) {
    if (buffer == NULL) {
        return;
    }
    buffer->buffer_state = state;
}

/* Barrier from tracked state into cmd, then mark. UNDEFINED source
 * uses TOP_OF_PIPE/0 (first use, like images). */
lc_result lc_vulkan_buffer_transition(VkCommandBuffer cmd, lc_buffer *buffer,
                                      lc_resource_state new_state) {
    VkPipelineStageFlags src_stage = 0;
    VkPipelineStageFlags dst_stage = 0;
    VkAccessFlags src_access = 0;
    VkAccessFlags dst_access = 0;
    lc_resource_state old_state;

    if (cmd == VK_NULL_HANDLE || buffer == NULL ||
        buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_state_valid_for_buffer(new_state)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    old_state = buffer->buffer_state;
    if (old_state != LC_RESOURCE_STATE_UNDEFINED &&
        !lc_vk_sync_state_valid_for_buffer(old_state)) {
        old_state = LC_RESOURCE_STATE_UNDEFINED;
    }
    if (!lc_vk_sync_buffer_barrier_params(old_state, &src_stage,
                                          &src_access) ||
        !lc_vk_sync_buffer_barrier_params(new_state, &dst_stage,
                                          &dst_access)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (old_state == new_state) {
        return LC_SUCCESS;
    }
    lc_vk_sync_record_buffer(cmd, buffer, 0, buffer->size, src_stage,
                             src_access, dst_stage, dst_access,
                             VK_QUEUE_FAMILY_IGNORED,
                             VK_QUEUE_FAMILY_IGNORED);
    buffer->buffer_state = new_state;
    return LC_SUCCESS;
}

/* Barrier parameters for one semantic state (transfer engine).
 * UNDEFINED/PRESENT have no barrier meaning here: rejected. */
int lc_vk_sync_barrier_params(lc_resource_state state,
                              VkImageLayout *out_layout,
                              VkPipelineStageFlags *out_stage,
                              VkAccessFlags *out_access) {
    VkImageLayout layout;
    VkPipelineStageFlags stage = 0;
    VkAccessFlags access = 0;

    if (out_layout == NULL || out_stage == NULL || out_access == NULL) {
        return 0;
    }
    if (state == LC_RESOURCE_STATE_UNDEFINED ||
        state == LC_RESOURCE_STATE_PRESENT) {
        return 0;
    }
    if (!lc_vk_state_map(state, &layout, &stage, &access)) {
        return 0;
    }
    if (layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        return 0;
    }
    *out_layout = layout;
    *out_stage = stage;
    *out_access = access;
    return 1;
}

int lc_vk_sync_validate_range(const lc_image *image, uint32_t base_mip,
                              uint32_t level_count, uint32_t base_layer,
                              uint32_t layer_count) {
    if (image == NULL || image->states == NULL) {
        return 0;
    }
    if (level_count == 0 || layer_count == 0 ||
        base_mip >= image->mip_levels ||
        level_count > image->mip_levels - base_mip ||
        base_layer >= image->array_layers ||
        layer_count > image->array_layers - base_layer) {
        return 0;
    }
    return 1;
}

/* Record one barrier for an explicit subresource span. Old layouts
 * come from tracking (callers mark after success). */
static void lc_vk_sync_record(VkCommandBuffer cmd, VkImage image,
                              VkImageAspectFlags aspect, uint32_t base_mip,
                              uint32_t level_count, uint32_t base_layer,
                              uint32_t layer_count,
                              VkPipelineStageFlags src_stage,
                              VkAccessFlags src_access,
                              VkPipelineStageFlags dst_stage,
                              VkAccessFlags dst_access,
                              VkImageLayout old_layout,
                              VkImageLayout new_layout) {
    VkImageMemoryBarrier barrier;

    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect;
    barrier.subresourceRange.baseMipLevel = base_mip;
    barrier.subresourceRange.levelCount = level_count;
    barrier.subresourceRange.baseArrayLayer = base_layer;
    barrier.subresourceRange.layerCount = layer_count;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
}

void lc_vk_sync_mark(lc_image *image, uint32_t base_mip,
                     uint32_t level_count, uint32_t base_layer,
                     uint32_t layer_count, lc_resource_state state) {
    uint32_t layer;
    uint32_t mip;
    uint64_t seq = 0;

    if (!lc_vk_sync_validate_range(image, base_mip, level_count,
                                   base_layer, layer_count)) {
        return;
    }
    if (image->device == NULL) {
        return;
    }
    /* Locked: marks nest inside reconciled executes (recursive
     * state shard). Stamps the submission sequence (PART L). */
    lc_device_lock_state(image->device);
    image->device->submission_seq++;
    seq = image->device->submission_seq;
    for (layer = base_layer; layer < base_layer + layer_count;
         layer++) {
        for (mip = base_mip; mip < base_mip + level_count; mip++) {
            size_t idx =
                (size_t)layer * image->mip_levels + mip;

            image->states[idx] = state;
            if (image->epochs != NULL) {
                image->epochs[idx] = seq;
            }
        }
    }
    lc_device_unlock_state(image->device);
}

int lc_vk_sync_all_equal(const lc_image *image, uint32_t base_mip,
                         uint32_t level_count, uint32_t base_layer,
                         uint32_t layer_count, lc_resource_state state) {
    uint32_t layer;
    uint32_t mip;
    int equal = 0;

    if (!lc_vk_sync_validate_range(image, base_mip, level_count,
                                   base_layer, layer_count)) {
        return 0;
    }
    lc_device_lock_state((lc_device *)image->device);
    equal = 1;
    for (layer = base_layer; layer < base_layer + layer_count;
         layer++) {
        for (mip = base_mip; mip < base_mip + level_count; mip++) {
            if (image->states[(size_t)layer * image->mip_levels +
                              mip] != state) {
                equal = 0;
                break;
            }
        }
        if (!equal) {
            break;
        }
    }
    lc_device_unlock_state((lc_device *)image->device);
    return equal;
}

/* Immediate transition of a range (upload context, submit + wait).
 * Per-subresource old states (each entry may legally differ). */
lc_result lc_vulkan_image_transition_range(
    lc_image *image, uint32_t base_mip, uint32_t level_count,
    uint32_t base_layer, uint32_t layer_count,
    lc_resource_state new_state) {
    VkImageLayout new_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags dst_stage = 0;
    VkAccessFlags dst_access = 0;
    VkImageAspectFlags aspect = 0;
    uint32_t layer;
    uint32_t mip;
    lc_result res;

    if (image == NULL || image->device == NULL ||
        image->states == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_validate_range(image, base_mip, level_count,
                                   base_layer, layer_count)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_state_valid_for_image(new_state) ||
        !lc_vk_state_map(new_state, &new_layout, &dst_stage,
                         &dst_access)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (image->device->device == VK_NULL_HANDLE ||
        image->vk_image == VK_NULL_HANDLE) {
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    /* Validate every source endpoint before recording anything. */
    for (layer = base_layer; layer < base_layer + layer_count;
         layer++) {
        for (mip = base_mip; mip < base_mip + level_count; mip++) {
            lc_resource_state old =
                image->states[lc_vk_state_index(image, mip, layer)];
            VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkPipelineStageFlags src_stage = 0;
            VkAccessFlags src_access = 0;

            if (!lc_vk_state_map(old, &old_layout, &src_stage,
                                 &src_access)) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    aspect = lc_vk_aspect_for(image->format);
    res = lc_vulkan_upload_begin(image->device);
    if (res != LC_SUCCESS) {
        return res;
    }
    for (layer = base_layer; layer < base_layer + layer_count;
         layer++) {
        for (mip = base_mip; mip < base_mip + level_count; mip++) {
            lc_resource_state old =
                image->states[lc_vk_state_index(image, mip, layer)];
            VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkPipelineStageFlags src_stage = 0;
            VkAccessFlags src_access = 0;

            /* Validated above; cannot fail here. */
            lc_vk_state_map(old, &old_layout, &src_stage, &src_access);
            if (old_layout == new_layout) {
                continue;
            }
            lc_vk_sync_record(lc_vulkan_upload_cmd(image->device),
                              image->vk_image, aspect, mip, 1, layer, 1,
                              src_stage, src_access, dst_stage,
                              dst_access, old_layout, new_layout);
        }
    }
    res = lc_vulkan_upload_submit(image->device);
    if (res != LC_SUCCESS) {
        /* Unknown GPU state: UNDEFINED is always legal to leave. */
        lc_vk_sync_mark(image, base_mip, level_count, base_layer,
                        layer_count, LC_RESOURCE_STATE_UNDEFINED);
        return res;
    }
    lc_vk_sync_mark(image, base_mip, level_count, base_layer,
                    layer_count, new_state);
    return LC_SUCCESS;
}

/* Whole-image immediate transition. */
lc_result lc_vulkan_image_transition(lc_image *image,
                                     lc_resource_state new_state) {
    if (image == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_image_transition_range(
        image, 0, image->mip_levels, 0, image->array_layers, new_state);
}

/* Adopt a layout-less state already reached by an explicit render
 * pass's finalLayout (no barrier emitted). Range-exact. */
lc_result lc_vulkan_image_notify_range(lc_image *image, uint32_t base_mip,
                                       uint32_t level_count,
                                       uint32_t base_layer,
                                       uint32_t layer_count,
                                       lc_resource_state new_state) {
    if (image == NULL || image->states == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_validate_range(image, base_mip, level_count,
                                   base_layer, layer_count)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    lc_vk_sync_mark(image, base_mip, level_count, base_layer,
                    layer_count, new_state);
    return LC_SUCCESS;
}

/* Pure barrier emission for a span with a KNOWN old state (no
 * tracking touch; the caller marks). Used by the mip-generation
 * dance, which sequences several barriers per level. */
lc_result lc_vk_sync_record_span(VkCommandBuffer cmd, lc_image *image,
                                uint32_t base_mip, uint32_t level_count,
                                uint32_t base_layer, uint32_t layer_count,
                                lc_resource_state old_state,
                                lc_resource_state new_state) {
    VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageLayout new_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags src_stage = 0;
    VkAccessFlags src_access = 0;
    VkPipelineStageFlags dst_stage = 0;
    VkAccessFlags dst_access = 0;

    if (cmd == VK_NULL_HANDLE || image == NULL ||
        image->vk_image == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_validate_range(image, base_mip, level_count,
                                   base_layer, layer_count)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_state_map(old_state, &old_layout, &src_stage,
                         &src_access) ||
        !lc_vk_state_map(new_state, &new_layout, &dst_stage,
                         &dst_access)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (new_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    lc_vk_sync_record(cmd, image->vk_image,
                      lc_vk_aspect_for(image->format), base_mip,
                      level_count, base_layer, layer_count, src_stage,
                      src_access, dst_stage, dst_access, old_layout,
                      new_layout);
    return LC_SUCCESS;
}

/* Recorded transition for lc_encoder_transition_image: barrier into
 * the frame command buffer, tracking updated immediately
 * (recorded-not-executed discipline). */
lc_result lc_vulkan_encoder_transition_image(    VkCommandBuffer cmd, lc_image *image, uint32_t base_mip,
    uint32_t level_count, uint32_t base_layer, uint32_t layer_count,
    lc_resource_state new_state) {
    VkImageLayout new_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkPipelineStageFlags dst_stage = 0;
    VkAccessFlags dst_access = 0;
    VkImageAspectFlags aspect = 0;
    uint32_t layer;
    uint32_t mip;

    if (cmd == VK_NULL_HANDLE || image == NULL ||
        image->states == NULL || image->vk_image == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_validate_range(image, base_mip, level_count,
                                   base_layer, layer_count)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_state_valid_for_image(new_state) ||
        !lc_vk_state_map(new_state, &new_layout, &dst_stage,
                         &dst_access)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    aspect = lc_vk_aspect_for(image->format);
    /* Snapshot old states under the state shard (P1 audit fix): the
     * previous code read image->states[] unlocked, racing transfer
     * completion marks and worker reconcile snapshots, so the emitted
     * oldLayout could disagree with the marked new state. */
    {
        uint64_t count =
            (uint64_t)level_count * (uint64_t)layer_count;
        lc_resource_state *olds = NULL;

        if (count == 0 || count > 65536u) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        olds = (lc_resource_state *)malloc(sizeof(*olds) *
                                           (size_t)count);
        if (olds == NULL) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
        lc_device_lock_state(image->device);
        for (layer = base_layer; layer < base_layer + layer_count;
             layer++) {
            for (mip = base_mip; mip < base_mip + level_count; mip++) {
                olds[(uint64_t)(layer - base_layer) *
                         (uint64_t)level_count +
                     (uint64_t)(mip - base_mip)] =
                    image->states[lc_vk_state_index(image, mip,
                                                    layer)];
            }
        }
        lc_device_unlock_state(image->device);
        for (layer = base_layer; layer < base_layer + layer_count;
             layer++) {
            for (mip = base_mip; mip < base_mip + level_count; mip++) {
                lc_resource_state old =
                    olds[(uint64_t)(layer - base_layer) *
                             (uint64_t)level_count +
                         (uint64_t)(mip - base_mip)];
                VkImageLayout old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
                VkPipelineStageFlags src_stage = 0;
                VkAccessFlags src_access = 0;

                if (!lc_vk_state_map(old, &old_layout, &src_stage,
                                     &src_access)) {
                    free(olds);
                    return LC_ERROR_INVALID_ARGUMENT;
                }
                if (old_layout == new_layout) {
                    continue;
                }
                lc_vk_sync_record(cmd, image->vk_image, aspect, mip,
                                  1, layer, 1, src_stage, src_access,
                                  dst_stage, dst_access, old_layout,
                                  new_layout);
            }
        }
        free(olds);
    }
    lc_vk_sync_mark(image, base_mip, level_count, base_layer,
                    layer_count, new_state);
    return LC_SUCCESS;
}
