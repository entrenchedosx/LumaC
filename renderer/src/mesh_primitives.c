/*
 * Renderer primitive meshes (Phase 13): cube, plane, sphere with
 * PBR-ready attributes (unit normals, UV-matched tangents with
 * handedness in w). Winding is CCW-outward everywhere to match the
 * renderer's BACK/CCW raster state (same convention proven by the
 * LumaC cube lineage). Pure data fillers stay headless-testable;
 * create wrappers upload through their renderer.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

#ifndef LR_PIF
#define LR_PIF 3.14159265358979323846f
#endif

static void lr_write_vertex(lr_vertex *v, float px, float py, float pz,
                            float nx, float ny, float nz, float tx, float ty,
                            float tz, float tw, float u, float vv) {
    v->position[0] = px;
    v->position[1] = py;
    v->position[2] = pz;
    v->normal[0] = nx;
    v->normal[1] = ny;
    v->normal[2] = nz;
    v->tangent[0] = tx;
    v->tangent[1] = ty;
    v->tangent[2] = tz;
    v->tangent[3] = tw;
    v->texcoord[0] = u;
    v->texcoord[1] = vv;
}

lr_result lr_mesh_cube_data(lr_vertex *out_vertices, uint32_t *out_indices,
                            float size) {
    static const uint32_t k_indices[LR_CUBE_INDEX_COUNT] = {
        0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7, 8, 9, 10, 8, 10, 11,
        12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22,
        20, 22, 23,
    };
    float h;

    if (out_vertices == NULL || out_indices == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!(size > 0.0f)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    h = size * 0.5f;
    /* Per-face tangents verified against UV axes (all w = +1). */
    lr_write_vertex(&out_vertices[0], -h, -h, h, 0, 0, 1, 1, 0, 0, 1, 0, 0);
    lr_write_vertex(&out_vertices[1], h, -h, h, 0, 0, 1, 1, 0, 0, 1, 1, 0);
    lr_write_vertex(&out_vertices[2], h, h, h, 0, 0, 1, 1, 0, 0, 1, 1, 1);
    lr_write_vertex(&out_vertices[3], -h, h, h, 0, 0, 1, 1, 0, 0, 1, 0, 1);
    lr_write_vertex(&out_vertices[4], h, -h, -h, 0, 0, -1, -1, 0, 0, 1, 0, 0);
    lr_write_vertex(&out_vertices[5], -h, -h, -h, 0, 0, -1, -1, 0, 0, 1, 1,
                    0);
    lr_write_vertex(&out_vertices[6], -h, h, -h, 0, 0, -1, -1, 0, 0, 1, 1,
                    1);
    lr_write_vertex(&out_vertices[7], h, h, -h, 0, 0, -1, -1, 0, 0, 1, 0,
                    1);
    lr_write_vertex(&out_vertices[8], h, -h, h, 1, 0, 0, 0, 0, -1, 1, 0, 0);
    lr_write_vertex(&out_vertices[9], h, -h, -h, 1, 0, 0, 0, 0, -1, 1, 1,
                    0);
    lr_write_vertex(&out_vertices[10], h, h, -h, 1, 0, 0, 0, 0, -1, 1, 1,
                    1);
    lr_write_vertex(&out_vertices[11], h, h, h, 1, 0, 0, 0, 0, -1, 1, 0,
                    1);
    lr_write_vertex(&out_vertices[12], -h, -h, -h, -1, 0, 0, 0, 0, 1, 1, 0,
                    0);
    lr_write_vertex(&out_vertices[13], -h, -h, h, -1, 0, 0, 0, 0, 1, 1, 1,
                    0);
    lr_write_vertex(&out_vertices[14], -h, h, h, -1, 0, 0, 0, 0, 1, 1, 1,
                    1);
    lr_write_vertex(&out_vertices[15], -h, h, -h, -1, 0, 0, 0, 0, 1, 1, 0,
                    1);
    lr_write_vertex(&out_vertices[16], -h, h, h, 0, 1, 0, 1, 0, 0, 1, 0,
                    0);
    lr_write_vertex(&out_vertices[17], h, h, h, 0, 1, 0, 1, 0, 0, 1, 1, 0);
    lr_write_vertex(&out_vertices[18], h, h, -h, 0, 1, 0, 1, 0, 0, 1, 1,
                    1);
    lr_write_vertex(&out_vertices[19], -h, h, -h, 0, 1, 0, 1, 0, 0, 1, 0,
                    1);
    lr_write_vertex(&out_vertices[20], -h, -h, -h, 0, -1, 0, 1, 0, 0, -1,
                    0, 0);
    lr_write_vertex(&out_vertices[21], h, -h, -h, 0, -1, 0, 1, 0, 0, -1,
                    1, 0);
    lr_write_vertex(&out_vertices[22], h, -h, h, 0, -1, 0, 1, 0, 0, -1,
                    1, 1);
    lr_write_vertex(&out_vertices[23], -h, -h, h, 0, -1, 0, 1, 0, 0, -1,
                    0, 1);
    memcpy(out_indices, k_indices, sizeof(k_indices));
    return LR_SUCCESS;
}

