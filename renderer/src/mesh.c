/*
 * Renderer meshes (Phase 13): static GPU geometry with local bounds.
 *
 * Meshes own one GPU-only vertex buffer (lr_vertex stride) plus one
 * GPU-only index buffer (UINT32). Uploads reuse public LumaC staging;
 * no backend headers included. Liveness is renderer-local (no global
 * registry, so multiple renderers stay independent).
 */

#include <float.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

void lr_mesh_list_add(lr_renderer *renderer, lr_mesh *mesh) {
    if (renderer == NULL || mesh == NULL) {
        return;
    }
    mesh->next = renderer->meshes;
    mesh->prev = NULL;
    if (renderer->meshes != NULL) {
        renderer->meshes->prev = mesh;
    }
    renderer->meshes = mesh;
}

void lr_mesh_list_remove(lr_mesh *mesh) {
    lr_renderer *renderer;

    if (mesh == NULL || mesh->renderer == NULL) {
        return;
    }
    renderer = mesh->renderer;
    if (mesh->prev != NULL) {
        mesh->prev->next = mesh->next;
    } else if (renderer->meshes == mesh) {
        renderer->meshes = mesh->next;
    }
    if (mesh->next != NULL) {
        mesh->next->prev = mesh->prev;
    }
    mesh->next = NULL;
    mesh->prev = NULL;
}

int lr_mesh_is_live(const lr_renderer *renderer, const lr_mesh *mesh) {
    const lr_mesh *it;

    if (renderer == NULL || mesh == NULL) {
        return 0;
    }
    for (it = renderer->meshes; it != NULL; it = it->next) {
        if (it == mesh) {
            return 1;
        }
    }
    return 0;
}

