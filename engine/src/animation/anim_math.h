/*
 * Animation math helpers (Phase 29): shared TRS/quat/matrix
 * routines for the sampler, pose evaluation, and CPU skinning.
 * Column-major 4x4, Y-up RH, quats (x,y,z,w) — same conventions
 * as the engine transform layer.
 */

#ifndef LE_ANIM_MATH_H
#define LE_ANIM_MATH_H

#include <math.h>
#include <string.h>

/* Finite checks without depending on engine math. */
static int le_am_finite(float v) {
    return isfinite(v) != 0;
}

static int le_am_finite3(const float v[3]) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static int le_am_finite4(const float v[4]) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]) &&
           isfinite(v[3]);
}

/* Normalize a quat; returns 0 and writes identity on zero-length
 * or non-finite input (never NaN). */
static int le_am_quat_normalize(const float q[4], float out[4]) {
    float n;

    if (q == NULL || out == NULL) {
        return 0;
    }
    n = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
    if (!(n > 1e-12f) || !isfinite(n)) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
        return 0;
    }
    n = sqrtf(n);
    out[0] = q[0] / n;
    out[1] = q[1] / n;
    out[2] = q[2] / n;
    out[3] = q[3] / n;
    return 1;
}

/* Shortest-path SLERP (hemisphere flip; linear fallback for
 * near-identical inputs). Inputs need not be unit (normalized
 * internally); outputs are unit. */
static void le_am_quat_slerp(const float a[4], const float b[4],
                              float t, float out[4]) {
    float na[4];
    float nb[4];
    float dot;
    float flip = 1.0f;

    le_am_quat_normalize(a, na);
    le_am_quat_normalize(b, nb);
    dot = na[0] * nb[0] + na[1] * nb[1] + na[2] * nb[2] +
          na[3] * nb[3];
    if (dot < 0.0f) {
        /* Same rotation, opposite hemisphere: flip for the
         * shortest path (q and -q test). */
        flip = -1.0f;
        dot = -dot;
    }
    if (dot > 0.9995f) {
        /* Near-identical: normalized lerp (cheap, stable). */
        float x = na[0] + (nb[0] * flip - na[0]) * t;
        float y = na[1] + (nb[1] * flip - na[1]) * t;
        float z = na[2] + (nb[2] * flip - na[2]) * t;
        float w = na[3] + (nb[3] * flip - na[3]) * t;
        float q[4] = { x, y, z, w };

        le_am_quat_normalize(q, out);
        return;
    }
    {
        float theta = acosf(dot > 1.0f ? 1.0f : dot);
        float s = sinf(theta);
        float wa = sinf((1.0f - t) * theta) / s;
        float wb = sinf(t * theta) / s * flip;
        float q[4] = { na[0] * wa + nb[0] * wb,
                       na[1] * wa + nb[1] * wb,
                       na[2] * wa + nb[2] * wb,
                       na[3] * wa + nb[3] * wb };

        le_am_quat_normalize(q, out);
    }
}

/* Compose local TRS into a column-major matrix (M = T * R * S). */
static void le_am_compose_trs(const float t[3], const float r[4],
                               const float s[3], float out[16]) {
    float x = r[0];
    float y = r[1];
    float z = r[2];
    float w = r[3];
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;
    float xy = x * y;
    float xz = x * z;
    float yz = y * z;
    float wx = w * x;
    float wy = w * y;
    float wz = w * z;
    /* Rotation rows (then scaled per column). */
    float r00 = 1.0f - 2.0f * (yy + zz);
    float r10 = 2.0f * (xy + wz);
    float r20 = 2.0f * (xz - wy);
    float r01 = 2.0f * (xy - wz);
    float r11 = 1.0f - 2.0f * (xx + zz);
    float r21 = 2.0f * (yz + wx);
    float r02 = 2.0f * (xz + wy);
    float r12 = 2.0f * (yz - wx);
    float r22 = 1.0f - 2.0f * (xx + yy);

    out[0] = r00 * s[0];
    out[1] = r10 * s[0];
    out[2] = r20 * s[0];
    out[3] = 0.0f;
    out[4] = r01 * s[1];
    out[5] = r11 * s[1];
    out[6] = r21 * s[1];
    out[7] = 0.0f;
    out[8] = r02 * s[2];
    out[9] = r12 * s[2];
    out[10] = r22 * s[2];
    out[11] = 0.0f;
    out[12] = t[0];
    out[13] = t[1];
    out[14] = t[2];
    out[15] = 1.0f;
}

/* out = a * b (column-major). */
static void le_am_mat4_mul(const float a[16], const float b[16],
                            float out[16]) {
    float t[16];
    int r;
    int c;
    int k;

    for (c = 0; c < 4; c++) {
        for (r = 0; r < 4; r++) {
            float sum = 0.0f;

            for (k = 0; k < 4; k++) {
                sum += a[k * 4 + r] * b[c * 4 + k];
            }
            t[c * 4 + r] = sum;
        }
    }
    memcpy(out, t, sizeof(t));
}

/* Transform a point by a column-major matrix (w = 1). */
static void le_am_mat4_point(const float m[16], const float p[3],
                              float out[3]) {
    out[0] = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12];
    out[1] = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13];
    out[2] = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14];
}

#endif /* LE_ANIM_MATH_H */
