/*
 * Fixed-step physics driver (Phase 28): ordered phases 2..8.
 *
 * Called once per fixed interval from the PASS2 loop AFTER
 * fixed_update scripts (they apply forces/impulses first):
 *   2. gravity + force accumulators -> velocities (dynamics)
 *   3. integrate velocities -> positions + orientations
 *      (semi-implicit Euler; quaternion integration normalized)
 *   4. kinematic follow (transform -> body) + velocity estimate
 *      (capped, documented) for pushing dynamics
 *   5. broad phase + narrow phase -> contact manifolds
 *   6. sequential-impulse solve
 *   7. physics -> engine transform sync (roots; dirty marks)
 *   8. ENTER/STAY/EXIT (+ trigger) event finalization
 *
 * Paused worlds never reach here (lifecycle short-circuits);
 * time_scale 0 yields no fixed intervals (accumulator grows by
 * 0); single-step preloads exactly one interval. No second
 * timer exists anywhere.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

/* Ensure contact scratch for up to LE_PHYS_MAX_CONTACTS. */
static int le_ensure_contacts(le_world *world) {
    struct le_physics_world *pw = world->physics;

    if (pw->contacts == NULL || pw->contact_cap == 0) {
        pw->contacts = (le_contact_point *)calloc(
            LE_PHYS_MAX_CONTACTS, sizeof(*pw->contacts));
        if (pw->contacts == NULL) {
            return 0;
        }
        pw->contact_cap = LE_PHYS_MAX_CONTACTS;
    }
    return 1;
}

static int le_ensure_pairs(uint32_t **pa, uint32_t **pb,
                           uint32_t *cap, uint32_t need) {
    if (*cap < need) {
        uint32_t grown = (*cap == 0) ? 256u : *cap;
        uint32_t *fa;
        uint32_t *fb;

        while (grown < need) {
            if (grown > 0x00FFFFFFu / 2u) {
                grown = need;
                break;
            }
            grown *= 2u;
        }
        fa = (uint32_t *)realloc(*pa, (size_t)grown *
                                           sizeof(*fa));
        if (fa == NULL) {
            return 0;
        }
        fb = (uint32_t *)realloc(*pb, (size_t)grown *
                                           sizeof(*fb));
        if (fb == NULL) {
            free(fa);
            return 0;
        }
        *pa = fa;
        *pb = fb;
        *cap = grown;
    }
    return 1;
}

/* Integrate one dynamic body (semi-implicit Euler + damping +
 * gravity; quaternion orientation integration, normalized). */
