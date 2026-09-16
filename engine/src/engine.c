/*
 * Luma Engine entry point: le_engine_create / le_engine_destroy /
 * le_engine_get_renderer. The engine owns its world list AND its
 * asset registry; worlds own everything else.
 */

#include <stdlib.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "luma_assets/luma_assets.h"
#include "input/input_internal.h"
#include "time/time_internal.h"

/* Phase 29: animation backing free (defined in
 * src/animation/anim_asset.c; struct-blind). */
void le_anim_free_slot_backing(struct le_skeleton_data *skeleton,
                               struct le_clip_data *clip);

le_result le_engine_create(const le_engine_desc *desc,
                           le_engine **out_engine) {
    le_engine *engine;

    if (out_engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    *out_engine = NULL;
    engine = (le_engine *)calloc(1, sizeof(le_engine));
    if (engine == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    if (desc != NULL) {
        engine->renderer = desc->renderer;
    }
    engine->worlds = NULL;
    /* Phase 27: engine-owned input + time state. Either may fail
     * (OOM) — unwind cleanly, registry untouched. */
    engine->input = le_input_create();
    if (engine->input == NULL) {
        free(engine);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    engine->time = le_time_create();
    if (engine->time == NULL) {
        le_input_destroy(engine->input);
        engine->input = NULL;
        free(engine);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    engine->quit_requested = 0;
    engine->has_focus = 1;
    engine->minimized = 0;
    engine->focus_window = NULL;
    engine->cursor_mode = LE_CURSOR_NORMAL;
    engine->windows = NULL;
    engine->window_count = 0;
    engine->window_cap = 0;
    *out_engine = engine;
    return LE_SUCCESS;
}

void le_engine_destroy(le_engine *engine) {
    uint32_t i;

    if (engine == NULL) {
        return;
    }
    /* Dependents first: worlds (renderables hold asset HANDLES, so
     * no world teardown touches backing resources), then the asset
     * registry (backing destroyed via public renderer/LumaC APIs),
     * then the glTF bridge manager (adopted materials borrow its
     * cached texture views — it MUST outlive the registry), then
     * the script runtime (backend states die AFTER every world
     * released its instance refs and every script chunk ref is
     * dropped), then the engine itself. */
    while (engine->worlds != NULL) {
        le_world_destroy(engine->worlds);
    }
    if (engine->assets != NULL) {
        for (i = 0; i < engine->asset_capacity; i++) {
            le_asset_slot *s = &engine->assets[i];

            if (!s->alive) {
                continue;
            }
            if (s->type == LE_ASSET_MESH && s->mesh != NULL) {
                lr_mesh_destroy(s->mesh);
            } else if (s->type == LE_ASSET_MATERIAL &&
                       s->material != NULL) {
                lr_material_destroy(s->material);
            } else if (s->type == LE_ASSET_TEXTURE) {
                lc_image_view_destroy(s->texture_view);
                lc_image_destroy(s->texture_image);
            } else if (s->type == LE_ASSET_SCRIPT) {
                /* Chunk refs die with the backend state; drop the
                 * ref BEFORE runtime destroy (unref-after-close
                 * is UB). */
                if (s->script_chunk_ref >= 0) {
                    le_script_release_chunk_token(
                        engine, s->script_chunk_ref);
                    s->script_chunk_ref = -2; /* LE_SCRIPT_NOREF */
                }
                free(s->script_source);
                s->script_source = NULL;
                s->script_size = 0;
            } else if (s->type == LE_ASSET_SKELETON ||
                       s->type == LE_ASSET_ANIMATION_CLIP) {
                /* Phase 29: pure CPU payloads (no renderer
                 * backing, no runtime ordering constraint). */
                le_anim_free_slot_backing(s->skeleton, s->clip);
                s->skeleton = NULL;
                s->clip = NULL;
            }
            free(s->source);
            free(s->scene_objects);
        }
        free(engine->assets);
        engine->assets = NULL;
    }
    la_asset_manager_destroy(engine->gltf_manager);
    engine->gltf_manager = NULL;
    le_script_runtime_destroy(engine);
    le_input_destroy(engine->input);
    engine->input = NULL;
    le_time_destroy(engine->time);
    engine->time = NULL;
    free(engine->windows);
    engine->windows = NULL;
    engine->window_count = 0;
    engine->window_cap = 0;
    free(engine);
}

lr_renderer *le_engine_get_renderer(const le_engine *engine) {
    if (engine == NULL) {
        return NULL;
    }
    return engine->renderer;
}
