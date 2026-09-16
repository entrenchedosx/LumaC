/*
 * Animator component (Phase 29): dense runtime entries with the
 * standard swap-remove discipline, playback (play/pause/resume/
 * stop/seek/speed/loop/crossfade), per-frame advance + pose
 * evaluation + animated transform writes.
 *
 * Ownership policy (explicit, tested):
 * - OBJECT tracks write the owner's LOCAL transform — allowed
 *   with no/static/kinematic bodies; REJECTED at add-time with a
 *   dynamic body (LE_ERROR_INVALID_HIERARCHY).
 * - JOINT tracks evaluate under the owner's frame (skeleton pose
 *   -> skin palette) and never touch the owner transform — valid
 *   under dynamic roots (physics drives root, animation drives
 *   joints).
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "animation/animation_internal.h"
#include "animation/anim_math.h"

int le_anim_entry_index(le_world *world, uint32_t slot) {
    uint32_t idx;

    if (world == NULL || slot >= world->capacity) {
        return -1;
    }
    if (!(world->slots[slot].present & LE_PRESENT_ANIMATOR)) {
        return -1;
    }
    idx = (uint32_t)world->slots[slot].animator_index;
    if (idx >= world->animator_count ||
        world->animators[idx].slot != slot) {
        return -1;
    }
    return (int)idx;
}

static le_result le_anim_grow(le_world *world) {
    uint32_t grown;
    struct le_animator_entry *fresh;

    if (world->animator_count < world->animator_capacity) {
        return LE_SUCCESS;
    }
    grown = (world->animator_capacity == 0)
                ? 16u
                : world->animator_capacity * 2u;
    if (grown > 0x00FFFFFFu) {
        return LE_ERROR_OVERFLOW;
    }
    fresh = (struct le_animator_entry *)realloc(
        world->animators, (size_t)grown * sizeof(*fresh));
    if (fresh == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    world->animators = fresh;
    world->animator_capacity = grown;
    return LE_SUCCESS;
}

/* Resolve a live entry (validates slot + generation drift like
 * every component). */
static struct le_animator_entry *le_anim_live(le_world *world,
                                              const le_object *object,
                                              uint32_t *out_slot,
                                              le_result *out_code) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (out_code != NULL) {
        *out_code = LE_SUCCESS;
    }
    if (world == NULL || object == NULL) {
        if (out_code != NULL) {
            *out_code = LE_ERROR_INVALID_ARGUMENT;
        }
        return NULL;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        if (out_code != NULL) {
            *out_code = code;
        }
        return NULL;
    }
    if (out_slot != NULL) {
        *out_slot = slot;
    }
    idx = le_anim_entry_index(world, slot);
    if (idx < 0) {
        if (out_code != NULL) {
            *out_code = LE_ERROR_MISSING_COMPONENT;
        }
        return NULL;
    }
    return &world->animators[(uint32_t)idx];
}

/* Resolve a clip payload (generation-checked handle). Returns
 * NULL for unset/invalid (caller holds pose). */
static struct le_clip_data *le_anim_clip(
    le_world *world, const struct le_animator_entry *e,
    const le_asset *which) {
    uint32_t aslot;
    le_result code = LE_SUCCESS;
    const le_asset *h = (which != NULL) ? which : &e->clip;

    if (world == NULL || world->engine == NULL || e == NULL) {
        return NULL;
    }
    if (!le_resolve_asset_live(world->engine, h, &aslot, &code)) {
        return NULL;
    }
    if (world->engine->assets[aslot].type !=
            LE_ASSET_ANIMATION_CLIP ||
        world->engine->assets[aslot].state != LE_ASSET_READY) {
        return NULL;
    }
    return world->engine->assets[aslot].clip;
}

static void le_anim_free_entry_arrays(struct le_animator_entry *e) {
    free(e->local_t);
    free(e->local_r);
    free(e->local_s);
    free(e->global_m);
    free(e->skin_m);
    free(e->fade_from_t);
    free(e->fade_from_r);
    free(e->fade_from_s);
    memset(e, 0, sizeof(*e));
}

