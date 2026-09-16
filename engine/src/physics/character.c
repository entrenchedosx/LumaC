/*
 * Kinematic character controller (Phase 30): dedicated capsule
 * movement primitive — NOT a dynamic rigid body.
 *
 * Pipeline per le_character_move call:
 *   1. initial overlap recovery (bounded depenetration)
 *   2. platform ride (kinematic ground delta inheritance)
 *   3. capsule sweep -> move to contact (skin margin)
 *   4. slide: v -= n * min(dot(v,n), 0), re-clipped against up
 *      to 3 blocking planes (corners settle, creases slide)
 *   5. step attempt (up -> forward -> down, walkable landing,
 *      headroom check) — bounded, slope-validated
 *   6. ground probe + snap (disabled while rising)
 *   7. final state publish (grounded, normals, handles)
 *
 * Identity: ground/platform objects are (slot, generation, tag)
 * — never raw pointers, never bare slot indices. Destroyed or
 * slot-reused platforms invalidate automatically. No Lua/VM
 * types here (plain floats + handles; AOT-compatible).
 *
 * Iteration caps: LE_CHAR_MAX_SLIDES (4) slide iterations,
 * LE_CHAR_MAX_DEPEN (8) recovery iterations, 1 step attempt
 * per slide contact. No per-move heap allocation (bounded
 * stack planes + world scratch only).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

#define LE_CHAR_MAX_SLIDES 4u
#define LE_CHAR_MAX_DEPEN 8u
#define LE_CHAR_MAX_PLANES 3u
#define LE_CHAR_EPS 1e-6f

/* Forward declarations (helpers defined below). */
static float le_ch_dot(const float a[3], const float b[3]);
static int le_ch_desc_valid(const le_character_desc *d,
                            float *out_cos);
static void le_ch_capsule(const struct le_character_entry *e,
                          const float origin[3],
                          float out_center[3], float *out_r,
                          float *out_h);
static void le_ch_up_quat(const float up[3], float q[4]);
static int le_ch_walkable(const struct le_character_entry *e,
                          const float n[3]);

