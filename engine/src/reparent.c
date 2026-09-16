/*
 * Luma Engine world-preserving reparent + TRS decomposition
 * (Phase 25, Stages 46-49).
 *
 * KEEP_WORLD: old_world = object world matrix; new_local =
 * inverse(new_parent_world) x old_world; decompose TRS. Residual
 * shear (rotated non-uniform ancestors) has no exact TRS form and
 * fails LE_ERROR_UNREPRESENTABLE_TRANSFORM — never silent
 * distortion. Mirrors decompose with the sign on the
 * smallest-magnitude scale axis (submit-parity convention).
 */

#include <math.h>
#include <stddef.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"

/* Inverse of a rigid+scale (TRS, no shear) column-major matrix:
 * M = T R S -> M^-1 = S^-1 R^T (-t). Returns 0 on singular axis. */
static int le_mat4_inverse_trs(const float m[16], float out_inv[16]) {
    float sx;
    float sy;
    float sz;
    float r00;
    float r01;
    float r02;
    float r10;
    float r11;
    float r12;
    float r20;
    float r21;
    float r22;
    float tx;
    float ty;
    float tz;
    float itx;
    float ity;
    float itz;

    if (m == NULL || out_inv == NULL) {
        return 0;
    }
    sx = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    sy = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    sz = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    if (!(sx > 1e-12f) || !(sy > 1e-12f) || !(sz > 1e-12f)) {
        return 0;
    }
    /* Normalized rotation rows of the inverse = R^T with 1/s. */
    r00 = m[0] / sx;
    r10 = m[1] / sx;
    r20 = m[2] / sx;
    r01 = m[4] / sy;
    r11 = m[5] / sy;
    r21 = m[6] / sy;
    r02 = m[8] / sz;
    r12 = m[9] / sz;
    r22 = m[10] / sz;
    tx = m[12];
    ty = m[13];
    tz = m[14];
    /* Inverse rotation part: (R S)^-1 = S^-1 R^T, i.e.
     * out[col*4+row] = R[col][row] / s_row (ROWS scale: row 0 by
     * 1/sx, row 1 by 1/sy, row 2 by 1/sz). */
    out_inv[0] = r00 / sx;
    out_inv[1] = r01 / sy;
    out_inv[2] = r02 / sz;
    out_inv[3] = 0.0f;
    out_inv[4] = r10 / sx;
    out_inv[5] = r11 / sy;
    out_inv[6] = r12 / sz;
    out_inv[7] = 0.0f;
    out_inv[8] = r20 / sx;
    out_inv[9] = r21 / sy;
    out_inv[10] = r22 / sz;
    out_inv[11] = 0.0f;
    /* Inverse translation: -(S^-1 R^T t). */
    itx = out_inv[0] * tx + out_inv[4] * ty + out_inv[8] * tz;
    ity = out_inv[1] * tx + out_inv[5] * ty + out_inv[9] * tz;
    itz = out_inv[2] * tx + out_inv[6] * ty + out_inv[10] * tz;
    out_inv[12] = -itx;
    out_inv[13] = -ity;
    out_inv[14] = -itz;
    out_inv[15] = 1.0f;
    return 1;
}

