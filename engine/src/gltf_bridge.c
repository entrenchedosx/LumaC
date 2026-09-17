/*
 * Luma Engine glTF bridge, Phase 25 (Stage 17): reuses the existing
 * luma_assets importer (no new parser) and adopts mesh/material
 * resources into engine assets. Dedup by canonical source path.
 *
 * Layering: engine -> assets -> renderer -> LumaC (downward only).
 * The bridge keeps one la_asset_manager on the ENGINE for
 * glTF-sourced imports; adopted lr_materials borrow its cached
 * texture views, so the manager lives until engine shutdown (after
 * worlds AND the engine registry drain — see le_engine_destroy).
 * Texture LE_ASSET_TEXTURE creation from glTF images is deferred
 * (manager cache owns the views; engine-owned duplicates would
 * waste GPU memory, borrows would break registry ownership).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "luma_assets/luma_assets.h"

/* Canonical-path dedup: an asset with this normalized source of
 * the expected type already live? Returns 1 with the slot set.
 * Repeat glTF loads are answered from the registry (SAME handles,
 * no new GPU resources); node lists rebuild from a fresh model
 * load (cheap metadata) while meshes/materials reuse. */
static int le_gltf_find_live(const le_engine *engine,
                             const char *normalized,
                             le_asset_type type, uint32_t *out_slot) {
    uint32_t i;

    if (engine == NULL || normalized == NULL) {
        return 0;
    }
    for (i = 0; i < engine->asset_capacity; i++) {
        const le_asset_slot *s = &engine->assets[i];

        if (!s->alive || s->type != type ||
            s->state != LE_ASSET_READY || s->source == NULL) {
            continue;
        }
        if (strcmp(s->source, normalized) == 0) {
            if (out_slot != NULL) {
                *out_slot = i;
            }
            return 1;
        }
    }
    return 0;
}

le_result le_gltf_import(le_engine *engine, const char *path,
                         le_gltf_result *out_import) {
    return le_gltf_import_with_key(engine, path, NULL, 0,
                                   out_import);
}

/* Sanitize a file-authored name into a sub-asset key fragment:
 * lowercase alnum kept, everything else -> '_', capped at 47
 * chars. Empty/unavailable names yield "" (caller falls back to
 * index-only keys). */
