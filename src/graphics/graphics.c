#include <stdlib.h>

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

    res = lc_vulkan_device_create(device, desc->enable_validation);
    if (res != LC_SUCCESS) {
        *out_device = NULL;
        free(device);
        return res;
    }

    lc_device_list_add(device);
    *out_device = device;
    return LC_SUCCESS;
}

void lc_device_destroy(lc_device *device) {
    if (device == NULL) {
        return;
    }
    /* Dependents first: swapchains, then surfaces. vkDestroySwapchainKHR
     * and vkDestroySurfaceKHR both need the logical device / instance,
     * which die with the device below. */
    lc_swapchain_destroy_for_device(device);
    lc_surface_destroy_for_device(device);
    lc_device_list_remove(device);
    lc_vulkan_device_destroy(device);
    free(device);
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
