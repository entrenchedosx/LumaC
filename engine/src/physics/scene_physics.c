/*
 * Physics scene capture/apply (Phase 28): authoring state only
 * (never accumulators, contacts, solver caches). Follows the
 * script record pattern (persistent IDs + bounded values +
 * transactional commit; velocities persist as explicit
 * authoring state — designers see what they simulate).
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

void le_physics_capture_for_record(le_world *world, uint32_t slot,
                                   le_scene_object *rec) {
    if (world == NULL || rec == NULL || slot >= world->capacity) {
        return;
    }
    rec->has_rigid_body = 0;
    rec->has_collider = 0;
    if (!(world->slots[slot].present & LE_PRESENT_RIGID_BODY)) {
        /* No body — but a collider may still exist. */
    } else {
        int idx = le_physics_body_index(world, slot);

        if (idx >= 0) {
            const le_body_entry *e =
                &world->physics->bodies[(uint32_t)idx];

            rec->has_rigid_body = 1;
            rec->body_type = e->type;
            rec->body_mass = e->mass;
            rec->body_linear_damping = e->linear_damping;
            rec->body_angular_damping = e->angular_damping;
            rec->body_gravity_scale = e->gravity_scale;
            memcpy(rec->body_linear_velocity,
                   e->linear_velocity,
                   sizeof(rec->body_linear_velocity));
            memcpy(rec->body_angular_velocity,
                   e->angular_velocity,
                   sizeof(rec->body_angular_velocity));
        }
    }
    if (!(world->slots[slot].present & LE_PRESENT_COLLIDER)) {
        return;
    }
    {
        int idx = le_physics_collider_index(world, slot);

        if (idx < 0) {
            return;
        }
        {
            const le_collider_entry *c =
                &world->physics->colliders[(uint32_t)idx];

            rec->has_collider = 1;
            rec->collider_shape = c->shape;
            rec->collider_radius = c->radius;
            rec->collider_capsule_radius = c->capsule_radius;
            rec->collider_capsule_half = c->capsule_half;
            memcpy(rec->collider_half_extents, c->half_extents,
                   sizeof(rec->collider_half_extents));
            memcpy(rec->collider_offset, c->offset,
                   sizeof(rec->collider_offset));
            memcpy(rec->collider_orientation, c->orientation,
                   sizeof(rec->collider_orientation));
            rec->collider_is_trigger = c->is_trigger;
            rec->collider_layer = c->layer;
            rec->collider_mask = c->mask;
            rec->collider_friction = c->friction;
            rec->collider_restitution = c->restitution;
        }
    }
}

/* Validate one record's physics (PASS1 shape; INVALID_ARGUMENT
 * on any malformed value — transactional load). */
