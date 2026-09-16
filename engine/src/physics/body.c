/*
 * Rigid bodies + colliders (Phase 28): component add/remove/get
 * with the standard dense + swap-remove discipline, strict
 * validation, and the dynamic-must-be-root parenting rule.
 *
 * Policy recap:
 * - Dynamic bodies must be world roots (parented dynamics are
 *   rejected with INVALID_HIERARCHY — no valid local inversion
 *   exists under arbitrary parents).
 * - Collider-only objects behave as static geometry.
 * - Body-only objects integrate but never collide (broad phase
 *   only sees colliders; never crashes on body-only).
 * - Static/kinematic inverse mass is 0; dynamic mass must be
 *   finite and > 0.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

int le_physics_body_index(le_world *world, uint32_t slot) {
    uint32_t idx;

    if (world == NULL || world->physics == NULL ||
        slot >= world->capacity) {
        return -1;
    }
    if (!(world->slots[slot].present & LE_PRESENT_RIGID_BODY)) {
        return -1;
    }
    idx = (uint32_t)world->slots[slot].body_index;
    if (idx >= world->physics->body_count ||
        world->physics->bodies[idx].slot != slot) {
        return -1;
    }
    return (int)idx;
}

int le_physics_collider_index(le_world *world, uint32_t slot) {
    uint32_t idx;

    if (world == NULL || world->physics == NULL ||
        slot >= world->capacity) {
        return -1;
    }
    if (!(world->slots[slot].present & LE_PRESENT_COLLIDER)) {
        return -1;
    }
    idx = (uint32_t)world->slots[slot].collider_index;
    if (idx >= world->physics->collider_count ||
        world->physics->colliders[idx].slot != slot) {
        return -1;
    }
    return (int)idx;
}

static int le_finite3(const float v[3]) {
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

static le_result le_ensure_body_cap(le_world *world) {
    struct le_physics_world *pw = world->physics;
    uint32_t i;

    for (i = 0; i < pw->body_cap; i++) {
        /* Dense: live entries are [0, body_count). */
        (void)i;
    }
    if (pw->bodies == NULL || pw->body_count >= pw->body_cap) {
        uint32_t grown =
            (pw->body_cap == 0) ? 16u : pw->body_cap * 2u;
        le_body_entry *fresh;

        if (grown > 0x00FFFFFFu) {
            return LE_ERROR_OVERFLOW;
        }
        fresh = (le_body_entry *)realloc(
            pw->bodies, (size_t)grown * sizeof(*fresh));
        if (fresh == NULL) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
        pw->bodies = fresh;
        pw->body_cap = grown;
    }
    return LE_SUCCESS;
}

static le_result le_ensure_collider_cap(le_world *world) {
    struct le_physics_world *pw = world->physics;

    if (pw->colliders == NULL ||
        pw->collider_count >= pw->collider_cap) {
        uint32_t grown =
            (pw->collider_cap == 0) ? 16u : pw->collider_cap * 2u;
        le_collider_entry *fresh;

        if (grown > 0x00FFFFFFu) {
            return LE_ERROR_OVERFLOW;
        }
        fresh = (le_collider_entry *)realloc(
            pw->colliders, (size_t)grown * sizeof(*fresh));
        if (fresh == NULL) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
        pw->colliders = fresh;
        pw->collider_cap = grown;
    }
    return LE_SUCCESS;
}

