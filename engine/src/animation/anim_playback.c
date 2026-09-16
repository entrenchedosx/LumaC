/*
 * Animator playback (Phase 29): play/pause/resume/stop/seek/
 * speed/loop/crossfade + queries. All VM-independent (Lua
 * bindings are thin; future native/AOT uses the same le_*).
 *
 * Semantics:
 * - play(same clip, restart=0) continues time; restart!=0 or a
 *   different clip restarts at 0. Always sets playing.
 * - stop(reset=0) holds the last pose (pauses); stop(reset!=0)
 *   returns to bind pose + time 0 (paused).
 * - seek clamps into [0, duration] and invalidates cursors.
 * - Crossfade(from=current blended pose, to, duration): w =
 *   elapsed/duration; 0-duration = immediate. One shared clock:
 *   the fade samples the DESTINATION at the running time while
 *   blending snapshot->destination (joints and object pose
 *   alike); completion adopts the destination handle with the
 *   clock untouched (no reset, no re-sample, no snap).
 *   Interrupting a fade snapshots the CURRENT blended pose
 *   (continuity).
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "animation/animation_internal.h"
#include "animation/anim_math.h"

static struct le_animator_entry *le_pb_live(
    le_world *world, const le_object *object, le_result *code) {
    uint32_t slot;
    le_result c = LE_SUCCESS;
    int idx;

    if (code != NULL) {
        *code = LE_SUCCESS;
    }
    if (world == NULL || object == NULL) {
        if (code != NULL) {
            *code = LE_ERROR_INVALID_ARGUMENT;
        }
        return NULL;
    }
    if (!le_resolve_live(world, object, &slot, &c)) {
        if (code != NULL) {
            *code = c;
        }
        return NULL;
    }
    idx = le_anim_entry_index(world, slot);
    if (idx < 0) {
        if (code != NULL) {
            *code = LE_ERROR_INVALID_ARGUMENT;
        }
        return NULL;
    }
    return &world->animators[(uint32_t)idx];
}

static float le_pb_duration(le_world *world,
                            struct le_animator_entry *e) {
    uint32_t aslot;
    le_result code = LE_SUCCESS;
    const le_asset *h;

    if (world == NULL || world->engine == NULL || e == NULL) {
        return 0.0f;
    }
    /* While fading, the destination owns the clock. */
    h = (e->fading) ? &e->fade_clip : &e->clip;
    if (!le_resolve_asset_live(world->engine, h, &aslot,
                               &code)) {
        return 0.0f;
    }
    if (world->engine->assets[aslot].type !=
            LE_ASSET_ANIMATION_CLIP ||
        world->engine->assets[aslot].clip == NULL) {
        return 0.0f;
    }
    return world->engine->assets[aslot].clip->duration;
}

static int le_pb_same_clip(const le_asset *a, const le_asset *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    return a->index == b->index && a->generation == b->generation;
}

le_result le_anim_play(le_world *world, const le_object *object,
                       const le_asset *clip, int restart) {
    le_result code = LE_SUCCESS;
    struct le_animator_entry *e = le_pb_live(world, object, &code);

    if (e == NULL) {
        return (code == LE_SUCCESS) ? LE_ERROR_INVALID_ARGUMENT
                                    : code;
    }
    if (clip != NULL && clip->index != 0xFFFFFFFFu) {
        uint32_t aslot;
        le_result c2 = LE_SUCCESS;

        if (!le_resolve_asset_live(world->engine, clip, &aslot,
                                   &c2)) {
            return LE_ERROR_STALE_ASSET;
        }
        if (world->engine->assets[aslot].type !=
            LE_ASSET_ANIMATION_CLIP) {
            return LE_ERROR_WRONG_ASSET_TYPE;
        }
        if (world->engine->assets[aslot].state != LE_ASSET_READY) {
            return LE_ERROR_MISSING_ASSET;
        }
        if (!le_pb_same_clip(&e->clip, clip) || restart) {
            e->clip = *clip;
            e->time = 0.0f;
            e->pingpong_dir = 1;
            e->fading = 0;
        }
    }
    e->playing = 1;
    e->pose_dirty = 1;
    return LE_SUCCESS;
}

