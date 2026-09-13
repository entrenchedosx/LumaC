/*
 * Renderer-local math + camera + transforms (Phase 13).
 *
 * Column-major matrices, Y-up right-handed world, Vulkan NDC
 * (depth 0..1, Y flipped in projection). No global state; pure
 * functions plus small struct helpers. Not a general math library by
 * design (a future LumaMath module may absorb it).
 */

#include <math.h>
#include <string.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

#ifndef LR_PI
#define LR_PI 3.14159265358979323846f
#endif

lr_result lr_map_result(lc_result res) {
    switch (res) {
    case LC_SUCCESS:
        return LR_SUCCESS;
    case LC_ERROR_INVALID_ARGUMENT:
        return LR_ERROR_INVALID_ARGUMENT;
    case LC_ERROR_NOT_INITIALIZED:
        return LR_ERROR_NOT_INITIALIZED;
    case LC_ERROR_OUT_OF_MEMORY:
        return LR_ERROR_OUT_OF_MEMORY;
    case LC_ERROR_UNSUPPORTED:
        return LR_ERROR_UNSUPPORTED;
    case LC_ERROR_PIPELINE_INCOMPATIBLE:
        return LR_ERROR_INCOMPATIBLE;
    default:
        return LR_ERROR_RENDER;
    }
}

void lr_mat4_identity(float *m) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

void lr_mat4_multiply(float *out, const float *a, const float *b) {
    float tmp[16];
    int col;
    int row;

    for (col = 0; col < 4; col++) {
        for (row = 0; row < 4; row++) {
            tmp[col * 4 + row] = a[0 * 4 + row] * b[col * 4 + 0] +
                                 a[1 * 4 + row] * b[col * 4 + 1] +
                                 a[2 * 4 + row] * b[col * 4 + 2] +
                                 a[3 * 4 + row] * b[col * 4 + 3];
        }
    }
    memcpy(out, tmp, sizeof(tmp));
}

void lr_mat4_perspective(float *m, float fov_y, float aspect, float near_z,
                         float far_z) {
    float f = 1.0f / tanf(fov_y * 0.5f);

    /* w = -z with near -> 0 / far -> 1 (verified against readback in
     * Phase 12); Y flipped for Vulkan framebuffers. */
    memset(m, 0, 16 * sizeof(float));
    m[0] = f / aspect;
    m[5] = -f;
    m[10] = -far_z / (far_z - near_z);
    m[11] = -1.0f;
    m[14] = -(far_z * near_z) / (far_z - near_z);
}

int lr_vec3_normalize(const float in[3], float out[3]) {
    float len = sqrtf(in[0] * in[0] + in[1] * in[1] + in[2] * in[2]);

    if (!(len > 1e-9f)) {
        return 0;
    }
    out[0] = in[0] / len;
    out[1] = in[1] / len;
    out[2] = in[2] / len;
    return 1;
}

/* Inverse-transpose of the upper 3x3 (column-major model matrix in,
 * 3 packed vec4 columns out for the push block). Correct normals and
 * tangents under rotation + non-uniform scale; never the raw model
 * matrix. Survives mirrors (sign folds into the cofactor, normalize
 * in-shader); fails only on singular input. */