le_result le_matrix_decompose(const float matrix[16],
                              float out_position[3],
                              float out_rotation[4],
                              float out_scale[3]) {
    float bx;
    float by;
    float bz;
    float cx;
    float cy;
    float cz;
    float dx;
    float dy;
    float dz;
    float sx;
    float sy;
    float sz;
    float r00;
    float r10;
    float r20;
    float r01;
    float r11;
    float r21;
    float r02;
    float r12;
    float r22;
    float det;
    float trace;
    float q[4];
    /* Orthogonality check (shear detection): normalized columns
     * must be pairwise perpendicular within tolerance. */
    float dot_xy;
    float dot_xz;
    float dot_yz;

    if (matrix == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    bx = matrix[0];
    by = matrix[1];
    bz = matrix[2];
    cx = matrix[4];
    cy = matrix[5];
    cz = matrix[6];
    dx = matrix[8];
    dy = matrix[9];
    dz = matrix[10];
    sx = sqrtf(bx * bx + by * by + bz * bz);
    sy = sqrtf(cx * cx + cy * cy + cz * cz);
    sz = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!(sx > 1e-12f) || !(sy > 1e-12f) || !(sz > 1e-12f) ||
        !isfinite(sx) || !isfinite(sy) || !isfinite(sz)) {
        return LE_ERROR_UNREPRESENTABLE_TRANSFORM;
    }
    r00 = bx / sx;
    r10 = by / sx;
    r20 = bz / sx;
    r01 = cx / sy;
    r11 = cy / sy;
    r21 = cz / sy;
    r02 = dx / sz;
    r12 = dy / sz;
    r22 = dz / sz;
    /* Shear gate: off-diagonal dot products must be ~0 (tolerance
     * scales with float32 rounding over T*R*S chains). */
    dot_xy = r00 * r01 + r10 * r11 + r20 * r21;
    dot_xz = r00 * r02 + r10 * r12 + r20 * r22;
    dot_yz = r01 * r02 + r11 * r12 + r21 * r22;
    if (dot_xy > 1e-4f || dot_xy < -1e-4f || dot_xz > 1e-4f ||
        dot_xz < -1e-4f || dot_yz > 1e-4f || dot_yz < -1e-4f) {
        return LE_ERROR_UNREPRESENTABLE_TRANSFORM;
    }
    det = r00 * (r11 * r22 - r12 * r21) -
          r01 * (r10 * r22 - r12 * r20) +
          r02 * (r10 * r21 - r11 * r20);
    if (det < 0.0f) {
        if (sx <= sy && sx <= sz) {
            sx = -sx;
            r00 = -r00;
            r10 = -r10;
            r20 = -r20;
        } else if (sy <= sx && sy <= sz) {
            sy = -sy;
            r01 = -r01;
            r11 = -r11;
            r21 = -r21;
        } else {
            sz = -sz;
            r02 = -r02;
            r12 = -r12;
            r22 = -r22;
        }
    }
    trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        float u = sqrtf(trace + 1.0f) * 2.0f;

        q[3] = 0.25f * u;
        q[0] = (r21 - r12) / u;
        q[1] = (r02 - r20) / u;
        q[2] = (r10 - r01) / u;
    } else if (r00 > r11 && r00 > r22) {
        float u = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;

        q[3] = (r21 - r12) / u;
        q[0] = 0.25f * u;
        q[1] = (r01 + r10) / u;
        q[2] = (r02 + r20) / u;
    } else if (r11 > r22) {
        float u = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;

        q[3] = (r02 - r20) / u;
        q[0] = (r01 + r10) / u;
        q[1] = 0.25f * u;
        q[2] = (r12 + r21) / u;
    } else {
        float u = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;

        q[3] = (r10 - r01) / u;
        q[0] = (r02 + r20) / u;
        q[1] = (r12 + r21) / u;
        q[2] = 0.25f * u;
    }
    le_quat_normalize(q, q);
    if (out_position != NULL) {
        out_position[0] = matrix[12];
        out_position[1] = matrix[13];
        out_position[2] = matrix[14];
        if (!isfinite(out_position[0]) ||
            !isfinite(out_position[1]) ||
            !isfinite(out_position[2])) {
            return LE_ERROR_UNREPRESENTABLE_TRANSFORM;
        }
    }
    if (out_rotation != NULL) {
        memcpy(out_rotation, q, sizeof(q));
    }
    if (out_scale != NULL) {
        out_scale[0] = sx;
        out_scale[1] = sy;
        out_scale[2] = sz;
    }
    return LE_SUCCESS;
}

