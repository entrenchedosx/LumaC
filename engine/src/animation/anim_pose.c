/*
 * Pose evaluation (Phase 29): bind globals, local->global
 * hierarchy (iterative topological — no recursion, 1000-deep
 * chains safe), two-pose blending, skin-matrix builds, and the
 * CPU reference skinning oracle.
 *
 * Skin-matrix convention: joint_global * inverse_bind, expressed
 * in the ANIMATED OBJECT's local frame. The renderer then applies
 * object/world/view/proj per its convention (model matrix carries
 * the object root; joints are relative to it — reconciled in
 * GPU_SKINNING.md, proven by the CPU/GPU oracle test).
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "animation/animation_internal.h"
#include "animation/anim_math.h"

le_result le_anim_bind_globals(const struct le_skeleton_data *skel,
                               float (*out_global)[16],
                               uint32_t joint_count) {
    uint32_t i;

    if (skel == NULL || out_global == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (joint_count < skel->joint_count) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Topological by construction: parents always precede
     * children? NOT guaranteed (import order is file order) — so
     * evaluate to a fixpoint (bounded: joint_count passes).
     * Order-independent, no recursion. */
    {
        uint32_t pass;

        for (pass = 0; pass < skel->joint_count; pass++) {
            for (i = 0; i < skel->joint_count; i++) {
                float local[16];
                const le_skeleton_joint *j = &skel->joints[i];

                le_am_compose_trs(j->bind_t, j->bind_r, j->bind_s,
                                  local);
                if (j->parent < 0) {
                    memcpy(out_global[i], local, sizeof(local));
                } else {
                    le_am_mat4_mul(
                        out_global[(uint32_t)j->parent], local,
                        out_global[i]);
                }
            }
        }
    }
    return LE_SUCCESS;
}

le_result le_anim_locals_to_globals(
    const struct le_skeleton_data *skel,
    const float (*local_t)[3], const float (*local_r)[4],
    const float (*local_s)[3], float (*out_global)[16],
    uint32_t joint_count) {
    uint32_t i;
    uint32_t pass;

    if (skel == NULL || local_t == NULL || local_r == NULL ||
        local_s == NULL || out_global == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (joint_count < skel->joint_count) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Fixpoint iteration (bounded passes; order-independent, no
     * recursion). Roots seed from local; children compose from
     * whatever the parent holds this pass (converges within depth
     * passes <= joint_count). */
    for (pass = 0; pass < skel->joint_count; pass++) {
        for (i = 0; i < skel->joint_count; i++) {
            float local[16];
            int32_t p = skel->joints[i].parent;

            le_am_compose_trs(local_t[i], local_r[i], local_s[i],
                              local);
            if (p < 0) {
                memcpy(out_global[i], local, sizeof(local));
            } else {
                le_am_mat4_mul(out_global[(uint32_t)p], local,
                               out_global[i]);
            }
        }
    }
    return LE_SUCCESS;
}

le_result le_anim_build_skin(
    const struct le_skeleton_data *skel,
    const float (*global_m)[16], float (*out_skin)[16],
    uint32_t joint_count) {
    uint32_t i;

    if (skel == NULL || global_m == NULL || out_skin == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (joint_count < skel->joint_count) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < skel->joint_count; i++) {
        le_am_mat4_mul(global_m[i], skel->joints[i].inverse_bind,
                       out_skin[i]);
    }
    return LE_SUCCESS;
}

le_result le_anim_blend_pose(
    uint32_t joint_count, const float (*a_t)[3],
    const float (*a_r)[4], const float (*a_s)[3],
    const float (*b_t)[3], const float (*b_r)[4],
    const float (*b_s)[3], float weight, float (*out_t)[3],
    float (*out_r)[4], float (*out_s)[3]) {
    uint32_t i;

    if (joint_count == 0 || joint_count > LE_ANIM_MAX_JOINTS) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (a_t == NULL || a_r == NULL || a_s == NULL || b_t == NULL ||
        b_r == NULL || b_s == NULL || out_t == NULL ||
        out_r == NULL || out_s == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_am_finite(weight)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (weight < 0.0f) {
        weight = 0.0f;
    } else if (weight > 1.0f) {
        weight = 1.0f;
    }
    for (i = 0; i < joint_count; i++) {
        out_t[i][0] = a_t[i][0] + (b_t[i][0] - a_t[i][0]) * weight;
        out_t[i][1] = a_t[i][1] + (b_t[i][1] - a_t[i][1]) * weight;
        out_t[i][2] = a_t[i][2] + (b_t[i][2] - a_t[i][2]) * weight;
        out_s[i][0] = a_s[i][0] + (b_s[i][0] - a_s[i][0]) * weight;
        out_s[i][1] = a_s[i][1] + (b_s[i][1] - a_s[i][1]) * weight;
        out_s[i][2] = a_s[i][2] + (b_s[i][2] - a_s[i][2]) * weight;
        le_am_quat_slerp(a_r[i], b_r[i], weight, out_r[i]);
    }
    return LE_SUCCESS;
}

le_result le_anim_skin_vertex(
    const float position[3], const float normal[3],
    const uint32_t joints[4], const float weights[4],
    const float (*joint_matrices)[16], uint32_t joint_count,
    float out_position[3], float out_normal[3]) {
    float wsum = 0.0f;
    float p[3] = { 0.0f, 0.0f, 0.0f };
    float n[3] = { 0.0f, 0.0f, 0.0f };
    uint32_t k;

    if (position == NULL || normal == NULL || joints == NULL ||
        weights == NULL || joint_matrices == NULL ||
        out_position == NULL || out_normal == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (joint_count == 0 ||
        joint_count > LE_ANIM_MAX_JOINTS) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    for (k = 0; k < 4u; k++) {
        if (!le_am_finite(weights[k])) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (joints[k] >= joint_count) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    }
    for (k = 0; k < 4u; k++) {
        wsum += weights[k];
    }
    if (!(wsum > 1e-9f) || !isfinite(wsum)) {
        /* Zero-weight vertex: hold position, pass normal
         * through (safe fallback, never NaN). */
        memcpy(out_position, position, 3u * sizeof(float));
        memcpy(out_normal, normal, 3u * sizeof(float));
        return LE_SUCCESS;
    }
    for (k = 0; k < 4u; k++) {
        float w = weights[k] / wsum; /* normalize policy */
        const float *m = joint_matrices[joints[k]];
        float tp[3];
        float tn[3];

        le_am_mat4_point(m, position, tp);
        /* Normal via upper 3x3 (rigid-ish assumption;
         * renormalized below). */
        tn[0] = m[0] * normal[0] + m[4] * normal[1] +
                m[8] * normal[2];
        tn[1] = m[1] * normal[0] + m[5] * normal[1] +
                m[9] * normal[2];
        tn[2] = m[2] * normal[0] + m[6] * normal[1] +
                m[10] * normal[2];
        p[0] += w * tp[0];
        p[1] += w * tp[1];
        p[2] += w * tp[2];
        n[0] += w * tn[0];
        n[1] += w * tn[1];
        n[2] += w * tn[2];
    }
    {
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

        if (l > 1e-9f && isfinite(l)) {
            n[0] /= l;
            n[1] /= l;
            n[2] /= l;
        } else {
            n[0] = normal[0];
            n[1] = normal[1];
            n[2] = normal[2];
        }
    }
    memcpy(out_position, p, sizeof(p));
    memcpy(out_normal, n, sizeof(n));
    return LE_SUCCESS;
}
