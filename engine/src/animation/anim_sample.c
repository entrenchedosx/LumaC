/*
 * Clip sampler (Phase 29): reusable engine sampling, no Lua.
 *
 * sample_clip(clip, time, pose) evaluates every track at `time`
 * (caller wraps time per its loop policy). Track kinds:
 * - STEP: previous value (exact next key = next value).
 * - LINEAR: lerp for T/S, shortest-path SLERP for R.
 * - CUBICSPLINE: glTF Hermite (in/value/out triples, tangents
 *   scaled by the key interval); quaternion results normalized.
 *
 * Segment location: cached cursor (binary-search fallback keeps
 * random seeks correct; sequential playback reuses the cursor).
 * Duplicate timestamps: last-wins (upper-bound search lands on
 * the final duplicate). Zero-length intervals never divide.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "animation/animation_internal.h"
#include "animation/anim_math.h"

/* Locate the segment [lo, lo+1] containing t (upper-bound: first
 * key with time > t, minus one). Returns key_count-1 when t is at
 * or past the last key (caller clamps to the end value). */
static uint32_t le_track_segment(const le_anim_track *tr, float t) {
    uint32_t lo;
    uint32_t hi;

    if (tr->key_count == 1 || t <= tr->times[0]) {
        return 0;
    }
    if (t >= tr->times[tr->key_count - 1u]) {
        return tr->key_count - 1u;
    }
    /* Cursor fast path (sequential playback): cursor <= answer
     * <= cursor+1 in the common case. */
    {
        uint32_t c = tr->cursor;

        if (c < tr->key_count - 1u && tr->times[c] <= t &&
            t < tr->times[c + 1u]) {
            /* Duplicate-timestamp edge: advance past equal keys
             * (last-wins). */
            while (c + 1u < tr->key_count - 1u &&
                   tr->times[c + 1u] <= t &&
                   tr->times[c + 1u] == tr->times[c]) {
                c++;
            }
            ((le_anim_track *)tr)->cursor = c;
            return c;
        }
        if (c + 1u < tr->key_count && tr->times[c + 1u] <= t &&
            (c + 2u >= tr->key_count ||
             t < tr->times[c + 2u])) {
            ((le_anim_track *)tr)->cursor = c + 1u;
            return c + 1u;
        }
    }
    /* Binary search (upper bound). */
    lo = 0;
    hi = tr->key_count - 1u;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo + 1u) / 2u;

        if (tr->times[mid] <= t) {
            lo = mid;
        } else {
            hi = mid - 1u;
        }
    }
    ((le_anim_track *)tr)->cursor = lo;
    return lo;
}