lr_result lr_mesh_compute_bounds(const lr_mesh_desc *desc,
                                 lr_bounds *out_bounds) {
    uint32_t i;
    float radius = 0.0f;

    if (desc == NULL || out_bounds == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (desc->vertices == NULL || desc->vertex_count == 0) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    out_bounds->min[0] = FLT_MAX;
    out_bounds->min[1] = FLT_MAX;
    out_bounds->min[2] = FLT_MAX;
    out_bounds->max[0] = -FLT_MAX;
    out_bounds->max[1] = -FLT_MAX;
    out_bounds->max[2] = -FLT_MAX;
    for (i = 0; i < desc->vertex_count; i++) {
        const float *p = desc->vertices[i].position;
        int a;

        for (a = 0; a < 3; a++) {
            if (p[a] < out_bounds->min[a]) {
                out_bounds->min[a] = p[a];
            }
            if (p[a] > out_bounds->max[a]) {
                out_bounds->max[a] = p[a];
            }
        }
    }
    out_bounds->center[0] =
        (out_bounds->min[0] + out_bounds->max[0]) * 0.5f;
    out_bounds->center[1] =
        (out_bounds->min[1] + out_bounds->max[1]) * 0.5f;
    out_bounds->center[2] =
        (out_bounds->min[2] + out_bounds->max[2]) * 0.5f;
    for (i = 0; i < desc->vertex_count; i++) {
        const float *p = desc->vertices[i].position;
        float dx = p[0] - out_bounds->center[0];
        float dy = p[1] - out_bounds->center[1];
        float dz = p[2] - out_bounds->center[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);

        if (d > radius) {
            radius = d;
        }
    }
    out_bounds->radius = radius;
    return LR_SUCCESS;
}

lr_result lr_mesh_create(lr_renderer *renderer,
                         const lr_mesh_desc *desc,
                         lr_mesh **out_mesh) {
    lr_mesh *mesh;
    lc_buffer_desc bdesc;
    uint32_t i;

    if (renderer == NULL || desc == NULL || out_mesh == NULL) {
        if (out_mesh != NULL) {
            *out_mesh = NULL;
        }
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (desc->vertices == NULL || desc->vertex_count == 0 ||
        desc->indices == NULL || desc->index_count == 0 ||
        (desc->index_count % 3u) != 0u) {
        *out_mesh = NULL;
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Every index must reference a live vertex. */
    for (i = 0; i < desc->index_count; i++) {
        if (desc->indices[i] >= desc->vertex_count) {
            *out_mesh = NULL;
            return LR_ERROR_INVALID_ARGUMENT;
        }
    }

    mesh = (lr_mesh *)calloc(1, sizeof(lr_mesh));
    if (mesh == NULL) {
        *out_mesh = NULL;
        return LR_ERROR_OUT_OF_MEMORY;
    }
    mesh->renderer = renderer;
    mesh->vertex_count = desc->vertex_count;
    mesh->index_count = desc->index_count;
    mesh->lod_count = 1; /* base level only until add_lod */
    mesh->lod_min_px[0] = 0.0f;
    mesh->lod_index_counts[0] = desc->index_count;
    /* Phase 29: one O(verts) scan decides the skinned flag for the
     * mesh's lifetime (exact float compare against the rigid
     * convention; see lr_skin_scan_vertices). */
    mesh->skinned = lr_skin_scan_vertices(desc->vertices,
                                          desc->vertex_count);
    if (lr_mesh_compute_bounds(desc, &mesh->bounds) != LR_SUCCESS) {
        free(mesh);
        *out_mesh = NULL;
        return LR_ERROR_INVALID_ARGUMENT;
    }

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = (uint64_t)desc->vertex_count * sizeof(lr_vertex);
    bdesc.usage = LC_BUFFER_USAGE_VERTEX;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(renderer->device, &bdesc, &mesh->vertex_buffer) !=
        LC_SUCCESS) {
        free(mesh);
        *out_mesh = NULL;
        return LR_ERROR_RENDER;
    }
    if (lc_buffer_write(mesh->vertex_buffer, 0, desc->vertices,
                        bdesc.size) != LC_SUCCESS) {
        lc_buffer_destroy(mesh->vertex_buffer);
        free(mesh);
        *out_mesh = NULL;
        return LR_ERROR_RENDER;
    }
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = (uint64_t)desc->index_count * sizeof(uint32_t);
    bdesc.usage = LC_BUFFER_USAGE_INDEX;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(renderer->device, &bdesc, &mesh->index_buffer) !=
        LC_SUCCESS) {
        lc_buffer_destroy(mesh->vertex_buffer);
        free(mesh);
        *out_mesh = NULL;
        return LR_ERROR_RENDER;
    }
    if (lc_buffer_write(mesh->index_buffer, 0, desc->indices, bdesc.size) !=
        LC_SUCCESS) {
        lc_buffer_destroy(mesh->index_buffer);
        lc_buffer_destroy(mesh->vertex_buffer);
        free(mesh);
        *out_mesh = NULL;
        return LR_ERROR_RENDER;
    }

    lr_mesh_list_add(renderer, mesh);
    *out_mesh = mesh;
    return LR_SUCCESS;
}

void lr_mesh_destroy(lr_mesh *mesh) {
    uint32_t i;

    if (mesh == NULL) {
        return;
    }
    lr_mesh_list_remove(mesh);
    /* Buffers die with plain LumaC destroys (NULL-safe); the device
     * must still be alive (dependents-first contract). */
    for (i = 1; i < mesh->lod_count && i < LR_MESH_MAX_LODS; i++) {
        lc_buffer_destroy(mesh->lod_index_buffers[i]);
        mesh->lod_index_buffers[i] = NULL;
    }
    lc_buffer_destroy(mesh->index_buffer);
    lc_buffer_destroy(mesh->vertex_buffer);
    free(mesh);
}

uint32_t lr_mesh_get_vertex_count(const lr_mesh *mesh) {
    if (mesh == NULL) {
        return 0;
    }
    return mesh->vertex_count;
}

uint32_t lr_mesh_get_index_count(const lr_mesh *mesh) {
    if (mesh == NULL) {
        return 0;
    }
    return mesh->index_count;
}

lr_result lr_mesh_add_lod(lr_mesh *mesh, const uint32_t *indices,
                          uint32_t index_count, float min_pixels) {
    lc_buffer_desc bdesc;
    lc_buffer *buffer = NULL;
    uint32_t i;

    if (mesh == NULL || indices == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (index_count == 0 || (index_count % 3u) != 0u) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!(min_pixels > 0.0f)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (mesh->lod_count >= LR_MESH_MAX_LODS) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Thresholds strictly descend: each level takes over at a
     * smaller projected size than the previous one. */
    if (mesh->lod_count > 1 &&
        min_pixels >= mesh->lod_min_px[mesh->lod_count - 1]) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Every LOD index must reference a base vertex (shared vertex
     * buffer; simplified topology only). */
    for (i = 0; i < index_count; i++) {
        if (indices[i] >= mesh->vertex_count) {
            return LR_ERROR_INVALID_ARGUMENT;
        }
    }
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = (uint64_t)index_count * sizeof(uint32_t);
    bdesc.usage = LC_BUFFER_USAGE_INDEX;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(mesh->renderer->device, &bdesc, &buffer) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_buffer_write(buffer, 0, indices, bdesc.size) !=
        LC_SUCCESS) {
        lc_buffer_destroy(buffer);
        return LR_ERROR_RENDER;
    }
    mesh->lod_index_buffers[mesh->lod_count] = buffer;
    mesh->lod_index_counts[mesh->lod_count] = index_count;
    mesh->lod_min_px[mesh->lod_count] = min_pixels;
    mesh->lod_count++;
    return LR_SUCCESS;
}

uint32_t lr_mesh_get_lod_count(const lr_mesh *mesh) {
    if (mesh == NULL) {
        return 0;
    }
    return mesh->lod_count;
}

void lr_mesh_get_bounds(const lr_mesh *mesh, lr_bounds *out_bounds) {
    if (out_bounds == NULL) {
        return;
    }
    if (mesh == NULL) {
        memset(out_bounds, 0, sizeof(*out_bounds));
        return;
    }
    *out_bounds = mesh->bounds;
}
