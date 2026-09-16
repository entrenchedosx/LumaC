/*
 * Animator frame advance (Phase 29): variable-dt visual path.
 *
 * Engine order (reconciled with Phase 27/28 lifecycle):
 *   le_world_update(dt)/le_world_simulate_engine(dt):
 *     matrices -> PASS1 starts -> PASS2 fixed (fixed_update +
 *     physics) -> PASS3 update scripts -> ANIM visual advance
 *     (this file: scaled dt, enabled animators only) -> matrices.
 * Extraction (sync.c) then reads evaluated poses + uploads skin
 * palettes (no re-evaluation: pose_version gating).
 *
 * Paused worlds never reach here (lifecycle short-circuits);
 * Time.scale 0 yields dt 0 (no advance; same pose re-uploads
 * only when dirty). Disabled objects/components hold time and
 * resume on re-enable (tested).
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "animation/animation_internal.h"
#include "animation/anim_math.h"

/* Wrap/clamp time per loop mode. Returns 1 when ONCE finished
 * (caller stops playing). */
static int le_advance_time(struct le_animator_entry *e, float dur,
                           float dt) {
    float nt;

    if (dur <= 0.0f) {
        return 1;
    }
    nt = e->time + dt * e->speed * (float)e->pingpong_dir;
    if (e->loop_mode == LE_ANIM_LOOP) {
        /* Bounded wrap (no drift: fmod-style subtract). */
        while (nt >= dur) {
            nt -= dur;
        }
        while (nt < 0.0f) {
            nt += dur;
        }
        e->time = nt;
        return 0;
    }
    if (e->loop_mode == LE_ANIM_PING_PONG) {
        /* Alternate direction each pass (bounded iteration). */
        int guard = 0;

        while ((nt >= dur || nt < 0.0f) && guard < 8) {
            guard++;
            if (nt >= dur) {
                nt = dur - (nt - dur);
                e->pingpong_dir = -1;
            } else {
                nt = -nt;
                e->pingpong_dir = 1;
            }
        }
        if (nt < 0.0f) {
            nt = 0.0f;
        }
        if (nt > dur) {
            nt = dur;
        }
        e->time = nt;
        return 0;
    }
    /* ONCE: clamp + finish. */
    if (nt >= dur) {
        e->time = dur;
        return 1;
    }
    if (nt < 0.0f) {
        e->time = 0.0f;
        return 1;
    }
    e->time = nt;
    return 0;
}

/* Seed local pose from the skeleton bind pose. */
static void le_seed_bind(const struct le_skeleton_data *skel,
                         float (*lt)[3], float (*lr)[4],
                         float (*ls)[3], uint32_t joints) {
    uint32_t i;

    for (i = 0; i < joints && i < skel->joint_count; i++) {
        memcpy(lt[i], skel->joints[i].bind_t, 3u * sizeof(float));
        memcpy(lr[i], skel->joints[i].bind_r, 4u * sizeof(float));
        memcpy(ls[i], skel->joints[i].bind_s, 3u * sizeof(float));
    }
}

/* Evaluate one animator (sample + fade-blend + globals + skin +
 * object transform write). Returns tracks sampled (stats).
 * Non-static: the palette getter evaluates static poses on
 * demand (no time advance). */
