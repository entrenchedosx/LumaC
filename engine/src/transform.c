/*
 * Luma Engine transforms: universal per-object position/quaternion/
 * scale + cached world matrices with iterative dirty propagation.
 */

#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"

void le_object_get_position(const le_world *world, const le_object *object,
                            float out_position[3]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_position == NULL) {
        return;
    }
    out_position[0] = 0.0f;
    out_position[1] = 0.0f;
    out_position[2] = 0.0f;
    if (world == NULL || object == NULL) {
        return;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return;
    }
    out_position[0] = world->slots[slot].position[0];
    out_position[1] = world->slots[slot].position[1];
    out_position[2] = world->slots[slot].position[2];
}

le_result le_object_set_position(le_world *world, const le_object *object,
                                 const float position[3]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int i;

    if (world == NULL || object == NULL || position == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    for (i = 0; i < 3; i++) {
        if (!isfinite(position[i])) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    }
    world->slots[slot].position[0] = position[0];
    world->slots[slot].position[1] = position[1];
    world->slots[slot].position[2] = position[2];
    le_mark_subtree_dirty(world, slot);
    return LE_SUCCESS;
}

void le_object_get_rotation(const le_world *world, const le_object *object,
                            float out_rotation[4]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_rotation == NULL) {
        return;
    }
    out_rotation[0] = 0.0f;
    out_rotation[1] = 0.0f;
    out_rotation[2] = 0.0f;
    out_rotation[3] = 1.0f;
    if (world == NULL || object == NULL) {
        return;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return;
    }
    out_rotation[0] = world->slots[slot].rotation[0];
    out_rotation[1] = world->slots[slot].rotation[1];
    out_rotation[2] = world->slots[slot].rotation[2];
    out_rotation[3] = world->slots[slot].rotation[3];
}

le_result le_object_set_rotation(le_world *world, const le_object *object,
                                 const float rotation[4]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    float clean[4];

    if (world == NULL || object == NULL || rotation == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    /* Normalize on store (zero-length/non-finite restores
     * identity): canonical storage is always unit length. */
    le_quat_normalize(rotation, clean);
    world->slots[slot].rotation[0] = clean[0];
    world->slots[slot].rotation[1] = clean[1];
    world->slots[slot].rotation[2] = clean[2];
    world->slots[slot].rotation[3] = clean[3];
    le_mark_subtree_dirty(world, slot);
    return LE_SUCCESS;
}

void le_object_get_scale(const le_world *world, const le_object *object,
                         float out_scale[3]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_scale == NULL) {
        return;
    }
    out_scale[0] = 1.0f;
    out_scale[1] = 1.0f;
    out_scale[2] = 1.0f;
    if (world == NULL || object == NULL) {
        return;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return;
    }
    out_scale[0] = world->slots[slot].scale[0];
    out_scale[1] = world->slots[slot].scale[1];
    out_scale[2] = world->slots[slot].scale[2];
}

le_result le_object_set_scale(le_world *world, const le_object *object,
                              const float scale[3]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int i;

    if (world == NULL || object == NULL || scale == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    for (i = 0; i < 3; i++) {
        if (!isfinite(scale[i])) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    }
    world->slots[slot].scale[0] = scale[0];
    world->slots[slot].scale[1] = scale[1];
    world->slots[slot].scale[2] = scale[2];
    le_mark_subtree_dirty(world, slot);
    return LE_SUCCESS;
}

void le_object_get_local_matrix(const le_world *world,
                                const le_object *object,
                                float out_matrix[16]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_matrix == NULL) {
        return;
    }
    le_mat4_identity(out_matrix);
    if (world == NULL || object == NULL) {
        return;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return;
    }
    le_transform_compose(world->slots[slot].position,
                         world->slots[slot].rotation,
                         world->slots[slot].scale, out_matrix);
}

void le_object_get_world_matrix(const le_world *world,
                                const le_object *object,
                                float out_matrix[16]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_matrix == NULL) {
        return;
    }
    le_mat4_identity(out_matrix);
    if (world == NULL || object == NULL) {
        return;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return;
    }
    /* Ancestors-first refresh along this object's chain only
     * (iterative, heap-backed for absurd depths): reads stay
     * O(depth) without forcing a whole-world refresh. The const
     * cast is disciplined: world-matrix caching is a documented
     * internal memo, invisible to callers. */
    {
        le_world *mutable_world = (le_world *)world;
        /* Collect the root-ward chain (bounded heap walk, no
         * recursion). */
        uint32_t chain_static[256];
        uint32_t *chain = chain_static;
        size_t chain_cap = 256;
        size_t chain_len = 0;
        int32_t cur = (int32_t)slot;
        int heap = 0;

        while (cur != LE_NO_LINK && cur >= 0 &&
               (uint32_t)cur < mutable_world->capacity &&
               mutable_world->slots[cur].alive) {
            if (chain_len >= chain_cap) {
                size_t grown = chain_cap * 2u;
                uint32_t *fresh;

                if (grown < chain_cap + 256u) {
                    grown = chain_cap + 256u;
                }
                fresh =
                    (uint32_t *)malloc(grown * sizeof(uint32_t));
                if (fresh == NULL) {
                    break;
                }
                if (!heap) {
                    memcpy(fresh, chain_static, sizeof(chain_static));
                    heap = 1;
                } else {
                    memcpy(fresh, chain, chain_len * sizeof(uint32_t));
                    free(chain);
                }
                chain = fresh;
                chain_cap = grown;
            }
            chain[chain_len++] = (uint32_t)cur;
            if (!mutable_world->slots[cur].dirty) {
                break;
            }
            cur = mutable_world->slots[cur].parent;
        }
        while (chain_len > 0) {
            chain_len--;
            le_compose_slot_world(mutable_world, chain[chain_len]);
        }
        if (heap) {
            free(chain);
        }
    }
    memcpy(out_matrix, world->slots[slot].world_matrix,
           sizeof(float) * 16u);
}