/* Validate skeleton+clip handles (live READY, right type). */
static le_result le_anim_check_assets(
    le_world *world, const le_asset *skeleton,
    const le_asset *clip) {
    if (skeleton != NULL && skeleton->index != 0xFFFFFFFFu) {
        uint32_t aslot;
        le_result code = LE_SUCCESS;

        if (!le_resolve_asset_live(world->engine, skeleton, &aslot,
                                   &code)) {
            return LE_ERROR_STALE_ASSET;
        }
        if (world->engine->assets[aslot].type != LE_ASSET_SKELETON) {
            return LE_ERROR_WRONG_ASSET_TYPE;
        }
        if (world->engine->assets[aslot].state != LE_ASSET_READY) {
            return LE_ERROR_MISSING_ASSET;
        }
    }
    if (clip != NULL && clip->index != 0xFFFFFFFFu) {
        uint32_t aslot;
        le_result code = LE_SUCCESS;

        if (!le_resolve_asset_live(world->engine, clip, &aslot,
                                   &code)) {
            return LE_ERROR_STALE_ASSET;
        }
        if (world->engine->assets[aslot].type !=
            LE_ASSET_ANIMATION_CLIP) {
            return LE_ERROR_WRONG_ASSET_TYPE;
        }
        if (world->engine->assets[aslot].state != LE_ASSET_READY) {
            return LE_ERROR_MISSING_ASSET;
        }
    }
    return LE_SUCCESS;
}

/* Does a clip carry OBJECT-target tracks? (drives the dynamic-
 * body conflict policy). */
static int le_clip_has_object_tracks(le_world *world,
                                     const le_asset *clip) {
    struct le_clip_data *c;
    struct le_animator_entry tmp;

    if (clip == NULL || clip->index == 0xFFFFFFFFu) {
        return 0;
    }
    memset(&tmp, 0, sizeof(tmp));
    tmp.clip = *clip;
    c = le_anim_clip(world, &tmp, NULL);
    if (c == NULL) {
        return 0;
    }
    {
        uint32_t i;

        for (i = 0; i < c->track_count; i++) {
            if (c->tracks[i].target_kind == LE_ANIM_TARGET_OBJECT) {
                return 1;
            }
        }
    }
    return 0;
}

le_result le_object_add_animator(le_world *world,
                                 const le_object *object,
                                 const le_animator_desc *desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_result rc;
    struct le_animator_entry *e;
    int existing;

    if (world == NULL || object == NULL || desc == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->loop_mode != LE_ANIM_ONCE &&
        desc->loop_mode != LE_ANIM_LOOP &&
        desc->loop_mode != LE_ANIM_PING_PONG) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!isfinite(desc->speed) || desc->speed < 0.0f ||
        !isfinite(desc->start_time)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    rc = le_anim_check_assets(world,
                              (desc->skeleton.index != 0xFFFFFFFFu)
                                  ? &desc->skeleton
                                  : NULL,
                              (desc->clip.index != 0xFFFFFFFFu)
                                  ? &desc->clip
                                  : NULL);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    /* Dynamic-body conflict: OBJECT tracks would fight physics. */
    if (desc->clip.index != 0xFFFFFFFFu &&
        le_clip_has_object_tracks(world, &desc->clip) &&
        (world->slots[slot].present & LE_PRESENT_RIGID_BODY) !=
            0u) {
        int bi = -1;

        if (world->slots[slot].body_index != LE_NO_LINK) {
            /* Dynamic check needs the physics entry type; ask
             * via presence + a live velocity probe is overkill —
             * read the body entry through the physics helper
             * would couple modules. Instead: only DYNAMIC bodies
             * integrate; static/kinematic are transform-driven.
             * The physics module exposes no type query, so check
             * the entry via the world's physics state... simplest
             * honest rule: object tracks + ANY rigid body where
             * the body is dynamic. Reuse the hierarchy rule's
             * spirit: dynamics are roots. A dynamic body is one
             * whose slot is a root AND has a body. Static/
             * kinematic may parent. Approximate precisely: query
             * the body desc through the PUBLIC API. */
            le_rigid_body_desc bd;

            memset(&bd, 0, sizeof(bd));
            if (le_object_get_rigid_body(world, object, &bd) &&
                bd.type == LE_BODY_DYNAMIC) {
                bi = 1;
            }
        }
        if (bi == 1) {
            return LE_ERROR_INVALID_HIERARCHY;
        }
    }
    existing = le_anim_entry_index(world, slot);
    if (existing >= 0) {
        /* Replace in place (keep pose scratch; reset playback). */
        e = &world->animators[(uint32_t)existing];
        e->skeleton = desc->skeleton;
        e->clip = desc->clip;
        e->loop_mode = desc->loop_mode;
        e->speed = desc->speed;
        e->time = (desc->start_time < 0.0f) ? 0.0f
                                            : desc->start_time;
        e->playing = desc->autoplay ? 1 : 0;
        e->pingpong_dir = 1;
        e->fading = 0;
        e->pose_dirty = 1;
        return LE_SUCCESS;
    }
    rc = le_anim_grow(world);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    e = &world->animators[world->animator_count];
    memset(e, 0, sizeof(*e));
    e->slot = slot;
    e->skeleton = desc->skeleton;
    e->clip = desc->clip;
    e->loop_mode = desc->loop_mode;
    e->speed = desc->speed;
    e->time = (desc->start_time < 0.0f) ? 0.0f
                                        : desc->start_time;
    e->playing = desc->autoplay ? 1 : 0;
    e->pingpong_dir = 1;
    e->weight = 1.0f;
    e->obj_r[3] = 1.0f;
    e->fade_obj_r[3] = 1.0f;
    e->obj_s[0] = e->obj_s[1] = e->obj_s[2] = 1.0f;
    e->fade_obj_s[0] = e->fade_obj_s[1] = e->fade_obj_s[2] = 1.0f;
    e->pose_dirty = 1;
    world->slots[slot].animator_index =
        (int32_t)world->animator_count;
    world->slots[slot].present |= LE_PRESENT_ANIMATOR;
    world->animator_count++;
    return LE_SUCCESS;
}

