/*
 * Physics internals (Phase 28). Never public; no renderer/Lua/VM
 * types here — plain floats, slots, and generational identity.
 *
 * Storage model (mirrors components):
 * - Bodies + colliders: dense arrays with slot->entry back-links,
 *   swap-remove on removal, geometric growth, OOM-safe. Entries
 *   key the OBJECT slot (never a body index) for pair identity:
 *   contacts/events key (slotA,slotB) + generations, so slot
 *   reuse can never inherit stale contact state.
 * - Broad phase: sweep-and-prune over per-collider AABBs rebuilt
 *   each step (insertion sort on coherent axis — O(n) typical,
 *   O(n^2) worst, deterministic). Brute force exists only as a
 *   test oracle (test_physics.c), never as the production path.
 * - Contacts: per-step manifold list (solver input) + persistent
 *   overlap set (previous-frame pairs for ENTER/STAY/EXIT).
 * - Events: per-object ring buffers (64), drained by C or Lua.
 */

#ifndef LE_PHYSICS_INTERNAL_H
#define LE_PHYSICS_INTERNAL_H

#include <stdint.h>

#include "luma_engine/luma_engine.h"

/* Solver + correction constants (documented in PHYSICS_SOLVER). */
#define LE_PHYS_DEFAULT_VELOCITY_ITERS 8u
#define LE_PHYS_DEFAULT_POSITION_ITERS 3u
#define LE_PHYS_MAX_ITERS 64u
#define LE_PHYS_SLOP 0.005f
#define LE_PHYS_BAUMGARTE 0.2f
#define LE_PHYS_MAX_CORRECTION 0.5f
#define LE_PHYS_RESTITUTION_SLOP 1.0f
#define LE_PHYS_MAX_EVENTS_PER_OBJECT 64u
#define LE_PHYS_MAX_CONTACTS 8192u
#define LE_PHYS_MAX_PAIRS 8192u
#define LE_PHYS_KINEMATIC_VELOCITY_CAP 50.0f

/* Dense body entry (slot back-link like every component). */
typedef struct le_body_entry {
    uint32_t slot;
    le_body_type type;
    float mass;
    float inv_mass;
    float linear_velocity[3];
    float angular_velocity[3];
    float force[3];
    float torque[3];
    float linear_damping;
    float angular_damping;
    float gravity_scale;
    /* World-space inverse inertia diagonal (sphere/box closed
     * forms, rotated per orientation each step). */
    float inv_inertia[3];
} le_body_entry;

/* Dense collider entry. */
typedef struct le_collider_entry {
    uint32_t slot;
    le_collider_shape shape;
    float radius;         /* sphere */
    float half_extents[3];/* box (local, pre-scale) */
    float offset[3];
    float orientation[4]; /* local quat, unit */
    int is_trigger;
    uint32_t layer; /* 0..31 */
    uint32_t mask;
    float friction;
    float restitution;
    /* Cached world AABB (rebuilt each step before SAP). */
    float aabb_min[3];
    float aabb_max[3];
    /* Cached world pose of the shape frame (center + basis). */
    float world_center[3];
    float world_basis[3][3]; /* columns = local axes in world */
    float world_radius;      /* sphere: scaled radius */
    float world_half[3];     /* box: scaled half extents */
    int aabb_valid;
} le_collider_entry;

/* One contact point (solver input + event payload source). */
typedef struct le_contact_point {
    uint32_t slot_a; /* sorted: slot_a < slot_b, or == with gen */
    uint32_t slot_b;
    uint32_t gen_a;
    uint32_t gen_b;
    float normal[3]; /* A -> B, unit */
    float point[3];  /* world */
    float penetration;
    int is_trigger;
    /* Solver accumulators (warm-start within the step only;
     * never persisted across steps in Phase 28). */
    float normal_impulse;
    float tangent_impulse[2];
    /* Restitution velocity target (e * |approach|), computed
     * ONCE per step before velocity iterations from the
     * pre-solve approach velocity. Must persist across all
     * iterations: recomputing per iteration unwinds the bounce
     * (iteration 2 sees separating velocity and eats iteration
     * 1's impulse). Zero when approach <= slop. */
    float rest_bias;
} le_contact_point;

/* Persistent overlap record (previous step's pair set). */
typedef struct le_overlap_rec {
    uint32_t slot_a;
    uint32_t slot_b;
    uint32_t gen_a;
    uint32_t gen_b;
    int was_trigger;
} le_overlap_rec;

/* Per-object pending event. */
typedef struct le_pending_collision {
    le_collision_event ev;
} le_pending_collision;