static float le_ch_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float le_ch_len(const float a[3]) {
    return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

static int le_ch_finite3(const float v[3]) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

/* Dense character entry (slot back-link like every component).
 * Defined here (character.c owns the layout); the internal
 * header forward-declares it for the le_world pointer. */
struct le_character_entry {
    uint32_t slot;
    le_character_desc cfg;
    float cos_slope; /* cos(max_slope_angle), precomputed */
    /* Runtime state (never serialized). */
    float fall_velocity; /* signed along +up (pos = rising) */
    float last_horizontal[3];
    float last_motion[3];
    int grounded;
    float ground_normal[3];
    uint32_t ground_slot;
    uint32_t ground_gen;
    float ground_center[3]; /* platform ride reference */
    int has_ground_ref;
    int enabled;
    uint32_t collision_count;
    int unresolved;
};

int le_character_entry_index(le_world *world, uint32_t slot) {
    uint32_t idx;

    if (world == NULL || slot >= world->capacity) {
        return -1;
    }
    if (!(world->slots[slot].present & LE_PRESENT_CHARACTER)) {
        return -1;
    }
    idx = (uint32_t)world->slots[slot].character_index;
    if (idx >= world->character_count ||
        world->characters[idx].slot != slot) {
        return -1;
    }
    return (int)idx;
}

static le_result le_ch_ensure_cap(le_world *world) {
    if (world->characters == NULL ||
        world->character_count >= world->character_capacity) {
        uint32_t grown = (world->character_capacity == 0)
                             ? 16u
                             : world->character_capacity * 2u;
        struct le_character_entry *fresh;

        if (grown > 0x00FFFFFFu) {
            return LE_ERROR_OVERFLOW;
        }
        fresh = (struct le_character_entry *)realloc(
            world->characters, (size_t)grown * sizeof(*fresh));
        if (fresh == NULL) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
        world->characters = fresh;
        world->character_capacity = grown;
    }
    return LE_SUCCESS;
}

/* Validate a character desc (authoring values only). */
static int le_ch_desc_valid(const le_character_desc *d,
                            float *out_cos) {
    float ul;

    if (d == NULL) {
        return 0;
    }
    if (!isfinite(d->radius) || d->radius <= 0.0f ||
        d->radius > 1e6f) {
        return 0;
    }
    if (!isfinite(d->height) || d->height <= 0.0f ||
        d->height > 1e6f) {
        return 0;
    }
    if (d->height < 2.0f * d->radius) {
        return 0;
    }
    if (!le_ch_finite3(d->up)) {
        return 0;
    }
    ul = le_ch_len(d->up);
    if (!isfinite(ul) || ul < 1e-6f) {
        return 0;
    }
    if (!isfinite(d->skin_width) || d->skin_width < 0.0f ||
        d->skin_width > d->radius) {
        return 0;
    }
    if (!isfinite(d->max_slope_angle) ||
        d->max_slope_angle < 0.0f ||
        d->max_slope_angle >= 1.5707963f) {
        return 0;
    }
    if (!isfinite(d->step_height) || d->step_height < 0.0f ||
        d->step_height > 1e6f) {
        return 0;
    }
    if (!isfinite(d->gravity) || d->gravity < 0.0f ||
        d->gravity > 1e6f) {
        return 0;
    }
    if (!isfinite(d->terminal_velocity) ||
        d->terminal_velocity < 0.0f ||
        d->terminal_velocity > 1e6f) {
        return 0;
    }
    if (!isfinite(d->snap_distance) || d->snap_distance < 0.0f ||
        d->snap_distance > 1e6f) {
        return 0;
    }
    if (!isfinite(d->push_strength) || d->push_strength < 0.0f ||
        d->push_strength > 1e9f) {
        return 0;
    }
    if (d->layer > 31u) {
        return 0;
    }
    if (out_cos != NULL) {
        *out_cos = cosf(d->max_slope_angle);
    }
    return 1;
}

le_result le_object_add_character(le_world *world,
                                  const le_object *object,
                                  const le_character_desc *desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_result rc;
    struct le_character_entry *e;
    float cosslope = 0.0f;
    le_character_desc norm;

    if (world == NULL || object == NULL || desc == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_ch_desc_valid(desc, &cosslope)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    /* Characters must be world roots (world-space sweep
     * mechanics have no valid parent-relative inversion). */
    if (world->slots[slot].parent != LE_NO_LINK) {
        return LE_ERROR_INVALID_HIERARCHY;
    }
    /* No DYNAMIC body on the same slot (kinematic controller
     * vs dynamic integration would fight for the transform). */
    if ((world->slots[slot].present & LE_PRESENT_RIGID_BODY) !=
        0u) {
        le_rigid_body_desc bd;

        memset(&bd, 0, sizeof(bd));
        if (le_object_get_rigid_body(world, object, &bd) &&
            bd.type == LE_BODY_DYNAMIC) {
            return LE_ERROR_INVALID_HIERARCHY;
        }
    }
    norm = *desc;
    /* Normalize up. */
    {
        float ul = le_ch_len(norm.up);

        norm.up[0] /= ul;
        norm.up[1] /= ul;
        norm.up[2] /= ul;
    }
    {
        int existing = le_character_entry_index(world, slot);

        if (existing >= 0) {
            e = &world->characters[(uint32_t)existing];
            e->cfg = norm;
            e->cos_slope = cosslope;
            return LE_SUCCESS;
        }
    }
    rc = le_ch_ensure_cap(world);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    e = &world->characters[world->character_count];
    memset(e, 0, sizeof(*e));
    e->slot = slot;
    e->cfg = norm;
    e->cos_slope = cosslope;
    e->enabled = 1;
    e->ground_normal[0] = norm.up[0];
    e->ground_normal[1] = norm.up[1];
    e->ground_normal[2] = norm.up[2];
    e->ground_slot = 0xFFFFFFFFu;
    world->slots[slot].character_index =
        (int32_t)world->character_count;
    world->slots[slot].present |= LE_PRESENT_CHARACTER;
    world->character_count++;
    return LE_SUCCESS;
}

le_result le_object_remove_character(le_world *world,
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
    idx = le_character_entry_index(world, slot);
    if (idx < 0) {
        return LE_SUCCESS;
    }
    {
        uint32_t last = world->character_count - 1u;

        if ((uint32_t)idx != last) {
            world->characters[(uint32_t)idx] =
                world->characters[last];
            world->slots[world->characters[(uint32_t)idx].slot]
                .character_index = (int32_t)idx;
        }
        world->character_count--;
    }
    world->slots[slot].character_index = LE_NO_LINK;
    world->slots[slot].present &= ~LE_PRESENT_CHARACTER;
    return LE_SUCCESS;
}

int le_object_get_character(const le_world *world,
                            const le_object *object,
                            le_character_desc *out_desc) {
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
    idx = le_character_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return 0;
    }
    *out_desc = world->characters[(uint32_t)idx].cfg;
    return 1;
}

void le_character_remove_slot(le_world *world, uint32_t slot) {
    int idx;

    if (world == NULL || slot >= world->capacity) {
        return;
    }
    idx = le_character_entry_index(world, slot);
    if (idx < 0) {
        return;
    }
    {
        uint32_t last = world->character_count - 1u;

        if ((uint32_t)idx != last) {
            world->characters[(uint32_t)idx] =
                world->characters[last];
            world->slots[world->characters[(uint32_t)idx].slot]
                .character_index = (int32_t)idx;
        }
        world->character_count--;
    }
}

void le_character_destroy_world(le_world *world) {
    if (world == NULL) {
        return;
    }
    free(world->characters);
    world->characters = NULL;
    world->character_capacity = 0;
    world->character_count = 0;
}

/* Capsule center for a character entry (world-space object
 * origin + half height along up... the controller capsule is
 * axis-aligned to `up`, NOT to object rotation: characters are
 * roots whose rotation is yaw-only by convention; the capsule
 * segment runs along `up` centered on the object origin + up *
 * (height/2 - radius)? Convention: object origin = FEET; center
 * = origin + up * (height * 0.5). Segment half length =
 * height * 0.5 - radius. */
static void le_ch_capsule(const struct le_character_entry *e,
                          const float origin[3],
                          float out_center[3], float *out_r,
                          float *out_h) {
    float hh = e->cfg.height * 0.5f - e->cfg.radius;

    if (hh < 0.0f) {
        hh = 0.0f;
    }
    out_center[0] =
        origin[0] + e->cfg.up[0] * e->cfg.height * 0.5f;
    out_center[1] =
        origin[1] + e->cfg.up[1] * e->cfg.height * 0.5f;
    out_center[2] =
        origin[2] + e->cfg.up[2] * e->cfg.height * 0.5f;
    *out_r = e->cfg.radius;
    *out_h = hh;
}

/* Orientation quat mapping local Y to `up` (for capsule casts).
 * Same construction as CCD. */
static void le_ch_up_quat(const float up[3], float q[4]) {
    float cx = 1.0f * up[2] - 0.0f * up[1];
    float cy = 0.0f * up[0] - 0.0f * up[2];
    float cz = 0.0f * up[1] - 1.0f * up[0];
    float dot = up[1];
    float s = sqrtf((1.0f + dot) * 2.0f);

    if (s < 1e-6f) {
        if (dot < 0.0f) {
            q[0] = 1.0f;
            q[1] = 0.0f;
            q[2] = 0.0f;
            q[3] = 0.0f;
        } else {
            q[0] = 0.0f;
            q[1] = 0.0f;
            q[2] = 0.0f;
            q[3] = 1.0f;
        }
        return;
    }
    {
        float inv = 1.0f / s;

        q[0] = cx * inv;
        q[1] = cy * inv;
        q[2] = cz * inv;
        q[3] = s * 0.5f;
    }
}

/* Walkable test: dot(normal, up) >= cos(max_slope). */
static int le_ch_walkable(const struct le_character_entry *e,
                          const float n[3]) {
    float d = le_ch_dot(n, e->cfg.up);

    return d >= e->cos_slope - 1e-6f;
}

/* Push a dynamic body hit by the character (bounded impulse).
 * Direction: along the character motion's inward-normal-removed
 * component... simplest coherent policy: impulse J = push *
 * m_body * approach_speed along the contact normal (pushing the
 * body AWAY from the character, i.e. along -hit_normal where
 * hit_normal points from body to character). Capped so the
 * character can never explode a crate. */
static void le_ch_push(le_world *world,
                       const struct le_character_entry *e,
                       const le_shape_hit *hit,
                       const float motion[3]) {
    uint32_t hs;
    int bi;
    le_body_entry *b;
    float approach;
    float imp;

    if (e->cfg.push_strength <= 0.0f) {
        return;
    }
    hs = hit->object.index;
    if (hs >= world->capacity || !world->slots[hs].alive) {
        return;
    }
    if (!(world->slots[hs].present & LE_PRESENT_RIGID_BODY)) {
        return;
    }
    bi = le_physics_body_index(world, hs);
    if (bi < 0) {
        return;
    }
    b = &world->physics->bodies[(uint32_t)bi];
    if (b->type != LE_BODY_DYNAMIC || b->inv_mass <= 0.0f) {
        return;
    }
    /* Approach speed = motion into the surface. */
    approach = -(motion[0] * hit->normal[0] +
                 motion[1] * hit->normal[1] +
                 motion[2] * hit->normal[2]);
    if (approach <= 0.0f || !isfinite(approach)) {
        return;
    }
    /* Cap approach contribution (no explosive shoves). */
    if (approach > 10.0f) {
        approach = 10.0f;
    }
    imp = e->cfg.push_strength * approach / b->inv_mass;
    if (imp > 100.0f / b->inv_mass) {
        imp = 100.0f / b->inv_mass;
    }
    /* Push direction: away from character = -hit normal. */
    b->linear_velocity[0] += -hit->normal[0] * imp *
                             b->inv_mass;
    b->linear_velocity[1] += -hit->normal[1] * imp *
                             b->inv_mass;
    b->linear_velocity[2] += -hit->normal[2] * imp *
                             b->inv_mass;
}

/* Depenetration: overlap recovery at the current pose.
 * Bounded LE_CHAR_MAX_DEPEN iterations; each iteration sweeps
 * a zero displacement (overlap query) and pushes out along the
 * min-penetration normal by (penetration + skin). Caps total
 * travel at 4 * (radius + step_height); reports unresolved when
 * still overlapped. Returns moved distance. */
static float le_ch_depenetrate(le_world *world,
                               struct le_character_entry *e,
                               uint32_t slot, int *out_unresolved) {
    float moved = 0.0f;
    float max_total =
        4.0f * (e->cfg.radius + e->cfg.step_height + 0.1f);
    uint32_t iter;

    *out_unresolved = 0;
    for (iter = 0; iter < LE_CHAR_MAX_DEPEN; iter++) {
        float origin[3];
        float center[3];
        float r;
        float h;
        float q[4];
        float dims[3];
        float zero[3] = { 0.0f, 0.0f, 0.0f };
        le_shape_hit hit;
        uint32_t mask;

        if (slot >= world->capacity ||
            !world->slots[slot].alive) {
            break;
        }
        le_refresh_world_matrices(world);
        origin[0] = world->slots[slot].position[0];
        origin[1] = world->slots[slot].position[1];
        origin[2] = world->slots[slot].position[2];
        le_ch_capsule(e, origin, center, &r, &h);
        le_ch_up_quat(e->cfg.up, q);
        dims[0] = r;
        dims[1] = h;
        dims[2] = 0.0f;
        mask = e->cfg.mask;
        memset(&hit, 0, sizeof(hit));
        if (!le_physics_shape_cast(
                world, LE_CAST_CAPSULE, center, q, dims, zero,
                mask, 0, slot, &hit, NULL)) {
            break; /* free */
        }
        if (!hit.started_overlapping || hit.penetration <=
                                            LE_CHAR_EPS) {
            break;
        }
        {
            float push = hit.penetration + e->cfg.skin_width;

            if (push > max_total - moved) {
                push = max_total - moved;
            }
            if (push <= LE_CHAR_EPS) {
                *out_unresolved = 1;
                break;
            }
            world->slots[slot].position[0] +=
                hit.normal[0] * push;
            world->slots[slot].position[1] +=
                hit.normal[1] * push;
            world->slots[slot].position[2] +=
                hit.normal[2] * push;
            moved += push;
            world->physics->stat_depenetrations++;
            le_mark_subtree_dirty(world, slot);
            if (moved >= max_total - LE_CHAR_EPS) {
                *out_unresolved = 1;
                break;
            }
        }
    }
    /* Final check: still overlapped? */
    if (!*out_unresolved) {
        float origin[3];
        float center[3];
        float r;
        float h;
        float q[4];
        float dims[3];
        float zero[3] = { 0.0f, 0.0f, 0.0f };
        le_shape_hit hit;

        if (slot < world->capacity &&
            world->slots[slot].alive) {
            origin[0] = world->slots[slot].position[0];
            origin[1] = world->slots[slot].position[1];
            origin[2] = world->slots[slot].position[2];
            le_ch_capsule(e, origin, center, &r, &h);
            le_ch_up_quat(e->cfg.up, q);
            dims[0] = r;
            dims[1] = h;
            dims[2] = 0.0f;
            memset(&hit, 0, sizeof(hit));
            if (le_physics_shape_cast(
                    world, LE_CAST_CAPSULE, center, q, dims,
                    zero, e->cfg.mask, 0, slot, &hit, NULL) &&
                hit.penetration > 0.01f) {
                *out_unresolved = 1;
            }
        }
    }
    return moved;
}

/* Platform ride: if grounded on a live kinematic body, inherit
 * its frame translation delta (current center vs stored ref).
 * Teleports (> 2 m in one move) invalidate inheritance.
 * Rotation contribution: deferred (translation only). */
static void le_ch_platform_ride(le_world *world,
                                struct le_character_entry *e,
                                uint32_t slot) {
    uint32_t gs;
    float wm[16];
    le_object h;
    float dx;
    float dy;
    float dz;
    float dl;

    if (!e->grounded || !e->has_ground_ref) {
        return;
    }
    gs = e->ground_slot;
    if (gs >= world->capacity || !world->slots[gs].alive ||
        world->slots[gs].generation != e->ground_gen) {
        /* Platform gone: airborne from here (probe decides). */
        e->grounded = 0;
        e->has_ground_ref = 0;
        return;
    }
    h.index = gs;
    h.generation = e->ground_gen;
    h.world_tag = world->tag;
    le_object_get_world_matrix(world, &h, wm);
    dx = wm[12] - e->ground_center[0];
    dy = wm[13] - e->ground_center[1];
    dz = wm[14] - e->ground_center[2];
    dl = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!isfinite(dl)) {
        e->has_ground_ref = 0;
        return;
    }
    if (dl > 2.0f) {
        /* Teleport: do not inherit absurd velocity; rebase. */
        e->ground_center[0] = wm[12];
        e->ground_center[1] = wm[13];
        e->ground_center[2] = wm[14];
        return;
    }
    if (dl > LE_CHAR_EPS && slot < world->capacity &&
        world->slots[slot].alive) {
        world->slots[slot].position[0] += dx;
        world->slots[slot].position[1] += dy;
        world->slots[slot].position[2] += dz;
        le_mark_subtree_dirty(world, slot);
    }
    e->ground_center[0] = wm[12];
    e->ground_center[1] = wm[13];
    e->ground_center[2] = wm[14];
}

