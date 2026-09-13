#include <stdlib.h>
#include <string.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

static void lc_device_list_add(lc_device *device) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || device == NULL) {
        return;
    }
    device->next = state->devices;
    device->prev = NULL;
    if (state->devices != NULL) {
        state->devices->prev = device;
    }
    state->devices = device;
}

static void lc_device_list_remove(lc_device *device) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || device == NULL) {
        return;
    }
    if (device->prev != NULL) {
        device->prev->next = device->next;
    } else if (state->devices == device) {
        state->devices = device->next;
    }
    if (device->next != NULL) {
        device->next->prev = device->prev;
    }
    device->next = NULL;
    device->prev = NULL;
}

lc_result lc_device_create(const lc_device_desc *desc, lc_device **out_device) {
    lc_state *state = lc_get_internal_state();
    lc_device *device;
    lc_result res;

    if (desc == NULL || out_device == NULL) {
        if (out_device != NULL) {
            *out_device = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_device = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (desc->backend != LC_BACKEND_VULKAN) {
        *out_device = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }

    device = (lc_device *)calloc(1, sizeof(lc_device));
    if (device == NULL) {
        *out_device = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    device->backend = LC_BACKEND_VULKAN;
    device->resource_id = lc_issue_resource_id();
    if (desc->pipeline_cache_path != NULL) {
        size_t n = strlen(desc->pipeline_cache_path);

        if (n >= sizeof(device->pipeline_cache_path)) {
            n = sizeof(device->pipeline_cache_path) - 1;
        }
        memcpy(device->pipeline_cache_path, desc->pipeline_cache_path, n);
        device->pipeline_cache_path[n] = '\0';
    }

    res = lc_vulkan_device_create(device, desc);
    if (res != LC_SUCCESS) {
        *out_device = NULL;
        free(device);
        return res;
    }

    lc_device_list_add(device);
    *out_device = device;
    return LC_SUCCESS;
}

void lc_device_destroy(lc_device *device) {    if (device == NULL) {
        return;
    }
    /* Dependents first: pipelines, binding sets and layouts, shaders,
     * samplers, render targets (borrow views), images (with views),
     * buffers, swapchains, then surfaces. Descriptor pools and the
     * render-pass cache die inside lc_vulkan_device_destroy, after
     * every set/target/framebuffer is freed. vkDestroySwapchainKHR and
     * vkDestroySurfaceKHR both need the logical device / instance,
     * which die with the device below. */
    lc_pipeline_destroy_for_device(device);
    lc_binding_set_destroy_for_device(device);
    lc_binding_layout_destroy_for_device(device);
    lc_shader_destroy_for_device(device);
    lc_sampler_destroy_for_device(device);
    lc_render_target_destroy_for_device(device);
    lc_image_view_destroy_for_device(device);
    lc_image_destroy_for_device(device);
    lc_buffer_destroy_for_device(device);
    lc_swapchain_destroy_for_device(device);
    lc_surface_destroy_for_device(device);
    lc_device_list_remove(device);
    lc_vulkan_device_destroy(device);
    free(device);
}

void lc_device_wait_idle(lc_device *device) {
    lc_state *state = lc_get_internal_state();
    const lc_device *it;
    int live = 0;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->devices; it != NULL; it = it->next) {
        if (it == device) {
            live = 1;
            break;
        }
    }
    if (!live) {
        return;
    }
    if (device->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->device);
    }
}

void lc_device_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->devices != NULL) {
        lc_device_destroy(state->devices);
    }
}

uint32_t lc_device_get_pass_cache_count(const lc_device *device) {
    if (device == NULL) {
        return 0;
    }
    return device->pass_cache_count;
}

const char *lc_device_get_name(const lc_device *device) {
    if (device == NULL) {
        return NULL;
    }
    return device->name;
}

lc_backend lc_device_get_backend(const lc_device *device) {
    if (device == NULL) {
        return (lc_backend)0;
    }
    return device->backend;
}

uint32_t lc_device_get_vendor_id(const lc_device *device) {
    if (device == NULL) {
        return 0;
    }
    return device->vendor_id;
}

uint32_t lc_device_get_device_id(const lc_device *device) {
    if (device == NULL) {
        return 0;
    }
    return device->device_id;
}

