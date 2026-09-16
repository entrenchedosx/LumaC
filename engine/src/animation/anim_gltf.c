/*
 * glTF skin/animation import (Phase 29): builds immutable
 * skeleton/clip payloads from cgltf parse results. Struct-BLIND
 * to cgltf (the engine gltf bridge passes opaque pointers after
 * including cgltf itself) — this TU defines minimal mirror
 * structs matching the cgltf layout it reads. Field offsets are
 * verified by the bridge (which passes real cgltf pointers), so
 * ANY cgltf layout change breaks the bridge build first... to
 * avoid that fragility, the bridge instead calls the TYPED
 * helpers below through its own TU. This file hosts the VALIDATE
 * + CONVERT core over plain accessor arrays provided by the
 * caller (assets module owns accessor decoding).
 *
 * Split of duties:
 * - assets/src/gltf_import.c: parses, decodes accessors, validates
 *   container-level structure (existing code, reused).
 * - engine bridge (gltf_bridge_anim.c): maps cgltf skin/animation
 *   indices to engine calls, passing DECODED float arrays.
 * - this file: validates animation-level semantics (hierarchy,
 *   times, quats) and builds le_skeleton_data / le_clip_data.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "animation/animation_internal.h"
#include "animation/anim_math.h"

/* Decoded skin input (caller-decoded from cgltf accessors):
 * - joint_names[joint_count] (may be NULL entries = "")
 * - parents[joint_count] (node-hierarchy derived, -1 root)
 * - bind_t/r/s[joint_count] (node local TRS)
 * - inverse_bind[joint_count * 16] (or NULL = identity) */
le_result le_anim_build_skeleton_decoded(
    const char **joint_names, const int32_t *parents,
    const float (*bind_t)[3], const float (*bind_r)[4],
    const float (*bind_s)[3], const float (*inverse_bind)[16],
    uint32_t joint_count, struct le_skeleton_data **out) {
    le_skeleton_asset_desc desc;
    le_skeleton_joint_desc *joints = NULL;
    uint32_t i;
    le_result rc;

    if (out != NULL) {
        *out = NULL;
    }
    if (joint_names == NULL || parents == NULL || bind_t == NULL ||
        bind_r == NULL || bind_s == NULL || out == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (joint_count == 0 || joint_count > LE_ANIM_MAX_JOINTS) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    joints = (le_skeleton_joint_desc *)calloc(
        joint_count, sizeof(*joints));
    if (joints == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < joint_count; i++) {
        if (joint_names[i] != NULL) {
            size_t n = strlen(joint_names[i]);

            if (n >= sizeof(joints[i].name)) {
                n = sizeof(joints[i].name) - 1u;
            }
            memcpy(joints[i].name, joint_names[i], n);
        }
        joints[i].parent = parents[i];
        memcpy(joints[i].translation, bind_t[i],
               sizeof(joints[i].translation));
        memcpy(joints[i].rotation, bind_r[i],
               sizeof(joints[i].rotation));
        memcpy(joints[i].scale, bind_s[i],
               sizeof(joints[i].scale));
        if (inverse_bind != NULL) {
            memcpy(joints[i].inverse_bind, inverse_bind[i],
                   sizeof(joints[i].inverse_bind));
        } else {
            memset(joints[i].inverse_bind, 0,
                   sizeof(joints[i].inverse_bind));
            joints[i].inverse_bind[0] = 1.0f;
            joints[i].inverse_bind[5] = 1.0f;
            joints[i].inverse_bind[10] = 1.0f;
            joints[i].inverse_bind[15] = 1.0f;
        }
    }
    memset(&desc, 0, sizeof(desc));
    desc.joints = joints;
    desc.joint_count = joint_count;
    rc = le_anim_create_skeleton_data(&desc, out);
    free(joints);
    return rc;
}

/* Decoded track input (caller-decoded): one track per call. */
le_result le_anim_build_track_decoded(
    le_anim_target_kind target_kind, uint32_t target_index,
    le_anim_channel channel, le_anim_interpolation interp,
    const float *times, const float *values, uint32_t key_count,
    le_anim_track_desc *out_desc, float **owned_times,
    float **owned_values) {
    if (out_desc == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memset(out_desc, 0, sizeof(*out_desc));
    out_desc->target_kind = target_kind;
    out_desc->target_index = target_index;
    out_desc->channel = channel;
    out_desc->interpolation = interp;
    out_desc->times = times;
    out_desc->values = values;
    out_desc->key_count = key_count;
    if (owned_times != NULL) {
        *owned_times = NULL;
    }
    if (owned_values != NULL) {
        *owned_values = NULL;
    }
    /* Validation happens in le_anim_create_clip_data (single
     * authority); this constructor only packs. */
    return LE_SUCCESS;
}

/* Stubs for the internal header's opaque-gltf entry points (kept
 * for documentation; the bridge uses the decoded builders above
 * because cgltf types cannot cross into engine TUs without the
 * vendored header — assets owns that include). */
le_result le_anim_build_skeleton_from_gltf(
    const void *cgltf_data, uint32_t skin_index,
    struct le_skeleton_data **out) {
    (void)cgltf_data;
    (void)skin_index;
    if (out != NULL) {
        *out = NULL;
    }
    return LE_ERROR_UNSUPPORTED;
}

le_result le_anim_build_clip_from_gltf(
    const void *cgltf_data, uint32_t anim_index,
    const struct le_skeleton_data *skeleton,
    struct le_clip_data **out, float *out_duration) {
    (void)cgltf_data;
    (void)anim_index;
    (void)skeleton;
    if (out != NULL) {
        *out = NULL;
    }
    if (out_duration != NULL) {
        *out_duration = 0.0f;
    }
    return LE_ERROR_UNSUPPORTED;
}
