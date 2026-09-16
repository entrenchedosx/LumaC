/*
 * Continuous collision detection foundation (Phase 30).
 *
 * Scope (deliberate subset, documented): CONTINUOUS dynamic
 * sphere/capsule bodies sweep along v*dt each fixed sub-step vs
 * static/kinematic geometry. Dynamic-vs-dynamic CCD is deferred
 * (discrete response still applies); box CCD is deferred (boxes
 * stay discrete even in CONTINUOUS mode — documented, no silent
 * fallback damage: they simply integrate like DISCRETE).
 *
 * Algorithm per body per sub-step (bounded LE_CCD_MAX_ITERS):
 *   remaining = dt
 *   loop:
 *     cast shape along v*remaining (blocking only, self excluded)
 *     on miss: integrate fully, done
 *     on hit at fraction f: advance (f * remaining) minus margin
 *       along v, kill inward-normal velocity (slide), count impact,
 *       continue with remaining *= (1 - f)
 * Never moves beyond first contact (unlike discrete correct-
 * after). Margin LE_CCD_MARGIN keeps t=0 re-hits impossible.
 */

#include <math.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

/* Sweep one CONTINUOUS dynamic body; returns 1 when the body was
 * handled (integrated here, caller must skip the discrete
 * integrate), 0 when the caller should integrate discretely
 * (wrong mode/shape/pair — never a failure, just deferred). */