le_result le_anim_pause(le_world *world, const le_object *object) {
    le_result code = LE_SUCCESS;
    struct le_animator_entry *e = le_pb_live(world, object, &code);

    if (e == NULL) {
        return (code == LE_SUCCESS) ? LE_ERROR_INVALID_ARGUMENT
                                    : code;
    }
    e->playing = 0;
    return LE_SUCCESS;
}

le_result le_anim_resume(le_world *world, const le_object *object) {
    le_result code = LE_SUCCESS;
    struct le_animator_entry *e = le_pb_live(world, object, &code);

    if (e == NULL) {
        return (code == LE_SUCCESS) ? LE_ERROR_INVALID_ARGUMENT
                                    : code;
    }
    e->playing = 1;
    e->pose_dirty = 1;
    return LE_SUCCESS;
}

le_result le_anim_stop(le_world *world, const le_object *object,
                       int reset) {
    le_result code = LE_SUCCESS;
    struct le_animator_entry *e = le_pb_live(world, object, &code);

    if (e == NULL) {
        return (code == LE_SUCCESS) ? LE_ERROR_INVALID_ARGUMENT
                                    : code;
    }
    e->playing = 0;
    e->fading = 0;
    if (reset) {
        e->time = 0.0f;
        e->pingpong_dir = 1;
    }
    e->pose_dirty = 1;
    return LE_SUCCESS;
}

