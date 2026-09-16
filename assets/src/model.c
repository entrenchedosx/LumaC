/*
 * Model objects (Phase 14, PART D/F/U/V/W): hierarchy + metadata +
 * submission. Import itself lives in gltf_import.c; GPU uploads go
 * through public renderer/LumaC APIs only.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_assets/luma_assets.h"
#include "internal/assets_internal.h"

/* ------------------------------------------------------------------
 * Pure geometry utilities (no GPU, headless-testable).
 * ------------------------------------------------------------------ */

la_result la_compute_normals(const float *positions, uint32_t vertex_count,
                             const uint32_t *indices, uint32_t index_count,
                             float *out_normals) {
    uint32_t t;
    uint32_t i;

    if (positions == NULL || indices == NULL || out_normals == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (vertex_count == 0 || index_count == 0 ||
        (index_count % 3u) != 0u) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < index_count; i++) {
        if (indices[i] >= vertex_count) {
            return LA_ERROR_INVALID_ARGUMENT;
        }
    }
    memset(out_normals, 0, sizeof(float) * vertex_count * 3u);
    for (t = 0; t < index_count; t += 3u) {
        const float *p0 = positions + (size_t)indices[t] * 3u;
        const float *p1 = positions + (size_t)indices[t + 1u] * 3u;
        const float *p2 = positions + (size_t)indices[t + 2u] * 3u;
        float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        /* Unnormalized cross = area-weighted face normal. */
        float n[3] = { e1[1] * e2[2] - e1[2] * e2[1],
                       e1[2] * e2[0] - e1[0] * e2[2],
                       e1[0] * e2[1] - e1[1] * e2[0] };
        float *o0 = out_normals + (size_t)indices[t] * 3u;
        float *o1 = out_normals + (size_t)indices[t + 1u] * 3u;
        float *o2 = out_normals + (size_t)indices[t + 2u] * 3u;

        o0[0] += n[0];
        o0[1] += n[1];
        o0[2] += n[2];
        o1[0] += n[0];
        o1[1] += n[1];
        o1[2] += n[2];
        o2[0] += n[0];
        o2[1] += n[1];
        o2[2] += n[2];
    }
    for (i = 0; i < vertex_count; i++) {
        float *n = out_normals + (size_t)i * 3u;
        float len =
            sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);

        if (len > 1e-9f) {
            n[0] /= len;
            n[1] /= len;
            n[2] /= len;
        } else {
            /* Isolated/degenerate fan: deterministic +Y fallback. */
            n[0] = 0.0f;
            n[1] = 1.0f;
            n[2] = 0.0f;
        }
    }
    return LA_SUCCESS;
}

