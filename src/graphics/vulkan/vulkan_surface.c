/*
 * Vulkan surface backend (Phase 4: presentation foundation, no swapchain).
 *
 * Creates a real VkSurfaceKHR for one lc_window using the lc_device's
 * VkInstance, then resolves which queue family can present to it and
 * verifies the surface is queryable (capabilities, format count,
 * present-mode count). No format/mode is selected and nothing is stored
 * for swapchain use - that negotiation belongs to Phase 5.
 *
 * Present family choice: the graphics family when it supports present,
 * otherwise the first present-capable family. The queue itself always
 * exists: device creation retrieves one queue per family (Approach C),
 * so no logical-device recreation is ever needed here.
 */

#include <string.h>

#include "platform/platform.h" /* native handles; windows.h before vulkan.h */
#include "graphics/graphics_internal.h"

static lc_result lc_vk_create_native_surface(lc_device *device,
                                             const lc_native_window_handle *native,
                                             VkSurfaceKHR *out_surface) {
#if defined(_WIN32) || defined(_WIN64)
    VkWin32SurfaceCreateInfoKHR info;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    info.hinstance = native->hinstance;
    info.hwnd = native->hwnd;

    if (vkCreateWin32SurfaceKHR(device->instance, &info, NULL, out_surface) !=
        VK_SUCCESS) {
        *out_surface = VK_NULL_HANDLE;
        return LC_ERROR_SURFACE_UNSUPPORTED;
    }
    return LC_SUCCESS;
#else
    VkXlibSurfaceCreateInfoKHR info;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    info.dpy = (Display *)native->display;
    info.window = (Window)native->window;

    if (vkCreateXlibSurfaceKHR(device->instance, &info, NULL, out_surface) !=
        VK_SUCCESS) {
        *out_surface = VK_NULL_HANDLE;
        return LC_ERROR_SURFACE_UNSUPPORTED;
    }
    return LC_SUCCESS;
#endif
}

/* Resolve a present-capable family for the surface. Prefers the graphics
 * family when it can present; never assumes graphics == present. Fails
 * when no family supports presentation. */
static lc_result lc_vk_find_present_family(VkPhysicalDevice physical,
                                           VkSurfaceKHR surface,
                                           uint32_t graphics_family,
                                           uint32_t *out_family) {
    uint32_t count = 0;
    uint32_t i;
    uint32_t first_present = UINT32_MAX;

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (count == 0) {
        return LC_ERROR_SURFACE_UNSUPPORTED;
    }
    for (i = 0; i < count; i++) {
        VkBool32 supported = VK_FALSE;
        if (vkGetPhysicalDeviceSurfaceSupportKHR(physical, i, surface,
                                                &supported) != VK_SUCCESS) {
            return LC_ERROR_SURFACE_UNSUPPORTED;
        }
        if (supported != VK_FALSE) {
            if (i == graphics_family) {
                *out_family = i;
                return LC_SUCCESS;
            }
            if (first_present == UINT32_MAX) {
                first_present = i;
            }
        }
    }
    if (first_present == UINT32_MAX) {
        return LC_ERROR_SURFACE_UNSUPPORTED;
    }
    *out_family = first_present;
    return LC_SUCCESS;
}

/* Confirm the surface is genuinely queryable. Counts only - selection
 * and storage belong to the future swapchain phase. */
static lc_result lc_vk_probe_surface(VkPhysicalDevice physical,
                                     VkSurfaceKHR surface) {
    VkSurfaceCapabilitiesKHR caps;
    uint32_t format_count = 0;
    uint32_t mode_count = 0;

    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &caps) !=
        VK_SUCCESS) {
        return LC_ERROR_SURFACE_UNSUPPORTED;
    }
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &format_count,
                                             NULL) != VK_SUCCESS) {
        return LC_ERROR_SURFACE_UNSUPPORTED;
    }
    if (vkGetPhysicalDeviceSurfacePresentModesKHR(physical, surface,
                                                  &mode_count,
                                                  NULL) != VK_SUCCESS) {
        return LC_ERROR_SURFACE_UNSUPPORTED;
    }
    return LC_SUCCESS;
}

lc_result lc_vulkan_surface_create(lc_surface *surface, lc_device *device,
                                   lc_window *window) {
    lc_native_window_handle native;
    lc_result res;
    VkSurfaceKHR vk_surface = VK_NULL_HANDLE;
    uint32_t present_family = UINT32_MAX;
    VkQueue present_queue = VK_NULL_HANDLE;

    if (surface == NULL || device == NULL || window == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Guard against partially-created devices (instance/physical/logical
     * handles are all required below). */
    if (device->instance == VK_NULL_HANDLE ||
        device->physical_device == VK_NULL_HANDLE ||
        device->device == VK_NULL_HANDLE) {
        return LC_ERROR_SURFACE_UNSUPPORTED;
    }

    res = lc_platform_get_native_handle(window, &native);
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vk_create_native_surface(device, &native, &vk_surface);
    if (res != LC_SUCCESS) {
        goto fail;
    }
    res = lc_vk_find_present_family(device->physical_device, vk_surface,
                                    device->graphics_queue_family,
                                    &present_family);
    if (res != LC_SUCCESS) {
        goto fail;
    }
    present_queue = lc_vulkan_get_queue(device, present_family);
    if (present_queue == VK_NULL_HANDLE) {
        res = LC_ERROR_SURFACE_UNSUPPORTED;
        goto fail;
    }
    res = lc_vk_probe_surface(device->physical_device, vk_surface);
    if (res != LC_SUCCESS) {
        goto fail;
    }

    surface->device = device;
    surface->window = window;
    surface->vk_surface = vk_surface;
    surface->present_queue_family = present_family;
    surface->present_queue = present_queue;
    surface->present_supported = 1;
    return LC_SUCCESS;

fail:
    if (vk_surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(device->instance, vk_surface, NULL);
    }
    return res;
}

void lc_vulkan_surface_destroy(lc_surface *surface) {
    if (surface == NULL || surface->vk_surface == VK_NULL_HANDLE) {
        return;
    }
    /* Callers guarantee device + window are still alive (dependency-
     * ordered teardown in surface.c), so the instance is valid here. */
    if (surface->device != NULL &&
        surface->device->instance != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(surface->device->instance, surface->vk_surface,
                            NULL);
    }
    surface->vk_surface = VK_NULL_HANDLE;
    surface->present_queue = VK_NULL_HANDLE;
    surface->present_supported = 0;
}
