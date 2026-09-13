#include <stdlib.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public render-target API + lifetime tracking (Phase 12).
 *
 * Offscreen targets borrow their views (non-owning); destroying a view
 * first invalidates dependent targets (destroyed via the for_view
 * hook, so use-after-destroy is rejected without dereferencing freed
 * memory). Pipelines hold only structural signatures, never target
 * objects. The borrowed swapchain target lives inline in lc_swapchain
 * and is refreshed on every rebuild; it is never tracked here.
 */

static void lc_render_target_list_add(lc_render_target *target) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || target == NULL) {
        return;
    }
    target->next = state->render_targets;
    target->prev = NULL;
    if (state->render_targets != NULL) {
        state->render_targets->prev = target;
    }
    state->render_targets = target;
}

static void lc_render_target_list_remove(lc_render_target *target) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || target == NULL) {
        return;
    }
    if (target->prev != NULL) {
        target->prev->next = target->next;
    } else if (state->render_targets == target) {
        state->render_targets = target->next;
    }
    if (target->next != NULL) {
        target->next->prev = target->prev;
    }
    target->next = NULL;
    target->prev = NULL;
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

static int lc_is_live_view(const lc_image_view *view) {
    lc_state *state = lc_get_internal_state();
    const lc_image_view *it;

    if (state == NULL || view == NULL) {
        return 0;
    }
    for (it = state->image_views; it != NULL; it = it->next) {
        if (it == view) {
            return 1;
        }
    }
    return 0;
}

int lc_render_target_is_live(const lc_render_target *target) {
    lc_state *state = lc_get_internal_state();
    const lc_render_target *it;

    if (state == NULL || target == NULL) {
        return 0;
    }
    /* Identity comparison only: borrowed swapchain targets are never
     * tracked (callers resolve those via their swapchain), and dead
     * handles simply miss. Never dereferences the candidate. */
    for (it = state->render_targets; it != NULL; it = it->next) {
        if (it == target) {
            return 1;
        }
    }
    return 0;
}

/* FNV-1a over count, color formats, depth format, samples. */
uint32_t lc_render_target_hash(uint32_t color_count,
                               const lc_format *color_formats,
                               lc_format depth_format,
                               lc_sample_count samples) {
    uint32_t hash = 2166136261u;
    uint32_t i;

    hash ^= color_count;
    hash *= 16777619u;
    for (i = 0; i < color_count; i++) {
        hash ^= (uint32_t)color_formats[i];
        hash *= 16777619u;
    }
    hash ^= (uint32_t)depth_format;
    hash *= 16777619u;
    hash ^= (uint32_t)samples;
    hash *= 16777619u;
    return hash;
}

int lc_render_target_desc_equal(const lc_render_target_desc *a,
                                const lc_render_target_desc *b) {
    uint32_t i;

    if (a == NULL || b == NULL) {
        return 0;
    }
    /* Width/height are deliberately excluded: pipelines are
     * extent-independent (dynamic viewport/scissor). */
    if (a->color_attachment_count != b->color_attachment_count ||
        a->depth_stencil_format != b->depth_stencil_format ||
        a->samples != b->samples) {
        return 0;
    }
    for (i = 0; i < a->color_attachment_count; i++) {
        if (a->color_formats[i] != b->color_formats[i]) {
            return 0;
        }
    }
    return 1;
}