void lc_device_get_pipeline_cache_info(
    const lc_device *device,
    lc_pipeline_cache_info *out_info) {    if (out_info == NULL) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (device == NULL) {
        return;
    }
    out_info->enabled = device->pipeline_cache_enabled;
    out_info->loaded_from_file = device->pipeline_cache_loaded;
    out_info->saved_to_file = device->pipeline_cache_saved;
    out_info->bytes_loaded = device->pipeline_cache_bytes_loaded;
    out_info->bytes_saved = device->pipeline_cache_bytes_saved;
}

lc_resource_id lc_issue_resource_id(void) {
    lc_state *state = lc_get_internal_state();

    if (state == NULL) {
        return 0;
    }
    if (state->next_resource_id == 0) {
        state->next_resource_id = 1;
    }
    /* Wrap guard: 0 is reserved for NULL; skip it on overflow. */
    if (state->next_resource_id == 0) {
        state->next_resource_id = 1;
    }
    return state->next_resource_id++;
}

void lc_device_get_limits(const lc_device *device,
                          lc_device_limits *out_limits) {
    VkPhysicalDeviceProperties props;

    if (out_limits == NULL) {
        return;
    }
    memset(out_limits, 0, sizeof(*out_limits));
    if (device == NULL || device->physical_device == VK_NULL_HANDLE) {
        return;
    }
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(device->physical_device, &props);
    out_limits->max_texture_2d_dimension = props.limits.maxImageDimension2D;
    out_limits->max_vertex_attributes =
        props.limits.maxVertexInputAttributes;
    out_limits->max_vertex_bindings = props.limits.maxVertexInputBindings;
    out_limits->max_uniform_buffer_size =
        (uint64_t)props.limits.maxUniformBufferRange;
    out_limits->max_image_array_layers = props.limits.maxImageArrayLayers;
    out_limits->max_sampler_anisotropy =
        (device->anisotropy_supported != 0) ? props.limits.maxSamplerAnisotropy
                                            : 1.0f;
    out_limits->min_uniform_buffer_offset_alignment =
        props.limits.minUniformBufferOffsetAlignment;
    out_limits->min_storage_buffer_offset_alignment =
        props.limits.minStorageBufferOffsetAlignment;
    out_limits->max_bound_resource_slots = props.limits.maxBoundDescriptorSets;
    /* Never promise more than the public structural maximum. */
    out_limits->max_color_attachments = props.limits.maxColorAttachments;
    if (out_limits->max_color_attachments > LC_MAX_COLOR_ATTACHMENTS) {
        out_limits->max_color_attachments = LC_MAX_COLOR_ATTACHMENTS;
    }
}

void lc_device_get_memory_stats(const lc_device *device,
                                lc_memory_stats *out_stats) {
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (device == NULL) {
        return;
    }
    lc_vk_mem_stats(device, out_stats);
}

void lc_device_get_memory_budget(const lc_device *device,
                                 lc_memory_budget *out_budget) {
    if (out_budget == NULL) {
        return;
    }
    memset(out_budget, 0, sizeof(*out_budget));
    if (device == NULL ||
        device->physical_device == VK_NULL_HANDLE ||
        device->instance == VK_NULL_HANDLE ||
        !device->mem_budget_supported) {
        return;
    }
    {
        /* Resolved per call: static-linking 1.1+ entry points would
         * break load on 1.0 loaders. */
        PFN_vkGetPhysicalDeviceMemoryProperties2 pfn =
            (PFN_vkGetPhysicalDeviceMemoryProperties2)
                vkGetInstanceProcAddr(
                    device->instance,
                    "vkGetPhysicalDeviceMemoryProperties2");
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget;
        VkPhysicalDeviceMemoryProperties2 props2;
        uint32_t i;

        if (pfn == NULL) {
            return;
        }
        memset(&budget, 0, sizeof(budget));
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        memset(&props2, 0, sizeof(props2));
        props2.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        props2.pNext = &budget;
        pfn(device->physical_device, &props2);
        for (i = 0; i < props2.memoryProperties.memoryHeapCount && i < LC_MAX_MEMORY_HEAPS; i++) {
            out_budget->heaps[i].budget = budget.heapBudget[i];
            out_budget->heaps[i].usage = budget.heapUsage[i];
            out_budget->heap_count++;
        }
        out_budget->available = (out_budget->heap_count > 0) ? 1 : 0;
    }
}