la_result la_compute_tangents(const float *positions, const float *normals,
                              const float *uvs, uint32_t vertex_count,
                              const uint32_t *indices, uint32_t index_count,
                              float *out_tangents) {
    float *tan1 = NULL;
    float *tan2 = NULL;
    uint32_t t;
    uint32_t i;
    la_result res = LA_SUCCESS;

    if (positions == NULL || normals == NULL || uvs == NULL ||
        indices == NULL || out_tangents == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (vertex_count == 0 || index_count == 0 ||
        (index_count % 3u) != 0u) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < index_count; i++) {
        if (indices[i] >= vertex_count) {
            return LA_ERROR_INVALID_ARGUMENT;
        }
    }
    tan1 = (float *)calloc((size_t)vertex_count * 3u, sizeof(float));
    tan2 = (float *)calloc((size_t)vertex_count * 3u, sizeof(float));
    if (tan1 == NULL || tan2 == NULL) {
        free(tan1);
        free(tan2);
        return LA_ERROR_OUT_OF_MEMORY;
    }
    for (t = 0; t < index_count; t += 3u) {
        uint32_t i0 = indices[t];
        uint32_t i1 = indices[t + 1u];
        uint32_t i2 = indices[t + 2u];
        const float *p0 = positions + (size_t)i0 * 3u;
        const float *p1 = positions + (size_t)i1 * 3u;
        const float *p2 = positions + (size_t)i2 * 3u;
        const float *w0 = uvs + (size_t)i0 * 2u;
        const float *w1 = uvs + (size_t)i1 * 2u;
        const float *w2 = uvs + (size_t)i2 * 2u;
        float e1[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
        float e2[3] = { p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2] };
        float du1 = w1[0] - w0[0];
        float dv1 = w1[1] - w0[1];
        float du2 = w2[0] - w0[0];
        float dv2 = w2[1] - w0[1];
        float denom = du1 * dv2 - du2 * dv1;
        float sdir[3];
        float tdir[3];
        float *a1;
        float *a2;
        float *b1;
        float *b2;
        float *c1;
        float *c2;

        /* Degenerate UV triangle: contributes nothing (neighbors or
         * the fallback below still define the vertex). */
        if (denom > -1e-12f && denom < 1e-12f) {
            continue;
        }
        sdir[0] = (dv2 * e1[0] - dv1 * e2[0]) / denom;
        sdir[1] = (dv2 * e1[1] - dv1 * e2[1]) / denom;
        sdir[2] = (dv2 * e1[2] - dv1 * e2[2]) / denom;
        tdir[0] = (du1 * e2[0] - du2 * e1[0]) / denom;
        tdir[1] = (du1 * e2[1] - du2 * e1[1]) / denom;
        tdir[2] = (du1 * e2[2] - du2 * e1[2]) / denom;
        a1 = tan1 + (size_t)i0 * 3u;
        a2 = tan2 + (size_t)i0 * 3u;
        b1 = tan1 + (size_t)i1 * 3u;
        b2 = tan2 + (size_t)i1 * 3u;
        c1 = tan1 + (size_t)i2 * 3u;
        c2 = tan2 + (size_t)i2 * 3u;
        a1[0] += sdir[0];
        a1[1] += sdir[1];
        a1[2] += sdir[2];
        b1[0] += sdir[0];
        b1[1] += sdir[1];
        b1[2] += sdir[2];
        c1[0] += sdir[0];
        c1[1] += sdir[1];
        c1[2] += sdir[2];
        a2[0] += tdir[0];
        a2[1] += tdir[1];
        a2[2] += tdir[2];
        b2[0] += tdir[0];
        b2[1] += tdir[1];
        b2[2] += tdir[2];
        c2[0] += tdir[0];
        c2[1] += tdir[1];
        c2[2] += tdir[2];
    }
    for (i = 0; i < vertex_count; i++) {
        const float *n = normals + (size_t)i * 3u;
        const float *t1 = tan1 + (size_t)i * 3u;
        const float *t2 = tan2 + (size_t)i * 3u;
        float *o = out_tangents + (size_t)i * 4u;
        float dot = n[0] * t1[0] + n[1] * t1[1] + n[2] * t1[2];
        float t[3] = { t1[0] - n[0] * dot, t1[1] - n[1] * dot,
                       t1[2] - n[2] * dot };
        float len = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        float cx;
        float cy;
        float cz;

        if (len > 1e-9f) {
            t[0] /= len;
            t[1] /= len;
            t[2] /= len;
        } else {
            t[0] = 1.0f;
            t[1] = 0.0f;
            t[2] = 0.0f;
        }
        /* Handedness: sign of (n x t) . tdir. */
        cx = n[1] * t[2] - n[2] * t[1];
        cy = n[2] * t[0] - n[0] * t[2];
        cz = n[0] * t[1] - n[1] * t[0];
        o[0] = t[0];
        o[1] = t[1];
        o[2] = t[2];
        o[3] = (cx * t2[0] + cy * t2[1] + cz * t2[2]) < 0.0f ? -1.0f : 1.0f;
    }
    free(tan1);
    free(tan2);
    return res;
}

