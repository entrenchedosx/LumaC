#include <stdlib.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public image-view API + lifetime tracking (Phase 10).
 *
 * Views borrow their image (and thereby its device). Image liveness
 * is verified before any dereference; image teardown and device
 * teardown destroy dependent views first, so a view never outlives
 * its VkImage. Deep descriptor validation lives in the backend
 * (vulkan_image.c), shared by every creation path.
 */

static void lc_image_view_list_add(lc_image_view *view) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || view == NULL) {
        return;
    }
    view->next = state->image_views;
    view->prev = NULL;
    if (state->image_views != NULL) {
        state->image_views->prev = view;
    }
    state->image_views = view;
}

static void lc_image_view_list_remove(lc_image_view *view) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || view == NULL) {
        return;
    }
    if (view->prev != NULL) {
        view->prev->next = view->next;
    } else if (state->image_views == view) {
        state->image_views = view->next;
    }
    if (view->next != NULL) {
        view->next->prev = view->prev;
    }
    view->next = NULL;
    view->prev = NULL;
}

static int lc_is_live_image(const lc_image *image) {
    lc_state *state = lc_get_internal_state();
    const lc_image *it;

    if (state == NULL || image == NULL) {
        return 0;
    }
    for (it = state->images; it != NULL; it = it->next) {
        if (it == image) {
            return 1;
        }
    }
    return 0;
}

lc_result lc_image_view_create(lc_image *image,
                               const lc_image_view_desc *desc,
                               lc_image_view **out_view) {
    lc_state *state = lc_get_internal_state();
    lc_image_view *view;
    lc_result res;

    if (image == NULL || desc == NULL || out_view == NULL) {
        if (out_view != NULL) {
            *out_view = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_view = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    /* Liveness before any dereference of the caller handle. */
    if (!lc_is_live_image(image)) {
        *out_view = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }

    view = (lc_image_view *)calloc(1, sizeof(lc_image_view));
    if (view == NULL) {
        *out_view = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    view->resource_id = lc_issue_resource_id();

    res = lc_vulkan_image_view_create(view, image, desc);
    if (res != LC_SUCCESS) {
        *out_view = NULL;
        free(view);
        return res;
    }

    lc_image_view_list_add(view);
    *out_view = view;
    return LC_SUCCESS;
}

void lc_image_view_destroy(lc_image_view *view) {
    if (view == NULL) {
        return;
    }
    /* Render targets borrow views: dependents die first so no stale
     * attachment pointer survives. */
    lc_render_target_destroy_for_view(view);
    lc_image_view_list_remove(view);
    lc_vulkan_image_view_destroy(view);
    free(view);
}

void lc_image_view_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->image_views != NULL) {
        lc_image_view_destroy(state->image_views);
    }
}

void lc_image_view_destroy_for_image(const lc_image *image) {
    lc_state *state = lc_get_internal_state();
    lc_image_view *it;
    lc_image_view *next;

    if (state == NULL || image == NULL) {
        return;
    }
    for (it = state->image_views; it != NULL; it = next) {
        next = it->next;
        if (it->image == image) {
            lc_image_view_destroy(it);
        }
    }
}

void lc_image_view_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_image_view *it;
    lc_image_view *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->image_views; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_image_view_destroy(it);
        }
    }
}
