/*
 * Vulkan swapchain backend (Phase 5: swapchain infrastructure, no rendering).
 *
 * Builds a real VkSwapchainKHR plus one 2D color VkImageView per image.
 * Selection is deterministic and capability-driven: format, present mode,
 * extent, image count, sharing mode, composite alpha, and transform all
 * derive from surface queries. Nothing is acquired or presented here;
 * no semaphores, fences, command buffers, or render passes exist yet.
 *
 * Rebuilds are transactional (see lc_vulkan_swapchain_rebuild): the new
 * swapchain is fully constructed - including all image views - before
 * the old state is touched, so a failed recreate leaves the existing
 * swapchain intact.
 */

#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

static uint32_t lc_vk_clamp_u32(uint32_t v, uint32_t lo, uint32_t hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* Deterministic format preference: sRGB B8G8R8A8, then sRGB R8G8B8A8,
 * otherwise the first reported format. A single VK_FORMAT_UNDEFINED
 * entry means the driver imposes no preference, so take our favorite.
 * Fails only when the driver reports no formats at all. */
static lc_result lc_vk_choose_format(VkPhysicalDevice physical,
                                     VkSurfaceKHR surface,
                                     VkFormat *out_format,
                                     VkColorSpaceKHR *out_color_space) {
    uint32_t count = 0;
    uint32_t i;
    VkSurfaceFormatKHR *formats = NULL;
    lc_result res = LC_ERROR_SWAPCHAIN_UNSUPPORTED;

    if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count,
                                             NULL) != VK_SUCCESS ||
        count == 0) {
        return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
    }
    formats = (VkSurfaceFormatKHR *)malloc(sizeof(VkSurfaceFormatKHR) * count);
    if (formats == NULL) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count,
                                             formats) != VK_SUCCESS) {
        free(formats);
        return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
    }

    if (count == 1 && formats[0].format == VK_FORMAT_UNDEFINED) {
        *out_format = VK_FORMAT_B8G8R8A8_SRGB;
        *out_color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        res = LC_SUCCESS;
    } else {
        *out_format = formats[0].format;
        *out_color_space = formats[0].colorSpace;
        res = LC_SUCCESS;
        for (i = 0; i < count; i++) {
            if (formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
                formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                *out_format = formats[i].format;
                *out_color_space = formats[i].colorSpace;
                break;
            }
            if (formats[i].format == VK_FORMAT_R8G8B8A8_SRGB &&
                formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR &&
                (*out_format != VK_FORMAT_B8G8R8A8_SRGB ||
                 *out_color_space != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)) {
                *out_format = formats[i].format;
                *out_color_space = formats[i].colorSpace;
            }
        }
    }
    free(formats);
    return res;
}

/* Present mode preference. FIFO is guaranteed by Vulkan so vsync always
 * resolves; without vsync prefer MAILBOX, then IMMEDIATE, then FIFO. */
static lc_result lc_vk_choose_present_mode(VkPhysicalDevice physical,
                                           VkSurfaceKHR surface, int vsync,
                                           VkPresentModeKHR *out_mode) {
    uint32_t count = 0;
    uint32_t i;
    VkPresentModeKHR *modes = NULL;
    VkPresentModeKHR chosen = VK_PRESENT_MODE_FIFO_KHR;

    if (vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count,
                                                  NULL) != VK_SUCCESS ||
        count == 0) {
        return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
    }
    modes = (VkPresentModeKHR *)malloc(sizeof(VkPresentModeKHR) * count);
    if (modes == NULL) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface, &count,
                                                  modes) != VK_SUCCESS) {
        free(modes);
        return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
    }
    if (vsync == 0) {
        chosen = VK_PRESENT_MODE_FIFO_KHR; /* fallback, always valid */
        for (i = 0; i < count; i++) {
            if (modes[i] == VK_PRESENT_MODE_MAILBOX_KHR) {
                chosen = modes[i];
                break;
            }
            if (modes[i] == VK_PRESENT_MODE_IMMEDIATE_KHR &&
                chosen == VK_PRESENT_MODE_FIFO_KHR) {
                chosen = modes[i];
            }
        }
    }
    free(modes);
    *out_mode = chosen;
    return LC_SUCCESS;
}

/* Extent selection: a fixed surface extent wins; otherwise clamp the
 * request into the allowed range. A zero result (minimized surface)
 * is a recoverable caller-side condition, not a Vulkan error. */
