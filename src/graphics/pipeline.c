#include <stdlib.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public pipeline API + lifetime tracking (Phase 7).
 *
 * Handle liveness is verified before any dereference. Shaders must be
 * live, correctly staged, and owned by the same device as the target
 * swapchain; only module handles are consumed, so shaders may die
 * while pipelines live on. Pipelines are anchored to their creation
 * swapchain for dependency cleanup but may bind on any same-device,
 * same-format swapchain (checked at bind time).
 */

static void lc_pipeline_list_add(lc_pipeline *pipeline) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || pipeline == NULL) {
        return;
    }
    pipeline->next = state->pipelines;
    pipeline->prev = NULL;
    if (state->pipelines != NULL) {
        state->pipelines->prev = pipeline;
    }
    state->pipelines = pipeline;
}

static void lc_pipeline_list_remove(lc_pipeline *pipeline) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || pipeline == NULL) {
        return;
    }
    if (pipeline->prev != NULL) {
        pipeline->prev->next = pipeline->next;
    } else if (state->pipelines == pipeline) {
        state->pipelines = pipeline->next;
    }
    if (pipeline->next != NULL) {
        pipeline->next->prev = pipeline->prev;
    }
    pipeline->next = NULL;
    pipeline->prev = NULL;
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

static int lc_is_live_swapchain(const lc_swapchain *swapchain) {
    lc_state *state = lc_get_internal_state();
    const lc_swapchain *it;

    if (state == NULL || swapchain == NULL) {
        return 0;
    }
    for (it = state->swapchains; it != NULL; it = it->next) {
        if (it == swapchain) {
            return 1;
        }
    }
    return 0;
}

static int lc_is_live_shader(const lc_shader *shader) {
    lc_state *state = lc_get_internal_state();
    const lc_shader *it;

    if (state == NULL || shader == NULL) {
        return 0;
    }
    for (it = state->shaders; it != NULL; it = it->next) {
        if (it == shader) {
            return 1;
        }
    }
    return 0;
}

int lc_pipeline_is_live(const lc_pipeline *pipeline) {
    lc_state *state = lc_get_internal_state();
    const lc_pipeline *it;

    if (state == NULL || pipeline == NULL) {
        return 0;
    }
    for (it = state->pipelines; it != NULL; it = it->next) {
        if (it == pipeline) {
            return 1;
        }
    }
    return 0;
}

lc_result lc_graphics_pipeline_create(
    lc_device *device, lc_swapchain *swapchain,
    const lc_graphics_pipeline_desc *desc, lc_pipeline **out_pipeline) {
    lc_state *state = lc_get_internal_state();
    lc_pipeline *pipeline;
    lc_result res;

    if (device == NULL || swapchain == NULL || desc == NULL ||
        out_pipeline == NULL) {
        if (out_pipeline != NULL) {
            *out_pipeline = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_pipeline = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    /* Liveness before any dereference of caller handles. */
    if (!lc_is_live_device(device) || !lc_is_live_swapchain(swapchain) ||
        !lc_is_live_shader(desc->vertex_shader) ||
        !lc_is_live_shader(desc->fragment_shader)) {
        *out_pipeline = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Everything must belong to one device; stages must match. */
    if (swapchain->device != device ||
        desc->vertex_shader->device != device ||
        desc->fragment_shader->device != device ||
        desc->vertex_shader->stage != LC_SHADER_STAGE_VERTEX ||
        desc->fragment_shader->stage != LC_SHADER_STAGE_FRAGMENT) {
        *out_pipeline = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }

    pipeline = (lc_pipeline *)calloc(1, sizeof(lc_pipeline));
    if (pipeline == NULL) {
        *out_pipeline = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }

    res = lc_vulkan_pipeline_create(pipeline, device, swapchain,
                                    desc->vertex_shader,
                                    desc->fragment_shader);
    if (res != LC_SUCCESS) {
        *out_pipeline = NULL;
        free(pipeline);
        return res;
    }

    lc_pipeline_list_add(pipeline);
    *out_pipeline = pipeline;
    return LC_SUCCESS;
}

void lc_pipeline_destroy(lc_pipeline *pipeline) {
    if (pipeline == NULL) {
        return;
    }
    lc_pipeline_list_remove(pipeline);
    lc_vulkan_pipeline_destroy(pipeline);
    free(pipeline);
}

void lc_pipeline_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->pipelines != NULL) {
        lc_pipeline_destroy(state->pipelines);
    }
}

void lc_pipeline_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_pipeline *it;
    lc_pipeline *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->pipelines; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_pipeline_destroy(it);
        }
    }
}

void lc_pipeline_destroy_for_swapchain(const lc_swapchain *swapchain) {
    lc_state *state = lc_get_internal_state();
    lc_pipeline *it;
    lc_pipeline *next;

    if (state == NULL || swapchain == NULL) {
        return;
    }
    for (it = state->pipelines; it != NULL; it = next) {
        next = it->next;
        if (it->swapchain == swapchain) {
            lc_pipeline_destroy(it);
        }
    }
}
