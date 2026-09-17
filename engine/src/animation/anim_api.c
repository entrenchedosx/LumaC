/*
 * Animation public API surface (Phase 29): asset create/getters,
 * sampling, bind pose, blend, CPU skinning oracle. Playback lives
 * in anim_playback.c; stepping in anim_step.c.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "animation/animation_internal.h"
#include "animation/anim_math.h"

le_result le_asset_create_skeleton(
    le_engine *engine, const le_skeleton_asset_desc *desc,
    le_asset *out_asset) {
    struct le_skeleton_data *skel = NULL;
    le_result rc;
    int32_t idx;
    le_asset handle = LE_ASSET_INVALID;
    le_asset_id id;

    if (out_asset != NULL) {
        *out_asset = LE_ASSET_INVALID;
    }
    if (engine == NULL || desc == NULL || out_asset == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    rc = le_anim_create_skeleton_data(desc, &skel);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    /* Portable identity (Phase 34A): keyed callers get
     * le_identity_for_key IDs (relocation-stable); legacy callers
     * keep minted UUIDs (each create distinct). */
    if (desc->identity_key != NULL && desc->identity_len > 0 &&
        desc->identity_sub_key != NULL &&
        desc->identity_sub_key[0] != '\0') {
        extern void le_identity_for_key(const void *key_bytes,
                                        size_t key_len,
                                        const char *sub_key,
                                        le_asset_id *out_id);

        le_identity_for_key(desc->identity_key,
                            desc->identity_len,
                            desc->identity_sub_key, &id);
    } else {
        le_uuid_mint(engine, &id.hi, &id.lo);
    }
    idx = le_asset_alloc(engine, LE_ASSET_SKELETON, LE_ASSET_READY,
                         &id, NULL, &rc, &handle);
    if (idx < 0) {
        le_anim_free_skeleton(skel);
        return rc;
    }
    engine->assets[idx].skeleton = skel;
    *out_asset = handle;
    return LE_SUCCESS;
}