le_result le_anim_sample_track(const le_anim_track *tr, float time,
                               float out_t[3], float out_r[4],
                               float out_s[3]) {
    uint32_t seg;
    const float *v0;
    const float *v1;
    float t0;
    float t1;
    float f = 0.0f;

    if (tr == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_am_finite(time)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (time < 0.0f) {
        time = 0.0f;
    }
    seg = le_track_segment(tr, time);
    if (seg >= tr->key_count - 1u) {
        /* At/past the last key: exact end value. */
        const float *v = &tr->values[(size_t)(tr->key_count - 1u) *
                                     (size_t)tr->stride];

        if (tr->interpolation == LE_ANIM_INTERP_CUBICSPLINE) {
            v += (tr->channel == LE_ANIM_CHANNEL_ROTATION) ? 4u
                                                           : 3u;
        }
        if (tr->channel == LE_ANIM_CHANNEL_TRANSLATION) {
            memcpy(out_t, v, 3u * sizeof(float));
        } else if (tr->channel == LE_ANIM_CHANNEL_SCALE) {
            memcpy(out_s, v, 3u * sizeof(float));
        } else {
            le_am_quat_normalize(v, out_r);
        }
        return LE_SUCCESS;
    }
    t0 = tr->times[seg];
    t1 = tr->times[seg + 1u];
    v0 = &tr->values[(size_t)seg * (size_t)tr->stride];
    v1 = &tr->values[(size_t)(seg + 1u) * (size_t)tr->stride];
    if (tr->interpolation == LE_ANIM_INTERP_STEP) {
        /* Previous value (exact next key handled above when the
         * search lands on it... note: upper-bound search returns
         * seg = the key <= t, so at exact t1 the segment is
         * [t1, t2] and v0 IS t1's value. Correct STEP semantics
         * fall out of the search. */
        if (tr->channel == LE_ANIM_CHANNEL_TRANSLATION) {
            memcpy(out_t, v0, 3u * sizeof(float));
        } else if (tr->channel == LE_ANIM_CHANNEL_SCALE) {
            memcpy(out_s, v0, 3u * sizeof(float));
        } else {
            le_am_quat_normalize(v0, out_r);
        }
        return LE_SUCCESS;
    }
    {
        float dt = t1 - t0;

        if (dt > 1e-9f) {
            f = (time - t0) / dt;
            if (f < 0.0f) {
                f = 0.0f;
            } else if (f > 1.0f) {
                f = 1.0f;
            }
        } else {
            /* Duplicate timestamps: last-wins (v1). */
            f = 1.0f;
        }
    }
    if (tr->interpolation == LE_ANIM_INTERP_CUBICSPLINE) {
        /* glTF Hermite: value_k, out_k, in_{k+1}; tangents scale
         * by dt (spec: NIST-tested formulae). */
        uint32_t comps =
            (tr->channel == LE_ANIM_CHANNEL_ROTATION) ? 4u : 3u;
        const float *p0 = v0 + comps;     /* value_k */
        const float *m0 = v0 + comps * 2u; /* out_k */
        const float *p1 = v1 + comps;     /* value_{k+1} */
        const float *m1 = v1;             /* in_{k+1} */
        float dt = t1 - t0;
        float f2 = f * f;
        float f3 = f2 * f;
        float h00 = 2.0f * f3 - 3.0f * f2 + 1.0f;
        float h10 = f3 - 2.0f * f2 + f;
        float h01 = -2.0f * f3 + 3.0f * f2;
        float h11 = f3 - f2;
        uint32_t c;

        if (tr->channel == LE_ANIM_CHANNEL_ROTATION) {
            float q[4];

            for (c = 0; c < 4u; c++) {
                q[c] = h00 * p0[c] + h10 * dt * m0[c] +
                       h01 * p1[c] + h11 * dt * m1[c];
            }
            le_am_quat_normalize(q, out_r);
        } else if (tr->channel == LE_ANIM_CHANNEL_TRANSLATION) {
            for (c = 0; c < 3u; c++) {
                out_t[c] = h00 * p0[c] + h10 * dt * m0[c] +
                           h01 * p1[c] + h11 * dt * m1[c];
            }
        } else {
            for (c = 0; c < 3u; c++) {
                out_s[c] = h00 * p0[c] + h10 * dt * m0[c] +
                           h01 * p1[c] + h11 * dt * m1[c];
            }
        }
        return LE_SUCCESS;
    }
    /* LINEAR. */
    if (tr->channel == LE_ANIM_CHANNEL_TRANSLATION) {
        out_t[0] = v0[0] + (v1[0] - v0[0]) * f;
        out_t[1] = v0[1] + (v1[1] - v0[1]) * f;
        out_t[2] = v0[2] + (v1[2] - v0[2]) * f;
    } else if (tr->channel == LE_ANIM_CHANNEL_SCALE) {
        out_s[0] = v0[0] + (v1[0] - v0[0]) * f;
        out_s[1] = v0[1] + (v1[1] - v0[1]) * f;
        out_s[2] = v0[2] + (v1[2] - v0[2]) * f;
    } else {
        le_am_quat_slerp(v0, v1, f, out_r);
    }
    return LE_SUCCESS;
}

le_result le_anim_sample_clip_data(
    const struct le_clip_data *clip, float time,
    float (*out_t)[3], float (*out_r)[4], float (*out_s)[3],
    uint32_t joint_count, float out_obj_t[3],
    float out_obj_r[4], float out_obj_s[3]) {
    uint32_t i;

    if (clip == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_am_finite(time) || time < 0.0f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Tracks write only their channel; untouched joints keep
     * whatever the caller seeded (bind pose by convention). */
    for (i = 0; i < clip->track_count; i++) {
        const le_anim_track *tr = &clip->tracks[i];
        float tt[3] = { 0.0f, 0.0f, 0.0f };
        float rr[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float ss[3] = { 1.0f, 1.0f, 1.0f };
        le_result rc =
            le_anim_sample_track(tr, time, tt, rr, ss);

        if (rc != LE_SUCCESS) {
            return rc;
        }
        if (tr->target_kind == LE_ANIM_TARGET_JOINT) {
            if (tr->target_index >= joint_count) {
                continue; /* stale target (skeleton swapped):
                           * skip, never OOB. */
            }
            if (tr->channel == LE_ANIM_CHANNEL_TRANSLATION) {
                memcpy(out_t[tr->target_index], tt, sizeof(tt));
            } else if (tr->channel == LE_ANIM_CHANNEL_SCALE) {
                memcpy(out_s[tr->target_index], ss, sizeof(ss));
            } else {
                memcpy(out_r[tr->target_index], rr, sizeof(rr));
            }
        } else {
            if (out_obj_t == NULL || out_obj_r == NULL ||
                out_obj_s == NULL) {
                continue;
            }
            if (tr->channel == LE_ANIM_CHANNEL_TRANSLATION) {
                memcpy(out_obj_t, tt, sizeof(tt));
            } else if (tr->channel == LE_ANIM_CHANNEL_SCALE) {
                memcpy(out_obj_s, ss, sizeof(ss));
            } else {
                memcpy(out_obj_r, rr, sizeof(rr));
            }
        }
    }
    return LE_SUCCESS;
}