la_result la_matrix_to_transform(const float matrix[16],
                                 lr_transform *out_transform) {
    float c0[3];
    float c1[3];
    float c2[3];
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
    float det;
    float trace;
    float s;
    float x;
    float y;
    float z;
    float w;

    if (matrix == NULL || out_transform == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    c0[0] = matrix[0];
    c0[1] = matrix[1];
    c0[2] = matrix[2];
    c1[0] = matrix[4];
    c1[1] = matrix[5];
    c1[2] = matrix[6];
    c2[0] = matrix[8];
    c2[1] = matrix[9];
    c2[2] = matrix[10];
    sx = sqrtf(c0[0] * c0[0] + c0[1] * c0[1] + c0[2] * c0[2]);
    sy = sqrtf(c1[0] * c1[0] + c1[1] * c1[1] + c1[2] * c1[2]);
    sz = sqrtf(c2[0] * c2[0] + c2[1] * c2[1] + c2[2] * c2[2]);
    if (sx < 1e-9f || sy < 1e-9f || sz < 1e-9f) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    /* Mirror absorption: a negative determinant folds one reflection
     * into X scale so rotation stays proper (documented convention). */
    det = c0[0] * (c1[1] * c2[2] - c1[2] * c2[1]) -
          c1[0] * (c0[1] * c2[2] - c0[2] * c2[1]) +
          c2[0] * (c0[1] * c1[2] - c0[2] * c1[1]);
    if (det < 0.0f) {
        sx = -sx;
    }
    r00 = c0[0] / sx;
    r10 = c0[1] / sx;
    r20 = c0[2] / sx;
    r01 = c1[0] / sy;
    r11 = c1[1] / sy;
    r21 = c1[2] / sy;
    r02 = c2[0] / sz;
    r12 = c2[1] / sz;
    r22 = c2[2] / sz;
    trace = r00 + r11 + r22;
    if (trace > 0.0f) {
        s = sqrtf(trace + 1.0f) * 2.0f;
        w = s * 0.25f;
        x = (r21 - r12) / s;
        y = (r02 - r20) / s;
        z = (r10 - r01) / s;
    } else if (r00 > r11 && r00 > r22) {
        s = sqrtf(1.0f + r00 - r11 - r22) * 2.0f;
        w = (r21 - r12) / s;
        x = s * 0.25f;
        y = (r01 + r10) / s;
        z = (r02 + r20) / s;
    } else if (r11 > r22) {
        s = sqrtf(1.0f + r11 - r00 - r22) * 2.0f;
        w = (r02 - r20) / s;
        x = (r01 + r10) / s;
        y = s * 0.25f;
        z = (r12 + r21) / s;
    } else {
        s = sqrtf(1.0f + r22 - r00 - r11) * 2.0f;
        w = (r10 - r01) / s;
        x = (r02 + r20) / s;
        y = (r12 + r21) / s;
        z = s * 0.25f;
    }
    out_transform->position[0] = matrix[12];
    out_transform->position[1] = matrix[13];
    out_transform->position[2] = matrix[14];
    out_transform->rotation[0] = x;
    out_transform->rotation[1] = y;
    out_transform->rotation[2] = z;
    out_transform->rotation[3] = w;
    out_transform->scale[0] = sx;
    out_transform->scale[1] = sy;
    out_transform->scale[2] = sz;
    return LA_SUCCESS;
}

/* ------------------------------------------------------------------
 * Model lifetime + inspection + submission.
 * ------------------------------------------------------------------ */

static void la_model_list_add(la_asset_manager *manager, la_model *model) {
    model->next = manager->models;
    model->prev = NULL;
    if (manager->models != NULL) {
        manager->models->prev = model;
    }
    manager->models = model;
}

static void la_model_list_remove(la_model *model) {
    la_asset_manager *manager = model->manager;

    if (model->prev != NULL) {
        model->prev->next = model->next;
    } else if (manager->models == model) {
        manager->models = model->next;
    }
    if (model->next != NULL) {
        model->next->prev = model->prev;
    }
    model->next = NULL;
    model->prev = NULL;
}

static int la_model_is_live(const la_asset_manager *manager,
                            const la_model *model) {
    const la_model *it;

    if (manager == NULL || model == NULL) {
        return 0;
    }
    for (it = manager->models; it != NULL; it = it->next) {
        if (it == model) {
            return 1;
        }
    }
    return 0;
}

void la_model_teardown(la_model *model) {
    uint32_t i;
    uint32_t p;

    if (model == NULL) {
        return;
    }
    if (model->meshes != NULL) {
        for (i = 0; i < model->mesh_count; i++) {
            for (p = 0; p < model->meshes[i].primitive_count; p++) {
                lr_mesh_destroy(model->meshes[i].primitives[p].mesh);
            }
            free(model->meshes[i].primitives);
        }
        free(model->meshes);
        model->meshes = NULL;
    }
    if (model->materials != NULL) {
        for (i = 0; i < model->material_count; i++) {
            lr_material_destroy(model->materials[i].material);
        }
        free(model->materials);
        model->materials = NULL;
    }
    if (model->textures != NULL) {
        for (i = 0; i < model->texture_count; i++) {
            la_texture_release(model->textures[i]);
        }
        free(model->textures);
        model->textures = NULL;
    }
    if (model->samplers != NULL) {
        for (i = 0; i < model->sampler_count; i++) {
            la_sampler_release(model->samplers[i]);
        }
        free(model->samplers);
        model->samplers = NULL;
    }
    for (i = 0; i < model->node_count; i++) {
        free((void *)model->nodes[i].name);
    }
    free(model->nodes);
    free(model->child_links);
    if (model->skins != NULL) {
        for (i = 0; i < model->skin_count; i++) {
            free(model->skins[i].joint_nodes);
            free(model->skins[i].inv_bind);
        }
        free(model->skins);
        model->skins = NULL;
    }
    if (model->anims != NULL) {
        for (i = 0; i < model->anim_count; i++) {
            uint32_t c;

            for (c = 0; c < model->anims[i].channel_count; c++) {
                free(model->anims[i].channels[c].times);
                free(model->anims[i].channels[c].values);
            }
            free(model->anims[i].channels);
        }
        free(model->anims);
        model->anims = NULL;
    }
    free(model->source_path);
    model->nodes = NULL;
    model->child_links = NULL;
    model->source_path = NULL;
}

void la_model_destroy(la_model *model) {
    if (model == NULL) {
        return;
    }
    if (model->manager != NULL) {
        la_model_list_remove(model);
    }
    la_model_teardown(model);
    free(model);
}

uint32_t la_model_get_node_count(const la_model *model) {
    return (model != NULL) ? model->node_count : 0;
}

uint32_t la_model_get_mesh_count(const la_model *model) {
    uint32_t count = 0;
    uint32_t m;

    if (model == NULL) {
        return 0;
    }
    /* Unique lr_mesh objects across all mesh primitives. */
    for (m = 0; m < model->mesh_count; m++) {
        count += model->meshes[m].primitive_count;
    }
    return count;
}

uint32_t la_model_get_material_count(const la_model *model) {
    return (model != NULL) ? model->material_count : 0;
}

uint32_t la_model_get_texture_count(const la_model *model) {
    return (model != NULL) ? model->texture_count : 0;
}

uint32_t la_model_get_sampler_count(const la_model *model) {
    return (model != NULL) ? model->sampler_count : 0;
}

uint32_t la_model_get_source_primitive_count(const la_model *model) {
    return (model != NULL) ? model->source_primitive_count : 0;
}

uint32_t la_model_get_source_texture_count(const la_model *model) {
    return (model != NULL) ? model->source_texture_count : 0;
}

uint32_t la_model_get_source_sampler_count(const la_model *model) {
    return (model != NULL) ? model->source_sampler_count : 0;
}

uint32_t la_model_get_instance_count(const la_model *model) {
    return (model != NULL) ? model->instance_count : 0;
}

la_result la_model_adopt_mesh(la_model *model, uint32_t mesh_index,
                              uint32_t primitive_index,
                              lr_mesh **out_mesh) {
    if (out_mesh != NULL) {
        *out_mesh = NULL;
    }
    if (model == NULL || out_mesh == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (!la_model_is_live(model->manager, model)) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (mesh_index >= model->mesh_count) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (primitive_index >=
        model->meshes[mesh_index].primitive_count) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (model->meshes[mesh_index].primitives[primitive_index].mesh ==
        NULL) {
        return LA_ERROR_INVALID_ARGUMENT; /* already adopted */
    }
    *out_mesh =
        model->meshes[mesh_index].primitives[primitive_index].mesh;
    model->meshes[mesh_index].primitives[primitive_index].mesh = NULL;
    return LA_SUCCESS;
}

la_result la_model_adopt_material(la_model *model,
                                  uint32_t material_index,
                                  lr_material **out_material) {
    if (out_material != NULL) {
        *out_material = NULL;
    }
    if (model == NULL || out_material == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (!la_model_is_live(model->manager, model)) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (material_index >= model->material_count) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (model->materials[material_index].material == NULL) {
        return LA_ERROR_INVALID_ARGUMENT; /* already adopted */
    }
    *out_material = model->materials[material_index].material;
    model->materials[material_index].material = NULL;
    return LA_SUCCESS;
}

lr_mesh *la_model_borrow_mesh(const la_model *model,
                              uint32_t mesh_index,
                              uint32_t primitive_index) {
    if (model == NULL || mesh_index >= model->mesh_count) {
        return NULL;
    }
    if (primitive_index >=
        model->meshes[mesh_index].primitive_count) {
        return NULL;
    }
    return model->meshes[mesh_index].primitives[primitive_index].mesh;
}

uint32_t la_model_get_primitive_count(const la_model *model,
                                      uint32_t mesh_index) {
    if (model == NULL || mesh_index >= model->mesh_count) {
        return 0;
    }
    return model->meshes[mesh_index].primitive_count;
}

uint32_t la_model_get_primitive_material(const la_model *model,
                                         uint32_t mesh_index,
                                         uint32_t primitive_index) {
    if (model == NULL || mesh_index >= model->mesh_count) {
        return UINT32_MAX;
    }
    if (primitive_index >=
        model->meshes[mesh_index].primitive_count) {
        return UINT32_MAX;
    }
    return model->meshes[mesh_index]
        .primitives[primitive_index]
        .material_index;
}

const la_model_node *la_model_get_node(const la_model *model,
                                       uint32_t index) {
    if (model == NULL || index >= model->node_count) {
        return NULL;
    }
    return &model->nodes[index];
}

const la_pbr_material_data *la_model_get_material_data(
    const la_model *model, uint32_t index) {
    if (model == NULL || index >= model->material_count) {
        return NULL;
    }
    return &model->materials[index].data;
}

void la_model_get_texture_info(const la_model *model, uint32_t index,
                               la_texture_info *out) {
    la_texture_info empty;

    memset(&empty, 0, sizeof(empty));
    if (out == NULL) {
        return;
    }
    if (model == NULL || index >= model->texture_count ||
        model->textures[index] == NULL) {
        *out = empty;
        return;
    }
    out->width = model->textures[index]->width;
    out->height = model->textures[index]->height;
    out->srgb = model->textures[index]->srgb;
    out->source = model->textures[index]->key;
}

void la_model_get_sampler_info(const la_model *model, uint32_t index,
                               la_sampler_info *out) {
    la_sampler_info empty;

    memset(&empty, 0, sizeof(empty));
    if (out == NULL) {
        return;
    }
    if (model == NULL || index >= model->sampler_count ||
        model->samplers[index] == NULL) {
        *out = empty;
        return;
    }
    out->params = model->samplers[index]->params;
}

void la_model_get_bounds(const la_model *model, lr_bounds *out) {
    if (out == NULL) {
        return;
    }
    if (model == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = model->bounds;
}

const char *la_model_get_source_path(const la_model *model) {
    if (model == NULL) {
        return NULL;
    }
    return (model->source_path != NULL) ? model->source_path : "";
}

/* ------------------------------------------------------------------
 * Skin + animation queries (Phase 29). All NULL-safe; every
 * out-of-range index yields the documented zero/NULL/-1/
 * identity/0 instead of touching memory.
 * ------------------------------------------------------------------ */

uint32_t la_model_get_skin_count(const la_model *model) {
    return (model != NULL) ? model->skin_count : 0;
}

uint32_t la_model_get_skin_joint_count(const la_model *model,
                                       uint32_t skin_index) {
    if (model == NULL || skin_index >= model->skin_count) {
        return 0;
    }
    return model->skins[skin_index].joint_count;
}

int32_t la_model_get_skin_joint_node(const la_model *model,
                                     uint32_t skin_index,
                                     uint32_t joint) {
    if (model == NULL || skin_index >= model->skin_count) {
        return -1;
    }
    if (joint >= model->skins[skin_index].joint_count) {
        return -1;
    }
    return model->skins[skin_index].joint_nodes[joint];
}

void la_model_get_skin_inverse_bind(const la_model *model,
                                    uint32_t skin_index,
                                    uint32_t joint,
                                    float out_inv_bind[16]) {
    static const float k_identity[16] = { 1.0f, 0.0f, 0.0f, 0.0f,
                                          0.0f, 1.0f, 0.0f, 0.0f,
                                          0.0f, 0.0f, 1.0f, 0.0f,
                                          0.0f, 0.0f, 0.0f, 1.0f };

    if (out_inv_bind == NULL) {
        return;
    }
    if (model == NULL || skin_index >= model->skin_count ||
        joint >= model->skins[skin_index].joint_count ||
        model->skins[skin_index].inv_bind == NULL) {
        memcpy(out_inv_bind, k_identity, sizeof(k_identity));
        return;
    }
    memcpy(out_inv_bind,
           model->skins[skin_index].inv_bind + (size_t)joint * 16u,
           sizeof(float) * 16u);
}

int32_t la_model_get_node_skin(const la_model *model,
                               uint32_t node_index) {
    if (model == NULL || node_index >= model->node_count) {
        return -1;
    }
    return model->nodes[node_index].skin_index;
}

uint32_t la_model_get_animation_count(const la_model *model) {
    return (model != NULL) ? model->anim_count : 0;
}

uint32_t la_model_get_animation_channel_count(const la_model *model,
                                              uint32_t anim_index) {
    if (model == NULL || anim_index >= model->anim_count) {
        return 0;
    }
    return model->anims[anim_index].channel_count;
}

int la_model_get_animation_channel(const la_model *model,
                                   uint32_t anim_index,
                                   uint32_t channel_index,
                                   la_anim_channel *out) {
    const la_model_anim_channel *src;

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (model == NULL || out == NULL) {
        return 0;
    }
    if (anim_index >= model->anim_count ||
        channel_index >= model->anims[anim_index].channel_count) {
        return 0;
    }
    src = &model->anims[anim_index].channels[channel_index];
    out->target_node = src->target_node;
    out->path = src->path;
    out->interpolation = src->interpolation;
    out->key_count = src->key_count;
    out->times = src->times;
    out->values = src->values;
    return 1;
}

float la_model_get_animation_duration(const la_model *model,
                                      uint32_t anim_index) {
    if (model == NULL || anim_index >= model->anim_count) {
        return 0.0f;
    }
    return model->anims[anim_index].duration;
}

la_result la_model_submit(const la_model *model, lr_renderer *renderer,
                          const lr_transform *root_transform) {
    float root_matrix[16];
    float *world;
    uint32_t *stack;
    uint32_t n;
    uint32_t p;
    uint32_t top;

    if (model == NULL || renderer == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (model->manager == NULL || model->manager->renderer != renderer) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (!la_model_is_live(model->manager, model)) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (root_transform != NULL) {
        lr_transform_to_matrix(root_transform, root_matrix);
    } else {
        memset(root_matrix, 0, sizeof(root_matrix));
        root_matrix[0] = 1.0f;
        root_matrix[5] = 1.0f;
        root_matrix[10] = 1.0f;
        root_matrix[15] = 1.0f;
    }
    if (model->node_count == 0) {
        return LA_SUCCESS;
    }
    /* Iterative depth-first traversal (file order; no recursion so
     * hostile depths cannot overflow the call stack). */
    world = (float *)malloc(sizeof(float) * 16u * model->node_count);
    stack = (uint32_t *)malloc(sizeof(uint32_t) * model->node_count);
    if (world == NULL || stack == NULL) {
        free(world);
        free(stack);
        return LA_ERROR_OUT_OF_MEMORY;
    }
    /* Seed roots in reverse so the lowest index pops first. */
    top = 0;
    for (n = model->node_count; n-- > 0;) {
        if (model->nodes[n].parent < 0) {
            float *w = world + (size_t)n * 16u;
            uint32_t r;
            uint32_t c;

            for (r = 0; r < 4; r++) {
                for (c = 0; c < 4; c++) {
                    w[c * 4u + r] = 0.0f;
                }
            }
            for (r = 0; r < 4; r++) {
                for (c = 0; c < 4; c++) {
                    float sum = 0.0f;
                    uint32_t k;

                    for (k = 0; k < 4; k++) {
                        sum += root_matrix[k * 4u + r] *
                               model->nodes[n].local_matrix[c * 4u + k];
                    }
                    w[c * 4u + r] = sum;
                }
            }
            stack[top++] = n;
        }
    }
    while (top > 0) {
        uint32_t ni;
        float *parent_world;

        /* Pop, submit, then push children reversed (file order). */
        top--;
        ni = stack[top];
        parent_world = world + (size_t)ni * 16u;
        if (model->nodes[ni].mesh_index >= 0) {
            uint32_t mi = (uint32_t)model->nodes[ni].mesh_index;

            if (mi < model->mesh_count) {
                for (p = 0; p < model->meshes[mi].primitive_count; p++) {
                    lr_draw_item item;
                    la_model_primitive *prim =
                        &model->meshes[mi].primitives[p];

                    memset(&item, 0, sizeof(item));
                    /* Mesh/material handles are model-owned and live
                     * (models outlive submission by contract); the
                     * renderer re-validates ownership at submit.
                     * Imported opaque models cast + receive shadows. */
                    item.mesh = prim->mesh;
                    if (prim->material_index < model->material_count) {
                        item.material =
                            model->materials[prim->material_index].material;
                    } else {
                        continue;
                    }
                    item.casts_shadow = 1;
                    item.receives_shadow = 1;
                    if (la_matrix_to_transform(parent_world,
                                               &item.transform) !=
                        LA_SUCCESS) {
                        free(world);
                        free(stack);
                        return LA_ERROR_RENDER;
                    }
                    if (la_map_lr(lr_renderer_submit(renderer, &item)) !=
                        LA_SUCCESS) {
                        free(world);
                        free(stack);
                        return LA_ERROR_RENDER;
                    }
                }
            }
        }
        {
            uint32_t ci;
            uint32_t base = model->nodes[ni].first_child;
            uint32_t count = model->nodes[ni].child_count;

            for (ci = count; ci-- > 0;) {
                uint32_t child = model->child_links[base + ci];
                float *cw = world + (size_t)child * 16u;
                const float *lw =
                    model->nodes[child].local_matrix;
                uint32_t r;
                uint32_t c;

                for (r = 0; r < 4; r++) {
                    for (c = 0; c < 4; c++) {
                        float sum = 0.0f;
                        uint32_t k;

                        for (k = 0; k < 4; k++) {
                            sum += parent_world[k * 4u + r] * lw[c * 4u + k];
                        }
                        cw[c * 4u + r] = sum;
                    }
                }
                stack[top++] = child;
            }
        }
    }
    free(world);
    free(stack);
    return LA_SUCCESS;
}