int lr_mat3_normal_from_mat4(const float m[16], float out_normal[12]) {
    float a00 = m[0];
    float a10 = m[1];
    float a20 = m[2];
    float a01 = m[4];
    float a11 = m[5];
    float a21 = m[6];
    float a02 = m[8];
    float a12 = m[9];
    float a22 = m[10];
    float c00 = a11 * a22 - a12 * a21;
    float c01 = a12 * a20 - a10 * a22;
    float c02 = a10 * a21 - a11 * a20;
    float det = a00 * c00 + a01 * c01 + a02 * c02;
    float inv_det;

    if (!(det > 1e-12f) && !(det < -1e-12f)) {
        return 0;
    }
    inv_det = 1.0f / det;
    /* N = inverse-transpose = cofactor/det, emitted column-major
     * (column j of N is (C[0][j], C[1][j], C[2][j])); w lanes zero. */
    out_normal[0] = c00 * inv_det;
    out_normal[1] = (a02 * a21 - a01 * a22) * inv_det;
    out_normal[2] = (a01 * a12 - a02 * a11) * inv_det;
    out_normal[3] = 0.0f;
    out_normal[4] = c01 * inv_det;
    out_normal[5] = (a00 * a22 - a02 * a20) * inv_det;
    out_normal[6] = (a02 * a10 - a00 * a12) * inv_det;
    out_normal[7] = 0.0f;
    out_normal[8] = c02 * inv_det;
    out_normal[9] = (a01 * a20 - a00 * a21) * inv_det;
    out_normal[10] = (a00 * a11 - a01 * a10) * inv_det;
    out_normal[11] = 0.0f;
    return 1;
}