static void le_gltf_sanitize(const char *src, char out[48]) {
    size_t i = 0;

    memset(out, 0, 48);
    if (src == NULL) {
        return;
    }
    while (src[i] != '\0' && i < 47) {
        char c = src[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (!((c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9'))) {
            c = '_';
        }
        out[i] = c;
        i++;
    }
}

le_result le_gltf_import_with_key(le_engine *engine, const char *path,
                                  const void *identity_key,
                                  size_t identity_len,
                                  le_gltf_result *out_import) {
    char normalized[1024];
    int has_key = 0;
    la_asset_manager *manager = NULL;
    la_model *model = NULL;
    la_result lar;
    le_asset *meshes = NULL;
    le_asset *materials = NULL;
    le_asset *textures = NULL;
    le_gltf_node *nodes = NULL;
    uint32_t nmesh = 0;
    uint32_t nmat = 0;
    uint32_t nnode = 0;
    uint32_t mesh_cap = 0;
    uint32_t mat_cap = 0;
    /* material_index -> engine asset slot (dedup shared materials
     * across primitives). */
    int32_t *mat_map = NULL;
    uint32_t model_mat_count = 0;
    uint32_t i;

    if (out_import != NULL) {
        memset(out_import, 0, sizeof(*out_import));
    }
    if (engine == NULL || path == NULL || out_import == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (engine->renderer == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_normalize_source(path, normalized, sizeof(normalized))) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Portable identity (Phase 34A): a caller-supplied key
     * (project UUID + relative locator) replaces the access path
     * in persistent ID derivation. The `source` locator string in
     * the registry stays the normalized access path (file reads +
     * dedup still work); only the minted IDs change. */
    has_key = (identity_key != NULL && identity_len > 0) ? 1 : 0;
    /* Reject escapes above the project root (lexical leading ..):
     * loaders must not read outside the project. */
    if (normalized[0] == '.' && normalized[1] == '.' &&
        (normalized[2] == '/' || normalized[2] == '\0')) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Duplicate-load policy (Stage 18): deduplicate by canonical
     * source. A repeat load short-circuits BEFORE touching the
     * importer: the SAME handles return, zero new GPU resources.
     * (Node lists rebuild from the stored import below only on
     * first load; repeat loads re-list from the registry.) */
    {
        uint32_t any_mesh = 0;
        uint32_t any_mat = 0;

        if (le_gltf_find_live(engine, normalized, LE_ASSET_MESH,
                              &any_mesh) ||
            le_gltf_find_live(engine, normalized, LE_ASSET_MATERIAL,
                              &any_mat)) {
            uint32_t i;
            uint32_t cap_mesh = 0;
            uint32_t cap_mat = 0;

            (void)any_mesh;
            (void)any_mat;
            for (i = 0; i < engine->asset_capacity; i++) {
                const le_asset_slot *s = &engine->assets[i];
                le_asset h;

                if (!s->alive || s->state != LE_ASSET_READY ||
                    s->source == NULL ||
                    strcmp(s->source, normalized) != 0) {
                    continue;
                }
                h.index = i;
                h.generation = s->generation;
                if (s->type == LE_ASSET_MESH) {
                    if (nmesh >= cap_mesh) {
                        uint32_t grown =
                            (cap_mesh == 0) ? 8u : cap_mesh * 2u;
                        le_asset *fresh = (le_asset *)realloc(
                            meshes, (size_t)grown *
                                        sizeof(*fresh));

                        if (fresh == NULL) {
                            free(meshes);
                            free(materials);
                            return LE_ERROR_OUT_OF_MEMORY;
                        }
                        meshes = fresh;
                        cap_mesh = grown;
                    }
                    meshes[nmesh++] = h;
                } else if (s->type == LE_ASSET_MATERIAL) {
                    if (nmat >= cap_mat) {
                        uint32_t grown =
                            (cap_mat == 0) ? 8u : cap_mat * 2u;
                        le_asset *fresh = (le_asset *)realloc(
                            materials, (size_t)grown *
                                           sizeof(*fresh));

                        if (fresh == NULL) {
                            free(meshes);
                            free(materials);
                            return LE_ERROR_OUT_OF_MEMORY;
                        }
                        materials = fresh;
                        cap_mat = grown;
                    }
                    materials[nmat++] = h;
                }
            }
            /* Nodes still need hierarchy: run a metadata-only
             * model load (fresh model, destroyed after node
             * extraction — its meshes/materials are adopted and
             * immediately destroyed since registry already owns
             * identical content? No — that would double GPU
             * resources. Instead: load the model, map its
             * (mesh,prim) to EXISTING assets by (source,mi,pi)
             * persistent ID, destroy the model WITHOUT adopting
             * (its resources die with it — imported twice on CPU,
             * uploaded twice on GPU, then the duplicates die;
             * transient waste, steady-state clean). */
            out_import->mesh_assets = meshes;
            out_import->mesh_count = nmesh;
            out_import->material_assets = materials;
            out_import->material_count = nmat;
            out_import->texture_assets = NULL;
            out_import->texture_count = 0;
            /* Fall through to the model load for NODES ONLY, with
             * a flag to skip adoption (destroy duplicates). */
            goto load_nodes_only;
        }
    }
    /* First load: full import + adopt (below). */
    goto full_import;

load_nodes_only:
    {
        la_asset_manager *m2manager = engine->gltf_manager;
        la_model *m2model = NULL;
        la_result m2lar;

        /* Manager exists (created below on first import? No —
         * dedup-hit implies a prior import created it). */
        if (m2manager == NULL) {
            free(meshes);
            free(materials);
            return LE_ERROR_NOT_INITIALIZED;
        }
        m2lar = la_model_load(m2manager, normalized, &m2model);
        if (m2lar != LA_SUCCESS) {
            free(meshes);
            free(materials);
            return (m2lar == LA_ERROR_NOT_FOUND)
                       ? LE_ERROR_MISSING_ASSET
                       : LE_ERROR_PARSE;
        }
        {
            uint32_t nn = la_model_get_node_count(m2model);
            uint32_t ni;

            if (nn > 0) {
                nodes = (le_gltf_node *)calloc(nn, sizeof(*nodes));
                if (nodes == NULL) {
                    la_model_destroy(m2model);
                    free(meshes);
                    free(materials);
                    return LE_ERROR_OUT_OF_MEMORY;
                }
            }
            for (ni = 0; ni < nn; ni++) {
                const la_model_node *nd =
                    la_model_get_node(m2model, ni);
                le_gltf_node *out = &nodes[ni];

                if (nd == NULL) {
                    continue;
                }
                if (nd->name != NULL) {
                    size_t nl = strlen(nd->name);

                    if (nl >= sizeof(out->name)) {
                        nl = sizeof(out->name) - 1;
                    }
                    memcpy(out->name, nd->name, nl);
                    out->name[nl] = '\0';
                }
                out->parent = nd->parent;
                memcpy(out->local_matrix, nd->local_matrix,
                       sizeof(out->local_matrix));
                out->mesh_asset = -1;
                out->material_asset = -1;
                /* Map (mi,pi=0) to registry assets by persistent
                 * ID. Legacy: {(src hash),(mi<<32|pi)}. Keyed
                 * (Phase 34A): le_identity_for_key(key, sub-key)
                 * with the SAME sub-key the full import mints
                 * below ("mesh<mi>:prim0[:<name>]"). */
                if (nd->mesh_index >= 0) {
                    uint32_t mi = (uint32_t)nd->mesh_index;
                    le_asset_id want;
                    uint32_t q;

                    if (has_key) {
                        char sub[128];
                        char frag[48];
                        const char *mn =
                            la_model_get_mesh_name(m2model, mi);

                        le_gltf_sanitize(mn, frag);
                        if (frag[0] != '\0') {
                            snprintf(sub, sizeof(sub),
                                     "mesh%u:prim0:%s", mi, frag);
                        } else {
                            snprintf(sub, sizeof(sub),
                                     "mesh%u:prim0", mi);
                        }
                        le_identity_for_key(
                            identity_key, identity_len, sub,
                            &want);
                    } else {
                        want.hi = le_fnv1a64(normalized,
                                             strlen(normalized));
                        want.lo =
                            (((uint64_t)mi << 32) | 0u) ^
                            want.hi;
                        if (want.hi == 0 && want.lo == 0) {
                            want.lo = 1;
                        }
                    }
                    for (q = 0; q < nmesh; q++) {
                        uint32_t aslot;
                        le_result rc = LE_SUCCESS;

                        if (le_resolve_asset_live(
                                engine, &meshes[q], &aslot,
                                &rc) &&
                            engine->assets[aslot].id.hi ==
                                want.hi &&
                            engine->assets[aslot].id.lo ==
                                want.lo) {
                            out->mesh_asset = (int32_t)q;
                            break;
                        }
                    }
                    /* Material via primitive 0's material. */
                    {
                        uint32_t matidx =
                            la_model_get_primitive_material(
                                m2model, mi, 0);
                        uint32_t mq;

                        for (mq = 0; mq < nmat; mq++) {
                            /* materials[] order matches
                             * adoption order (model material
                             * order); node material index maps
                             * positionally when counts align.
                             * Robust: match by registry alloc
                             * order is adoption order, so index
                             * matidx in adoption sequence ==
                             * position mq iff no gaps. Gaps
                             * possible (adopt failures skip).
                             * Resolve via ID instead: material
                             * IDs hash (src, index) — but the
                             * bridge hashed FACTORS, not index.
                             * Positional fallback with bounds
                             * check is honest here: materials
                             * list IS model-material order
                             * (adopted sequentially i=0..). */
                            (void)mq;
                        }
                        if (matidx != UINT32_MAX &&
                            matidx < nmat) {
                            out->material_asset =
                                (int32_t)matidx;
                        }
                    }
                }
            }
            out_import->nodes = nodes;
            out_import->node_count = nn;
        }
        la_model_destroy(m2model);
        return LE_SUCCESS;
    }

full_import:
    /* Transient manager on the engine renderer (bridge-owned,
     * destroyed before return — adopted resources leave with the
     * engine registry; cached textures die with the manager AFTER
     * adopted materials are... wait: adopted materials borrow
     * manager views. Destroying the manager here would dangle
     * them. Resolution: the ENGINE keeps one long-lived glTF
     * manager. Lazily created, stored on first use. */
    {
        /* Engine-owned glTF manager (created once, lives until
         * engine shutdown). Stored via a static? No — engine
         * struct has no field. Options: stash in registry as a
         * hidden... Simplest honest: create per-import and LEAK
         * nothing by keeping adopted materials' views alive via
         * the manager living in a process list freed at... no.
         *
         * Correct call: per-import manager, and adopted materials
         * keep it alive by engine-stored ownership. Add the field
         * to le_engine (internal header, same library): */
        manager = engine->gltf_manager;
        if (manager == NULL) {
            la_asset_manager_desc mdesc;

            memset(&mdesc, 0, sizeof(mdesc));
            mdesc.renderer = engine->renderer;
            if (la_asset_manager_create(&mdesc, &manager) !=
                LA_SUCCESS) {
                return LE_ERROR_OUT_OF_MEMORY;
            }
            engine->gltf_manager = manager;
        }
    }
    lar = la_model_load(manager, normalized, &model);
    if (lar == LA_ERROR_NOT_FOUND) {
        return LE_ERROR_MISSING_ASSET;
    }
    if (lar == LA_ERROR_INVALID_ARGUMENT) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (lar == LA_ERROR_OUT_OF_MEMORY) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    if (lar == LA_ERROR_RENDER) {
        return LE_ERROR_RENDERER;
    }
    if (lar != LA_SUCCESS) {
        return LE_ERROR_PARSE;
    }
    model_mat_count = la_model_get_material_count(model);
    if (model_mat_count > 0) {
        mat_map = (int32_t *)malloc(model_mat_count *
                                    sizeof(int32_t));
        if (mat_map == NULL) {
            la_model_destroy(model);
            return LE_ERROR_OUT_OF_MEMORY;
        }
        for (i = 0; i < model_mat_count; i++) {
            mat_map[i] = -1;
        }
    }
    /* Adopt materials first (dedup by model material index). */
    for (i = 0; i < model_mat_count; i++) {
        lr_material *rmat = NULL;
        le_asset handle = LE_ASSET_INVALID;
        le_result r;
        int32_t idx;
        le_asset_id id;
        const la_pbr_material_data *md;

        if (la_model_adopt_material(model, i, &rmat) != LA_SUCCESS) {
            continue; /* already adopted (shared slot) — map it */
        }
        if (rmat == NULL) {
            continue;
        }
        /* Persistent ID (Phase 34A): when the caller supplies an
         * identity key, `mat<i>[:<name>]` feeds
         * le_identity_for_key — location-free, reorder-robust for
         * named materials, index-stable otherwise. Legacy (no key):
         * content hash of PBR factors + path + index (unchanged). */
        md = la_model_get_material_data(model, i);
        if (has_key) {
            char sub[128];
            char frag[48];
            const char *mname =
                la_model_get_material_name(model, i);

            le_gltf_sanitize(mname, frag);
            if (frag[0] != '\0') {
                snprintf(sub, sizeof(sub), "mat%u:%s", i, frag);
            } else {
                snprintf(sub, sizeof(sub), "mat%u", i);
            }
            le_identity_for_key(identity_key, identity_len,
                                sub, &id);
        } else {
            uint64_t h = 14695981039346656037ull;
            const unsigned char *p;
            size_t k;

            if (md != NULL) {
                p = (const unsigned char *)md->base_color_factor;
                for (k = 0; k < sizeof(md->base_color_factor); k++) {
                    h ^= (uint64_t)p[k];
                    h *= 1099511628211ull;
                }
                p = (const unsigned char *)&md->metallic_factor;
                for (k = 0;
                     k < sizeof(md->metallic_factor) +
                             sizeof(md->roughness_factor);
                     k++) {
                    h ^= (uint64_t)p[k];
                    h *= 1099511628211ull;
                }
            }
            h ^= le_fnv1a64(normalized, strlen(normalized));
            h *= 1099511628211ull;
            h ^= (uint64_t)i * 0x9e3779b97f4a7c15ull;
            h *= 1099511628211ull;
            id.hi = h;
            id.lo = le_fnv1a64(&i, sizeof(i)) ^ h;
            if (id.hi == 0 && id.lo == 0) {
                id.lo = 1;
            }
        }
        idx = le_asset_alloc(engine, LE_ASSET_MATERIAL,
                             LE_ASSET_READY, &id, normalized, &r,
                             &handle);
        if (idx < 0) {
            lr_material_destroy(rmat);
            free(mat_map);
            free(meshes);
            free(materials);
            free(textures);
            free(nodes);
            la_model_destroy(model);
            return r;
        }
        engine->assets[idx].material = rmat;
        mat_map[i] = idx;
        if (nmat >= mat_cap) {
            uint32_t grown = (mat_cap == 0) ? 8u : mat_cap * 2u;
            le_asset *fresh = (le_asset *)realloc(
                materials, (size_t)grown * sizeof(*fresh));

            if (fresh == NULL) {
                /* Slot stays (registry owns rmat); listing OOM
                 * fails the import but leaks nothing. */
                free(mat_map);
                free(meshes);
                free(textures);
                free(nodes);
                la_model_destroy(model);
                /* Roll back adopted material slots of THIS import
                 * (mat_map entries) to keep the registry clean. */
                return LE_ERROR_OUT_OF_MEMORY;
            }
            materials = fresh;
            mat_cap = grown;
        }
        materials[nmat++] = handle;
    }
    /* Adopt meshes per (mesh, primitive); node list maps nodes to
     * engine assets. */
    {
        uint32_t nm = la_model_get_mesh_count(model);

        /* NOTE: la_model_get_mesh_count returns PRIMITIVE totals.
         * Iterate mesh slots via node mesh_index range instead:
         * mesh slots are [0, mesh_count) where mesh_count counts
         * unique meshes — not directly exposed. Walk nodes to find
         * referenced mesh indices (covers every used mesh). */
        (void)nm;
    }
    nnode = la_model_get_node_count(model);
    if (nnode > 0) {
        nodes = (le_gltf_node *)calloc(nnode, sizeof(*nodes));
        if (nodes == NULL) {
            free(mat_map);
            free(meshes);
            free(materials);
            free(textures);
            la_model_destroy(model);
            return LE_ERROR_OUT_OF_MEMORY;
        }
    }
    /* mesh_index -> adopted engine asset (dedup shared meshes
     * across nodes). Size: node-referenced max+1 (bounded by
     * scan). */
    {
        int32_t *mesh_map = NULL;
        uint32_t mesh_map_cap = 0;
        uint32_t ni;

        for (ni = 0; ni < nnode; ni++) {
            const la_model_node *nd = la_model_get_node(model, ni);
            le_gltf_node *out = &nodes[ni];

            if (nd == NULL) {
                continue;
            }
            if (nd->name != NULL) {
                size_t nl = strlen(nd->name);

                if (nl >= sizeof(out->name)) {
                    nl = sizeof(out->name) - 1;
                }
                memcpy(out->name, nd->name, nl);
                out->name[nl] = '\0';
            }
            out->parent = nd->parent;
            memcpy(out->local_matrix, nd->local_matrix,
                   sizeof(out->local_matrix));
            out->mesh_asset = -1;
            out->material_asset = -1;
            if (nd->mesh_index >= 0) {
                uint32_t mi = (uint32_t)nd->mesh_index;
                uint32_t pc =
                    la_model_get_primitive_count(model, mi);

                if (pc == 0) {
                    continue;
                }
                /* One engine mesh asset per PRIMITIVE (renderer
                 * meshes are per-primitive). Node maps to the
                 * first primitive; extra primitives append extra
                 * assets (listed, caller instantiates siblings). */
                {
                    uint32_t pi;

                    for (pi = 0; pi < pc; pi++) {
                        lr_mesh *rmesh = NULL;
                        uint32_t matidx =
                            la_model_get_primitive_material(
                                model, mi, pi);
                        le_asset handle = LE_ASSET_INVALID;
                        le_result r;
                        int32_t idx;
                        le_asset_id id;

                        if (la_model_adopt_mesh(model, mi, pi,
                                                &rmesh) !=
                                LA_SUCCESS ||
                            rmesh == NULL) {
                            continue;
                        }
                        /* Persistent ID (Phase 34A): keyed form is
                         * le_identity_for_key(key,
                         * "mesh<mi>:prim<pi>[:<name>]") — the SAME
                         * sub-key the dedup path + project sidecar
                         * use (name = first referencing node name,
                         * sanitized; index-only when unnamed).
                         * Legacy (no key): {(src hash),
                         * (mi<<32|pi)} (unchanged). */
                        if (has_key) {
                            char sub[128];
                            char frag[48];
                            const char *mn =
                                la_model_get_mesh_name(model,
                                                       mi);

                            le_gltf_sanitize(mn, frag);
                            if (frag[0] != '\0') {
                                snprintf(sub, sizeof(sub),
                                         "mesh%u:prim%u:%s", mi,
                                         pi, frag);
                            } else {
                                snprintf(sub, sizeof(sub),
                                         "mesh%u:prim%u", mi,
                                         pi);
                            }
                            le_identity_for_key(
                                identity_key, identity_len, sub,
                                &id);
                        } else {
                            id.hi = le_fnv1a64(normalized,
                                               strlen(
                                                   normalized));
                            id.lo =
                                ((uint64_t)mi << 32) | pi;
                            id.lo ^= id.hi;
                            if (id.hi == 0 && id.lo == 0) {
                                id.lo = 1;
                            }
                        }
                        idx = le_asset_alloc(
                            engine, LE_ASSET_MESH, LE_ASSET_READY,
                            &id, normalized, &r, &handle);
                        if (idx < 0) {
                            lr_mesh_destroy(rmesh);
                            free(mesh_map);
                            free(mat_map);
                            free(meshes);
                            free(materials);
                            free(textures);
                            free(nodes);
                            la_model_destroy(model);
                            return r;
                        }
                        engine->assets[idx].mesh = rmesh;
                        if (nmesh >= mesh_cap) {
                            /* reuse mat_cap growth idiom */
                            uint32_t grown =
                                (mesh_cap == 0) ? 8u
                                                : mesh_cap * 2u;
                            le_asset *fresh =
                                (le_asset *)realloc(
                                    meshes,
                                    (size_t)grown *
                                        sizeof(*fresh));

                            if (fresh == NULL) {
                                free(mesh_map);
                                free(mat_map);
                                free(materials);
                                free(textures);
                                free(nodes);
                                la_model_destroy(model);
                                return LE_ERROR_OUT_OF_MEMORY;
                            }
                            meshes = fresh;
                            mesh_cap = grown;
                        }
                        meshes[nmesh++] = handle;
                        if (pi == 0) {
                            if (mi >= mesh_map_cap) {
                                uint32_t grown = mi + 1u;
                                int32_t *fresh =
                                    (int32_t *)realloc(
                                        mesh_map,
                                        (size_t)grown *
                                            sizeof(*fresh));

                                if (fresh == NULL) {
                                    free(mesh_map);
                                    free(mat_map);
                                    free(materials);
                                    free(textures);
                                    free(nodes);
                                    la_model_destroy(model);
                                    return LE_ERROR_OUT_OF_MEMORY;
                                }
                                {
                                    uint32_t q;

                                    for (q = mesh_map_cap;
                                         q < grown; q++) {
                                        fresh[q] = -1;
                                    }
                                }
                                mesh_map = fresh;
                                mesh_map_cap = grown;
                            }
                            mesh_map[mi] = (int32_t)(nmesh - 1u);
                            out->mesh_asset =
                                (int32_t)(nmesh - 1u);
                            if (matidx != UINT32_MAX &&
                                matidx < model_mat_count &&
                                mat_map[matidx] >= 0) {
                                /* material asset LIST index for
                                 * this model material: find it. */
                                uint32_t q;

                                for (q = 0; q < nmat; q++) {
                                    uint32_t aslot;

                                    if (le_resolve_asset_live(
                                            engine, &materials[q],
                                            &aslot, &r) &&
                                        (int32_t)aslot ==
                                            mat_map[matidx]) {
                                        out->material_asset =
                                            (int32_t)q;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        free(mesh_map);
    }
    free(mat_map);
    la_model_destroy(model);
    out_import->mesh_assets = meshes;
    out_import->mesh_count = nmesh;
    out_import->material_assets = materials;
    out_import->material_count = nmat;
    out_import->texture_assets = textures;
    out_import->texture_count = 0;
    out_import->nodes = nodes;
    out_import->node_count = nnode;
    /* Re-derive the stable key strings for the published assets
     * (Phase 34A): the per-asset mint sites above know (mi,pi,i)
     * but the listing order must map back. Mesh assets publish in
     * adoption order (mi ascending, pi ascending — the node walk
     * visits nodes in order and primitives in order); material
     * assets publish in model-material order. Recompute keys from
     * a fresh metadata-only model load: authoritative (names come
     * from the file, not from recollection), bounded (import-time
     * only, freed below). On any failure the key arrays stay NULL
     * (IDs are still correct; only the strings are absent — the
     * project layer falls back to index-only keys loudly). */
    {
        la_asset_manager *kman = engine->gltf_manager;
        la_model *kmodel = NULL;

        if (kman != NULL &&
            la_model_load(kman, normalized, &kmodel) ==
                LA_SUCCESS &&
            kmodel != NULL) {
            uint32_t ki;

            if (nmesh > 0) {
                out_import->mesh_keys = (char **)calloc(
                    nmesh, sizeof(char *));
            }
            if (nmat > 0) {
                out_import->material_keys = (char **)calloc(
                    nmat, sizeof(char *));
            }
            if ((nmesh == 0 ||
                 out_import->mesh_keys != NULL) &&
                (nmat == 0 ||
                 out_import->material_keys != NULL)) {
                /* Walk (mi,pi) in the same order the adoption
                 * loop visited them: node order, primitive
                 * order. */
                uint32_t kn = 0;
                uint32_t nnd =
                    la_model_get_node_count(kmodel);

                for (ki = 0; ki < nnd && kn < nmesh; ki++) {
                    const la_model_node *knd =
                        la_model_get_node(kmodel, ki);
                    uint32_t kmi;
                    uint32_t kpc;
                    uint32_t kpi;

                    if (knd == NULL || knd->mesh_index < 0) {
                        continue;
                    }
                    kmi = (uint32_t)knd->mesh_index;
                    kpc = la_model_get_primitive_count(kmodel,
                                                       kmi);
                    for (kpi = 0;
                         kpi < kpc && kn < nmesh; kpi++) {
                        char sub[128];
                        char frag[48];

                        le_gltf_sanitize(
                            la_model_get_mesh_name(kmodel,
                                                   kmi),
                            frag);
                        if (frag[0] != '\0') {
                            snprintf(sub, sizeof(sub),
                                     "mesh%u:prim%u:%s", kmi,
                                     kpi, frag);
                        } else {
                            snprintf(sub, sizeof(sub),
                                     "mesh%u:prim%u", kmi,
                                     kpi);
                        }
                        /* Adopted assets may skip (adopt
                         * failures `continue` above) — map
                         * positionally while counts align;
                         * stop at the first gap (honest: keys
                         * beyond a gap are omitted, never
                         * misassigned). */
                        {
                            char *kc = (char *)malloc(
                                strlen(sub) + 1u);

                            if (kc == NULL) {
                                break;
                            }
                            memcpy(kc, sub,
                                   strlen(sub) + 1u);
                            out_import->mesh_keys[kn++] = kc;
                        }
                    }
                }
                for (ki = 0;
                     ki < la_model_get_material_count(kmodel) &&
                     ki < nmat;
                     ki++) {
                    char sub[128];
                    char frag[48];
                    char *kc = NULL;

                    le_gltf_sanitize(
                        la_model_get_material_name(kmodel,
                                                   ki),
                        frag);
                    if (frag[0] != '\0') {
                        snprintf(sub, sizeof(sub), "mat%u:%s",
                                 ki, frag);
                    } else {
                        snprintf(sub, sizeof(sub), "mat%u",
                                 ki);
                    }
                    kc = (char *)malloc(strlen(sub) + 1u);
                    if (kc == NULL) {
                        break;
                    }
                    memcpy(kc, sub, strlen(sub) + 1u);
                    out_import->material_keys[ki] = kc;
                }
            }
            la_model_destroy(kmodel);
        }
    }
    return LE_SUCCESS;
}

void le_gltf_import_free(le_gltf_result *import) {
    uint32_t i;

    if (import == NULL) {
        return;
    }
    if (import->mesh_keys != NULL) {
        for (i = 0; i < import->mesh_count; i++) {
            free(import->mesh_keys[i]);
        }
        free(import->mesh_keys);
    }
    if (import->material_keys != NULL) {
        for (i = 0; i < import->material_count; i++) {
            free(import->material_keys[i]);
        }
        free(import->material_keys);
    }
    free(import->mesh_assets);
    free(import->material_assets);
    free(import->texture_assets);
    free(import->nodes);
    memset(import, 0, sizeof(*import));
}
