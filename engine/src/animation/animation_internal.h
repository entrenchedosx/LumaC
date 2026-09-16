/*
 * Animation internals (Phase 29). Never public; no renderer/Lua/VM
 * types here — plain floats, slots, and generational identity.
 *
 * Storage model (mirrors components + physics):
 * - Skeleton/clip assets: immutable, registry-owned
 *   (le_skeleton_data / le_clip_data on the asset slot). Many
 *   animators share them; runtime poses stay per-animator.
 * - Animators: dense per-world array with slot->entry back-links,
 *   swap-remove on removal, geometric growth, OOM-safe.
 * - Poses: contiguous per-animator TRS arrays (local + global +
 *   skin palette), grown with the animator's joint count.
 */

#ifndef LE_ANIMATION_INTERNAL_H
#define LE_ANIMATION_INTERNAL_H

#include <stdint.h>

#include "luma_engine/luma_engine.h"

/* Limits (public ceilings live in luma_engine.h). */
#define LE_ANIM_MAX_EVENTS_PER_CLIP 64u
#define LE_ANIM_NAME_MAX 64u

/* One skeleton joint (immutable, compact runtime identity =
 * the array index; names are import/debug/Lua sugar). */
typedef struct le_skeleton_joint {
    char name[LE_ANIM_NAME_MAX];
    int32_t parent; /* -1 = root */
    float bind_t[3];
    float bind_r[4]; /* unit quat */
    float bind_s[3];
    float inverse_bind[16]; /* column-major */
} le_skeleton_joint;

/* Immutable skeleton payload. */
typedef struct le_skeleton_data {
    le_skeleton_joint *joints; /* [joint_count] */
    uint32_t joint_count;
    uint32_t root_count;
} le_skeleton_data;

/* One key track (immutable). Times are non-decreasing, finite,
 * >= 0. Values are vec3 (T/S) or quat (R). CUBICSPLINE stores
 * glTF Hermite triples per key: in/value/out (tangent scale =
 * key interval, applied at sample time). */
typedef struct le_anim_track {
    le_anim_target_kind target_kind;
    uint32_t target_index;
    le_anim_channel channel;
    le_anim_interpolation interpolation;
    float *times;  /* [key_count] */
    float *values; /* [key_count * stride] (stride 3, or 9 for cubic) */
    uint32_t key_count;
    uint32_t stride; /* 3 (T/S), 4 (R), 9 (cubic T/S), 12 (cubic R) */
    uint32_t cursor; /* MUTABLE search hint (binary-search fallback
                      * keeps seeks correct; sequential steps reuse) */
} le_anim_track;

/* Immutable clip payload. */
typedef struct le_clip_data {
    float duration;
    le_anim_track *tracks; /* [track_count] */
    uint32_t track_count;
} le_clip_data;

/* Per-animator runtime entry (dense, swap-remove). */
typedef struct le_animator_entry {
    uint32_t slot;
    /* Asset handles (generation-checked every step; stale ->
     * animator holds pose and reports missing). */
    le_asset skeleton;
    le_asset clip;
    /* Playback state. */
    float time;
    float speed;
    le_anim_loop_mode loop_mode;
    int playing;
    int pingpong_dir; /* +1/-1 for PING_PONG */
    float weight;     /* reserved for multi-layer (always 1 now) */
    /* Crossfade state (A->B with snapshot; interrupt captures
     * the current blended pose). */
    int fading;
    le_asset fade_clip; /* destination (== clip when settled) */
    float fade_elapsed;
    float fade_duration;
    float *fade_from_t; /* snapshot local pose [joints] (+1 obj) */
    float *fade_from_r;
    float *fade_from_s;
    float fade_obj_t[3];
    float fade_obj_r[4];
    float fade_obj_s[3];
    /* Pose scratch (sized to skeleton joint_count, +1 object
     * slot at index [joint_count]). */
    float *local_t;
    float *local_r;
    float *local_s;
    float *global_m; /* [joint_count * 16] column-major */
    float *skin_m;   /* [joint_count * 16] joint_global*inv_bind */
    uint32_t pose_cap; /* joints allocated */
    uint32_t pose_joints; /* joints evaluated this frame */
    float obj_t[3]; /* OBJECT-track local pose (animator owner) */
    float obj_r[4];
    float obj_s[3];
    uint64_t pose_version; /* bumped per evaluation */
    uint64_t palette_version; /* last uploaded */
    int pose_dirty;
    int has_object_tracks;
} le_animator_entry;

/* Asset create/validate (anim_asset.c). */
le_result le_anim_create_skeleton_data(
    const le_skeleton_asset_desc *desc,
    struct le_skeleton_data **out);
le_result le_anim_create_clip_data(
    const le_animation_clip_desc *desc,
    struct le_clip_data **out);
void le_anim_free_skeleton(struct le_skeleton_data *skel);
void le_anim_free_clip(struct le_clip_data *clip);

/* Sampler (anim_sample.c): sample one track / whole clip. */
le_result le_anim_sample_track(const le_anim_track *track, float time,
                               float out_t[3], float out_r[4],
                               float out_s[3]);
le_result le_anim_sample_clip_data(
    const struct le_clip_data *clip, float time,
    float (*out_t)[3], float (*out_r)[4], float (*out_s)[3],
    uint32_t joint_count, float out_obj_t[3],
    float out_obj_r[4], float out_obj_s[3]);

/* Pose (anim_pose.c): bind globals, local->global, blend, skin. */
le_result le_anim_bind_globals(const struct le_skeleton_data *skel,
                               float (*out_global)[16],
                               uint32_t joint_count);
le_result le_anim_locals_to_globals(
    const struct le_skeleton_data *skel,
    const float (*local_t)[3], const float (*local_r)[4],
    const float (*local_s)[3], float (*out_global)[16],
    uint32_t joint_count);
le_result le_anim_build_skin(
    const struct le_skeleton_data *skel,
    const float (*global_m)[16], float (*out_skin)[16],
    uint32_t joint_count);

/* Animator component (animator.c). */
int le_anim_entry_index(le_world *world, uint32_t slot);
le_result le_anim_ensure_entry(le_world *world, uint32_t slot,
                               struct le_animator_entry **out);
/* Pose scratch growth (anim_step.c; shared with the evaluator). */
le_result le_anim_grow_pose_for(le_world *world,
                                struct le_animator_entry *e,
                                uint32_t joints);
/* Evaluate one entry's pose without advancing time (anim_step.c;
 * used by the visual step and the on-demand palette getter). */
uint32_t le_anim_evaluate_entry(le_world *world,
                                struct le_animator_entry *e);

/* glTF import helpers (anim_gltf.c): build skeleton/clip payloads
 * from parsed cgltf structs without depending on the assets
 * module (caller passes raw pointers). Struct-blind: the caller
 * (engine gltf bridge) includes cgltf itself. */
struct cgltf_data_sel;
le_result le_anim_build_skeleton_from_gltf(
    const void *cgltf_data, uint32_t skin_index,
    struct le_skeleton_data **out);
le_result le_anim_build_clip_from_gltf(
    const void *cgltf_data, uint32_t anim_index,
    const struct le_skeleton_data *skeleton /* may be NULL */,
    struct le_clip_data **out, float *out_duration);

#endif /* LE_ANIMATION_INTERNAL_H */
