/*
 * glTF 2.0 import (Phase 14, PART B/H–AA): parse via vendored cgltf,
 * validate strictly, convert accessors with stride/offset/normalized
 * handling, generate missing normals/tangents, resolve hierarchy and
 * materials, upload through public renderer/LumaC APIs.
 *
 * Determinism: file order is preserved everywhere (nodes, meshes,
 * primitives, materials, textures); caches are insertion-ordered
 * lists, never hash iteration.
 *
 * Conventions (documented once, PART G/L):
 * - glTF is Y-up right-handed with column-major matrices and CCW
 *   front faces — exactly Luma's world convention — so node matrices
 *   copy through UNTRANSPOSED and winding is preserved.
 * - glTF quaternions are (x, y, z, w), matching lr_transform.
 * - UV origin is top-left in both glTF and Vulkan: no V flip, and
 *   images upload row-major exactly as decoded (never flip one
 *   without the other).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include "luma_assets/luma_assets.h"
#include "internal/assets_internal.h"

/* ------------------------------------------------------------------
 * Import context (per-load scratch).
 * ------------------------------------------------------------------ */

typedef struct la_import {
    la_asset_manager *manager;
    const char *path; /* borrowed source path (for keys/messages) */
    char *dir; /* owned glTF directory (external resolution) */
    cgltf_data *data;
    la_model *model;
    char failed_path[256]; /* file-callback failure record */
} la_import;

/* Centralized file read for cgltf (PART AH): records the failing
 * path so missing externals map to NOT_FOUND precisely. */
static cgltf_result la_cgltf_read(
    const struct cgltf_memory_options *memory_options,
    const struct cgltf_file_options *file_options, const char *path,
    cgltf_size *size, void **data) {
    la_import *imp = (la_import *)file_options->user_data;
    unsigned char *bytes = NULL;
    size_t length = 0;
    la_result res;

    (void)memory_options;
    res = la_fs_read(path, &bytes, &length);
    if (res == LA_ERROR_NOT_FOUND) {
        if (imp != NULL && path != NULL) {
            snprintf(imp->failed_path, sizeof(imp->failed_path), "%s",
                     path);
        }
        return cgltf_result_io_error;
    }
    if (res != LA_SUCCESS) {
        return cgltf_result_io_error;
    }
    *size = (cgltf_size)length;
    *data = bytes;
    return cgltf_result_success;
}

static void la_cgltf_release(
    const struct cgltf_memory_options *memory_options,
    const struct cgltf_file_options *file_options, void *data) {
    (void)memory_options;
    (void)file_options;
    la_fs_free((unsigned char *)data);
}

/* ------------------------------------------------------------------
 * Strict base64 (data: URIs). Rejects bad characters, bad padding,
 * and truncated quanta — corrupt URIs are IMPORT errors, not silent
 * truncation.
 * ------------------------------------------------------------------ */

static int la_b64_value(char c, unsigned *out) {
    if (c >= 'A' && c <= 'Z') {
        *out = (unsigned)(c - 'A');
        return 1;
    }
    if (c >= 'a' && c <= 'z') {
        *out = (unsigned)(c - 'a' + 26u);
        return 1;
    }
    if (c >= '0' && c <= '9') {
        *out = (unsigned)(c - '0' + 52u);
        return 1;
    }
    if (c == '+') {
        *out = 62u;
        return 1;
    }
    if (c == '/') {
        *out = 63u;
        return 1;
    }
    return 0;
}

