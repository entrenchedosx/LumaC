/*
 * Renderer draw list (Phase 13): per-frame queue with CPU frustum
 * culling and opaque sorting (material, then mesh) to skip redundant
 * binds. Sorted indices resolve at render time; dead entries are
 * skipped defensively (never dereferenced).
 */

#include <stdlib.h>
#include <string.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

static void lr_world_sphere(const float matrix[16], const float center[3],
                            float radius, const float scale[3],
                            float out_center[3], float *out_radius) {
    float max_scale = scale[0];
    int i;

    out_center[0] = matrix[0] * center[0] + matrix[4] * center[1] +
                    matrix[8] * center[2] + matrix[12];
    out_center[1] = matrix[1] * center[0] + matrix[5] * center[1] +
                    matrix[9] * center[2] + matrix[13];
    out_center[2] = matrix[2] * center[0] + matrix[6] * center[1] +
                    matrix[10] * center[2] + matrix[14];
    for (i = 1; i < 3; i++) {
        float s = (scale[i] < 0.0f) ? -scale[i] : scale[i];

        if (s > max_scale) {
            max_scale = s;
        }
    }
    if (max_scale < 0.0f) {
        max_scale = -max_scale;
    }
    *out_radius = radius * max_scale;
}

lr_result lr_renderer_submit(lr_renderer *renderer,
                             const lr_draw_item *item) {
    lr_queued_item *slot;
    float matrix[16];

    if (renderer == NULL || item == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!renderer->frame_open || renderer->shadows_prepared) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (item->mesh == NULL || item->material == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Ownership + liveness from renderer-local registries (no global
     * state, safe against cross-renderer or destroyed objects). */
    if (!lr_mesh_is_live(renderer, item->mesh) ||
        !lr_material_is_live(renderer, item->material)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->queued >= renderer->max_objects) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    lr_transform_to_matrix(&item->transform, matrix);

    renderer->stats.submitted_objects++;
    slot = &renderer->queue[renderer->queued];
    slot->mesh = item->mesh;
    slot->material = item->material;
    memcpy(slot->matrix, matrix, sizeof(matrix));
    slot->casts_shadow = (item->casts_shadow != 0) ? 1 : 0;
    slot->receives_shadow = (item->receives_shadow != 0) ? 1 : 0;
    lr_world_sphere(matrix, item->mesh->bounds.center,
                    item->mesh->bounds.radius, item->transform.scale,
                    slot->sphere_center, &slot->sphere_radius);
    /* Every submission is stored (up to max_objects) so off-screen
     * casters still reach shadow passes; main-pass visibility is
     * just a flag now. */
    if (!lr_frustum_test_sphere(renderer->frustum_planes,
                                slot->sphere_center, slot->sphere_radius)) {
        slot->main_visible = 0;
    } else {
        slot->main_visible = 1;
        renderer->stats.visible_objects++;
    }
    renderer->queued++;
    return LR_SUCCESS;
}

/* Opaque order: shared material first (set binds dominate), then
 * shared mesh (vertex/index binds). */
static int lr_queue_compare(const void *a, const void *b) {
    const lr_queued_item *qa = (const lr_queued_item *)a;
    const lr_queued_item *qb = (const lr_queued_item *)b;

    if (qa->material < qb->material) {
        return -1;
    }
    if (qa->material > qb->material) {
        return 1;
    }
    if (qa->mesh < qb->mesh) {
        return -1;
    }
    if (qa->mesh > qb->mesh) {
        return 1;
    }
    return 0;
}

void lr_queue_sort(lr_queued_item *items, uint32_t count) {
    if (items == NULL || count < 2) {
        return;
    }
    qsort(items, count, sizeof(lr_queued_item), lr_queue_compare);
}