le_result le_character_move(
    le_world *world, const le_object *object,
    const float displacement[3],
    le_character_move_result *out_result) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;
    struct le_character_entry *e;
    le_character_move_result res;
    float remaining[3];
    float planes[LE_CHAR_MAX_PLANES][3];
    uint32_t nplanes = 0;
    uint32_t slide_iter;
    float total_moved[3] = { 0.0f, 0.0f, 0.0f };
    int hit_wall = 0;
    int hit_ceiling = 0;
    int stepped = 0;
    uint32_t collisions = 0;

    memset(&res, 0, sizeof(res));
    res.ground_object = LE_OBJECT_INVALID;
    if (out_result != NULL) {
        *out_result = res;
    }
    if (world == NULL || object == NULL ||
        displacement == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!isfinite(displacement[0]) ||
        !isfinite(displacement[1]) ||
        !isfinite(displacement[2])) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    {
        float dl = le_ch_len(displacement);

        if (!isfinite(dl) || dl > 1e6f) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    idx = le_character_entry_index(world, slot);
    if (idx < 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    e = &world->characters[(uint32_t)idx];
    if (!e->enabled) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    /* Root rule enforced at add; re-check (reparent since). */
    if (world->slots[slot].parent != LE_NO_LINK) {
        return LE_ERROR_INVALID_HIERARCHY;
    }
    res.requested[0] = displacement[0];
    res.requested[1] = displacement[1];
    res.requested[2] = displacement[2];
    world->physics->stat_character_sweeps++;
    /* 1. Depenetration first (spawn/platform/teleport/drift). */
    {
        int unresolved = 0;

        le_ch_depenetrate(world, e, slot, &unresolved);
        res.unresolved_penetration = unresolved;
        e->unresolved = unresolved;
    }
    /* 2. Platform ride (uses pre-move ground). */
    le_ch_platform_ride(world, e, slot);
    /* Assume airborne until the probe proves ground (platform
     * ride preserved e->grounded only as a ride input; the move
     * re-derives it). Keep previous ground as fallback only
     * when displacement is ~zero AND probe hits nothing AND we
     * were grounded? No — deterministic rule: probe decides.
     * Save prev for step-up continuity. */
    {
        int was_grounded = e->grounded;

        (void)was_grounded;
    }
    e->grounded = 0;
    remaining[0] = displacement[0];
    remaining[1] = displacement[1];
    remaining[2] = displacement[2];
    /* 3+4. Move-and-slide loop. */
    for (slide_iter = 0; slide_iter < LE_CHAR_MAX_SLIDES;
         slide_iter++) {
        float rlen = le_ch_len(remaining);

        if (rlen < LE_CHAR_EPS) {
            break;
        }
        {
            float origin[3];
            float center[3];
            float r;
            float h;
            float q[4];
            float dims[3];
            le_shape_hit hit;
            float eff_disp[3];

            le_refresh_world_matrices(world);
            origin[0] = world->slots[slot].position[0];
            origin[1] = world->slots[slot].position[1];
            origin[2] = world->slots[slot].position[2];
            le_ch_capsule(e, origin, center, &r, &h);
            le_ch_up_quat(e->cfg.up, q);
            /* Skin margin: shrink the sweep displacement by
             * nothing but stop skin_width short of contact
             * (handled via advance = fraction - skin/|d|). */
            dims[0] = r;
            dims[1] = h;
            dims[2] = 0.0f;
            memset(&hit, 0, sizeof(hit));
            eff_disp[0] = remaining[0];
            eff_disp[1] = remaining[1];
            eff_disp[2] = remaining[2];
            if (!le_physics_shape_cast(
                    world, LE_CAST_CAPSULE, center, q, dims,
                    eff_disp, e->cfg.mask, 0, slot, &hit,
                    NULL)) {
                /* Free: take it all. */
                world->slots[slot].position[0] += remaining[0];
                world->slots[slot].position[1] += remaining[1];
                world->slots[slot].position[2] += remaining[2];
                total_moved[0] += remaining[0];
                total_moved[1] += remaining[1];
                total_moved[2] += remaining[2];
                le_mark_subtree_dirty(world, slot);
                remaining[0] = remaining[1] = remaining[2] =
                    0.0f;
                break;
            }
            collisions++;
            world->physics->stat_character_slides++;
            /* Dynamic push (bounded) before treating as
             * blocker. */
            {
                uint32_t hs = hit.object.index;

                if (hs < world->capacity &&
                    world->slots[hs].alive &&
                    (world->slots[hs].present &
                     LE_PRESENT_RIGID_BODY)) {
                    int bi =
                        le_physics_body_index(world, hs);

                    if (bi >= 0 &&
                        world->physics
                                ->bodies[(uint32_t)bi]
                                .type == LE_BODY_DYNAMIC) {
                        le_ch_push(world, e, &hit, remaining);
                    }
                }
            }
            /* Classify the hit. */
            {
                float upness = le_ch_dot(hit.normal,
                                         e->cfg.up);

                if (upness > 0.7f) {
                    /* Floor-ish (also steep-slope candidacy
                     * resolved by walkable test at probe). */
                } else if (upness < -0.7f) {
                    hit_ceiling = 1;
                } else {
                    hit_wall = 1;
                }
            }
            /* Advance to contact minus skin. The horizontal
             * part of the PRE-advance remainder is the step
             * candidate: per-frame moves are small, so the
             * post-advance sliver alone would never trigger a
             * step (must snapshot before consuming). */
            float pre_horiz[3];
            {
                float adv = hit.fraction;
                float back = 0.0f;

                pre_horiz[0] = remaining[0] -
                               e->cfg.up[0] *
                                   le_ch_dot(remaining,
                                             e->cfg.up);
                pre_horiz[1] = remaining[1] -
                               e->cfg.up[1] *
                                   le_ch_dot(remaining,
                                             e->cfg.up);
                pre_horiz[2] = remaining[2] -
                               e->cfg.up[2] *
                                   le_ch_dot(remaining,
                                             e->cfg.up);
                if (rlen > 1e-12f) {
                    back = e->cfg.skin_width / rlen;
                }
                if (adv > back) {
                    adv -= back;
                } else {
                    adv = 0.0f;
                }
                world->slots[slot].position[0] +=
                    remaining[0] * adv;
                world->slots[slot].position[1] +=
                    remaining[1] * adv;
                world->slots[slot].position[2] +=
                    remaining[2] * adv;
                total_moved[0] += remaining[0] * adv;
                total_moved[1] += remaining[1] * adv;
                total_moved[2] += remaining[2] * adv;
                le_mark_subtree_dirty(world, slot);
                /* Consume the travelled part + the blocked
                 * part is re-projected below. */
                {
                    float kept[3];

                    kept[0] = remaining[0] * (1.0f - adv);
                    kept[1] = remaining[1] * (1.0f - adv);
                    kept[2] = remaining[2] * (1.0f - adv);
                    remaining[0] = kept[0];
                    remaining[1] = kept[1];
                    remaining[2] = kept[2];
                }
            }
            if (hit.fraction <= LE_CHAR_EPS) {
                /* Started touching: still slide (normal
                 * known), but a step attempt gets priority
                 * for horizontal blocks. */
            }
            /* 5. Step attempt (horizontal block + step budget
             * + headroom + walkable landing). Tried once per
             * slide contact. Uses the PRE-advance horizontal
             * remainder (per-frame moves are small; the
             * post-advance sliver would rarely exceed
             * epsilon). The forward probe additionally gets a
             * minimum reach (skin + radius overhang) so the
             * landing clears the step edge instead of
             * catching its face. */
            {
                float horiz[3];
                float hlen;

                horiz[0] = pre_horiz[0];
                horiz[1] = pre_horiz[1];
                horiz[2] = pre_horiz[2];
                hlen = le_ch_len(horiz);
                if (hlen > LE_CHAR_EPS &&
                    e->cfg.step_height > LE_CHAR_EPS) {
                    float origin2[3];
                    float center2[3];
                    float r2;
                    float h2;
                    float q2[4];
                    float dims2[3];
                    float up_disp[3];
                    le_shape_hit up_hit;
                    int up_free = 0;

                    world->physics->stat_step_attempts++;
                    le_refresh_world_matrices(world);
                    origin2[0] = world->slots[slot]
                                     .position[0];
                    origin2[1] = world->slots[slot]
                                     .position[1];
                    origin2[2] = world->slots[slot]
                                     .position[2];
                    le_ch_capsule(e, origin2, center2, &r2,
                                  &h2);
                    le_ch_up_quat(e->cfg.up, q2);
                    dims2[0] = r2;
                    dims2[1] = h2;
                    dims2[2] = 0.0f;
                    up_disp[0] = e->cfg.up[0] *
                                 (e->cfg.step_height +
                                  e->cfg.skin_width);
                    up_disp[1] = e->cfg.up[1] *
                                 (e->cfg.step_height +
                                  e->cfg.skin_width);
                    up_disp[2] = e->cfg.up[2] *
                                 (e->cfg.step_height +
                                  e->cfg.skin_width);
                    memset(&up_hit, 0, sizeof(up_hit));
                    if (!le_physics_shape_cast(
                            world, LE_CAST_CAPSULE, center2,
                            q2, dims2, up_disp, e->cfg.mask,
                            0, slot, &up_hit, NULL)) {
                        up_free = 1;
                    } else {
                        /* Any headroom block kills the step
                         * (the rise path is obstructed; a
                         * partial rise would wedge the
                         * capsule into the step face). */
                        up_free = 0;
                    }
                    if (up_free) {
                        /* Forward probe from the raised pose
                         * (without committing): raised center
                         * + horizontal remainder. Short
                         * per-frame remainders land the drop
                         * on the step's front EDGE (blended,
                         * unwalkable normal) instead of its
                         * top: extend the probe past the edge
                         * by up to half a radius along the
                         * motion (bounded, swept so no
                         * tunneling, deterministic). The
                         * too-tall guard below (drop must
                         * descend >= skin) still rejects
                         * over-budget obstacles: extension is
                         * horizontal only and never lifts the
                         * landing. */
                        float raised[3];
                        float fwd[3];
                        le_shape_hit fwd_hit;

                        raised[0] = center2[0] + up_disp[0];
                        raised[1] = center2[1] + up_disp[1];
                        raised[2] = center2[2] + up_disp[2];
                        fwd[0] = horiz[0];
                        fwd[1] = horiz[1];
                        fwd[2] = horiz[2];
                        {
                            float flen = le_ch_len(fwd);

                            if (flen > LE_CHAR_EPS) {
                                float ext =
                                    e->cfg.skin_width + r2;

                                if (ext > r2) {
                                    ext = r2;
                                }
                                {
                                    float s =
                                        (flen + ext) / flen;

                                    fwd[0] *= s;
                                    fwd[1] *= s;
                                    fwd[2] *= s;
                                }
                            }
                        }
                        memset(&fwd_hit, 0, sizeof(fwd_hit));
                        if (!le_physics_shape_cast(
                                world, LE_CAST_CAPSULE,
                                raised, q2, dims2, fwd,
                                e->cfg.mask, 0, slot,
                                &fwd_hit, NULL)) {
                            /* Down probe: find the landing
                             * within step_height + snap. */
                            float down[3];
                            float land[3];
                            le_shape_hit dn_hit;
                            int dn_hit_ok;

                            down[0] = -e->cfg.up[0] *
                                      (e->cfg.step_height +
                                       e->cfg.snap_distance +
                                       e->cfg.skin_width *
                                           2.0f);
                            down[1] = -e->cfg.up[1] *
                                      (e->cfg.step_height +
                                       e->cfg.snap_distance +
                                       e->cfg.skin_width *
                                           2.0f);
                            down[2] = -e->cfg.up[2] *
                                      (e->cfg.step_height +
                                       e->cfg.snap_distance +
                                       e->cfg.skin_width *
                                           2.0f);
                            land[0] = raised[0] + fwd[0];
                            land[1] = raised[1] + fwd[1];
                            land[2] = raised[2] + fwd[2];
                            memset(&dn_hit, 0,
                                   sizeof(dn_hit));
                            dn_hit_ok = le_physics_shape_cast(
                                world, LE_CAST_CAPSULE,
                                land, q2, dims2, down,
                                e->cfg.mask, 0, slot,
                                &dn_hit, NULL);
                            if (dn_hit_ok &&
                                le_ch_walkable(e,
                                               dn_hit
                                                   .normal)) {
                                /* Accept the step: commit
                                 * rise + forward + drop. Two
                                 * guards keep the landing
                                 * honest: (a) the drop must
                                 * descend at least skin from
                                 * the raised pose — an
                                 * immediate graze means the
                                 * obstacle reaches the raised
                                 * feet (taller than the step
                                 * budget); (b) stop one skin
                                 * short of the drop contact so
                                 * edge/corner touches (which
                                 * contact BELOW the walkable
                                 * surface) do not embed the
                                 * flank and wedge. The ground
                                 * probe's snap budget covers
                                 * the skin gap. */
                                float drop =
                                    dn_hit.fraction;
                                float down_len =
                                    le_ch_len(down);
                                float skin_frac = 0.0f;

                                if (down_len > 1e-12f) {
                                    skin_frac =
                                        e->cfg.skin_width /
                                        down_len;
                                }
                                if (drop < skin_frac) {
                                    /* Too tall: no step. Fall
                                     * through to sliding. */
                                } else {
                                    float land_feet;
                                    float start_feet;

                                    drop -= skin_frac;
                                    /* A step must gain height:
                                     * landing at/below the
                                     * start feet is a no-op
                                     * drop back to the current
                                     * ground (burns the move and
                                     * re-hits the same face
                                     * forever). */
                                    land_feet =
                                        origin2[1] +
                                        up_disp[1] + fwd[1] +
                                        down[1] * drop;
                                    start_feet = origin2[1];
                                    if (land_feet <=
                                        start_feet +
                                            e->cfg.skin_width) {
                                        /* No gain: slide. */
                                    } else {

                                    world->slots[slot]
                                        .position[0] +=
                                        up_disp[0] + fwd[0] +
                                        down[0] * drop;
                                    world->slots[slot]
                                        .position[1] +=
                                        up_disp[1] + fwd[1] +
                                        down[1] * drop;
                                    world->slots[slot]
                                        .position[2] +=
                                        up_disp[2] + fwd[2] +
                                        down[2] * drop;
                                    total_moved[0] +=
                                        up_disp[0] + fwd[0] +
                                        down[0] * drop;
                                    total_moved[1] +=
                                        up_disp[1] + fwd[1] +
                                        down[1] * drop;
                                    total_moved[2] +=
                                        up_disp[2] + fwd[2] +
                                        down[2] * drop;
                                    le_mark_subtree_dirty(
                                        world, slot);
                                    stepped = 1;
                                    /* Landing is ground. */
                                    e->grounded = 1;
                                    e->ground_normal[0] =
                                        dn_hit.normal[0];
                                    e->ground_normal[1] =
                                        dn_hit.normal[1];
                                    e->ground_normal[2] =
                                        dn_hit.normal[2];
                                    e->ground_slot =
                                        dn_hit.object.index;
                                    e->ground_gen =
                                        dn_hit.object
                                            .generation;
                                    e->has_ground_ref = 0;
                                    remaining[0] =
                                        remaining[1] =
                                            remaining[2] =
                                                0.0f;
                                    break;
                                    } /* gain guard */
                                } /* drop >= skin guard */
                            } /* walkable */
                        } /* fwd free */
                    } /* up free */
                } /* hlen + budget */
            } /* step attempt */
            /* Slide: record the plane, re-clip remaining
             * against ALL planes (corner settle, crease
             * preserved). v_slide = v - n*min(dot,0). */
            if (nplanes < LE_CHAR_MAX_PLANES) {
                planes[nplanes][0] = hit.normal[0];
                planes[nplanes][1] = hit.normal[1];
                planes[nplanes][2] = hit.normal[2];
                nplanes++;
            }
            {
                float v[3];

                v[0] = remaining[0];
                v[1] = remaining[1];
                v[2] = remaining[2];
                {
                    uint32_t p;

                    for (p = 0; p < nplanes; p++) {
                        float d =
                            v[0] * planes[p][0] +
                            v[1] * planes[p][1] +
                            v[2] * planes[p][2];

                        if (d < 0.0f) {
                            v[0] -= planes[p][0] * d;
                            v[1] -= planes[p][1] * d;
                            v[2] -= planes[p][2] * d;
                        }
                    }
                }
                remaining[0] = v[0];
                remaining[1] = v[1];
                remaining[2] = v[2];
            }
            if (le_ch_len(remaining) < LE_CHAR_EPS) {
                remaining[0] = remaining[1] = remaining[2] =
                    0.0f;
                break;
            }
        }
    }
    /* 6. Ground probe + snap. Probe down (snap + skin) from the
     * final pose; walkable hit -> grounded (+ snap the gap,
     * unless rising: fall_velocity > 0 means deliberate upward
     * motion — never snap down). */
    {
        float origin[3];
        float center[3];
        float r;
        float h;
        float q[4];
        float dims[3];
        float down[3];
        le_shape_hit ghit;

        world->physics->stat_ground_probes++;
        le_refresh_world_matrices(world);
        origin[0] = world->slots[slot].position[0];
        origin[1] = world->slots[slot].position[1];
        origin[2] = world->slots[slot].position[2];
        le_ch_capsule(e, origin, center, &r, &h);
        le_ch_up_quat(e->cfg.up, q);
        dims[0] = r;
        dims[1] = h;
        dims[2] = 0.0f;
        down[0] = -e->cfg.up[0] *
                  (e->cfg.snap_distance + e->cfg.skin_width);
        down[1] = -e->cfg.up[1] *
                  (e->cfg.snap_distance + e->cfg.skin_width);
        down[2] = -e->cfg.up[2] *
                  (e->cfg.snap_distance + e->cfg.skin_width);
        memset(&ghit, 0, sizeof(ghit));
        if (e->fall_velocity <= LE_CHAR_EPS &&
            le_physics_shape_cast(
                world, LE_CAST_CAPSULE, center, q, dims, down,
                e->cfg.mask, 0, slot, &ghit, NULL) &&
            le_ch_walkable(e, ghit.normal)) {
            e->grounded = 1;
            e->ground_normal[0] = ghit.normal[0];
            e->ground_normal[1] = ghit.normal[1];
            e->ground_normal[2] = ghit.normal[2];
            e->ground_slot = ghit.object.index;
            e->ground_gen = ghit.object.generation;
            /* Snap the gap (leave skin). */
            {
                float gap = ghit.fraction;
                float dl = le_ch_len(down);
                float want = gap * dl - e->cfg.skin_width;

                if (want > 0.0f && dl > 1e-12f) {
                    float f = want / dl;

                    world->slots[slot].position[0] +=
                        down[0] * f;
                    world->slots[slot].position[1] +=
                        down[1] * f;
                    world->slots[slot].position[2] +=
                        down[2] * f;
                    total_moved[0] += down[0] * f;
                    total_moved[1] += down[1] * f;
                    total_moved[2] += down[2] * f;
                    le_mark_subtree_dirty(world, slot);
                    res.snapped = 1;
                }
            }
            /* Ground platform ref: store its center for ride. */
            {
                uint32_t gs = e->ground_slot;
                le_object gh;

                if (gs < world->capacity &&
                    world->slots[gs].alive &&
                    world->slots[gs].generation ==
                        e->ground_gen) {
                    gh.index = gs;
                    gh.generation = e->ground_gen;
                    gh.world_tag = world->tag;
                    /* Store the platform's world center for ride
                     * delta tracking. */
                    {
                        float wm[16];

                        le_object_get_world_matrix(world,
                                                   &gh, wm);
                        e->ground_center[0] = wm[12];
                        e->ground_center[1] = wm[13];
                        e->ground_center[2] = wm[14];
                        e->has_ground_ref = 1;
                    }
                } else {
                    e->has_ground_ref = 0;
                }
            }
            /* Landing kills downward velocity. */
            if (e->fall_velocity < 0.0f) {
                e->fall_velocity = 0.0f;
            }
        } else {
            /* Probe missed/unwalkable — but a stepped landing
             * already grounded us. */
            if (!e->grounded) {
                e->grounded = 0;
                e->has_ground_ref = 0;
            }
        }
    }
    /* 7. Publish. */
    e->last_motion[0] = total_moved[0];
    e->last_motion[1] = total_moved[1];
    e->last_motion[2] = total_moved[2];
    {
        float upness = le_ch_dot(total_moved, e->cfg.up);

        e->last_horizontal[0] =
            total_moved[0] - e->cfg.up[0] * upness;
        e->last_horizontal[1] =
            total_moved[1] - e->cfg.up[1] * upness;
        e->last_horizontal[2] =
            total_moved[2] - e->cfg.up[2] * upness;
    }
    e->collision_count = collisions;
    res.actual[0] = total_moved[0];
    res.actual[1] = total_moved[1];
    res.actual[2] = total_moved[2];
    res.grounded = e->grounded;
    res.ground_normal[0] = e->ground_normal[0];
    res.ground_normal[1] = e->ground_normal[1];
    res.ground_normal[2] = e->ground_normal[2];
    if (e->grounded) {
        res.ground_object.index = e->ground_slot;
        res.ground_object.generation = e->ground_gen;
        res.ground_object.world_tag = world->tag;
        /* Validate liveness (destroyed platform -> INVALID). */
        if (e->ground_slot >= world->capacity ||
            !world->slots[e->ground_slot].alive ||
            world->slots[e->ground_slot].generation !=
                e->ground_gen) {
            res.ground_object = LE_OBJECT_INVALID;
            e->grounded = 0;
            res.grounded = 0;
        }
    } else {
        res.ground_object = LE_OBJECT_INVALID;
    }
    res.hit_wall = hit_wall;
    res.hit_ceiling = hit_ceiling;
    res.stepped = stepped;
    res.collision_count = collisions;
    le_refresh_world_matrices(world);
    if (out_result != NULL) {
        *out_result = res;
    }
    return LE_SUCCESS;
}

le_result le_character_gravity(le_world *world,
                               const le_object *object,
                               float dt) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;
    struct le_character_entry *e;
    float disp[3];

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!isfinite(dt) || dt < 0.0f || dt > 10.0f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    idx = le_character_entry_index(world, slot);
    if (idx < 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    e = &world->characters[(uint32_t)idx];
    if (!e->enabled) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Integrate fall velocity (fixed-dt; frame-rate
     * independent by construction when called per fixed step).
     * Stored signed along +up (positive = rising, negative =
     * falling); gravity decreases it, clamped to terminal. */
    e->fall_velocity -= e->cfg.gravity * dt;
    if (e->fall_velocity < -e->cfg.terminal_velocity) {
        e->fall_velocity = -e->cfg.terminal_velocity;
    }
    if (!isfinite(e->fall_velocity)) {
        e->fall_velocity = 0.0f;
    }
    disp[0] = e->cfg.up[0] * e->fall_velocity * dt;
    disp[1] = e->cfg.up[1] * e->fall_velocity * dt;
    disp[2] = e->cfg.up[2] * e->fall_velocity * dt;
    /* fall_velocity is signed along +up (positive = rising):
     * displacement = up * fall * dt. */
    return le_character_move(world, object, disp, NULL);
}

int le_character_is_grounded(const le_world *world,
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
    idx = le_character_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return 0;
    }
    {
        const struct le_character_entry *e =
            &world->characters[(uint32_t)idx];

        if (!e->enabled) {
            return 0;
        }
        /* Generation-guard the ground (stale platform). */
        if (e->grounded) {
            if (e->ground_slot >= world->capacity ||
                !world->slots[e->ground_slot].alive ||
                world->slots[e->ground_slot].generation !=
                    e->ground_gen) {
                return 0;
            }
        }
        return e->grounded;
    }
}

