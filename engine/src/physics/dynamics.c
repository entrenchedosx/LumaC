/*
 * Per-frame dynamics API (Phase 28): force/torque accumulators
 * (cleared after each step), immediate impulses (center + point),
 * velocity get/set, teleport, gravity, solver iterations.
 *
 * Conventions: forces in newtons accumulate until the next step;
 * impulses apply dv = J*inv_mass immediately (point impulses add
 * dw = I^-1 (r x J)); static/kinematic/missing bodies ignore
 * dynamic-only calls with SUCCESS (queries still validate).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

static int le_dynamic_entry(le_world *world, const le_object *object,
                            uint32_t *out_slot,
                            le_body_entry **out_entry,
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
        return 0;
    }
    if (world->physics == NULL) {
        if (out_code != NULL) {
            *out_code = LE_ERROR_NOT_INITIALIZED;
        }
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        if (out_code != NULL) {
            *out_code = code;
        }
        return 0;
    }
    if (out_slot != NULL) {
        *out_slot = slot;
    }
    idx = le_physics_body_index(world, slot);
    if (idx < 0) {
        return 0; /* bodyless: caller decides SUCCESS-ignore */
    }
    if (out_entry != NULL) {
        *out_entry = &world->physics->bodies[(uint32_t)idx];
    }
    return 1;
}

le_result le_physics_add_force(le_world *world,
                               const le_object *object, float fx,
                               float fy, float fz) {
    le_body_entry *e = NULL;
    le_result code = LE_SUCCESS;

    if (!isfinite(fx) || !isfinite(fy) || !isfinite(fz)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_dynamic_entry(world, object, NULL, &e, &code)) {
        /* NULL/stale fail; bodyless/foreign-world per code. */
        if (code != LE_SUCCESS) {
            return code;
        }
        return LE_SUCCESS;
    }
    if (e->type != LE_BODY_DYNAMIC) {
        return LE_SUCCESS; /* static/kinematic ignore forces */
    }
    e->force[0] += fx;
    e->force[1] += fy;
    e->force[2] += fz;
    return LE_SUCCESS;
}

le_result le_physics_add_torque(le_world *world,
                                const le_object *object, float tx,
                                float ty, float tz) {
    le_body_entry *e = NULL;
    le_result code = LE_SUCCESS;

    if (!isfinite(tx) || !isfinite(ty) || !isfinite(tz)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_dynamic_entry(world, object, NULL, &e, &code)) {
        if (code != LE_SUCCESS) {
            return code;
        }
        return LE_SUCCESS;
    }
    if (e->type != LE_BODY_DYNAMIC) {
        return LE_SUCCESS;
    }
    e->torque[0] += tx;
    e->torque[1] += ty;
    e->torque[2] += tz;
    return LE_SUCCESS;
}

le_result le_physics_apply_impulse(le_world *world,
                                   const le_object *object, float jx,
                                   float jy, float jz) {
    le_body_entry *e = NULL;
    le_result code = LE_SUCCESS;

    if (!isfinite(jx) || !isfinite(jy) || !isfinite(jz)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_dynamic_entry(world, object, NULL, &e, &code)) {
        if (code != LE_SUCCESS) {
            return code;
        }
        return LE_SUCCESS;
    }
    if (e->type != LE_BODY_DYNAMIC) {
        return LE_SUCCESS;
    }
    e->linear_velocity[0] += jx * e->inv_mass;
    e->linear_velocity[1] += jy * e->inv_mass;
    e->linear_velocity[2] += jz * e->inv_mass;
    return LE_SUCCESS;
}

