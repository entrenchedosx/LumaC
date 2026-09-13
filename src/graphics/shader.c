#include <stdlib.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public shader API + lifetime tracking (Phase 7).
 *
 * Device liveness is verified against the internal list before any
 * dereference, mirroring surface.c/swapchain.c. Live shaders are
 * tracked so device teardown and shutdown destroy them in dependency
 * order. Pipelines consume only module handles, so a shader may die
 * while its pipelines live on.
 */

static void lc_shader_list_add(lc_shader *shader) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || shader == NULL) {
        return;
    }
    shader->next = state->shaders;
    shader->prev = NULL;
    if (state->shaders != NULL) {
        state->shaders->prev = shader;
    }
    state->shaders = shader;
}

static void lc_shader_list_remove(lc_shader *shader) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || shader == NULL) {
        return;
    }
    if (shader->prev != NULL) {
        shader->prev->next = shader->next;
    } else if (state->shaders == shader) {
        state->shaders = shader->next;
    }
    if (shader->next != NULL) {
        shader->next->prev = shader->prev;
    }
    shader->next = NULL;
    shader->prev = NULL;
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

lc_result lc_shader_create(lc_device *device, const lc_shader_desc *desc,
                           lc_shader **out_shader) {
    lc_state *state = lc_get_internal_state();
    lc_shader *shader;
    lc_result res;

    if (device == NULL || desc == NULL || out_shader == NULL) {
        if (out_shader != NULL) {
            *out_shader = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_shader = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    /* Liveness before any dereference of the caller handle. */
    if (!lc_is_live_device(device)) {
        *out_shader = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }

    shader = (lc_shader *)calloc(1, sizeof(lc_shader));
    if (shader == NULL) {
        *out_shader = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    shader->resource_id = lc_issue_resource_id();

    res = lc_vulkan_shader_create(shader, device, desc);
    if (res != LC_SUCCESS) {
        *out_shader = NULL;
        free(shader);
        return res;
    }

    lc_shader_list_add(shader);
    *out_shader = shader;
    return LC_SUCCESS;
}

void lc_shader_destroy(lc_shader *shader) {
    if (shader == NULL) {
        return;
    }
    lc_shader_list_remove(shader);
    lc_vulkan_shader_destroy(shader);
    free(shader);
}

void lc_shader_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->shaders != NULL) {
        lc_shader_destroy(state->shaders);
    }
}

void lc_shader_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_shader *it;
    lc_shader *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->shaders; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_shader_destroy(it);
        }
    }
}