int le_ccd_sweep_body(le_world *world, le_body_entry *b,
                      float dt) {
    struct le_physics_world *pw;
    int ci;
    le_collider_entry *c;
    float remaining;
    uint32_t iter;

    if (world == NULL || b == NULL || world->physics == NULL) {
        return 0;
    }
    if (!b->ccd || b->type != LE_BODY_DYNAMIC) {
        return 0;
    }
    if (!isfinite(dt) || dt <= 0.0f) {
        return 0;
    }
    pw = world->physics;
    ci = le_physics_collider_index(world, b->slot);
    if (ci < 0) {
        return 0; /* body-only: nothing to sweep */
    }
    c = &pw->colliders[(uint32_t)ci];
    if (c->shape != LE_COLLIDER_SPHERE &&
        c->shape != LE_COLLIDER_CAPSULE) {
        return 0; /* box CCD deferred -> discrete path */
    }
    if (b->slot >= world->capacity ||
        !world->slots[b->slot].alive) {
        return 0;
    }
    /* Kinematic/static targets only: dynamic-vs-dynamic
     * deferred (their motion during OUR sweep is unknown). The
     * shape cast itself tests every collider; filter here by
     * re-checking the hit object type after each cast. */
    remaining = dt;
    for (iter = 0; iter < LE_CCD_MAX_ITERS; iter++) {
        float disp[3];
        float center[3];
        float quat[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float dims[3] = { 0.0f, 0.0f, 0.0f };
        le_shape_hit hit;
        le_cast_shape cs;
        float dl0;

        disp[0] = b->linear_velocity[0] * remaining;
        disp[1] = b->linear_velocity[1] * remaining;
        disp[2] = b->linear_velocity[2] * remaining;
        dl0 = sqrtf(disp[0] * disp[0] + disp[1] * disp[1] +
                    disp[2] * disp[2]);
        if (!isfinite(dl0)) {
            return 1;
        }
        /* Resting contact shortcut: when the remaining motion
         * is below the CCD margin scale, the discrete solver's
         * contact response owns this regime (prevents hover
         * from sweep-backoff fighting Baumgarte). */
        if (dl0 < LE_CCD_MARGIN * 2.0f) {
            /* Integrate the (tiny) remainder discretely. */
            if (b->slot < world->capacity &&
                world->slots[b->slot].alive) {
                world->slots[b->slot].position[0] += disp[0];
                world->slots[b->slot].position[1] += disp[1];
                world->slots[b->slot].position[2] += disp[2];
                le_mark_subtree_dirty(world, b->slot);
            }
            le_refresh_world_matrices(world);
            return 1;
        }
        {
            float dl = dl0;

            if (!isfinite(dl) || dl < 1e-12f) {
                return 1; /* no motion left: handled */
            }
        }
        /* Current shape center: refresh from the transform so
         * chained iterations use the advanced pose. The
         * collider entry pointer may dangle across refresh
         * (swap-remove never runs here, but re-lookup by slot
         * is free and bulletproof). */
        le_refresh_world_matrices(world);
        {
            int ci2 = le_physics_collider_index(world, b->slot);

            if (ci2 < 0) {
                return 1;
            }
            c = &world->physics->colliders[(uint32_t)ci2];
        }
        le_physics_refresh_collider(world, c);
        if (!c->aabb_valid) {
            return 1;
        }
        center[0] = c->world_center[0];
        center[1] = c->world_center[1];
        center[2] = c->world_center[2];
        if (c->shape == LE_COLLIDER_SPHERE) {
            cs = LE_CAST_SPHERE;
            dims[0] = c->world_radius;
        } else {
            cs = LE_CAST_CAPSULE;
            dims[0] = c->world_cap_radius;
            dims[1] = c->world_cap_half;
            /* Orientation from the world segment axis: build a
             * quat mapping local Y to world_axis. */
            {
                float ax = c->world_axis[0];
                float ay = c->world_axis[1];
                float az = c->world_axis[2];
                /* Rotation from (0,1,0) to a: q = axis x ... */
                float cx = 1.0f * az - 0.0f * ay;
                float cy = 0.0f * ax - 0.0f * az;
                float cz = 0.0f * ay - 1.0f * ax;
                float dot = ay; /* (0,1,0).a */
                float s = sqrtf((1.0f + dot) * 2.0f);

                if (s < 1e-6f) {
                    if (dot < 0.0f) {
                        /* Opposite: 180 deg about X. */
                        quat[0] = 1.0f;
                        quat[1] = 0.0f;
                        quat[2] = 0.0f;
                        quat[3] = 0.0f;
                    } else {
                        quat[0] = 0.0f;
                        quat[1] = 0.0f;
                        quat[2] = 0.0f;
                        quat[3] = 1.0f;
                    }
                } else {
                    float inv = 1.0f / s;

                    quat[0] = cx * inv;
                    quat[1] = cy * inv;
                    quat[2] = cz * inv;
                    quat[3] = s * 0.5f;
                }
            }
        }
        pw->stat_ccd_casts++;
        memset(&hit, 0, sizeof(hit));
        /* Aim sweeps at the collider's own layer/mask pair:
         * use the body's mask so character layers etc. behave
         * like the discrete filter (both-directions rule lives
         * in the cast's swept gather). */
        {
            uint32_t mask = c->mask;

            /* Sweep vs everything except self; triggers never
             * block (cast rule). */
            if (!le_physics_shape_cast(
                    world, cs, center, quat, dims, disp, mask,
                    0, b->slot, &hit, NULL)) {
                break; /* free path: integrate remainder */
            }
            /* Dynamic-vs-dynamic deferred: if the hit object
             * owns a DYNAMIC body, stop sweeping (leave the
             * discrete solver to resolve) but keep the advance
             * so far. */
            {
                uint32_t hs = hit.object.index;

                if (hs < world->capacity &&
                    world->slots[hs].alive &&
                    (world->slots[hs].present &
                     LE_PRESENT_RIGID_BODY)) {
                    int bi = le_physics_body_index(world, hs);

                    if (bi >= 0 &&
                        pw->bodies[(uint32_t)bi].type ==
                            LE_BODY_DYNAMIC) {
                        break;
                    }
                }
            }
            pw->stat_ccd_impacts++;
            /* Resting-contact short-circuit: a hit at fraction
             * ~0 with the velocity already pointing AWAY (or
             * negligibly inward) means the discrete contact
             * response owns this regime — stop sweeping and let
             * the solver's Baumgarte/position pass settle it.
             * Without this, CCD hover-fights the solver: each
             * sub-step backs off by the margin and gravity
             * re-adds the same drop, freezing the fall. */
            {
                float vn0 = b->linear_velocity[0] *
                                hit.normal[0] +
                            b->linear_velocity[1] *
                                hit.normal[1] +
                            b->linear_velocity[2] *
                                hit.normal[2];

                if (hit.fraction <= 0.01f && vn0 >= -0.5f) {
                    break;
                }
            }
            /* Advance to contact minus margin along disp. */
            {
                float adv = hit.fraction;
                float back = 0.0f;
                float dl = sqrtf(disp[0] * disp[0] +
                                 disp[1] * disp[1] +
                                 disp[2] * disp[2]);

                if (dl > 1e-12f) {
                    back = LE_CCD_MARGIN / dl;
                }
                if (adv > back) {
                    adv -= back;
                } else {
                    adv = 0.0f;
                }
                if (b->slot < world->capacity &&
                    world->slots[b->slot].alive) {
                    world->slots[b->slot].position[0] +=
                        disp[0] * adv;
                    world->slots[b->slot].position[1] +=
                        disp[1] * adv;
                    world->slots[b->slot].position[2] +=
                        disp[2] * adv;
                    le_mark_subtree_dirty(world, b->slot);
                }
                remaining *= (1.0f - hit.fraction);
            }
            /* Slide: kill inward-normal velocity (keep the
             * tangential part; restitution handled by the
             * discrete solver on the resulting contact). */
            {
                float vn = b->linear_velocity[0] *
                               hit.normal[0] +
                           b->linear_velocity[1] *
                               hit.normal[1] +
                           b->linear_velocity[2] *
                               hit.normal[2];

                if (vn < 0.0f) {
                    b->linear_velocity[0] -= hit.normal[0] * vn;
                    b->linear_velocity[1] -= hit.normal[1] * vn;
                    b->linear_velocity[2] -= hit.normal[2] * vn;
                }
            }
            if (remaining <= 1e-9f) {
                le_refresh_world_matrices(world);
                return 1;
            }
            if (hit.fraction <= 0.0f) {
                /* Started overlapping / zero advance: avoid a
                 * corner loop — one slide resolution is enough. */
                break;
            }
        }
    }
    /* Integrate whatever remains discretely (same semi-implicit
     * Euler as le_integrate_body, minus force handling which the
     * caller already applied — here only position advance). */
    if (b->slot < world->capacity &&
        world->slots[b->slot].alive) {
        world->slots[b->slot].position[0] +=
            b->linear_velocity[0] * remaining;
        world->slots[b->slot].position[1] +=
            b->linear_velocity[1] * remaining;
        world->slots[b->slot].position[2] +=
            b->linear_velocity[2] * remaining;
        le_mark_subtree_dirty(world, b->slot);
    }
    le_refresh_world_matrices(world);
    return 1;
}
