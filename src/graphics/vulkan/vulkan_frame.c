/*
 * Vulkan frame backend (Phase 6: acquire, clear, submit, present).
 *
 * Swapchain-local frame lifecycle with LC_MAX_FRAMES_IN_FLIGHT slots.
 * Each slot owns its semaphores, fence, and reusable command buffer;
 * per-image fences track which submission still owns an image, and a
 * per-image flag records whether UNDEFINED may no longer be claimed.
 *
 * Clearing uses vkCmdClearColorImage with explicit layout transitions
 * (UNDEFINED-or-PRESENT_SRC -> TRANSFER_DST -> PRESENT_SRC), so no
 * render pass, pipeline, or shaders are needed. Queue families never
 * change under a swapchain, and concurrent sharing (when graphics and
 * present families differ) needs no ownership transfers.
 *
 * Out-of-date and suboptimal conditions map to recoverable results;
 * anything else fatal surfaces as a plain error. Submit failure is
 * documented as fatal for the swapchain (device-loss territory).
 */

#include <string.h>

#include "graphics/graphics_internal.h"

static float lc_vk_clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

static void lc_vk_image_barrier(VkCommandBuffer cmd, VkImage image,
                                VkAccessFlags src_access,
                                VkPipelineStageFlags src_stage,
                                VkAccessFlags dst_access,
                                VkPipelineStageFlags dst_stage,
                                VkImageLayout old_layout,
                                VkImageLayout new_layout) {
    VkImageMemoryBarrier barrier;

    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
}

/* Best-effort unwind of partially created frame objects. All handles
 * are NULL-checked; the struct is left empty. */
static void lc_vk_frame_unwind(lc_swapchain *swapchain, VkDevice device) {
    uint32_t i;

    for (i = 0; i < LC_MAX_FRAMES_IN_FLIGHT; i++) {
        lc_vk_flight *flight = &swapchain->flights[i];
        if (flight->image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, flight->image_available, NULL);
            flight->image_available = VK_NULL_HANDLE;
        }
        if (flight->fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, flight->fence, NULL);
            flight->fence = VK_NULL_HANDLE;
        }
        flight->cmd = VK_NULL_HANDLE;
    }
    if (swapchain->cmd_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, swapchain->cmd_pool, NULL);
        swapchain->cmd_pool = VK_NULL_HANDLE;
    }
}

lc_result lc_vulkan_frame_init(lc_swapchain *swapchain) {
    VkDevice device;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo alloc_info;
    VkCommandBuffer cmds[LC_MAX_FRAMES_IN_FLIGHT];
    uint32_t i;

    if (swapchain == NULL || swapchain->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = swapchain->device->device;
    if (device == VK_NULL_HANDLE) {
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }

    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    /* Per-buffer reset each frame; the pool itself lives as long as
     * the swapchain's frame state (across recreates). */
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = swapchain->device->graphics_queue_family;

    if (vkCreateCommandPool(device, &pool_info, NULL, &swapchain->cmd_pool) !=
        VK_SUCCESS) {
        swapchain->cmd_pool = VK_NULL_HANDLE;
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }

    for (i = 0; i < LC_MAX_FRAMES_IN_FLIGHT; i++) {
        lc_vk_flight *flight = &swapchain->flights[i];
        VkSemaphoreCreateInfo sem_info;
        VkFenceCreateInfo fence_info;

        memset(&sem_info, 0, sizeof(sem_info));
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        memset(&fence_info, 0, sizeof(fence_info));
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        /* Start signaled so the very first frame never deadlocks
         * waiting on a fence no submission has signaled yet. */
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        if (vkCreateSemaphore(device, &sem_info, NULL,
                              &flight->image_available) != VK_SUCCESS ||
            vkCreateFence(device, &fence_info, NULL, &flight->fence) !=
                VK_SUCCESS) {
            lc_vk_frame_unwind(swapchain, device);
            return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
        }
    }

    /* One primary buffer per slot, allocated once and reused. */
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = swapchain->cmd_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = LC_MAX_FRAMES_IN_FLIGHT;
    if (vkAllocateCommandBuffers(device, &alloc_info, cmds) != VK_SUCCESS) {
        lc_vk_frame_unwind(swapchain, device);
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }
    for (i = 0; i < LC_MAX_FRAMES_IN_FLIGHT; i++) {
        swapchain->flights[i].cmd = cmds[i];
    }
    return LC_SUCCESS;
}

void lc_vulkan_frame_teardown(lc_swapchain *swapchain) {
    VkDevice device = VK_NULL_HANDLE;
    uint32_t i;

    if (swapchain == NULL) {
        return;
    }
    /* Requires no in-flight submissions: callers run the image teardown
     * (which waits idle) first. Pool destroy implicitly frees the
     * command buffers, including any begun-but-unsubmitted recording. */
    if (swapchain->device != NULL) {
        device = swapchain->device->device;
    }
    for (i = 0; i < LC_MAX_FRAMES_IN_FLIGHT; i++) {
        lc_vk_flight *flight = &swapchain->flights[i];
        if (device != VK_NULL_HANDLE) {
            if (flight->image_available != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, flight->image_available, NULL);
            }
            if (flight->fence != VK_NULL_HANDLE) {
                vkDestroyFence(device, flight->fence, NULL);
            }
        }
        flight->image_available = VK_NULL_HANDLE;
        flight->fence = VK_NULL_HANDLE;
        flight->cmd = VK_NULL_HANDLE;
    }
    if (swapchain->cmd_pool != VK_NULL_HANDLE) {
        if (device != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, swapchain->cmd_pool, NULL);
        }
        swapchain->cmd_pool = VK_NULL_HANDLE;
    }
    swapchain->current_frame = 0;
    swapchain->current_image = UINT32_MAX;
    swapchain->frame_active = 0;
    swapchain->frame_suboptimal = 0;
}

