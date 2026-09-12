/*
 * Vulkan backend (Phase 5: device + surface + swapchain foundation).
 *
 * Creates a real VkInstance, picks a physical GPU, and creates a logical
 * VkDevice with one queue from EVERY queue family (see lc_vk_create_logical)
 * and VK_KHR_swapchain enabled. Surface creation lives in vulkan_surface.c;
 * swapchain creation lives in vulkan_swapchain.c. Deliberately absent:
 * command buffers, synchronization, render passes, pipelines, shaders,
 * buffers, textures, acquire/present. Those are later phases.
 *
 * GPU selection is deterministic: candidates must expose a graphics queue
 * family AND VK_KHR_swapchain support (a LumaC Vulkan device must be able
 * to present; window-independent creation is preserved - no window is
 * needed - but the GPU itself must be swapchain-capable), then rank by
 * (device type, max image dimension, API version), preferring discrete >
 * integrated > virtual > CPU > other. Integrated GPUs are fully accepted
 * when no discrete GPU exists.
 *
 * Destruction order: VkDevice -> debug messenger -> VkInstance.
 * (VkSwapchainKHR/VkImageView objects die earlier via swapchain.c hooks,
 * VkSurfaceKHR objects via surface.c hooks.)
 */

#if defined(_WIN32) || defined(_WIN64)
#define LC_VK_PLATFORM_SURFACE_EXT VK_KHR_WIN32_SURFACE_EXTENSION_NAME
#else
#define LC_VK_PLATFORM_SURFACE_EXT VK_KHR_XLIB_SURFACE_EXTENSION_NAME
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

#define LC_VK_VALIDATION_LAYER_NAME "VK_LAYER_KHRONOS_validation"

/* Negotiate a conservative instance version: Vulkan 1.2 when the loader
 * supports it (this phase needs only basic instance/device calls),
 * otherwise whatever the loader reports. vkEnumerateInstanceVersion is
 * resolved via vkGetInstanceProcAddr so old (< 1.1) loaders safely yield
 * 1.0 instead of a missing-import crash. */
static uint32_t lc_vk_negotiate_api_version(void) {
    PFN_vkEnumerateInstanceVersion pfn_enumerate_version =
        (PFN_vkEnumerateInstanceVersion)vkGetInstanceProcAddr(
            NULL, "vkEnumerateInstanceVersion");
    uint32_t loader_version = VK_API_VERSION_1_0;
    VkResult res;

    if (pfn_enumerate_version == NULL) {
        return VK_API_VERSION_1_0;
    }
    res = pfn_enumerate_version(&loader_version);
    if (res != VK_SUCCESS) {
        return VK_API_VERSION_1_0;
    }
    if (loader_version > VK_API_VERSION_1_2) {
        return VK_API_VERSION_1_2;
    }
    return loader_version;
}

static int lc_vk_has_validation_layer(void) {
    uint32_t count = 0;
    uint32_t i;
    VkLayerProperties *layers = NULL;
    int found = 0;

    if (vkEnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS) {
        return 0;
    }
    if (count == 0) {
        return 0;
    }
    layers = (VkLayerProperties *)malloc(sizeof(VkLayerProperties) * count);
    if (layers == NULL) {
        return 0;
    }
    if (vkEnumerateInstanceLayerProperties(&count, layers) != VK_SUCCESS) {
        free(layers);
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(layers[i].layerName, LC_VK_VALIDATION_LAYER_NAME) == 0) {
            found = 1;
            break;
        }
    }
    free(layers);
    return found;
}

static int lc_vk_has_instance_extension(const char *name) {
    uint32_t count = 0;
    uint32_t i;
    VkExtensionProperties *exts = NULL;
    int found = 0;

    if (vkEnumerateInstanceExtensionProperties(NULL, &count, NULL) != VK_SUCCESS) {
        return 0;
    }
    if (count == 0) {
        return 0;
    }
    exts = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) * count);
    if (exts == NULL) {
        return 0;
    }
    if (vkEnumerateInstanceExtensionProperties(NULL, &count, exts) != VK_SUCCESS) {
        free(exts);
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(exts[i].extensionName, name) == 0) {
            found = 1;
            break;
        }
    }
    free(exts);
    return found;
}