lc_result lc_render_target_create(
    lc_device *device, const lc_render_target_create_desc *desc,
    lc_render_target **out_target) {
    lc_state *state = lc_get_internal_state();
    lc_render_target *target;
    lc_result res;

    if (device == NULL || desc == NULL || out_target == NULL) {
        if (out_target != NULL) {
            *out_target = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_target = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_is_live_device(device)) {
        *out_target = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Basic shape checks before touching views (deep content lands in
     * the backend with full image state visible). */
    if (desc->width == 0 || desc->height == 0) {
        *out_target = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->color_attachment_count > LC_MAX_COLOR_ATTACHMENTS ||
        (desc->color_attachment_count == 0 &&
         desc->depth_stencil_attachment == NULL)) {
        *out_target = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->color_attachment_count > 0 &&
        desc->color_attachments == NULL) {
        *out_target = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Liveness before any dereference of caller views. */
    {
        uint32_t i;

        for (i = 0; i < desc->color_attachment_count; i++) {
            if (desc->color_attachments[i].view == NULL ||
                !lc_is_live_view(desc->color_attachments[i].view)) {
                *out_target = NULL;
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
        if (desc->depth_stencil_attachment != NULL &&
            !lc_is_live_view(desc->depth_stencil_attachment)) {
            *out_target = NULL;
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }

    target = (lc_render_target *)calloc(1, sizeof(lc_render_target));
    if (target == NULL) {
        *out_target = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    target->resource_id = lc_issue_resource_id();

    res = lc_vulkan_render_target_create(target, device, desc);
    if (res != LC_SUCCESS) {
        *out_target = NULL;
        free(target);
        return res;
    }

    lc_render_target_list_add(target);
    *out_target = target;
    return LC_SUCCESS;
}

void lc_render_target_destroy(lc_render_target *target) {
    if (target == NULL) {
        return;
    }
    /* Borrowed swapchain targets are never destroyed directly; still
     * remove defensively if one ever arrives here untracked. */
    if (target->is_swapchain_borrow) {
        return;
    }
    lc_render_target_list_remove(target);
    lc_vulkan_render_target_destroy(target);
    free(target);
}

void lc_render_target_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->render_targets != NULL) {
        lc_render_target_destroy(state->render_targets);
    }
}

void lc_render_target_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_render_target *it;
    lc_render_target *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->render_targets; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_render_target_destroy(it);
        }
    }
}

void lc_render_target_destroy_for_view(const lc_image_view *view) {
    lc_state *state = lc_get_internal_state();
    lc_render_target *it;
    lc_render_target *next;

    if (state == NULL || view == NULL) {
        return;
    }
    for (it = state->render_targets; it != NULL; it = next) {
        uint32_t i;
        int hit = 0;

        next = it->next;
        if (it->depth_view == view) {
            hit = 1;
        }
        for (i = 0; !hit && i < it->color_count; i++) {
            if (it->color_views[i] == view) {
                hit = 1;
            }
        }
        if (hit) {
            lc_render_target_destroy(it);
        }
    }
}

uint32_t lc_render_target_get_width(const lc_render_target *target) {
    if (target == NULL) {
        return 0;
    }
    return target->width;
}

uint32_t lc_render_target_get_height(const lc_render_target *target) {
    if (target == NULL) {
        return 0;
    }
    return target->height;
}

uint32_t lc_render_target_get_color_count(const lc_render_target *target) {
    if (target == NULL) {
        return 0;
    }
    return target->color_count;
}

lc_format lc_render_target_get_color_format(const lc_render_target *target,
                                            uint32_t index) {
    if (target == NULL || index >= target->color_count) {
        return LC_FORMAT_UNDEFINED;
    }
    return target->color_formats[index];
}

lc_format lc_render_target_get_depth_format(const lc_render_target *target) {
    if (target == NULL) {
        return LC_FORMAT_UNDEFINED;
    }
    return target->depth_format;
}

lc_sample_count
lc_render_target_get_samples(const lc_render_target *target) {
    if (target == NULL) {
        return (lc_sample_count)0;
    }
    return target->samples;
}

lc_image_view *lc_render_target_get_color_view(
    const lc_render_target *target, uint32_t index) {
    lc_state *state = lc_get_internal_state();
    const lc_swapchain *it;

    if (target == NULL) {
        return NULL;
    }
    /* Comparison-only liveness (never dereference a possibly-dead
     * handle): borrowed swapchain targets resolve via their parent;
     * offscreen targets via the tracked list. */
    if (state != NULL) {
        for (it = state->swapchains; it != NULL; it = it->next) {
            if (target == &it->swapchain_target) {
                return NULL; /* swapchain views are internal (no lc views) */
            }
        }
    }
    if (!lc_render_target_is_live(target) || index >= target->color_count) {
        return NULL;
    }
    return target->color_views[index];
}

lc_image_view *
lc_render_target_get_depth_view(const lc_render_target *target) {
    lc_state *state = lc_get_internal_state();
    const lc_swapchain *it;

    if (target == NULL) {
        return NULL;
    }
    if (state != NULL) {
        for (it = state->swapchains; it != NULL; it = it->next) {
            if (target == &it->swapchain_target) {
                return NULL;
            }
        }
    }
    if (!lc_render_target_is_live(target)) {
        return NULL;
    }
    return target->depth_view;
}

int lc_render_target_is_compatible_with_pipeline(
    const lc_render_target *target, const lc_pipeline *pipeline) {
    lc_render_target_desc desc;
    const lc_swapchain *it;
    lc_state *state = lc_get_internal_state();
    int borrowed = 0;

    if (target == NULL || pipeline == NULL) {
        return 0;
    }
    /* Liveness by pointer comparison only (never dereference a
     * possibly-dead handle): borrowed swapchain targets live inline
     * in a live swapchain; offscreen targets live in the tracked
     * list. Anything else is safely incompatible. */
    if (state != NULL) {
        for (it = state->swapchains; it != NULL; it = it->next) {
            if (target == &it->swapchain_target) {
                borrowed = 1;
                break;
            }
        }
    }
    if (!borrowed && !lc_render_target_is_live(target)) {
        return 0;
    }
    if (!lc_pipeline_is_live(pipeline)) {
        return 0;
    }
    if (target->device != pipeline->device) {
        return 0;
    }
    desc.width = target->width;
    desc.height = target->height;
    desc.color_attachment_count = target->color_count;
    {
        uint32_t i;

        for (i = 0; i < target->color_count; i++) {
            desc.color_formats[i] = target->color_formats[i];
        }
    }
    desc.depth_stencil_format = target->depth_format;
    desc.samples = target->samples;
    /* Fast hash reject, structural compare to resolve collisions. */
    if (lc_render_target_hash(desc.color_attachment_count,
                              desc.color_formats, desc.depth_stencil_format,
                              desc.samples) != pipeline->target_hash) {
        return 0;
    }
    {
        lc_render_target_desc pipe_desc;

        pipe_desc.width = 0;
        pipe_desc.height = 0;
        pipe_desc.color_attachment_count = pipeline->target_color_count;
        {
            uint32_t i;

            for (i = 0; i < pipeline->target_color_count; i++) {
                pipe_desc.color_formats[i] =
                    pipeline->target_color_formats[i];
            }
        }
        pipe_desc.depth_stencil_format = pipeline->target_depth_format;
        pipe_desc.samples = pipeline->target_samples;
        return lc_render_target_desc_equal(&desc, &pipe_desc);
    }
}

lc_render_target *lc_swapchain_get_render_target(lc_swapchain *swapchain) {
    if (swapchain == NULL) {
        return NULL;
    }
    /* Borrowed inline snapshot; valid until the next recreate/destroy.
     * Liveness is the swapchain's liveness (callers hold it). */
    return &swapchain->swapchain_target;
}

lc_result lc_swapchain_get_encoder(lc_swapchain *swapchain,
                                   lc_command_encoder **out_encoder) {
    lc_state *state = lc_get_internal_state();

    if (swapchain == NULL || out_encoder == NULL) {
        if (out_encoder != NULL) {
            *out_encoder = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_encoder = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_is_live_swapchain(swapchain)) {
        *out_encoder = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!swapchain->frame_active) {
        *out_encoder = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    *out_encoder = &swapchain->encoder;
    return LC_SUCCESS;
}