static lc_result lc_vk_choose_extent(const VkSurfaceCapabilitiesKHR *caps,
                                     uint32_t req_width, uint32_t req_height,
                                     VkExtent2D *out_extent) {
    VkExtent2D extent;

    if (caps->currentExtent.width != UINT32_MAX) {
        extent = caps->currentExtent;
    } else {
        extent.width = lc_vk_clamp_u32(req_width,
                                       caps->minImageExtent.width,
                                       caps->maxImageExtent.width);
        extent.height = lc_vk_clamp_u32(req_height,
                                        caps->minImageExtent.height,
                                        caps->maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) {
        return LC_ERROR_ZERO_EXTENT;
    }
    *out_extent = extent;
    return LC_SUCCESS;
}

/* Image count: explicit preference clamped into [min, max] (max == 0
 * means unbounded), otherwise min + 1 for a spare. Never assumes
 * triple buffering. */
static uint32_t lc_vk_choose_image_count(const VkSurfaceCapabilitiesKHR *caps,
                                         uint32_t preferred) {
    uint32_t count =
        (preferred > 0) ? preferred : (caps->minImageCount + 1u);

    if (count < caps->minImageCount) {
        count = caps->minImageCount;
    }
    if (caps->maxImageCount > 0 && count > caps->maxImageCount) {
        count = caps->maxImageCount;
    }
    return count;
}

/* Composite alpha: opaque first, then the remaining flags in a fixed
 * order. Never assumes opaque is supported. */
static lc_result lc_vk_choose_alpha(const VkSurfaceCapabilitiesKHR *caps,
                                    VkCompositeAlphaFlagBitsKHR *out_alpha) {
    static const VkCompositeAlphaFlagBitsKHR order[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
    };
    size_t i;

    for (i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
        if ((caps->supportedCompositeAlpha & order[i]) != 0) {
            *out_alpha = order[i];
            return LC_SUCCESS;
        }
    }
    return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
}

static VkSurfaceTransformFlagBitsKHR lc_vk_choose_transform(
    const VkSurfaceCapabilitiesKHR *caps) {
    if ((caps->supportedTransforms &
         VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) != 0) {
        return VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    }
    return caps->currentTransform;
}

void lc_vulkan_swapchain_teardown(lc_swapchain *swapchain) {
    uint32_t i;
    VkDevice device_handle = VK_NULL_HANDLE;

    if (swapchain == NULL) {
        return;
    }
    if (swapchain->device != NULL) {
        device_handle = swapchain->device->device;
    }
    /* Coarse but correct: frames may be in flight (submitted, awaiting
     * present), and destroying a swapchain with pending work is
     * invalid. Nothing here needs finer granularity yet. */
    if (device_handle != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_handle);
    }
    /* Views first (children of the swapchain), then the swapchain,
     * then the host arrays. Images belong to VkSwapchainKHR and are
     * never destroyed directly. Frame objects (pool/sync) persist:
     * they do not depend on individual images. */
    if (swapchain->image_views != NULL) {
        if (device_handle != VK_NULL_HANDLE) {
            for (i = 0; i < swapchain->image_count; i++) {
                if (swapchain->image_views[i] != VK_NULL_HANDLE) {
                    vkDestroyImageView(device_handle,
                                       swapchain->image_views[i], NULL);
                }
            }
        }
        free(swapchain->image_views);
        swapchain->image_views = NULL;
    }
    if (swapchain->images != NULL) {
        free(swapchain->images);
        swapchain->images = NULL;
    }
    /* Present semaphores die here too (after the idle wait above, no
     * present can still reference them). Frame slots/pool persist. */
    if (swapchain->present_semaphores != NULL) {
        if (device_handle != VK_NULL_HANDLE) {
            for (i = 0; i < swapchain->image_count; i++) {
                if (swapchain->present_semaphores[i] != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device_handle,
                                       swapchain->present_semaphores[i],
                                       NULL);
                }
            }
        }
        free(swapchain->present_semaphores);
        swapchain->present_semaphores = NULL;
    }
    if (swapchain->images_in_flight != NULL) {
        free(swapchain->images_in_flight);
        swapchain->images_in_flight = NULL;
    }
    if (swapchain->image_initialized != NULL) {
        free(swapchain->image_initialized);
        swapchain->image_initialized = NULL;
    }
    if (swapchain->vk_swapchain != VK_NULL_HANDLE) {
        if (device_handle != VK_NULL_HANDLE) {
            vkDestroySwapchainKHR(device_handle, swapchain->vk_swapchain,
                                  NULL);
        }
        swapchain->vk_swapchain = VK_NULL_HANDLE;
    }
    swapchain->image_count = 0;
    swapchain->extent.width = 0;
    swapchain->extent.height = 0;
    swapchain->frame_active = 0;
    swapchain->frame_suboptimal = 0;
}