float lr_vec3_length(const float v[3]) {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

void lr_mat4_look_at(float *m, const float eye[3], const float center[3],
                     const float up[3]) {
    float f[3];
    float s[3];
    float u[3];
    float neg_eye[3];

    f[0] = center[0] - eye[0];
    f[1] = center[1] - eye[1];
    f[2] = center[2] - eye[2];
    if (!lr_vec3_normalize(f, f)) {
        lr_mat4_identity(m);
        return;
    }
    /* s = normalize(cross(f, up)); u = cross(s, f). */
    s[0] = f[1] * up[2] - f[2] * up[1];
    s[1] = f[2] * up[0] - f[0] * up[2];
    s[2] = f[0] * up[1] - f[1] * up[0];
    if (!lr_vec3_normalize(s, s)) {
        lr_mat4_identity(m);
        return;
    }
    u[0] = s[1] * f[2] - s[2] * f[1];
    u[1] = s[2] * f[0] - s[0] * f[2];
    u[2] = s[0] * f[1] - s[1] * f[0];
    neg_eye[0] = -eye[0];
    neg_eye[1] = -eye[1];
    neg_eye[2] = -eye[2];
    memset(m, 0, 16 * sizeof(float));
    m[0] = s[0];
    m[1] = u[0];
    m[2] = -f[0];
    m[4] = s[1];
    m[5] = u[1];
    m[6] = -f[1];
    m[8] = s[2];
    m[9] = u[2];
    m[10] = -f[2];
    m[12] = s[0] * neg_eye[0] + s[1] * neg_eye[1] + s[2] * neg_eye[2];
    m[13] = u[0] * neg_eye[0] + u[1] * neg_eye[1] + u[2] * neg_eye[2];
    m[14] = -f[0] * neg_eye[0] - f[1] * neg_eye[1] - f[2] * neg_eye[2];
    m[15] = 1.0f;
}

void lr_mat4_translate_view(float *m, const float basis[9],
                            const float eye[3]) {
    /* Rebuild a view matrix from an orthonormal basis (column-major
     * 3x3: sx,sy,sz, ux,uy,uz, -fx,-fy,-fz) plus a new eye. */
    memset(m, 0, 16 * sizeof(float));
    m[0] = basis[0];
    m[1] = basis[3];
    m[2] = basis[6];
    m[4] = basis[1];
    m[5] = basis[4];
    m[6] = basis[7];
    m[8] = basis[2];
    m[9] = basis[5];
    m[10] = basis[8];
    m[12] = -(basis[0] * eye[0] + basis[1] * eye[1] + basis[2] * eye[2]);
    m[13] = -(basis[3] * eye[0] + basis[4] * eye[1] + basis[5] * eye[2]);
    m[14] = -(basis[6] * eye[0] + basis[7] * eye[1] + basis[8] * eye[2]);
    m[15] = 1.0f;
}

void lr_camera_init(lr_camera *camera) {
    if (camera == NULL) {
        return;
    }
    memset(camera, 0, sizeof(*camera));
    lr_mat4_identity(camera->view);
    lr_mat4_identity(camera->projection);
    camera->near_plane = 0.1f;
    camera->far_plane = 100.0f;
    camera->vertical_fov = (float)(45.0 * LR_PI / 180.0);
    camera->aspect_ratio = 1.0f;
}

lr_result lr_camera_set_perspective(lr_camera *camera, float fov_y_rad,
                                    float aspect, float near_plane,
                                    float far_plane) {
    if (camera == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!(fov_y_rad > 0.0f) || !(fov_y_rad < (float)LR_PI) ||
        !(aspect > 0.0f) || !(near_plane > 0.0f) ||
        !(far_plane > near_plane)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    camera->vertical_fov = fov_y_rad;
    camera->aspect_ratio = aspect;
    camera->near_plane = near_plane;
    camera->far_plane = far_plane;
    lr_mat4_perspective(camera->projection, fov_y_rad, aspect, near_plane,
                        far_plane);
    return LR_SUCCESS;
}

lr_result lr_camera_look_at(lr_camera *camera, const float eye[3],
                            const float center[3], const float up[3]) {
    float f[3];

    if (camera == NULL || eye == NULL || center == NULL || up == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    f[0] = center[0] - eye[0];
    f[1] = center[1] - eye[1];
    f[2] = center[2] - eye[2];
    if (!(lr_vec3_length(f) > 1e-6f)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    {
        float s[3];

        s[0] = f[1] * up[2] - f[2] * up[1];
        s[1] = f[2] * up[0] - f[0] * up[2];
        s[2] = f[0] * up[1] - f[1] * up[0];
        if (!(lr_vec3_length(s) > 1e-6f)) {
            return LR_ERROR_INVALID_ARGUMENT;
        }
    }
    camera->position[0] = eye[0];
    camera->position[1] = eye[1];
    camera->position[2] = eye[2];
    lr_mat4_look_at(camera->view, eye, center, up);
    return LR_SUCCESS;
}

void lr_camera_set_position(lr_camera *camera, const float position[3]) {
    float basis[9];

    if (camera == NULL || position == NULL) {
        return;
    }
    /* Keep orientation: basis columns are the view rotation rows. */
    basis[0] = camera->view[0];
    basis[1] = camera->view[4];
    basis[2] = camera->view[8];
    basis[3] = camera->view[1];
    basis[4] = camera->view[5];
    basis[5] = camera->view[9];
    basis[6] = camera->view[2];
    basis[7] = camera->view[6];
    basis[8] = camera->view[10];
    camera->position[0] = position[0];
    camera->position[1] = position[1];
    camera->position[2] = position[2];
    lr_mat4_translate_view(camera->view, basis, position);
}

void lr_transform_identity(lr_transform *transform) {
    if (transform == NULL) {
        return;
    }
    transform->position[0] = 0.0f;
    transform->position[1] = 0.0f;
    transform->position[2] = 0.0f;
    transform->rotation[0] = 0.0f;
    transform->rotation[1] = 0.0f;
    transform->rotation[2] = 0.0f;
    transform->rotation[3] = 1.0f;
    transform->scale[0] = 1.0f;
    transform->scale[1] = 1.0f;
    transform->scale[2] = 1.0f;
}

void lr_quat_from_axis_angle(const float axis[3], float angle_rad,
                             float out_quat[4]) {
    float n[3];
    float half;
    float s;

    if (axis == NULL || out_quat == NULL) {
        return;
    }
    if (!lr_vec3_normalize(axis, n)) {
        out_quat[0] = 0.0f;
        out_quat[1] = 0.0f;
        out_quat[2] = 0.0f;
        out_quat[3] = 1.0f;
        return;
    }
    half = angle_rad * 0.5f;
    s = sinf(half);
    out_quat[0] = n[0] * s;
    out_quat[1] = n[1] * s;
    out_quat[2] = n[2] * s;
    out_quat[3] = cosf(half);
}

void lr_quat_multiply(const float a[4], const float b[4], float out[4]) {
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

void lr_transform_to_matrix(const lr_transform *transform,
                            float out_matrix[16]) {
    float rot[16];
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

    if (transform == NULL || out_matrix == NULL) {
        return;
    }
    x = transform->rotation[0];
    y = transform->rotation[1];
    z = transform->rotation[2];
    w = transform->rotation[3];
    xx = x * x;
    yy = y * y;
    zz = z * z;
    xy = x * y;
    xz = x * z;
    yz = y * z;
    wx = w * x;
    wy = w * y;
    wz = w * z;
    /* Column-major rotation. */
    memset(rot, 0, sizeof(rot));
    rot[0] = 1.0f - 2.0f * (yy + zz);
    rot[1] = 2.0f * (xy + wz);
    rot[2] = 2.0f * (xz - wy);
    rot[4] = 2.0f * (xy - wz);
    rot[5] = 1.0f - 2.0f * (xx + zz);
    rot[6] = 2.0f * (yz + wx);
    rot[8] = 2.0f * (xz + wy);
    rot[9] = 2.0f * (yz - wx);
    rot[10] = 1.0f - 2.0f * (xx + yy);
    rot[15] = 1.0f;
    /* T * R * S: scale rotation columns, then translate. */
    out_matrix[0] = rot[0] * transform->scale[0];
    out_matrix[1] = rot[1] * transform->scale[0];
    out_matrix[2] = rot[2] * transform->scale[0];
    out_matrix[3] = 0.0f;
    out_matrix[4] = rot[4] * transform->scale[1];
    out_matrix[5] = rot[5] * transform->scale[1];
    out_matrix[6] = rot[6] * transform->scale[1];
    out_matrix[7] = 0.0f;
    out_matrix[8] = rot[8] * transform->scale[2];
    out_matrix[9] = rot[9] * transform->scale[2];
    out_matrix[10] = rot[10] * transform->scale[2];
    out_matrix[11] = 0.0f;
    out_matrix[12] = transform->position[0];
    out_matrix[13] = transform->position[1];
    out_matrix[14] = transform->position[2];
    out_matrix[15] = 1.0f;
}

void lr_frustum_from_viewproj(const float vp[16], float planes[6][4]) {
    int i;

    if (vp == NULL || planes == NULL) {
        return;
    }
    /* Gribb/Hartmann extraction on rows (column-major assembly),
     * inward-facing, normalized. Order: left, right, bottom, top,
     * near, far. */
    {
        float rows[4][4];
        int r;
        int c;

        for (r = 0; r < 4; r++) {
            for (c = 0; c < 4; c++) {
                rows[r][c] = vp[c * 4 + r];
            }
        }
        for (c = 0; c < 4; c++) {
            planes[0][c] = rows[3][c] + rows[0][c]; /* left */
            planes[1][c] = rows[3][c] - rows[0][c]; /* right */
            planes[2][c] = rows[3][c] + rows[1][c]; /* bottom */
            planes[3][c] = rows[3][c] - rows[1][c]; /* top */
            planes[4][c] = rows[3][c] + rows[2][c]; /* near */
            planes[5][c] = rows[3][c] - rows[2][c]; /* far */
        }
    }
    for (i = 0; i < 6; i++) {
        float len = sqrtf(planes[i][0] * planes[i][0] +
                          planes[i][1] * planes[i][1] +
                          planes[i][2] * planes[i][2]);

        if (len > 1e-9f) {
            planes[i][0] /= len;
            planes[i][1] /= len;
            planes[i][2] /= len;
            planes[i][3] /= len;
        }
    }
}

int lr_frustum_test_sphere(float planes[6][4], const float center[3],
                            float radius) {
    int i;

    if (planes == NULL || center == NULL) {
        return 0;
    }
    for (i = 0; i < 6; i++) {
        float d = planes[i][0] * center[0] + planes[i][1] * center[1] +
                  planes[i][2] * center[2] + planes[i][3];

        if (d < -radius) {
            return 0;
        }
    }
    return 1;
}

/* General 4x4 inverse via Gauss-Jordan elimination with partial
 * pivoting on [M|I] (row-major scratch; storage stays column-major).
 * Deterministic, branch-light, obviously correct — preferred over a
 * hand-expanded adjugate here. */
int lr_mat4_inverse(const float m[16], float out_inv[16]) {
    double a[4][8];
    int r;
    int c;

    if (m == NULL || out_inv == NULL) {
        return 0;
    }
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            a[r][c] = (double)m[c * 4 + r];
        }
        for (c = 4; c < 8; c++) {
            a[r][c] = (r == c - 4) ? 1.0 : 0.0;
        }
    }
    for (c = 0; c < 4; c++) {
        int pivot = c;
        int rr;
        double div;

        for (rr = c + 1; rr < 4; rr++) {
            if (fabs(a[rr][c]) > fabs(a[pivot][c])) {
                pivot = rr;
            }
        }
        if (!(fabs(a[pivot][c]) > 1e-12)) {
            return 0;
        }
        if (pivot != c) {
            for (rr = 0; rr < 8; rr++) {
                double tmp = a[c][rr];

                a[c][rr] = a[pivot][rr];
                a[pivot][rr] = tmp;
            }
        }
        div = a[c][c];
        for (rr = 0; rr < 8; rr++) {
            a[c][rr] /= div;
        }
        for (r = 0; r < 4; r++) {
            if (r != c) {
                double f = a[r][c];

                for (rr = 0; rr < 8; rr++) {
                    a[r][rr] -= f * a[c][rr];
                }
            }
        }
    }
    for (r = 0; r < 4; r++) {
        for (c = 0; c < 4; c++) {
            out_inv[c * 4 + r] = (float)a[r][4 + c];
        }
    }
    return 1;
}

/* Orthographic projection WITH the renderer Y-flip (matching
 * lr_mat4_perspective, so one clip->UV rule serves both cameras):
 * view-up maps to negative clip Y, depth near->0 far->1. */
void lr_mat4_ortho(float *m, float left, float right, float bottom,
                   float top, float near_z, float far_z) {
    float rl;
    float tb;
    float fn;

    if (m == NULL) {
        return;
    }
    rl = right - left;
    tb = top - bottom;
    fn = far_z - near_z;
    memset(m, 0, 16 * sizeof(float));
    if (rl == 0.0f || tb == 0.0f || fn == 0.0f) {
        return;
    }
    m[0] = 2.0f / rl;
    m[5] = -2.0f / tb;
    m[10] = -1.0f / fn;
    m[12] = -(right + left) / rl;
    m[13] = (top + bottom) / tb;
    m[14] = -near_z / fn;
    m[15] = 1.0f;
}

/* ------------------------------------------------------------------
 * Shadow-camera math (Phase 16): fitted directional volumes with
 * structural translation stability. All column-major; pure
 * functions.
 * ------------------------------------------------------------------ */

/* Light basis from a travel direction (shared by fit + view so both
 * agree exactly): Z = -travel, X = norm(up_hint x Z), Y = Z x X. */
static int lr_shadow_basis(const float light_dir[3], float out_x[3],
                           float out_y[3], float out_z[3]) {
    float up[3];
    float x[3];

    out_z[0] = -light_dir[0];
    out_z[1] = -light_dir[1];
    out_z[2] = -light_dir[2];
    if (fabsf(light_dir[1]) > 0.99f) {
        up[0] = 1.0f;
        up[1] = 0.0f;
        up[2] = 0.0f;
    } else {
        up[0] = 0.0f;
        up[1] = 1.0f;
        up[2] = 0.0f;
    }
    /* x = norm(up x z) */
    x[0] = up[1] * out_z[2] - up[2] * out_z[1];
    x[1] = up[2] * out_z[0] - up[0] * out_z[2];
    x[2] = up[0] * out_z[1] - up[1] * out_z[0];
    if (!lr_vec3_normalize(x, out_x)) {
        return 0;
    }
    /* y = z x x */
    out_y[0] = out_z[1] * out_x[2] - out_z[2] * out_x[1];
    out_y[1] = out_z[2] * out_x[0] - out_z[0] * out_x[2];
    out_y[2] = out_z[0] * out_x[1] - out_z[1] * out_x[0];
    return 1;
}

void lr_shadow_frustum_corners(const float view_proj[16], float z_near_ndc,
                               float z_far_ndc, float out_corners[8][3]) {
    float inv[16];
    int i;

    if (view_proj == NULL || out_corners == NULL) {
        return;
    }
    if (!lr_mat4_inverse(view_proj, inv)) {
        memset(out_corners, 0, sizeof(float) * 8u * 3u);
        return;
    }
    for (i = 0; i < 8; i++) {
        float x = (i & 1) ? 1.0f : -1.0f;
        float y = (i & 2) ? 1.0f : -1.0f;
        float z = (i & 4) ? z_far_ndc : z_near_ndc;
        float p[4];

        p[0] = inv[0] * x + inv[4] * y + inv[8] * z + inv[12];
        p[1] = inv[1] * x + inv[5] * y + inv[9] * z + inv[13];
        p[2] = inv[2] * x + inv[6] * y + inv[10] * z + inv[14];
        p[3] = inv[3] * x + inv[7] * y + inv[11] * z + inv[15];
        if (p[3] != 0.0f) {
            out_corners[i][0] = p[0] / p[3];
            out_corners[i][1] = p[1] / p[3];
            out_corners[i][2] = p[2] / p[3];
        } else {
            out_corners[i][0] = 0.0f;
            out_corners[i][1] = 0.0f;
            out_corners[i][2] = 0.0f;
        }
    }
}

void lr_shadow_fit_directional(const float corners[8][3],
                               const float light_dir[3],
                               float out_bounds[6], float out_eye[3],
                               float out_view[16]) {
    float x[3];
    float y[3];
    float z[3];
    float mn[3];
    float mx[3];
    float ex;
    float ey;
    float ez;
    float backoff = 5.0f;
    float near_z = 0.1f;
    float far_z;
    float center_w[3];
    int i;

    if (corners == NULL || light_dir == NULL || out_bounds == NULL ||
        out_eye == NULL || out_view == NULL) {
        return;
    }
    {
        float dir[3];

        /* Reuse the submit-time normalization contract (callers pass
         * normalized directions; tolerate anything nonzero). */
        if (!lr_vec3_normalize(light_dir, dir)) {
            return;
        }
        if (!lr_shadow_basis(dir, x, y, z)) {
            return;
        }
        mn[0] = x[0] * corners[0][0] + x[1] * corners[0][1] +
                x[2] * corners[0][2];
        mn[1] = y[0] * corners[0][0] + y[1] * corners[0][1] +
                y[2] * corners[0][2];
        mn[2] = z[0] * corners[0][0] + z[1] * corners[0][1] +
                z[2] * corners[0][2];
        mx[0] = mn[0];
        mx[1] = mn[1];
        mx[2] = mn[2];
        for (i = 1; i < 8; i++) {
            float lx = x[0] * corners[i][0] + x[1] * corners[i][1] +
                       x[2] * corners[i][2];
            float ly = y[0] * corners[i][0] + y[1] * corners[i][1] +
                       y[2] * corners[i][2];
            float lz = z[0] * corners[i][0] + z[1] * corners[i][1] +
                       z[2] * corners[i][2];

            if (lx < mn[0]) {
                mn[0] = lx;
            }
            if (lx > mx[0]) {
                mx[0] = lx;
            }
            if (ly < mn[1]) {
                mn[1] = ly;
            }
            if (ly > mx[1]) {
                mx[1] = ly;
            }
            if (lz < mn[2]) {
                mn[2] = lz;
            }
            if (lz > mx[2]) {
                mx[2] = lz;
            }
        }
        ex = (mx[0] - mn[0]) * 0.5f;
        ey = (mx[1] - mn[1]) * 0.5f;
        ez = (mx[2] - mn[2]) * 0.5f;
        /* Degenerate slices cannot happen for perspective cameras,
         * but never divide by zero downstream. */
        if (ex < 1e-4f) {
            ex = 1e-4f;
        }
        if (ey < 1e-4f) {
            ey = 1e-4f;
        }
        if (ez < 1e-4f) {
            ez = 1e-4f;
        }
        /* Centered bounds (eye-frame symmetric by construction):
         * translation moves the fitted slice rigidly, so extents —
         * and hence the projection — are translation-invariant.
         * That structural property (not grid snapping) is what keeps
         * shadows from crawling; rotation genuinely refits. */
        out_bounds[0] = -ex;
        out_bounds[1] = ex;
        out_bounds[2] = -ey;
        out_bounds[3] = ey;
        far_z = 2.0f * ez + backoff + 2.0f;
        out_bounds[4] = near_z;
        out_bounds[5] = far_z;
        /* World fit center back out of light space, then the eye sits
         * up-travel from it (lights shine from -travel side). */
        center_w[0] = x[0] * (mn[0] + mx[0]) * 0.5f +
                      y[0] * (mn[1] + mx[1]) * 0.5f +
                      z[0] * (mn[2] + mx[2]) * 0.5f;
        center_w[1] = x[1] * (mn[0] + mx[0]) * 0.5f +
                      y[1] * (mn[1] + mx[1]) * 0.5f +
                      z[1] * (mn[2] + mx[2]) * 0.5f;
        center_w[2] = x[2] * (mn[0] + mx[0]) * 0.5f +
                      y[2] * (mn[1] + mx[1]) * 0.5f +
                      z[2] * (mn[2] + mx[2]) * 0.5f;
        out_eye[0] = center_w[0] - dir[0] * (ez + backoff);
        out_eye[1] = center_w[1] - dir[1] * (ez + backoff);
        out_eye[2] = center_w[2] - dir[2] * (ez + backoff);
        {
            /* Final view from the same basis (eye + axes agree). */
            float basis[9];

            basis[0] = x[0];
            basis[1] = x[1];
            basis[2] = x[2];
            basis[3] = y[0];
            basis[4] = y[1];
            basis[5] = y[2];
            basis[6] = z[0];
            basis[7] = z[1];
            basis[8] = z[2];
            lr_mat4_translate_view(out_view, basis, out_eye);
        }
    }
}

void lr_shadow_light_view(const float eye[3], const float target[3],
                          float out_view[16]) {
    float fwd[3];
    float up[3];

    if (eye == NULL || target == NULL || out_view == NULL) {
        return;
    }
    fwd[0] = target[0] - eye[0];
    fwd[1] = target[1] - eye[1];
    fwd[2] = target[2] - eye[2];
    if (!lr_vec3_normalize(fwd, fwd)) {
        lr_mat4_identity(out_view);
        return;
    }
    /* Same degenerate-up rule as the fit basis. */
    if (fabsf(fwd[1]) > 0.99f) {
        up[0] = 1.0f;
        up[1] = 0.0f;
        up[2] = 0.0f;
    } else {
        up[0] = 0.0f;
        up[1] = 1.0f;
        up[2] = 0.0f;
    }
    lr_mat4_look_at(out_view, eye, target, up);
}