le_result le_anim_seek(le_world *world, const le_object *object,
                       float time) {
    le_result code = LE_SUCCESS;
    struct le_animator_entry *e = le_pb_live(world, object, &code);
    float dur;

    if (e == NULL) {
        return (code == LE_SUCCESS) ? LE_ERROR_INVALID_ARGUMENT
                                    : code;
    }
    if (!isfinite(time)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    dur = le_pb_duration(world, e);
    if (time < 0.0f) {
        time = 0.0f;
    }
    if (dur > 0.0f && time > dur) {
        time = dur;
    }
    e->time = time;
    e->pose_dirty = 1;
    /* Cursor invalidation: cursors live on clip tracks (shared);
     * seeks go through binary search next sample (the cursor
     * fast path self-corrects). Mark via pose_dirty; the sampler
     * handles arbitrary jumps correctly. */
    return LE_SUCCESS;
}

le_result le_anim_set_speed(le_world *world, const le_object *object,
                            float speed) {
    le_result code = LE_SUCCESS;
    struct le_animator_entry *e = le_pb_live(world, object, &code);

    if (e == NULL) {
        return (code == LE_SUCCESS) ? LE_ERROR_INVALID_ARGUMENT
                                    : code;
    }
    if (!isfinite(speed) || speed < 0.0f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    e->speed = speed;
    return LE_SUCCESS;
}

le_result le_anim_set_loop(le_world *world, const le_object *object,
                           le_anim_loop_mode loop) {
    le_result code = LE_SUCCESS;
    struct le_animator_entry *e = le_pb_live(world, object, &code);

    if (e == NULL) {
        return (code == LE_SUCCESS) ? LE_ERROR_INVALID_ARGUMENT
                                    : code;
    }
    if (loop != LE_ANIM_ONCE && loop != LE_ANIM_LOOP &&
        loop != LE_ANIM_PING_PONG) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    e->loop_mode = loop;
    return LE_SUCCESS;
}

/* Snapshot the animator's CURRENT evaluated local pose into the
 * fade_from scratch (interrupt continuity). Requires pose
 * scratch sized for the skeleton. The object pose rides
 * dedicated fade_obj_* fields (the sampler writes OBJECT tracks
 * to e->obj_*, never into the joint arrays). */
static int le_pb_snapshot_current(le_world *world,
                                  struct le_animator_entry *e,
                                  uint32_t joints) {
    uint32_t need = joints + 1u;
    uint32_t i;

    if (e->pose_cap < need || e->local_t == NULL) {
        return 0;
    }
    for (i = 0; i < joints; i++) {
        e->fade_from_t[i * 3u + 0] = e->local_t[i * 3u + 0];
        e->fade_from_t[i * 3u + 1] = e->local_t[i * 3u + 1];
        e->fade_from_t[i * 3u + 2] = e->local_t[i * 3u + 2];
        e->fade_from_r[i * 4u + 0] = e->local_r[i * 4u + 0];
        e->fade_from_r[i * 4u + 1] = e->local_r[i * 4u + 1];
        e->fade_from_r[i * 4u + 2] = e->local_r[i * 4u + 2];
        e->fade_from_r[i * 4u + 3] = e->local_r[i * 4u + 3];
        e->fade_from_s[i * 3u + 0] = e->local_s[i * 3u + 0];
        e->fade_from_s[i * 3u + 1] = e->local_s[i * 3u + 1];
        e->fade_from_s[i * 3u + 2] = e->local_s[i * 3u + 2];
    }
    memcpy(e->fade_obj_t, e->obj_t, sizeof(e->fade_obj_t));
    memcpy(e->fade_obj_r, e->obj_r, sizeof(e->fade_obj_r));
    memcpy(e->fade_obj_s, e->obj_s, sizeof(e->fade_obj_s));
    return 1;
}

static uint32_t le_pb_skel_joints(le_world *world,
                                  struct le_animator_entry *e) {
    uint32_t aslot;
    le_result code = LE_SUCCESS;

    if (world == NULL || world->engine == NULL || e == NULL) {
        return 0;
    }
    if (!le_resolve_asset_live(world->engine, &e->skeleton, &aslot,
                               &code)) {
        return 0;
    }
    if (world->engine->assets[aslot].type != LE_ASSET_SKELETON ||
        world->engine->assets[aslot].skeleton == NULL) {
        return 0;
    }
    return world->engine->assets[aslot].skeleton->joint_count;
}

le_result le_anim_crossfade(le_world *world, const le_object *object,
                            const le_asset *clip, float duration) {
    le_result code = LE_SUCCESS;
    struct le_animator_entry *e = le_pb_live(world, object, &code);
    uint32_t joints;

    if (e == NULL) {
        return (code == LE_SUCCESS) ? LE_ERROR_INVALID_ARGUMENT
                                    : code;
    }
    if (clip == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!isfinite(duration) || duration < 0.0f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    {
        uint32_t aslot;
        le_result c2 = LE_SUCCESS;

        if (!le_resolve_asset_live(world->engine, clip, &aslot,
                                   &c2)) {
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
    joints = le_pb_skel_joints(world, e);
    if (duration <= 0.0f) {
        /* Immediate transition (no divide-by-zero). */
        e->clip = *clip;
        e->time = 0.0f;
        e->pingpong_dir = 1;
        e->fading = 0;
        e->playing = 1;
        e->pose_dirty = 1;
        return LE_SUCCESS;
    }
    /* Snapshot current blended pose for continuity (interrupt-
     * safe: fading or not, the snapshot is what's on screen).
     * Fresh animators may never have evaluated: grow scratch +
     * evaluate the static pose first so the snapshot is exact.
     * Skeletonless (object-only) animators fade the single
     * object slot — same path with joints == 0. */
    {
        extern le_result le_anim_grow_pose_for(
            le_world *world, struct le_animator_entry *e,
            uint32_t joints);
        extern uint32_t le_anim_evaluate_entry(
            le_world *world, struct le_animator_entry *e);

        if (le_anim_grow_pose_for(world, e, joints) !=
            LE_SUCCESS) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
        if (e->pose_version == 0 || e->pose_dirty) {
            le_anim_evaluate_entry(world, e);
        }
        if (!le_pb_snapshot_current(world, e, joints)) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
    }
    e->fade_clip = *clip;
    e->fade_elapsed = 0.0f;
    e->fade_duration = duration;
    e->fading = 1;
    e->playing = 1;
    e->pose_dirty = 1;
    return LE_SUCCESS;
}

int le_anim_is_playing(const le_world *world,
                       const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    idx = le_anim_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return 0;
    }
    return world->animators[(uint32_t)idx].playing ? 1 : 0;
}

float le_anim_get_time(const le_world *world,
                       const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (world == NULL || object == NULL) {
        return 0.0f;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0.0f;
    }
    idx = le_anim_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return 0.0f;
    }
    return world->animators[(uint32_t)idx].time;
}

float le_anim_get_duration(const le_world *world,
                           const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (world == NULL || object == NULL) {
        return 0.0f;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0.0f;
    }
    idx = le_anim_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return 0.0f;
    }
    return le_pb_duration((le_world *)world,
                          &((le_world *)world)
                               ->animators[(uint32_t)idx]);
}