le_result le_physics_apply_impulse_at_point(
    le_world *world, const le_object *object, float jx, float jy,
    float jz, float px, float py, float pz) {
    le_body_entry *e = NULL;
    le_result code = LE_SUCCESS;
    uint32_t slot = 0;

    if (!isfinite(jx) || !isfinite(jy) || !isfinite(jz) ||
        !isfinite(px) || !isfinite(py) || !isfinite(pz)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_dynamic_entry(world, object, &slot, &e, &code)) {
        if (code != LE_SUCCESS) {
            return code;
        }
        return LE_SUCCESS;
    }
    if (e->type != LE_BODY_DYNAMIC) {
        return LE_SUCCESS;
    }
    e->linear_velocity[0] += jx * e->inv_mass;
    e->linear_velocity[1] += jy * e->inv_mass;
    e->linear_velocity[2] += jz * e->inv_mass;
    /* Angular: dw = I^-1 (r x J), r = point - center of mass.
     * Center of mass = shape world center (collider-aware when
     * present, else the object's world position). */
    {
        float cx;
        float cy;
        float cz;
        float rx;
        float ry;
        float rz;
        float tx;
        float ty;
        float tz;

        {
            float wm[16];

            le_object_get_world_matrix(world,
                                       (const le_object *)object,
                                       wm);
            cx = wm[12];
            cy = wm[13];
            cz = wm[14];
        }
        {
            int ci = le_physics_collider_index(world, slot);

            if (ci >= 0) {
                const le_collider_entry *c =
                    &world->physics->colliders[(uint32_t)ci];

                if (c->aabb_valid) {
                    cx = c->world_center[0];
                    cy = c->world_center[1];
                    cz = c->world_center[2];
                }
            }
        }
        rx = px - cx;
        ry = py - cy;
        rz = pz - cz;
        /* r x J */
        tx = ry * jz - rz * jy;
        ty = rz * jx - rx * jz;
        tz = rx * jy - ry * jx;
        e->angular_velocity[0] += tx * e->inv_inertia[0];
        e->angular_velocity[1] += ty * e->inv_inertia[1];
        e->angular_velocity[2] += tz * e->inv_inertia[2];
    }
    return LE_SUCCESS;
}

le_result le_physics_set_linear_velocity(le_world *world,
                                         const le_object *object,
                                         float vx, float vy,
                                         float vz) {
    le_body_entry *e = NULL;
    le_result code = LE_SUCCESS;

    if (!isfinite(vx) || !isfinite(vy) || !isfinite(vz)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_dynamic_entry(world, object, NULL, &e, &code)) {
        if (code != LE_SUCCESS) {
            return code;
        }
        return LE_ERROR_INVALID_ARGUMENT;
    }
    e->linear_velocity[0] = vx;
    e->linear_velocity[1] = vy;
    e->linear_velocity[2] = vz;
    return LE_SUCCESS;
}

