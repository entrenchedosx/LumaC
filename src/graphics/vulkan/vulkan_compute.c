/*
 * Vulkan isolated compute submit (Phase 21, PART Z).
 *
 * Production dispatch records into frame/worker command buffers
 * (graphics queue baseline). This module exists for one honest
 * purpose: proving a dedicated compute queue executes real work
 * with correct cross-queue visibility, without building the full
 * async-compute scheduler (explicitly deferred, PART AA27).
 *
 * Timeline mode is fire-and-forget (returns the signaled value for
 * GPU-side waits). Fallback mode has no device-wide completion
 * domain, so it waits its own fence and returns 0 (already
 * complete) — documented synchronous fallback, test path only.
 */

#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

static lc_result lc_vk_compute_pool_ensure(lc_device *device) {
    VkCommandPoolCreateInfo info;

    if (device->compute_pool != VK_NULL_HANDLE) {
        return LC_SUCCESS;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = device->compute_queue_family;
    if (vkCreateCommandPool(device->device, &info, NULL,
                            &device->compute_pool) != VK_SUCCESS) {
        device->compute_pool = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    return LC_SUCCESS;
}

void lc_vk_compute_pool_shutdown(lc_device *device) {
    uint32_t i;

    if (device == NULL) {
        return;
    }
    /* Drain isolated once-submits (bounded: the test path submits a
     * handful, and the GPU always progresses). */
    for (i = 0; i < device->compute_once_count; i++) {
        if (device->compute_once_fences[i] != VK_NULL_HANDLE &&
            device->device != VK_NULL_HANDLE) {
            vkWaitForFences(device->device, 1,
                            &device->compute_once_fences[i], VK_TRUE,
                            UINT64_MAX);
            vkDestroyFence(device->device,
                           device->compute_once_fences[i], NULL);
        }
        device->compute_once_fences[i] = VK_NULL_HANDLE;
        device->compute_once_cmds[i] = VK_NULL_HANDLE;
    }
    device->compute_once_count = 0;
    if (device->compute_pool != VK_NULL_HANDLE &&
        device->device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device->device, device->compute_pool,
                             NULL);
    }
    device->compute_pool = VK_NULL_HANDLE;
}

/* Reclaim completed once-submits (non-blocking fence polls).
 * Caller holds the submit lock. */
static void lc_vk_compute_once_reclaim(lc_device *device) {
    uint32_t i = 0;

    while (i < device->compute_once_count) {
        VkFence fence = device->compute_once_fences[i];

        if (fence != VK_NULL_HANDLE &&
            device->device != VK_NULL_HANDLE &&
            vkGetFenceStatus(device->device, fence) != VK_SUCCESS) {
            i++;
            continue;
        }
        if (fence != VK_NULL_HANDLE &&
            device->device != VK_NULL_HANDLE) {
            vkDestroyFence(device->device, fence, NULL);
        }
        if (device->compute_once_cmds[i] != VK_NULL_HANDLE &&
            device->compute_pool != VK_NULL_HANDLE &&
            device->device != VK_NULL_HANDLE) {
            vkFreeCommandBuffers(device->device, device->compute_pool,
                                 1, &device->compute_once_cmds[i]);
        }
        device->compute_once_cmds[i] =
            device->compute_once_cmds[device->compute_once_count - 1];
        device->compute_once_fences[i] = device->compute_once_fences
                                             [device->compute_once_count -
                                              1];
        device->compute_once_count--;
    }
}

lc_result lc_vk_compute_dispatch_once(
    lc_device *device, lc_compute_pipeline *pipeline,
    lc_binding_set *const *sets, const uint32_t *slots,
    uint32_t set_count, const void *push_data, uint32_t push_size,
    uint32_t x, uint32_t y, uint32_t z, uint64_t *out_value) {
    VkCommandBufferAllocateInfo alloc_info;
    VkCommandBufferBeginInfo begin_info;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkSubmitInfo submit_info;
    VkTimelineSemaphoreSubmitInfo tinfo;
    uint64_t value = 0;
    VkFence fence = VK_NULL_HANDLE;
    uint32_t i;
    lc_result res;

    if (out_value != NULL) {
        *out_value = 0;
    }
    if (device == NULL || pipeline == NULL ||
        (set_count > 0 && (sets == NULL || slots == NULL))) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_compute_pipeline_is_live(pipeline) ||
        pipeline->device != device ||
        pipeline->pipeline == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!device->compute_supported ||
        device->compute_queue == VK_NULL_HANDLE ||
        device->device == VK_NULL_HANDLE) {
        return LC_ERROR_UNSUPPORTED;
    }
    if (x == 0 || y == 0 || z == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (x > device->max_workgroup_count[0] ||
        y > device->max_workgroup_count[1] ||
        z > device->max_workgroup_count[2]) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < set_count; i++) {
        uint32_t n = 0;

        if (sets[i] == NULL || sets[i]->device != device ||
            sets[i]->vk_set == VK_NULL_HANDLE) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (slots[i] >= pipeline->layout_count) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        n = pipeline->slot_signature_counts[slots[i]];
        if (n != sets[i]->slot_count ||
            !lc_binding_signature_equal(
                pipeline->slot_signatures[slots[i]], n, sets[i]->slots,
                sets[i]->slot_count)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    if (push_data != NULL && push_size > 0) {
        int fits = 0;

        if (pipeline->push_ranges == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        for (i = 0; i < pipeline->push_range_count; i++) {
            uint32_t r_off = pipeline->push_ranges[i].offset;
            uint32_t r_size = pipeline->push_ranges[i].size;
            uint32_t r_vis = pipeline->push_ranges[i].visibility;

            if ((LC_SHADER_VISIBILITY_COMPUTE & ~r_vis) != 0) {
                continue;
            }
            if (r_off == 0 && push_size <= r_size) {
                fits = 1;
                break;
            }
        }
        if (!fits) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }

    lc_device_lock_submit(device);
    res = lc_vk_compute_pool_ensure(device);
    /* Reclaim prior once-submits whose fences flipped (keeps the
     * small pending ring bounded without ever blocking). */
    if (res == LC_SUCCESS) {
        lc_vk_compute_once_reclaim(device);
    }
    if (res == LC_SUCCESS) {
        memset(&alloc_info, 0, sizeof(alloc_info));
        alloc_info.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = device->compute_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device->device, &alloc_info,
                                     &cmd) != VK_SUCCESS) {
            res = LC_ERROR_OUT_OF_MEMORY;
        }
    }
    if (res == LC_SUCCESS) {
        memset(&begin_info, 0, sizeof(begin_info));
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
            res = LC_ERROR_UNKNOWN;
        }
    }
    if (res == LC_SUCCESS) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          pipeline->pipeline);
        for (i = 0; i < set_count; i++) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                    pipeline->layout, slots[i], 1,
                                    &sets[i]->vk_set, 0, NULL);
        }
        if (push_data != NULL && push_size > 0) {
            vkCmdPushConstants(cmd, pipeline->layout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, push_size,
                               push_data);
        }
        vkCmdDispatch(cmd, x, y, z);
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) {
            res = LC_ERROR_UNKNOWN;
        }
    }
    if (res == LC_SUCCESS) {
        if (device->timeline_ok) {
            value = lc_vk_signal_reserve(device);
        }
        /* A fence always rides the submit: fallback waits it
         * (synchronous), timeline parks it for the pending ring. */
        {
            VkFenceCreateInfo fence_info;

            memset(&fence_info, 0, sizeof(fence_info));
            fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if (vkCreateFence(device->device, &fence_info, NULL,
                              &fence) != VK_SUCCESS) {
                fence = VK_NULL_HANDLE;
                res = LC_ERROR_OUT_OF_MEMORY;
            }
        }
    }
    if (res == LC_SUCCESS) {
        uint64_t sig_val = value;

        memset(&submit_info, 0, sizeof(submit_info));
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &cmd;
        if (device->timeline_ok) {
            memset(&tinfo, 0, sizeof(tinfo));
            tinfo.sType =
                VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            tinfo.signalSemaphoreValueCount = 1;
            tinfo.pSignalSemaphoreValues = &sig_val;
            submit_info.pNext = &tinfo;
            submit_info.signalSemaphoreCount = 1;
            submit_info.pSignalSemaphores = &device->timeline;
        }
        if (vkQueueSubmit(device->compute_queue, 1, &submit_info,
                          fence) != VK_SUCCESS) {
            res = LC_ERROR_UNKNOWN;
        }
    }
    lc_device_unlock_submit(device);
    /* Command-buffer lifetime: the buffer must stay valid until its
     * submit completes. Fallback waits its own fence (documented
     * synchronous fallback). Timeline mode parks the buffer+fence
     * in the bounded pending ring (reclaimed non-blockingly on the
     * next call, drained at shutdown) and returns the signaled
     * value for GPU-side waits. */
    if (res == LC_SUCCESS && !device->timeline_ok &&
        fence != VK_NULL_HANDLE) {
        if (vkWaitForFences(device->device, 1, &fence, VK_TRUE,
                            UINT64_MAX) != VK_SUCCESS) {
            res = LC_ERROR_UNKNOWN;
        }
        vkDestroyFence(device->device, fence, NULL);
        fence = VK_NULL_HANDLE;
    }
    if (res == LC_SUCCESS && cmd != VK_NULL_HANDLE) {
        /* Timeline mode parks buffer+fence in the bounded pending
         * ring (reclaimed non-blockingly on the next call, drained
         * at shutdown). A full ring waits the oldest entry
         * (bounded: the GPU progresses) rather than leaking or
         * freeing live memory. Fallback already waited+freed
         * above, so cmd is NULL there. */
        lc_device_lock_submit(device);
        lc_vk_compute_once_reclaim(device);
        if (device->compute_once_count >= 8) {
            VkFence oldest = device->compute_once_fences[0];

            lc_device_unlock_submit(device);
            if (vkWaitForFences(device->device, 1, &oldest, VK_TRUE,
                                UINT64_MAX) != VK_SUCCESS) {
                res = LC_ERROR_UNKNOWN;
            }
            lc_device_lock_submit(device);
            lc_vk_compute_once_reclaim(device);
        }
        if (res == LC_SUCCESS && device->compute_once_count < 8) {
            uint32_t slot = device->compute_once_count++;

            device->compute_once_cmds[slot] = cmd;
            device->compute_once_fences[slot] = fence;
            cmd = VK_NULL_HANDLE;
            fence = VK_NULL_HANDLE;
        }
        lc_device_unlock_submit(device);
    }
    if (cmd != VK_NULL_HANDLE && device->compute_pool != VK_NULL_HANDLE &&
        device->device != VK_NULL_HANDLE) {
        lc_device_lock_submit(device);
        vkFreeCommandBuffers(device->device, device->compute_pool, 1,
                             &cmd);
        lc_device_unlock_submit(device);
    }
    if (res == LC_SUCCESS && out_value != NULL) {
        *out_value = value;
    }
    return res;
}
