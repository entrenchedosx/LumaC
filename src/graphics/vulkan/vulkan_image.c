/*
 * Vulkan image backend (Phase 9: texture/image resource foundation).
 *
 * Device-local optimal-tiling images with a default full-resource
 * view, whole-image layout tracking, staging uploads, and GPU mipmap
 * generation. All transfer work reuses the device immediate-submit
 * upload context; no second submit subsystem exists. Per-subresource
 * state tracking is documented future work (see struct comment).
 */

#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

/* All currently defined lc_image_usage bits. */
#define LC_IMAGE_USAGE_KNOWN_MASK 0x3Fu

/* Full mip-chain length for an extent: floor(log2(max))) + 1. */
static uint32_t lc_vk_full_mip_levels(uint32_t w, uint32_t h, uint32_t d) {
    uint32_t m = w;
    uint32_t levels = 0;

    if (h > m) {
        m = h;
    }
    if (d > m) {
        m = d;
    }
    while (m > 0) {
        levels++;
        m >>= 1;
    }
    return levels;
}

static VkImageType lc_vk_image_type(lc_image_type type) {
    switch (type) {
    case LC_IMAGE_TYPE_1D:
        return VK_IMAGE_TYPE_1D;
    case LC_IMAGE_TYPE_3D:
        return VK_IMAGE_TYPE_3D;
    case LC_IMAGE_TYPE_2D:
    default:
        return VK_IMAGE_TYPE_2D;
    }
}

