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
    /* swapchain always; timeline KHR when the driver offers it
     * (core 1.2 path needs no extension — enabling is harmless and
     * version-gated below). */
    const char *device_extensions[2];
    uint32_t extension_count = 1;
    int want_timeline_khr = 0;
    VkPhysicalDeviceTimelineSemaphoreFeaturesKHR timeline_features;
    VkPhysicalDeviceProperties props;

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

    /* Timeline semaphores (Phase 20, PART R): KHR extension when
     * offered (works on any loader version); the 1.2-core entry
     * points are used unconditionally below guarded by support. */
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(physical, &props);
    {
        /* Query the actual feature bit (never enable blindly:
         * unsupported TRUE fails device creation). features2 is
         * core in 1.1; resolve dynamically for 1.0 loaders. */
        PFN_vkGetPhysicalDeviceFeatures2 pfn_features2 =
            (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(
                device->instance, "vkGetPhysicalDeviceFeatures2");
        VkPhysicalDeviceTimelineSemaphoreFeaturesKHR query;
        int have_ext = lc_vk_has_device_extension(
            physical, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);

        memset(&query, 0, sizeof(query));
        if (pfn_features2 != NULL &&
            (props.apiVersion >= VK_API_VERSION_1_2 || have_ext)) {
            VkPhysicalDeviceFeatures2 features2;

            query.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;
            memset(&features2, 0, sizeof(features2));
            features2.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &query;
            pfn_features2(physical, &features2);
            if (query.timelineSemaphore == VK_TRUE) {
                /* KHR path only needs the extension on < 1.2. */
                if (props.apiVersion < VK_API_VERSION_1_2) {
                    device_extensions[extension_count++] =
                        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
                }
                want_timeline_khr = 1;
            }
        }
    }
    /* Forced binary fallback (Phase 22 test path): never even ask
     * for timeline semaphores. */
    if (device->force_binary_fallback) {
        want_timeline_khr = 0;
    }

    /* Deliberate minimal features: sampler anisotropy is enabled when
     * supported so samplers can offer it; multi-draw indirect is
     * enabled when supported (Phase 21 native path; harmless until
     * used); everything else stays off. Support is recorded on the
     * device for sampler validation and capability queries. */
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
        if (supported.multiDrawIndirect != VK_FALSE) {
            enabled_features.multiDrawIndirect = VK_TRUE;
        }
    }

    memset(&create_info, 0, sizeof(create_info));
    create_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_info.queueCreateInfoCount = family_count;
    create_info.pQueueCreateInfos = queue_infos;
    create_info.enabledExtensionCount = extension_count;
    create_info.ppEnabledExtensionNames = device_extensions;
    create_info.pEnabledFeatures = &enabled_features;
    /* drawIndirectCount (Phase 21 capability reporting, Phase 23
     * native count path): enable through the 1.2 feature struct
     * when the loader offers it. The timeline feature folds into
     * the SAME struct on 1.2 (the KHR struct must not co-exist
     * with it in one pNext chain); pre-1.2 keeps the KHR struct
     * alone. Never enabled blindly. */
    {
        PFN_vkGetPhysicalDeviceFeatures2 pfn12 =
            (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(
                device->instance, "vkGetPhysicalDeviceFeatures2");

        memset(&device->vulkan12_enabled, 0,
               sizeof(device->vulkan12_enabled));
        if (pfn12 != NULL &&
            props.apiVersion >= VK_API_VERSION_1_2) {
            VkPhysicalDeviceVulkan12Features query;
            VkPhysicalDeviceFeatures2 features2;

            memset(&query, 0, sizeof(query));
            query.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            memset(&features2, 0, sizeof(features2));
            features2.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
            features2.pNext = &query;
            pfn12(physical, &features2);
            device->vulkan12_enabled.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
            if (query.drawIndirectCount != VK_FALSE) {
                device->vulkan12_enabled.drawIndirectCount = VK_TRUE;
            }
            if (want_timeline_khr &&
                query.timelineSemaphore != VK_FALSE) {
                device->vulkan12_enabled.timelineSemaphore = VK_TRUE;
            } else if (want_timeline_khr) {
                want_timeline_khr = 0;
            }
        }
    }
    if (device->vulkan12_enabled.drawIndirectCount != VK_FALSE ||
        device->vulkan12_enabled.timelineSemaphore != VK_FALSE) {
        device->vulkan12_enabled.pNext = (void *)create_info.pNext;
        create_info.pNext = &device->vulkan12_enabled;
    }
    if (want_timeline_khr &&
        device->vulkan12_enabled.timelineSemaphore == VK_FALSE) {
        memset(&timeline_features, 0, sizeof(timeline_features));
        timeline_features.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES_KHR;
        timeline_features.timelineSemaphore = VK_TRUE;
        timeline_features.pNext = (void *)create_info.pNext;
        create_info.pNext = &timeline_features;
    }

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
    /* Transfer queue (Phase 20, PART O): prefer TRANSFER-only, then
     * TRANSFER-without-graphics, then any TRANSFER-capable family;
     * fall back to the graphics family (identical behavior). */
    {
        VkQueueFamilyProperties *families = NULL;
        uint32_t count = 0;
        uint32_t best = graphics_family;
        int best_rank = -1;
        uint32_t i;

        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count,
                                                 NULL);
        if (count > 0) {
            families = (VkQueueFamilyProperties *)malloc(
                sizeof(VkQueueFamilyProperties) * count);
        }
        if (families != NULL) {
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count,
                                                     families);
            for (i = 0; i < count; i++) {
                VkQueueFlags flags = families[i].queueFlags;
                int rank = -1;

                if ((flags & VK_QUEUE_TRANSFER_BIT) == 0) {
                    continue;
                }
                if ((flags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
                    (flags & VK_QUEUE_COMPUTE_BIT) == 0) {
                    rank = 3; /* TRANSFER-only: best */
                } else if ((flags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                    rank = 2;
                } else if (i != graphics_family) {
                    rank = 1;
                } else {
                    rank = 0;
                }
                if (rank > best_rank) {
                    best_rank = rank;
                    best = i;
                }
            }
            free(families);
        }
        device->transfer_queue_family = (uint32_t)best;
        device->transfer_queue =
            lc_vulkan_get_queue(device, (uint32_t)best);
        if (device->transfer_queue == VK_NULL_HANDLE) {
            device->transfer_queue_family = graphics_family;
            device->transfer_queue = device->graphics_queue;
        }
        device->has_dedicated_transfer =
            (device->transfer_queue_family != graphics_family) ? 1 : 0;
        /* Forced graphics transfer (Phase 22 test path). */
        if (device->force_graphics_transfer) {
            device->transfer_queue_family = graphics_family;
            device->transfer_queue = device->graphics_queue;
            device->has_dedicated_transfer = 0;
        }
    }
    /* Compute queue (Phase 21, PART G): prefer a COMPUTE-only
     * family, then COMPUTE-without-graphics, then any
     * COMPUTE-capable family; fall back to aliasing graphics.
     * Production dispatch records into frame/worker buffers (no
     * scheduler yet); the separate handle proves cross-queue
     * dispatch in isolation and reserves the async-compute shape. */
    {
        VkQueueFamilyProperties *families = NULL;
        uint32_t count = 0;
        uint32_t best = graphics_family;
        int best_rank = -1;
        uint32_t i;

        vkGetPhysicalDeviceQueueFamilyProperties(physical, &count,
                                                 NULL);
        if (count > 0) {
            families = (VkQueueFamilyProperties *)malloc(
                sizeof(VkQueueFamilyProperties) * count);
        }
        if (families != NULL) {
            vkGetPhysicalDeviceQueueFamilyProperties(physical, &count,
                                                     families);
            for (i = 0; i < count; i++) {
                VkQueueFlags flags = families[i].queueFlags;
                int rank = -1;

                if ((flags & VK_QUEUE_COMPUTE_BIT) == 0) {
                    continue;
                }
                if ((flags & VK_QUEUE_GRAPHICS_BIT) == 0 &&
                    (flags & VK_QUEUE_TRANSFER_BIT) == 0) {
                    rank = 3; /* COMPUTE-only: best */
                } else if ((flags & VK_QUEUE_GRAPHICS_BIT) == 0) {
                    rank = 2;
                } else if (i != graphics_family) {
                    rank = 1;
                } else {
                    rank = 0;
                }
                if (rank > best_rank) {
                    best_rank = rank;
                    best = i;
                }
            }
            free(families);
        }
        device->compute_queue_family = (uint32_t)best;
        device->compute_queue =
            lc_vulkan_get_queue(device, (uint32_t)best);
        if (device->compute_queue == VK_NULL_HANDLE) {
            device->compute_queue_family = graphics_family;
            device->compute_queue = device->graphics_queue;
        }
        device->has_dedicated_compute =
            (device->compute_queue_family != graphics_family) ? 1 : 0;
        /* Forced graphics compute (Phase 22 test path). */
        if (device->force_graphics_compute) {
            device->compute_queue_family = graphics_family;
            device->compute_queue = device->graphics_queue;
            device->has_dedicated_compute = 0;
        }
    }
    /* Compute/indirect properties (Phase 21, PARTs AQ/AL): queried
     * once; zeros when compute is unsupported (no compute queue
     * family found at all is treated as unsupported). The 1.1 entry
     * points resolve dynamically so 1.0 loaders keep working (1.2
     * features then read as unsupported). */
    {
        VkPhysicalDeviceProperties props;
        VkPhysicalDeviceFeatures feats;
        PFN_vkGetPhysicalDeviceFeatures2 pfn_features2 = NULL;
        PFN_vkGetPhysicalDeviceProperties2 pfn_props2 = NULL;

        memset(&props, 0, sizeof(props));
        vkGetPhysicalDeviceProperties(physical, &props);
        memset(&feats, 0, sizeof(feats));
        vkGetPhysicalDeviceFeatures(physical, &feats);
        if (device->instance != VK_NULL_HANDLE) {
            pfn_features2 =
                (PFN_vkGetPhysicalDeviceFeatures2)vkGetInstanceProcAddr(
                    device->instance, "vkGetPhysicalDeviceFeatures2");
            pfn_props2 =
                (PFN_vkGetPhysicalDeviceProperties2)vkGetInstanceProcAddr(
                    device->instance,
                    "vkGetPhysicalDeviceProperties2");
        }
        device->compute_supported =
            (device->compute_queue != VK_NULL_HANDLE) ? 1 : 0;
        device->indirect_draw_supported = device->compute_supported;
        device->multi_draw_indirect =
            (feats.multiDrawIndirect != 0) ? 1 : 0;
        /* Honest reporting: usable means ENABLED on this device,
         * not merely offered by the hardware (the enable record
         * above is the source of truth; pre-1.2 keeps fallback). */
        device->indirect_count_supported =
            (device->vulkan12_enabled.drawIndirectCount != 0) ? 1
                                                              : 0;
        device->max_workgroup_size[0] =
            props.limits.maxComputeWorkGroupSize[0];
        device->max_workgroup_size[1] =
            props.limits.maxComputeWorkGroupSize[1];
        device->max_workgroup_size[2] =
            props.limits.maxComputeWorkGroupSize[2];
        device->max_workgroup_count[0] =
            props.limits.maxComputeWorkGroupCount[0];
        device->max_workgroup_count[1] =
            props.limits.maxComputeWorkGroupCount[1];
        device->max_workgroup_count[2] =
            props.limits.maxComputeWorkGroupCount[2];
        device->max_workgroup_invocations =
            props.limits.maxComputeWorkGroupInvocations;
        device->max_compute_shared_memory =
            props.limits.maxComputeSharedMemorySize;
        device->max_compute_push_size =
            props.limits.maxPushConstantsSize;
        device->subgroup_size = 0;
        if (pfn_props2 != NULL) {
            VkPhysicalDeviceSubgroupProperties subgroup;
            VkPhysicalDeviceProperties2 props2;

            memset(&subgroup, 0, sizeof(subgroup));
            subgroup.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
            memset(&props2, 0, sizeof(props2));
            props2.sType =
                VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
            props2.pNext = &subgroup;
            pfn_props2(physical, &props2);
            if ((subgroup.supportedOperations &
                 VK_SUBGROUP_FEATURE_BASIC_BIT) != 0) {
                device->subgroup_size = subgroup.subgroupSize;
            }
        }
    }
    /* Timeline semaphore (Phase 20, PARTs Q–R): shared by graphics
     * and transfer submissions (one monotonic value domain). Falls
     * back to binary fences per request when unsupported. */
    device->timeline = VK_NULL_HANDLE;
    device->timeline_next = 1;
    device->timeline_ok = 0;
    if (want_timeline_khr) {
        VkSemaphoreTypeCreateInfo type_info;
        VkSemaphoreCreateInfo sem_info;

        memset(&type_info, 0, sizeof(type_info));
        type_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
        type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type_info.initialValue = 0;
        memset(&sem_info, 0, sizeof(sem_info));
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        sem_info.pNext = &type_info;
        if (vkCreateSemaphore(device->device, &sem_info, NULL,
                               &device->timeline) == VK_SUCCESS) {
            device->timeline_ok = 1;
            /* Core-1.2 entry points may not exist on 1.0 loaders;
             * resolve dynamically (NULL = fallback mode). The
             * signal pointer is required too: Phase 22 host-signals
             * reserved-but-unsubmitted values on submit failure so
             * no waiter can stall on a timeline gap. */
            device->pfn_sem_counter =
                (PFN_vkGetSemaphoreCounterValue)vkGetDeviceProcAddr(
                    device->device, "vkGetSemaphoreCounterValue");
            device->pfn_sem_wait =
                (PFN_vkWaitSemaphores)vkGetDeviceProcAddr(
                    device->device, "vkWaitSemaphores");
            device->pfn_sem_signal =
                (PFN_vkSignalSemaphore)vkGetDeviceProcAddr(
                    device->device, "vkSignalSemaphore");
            if (device->pfn_sem_counter == NULL ||
                device->pfn_sem_wait == NULL ||
                device->pfn_sem_signal == NULL) {
                vkDestroySemaphore(device->device, device->timeline,
                                   NULL);
                device->timeline = VK_NULL_HANDLE;
                device->timeline_ok = 0;
                device->pfn_sem_counter = NULL;
                device->pfn_sem_wait = NULL;
                device->pfn_sem_signal = NULL;
            }
        } else {
            device->timeline = VK_NULL_HANDLE;
        }
    }
    /* Phase 20 locks: transfer scheduler, resource-state tracking,
     * pipeline/pass caches, descriptor allocator. Best effort each;
     * unlocked operation still honors the single-threaded contract
     * (documented). The transfer shard is recursive: schedulers nest
     * immediate submits inside transfer-locked sections. */
#if defined(_WIN32) || defined(_WIN64)
    device->transfer_mutex_init = 0;
    /* CRITICAL_SECTION is already reentrant. */
    if (InitializeCriticalSectionAndSpinCount(&device->transfer_mutex,
                                              4000) != 0) {
        device->transfer_mutex_init = 1;
    }
#else
        pthread_mutexattr_t attr;

        if (pthread_mutexattr_init(&attr) == 0) {
            if (pthread_mutexattr_settype(&attr,
                                          PTHREAD_MUTEX_RECURSIVE) == 0 &&
                pthread_mutex_init(&device->transfer_mutex, &attr) ==
                    0) {
                device->transfer_mutex_init = 1;
            }
            pthread_mutexattr_destroy(&attr);
        }
#endif
#if defined(_WIN32) || defined(_WIN64)
    device->state_mutex_init =
        (InitializeCriticalSectionAndSpinCount(&device->state_mutex,
                                               4000) != 0)
            ? 1
            : 0;
    device->cache_mutex_init =
        (InitializeCriticalSectionAndSpinCount(&device->cache_mutex,
                                               4000) != 0)
            ? 1
            : 0;
    device->desc_mutex_init =
        (InitializeCriticalSectionAndSpinCount(&device->desc_mutex,
                                               4000) != 0)
            ? 1
            : 0;
    device->submit_mutex_init =
        (InitializeCriticalSectionAndSpinCount(&device->submit_mutex,
                                               4000) != 0)
            ? 1
            : 0;
#else
    /* (transfer_mutex already recursive above) */
    /* The state shard nests (marks inside reconciled executes), so
     * POSIX needs a recursive mutex (CRITICAL_SECTION is already
     * reentrant on Windows). */
    {
        pthread_mutexattr_t attr;

        device->state_mutex_init = 0;
        if (pthread_mutexattr_init(&attr) == 0) {
            if (pthread_mutexattr_settype(&attr,
                                          PTHREAD_MUTEX_RECURSIVE) == 0 &&
                pthread_mutex_init(&device->state_mutex, &attr) == 0) {
                device->state_mutex_init = 1;
            }
            pthread_mutexattr_destroy(&attr);
        }
    }
    device->cache_mutex_init =
        (pthread_mutex_init(&device->cache_mutex, NULL) == 0) ? 1 : 0;
    device->desc_mutex_init =
        (pthread_mutex_init(&device->desc_mutex, NULL) == 0) ? 1 : 0;
    /* Phase 22: submit nests transfer (frame holds submit across
     * emit), so POSIX needs it recursive like the state shard
     * (CRITICAL_SECTION is already reentrant on Windows). */
    {
        pthread_mutexattr_t submit_attr;

        device->submit_mutex_init = 0;
        if (pthread_mutexattr_init(&submit_attr) == 0) {
            if (pthread_mutexattr_settype(&submit_attr,
                                          PTHREAD_MUTEX_RECURSIVE) == 0 &&
                pthread_mutex_init(&device->submit_mutex,
                                   &submit_attr) == 0) {
                device->submit_mutex_init = 1;
            }
            pthread_mutexattr_destroy(&submit_attr);
        }
    }
#endif
    /* Allocator mutex (PART AL): internal lock only, never a giant
     * LumaC lock. Best effort: without it the allocator runs
     * unlocked (single-threaded contract still holds). */
#if defined(_WIN32) || defined(_WIN64)
    if (InitializeCriticalSectionAndSpinCount(&device->mem_mutex,
                                              4000) != 0) {
        device->mem_mutex_init = 1;
    } else {
        device->mem_mutex_init = 0;
    }
#else
    device->mem_mutex_init =
        (pthread_mutex_init(&device->mem_mutex, NULL) == 0) ? 1 : 0;
#endif
    /* Memory-budget extension (optional; queried per use). */
    device->mem_budget_supported = lc_vk_has_device_extension(
        physical, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
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

/* Maximum pipeline-cache blob kept on disk (64 MiB). Larger blobs
 * are kept in memory for the session but never written (no
 * truncation — truncating a cache blob corrupts it). */
#define LC_PIPELINE_CACHE_MAX_BYTES (64u * 1024u * 1024u)

/* Warning-clean fopen (C4996 on MSVC): fopen_s where available,
 * plain fopen elsewhere. */
static FILE *lc_vk_cache_fopen(const char *path, const char *mode) {
#if defined(_MSC_VER)
    FILE *f = NULL;

    if (fopen_s(&f, path, mode) != 0) {
        return NULL;
    }
    return f;
#else
    return fopen(path, mode);
#endif
}

/* Load a cache blob from disk. Returns NULL when no usable file
 * exists (missing, unreadable, empty, oversized). Corrupt content
 * is NOT rejected here — Vulkan validates the header at
 * vkCreatePipelineCache and we fall back to empty on failure. */
static void *lc_vk_cache_load(const char *path, size_t *out_size) {
    FILE *f = NULL;
    long size = 0;
    void *data = NULL;
    size_t got = 0;

    if (out_size != NULL) {
        *out_size = 0;
    }
    if (path == NULL || path[0] == '\0' || out_size == NULL) {
        return NULL;
    }
    f = lc_vk_cache_fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    size = ftell(f);
    if (size <= 0 || (uint64_t)size > LC_PIPELINE_CACHE_MAX_BYTES) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    data = malloc((size_t)size);
    if (data == NULL) {
        fclose(f);
        return NULL;
    }
    got = fread(data, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        free(data);
        return NULL;
    }
    *out_size = (size_t)size;
    return data;
}

/* Atomically replace `path` with `data[0..size)`: write sibling tmp,
 * flush/close, then rename over the destination. Returns nonzero on
 * success. Never truncates the destination on failure. */
static int lc_vk_cache_save_atomic(const char *path, const void *data,
                                   size_t size) {
    char tmp[576];
    FILE *f = NULL;
    size_t wrote = 0;

    if (path == NULL || path[0] == '\0' || data == NULL || size == 0) {
        return 0;
    }
    if (size > LC_PIPELINE_CACHE_MAX_BYTES) {
        fprintf(stderr,
                "[lumac] pipeline cache blob too large (%lu bytes); "
                "keeping in-memory only\n",
                (unsigned long)size);
        return 0;
    }
    memset(tmp, 0, sizeof(tmp));
    /* Sibling tmp path; snprintf truncation still NUL-terminates. */
    snprintf(tmp, sizeof(tmp) - 1, "%s.tmp", path);
    f = lc_vk_cache_fopen(tmp, "wb");
    if (f == NULL) {
        return 0;
    }
    wrote = fwrite(data, 1, size, f);
    if (fflush(f) != 0) {
        wrote = 0;
    }
    fclose(f);
    if (wrote != size) {
        remove(tmp);
        return 0;
    }
    /* Windows rename fails when the destination exists; remove first
     * (a crash between remove and rename loses the old cache, but
     * never corrupts it — the new tmp remains for manual recovery). */
    remove(path);
    if (rename(tmp, path) != 0) {
        remove(tmp);
        return 0;
    }
    return 1;
}

/* Create device->pipeline_cache: empty, or seeded from the configured
 * path when compatible. Never fails device creation: bad blobs fall
 * back to an empty cache with a stderr notice. */
static void lc_vk_cache_create(lc_device *device) {
    VkPipelineCacheCreateInfo info;
    void *blob = NULL;
    size_t blob_size = 0;

    device->pipeline_cache = VK_NULL_HANDLE;
    device->pipeline_cache_enabled = 0;
    device->pipeline_cache_loaded = 0;
    device->pipeline_cache_bytes_loaded = 0;
    if (device->device == VK_NULL_HANDLE) {
        return;
    }
    if (device->pipeline_cache_path[0] != '\0') {
        blob = lc_vk_cache_load(device->pipeline_cache_path, &blob_size);
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    if (blob != NULL && blob_size > 0) {
        info.initialDataSize = blob_size;
        info.pInitialData = blob;
    }
    if (vkCreatePipelineCache(device->device, &info, NULL,
                              &device->pipeline_cache) != VK_SUCCESS) {
        /* Incompatible/corrupt blob: retry empty (Vulkan validates
         * vendor/device/driver UUIDs in the header). */
        if (blob != NULL) {
            fprintf(stderr,
                    "[lumac] pipeline cache incompatible or corrupt; "
                    "starting empty\n");
        }
        info.initialDataSize = 0;
        info.pInitialData = NULL;
        if (vkCreatePipelineCache(device->device, &info, NULL,
                                  &device->pipeline_cache) != VK_SUCCESS) {
            device->pipeline_cache = VK_NULL_HANDLE;
            free(blob);
            return;
        }
        free(blob);
        blob = NULL;
        blob_size = 0;
    } else if (blob != NULL && blob_size > 0) {
        device->pipeline_cache_loaded = 1;
        device->pipeline_cache_bytes_loaded = (uint64_t)blob_size;
    }
    free(blob);
    device->pipeline_cache_enabled =
        (device->pipeline_cache != VK_NULL_HANDLE) ? 1 : 0;
}

/* Save device->pipeline_cache back to the configured path (atomic).
 * No-op without a path or without a cache. */
static void lc_vk_cache_save(lc_device *device) {
    void *data = NULL;
    size_t size = 0;
    VkResult res;

    device->pipeline_cache_saved = 0;
    device->pipeline_cache_bytes_saved = 0;
    if (device->pipeline_cache == VK_NULL_HANDLE ||
        device->device == VK_NULL_HANDLE ||
        device->pipeline_cache_path[0] == '\0') {
        return;
    }
    res = vkGetPipelineCacheData(device->device, device->pipeline_cache,
                                 &size, NULL);
    if (res != VK_SUCCESS || size == 0) {
        return;
    }
    if (size > LC_PIPELINE_CACHE_MAX_BYTES) {
        fprintf(stderr,
                "[lumac] pipeline cache blob too large (%lu bytes); "
                "keeping in-memory only\n",
                (unsigned long)size);
        return;
    }
    data = malloc(size);
    if (data == NULL) {
        return;
    }
    res = vkGetPipelineCacheData(device->device, device->pipeline_cache,
                                 &size, data);
    if (res != VK_SUCCESS || size == 0) {
        free(data);
        return;
    }
    if (lc_vk_cache_save_atomic(device->pipeline_cache_path, data, size)) {
        device->pipeline_cache_saved = 1;
        device->pipeline_cache_bytes_saved = (uint64_t)size;
    }
    free(data);
}

lc_result lc_vulkan_device_create(lc_device *device,
                                  const lc_device_desc *desc) {
    int use_validation = 0;
    int enable_validation = 0;
    int disable_cache = 0;
    uint32_t api_version;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props;
    uint32_t family = UINT32_MAX;
    lc_result res;

    if (device == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&props, 0, sizeof(props));
    enable_validation = desc->enable_validation;
    disable_cache = (desc->disable_pipeline_cache != 0) ? 1 : 0;
    /* Staging cap (PART V): 0 selects the default. */
    device->staging_cap = (desc->upload_staging_cap != 0)
                              ? desc->upload_staging_cap
                              : (256u * 1024u * 1024u);
    /* Test-only forcing (Phase 22): honored below at queue and
     * timeline selection. */
    device->force_binary_fallback =
        (desc->force_binary_fallback != 0) ? 1 : 0;
    device->force_graphics_transfer =
        (desc->force_graphics_transfer != 0) ? 1 : 0;
    device->force_graphics_compute =
        (desc->force_graphics_compute != 0) ? 1 : 0;

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
    if (!disable_cache) {
        lc_vk_cache_create(device);
    }
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
    /* Pipeline-cache save first (needs VkDevice alive), then the
     * usual teardown: upload context, descriptor pools, pass cache,
     * VkDevice, queues, messenger, instance. */
    lc_vk_cache_save(device);
    if (device->pipeline_cache != VK_NULL_HANDLE &&
        device->device != VK_NULL_HANDLE) {
        vkDestroyPipelineCache(device->device, device->pipeline_cache,
                               NULL);
    }
    device->pipeline_cache = VK_NULL_HANDLE;
    /* Phase 20 transfer/retirement flush first (waits for GPU work,
     * then frees staging, command buffers, retired objects — the
     * documented shutdown exception to the no-global-idle rule),
     * then the timeline semaphore itself. */
    lc_vk_transfer_shutdown(device);
    if (device->timeline != VK_NULL_HANDLE &&
        device->device != VK_NULL_HANDLE) {
        vkDestroySemaphore(device->device, device->timeline, NULL);
    }
    device->timeline = VK_NULL_HANDLE;
    device->timeline_ok = 0;
    /* Memory allocator first (frees all blocks; tracked dependents
     * already gone via device-destroy hooks), then upload context,
     * descriptor pools, and the Phase 12 render-pass cache:
     * pools/fences/passes die before VkDevice. */
    lc_vk_mem_teardown(device);
    lc_vulkan_upload_teardown(device);
    lc_vulkan_desc_teardown(device);
    lc_vulkan_pass_cache_teardown(device);
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
    /* Phase 20 locks (transfer/state/cache/desc/submit). */
#if defined(_WIN32) || defined(_WIN64)
    if (device->transfer_mutex_init) {
        DeleteCriticalSection(&device->transfer_mutex);
        device->transfer_mutex_init = 0;
    }
    if (device->state_mutex_init) {
        DeleteCriticalSection(&device->state_mutex);
        device->state_mutex_init = 0;
    }
    if (device->cache_mutex_init) {
        DeleteCriticalSection(&device->cache_mutex);
        device->cache_mutex_init = 0;
    }
    if (device->desc_mutex_init) {
        DeleteCriticalSection(&device->desc_mutex);
        device->desc_mutex_init = 0;
    }
    if (device->submit_mutex_init) {
        DeleteCriticalSection(&device->submit_mutex);
        device->submit_mutex_init = 0;
    }
#else
    if (device->transfer_mutex_init) {
        pthread_mutex_destroy(&device->transfer_mutex);
        device->transfer_mutex_init = 0;
    }
    if (device->state_mutex_init) {
        pthread_mutex_destroy(&device->state_mutex);
        device->state_mutex_init = 0;
    }
    if (device->cache_mutex_init) {
        pthread_mutex_destroy(&device->cache_mutex);
        device->cache_mutex_init = 0;
    }
    if (device->desc_mutex_init) {
        pthread_mutex_destroy(&device->desc_mutex);
        device->desc_mutex_init = 0;
    }
    if (device->submit_mutex_init) {
        pthread_mutex_destroy(&device->submit_mutex);
        device->submit_mutex_init = 0;
    }
#endif
}