le_result le_asset_create_clip(le_engine *engine,
                               const le_animation_clip_desc *desc,
                               le_asset *out_asset) {
    struct le_clip_data *clip = NULL;
    le_result rc;
    int32_t idx;
    le_asset handle = LE_ASSET_INVALID;
    le_asset_id id;

    if (out_asset != NULL) {
        *out_asset = LE_ASSET_INVALID;
    }
    if (engine == NULL || desc == NULL || out_asset == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    rc = le_anim_create_clip_data(desc, &clip);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    if (desc->identity_key != NULL && desc->identity_len > 0 &&
        desc->identity_sub_key != NULL &&
        desc->identity_sub_key[0] != '\0') {
        extern void le_identity_for_key(const void *key_bytes,
                                        size_t key_len,
                                        const char *sub_key,
                                        le_asset_id *out_id);

        le_identity_for_key(desc->identity_key,
                            desc->identity_len,
                            desc->identity_sub_key, &id);
    } else {
        le_uuid_mint(engine, &id.hi, &id.lo);
    }
    idx = le_asset_alloc(engine, LE_ASSET_ANIMATION_CLIP,
                         LE_ASSET_READY, &id, NULL, &rc, &handle);
    if (idx < 0) {
        le_anim_free_clip(clip);
        return rc;
    }
    engine->assets[idx].clip = clip;
    *out_asset = handle;
    return LE_SUCCESS;
}

uint32_t le_skeleton_get_joint_count(const le_engine *engine,
                                     const le_asset *skeleton) {
    uint32_t aslot;
    le_result code = LE_SUCCESS;

    if (engine == NULL || skeleton == NULL) {
        return 0;
    }
    if (!le_resolve_asset_live(engine, skeleton, &aslot, &code)) {
        return 0;
    }
    if (engine->assets[aslot].type != LE_ASSET_SKELETON ||
        engine->assets[aslot].skeleton == NULL) {
        return 0;
    }
    return engine->assets[aslot].skeleton->joint_count;
}

int le_skeleton_find_joint(const le_engine *engine,
                           const le_asset *skeleton,
                           const char *name, uint32_t *out_index) {
    uint32_t aslot;
    le_result code = LE_SUCCESS;
    uint32_t i;

    if (out_index != NULL) {
        *out_index = 0;
    }
    if (engine == NULL || skeleton == NULL || name == NULL) {
        return 0;
    }
    if (!le_resolve_asset_live(engine, skeleton, &aslot, &code)) {
        return 0;
    }
    if (engine->assets[aslot].type != LE_ASSET_SKELETON ||
        engine->assets[aslot].skeleton == NULL) {
        return 0;
    }
    {
        const struct le_skeleton_data *skel =
            engine->assets[aslot].skeleton;

        for (i = 0; i < skel->joint_count; i++) {
            if (strcmp(skel->joints[i].name, name) == 0) {
                if (out_index != NULL) {
                    *out_index = i;
                }
                return 1; /* first match wins (duplicates) */
            }
        }
    }
    return 0;
}

float le_clip_get_duration(const le_engine *engine,
                           const le_asset *clip) {
    uint32_t aslot;
    le_result code = LE_SUCCESS;

    if (engine == NULL || clip == NULL) {
        return 0.0f;
    }
    if (!le_resolve_asset_live(engine, clip, &aslot, &code)) {
        return 0.0f;
    }
    if (engine->assets[aslot].type != LE_ASSET_ANIMATION_CLIP ||
        engine->assets[aslot].clip == NULL) {
        return 0.0f;
    }
    return engine->assets[aslot].clip->duration;
}

uint32_t le_clip_get_track_count(const le_engine *engine,
                                 const le_asset *clip) {
    uint32_t aslot;
    le_result code = LE_SUCCESS;

    if (engine == NULL || clip == NULL) {
        return 0;
    }
    if (!le_resolve_asset_live(engine, clip, &aslot, &code)) {
        return 0;
    }
    if (engine->assets[aslot].type != LE_ASSET_ANIMATION_CLIP ||
        engine->assets[aslot].clip == NULL) {
        return 0;
    }
    return engine->assets[aslot].clip->track_count;
}

le_result le_anim_sample_clip(
    const le_engine *engine, const le_asset *clip, float time,
    float (*out_t)[3], float (*out_r)[4], float (*out_s)[3],
    uint32_t joint_count, float out_obj_t[3],
    float out_obj_r[4], float out_obj_s[3]) {
    uint32_t aslot;
    le_result code = LE_SUCCESS;
    const struct le_clip_data *c;

    if (engine == NULL || clip == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_am_finite(time) || time < 0.0f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (joint_count > 0 &&
        (out_t == NULL || out_r == NULL || out_s == NULL)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_asset_live(engine, clip, &aslot, &code)) {
        return code;
    }
    if (engine->assets[aslot].type != LE_ASSET_ANIMATION_CLIP ||
        engine->assets[aslot].clip == NULL) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    c = engine->assets[aslot].clip;
    return le_anim_sample_clip_data(c, time, out_t, out_r, out_s,
                                    joint_count, out_obj_t,
                                    out_obj_r, out_obj_s);
}

le_result le_anim_bind_pose(const le_engine *engine,
                            const le_asset *skeleton,
                            float (*out_global)[16],
                            uint32_t joint_count) {
    uint32_t aslot;
    le_result code = LE_SUCCESS;
    const struct le_skeleton_data *skel;

    if (engine == NULL || skeleton == NULL ||
        out_global == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_asset_live(engine, skeleton, &aslot, &code)) {
        return code;
    }
    if (engine->assets[aslot].type != LE_ASSET_SKELETON ||
        engine->assets[aslot].skeleton == NULL) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    skel = engine->assets[aslot].skeleton;
    return le_anim_bind_globals(skel, out_global, joint_count);
}