static VkImageUsageFlags lc_vk_translate_image_usage(uint32_t usage) {
    VkImageUsageFlags flags = 0;

    if ((usage & LC_IMAGE_USAGE_SAMPLED) != 0) {
        flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    if ((usage & LC_IMAGE_USAGE_STORAGE) != 0) {
        flags |= VK_IMAGE_USAGE_STORAGE_BIT;
    }
    if ((usage & LC_IMAGE_USAGE_COLOR_ATTACHMENT) != 0) {
        flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    }
    if ((usage & LC_IMAGE_USAGE_DEPTH_STENCIL) != 0) {
        flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    }
    if ((usage & LC_IMAGE_USAGE_TRANSFER_SRC) != 0) {
        flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    if ((usage & LC_IMAGE_USAGE_TRANSFER_DST) != 0) {
        flags |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }
    return flags;
}

static VkImageAspectFlags lc_vk_aspect_for(lc_format format) {
    VkImageAspectFlags aspect = 0;

    if (lc_format_is_color(format)) {
        aspect |= VK_IMAGE_ASPECT_COLOR_BIT;
    }
    if (lc_format_is_depth(format)) {
        aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (lc_format_is_stencil(format)) {
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    return aspect;
}

static VkSampleCountFlagBits lc_vk_sample_count(uint32_t samples) {
    switch (samples) {
    case 2:
        return VK_SAMPLE_COUNT_2_BIT;
    case 4:
        return VK_SAMPLE_COUNT_4_BIT;
    case 8:
        return VK_SAMPLE_COUNT_8_BIT;
    case 1:
    default:
        return VK_SAMPLE_COUNT_1_BIT;
    }
}

/* Deep descriptor validation. Public layer proved liveness; everything
 * structural lands here so both create paths share one checker. */
static lc_result lc_vk_validate_image_desc(const lc_image_desc *desc,
                                           uint32_t *out_mips) {
    uint32_t full_chain = 0;

    if (desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->type != LC_IMAGE_TYPE_1D &&
        desc->type != LC_IMAGE_TYPE_2D &&
        desc->type != LC_IMAGE_TYPE_3D) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->format == LC_FORMAT_UNDEFINED ||
        lc_vulkan_translate_format(desc->format) == VK_FORMAT_UNDEFINED) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->type == LC_IMAGE_TYPE_1D) {
        if (desc->height != 1 || desc->depth != 1) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    } else if (desc->type == LC_IMAGE_TYPE_2D) {
        if (desc->height == 0 || desc->depth != 1) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    } else {
        if (desc->height == 0 || desc->depth == 0) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    if (desc->array_layers == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->type == LC_IMAGE_TYPE_3D && desc->array_layers != 1) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->usage == 0 || (desc->usage & ~LC_IMAGE_USAGE_KNOWN_MASK) != 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Nonsense usage/format pairs fail fast instead of producing an
     * image no pipeline could ever bind. */
    if (lc_format_is_depth(desc->format) || lc_format_is_stencil(desc->format)) {
        if ((desc->usage & LC_IMAGE_USAGE_COLOR_ATTACHMENT) != 0) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    } else {
        if ((desc->usage & LC_IMAGE_USAGE_DEPTH_STENCIL) != 0) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    if (desc->samples != LC_SAMPLE_COUNT_1 &&
        desc->samples != LC_SAMPLE_COUNT_2 &&
        desc->samples != LC_SAMPLE_COUNT_4 &&
        desc->samples != LC_SAMPLE_COUNT_8) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->samples != LC_SAMPLE_COUNT_1) {
        /* Multisample targets are single-mip 2D resources. */
        if (desc->type != LC_IMAGE_TYPE_2D ||
            (desc->mip_levels != 0 && desc->mip_levels != 1)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    if ((desc->flags & ~LC_IMAGE_FLAG_CUBE_COMPATIBLE) != 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((desc->flags & LC_IMAGE_FLAG_CUBE_COMPATIBLE) != 0) {
        if (desc->type != LC_IMAGE_TYPE_2D || desc->array_layers < 6 ||
            (desc->array_layers % 6) != 0) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }

    full_chain =
        lc_vk_full_mip_levels(desc->width, desc->height, desc->depth);
    if (desc->mip_levels == 0) {
        *out_mips = full_chain;
    } else {
        if (desc->mip_levels > full_chain) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        *out_mips = desc->mip_levels;
    }
    return LC_SUCCESS;
}

lc_result lc_vulkan_image_create(lc_image *image, lc_device *device,
                                 const lc_image_desc *desc) {
    VkImageCreateInfo info;
    VkMemoryRequirements reqs;
    VkMemoryAllocateInfo alloc_info;
    uint32_t mem_index = 0;
    VkImageViewCreateInfo view_info;
    VkFormat vk_format;
    uint32_t mip_levels = 0;
    lc_result res;

    if (image == NULL || device == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE ||
        device->physical_device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    res = lc_vk_validate_image_desc(desc, &mip_levels);
    if (res != LC_SUCCESS) {
        return res;
    }
    vk_format = lc_vulkan_translate_format(desc->format);

    image->device = device;
    image->type = desc->type;
    image->format = desc->format;
    image->width = desc->width;
    /* 1D height/depth are conceptually 1; store the validated values. */
    image->height = (desc->type == LC_IMAGE_TYPE_1D) ? 1 : desc->height;
    image->depth = (desc->type == LC_IMAGE_TYPE_1D ||
                    desc->type == LC_IMAGE_TYPE_2D)
                       ? 1
                       : desc->depth;
    image->mip_levels = mip_levels;
    image->array_layers = desc->array_layers;
    image->usage = desc->usage;
    image->flags = desc->flags;
    image->samples = (uint32_t)desc->samples;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    if ((desc->flags & LC_IMAGE_FLAG_CUBE_COMPATIBLE) != 0) {
        info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    }
    info.imageType = lc_vk_image_type(desc->type);
    info.format = vk_format;
    info.extent.width = image->width;
    info.extent.height = image->height;
    info.extent.depth = image->depth;
    info.mipLevels = image->mip_levels;
    info.arrayLayers = image->array_layers;
    info.samples = lc_vk_sample_count(image->samples);
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = lc_vk_translate_image_usage(desc->usage);
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device->device, &info, NULL, &image->vk_image) !=
        VK_SUCCESS) {
        image->vk_image = VK_NULL_HANDLE;
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    vkGetImageMemoryRequirements(device->device, image->vk_image, &reqs);
    {
        VkPhysicalDeviceMemoryProperties props;
        uint32_t i;
        int found = 0;

        vkGetPhysicalDeviceMemoryProperties(device->physical_device, &props);
        for (i = 0; i < props.memoryTypeCount; i++) {
            if ((reqs.memoryTypeBits & (1u << i)) == 0) {
                continue;
            }
            if ((props.memoryTypes[i].propertyFlags &
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0) {
                mem_index = i;
                found = 1;
                break;
            }
        }
        if (!found) {
            for (i = 0; i < props.memoryTypeCount; i++) {
                if ((reqs.memoryTypeBits & (1u << i)) != 0) {
                    mem_index = i;
                    found = 1;
                    break;
                }
            }
        }
        if (!found) {
            vkDestroyImage(device->device, image->vk_image, NULL);
            image->vk_image = VK_NULL_HANDLE;
            return LC_ERROR_OUT_OF_MEMORY;
        }
    }

    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = reqs.size;
    alloc_info.memoryTypeIndex = mem_index;
    if (vkAllocateMemory(device->device, &alloc_info, NULL,
                         &image->vk_memory) != VK_SUCCESS) {
        vkDestroyImage(device->device, image->vk_image, NULL);
        image->vk_image = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    if (vkBindImageMemory(device->device, image->vk_image, image->vk_memory,
                          0) != VK_SUCCESS) {
        vkFreeMemory(device->device, image->vk_memory, NULL);
        image->vk_memory = VK_NULL_HANDLE;
        vkDestroyImage(device->device, image->vk_image, NULL);
        image->vk_image = VK_NULL_HANDLE;
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }

    /* Default full-resource view. Cube-compatible images get a 2D array
     * view (all layers addressable); dedicated cube views arrive with
     * cubemap rendering. */
    memset(&view_info, 0, sizeof(view_info));
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = image->vk_image;
    if (desc->type == LC_IMAGE_TYPE_1D) {
        view_info.viewType = (image->array_layers > 1)
                                 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
                                 : VK_IMAGE_VIEW_TYPE_1D;
    } else if (desc->type == LC_IMAGE_TYPE_3D) {
        view_info.viewType = VK_IMAGE_VIEW_TYPE_3D;
    } else {
        view_info.viewType = (image->array_layers > 1)
                                 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                 : VK_IMAGE_VIEW_TYPE_2D;
    }
    view_info.format = vk_format;
    view_info.subresourceRange.aspectMask = lc_vk_aspect_for(desc->format);
    view_info.subresourceRange.baseMipLevel = 0;
    view_info.subresourceRange.levelCount = image->mip_levels;
    view_info.subresourceRange.baseArrayLayer = 0;
    view_info.subresourceRange.layerCount = image->array_layers;
    if (vkCreateImageView(device->device, &view_info, NULL,
                          &image->default_view) != VK_SUCCESS) {
        image->default_view = VK_NULL_HANDLE;
        vkFreeMemory(device->device, image->vk_memory, NULL);
        image->vk_memory = VK_NULL_HANDLE;
        vkDestroyImage(device->device, image->vk_image, NULL);
        image->vk_image = VK_NULL_HANDLE;
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }

    image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return LC_SUCCESS;
}

void lc_vulkan_image_destroy(lc_image *image) {
    VkDevice device_handle = VK_NULL_HANDLE;

    if (image == NULL) {
        return;
    }
    if (image->device != NULL) {
        device_handle = image->device->device;
    }
    if (device_handle == VK_NULL_HANDLE) {
        image->default_view = VK_NULL_HANDLE;
        image->vk_image = VK_NULL_HANDLE;
        image->vk_memory = VK_NULL_HANDLE;
        return;
    }
    if (image->default_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device_handle, image->default_view, NULL);
        image->default_view = VK_NULL_HANDLE;
    }
    if (image->vk_image != VK_NULL_HANDLE) {
        vkDestroyImage(device_handle, image->vk_image, NULL);
        image->vk_image = VK_NULL_HANDLE;
    }
    if (image->vk_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device_handle, image->vk_memory, NULL);
        image->vk_memory = VK_NULL_HANDLE;
    }
}

/*
 * Whole-image layout transition on the upload context. Stage/access
 * masks pair each supported endpoint; anything else fails rather than
 * emitting an invalid barrier. On success the tracked layout follows.
 */
lc_result lc_vulkan_image_transition(lc_image *image,
                                     VkImageLayout new_layout) {
    lc_device *device;
    VkImageMemoryBarrier barrier;
    VkPipelineStageFlags src_stage = 0;
    VkAccessFlags src_access = 0;
    VkPipelineStageFlags dst_stage = 0;
    VkAccessFlags dst_access = 0;
    lc_result res;

    if (image == NULL || image->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = image->device;
    if (device->device == VK_NULL_HANDLE ||
        image->vk_image == VK_NULL_HANDLE) {
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    if (image->layout == new_layout) {
        return LC_SUCCESS;
    }

    /* Source endpoint from current tracked layout. Prior work always
     * completed synchronously (immediate submits), so no earlier
     * access needs waiting beyond what is listed. */
    switch (image->layout) {
    case VK_IMAGE_LAYOUT_UNDEFINED:
        src_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        src_access = 0;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        src_access = VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        /* Last use was a sampled read (or an upload-final state);
         * nothing written is outstanding by construction. */
        src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        src_access = VK_ACCESS_SHADER_READ_BIT;
        break;
    default:
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Destination endpoint. Sampled reads target the fragment stage,
     * covering graphics sampling; compute sampling extends this. */
    switch (new_layout) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_access = VK_ACCESS_TRANSFER_WRITE_BIT;
        break;
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        dst_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dst_access = VK_ACCESS_TRANSFER_READ_BIT;
        break;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        dst_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dst_access = VK_ACCESS_SHADER_READ_BIT;
        break;
    default:
        return LC_ERROR_INVALID_ARGUMENT;
    }

    res = lc_vulkan_upload_begin(device);
    if (res != LC_SUCCESS) {
        return res;
    }
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = image->layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image->vk_image;
    barrier.subresourceRange.aspectMask = lc_vk_aspect_for(image->format);
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = image->mip_levels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = image->array_layers;
    vkCmdPipelineBarrier(lc_vulkan_upload_cmd(device), src_stage, dst_stage,
                         0, 0, NULL, 0, NULL, 1, &barrier);
    res = lc_vulkan_upload_submit(device);
    if (res != LC_SUCCESS) {
        return res;
    }
    image->layout = new_layout;
    return LC_SUCCESS;
}

/* Mip extent for one level (minimum 1 per axis). */
static void lc_vk_mip_extent(const lc_image *image, uint32_t mip,
                             uint32_t *w, uint32_t *h, uint32_t *d) {
    *w = image->width >> mip;
    *h = image->height >> mip;
    *d = image->depth >> mip;
    if (*w == 0) {
        *w = 1;
    }
    if (*h == 0) {
        *h = 1;
    }
    if (*d == 0) {
        *d = 1;
    }
}

/* Single range barrier recorder for the mip-generation dance, where
 * each level carries its own old layout. */
static void lc_vk_mip_barrier(VkCommandBuffer cmd, VkImage image,
                              VkImageAspectFlags aspect, uint32_t base_mip,
                              uint32_t level_count,
                              VkAccessFlags src_access,
                              VkPipelineStageFlags src_stage,
                              VkAccessFlags dst_access,
                              VkPipelineStageFlags dst_stage,
                              VkImageLayout old_layout,
                              VkImageLayout new_layout,
                              uint32_t layer_count) {
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
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = layer_count;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1,
                         &barrier);
}

lc_result lc_vulkan_image_write(lc_image *image,
                                const lc_image_upload_desc *upload) {
    lc_device *device;
    uint32_t mw = 0;
    uint32_t mh = 0;
    uint32_t md = 0;
    uint64_t expected = 0;
    uint32_t elem = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_mem = VK_NULL_HANDLE;
    void *staging_ptr = NULL;
    VkMemoryPropertyFlags required;
    VkMemoryPropertyFlags preferred;
    VkBufferImageCopy region;
    lc_result res;

    if (image == NULL || upload == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (image->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = image->device;
    if (device->device == VK_NULL_HANDLE ||
        image->vk_image == VK_NULL_HANDLE) {
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    if (upload->data == NULL || upload->data_size == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (upload->mip_level >= image->mip_levels ||
        upload->array_layer >= image->array_layers) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (image->samples != 1) {
        /* Multisample uploads are meaningless; MSAA targets render. */
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((image->usage & LC_IMAGE_USAGE_TRANSFER_DST) == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    lc_vk_mip_extent(image, upload->mip_level, &mw, &mh, &md);
    if (upload->width == 0 || upload->width > mw || upload->height == 0 ||
        upload->height > mh || upload->depth == 0 || upload->depth > md) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    elem = lc_format_byte_size(image->format);
    if (elem == 0) {
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    expected = (uint64_t)upload->width * (uint64_t)upload->height *
               (uint64_t)upload->depth * (uint64_t)elem;
    if (upload->data_size < expected) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    /* Staging: CPU-visible scratch sized to the payload. */
    required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    res = lc_vulkan_storage_create(device, upload->data_size,
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT, required,
                                   preferred, &staging, &staging_mem,
                                   &staging_ptr);
    if (res != LC_SUCCESS) {
        return res;
    }
    memcpy(staging_ptr, upload->data, (size_t)upload->data_size);
    vkUnmapMemory(device->device, staging_mem);

    /* Level ends sampled-readable per API contract. */
    res = lc_vulkan_image_transition(image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    if (res != LC_SUCCESS) {
        goto cleanup;
    }
    res = lc_vulkan_upload_begin(device);
    if (res != LC_SUCCESS) {
        goto cleanup;
    }
    memset(&region, 0, sizeof(region));
    region.bufferOffset = 0;
    region.bufferRowLength = 0; /* tightly packed */
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = lc_vk_aspect_for(image->format);
    region.imageSubresource.mipLevel = upload->mip_level;
    region.imageSubresource.baseArrayLayer = upload->array_layer;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = 0;
    region.imageOffset.y = 0;
    region.imageOffset.z = 0;
    region.imageExtent.width = upload->width;
    region.imageExtent.height = upload->height;
    region.imageExtent.depth = upload->depth;
    vkCmdCopyBufferToImage(lc_vulkan_upload_cmd(device), staging,
                           image->vk_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    res = lc_vulkan_upload_submit(device);
    if (res != LC_SUCCESS) {
        goto cleanup;
    }
    res = lc_vulkan_image_transition(image,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

cleanup:
    vkDestroyBuffer(device->device, staging, NULL);
    vkFreeMemory(device->device, staging_mem, NULL);
    return res;
}

/*
 * Copy one mip/layer region into a buffer (white-box test path; no
 * public copy API exists by design). Leaves the image sampled-readable
 * so the round-trip is repeatable. Validates handles and range;
 * callers own format/size reasoning.
 */
LC_API lc_result lc_vulkan_copy_image_to_buffer(
    lc_device *device, lc_image *image, uint32_t mip_level,
    uint32_t array_layer, uint32_t width, uint32_t height, uint32_t depth,
    VkBuffer dst, uint64_t dst_offset) {
    uint32_t mw = 0;
    uint32_t mh = 0;
    uint32_t md = 0;
    VkBufferImageCopy region;
    lc_result res;

    if (device == NULL || image == NULL || dst == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE ||
        image->vk_image == VK_NULL_HANDLE) {
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    if (mip_level >= image->mip_levels ||
        array_layer >= image->array_layers) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    lc_vk_mip_extent(image, mip_level, &mw, &mh, &md);
    if (width == 0 || width > mw || height == 0 || height > mh ||
        depth == 0 || depth > md) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    res = lc_vulkan_image_transition(image,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vulkan_upload_begin(device);
    if (res != LC_SUCCESS) {
        return res;
    }
    memset(&region, 0, sizeof(region));
    region.bufferOffset = (VkDeviceSize)dst_offset;
    region.bufferRowLength = 0; /* tightly packed */
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = lc_vk_aspect_for(image->format);
    region.imageSubresource.mipLevel = mip_level;
    region.imageSubresource.baseArrayLayer = array_layer;
    region.imageSubresource.layerCount = 1;
    region.imageOffset.x = 0;
    region.imageOffset.y = 0;
    region.imageOffset.z = 0;
    region.imageExtent.width = width;
    region.imageExtent.height = height;
    region.imageExtent.depth = depth;
    vkCmdCopyImageToBuffer(lc_vulkan_upload_cmd(device), image->vk_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1,
                           &region);
    res = lc_vulkan_upload_submit(device);
    if (res != LC_SUCCESS) {
        return res;
    }
    return lc_vulkan_image_transition(
        image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

lc_result lc_vulkan_image_generate_mipmaps(lc_image *image) {
    lc_device *device;
    VkFormatProperties format_props;
    VkImageAspectFlags aspect;
    uint32_t i;
    lc_result res;

    if (image == NULL || image->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = image->device;
    if (device->device == VK_NULL_HANDLE ||
        image->vk_image == VK_NULL_HANDLE) {
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    if (image->samples != 1 || !lc_format_is_color(image->format)) {
        return LC_ERROR_UNSUPPORTED;
    }
    /* Single level: nothing to build, just settle sampled-readable.
     * No blit happens, so TRANSFER_SRC is not required here. */
    if (image->mip_levels < 2) {
        return lc_vulkan_image_transition(
            image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
    if ((image->usage & (LC_IMAGE_USAGE_TRANSFER_SRC |
                         LC_IMAGE_USAGE_TRANSFER_DST)) !=
        (LC_IMAGE_USAGE_TRANSFER_SRC | LC_IMAGE_USAGE_TRANSFER_DST)) {
        return LC_ERROR_UNSUPPORTED;
    }
    memset(&format_props, 0, sizeof(format_props));
    vkGetPhysicalDeviceFormatProperties(
        device->physical_device, lc_vulkan_translate_format(image->format),
        &format_props);
    if ((format_props.optimalTilingFeatures &
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) == 0) {
        return LC_ERROR_UNSUPPORTED;
    }

    aspect = lc_vk_aspect_for(image->format);
    res = lc_vulkan_upload_begin(device);
    if (res != LC_SUCCESS) {
        return res;
    }
    /* Whole image to TRANSFER_DST first, from whatever tracked
     * layout it holds (fresh UNDEFINED, sampled, or a state left by
     * an earlier failed operation). */
    {
        VkImageLayout from = image->layout;
        VkPipelineStageFlags src_stage =
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        VkAccessFlags src_access = 0;
        VkCommandBuffer cmd = lc_vulkan_upload_cmd(device);

        if (from == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            src_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
            src_access = VK_ACCESS_SHADER_READ_BIT;
        } else if (from == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            src_access = VK_ACCESS_TRANSFER_WRITE_BIT;
        } else if (from == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL) {
            src_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            src_access = VK_ACCESS_TRANSFER_READ_BIT;
        }
        if (from != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            lc_vk_mip_barrier(cmd, image->vk_image, aspect, 0,
                              image->mip_levels, src_access, src_stage,
                              VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, from,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              image->array_layers);
        }
        for (i = 1; i < image->mip_levels; i++) {
            uint32_t sw = 0;
            uint32_t sh = 0;
            uint32_t sd = 0;
            uint32_t dw = 0;
            uint32_t dh = 0;
            uint32_t dd = 0;
            VkImageBlit blit;

            lc_vk_mip_extent(image, i - 1, &sw, &sh, &sd);
            lc_vk_mip_extent(image, i, &dw, &dh, &dd);

            /* Source level becomes readable... */
            lc_vk_mip_barrier(cmd, image->vk_image, aspect, i - 1, 1,
                              VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_TRANSFER_READ_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              image->array_layers);
            memset(&blit, 0, sizeof(blit));
            blit.srcSubresource.aspectMask = aspect;
            blit.srcSubresource.mipLevel = i - 1;
            blit.srcSubresource.baseArrayLayer = 0;
            blit.srcSubresource.layerCount = image->array_layers;
            blit.srcOffsets[0].x = 0;
            blit.srcOffsets[0].y = 0;
            blit.srcOffsets[0].z = 0;
            blit.srcOffsets[1].x = (int32_t)sw;
            blit.srcOffsets[1].y = (int32_t)sh;
            blit.srcOffsets[1].z = (int32_t)sd;
            blit.dstSubresource.aspectMask = aspect;
            blit.dstSubresource.mipLevel = i;
            blit.dstSubresource.baseArrayLayer = 0;
            blit.dstSubresource.layerCount = image->array_layers;
            blit.dstOffsets[0].x = 0;
            blit.dstOffsets[0].y = 0;
            blit.dstOffsets[0].z = 0;
            blit.dstOffsets[1].x = (int32_t)dw;
            blit.dstOffsets[1].y = (int32_t)dh;
            blit.dstOffsets[1].z = (int32_t)dd;
            vkCmdBlitImage(cmd, image->vk_image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           image->vk_image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                           VK_FILTER_LINEAR);
            /* ...then sampled-readable; it is never a blit source
             * again, so no later transition touches it. */
            lc_vk_mip_barrier(cmd, image->vk_image, aspect, i - 1, 1,
                              VK_ACCESS_TRANSFER_READ_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_SHADER_READ_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              image->array_layers);
        }
        /* Final level never sourced a blit: straight to sampled. */
        lc_vk_mip_barrier(cmd, image->vk_image, aspect, image->mip_levels - 1,
                          1, VK_ACCESS_TRANSFER_WRITE_BIT,
                          VK_PIPELINE_STAGE_TRANSFER_BIT,
                          VK_ACCESS_SHADER_READ_BIT,
                          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                          image->array_layers);
    }
    res = lc_vulkan_upload_submit(device);
    if (res != LC_SUCCESS) {
        /* Mid-sequence failure leaves mixed layouts; mark unknown so
         * the next transition starts from UNDEFINED (always legal). */
        image->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        return res;
    }
    image->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return LC_SUCCESS;
}
