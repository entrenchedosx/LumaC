#include <stdlib.h>
#include <string.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public buffer API + lifetime tracking (Phase 8).
 *
 * Buffers belong to one device and survive swapchain recreation (they
 * are never tied to surfaces or swapchains). Device liveness is
 * verified before any dereference; device teardown and shutdown
 * destroy dependent buffers before VkDevice. See vulkan_buffer.c for
 * allocation, persistent mapping, and staging uploads.
 */

/* All currently defined lc_buffer_usage bits. */
#define LC_BUFFER_USAGE_KNOWN_MASK 0x3Fu

static void lc_buffer_list_add(lc_buffer *buffer) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || buffer == NULL) {
        return;
    }
    buffer->next = state->buffers;
    buffer->prev = NULL;
    if (state->buffers != NULL) {
        state->buffers->prev = buffer;
    }
    state->buffers = buffer;
}

static void lc_buffer_list_remove(lc_buffer *buffer) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || buffer == NULL) {
        return;
    }
    if (buffer->prev != NULL) {
        buffer->prev->next = buffer->next;
    } else if (state->buffers == buffer) {
        state->buffers = buffer->next;
    }
    if (buffer->next != NULL) {
        buffer->next->prev = buffer->prev;
    }
    buffer->next = NULL;
    buffer->prev = NULL;
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

static int lc_is_live_buffer(const lc_buffer *buffer) {
    lc_state *state = lc_get_internal_state();
    const lc_buffer *it;

    if (state == NULL || buffer == NULL) {
        return 0;
    }
    for (it = state->buffers; it != NULL; it = it->next) {
        if (it == buffer) {
            return 1;
        }
    }
    return 0;
}

lc_result lc_buffer_create(lc_device *device, const lc_buffer_desc *desc,
                           lc_buffer **out_buffer) {
    lc_state *state = lc_get_internal_state();
    lc_buffer *buffer;
    lc_result res;

    if (device == NULL || desc == NULL || out_buffer == NULL) {
        if (out_buffer != NULL) {
            *out_buffer = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_buffer = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    /* Liveness before any dereference of the caller handle. */
    if (!lc_is_live_device(device)) {
        *out_buffer = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->size == 0 || desc->usage == 0 ||
        (desc->usage & ~LC_BUFFER_USAGE_KNOWN_MASK) != 0 ||
        desc->memory > LC_MEMORY_GPU_TO_CPU) {
        *out_buffer = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }

    buffer = (lc_buffer *)calloc(1, sizeof(lc_buffer));
    if (buffer == NULL) {
        *out_buffer = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    buffer->device = device;
    buffer->resource_id = lc_issue_resource_id();
    buffer->size = desc->size;
    buffer->usage = desc->usage;
    buffer->memory_usage = desc->memory;

    res = lc_vulkan_buffer_create(buffer);
    if (res != LC_SUCCESS) {
        *out_buffer = NULL;
        free(buffer);
        return res;
    }

    lc_buffer_list_add(buffer);
    *out_buffer = buffer;
    return LC_SUCCESS;
}

void lc_buffer_destroy(lc_buffer *buffer) {
    if (buffer == NULL) {
        return;
    }
    lc_buffer_list_remove(buffer);
    lc_vulkan_buffer_destroy(buffer);
    free(buffer);
}

uint64_t lc_buffer_get_size(const lc_buffer *buffer) {
    if (buffer == NULL) {
        return 0;
    }
    return buffer->size;
}

void lc_buffer_get_memory_info(const lc_buffer *buffer,
                               lc_resource_memory_info *out_info) {
    if (out_info == NULL) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (buffer == NULL || !lc_is_live_buffer(buffer)) {
        return;
    }
    out_info->requested_size = buffer->size;
    out_info->allocation_size = buffer->allocation_size;
    out_info->dedicated = buffer->memory_dedicated;
    out_info->memory_class = buffer->memory_class;
}

lc_result lc_buffer_map(lc_buffer *buffer, void **out_data) {
    if (buffer == NULL || out_data == NULL) {
        if (out_data != NULL) {
            *out_data = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_is_live_buffer(buffer)) {
        *out_data = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Only CPU-visible placements are persistently mapped; GPU-only
     * mapping is rejected rather than emulated. Non-coherent ranges
     * invalidate before CPU access (PART W). */
    if (buffer->mapped_ptr == NULL) {
        *out_data = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (lc_vulkan_buffer_invalidate(buffer) != LC_SUCCESS) {
        *out_data = NULL;
        return LC_ERROR_UNKNOWN;
    }
    *out_data = buffer->mapped_ptr;
    return LC_SUCCESS;
}

void lc_buffer_unmap(lc_buffer *buffer) {
    /* Persistent mapping: intentionally a no-op on every path
     * (NULL, dead, GPU-only, or live CPU-visible alike). Memory is
     * unmapped once, at destroy time. */
    (void)buffer;
}

lc_result lc_buffer_write(lc_buffer *buffer, uint64_t offset, const void *data,
                          uint64_t size) {
    if (buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_is_live_buffer(buffer)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0) {
        return LC_SUCCESS;
    }
    if (data == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Overflow-safe bounds check: offset first, then the remainder. */
    if (offset > buffer->size || size > buffer->size - offset) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_buffer_write(buffer, offset, data, size);
}

void lc_buffer_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->buffers != NULL) {
        lc_buffer_destroy(state->buffers);
    }
}

void lc_buffer_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_buffer *it;
    lc_buffer *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->buffers; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_buffer_destroy(it);
        }
    }
}
