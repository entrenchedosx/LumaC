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
    lc_vk_mem_binding binding;
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
    /* Early OOM: extents no device can back (avoids a doomed
     * vkCreateImage and its validation noise). */
    {
        VkPhysicalDeviceProperties props;

        memset(&props, 0, sizeof(props));
        vkGetPhysicalDeviceProperties(device->physical_device, &props);
        if (desc->width > props.limits.maxImageDimension2D ||
            desc->height > props.limits.maxImageDimension2D ||
            (desc->type == LC_IMAGE_TYPE_3D &&
             desc->depth > props.limits.maxImageDimension3D)) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
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
    /* Allocator binding (Phase 19): suballocated block region, with
     * the historical prefer-device-local/fallback-any policy. */
    memset(&binding, 0, sizeof(binding));
    res = lc_vk_mem_alloc(device, LC_VK_MEM_DEVICE_IMAGES,
                          reqs.memoryTypeBits, 0, reqs.size,
                          reqs.alignment, &binding);
    if (res != LC_SUCCESS) {
        vkDestroyImage(device->device, image->vk_image, NULL);
        image->vk_image = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    if (vkBindImageMemory(device->device, image->vk_image,
                          binding.memory,
                          (VkDeviceSize)binding.offset) != VK_SUCCESS) {
        lc_vk_mem_free(device, &binding);
        vkDestroyImage(device->device, image->vk_image, NULL);
        image->vk_image = VK_NULL_HANDLE;
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    image->vk_memory = binding.memory;
    image->vk_memory_offset = binding.offset;
    image->memory_dedicated = binding.dedicated;
    image->memory_block = binding.block;
    image->memory_class = LC_MEMORY_CLASS_DEVICE_LOCAL;
    image->allocation_size = binding.size;

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
        lc_vk_mem_binding doomed;

        image->default_view = VK_NULL_HANDLE;
        /* Roll back the binding (Section 15: clean allocator state
         * on post-alloc failure). */
        memset(&doomed, 0, sizeof(doomed));
        doomed.memory = image->vk_memory;
        doomed.offset = image->vk_memory_offset;
        doomed.size = image->allocation_size;
        doomed.dedicated = image->memory_dedicated;
        doomed.block = image->memory_block;
        lc_vk_mem_free(device, &doomed);
        image->vk_memory = VK_NULL_HANDLE;
        image->vk_memory_offset = 0;
        image->memory_block = NULL;
        vkDestroyImage(device->device, image->vk_image, NULL);
        image->vk_image = VK_NULL_HANDLE;
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }

    /* Per-subresource semantic states, all UNDEFINED (calloc zeroes,
     * and UNDEFINED is 0 by design). Sized mips x layers so mixed
     * states stay representable and truthful. Owners/epochs start
     * zeroed (owners fixed up below). */
    image->states = (lc_resource_state *)calloc(
        (size_t)image->mip_levels * (size_t)image->array_layers,
        sizeof(lc_resource_state));
    image->owners = (uint32_t *)calloc(
        (size_t)image->mip_levels * (size_t)image->array_layers,
        sizeof(uint32_t));
    image->epochs = (uint64_t *)calloc(
        (size_t)image->mip_levels * (size_t)image->array_layers,
        sizeof(uint64_t));
    if (image->states == NULL || image->owners == NULL ||
        image->epochs == NULL) {
        lc_vk_mem_binding doomed;

        free(image->states);
        image->states = NULL;
        free(image->owners);
        image->owners = NULL;
        free(image->epochs);
        image->epochs = NULL;

        vkDestroyImageView(device->device, image->default_view, NULL);
        image->default_view = VK_NULL_HANDLE;
        memset(&doomed, 0, sizeof(doomed));
        doomed.memory = image->vk_memory;
        doomed.offset = image->vk_memory_offset;
        doomed.size = image->allocation_size;
        doomed.dedicated = image->memory_dedicated;
        doomed.block = image->memory_block;
        lc_vk_mem_free(device, &doomed);
        image->vk_memory = VK_NULL_HANDLE;
        image->vk_memory_offset = 0;
        image->memory_block = NULL;
        vkDestroyImage(device->device, image->vk_image, NULL);
        image->vk_image = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    /* Owners start at the graphics family (calloc would zero, which
     * need not equal any family). */
    {
        uint32_t m;
        uint32_t l;

        for (l = 0; l < image->array_layers; l++) {
            for (m = 0; m < image->mip_levels; m++) {
                image->owners[(size_t)l * image->mip_levels + m] =
                    device->graphics_queue_family;
            }
        }
    }
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
    /* State tracking is host memory; free it on every path. Views are
     * destroyed by image_view.c hooks before this runs. */
    if (image->states != NULL) {
        free(image->states);
        image->states = NULL;
    }
    if (image->owners != NULL) {
        free(image->owners);
        image->owners = NULL;
    }
    if (image->epochs != NULL) {
        free(image->epochs);
        image->epochs = NULL;
    }
    if (device_handle == VK_NULL_HANDLE) {
        image->default_view = VK_NULL_HANDLE;
        image->vk_image = VK_NULL_HANDLE;
        image->vk_memory = VK_NULL_HANDLE;
        image->vk_memory_offset = 0;
        image->memory_block = NULL;
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
        lc_vk_mem_binding binding;

        memset(&binding, 0, sizeof(binding));
        binding.memory = image->vk_memory;
        binding.offset = image->vk_memory_offset;
        binding.size = image->allocation_size;
        binding.dedicated = image->memory_dedicated;
        binding.block = image->memory_block;
        lc_vk_mem_free(image->device, &binding);
        image->vk_memory = VK_NULL_HANDLE;
        image->vk_memory_offset = 0;
        image->memory_block = NULL;
    }
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

lc_result lc_vulkan_image_write(lc_image *image,
                                const lc_image_upload_desc *upload) {
    lc_device *device;
    uint32_t mw = 0;
    uint32_t mh = 0;
    uint32_t md = 0;
    uint64_t expected = 0;
    uint32_t elem = 0;
    VkBuffer staging = VK_NULL_HANDLE;
    lc_vk_mem_binding stage;
    void *staging_ptr = NULL;
    VkBufferImageCopy region;
    lc_result res;

    memset(&stage, 0, sizeof(stage));

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

    /* Pool staging (PART Z): suballocated, persistently mapped;
     * no dedicated VkDeviceMemory churn per upload. */
    res = lc_vk_stage_acquire(device, upload->data_size, 1,
                              VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging,
                              &stage, &staging_ptr);
    if (res != LC_SUCCESS) {
        return res;
    }
    memcpy(staging_ptr, upload->data, (size_t)upload->data_size);
    res = lc_vk_mem_flush(device, &stage, 0, upload->data_size);
    if (res != LC_SUCCESS) {
        goto cleanup;
    }

    /* Level ends sampled-readable per API contract. */
    res = lc_vulkan_image_transition(image,
                                     LC_RESOURCE_STATE_TRANSFER_DST);
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
    /* Level ends sampled-readable per API contract when the image
     * can be sampled; otherwise TRANSFER_DST (SHADER_READ would be
     * a VUID without the SAMPLED bit). */
    if ((image->usage & LC_IMAGE_USAGE_SAMPLED) != 0) {
        res = lc_vulkan_image_transition(
            image, LC_RESOURCE_STATE_SHADER_READ);
    } else {
        res = lc_vulkan_image_transition(
            image, LC_RESOURCE_STATE_TRANSFER_DST);
    }

cleanup:
    lc_vk_stage_release(device, staging, &stage);
    return res;
}

/*
 * Public readback backend (Phase 18): synchronous CPU-visible copy.
 * Drains prior device work, stages through a host-visible buffer,
 * and returns tightly packed bytes (callers validated everything).
 */
lc_result lc_vulkan_image_readback(lc_image *image, uint32_t mip_level,
                                   uint32_t array_layer, uint32_t width,
                                   uint32_t height, uint32_t depth, void *dst,
                                   size_t byte_size) {
    lc_device *device;
    VkBuffer staging = VK_NULL_HANDLE;
    lc_vk_mem_binding stage;
    void *staging_ptr = NULL;
    lc_result res;

    memset(&stage, 0, sizeof(stage));
    if (image == NULL || image->device == NULL || dst == NULL ||
        byte_size == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = image->device;
    if (device->device == VK_NULL_HANDLE) {
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    /* Drain prior work (renders, uploads): readback observes finished
     * results. Coarse but correct; documented stall. */
    vkDeviceWaitIdle(device->device);

    res = lc_vk_stage_acquire(device, (uint64_t)byte_size, 0,
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT, &staging,
                              &stage, &staging_ptr);
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vulkan_copy_image_to_buffer(device, image, mip_level,
                                         array_layer, width, height, depth,
                                         staging, 0);
    if (res != LC_SUCCESS) {
        lc_vk_stage_release(device, staging, &stage);
        return res;
    }
    /* Invalidate for non-coherent before CPU access (PART W). */
    res = lc_vk_mem_invalidate(device, &stage, 0, (uint64_t)byte_size);
    if (res != LC_SUCCESS) {
        lc_vk_stage_release(device, staging, &stage);
        return res;
    }
    memcpy(dst, staging_ptr, byte_size);
    lc_vk_stage_release(device, staging, &stage);
    return LC_SUCCESS;
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
    /* Readback sources must opt into transfer (loud INVALID instead of
     * driver-dependent behavior or silent zeros). */
    if ((image->usage & LC_IMAGE_USAGE_TRANSFER_SRC) == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    lc_vk_mip_extent(image, mip_level, &mw, &mh, &md);
    if (width == 0 || width > mw || height == 0 || height > mh ||
        depth == 0 || depth > md) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    res = lc_vulkan_image_transition_range(
        image, mip_level, 1, array_layer, 1,
        LC_RESOURCE_STATE_TRANSFER_SRC);
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
    /* Settle sampled-readable when the image can be sampled; pure
     * transfer/depth images (no SAMPLED usage) settle back to
     * TRANSFER_SRC — SHADER_READ would be a VUID without the
     * SAMPLED bit. Range-exact so other mips/layers keep their
     * states (PART D). */
    if ((image->usage & LC_IMAGE_USAGE_SAMPLED) != 0) {
        return lc_vulkan_image_transition_range(
            image, mip_level, 1, array_layer, 1,
            LC_RESOURCE_STATE_SHADER_READ);
    }
    return lc_vulkan_image_transition_range(
        image, mip_level, 1, array_layer, 1,
        LC_RESOURCE_STATE_TRANSFER_SRC);
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
        return lc_vulkan_image_transition(image,
                                          LC_RESOURCE_STATE_SHADER_READ);
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
    /* Whole image to TRANSFER_DST first via tracked state, so every
     * level below starts from a known state. */
    res = lc_vulkan_image_transition(image,
                                     LC_RESOURCE_STATE_TRANSFER_DST);
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vulkan_upload_begin(device);
    if (res != LC_SUCCESS) {
        return res;
    }
    {
        VkCommandBuffer cmd = lc_vulkan_upload_cmd(device);

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
            res = lc_vk_sync_record_span(
                cmd, image, i - 1, 1, 0, image->array_layers,
                LC_RESOURCE_STATE_TRANSFER_DST,
                LC_RESOURCE_STATE_TRANSFER_SRC);
            if (res != LC_SUCCESS) {
                break;
            }
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
            res = lc_vk_sync_record_span(
                cmd, image, i - 1, 1, 0, image->array_layers,
                LC_RESOURCE_STATE_TRANSFER_SRC,
                LC_RESOURCE_STATE_SHADER_READ);
            if (res != LC_SUCCESS) {
                break;
            }
        }
        if (res == LC_SUCCESS) {
            /* Final level never sourced a blit: straight to sampled. */
            res = lc_vk_sync_record_span(
                cmd, image, image->mip_levels - 1, 1, 0,
                image->array_layers, LC_RESOURCE_STATE_TRANSFER_DST,
                LC_RESOURCE_STATE_SHADER_READ);
        }
    }
    if (res != LC_SUCCESS) {
        lc_vulkan_upload_submit(device);
        lc_vk_sync_mark(image, 0, image->mip_levels, 0,
                        image->array_layers,
                        LC_RESOURCE_STATE_UNDEFINED);
        return res;
    }
    res = lc_vulkan_upload_submit(device);
    {
        /* Bulk state update: every level ends sampled-readable on
         * success. On failure the real states are unknown, so mark
         * everything UNDEFINED (always legal to leave from). */
        lc_resource_state mark = (res == LC_SUCCESS)
                                     ? LC_RESOURCE_STATE_SHADER_READ
                                     : LC_RESOURCE_STATE_UNDEFINED;

        lc_vk_sync_mark(image, 0, image->mip_levels, 0,
                        image->array_layers, mark);
    }
    return res;
}

/* Map a backend-neutral view type onto Vulkan for an image of known
 * dimensionality, enforcing the pairing rules. Returns 1 on success. */
static int lc_vk_view_type_for(lc_image_view_type type, lc_image_type image_type,
                               uint32_t layer_count, int cube_flag,
                               VkImageViewType *out) {
    switch (type) {
    case LC_IMAGE_VIEW_1D:
        if (image_type != LC_IMAGE_TYPE_1D || layer_count != 1) {
            return 0;
        }
        *out = VK_IMAGE_VIEW_TYPE_1D;
        return 1;
    case LC_IMAGE_VIEW_1D_ARRAY:
        if (image_type != LC_IMAGE_TYPE_1D) {
            return 0;
        }
        *out = VK_IMAGE_VIEW_TYPE_1D_ARRAY;
        return 1;
    case LC_IMAGE_VIEW_2D:
        if (image_type != LC_IMAGE_TYPE_2D || layer_count != 1) {
            return 0;
        }
        *out = VK_IMAGE_VIEW_TYPE_2D;
        return 1;
    case LC_IMAGE_VIEW_2D_ARRAY:
        if (image_type != LC_IMAGE_TYPE_2D) {
            return 0;
        }
        *out = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        return 1;
    case LC_IMAGE_VIEW_3D:
        if (image_type != LC_IMAGE_TYPE_3D || layer_count != 1) {
            return 0;
        }
        *out = VK_IMAGE_VIEW_TYPE_3D;
        return 1;
    case LC_IMAGE_VIEW_CUBE:
        if (image_type != LC_IMAGE_TYPE_2D || !cube_flag ||
            layer_count != 6) {
            return 0;
        }
        *out = VK_IMAGE_VIEW_TYPE_CUBE;
        return 1;
    case LC_IMAGE_VIEW_CUBE_ARRAY:
        if (image_type != LC_IMAGE_TYPE_2D || !cube_flag ||
            layer_count < 6 || (layer_count % 6) != 0) {
            return 0;
        }
        *out = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
        return 1;
    default:
        return 0;
    }
}

lc_result lc_vulkan_image_view_create(lc_image_view *view, lc_image *image,
                                      const lc_image_view_desc *desc) {
    VkImageViewType vk_type = VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo info;
    VkFormat vk_format;
    VkImageAspectFlags aspect;
    int cube_flag;

    if (view == NULL || image == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (image->device == NULL ||
        image->device->device == VK_NULL_HANDLE ||
        image->vk_image == VK_NULL_HANDLE) {
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    /* Aspect must be a nonzero subset of what the format carries. */
    aspect = 0;
    if ((desc->aspect & LC_IMAGE_ASPECT_COLOR) != 0 &&
        lc_format_is_color(image->format)) {
        aspect |= VK_IMAGE_ASPECT_COLOR_BIT;
    }
    if ((desc->aspect & LC_IMAGE_ASPECT_DEPTH) != 0 &&
        lc_format_is_depth(image->format)) {
        aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if ((desc->aspect & LC_IMAGE_ASPECT_STENCIL) != 0 &&
        lc_format_is_stencil(image->format)) {
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (aspect == 0 ||
        (desc->aspect & ~(LC_IMAGE_ASPECT_COLOR | LC_IMAGE_ASPECT_DEPTH |
                          LC_IMAGE_ASPECT_STENCIL)) != 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Ranges must be nonempty and fit. Subtraction order is
     * overflow-safe: base is checked first. */
    if (desc->mip_level_count == 0 ||
        desc->base_mip_level >= image->mip_levels ||
        desc->mip_level_count > image->mip_levels - desc->base_mip_level) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->array_layer_count == 0 ||
        desc->base_array_layer >= image->array_layers ||
        desc->array_layer_count >
            image->array_layers - desc->base_array_layer) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    cube_flag = (image->flags & LC_IMAGE_FLAG_CUBE_COMPATIBLE) != 0;
    if (!lc_vk_view_type_for(desc->type, image->type,
                             desc->array_layer_count, cube_flag, &vk_type)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Format equality only (no reinterpretation yet, documented). */
    if (desc->format == LC_FORMAT_UNDEFINED) {
        vk_format = lc_vulkan_translate_format(image->format);
    } else if (desc->format == image->format) {
        vk_format = lc_vulkan_translate_format(desc->format);
    } else {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (vk_format == VK_FORMAT_UNDEFINED) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    view->device = image->device;
    view->image = image;
    view->type = desc->type;
    view->format =
        (desc->format == LC_FORMAT_UNDEFINED) ? image->format : desc->format;
    view->aspect = desc->aspect;
    view->base_mip_level = desc->base_mip_level;
    view->mip_level_count = desc->mip_level_count;
    view->base_array_layer = desc->base_array_layer;
    view->array_layer_count = desc->array_layer_count;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image->vk_image;
    info.viewType = vk_type;
    info.format = vk_format;
    info.subresourceRange.aspectMask = aspect;
    info.subresourceRange.baseMipLevel = desc->base_mip_level;
    info.subresourceRange.levelCount = desc->mip_level_count;
    info.subresourceRange.baseArrayLayer = desc->base_array_layer;
    info.subresourceRange.layerCount = desc->array_layer_count;
    if (vkCreateImageView(image->device->device, &info, NULL,
                          &view->vk_view) != VK_SUCCESS) {
        view->vk_view = VK_NULL_HANDLE;
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    return LC_SUCCESS;
}

void lc_vulkan_image_view_destroy(lc_image_view *view) {
    if (view == NULL || view->vk_view == VK_NULL_HANDLE) {
        return;
    }
    /* Device must still be alive; image teardown destroys dependent
     * views before the image, and device teardown destroys views
     * before VkDevice. */
    if (view->device != NULL && view->device->device != VK_NULL_HANDLE) {
        vkDestroyImageView(view->device->device, view->vk_view, NULL);
    }
    view->vk_view = VK_NULL_HANDLE;
}
