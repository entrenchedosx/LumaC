/*
 * Vulkan buffer backend (Phase 8: generic GPU buffers).
 *
 * Small explicit allocation path (no VMA yet by design): create buffer,
 * query requirements, pick a memory type centrally, allocate, bind.
 * CPU-visible memory is persistently mapped at creation and is always
 * required coherent, so mapped writes never need flushing. GPU-only
 * buffers are written through a temporary staging buffer plus the
 * device-level immediate-submit upload context below (graphics queue:
 * universally supported; a dedicated transfer queue is future work).
 */

#include <string.h>

#include "graphics/graphics_internal.h"

/* Centralized memory-type search lives in vulkan_memory.c
 * (lc_vk_find_memory_type, shared with staging scratch). */

static VkBufferUsageFlags lc_vk_translate_usage(uint32_t usage) {
    VkBufferUsageFlags flags = 0;

    if ((usage & LC_BUFFER_USAGE_VERTEX) != 0) {
        flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    }
    if ((usage & LC_BUFFER_USAGE_INDEX) != 0) {
        flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    }
    if ((usage & LC_BUFFER_USAGE_UNIFORM) != 0) {
        flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    }
    if ((usage & LC_BUFFER_USAGE_STORAGE) != 0) {
        flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    }
    if ((usage & LC_BUFFER_USAGE_TRANSFER_SRC) != 0) {
        flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    if ((usage & LC_BUFFER_USAGE_TRANSFER_DST) != 0) {
        flags |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    if ((usage & LC_BUFFER_USAGE_INDIRECT) != 0) {
        flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    }
    return flags;
}

/* Memory policy per placement model (Phase 19: coherent is
 * preferred, never required). Non-coherent host types work through
 * flush/invalidate; failure surfaces as OUT_OF_MEMORY ("no suitable
 * device memory"). */
static void lc_vk_memory_policy(lc_memory_usage memory,
                                lc_vk_mem_class *cls,
                                VkMemoryPropertyFlags *required) {
    switch (memory) {
    case LC_MEMORY_GPU_ONLY:
        *cls = LC_VK_MEM_DEVICE_BUFFERS;
        *required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case LC_MEMORY_CPU_TO_GPU:
        *cls = LC_VK_MEM_UPLOAD;
        *required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        break;
    case LC_MEMORY_GPU_TO_CPU:
    default:
        *cls = LC_VK_MEM_READBACK;
        *required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
        break;
    }
}

/* Raw storage block: create, size, bind. Optionally maps persistently.
 * Fully unwinding; shared by tracked buffers, staging scratch, and
 * image staging. */
lc_result lc_vulkan_storage_create(
    lc_device *device, uint64_t size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
    VkBuffer *out_buffer, VkDeviceMemory *out_memory, void **out_mapped) {
    VkBufferCreateInfo buffer_info;
    VkMemoryRequirements reqs;
    uint32_t mem_index = 0;
    VkMemoryAllocateInfo alloc_info;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void *mapped = NULL;

    memset(&buffer_info, 0, sizeof(buffer_info));
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = (VkDeviceSize)size;
    buffer_info.usage = usage;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vkCreateBuffer(device->device, &buffer_info, NULL, &buffer) !=
        VK_SUCCESS) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    vkGetBufferMemoryRequirements(device->device, buffer, &reqs);
    if (!lc_vk_find_memory_type(device->physical_device,
                                reqs.memoryTypeBits, required, preferred,
                                &mem_index)) {
        vkDestroyBuffer(device->device, buffer, NULL);
        return LC_ERROR_OUT_OF_MEMORY;
    }

    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = reqs.size;
    alloc_info.memoryTypeIndex = mem_index;

    if (vkAllocateMemory(device->device, &alloc_info, NULL, &memory) !=
        VK_SUCCESS) {
        vkDestroyBuffer(device->device, buffer, NULL);
        return LC_ERROR_OUT_OF_MEMORY;
    }
    if (vkBindBufferMemory(device->device, buffer, memory, 0) !=
        VK_SUCCESS) {
        vkFreeMemory(device->device, memory, NULL);
        vkDestroyBuffer(device->device, buffer, NULL);
        return LC_ERROR_UNKNOWN;
    }
    if (out_mapped != NULL) {
        if (vkMapMemory(device->device, memory, 0, reqs.size, 0, &mapped) !=
            VK_SUCCESS) {
            vkFreeMemory(device->device, memory, NULL);
            vkDestroyBuffer(device->device, buffer, NULL);
            return LC_ERROR_OUT_OF_MEMORY;
        }
    }

    *out_buffer = buffer;
    *out_memory = memory;
    if (out_mapped != NULL) {
        *out_mapped = mapped;
    }
    return LC_SUCCESS;
}

lc_result lc_vulkan_buffer_create(lc_buffer *buffer) {
    lc_device *device;
    VkBufferUsageFlags usage;
    VkMemoryPropertyFlags required = 0;
    lc_vk_mem_class cls = LC_VK_MEM_DEVICE_BUFFERS;
    VkMemoryRequirements reqs;
    VkBufferCreateInfo buffer_info;
    lc_vk_mem_binding binding;
    lc_result res;

    if (buffer == NULL || buffer->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = buffer->device;
    if (device->device == VK_NULL_HANDLE ||
        device->physical_device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }

    usage = lc_vk_translate_usage(buffer->usage);
    /* GPU-only buffers always gain transfer source+sink so staging
     * copies work regardless of requested bits (writes sink,
     * test/debug downloads source). */
    if (buffer->memory_usage == LC_MEMORY_GPU_ONLY) {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        usage |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    }
    lc_vk_memory_policy(buffer->memory_usage, &cls, &required);

    memset(&buffer_info, 0, sizeof(buffer_info));
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = (VkDeviceSize)buffer->size;
    buffer_info.usage = usage;
    if (device->has_dedicated_transfer) {
        const uint32_t families[2] = {device->graphics_queue_family,
                                      device->transfer_queue_family};

        buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        buffer_info.queueFamilyIndexCount = 2;
        buffer_info.pQueueFamilyIndices = families;
        if (vkCreateBuffer(device->device, &buffer_info, NULL,
                           &buffer->vk_buffer) != VK_SUCCESS) {
            buffer->vk_buffer = VK_NULL_HANDLE;
            return LC_ERROR_OUT_OF_MEMORY;
        }
    } else if (vkCreateBuffer(device->device, &buffer_info, NULL,
                              &buffer->vk_buffer) != VK_SUCCESS) {
        buffer->vk_buffer = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    vkGetBufferMemoryRequirements(device->device, buffer->vk_buffer,
                                  &reqs);
    memset(&binding, 0, sizeof(binding));
    res = lc_vk_mem_alloc(device, cls, reqs.memoryTypeBits, required,
                          reqs.size, reqs.alignment, &binding);
    if (res != LC_SUCCESS) {
        vkDestroyBuffer(device->device, buffer->vk_buffer, NULL);
        buffer->vk_buffer = VK_NULL_HANDLE;
        return res;
    }
    if (vkBindBufferMemory(device->device, buffer->vk_buffer,
                           binding.memory,
                           (VkDeviceSize)binding.offset) != VK_SUCCESS) {
        lc_vk_mem_free(device, &binding);
        vkDestroyBuffer(device->device, buffer->vk_buffer, NULL);
        buffer->vk_buffer = VK_NULL_HANDLE;
        return LC_ERROR_UNKNOWN;
    }
    buffer->vk_memory = binding.memory;
    buffer->vk_memory_offset = binding.offset;
    buffer->mapped_ptr = binding.mapped;
    buffer->memory_coherent = binding.coherent;
    buffer->memory_dedicated = binding.dedicated;
    buffer->memory_block = binding.block;    buffer->memory_class =
        (cls == LC_VK_MEM_DEVICE_BUFFERS)
            ? LC_MEMORY_CLASS_DEVICE_LOCAL
            : ((cls == LC_VK_MEM_UPLOAD) ? LC_MEMORY_CLASS_UPLOAD
                                         : LC_MEMORY_CLASS_READBACK);
    buffer->allocation_size = binding.size;
    buffer->owner_family = device->has_dedicated_transfer
                               ? VK_QUEUE_FAMILY_IGNORED
                               : device->graphics_queue_family;
    return LC_SUCCESS;
}

void lc_vulkan_buffer_destroy(lc_buffer *buffer) {
    lc_vk_mem_binding binding;

    if (buffer == NULL) {
        return;
    }
    if (buffer->device != NULL &&
        buffer->device->device != VK_NULL_HANDLE) {
        if (buffer->vk_buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(buffer->device->device, buffer->vk_buffer,
                            NULL);
        }
        /* Persistent block mappings stay mapped (shared by other
         * suballocations); only dedicated memory unmaps, inside
         * free — so the mapped pointer travels with the binding. */
        memset(&binding, 0, sizeof(binding));
        binding.memory = buffer->vk_memory;
        binding.offset = buffer->vk_memory_offset;
        binding.size = buffer->allocation_size;
        binding.mapped = buffer->mapped_ptr;
        binding.coherent = buffer->memory_coherent;
        binding.dedicated = buffer->memory_dedicated;
        binding.block = buffer->memory_block;
        lc_vk_mem_free(buffer->device, &binding);
    }
    /* Dead device (teardown): blocks already freed by the allocator
     * teardown; just detach. */
    buffer->vk_buffer = VK_NULL_HANDLE;
    buffer->vk_memory = VK_NULL_HANDLE;
    buffer->vk_memory_offset = 0;
    buffer->mapped_ptr = NULL;
    buffer->memory_block = NULL;
}

lc_result lc_vulkan_upload_ensure(lc_device *device) {
    VkDevice dev_handle;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo alloc_info;
    VkFenceCreateInfo fence_info;

    if (device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->upload_pool != VK_NULL_HANDLE) {
        return LC_SUCCESS;
    }
    dev_handle = device->device;
    if (dev_handle == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }

    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device->graphics_queue_family;
    if (vkCreateCommandPool(dev_handle, &pool_info, NULL,
                            &device->upload_pool) != VK_SUCCESS) {
        device->upload_pool = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }

    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = device->upload_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(dev_handle, &alloc_info,
                                 &device->upload_cmd) != VK_SUCCESS) {
        device->upload_cmd = VK_NULL_HANDLE;
        vkDestroyCommandPool(dev_handle, device->upload_pool, NULL);
        device->upload_pool = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }

    memset(&fence_info, 0, sizeof(fence_info));
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateFence(dev_handle, &fence_info, NULL, &device->upload_fence) !=
        VK_SUCCESS) {
        device->upload_fence = VK_NULL_HANDLE;
        vkDestroyCommandPool(dev_handle, device->upload_pool, NULL);
        device->upload_pool = VK_NULL_HANDLE;
        device->upload_cmd = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    return LC_SUCCESS;
}

void lc_vulkan_upload_teardown(lc_device *device) {
    VkDevice dev_handle = VK_NULL_HANDLE;

    if (device == NULL) {
        return;
    }
    dev_handle = device->device;
    /* Pool destroy implicitly frees the upload command buffer. */
    if (device->upload_fence != VK_NULL_HANDLE) {
        if (dev_handle != VK_NULL_HANDLE) {
            vkDestroyFence(dev_handle, device->upload_fence, NULL);
        }
        device->upload_fence = VK_NULL_HANDLE;
    }
    if (device->upload_pool != VK_NULL_HANDLE) {
        if (dev_handle != VK_NULL_HANDLE) {
            vkDestroyCommandPool(dev_handle, device->upload_pool, NULL);
        }
        device->upload_pool = VK_NULL_HANDLE;
        device->upload_cmd = VK_NULL_HANDLE;
    }
}

/* Immediate-submit upload session: ensure the context, drain any
 * prior use, and leave the command buffer recording. Callers record
 * arbitrary upload work (copies, blits, barriers), then finish with
 * lc_vulkan_upload_submit(). Private; later replaceable by async
 * upload queues without touching callers. */
lc_result lc_vulkan_upload_begin(lc_device *device) {
    VkDevice dev_handle;
    VkCommandBufferBeginInfo begin_info;
    lc_result res;

    if (device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vulkan_upload_ensure(device);
    if (res != LC_SUCCESS) {
        return res;
    }
    dev_handle = device->device;

    if (vkWaitForFences(dev_handle, 1, &device->upload_fence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    if (vkResetFences(dev_handle, 1, &device->upload_fence) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    if (vkResetCommandBuffer(device->upload_cmd, 0) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }

    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(device->upload_cmd, &begin_info) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    return LC_SUCCESS;
}

VkCommandBuffer lc_vulkan_upload_cmd(const lc_device *device) {
    if (device == NULL) {
        return VK_NULL_HANDLE;
    }
    return device->upload_cmd;
}

lc_result lc_vulkan_upload_submit(lc_device *device) {
    VkDevice dev_handle;
    VkSubmitInfo submit_info;

    if (device == NULL || device->device == VK_NULL_HANDLE ||
        device->upload_cmd == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    dev_handle = device->device;

    if (vkEndCommandBuffer(device->upload_cmd) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    memset(&submit_info, 0, sizeof(submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &device->upload_cmd;
    if (vkQueueSubmit(device->graphics_queue, 1, &submit_info,
                      device->upload_fence) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    if (vkWaitForFences(dev_handle, 1, &device->upload_fence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    return LC_SUCCESS;
}

lc_result lc_vulkan_copy_buffer(lc_device *device, VkBuffer dst,
                                uint64_t dst_offset, VkBuffer src,
                                uint64_t src_offset, uint64_t size) {
    VkBufferCopy region;
    lc_result res;

    if (device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vulkan_upload_begin(device);
    if (res != LC_SUCCESS) {
        return res;
    }
    memset(&region, 0, sizeof(region));
    region.srcOffset = (VkDeviceSize)src_offset;
    region.dstOffset = (VkDeviceSize)dst_offset;
    region.size = (VkDeviceSize)size;
    vkCmdCopyBuffer(lc_vulkan_upload_cmd(device), src, dst, 1, &region);
    return lc_vulkan_upload_submit(device);
}

lc_result lc_vulkan_buffer_write(lc_buffer *buffer, uint64_t offset,
                                 const void *data, uint64_t size) {
    lc_device *device;
    VkBuffer staging = VK_NULL_HANDLE;
    lc_vk_mem_binding stage;
    void *staging_ptr = NULL;
    lc_result res;

    if (buffer == NULL || buffer->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0) {
        return LC_SUCCESS;
    }
    device = buffer->device;
    if (device->device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }

    /* Persistently mapped: memcpy + flush for non-coherent. */
    if (buffer->mapped_ptr != NULL) {
        lc_vk_mem_binding self;

        memcpy((char *)buffer->mapped_ptr + offset, data,
               (size_t)size);
        memset(&self, 0, sizeof(self));
        self.memory = buffer->vk_memory;
        self.offset = buffer->vk_memory_offset;
        self.size = buffer->allocation_size;
        self.mapped = buffer->mapped_ptr;
        self.coherent = buffer->memory_coherent;
        return lc_vk_mem_flush(device, &self, offset, size);
    }

    /* GPU-only path: pool staging buffer (no dedicated VkDeviceMemory
     * churn per write, PART Z), then an immediate-submit copy. */
    memset(&stage, 0, sizeof(stage));
    res = lc_vk_stage_acquire(device, size, 1,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging,
                              &stage, &staging_ptr);
    if (res != LC_SUCCESS) {
        return res;
    }
    memcpy(staging_ptr, data, (size_t)size);
    res = lc_vk_mem_flush(device, &stage, 0, size);
    if (res != LC_SUCCESS) {
        lc_vk_stage_release(device, staging, &stage);
        return res;
    }
    res = lc_vulkan_copy_buffer(device, buffer->vk_buffer, offset,
                                staging, 0, size);
    lc_vk_stage_release(device, staging, &stage);
    if (res == LC_SUCCESS) {
        /* Phase 21: the staging copy wrote the buffer (transfer
         * shard owns buffer_state; the sync path is externally
         * serialized like the async scheduler). */
        lc_device_lock_transfer(device);
        lc_vk_sync_mark_buffer(buffer, LC_RESOURCE_STATE_TRANSFER_DST);
        lc_device_unlock_transfer(device);
    }
    return res;
}

/* Full-range invalidate for map (non-coherent correctness). */

/* Synchronous buffer download (Phase 21, test/debug path): drain
 * prior device work first, then copy through a host-visible
 * staging buffer. Mirrors the write path; never used in steady
 * production frames. */
lc_result lc_vulkan_buffer_read(lc_buffer *buffer, uint64_t offset,
                                void *dst, uint64_t size) {
    lc_device *device;
    VkBuffer staging = VK_NULL_HANDLE;
    lc_vk_mem_binding stage;
    void *staging_ptr = NULL;
    lc_result res;

    if (buffer == NULL || buffer->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0) {
        return LC_SUCCESS;
    }
    if (dst == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = buffer->device;
    if (device->device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }

    /* Persistently mapped: invalidate for non-coherent, then
     * memcpy (never a GPU copy). */
    if (buffer->mapped_ptr != NULL) {
        lc_vk_mem_binding self;

        memset(&self, 0, sizeof(self));
        self.memory = buffer->vk_memory;
        self.offset = buffer->vk_memory_offset;
        self.size = buffer->allocation_size;
        self.mapped = buffer->mapped_ptr;
        self.coherent = buffer->memory_coherent;
        res = lc_vk_mem_invalidate(device, &self, offset, size);
        if (res != LC_SUCCESS) {
            return res;
        }
        memcpy(dst, (const char *)buffer->mapped_ptr + offset,
               (size_t)size);
        return LC_SUCCESS;
    }

    /* GPU-only path: drain, immediate-submit copy into staging,
     * invalidate, memcpy out. */
    if (device->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->device);
    }
    memset(&stage, 0, sizeof(stage));
    res = lc_vk_stage_acquire(device, size, 0,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT, &staging,
                              &stage, &staging_ptr);
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vulkan_copy_buffer(device, staging, 0, buffer->vk_buffer,
                                offset, size);
    if (res != LC_SUCCESS) {
        lc_vk_stage_release(device, staging, &stage);
        return res;
    }
    res = lc_vk_mem_invalidate(device, &stage, 0, size);
    if (res != LC_SUCCESS) {
        lc_vk_stage_release(device, staging, &stage);
        return res;
    }
    memcpy(dst, staging_ptr, (size_t)size);
    lc_vk_stage_release(device, staging, &stage);
    return LC_SUCCESS;
}lc_result lc_vulkan_buffer_invalidate(lc_buffer *buffer) {
    lc_vk_mem_binding self;

    if (buffer == NULL || buffer->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (buffer->mapped_ptr == NULL) {
        return LC_SUCCESS;
    }
    memset(&self, 0, sizeof(self));
    self.memory = buffer->vk_memory;
    self.offset = buffer->vk_memory_offset;
    self.size = buffer->allocation_size;
    self.mapped = buffer->mapped_ptr;
    self.coherent = buffer->memory_coherent;
    return lc_vk_mem_invalidate(buffer->device, &self, 0,
                                buffer->allocation_size);
}
