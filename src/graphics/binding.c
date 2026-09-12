#include <stdlib.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public binding API + lifetime tracking (Phase 10).
 *
 * Binding layouts describe shader resource slots; binding sets are
 * populated instances. Handle liveness is verified before any
 * dereference. Deep content validation (slots, types, bounds, ranges,
 * alignment, sampled state) lives in the backend (vulkan_binding.c),
 * which sees full state. Layouts and sets are device-owned; the
 * pipeline/frame layers compare layout anchors without dereferencing
 * dead objects.
 */

static void lc_binding_layout_list_add(lc_binding_layout *layout) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || layout == NULL) {
        return;
    }
    layout->next = state->binding_layouts;
    layout->prev = NULL;
    if (state->binding_layouts != NULL) {
        state->binding_layouts->prev = layout;
    }
    state->binding_layouts = layout;
}

static void lc_binding_layout_list_remove(lc_binding_layout *layout) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || layout == NULL) {
        return;
    }
    if (layout->prev != NULL) {
        layout->prev->next = layout->next;
    } else if (state->binding_layouts == layout) {
        state->binding_layouts = layout->next;
    }
    if (layout->next != NULL) {
        layout->next->prev = layout->prev;
    }
    layout->next = NULL;
    layout->prev = NULL;
}

static void lc_binding_set_list_add(lc_binding_set *set) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || set == NULL) {
        return;
    }
    set->next = state->binding_sets;
    set->prev = NULL;
    if (state->binding_sets != NULL) {
        state->binding_sets->prev = set;
    }
    state->binding_sets = set;
}

static void lc_binding_set_list_remove(lc_binding_set *set) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || set == NULL) {
        return;
    }
    if (set->prev != NULL) {
        set->prev->next = set->next;
    } else if (state->binding_sets == set) {
        state->binding_sets = set->next;
    }
    if (set->next != NULL) {
        set->next->prev = set->prev;
    }
    set->next = NULL;
    set->prev = NULL;
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

static int lc_is_live_binding_layout(const lc_binding_layout *layout) {
    lc_state *state = lc_get_internal_state();
    const lc_binding_layout *it;

    if (state == NULL || layout == NULL) {
        return 0;
    }
    for (it = state->binding_layouts; it != NULL; it = it->next) {
        if (it == layout) {
            return 1;
        }
    }
    return 0;
}

static int lc_is_live_binding_set(const lc_binding_set *set) {
    lc_state *state = lc_get_internal_state();
    const lc_binding_set *it;

    if (state == NULL || set == NULL) {
        return 0;
    }
    for (it = state->binding_sets; it != NULL; it = it->next) {
        if (it == set) {
            return 1;
        }
    }
    return 0;
}

lc_result lc_binding_layout_create(lc_device *device,
                                   const lc_binding_layout_desc *desc,
                                   lc_binding_layout **out_layout) {
    lc_state *state = lc_get_internal_state();
    lc_binding_layout *layout;
    lc_result res;

    if (device == NULL || desc == NULL || out_layout == NULL) {
        if (out_layout != NULL) {
            *out_layout = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_layout = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    /* Liveness before any dereference of the caller handle. */
    if (!lc_is_live_device(device)) {
        *out_layout = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }

    layout = (lc_binding_layout *)calloc(1, sizeof(lc_binding_layout));
    if (layout == NULL) {
        *out_layout = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }

    res = lc_vulkan_binding_layout_create(layout, device, desc);
    if (res != LC_SUCCESS) {
        *out_layout = NULL;
        free(layout);
        return res;
    }

    lc_binding_layout_list_add(layout);
    *out_layout = layout;
    return LC_SUCCESS;
}

void lc_binding_layout_destroy(lc_binding_layout *layout) {
    if (layout == NULL) {
        return;
    }
    lc_binding_layout_list_remove(layout);
    lc_vulkan_binding_layout_destroy(layout);
    free(layout);
}

void lc_binding_layout_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->binding_layouts != NULL) {
        lc_binding_layout_destroy(state->binding_layouts);
    }
}

void lc_binding_layout_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_binding_layout *it;
    lc_binding_layout *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->binding_layouts; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_binding_layout_destroy(it);
        }
    }
}

lc_result lc_binding_set_create(lc_binding_layout *layout,
                                lc_binding_set **out_set) {
    lc_state *state = lc_get_internal_state();
    lc_binding_set *set;
    lc_result res;

    if (layout == NULL || out_set == NULL) {
        if (out_set != NULL) {
            *out_set = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_set = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_is_live_binding_layout(layout)) {
        *out_set = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }

    set = (lc_binding_set *)calloc(1, sizeof(lc_binding_set));
    if (set == NULL) {
        *out_set = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }

    res = lc_vulkan_binding_set_create(set, layout);
    if (res != LC_SUCCESS) {
        *out_set = NULL;
        free(set);
        return res;
    }

    lc_binding_set_list_add(set);
    *out_set = set;
    return LC_SUCCESS;
}

void lc_binding_set_destroy(lc_binding_set *set) {
    if (set == NULL) {
        return;
    }
    lc_binding_set_list_remove(set);
    lc_vulkan_binding_set_destroy(set);
    free(set);
}

void lc_binding_set_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->binding_sets != NULL) {
        lc_binding_set_destroy(state->binding_sets);
    }
}

void lc_binding_set_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_binding_set *it;
    lc_binding_set *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->binding_sets; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_binding_set_destroy(it);
        }
    }
}

lc_result lc_binding_set_update(lc_binding_set *set,
                                const lc_binding_write *writes,
                                uint32_t write_count) {
    lc_state *state = lc_get_internal_state();

    if (set == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_is_live_binding_set(set)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (write_count > 0 && writes == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* The backend validates every write before recording anything,
     * so a rejected batch leaves prior state untouched. */
    return lc_vulkan_binding_set_update(set, writes, write_count);
}