void le_character_ground_normal(const le_world *world,
                                const le_object *object,
                                float out_normal[3]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (out_normal != NULL) {
        out_normal[0] = 0.0f;
        out_normal[1] = 1.0f;
        out_normal[2] = 0.0f;
    }
    if (world == NULL || object == NULL ||
        out_normal == NULL) {
        return;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return;
    }
    idx = le_character_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return;
    }
    memcpy(out_normal,
           world->characters[(uint32_t)idx].ground_normal,
           3u * sizeof(float));
}

le_object le_character_ground_object(const le_world *world,
                                     const le_object *object) {
    le_object bad = LE_OBJECT_INVALID;
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (world == NULL || object == NULL) {
        return bad;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return bad;
    }
    idx = le_character_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return bad;
    }
    {
        const struct le_character_entry *e =
            &world->characters[(uint32_t)idx];

        if (!e->grounded) {
            return bad;
        }
        if (e->ground_slot >= world->capacity ||
            !world->slots[e->ground_slot].alive ||
            world->slots[e->ground_slot].generation !=
                e->ground_gen) {
            return bad;
        }
        {
            le_object o;

            o.index = e->ground_slot;
            o.generation = e->ground_gen;
            o.world_tag = world->tag;
            return o;
        }
    }
}

void le_character_get_velocity(const le_world *world,
                               const le_object *object,
                               float out_velocity[3]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    /* Exposed velocity = last actual horizontal motion per
     * move + vertical fall velocity along up. Callers needing
     * per-second velocity divide by their fixed dt. */
    if (out_velocity != NULL) {
        out_velocity[0] = 0.0f;
        out_velocity[1] = 0.0f;
        out_velocity[2] = 0.0f;
    }
    if (world == NULL || object == NULL ||
        out_velocity == NULL) {
        return;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return;
    }
    idx = le_character_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return;
    }
    {
        const struct le_character_entry *e =
            &world->characters[(uint32_t)idx];

        out_velocity[0] =
            e->last_horizontal[0] +
            e->cfg.up[0] * e->fall_velocity;
        out_velocity[1] =
            e->last_horizontal[1] +
            e->cfg.up[1] * e->fall_velocity;
        out_velocity[2] =
            e->last_horizontal[2] +
            e->cfg.up[2] * e->fall_velocity;
    }
}