uint32_t le_anim_evaluate_entry(le_world *world,
                                struct le_animator_entry *e) {
    struct le_skeleton_data *skel = NULL;
    struct le_clip_data *clip = NULL;
    uint32_t joints = 0;
    uint32_t sampled = 0;
    uint32_t aslot;
    le_result code = LE_SUCCESS;

    if (world == NULL || world->engine == NULL || e == NULL) {
        return 0;
    }
    /* Skeleton (optional: object-only clips animate without one). */
    if (e->skeleton.index != 0xFFFFFFFFu &&
        le_resolve_asset_live(world->engine, &e->skeleton, &aslot,
                              &code) &&
        world->engine->assets[aslot].type == LE_ASSET_SKELETON &&
        world->engine->assets[aslot].state == LE_ASSET_READY) {
        skel = world->engine->assets[aslot].skeleton;
    }
    if (skel != NULL) {
        joints = skel->joint_count;
    }
    if (le_resolve_asset_live(world->engine, &e->clip, &aslot,
                              &code) &&
        world->engine->assets[aslot].type ==
            LE_ASSET_ANIMATION_CLIP &&
        world->engine->assets[aslot].state == LE_ASSET_READY) {
        clip = world->engine->assets[aslot].clip;
    }
    /* Fade destination (resolved when fading; a destination
     * that died mid-fade aborts the fade below). */
    {
        struct le_clip_data *dest = NULL;

        if (e->fading) {
            uint32_t as2;
            le_result c2 = LE_SUCCESS;

            if (le_resolve_asset_live(world->engine,
                                      &e->fade_clip, &as2,
                                      &c2) &&
                world->engine->assets[as2].type ==
                    LE_ASSET_ANIMATION_CLIP &&
                world->engine->assets[as2].state ==
                    LE_ASSET_READY) {
                dest = world->engine->assets[as2].clip;
            }
            if (dest == NULL) {
                e->fading = 0; /* destination died: hold */
            } else {
                /* Shared clock: the fade samples the
                 * DESTINATION at the running time (the
                 * snapshot holds the source side). */
                clip = dest;
            }
        }
    }
    /* Ensure scratch (skeletonless object clips need 1 slot). */
    if (joints == 0 && clip == NULL) {
        return 0; /* nothing to evaluate */
    }
    {
        extern le_result le_anim_grow_pose_for(
            le_world *world, struct le_animator_entry *e,
            uint32_t joints);

        if (le_anim_grow_pose_for(world, e, joints) !=
            LE_SUCCESS) {
            return 0; /* OOM: hold pose */
        }
    }
    {
        float(*lt)[3] = (float(*)[3])e->local_t;
        float(*lr)[4] = (float(*)[4])e->local_r;
        float(*ls)[3] = (float(*)[3])e->local_s;
        uint32_t need = joints + 1u;
        uint32_t i;

        if (skel != NULL) {
            le_seed_bind(skel, lt, lr, ls, joints);
        } else {
            for (i = 0; i < need; i++) {
                lt[i][0] = lt[i][1] = lt[i][2] = 0.0f;
                lr[i][0] = lr[i][1] = lr[i][2] = 0.0f;
                lr[i][3] = 1.0f;
                ls[i][0] = ls[i][1] = ls[i][2] = 1.0f;
            }
        }
        /* Object slot defaults (identity; OBJECT tracks write). */
        e->obj_t[0] = e->obj_t[1] = e->obj_t[2] = 0.0f;
        e->obj_r[0] = e->obj_r[1] = e->obj_r[2] = 0.0f;
        e->obj_r[3] = 1.0f;
        e->obj_s[0] = e->obj_s[1] = e->obj_s[2] = 1.0f;
        if (clip != NULL) {
            if (le_anim_sample_clip_data(
                    clip, e->time, lt, lr, ls, joints, e->obj_t,
                    e->obj_r, e->obj_s) == LE_SUCCESS) {
                sampled = clip->track_count;
            }
        }
        e->has_object_tracks = 0;
        if (clip != NULL) {
            for (i = 0; i < clip->track_count; i++) {
                if (clip->tracks[i].target_kind ==
                    LE_ANIM_TARGET_OBJECT) {
                    e->has_object_tracks = 1;
                    break;
                }
            }
        }
        /* Crossfade blend (fade_from snapshot -> current).
         * Skeletonless animators fade the single object slot
         * (need == 1); joint animators fade need == joints+1
         * (object slot rides at index [joints]). */
        if (e->fading) {
            float w = (e->fade_duration > 1e-9f)
                          ? e->fade_elapsed / e->fade_duration
                          : 1.0f;

            if (w < 0.0f) {
                w = 0.0f;
            }
            if (w >= 1.0f) {
                /* Fade complete: adopt the destination handle.
                 * The shared clock keeps running (no reset);
                 * the current locals already hold the
                 * destination pose at w = 1 exactly, so no
                 * re-sample (no snap). */
                e->clip = e->fade_clip;
                e->fading = 0;
            } else {
                /* Blend snapshot -> current in place (joint
                 * slots; the object pose blends separately
                 * below — the sampler writes OBJECT tracks to
                 * e->obj_*, never into the joint arrays). */
                float(*ft)[3] = (float(*)[3])e->fade_from_t;
                float(*fr)[4] = (float(*)[4])e->fade_from_r;
                float(*fs)[3] = (float(*)[3])e->fade_from_s;

                /* Copy current to temp via the global scratch
                 * (reuse skin_m as temp: re-evaluated below). */
                float(*tmp_t)[3] = (float(*)[3])e->skin_m;
                for (i = 0; i < joints; i++) {
                    memcpy(tmp_t[i], lt[i], 3u * sizeof(float));
                }
                /* Blend T/S directly; R via slerp. */
                for (i = 0; i < joints; i++) {
                    lt[i][0] = ft[i][0] +
                               (tmp_t[i][0] - ft[i][0]) * w;
                    lt[i][1] = ft[i][1] +
                               (tmp_t[i][1] - ft[i][1]) * w;
                    lt[i][2] = ft[i][2] +
                               (tmp_t[i][2] - ft[i][2]) * w;
                    ls[i][0] = fs[i][0] +
                               (e->local_s[i * 3u + 0] -
                                fs[i][0]) *
                                   w;
                    ls[i][1] = fs[i][1] +
                               (e->local_s[i * 3u + 1] -
                                fs[i][1]) *
                                   w;
                    ls[i][2] = fs[i][2] +
                               (e->local_s[i * 3u + 2] -
                                fs[i][2]) *
                                   w;
                }
                {
                    float *cur_r = e->local_r;

                    for (i = 0; i < joints; i++) {
                        float a[4] = {
                            fr[i][0], fr[i][1], fr[i][2], fr[i][3]
                        };
                        float b[4] = {
                            cur_r[i * 4u + 0], cur_r[i * 4u + 1],
                            cur_r[i * 4u + 2], cur_r[i * 4u + 3]
                        };

                        le_am_quat_slerp(a, b, w, lr[i]);
                    }
                }
                /* Object pose blend (snapshot -> current). */
                {
                    float co_t[3];
                    float co_s[3];

                    memcpy(co_t, e->obj_t, sizeof(co_t));
                    memcpy(co_s, e->obj_s, sizeof(co_s));
                    e->obj_t[0] =
                        e->fade_obj_t[0] +
                        (co_t[0] - e->fade_obj_t[0]) * w;
                    e->obj_t[1] =
                        e->fade_obj_t[1] +
                        (co_t[1] - e->fade_obj_t[1]) * w;
                    e->obj_t[2] =
                        e->fade_obj_t[2] +
                        (co_t[2] - e->fade_obj_t[2]) * w;
                    e->obj_s[0] =
                        e->fade_obj_s[0] +
                        (co_s[0] - e->fade_obj_s[0]) * w;
                    e->obj_s[1] =
                        e->fade_obj_s[1] +
                        (co_s[1] - e->fade_obj_s[1]) * w;
                    e->obj_s[2] =
                        e->fade_obj_s[2] +
                        (co_s[2] - e->fade_obj_s[2]) * w;
                    le_am_quat_slerp(e->fade_obj_r, e->obj_r,
                                     w, e->obj_r);
                }
            }
        }
        /* Globals + skin. */
        if (skel != NULL && joints > 0) {
            float(*gm)[16] = (float(*)[16])e->global_m;
            float(*sm)[16] = (float(*)[16])e->skin_m;

            le_anim_locals_to_globals(skel, lt, lr, ls, gm,
                                      joints);
            le_anim_build_skin(skel, gm, sm, joints);
            e->pose_joints = joints;
        } else {
            e->pose_joints = 0;
        }
        e->pose_version++;
        e->pose_dirty = 0;
        /* OBJECT tracks -> owner local transform (ownership
         * policy enforced at add-time; static/kinematic/plain
         * owners only). */
        if (e->has_object_tracks && e->slot < world->capacity &&
            world->slots[e->slot].alive) {
            le_object_slot *s = &world->slots[e->slot];

            memcpy(s->position, e->obj_t, sizeof(s->position));
            memcpy(s->rotation, e->obj_r, sizeof(s->rotation));
            memcpy(s->scale, e->obj_s, sizeof(s->scale));
            le_mark_subtree_dirty(world, e->slot);
        }
    }
    return sampled;
}