static void le_integrate_body(le_world *world, le_body_entry *b,
                              float dt) {
    uint32_t slot = b->slot;
    float *pos;
    float *rot;

    /* Gravity + forces -> velocity. */
    {
        struct le_physics_world *pw = world->physics;

        b->linear_velocity[0] +=
            (pw->gravity[0] * b->gravity_scale +
             b->force[0] * b->inv_mass) *
            dt;
        b->linear_velocity[1] +=
            (pw->gravity[1] * b->gravity_scale +
             b->force[1] * b->inv_mass) *
            dt;
        b->linear_velocity[2] +=
            (pw->gravity[2] * b->gravity_scale +
             b->force[2] * b->inv_mass) *
            dt;
        b->angular_velocity[0] +=
            b->torque[0] * b->inv_inertia[0] * dt;
        b->angular_velocity[1] +=
            b->torque[1] * b->inv_inertia[1] * dt;
        b->angular_velocity[2] +=
            b->torque[2] * b->inv_inertia[2] * dt;
        /* Damping: v *= 1/(1+d*dt) (stable for any dt). */
        {
            float kl =
                1.0f / (1.0f + b->linear_damping * dt);
            float ka =
                1.0f / (1.0f + b->angular_damping * dt);

            b->linear_velocity[0] *= kl;
            b->linear_velocity[1] *= kl;
            b->linear_velocity[2] *= kl;
            b->angular_velocity[0] *= ka;
            b->angular_velocity[1] *= ka;
            b->angular_velocity[2] *= ka;
        }
        /* Sanitize: one NaN body must never poison the world. */
        if (!isfinite(b->linear_velocity[0]) ||
            !isfinite(b->linear_velocity[1]) ||
            !isfinite(b->linear_velocity[2])) {
            b->linear_velocity[0] = b->linear_velocity[1] =
                b->linear_velocity[2] = 0.0f;
        }
        if (!isfinite(b->angular_velocity[0]) ||
            !isfinite(b->angular_velocity[1]) ||
            !isfinite(b->angular_velocity[2])) {
            b->angular_velocity[0] = b->angular_velocity[1] =
                b->angular_velocity[2] = 0.0f;
        }
        memset(b->force, 0, sizeof(b->force));
        memset(b->torque, 0, sizeof(b->torque));
    }
    if (slot >= world->capacity ||
        !world->slots[slot].alive) {
        return;
    }
    pos = world->slots[slot].position;
    rot = world->slots[slot].rotation;
    /* Dynamics are roots by rule: local == world offset. */
    pos[0] += b->linear_velocity[0] * dt;
    pos[1] += b->linear_velocity[1] * dt;
    pos[2] += b->linear_velocity[2] * dt;
    if (!isfinite(pos[0]) || !isfinite(pos[1]) ||
        !isfinite(pos[2])) {
        pos[0] = pos[1] = pos[2] = 0.0f;
        b->linear_velocity[0] = b->linear_velocity[1] =
            b->linear_velocity[2] = 0.0f;
    }
    /* Quaternion integration: q += 0.5 * w_quat * q, then
     * normalize (keeps unit under any angular velocity). */
    {
        float wx = b->angular_velocity[0];
        float wy = b->angular_velocity[1];
        float wz = b->angular_velocity[2];
        float qx = rot[0];
        float qy = rot[1];
        float qz = rot[2];
        float qw = rot[3];
        float hx = 0.5f * dt * wx;
        float hy = 0.5f * dt * wy;
        float hz = 0.5f * dt * wz;
        float nqx = qx + hx * qw + hy * qz - hz * qy;
        float nqy = qy - hx * qz + hy * qw + hz * qx;
        float nqz = qz + hx * qy - hy * qx + hz * qw;
        float nqw = qw - hx * qx - hy * qy - hz * qz;
        float n = sqrtf(nqx * nqx + nqy * nqy + nqz * nqz +
                        nqw * nqw);

        if (n > 1e-9f && isfinite(n)) {
            rot[0] = nqx / n;
            rot[1] = nqy / n;
            rot[2] = nqz / n;
            rot[3] = nqw / n;
        } else {
            rot[0] = 0.0f;
            rot[1] = 0.0f;
            rot[2] = 0.0f;
            rot[3] = 1.0f;
        }
    }
    le_mark_subtree_dirty(world, slot);
}

/* Kinematic follow: body velocity estimated from transform
 * motion over dt (capped — teleport spikes do not create
 * enormous fake velocities). The estimate drives contact
 * response against dynamics (infinite mass pushes). */
static void le_follow_kinematic(le_world *world, le_body_entry *b,
                                float dt) {
    uint32_t slot = b->slot;
    float wm[16];
    le_object h;

    if (slot >= world->capacity ||
        !world->slots[slot].alive || dt <= 0.0f) {
        return;
    }
    h.index = slot;
    h.generation = world->slots[slot].generation;
    h.world_tag = world->tag;
    le_object_get_world_matrix(world, &h, wm);
    /* Velocity = (new_center - old_center) / dt, where the old
     * center is reconstructed by backing out last step's
     * velocity (first step after creation yields exact 0). */
    {
        float cx = wm[12];
        float cy = wm[13];
        float cz = wm[14];
        /* We store last center in force[] scratch (forces are
         * always zero for kinematics — documented reuse, set
         * below on first follow). */
        float vx = (cx - b->force[0]) / dt;
        float vy = (cy - b->force[1]) / dt;
        float vz = (cz - b->force[2]) / dt;
        float vl =
            sqrtf(vx * vx + vy * vy + vz * vz);

        if (!isfinite(vl)) {
            vx = vy = vz = 0.0f;
            vl = 0.0f;
        }
        if (vl > LE_PHYS_KINEMATIC_VELOCITY_CAP) {
            float s = LE_PHYS_KINEMATIC_VELOCITY_CAP / vl;

            vx *= s;
            vy *= s;
            vz *= s;
        }
        b->linear_velocity[0] = vx;
        b->linear_velocity[1] = vy;
        b->linear_velocity[2] = vz;
        b->force[0] = cx;
        b->force[1] = cy;
        b->force[2] = cz;
    }
}

