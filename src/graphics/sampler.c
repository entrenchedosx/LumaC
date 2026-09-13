#include <stdlib.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public sampler API + lifetime tracking (Phase 9).
 *
 * Samplers are standalone, device-owned configuration handles: no
 * image bindings exist yet, so there are no cross-resource hooks
 * beyond the device. Device liveness is verified before any
 * dereference; device teardown and shutdown destroy dependent
 * samplers before VkDevice.
 */

static void lc_sampler_list_add(lc_sampler *sampler) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || sampler == NULL) {
        return;
    }
    sampler->next = state->samplers;
    sampler->prev = NULL;
    if (state->samplers != NULL) {
        state->samplers->prev = sampler;
    }
    state->samplers = sampler;
}

static void lc_sampler_list_remove(lc_sampler *sampler) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || sampler == NULL) {
        return;
    }
    if (sampler->prev != NULL) {
        sampler->prev->next = sampler->next;
    } else if (state->samplers == sampler) {
        state->samplers = sampler->next;
    }
    if (sampler->next != NULL) {
        sampler->next->prev = sampler->prev;
    }
    sampler->next = NULL;
    sampler->prev = NULL;
}

static int lc_is_live_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    const lc_device *it;

    if (state == NULL || device == NULL) {
        return 0;
    }
    for (it = state->devices; it != NULL; it = it->next) {
        if (it == device) {
            return 1;
        }
    }
    return 0;
}

lc_result lc_sampler_create(lc_device *device, const lc_sampler_desc *desc,
                            lc_sampler **out_sampler) {
    lc_state *state = lc_get_internal_state();
    lc_sampler *sampler;
    lc_result res;

    if (device == NULL || desc == NULL || out_sampler == NULL) {
        if (out_sampler != NULL) {
            *out_sampler = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_sampler = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    /* Liveness before any dereference of the caller handle. */
    if (!lc_is_live_device(device)) {
        *out_sampler = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }

    sampler = (lc_sampler *)calloc(1, sizeof(lc_sampler));
    if (sampler == NULL) {
        *out_sampler = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    sampler->resource_id = lc_issue_resource_id();

    res = lc_vulkan_sampler_create(sampler, device, desc);
    if (res != LC_SUCCESS) {
        *out_sampler = NULL;
        free(sampler);
        return res;
    }

    lc_sampler_list_add(sampler);
    *out_sampler = sampler;
    return LC_SUCCESS;
}

void lc_sampler_destroy(lc_sampler *sampler) {
    if (sampler == NULL) {
        return;
    }
    lc_sampler_list_remove(sampler);
    lc_vulkan_sampler_destroy(sampler);
    free(sampler);
}

void lc_sampler_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->samplers != NULL) {
        lc_sampler_destroy(state->samplers);
    }
}

void lc_sampler_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_sampler *it;
    lc_sampler *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->samplers; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_sampler_destroy(it);
        }
    }
}
