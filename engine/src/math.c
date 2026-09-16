/*
 * Luma Engine math (engine-local minimum: identity, multiply,
 * T*R*S compose, quaternion normalize/multiply/axis-angle, mirror
 * parity). Column-major, Y-up RH, Vulkan NDC — same convention as
 * the renderer, but an independent copy: neither layer includes
 * the other's internals.
 */

#include <math.h>
#include <stddef.h>

#include "luma_engine/luma_engine.h"

static int le_finite3(const float v[3]) {
    int i;

    for (i = 0; i < 3; i++) {
        if (!isfinite(v[i])) {
            return 0;
        }
    }
    return 1;
}

void le_mat4_identity(float out_matrix[16]) {
    int i;

    if (out_matrix == NULL) {
        return;
    }
    for (i = 0; i < 16; i++) {
        out_matrix[i] = 0.0f;
    }
    out_matrix[0] = 1.0f;
    out_matrix[5] = 1.0f;
    out_matrix[10] = 1.0f;
    out_matrix[15] = 1.0f;
}

void le_mat4_multiply(float out[16], const float a[16],
                      const float b[16]) {
    float tmp[16];
    int col;
    int row;

    if (out == NULL || a == NULL || b == NULL) {
        return;
    }
    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            tmp[col * 4 + row] =
                a[0 * 4 + row] * b[col * 4 + 0] +
                a[1 * 4 + row] * b[col * 4 + 1] +
                a[2 * 4 + row] * b[col * 4 + 2] +
                a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    for (col = 0; col < 16; col++) {
        out[col] = tmp[col];
    }
}

void le_quat_normalize(const float in[4], float out[4]) {
    double len;

    if (in == NULL || out == NULL) {
        return;
    }
    if (!isfinite(in[0]) || !isfinite(in[1]) || !isfinite(in[2]) ||
        !isfinite(in[3])) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    len = sqrt((double)in[0] * in[0] + (double)in[1] * in[1] +
               (double)in[2] * in[2] + (double)in[3] * in[3]);
    if (!(len > 1e-12)) {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
        out[3] = 1.0f;
        return;
    }
    out[0] = (float)(in[0] / len);
    out[1] = (float)(in[1] / len);
    out[2] = (float)(in[2] / len);
    out[3] = (float)(in[3] / len);
}

void le_quat_multiply(const float a[4], const float b[4],
                      float out[4]) {
    float r[4];

    if (a == NULL || b == NULL || out == NULL) {
        return;
    }
    r[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    r[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    r[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    r[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = r[0];
    out[1] = r[1];
    out[2] = r[2];
    out[3] = r[3];
}

void le_quat_from_axis_angle(const float axis[3], float angle_rad,
                             float out_quat[4]) {
    double len;
    double half;
    double s;

    if (axis == NULL || out_quat == NULL) {
        return;
    }
    if (!le_finite3(axis) || !isfinite(angle_rad)) {
        out_quat[0] = 0.0f;
        out_quat[1] = 0.0f;
        out_quat[2] = 0.0f;
        out_quat[3] = 1.0f;
        return;
    }
    len = sqrt((double)axis[0] * axis[0] + (double)axis[1] * axis[1] +
               (double)axis[2] * axis[2]);
    if (!(len > 1e-12)) {
        out_quat[0] = 0.0f;
        out_quat[1] = 0.0f;
        out_quat[2] = 0.0f;
        out_quat[3] = 1.0f;
        return;
    }
    half = (double)angle_rad * 0.5;
    s = sin(half) / len;
    out_quat[0] = (float)(axis[0] * s);
    out_quat[1] = (float)(axis[1] * s);
    out_quat[2] = (float)(axis[2] * s);
    out_quat[3] = (float)cos(half);
}

void le_transform_compose(const float position[3],
                          const float rotation[4],
                          const float scale[3], float out_matrix[16]) {
    float x;
    float y;
    float z;
    float w;
    float xx;
    float yy;
    float zz;
    float xy;
    float xz;
    float yz;
    float wx;
    float wy;
    float wz;
    float r00;
    float r10;
    float r20;
    float r01;
    float r11;
    float r21;
    float r02;
    float r12;
    float r22;

    if (position == NULL || rotation == NULL || scale == NULL ||
        out_matrix == NULL) {
        return;
    }
    x = rotation[0];
    y = rotation[1];
    z = rotation[2];
    w = rotation[3];
    xx = x * x;
    yy = y * y;
    zz = z * z;
    xy = x * y;
    xz = x * z;
    yz = y * z;
    wx = w * x;
    wy = w * y;
    wz = w * z;
    /* Column-major rotation from (possibly unnormalized — callers
     * store normalized; compose defensively tolerates drift). */
    r00 = 1.0f - 2.0f * (yy + zz);
    r10 = 2.0f * (xy + wz);
    r20 = 2.0f * (xz - wy);
    r01 = 2.0f * (xy - wz);
    r11 = 1.0f - 2.0f * (xx + zz);
    r21 = 2.0f * (yz + wx);
    r02 = 2.0f * (xz + wy);
    r12 = 2.0f * (yz - wx);
    r22 = 1.0f - 2.0f * (xx + yy);
    /* T * R * S: scale rotation columns, then translate. */
    out_matrix[0] = r00 * scale[0];
    out_matrix[1] = r10 * scale[0];
    out_matrix[2] = r20 * scale[0];
    out_matrix[3] = 0.0f;
    out_matrix[4] = r01 * scale[1];
    out_matrix[5] = r11 * scale[1];
    out_matrix[6] = r21 * scale[1];
    out_matrix[7] = 0.0f;
    out_matrix[8] = r02 * scale[2];
    out_matrix[9] = r12 * scale[2];
    out_matrix[10] = r22 * scale[2];
    out_matrix[11] = 0.0f;
    out_matrix[12] = position[0];
    out_matrix[13] = position[1];
    out_matrix[14] = position[2];
    out_matrix[15] = 1.0f;
}

int le_matrix_is_mirrored(const float m[16]) {
    float a00;
    float a10;
    float a20;
    float a01;
    float a11;
    float a21;
    float a02;
    float a12;
    float a22;
    float c00;
    float c01;
    float c02;
    float det;

    if (m == NULL) {
        return 0;
    }
    a00 = m[0];
    a10 = m[1];
    a20 = m[2];
    a01 = m[4];
    a11 = m[5];
    a21 = m[6];
    a02 = m[8];
    a12 = m[9];
    a22 = m[10];
    c00 = a11 * a22 - a12 * a21;
    c01 = a12 * a20 - a10 * a22;
    c02 = a10 * a21 - a11 * a20;
    det = a00 * c00 + a01 * c01 + a02 * c02;
    return (det < 0.0f) ? 1 : 0;
}