le_result le_object_add_rigid_body(le_world *world,
                                   const le_object *object,
                                   const le_rigid_body_desc *desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_result rc;
    struct le_physics_world *pw;
    le_body_entry *e;

    if (world == NULL || object == NULL || desc == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    if (desc->type != LE_BODY_STATIC &&
        desc->type != LE_BODY_DYNAMIC &&
        desc->type != LE_BODY_KINEMATIC) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_finite3(desc->linear_velocity) ||
        !le_finite3(desc->angular_velocity)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!isfinite(desc->linear_damping) ||
        desc->linear_damping < 0.0f ||
        !isfinite(desc->angular_damping) ||
        desc->angular_damping < 0.0f ||
        !isfinite(desc->gravity_scale)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->type == LE_BODY_DYNAMIC) {
        if (!isfinite(desc->mass) || desc->mass <= 0.0f) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        /* Dynamic bodies must be world roots. */
        if (world->slots[slot].parent != LE_NO_LINK) {
            return LE_ERROR_INVALID_HIERARCHY;
        }
    }
    pw = world->physics;
    /* Replace in place when present (keep velocities unless the
     * new desc says otherwise — replace means full adopt). */
    {
        int existing = le_physics_body_index(world, slot);

        if (existing >= 0) {
            e = &pw->bodies[(uint32_t)existing];
            e->type = desc->type;
            e->mass = (desc->type == LE_BODY_DYNAMIC)
                          ? desc->mass
                          : 0.0f;
            e->inv_mass = (desc->type == LE_BODY_DYNAMIC)
                              ? 1.0f / desc->mass
                              : 0.0f;
            memcpy(e->linear_velocity, desc->linear_velocity,
                   sizeof(e->linear_velocity));
            memcpy(e->angular_velocity, desc->angular_velocity,
                   sizeof(e->angular_velocity));
            memset(e->force, 0, sizeof(e->force));
            memset(e->torque, 0, sizeof(e->torque));
            e->linear_damping = desc->linear_damping;
            e->angular_damping = desc->angular_damping;
            e->gravity_scale = desc->gravity_scale;
            memset(e->inv_inertia, 0, sizeof(e->inv_inertia));
            return LE_SUCCESS;
        }
    }
    rc = le_ensure_body_cap(world);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    rc = le_physics_ensure_events(pw, world->capacity);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    e = &pw->bodies[pw->body_count];
    memset(e, 0, sizeof(*e));
    e->slot = slot;
    e->type = desc->type;
    e->mass =
        (desc->type == LE_BODY_DYNAMIC) ? desc->mass : 0.0f;
    e->inv_mass = (desc->type == LE_BODY_DYNAMIC)
                      ? 1.0f / desc->mass
                      : 0.0f;
    memcpy(e->linear_velocity, desc->linear_velocity,
           sizeof(e->linear_velocity));
    memcpy(e->angular_velocity, desc->angular_velocity,
           sizeof(e->angular_velocity));
    e->linear_damping = desc->linear_damping;
    e->angular_damping = desc->angular_damping;
    e->gravity_scale = desc->gravity_scale;
    world->slots[slot].body_index = (int32_t)pw->body_count;
    world->slots[slot].present |= LE_PRESENT_RIGID_BODY;
    pw->body_count++;
    return LE_SUCCESS;
}

le_result le_object_remove_rigid_body(le_world *world,
                                      const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    struct le_physics_world *pw;
    int idx;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_SUCCESS; /* nothing to remove */
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    pw = world->physics;
    idx = le_physics_body_index(world, slot);
    if (idx < 0) {
        return LE_SUCCESS; /* missing = no-op */
    }
    {
        uint32_t last = pw->body_count - 1u;

        if ((uint32_t)idx != last) {
            pw->bodies[(uint32_t)idx] = pw->bodies[last];
            world->slots[pw->bodies[(uint32_t)idx].slot]
                .body_index = (int32_t)idx;
        }
        pw->body_count--;
    }
    world->slots[slot].body_index = LE_NO_LINK;
    world->slots[slot].present &= ~LE_PRESENT_RIGID_BODY;
    le_physics_retire_slot(world, slot);
    return LE_SUCCESS;
}

int le_object_get_rigid_body(const le_world *world,
                             const le_object *object,
                             le_rigid_body_desc *out_desc) {
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
    idx = le_physics_body_index((le_world *)world, slot);
    if (idx < 0) {
        return 0;
    }
    {
        const le_body_entry *e =
            &world->physics->bodies[(uint32_t)idx];

        out_desc->type = e->type;
        out_desc->mass = e->mass;
        out_desc->linear_damping = e->linear_damping;
        out_desc->angular_damping = e->angular_damping;
        out_desc->gravity_scale = e->gravity_scale;
        memcpy(out_desc->linear_velocity, e->linear_velocity,
               sizeof(out_desc->linear_velocity));
        memcpy(out_desc->angular_velocity, e->angular_velocity,
               sizeof(out_desc->angular_velocity));
        return 1;
    }
}