/* Check one device extension on a physical device. Never requested
 * blindly: VK_KHR_swapchain is verified here before any device asks
 * for it. */
static int lc_vk_has_device_extension(VkPhysicalDevice physical,
                                      const char *name) {
    uint32_t count = 0;
    uint32_t i;
    VkExtensionProperties *exts = NULL;
    int found = 0;

    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) !=
        VK_SUCCESS) {
        return 0;
    }
    if (count == 0) {
        return 0;
    }
    exts = (VkExtensionProperties *)malloc(sizeof(VkExtensionProperties) *
                                           count);
    if (exts == NULL) {
        return 0;
    }
    if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, exts) !=
        VK_SUCCESS) {
        free(exts);
        return 0;
    }
    for (i = 0; i < count; i++) {
        if (strcmp(exts[i].extensionName, name) == 0) {
            found = 1;
            break;
        }
    }
    free(exts);
    return found;
}

/* Concise validation callback: warnings/errors to stderr only, never aborts. */
static VKAPI_ATTR VkBool32 VKAPI_CALL lc_vk_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *callback_data,
    void *user_data) {
    (void)types;
    (void)user_data;
    if (callback_data != NULL && callback_data->pMessage != NULL &&
        severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        const char *level =
            (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
                ? "error"
                : "warning";
        fprintf(stderr, "[lumac][vulkan][%s] %s\n", level,
                callback_data->pMessage);
    }
    return VK_FALSE;
}

static lc_result lc_vk_create_messenger(lc_device *device) {
    PFN_vkCreateDebugUtilsMessengerEXT pfn_create =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            device->instance, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerCreateInfoEXT info;

    if (pfn_create == NULL) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    info.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType =
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = lc_vk_debug_callback;

    if (pfn_create(device->instance, &info, NULL,
                   &device->debug_messenger) != VK_SUCCESS) {
        device->debug_messenger = VK_NULL_HANDLE;
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    device->pfn_destroy_messenger =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
            device->instance, "vkDestroyDebugUtilsMessengerEXT");
    return LC_SUCCESS;
}

static lc_result lc_vk_create_instance(lc_device *device,
                                       int use_validation,
                                       uint32_t api_version) {
    VkApplicationInfo app_info;
    VkInstanceCreateInfo create_info;
    const char *layers[1];
    const char *extensions[3];
    uint32_t extension_count = 0;

    memset(&app_info, 0, sizeof(app_info));
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "LumaC Application";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = "LumaC";
    app_info.engineVersion = VK_MAKE_VERSION(LC_VERSION_MAJOR,
                                             LC_VERSION_MINOR,
                                             LC_VERSION_PATCH);
    app_info.apiVersion = api_version;

    /* Surface extensions are always enabled: the instance belongs to the
     * device, and a device created now must be able to host a surface
     * later without recreating its instance. Enabling them does NOT
     * require a window to exist, and no surface is created here.
     * Only the current platform's extension is requested, and only
     * after verifying availability (never blindly). */
    if (!lc_vk_has_instance_extension(VK_KHR_SURFACE_EXTENSION_NAME) ||
        !lc_vk_has_instance_extension(LC_VK_PLATFORM_SURFACE_EXT)) {
        return LC_ERROR_SURFACE_UNSUPPORTED;
    }
    extensions[extension_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
    extensions[extension_count++] = LC_VK_PLATFORM_SURFACE_EXT;

    layers[0] = LC_VK_VALIDATION_LAYER_NAME;
    if (use_validation) {
        extensions[extension_count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    }

    memset(&create_info, 0, sizeof(create_info));
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = extension_count;
    create_info.ppEnabledExtensionNames = extensions;
    if (use_validation) {
        create_info.enabledLayerCount = 1;
        create_info.ppEnabledLayerNames = layers;
    }

    if (vkCreateInstance(&create_info, NULL, &device->instance) != VK_SUCCESS) {
        device->instance = VK_NULL_HANDLE;
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    return LC_SUCCESS;
}

static int lc_vk_type_rank(VkPhysicalDeviceType type) {
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return 4;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
        return 3;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
        return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
        return 1;
    default:
        return 0;
    }
}

/* First queue family with graphics support, or UINT32_MAX if none. */
static uint32_t lc_vk_find_graphics_family(VkPhysicalDevice physical) {
    uint32_t count = 0;
    uint32_t i;
    VkQueueFamilyProperties *families = NULL;
    uint32_t found = UINT32_MAX;

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, NULL);
    if (count == 0) {
        return UINT32_MAX;
    }
    families =
        (VkQueueFamilyProperties *)malloc(sizeof(VkQueueFamilyProperties) * count);
    if (families == NULL) {
        return UINT32_MAX;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families);
    for (i = 0; i < count; i++) {
        if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0) {
            found = i;
            break;
        }
    }
    free(families);
    return found;
}

/* Deterministic pick: requires a graphics queue; ranks by device type,
 * then max image dimension, then device API version. Ties keep the
 * lowest enumeration index. */
static lc_result lc_vk_pick_physical(VkInstance instance,
                                     VkPhysicalDevice *out_physical,
                                     VkPhysicalDeviceProperties *out_props,
                                     uint32_t *out_family) {
    uint32_t count = 0;
    uint32_t i;
    VkPhysicalDevice *list = NULL;
    int have_best = 0;
    int best_rank = -1;
    uint32_t best_dim = 0;
    uint32_t best_api = 0;
    VkPhysicalDevice best_physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties best_props;
    uint32_t best_family = UINT32_MAX;

    memset(&best_props, 0, sizeof(best_props));

    if (vkEnumeratePhysicalDevices(instance, &count, NULL) != VK_SUCCESS ||
        count == 0) {
        return LC_ERROR_NO_SUPPORTED_DEVICE;
    }
    list = (VkPhysicalDevice *)malloc(sizeof(VkPhysicalDevice) * count);
    if (list == NULL) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    if (vkEnumeratePhysicalDevices(instance, &count, list) != VK_SUCCESS) {
        free(list);
        return LC_ERROR_NO_SUPPORTED_DEVICE;
    }

    for (i = 0; i < count; i++) {
        VkPhysicalDeviceProperties props;
        uint32_t family;
        int rank;

        vkGetPhysicalDeviceProperties(list[i], &props);
        family = lc_vk_find_graphics_family(list[i]);
        if (family == UINT32_MAX) {
            continue; /* no graphics queue: unsuitable */
        }
        /* Swapchain-capable GPUs only: a LumaC Vulkan device must be
         * able to present, so VK_KHR_swapchain is verified (never
         * blindly requested) before the device can select this GPU. */
        if (!lc_vk_has_device_extension(list[i],
                                        VK_KHR_SWAPCHAIN_EXTENSION_NAME)) {
            continue;
        }
        rank = lc_vk_type_rank(props.deviceType);
        if (!have_best || rank > best_rank ||
            (rank == best_rank &&
             (props.limits.maxImageDimension2D > best_dim ||
              (props.limits.maxImageDimension2D == best_dim &&
               props.apiVersion > best_api)))) {
            have_best = 1;
            best_rank = rank;
            best_dim = props.limits.maxImageDimension2D;
            best_api = props.apiVersion;
            best_physical = list[i];
            best_props = props;
            best_family = family;
        }
    }
    free(list);

    if (!have_best) {
        return LC_ERROR_NO_SUPPORTED_DEVICE;
    }
    *out_physical = best_physical;
    *out_props = best_props;
    *out_family = best_family;
    return LC_SUCCESS;
}

/* Queue strategy (Approach C): presentation support cannot be known at
 * device creation time because no surface exists yet, and recreating the
 * logical device when a surface appears is undesirable. So request one
 * queue from EVERY queue family exposed by the selected GPU. The family
 * count is small (typically a handful), each family reports at least one
 * queue so queueCount=1 never exceeds it, and a single shared priority
 * is valid since every entry requests exactly one queue. Once the device
 * exists, any present-capable family discovered later already has a
 * retrieved queue - no recreation needed. The sole device extension is
 * VK_KHR_swapchain (verified during selection); no optional features,
 * so the device works on broad hardware. */
static lc_result lc_vk_create_logical(lc_device *device,
                                      VkPhysicalDevice physical,
                                      uint32_t graphics_family) {
    float priority = 1.0f;
    uint32_t family_count = 0;
    uint32_t i;
    VkDeviceQueueCreateInfo *queue_infos = NULL;
    VkDeviceCreateInfo create_info;
    VkPhysicalDeviceFeatures enabled_features;
    const char *device_extensions[1];

    vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, NULL);
    if (family_count == 0) {
        return LC_ERROR_DEVICE_CREATION_FAILED;
    }
    queue_infos = (VkDeviceQueueCreateInfo *)malloc(
        sizeof(VkDeviceQueueCreateInfo) * family_count);
    if (queue_infos == NULL) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < family_count; i++) {
        memset(&queue_infos[i], 0, sizeof(queue_infos[i]));
        queue_infos[i].sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_infos[i].queueFamilyIndex = i;
        queue_infos[i].queueCount = 1;
        queue_infos[i].pQueuePriorities = &priority;
    }

    /* VK_KHR_swapchain only: verified present on this physical device
     * during selection, so requesting it here cannot fail for missing
     * support. */
    device_extensions[0] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

    /* Deliberate minimal features: sampler anisotropy is enabled when
     * supported so samplers can offer it; everything else stays off.
     * Support is recorded on the device for sampler validation and
     * capability queries. */
    memset(&enabled_features, 0, sizeof(enabled_features));
    {
        VkPhysicalDeviceFeatures supported;

        memset(&supported, 0, sizeof(supported));
        vkGetPhysicalDeviceFeatures(physical, &supported);
        if (supported.samplerAnisotropy != VK_FALSE) {
            enabled_features.samplerAnisotropy = VK_TRUE;
            device->anisotropy_supported = 1;
        } else {
            device->anisotropy_supported = 0;
        }
    }

    memset(&create_info, 0, sizeof(create_info));
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = family_count;
    create_info.pQueueCreateInfos = queue_infos;
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = device_extensions;
    create_info.pEnabledFeatures = &enabled_features;

    if (vkCreateDevice(physical, &create_info, NULL, &device->device) !=
        VK_SUCCESS) {
        free(queue_infos);
        device->device = VK_NULL_HANDLE;
        return LC_ERROR_DEVICE_CREATION_FAILED;
    }
    free(queue_infos);

    device->queues = (lc_vk_queue *)malloc(sizeof(lc_vk_queue) * family_count);
    if (device->queues == NULL) {
        vkDestroyDevice(device->device, NULL);
        device->device = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    device->queue_count = family_count;
    for (i = 0; i < family_count; i++) {
        device->queues[i].family_index = i;
        device->queues[i].queue = VK_NULL_HANDLE;
        vkGetDeviceQueue(device->device, i, 0, &device->queues[i].queue);
        if (device->queues[i].queue == VK_NULL_HANDLE) {
            free(device->queues);
            device->queues = NULL;
            device->queue_count = 0;
            vkDestroyDevice(device->device, NULL);
            device->device = VK_NULL_HANDLE;
            return LC_ERROR_DEVICE_CREATION_FAILED;
        }
    }

    device->physical_device = physical;
    device->graphics_queue_family = graphics_family;
    device->graphics_queue =
        lc_vulkan_get_queue(device, graphics_family);
    if (device->graphics_queue == VK_NULL_HANDLE) {
        lc_vulkan_device_destroy(device);
        return LC_ERROR_DEVICE_CREATION_FAILED;
    }
    return LC_SUCCESS;
}

