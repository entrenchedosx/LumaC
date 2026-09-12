/*
 * Vulkan sampler backend (Phase 9: sampling configuration).
 *
 * Straightforward VkSampler creation from backend-neutral enums.
 * Anisotropy is enabled only when the device negotiated support at
 * creation (see lc_device.anisotropy_supported); requested values are
 * clamped to the device maximum. No descriptor or binding objects
 * exist yet - samplers are standalone configuration handles.
 */

#include <string.h>

#include "graphics/graphics_internal.h"

static VkFilter lc_vk_filter(lc_filter filter) {
    return (filter == LC_FILTER_LINEAR) ? VK_FILTER_LINEAR
                                        : VK_FILTER_NEAREST;
}

static VkSamplerMipmapMode lc_vk_mipmap_mode(lc_mipmap_mode mode) {
    return (mode == LC_MIPMAP_MODE_LINEAR)
               ? VK_SAMPLER_MIPMAP_MODE_LINEAR
               : VK_SAMPLER_MIPMAP_MODE_NEAREST;
}

static VkSamplerAddressMode lc_vk_address_mode(lc_address_mode mode) {
    switch (mode) {
    case LC_ADDRESS_MIRRORED_REPEAT:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case LC_ADDRESS_CLAMP_TO_EDGE:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case LC_ADDRESS_CLAMP_TO_BORDER:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    case LC_ADDRESS_REPEAT:
    default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

lc_result lc_vulkan_sampler_create(lc_sampler *sampler, lc_device *device,
                                   const lc_sampler_desc *desc) {
    VkPhysicalDeviceProperties props;
    VkSamplerCreateInfo info;
    float max_supported = 1.0f;
    float anisotropy = 1.0f;

    if (sampler == NULL || device == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE ||
        device->physical_device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    if (desc->min_filter != LC_FILTER_NEAREST &&
        desc->min_filter != LC_FILTER_LINEAR) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->mag_filter != LC_FILTER_NEAREST &&
        desc->mag_filter != LC_FILTER_LINEAR) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->mipmap_mode != LC_MIPMAP_MODE_NEAREST &&
        desc->mipmap_mode != LC_MIPMAP_MODE_LINEAR) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->address_u > LC_ADDRESS_CLAMP_TO_BORDER ||
        desc->address_v > LC_ADDRESS_CLAMP_TO_BORDER ||
        desc->address_w > LC_ADDRESS_CLAMP_TO_BORDER) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->min_lod < 0.0f || desc->max_lod < 0.0f ||
        desc->min_lod > desc->max_lod) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->max_anisotropy < 1.0f) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    /* Anisotropy above 1 needs negotiated device support; without it
     * the effective maximum is 1 (disabled). Requested values clamp
     * to the device maximum rather than failing. */
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(device->physical_device, &props);
    if (device->anisotropy_supported != 0) {
        max_supported = props.limits.maxSamplerAnisotropy;
        if (max_supported < 1.0f) {
            max_supported = 1.0f;
        }
    }
    anisotropy = desc->max_anisotropy;
    if (anisotropy > max_supported) {
        anisotropy = max_supported;
    }

    sampler->device = device;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    info.magFilter = lc_vk_filter(desc->mag_filter);
    info.minFilter = lc_vk_filter(desc->min_filter);
    info.mipmapMode = lc_vk_mipmap_mode(desc->mipmap_mode);
    info.addressModeU = lc_vk_address_mode(desc->address_u);
    info.addressModeV = lc_vk_address_mode(desc->address_v);
    info.addressModeW = lc_vk_address_mode(desc->address_w);
    info.mipLodBias = desc->mip_lod_bias;
    /* Anisotropy is only meaningful with linear filtering, but Vulkan
     * permits the combination regardless; enabling follows support. */
    info.anisotropyEnable =
        (anisotropy > 1.0f) ? VK_TRUE : VK_FALSE;
    info.maxAnisotropy = anisotropy;
    info.compareEnable = VK_FALSE;
    info.compareOp = VK_COMPARE_OP_ALWAYS;
    info.minLod = desc->min_lod;
    info.maxLod = desc->max_lod;
    info.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    info.unnormalizedCoordinates = VK_FALSE;

    if (vkCreateSampler(device->device, &info, NULL, &sampler->vk_sampler) !=
        VK_SUCCESS) {
        sampler->vk_sampler = VK_NULL_HANDLE;
        return LC_ERROR_SAMPLER_CREATION_FAILED;
    }
    return LC_SUCCESS;
}

void lc_vulkan_sampler_destroy(lc_sampler *sampler) {
    if (sampler == NULL || sampler->vk_sampler == VK_NULL_HANDLE) {
        return;
    }
    /* Device must still be alive; device teardown destroys its
     * samplers before tearing down the VkDevice. */
    if (sampler->device != NULL &&
        sampler->device->device != VK_NULL_HANDLE) {
        vkDestroySampler(sampler->device->device, sampler->vk_sampler, NULL);
    }
    sampler->vk_sampler = VK_NULL_HANDLE;
}
