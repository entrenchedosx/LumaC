#include <stdlib.h>
#include <string.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public compute pipeline API + lifetime tracking (Phase 21).
 *
 * Handle liveness is verified before any dereference. The compute
 * shader must be live, COMPUTE-staged, and owned by the same device.
 * Vk objects retire through the shared deferred-retirement queue
 * (LC_RETIRE_PIPELINE); worker command lists referencing a
 * destroyed pipeline are poisoned so execute fails loudly.
 */

void lc_compute_pipeline_list_add(lc_compute_pipeline *pipeline) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || pipeline == NULL) {
        return;
    }
    pipeline->next = state->compute_pipelines;
    pipeline->prev = NULL;
    if (state->compute_pipelines != NULL) {
        state->compute_pipelines->prev = pipeline;
    }
    state->compute_pipelines = pipeline;
}

void lc_compute_pipeline_list_remove(lc_compute_pipeline *pipeline) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || pipeline == NULL) {
        return;
    }
    if (pipeline->prev != NULL) {
        pipeline->prev->next = pipeline->next;
    } else if (state->compute_pipelines == pipeline) {
        state->compute_pipelines = pipeline->next;
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

static int lc_is_live_compute_shader(const lc_shader *shader) {
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

int lc_compute_pipeline_is_live(const lc_compute_pipeline *pipeline) {
    lc_state *state = lc_get_internal_state();
    const lc_compute_pipeline *it;

    if (state == NULL || pipeline == NULL) {
        return 0;
    }
    for (it = state->compute_pipelines; it != NULL; it = it->next) {
        if (it == pipeline) {
            return 1;
        }
    }
    return 0;
}

lc_result lc_compute_pipeline_create(
    lc_device *device, const lc_compute_pipeline_desc *desc,
    lc_compute_pipeline **out_pipeline) {
    lc_state *state = lc_get_internal_state();
    lc_compute_pipeline *pipeline = NULL;
    lc_result res;

    if (out_pipeline != NULL) {
        *out_pipeline = NULL;
    }
    if (device == NULL || desc == NULL || out_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_is_live_device(device)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->compute_shader == NULL ||
        !lc_is_live_compute_shader(desc->compute_shader) ||
        desc->compute_shader->device != device ||
        desc->compute_shader->stage != LC_SHADER_STAGE_COMPUTE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    if (!device->compute_supported) {
        return LC_ERROR_UNSUPPORTED;
    }
    pipeline =
        (lc_compute_pipeline *)calloc(1, sizeof(lc_compute_pipeline));
    if (pipeline == NULL) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    res = lc_vulkan_compute_pipeline_create(pipeline, device, desc);
    if (res != LC_SUCCESS) {
        free(pipeline);
        return res;
    }
    pipeline->resource_id = lc_issue_resource_id();
    lc_compute_pipeline_list_add(pipeline);
    *out_pipeline = pipeline;
    return LC_SUCCESS;
}

void lc_compute_pipeline_destroy(lc_compute_pipeline *pipeline) {
    lc_device *device;
    lc_retire_entry entry;

    if (pipeline == NULL) {
        return;
    }
    device = pipeline->device;
    lc_compute_pipeline_list_remove(pipeline);
    /* Unexecuted worker lists referencing this pipeline fail loudly
     * at execute instead of dereferencing a freed wrapper. */
    if (device != NULL) {
        lc_vk_cmdlist_poison_for(device, pipeline);
    }
    /* The VkPipeline/VkPipelineLayout retire (no global idle);
     * canonical CPU copies free via the backend destroy, whose Vk
     * handles are already stolen below. */
    memset(&entry, 0, sizeof(entry));
    entry.kind = LC_RETIRE_PIPELINE;
    entry.pipeline = pipeline->pipeline;
    entry.pipeline_layout = pipeline->layout;
    pipeline->pipeline = VK_NULL_HANDLE;
    pipeline->layout = VK_NULL_HANDLE;
    if (device != NULL) {
        lc_vk_retire(device, &entry);
    }
    lc_vulkan_compute_pipeline_destroy(pipeline);
    free(pipeline);
}

void lc_compute_pipeline_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->compute_pipelines != NULL) {
        lc_compute_pipeline_destroy(state->compute_pipelines);
    }
}

void lc_compute_pipeline_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_compute_pipeline *it;
    lc_compute_pipeline *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->compute_pipelines; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_compute_pipeline_destroy(it);
        }
    }
}