/* Retrieve every swapchain image into a fresh malloc'd array. */
static lc_result lc_vk_get_images(VkDevice device, VkSwapchainKHR swapchain,
                                  VkImage **out_images,
                                  uint32_t *out_count) {
    uint32_t count = 0;
    VkImage *images = NULL;

    if (vkGetSwapchainImagesKHR(device, swapchain, &count, NULL) !=
            VK_SUCCESS ||
        count == 0) {
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }
    images = (VkImage *)malloc(sizeof(VkImage) * count);
    if (images == NULL) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    if (vkGetSwapchainImagesKHR(device, swapchain, &count, images) !=
        VK_SUCCESS) {
        free(images);
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }
    *out_images = images;
    *out_count = count;
    return LC_SUCCESS;
}

/* Destroy a view array (skipping NULL entries) and free it. */
static void lc_vk_destroy_view_list(VkDevice device, VkImageView *views,
                                    uint32_t count) {
    uint32_t i;

    if (views == NULL) {
        return;
    }
    if (device != VK_NULL_HANDLE) {
        for (i = 0; i < count; i++) {
            if (views[i] != VK_NULL_HANDLE) {
                vkDestroyImageView(device, views[i], NULL);
            }
        }
    }
    free(views);
}

/* One 2D color view per image (single mip, single layer). Midway
 * failure destroys the views made so far and reports failure; the
 * caller then destroys the new swapchain. */
static lc_result lc_vk_create_views(VkDevice device, VkFormat format,
                                    VkImage *images, uint32_t count,
                                    VkImageView **out_views) {
    uint32_t i;
    VkImageView *views = (VkImageView *)calloc(count, sizeof(VkImageView));

    if (views == NULL) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < count; i++) {
        VkImageViewCreateInfo info;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.image = images[i];
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        info.format = format;
        info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        info.subresourceRange.baseMipLevel = 0;
        info.subresourceRange.levelCount = 1;
        info.subresourceRange.baseArrayLayer = 0;
        info.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &info, NULL, &views[i]) != VK_SUCCESS) {
            lc_vk_destroy_view_list(device, views, count);
            return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
        }
    }
    *out_views = views;
    return LC_SUCCESS;
}