float le_character_horizontal_speed(
    const le_world *world, const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (world == NULL || object == NULL) {
        return 0.0f;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0.0f;
    }
    idx = le_character_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return 0.0f;
    }
    {
        const struct le_character_entry *e =
            &world->characters[(uint32_t)idx];
        float s = le_ch_len(e->last_horizontal);

        return isfinite(s) ? s : 0.0f;
    }
}

le_result le_character_set_vertical_velocity(
    le_world *world, const le_object *object, float v) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!isfinite(v)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    idx = le_character_entry_index(world, slot);
    if (idx < 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    {
        struct le_character_entry *e =
            &world->characters[(uint32_t)idx];
        float cap = e->cfg.terminal_velocity * 4.0f;

        if (cap < 10.0f) {
            cap = 10.0f;
        }
        if (v < -e->cfg.terminal_velocity) {
            v = -e->cfg.terminal_velocity;
        } else if (v > cap) {
            v = cap;
        }
        e->fall_velocity = v; /* stored along +up */
    }
    return LE_SUCCESS;
}

float le_character_get_vertical_velocity(
    const le_world *world, const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (world == NULL || object == NULL) {
        return 0.0f;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0.0f;
    }
    idx = le_character_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return 0.0f;
    }
    return world->characters[(uint32_t)idx].fall_velocity;
}

