/*
 * Collider world-pose refresh (Phase 28): local shape frame ->
 * world center + basis + conservative dimensions + AABB.
 *
 * Scale policy (documented, tested):
 * - Sphere: radius *= max(|world scale axes|). Conservative:
 *   the sphere covers the ellipsoid the scaled shape implies.
 * - Box: half extents *= per-axis |world scale| in the shape's
 *   world basis. Negative scales never negate dimensions.
 * - World scale comes from decomposing the object's world matrix
 *   columns (lengths); rotation comes from the normalized matrix.
 * - Shear detection: if the basis columns are not mutually
 *   perpendicular (beyond tolerance), the transform is sheared
 *   and the collider is SKIPPED for the step (returns 0) — never
 *   a silently invalid rigid shape. NaN/Inf anywhere also skips.
 * - Hierarchy: pose composes through parents naturally (world
 *   matrix is authoritative); dynamic bodies are roots by rule,
 *   but static/kinematic children work.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

/* Local inverse inertia diagonal (body frame). Triggers return 0
 * (never respond). Sphere I=2/5 m r^2; box standard cuboid form
 * on LOCAL dimensions (world scale enters via inv_mass scaling
 * at solve time — documented approximation for scaled bodies). */
void le_physics_shape_inertia(const le_collider_entry *c,
                              float mass, float out_inv[3]) {
    out_inv[0] = out_inv[1] = out_inv[2] = 0.0f;
    if (c == NULL || mass <= 0.0f || !isfinite(mass)) {
        return;
    }
    if (c->is_trigger) {
        return;
    }
    if (c->shape == LE_COLLIDER_SPHERE) {
        float i;

        if (c->radius <= 0.0f || !isfinite(c->radius)) {
            return;
        }
        i = 0.4f * mass * c->radius * c->radius;
        if (i > 1e-12f) {
            out_inv[0] = out_inv[1] = out_inv[2] = 1.0f / i;
        }
        return;
    }
    if (c->shape == LE_COLLIDER_BOX) {
        float hx = c->half_extents[0];
        float hy = c->half_extents[1];
        float hz = c->half_extents[2];
        float ix;
        float iy;
        float iz;

        if (hx <= 0.0f || hy <= 0.0f || hz <= 0.0f) {
            return;
        }
        /* Full extents 2h; Ix = m/12 ((2hy)^2 + (2hz)^2). */
        ix = mass / 3.0f * (hy * hy + hz * hz);
        iy = mass / 3.0f * (hx * hx + hz * hz);
        iz = mass / 3.0f * (hx * hx + hy * hy);
        if (ix > 1e-12f) {
            out_inv[0] = 1.0f / ix;
        }
        if (iy > 1e-12f) {
            out_inv[1] = 1.0f / iy;
        }
        if (iz > 1e-12f) {
            out_inv[2] = 1.0f / iz;
        }
        return;
    }
    if (c->shape == LE_COLLIDER_CAPSULE) {
        /* Solid capsule about local Y (segment axis): cylinder of
         * half-length h plus two hemispherical caps of radius r.
         * Total mass m; cylinder mass fraction ~ h/(h+4r/3).
         * Closed forms (e.g. standard capsule inertia):
         *   I_axial (Y) = 0.5 * m * r^2 (caps + cylinder share
         *     the axial value to good approximation).
         *   I_perp = m * (0.25*r^2 + h^2/3 ... ) — use the exact
         *     composite: cylinder I_perp_c = mc*(3r^2+h_cyl^2)/12
         *     plus cap terms; approximate with the widely used
         *     capsule form I = m*(0.25 r^2 + (h^2)/3 + ...) —
         *     here computed as cylinder + point-cap correction,
         *     always finite and positive. Zero half-length falls
         *     back to the sphere form. */
        float r = c->capsule_radius;
        float h = c->capsule_half; /* half cylinder length */
        float hcyl = 2.0f * h;
        float ia;
        float ip;

        if (r <= 0.0f || !isfinite(r) || !isfinite(h) || h < 0.0f) {
            return;
        }
        if (h <= 1e-9f) {
            float i = 0.4f * mass * r * r;

            if (i > 1e-12f) {
                out_inv[0] = out_inv[1] = out_inv[2] = 1.0f / i;
            }
            return;
        }
        ia = 0.5f * mass * r * r;
        /* Transverse: cylinder term + caps (parallel-axis). */
        ip = mass *
             (0.25f * r * r + (hcyl * hcyl) / 12.0f +
              0.375f * r * hcyl + 0.25f * r * r * 0.0f);
        /* Guard against degenerate tiny values. */
        if (ia > 1e-12f) {
            out_inv[1] = 1.0f / ia; /* local Y = segment axis */
        }
        if (ip > 1e-12f) {
            /* Local-frame diagonal is axis-aligned here; the
             * stepper rotates it by orientation each step, so
             * assign transverse to X/Z. */
            out_inv[0] = 1.0f / ip;
            out_inv[2] = 1.0f / ip;
        }
        return;
    }
}

