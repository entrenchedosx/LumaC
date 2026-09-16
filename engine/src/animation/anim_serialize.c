/*
 * Animator scene capture/apply + palette query (Phase 29):
 * authoring/playback state only (never evaluated poses or skin
 * palettes). Follows the script/physics record pattern
 * (persistent asset IDs + transactional commit).
 *
 * le_anim_get_palette (struct-blind for sync.c): borrows the
 * animator's evaluated skin palette for one slot (NULL/0 when no
 * animator or no evaluated pose). Lazily evaluates static poses
 * so extraction-before-first-step still skins. Lifetime: borrows
 * animator scratch (stable across frames; regrown only when the
 * skeleton joint count changes). The renderer copies at submit,
 * so the borrow never escapes the frame.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "animation/animation_internal.h"

/* Evaluate one entry's static pose on demand (no time advance;
 * used by the palette getter when no frame has run yet).
 * Implemented in anim_step.c (non-static entry evaluator). */

void le_anim_capture_for_record(le_world *world, uint32_t slot,
                                le_scene_object *rec) {
    int idx;

    if (world == NULL || rec == NULL || slot >= world->capacity) {
        return;
    }
    rec->has_animator = 0;
    memset(&rec->skeleton_id, 0, sizeof(rec->skeleton_id));
    memset(&rec->clip_id, 0, sizeof(rec->clip_id));
    rec->animator_autoplay = 0;
    rec->animator_loop = LE_ANIM_ONCE;
    rec->animator_speed = 1.0f;
    rec->animator_start_time = 0.0f;
    idx = le_anim_entry_index(world, slot);
    if (idx < 0) {
        return;
    }
    {
        const struct le_animator_entry *e =
            &world->animators[(uint32_t)idx];
        uint32_t aslot;
        le_result code = LE_SUCCESS;

        /* Persist registry IDs (authoritative identity survives
         * save/load; handles do not). Unresolvable assets persist
         * as nil IDs and re-resolve as MISSING at instantiate. */
        if (world->engine != NULL) {
            if (le_resolve_asset_live(world->engine, &e->skeleton,
                                      &aslot, &code) &&
                world->engine->assets[aslot].type ==
                    LE_ASSET_SKELETON) {
                rec->skeleton_id =
                    world->engine->assets[aslot].id;
            }
            code = LE_SUCCESS;
            if (le_resolve_asset_live(world->engine, &e->clip,
                                      &aslot, &code) &&
                world->engine->assets[aslot].type ==
                    LE_ASSET_ANIMATION_CLIP) {
                rec->clip_id = world->engine->assets[aslot].id;
            }
        }
        rec->has_animator = 1;
        rec->animator_autoplay = e->playing ? 1 : 0;
        rec->animator_loop = (int)e->loop_mode;
        rec->animator_speed = e->speed;
        rec->animator_start_time = e->time;
    }
}

/* Validate one record's animator (PASS1 shape; INVALID_ARGUMENT
 * on any malformed value — transactional load). */
le_result le_anim_validate_record(const le_scene_object *rec) {
    if (rec == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!rec->has_animator) {
        return LE_SUCCESS;
    }
    if (rec->animator_loop != LE_ANIM_ONCE &&
        rec->animator_loop != LE_ANIM_LOOP &&
        rec->animator_loop != LE_ANIM_PING_PONG) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!(rec->animator_speed == rec->animator_speed) ||
        rec->animator_speed < 0.0f ||
        rec->animator_speed > 1e9f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!(rec->animator_start_time ==
          rec->animator_start_time) ||
        rec->animator_start_time < 0.0f ||
        rec->animator_start_time > 1e9f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    return LE_SUCCESS;
}