lc_result lc_vulkan_swapchain_rebuild(lc_swapchain *swapchain, uint32_t width,
                                      uint32_t height,
                                      VkSwapchainKHR old_swapchain) {
    lc_device *device;
    lc_surface *surface;
    VkSurfaceCapabilitiesKHR caps;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D extent;
    uint32_t image_count = 0;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    VkSurfaceTransformFlagBitsKHR transform;
    VkSwapchainCreateInfoKHR info;
    uint32_t queue_families[2];
    VkSwapchainKHR new_handle = VK_NULL_HANDLE;
    VkImage *images = NULL;
    VkImageView *views = NULL;
    VkFence *in_flight = NULL;
    uint8_t *initialized = NULL;
    VkSemaphore *present_sems = NULL;
    lc_result res;

    if (swapchain == NULL || swapchain->device == NULL ||
        swapchain->surface == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (width == 0 || height == 0) {
        return LC_ERROR_ZERO_EXTENT;
    }
    device = swapchain->device;
    surface = swapchain->surface;
    if (device->device == VK_NULL_HANDLE ||
        device->physical_device == VK_NULL_HANDLE ||
        surface->vk_surface == VK_NULL_HANDLE) {
        return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
    }

    /* Capability-driven selection; any failure leaves the existing
     * swapchain (if any) completely untouched. */
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device->physical_device,
                                                  surface->vk_surface,
                                                  &caps) != VK_SUCCESS) {
        return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
    }
    /* Clearing uses vkCmdClearColorImage, so images must support both
     * future color-attachment rendering and transfer-destination clears.
     * Both flags are universal on modern desktop drivers; anything else
     * fails cleanly instead of issuing invalid commands. */
    if ((caps.supportedUsageFlags &
         (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
          VK_IMAGE_USAGE_TRANSFER_DST_BIT)) !=
        (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
         VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
    }
    res = lc_vk_choose_format(device->physical_device, surface->vk_surface,
                              &format, &color_space);
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vk_choose_present_mode(device->physical_device,
                                    surface->vk_surface, swapchain->vsync,
                                    &present_mode);
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vk_choose_extent(&caps, width, height, &extent);
    if (res != LC_SUCCESS) {
        return res;
    }
    image_count =
        lc_vk_choose_image_count(&caps, swapchain->preferred_image_count);
    res = lc_vk_choose_alpha(&caps, &alpha);
    if (res != LC_SUCCESS) {
        return res;
    }
    transform = lc_vk_choose_transform(&caps);

    /* Sharing: exclusive when one family does graphics + present (the
     * common case); concurrent with both families otherwise, since
     * command-buffer ownership transfer does not exist yet. */
    queue_families[0] = device->graphics_queue_family;
    queue_families[1] = surface->present_queue_family;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.surface = surface->vk_surface;
    info.minImageCount = image_count;
    info.imageFormat = format;
    info.imageColorSpace = color_space;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (queue_families[0] == queue_families[1]) {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        info.queueFamilyIndexCount = 0;
        info.pQueueFamilyIndices = NULL;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = queue_families;
    }
    info.preTransform = transform;
    info.compositeAlpha = alpha;
    info.presentMode = present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old_swapchain;

    if (vkCreateSwapchainKHR(device->device, &info, NULL, &new_handle) !=
        VK_SUCCESS) {
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }

    res = lc_vk_get_images(device->device, new_handle, &images, &image_count);
    if (res != LC_SUCCESS) {
        vkDestroySwapchainKHR(device->device, new_handle, NULL);
        return res;
    }
    /* Per-image frame tracking: fresh images start with no owner fence
     * and undefined contents. Built alongside the views so commit is
     * all-or-nothing. */
    in_flight = (VkFence *)calloc(image_count, sizeof(VkFence));
    initialized = (uint8_t *)calloc(image_count, sizeof(uint8_t));
    if (in_flight == NULL || initialized == NULL) {
        free(in_flight);
        free(initialized);
        vkDestroySwapchainKHR(device->device, new_handle, NULL);
        free(images);
        return LC_ERROR_OUT_OF_MEMORY;
    }
    res = lc_vk_create_views(device->device, format, images, image_count,
                             &views);
    if (res != LC_SUCCESS) {
        vkDestroySwapchainKHR(device->device, new_handle, NULL);
        free(images);
        free(in_flight);
        free(initialized);
        return res;
    }
    /* One present semaphore per image (see struct comment): submit
     * signals the acquired image's semaphore, presentation consumes
     * it asynchronously. */
    present_sems = (VkSemaphore *)calloc(image_count, sizeof(VkSemaphore));
    if (present_sems == NULL) {
        lc_vk_destroy_view_list(device->device, views, image_count);
        vkDestroySwapchainKHR(device->device, new_handle, NULL);
        free(images);
        free(in_flight);
        free(initialized);
        return LC_ERROR_OUT_OF_MEMORY;
    }
    {
        VkSemaphoreCreateInfo sem_info;
        uint32_t i;

        memset(&sem_info, 0, sizeof(sem_info));
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        for (i = 0; i < image_count; i++) {
            if (vkCreateSemaphore(device->device, &sem_info, NULL,
                                  &present_sems[i]) != VK_SUCCESS) {
                uint32_t j;
                for (j = 0; j < i; j++) {
                    vkDestroySemaphore(device->device, present_sems[j],
                                       NULL);
                }
                free(present_sems);
                lc_vk_destroy_view_list(device->device, views, image_count);
                vkDestroySwapchainKHR(device->device, new_handle, NULL);
                free(images);
                free(in_flight);
                free(initialized);
                return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
            }
        }
    }
    /* Frame objects persist across recreates; create them once. Only a
     * fresh swapchain reaches here with no pool, so failure unwinds a
     * fully new (never committed) state. */
    if (swapchain->cmd_pool == VK_NULL_HANDLE) {
        res = lc_vulkan_frame_init(swapchain);
        if (res != LC_SUCCESS) {
            uint32_t i;
            for (i = 0; i < image_count; i++) {
                vkDestroySemaphore(device->device, present_sems[i], NULL);
            }
            free(present_sems);
            lc_vk_destroy_view_list(device->device, views, image_count);
            vkDestroySwapchainKHR(device->device, new_handle, NULL);
            free(images);
            free(in_flight);
            free(initialized);
            return res;
        }
    }

    /* Fully built: retire the old state, then commit. From here on the
     * old swapchain, its views, its present semaphores, and its arrays
     * are gone. */
    lc_vulkan_swapchain_teardown(swapchain);
    swapchain->vk_swapchain = new_handle;
    swapchain->format = format;
    swapchain->color_space = color_space;
    swapchain->present_mode = present_mode;
    swapchain->extent = extent;
    swapchain->images = images;
    swapchain->image_views = views;
    swapchain->image_count = image_count;
    swapchain->images_in_flight = in_flight;
    swapchain->image_initialized = initialized;
    swapchain->present_semaphores = present_sems;
    swapchain->current_image = UINT32_MAX;
    return LC_SUCCESS;
}