le_result le_object_remove_animator(le_world *world,
                                    const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    idx = le_anim_entry_index(world, slot);
    if (idx < 0) {
        return LE_SUCCESS;
    }
    {
        uint32_t last = world->animator_count - 1u;

        le_anim_free_entry_arrays(&world->animators[(uint32_t)idx]);
        if ((uint32_t)idx != last) {
            world->animators[(uint32_t)idx] =
                world->animators[last];
            world->slots[world->animators[(uint32_t)idx].slot]
                .animator_index = (int32_t)idx;
        }
        memset(&world->animators[last], 0,
               sizeof(world->animators[last]));
        world->animator_count--;
    }
    world->slots[slot].animator_index = LE_NO_LINK;
    world->slots[slot].present &= ~LE_PRESENT_ANIMATOR;
    return LE_SUCCESS;
}

int le_object_get_animator(const le_world *world,
                           const le_object *object,
                           le_animator_desc *out_desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (out_desc != NULL) {
        memset(out_desc, 0, sizeof(*out_desc));
    }
    if (world == NULL || object == NULL || out_desc == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    idx = le_anim_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return 0;
    }
    {
        const struct le_animator_entry *e =
            &world->animators[(uint32_t)idx];

        out_desc->skeleton = e->skeleton;
        out_desc->clip = e->clip;
        out_desc->autoplay = e->playing;
        out_desc->loop_mode = e->loop_mode;
        out_desc->speed = e->speed;
        out_desc->start_time = e->time;
        return 1;
    }
}

void le_anim_remove_slot_animator(le_world *world, uint32_t slot) {
    int idx;

    if (world == NULL || slot >= world->capacity) {
        return;
    }
    idx = le_anim_entry_index(world, slot);
    if (idx < 0) {
        return;
    }
    {
        uint32_t last = world->animator_count - 1u;

        le_anim_free_entry_arrays(&world->animators[(uint32_t)idx]);
        if ((uint32_t)idx != last) {
            world->animators[(uint32_t)idx] =
                world->animators[last];
            world->slots[world->animators[(uint32_t)idx].slot]
                .animator_index = (int32_t)idx;
        }
        memset(&world->animators[last], 0,
               sizeof(world->animators[last]));
        if (world->animator_count > 0) {
            world->animator_count--;
        }
    }
}

void le_anim_destroy_world(le_world *world) {
    uint32_t i;

    if (world == NULL) {
        return;
    }
    if (world->animators != NULL) {
        for (i = 0; i < world->animator_count; i++) {
            le_anim_free_entry_arrays(&world->animators[i]);
        }
        free(world->animators);
        world->animators = NULL;
    }
    world->animator_count = 0;
    world->animator_capacity = 0;
}