le_result le_character_teleport(le_world *world,
                                const le_object *object,
                                const float position[3]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;
    le_result rc;

    if (world == NULL || object == NULL || position == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_ch_finite3(position)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    idx = le_character_entry_index(world, slot);
    if (idx < 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    rc = le_object_set_position(world, object, position);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    {
        struct le_character_entry *e =
            &world->characters[(uint32_t)idx];

        e->fall_velocity = 0.0f;
        e->grounded = 0;
        e->has_ground_ref = 0;
        e->ground_slot = 0xFFFFFFFFu;
        memset(e->last_motion, 0, sizeof(e->last_motion));
        memset(e->last_horizontal, 0,
               sizeof(e->last_horizontal));
    }
    le_refresh_world_matrices(world);
    return LE_SUCCESS;
}

le_result le_character_set_enabled(le_world *world,
                                   const le_object *object,
                                   int enabled) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    idx = le_character_entry_index(world, slot);
    if (idx < 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    world->characters[(uint32_t)idx].enabled =
        enabled ? 1 : 0;
    return LE_SUCCESS;
}

int le_character_is_enabled(const le_world *world,
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
    idx = le_character_entry_index((le_world *)world, slot);
    if (idx < 0) {
        return 0;
    }
    return world->characters[(uint32_t)idx].enabled;
}

void le_character_get_stats(const le_world *world,
                            le_character_stats *out) {
    uint32_t i;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (world == NULL) {
        return;
    }
    for (i = 0; i < world->character_count; i++) {
        const struct le_character_entry *e =
            &world->characters[i];

        out->controller_count++;
        if (e->grounded) {
            out->grounded_count++;
        }
        if (e->has_ground_ref) {
            out->platform_attachments++;
        }
        if (e->unresolved) {
            out->unresolved_penetrations++;
        }
    }
}

/* ---- scene capture/apply/validate (authoring config only) ---- */

void le_character_capture_for_record(le_world *world,
                                     uint32_t slot,
                                     le_scene_object *rec) {
    int idx;

    if (world == NULL || rec == NULL) {
        return;
    }
    rec->has_character = 0;
    if (slot >= world->capacity) {
        return;
    }
    idx = le_character_entry_index(world, slot);
    if (idx < 0) {
        return;
    }
    {
        const struct le_character_entry *e =
            &world->characters[(uint32_t)idx];

        rec->has_character = 1;
        rec->character_radius = e->cfg.radius;
        rec->character_height = e->cfg.height;
        memcpy(rec->character_up, e->cfg.up,
               sizeof(rec->character_up));
        rec->character_skin_width = e->cfg.skin_width;
        rec->character_slope_angle = e->cfg.max_slope_angle;
        rec->character_step_height = e->cfg.step_height;
        rec->character_gravity = e->cfg.gravity;
        rec->character_terminal_velocity =
            e->cfg.terminal_velocity;
        rec->character_snap_distance = e->cfg.snap_distance;
        rec->character_push_strength = e->cfg.push_strength;
        rec->character_layer = e->cfg.layer;
        rec->character_mask = e->cfg.mask;
    }
}

le_result le_character_validate_record(
    const le_scene_object *rec) {
    le_character_desc d;

    if (rec == NULL || !rec->has_character) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memset(&d, 0, sizeof(d));
    d.radius = rec->character_radius;
    d.height = rec->character_height;
    memcpy(d.up, rec->character_up, sizeof(d.up));
    d.skin_width = rec->character_skin_width;
    d.max_slope_angle = rec->character_slope_angle;
    d.step_height = rec->character_step_height;
    d.gravity = rec->character_gravity;
    d.terminal_velocity = rec->character_terminal_velocity;
    d.snap_distance = rec->character_snap_distance;
    d.push_strength = rec->character_push_strength;
    d.layer = rec->character_layer;
    d.mask = rec->character_mask;
    if (!le_ch_desc_valid(&d, NULL)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    return LE_SUCCESS;
}

le_result le_character_apply_record(le_world *world,
                                    const le_object *obj,
                                    const le_scene_object *rec) {
    le_character_desc d;

    if (world == NULL || obj == NULL || rec == NULL ||
        !rec->has_character) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memset(&d, 0, sizeof(d));
    d.radius = rec->character_radius;
    d.height = rec->character_height;
    memcpy(d.up, rec->character_up, sizeof(d.up));
    d.skin_width = rec->character_skin_width;
    d.max_slope_angle = rec->character_slope_angle;
    d.step_height = rec->character_step_height;
    d.gravity = rec->character_gravity;
    d.terminal_velocity = rec->character_terminal_velocity;
    d.snap_distance = rec->character_snap_distance;
    d.push_strength = rec->character_push_strength;
    d.layer = rec->character_layer;
    d.mask = rec->character_mask;
    return le_object_add_character(world, obj, &d);
}