VkQueue lc_vulkan_get_queue(const lc_device *device, uint32_t family_index) {
    uint32_t i;

    if (device == NULL || device->queues == NULL) {
        return VK_NULL_HANDLE;
    }
    for (i = 0; i < device->queue_count; i++) {
        if (device->queues[i].family_index == family_index) {
            return device->queues[i].queue;
        }
    }
    return VK_NULL_HANDLE;
}

static void lc_vk_store_properties(lc_device *device,
                                   const VkPhysicalDeviceProperties *props) {
    size_t i = 0;
    /* Manual copy to stay warning-clean on MSVC (/W3 flags strncpy). */
    while (i < LC_DEVICE_NAME_SIZE - 1 && props->deviceName[i] != '\0') {
        device->name[i] = props->deviceName[i];
        i++;
    }
    device->name[i] = '\0';
    device->vendor_id = props->vendorID;
    device->device_id = props->deviceID;
}

lc_result lc_vulkan_device_create(lc_device *device, int enable_validation) {
    int use_validation = 0;
    uint32_t api_version;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props;
    uint32_t family = UINT32_MAX;
    lc_result res;

    if (device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&props, 0, sizeof(props));

    if (enable_validation != 0) {
        if (lc_vk_has_validation_layer() &&
            lc_vk_has_instance_extension(
                VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            use_validation = 1;
        } else {
            /* Best-effort diagnostics: a single stderr line, not a
             * logging framework. Creation continues without validation. */
            fprintf(stderr,
                    "[lumac] validation requested but "
                    "VK_LAYER_KHRONOS_validation is unavailable; "
                    "continuing without validation\n");
        }
    }

    api_version = lc_vk_negotiate_api_version();

    res = lc_vk_create_instance(device, use_validation, api_version);
    if (res != LC_SUCCESS) {
        goto fail;
    }
    if (use_validation) {
        res = lc_vk_create_messenger(device);
        if (res != LC_SUCCESS) {
            goto fail;
        }
    }
    res = lc_vk_pick_physical(device->instance, &physical, &props, &family);
    if (res != LC_SUCCESS) {
        goto fail;
    }
    res = lc_vk_create_logical(device, physical, family);
    if (res != LC_SUCCESS) {
        goto fail;
    }

    lc_vk_store_properties(device, &props);
    return LC_SUCCESS;

fail:
    /* Unwinds VkDevice -> messenger -> VkInstance via NULL-handle checks. */
    lc_vulkan_device_destroy(device);
    return res;
}

void lc_vulkan_device_destroy(lc_device *device) {
    if (device == NULL) {
        return;
    }
    /* Upload context first: its pool/fence die before VkDevice.
     * Tracked buffers are already gone (device-destroy hook). */
    lc_vulkan_upload_teardown(device);
    if (device->device != VK_NULL_HANDLE) {
        vkDestroyDevice(device->device, NULL);
        device->device = VK_NULL_HANDLE;
        device->graphics_queue = VK_NULL_HANDLE;
    }
    if (device->queues != NULL) {
        free(device->queues);
        device->queues = NULL;
        device->queue_count = 0;
    }
    if (device->debug_messenger != VK_NULL_HANDLE) {
        if (device->pfn_destroy_messenger != NULL &&
            device->instance != VK_NULL_HANDLE) {
            device->pfn_destroy_messenger(device->instance,
                                          device->debug_messenger, NULL);
        }
        device->debug_messenger = VK_NULL_HANDLE;
        device->pfn_destroy_messenger = NULL;
    }
    if (device->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(device->instance, NULL);
        device->instance = VK_NULL_HANDLE;
    }
    device->physical_device = VK_NULL_HANDLE;
}
