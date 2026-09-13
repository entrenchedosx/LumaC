/* Stable resource identity getters + borrowed view->image accessor.
 *
 * Every getter returns 0 for NULL or dead handles (never crashes).
 * IDs are assigned once at creation from a process-monotonic counter
 * and never reused, so allocator pointer recycling can never alias
 * two live-epoch resources. Caches must compare these IDs.
 */

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

static int lc_is_live_in_list(const void *handle, const void *head,
                              size_t next_off) {
    const char *it;

    if (handle == NULL || head == NULL) {
        return 0;
    }
    for (it = (const char *)head; it != NULL;
         it = *(const char *const *)(it + next_off)) {
        if (it == (const char *)handle) {
            return 1;
        }
    }
    return 0;
}

/* next/prev are adjacent in every resource struct (next first). */
#define LC_NEXT_OFF(struct_type) ((size_t) & ((struct_type *)0)->next)

static int lc_image_live(const lc_image *image) {
    lc_state *state = lc_get_internal_state();

    if (state == NULL || image == NULL) {
        return 0;
    }
    return lc_is_live_in_list(image, state->images,
                              LC_NEXT_OFF(lc_image));
}

static int lc_view_live(const lc_image_view *view) {
    lc_state *state = lc_get_internal_state();

    if (state == NULL || view == NULL) {
        return 0;
    }
    return lc_is_live_in_list(view, state->image_views,
                              LC_NEXT_OFF(lc_image_view));
}

static int lc_buffer_live(const lc_buffer *buffer) {
    lc_state *state = lc_get_internal_state();

    if (state == NULL || buffer == NULL) {
        return 0;
    }
    return lc_is_live_in_list(buffer, state->buffers,
                              LC_NEXT_OFF(lc_buffer));
}

static int lc_sampler_live(const lc_sampler *sampler) {
    lc_state *state = lc_get_internal_state();

    if (state == NULL || sampler == NULL) {
        return 0;
    }
    return lc_is_live_in_list(sampler, state->samplers,
                              LC_NEXT_OFF(lc_sampler));
}

static int lc_pipeline_live(const lc_pipeline *pipeline) {
    lc_state *state = lc_get_internal_state();

    if (state == NULL || pipeline == NULL) {
        return 0;
    }
    return lc_is_live_in_list(pipeline, state->pipelines,
                              LC_NEXT_OFF(lc_pipeline));
}

static int lc_set_live(const lc_binding_set *set) {
    lc_state *state = lc_get_internal_state();

    if (state == NULL || set == NULL) {
        return 0;
    }
    return lc_is_live_in_list(set, state->binding_sets,
                              LC_NEXT_OFF(lc_binding_set));
}

static int lc_layout_live(const lc_binding_layout *layout) {
    lc_state *state = lc_get_internal_state();

    if (state == NULL || layout == NULL) {
        return 0;
    }
    return lc_is_live_in_list(layout, state->binding_layouts,
                              LC_NEXT_OFF(lc_binding_layout));
}

lc_resource_id lc_image_get_resource_id(const lc_image *image) {
    if (!lc_image_live(image)) {
        return 0;
    }
    return image->resource_id;
}

lc_resource_id lc_image_view_get_resource_id(const lc_image_view *view) {
    if (!lc_view_live(view)) {
        return 0;
    }
    return view->resource_id;
}

lc_resource_id lc_buffer_get_resource_id(const lc_buffer *buffer) {
    if (!lc_buffer_live(buffer)) {
        return 0;
    }
    return buffer->resource_id;
}

lc_resource_id lc_sampler_get_resource_id(const lc_sampler *sampler) {
    if (!lc_sampler_live(sampler)) {
        return 0;
    }
    return sampler->resource_id;
}

lc_resource_id lc_render_target_get_resource_id(
    const lc_render_target *target) {
    lc_state *state = lc_get_internal_state();
    const lc_swapchain *sw;

    if (target == NULL || state == NULL) {
        return 0;
    }
    /* Borrowed swapchain targets live inline (never tracked): match
     * by address against live swapchains. */
    for (sw = state->swapchains; sw != NULL; sw = sw->next) {
        if (target == &sw->swapchain_target) {
            return target->resource_id;
        }
    }
    return lc_is_live_in_list(target, state->render_targets,
                              LC_NEXT_OFF(lc_render_target))
               ? target->resource_id
               : 0;
}

lc_resource_id lc_pipeline_get_resource_id(const lc_pipeline *pipeline) {
    if (!lc_pipeline_live(pipeline)) {
        return 0;
    }
    return pipeline->resource_id;
}

lc_resource_id lc_binding_set_get_resource_id(const lc_binding_set *set) {
    if (!lc_set_live(set)) {
        return 0;
    }
    return set->resource_id;
}

lc_resource_id lc_binding_layout_get_resource_id(
    const lc_binding_layout *layout) {
    if (!lc_layout_live(layout)) {
        return 0;
    }
    return layout->resource_id;
}

lc_image *lc_image_view_get_image(lc_image_view *view) {
    if (!lc_view_live(view)) {
        return NULL;
    }
    /* The image outlives its views by contract (image destroy kills
     * dependent views first), but verify liveness defensively. */
    if (!lc_image_live(view->image)) {
        return NULL;
    }
    return view->image;
}