le_result le_object_reparent(le_world *world, const le_object *child,
                             const le_object *parent,
                             le_reparent_mode mode) {
    uint32_t child_slot;
    le_result code = LE_SUCCESS;
    float old_world[16];
    float parent_world[16];
    float parent_inv[16];
    float new_local[16];
    float pos[3];
    float rot[4];
    float scl[3];
    le_result dec;

    if (world == NULL || child == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (mode != LE_REPARENT_KEEP_LOCAL &&
        mode != LE_REPARENT_KEEP_WORLD) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (mode == LE_REPARENT_KEEP_LOCAL) {
        return le_object_set_parent(world, child, parent);
    }
    /* KEEP_WORLD: validate + snapshot BEFORE mutating (world
     * unchanged on every failure path). */
    if (!le_resolve_live(world, child, &child_slot, &code)) {
        return code;
    }
    {
        int has_parent = 0;
        uint32_t parent_slot = 0;

        if (parent != NULL && le_object_is_valid(parent)) {
            le_result pcode = LE_SUCCESS;

            if (!le_resolve_live(world, parent, &parent_slot,
                                 &pcode)) {
                return pcode;
            }
            has_parent = 1;
        } else if (parent != NULL &&
                   (parent->index != LE_OBJECT_INVALID.index ||
                    parent->generation !=
                        LE_OBJECT_INVALID.generation)) {
            return LE_ERROR_STALE_HANDLE;
        }
        if (has_parent) {
            if (parent_slot == child_slot) {
                return LE_ERROR_CYCLE;
            }
            if (le_is_ancestor(world, child_slot, parent_slot)) {
                return LE_ERROR_CYCLE;
            }
        }
        /* Snapshot matrices (ancestors-first refresh included).
         * Detach-to-root uses identity as the "new parent"
         * world (uniform path: no special-casing below). */
        le_object_get_world_matrix(world, child, old_world);
        if (has_parent) {
            le_object tmp = { parent_slot,
                              world->slots[parent_slot].generation,
                              world->tag };

            le_object_get_world_matrix(world, &tmp, parent_world);
            if (!le_mat4_inverse_trs(parent_world, parent_inv)) {
                return LE_ERROR_UNREPRESENTABLE_TRANSFORM;
            }
            le_mat4_multiply(new_local, parent_inv, old_world);
        } else {
            le_mat4_identity(parent_world);
            le_mat4_multiply(new_local, parent_world, old_world);
        }
        dec = le_matrix_decompose(new_local, pos, rot, scl);
        if (dec != LE_SUCCESS) {
            return dec;
        }
        /* Commit-time verification: parent_world x T*R*S(new_local
         * decomposition) must reproduce old_world within float
         * tolerance. If the decomposition lost anything (shear the
         * gate missed, mirror-axis ambiguity), fail instead of
         * committing a distorted transform. */
        {
            float check_local[16];
            float check_world[16];
            int k;
            int drift = 0;

            le_transform_compose(pos, rot, scl, check_local);
            le_mat4_multiply(check_world, parent_world,
                             check_local);
            for (k = 0; k < 16; k++) {
                float d = check_world[k] - old_world[k];
                float tol =
                    1e-3f *
                    (1.0f +
                     ((old_world[k] < 0.0f) ? -old_world[k]
                                           : old_world[k]));

                if (d > tol || d < -tol) {
                    drift = 1;
                    break;
                }
            }
            if (drift) {
                return LE_ERROR_UNREPRESENTABLE_TRANSFORM;
            }
        }
        /* Commit: reparent (KEEP_LOCAL, infallible post-checks)
         * then overwrite local. Setters cannot fail on finite
         * decomposed output; rotation normalizes canonically. */
        code = le_object_set_parent(world, child, parent);
        if (code != LE_SUCCESS) {
            return code;
        }
        /* Re-resolve (slot index stable across reparent). */
        if (!le_resolve_live(world, child, &child_slot, &code)) {
            return code;
        }
        memcpy(world->slots[child_slot].position, pos,
               sizeof(pos));
        memcpy(world->slots[child_slot].rotation, rot,
               sizeof(rot));
        memcpy(world->slots[child_slot].scale, scl, sizeof(scl));
        le_mark_subtree_dirty(world, child_slot);
        return LE_SUCCESS;
    }
}