le_result le_physics_validate_record(const le_scene_object *rec) {
    if (rec == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (rec->has_rigid_body) {
        if (rec->body_type != LE_BODY_STATIC &&
            rec->body_type != LE_BODY_DYNAMIC &&
            rec->body_type != LE_BODY_KINEMATIC) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (rec->body_type == LE_BODY_DYNAMIC) {
            if (!(rec->body_mass > 0.0f) ||
                !(rec->body_mass < 1e30f)) {
                return LE_ERROR_INVALID_ARGUMENT;
            }
        }
        if (!(rec->body_linear_damping >= 0.0f) ||
            !(rec->body_linear_damping < 1e9f) ||
            !(rec->body_angular_damping >= 0.0f) ||
            !(rec->body_angular_damping < 1e9f)) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        {
            int k;

            for (k = 0; k < 3; k++) {
                if (rec->body_linear_velocity[k] !=
                        rec->body_linear_velocity[k] ||
                    rec->body_linear_velocity[k] > 1e9f ||
                    rec->body_linear_velocity[k] < -1e9f ||
                    rec->body_angular_velocity[k] !=
                        rec->body_angular_velocity[k] ||
                    rec->body_angular_velocity[k] > 1e9f ||
                    rec->body_angular_velocity[k] < -1e9f) {
                    return LE_ERROR_INVALID_ARGUMENT;
                }
            }
        }
    }
    if (rec->has_collider) {
        if (rec->collider_shape != LE_COLLIDER_SPHERE &&
            rec->collider_shape != LE_COLLIDER_BOX &&
            rec->collider_shape != LE_COLLIDER_CAPSULE) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (rec->collider_shape == LE_COLLIDER_SPHERE) {
            if (!(rec->collider_radius > 0.0f) ||
                !(rec->collider_radius <= 1e6f)) {
                return LE_ERROR_INVALID_ARGUMENT;
            }
        } else if (rec->collider_shape ==
                   LE_COLLIDER_CAPSULE) {
            if (!(rec->collider_capsule_radius > 0.0f) ||
                !(rec->collider_capsule_radius <= 1e6f)) {
                return LE_ERROR_INVALID_ARGUMENT;
            }
            if (!(rec->collider_capsule_half >= 0.0f) ||
                !(rec->collider_capsule_half <= 1e6f)) {
                return LE_ERROR_INVALID_ARGUMENT;
            }
        } else {
            int k;

            for (k = 0; k < 3; k++) {
                if (!(rec->collider_half_extents[k] > 0.0f) ||
                    !(rec->collider_half_extents[k] <=
                      1e6f)) {
                    return LE_ERROR_INVALID_ARGUMENT;
                }
            }
        }
        if (rec->collider_layer > 31u) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (!(rec->collider_friction >= 0.0f) ||
            !(rec->collider_friction <= 1e6f)) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (!(rec->collider_restitution >= 0.0f) ||
            !(rec->collider_restitution <= 1.0f)) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        {
            float qlen =
                rec->collider_orientation[0] *
                    rec->collider_orientation[0] +
                rec->collider_orientation[1] *
                    rec->collider_orientation[1] +
                rec->collider_orientation[2] *
                    rec->collider_orientation[2] +
                rec->collider_orientation[3] *
                    rec->collider_orientation[3];

            if (!(qlen > 1e-12f)) {
                return LE_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    return LE_SUCCESS;
}

le_result le_physics_apply_record(le_world *world,
                                  const le_object *obj,
                                  const le_scene_object *rec) {
    le_result rc;

    if (world == NULL || obj == NULL || rec == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (rec->has_rigid_body) {
        le_rigid_body_desc d;

        memset(&d, 0, sizeof(d));
        d.type = rec->body_type;
        d.mass = rec->body_mass;
        d.linear_damping = rec->body_linear_damping;
        d.angular_damping = rec->body_angular_damping;
        d.gravity_scale = rec->body_gravity_scale;
        memcpy(d.linear_velocity, rec->body_linear_velocity,
               sizeof(d.linear_velocity));
        memcpy(d.angular_velocity, rec->body_angular_velocity,
               sizeof(d.angular_velocity));
        rc = le_object_add_rigid_body(world, obj, &d);
        if (rc != LE_SUCCESS) {
            return rc;
        }
    }
    if (rec->has_collider) {
        le_collider_desc d;

        memset(&d, 0, sizeof(d));
        d.shape = rec->collider_shape;
        d.radius = rec->collider_radius;
        d.capsule_radius = rec->collider_capsule_radius;
        d.capsule_half_height = rec->collider_capsule_half;
        memcpy(d.half_extents, rec->collider_half_extents,
               sizeof(d.half_extents));
        memcpy(d.offset, rec->collider_offset,
               sizeof(d.offset));
        memcpy(d.orientation, rec->collider_orientation,
               sizeof(d.orientation));
        d.is_trigger = rec->collider_is_trigger;
        d.layer = rec->collider_layer;
        d.mask = rec->collider_mask;
        d.friction = rec->collider_friction;
        d.restitution = rec->collider_restitution;
        rc = le_object_add_collider(world, obj, &d);
        if (rc != LE_SUCCESS) {
            return rc;
        }
    }
    return LE_SUCCESS;
}