typedef struct le_object_events {
    uint32_t slot;
    le_pending_collision ring[LE_PHYS_MAX_EVENTS_PER_OBJECT];
    uint32_t head;
    uint32_t count;
    uint64_t dropped;
} le_object_events;

struct le_physics_world {
    le_body_entry *bodies;
    uint32_t body_cap;
    uint32_t body_count;
    le_collider_entry *colliders;
    uint32_t collider_cap;
    uint32_t collider_count;
    float gravity[3];
    uint32_t velocity_iters;
    uint32_t position_iters;
    /* Per-step scratch (grown geometrically, reused, never
     * per-contact heap allocation in the hot loop). */
    le_contact_point *contacts;
    uint32_t contact_cap;
    uint32_t contact_count;
    le_overlap_rec *prev_overlaps;
    uint32_t prev_cap;
    uint32_t prev_count;
    uint32_t *sap_order; /* collider indices sorted by min-x */
    uint32_t sap_cap;
    /* Candidate pair scratch (per-world reuse; never global). */
    uint32_t *pair_a;
    uint32_t *pair_b;
    uint32_t pair_cap;
    /* Per-object event rings (parallel to slots; indexed by
     * slot, validated by generation at drain). */
    le_object_events *events;
    uint32_t events_cap;
    uint64_t events_dropped_total;
    /* Stats. */
    uint32_t stat_candidates;
    uint32_t stat_narrow_tests;
    uint64_t stat_ray_queries;
};

/* Lifecycle (physics.c). */
struct le_physics_world *le_physics_create(void);
void le_physics_destroy(struct le_physics_world *pw);
/* Ensure event storage covers `slots` slots (world growth). */
le_result le_physics_ensure_events(struct le_physics_world *pw,
                                   uint32_t slots);
/* Retire every trace of a slot (destroy/remove paths): contacts
 * drop it this step; overlaps emit EXIT; events clear. */
void le_physics_retire_slot(le_world *world, uint32_t slot);
/* Queue one event onto a slot's ring (events.c; shared with
 * retire-time EXIT emission). */
void le_physics_queue_event(le_world *world, uint32_t slot,
                            const le_collision_event *ev);

/* Fixed-step entry (step.c): full ordered phase 1..8. */
void le_physics_step(le_world *world, float dt);

/* Body/collider helpers (body.c/collider.c). */
int le_physics_body_index(le_world *world, uint32_t slot);
int le_physics_collider_index(le_world *world, uint32_t slot);
/* Refresh one collider's world pose + AABB from engine
 * transforms (fails 0 on shear/scale pathology — caller skips
 * the collider for the step, never poisons the world). */
int le_physics_refresh_collider(le_world *world,
                                le_collider_entry *c);
/* Local inverse inertia diagonal for a shape (0 for triggers —
 * triggers never respond). */
void le_physics_shape_inertia(const le_collider_entry *c,
                              float mass, float out_inv[3]);

/* Broad phase (broadphase.c): rebuild AABBs, SAP sweep, emit
 * candidate collider-index pairs (sorted, duplicate-free). */
uint32_t le_physics_broadphase(
    le_world *world, uint32_t *out_a, uint32_t *out_b,
    uint32_t cap);

/* Narrow phase (narrowphase.c): exact manifold for one pair.
 * Returns contact count (0/1/2 in Phase 28: deepest feature).
 * Normal is A -> B; swapped order reverses it (tested). */
int le_physics_narrow_pair(le_world *world,
                           const le_collider_entry *a,
                           const le_collider_entry *b,
                           le_contact_point *out);

/* Solver (solver.c): sequential impulse over contacts. */
void le_physics_solve(le_world *world, float dt);

/* Events (events.c): diff overlaps vs contacts, queue ENTER/
 * STAY/EXIT + trigger variants, retire destroyed slots. */
void le_physics_emit_events(le_world *world);

/* Queries (query.c): ray/overlap against step-fresh AABBs +
 * narrow phase. `fired` gates trigger inclusion. */
int le_physics_ray_narrow(le_world *world,
                          const le_collider_entry *c, float ox,
                          float oy, float oz, float dx, float dy,
                          float dz, float max_t, float *out_t,
                          float out_n[3]);

/* Scene capture/apply (scene_physics.c): authoring state only. */
void le_physics_capture_for_record(le_world *world, uint32_t slot,
                                   le_scene_object *rec);
le_result le_physics_validate_record(const le_scene_object *rec);
le_result le_physics_apply_record(le_world *world,
                                  const le_object *obj,
                                  const le_scene_object *rec);

#endif /* LE_PHYSICS_INTERNAL_H */