le_result le_anim_apply_record(le_world *world, const le_object *obj,
                               const le_scene_object *rec) {
    le_engine *engine;
    le_animator_desc d;
    le_asset skeleton = LE_ASSET_INVALID;
    le_asset clip = LE_ASSET_INVALID;
    int have_skeleton = 0;
    int have_clip = 0;

    if (world == NULL || obj == NULL || rec == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!rec->has_animator) {
        return LE_SUCCESS;
    }
    engine = world->engine;
    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (le_anim_validate_record(rec) != LE_SUCCESS) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Nil IDs mean "unset" (object-only animator or skeletonless
     * clip); non-nil IDs must resolve READY (transactional: the
     * world is untouched on failure — add runs after resolves). */
    if (!(rec->skeleton_id.hi == 0 && rec->skeleton_id.lo == 0)) {
        if (!le_asset_find_by_id(engine, &rec->skeleton_id,
                                 &skeleton)) {
            return LE_ERROR_MISSING_ASSET;
        }
        have_skeleton = 1;
    }
    if (!(rec->clip_id.hi == 0 && rec->clip_id.lo == 0)) {
        if (!le_asset_find_by_id(engine, &rec->clip_id, &clip)) {
            return LE_ERROR_MISSING_ASSET;
        }
        have_clip = 1;
    }
    memset(&d, 0, sizeof(d));
    d.skeleton = have_skeleton ? skeleton : LE_ASSET_INVALID;
    d.clip = have_clip ? clip : LE_ASSET_INVALID;
    d.autoplay = rec->animator_autoplay ? 1 : 0;
    d.loop_mode = (le_anim_loop_mode)rec->animator_loop;
    d.speed = rec->animator_speed;
    d.start_time = rec->animator_start_time;
    return le_object_add_animator(world, obj, &d);
}

/* Borrow one slot's evaluated skin palette (struct-blind for
 * sync.c's submit/extraction paths). Returns 1 with *out_palette
 * / *out_joints set when an animator with an evaluated pose
 * exists; 0 (outs zeroed) otherwise. Static poses evaluate on
 * demand so extraction-before-first-step still skins. */
int le_anim_get_palette(le_world *world, uint32_t slot,
                        const float (**out_palette)[16],
                        uint32_t *out_joints) {
    int idx;

    if (out_palette != NULL) {
        *out_palette = NULL;
    }
    if (out_joints != NULL) {
        *out_joints = 0;
    }
    if (world == NULL || slot >= world->capacity ||
        out_palette == NULL || out_joints == NULL) {
        return 0;
    }
    if (!world->slots[slot].alive) {
        return 0;
    }
    idx = le_anim_entry_index(world, slot);
    if (idx < 0) {
        return 0;
    }
    {
        struct le_animator_entry *e =
            &world->animators[(uint32_t)idx];

        if (e->pose_version == 0 || e->pose_dirty) {
            /* No evaluated pose yet (or invalidated): evaluate
             * the static pose now (no time advance). */
            le_anim_evaluate_entry(world, e);
        }
        if (e->pose_version == 0 || e->pose_joints == 0 ||
            e->skin_m == NULL) {
            return 0;
        }
        *out_palette = (const float (*)[16])e->skin_m;
        *out_joints = e->pose_joints;
        return 1;
    }
}

/* Count animator references to one asset slot (struct-blind
 * for asset.c's refcount: skeleton + clip + in-flight fade
 * destination, generation-checked like every other handle
 * user). Skeleton/clip unload refuses while referenced
 * (LE_ERROR_ASSET_IN_USE — same discipline as script
 * assets). */
uint32_t le_anim_refcount_slot(const le_world *world,
                               uint32_t asset_slot,
                               uint32_t generation) {
    uint32_t n = 0;
    uint32_t i;

    if (world == NULL || world->animators == NULL) {
        return 0;
    }
    for (i = 0; i < world->animator_count; i++) {
        const struct le_animator_entry *e =
            &world->animators[i];
        const le_asset *handles[3];

        handles[0] = &e->skeleton;
        handles[1] = &e->clip;
        handles[2] = e->fading ? &e->fade_clip : NULL;
        {
            uint32_t k;

            for (k = 0; k < 3; k++) {
                if (handles[k] != NULL &&
                    handles[k]->index == asset_slot &&
                    handles[k]->generation == generation) {
                    n++;
                }
            }
        }
    }
    return n;
}

/* Submit skin palettes for animated renderables. The palette
 * rides the submit item itself (renderer copies at submit), so
 * this hook only guarantees poses are evaluated before the
 * extraction pass reads them (dirty-gated, no re-evaluation).
 * Called from the submit path once per frame. */
void le_anim_submit_palettes(le_world *world) {
    uint32_t i;

    if (world == NULL || world->animator_count == 0 ||
        world->animators == NULL) {
        return;
    }
    for (i = 0; i < world->animator_count; i++) {
        struct le_animator_entry *e = &world->animators[i];

        if (e->slot >= world->capacity ||
            !world->slots[e->slot].alive) {
            continue;
        }
        if (e->pose_version == 0 || e->pose_dirty) {
            le_anim_evaluate_entry(world, e);
        }
    }
}
