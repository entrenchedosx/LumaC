/*
 * Contact solver (Phase 28): sequential impulse.
 *
 * Per iteration over deterministic contact order:
 * - Normal impulse with restitution (bounce only above
 *   LE_PHYS_RESTITUTION_SLOP relative velocity; pair
 *   restitution = max).
 * - Coulomb friction via two tangent directions (pair friction
 *   = geometric mean; tangent impulse clamped to mu * normal).
 * - Baumgarte stabilization with slop (no correction below
 *   slop; capped per-step correction) + a position pass that
 *   splits residual penetration correction by inverse mass.
 * Static/kinematic bodies have inv_mass 0 (never move);
 * triggers never enter the solver (events only).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

static le_body_entry *le_body_for(le_world *world, uint32_t slot) {
    int idx = le_physics_body_index(world, slot);

    if (idx < 0) {
        return NULL;
    }
    return &world->physics->bodies[(uint32_t)idx];
}

static const le_collider_entry *le_collider_for(le_world *world,
                                                uint32_t slot) {
    int idx = le_physics_collider_index(world, slot);

    if (idx < 0) {
        return NULL;
    }
    return &world->physics->colliders[(uint32_t)idx];
}

/* Angular factor: (I^-1 (r x n)) x r, dotted per component —
 * returns the scalar rotational contribution to effective mass
 * along n for one body. World-diagonal inertia approximation. */
static float le_angular_factor(const le_body_entry *body,
                               const float r[3],
                               const float n[3]) {
    float rx_n[3];
    float t[3];

    rx_n[0] = r[1] * n[2] - r[2] * n[1];
    rx_n[1] = r[2] * n[0] - r[0] * n[2];
    rx_n[2] = r[0] * n[1] - r[1] * n[0];
    t[0] = rx_n[0] * body->inv_inertia[0];
    t[1] = rx_n[1] * body->inv_inertia[1];
    t[2] = rx_n[2] * body->inv_inertia[2];
    {
        float txr[3];

        txr[0] = t[1] * r[2] - t[2] * r[1];
        txr[1] = t[2] * r[0] - t[0] * r[2];
        txr[2] = t[0] * r[1] - t[1] * r[0];
        return txr[0] * n[0] + txr[1] * n[1] + txr[2] * n[2];
    }
}

/* Point velocity of a body at world offset r from its center. */
static void le_point_velocity(const le_body_entry *body,
                              const float com[3],
                              const float point[3],
                              float out_v[3]) {
    float r[3];

    r[0] = point[0] - com[0];
    r[1] = point[1] - com[1];
    r[2] = point[2] - com[2];
    out_v[0] = body->linear_velocity[0] +
               (body->angular_velocity[1] * r[2] -
                body->angular_velocity[2] * r[1]);
    out_v[1] = body->linear_velocity[1] +
               (body->angular_velocity[2] * r[0] -
                body->angular_velocity[0] * r[2]);
    out_v[2] = body->linear_velocity[2] +
               (body->angular_velocity[0] * r[1] -
                body->angular_velocity[1] * r[0]);
}

static void le_body_center(le_world *world, uint32_t slot,
                           float out_c[3]) {
    int ci = le_physics_collider_index(world, slot);

    if (ci >= 0 &&
        world->physics->colliders[(uint32_t)ci].aabb_valid) {
        memcpy(out_c,
               world->physics->colliders[(uint32_t)ci]
                   .world_center,
               3u * sizeof(float));
        return;
    }
    {
        float wm[16];
        le_object h;

        h.index = slot;
        h.generation = world->slots[slot].generation;
        h.world_tag = world->tag;
        le_object_get_world_matrix(world, &h, wm);
        out_c[0] = wm[12];
        out_c[1] = wm[13];
        out_c[2] = wm[14];
    }
}