static la_result la_b64_decode(const char *text, size_t len,
                               unsigned char **out_bytes, size_t *out_size) {
    unsigned char *bytes;
    size_t full_quanta;
    size_t rem;
    size_t i;
    size_t o;

    if (text == NULL || out_bytes == NULL || out_size == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    *out_bytes = NULL;
    *out_size = 0;
    if (len == 0 || (len % 4u) != 0u) {
        return LA_ERROR_IMPORT;
    }
    full_quanta = len / 4u;
    bytes = (unsigned char *)malloc(full_quanta * 3u + 1u);
    if (bytes == NULL) {
        return LA_ERROR_OUT_OF_MEMORY;
    }
    o = 0;
    for (i = 0; i < full_quanta; i++) {
        unsigned v[4];
        int pad = 0;
        size_t k;

        for (k = 0; k < 4u; k++) {
            char c = text[i * 4u + k];

            if (c == '=') {
                /* Padding only valid as the last 1-2 chars overall. */
                if (i != full_quanta - 1u || k < 2u) {
                    free(bytes);
                    return LA_ERROR_IMPORT;
                }
                v[k] = 0;
                pad++;
            } else if (!la_b64_value(c, &v[k])) {
                free(bytes);
                return LA_ERROR_IMPORT;
            }
        }
        if (pad > 2) {
            free(bytes);
            return LA_ERROR_IMPORT;
        }
        bytes[o++] = (unsigned char)((v[0] << 2) | (v[1] >> 4));
        if (pad < 2) {
            bytes[o++] = (unsigned char)(((v[1] & 0xFu) << 4) | (v[2] >> 2));
        }
        if (pad < 1) {
            bytes[o++] =
                (unsigned char)(((v[2] & 0x3u) << 6) | v[3]);
        }
    }
    rem = o;
    *out_bytes = bytes;
    *out_size = rem;
    return LA_SUCCESS;
}

/* Split "data:[<mediatype>][;base64],<payload>". Only base64 payloads
 * are supported (plain-text URIs are vanishingly rare); returns the
 * payload span for decoding. */
static la_result la_data_uri_payload(const char *uri, const char **out_body,
                                     size_t *out_len) {
    const char *comma;
    const char *semi;

    if (uri == NULL || out_body == NULL || out_len == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (strncmp(uri, "data:", 5) != 0) {
        return LA_ERROR_NOT_FOUND; /* not a data URI at all */
    }
    comma = strchr(uri, ',');
    if (comma == NULL || comma[1] == '\0') {
        return LA_ERROR_IMPORT;
    }
    semi = strchr(uri, ';');
    if (semi == NULL || semi > comma ||
        strncmp(semi, ";base64", (size_t)(comma - semi)) != 0) {
        return LA_ERROR_UNSUPPORTED;
    }
    *out_body = comma + 1;
    *out_len = strlen(comma + 1);
    return LA_SUCCESS;
}

/* ------------------------------------------------------------------
 * Accessor readers (PART AA): stride/offset/normalized aware,
 * bounds-checked against the buffer on every element. Sparse
 * accessors are rejected (documented; cgltf parses the metadata).
 * ------------------------------------------------------------------ */

static la_result la_accessor_bytes(const cgltf_accessor *accessor,
                                   const unsigned char **out_base,
                                   size_t *out_size) {
    const cgltf_buffer_view *view;

    if (accessor == NULL || accessor->buffer_view == NULL ||
        out_base == NULL || out_size == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (accessor->is_sparse) {
        return LA_ERROR_UNSUPPORTED;
    }
    view = accessor->buffer_view;
    if (view->buffer == NULL || view->buffer->data == NULL) {
        return LA_ERROR_IMPORT;
    }
    *out_base = (const unsigned char *)view->buffer->data;
    *out_size = view->buffer->size;
    return LA_SUCCESS;
}

static uint32_t la_component_size(cgltf_component_type type) {
    switch (type) {
    case cgltf_component_type_r_8:
    case cgltf_component_type_r_8u:
        return 1u;
    case cgltf_component_type_r_16:
    case cgltf_component_type_r_16u:
        return 2u;
    case cgltf_component_type_r_32f:
    case cgltf_component_type_r_32u:
        return 4u;
    default:
        return 0u;
    }
}

static uint32_t la_type_components(cgltf_type type) {
    switch (type) {
    case cgltf_type_scalar:
        return 1u;
    case cgltf_type_vec2:
        return 2u;
    case cgltf_type_vec3:
        return 3u;
    case cgltf_type_vec4:
        return 4u;
    default:
        return 0u;
    }
}

/* Normalized integer element as float (glTF 3.6.2: signed maps
 * c/max clamped to [-1,1]; unsigned maps c/max). TEXCOORD forms. */
static float la_int_to_float(const unsigned char *p,
                             cgltf_component_type type) {
    switch (type) {
    case cgltf_component_type_r_8: {
        int v = (signed char)p[0];

        if (v <= -127) {
            return -1.0f;
        }
        return (float)v / 127.0f;
    }
    case cgltf_component_type_r_8u:
        return (float)p[0] / 255.0f;
    case cgltf_component_type_r_16: {
        int v = (int)(int16_t)(p[0] | ((unsigned)p[1] << 8));

        if (v <= -32767) {
            return -1.0f;
        }
        return (float)v / 32767.0f;
    }
    case cgltf_component_type_r_16u: {
        unsigned v = (unsigned)p[0] | ((unsigned)p[1] << 8);

        return (float)v / 65535.0f;
    }
    default:
        return 0.0f;
    }
}

/* Read a VECn float attribute (float32, or normalized u8/u16 for
 * TEXCOORD forms). `comps` is the expected component count. */
static la_result la_read_float_attr(const cgltf_accessor *accessor,
                                    uint32_t comps, float *out) {
    const unsigned char *base;
    size_t buf_size;
    size_t elem_size;
    size_t stride;
    size_t view_off;
    size_t i;
    la_result res;

    if (accessor == NULL || out == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (la_type_components(accessor->type) != comps ||
        accessor->count > UINT32_MAX) {
        return LA_ERROR_IMPORT;
    }
    if (accessor->component_type != cgltf_component_type_r_32f &&
        !(accessor->normalized &&
          (accessor->component_type == cgltf_component_type_r_8u ||
           accessor->component_type == cgltf_component_type_r_16u))) {
        /* POSITION/NORMAL/TANGENT must be float32 per spec; integer
         * TEXCOORD forms beyond normalized u8/u16 are rejected. */
        return LA_ERROR_IMPORT;
    }
    res = la_accessor_bytes(accessor, &base, &buf_size);
    if (res != LA_SUCCESS) {
        return res;
    }
    elem_size = (size_t)la_component_size(accessor->component_type) * comps;
    if (elem_size == 0) {
        return LA_ERROR_IMPORT;
    }
    stride = accessor->stride > 0 ? accessor->stride : elem_size;
    if (stride < elem_size) {
        return LA_ERROR_IMPORT;
    }
    view_off = accessor->buffer_view->offset + accessor->offset;
    for (i = 0; i < accessor->count; i++) {
        size_t at = view_off + i * stride;
        uint32_t c;

        if (at > buf_size || elem_size > buf_size - at) {
            return LA_ERROR_IMPORT;
        }
        for (c = 0; c < comps; c++) {
            if (accessor->component_type == cgltf_component_type_r_32f) {
                float v;

                memcpy(&v, base + at + (size_t)c * 4u, 4u);
                out[i * comps + c] = v;
            } else {
                out[i * comps + c] = la_int_to_float(
                    base + at +
                        (size_t)c *
                            la_component_size(accessor->component_type),
                    accessor->component_type);
            }
        }
    }
    return LA_SUCCESS;
}

/* Read indices (u8/u16/u32) widened to u32 with range validation. */
static la_result la_read_indices(const cgltf_accessor *accessor,
                                 uint32_t vertex_count, uint32_t *out) {
    const unsigned char *base;
    size_t buf_size;
    size_t stride;
    size_t view_off;
    size_t i;
    la_result res;
    uint32_t stride_u32;

    if (accessor == NULL || out == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (accessor->type != cgltf_type_scalar || accessor->count > UINT32_MAX ||
        accessor->normalized) {
        return LA_ERROR_IMPORT;
    }
    if (accessor->component_type != cgltf_component_type_r_8u &&
        accessor->component_type != cgltf_component_type_r_16u &&
        accessor->component_type != cgltf_component_type_r_32u) {
        return LA_ERROR_UNSUPPORTED;
    }
    res = la_accessor_bytes(accessor, &base, &buf_size);
    if (res != LA_SUCCESS) {
        return res;
    }
    stride_u32 = la_component_size(accessor->component_type);
    stride = accessor->stride > 0 ? accessor->stride : stride_u32;
    if (stride < stride_u32) {
        return LA_ERROR_IMPORT;
    }
    view_off = accessor->buffer_view->offset + accessor->offset;
    for (i = 0; i < accessor->count; i++) {
        size_t at = view_off + i * stride;
        uint32_t v = 0;

        if (at > buf_size || stride_u32 > buf_size - at) {
            return LA_ERROR_IMPORT;
        }
        if (accessor->component_type == cgltf_component_type_r_8u) {
            v = base[at];
        } else if (accessor->component_type == cgltf_component_type_r_16u) {
            v = (uint32_t)base[at] | ((uint32_t)base[at + 1u] << 8);
        } else {
            memcpy(&v, base + at, 4u);
        }
        if (v >= vertex_count) {
            return LA_ERROR_IMPORT;
        }
        out[i] = v;
    }
    return LA_SUCCESS;
}

/* ------------------------------------------------------------------
 * Skin attributes (Phase 29): JOINTS_0/WEIGHTS_0 decode.
 *
 * glTF 2.0 fixes JOINTS_0 to VEC4 UNSIGNED_BYTE/UNSIGNED_SHORT
 * (scalar-per-component, never normalized) and WEIGHTS_0 to
 * VEC4 FLOAT (or normalized u8/u16). Four influences max:
 * JOINTS_1/WEIGHTS_1 presence fails the import loudly (never
 * silently dropped). Missing JOINTS_0/WEIGHTS_0 selects rigid
 * defaults ({0,0,0,0} + {1,0,0,0}); when exactly one is present
 * the other defaults. Weights normalize per vertex (all-zero
 * falls back to rigid; never NaN).
 * ------------------------------------------------------------------ */

/* Widen one JOINTS_0 accessor to uint32 per component. */
la_result la_decode_joints(const cgltf_accessor *accessor,
                           uint32_t vertex_count, uint32_t *out) {
    const unsigned char *base;
    size_t buf_size;
    size_t elem_size;
    size_t stride;
    size_t view_off;
    size_t i;
    la_result res;
    uint32_t comp_size;

    if (accessor == NULL || out == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (accessor->type != cgltf_type_vec4 ||
        accessor->count != (cgltf_size)vertex_count) {
        return LA_ERROR_IMPORT;
    }
    if (accessor->normalized ||
        (accessor->component_type != cgltf_component_type_r_8u &&
         accessor->component_type != cgltf_component_type_r_16u)) {
        /* JOINTS are indices, never normalized floats: any other
         * component type (or a normalized flag) is a spec
         * violation in the component-type sense. */
        return LA_ERROR_UNSUPPORTED;
    }
    res = la_accessor_bytes(accessor, &base, &buf_size);
    if (res != LA_SUCCESS) {
        return res;
    }
    comp_size = la_component_size(accessor->component_type);
    elem_size = (size_t)comp_size * 4u;
    /* cgltf sets stride 0 for tightly packed accessors. */
    if (accessor->stride == 0) {
        stride = elem_size;
    } else {
        stride = accessor->stride;
    }
    if (stride < elem_size) {
        return LA_ERROR_IMPORT;
    }
    view_off = accessor->buffer_view->offset + accessor->offset;
    for (i = 0; i < (size_t)vertex_count; i++) {
        size_t at = view_off + i * stride;
        uint32_t c;

        if (at > buf_size || elem_size > buf_size - at) {
            return LA_ERROR_IMPORT;
        }
        for (c = 0; c < 4u; c++) {
            const unsigned char *p =
                base + at + (size_t)c * comp_size;

            if (comp_size == 1u) {
                out[i * 4u + c] = p[0];
            } else {
                out[i * 4u + c] =
                    (uint32_t)p[0] | ((uint32_t)p[1] << 8);
            }
        }
    }
    return LA_SUCCESS;
}

/* Normalize one vertex's 4 weights to sum 1. All-zero sums fall
 * back to rigid {1,0,0,0} (never NaN); callers finite-check
 * first so NaN/Inf inputs never arrive. */
void la_normalize_weights(float w[4]) {
    float sum;

    if (w == NULL) {
        return;
    }
    sum = w[0] + w[1] + w[2] + w[3];
    if (!(sum > 0.0f)) {
        /* Zero (or denormal/negative-cancellation) sum: rigid. */
        w[0] = 1.0f;
        w[1] = 0.0f;
        w[2] = 0.0f;
        w[3] = 0.0f;
        return;
    }
    w[0] /= sum;
    w[1] /= sum;
    w[2] /= sum;
    w[3] /= sum;
}

/* Decode one primitive's skinning streams. Either accessor may be
 * NULL (rigid defaults for that stream); both NULL is the common
 * unskinned case. WEIGHTS reuses the normalized-float path
 * (float32 or normalized u8/u16); anything else is UNSUPPORTED.
 * Non-finite weights are IMPORT errors (they would poison the
 * normalize step). */
la_result la_decode_skin_vertex(const cgltf_accessor *joints,
                                const cgltf_accessor *weights,
                                uint32_t vertex_count, uint32_t *out_joints,
                                float *out_weights) {
    uint32_t i;
    la_result res;

    if (out_joints == NULL || out_weights == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (vertex_count == 0) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (joints != NULL) {
        if (joints->is_sparse) {
            return LA_ERROR_UNSUPPORTED;
        }
        res = la_decode_joints(joints, vertex_count, out_joints);
        if (res != LA_SUCCESS) {
            return res;
        }
    } else {
        for (i = 0; i < vertex_count; i++) {
            out_joints[i * 4u + 0] = 0;
            out_joints[i * 4u + 1] = 0;
            out_joints[i * 4u + 2] = 0;
            out_joints[i * 4u + 3] = 0;
        }
    }
    if (weights != NULL) {
        float *tmp;

        if (weights->type != cgltf_type_vec4 ||
            weights->count != (cgltf_size)vertex_count ||
            weights->is_sparse) {
            if (weights->is_sparse) {
                return LA_ERROR_UNSUPPORTED;
            }
            return LA_ERROR_IMPORT;
        }
        if (weights->component_type != cgltf_component_type_r_32f &&
            !(weights->normalized &&
              (weights->component_type == cgltf_component_type_r_8u ||
               weights->component_type ==
                   cgltf_component_type_r_16u))) {
            return LA_ERROR_UNSUPPORTED;
        }
        tmp = (float *)malloc(sizeof(float) * (size_t)vertex_count *
                              4u);
        if (tmp == NULL) {
            return LA_ERROR_OUT_OF_MEMORY;
        }
        res = la_read_float_attr(weights, 4u, tmp);
        if (res != LA_SUCCESS) {
            free(tmp);
            return res;
        }
        for (i = 0; i < vertex_count; i++) {
            float w[4];

            w[0] = tmp[i * 4u + 0];
            w[1] = tmp[i * 4u + 1];
            w[2] = tmp[i * 4u + 2];
            w[3] = tmp[i * 4u + 3];
            if (!(w[0] == w[0] && w[1] == w[1] && w[2] == w[2] &&
                  w[3] == w[3])) {
                /* NaN poisons normalization: reject, never store. */
                free(tmp);
                return LA_ERROR_IMPORT;
            }
            if (w[0] > 3.4028235e38f || w[0] < -3.4028235e38f ||
                w[1] > 3.4028235e38f || w[1] < -3.4028235e38f ||
                w[2] > 3.4028235e38f || w[2] < -3.4028235e38f ||
                w[3] > 3.4028235e38f || w[3] < -3.4028235e38f) {
                /* Inf (float32 has no larger finite value):
                 * same policy as NaN. */
                free(tmp);
                return LA_ERROR_IMPORT;
            }
            la_normalize_weights(w);
            out_weights[i * 4u + 0] = w[0];
            out_weights[i * 4u + 1] = w[1];
            out_weights[i * 4u + 2] = w[2];
            out_weights[i * 4u + 3] = w[3];
        }
        free(tmp);
    } else {
        for (i = 0; i < vertex_count; i++) {
            out_weights[i * 4u + 0] = 1.0f;
            out_weights[i * 4u + 1] = 0.0f;
            out_weights[i * 4u + 2] = 0.0f;
            out_weights[i * 4u + 3] = 0.0f;
        }
    }
    return LA_SUCCESS;
}

/* ------------------------------------------------------------------
 * Node transforms (PART G): TRS or raw matrix, column-major in and
 * out, quaternions (x, y, z, w) both sides — no transposition, no
 * reordering, ever.
 * ------------------------------------------------------------------ */

static void la_compose_trs(const float t[3], const float q[4],
                           const float s[3], float out_m[16]) {
    lr_transform tr;

    tr.position[0] = t[0];
    tr.position[1] = t[1];
    tr.position[2] = t[2];
    tr.rotation[0] = q[0];
    tr.rotation[1] = q[1];
    tr.rotation[2] = q[2];
    tr.rotation[3] = q[3];
    tr.scale[0] = s[0];
    tr.scale[1] = s[1];
    tr.scale[2] = s[2];
    lr_transform_to_matrix(&tr, out_m);
}

/* Fill both representations for one node. Matrix-form nodes copy raw
 * and decompose for inspection; TRS nodes compose (missing parts are
 * glTF identity defaults). */
static la_result la_node_local(const cgltf_node *node, lr_transform *out_tr,
                               float out_m[16]) {
    static const float k_ident_t[3] = { 0.0f, 0.0f, 0.0f };
    static const float k_ident_q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    static const float k_ident_s[3] = { 1.0f, 1.0f, 1.0f };

    if (node->has_matrix) {
        memcpy(out_m, node->matrix, sizeof(float) * 16u);
        if (la_matrix_to_transform(node->matrix, out_tr) != LA_SUCCESS) {
            return LA_ERROR_IMPORT;
        }
        return LA_SUCCESS;
    }
    out_tr->position[0] = node->has_translation ? node->translation[0] : 0.0f;
    out_tr->position[1] = node->has_translation ? node->translation[1] : 0.0f;
    out_tr->position[2] = node->has_translation ? node->translation[2] : 0.0f;
    if (node->has_rotation) {
        out_tr->rotation[0] = node->rotation[0];
        out_tr->rotation[1] = node->rotation[1];
        out_tr->rotation[2] = node->rotation[2];
        out_tr->rotation[3] = node->rotation[3];
    } else {
        memcpy(out_tr->rotation, k_ident_q, sizeof(k_ident_q));
    }
    out_tr->scale[0] = node->has_scale ? node->scale[0] : 1.0f;
    out_tr->scale[1] = node->has_scale ? node->scale[1] : 1.0f;
    out_tr->scale[2] = node->has_scale ? node->scale[2] : 1.0f;
    la_compose_trs(node->has_translation ? node->translation : k_ident_t,
                   node->has_rotation ? node->rotation : k_ident_q,
                   node->has_scale ? node->scale : k_ident_s, out_m);
    return LA_SUCCESS;
}

/* ------------------------------------------------------------------
 * Texture/image resolution (PART O/Q): embedded GLB buffers, data
 * URIs, and relative external files. Keys are stable identities.
 * ------------------------------------------------------------------ */

static la_result la_image_key(la_import *imp, cgltf_size image_index,
                              const cgltf_image *image, char **out_key) {
    /* key buffer: path + "#image<N>" + role suffix appended later. */
    size_t need;
    char *key;
    int written;

    need = strlen(imp->path) + 32u;
    key = (char *)malloc(need);
    if (key == NULL) {
        return LA_ERROR_OUT_OF_MEMORY;
    }
    written = snprintf(key, need, "%s#image%u", imp->path,
                       (unsigned)image_index);
    if (written < 0 || (size_t)written >= need) {
        free(key);
        return LA_ERROR_OUT_OF_MEMORY;
    }
    (void)image;
    *out_key = key;
    return LA_SUCCESS;
}

static la_result la_resolve_image_bytes(la_import *imp,
                                        const cgltf_image *image,
                                        const unsigned char **out_bytes,
                                        size_t *out_size, int *out_owned,
                                        char **out_key) {
    char *key = NULL;
    la_result res;

    if (imp == NULL || image == NULL || out_bytes == NULL ||
        out_size == NULL || out_owned == NULL || out_key == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    *out_bytes = NULL;
    *out_size = 0;
    *out_owned = 0;
    *out_key = NULL;
    res = la_image_key(imp, (cgltf_size)(image - imp->data->images), image,
                       &key);
    if (res != LA_SUCCESS) {
        return res;
    }
    if (image->buffer_view != NULL) {
        /* Embedded bytes (GLB BIN chunk or .gltf-embedded buffer). */
        const cgltf_buffer_view *view = image->buffer_view;

        if (view->buffer == NULL || view->buffer->data == NULL ||
            view->offset > view->buffer->size ||
            view->size > view->buffer->size - view->offset) {
            free(key);
            la_set_error(imp->manager, "image buffer view out of range");
            return LA_ERROR_IMPORT;
        }
        *out_bytes = (const unsigned char *)view->buffer->data + view->offset;
        *out_size = view->size;
        *out_key = key;
        return LA_SUCCESS;
    }
    if (image->uri == NULL || image->uri[0] == '\0') {
        free(key);
        la_set_error(imp->manager, "image has no data source");
        return LA_ERROR_IMPORT;
    }
    if (strncmp(image->uri, "data:", 5) == 0) {
        const char *body;
        size_t body_len;
        unsigned char *decoded = NULL;
        size_t decoded_len = 0;

        res = la_data_uri_payload(image->uri, &body, &body_len);
        if (res != LA_SUCCESS) {
            free(key);
            la_set_error(imp->manager, "bad image data URI");
            return res;
        }
        res = la_b64_decode(body, body_len, &decoded, &decoded_len);
        if (res != LA_SUCCESS) {
            free(key);
            la_set_error(imp->manager, "corrupt image data URI");
            return res;
        }
        *out_bytes = decoded;
        *out_size = decoded_len;
        *out_owned = 1;
        *out_key = key;
        return LA_SUCCESS;
    }
    {
        /* External file, resolved against the glTF directory. */
        char *joined = NULL;
        unsigned char *bytes = NULL;
        size_t length = 0;

        free(key);
        key = NULL;
        res = la_fs_join(imp->dir, image->uri, &joined);
        if (res != LA_SUCCESS) {
            la_set_error(imp->manager, "cannot resolve image path '%s'",
                         image->uri);
            return res;
        }
        res = la_fs_read(joined, &bytes, &length);
        if (res == LA_ERROR_NOT_FOUND) {
            la_set_error(imp->manager, "missing external image '%s'",
                         joined);
            free(joined);
            return LA_ERROR_NOT_FOUND;
        }
        if (res != LA_SUCCESS) {
            la_set_error(imp->manager, "cannot read image '%s'", joined);
            free(joined);
            return LA_ERROR_IMPORT;
        }
        *out_bytes = bytes;
        *out_size = length;
        *out_owned = 1;
        *out_key = joined;
        return LA_SUCCESS;
    }
}

/* Import one source image as a shared GPU texture with the given
 * color-space role; registers the model ref. `model_slot` receives
 * the model's texture index. */
static la_result la_import_texture(la_import *imp, cgltf_size image_index,
                                   int srgb, uint32_t *out_slot) {
    cgltf_image *image;
    const unsigned char *bytes = NULL;
    size_t size = 0;
    int owned = 0;
    char *key = NULL;
    char *role_key = NULL;
    unsigned char *rgba = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    la_texture_asset *asset = NULL;
    la_result res;

    if (image_index >= imp->data->images_count) {
        return LA_ERROR_IMPORT;
    }
    image = &imp->data->images[image_index];
    res = la_resolve_image_bytes(imp, image, &bytes, &size, &owned, &key);
    if (res != LA_SUCCESS) {
        return res;
    }
    res = la_decode_rgba(bytes, size, &rgba, &width, &height);
    if (owned) {
        la_fs_free((unsigned char *)bytes);
    }
    if (res != LA_SUCCESS) {
        free(key);
        la_set_error(imp->manager, "image decode failed (not PNG/JPEG?)");
        return LA_ERROR_IMPORT;
    }
    /* Role-qualified key: the same bytes as baseColor (sRGB) and as
     * normal data (linear) are different GPU resources. */
    {
        size_t need = strlen(key) + 8u;

        role_key = (char *)malloc(need);
        if (role_key == NULL) {
            la_decode_free(rgba);
            free(key);
            return LA_ERROR_OUT_OF_MEMORY;
        }
        snprintf(role_key, need, "%s%s", key, srgb ? "|srgb" : "|lin");
        free(key);
    }
    res = la_texture_get_or_create(imp->manager, role_key, rgba, width,
                                   height, srgb, &asset);
    la_decode_free(rgba);
    free(role_key);
    if (res != LA_SUCCESS) {
        la_set_error(imp->manager, "texture upload failed");
        return (res == LA_ERROR_OUT_OF_MEMORY) ? res : LA_ERROR_RENDER;
    }
    /* Register the model ref (dedupe: same asset once per model). */
    {
        uint32_t i;

        for (i = 0; i < imp->model->texture_count; i++) {
            if (imp->model->textures[i] == asset) {
                *out_slot = i;
                return LA_SUCCESS;
            }
        }
    }
    {
        la_texture_asset **grown = (la_texture_asset **)realloc(
            imp->model->textures,
            sizeof(la_texture_asset *) * (imp->model->texture_count + 1u));

        if (grown == NULL) {
            return LA_ERROR_OUT_OF_MEMORY;
        }
        imp->model->textures = grown;
        imp->model->textures[imp->model->texture_count] = asset;
        *out_slot = imp->model->texture_count;
        imp->model->texture_count++;
    }
    return LA_SUCCESS;
}

/* Resolve one texture view (image + sampler) into model texture and
 * sampler slots. `want_sampler` always produces a sampler slot (glTF
 * defaults when the view names none). */
static la_result la_import_texture_view(la_import *imp,
                                        const cgltf_texture_view *view,
                                        int srgb, uint32_t *out_tex_slot,
                                        uint32_t *out_samp_slot) {
    cgltf_image *image;
    cgltf_size image_index;
    lc_sampler_desc params;
    la_sampler_asset *samp = NULL;
    la_result res;

    if (view == NULL || view->texture == NULL ||
        view->texture->image == NULL) {
        la_set_error(imp->manager, "texture view has no image");
        return LA_ERROR_IMPORT;
    }
    image = view->texture->image;
    if (image < imp->data->images ||
        (size_t)(image - imp->data->images) >= imp->data->images_count) {
        la_set_error(imp->manager, "texture image out of range");
        return LA_ERROR_IMPORT;
    }
    image_index = (cgltf_size)(image - imp->data->images);
    res = la_import_texture(imp, image_index, srgb, out_tex_slot);
    if (res != LA_SUCCESS) {
        return res;
    }
    /* The sampler lives on the texture object (may be NULL for glTF
     * defaults); dedupe through the manager cache either way. */
    res = la_sampler_from_gltf(view->texture->sampler, &params);
    if (res != LA_SUCCESS) {
        return res;
    }
    res = la_sampler_get_or_create(imp->manager, &params, &samp);
    if (res != LA_SUCCESS) {
        la_set_error(imp->manager, "sampler creation failed");
        return (res == LA_ERROR_OUT_OF_MEMORY) ? res : LA_ERROR_RENDER;
    }
    {
        uint32_t i;

        for (i = 0; i < imp->model->sampler_count; i++) {
            if (imp->model->samplers[i] == samp) {
                *out_samp_slot = i;
                return LA_SUCCESS;
            }
        }
    }
    {
        la_sampler_asset **grown = (la_sampler_asset **)realloc(
            imp->model->samplers,
            sizeof(la_sampler_asset *) * (imp->model->sampler_count + 1u));

        if (grown == NULL) {
            return LA_ERROR_OUT_OF_MEMORY;
        }
        imp->model->samplers = grown;
        imp->model->samplers[imp->model->sampler_count] = samp;
        *out_samp_slot = imp->model->sampler_count;
        imp->model->sampler_count++;
    }
    return LA_SUCCESS;
}

/* ------------------------------------------------------------------
 * Mesh primitives (PART H/I/J/K): attributes, indices, generation.
 * ------------------------------------------------------------------ */

static const cgltf_accessor *la_find_attr(const cgltf_primitive *prim,
                                          cgltf_attribute_type type,
                                          uint32_t index) {
    cgltf_size i;

    for (i = 0; i < prim->attributes_count; i++) {
        if (prim->attributes[i].type == type &&
            prim->attributes[i].index == index) {
            return prim->attributes[i].data;
        }
    }
    return NULL;
}

/* Import one primitive into an lr_mesh (uploads immediately; CPU
 * copies freed on return). `out_rmesh` receives the owned handle. */
static la_result la_import_primitive(la_import *imp,
                                     const cgltf_primitive *prim,
                                     lr_mesh **out_rmesh) {
    const cgltf_accessor *pos;
    const cgltf_accessor *nrm;
    const cgltf_accessor *tan;
    const cgltf_accessor *uv;
    const cgltf_accessor *joints;
    const cgltf_accessor *weights;
    uint32_t vertex_count;
    uint32_t index_count;
    float *positions = NULL;
    float *normals = NULL;
    float *tangents = NULL;
    float *uvs = NULL;
    uint32_t *skin_joints = NULL;
    float *skin_weights = NULL;
    uint32_t *indices = NULL;
    lr_vertex *verts = NULL;
    lr_mesh *rmesh = NULL;
    lr_mesh_desc mdesc;
    la_result res;
    uint32_t i;

    if (prim->type != cgltf_primitive_type_triangles) {
        la_set_error(imp->manager,
                     "unsupported primitive mode (only TRIANGLES)");
        return LA_ERROR_UNSUPPORTED;
    }
    /* Four-influence limit: extra joint/weight sets never silently
     * drop — the whole import fails. (Scans raw attributes so
     * JOINTS_2+ without JOINTS_1 cannot slip through either.) */
    {
        cgltf_size a;

        for (a = 0; a < prim->attributes_count; a++) {
            if ((prim->attributes[a].type ==
                     cgltf_attribute_type_joints ||
                 prim->attributes[a].type ==
                     cgltf_attribute_type_weights) &&
                prim->attributes[a].index != 0) {
                la_set_error(imp->manager,
                             "JOINTS_1/WEIGHTS_1 unsupported "
                             "(four-influence limit)");
                return LA_ERROR_UNSUPPORTED;
            }
        }
    }
    pos = la_find_attr(prim, cgltf_attribute_type_position, 0);
    if (pos == NULL) {
        la_set_error(imp->manager, "primitive missing POSITION");
        return LA_ERROR_IMPORT;
    }
    if (pos->type != cgltf_type_vec3 ||
        pos->component_type != cgltf_component_type_r_32f ||
        pos->count == 0 || pos->count > UINT32_MAX) {
        la_set_error(imp->manager, "bad POSITION accessor");
        return LA_ERROR_IMPORT;
    }
    if (pos->is_sparse) {
        la_set_error(imp->manager, "sparse POSITION unsupported");
        return LA_ERROR_UNSUPPORTED;
    }
    vertex_count = (uint32_t)pos->count;
    if (prim->indices != NULL) {
        if (prim->indices->count == 0 ||
            prim->indices->count > UINT32_MAX ||
            (prim->indices->count % 3u) != 0u) {
            la_set_error(imp->manager, "bad index count");
            return LA_ERROR_IMPORT;
        }
        index_count = (uint32_t)prim->indices->count;
    } else {
        /* Non-indexed triangles: sequential indices. */
        if (((uint64_t)vertex_count % 3u) != 0u) {
            la_set_error(imp->manager,
                         "non-indexed vertex count not a multiple of 3");
            return LA_ERROR_IMPORT;
        }
        index_count = vertex_count;
    }

    positions =
        (float *)malloc(sizeof(float) * (size_t)vertex_count * 3u);
    normals = (float *)malloc(sizeof(float) * (size_t)vertex_count * 3u);
    tangents =
        (float *)malloc(sizeof(float) * (size_t)vertex_count * 4u);
    uvs = (float *)malloc(sizeof(float) * (size_t)vertex_count * 2u);
    skin_joints = (uint32_t *)malloc(sizeof(uint32_t) *
                                     (size_t)vertex_count * 4u);
    skin_weights =
        (float *)malloc(sizeof(float) * (size_t)vertex_count * 4u);
    indices = (uint32_t *)malloc(sizeof(uint32_t) * (size_t)index_count);
    verts = (lr_vertex *)malloc(sizeof(lr_vertex) * (size_t)vertex_count);
    if (positions == NULL || normals == NULL || tangents == NULL ||
        uvs == NULL || skin_joints == NULL || skin_weights == NULL ||
        indices == NULL || verts == NULL) {
        res = LA_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    res = la_read_float_attr(pos, 3u, positions);
    if (res != LA_SUCCESS) {
        la_set_error(imp->manager, "bad POSITION data");
        goto done;
    }
    nrm = la_find_attr(prim, cgltf_attribute_type_normal, 0);
    if (nrm != NULL) {
        if (nrm->type != cgltf_type_vec3 ||
            nrm->component_type != cgltf_component_type_r_32f ||
            nrm->count != pos->count || nrm->is_sparse) {
            la_set_error(imp->manager, "bad NORMAL accessor");
            res = LA_ERROR_IMPORT;
            goto done;
        }
        res = la_read_float_attr(nrm, 3u, normals);
        if (res != LA_SUCCESS) {
            la_set_error(imp->manager, "bad NORMAL data");
            goto done;
        }
    } else {
        /* Placeholder; generated below once indices are known. */
        memset(normals, 0, sizeof(float) * (size_t)vertex_count * 3u);
    }
    uv = la_find_attr(prim, cgltf_attribute_type_texcoord, 0);
    if (uv != NULL) {
        if (uv->type != cgltf_type_vec2 || uv->count != pos->count ||
            uv->is_sparse) {
            la_set_error(imp->manager, "bad TEXCOORD_0 accessor");
            res = LA_ERROR_IMPORT;
            goto done;
        }
        res = la_read_float_attr(uv, 2u, uvs);
        if (res != LA_SUCCESS) {
            la_set_error(imp->manager, "bad TEXCOORD_0 data");
            goto done;
        }
    } else {
        memset(uvs, 0, sizeof(float) * (size_t)vertex_count * 2u);
    }
    if (prim->indices != NULL) {
        res = la_read_indices(prim->indices, vertex_count, indices);
        if (res != LA_SUCCESS) {
            if (res == LA_ERROR_UNSUPPORTED) {
                la_set_error(imp->manager,
                             "unsupported index component type");
            } else {
                la_set_error(imp->manager, "bad index data");
            }
            goto done;
        }
    } else {
        for (i = 0; i < index_count; i++) {
            indices[i] = i;
        }
    }
    if (nrm == NULL) {
        res = la_compute_normals(positions, vertex_count, indices,
                                 index_count, normals);
        if (res != LA_SUCCESS) {
            goto done;
        }
    }
    tan = la_find_attr(prim, cgltf_attribute_type_tangent, 0);
    if (tan != NULL) {
        if (tan->type != cgltf_type_vec4 ||
            tan->component_type != cgltf_component_type_r_32f ||
            tan->count != pos->count || tan->is_sparse) {
            la_set_error(imp->manager, "bad TANGENT accessor");
            res = LA_ERROR_IMPORT;
            goto done;
        }
        res = la_read_float_attr(tan, 4u, tangents);
        if (res != LA_SUCCESS) {
            la_set_error(imp->manager, "bad TANGENT data");
            goto done;
        }
    } else if (uv != NULL) {
        res = la_compute_tangents(positions, normals, uvs, vertex_count,
                                  indices, index_count, tangents);
        if (res != LA_SUCCESS) {
            goto done;
        }
    } else {
        /* No UVs: deterministic +X/unit fallback (normal mapping
         * needs UVs anyway; deferred with MikkTSpace conformance). */
        for (i = 0; i < vertex_count; i++) {
            tangents[i * 4u + 0] = 1.0f;
            tangents[i * 4u + 1] = 0.0f;
            tangents[i * 4u + 2] = 0.0f;
            tangents[i * 4u + 3] = 1.0f;
        }
    }
    joints = la_find_attr(prim, cgltf_attribute_type_joints, 0);
    weights = la_find_attr(prim, cgltf_attribute_type_weights, 0);
    res = la_decode_skin_vertex(joints, weights, vertex_count,
                                skin_joints, skin_weights);
    if (res != LA_SUCCESS) {
        if (res == LA_ERROR_UNSUPPORTED) {
            la_set_error(imp->manager,
                         "unsupported JOINTS_0/WEIGHTS_0 component type "
                         "(joints need u8/u16, weights float or "
                         "normalized u8/u16)");
        } else {
            la_set_error(imp->manager, "bad JOINTS_0/WEIGHTS_0 data");
        }
        goto done;
    }
    for (i = 0; i < vertex_count; i++) {
        verts[i].position[0] = positions[i * 3u + 0];
        verts[i].position[1] = positions[i * 3u + 1];
        verts[i].position[2] = positions[i * 3u + 2];
        verts[i].normal[0] = normals[i * 3u + 0];
        verts[i].normal[1] = normals[i * 3u + 1];
        verts[i].normal[2] = normals[i * 3u + 2];
        verts[i].tangent[0] = tangents[i * 4u + 0];
        verts[i].tangent[1] = tangents[i * 4u + 1];
        verts[i].tangent[2] = tangents[i * 4u + 2];
        verts[i].tangent[3] = tangents[i * 4u + 3];
        verts[i].texcoord[0] = uvs[i * 2u + 0];
        verts[i].texcoord[1] = uvs[i * 2u + 1];
        verts[i].joints[0] = skin_joints[i * 4u + 0];
        verts[i].joints[1] = skin_joints[i * 4u + 1];
        verts[i].joints[2] = skin_joints[i * 4u + 2];
        verts[i].joints[3] = skin_joints[i * 4u + 3];
        verts[i].weights[0] = skin_weights[i * 4u + 0];
        verts[i].weights[1] = skin_weights[i * 4u + 1];
        verts[i].weights[2] = skin_weights[i * 4u + 2];
        verts[i].weights[3] = skin_weights[i * 4u + 3];
    }
    memset(&mdesc, 0, sizeof(mdesc));
    mdesc.vertices = verts;
    mdesc.vertex_count = vertex_count;
    mdesc.indices = indices;
    mdesc.index_count = index_count;
    if (la_map_lr(lr_mesh_create(imp->manager->renderer, &mdesc, &rmesh)) !=
        LA_SUCCESS) {
        la_set_error(imp->manager, "mesh GPU upload failed");
        res = LA_ERROR_RENDER;
        goto done;
    }
    res = LA_SUCCESS;
    *out_rmesh = rmesh;

done:
    free(positions);
    free(normals);
    free(tangents);
    free(uvs);
    free(skin_joints);
    free(skin_weights);
    free(indices);
    free(verts);
    return res;
}

/* ------------------------------------------------------------------
 * Materials (PART M/N): full PBR metadata preserved, unlit subset
 * uploaded. Default white-opaque material appended iff some
 * primitive names none.
 * ------------------------------------------------------------------ */

static void la_material_defaults(la_pbr_material_data *data) {
    memset(data, 0, sizeof(*data));
    data->base_color_factor[0] = 1.0f;
    data->base_color_factor[1] = 1.0f;
    data->base_color_factor[2] = 1.0f;
    data->base_color_factor[3] = 1.0f;
    data->metallic_factor = 1.0f;
    data->roughness_factor = 1.0f;
    data->base_color_texture = -1;
    data->metallic_roughness_texture = -1;
    data->normal_texture = -1;
    data->occlusion_texture = -1;
    data->emissive_texture = -1;
    data->emissive_factor[0] = 0.0f;
    data->emissive_factor[1] = 0.0f;
    data->emissive_factor[2] = 0.0f;
    data->normal_scale = 1.0f;
    data->occlusion_strength = 1.0f;
    data->alpha_mode = LA_ALPHA_OPAQUE;
    data->alpha_cutoff = 0.5f;
    data->double_sided = 0;
}

/* Import one texture view for a data role; `role_tex_slot` and
 * `role_samp_slot` receive model slots (sampler resolved even for
 * texture-less roles? No — only called with a real view). */
static la_result la_import_material_texture(
    la_import *imp, const cgltf_texture_view *view, int srgb,
    uint32_t *out_tex_slot, uint32_t *out_samp_slot) {
    if (view == NULL || view->texture == NULL || view->texture->image == NULL) {
        return LA_ERROR_IMPORT;
    }
    return la_import_texture_view(imp, view, srgb, out_tex_slot,
                                  out_samp_slot);
}

/* Build the renderer material for one model material slot. Every
 * imported role reaches the Phase-15 PBR pipeline; the single
 * shared sampler is the base-color view's (first available view's
 * when no base map exists). Double-sided and alpha mode flow into
 * the material variant + UBO flags. */
static la_result la_material_upload(la_import *imp, uint32_t slot,
                                    const uint32_t *tex_slots,
                                    const uint32_t *samp_slots) {
    la_model_material *mat = &imp->model->materials[slot];
    lr_pbr_material_desc pdesc;
    uint32_t samp_slot = UINT32_MAX;

    memset(&pdesc, 0, sizeof(pdesc));
    memcpy(pdesc.base_color_factor, mat->data.base_color_factor,
           sizeof(pdesc.base_color_factor));
    pdesc.metallic_factor = mat->data.metallic_factor;
    pdesc.roughness_factor = mat->data.roughness_factor;
    if (mat->data.base_color_texture >= 0) {
        pdesc.base_color_texture =
            imp->model->textures[tex_slots[0]]->view;
        samp_slot = samp_slots[0];
    }
    if (mat->data.metallic_roughness_texture >= 0) {
        pdesc.metallic_roughness_texture =
            imp->model->textures[tex_slots[1]]->view;
        if (samp_slot == UINT32_MAX) {
            samp_slot = samp_slots[1];
        }
    }
    if (mat->data.normal_texture >= 0) {
        pdesc.normal_texture = imp->model->textures[tex_slots[2]]->view;
        if (samp_slot == UINT32_MAX) {
            samp_slot = samp_slots[2];
        }
    }
    if (mat->data.occlusion_texture >= 0) {
        pdesc.occlusion_texture =
            imp->model->textures[tex_slots[3]]->view;
        if (samp_slot == UINT32_MAX) {
            samp_slot = samp_slots[3];
        }
    }
    if (mat->data.emissive_texture >= 0) {
        pdesc.emissive_texture =
            imp->model->textures[tex_slots[4]]->view;
        if (samp_slot == UINT32_MAX) {
            samp_slot = samp_slots[4];
        }
    }
    if (samp_slot != UINT32_MAX) {
        pdesc.sampler = imp->model->samplers[samp_slot]->sampler;
    }
    memcpy(pdesc.emissive_factor, mat->data.emissive_factor,
           sizeof(pdesc.emissive_factor));
    pdesc.normal_scale = mat->data.normal_scale;
    pdesc.occlusion_strength = mat->data.occlusion_strength;
    pdesc.double_sided = mat->data.double_sided;
    /* LA_ALPHA_* and LR_ALPHA_* share numeric order (documented). */
    pdesc.alpha_mode = (lr_alpha_mode)mat->data.alpha_mode;
    if (la_map_lr(lr_material_create_pbr(imp->manager->renderer, &pdesc,
                                         &mat->material)) != LA_SUCCESS) {
        la_set_error(imp->manager, "renderer PBR material creation failed");
        return LA_ERROR_RENDER;
    }
    return LA_SUCCESS;
}

static la_result la_import_material(la_import *imp, cgltf_size mat_index,
                                    uint32_t slot) {
    cgltf_material *src = &imp->data->materials[mat_index];
    la_model_material *mat = &imp->model->materials[slot];
    la_result res;
    /* Per-role model slots: base, metallic-roughness, normal,
     * occlusion, emissive (UINT32_MAX when the role is absent). */
    uint32_t tex_slots[5];
    uint32_t samp_slots[5];
    int r;

    for (r = 0; r < 5; r++) {
        tex_slots[r] = UINT32_MAX;
        samp_slots[r] = UINT32_MAX;
    }

    la_material_defaults(&mat->data);
    mat->material = NULL;
    if (src->has_pbr_metallic_roughness) {
        const cgltf_pbr_metallic_roughness *pbr =
            &src->pbr_metallic_roughness;

        memcpy(mat->data.base_color_factor, pbr->base_color_factor,
               sizeof(mat->data.base_color_factor));
        mat->data.metallic_factor = pbr->metallic_factor;
        mat->data.roughness_factor = pbr->roughness_factor;
        if (pbr->base_color_texture.texture != NULL) {
            if (pbr->base_color_texture.texture->image == NULL) {
                la_set_error(imp->manager, "base color texture has no image");
                return LA_ERROR_IMPORT;
            }
            mat->data.base_color_texture =
                (int32_t)(pbr->base_color_texture.texture->image -
                          imp->data->images);
            res = la_import_material_texture(
                imp, &pbr->base_color_texture, 1, &tex_slots[0],
                &samp_slots[0]);
            if (res != LA_SUCCESS) {
                return res;
            }
        }
        if (pbr->metallic_roughness_texture.texture != NULL) {
            if (pbr->metallic_roughness_texture.texture->image == NULL) {
                la_set_error(imp->manager, "metallic-roughness texture has "
                                           "no image");
                return LA_ERROR_IMPORT;
            }
            mat->data.metallic_roughness_texture =
                (int32_t)(pbr->metallic_roughness_texture.texture->image -
                          imp->data->images);
            res = la_import_material_texture(
                imp, &pbr->metallic_roughness_texture, 0, &tex_slots[1],
                &samp_slots[1]);
            if (res != LA_SUCCESS) {
                return res;
            }
        }
    }
    if (src->normal_texture.texture != NULL) {
        if (src->normal_texture.texture->image == NULL) {
            la_set_error(imp->manager, "normal texture has no image");
            return LA_ERROR_IMPORT;
        }
        mat->data.normal_texture =
            (int32_t)(src->normal_texture.texture->image -
                      imp->data->images);
        mat->data.normal_scale = src->normal_texture.scale;
        res = la_import_material_texture(imp, &src->normal_texture, 0,
                                         &tex_slots[2], &samp_slots[2]);
        if (res != LA_SUCCESS) {
            return res;
        }
    }
    if (src->occlusion_texture.texture != NULL) {
        if (src->occlusion_texture.texture->image == NULL) {
            la_set_error(imp->manager, "occlusion texture has no image");
            return LA_ERROR_IMPORT;
        }
        mat->data.occlusion_texture =
            (int32_t)(src->occlusion_texture.texture->image -
                      imp->data->images);
        /* scale doubles as occlusion strength in cgltf. */
        mat->data.occlusion_strength = src->occlusion_texture.scale;
        res = la_import_material_texture(imp, &src->occlusion_texture, 0,
                                         &tex_slots[3], &samp_slots[3]);
        if (res != LA_SUCCESS) {
            return res;
        }
    }
    if (src->emissive_texture.texture != NULL) {
        if (src->emissive_texture.texture->image == NULL) {
            la_set_error(imp->manager, "emissive texture has no image");
            return LA_ERROR_IMPORT;
        }
        mat->data.emissive_texture =
            (int32_t)(src->emissive_texture.texture->image -
                      imp->data->images);
        res = la_import_material_texture(imp, &src->emissive_texture, 1,
                                         &tex_slots[4], &samp_slots[4]);
        if (res != LA_SUCCESS) {
            return res;
        }
    }
    memcpy(mat->data.emissive_factor, src->emissive_factor,
           sizeof(mat->data.emissive_factor));
    switch (src->alpha_mode) {
    case cgltf_alpha_mode_mask:
        mat->data.alpha_mode = LA_ALPHA_MASK;
        break;
    case cgltf_alpha_mode_blend:
        mat->data.alpha_mode = LA_ALPHA_BLEND;
        break;
    case cgltf_alpha_mode_opaque:
    default:
        mat->data.alpha_mode = LA_ALPHA_OPAQUE;
        break;
    }
    mat->data.alpha_cutoff = src->alpha_cutoff;
    mat->data.double_sided = src->double_sided ? 1 : 0;
    return la_material_upload(imp, slot, tex_slots, samp_slots);
}

/* ------------------------------------------------------------------
 * Hierarchy (PART F/G/U): scene roots, validated links, DFS order,
 * world matrices. No flattening: parent/children survive import.
 * ------------------------------------------------------------------ */

static int la_node_index(la_import *imp, const cgltf_node *node) {
    ptrdiff_t d;

    if (node < imp->data->nodes ||
        (size_t)(node - imp->data->nodes) >= imp->data->nodes_count) {
        return -1;
    }
    d = node - imp->data->nodes;
    return (int)d;
}

/* Build the node array in file order with validated hierarchy. Roots
 * come from the default scene when present, else every unparented
 * node — both in file order. Detects bad pointers, double parenting,
 * and cycles. */
static la_result la_build_nodes(la_import *imp) {
    cgltf_data *data = imp->data;
    la_model *model = imp->model;
    uint8_t *referenced = NULL;
    uint8_t *state = NULL;
    cgltf_size *roots = NULL;
    size_t root_count = 0;
    size_t root_cap = 0;
    size_t i;
    la_result res = LA_SUCCESS;

    model->node_count = (uint32_t)data->nodes_count;
    if (data->nodes_count == 0) {
        model->nodes = NULL;
        model->child_links = NULL;
        model->child_link_count = 0;
        return LA_SUCCESS;
    }
    if (data->nodes_count > (cgltf_size)INT32_MAX) {
        la_set_error(imp->manager, "node count out of range");
        return LA_ERROR_IMPORT;
    }
    model->nodes =
        (la_model_node *)calloc(data->nodes_count, sizeof(la_model_node));
    referenced = (uint8_t *)calloc(data->nodes_count, 1);
    state = (uint8_t *)calloc(data->nodes_count, 1);
    if (model->nodes == NULL || referenced == NULL || state == NULL) {
        res = LA_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    /* Validate links + mark referenced children. */
    for (i = 0; i < data->nodes_count; i++) {
        cgltf_size k;

        for (k = 0; k < data->nodes[i].children_count; k++) {
            int ci = la_node_index(imp, data->nodes[i].children[k]);

            if (ci < 0) {
                la_set_error(imp->manager, "node child out of range");
                res = LA_ERROR_IMPORT;
                goto done;
            }
            if (referenced[ci]) {
                la_set_error(imp->manager, "node parented twice");
                res = LA_ERROR_IMPORT;
                goto done;
            }
            referenced[ci] = 1;
        }
    }
    /* Roots: default scene order, else unreferenced nodes in order. */
    if (data->scene != NULL && data->scene->nodes_count > 0) {
        for (i = 0; i < data->scene->nodes_count; i++) {
            int ri = la_node_index(imp, data->scene->nodes[i]);

            if (ri < 0) {
                la_set_error(imp->manager, "scene node out of range");
                res = LA_ERROR_IMPORT;
                goto done;
            }
            if (root_count == root_cap) {
                cgltf_size *grown = (cgltf_size *)realloc(
                    roots, sizeof(cgltf_size) * (root_cap + 8u));

                if (grown == NULL) {
                    res = LA_ERROR_OUT_OF_MEMORY;
                    goto done;
                }
                roots = grown;
                root_cap += 8u;
            }
            roots[root_count++] = (cgltf_size)ri;
        }
    } else {
        for (i = 0; i < data->nodes_count; i++) {
            if (!referenced[i]) {
                if (root_count == root_cap) {
                    cgltf_size *grown = (cgltf_size *)realloc(
                        roots, sizeof(cgltf_size) * (root_cap + 8u));

                    if (grown == NULL) {
                        res = LA_ERROR_OUT_OF_MEMORY;
                        goto done;
                    }
                    roots = grown;
                    root_cap += 8u;
                }
                roots[root_count++] = i;
            }
        }
    }
    /* Fill nodes in file order (names, parents, transforms, links). */
    {
        /* Count links first for one flat allocation. */
        size_t links = 0;

        for (i = 0; i < data->nodes_count; i++) {
            links += data->nodes[i].children_count;
        }
        if (links > 0) {
            model->child_links =
                (uint32_t *)malloc(sizeof(uint32_t) * links);
            if (model->child_links == NULL) {
                res = LA_ERROR_OUT_OF_MEMORY;
                goto done;
            }
        }
        model->child_link_count = (uint32_t)links;
    }
    {
        size_t cursor = 0;

        for (i = 0; i < data->nodes_count; i++) {
            la_model_node *n = &model->nodes[i];
            const cgltf_node *src = &data->nodes[i];
            cgltf_size k;
            size_t name_len;
            char *copy;

            /* Always heap-allocated (even "" when unnamed) so
             * teardown can free unconditionally — freeing a string
             * literal is UB and trips debug CRT assertions. */
            if (src->name != NULL) {
                name_len = strlen(src->name);
            } else {
                name_len = 0;
            }
            copy = (char *)malloc(name_len + 1u);
            if (copy == NULL) {
                res = LA_ERROR_OUT_OF_MEMORY;
                goto done;
            }
            if (name_len > 0) {
                memcpy(copy, src->name, name_len + 1u);
            } else {
                copy[0] = '\0';
            }
            n->name = copy;
            n->parent = -1;
            n->first_child = (uint32_t)cursor;
            n->child_count = (uint32_t)src->children_count;
            for (k = 0; k < src->children_count; k++) {
                model->child_links[cursor++] =
                    (uint32_t)la_node_index(imp, src->children[k]);
            }
            if (src->mesh != NULL) {
                ptrdiff_t md = src->mesh - data->meshes;

                if (src->mesh < data->meshes ||
                    (size_t)md >= data->meshes_count) {
                    la_set_error(imp->manager, "node mesh out of range");
                    res = LA_ERROR_IMPORT;
                    goto done;
                }
                n->mesh_index = (int32_t)md;
            } else {
                n->mesh_index = -1;
            }
            /* Skin link mirrors the mesh link above (pointer to
             * file-order index, -1 when unskinned). Skins
             * themselves decode later (la_build_skins), once
             * every node index is known. */
            if (src->skin != NULL) {
                ptrdiff_t sd = src->skin - data->skins;

                if (src->skin < data->skins ||
                    (size_t)sd >= data->skins_count ||
                    data->skins_count > (cgltf_size)INT32_MAX) {
                    la_set_error(imp->manager, "node skin out of range");
                    res = LA_ERROR_IMPORT;
                    goto done;
                }
                n->skin_index = (int32_t)sd;
            } else {
                n->skin_index = -1;
            }
            res = la_node_local(src, &n->local_transform, n->local_matrix);
            if (res != LA_SUCCESS) {
                la_set_error(imp->manager, "bad node transform");
                goto done;
            }
        }
    }
    /* Parents from links (validates cgltf's own parent pointers too). */
    for (i = 0; i < data->nodes_count; i++) {
        uint32_t k;

        for (k = 0; k < model->nodes[i].child_count; k++) {
            uint32_t child = model->child_links[model->nodes[i].first_child +
                                                k];

            if ((int32_t)child < 0 ||
                child >= (uint32_t)data->nodes_count) {
                la_set_error(imp->manager, "child link out of range");
                res = LA_ERROR_IMPORT;
                goto done;
            }
            if (model->nodes[child].parent >= 0) {
                la_set_error(imp->manager, "node parented twice");
                res = LA_ERROR_IMPORT;
                goto done;
            }
            model->nodes[child].parent = (int32_t)i;
        }
    }
    /* Cycle check from every root (iterative, color-marked). */
    {
        uint32_t *stack = NULL;
        size_t top = 0;
        size_t r;

        if (root_count > 0) {
            stack = (uint32_t *)malloc(sizeof(uint32_t) *
                                       (data->nodes_count + root_count));
            if (stack == NULL) {
                res = LA_ERROR_OUT_OF_MEMORY;
                goto done;
            }
        }
        for (r = 0; r < root_count; r++) {
            uint32_t n0 = (uint32_t)roots[r];

            if (state[n0] == 2u) {
                continue;
            }
            /* Iterative DFS with explicit enter/exit markers. */
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
                    la_set_error(imp->manager, "cyclic node hierarchy");
                    res = LA_ERROR_IMPORT;
                    free(stack);
                    goto done;
                }
                state[ni] = 1u;
                stack[top++] = ni | 0x80000000u;
                {
                    uint32_t k;

                    for (k = model->nodes[ni].child_count; k-- > 0;) {
                        stack[top++] = model->child_links[model->nodes[ni]
                                                              .first_child +
                                                          k];
                    }
                }
            }
        }
        free(stack);
    }
    /* Every node must be reachable (orphans outside all roots mean a
     * broken forest, e.g. a pure cycle with no entry). */
    for (i = 0; i < data->nodes_count; i++) {
        if (state[i] == 0u) {
            la_set_error(imp->manager, "unreachable node in hierarchy");
            res = LA_ERROR_IMPORT;
            goto done;
        }
    }
    res = LA_SUCCESS;

done:
    free(referenced);
    free(state);
    free(roots);
    if (res != LA_SUCCESS) {
        /* Leave teardown to la_model_teardown (partial arrays freed
         * there); just drop what it cannot see yet. */
        ;
    }
    return res;
}

/* ------------------------------------------------------------------
 * Skins (Phase 29): joint-node lists + inverse-bind matrices.
 *
 * Each file skin becomes one model skin (file order preserved).
 * Joint pointers become node indices (in range, unique within
 * the skin — the engine keys skeleton joints by node, so a
 * duplicate would collapse two joints into one and is rejected
 * here, loudly). Inverse-bind matrices decode ONCE into
 * model-owned column-major floats (identity per joint when the
 * file omits the accessor); every float is finite-checked.
 * Zero-joint skins are malformed. The skin's `skeleton` root
 * hint is intentionally ignored: parents always derive from
 * the node hierarchy (documented in GLTF_ANIMATION_IMPORT.md).
 * ------------------------------------------------------------------ */

/* Decode one MAT4 float32 accessor into `out` (column-major
 * passthrough; `count` matrices). */
static la_result la_read_mat4_attr(const cgltf_accessor *accessor,
                                   uint32_t count, float *out) {
    const unsigned char *base;
    size_t buf_size;
    size_t elem_size;
    size_t stride;
    size_t view_off;
    size_t i;
    la_result res;

    if (accessor == NULL || out == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (accessor->type != cgltf_type_mat4 ||
        accessor->component_type != cgltf_component_type_r_32f ||
        accessor->count != (cgltf_size)count) {
        return LA_ERROR_IMPORT;
    }
    if (accessor->is_sparse) {
        return LA_ERROR_UNSUPPORTED;
    }
    res = la_accessor_bytes(accessor, &base, &buf_size);
    if (res != LA_SUCCESS) {
        return res;
    }
    elem_size = sizeof(float) * 16u;
    /* cgltf sets stride 0 for tightly packed accessors. */
    if (accessor->stride == 0) {
        stride = elem_size;
    } else {
        stride = accessor->stride;
    }
    if (stride < elem_size) {
        return LA_ERROR_IMPORT;
    }
    view_off = accessor->buffer_view->offset + accessor->offset;
    for (i = 0; i < (size_t)count; i++) {
        size_t at = view_off + i * stride;
        uint32_t c;

        if (at > buf_size || elem_size > buf_size - at) {
            return LA_ERROR_IMPORT;
        }
        for (c = 0; c < 16u; c++) {
            float v;

            memcpy(&v, base + at + (size_t)c * 4u, 4u);
            if (!(v == v) || v > 3.4028235e38f ||
                v < -3.4028235e38f) {
                /* NaN/Inf inverse bind would poison every
                 * skinned vertex: reject, never store. */
                return LA_ERROR_IMPORT;
            }
            out[i * 16u + c] = v;
        }
    }
    return LA_SUCCESS;
}

static la_result la_build_skins(la_import *imp) {
    cgltf_data *data = imp->data;
    la_model *model = imp->model;
    size_t s;

    if (data->skins_count == 0) {
        model->skins = NULL;
        model->skin_count = 0;
        return LA_SUCCESS;
    }
    if (data->skins_count > UINT32_MAX) {
        la_set_error(imp->manager, "skin count out of range");
        return LA_ERROR_IMPORT;
    }
    /* skin_count is published before filling so teardown frees
     * partial rows (calloc zeroes the tail; free(NULL) is safe). */
    model->skins =
        (la_model_skin *)calloc(data->skins_count, sizeof(la_model_skin));
    if (model->skins == NULL) {
        return LA_ERROR_OUT_OF_MEMORY;
    }
    model->skin_count = (uint32_t)data->skins_count;
    for (s = 0; s < data->skins_count; s++) {
        const cgltf_skin *src = &data->skins[s];
        la_model_skin *dst = &model->skins[s];
        cgltf_size j;

        if (src->joints_count == 0 ||
            src->joints_count > UINT32_MAX) {
            la_set_error(imp->manager, "skin has no joints");
            return LA_ERROR_IMPORT;
        }
        dst->joint_nodes = (int32_t *)malloc(sizeof(int32_t) *
                                             src->joints_count);
        dst->inv_bind = (float *)malloc(sizeof(float) * 16u *
                                        src->joints_count);
        if (dst->joint_nodes == NULL || dst->inv_bind == NULL) {
            la_set_error(imp->manager, "out of memory decoding skin");
            return LA_ERROR_OUT_OF_MEMORY;
        }
        dst->joint_count = (uint32_t)src->joints_count;
        for (j = 0; j < src->joints_count; j++) {
            int ni = la_node_index(imp, src->joints[j]);
            cgltf_size k;

            if (ni < 0 ||
                (uint64_t)ni >= (uint64_t)model->node_count) {
                la_set_error(imp->manager, "skin joint out of range");
                return LA_ERROR_IMPORT;
            }
            for (k = 0; k < j; k++) {
                if (dst->joint_nodes[k] == ni) {
                    la_set_error(imp->manager,
                                 "duplicate joint node in skin");
                    return LA_ERROR_IMPORT;
                }
            }
            dst->joint_nodes[j] = ni;
        }
        if (src->inverse_bind_matrices != NULL) {
            la_result res = la_read_mat4_attr(
                src->inverse_bind_matrices, dst->joint_count,
                dst->inv_bind);

            if (res != LA_SUCCESS) {
                if (res == LA_ERROR_UNSUPPORTED) {
                    la_set_error(imp->manager,
                                 "sparse inverseBindMatrices "
                                 "unsupported");
                } else {
                    la_set_error(imp->manager,
                                 "bad inverseBindMatrices accessor");
                }
                return res;
            }
        } else {
            /* Spec default: identity per joint. */
            for (j = 0; j < src->joints_count; j++) {
                uint32_t c;

                for (c = 0; c < 16u; c++) {
                    dst->inv_bind[j * 16u + c] =
                        (c % 5u == 0) ? 1.0f : 0.0f;
                }
            }
        }
    }
    return LA_SUCCESS;
}

/* ------------------------------------------------------------------
 * Animations (Phase 29): decoded, engine-ready key tracks.
 *
 * Each file animation becomes at most one model animation:
 * morph-target (weights) channels are SKIPPED (morph targets
 * are a deferred feature), and an animation left with zero
 * importable channels is excluded from the model entirely
 * (count, indices, and durations only ever describe kept
 * data). Anything else malformed — bad accessors, negative or
 * non-finite times, out-of-range target nodes, count
 * mismatches — fails the whole import (transactional).
 *
 * Times decode from scalar-float inputs (finite, >= 0,
 * non-decreasing; strict increase is NOT required — the engine
 * samples duplicates last-wins). Outputs are float vec3 (T/S)
 * or vec4 (R), finite-checked (quats are normalized by the
 * engine, not here). CUBICSPLINE outputs hold key_count * 3
 * vectors (in/value/out Hermite triples); `key_count` always
 * equals the input count. Duration is the max last-key time
 * across the animation's KEPT channels.
 * ------------------------------------------------------------------ */

/* Decode one sampler input (times): scalar float, finite, >= 0. */
static la_result la_read_anim_times(const cgltf_accessor *accessor,
                                    float *out) {
    const unsigned char *base;
    size_t buf_size;
    size_t elem_size;
    size_t stride;
    size_t view_off;
    size_t i;
    la_result res;

    if (accessor == NULL || out == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (accessor->type != cgltf_type_scalar ||
        accessor->component_type != cgltf_component_type_r_32f ||
        accessor->count == 0 || accessor->count > UINT32_MAX ||
        accessor->normalized || accessor->is_sparse) {
        return LA_ERROR_IMPORT;
    }
    res = la_accessor_bytes(accessor, &base, &buf_size);
    if (res != LA_SUCCESS) {
        return res;
    }
    elem_size = sizeof(float);
    /* cgltf sets stride 0 for tightly packed accessors. */
    if (accessor->stride == 0) {
        stride = elem_size;
    } else {
        stride = accessor->stride;
    }
    if (stride < elem_size) {
        return LA_ERROR_IMPORT;
    }
    view_off = accessor->buffer_view->offset + accessor->offset;
    for (i = 0; i < accessor->count; i++) {
        size_t at = view_off + i * stride;
        float v;

        if (at > buf_size || elem_size > buf_size - at) {
            return LA_ERROR_IMPORT;
        }
        memcpy(&v, base + at, 4u);
        if (!(v == v) || v > 3.4028235e38f) {
            /* NaN/Inf key times are un-sampleable. */
            return LA_ERROR_IMPORT;
        }
        if (v < 0.0f) {
            return LA_ERROR_IMPORT;
        }
        if (i > 0 && v < out[i - 1u]) {
            /* Decreasing times break segment search. */
            return LA_ERROR_IMPORT;
        }
        out[i] = v;
    }
    return LA_SUCCESS;
}

/* Decode one sampler output (values): float vec3/vec4,
 * finite-checked. `count` is the ELEMENT count (vectors, not
 * floats — keys, or keys * 3 under CUBICSPLINE); each element
 * holds `comps` floats. */
static la_result la_read_anim_values(const cgltf_accessor *accessor,
                                     uint32_t comps, uint32_t count,
                                     float *out) {
    const unsigned char *base;
    size_t buf_size;
    size_t elem_size;
    size_t stride;
    size_t view_off;
    size_t i;
    la_result res;

    if (accessor == NULL || out == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if ((comps != 3u && comps != 4u) ||
        la_type_components(accessor->type) != comps ||
        accessor->component_type != cgltf_component_type_r_32f ||
        accessor->count != (cgltf_size)count || accessor->normalized ||
        accessor->is_sparse) {
        return LA_ERROR_IMPORT;
    }
    res = la_accessor_bytes(accessor, &base, &buf_size);
    if (res != LA_SUCCESS) {
        return res;
    }
    elem_size = sizeof(float) * comps;
    /* cgltf sets stride 0 for tightly packed accessors. */
    if (accessor->stride == 0) {
        stride = elem_size;
    } else {
        stride = accessor->stride;
    }
    if (stride < elem_size) {
        return LA_ERROR_IMPORT;
    }
    view_off = accessor->buffer_view->offset + accessor->offset;
    for (i = 0; i < (size_t)count; i++) {
        size_t at = view_off + i * stride;
        uint32_t c;

        if (at > buf_size || elem_size > buf_size - at) {
            return LA_ERROR_IMPORT;
        }
        for (c = 0; c < comps; c++) {
            float v;

            memcpy(&v, base + at + (size_t)c * 4u, 4u);
            if (!(v == v) || v > 3.4028235e38f ||
                v < -3.4028235e38f) {
                return LA_ERROR_IMPORT;
            }
            out[i * comps + c] = v;
        }
    }
    return LA_SUCCESS;
}

/* Decode one channel into `dst` (owns fresh times/values on
 * success; frees them before reporting failure). Returns 1 when
 * the channel is morph-targeted (skip it — NOT an error), 0 on
 * a decoded channel, and the la_result code (as int) on
 * malformed data. */
static void la_free_channel(la_model_anim_channel *ch) {
    if (ch == NULL) {
        return;
    }
    free(ch->times);
    free(ch->values);
    ch->times = NULL;
    ch->values = NULL;
}

static int la_decode_channel(la_import *imp,
                             const cgltf_animation_channel *ch,
                             la_model_anim_channel *dst) {
    const cgltf_animation_sampler *sampler;
    uint32_t comps;
    uint32_t keys;
    uint64_t out_count;
    la_result res;

    memset(dst, 0, sizeof(*dst));
    if (ch == NULL || ch->sampler == NULL) {
        la_set_error(imp->manager, "animation channel has no sampler");
        return (int)LA_ERROR_IMPORT;
    }
    sampler = ch->sampler;
    if (ch->target_path == cgltf_animation_path_type_weights) {
        /* Morph targets deferred: skip the channel (loud at the
         * engine layer, which counts surviving tracks). */
        return 1;
    }
    if (ch->target_node == NULL) {
        la_set_error(imp->manager, "animation channel has no target");
        return (int)LA_ERROR_IMPORT;
    }
    {
        int ni = la_node_index(imp, ch->target_node);

        if (ni < 0 || (uint64_t)ni >= (uint64_t)imp->model->node_count) {
            la_set_error(imp->manager, "animation target out of range");
            return (int)LA_ERROR_IMPORT;
        }
        dst->target_node = ni;
    }
    switch (ch->target_path) {
    case cgltf_animation_path_type_translation:
        dst->path = 0;
        comps = 3u;
        break;
    case cgltf_animation_path_type_rotation:
        dst->path = 1;
        comps = 4u;
        break;
    case cgltf_animation_path_type_scale:
        dst->path = 2;
        comps = 3u;
        break;
    default:
        la_set_error(imp->manager, "unknown animation path");
        return (int)LA_ERROR_IMPORT;
    }
    switch (sampler->interpolation) {
    case cgltf_interpolation_type_step:
        dst->interpolation = 0;
        break;
    case cgltf_interpolation_type_linear:
        dst->interpolation = 1;
        break;
    case cgltf_interpolation_type_cubic_spline:
        dst->interpolation = 2;
        break;
    default:
        la_set_error(imp->manager, "unknown animation interpolation");
        return (int)LA_ERROR_IMPORT;
    }
    if (sampler->input == NULL || sampler->output == NULL) {
        la_set_error(imp->manager, "animation sampler has no keys");
        return (int)LA_ERROR_IMPORT;
    }
    if (sampler->input->count == 0 ||
        sampler->input->count > UINT32_MAX) {
        la_set_error(imp->manager, "animation sampler has no keys");
        return (int)LA_ERROR_IMPORT;
    }
    keys = (uint32_t)sampler->input->count;
    /* Output ELEMENT count (cgltf counts elements, not floats:
     * keys vectors, or keys * 3 under CUBICSPLINE). The values
     * buffer holds out_count * comps floats. */
    out_count = (uint64_t)keys;
    if (dst->interpolation == 2u) {
        out_count *= 3u; /* in/value/out triples */
    }
    if (out_count > (uint64_t)UINT32_MAX) {
        la_set_error(imp->manager, "animation output count out of range");
        return (int)LA_ERROR_IMPORT;
    }
    if ((uint64_t)sampler->output->count != out_count) {
        la_set_error(imp->manager, "animation output count mismatch");
        return (int)LA_ERROR_IMPORT;
    }
    if (out_count > (uint64_t)UINT32_MAX / comps) {
        la_set_error(imp->manager, "animation output count out of range");
        return (int)LA_ERROR_OUT_OF_MEMORY;
    }
    dst->times = (float *)malloc(sizeof(float) * (size_t)keys);
    dst->values = (float *)malloc(sizeof(float) * (size_t)out_count *
                                  comps);
    if (dst->times == NULL || dst->values == NULL) {
        la_set_error(imp->manager, "out of memory decoding animation");
        return (int)LA_ERROR_OUT_OF_MEMORY;
    }
    dst->key_count = keys;
    res = la_read_anim_times(sampler->input, dst->times);
    if (res != LA_SUCCESS) {
        la_set_error(imp->manager, "bad animation key times");
        la_free_channel(dst);
        return (int)res;
    }
    res = la_read_anim_values(sampler->output, comps,
                              (uint32_t)out_count, dst->values);
    if (res != LA_SUCCESS) {
        la_set_error(imp->manager, "bad animation key values");
        la_free_channel(dst);
        return (int)res;
    }
    return 0;
}

static la_result la_build_anims(la_import *imp) {
    cgltf_data *data = imp->data;
    la_model_animation *kept = NULL;
    uint32_t kept_count = 0;
    size_t a;

    if (data->animations_count == 0) {
        imp->model->anims = NULL;
        imp->model->anim_count = 0;
        return LA_SUCCESS;
    }
    if (data->animations_count > UINT32_MAX) {
        la_set_error(imp->manager, "animation count out of range");
        return LA_ERROR_IMPORT;
    }
    /* Scratch holds every file animation's kept channels; only
     * animations with >= 1 kept channel publish to the model.
     * Nothing publishes until the whole file decodes (any error
     * frees the scratch outright — transactional). */
    kept = (la_model_animation *)calloc(data->animations_count,
                                        sizeof(la_model_animation));
    if (kept == NULL) {
        return LA_ERROR_OUT_OF_MEMORY;
    }
    for (a = 0; a < data->animations_count; a++) {
        const cgltf_animation *src = &data->animations[a];
        la_model_anim_channel *channels = NULL;
        uint32_t n_kept = 0;
        float duration = 0.0f;
        cgltf_size c;
        la_result res = LA_SUCCESS;

        if (src->channels_count > UINT32_MAX) {
            la_set_error(imp->manager,
                         "animation channel count out of range");
            res = LA_ERROR_IMPORT;
        } else if (src->channels_count > 0) {
            channels = (la_model_anim_channel *)calloc(
                src->channels_count, sizeof(la_model_anim_channel));
            if (channels == NULL) {
                res = LA_ERROR_OUT_OF_MEMORY;
            }
        }
        for (c = 0; res == LA_SUCCESS && c < src->channels_count;
             c++) {
            la_model_anim_channel ch;
            int dc = la_decode_channel(imp, &src->channels[c], &ch);

            if (dc == 1) {
                continue; /* morph-target channel: skipped */
            }
            if (dc != 0) {
                res = (la_result)dc;
                break;
            }
            channels[n_kept] = ch;
            if (ch.key_count > 0 &&
                ch.times[ch.key_count - 1u] > duration) {
                duration = ch.times[ch.key_count - 1u];
            }
            n_kept++;
        }
        if (res != LA_SUCCESS) {
            uint32_t k;
            size_t p;

            for (k = 0; k < n_kept; k++) {
                la_free_channel(&channels[k]);
            }
            free(channels);
            for (p = 0; p < (size_t)kept_count; p++) {
                uint32_t k;

                for (k = 0; k < kept[p].channel_count; k++) {
                    la_free_channel(&kept[p].channels[k]);
                }
                free(kept[p].channels);
            }
            free(kept);
            return res;
        }
        if (n_kept == 0) {
            /* Morph-only (or empty) animation: excluded. */
            free(channels);
            continue;
        }
        if (n_kept < src->channels_count) {
            /* Shrink to the kept prefix (keeps borrowing math
             * exact; a shrink failure keeps the valid oversize
             * array — queries still bound by channel_count). */
            la_model_anim_channel *smaller =
                (la_model_anim_channel *)realloc(
                    channels,
                    sizeof(la_model_anim_channel) * n_kept);

            if (smaller != NULL) {
                channels = smaller;
            }
        }
        kept[kept_count].channels = channels;
        kept[kept_count].channel_count = n_kept;
        kept[kept_count].duration = duration;
        kept_count++;
    }
    if (kept_count == 0) {
        free(kept);
        imp->model->anims = NULL;
        imp->model->anim_count = 0;
        return LA_SUCCESS;
    }
    if (kept_count < (uint32_t)data->animations_count) {
        la_model_animation *smaller = (la_model_animation *)realloc(
            kept, sizeof(la_model_animation) * kept_count);

        if (smaller != NULL) {
            kept = smaller;
        }
    }
    imp->model->anims = kept;
    imp->model->anim_count = kept_count;
    return LA_SUCCESS;
}

/* ------------------------------------------------------------------
 * Mesh/material assembly + bounds (PART H/R/S/V).
 * ------------------------------------------------------------------ */

static la_result la_build_meshes(la_import *imp) {
    cgltf_data *data = imp->data;
    la_model *model = imp->model;
    int needs_default = 0;
    size_t mi;
    la_result res;

    if (data->meshes_count == 0) {
        model->meshes = NULL;
        model->mesh_count = 0;
        return LA_SUCCESS;
    }
    if (data->meshes_count > UINT32_MAX) {
        return LA_ERROR_IMPORT;
    }
    model->meshes =
        (la_model_mesh *)calloc(data->meshes_count, sizeof(la_model_mesh));
    if (model->meshes == NULL) {
        return LA_ERROR_OUT_OF_MEMORY;
    }
    model->mesh_count = (uint32_t)data->meshes_count;
    for (mi = 0; mi < data->meshes_count; mi++) {
        cgltf_mesh *src = &data->meshes[mi];
        la_model_mesh *dst = &model->meshes[mi];
        size_t pi;

        if (src->primitives_count == 0 ||
            src->primitives_count > UINT32_MAX) {
            la_set_error(imp->manager, "mesh has no primitives");
            return LA_ERROR_IMPORT;
        }
        dst->primitives = (la_model_primitive *)calloc(
            src->primitives_count, sizeof(la_model_primitive));
        if (dst->primitives == NULL) {
            return LA_ERROR_OUT_OF_MEMORY;
        }
        dst->primitive_count = (uint32_t)src->primitives_count;
        model->source_primitive_count += dst->primitive_count;
        for (pi = 0; pi < src->primitives_count; pi++) {
            cgltf_primitive *prim = &src->primitives[pi];
            lr_mesh *rmesh = NULL;

            res = la_import_primitive(imp, prim, &rmesh);
            if (res != LA_SUCCESS) {
                return res;
            }
            dst->primitives[pi].mesh = rmesh;
            if (prim->material != NULL) {
                ptrdiff_t mdi = prim->material - data->materials;

                if (prim->material < data->materials ||
                    (size_t)mdi >= data->materials_count) {
                    la_set_error(imp->manager,
                                 "primitive material out of range");
                    return LA_ERROR_IMPORT;
                }
                dst->primitives[pi].material_index = (uint32_t)mdi;
            } else {
                needs_default = 1;
                dst->primitives[pi].material_index = UINT32_MAX;
            }
        }
    }
    /* Materials: one slot per source material + optional default. */
    {
        size_t mcount = data->materials_count + (needs_default ? 1u : 0u);
        size_t i;

        if (mcount > 0) {
            model->materials = (la_model_material *)calloc(
                mcount, sizeof(la_model_material));
            if (model->materials == NULL) {
                return LA_ERROR_OUT_OF_MEMORY;
            }
            model->material_count = (uint32_t)mcount;
            for (i = 0; i < data->materials_count; i++) {
                res = la_import_material(imp, i, (uint32_t)i);
                if (res != LA_SUCCESS) {
                    return res;
                }
            }
            if (needs_default) {
                la_model_material *dflt =
                    &model->materials[data->materials_count];
                lr_pbr_material_desc pdesc;

                la_material_defaults(&dflt->data);
                memset(&pdesc, 0, sizeof(pdesc));
                memcpy(pdesc.base_color_factor,
                       dflt->data.base_color_factor,
                       sizeof(pdesc.base_color_factor));
                pdesc.metallic_factor = dflt->data.metallic_factor;
                pdesc.roughness_factor = dflt->data.roughness_factor;
                pdesc.normal_scale = dflt->data.normal_scale;
                pdesc.occlusion_strength = dflt->data.occlusion_strength;
                pdesc.alpha_mode = LR_ALPHA_OPAQUE;
                if (la_map_lr(lr_material_create_pbr(
                        imp->manager->renderer, &pdesc,
                        &dflt->material)) != LA_SUCCESS) {
                    la_set_error(imp->manager,
                                 "default material creation failed");
                    return LA_ERROR_RENDER;
                }
                /* Rewrite placeholder indices to the default slot. */
                for (mi = 0; mi < model->mesh_count; mi++) {
                    uint32_t pi;

                    for (pi = 0;
                         pi < model->meshes[mi].primitive_count; pi++) {
                        if (model->meshes[mi].primitives[pi].material_index ==
                            UINT32_MAX) {
                            model->meshes[mi].primitives[pi].material_index =
                                (uint32_t)data->materials_count;
                        }
                    }
                }
            }
        }
    }
    return LA_SUCCESS;
}

/* Model-space bounds over every mesh instance (root = identity):
 * transform each primitive AABB's corners by its world matrix. */
static la_result la_build_bounds(la_import *imp) {
    la_model *model = imp->model;
    float *world;
    uint32_t n;
    int have = 0;
    float bmin[3];
    float bmax[3];

    if (model->node_count == 0) {
        memset(&model->bounds, 0, sizeof(model->bounds));
        return LA_SUCCESS;
    }
    world = (float *)malloc(sizeof(float) * 16u * model->node_count);
    if (world == NULL) {
        return LA_ERROR_OUT_OF_MEMORY;
    }
    /* Roots first (same order discipline as submit). */
    for (n = 0; n < model->node_count; n++) {
        if (model->nodes[n].parent < 0) {
            memcpy(world + (size_t)n * 16u,
                   model->nodes[n].local_matrix, sizeof(float) * 16u);
        }
    }
    /* Children after parents: explicit stack from roots below. */
    {
        uint32_t *stack =
            (uint32_t *)malloc(sizeof(uint32_t) * model->node_count);
        size_t top = 0;

        if (stack == NULL) {
            free(world);
            return LA_ERROR_OUT_OF_MEMORY;
        }
        for (n = model->node_count; n-- > 0;) {
            if (model->nodes[n].parent < 0) {
                stack[top++] = n;
            }
        }
        bmin[0] = bmin[1] = bmin[2] = 0.0f;
        bmax[0] = bmax[1] = bmax[2] = 0.0f;
        while (top > 0) {
            uint32_t ni;
            float *pw;
            uint32_t k;

            top--;
            ni = stack[top];
            pw = world + (size_t)ni * 16u;
            if (model->nodes[ni].mesh_index >= 0) {
                uint32_t mi = (uint32_t)model->nodes[ni].mesh_index;

                if (mi < model->mesh_count) {
                    uint32_t p;

                    for (p = 0; p < model->meshes[mi].primitive_count; p++) {
                        lr_bounds lb;
                        float corner[3];
                        uint32_t cx;
                        uint32_t cy;
                        uint32_t cz;

                        lr_mesh_get_bounds(
                            model->meshes[mi].primitives[p].mesh, &lb);
                        for (cx = 0; cx < 2u; cx++) {
                            for (cy = 0; cy < 2u; cy++) {
                                for (cz = 0; cz < 2u; cz++) {
                                    float local[3] = {
                                        cx ? lb.max[0] : lb.min[0],
                                        cy ? lb.max[1] : lb.min[1],
                                        cz ? lb.max[2] : lb.min[2],
                                    };
                                    float wpt[3] = {
                                        pw[0] * local[0] +
                                            pw[4] * local[1] +
                                            pw[8] * local[2] + pw[12],
                                        pw[1] * local[0] +
                                            pw[5] * local[1] +
                                            pw[9] * local[2] + pw[13],
                                        pw[2] * local[0] +
                                            pw[6] * local[1] +
                                            pw[10] * local[2] + pw[14],
                                    };
                                    int a;

                                    if (!have) {
                                        memcpy(bmin, wpt, sizeof(bmin));
                                        memcpy(bmax, wpt, sizeof(bmax));
                                        have = 1;
                                    } else {
                                        for (a = 0; a < 3; a++) {
                                            if (wpt[a] < bmin[a]) {
                                                bmin[a] = wpt[a];
                                            }
                                            if (wpt[a] > bmax[a]) {
                                                bmax[a] = wpt[a];
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        (void)corner;
                    }
                    model->instance_count +=
                        model->meshes[mi].primitive_count;
                }
            }
            for (k = model->nodes[ni].child_count; k-- > 0;) {
                uint32_t child = model->child_links[model->nodes[ni]
                                                        .first_child +
                                                    k];
                float *cw = world + (size_t)child * 16u;
                const float *lw = model->nodes[child].local_matrix;
                uint32_t r;
                uint32_t c;

                for (r = 0; r < 4; r++) {
                    for (c = 0; c < 4; c++) {
                        float sum = 0.0f;
                        uint32_t kk;

                        for (kk = 0; kk < 4; kk++) {
                            sum += pw[kk * 4u + r] * lw[c * 4u + kk];
                        }
                        cw[c * 4u + r] = sum;
                    }
                }
                stack[top++] = child;
            }
        }
        free(stack);
    }
    free(world);
    if (!have) {
        memset(&model->bounds, 0, sizeof(model->bounds));
        return LA_SUCCESS;
    }
    model->bounds.min[0] = bmin[0];
    model->bounds.min[1] = bmin[1];
    model->bounds.min[2] = bmin[2];
    model->bounds.max[0] = bmax[0];
    model->bounds.max[1] = bmax[1];
    model->bounds.max[2] = bmax[2];
    model->bounds.center[0] = (bmin[0] + bmax[0]) * 0.5f;
    model->bounds.center[1] = (bmin[1] + bmax[1]) * 0.5f;
    model->bounds.center[2] = (bmin[2] + bmax[2]) * 0.5f;
    {
        float dx = bmax[0] - bmin[0];
        float dy = bmax[1] - bmin[1];
        float dz = bmax[2] - bmin[2];

        model->bounds.radius =
            sqrtf(dx * dx + dy * dy + dz * dz) * 0.5f;
    }
    return LA_SUCCESS;
}

/* ------------------------------------------------------------------
 * Load entry (PART D).
 * ------------------------------------------------------------------ */

la_result la_model_load(la_asset_manager *manager, const char *path,
                        la_model **out_model) {
    unsigned char *bytes = NULL;
    size_t size = 0;
    la_result res;

    if (manager == NULL || path == NULL || out_model == NULL) {
        if (out_model != NULL) {
            *out_model = NULL;
        }
        return LA_ERROR_INVALID_ARGUMENT;
    }
    *out_model = NULL;
    if (manager->renderer == NULL) {
        la_set_error(manager, "manager has no renderer");
        return LA_ERROR_INVALID_ARGUMENT;
    }
    res = la_fs_read(path, &bytes, &size);
    if (res == LA_ERROR_NOT_FOUND) {
        la_set_error(manager, "model file not found '%s'", path);
        return LA_ERROR_NOT_FOUND;
    }
    if (res != LA_SUCCESS) {
        la_set_error(manager, "cannot read model file '%s'", path);
        return LA_ERROR_IMPORT;
    }
    res = la_import_gltf(manager, path, bytes, size, out_model);
    la_fs_free(bytes);
    return res;
}

la_result la_import_gltf(la_asset_manager *manager, const char *path,
                         const unsigned char *bytes, size_t size,
                         la_model **out_model) {
    la_import imp;
    cgltf_options options;
    cgltf_result pres;
    la_result res;
    la_model *model = NULL;

    if (manager == NULL || path == NULL || bytes == NULL || size == 0 ||
        out_model == NULL) {
        if (out_model != NULL) {
            *out_model = NULL;
        }
        return LA_ERROR_INVALID_ARGUMENT;
    }
    *out_model = NULL;
    memset(&imp, 0, sizeof(imp));
    imp.manager = manager;
    imp.path = path;
    imp.failed_path[0] = '\0';
    res = la_fs_dirname(path, &imp.dir);
    if (res != LA_SUCCESS) {
        return res;
    }
    memset(&options, 0, sizeof(options));
    options.file.read = la_cgltf_read;
    options.file.release = la_cgltf_release;
    options.file.user_data = &imp;
    pres = cgltf_parse(&options, bytes, size, &imp.data);
    if (pres == cgltf_result_out_of_memory) {
        la_set_error(manager, "out of memory parsing '%s'", path);
        res = LA_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    if (pres == cgltf_result_invalid_json) {
        la_set_error(manager, "invalid JSON in '%s'", path);
        res = LA_ERROR_IMPORT;
        goto done;
    }
    if (pres == cgltf_result_invalid_gltf) {
        la_set_error(manager, "invalid glTF container in '%s'", path);
        res = LA_ERROR_IMPORT;
        goto done;
    }
    if (pres != cgltf_result_success) {
        la_set_error(manager, "cannot parse '%s' (cgltf %d)", path,
                     (int)pres);
        res = LA_ERROR_IMPORT;
        goto done;
    }
    pres = cgltf_load_buffers(&options, imp.data, path);
    if (pres != cgltf_result_success) {
        if (pres == cgltf_result_out_of_memory) {
            la_set_error(manager, "out of memory loading buffers");
            res = LA_ERROR_OUT_OF_MEMORY;
        } else if (imp.failed_path[0] != '\0') {
            la_set_error(manager, "missing buffer file '%s'",
                         imp.failed_path);
            res = LA_ERROR_NOT_FOUND;
        } else {
            la_set_error(manager, "cannot load buffers for '%s'", path);
            res = LA_ERROR_IMPORT;
        }
        goto done;
    }
    if (imp.data->meshes_count > UINT32_MAX ||
        imp.data->materials_count > UINT32_MAX ||
        imp.data->images_count > UINT32_MAX ||
        imp.data->samplers_count > UINT32_MAX ||
        imp.data->skins_count > UINT32_MAX ||
        imp.data->animations_count > UINT32_MAX ||
        imp.data->nodes_count > (cgltf_size)INT32_MAX) {
        la_set_error(manager, "asset counts out of range");
        res = LA_ERROR_IMPORT;
        goto done;
    }
    model = (la_model *)calloc(1, sizeof(la_model));
    if (model == NULL) {
        res = LA_ERROR_OUT_OF_MEMORY;
        goto done;
    }
    model->manager = manager;
    model->source_path = NULL;
    {
        size_t len = strlen(path);

        model->source_path = (char *)malloc(len + 1u);
        if (model->source_path == NULL) {
            res = LA_ERROR_OUT_OF_MEMORY;
            goto done;
        }
        memcpy(model->source_path, path, len + 1u);
    }
    imp.model = model;
    model->source_texture_count = (uint32_t)imp.data->images_count;
    model->source_sampler_count = (uint32_t)imp.data->samplers_count;
    res = la_build_nodes(&imp);
    if (res != LA_SUCCESS) {
        goto done;
    }
    res = la_build_skins(&imp);
    if (res != LA_SUCCESS) {
        goto done;
    }
    /* Animations decode before any mesh uploads so malformed
     * tracks fail pre-GPU (headless-testable, no partial GPU
     * state to unwind). */
    res = la_build_anims(&imp);
    if (res != LA_SUCCESS) {
        goto done;
    }
    res = la_build_meshes(&imp);
    if (res != LA_SUCCESS) {
        goto done;
    }
    res = la_build_bounds(&imp);
    if (res != LA_SUCCESS) {
        goto done;
    }
    /* Publish: link after full success only. */
    model->next = manager->models;
    model->prev = NULL;
    if (manager->models != NULL) {
        manager->models->prev = model;
    }
    manager->models = model;
    *out_model = model;
    model = NULL;
    res = LA_SUCCESS;

done:
    if (model != NULL) {
        la_model_teardown(model);
        free(model);
    }
    if (imp.data != NULL) {
        cgltf_free(imp.data);
    }
    free(imp.dir);
    return res;
}