void le_physics_step(le_world *world, float dt) {
    struct le_physics_world *pw;
    uint32_t i;

    if (world == NULL || world->physics == NULL) {
        return;
    }
    if (!isfinite(dt) || dt <= 0.0f) {
        return;
    }
    pw = world->physics;
    pw->contact_count = 0;
    pw->stat_candidates = 0;
    pw->stat_narrow_tests = 0;
    /* Phase 2+3: integrate dynamics; follow kinematics. Body
     * inertia refreshes from collider shapes each step (cheap
     * closed forms; scale documented as local-frame). */
    for (i = 0; i < pw->body_count; i++) {
        le_body_entry *b = &pw->bodies[i];

        if (b->slot >= world->capacity ||
            !world->slots[b->slot].alive) {
            continue;
        }
        if (b->type == LE_BODY_DYNAMIC) {
            int ci = le_physics_collider_index(world, b->slot);

            if (ci >= 0) {
                le_physics_shape_inertia(
                    &pw->colliders[(uint32_t)ci], b->mass,
                    b->inv_inertia);
            } else {
                /* Body-only: point mass (no rotation). */
                b->inv_inertia[0] = b->inv_inertia[1] =
                    b->inv_inertia[2] = 0.0f;
            }
            le_integrate_body(world, b, dt);
        } else if (b->type == LE_BODY_KINEMATIC) {
            le_follow_kinematic(world, b, dt);
        }
    }
    le_refresh_world_matrices(world);
    /* Phase 4+5: broad phase -> narrow phase -> manifolds. */
    if (pw->collider_count >= 2 && le_ensure_contacts(world)) {
        uint32_t need =
            (pw->collider_count < LE_PHYS_MAX_PAIRS)
                ? pw->collider_count
                : LE_PHYS_MAX_PAIRS;
        uint32_t pairs = 0;

        if (le_ensure_pairs(&pw->pair_a, &pw->pair_b,
                            &pw->pair_cap, need)) {
            pairs = le_physics_broadphase(world, pw->pair_a,
                                          pw->pair_b, need);
        }
        for (i = 0; i < pairs; i++) {
            le_collider_entry *a =
                &pw->colliders[pw->pair_a[i]];
            le_collider_entry *bc =
                &pw->colliders[pw->pair_b[i]];
            le_contact_point cp;

            if (!a->aabb_valid || !bc->aabb_valid) {
                continue;
            }
            pw->stat_narrow_tests++;
            if (le_physics_narrow_pair(world, a, bc, &cp) == 0) {
                continue;
            }
            if (pw->contact_count >= LE_PHYS_MAX_CONTACTS) {
                break;
            }
            /* Canonical pair identity (slot, generation). */
            {
                uint32_t sa = a->slot;
                uint32_t sbb = bc->slot;

                if (sa > sbb ||
                    (sa == sbb &&
                     world->slots[sa].generation >
                         world->slots[sbb].generation)) {
                    /* Swap to canonical (slot_a <= slot_b)
                     * and flip the normal (A -> B). */
                    cp.slot_a = sbb;
                    cp.slot_b = sa;
                    cp.gen_a =
                        world->slots[sbb].generation;
                    cp.gen_b = world->slots[sa].generation;
                    cp.normal[0] = -cp.normal[0];
                    cp.normal[1] = -cp.normal[1];
                    cp.normal[2] = -cp.normal[2];
                } else {
                    cp.slot_a = sa;
                    cp.slot_b = sbb;
                    cp.gen_a = world->slots[sa].generation;
                    cp.gen_b = world->slots[sbb].generation;
                }
            }
            cp.is_trigger = (a->is_trigger || bc->is_trigger)
                                ? 1
                                : 0;
            cp.normal_impulse = 0.0f;
            cp.tangent_impulse[0] = cp.tangent_impulse[1] =
                0.0f;
            cp.rest_bias = 0.0f; /* solver pre-pass fills */
            pw->contacts[pw->contact_count++] = cp;
        }
    }
    /* Phase 6: solve (triggers skipped inside). */
    le_physics_solve(world, dt);
    le_refresh_world_matrices(world);
    /* Phase 8: events (ENTER/STAY/EXIT + trigger variants). */
    le_physics_emit_events(world);
    /* Collision -> script fan-out: live scripted objects consume
     * their queued events through le_script_fire_collision
     * (snapshot iteration; callbacks may destroy — generation
     * guards + live-entry re-resolve per event, same discipline
     * as PASS3). Unscripted objects keep events for C drain. */
    {
        struct le_physics_world *pw2 = world->physics;
        uint32_t scap = world->script_count;
        uint32_t *slots = NULL;
        uint32_t *gens = NULL;
        uint32_t n = 0;
        uint32_t s;

        if (scap > 0 && pw2 != NULL) {
            slots = (uint32_t *)malloc((size_t)scap *
                                       sizeof(*slots));
            gens = (uint32_t *)malloc((size_t)scap *
                                      sizeof(*gens));
        }
        if (slots != NULL && gens != NULL) {
            for (s = 0; s < world->script_count; s++) {
                uint32_t sl = world->scripts[s].slot;

                if (sl >= world->capacity) {
                    continue;
                }
                slots[n] = sl;
                gens[n] = world->slots[sl].generation;
                n++;
            }
        }
        for (s = 0; s < n; s++) {
            uint32_t sl = slots[s];
            le_collision_event evs[16];
            uint32_t total = 0;
            uint32_t got = 0;
            le_object self;
            le_result drc;

            if (sl >= world->capacity ||
                !world->slots[sl].alive ||
                world->slots[sl].generation != gens[s]) {
                continue;
            }
            self.index = sl;
            self.generation = gens[s];
            self.world_tag = world->tag;
            /* Find the live script entry (swap-remove moves). */
            {
                uint32_t si;

                if (!(world->slots[sl].present &
                      LE_PRESENT_SCRIPT)) {
                    continue;
                }
                si = (uint32_t)world->slots[sl].script_index;
                if (si >= world->script_count ||
                    world->scripts[si].slot != sl) {
                    continue;
                }
                if (!world->scripts[si].started ||
                    world->scripts[si].failed) {
                    continue;
                }
                /* Drain in chunks (rings hold 64; Lua errors
                 * mark failed and stop the drain). */
                for (;;) {
                    uint32_t k;

                    drc = le_physics_drain_events(
                        world, &self, evs, 16, &total);
                    if (drc != LE_SUCCESS || total == 0) {
                        break;
                    }
                    got = (total < 16u) ? total : 16u;
                    for (k = 0; k < got; k++) {
                        static const char *names[] = {
                            "collision_enter",
                            "collision_stay",
                            "collision_exit",
                            "trigger_enter",
                            "trigger_stay",
                            "trigger_exit",
                        };
                        const char *nm =
                            names[(int)evs[k].type % 6];
                        le_object other = evs[k].other;
                        uint32_t sj;
                        int frc;

                        /* Re-resolve the entry per event
                         * (callbacks may destroy/realloc). */
                        if (sl >= world->capacity ||
                            !world->slots[sl].alive ||
                            world->slots[sl].generation !=
                                gens[s] ||
                            !(world->slots[sl].present &
                              LE_PRESENT_SCRIPT)) {
                            break;
                        }
                        sj = (uint32_t)world->slots[sl]
                                 .script_index;
                        if (sj >= world->script_count ||
                            world->scripts[sj].slot != sl ||
                            !world->scripts[sj].started ||
                            world->scripts[sj].failed) {
                            break;
                        }
                        frc = le_script_fire_collision(
                            world, &world->scripts[sj], nm,
                            &other, evs[k].normal,
                            evs[k].point, evs[k].penetration);
                        if (frc != 0) {
                            /* Re-resolve to mark failed. */
                            if (sl < world->capacity &&
                                world->slots[sl].alive &&
                                (world->slots[sl].present &
                                 LE_PRESENT_SCRIPT)) {
                                uint32_t sk =
                                    (uint32_t)world->slots[sl]
                                        .script_index;

                                if (sk <
                                        world->script_count &&
                                    world->scripts[sk].slot ==
                                        sl) {
                                    world->scripts[sk].failed =
                                        1;
                                }
                            }
                            break;
                        }
                    }
                    if (total <= 16u) {
                        break;
                    }
                }
            }
        }
        free(slots);
        free(gens);
    }
}