le_result le_physics_get_linear_velocity(const le_world *world,
                                         const le_object *object,
                                         float out_v[3]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (out_v != NULL) {
        out_v[0] = out_v[1] = out_v[2] = 0.0f;
    }
    if (world == NULL || object == NULL || out_v == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    idx = le_physics_body_index((le_world *)world, slot);
    if (idx < 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memcpy(out_v,
           world->physics->bodies[(uint32_t)idx].linear_velocity,
           3u * sizeof(float));
    return LE_SUCCESS;
}

le_result le_physics_set_angular_velocity(le_world *world,
                                          const le_object *object,
                                          float wx, float wy,
                                          float wz) {
    le_body_entry *e = NULL;
    le_result code = LE_SUCCESS;

    if (!isfinite(wx) || !isfinite(wy) || !isfinite(wz)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_dynamic_entry(world, object, NULL, &e, &code)) {
        if (code != LE_SUCCESS) {
            return code;
        }
        return LE_ERROR_INVALID_ARGUMENT;
    }
    e->angular_velocity[0] = wx;
    e->angular_velocity[1] = wy;
    e->angular_velocity[2] = wz;
    return LE_SUCCESS;
}

le_result le_physics_get_angular_velocity(const le_world *world,
                                          const le_object *object,
                                          float out_w[3]) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (out_w != NULL) {
        out_w[0] = out_w[1] = out_w[2] = 0.0f;
    }
    if (world == NULL || object == NULL || out_w == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    idx = le_physics_body_index((le_world *)world, slot);
    if (idx < 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memcpy(out_w,
           world->physics->bodies[(uint32_t)idx].angular_velocity,
           3u * sizeof(float));
    return LE_SUCCESS;
}

le_result le_physics_teleport(le_world *world,
                              const le_object *object,
                              const float position[3],
                              const float rotation[4],
                              int clear_velocity) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_result rc;
    int idx;

    if (world == NULL || object == NULL || position == NULL ||
        rotation == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    idx = le_physics_body_index(world, slot);
    if (idx < 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Teleport is meaningful for dynamics (kinematics/statics
     * move via plain setters); still honor it for any body so
     * the operation is total and explicit. */
    rc = le_object_set_position(world, object, position);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    rc = le_object_set_rotation(world, object, rotation);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    le_refresh_world_matrices(world);
    if (clear_velocity) {
        le_body_entry *e = &world->physics->bodies[(uint32_t)idx];

        memset(e->linear_velocity, 0, sizeof(e->linear_velocity));
        memset(e->angular_velocity, 0,
               sizeof(e->angular_velocity));
        memset(e->force, 0, sizeof(e->force));
        memset(e->torque, 0, sizeof(e->torque));
    }
    /* Collider AABBs refresh next step (aabb_valid cleared so no
     * stale bound survives the teleport). */
    {
        int ci = le_physics_collider_index(world, slot);

        if (ci >= 0) {
            world->physics->colliders[(uint32_t)ci].aabb_valid =
                0;
        }
    }
    return LE_SUCCESS;
}

le_result le_physics_set_gravity(le_world *world, float gx,
                                 float gy, float gz) {
    if (world == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    if (!isfinite(gx) || !isfinite(gy) || !isfinite(gz)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (gx > 1e6f || gx < -1e6f || gy > 1e6f || gy < -1e6f ||
        gz > 1e6f || gz < -1e6f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    world->physics->gravity[0] = gx;
    world->physics->gravity[1] = gy;
    world->physics->gravity[2] = gz;
    return LE_SUCCESS;
}

void le_physics_get_gravity(const le_world *world, float out_g[3]) {
    if (out_g != NULL) {
        out_g[0] = 0.0f;
        out_g[1] = -9.81f;
        out_g[2] = 0.0f;
    }
    if (world == NULL || world->physics == NULL ||
        out_g == NULL) {
        return;
    }
    memcpy(out_g, world->physics->gravity, 3u * sizeof(float));
}

le_result le_physics_set_iterations(le_world *world,
                                    uint32_t velocity_iters,
                                    uint32_t position_iters) {
    if (world == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    if (velocity_iters != 0) {
        if (velocity_iters > LE_PHYS_MAX_ITERS) {
            velocity_iters = LE_PHYS_MAX_ITERS;
        }
        world->physics->velocity_iters = velocity_iters;
    }
    if (position_iters != 0) {
        if (position_iters > LE_PHYS_MAX_ITERS) {
            position_iters = LE_PHYS_MAX_ITERS;
        }
        world->physics->position_iters = position_iters;
    }
    return LE_SUCCESS;
}

void le_physics_get_iterations(const le_world *world,
                               uint32_t *out_velocity,
                               uint32_t *out_position) {
    if (out_velocity != NULL) {
        *out_velocity = LE_PHYS_DEFAULT_VELOCITY_ITERS;
    }
    if (out_position != NULL) {
        *out_position = LE_PHYS_DEFAULT_POSITION_ITERS;
    }
    if (world == NULL || world->physics == NULL) {
        return;
    }
    if (out_velocity != NULL) {
        *out_velocity = world->physics->velocity_iters;
    }
    if (out_position != NULL) {
        *out_position = world->physics->position_iters;
    }
}

/* Collision mode (Phase 30 CCD foundation). Default DISCRETE;
 * only dynamic bodies meaningfully use CONTINUOUS (accepted for
 * any body, stored regardless — the stepper only sweeps
 * dynamics). */
le_result le_physics_set_collision_mode(
    le_world *world, const le_object *object,
    le_collision_mode mode) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    if (mode != LE_COLLISION_DISCRETE &&
        mode != LE_COLLISION_CONTINUOUS) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    idx = le_physics_body_index(world, slot);
    if (idx < 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    world->physics->bodies[(uint32_t)idx].ccd =
        (mode == LE_COLLISION_CONTINUOUS) ? 1 : 0;
    return LE_SUCCESS;
}

le_collision_mode le_physics_get_collision_mode(
    const le_world *world, const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int idx;

    if (world == NULL || object == NULL) {
        return LE_COLLISION_DISCRETE;
    }
    if (world->physics == NULL) {
        return LE_COLLISION_DISCRETE;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return LE_COLLISION_DISCRETE;
    }
    idx = le_physics_body_index((le_world *)world, slot);
    if (idx < 0) {
        return LE_COLLISION_DISCRETE;
    }
    return world->physics->bodies[(uint32_t)idx].ccd
               ? LE_COLLISION_CONTINUOUS
               : LE_COLLISION_DISCRETE;
}
