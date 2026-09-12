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

/* Centralized memory-type search: required bits must all match;
 * preferred bits are tried first, then required alone. */
static int lc_vk_find_memory_type(VkPhysicalDevice physical,
                                  uint32_t type_bits,
                                  VkMemoryPropertyFlags required,
                                  VkMemoryPropertyFlags preferred,
                                  uint32_t *out_index) {
    VkPhysicalDeviceMemoryProperties props;
    uint32_t i;

    vkGetPhysicalDeviceMemoryProperties(physical, &props);
    for (i = 0; i < props.memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) == 0) {
            continue;
        }
        if ((props.memoryTypes[i].propertyFlags &
             (required | preferred)) == (required | preferred)) {
            *out_index = i;
            return 1;
        }
    }
    if (preferred != 0) {
        for (i = 0; i < props.memoryTypeCount; i++) {
            if ((type_bits & (1u << i)) == 0) {
                continue;
            }
            if ((props.memoryTypes[i].propertyFlags & required) == required) {
                *out_index = i;
                return 1;
            }
        }
    }
    return 0;
}

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
    return flags;
}

/* Memory policy per placement model. Coherent host memory is required
 * for CPU-visible allocations (no flush API in this phase); failure
 * surfaces as OUT_OF_MEMORY ("no suitable device memory"). */
static void lc_vk_memory_policy(lc_memory_usage memory,
                                VkMemoryPropertyFlags *required,
                                VkMemoryPropertyFlags *preferred) {
    switch (memory) {
    case LC_MEMORY_GPU_ONLY:
        *required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        *preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case LC_MEMORY_CPU_TO_GPU:
        *required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        *preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        break;
    case LC_MEMORY_GPU_TO_CPU:
    default:
        *required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        *preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
        break;
    }
}

/* Raw storage block: create, size, bind. Optionally maps persistently.
 * Fully unwinding; shared by tracked buffers and staging. */
static lc_result lc_vk_storage_create(
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
    VkMemoryPropertyFlags preferred = 0;
    void *mapped = NULL;
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
    /* GPU-only buffers always gain transfer-destination so
     * lc_buffer_write() staging works regardless of requested bits. */
    if (buffer->memory_usage == LC_MEMORY_GPU_ONLY) {
        usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    }
    lc_vk_memory_policy(buffer->memory_usage, &required, &preferred);

    if (buffer->memory_usage == LC_MEMORY_GPU_ONLY) {
        res = lc_vk_storage_create(device, buffer->size, usage, required,
                                   preferred, &buffer->vk_buffer,
                                   &buffer->vk_memory, NULL);
    } else {
        res = lc_vk_storage_create(device, buffer->size, usage, required,
                                   preferred, &buffer->vk_buffer,
                                   &buffer->vk_memory, &mapped);
    }
    if (res != LC_SUCCESS) {
        buffer->vk_buffer = VK_NULL_HANDLE;
        buffer->vk_memory = VK_NULL_HANDLE;
        buffer->mapped_ptr = NULL;
        return res;
    }
    buffer->mapped_ptr = mapped;
    return LC_SUCCESS;
}

void lc_vulkan_buffer_destroy(lc_buffer *buffer) {
    VkDevice device_handle = VK_NULL_HANDLE;

    if (buffer == NULL) {
        return;
    }
    if (buffer->device != NULL) {
        device_handle = buffer->device->device;
    }
    if (device_handle == VK_NULL_HANDLE) {
        buffer->vk_buffer = VK_NULL_HANDLE;
        buffer->vk_memory = VK_NULL_HANDLE;
        buffer->mapped_ptr = NULL;
        return;
    }
    /* Persistent mapping ends here (unmap exactly once); then buffer,
     * then memory. */
    if (buffer->mapped_ptr != NULL) {
        vkUnmapMemory(device_handle, buffer->vk_memory);
        buffer->mapped_ptr = NULL;
    }
    if (buffer->vk_buffer != VK_NULL_HANDLE) {
        vkDestroyBuffer(device_handle, buffer->vk_buffer, NULL);
        buffer->vk_buffer = VK_NULL_HANDLE;
    }
    if (buffer->vk_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_handle, buffer->vk_memory, NULL);
        buffer->vk_memory = VK_NULL_HANDLE;
    }
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

lc_result lc_vulkan_copy_buffer(lc_device *device, VkBuffer dst,
                                uint64_t dst_offset, VkBuffer src,
                                uint64_t src_offset, uint64_t size) {
    VkDevice dev_handle;
    VkCommandBufferBeginInfo begin_info;
    VkBufferCopy region;
    VkSubmitInfo submit_info;
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
    memset(&region, 0, sizeof(region));
    region.srcOffset = (VkDeviceSize)src_offset;
    region.dstOffset = (VkDeviceSize)dst_offset;
    region.size = (VkDeviceSize)size;
    vkCmdCopyBuffer(device->upload_cmd, src, dst, 1, &region);
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

lc_result lc_vulkan_buffer_write(lc_buffer *buffer, uint64_t offset,
                                 const void *data, uint64_t size) {
    lc_device *device;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void *staging_ptr = NULL;
    VkMemoryPropertyFlags required;
    VkMemoryPropertyFlags preferred;
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

    /* Coherent persistent mapping: plain memcpy, no flush needed. */
    if (buffer->mapped_ptr != NULL) {
        memcpy((char *)buffer->mapped_ptr + offset, data, (size_t)size);
        return LC_SUCCESS;
    }

    /* GPU-only path: temporary CPU-visible staging buffer, then an
     * immediate-submit copy. Staging is untracked host-side scratch:
     * created and destroyed inside this call. */
    lc_vk_memory_policy(LC_MEMORY_CPU_TO_GPU, &required, &preferred);
    res = lc_vk_storage_create(device, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               required, preferred, &staging, &staging_mem,
                               &staging_ptr);
    if (res != LC_SUCCESS) {
        return res;
    }
    memcpy(staging_ptr, data, (size_t)size);
    vkUnmapMemory(device->device, staging_mem);

    res = lc_vulkan_copy_buffer(device, buffer->vk_buffer, offset, staging,
                                0, size);
    vkDestroyBuffer(device->device, staging, NULL);
    vkFreeMemory(device->device, staging_mem, NULL);
    return res;
}