le_result le_object_add_collider(le_world *world,
                                 const le_object *object,
                                 const le_collider_desc *desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_result rc;
    struct le_physics_world *pw;
    le_collider_entry *e;
    float qlen;

    if (world == NULL || object == NULL || desc == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    if (desc->shape != LE_COLLIDER_SPHERE &&
        desc->shape != LE_COLLIDER_BOX &&
        desc->shape != LE_COLLIDER_CAPSULE) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->shape == LE_COLLIDER_SPHERE) {
        if (!isfinite(desc->radius) || desc->radius <= 0.0f ||
            desc->radius > 1e6f) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    } else if (desc->shape == LE_COLLIDER_CAPSULE) {
        /* Capsule: radius > 0, half cylinder length >= 0
         * (0 = sphere). Both finite and bounded. */
        if (!isfinite(desc->capsule_radius) ||
            desc->capsule_radius <= 0.0f ||
            desc->capsule_radius > 1e6f) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (!isfinite(desc->capsule_half_height) ||
            desc->capsule_half_height < 0.0f ||
            desc->capsule_half_height > 1e6f) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    } else {
        if (!le_finite3(desc->half_extents) ||
            desc->half_extents[0] <= 0.0f ||
            desc->half_extents[1] <= 0.0f ||
            desc->half_extents[2] <= 0.0f ||
            desc->half_extents[0] > 1e6f ||
            desc->half_extents[1] > 1e6f ||
            desc->half_extents[2] > 1e6f) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    }
    if (!le_finite3(desc->offset)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    qlen = desc->orientation[0] * desc->orientation[0] +
           desc->orientation[1] * desc->orientation[1] +
           desc->orientation[2] * desc->orientation[2] +
           desc->orientation[3] * desc->orientation[3];
    if (!isfinite(qlen) || qlen < 1e-12f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->layer > 31u) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!isfinite(desc->friction) || desc->friction < 0.0f ||
        desc->friction > 1e6f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!isfinite(desc->restitution) || desc->restitution < 0.0f ||
        desc->restitution > 1.0f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    pw = world->physics;
    {
        int existing = le_physics_collider_index(world, slot);

        if (existing >= 0) {
            e = &pw->colliders[(uint32_t)existing];
            e->shape = desc->shape;
            e->radius = desc->radius;
            e->capsule_radius = desc->capsule_radius;
            e->capsule_half = desc->capsule_half_height;
            memcpy(e->half_extents, desc->half_extents,
                   sizeof(e->half_extents));
            memcpy(e->offset, desc->offset, sizeof(e->offset));
            {
                float inv = 1.0f / sqrtf(qlen);

                e->orientation[0] = desc->orientation[0] * inv;
                e->orientation[1] = desc->orientation[1] * inv;
                e->orientation[2] = desc->orientation[2] * inv;
                e->orientation[3] = desc->orientation[3] * inv;
            }
            e->is_trigger = desc->is_trigger ? 1 : 0;
            e->layer = desc->layer;
            e->mask = desc->mask;
            e->friction = desc->friction;
            e->restitution = desc->restitution;
            e->aabb_valid = 0;
            return LE_SUCCESS;
        }
    }
    rc = le_ensure_collider_cap(world);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    rc = le_physics_ensure_events(pw, world->capacity);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    e = &pw->colliders[pw->collider_count];
    memset(e, 0, sizeof(*e));
    e->slot = slot;
    e->shape = desc->shape;
    e->radius = desc->radius;
    e->capsule_radius = desc->capsule_radius;
    e->capsule_half = desc->capsule_half_height;
    memcpy(e->half_extents, desc->half_extents,
           sizeof(e->half_extents));
    memcpy(e->offset, desc->offset, sizeof(e->offset));
    {
        float inv = 1.0f / sqrtf(qlen);

        e->orientation[0] = desc->orientation[0] * inv;
        e->orientation[1] = desc->orientation[1] * inv;
        e->orientation[2] = desc->orientation[2] * inv;
        e->orientation[3] = desc->orientation[3] * inv;
    }
    e->is_trigger = desc->is_trigger ? 1 : 0;
    e->layer = desc->layer;
    e->mask = desc->mask;
    e->friction = desc->friction;
    e->restitution = desc->restitution;
    e->aabb_valid = 0;
    world->slots[slot].collider_index = (int32_t)pw->collider_count;
    world->slots[slot].present |= LE_PRESENT_COLLIDER;
    pw->collider_count++;
    return LE_SUCCESS;
}

le_result le_object_remove_collider(le_world *world,
                                    const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    struct le_physics_world *pw;
    int idx;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_SUCCESS;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    pw = world->physics;
    idx = le_physics_collider_index(world, slot);
    if (idx < 0) {
        return LE_SUCCESS;
    }
    {
        uint32_t last = pw->collider_count - 1u;

        if ((uint32_t)idx != last) {
            pw->colliders[(uint32_t)idx] = pw->colliders[last];
            world->slots[pw->colliders[(uint32_t)idx].slot]
                .collider_index = (int32_t)idx;
        }
        pw->collider_count--;
    }
    world->slots[slot].collider_index = LE_NO_LINK;
    world->slots[slot].present &= ~LE_PRESENT_COLLIDER;
    le_physics_retire_slot(world, slot);
    return LE_SUCCESS;
}

int le_object_get_collider(const le_world *world,
                           const le_object *object,
                           le_collider_desc *out_desc) {
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
    idx = le_physics_collider_index((le_world *)world, slot);
    if (idx < 0) {
        return 0;
    }
    {
        const le_collider_entry *e =
            &world->physics->colliders[(uint32_t)idx];

        out_desc->shape = e->shape;
        out_desc->radius = e->radius;
        out_desc->capsule_radius = e->capsule_radius;
        out_desc->capsule_half_height = e->capsule_half;
        memcpy(out_desc->half_extents, e->half_extents,
               sizeof(out_desc->half_extents));
        memcpy(out_desc->offset, e->offset,
               sizeof(out_desc->offset));
        memcpy(out_desc->orientation, e->orientation,
               sizeof(out_desc->orientation));
        out_desc->is_trigger = e->is_trigger;
        out_desc->layer = e->layer;
        out_desc->mask = e->mask;
        out_desc->friction = e->friction;
        out_desc->restitution = e->restitution;
        return 1;
    }
}
