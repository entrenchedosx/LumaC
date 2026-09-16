/*
 * Animation asset payloads (Phase 29): immutable skeleton + clip
 * construction with strict validation. Malformed input creates
 * NOTHING (transactional; registry untouched on failure — the
 * caller allocates the slot only after these succeed).
 *
 * Skeleton policy: parents in range, no self-parent, no cycles,
 * every joint reachable from a root, at least one root, joint
 * count in [1, LE_ANIM_MAX_JOINTS]. Duplicate names are ALLOWED
 * (first match wins lookups; documented) but empty names are
 * fine too. Multiple roots are LEGAL (forests animate each root
 * from identity — documented).
 *
 * Clip policy: duration finite > 0; times finite, >= 0,
 * non-decreasing (duplicates = last-wins sampling, explicit);
 * values finite; rotation quats normalizable; counts in range.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "animation/animation_internal.h"
#include "animation/anim_math.h"

void le_anim_free_skeleton(struct le_skeleton_data *skel) {
    if (skel == NULL) {
        return;
    }
    free(skel->joints);
    free(skel);
}

void le_anim_free_clip(struct le_clip_data *clip) {
    uint32_t i;

    if (clip == NULL) {
        return;
    }
    if (clip->tracks != NULL) {
        for (i = 0; i < clip->track_count; i++) {
            free(clip->tracks[i].times);
            free(clip->tracks[i].values);
        }
        free(clip->tracks);
    }
    free(clip);
}

/* Struct-blind free for asset.c/engine.c teardown. */
void le_anim_free_slot_backing(struct le_skeleton_data *skeleton,
                               struct le_clip_data *clip) {
    le_anim_free_skeleton(skeleton);
    le_anim_free_clip(clip);
}