int le_physics_refresh_collider(le_world *world,
                                le_collider_entry *c) {
    float wm[16];
    float bx[3];
    float by[3];
    float bz[3];
    float sx;
    float sy;
    float sz;
    float dot;
    le_object h;
    int i;

    if (world == NULL || c == NULL) {
        return 0;
    }
    if (c->slot >= world->capacity ||
        !world->slots[c->slot].alive) {
        return 0;
    }
    h.index = c->slot;
    h.generation = world->slots[c->slot].generation;
    h.world_tag = world->tag;
    le_object_get_world_matrix(world, &h, wm);
    for (i = 0; i < 16; i++) {
        if (!isfinite(wm[i])) {
            c->aabb_valid = 0;
            return 0;
        }
    }
    /* Basis columns + scales from the world matrix. */
    bx[0] = wm[0];
    bx[1] = wm[1];
    bx[2] = wm[2];
    by[0] = wm[4];
    by[1] = wm[5];
    by[2] = wm[6];
    bz[0] = wm[8];
    bz[1] = wm[9];
    bz[2] = wm[10];
    sx = sqrtf(bx[0] * bx[0] + bx[1] * bx[1] + bx[2] * bx[2]);
    sy = sqrtf(by[0] * by[0] + by[1] * by[1] + by[2] * by[2]);
    sz = sqrtf(bz[0] * bz[0] + bz[1] * bz[1] + bz[2] * bz[2]);
    if (!isfinite(sx) || !isfinite(sy) || !isfinite(sz) ||
        sx < 1e-9f || sy < 1e-9f || sz < 1e-9f) {
        /* Degenerate scale: no valid rigid shape. */
        c->aabb_valid = 0;
        return 0;
    }
    /* Shear check: normalized columns must be perpendicular. */
    dot = (bx[0] * by[0] + bx[1] * by[1] + bx[2] * by[2]) /
          (sx * sy);
    if (dot < 0.0f ? -dot > 1e-4f : dot > 1e-4f) {
        c->aabb_valid = 0;
        return 0;
    }
    dot = (bx[0] * bz[0] + bx[1] * bz[1] + bx[2] * bz[2]) /
          (sx * sz);
    if (dot < 0.0f ? -dot > 1e-4f : dot > 1e-4f) {
        c->aabb_valid = 0;
        return 0;
    }
    dot = (by[0] * bz[0] + by[1] * bz[1] + by[2] * bz[2]) /
          (sy * sz);
    if (dot < 0.0f ? -dot > 1e-4f : dot > 1e-4f) {
        c->aabb_valid = 0;
        return 0;
    }
    /* World basis (normalized columns) + translation. */
    c->world_basis[0][0] = bx[0] / sx;
    c->world_basis[1][0] = bx[1] / sx;
    c->world_basis[2][0] = bx[2] / sx;
    c->world_basis[0][1] = by[0] / sy;
    c->world_basis[1][1] = by[1] / sy;
    c->world_basis[2][1] = by[2] / sy;
    c->world_basis[0][2] = bz[0] / sz;
    c->world_basis[1][2] = bz[1] / sz;
    c->world_basis[2][2] = bz[2] / sz;
    /* Shape frame: object world * local offset/orientation.
     * Offset rotates by the object's world rotation (basis). */
    {
        float ox = c->offset[0];
        float oy = c->offset[1];
        float oz = c->offset[2];

        c->world_center[0] =
            wm[12] + c->world_basis[0][0] * ox * sx +
            c->world_basis[0][1] * oy * sy +
            c->world_basis[0][2] * oz * sz;
        c->world_center[1] =
            wm[13] + c->world_basis[1][0] * ox * sx +
            c->world_basis[1][1] * oy * sy +
            c->world_basis[1][2] * oz * sz;
        c->world_center[2] =
            wm[14] + c->world_basis[2][0] * ox * sx +
            c->world_basis[2][1] * oy * sy +
            c->world_basis[2][2] * oz * sz;
    }
    /* Shape orientation: world basis * local quat. Compose the
     * local orientation into the basis columns. */
    {
        float qx = c->orientation[0];
        float qy = c->orientation[1];
        float qz = c->orientation[2];
        float qw = c->orientation[3];
        /* Rotation matrix from unit quat (column-major 3x3). */
        float r00 = 1.0f - 2.0f * (qy * qy + qz * qz);
        float r10 = 2.0f * (qx * qy + qz * qw);
        float r20 = 2.0f * (qx * qz - qy * qw);
        float r01 = 2.0f * (qx * qy - qz * qw);
        float r11 = 1.0f - 2.0f * (qx * qx + qz * qz);
        float r21 = 2.0f * (qy * qz + qx * qw);
        float r02 = 2.0f * (qx * qz + qy * qw);
        float r12 = 2.0f * (qy * qz - qx * qw);
        float r22 = 1.0f - 2.0f * (qx * qx + qy * qy);
        float b[3][3];
        int r;
        int col;

        /* world_shape_basis = world_basis * R_local. */
        for (col = 0; col < 3; col++) {
            float l0 = (col == 0) ? r00 : (col == 1) ? r01 : r02;
            float l1 = (col == 0) ? r10 : (col == 1) ? r11 : r12;
            float l2 = (col == 0) ? r20 : (col == 1) ? r21 : r22;

            for (r = 0; r < 3; r++) {
                b[r][col] = c->world_basis[r][0] * l0 +
                            c->world_basis[r][1] * l1 +
                            c->world_basis[r][2] * l2;
            }
        }
        memcpy(c->world_basis, b, sizeof(b));
        /* Dimensions: scale sits between the object rotation and
         * the local shape rotation (M = R_obj * S_obj * R_loc),
         * so per-axis object scales do not map 1:1 onto shape
         * axes under local rotation. Conservative + exact for
         * the common cases (uniform scale, axis-aligned local
         * frame): sphere takes max axis (covers the ellipsoid);
         * box takes max axis too (covers any local rotation).
         * Never underestimates (no broad-phase false
         * negatives); slight overestimation only for
         * non-uniform-scale + rotated-local-frame combos. */
        {
            float m = sx;

            if (sy > m) {
                m = sy;
            }
            if (sz > m) {
                m = sz;
            }
            if (c->shape == LE_COLLIDER_SPHERE) {
                c->world_radius = c->radius * m;
            } else if (c->shape == LE_COLLIDER_CAPSULE) {
                /* Capsule scale policy (Phase 30, documented):
                 * uniform scale is supported exactly; non-uniform
                 * scale is REJECTED (collider skipped for the step)
                 * because a non-uniformly scaled capsule is an
                 * ellipsoid-segment hybrid with no closed-form
                 * narrow phase. Negative scales use absolute
                 * magnitude (column lengths are already >= 0).
                 * The check below runs before writing world dims. */
                float mn = sx;

                if (sy < mn) {
                    mn = sy;
                }
                if (sz < mn) {
                    mn = sz;
                }
                if (m > 1e-9f && (m - mn) / m > 1e-4f) {
                    c->aabb_valid = 0;
                    return 0;
                }
                c->world_cap_radius = c->capsule_radius * m;
                c->world_cap_half = c->capsule_half * m;
            } else {
                c->world_half[0] = c->half_extents[0] * m;
                c->world_half[1] = c->half_extents[1] * m;
                c->world_half[2] = c->half_extents[2] * m;
            }
        }
    }
    /* AABB from center + basis * extents (conservative, exact
     * for boxes; sphere uses radius on all axes; capsule uses
     * segment endpoints +/- radius — conservative under arbitrary
     * orientation, never a false-negative bound). */
    {
        float ex;
        float ey;
        float ez;

        if (c->shape == LE_COLLIDER_SPHERE) {
            ex = ey = ez = c->world_radius;
        } else if (c->shape == LE_COLLIDER_CAPSULE) {
            /* Segment axis = local Y column of the shape basis. */
            float ax = c->world_basis[0][1];
            float ay = c->world_basis[1][1];
            float az = c->world_basis[2][1];
            float al = sqrtf(ax * ax + ay * ay + az * az);
            float hx;
            float hy;
            float hz;

            if (al < 1e-9f || !isfinite(al)) {
                c->aabb_valid = 0;
                return 0;
            }
            ax /= al;
            ay /= al;
            az /= al;
            c->world_axis[0] = ax;
            c->world_axis[1] = ay;
            c->world_axis[2] = az;
            c->world_p0[0] = c->world_center[0] - ax * c->world_cap_half;
            c->world_p0[1] = c->world_center[1] - ay * c->world_cap_half;
            c->world_p0[2] = c->world_center[2] - az * c->world_cap_half;
            c->world_p1[0] = c->world_center[0] + ax * c->world_cap_half;
            c->world_p1[1] = c->world_center[1] + ay * c->world_cap_half;
            c->world_p1[2] = c->world_center[2] + az * c->world_cap_half;
            hx = (ax < 0.0f ? -ax : ax) * c->world_cap_half;
            hy = (ay < 0.0f ? -ay : ay) * c->world_cap_half;
            hz = (az < 0.0f ? -az : az) * c->world_cap_half;
            ex = hx + c->world_cap_radius;
            ey = hy + c->world_cap_radius;
            ez = hz + c->world_cap_radius;
        } else {
            /* |basis row| . half (SAT-style AABB of an OBB). */
            float ax0 = c->world_basis[0][0];
            float ax1 = c->world_basis[0][1];
            float ax2 = c->world_basis[0][2];
            float ay0 = c->world_basis[1][0];
            float ay1 = c->world_basis[1][1];
            float ay2 = c->world_basis[1][2];
            float az0 = c->world_basis[2][0];
            float az1 = c->world_basis[2][1];
            float az2 = c->world_basis[2][2];

            if (ax0 < 0.0f) {
                ax0 = -ax0;
            }
            if (ax1 < 0.0f) {
                ax1 = -ax1;
            }
            if (ax2 < 0.0f) {
                ax2 = -ax2;
            }
            if (ay0 < 0.0f) {
                ay0 = -ay0;
            }
            if (ay1 < 0.0f) {
                ay1 = -ay1;
            }
            if (ay2 < 0.0f) {
                ay2 = -ay2;
            }
            if (az0 < 0.0f) {
                az0 = -az0;
            }
            if (az1 < 0.0f) {
                az1 = -az1;
            }
            if (az2 < 0.0f) {
                az2 = -az2;
            }
            ex = ax0 * c->world_half[0] + ax1 * c->world_half[1] +
                 ax2 * c->world_half[2];
            ey = ay0 * c->world_half[0] + ay1 * c->world_half[1] +
                 ay2 * c->world_half[2];
            ez = az0 * c->world_half[0] + az1 * c->world_half[1] +
                 az2 * c->world_half[2];
        }
        c->aabb_min[0] = c->world_center[0] - ex;
        c->aabb_min[1] = c->world_center[1] - ey;
        c->aabb_min[2] = c->world_center[2] - ez;
        c->aabb_max[0] = c->world_center[0] + ex;
        c->aabb_max[1] = c->world_center[1] + ey;
        c->aabb_max[2] = c->world_center[2] + ez;
    }
    c->aabb_valid = 1;
    return 1;
}