/* Grow pose scratch (public within the animation module). */
le_result le_anim_grow_pose_for(le_world *world,
                                struct le_animator_entry *e,
                                uint32_t joints) {
    uint32_t need = joints + 1u;

    (void)world;
    if (e == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (e->pose_cap >= need && e->local_t != NULL &&
        e->local_r != NULL && e->local_s != NULL &&
        e->global_m != NULL && e->skin_m != NULL &&
        e->fade_from_t != NULL && e->fade_from_r != NULL &&
        e->fade_from_s != NULL) {
        return LE_SUCCESS;
    }
    {
        float *nt = (float *)realloc(e->local_t,
                                     (size_t)need * 3u *
                                         sizeof(float));
        float *nr = (float *)realloc(e->local_r,
                                     (size_t)need * 4u *
                                         sizeof(float));
        float *ns = (float *)realloc(e->local_s,
                                     (size_t)need * 3u *
                                         sizeof(float));
        float *gm = (float *)realloc(e->global_m,
                                     (size_t)need * 16u *
                                         sizeof(float));
        float *sm = (float *)realloc(e->skin_m,
                                     (size_t)need * 16u *
                                         sizeof(float));
        float *ft = (float *)realloc(e->fade_from_t,
                                     (size_t)need * 3u *
                                         sizeof(float));
        float *fr = (float *)realloc(e->fade_from_r,
                                     (size_t)need * 4u *
                                         sizeof(float));
        float *fs = (float *)realloc(e->fade_from_s,
                                     (size_t)need * 3u *
                                         sizeof(float));

        if (nt == NULL || nr == NULL || ns == NULL || gm == NULL ||
            sm == NULL || ft == NULL || fr == NULL || fs == NULL) {
            if (nt != NULL) {
                e->local_t = nt;
            }
            if (nr != NULL) {
                e->local_r = nr;
            }
            if (ns != NULL) {
                e->local_s = ns;
            }
            if (gm != NULL) {
                e->global_m = gm;
            }
            if (sm != NULL) {
                e->skin_m = sm;
            }
            if (ft != NULL) {
                e->fade_from_t = ft;
            }
            if (fr != NULL) {
                e->fade_from_r = fr;
            }
            if (fs != NULL) {
                e->fade_from_s = fs;
            }
            /* Adopt successes, recompute cap conservatively:
             * only claim what ALL arrays satisfy. Simplest
             * honest rule: keep old cap (retry next frame). */
            return LE_ERROR_OUT_OF_MEMORY;
        }
        e->local_t = nt;
        e->local_r = nr;
        e->local_s = ns;
        e->global_m = gm;
        e->skin_m = sm;
        e->fade_from_t = ft;
        e->fade_from_r = fr;
        e->fade_from_s = fs;
        e->pose_cap = need;
        return LE_SUCCESS;
    }
}

static int le_anim_effective(le_world *world, uint32_t slot) {
    le_object h;

    if (world == NULL || slot >= world->capacity) {
        return 0;
    }
    h.index = slot;
    h.generation = world->slots[slot].generation;
    h.world_tag = world->tag;
    return le_object_is_effectively_enabled(world, &h);
}

void le_anim_step_visual(le_world *world, float dt) {
    uint32_t i;
    uint64_t tracks = 0;
    uint64_t joints = 0;
    uint64_t fades = 0;

    if (world == NULL) {
        return;
    }
    if (!isfinite(dt) || dt <= 0.0f) {
        return;
    }
    if (world->animator_count == 0 || world->animators == NULL) {
        return;
    }
    for (i = 0; i < world->animator_count; i++) {
        struct le_animator_entry *e = &world->animators[i];
        float dur = 0.0f;

        if (e->slot >= world->capacity ||
            !world->slots[e->slot].alive) {
            continue;
        }
        if (!le_anim_effective(world, e->slot)) {
            continue; /* disabled: hold time, resume on enable */
        }
        /* Duration of the active clip (fade destination while
         * fading). */
        {
            uint32_t aslot;
            le_result code = LE_SUCCESS;
            const le_asset *h =
                (e->fading) ? &e->fade_clip : &e->clip;

            if (le_resolve_asset_live(world->engine, h, &aslot,
                                      &code) &&
                world->engine->assets[aslot].type ==
                    LE_ASSET_ANIMATION_CLIP &&
                world->engine->assets[aslot].clip != NULL) {
                dur = world->engine->assets[aslot].clip->duration;
            }
        }
        if (e->playing && e->speed > 0.0f && dur > 0.0f) {
            if (le_advance_time(e, dur, dt)) {
                /* ONCE finished: hold end pose, stop. */
                e->playing = 0;
            }
            e->pose_dirty = 1;
            /* Fade clock advances with playback. */
            if (e->fading) {
                e->fade_elapsed += dt * e->speed;
            }
        } else if (e->fading && e->playing) {
            /* Speed 0 with an active fade: still progress the
             * blend (visual continuity over paused time). */
            e->fade_elapsed += dt;
            e->pose_dirty = 1;
        }
        if (e->pose_dirty || e->fading) {
            uint32_t s = le_anim_evaluate_entry(world, e);

            tracks += s;
            joints += e->pose_joints;
            if (e->fading) {
                fades++;
            }
        }
    }
    /* Stats accumulation (phase totals live on the world via a
     * lightweight static? No — stats are recomputed on demand
     * in le_anim_get_stats; per-frame counters would need world
     * fields. Track frames_advanced via pose_version sum? Keep
     * it simple: stats derive from live entries on query). */
    (void)tracks;
    (void)joints;
    (void)fades;
}

void le_anim_get_stats(const le_world *world,
                       le_anim_stats *out_stats) {
    uint32_t i;

    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (world == NULL) {
        return;
    }
    out_stats->animator_count = world->animator_count;
    for (i = 0; i < world->animator_count; i++) {
        const struct le_animator_entry *e = &world->animators[i];

        if (e->playing) {
            out_stats->playing_count++;
        }
        if (e->fading) {
            out_stats->active_crossfades++;
        }
        out_stats->evaluated_joints += e->pose_joints;
        out_stats->frames_advanced += e->pose_version;
    }
    /* sampled_tracks: track counts of live clips (authoring
     * census, not per-frame integration). */
    if (world->engine != NULL) {
        for (i = 0; i < world->animator_count; i++) {
            const struct le_animator_entry *e =
                &world->animators[i];
            uint32_t aslot;
            le_result code = LE_SUCCESS;

            if (le_resolve_asset_live(world->engine, &e->clip,
                                      &aslot, &code) &&
                world->engine->assets[aslot].type ==
                    LE_ASSET_ANIMATION_CLIP &&
                world->engine->assets[aslot].clip != NULL) {
                out_stats->sampled_tracks +=
                    world->engine->assets[aslot]
                        .clip->track_count;
            }
        }
    }
}