le_result le_anim_create_skeleton_data(
    const le_skeleton_asset_desc *desc,
    struct le_skeleton_data **out) {
    struct le_skeleton_data *skel = NULL;
    uint32_t i;

    if (out != NULL) {
        *out = NULL;
    }
    if (desc == NULL || out == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->joint_count == 0 ||
        desc->joint_count > LE_ANIM_MAX_JOINTS ||
        desc->joints == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    skel = (struct le_skeleton_data *)calloc(1, sizeof(*skel));
    if (skel == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    skel->joints = (le_skeleton_joint *)calloc(
        desc->joint_count, sizeof(*skel->joints));
    if (skel->joints == NULL) {
        free(skel);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    skel->joint_count = desc->joint_count;
    /* Copy + validate each joint. */
    for (i = 0; i < desc->joint_count; i++) {
        const le_skeleton_joint_desc *d = &desc->joints[i];
        le_skeleton_joint *j = &skel->joints[i];
        size_t n;

        if (d->parent >= (int32_t)desc->joint_count ||
            d->parent < -1) {
            le_anim_free_skeleton(skel);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (d->parent == (int32_t)i) {
            /* Self-parent. */
            le_anim_free_skeleton(skel);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (!le_am_finite3(d->translation) ||
            !le_am_finite4(d->rotation) ||
            !le_am_finite3(d->scale)) {
            le_anim_free_skeleton(skel);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (d->scale[0] == 0.0f || d->scale[1] == 0.0f ||
            d->scale[2] == 0.0f) {
            /* Zero scale collapses the subtree (singular); reject
             * at ingestion (animate scale tracks instead). */
            le_anim_free_skeleton(skel);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        {
            uint32_t r;
            uint32_t c;

            for (r = 0; r < 4; r++) {
                for (c = 0; c < 4; c++) {
                    if (!le_am_finite(
                            d->inverse_bind[c * 4u + r])) {
                        le_anim_free_skeleton(skel);
                        return LE_ERROR_INVALID_ARGUMENT;
                    }
                }
            }
        }
        memset(j->name, 0, sizeof(j->name));
        n = strlen(d->name);
        if (n >= sizeof(j->name)) {
            n = sizeof(j->name) - 1u;
        }
        memcpy(j->name, d->name, n);
        j->parent = d->parent;
        memcpy(j->bind_t, d->translation, sizeof(j->bind_t));
        if (!le_am_quat_normalize(d->rotation, j->bind_r)) {
            le_anim_free_skeleton(skel);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        memcpy(j->bind_s, d->scale, sizeof(j->bind_s));
        memcpy(j->inverse_bind, d->inverse_bind,
               sizeof(j->inverse_bind));
        if (d->parent < 0) {
            skel->root_count++;
        }
    }
    if (skel->root_count == 0) {
        le_anim_free_skeleton(skel);
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Cycle + reachability check (iterative color-marked DFS from
     * every root; hostile depths cannot overflow the stack). */
    {
        uint8_t *state = (uint8_t *)calloc(desc->joint_count, 1);
        uint32_t *stack = NULL;
        size_t top = 0;
        uint32_t r;

        if (state == NULL) {
            le_anim_free_skeleton(skel);
            return LE_ERROR_OUT_OF_MEMORY;
        }
        stack = (uint32_t *)malloc(sizeof(uint32_t) *
                                   ((size_t)desc->joint_count +
                                    skel->root_count + 1u));
        if (stack == NULL) {
            free(state);
            le_anim_free_skeleton(skel);
            return LE_ERROR_OUT_OF_MEMORY;
        }
        for (r = 0; r < desc->joint_count; r++) {
            uint32_t n0;
            uint32_t k;

            if (skel->joints[r].parent >= 0) {
                continue;
            }
            n0 = r;
            if (state[n0] == 2u) {
                continue;
            }
            top = 0;
            stack[top++] = n0;
            while (top > 0) {
                uint32_t ni = stack[--top];

                if (ni & 0x80000000u) {
                    state[ni & ~0x80000000u] = 2u;
                    continue;
                }
                if (state[ni] == 2u) {
                    continue;
                }
                if (state[ni] == 1u) {
                    free(stack);
                    free(state);
                    le_anim_free_skeleton(skel);
                    return LE_ERROR_INVALID_ARGUMENT;
                }
                state[ni] = 1u;
                stack[top++] = ni | 0x80000000u;
                for (k = 0; k < desc->joint_count; k++) {
                    if (skel->joints[k].parent == (int32_t)ni) {
                        stack[top++] = k;
                    }
                }
            }
        }
        free(stack);
        for (i = 0; i < desc->joint_count; i++) {
            if (state[i] == 0u) {
                /* Unreachable (pure cycle with no root entry). */
                free(state);
                le_anim_free_skeleton(skel);
                return LE_ERROR_INVALID_ARGUMENT;
            }
        }
        free(state);
    }
    *out = skel;
    return LE_SUCCESS;
}

le_result le_anim_create_clip_data(
    const le_animation_clip_desc *desc,
    struct le_clip_data **out) {
    struct le_clip_data *clip = NULL;
    uint32_t t;

    if (out != NULL) {
        *out = NULL;
    }
    if (desc == NULL || out == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_am_finite(desc->duration) || desc->duration <= 0.0f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->track_count > LE_ANIM_MAX_TRACKS ||
        (desc->track_count > 0 && desc->tracks == NULL)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    clip = (struct le_clip_data *)calloc(1, sizeof(*clip));
    if (clip == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    clip->duration = desc->duration;
    if (desc->track_count == 0) {
        *out = clip;
        return LE_SUCCESS;
    }
    clip->tracks = (le_anim_track *)calloc(
        desc->track_count, sizeof(*clip->tracks));
    if (clip->tracks == NULL) {
        free(clip);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    clip->track_count = desc->track_count;
    for (t = 0; t < desc->track_count; t++) {
        const le_anim_track_desc *d = &desc->tracks[t];
        le_anim_track *tr = &clip->tracks[t];
        uint32_t comps;
        uint32_t k;

        if (d->target_kind != LE_ANIM_TARGET_JOINT &&
            d->target_kind != LE_ANIM_TARGET_OBJECT) {
            le_anim_free_clip(clip);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (d->channel != LE_ANIM_CHANNEL_TRANSLATION &&
            d->channel != LE_ANIM_CHANNEL_ROTATION &&
            d->channel != LE_ANIM_CHANNEL_SCALE) {
            le_anim_free_clip(clip);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (d->interpolation != LE_ANIM_INTERP_STEP &&
            d->interpolation != LE_ANIM_INTERP_LINEAR &&
            d->interpolation != LE_ANIM_INTERP_CUBICSPLINE) {
            le_anim_free_clip(clip);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (d->key_count == 0 ||
            d->key_count > LE_ANIM_MAX_KEYS_PER_TRACK ||
            d->times == NULL || d->values == NULL) {
            le_anim_free_clip(clip);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (d->target_kind == LE_ANIM_TARGET_JOINT &&
            d->target_index >= LE_ANIM_MAX_JOINTS) {
            le_anim_free_clip(clip);
            return LE_ERROR_INVALID_ARGUMENT;
        }
        comps = (d->channel == LE_ANIM_CHANNEL_ROTATION) ? 4u : 3u;
        tr->stride = (d->interpolation == LE_ANIM_INTERP_CUBICSPLINE)
                         ? comps * 3u
                         : comps;
        /* Checked size arithmetic. */
        {
            uint64_t need_vals =
                (uint64_t)d->key_count * (uint64_t)tr->stride;

            if (need_vals > (uint64_t)0x00FFFFFFu) {
                le_anim_free_clip(clip);
                return LE_ERROR_OVERFLOW;
            }
        }
        tr->times =
            (float *)malloc(sizeof(float) * (size_t)d->key_count);
        tr->values = (float *)malloc(sizeof(float) *
                                     (size_t)d->key_count *
                                     (size_t)tr->stride);
        if (tr->times == NULL || tr->values == NULL) {
            le_anim_free_clip(clip);
            return LE_ERROR_OUT_OF_MEMORY;
        }
        memcpy(tr->times, d->times,
               sizeof(float) * (size_t)d->key_count);
        memcpy(tr->values, d->values,
               sizeof(float) * (size_t)d->key_count *
                   (size_t)tr->stride);
        tr->key_count = d->key_count;
        tr->target_kind = d->target_kind;
        tr->target_index = d->target_index;
        tr->channel = d->channel;
        tr->interpolation = d->interpolation;
        tr->cursor = 0;
        /* Validate times + values. */
        for (k = 0; k < d->key_count; k++) {
            float tm = tr->times[k];

            if (!le_am_finite(tm) || tm < 0.0f) {
                le_anim_free_clip(clip);
                return LE_ERROR_INVALID_ARGUMENT;
            }
            if (k > 0 && tm < tr->times[k - 1u]) {
                /* Unsorted (duplicates allowed, backwards not). */
                le_anim_free_clip(clip);
                return LE_ERROR_INVALID_ARGUMENT;
            }
            {
                uint32_t c;

                for (c = 0; c < tr->stride; c++) {
                    float v = tr->values[(size_t)k *
                                             (size_t)tr->stride +
                                         c];

                    if (!le_am_finite(v)) {
                        le_anim_free_clip(clip);
                        return LE_ERROR_INVALID_ARGUMENT;
                    }
                }
            }
            if (d->channel == LE_ANIM_CHANNEL_ROTATION) {
                /* Every stored quat (value, or value-slot of a
                 * cubic triple) must be normalizable. */
                const float *q = (d->interpolation ==
                                  LE_ANIM_INTERP_CUBICSPLINE)
                                     ? &tr->values[(size_t)k *
                                                       (size_t)
                                                           tr->stride +
                                                   4u]
                                     : &tr->values[(size_t)k *
                                                   (size_t)
                                                       tr->stride];
                float n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] +
                          q[3] * q[3];

                if (!(n > 1e-12f) || !isfinite(n)) {
                    le_anim_free_clip(clip);
                    return LE_ERROR_INVALID_ARGUMENT;
                }
            }
        }
    }
    *out = clip;
    return LE_SUCCESS;
}