/* Backend handles must exist; the public layer already proved the
 * swapchain pointer live. Returns 1 when usable, 0 otherwise. */
static int lc_vk_frame_ready(const lc_swapchain *swapchain) {
    return swapchain != NULL && swapchain->device != NULL &&
           swapchain->surface != NULL &&
           swapchain->device->device != VK_NULL_HANDLE &&
           swapchain->vk_swapchain != VK_NULL_HANDLE &&
           swapchain->cmd_pool != VK_NULL_HANDLE &&
           swapchain->current_frame < LC_MAX_FRAMES_IN_FLIGHT;
}

lc_result lc_vulkan_frame_begin(lc_swapchain *swapchain) {
    lc_device *device;
    lc_vk_flight *flight;
    VkResult acquire_res;
    uint32_t image_index = UINT32_MAX;
    VkFence owner;
    VkCommandBufferBeginInfo begin_info;

    if (swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_frame_ready(swapchain)) {
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }
    device = swapchain->device;
    flight = &swapchain->flights[swapchain->current_frame];

    /* A free slot: the previous use of this slot has completed. */
    if (vkWaitForFences(device->device, 1, &flight->fence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }

    acquire_res = vkAcquireNextImageKHR(device->device,
                                        swapchain->vk_swapchain, UINT64_MAX,
                                        flight->image_available,
                                        VK_NULL_HANDLE, &image_index);
    if (acquire_res == VK_ERROR_OUT_OF_DATE_KHR) {
        /* Nothing started: fence untouched (still signaled), no frame
         * open. Caller recreates and retries. */
        return LC_ERROR_SWAPCHAIN_OUT_OF_DATE;
    }
    if (acquire_res == VK_SUBOPTIMAL_KHR) {
        swapchain->frame_suboptimal = 1;
    } else if (acquire_res == VK_ERROR_SURFACE_LOST_KHR) {
        return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
    } else if (acquire_res != VK_SUCCESS) {
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }
    if (image_index >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }

    /* Frames in flight can exceed image count: an image may still be
     * owned by an earlier submission. Wait it out before reuse. */
    owner = swapchain->images_in_flight[image_index];
    if (owner != VK_NULL_HANDLE && owner != flight->fence) {
        if (vkWaitForFences(device->device, 1, &owner, VK_TRUE,
                            UINT64_MAX) != VK_SUCCESS) {
            return LC_ERROR_UNKNOWN;
        }
    }
    swapchain->images_in_flight[image_index] = flight->fence;

    if (vkResetFences(device->device, 1, &flight->fence) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    if (vkResetCommandBuffer(flight->cmd, 0) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(flight->cmd, &begin_info) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }

    /* Transition for clearing. First use discards undefined contents;
     * later frames come back from presentation. The acquire semaphore
     * already guarantees prior presentation finished, so no source
     * access needs waiting on. */
    if (swapchain->image_initialized[image_index] != 0) {
        lc_vk_image_barrier(flight->cmd, swapchain->images[image_index], 0,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    } else {
        lc_vk_image_barrier(flight->cmd, swapchain->images[image_index], 0,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                            VK_ACCESS_TRANSFER_WRITE_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_IMAGE_LAYOUT_UNDEFINED,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }

    swapchain->current_image = image_index;
    swapchain->frame_active = 1;
    return LC_SUCCESS;
}

lc_result lc_vulkan_frame_clear(lc_swapchain *swapchain, float r, float g,
                                float b, float a) {
    VkClearColorValue color;
    VkImageSubresourceRange range;
    lc_vk_flight *flight;

    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_frame_ready(swapchain) ||
        swapchain->current_image >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }
    flight = &swapchain->flights[swapchain->current_frame];

    color.float32[0] = lc_vk_clamp01(r);
    color.float32[1] = lc_vk_clamp01(g);
    color.float32[2] = lc_vk_clamp01(b);
    color.float32[3] = lc_vk_clamp01(a);

    memset(&range, 0, sizeof(range));
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.baseMipLevel = 0;
    range.levelCount = 1;
    range.baseArrayLayer = 0;
    range.layerCount = 1;

    vkCmdClearColorImage(flight->cmd, swapchain->images[swapchain->current_image],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
                         &range);
    return LC_SUCCESS;
}

lc_result lc_vulkan_frame_end(lc_swapchain *swapchain) {
    lc_device *device;
    lc_surface *surface;
    lc_vk_flight *flight;
    VkImage image;
    VkSubmitInfo submit_info;
    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkPresentInfoKHR present_info;
    VkResult present_res;
    VkSemaphore present_sem = VK_NULL_HANDLE;
    uint32_t presented_image = UINT32_MAX;
    int reported_suboptimal;

    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_frame_ready(swapchain) ||
        swapchain->current_image >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }
    device = swapchain->device;
    surface = swapchain->surface;
    flight = &swapchain->flights[swapchain->current_frame];
    image = swapchain->images[swapchain->current_image];
    /* Per-image present semaphore: the submit signals it and the
     * presentation engine consumes it asynchronously, so a per-slot
     * semaphore could be re-signaled for a new image while an older
     * present still holds it. */
    if (swapchain->present_semaphores == NULL) {
        return LC_ERROR_UNKNOWN;
    }
    present_sem = swapchain->present_semaphores[swapchain->current_image];
    if (present_sem == VK_NULL_HANDLE) {
        return LC_ERROR_UNKNOWN;
    }

    /* Back to a presentable layout; the transfer write must complete
     * before the presentation engine may read. */
    lc_vk_image_barrier(flight->cmd, image,
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                        VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

    if (vkEndCommandBuffer(flight->cmd) != VK_SUCCESS) {
        /* Recording failed: drop the frame without submitting. The
         * buffer is reset clean on the next begin. */
        swapchain->frame_active = 0;
        return LC_ERROR_UNKNOWN;
    }

    memset(&submit_info, 0, sizeof(submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &flight->image_available;
    /* The clear is a transfer operation: wait there, not earlier. */
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &flight->cmd;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &present_sem;

    if (vkQueueSubmit(device->graphics_queue, 1, &submit_info,
                      flight->fence) != VK_SUCCESS) {
        /* Fatal for this swapchain (device-loss territory): the fence
         * will never signal, so the frame is dropped and the caller
         * must destroy/recreate rather than continue. */
        swapchain->frame_active = 0;
        return LC_ERROR_UNKNOWN;
    }

    memset(&present_info, 0, sizeof(present_info));
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &present_sem;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain->vk_swapchain;
    present_info.pImageIndices = &swapchain->current_image;

    /* Present on the present queue, which may differ from graphics. */
    present_res =
        vkQueuePresentKHR(surface->present_queue, &present_info);

    /* The submission owns a fence now; the slot rotates regardless of
     * how presentation went. */
    presented_image = swapchain->current_image;
    swapchain->current_frame =
        (swapchain->current_frame + 1u) % LC_MAX_FRAMES_IN_FLIGHT;
    swapchain->current_image = UINT32_MAX;
    swapchain->frame_active = 0;
    reported_suboptimal = swapchain->frame_suboptimal;
    swapchain->frame_suboptimal = 0;

    if (present_res == VK_SUCCESS || present_res == VK_SUBOPTIMAL_KHR) {
        /* The image completed a full cycle and sits in PRESENT_SRC. */
        swapchain->image_initialized[presented_image] = 1;
    }
    if (present_res == VK_SUCCESS) {
        return reported_suboptimal ? LC_SUBOPTIMAL : LC_SUCCESS;
    }
    if (present_res == VK_SUBOPTIMAL_KHR) {
        return LC_SUBOPTIMAL;
    }
    if (present_res == VK_ERROR_OUT_OF_DATE_KHR) {
        return LC_ERROR_SWAPCHAIN_OUT_OF_DATE;
    }
    if (present_res == VK_ERROR_SURFACE_LOST_KHR) {
        return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
    }
    return LC_ERROR_UNKNOWN;
}