void le_physics_solve(le_world *world, float dt) {
    struct le_physics_world *pw;
    uint32_t vit;
    uint32_t pit;
    uint32_t it;
    uint32_t c;

    if (world == NULL || world->physics == NULL || dt <= 0.0f) {
        return;
    }
    pw = world->physics;
    if (pw->contact_count == 0) {
        return;
    }
    vit = pw->velocity_iters;
    pit = pw->position_iters;
    if (vit == 0) {
        vit = 1;
    }
    if (vit > LE_PHYS_MAX_ITERS) {
        vit = LE_PHYS_MAX_ITERS;
    }
    if (pit > LE_PHYS_MAX_ITERS) {
        pit = LE_PHYS_MAX_ITERS;
    }
    /* Pre-pass: restitution targets from PRE-SOLVE approach
     * velocities (Newton-style bounce vn' = e*|vn0|). Computed
     * once per step per contact — never inside the iteration
     * loop (later iterations observe post-impulse separating
     * velocity and would cancel the bounce). */
    for (c = 0; c < pw->contact_count; c++) {
        le_contact_point *cp = &pw->contacts[c];
        le_body_entry *ba0;
        le_body_entry *bb0;
        const le_collider_entry *ca0;
        const le_collider_entry *cb0;
        float coma0[3];
        float comb0[3];
        float va0[3];
        float vb0[3];
        float vn0;
        float e;

        cp->rest_bias = 0.0f;
        if (cp->is_trigger) {
            continue;
        }
        ba0 = le_body_for(world, cp->slot_a);
        bb0 = le_body_for(world, cp->slot_b);
        ca0 = le_collider_for(world, cp->slot_a);
        cb0 = le_collider_for(world, cp->slot_b);
        if (ba0 == NULL || bb0 == NULL || ca0 == NULL ||
            cb0 == NULL) {
            continue;
        }
        if (ba0->type != LE_BODY_DYNAMIC &&
            bb0->type != LE_BODY_DYNAMIC) {
            continue;
        }
        le_body_center(world, cp->slot_a, coma0);
        le_body_center(world, cp->slot_b, comb0);
        le_point_velocity(ba0, coma0, cp->point, va0);
        le_point_velocity(bb0, comb0, cp->point, vb0);
        vn0 = (vb0[0] - va0[0]) * cp->normal[0] +
              (vb0[1] - va0[1]) * cp->normal[1] +
              (vb0[2] - va0[2]) * cp->normal[2];
        e = (ca0->restitution > cb0->restitution)
                ? ca0->restitution
                : cb0->restitution;
        if (vn0 < -LE_PHYS_RESTITUTION_SLOP) {
            cp->rest_bias = e * -vn0;
        }
    }
    /* Velocity iterations: normal + restitution + friction. */
    for (it = 0; it < vit; it++) {
        for (c = 0; c < pw->contact_count; c++) {
            le_contact_point *cp = &pw->contacts[c];
            le_body_entry *ba;
            le_body_entry *bb;
            const le_collider_entry *ca;
            const le_collider_entry *cb;
            float coma[3];
            float comb[3];
            float ra[3];
            float rb[3];
            float va[3];
            float vb[3];
            float rv[3];
            float vn;
            float kn;
            float mu;
            float bias = 0.0f;
            float lambda;

            if (cp->is_trigger) {
                continue;
            }
            ba = le_body_for(world, cp->slot_a);
            bb = le_body_for(world, cp->slot_b);
            ca = le_collider_for(world, cp->slot_a);
            cb = le_collider_for(world, cp->slot_b);
            if (ba == NULL || bb == NULL || ca == NULL ||
                cb == NULL) {
                continue;
            }
            if (ba->type != LE_BODY_DYNAMIC &&
                bb->type != LE_BODY_DYNAMIC) {
                continue; /* nothing to move */
            }
            le_body_center(world, cp->slot_a, coma);
            le_body_center(world, cp->slot_b, comb);
            ra[0] = cp->point[0] - coma[0];
            ra[1] = cp->point[1] - coma[1];
            ra[2] = cp->point[2] - coma[2];
            rb[0] = cp->point[0] - comb[0];
            rb[1] = cp->point[1] - comb[1];
            rb[2] = cp->point[2] - comb[2];
            le_point_velocity(ba, coma, cp->point, va);
            le_point_velocity(bb, comb, cp->point, vb);
            rv[0] = vb[0] - va[0];
            rv[1] = vb[1] - va[1];
            rv[2] = vb[2] - va[2];
            vn = rv[0] * cp->normal[0] + rv[1] * cp->normal[1] +
                 rv[2] * cp->normal[2];
            kn = ba->inv_mass + bb->inv_mass +
                 le_angular_factor(ba, ra, cp->normal) +
                 le_angular_factor(bb, rb, cp->normal);
            if (kn < 1e-12f) {
                continue;
            }
            /* Restitution: pre-solve target (cp->rest_bias, pair
             * max above slop, computed before the loop): the loop
             * converges to vn = +rest_bias instead of unwinding
             * the bounce on iteration 2. */
            bias = cp->rest_bias;
            /* Baumgarte: correct penetration beyond slop. */
            if (cp->penetration > LE_PHYS_SLOP) {
                float corr = LE_PHYS_BAUMGARTE / dt *
                             (cp->penetration - LE_PHYS_SLOP);

                if (corr > LE_PHYS_MAX_CORRECTION / dt) {
                    corr = LE_PHYS_MAX_CORRECTION / dt;
                }
                bias += corr;
            }
            lambda = -(vn - bias) / kn;
            /* Clamp accumulated normal impulse >= 0 (no
             * pulling). */
            {
                float new_acc = cp->normal_impulse + lambda;

                if (new_acc < 0.0f) {
                    lambda = -cp->normal_impulse;
                    cp->normal_impulse = 0.0f;
                } else {
                    cp->normal_impulse = new_acc;
                }
            }
            /* Apply: A -=, B += (normal is A -> B). */
            {
                float px = cp->normal[0] * lambda;
                float py = cp->normal[1] * lambda;
                float pz = cp->normal[2] * lambda;

                ba->linear_velocity[0] -= px * ba->inv_mass;
                ba->linear_velocity[1] -= py * ba->inv_mass;
                ba->linear_velocity[2] -= pz * ba->inv_mass;
                bb->linear_velocity[0] += px * bb->inv_mass;
                bb->linear_velocity[1] += py * bb->inv_mass;
                bb->linear_velocity[2] += pz * bb->inv_mass;
                /* Angular: dw = I^-1 (r x P). */
                {
                    float tax = ra[1] * pz - ra[2] * py;
                    float tay = ra[2] * px - ra[0] * pz;
                    float taz = ra[0] * py - ra[1] * px;
                    float tbx = rb[1] * pz - rb[2] * py;
                    float tby = rb[2] * px - rb[0] * pz;
                    float tbz = rb[0] * py - rb[1] * px;

                    ba->angular_velocity[0] -=
                        tax * ba->inv_inertia[0];
                    ba->angular_velocity[1] -=
                        tay * ba->inv_inertia[1];
                    ba->angular_velocity[2] -=
                        taz * ba->inv_inertia[2];
                    bb->angular_velocity[0] +=
                        tbx * bb->inv_inertia[0];
                    bb->angular_velocity[1] +=
                        tby * bb->inv_inertia[1];
                    bb->angular_velocity[2] +=
                        tbz * bb->inv_inertia[2];
                }
            }
            /* Friction: pair mu = geometric mean; two tangent
             * dirs from the normal; clamp |jt| <= mu * jn. */
            mu = sqrtf(ca->friction * cb->friction);
            if (mu > 0.0f && cp->normal_impulse > 0.0f) {
                float t1[3];
                float t2[3];
                float ax[3];
                int ti;

                /* Pick a stable tangent basis. */
                if (cp->normal[0] < 0.0f ? -cp->normal[0] < 0.9f
                                         : cp->normal[0] < 0.9f) {
                    ax[0] = 1.0f;
                    ax[1] = 0.0f;
                    ax[2] = 0.0f;
                } else {
                    ax[0] = 0.0f;
                    ax[1] = 1.0f;
                    ax[2] = 0.0f;
                }
                /* t1 = normalize(n x ax), t2 = n x t1. */
                t1[0] = cp->normal[1] * ax[2] -
                        cp->normal[2] * ax[1];
                t1[1] = cp->normal[2] * ax[0] -
                        cp->normal[0] * ax[2];
                t1[2] = cp->normal[0] * ax[1] -
                        cp->normal[1] * ax[0];
                {
                    float l = sqrtf(t1[0] * t1[0] +
                                    t1[1] * t1[1] +
                                    t1[2] * t1[2]);

                    if (l < 1e-9f) {
                        continue;
                    }
                    t1[0] /= l;
                    t1[1] /= l;
                    t1[2] /= l;
                }
                t2[0] = cp->normal[1] * t1[2] -
                        cp->normal[2] * t1[1];
                t2[1] = cp->normal[2] * t1[0] -
                        cp->normal[0] * t1[2];
                t2[2] = cp->normal[0] * t1[1] -
                        cp->normal[1] * t1[0];
                for (ti = 0; ti < 2; ti++) {
                    const float *tt = (ti == 0) ? t1 : t2;
                    float kt;
                    float vt;
                    float jt;
                    float maxf;

                    le_point_velocity(ba, coma, cp->point, va);
                    le_point_velocity(bb, comb, cp->point, vb);
                    vt = (vb[0] - va[0]) * tt[0] +
                         (vb[1] - va[1]) * tt[1] +
                         (vb[2] - va[2]) * tt[2];
                    kt = ba->inv_mass + bb->inv_mass +
                         le_angular_factor(ba, ra, tt) +
                         le_angular_factor(bb, rb, tt);
                    if (kt < 1e-12f) {
                        continue;
                    }
                    jt = -vt / kt;
                    maxf = mu * cp->normal_impulse;
                    {
                        float acc =
                            cp->tangent_impulse[ti] + jt;

                        if (acc < -maxf) {
                            jt = -maxf -
                                 cp->tangent_impulse[ti];
                            cp->tangent_impulse[ti] = -maxf;
                        } else if (acc > maxf) {
                            jt = maxf -
                                 cp->tangent_impulse[ti];
                            cp->tangent_impulse[ti] = maxf;
                        } else {
                            cp->tangent_impulse[ti] = acc;
                        }
                    }
                    {
                        float px = tt[0] * jt;
                        float py = tt[1] * jt;
                        float pz = tt[2] * jt;

                        ba->linear_velocity[0] -=
                            px * ba->inv_mass;
                        ba->linear_velocity[1] -=
                            py * ba->inv_mass;
                        ba->linear_velocity[2] -=
                            pz * ba->inv_mass;
                        bb->linear_velocity[0] +=
                            px * bb->inv_mass;
                        bb->linear_velocity[1] +=
                            py * bb->inv_mass;
                        bb->linear_velocity[2] +=
                            pz * bb->inv_mass;
                        {
                            float tax =
                                ra[1] * pz - ra[2] * py;
                            float tay =
                                ra[2] * px - ra[0] * pz;
                            float taz =
                                ra[0] * py - ra[1] * px;
                            float tbx =
                                rb[1] * pz - rb[2] * py;
                            float tby =
                                rb[2] * px - rb[0] * pz;
                            float tbz =
                                rb[0] * py - rb[1] * px;

                            ba->angular_velocity[0] -=
                                tax * ba->inv_inertia[0];
                            ba->angular_velocity[1] -=
                                tay * ba->inv_inertia[1];
                            ba->angular_velocity[2] -=
                                taz * ba->inv_inertia[2];
                            bb->angular_velocity[0] +=
                                tbx * bb->inv_inertia[0];
                            bb->angular_velocity[1] +=
                                tby * bb->inv_inertia[1];
                            bb->angular_velocity[2] +=
                                tbz * bb->inv_inertia[2];
                        }
                    }
                }
            }
        }
    }
    /* Position pass: split residual correction by inverse mass
     * (kills sink without adding energy). Corrections write
     * directly to root body slots (dynamics are roots by rule,
     * so local == world offset) with subtree dirty marks batched
     * at the end — one calloc for the whole pass, never per
     * contact. Static/kinematic bodies (inv_mass 0) never move;
     * the dynamic partner takes the full split. */
    {
        /* Direct per-contact apply with a moved-mask keeps it
         * O(contacts * iters). */
        uint8_t *moved = NULL;

        if (pit > 0 && pw->contact_count > 0) {
            moved = (uint8_t *)calloc(world->capacity,
                                      sizeof(*moved));
        }
        for (it = 0; it < pit; it++) {
            if (moved != NULL) {
                memset(moved, 0,
                       world->capacity * sizeof(*moved));
            }
            for (c = 0; c < pw->contact_count; c++) {
                le_contact_point *cp = &pw->contacts[c];
                le_body_entry *ba;
                le_body_entry *bb;
                float total_inv;
                float corr_a;
                float corr_b;

                if (cp->is_trigger) {
                    continue;
                }
                if (cp->penetration <= LE_PHYS_SLOP) {
                    continue;
                }
                ba = le_body_for(world, cp->slot_a);
                bb = le_body_for(world, cp->slot_b);
                if (ba == NULL || bb == NULL) {
                    continue;
                }
                total_inv = ba->inv_mass + bb->inv_mass;
                if (total_inv < 1e-12f) {
                    continue;
                }
                /* Split by inverse mass share, capped. */
                corr_a = (cp->penetration - LE_PHYS_SLOP) *
                         (ba->inv_mass / total_inv);
                corr_b = (cp->penetration - LE_PHYS_SLOP) *
                         (bb->inv_mass / total_inv);
                if (corr_a > LE_PHYS_MAX_CORRECTION) {
                    corr_a = LE_PHYS_MAX_CORRECTION;
                }
                if (corr_b > LE_PHYS_MAX_CORRECTION) {
                    corr_b = LE_PHYS_MAX_CORRECTION;
                }
                /* A moves along -n, B along +n (normal A->B). */
                if (corr_a > 0.0f && cp->slot_a < world->capacity &&
                    world->slots[cp->slot_a].alive) {
                    le_object_slot *s =
                        &world->slots[cp->slot_a];

                    s->position[0] -= cp->normal[0] * corr_a;
                    s->position[1] -= cp->normal[1] * corr_a;
                    s->position[2] -= cp->normal[2] * corr_a;
                    if (moved != NULL) {
                        moved[cp->slot_a] = 1;
                    }
                }
                if (corr_b > 0.0f && cp->slot_b < world->capacity &&
                    world->slots[cp->slot_b].alive) {
                    le_object_slot *s =
                        &world->slots[cp->slot_b];

                    s->position[0] += cp->normal[0] * corr_b;
                    s->position[1] += cp->normal[1] * corr_b;
                    s->position[2] += cp->normal[2] * corr_b;
                    if (moved != NULL) {
                        moved[cp->slot_b] = 1;
                    }
                }
                /* Shrink the manifold depth so repeated
                 * iterations converge instead of re-applying. */
                cp->penetration -=
                    (cp->penetration - LE_PHYS_SLOP) * 0.5f;
            }
        }
        if (moved != NULL) {
            uint32_t s;

            for (s = 0; s < world->capacity; s++) {
                if (moved[s]) {
                    le_mark_subtree_dirty(world, s);
                }
            }
            free(moved);
        }
    }
}