lr_result lr_mesh_create_cube(lr_renderer *renderer, float size,
                              lr_mesh **out_mesh) {
    lr_vertex verts[LR_CUBE_VERTEX_COUNT];
    uint32_t indices[LR_CUBE_INDEX_COUNT];
    lr_mesh_desc desc;

    if (renderer == NULL || out_mesh == NULL) {
        if (out_mesh != NULL) {
            *out_mesh = NULL;
        }
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (lr_mesh_cube_data(verts, indices, size) != LR_SUCCESS) {
        *out_mesh = NULL;
        return LR_ERROR_INVALID_ARGUMENT;
    }
    memset(&desc, 0, sizeof(desc));
    desc.vertices = verts;
    desc.vertex_count = LR_CUBE_VERTEX_COUNT;
    desc.indices = indices;
    desc.index_count = LR_CUBE_INDEX_COUNT;
    return lr_mesh_create(renderer, &desc, out_mesh);
}

lr_result lr_mesh_plane_data(lr_vertex *out_vertices, uint32_t *out_indices,
                             float width, float depth) {
    static const uint32_t k_indices[LR_PLANE_INDEX_COUNT] = { 0, 1, 2, 0,
                                                              2, 3 };
    float hw;
    float hd;

    if (out_vertices == NULL || out_indices == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!(width > 0.0f) || !(depth > 0.0f)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    hw = width * 0.5f;
    hd = depth * 0.5f;
    /* +Y face, CCW-outward, tangent +X with w = -1 (V runs +Z). */
    lr_write_vertex(&out_vertices[0], -hw, 0, -hd, 0, 1, 0, 1, 0, 0, -1, 0,
                    0);
    lr_write_vertex(&out_vertices[1], -hw, 0, hd, 0, 1, 0, 1, 0, 0, -1, 0,
                    1);
    lr_write_vertex(&out_vertices[2], hw, 0, hd, 0, 1, 0, 1, 0, 0, -1, 1,
                    1);
    lr_write_vertex(&out_vertices[3], hw, 0, -hd, 0, 1, 0, 1, 0, 0, -1, 1,
                    0);
    memcpy(out_indices, k_indices, sizeof(k_indices));
    return LR_SUCCESS;
}

lr_result lr_mesh_create_plane(lr_renderer *renderer, float width,
                               float depth, lr_mesh **out_mesh) {
    lr_vertex verts[LR_PLANE_VERTEX_COUNT];
    uint32_t indices[LR_PLANE_INDEX_COUNT];
    lr_mesh_desc desc;

    if (renderer == NULL || out_mesh == NULL) {
        if (out_mesh != NULL) {
            *out_mesh = NULL;
        }
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (lr_mesh_plane_data(verts, indices, width, depth) != LR_SUCCESS) {
        *out_mesh = NULL;
        return LR_ERROR_INVALID_ARGUMENT;
    }
    memset(&desc, 0, sizeof(desc));
    desc.vertices = verts;
    desc.vertex_count = LR_PLANE_VERTEX_COUNT;
    desc.indices = indices;
    desc.index_count = LR_PLANE_INDEX_COUNT;
    return lr_mesh_create(renderer, &desc, out_mesh);
}

lr_result lr_mesh_sphere_data(lr_vertex *out_vertices, uint32_t *out_indices,
                              float radius, uint32_t segments,
                              uint32_t rings) {
    uint32_t cols;
    uint32_t i;
    uint32_t j;

    if (out_vertices == NULL || out_indices == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!(radius > 0.0f) || segments < 3 || rings < 2) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    cols = segments + 1u;
    for (j = 0; j <= rings; j++) {
        float theta = LR_PIF * (float)j / (float)rings;
        float sin_t = sinf(theta);
        float cos_t = cosf(theta);

        for (i = 0; i <= segments; i++) {
            float phi = 2.0f * LR_PIF * (float)i / (float)segments;
            float sin_p = sinf(phi);
            float cos_p = cosf(phi);
            float nx = sin_t * cos_p;
            float ny = cos_t;
            float nz = sin_t * sin_p;
            float tx = -sin_p;
            float ty = 0.0f;
            float tz = cos_p;
            float bx = cos_t * cos_p;
            float by = -sin_t;
            float bz = cos_t * sin_p;
            float cx = ny * tz - nz * ty;
            float cy = nz * tx - nx * tz;
            float cz = nx * ty - ny * tx;
            float w = (cx * bx + cy * by + cz * bz) < 0.0f ? -1.0f : 1.0f;
            lr_vertex *v = &out_vertices[j * cols + i];

            v->position[0] = radius * nx;
            v->position[1] = radius * ny;
            v->position[2] = radius * nz;
            v->normal[0] = nx;
            v->normal[1] = ny;
            v->normal[2] = nz;
            v->tangent[0] = tx;
            v->tangent[1] = ty;
            v->tangent[2] = tz;
            v->tangent[3] = w;
            v->texcoord[0] = (float)i / (float)segments;
            v->texcoord[1] = (float)j / (float)rings;
        }
    }
    {
        uint32_t k = 0;

        for (j = 0; j < rings; j++) {
            for (i = 0; i < segments; i++) {
                uint32_t a = j * cols + i;
                uint32_t b = a + cols;
                uint32_t c = a + 1u;
                uint32_t d = b + 1u;

                /* CCW-outward (verified at the equator). */
                out_indices[k++] = a;
                out_indices[k++] = d;
                out_indices[k++] = b;
                out_indices[k++] = a;
                out_indices[k++] = c;
                out_indices[k++] = d;
            }
        }
    }
    return LR_SUCCESS;
}

lr_result lr_mesh_create_sphere(lr_renderer *renderer, float radius,
                                uint32_t segments, uint32_t rings,
                                lr_mesh **out_mesh) {
    lr_vertex *verts = NULL;
    uint32_t *indices = NULL;
    lr_mesh_desc desc;
    lr_result res;

    if (renderer == NULL || out_mesh == NULL) {
        if (out_mesh != NULL) {
            *out_mesh = NULL;
        }
        return LR_ERROR_INVALID_ARGUMENT;
    }
    verts = (lr_vertex *)malloc(sizeof(lr_vertex) * (segments + 1u) *
                               (rings + 1u));
    indices = (uint32_t *)malloc(sizeof(uint32_t) * segments * rings * 6u);
    if (verts == NULL || indices == NULL) {
        free(verts);
        free(indices);
        *out_mesh = NULL;
        return LR_ERROR_OUT_OF_MEMORY;
    }
    /* Data validation first (bad size/segments fail before upload). */
    res = lr_mesh_sphere_data(verts, indices, radius, segments, rings);
    if (res != LR_SUCCESS) {
        free(verts);
        free(indices);
        *out_mesh = NULL;
        return res;
    }
    /* Guard against overflow-shaped requests the data call accepted. */
    if (segments > 4096u || rings > 4096u) {
        free(verts);
        free(indices);
        *out_mesh = NULL;
        return LR_ERROR_INVALID_ARGUMENT;
    }
    memset(&desc, 0, sizeof(desc));
    desc.vertices = verts;
    desc.vertex_count = (segments + 1u) * (rings + 1u);
    desc.indices = indices;
    desc.index_count = segments * rings * 6u;
    res = lr_mesh_create(renderer, &desc, out_mesh);
    free(verts);
    free(indices);
    return res;
}
