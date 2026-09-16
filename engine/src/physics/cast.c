/*
 * Shape casts / sweeps (Phase 30): sphere, capsule, and box casts
 * against step-fresh collider poses.
 *
 * Semantics:
 * - fraction in [0,1]: 0 = immediate hit at start, 1 = full
 *   movement free. distance = fraction * |displacement|.
 * - Zero displacement = overlap query at the start pose (hit iff
 *   penetrating with fraction 0 + started_overlapping).
 * - Initial overlap never pretends collision-free: fraction 0,
 *   started_overlapping = 1, depenetration normal/depth.
 * - Swept AABB = union(start AABB, end AABB) + margin selects
 *   candidates through the existing collider set (layer/mask both
 *   directions, self-skip, trigger policy). Ties break by stable
 *   (slot, generation) identity, never traversal order.
 * - Narrow TOI: sphere sweeps are analytic (Minkowski expansion:
 *   sphere-vs-sphere = ray-sphere with R+r; sphere-vs-box = ray
 *   vs expanded OBB; sphere-vs-capsule = ray vs expanded capsule
 *   = cylinder + cap spheres). Capsule sweeps use conservative
 *   advancement (bounded 32 iterations on clearance): robust for
 *   every static pair without closed forms. Box sweeps use the
 *   same conservative advancement with box-vs-shape clearance
 *   from SAT narrow phase. All TOI math is re-entrant scratch
 *   only — no heap per cast (bounded stack storage).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

#define LE_CAST_MAX_ITERS 32u
#define LE_CAST_MARGIN 1e-4f

static float le_c_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float le_c_len(const float a[3]) {
    return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

static void le_c_normalize(float a[3]) {
    float l = le_c_len(a);

    if (l > 1e-12f && isfinite(l)) {
        a[0] /= l;
        a[1] /= l;
        a[2] /= l;
    } else {
        a[0] = 1.0f;
        a[1] = 0.0f;
        a[2] = 0.0f;
    }
}

/* Build a cast-shape probe collider entry in WORLD frame (the
 * narrow phase consumes world_* fields). Orientation is a unit
 * quat mapping local shape axes to world; for capsules local Y
 * is the segment axis. Returns 0 on malformed input. */
static int le_cast_probe(le_cast_shape shape, const float center[3],
                         const float orientation[4],
                         const float dims[3],
                         le_collider_entry *out) {
    float qx;
    float qy;
    float qz;
    float qw;
    float ql;

    if (center == NULL || out == NULL) {
        return 0;
    }
    if (!isfinite(center[0]) || !isfinite(center[1]) ||
        !isfinite(center[2])) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->slot = 0xFFFFFFFFu;
    if (shape == LE_CAST_SPHERE) {
        float r = (dims != NULL) ? dims[0] : 0.0f;

        if (!isfinite(r) || r <= 0.0f || r > 1e6f) {
            return 0;
        }
        out->shape = LE_COLLIDER_SPHERE;
        out->world_radius = r;
        out->world_center[0] = center[0];
        out->world_center[1] = center[1];
        out->world_center[2] = center[2];
        out->aabb_valid = 1;
        out->aabb_min[0] = center[0] - r;
        out->aabb_min[1] = center[1] - r;
        out->aabb_min[2] = center[2] - r;
        out->aabb_max[0] = center[0] + r;
        out->aabb_max[1] = center[1] + r;
        out->aabb_max[2] = center[2] + r;
        return 1;
    }
    if (orientation == NULL) {
        return 0;
    }
    qx = orientation[0];
    qy = orientation[1];
    qz = orientation[2];
    qw = orientation[3];
    ql = qx * qx + qy * qy + qz * qz + qw * qw;
    if (!isfinite(ql) || ql < 1e-12f) {
        return 0;
    }
    {
        /* Unit quat -> world basis columns. */
        float inv = 1.0f / sqrtf(ql);

        qx *= inv;
        qy *= inv;
        qz *= inv;
        qw *= inv;
        out->world_basis[0][0] =
            1.0f - 2.0f * (qy * qy + qz * qz);
        out->world_basis[1][0] =
            2.0f * (qx * qy + qz * qw);
        out->world_basis[2][0] =
            2.0f * (qx * qz - qy * qw);
        out->world_basis[0][1] =
            2.0f * (qx * qy - qz * qw);
        out->world_basis[1][1] =
            1.0f - 2.0f * (qx * qx + qz * qz);
        out->world_basis[2][1] =
            2.0f * (qy * qz + qx * qw);
        out->world_basis[0][2] =
            2.0f * (qx * qz + qy * qw);
        out->world_basis[1][2] =
            2.0f * (qy * qz - qx * qw);
        out->world_basis[2][2] =
            1.0f - 2.0f * (qx * qx + qy * qy);
    }
    if (shape == LE_CAST_CAPSULE) {
        float r = (dims != NULL) ? dims[0] : 0.0f;
        float h = (dims != NULL) ? dims[1] : -1.0f;
        float ax;
        float ay;
        float az;

        if (!isfinite(r) || r <= 0.0f || r > 1e6f) {
            return 0;
        }
        if (!isfinite(h) || h < 0.0f || h > 1e6f) {
            return 0;
        }
        out->shape = LE_COLLIDER_CAPSULE;
        out->world_cap_radius = r;
        out->world_cap_half = h;
        out->world_center[0] = center[0];
        out->world_center[1] = center[1];
        out->world_center[2] = center[2];
        /* Segment axis = local Y column. */
        ax = out->world_basis[0][1];
        ay = out->world_basis[1][1];
        az = out->world_basis[2][1];
        {
            float al = sqrtf(ax * ax + ay * ay + az * az);

            if (al < 1e-9f || !isfinite(al)) {
                return 0;
            }
            ax /= al;
            ay /= al;
            az /= al;
        }
        out->world_axis[0] = ax;
        out->world_axis[1] = ay;
        out->world_axis[2] = az;
        out->world_p0[0] = center[0] - ax * h;
        out->world_p0[1] = center[1] - ay * h;
        out->world_p0[2] = center[2] - az * h;
        out->world_p1[0] = center[0] + ax * h;
        out->world_p1[1] = center[1] + ay * h;
        out->world_p1[2] = center[2] + az * h;
        {
            float ex = ((ax < 0.0f) ? -ax : ax) * h + r;
            float ey = ((ay < 0.0f) ? -ay : ay) * h + r;
            float ez = ((az < 0.0f) ? -az : az) * h + r;

            out->aabb_min[0] = center[0] - ex;
            out->aabb_min[1] = center[1] - ey;
            out->aabb_min[2] = center[2] - ez;
            out->aabb_max[0] = center[0] + ex;
            out->aabb_max[1] = center[1] + ey;
            out->aabb_max[2] = center[2] + ez;
        }
        out->aabb_valid = 1;
        return 1;
    }
    if (shape == LE_CAST_BOX) {
        float ex;
        float ey;
        float ez;

        if (dims == NULL || !isfinite(dims[0]) ||
            !isfinite(dims[1]) || !isfinite(dims[2]) ||
            dims[0] <= 0.0f || dims[1] <= 0.0f ||
            dims[2] <= 0.0f || dims[0] > 1e6f ||
            dims[1] > 1e6f || dims[2] > 1e6f) {
            return 0;
        }
        out->shape = LE_COLLIDER_BOX;
        out->world_half[0] = dims[0];
        out->world_half[1] = dims[1];
        out->world_half[2] = dims[2];
        out->world_center[0] = center[0];
        out->world_center[1] = center[1];
        out->world_center[2] = center[2];
        {
            float ax0 = out->world_basis[0][0];
            float ax1 = out->world_basis[0][1];
            float ax2 = out->world_basis[0][2];

            if (ax0 < 0.0f) {
                ax0 = -ax0;
            }
            if (ax1 < 0.0f) {
                ax1 = -ax1;
            }
            if (ax2 < 0.0f) {
                ax2 = -ax2;
            }
            ex = ax0 * dims[0] + ax1 * dims[1] + ax2 * dims[2];
        }
        {
            float ay0 = out->world_basis[1][0];
            float ay1 = out->world_basis[1][1];
            float ay2 = out->world_basis[1][2];

            if (ay0 < 0.0f) {
                ay0 = -ay0;
            }
            if (ay1 < 0.0f) {
                ay1 = -ay1;
            }
            if (ay2 < 0.0f) {
                ay2 = -ay2;
            }
            ey = ay0 * dims[0] + ay1 * dims[1] + ay2 * dims[2];
        }
        {
            float az0 = out->world_basis[2][0];
            float az1 = out->world_basis[2][1];
            float az2 = out->world_basis[2][2];

            if (az0 < 0.0f) {
                az0 = -az0;
            }
            if (az1 < 0.0f) {
                az1 = -az1;
            }
            if (az2 < 0.0f) {
                az2 = -az2;
            }
            ez = az0 * dims[0] + az1 * dims[1] + az2 * dims[2];
        }
        out->aabb_min[0] = center[0] - ex;
        out->aabb_min[1] = center[1] - ey;
        out->aabb_min[2] = center[2] - ez;
        out->aabb_max[0] = center[0] + ex;
        out->aabb_max[1] = center[1] + ey;
        out->aabb_max[2] = center[2] + ez;
        out->aabb_valid = 1;
        return 1;
    }
    return 0;
}

/* Translate a probe by d*t (segment endpoints + center). */
static void le_probe_advance(le_collider_entry *p, const float d[3],
                             float t) {
    p->world_center[0] += d[0] * t;
    p->world_center[1] += d[1] * t;
    p->world_center[2] += d[2] * t;
    if (p->shape == LE_COLLIDER_CAPSULE) {
        p->world_p0[0] += d[0] * t;
        p->world_p0[1] += d[1] * t;
        p->world_p0[2] += d[2] * t;
        p->world_p1[0] += d[0] * t;
        p->world_p1[1] += d[1] * t;
        p->world_p1[2] += d[2] * t;
    }
}

/* Clearance between a probe and a static collider: penetration-
 * style query via narrow phase (hit => -penetration, miss =>
 * +separation estimate). For miss separation we need distance,
 * not just boolean: approximate with feature distances —
 * sphere probes: exact (center distance / OBB distance);
 * capsule probes: exact segment-vs-shape distance (segment vs
 * sphere-center / segment vs OBB / segment vs segment);
 * box probes: narrow-phase hit test at the pose plus
 * AABB-gap lower bound. Conservative advancement only needs a
 * SAFE lower bound on distance (never an overestimate). */
static float le_probe_clearance(le_world *world,
                                const le_collider_entry *probe,
                                const le_collider_entry *c) {
    le_contact_point cp;

    memset(&cp, 0, sizeof(cp));
    if (le_physics_narrow_pair(world, probe, c, &cp)) {
        return -cp.penetration; /* penetrating: negative */
    }
    if (probe->shape == LE_COLLIDER_CAPSULE) {
        /* Exact segment-vs-shape clearance (safe AND tight, so
         * sub-half sweeps converge within the iteration cap). */
        if (c->shape == LE_COLLIDER_SPHERE) {
            float q[3];
            float dx;
            float dy;
            float dz;
            float dd;

            le_seg_closest_point(probe->world_p0,
                                 probe->world_p1,
                                 c->world_center, q);
            dx = q[0] - c->world_center[0];
            dy = q[1] - c->world_center[1];
            dz = q[2] - c->world_center[2];
            dd = sqrtf(dx * dx + dy * dy + dz * dz) -
                 probe->world_cap_radius -
                 c->world_radius;
            return (dd > 0.0f) ? dd : 0.0f;
        }
        if (c->shape == LE_COLLIDER_BOX) {
            float sq[3];
            float bq[3];
            float dx;
            float dy;
            float dz;
            float dd;

            (void)le_seg_obb_closest(
                probe->world_p0, probe->world_p1,
                c->world_center, c->world_basis[0],
                c->world_basis[1], c->world_basis[2],
                c->world_half, sq, bq);
            dx = sq[0] - bq[0];
            dy = sq[1] - bq[1];
            dz = sq[2] - bq[2];
            dd = sqrtf(dx * dx + dy * dy + dz * dz) -
                 probe->world_cap_radius;
            return (dd > 0.0f) ? dd : 0.0f;
        }
        if (c->shape == LE_COLLIDER_CAPSULE) {
            float c1[3];
            float c2[3];
            float dx;
            float dy;
            float dz;
            float dd;

            (void)le_seg_seg_closest(
                probe->world_p0, probe->world_p1,
                c->world_p0, c->world_p1, c1, c2);
            dx = c1[0] - c2[0];
            dy = c1[1] - c2[1];
            dz = c1[2] - c2[2];
            dd = sqrtf(dx * dx + dy * dy + dz * dz) -
                 probe->world_cap_radius -
                 c->world_cap_radius;
            return (dd > 0.0f) ? dd : 0.0f;
        }
    }
    /* Separated: safe lower bound via AABBs of probe vs target
     * (always <= true distance, never overestimates). Probe
     * AABB is rebuilt by the caller at each advance. */
    {
        float gx = 0.0f;
        float gy = 0.0f;
        float gz = 0.0f;

        if (probe->aabb_max[0] < c->aabb_min[0]) {
            gx = c->aabb_min[0] - probe->aabb_max[0];
        } else if (c->aabb_max[0] < probe->aabb_min[0]) {
            gx = probe->aabb_min[0] - c->aabb_max[0];
        }
        if (probe->aabb_max[1] < c->aabb_min[1]) {
            gy = c->aabb_min[1] - probe->aabb_max[1];
        } else if (c->aabb_max[1] < probe->aabb_min[1]) {
            gy = probe->aabb_min[1] - c->aabb_max[1];
        }
        if (probe->aabb_max[2] < c->aabb_min[2]) {
            gz = c->aabb_min[2] - probe->aabb_max[2];
        } else if (c->aabb_max[2] < probe->aabb_min[2]) {
            gz = probe->aabb_min[2] - c->aabb_max[2];
        }
        /* AABB gap is a lower bound on the true clearance, but
         * for rotated boxes it can be much smaller (safe, just
         * slower convergence). For sphere probes use the exact
         * distance to keep oracles tight. */
        if (probe->shape == LE_COLLIDER_SPHERE) {
            if (c->shape == LE_COLLIDER_SPHERE) {
                float dx = c->world_center[0] -
                           probe->world_center[0];
                float dy = c->world_center[1] -
                           probe->world_center[1];
                float dz = c->world_center[2] -
                           probe->world_center[2];
                float dd = sqrtf(dx * dx + dy * dy +
                                 dz * dz) -
                           probe->world_radius -
                           c->world_radius;

                return (dd > 0.0f) ? dd : 0.0f;
            }
            if (c->shape == LE_COLLIDER_BOX) {
                /* Exact point-vs-OBB distance minus radius. */
                float dx = probe->world_center[0] -
                           c->world_center[0];
                float dy = probe->world_center[1] -
                           c->world_center[1];
                float dz = probe->world_center[2] -
                           c->world_center[2];
                float ex = 0.0f;
                float ey = 0.0f;
                float ez = 0.0f;
                float s;

                s = dx * c->world_basis[0][0] +
                    dy * c->world_basis[1][0] +
                    dz * c->world_basis[2][0];
                if (s < -c->world_half[0]) {
                    float dd = s + c->world_half[0];

                    ex += c->world_basis[0][0] * dd;
                    ey += c->world_basis[1][0] * dd;
                    ez += c->world_basis[2][0] * dd;
                } else if (s > c->world_half[0]) {
                    float dd = s - c->world_half[0];

                    ex += c->world_basis[0][0] * dd;
                    ey += c->world_basis[1][0] * dd;
                    ez += c->world_basis[2][0] * dd;
                }
                s = dx * c->world_basis[0][1] +
                    dy * c->world_basis[1][1] +
                    dz * c->world_basis[2][1];
                if (s < -c->world_half[1]) {
                    float dd = s + c->world_half[1];

                    ex += c->world_basis[0][1] * dd;
                    ey += c->world_basis[1][1] * dd;
                    ez += c->world_basis[2][1] * dd;
                } else if (s > c->world_half[1]) {
                    float dd = s - c->world_half[1];

                    ex += c->world_basis[0][1] * dd;
                    ey += c->world_basis[1][1] * dd;
                    ez += c->world_basis[2][1] * dd;
                }
                s = dx * c->world_basis[0][2] +
                    dy * c->world_basis[1][2] +
                    dz * c->world_basis[2][2];
                if (s < -c->world_half[2]) {
                    float dd = s + c->world_half[2];

                    ex += c->world_basis[0][2] * dd;
                    ey += c->world_basis[1][2] * dd;
                    ez += c->world_basis[2][2] * dd;
                } else if (s > c->world_half[2]) {
                    float dd = s - c->world_half[2];

                    ex += c->world_basis[0][2] * dd;
                    ey += c->world_basis[1][2] * dd;
                    ez += c->world_basis[2][2] * dd;
                }
                {
                    float dd = sqrtf(ex * ex + ey * ey +
                                     ez * ez) -
                               probe->world_radius;

                    return (dd > 0.0f) ? dd : 0.0f;
                }
            }
        }
        {
            float gap =
                sqrtf(gx * gx + gy * gy + gz * gz);

            return gap;
        }
    }
}

/* Rebuild a translated probe's AABB (same formulas as the
 * pose refresh, without matrix work — center/endpoints known). */
static void le_probe_reaabb(le_collider_entry *p) {
    if (p->shape == LE_COLLIDER_SPHERE) {
        float r = p->world_radius;

        p->aabb_min[0] = p->world_center[0] - r;
        p->aabb_min[1] = p->world_center[1] - r;
        p->aabb_min[2] = p->world_center[2] - r;
        p->aabb_max[0] = p->world_center[0] + r;
        p->aabb_max[1] = p->world_center[1] + r;
        p->aabb_max[2] = p->world_center[2] + r;
    } else if (p->shape == LE_COLLIDER_CAPSULE) {
        float ax = p->world_axis[0];
        float ay = p->world_axis[1];
        float az = p->world_axis[2];
        float ex =
            ((ax < 0.0f) ? -ax : ax) * p->world_cap_half +
            p->world_cap_radius;
        float ey =
            ((ay < 0.0f) ? -ay : ay) * p->world_cap_half +
            p->world_cap_radius;
        float ez =
            ((az < 0.0f) ? -az : az) * p->world_cap_half +
            p->world_cap_radius;

        p->aabb_min[0] = p->world_center[0] - ex;
        p->aabb_min[1] = p->world_center[1] - ey;
        p->aabb_min[2] = p->world_center[2] - ez;
        p->aabb_max[0] = p->world_center[0] + ex;
        p->aabb_max[1] = p->world_center[1] + ey;
        p->aabb_max[2] = p->world_center[2] + ez;
    } else {
        float ex;
        float ey;
        float ez;
        float a0 = p->world_basis[0][0];
        float a1 = p->world_basis[0][1];
        float a2 = p->world_basis[0][2];

        if (a0 < 0.0f) {
            a0 = -a0;
        }
        if (a1 < 0.0f) {
            a1 = -a1;
        }
        if (a2 < 0.0f) {
            a2 = -a2;
        }
        ex = a0 * p->world_half[0] + a1 * p->world_half[1] +
             a2 * p->world_half[2];
        a0 = p->world_basis[1][0];
        a1 = p->world_basis[1][1];
        a2 = p->world_basis[1][2];
        if (a0 < 0.0f) {
            a0 = -a0;
        }
        if (a1 < 0.0f) {
            a1 = -a1;
        }
        if (a2 < 0.0f) {
            a2 = -a2;
        }
        ey = a0 * p->world_half[0] + a1 * p->world_half[1] +
             a2 * p->world_half[2];
        a0 = p->world_basis[2][0];
        a1 = p->world_basis[2][1];
        a2 = p->world_basis[2][2];
        if (a0 < 0.0f) {
            a0 = -a0;
        }
        if (a1 < 0.0f) {
            a1 = -a1;
        }
        if (a2 < 0.0f) {
            a2 = -a2;
        }
        ez = a0 * p->world_half[0] + a1 * p->world_half[1] +
             a2 * p->world_half[2];
        p->aabb_min[0] = p->world_center[0] - ex;
        p->aabb_min[1] = p->world_center[1] - ey;
        p->aabb_min[2] = p->world_center[2] - ez;
        p->aabb_max[0] = p->world_center[0] + ex;
        p->aabb_max[1] = p->world_center[1] + ey;
        p->aabb_max[2] = p->world_center[2] + ez;
    }
}

/* Analytic sphere-vs-sphere sweep TOI in fraction [0,1]:
 * ray p+t*d vs sphere (center c, radius R+r). Returns 1 with
 * *out_t when a blocking hit exists in range. */
static int le_sweep_sphere_sphere(const float p[3],
                                  const float d[3],
                                  const float c[3], float R,
                                  float *out_t) {
    float oc[3];
    float A;
    float B;
    float C;
    float disc;

    oc[0] = p[0] - c[0];
    oc[1] = p[1] - c[1];
    oc[2] = p[2] - c[2];
    A = le_c_dot(d, d);
    if (A < 1e-24f) {
        return 0;
    }
    B = le_c_dot(oc, d);
    C = le_c_dot(oc, oc) - R * R;
    if (C <= 0.0f) {
        *out_t = 0.0f; /* starts touching/overlapping */
        return 1;
    }
    disc = B * B - A * C;
    if (disc < 0.0f) {
        return 0;
    }
    {
        float sq = sqrtf(disc);
        float t = (-B - sq) / A;

        if (t < 0.0f || t > 1.0f) {
            return 0;
        }
        *out_t = t;
        return 1;
    }
}

/* Analytic sphere-vs-OBB sweep: ray vs box expanded by radius
 * (slab test in the box frame). Returns 1 with *out_t and the
 * face normal (world) on hit. */
static int le_sweep_sphere_box(const float p[3], const float d[3],
                               const le_collider_entry *bx,
                               float radius, float *out_t,
                               float out_n[3]) {
    float lo[3];
    float ld[3];
    float mn[3];
    float mx[3];
    int i;
    float lx = p[0] - bx->world_center[0];
    float ly = p[1] - bx->world_center[1];
    float lz = p[2] - bx->world_center[2];

    for (i = 0; i < 3; i++) {
        lo[i] = lx * bx->world_basis[0][i] +
                ly * bx->world_basis[1][i] +
                lz * bx->world_basis[2][i];
        ld[i] = d[0] * bx->world_basis[0][i] +
                d[1] * bx->world_basis[1][i] +
                d[2] * bx->world_basis[2][i];
        mn[i] = -bx->world_half[i] - radius;
        mx[i] = bx->world_half[i] + radius;
    }
    /* The narrow phase treats exact touch (dist == sum) as a
     * MISS (solver convention: penetration > 0 required), so
     * the sweep must agree: shrink the overlap region by a
     * touch epsilon. True penetration still reports t=0. */
    for (i = 0; i < 3; i++) {
        mn[i] += 1e-6f;
        mx[i] -= 1e-6f;
    }
    /* The narrow phase treats exact touch (dist == sum) as a
     * MISS (solver convention: penetration > 0 required), so
     * the sweep's overlap region shrank above by a touch
     * epsilon. True penetration still reports t=0. */
    {
        /* Inside the expanded box => overlap hit with
         * min-penetration normal. */
        if (lo[0] > mn[0] && lo[0] < mx[0] && lo[1] > mn[1] &&
            lo[1] < mx[1] && lo[2] > mn[2] &&
            lo[2] < mx[2]) {
            *out_t = 0.0f;
            /* Normal = min-penetration face (local), to world. */
            {
                float ex = mx[0] - lo[0];
                float ex0 = lo[0] - mn[0];
                float ey = mx[1] - lo[1];
                float ey0 = lo[1] - mn[1];
                float ez = mx[2] - lo[2];
                float ez0 = lo[2] - mn[2];
                float best = ex;
                float ln[3] = { 1.0f, 0.0f, 0.0f };

                if (ex0 < best) {
                    best = ex0;
                    ln[0] = -1.0f;
                    ln[1] = 0.0f;
                    ln[2] = 0.0f;
                }
                if (ey < best) {
                    best = ey;
                    ln[0] = 0.0f;
                    ln[1] = 1.0f;
                    ln[2] = 0.0f;
                }
                if (ey0 < best) {
                    best = ey0;
                    ln[0] = 0.0f;
                    ln[1] = -1.0f;
                    ln[2] = 0.0f;
                }
                if (ez < best) {
                    best = ez;
                    ln[0] = 0.0f;
                    ln[1] = 0.0f;
                    ln[2] = 1.0f;
                }
                if (ez0 < best) {
                    ln[0] = 0.0f;
                    ln[1] = 0.0f;
                    ln[2] = -1.0f;
                }
                out_n[0] = bx->world_basis[0][0] * ln[0] +
                           bx->world_basis[0][1] * ln[1] +
                           bx->world_basis[0][2] * ln[2];
                out_n[1] = bx->world_basis[1][0] * ln[0] +
                           bx->world_basis[1][1] * ln[1] +
                           bx->world_basis[1][2] * ln[2];
                out_n[2] = bx->world_basis[2][0] * ln[0] +
                           bx->world_basis[2][1] * ln[1] +
                           bx->world_basis[2][2] * ln[2];
            }
            return 1;
        }
    }
    {
        float tmin = 0.0f;
        float tmax = 1.0f;
        int hit_axis = -1;
        float hit_sign = 1.0f;

        for (i = 0; i < 3; i++) {
            float o = lo[i];
            float dd = ld[i];
            float t1;
            float t2;

            if (dd > -1e-12f && dd < 1e-12f) {
                if (o < mn[i] || o > mx[i]) {
                    return 0;
                }
                continue;
            }
            t1 = (mn[i] - o) / dd;
            t2 = (mx[i] - o) / dd;
            {
                float ts = (t1 < t2) ? t1 : t2;

                if (ts > tmin) {
                    hit_axis = i;
                    hit_sign = (t1 < t2) ? -1.0f : 1.0f;
                }
            }
            if (t1 > t2) {
                float tmp = t1;

                t1 = t2;
                t2 = tmp;
            }
            if (t1 > tmin) {
                tmin = t1;
            }
            if (t2 < tmax) {
                tmax = t2;
            }
            if (tmin > tmax) {
                return 0;
            }
        }
        if (tmin < 0.0f || tmin > 1.0f) {
            return 0;
        }
        *out_t = tmin;
        if (hit_axis < 0) {
            out_n[0] = 0.0f;
            out_n[1] = 1.0f;
            out_n[2] = 0.0f;
        } else {
            float ln[3] = { 0.0f, 0.0f, 0.0f };

            ln[hit_axis] = hit_sign;
            out_n[0] = bx->world_basis[0][0] * ln[0] +
                       bx->world_basis[0][1] * ln[1] +
                       bx->world_basis[0][2] * ln[2];
            out_n[1] = bx->world_basis[1][0] * ln[0] +
                       bx->world_basis[1][1] * ln[1] +
                       bx->world_basis[1][2] * ln[2];
            out_n[2] = bx->world_basis[2][0] * ln[0] +
                       bx->world_basis[2][1] * ln[1] +
                       bx->world_basis[2][2] * ln[2];
        }
        return 1;
    }
}

/* Sphere-vs-capsule sweep: ray vs capsule expanded by the sweep
 * radius (cylinder part + cap spheres). Returns 1 with *out_t
 * and world normal. */
static int le_sweep_sphere_capsule(const float p[3],
                                   const float d[3],
                                   const le_collider_entry *cap,
                                   float radius, float *out_t,
                                   float out_n[3]) {
    float R = cap->world_cap_radius + radius;
    float best = 2.0f;
    int found = 0;
    float bn[3] = { 0.0f, 1.0f, 0.0f };
    float abx = cap->world_p1[0] - cap->world_p0[0];
    float aby = cap->world_p1[1] - cap->world_p0[1];
    float abz = cap->world_p1[2] - cap->world_p0[2];
    float len2 = abx * abx + aby * aby + abz * abz;

    if (len2 > 1e-18f) {
        float ul = sqrtf(len2);
        float ux = abx / ul;
        float uy = aby / ul;
        float uz = abz / ul;
        float ocx = p[0] - cap->world_center[0];
        float ocy = p[1] - cap->world_center[1];
        float ocz = p[2] - cap->world_center[2];
        float d_u = le_c_dot(d, (float[3]){ ux, uy, uz });
        float o_u = ocx * ux + ocy * uy + ocz * uz;
        float ex = d[0] - ux * d_u;
        float ey = d[1] - uy * d_u;
        float ez = d[2] - uz * d_u;
        float fx = ocx - ux * o_u;
        float fy = ocy - uy * o_u;
        float fz = ocz - uz * o_u;
        float A = ex * ex + ey * ey + ez * ez;
        float B = ex * fx + ey * fy + ez * fz;
        float C = fx * fx + fy * fy + fz * fz - R * R;

        if (A > 1e-24f) {
            float disc = B * B - A * C;

            if (C <= 0.0f) {
                /* Starts inside the infinite cylinder slab:
                 * check the axial range; if inside, t=0. */
                float s = o_u;

                if (s >= -cap->world_cap_half &&
                    s <= cap->world_cap_half) {
                    *out_t = 0.0f;
                    /* Normal = radial. */
                    {
                        float nl =
                            sqrtf(fx * fx + fy * fy + fz * fz);

                        if (nl > 1e-9f) {
                            out_n[0] = fx / nl;
                            out_n[1] = fy / nl;
                            out_n[2] = fz / nl;
                        } else {
                            out_n[0] = -d[0];
                            out_n[1] = -d[1];
                            out_n[2] = -d[2];
                            le_c_normalize(out_n);
                        }
                    }
                    return 1;
                }
            }
            if (disc >= 0.0f) {
                float sq = sqrtf(disc);
                float tc[2] = { (-B - sq) / A,
                                (-B + sq) / A };
                int k;

                for (k = 0; k < 2; k++) {
                    float t = tc[k];
                    float hx;
                    float hy;
                    float hz;
                    float s;

                    if (t < 0.0f || t > 1.0f) {
                        continue;
                    }
                    hx = p[0] + d[0] * t -
                         cap->world_center[0];
                    hy = p[1] + d[1] * t -
                         cap->world_center[1];
                    hz = p[2] + d[2] * t -
                         cap->world_center[2];
                    s = hx * ux + hy * uy + hz * uz;
                    if (s < -cap->world_cap_half ||
                        s > cap->world_cap_half) {
                        continue;
                    }
                    if (!found || t < best) {
                        float rx = hx - ux * s;
                        float ry = hy - uy * s;
                        float rz = hz - uz * s;
                        float rl =
                            sqrtf(rx * rx + ry * ry + rz * rz);

                        found = 1;
                        best = t;
                        if (rl > 1e-9f) {
                            bn[0] = rx / rl;
                            bn[1] = ry / rl;
                            bn[2] = rz / rl;
                        } else {
                            bn[0] = -d[0];
                            bn[1] = -d[1];
                            bn[2] = -d[2];
                            le_c_normalize(bn);
                        }
                    }
                }
            }
        }
    }
    {
        const float *caps[2] = { cap->world_p0,
                                 cap->world_p1 };
        int k;

        for (k = 0; k < 2; k++) {
            float t;

            if (!le_sweep_sphere_sphere(p, d, caps[k], R,
                                        &t)) {
                continue;
            }
            if (!found || t < best) {
                float hx = p[0] + d[0] * t - caps[k][0];
                float hy = p[1] + d[1] * t - caps[k][1];
                float hz = p[2] + d[2] * t - caps[k][2];
                float hl = sqrtf(hx * hx + hy * hy +
                                 hz * hz);

                found = 1;
                best = t;
                if (hl > 1e-9f) {
                    bn[0] = hx / hl;
                    bn[1] = hy / hl;
                    bn[2] = hz / hl;
                } else {
                    bn[0] = -d[0];
                    bn[1] = -d[1];
                    bn[2] = -d[2];
                    le_c_normalize(bn);
                }
            }
        }
    }
    if (!found) {
        return 0;
    }
    *out_t = best;
    out_n[0] = bn[0];
    out_n[1] = bn[1];
    out_n[2] = bn[2];
    return 1;
}

/* Refresh poses on demand (same as query.c: queries work before
 * the first fixed step). */
static void le_cast_refresh(le_world *world) {
    struct le_physics_world *pw;
    uint32_t i;

    if (world == NULL || world->physics == NULL) {
        return;
    }
    pw = world->physics;
    le_refresh_world_matrices(world);
    for (i = 0; i < pw->collider_count; i++) {
        le_collider_entry *c = &pw->colliders[i];

        if (c->slot >= world->capacity ||
            !world->slots[c->slot].alive) {
            c->aabb_valid = 0;
            continue;
        }
        if (!(world->slots[c->slot].present &
              LE_PRESENT_COLLIDER)) {
            c->aabb_valid = 0;
            continue;
        }
        le_physics_refresh_collider(world, c);
    }
}

/* Candidate filter shared with the broad phase discipline.
 * NOTE: unlike the discrete broad phase (which requires BOTH
 * masks to contain the other's layer), sweeps and CCD use the
 * query convention (Phase 28 raycast/overlap): the caller's
 * layer_mask selects target layers unilaterally. Self-hits are
 * excluded by slot, and the target's own mask does not veto a
 * query — queries are observations, not simulated contacts. */
static int le_cast_visible(const le_collider_entry *c,
                           uint32_t layer_mask,
                           int hit_triggers,
                           int blocking_pass) {
    uint32_t bit;

    if (c->layer > 31u) {
        return 0;
    }
    bit = 1u << c->layer;
    if ((layer_mask & bit) == 0u) {
        return 0;
    }
    if (blocking_pass) {
        /* Triggers never block locomotion/sweeps. */
        if (c->is_trigger) {
            return 0;
        }
    } else {
        if (c->is_trigger && !hit_triggers) {
            return 0;
        }
        if (!c->is_trigger) {
            return 0; /* trigger pass: triggers only */
        }
    }
    if (!c->aabb_valid) {
        return 0;
    }
    return 1;
}

/* Swept-AABB reject: union(probe start/end AABB) vs target AABB. */
static int le_swept_aabb_hit(const float smin[3],
                             const float smax[3],
                             const le_collider_entry *c) {
    if (smin[0] > c->aabb_max[0] || c->aabb_min[0] > smax[0]) {
        return 0;
    }
    if (smin[1] > c->aabb_max[1] || c->aabb_min[1] > smax[1]) {
        return 0;
    }
    if (smin[2] > c->aabb_max[2] || c->aabb_min[2] > smax[2]) {
        return 0;
    }
    return 1;
}

/* Conservative-advancement TOI for capsule/box probes vs one
 * static collider. probe is the START pose (translated
 * internally); d is the full displacement. Returns 1 with
 * *out_t (fraction) on hit. */
static int le_advance_toi(le_world *world,
                          const le_collider_entry *start,
                          const float d[3],
                          const le_collider_entry *c,
                          float *out_t) {
    float dlen = le_c_len(d);
    le_collider_entry probe;
    float t = 0.0f;
    uint32_t iter;

    if (dlen < 1e-12f || !isfinite(dlen)) {
        return 0; /* zero displacement handled as overlap */
    }
    memcpy(&probe, start, sizeof(probe));
    for (iter = 0; iter < LE_CAST_MAX_ITERS; iter++) {
        float clearance;
        float step;

        le_probe_reaabb(&probe);
        clearance =
            le_probe_clearance(world, &probe, c);
        if (clearance <= 0.0f) {
            *out_t = t;
            return 1;
        }
        /* Safe advance: clearance / |d| of the remaining path,
         * clamped to reach exactly t=1. Never overshoots: the
         * clearance is a lower bound on distance, so moving
         * less than clearance/|d| cannot pass through. */
        step = clearance / dlen;
        if (step < 1e-6f) {
            step = 1e-6f;
        }
        if (t + step >= 1.0f) {
            /* Final pose check at t=1. */
            le_probe_advance(&probe, d, 1.0f - t);
            le_probe_reaabb(&probe);
            clearance =
                le_probe_clearance(world, &probe, c);
            if (clearance <= 0.0f) {
                *out_t = 1.0f;
                return 1;
            }
            return 0;
        }
        le_probe_advance(&probe, d, step);
        t += step;
    }
    /* No convergence within the cap: treat the nearest approach
     * as the TOI only if within margin, else miss. (Cap is high
     * enough that analytic sphere paths never reach here.) */
    le_probe_reaabb(&probe);
    if (le_probe_clearance(world, &probe, c) <= 0.0f) {
        *out_t = t;
        return 1;
    }
    return 0;
}

/* Contact normal + point at TOI pose: re-run narrow phase with
 * static collider toward the probe (against motion). */
static void le_toi_contact(le_world *world,
                           const le_collider_entry *start,
                           const float d[3], float t,
                           const le_collider_entry *c,
                           float out_n[3], float out_p[3],
                           float *out_dep) {
    le_collider_entry probe;
    le_contact_point cp;

    memcpy(&probe, start, sizeof(probe));
    le_probe_advance(&probe, d, t);
    le_probe_reaabb(&probe);
    memset(&cp, 0, sizeof(cp));
    if (le_physics_narrow_pair(world, c, &probe, &cp)) {
        /* narrow_pair(c, probe): normal is c -> probe =
         * against motion. Point is on c's surface. */
        out_n[0] = cp.normal[0];
        out_n[1] = cp.normal[1];
        out_n[2] = cp.normal[2];
        memcpy(out_p, cp.point, sizeof(cp.point));
        if (out_dep != NULL) {
            *out_dep = cp.penetration;
        }
        return;
    }
    /* Touching exactly (dist == sum): the narrow phase reports
     * a miss, but the sweep DID arrive at a surface. Retry
     * with a hair-inflated probe (1e-4): the overlap forces
     * the narrow phase to report the TRUE geometric normal
     * instead of a motion-aligned guess. (Without this, steep
     * slopes report -d = +up as the "ground" normal and read
     * as walkable.) */
    {
        le_collider_entry fat = probe;

        if (fat.shape == LE_COLLIDER_SPHERE) {
            fat.world_radius += 1e-4f;
        } else if (fat.shape == LE_COLLIDER_CAPSULE) {
            fat.world_cap_radius += 1e-4f;
        } else {
            fat.world_half[0] += 1e-4f;
            fat.world_half[1] += 1e-4f;
            fat.world_half[2] += 1e-4f;
        }
        le_probe_reaabb(&fat);
        memset(&cp, 0, sizeof(cp));
        if (le_physics_narrow_pair(world, c, &fat, &cp) &&
            cp.penetration > 0.0f) {
            out_n[0] = cp.normal[0];
            out_n[1] = cp.normal[1];
            out_n[2] = cp.normal[2];
            memcpy(out_p, cp.point, sizeof(cp.point));
            if (out_dep != NULL) {
                *out_dep = 0.0f;
            }
            return;
        }
    }
    /* Last resort: against the motion. */
    {
        float dl = le_c_len(d);

        if (dl > 1e-12f) {
            out_n[0] = -d[0] / dl;
            out_n[1] = -d[1] / dl;
            out_n[2] = -d[2] / dl;
        } else {
            out_n[0] = 0.0f;
            out_n[1] = 1.0f;
            out_n[2] = 0.0f;
        }
        /* Point: probe surface toward the motion. */
        out_p[0] = probe.world_center[0];
        out_p[1] = probe.world_center[1];
        out_p[2] = probe.world_center[2];
        if (out_dep != NULL) {
            *out_dep = 0.0f;
        }
    }
}

int le_physics_shape_cast(
    le_world *world, le_cast_shape shape, const float center[3],
    const float orientation[4], const float dims[3],
    const float displacement[3], uint32_t layer_mask,
    int hit_triggers, uint32_t exclude_slot,
    le_shape_hit *out_hit, le_shape_hit *out_trigger) {
    le_collider_entry probe;
    float d[3];
    float dlen;
    float smin[3];
    float smax[3];
    uint32_t i;
    /* Best blocking hit + best trigger overlap (deterministic
     * ordering by (fraction, slot, generation)). */
    int have_block = 0;
    le_shape_hit block;
    int have_trig = 0;
    le_shape_hit trig;

    if (out_hit != NULL) {
        memset(out_hit, 0, sizeof(*out_hit));
        out_hit->fraction = 1.0f;
        out_hit->object = LE_OBJECT_INVALID;
    }
    if (out_trigger != NULL) {
        memset(out_trigger, 0, sizeof(*out_trigger));
        out_trigger->fraction = 1.0f;
        out_trigger->object = LE_OBJECT_INVALID;
    }
    if (world == NULL || world->physics == NULL ||
        center == NULL || displacement == NULL) {
        return 0;
    }
    if (shape != LE_CAST_SPHERE && shape != LE_CAST_CAPSULE &&
        shape != LE_CAST_BOX) {
        return 0;
    }
    if (!isfinite(displacement[0]) || !isfinite(displacement[1]) ||
        !isfinite(displacement[2])) {
        return 0;
    }
    d[0] = displacement[0];
    d[1] = displacement[1];
    d[2] = displacement[2];
    dlen = le_c_len(d);
    if (!isfinite(dlen)) {
        return 0;
    }
    {
        float idims[3] = { 0.0f, 0.0f, 0.0f };

        if (shape == LE_CAST_SPHERE) {
            idims[0] = (dims != NULL) ? dims[0] : 0.0f;
        } else if (shape == LE_CAST_CAPSULE) {
            idims[0] = (dims != NULL) ? dims[0] : 0.0f;
            idims[1] = (dims != NULL) ? dims[1] : -1.0f;
        } else if (dims != NULL) {
            idims[0] = dims[0];
            idims[1] = dims[1];
            idims[2] = dims[2];
        }
        if (!le_cast_probe(shape, center, orientation, idims,
                           &probe)) {
            return 0;
        }
    }
    memset(&block, 0, sizeof(block));
    block.fraction = 1.0f;
    block.object = LE_OBJECT_INVALID;
    memset(&trig, 0, sizeof(trig));
    trig.fraction = 1.0f;
    trig.object = LE_OBJECT_INVALID;
    /* Poses may be stale (teleport since last step, or no step
     * ran yet): refresh exactly like every other query, and
     * invalidate nothing — le_cast_refresh only rebuilds. */
    le_cast_refresh(world);
    world->physics->stat_shape_casts++;
    /* Swept AABB = union(start, start + d) + margin. */
    for (i = 0; i < 3; i++) {
        float e0 = (&d[0])[i] < 0.0f ? (&d[0])[i] : 0.0f;
        float e1 = (&d[0])[i] > 0.0f ? (&d[0])[i] : 0.0f;

        (&smin[0])[i] = (&probe.aabb_min[0])[i] + e0 -
                        LE_CAST_MARGIN;
        (&smax[0])[i] = (&probe.aabb_max[0])[i] + e1 +
                        LE_CAST_MARGIN;
    }
    /* Zero displacement: overlap query at the start pose. */
    if (dlen < 1e-12f) {
        struct le_physics_world *pw = world->physics;

        for (i = 0; i < pw->collider_count; i++) {
            le_collider_entry *c = &pw->colliders[i];
            le_contact_point cp;

            if (c->slot == exclude_slot) {
                continue;
            }
            if (c->slot >= world->capacity ||
                !world->slots[c->slot].alive) {
                continue;
            }
            if (!(world->slots[c->slot].present &
                  LE_PRESENT_COLLIDER)) {
                continue;
            }
            if (!le_cast_visible(c, layer_mask, hit_triggers,
                                 1)) {
                continue;
            }
            pw->stat_cast_candidates++;
            memset(&cp, 0, sizeof(cp));
            if (!le_physics_narrow_pair(world, &probe, c,
                                        &cp) ||
                cp.penetration <= 0.0f) {
                continue;
            }
            /* Penetrating at zero displacement. */
            if (!have_block) {
                have_block = 1;
                block.object.index = c->slot;
                block.object.generation =
                    world->slots[c->slot].generation;
                block.object.world_tag = world->tag;
                block.fraction = 0.0f;
                block.distance = 0.0f;
                block.normal[0] = -cp.normal[0];
                block.normal[1] = -cp.normal[1];
                block.normal[2] = -cp.normal[2];
                memcpy(block.point, cp.point,
                       sizeof(block.point));
                block.started_overlapping = 1;
                block.penetration = cp.penetration;
            } else {
                /* Deterministic: keep the lowest (slot, gen). */
                uint32_t os = block.object.index;
                uint32_t og = block.object.generation;

                if (c->slot < os ||
                    (c->slot == os &&
                     world->slots[c->slot].generation < og)) {
                    block.object.index = c->slot;
                    block.object.generation =
                        world->slots[c->slot].generation;
                    block.normal[0] = -cp.normal[0];
                    block.normal[1] = -cp.normal[1];
                    block.normal[2] = -cp.normal[2];
                    memcpy(block.point, cp.point,
                           sizeof(block.point));
                    block.penetration = cp.penetration;
                }
            }
        }
        if (have_block && out_hit != NULL) {
            *out_hit = block;
        }
        return have_block;
    }
    /* Moving sweep: per-candidate TOI. */
    {
        struct le_physics_world *pw = world->physics;

        for (i = 0; i < pw->collider_count; i++) {
            le_collider_entry *c = &pw->colliders[i];
            float toi = 2.0f;
            float n[3] = { 0.0f, 1.0f, 0.0f };
            int hit = 0;

            if (c->slot == exclude_slot) {
                continue;
            }
            if (c->slot >= world->capacity ||
                !world->slots[c->slot].alive) {
                continue;
            }
            if (!(world->slots[c->slot].present &
                  LE_PRESENT_COLLIDER)) {
                continue;
            }
            if (!le_cast_visible(c, layer_mask, hit_triggers,
                                 1)) {
                continue;
            }
            if (!le_swept_aabb_hit(smin, smax, c)) {
                continue;
            }
            pw->stat_cast_candidates++;
            /* Initial overlap: narrow phase at t=0 hits. NOTE:
             * narrow_pair(probe, c) reports normal probe -> c
             * (into the obstacle); the sweep convention needs
             * the DEPENETRATION direction (obstacle -> probe),
             * hence the negation. True overlap only: touching
             * (dist == sum, penetration == 0) is a narrow-phase
             * MISS, so any hit here is genuine penetration. */
            {
                le_contact_point cp0;

                memset(&cp0, 0, sizeof(cp0));
                if (le_physics_narrow_pair(world, &probe, c,
                                           &cp0) &&
                    cp0.penetration > 0.0f) {
                    hit = 1;
                    toi = 0.0f;
                    n[0] = -cp0.normal[0];
                    n[1] = -cp0.normal[1];
                    n[2] = -cp0.normal[2];
                    /* Contact point from the overlap. */
                    if (!have_block || toi < block.fraction) {
                        le_shape_hit cand;

                        memset(&cand, 0, sizeof(cand));
                        cand.object.index = c->slot;
                        cand.object.generation =
                            world->slots[c->slot].generation;
                        cand.object.world_tag = world->tag;
                        cand.fraction = 0.0f;
                        cand.distance = 0.0f;
                        cand.normal[0] = n[0];
                        cand.normal[1] = n[1];
                        cand.normal[2] = n[2];
                        memcpy(cand.point, cp0.point,
                               sizeof(cand.point));
                        cand.started_overlapping = 1;
                        cand.penetration =
                            cp0.penetration;
                        if (!have_block ||
                            cand.fraction <
                                block.fraction - 1e-9f ||
                            ((cand.fraction <=
                                  block.fraction + 1e-9f) &&
                             (cand.object.index <
                                  block.object.index ||
                              (cand.object.index ==
                                   block.object.index &&
                               cand.object.generation <
                                   block.object.generation)))) {
                            block = cand;
                            have_block = 1;
                        }
                    }
                    continue;
                }
            }
            if (probe.shape == LE_COLLIDER_SPHERE) {
                if (c->shape == LE_COLLIDER_SPHERE) {
                    float t;

                    if (le_sweep_sphere_sphere(
                            probe.world_center, d,
                            c->world_center,
                            probe.world_radius +
                                c->world_radius,
                            &t)) {
                        float hx =
                            probe.world_center[0] + d[0] * t -
                            c->world_center[0];
                        float hy =
                            probe.world_center[1] + d[1] * t -
                            c->world_center[1];
                        float hz =
                            probe.world_center[2] + d[2] * t -
                            c->world_center[2];
                        float hl = sqrtf(hx * hx + hy * hy +
                                         hz * hz);

                        hit = 1;
                        toi = t;
                        if (hl > 1e-9f) {
                            /* Normal: from hit surface toward
                             * the cast = (probe - target)/|..|
                             * (against motion). */
                            n[0] = hx / hl;
                            n[1] = hy / hl;
                            n[2] = hz / hl;
                        } else {
                            n[0] = -d[0] / dlen;
                            n[1] = -d[1] / dlen;
                            n[2] = -d[2] / dlen;
                        }
                    }
                } else if (c->shape == LE_COLLIDER_BOX) {
                    float t;
                    float nn[3];

                    if (le_sweep_sphere_box(
                            probe.world_center, d, c,
                            probe.world_radius, &t, nn)) {
                        hit = 1;
                        toi = t;
                        n[0] = nn[0];
                        n[1] = nn[1];
                        n[2] = nn[2];
                    }
                } else if (c->shape ==
                           LE_COLLIDER_CAPSULE) {
                    float t;
                    float nn[3];

                    if (le_sweep_sphere_capsule(
                            probe.world_center, d, c,
                            probe.world_radius, &t, nn)) {
                        hit = 1;
                        toi = t;
                        n[0] = nn[0];
                        n[1] = nn[1];
                        n[2] = nn[2];
                    }
                }
            } else {
                /* Capsule/box probes: conservative advancement. */
                float t;

                if (le_advance_toi(world, &probe, d, c, &t)) {
                    float nn[3];
                    float pp[3];

                    hit = 1;
                    toi = t;
                    le_toi_contact(world, &probe, d, t, c, nn,
                                   pp, NULL);
                    n[0] = nn[0];
                    n[1] = nn[1];
                    n[2] = nn[2];
                }
            }
            if (!hit) {
                continue;
            }
            if (toi < 0.0f) {
                toi = 0.0f;
            } else if (toi > 1.0f) {
                toi = 1.0f;
            }
            {
                uint32_t gs =
                    world->slots[c->slot].generation;

                if (!have_block || toi < block.fraction -
                                                 1e-9f ||
                    ((toi <= block.fraction + 1e-9f) &&
                     (c->slot < block.object.index ||
                      (c->slot == block.object.index &&
                       gs < block.object.generation)))) {
                    le_shape_hit cand;

                    memset(&cand, 0, sizeof(cand));
                    cand.object.index = c->slot;
                    cand.object.generation = gs;
                    cand.object.world_tag = world->tag;
                    cand.fraction = toi;
                    cand.distance = toi * dlen;
                    if (probe.shape !=
                            LE_COLLIDER_SPHERE ||
                        (c->shape != LE_COLLIDER_SPHERE &&
                         c->shape != LE_COLLIDER_BOX &&
                         c->shape != LE_COLLIDER_CAPSULE)) {
                        float nn[3];
                        float pp[3];

                        le_toi_contact(world, &probe, d, toi,
                                       c, nn, pp, NULL);
                        cand.normal[0] = nn[0];
                        cand.normal[1] = nn[1];
                        cand.normal[2] = nn[2];
                        cand.point[0] = pp[0];
                        cand.point[1] = pp[1];
                        cand.point[2] = pp[2];
                    } else {
                        /* Sphere analytic paths: contact point
                         * = probe center at TOI pushed back
                         * along the normal by the radius. */
                        float hx =
                            probe.world_center[0] +
                            d[0] * toi;
                        float hy =
                            probe.world_center[1] +
                            d[1] * toi;
                        float hz =
                            probe.world_center[2] +
                            d[2] * toi;

                        cand.normal[0] = n[0];
                        cand.normal[1] = n[1];
                        cand.normal[2] = n[2];
                        cand.point[0] =
                            hx - n[0] * probe.world_radius;
                        cand.point[1] =
                            hy - n[1] * probe.world_radius;
                        cand.point[2] =
                            hz - n[2] * probe.world_radius;
                    }
                    cand.started_overlapping =
                        (toi <= 0.0f) ? 1 : 0;
                    block = cand;
                    have_block = 1;
                }
            }
        }
    }
    /* Trigger report-only pass (nearest trigger overlap along
     * the sweep; never affects the blocking result). */
    if (hit_triggers) {
        struct le_physics_world *pw = world->physics;

        for (i = 0; i < pw->collider_count; i++) {
            le_collider_entry *c = &pw->colliders[i];

            if (c->slot == exclude_slot) {
                continue;
            }
            if (c->slot >= world->capacity ||
                !world->slots[c->slot].alive) {
                continue;
            }
            if (!(world->slots[c->slot].present &
                  LE_PRESENT_COLLIDER)) {
                continue;
            }
            if (!le_cast_visible(c, layer_mask, hit_triggers,
                                 0)) {
                continue;
            }
            if (!le_swept_aabb_hit(smin, smax, c)) {
                continue;
            }
            {
                /* Trigger TOI = first fraction where the probe
                 * overlaps the trigger (conservative advance;
                 * sphere probes use the analytic sphere paths
                 * with trigger targets included). */
                float toi = 2.0f;
                int hit = 0;

                {
                    le_contact_point cp0;

                    memset(&cp0, 0, sizeof(cp0));
                    if (le_physics_narrow_pair(world, &probe,
                                               c, &cp0)) {
                        hit = 1;
                        toi = 0.0f;
                    }
                }
                if (!hit) {
                    float t;

                    if (probe.shape ==
                        LE_COLLIDER_SPHERE) {
                        if (c->shape ==
                            LE_COLLIDER_SPHERE) {
                            if (le_sweep_sphere_sphere(
                                    probe.world_center, d,
                                    c->world_center,
                                    probe.world_radius +
                                        c->world_radius,
                                    &t)) {
                                hit = 1;
                                toi = t;
                            }
                        } else if (
                            c->shape ==
                            LE_COLLIDER_BOX) {
                            float nn[3];

                            if (le_sweep_sphere_box(
                                    probe.world_center, d, c,
                                    probe.world_radius, &t,
                                    nn)) {
                                hit = 1;
                                toi = t;
                            }
                        } else if (
                            c->shape ==
                            LE_COLLIDER_CAPSULE) {
                            float nn[3];

                            if (le_sweep_sphere_capsule(
                                    probe.world_center, d, c,
                                    probe.world_radius, &t,
                                    nn)) {
                                hit = 1;
                                toi = t;
                            }
                        }
                    } else if (le_advance_toi(world, &probe,
                                              d, c, &t)) {
                        hit = 1;
                        toi = t;
                    }
                }
                if (!hit) {
                    continue;
                }
                if (toi < 0.0f) {
                    toi = 0.0f;
                } else if (toi > 1.0f) {
                    toi = 1.0f;
                }
                {
                    uint32_t gs =
                        world->slots[c->slot].generation;

                    if (!have_trig ||
                        toi < trig.fraction - 1e-9f ||
                        ((toi <= trig.fraction + 1e-9f) &&
                         (c->slot < trig.object.index ||
                          (c->slot == trig.object.index &&
                           gs <
                               trig.object.generation)))) {
                        float nn[3];
                        float pp[3];

                        le_toi_contact(world, &probe, d, toi,
                                       c, nn, pp, NULL);
                        memset(&trig, 0, sizeof(trig));
                        trig.object.index = c->slot;
                        trig.object.generation = gs;
                        trig.object.world_tag = world->tag;
                        trig.fraction = toi;
                        trig.distance = toi * dlen;
                        trig.normal[0] = nn[0];
                        trig.normal[1] = nn[1];
                        trig.normal[2] = nn[2];
                        memcpy(trig.point, pp,
                               sizeof(trig.point));
                        trig.started_overlapping =
                            (toi <= 0.0f) ? 1 : 0;
                        have_trig = 1;
                    }
                }
            }
        }
    }
    if (have_block && out_hit != NULL) {
        *out_hit = block;
    }
    if (have_trig && out_trigger != NULL) {
        *out_trigger = trig;
    }
    return have_block;
}

int le_physics_sphere_cast(
    le_world *world, const float center[3], float radius,
    const float displacement[3], uint32_t layer_mask,
    int hit_triggers, uint32_t exclude_slot,
    le_shape_hit *out_hit) {
    float dims[3] = { radius, 0.0f, 0.0f };

    return le_physics_shape_cast(
        world, LE_CAST_SPHERE, center, NULL, dims,
        displacement, layer_mask, hit_triggers, exclude_slot,
        out_hit, NULL);
}

int le_physics_capsule_cast(
    le_world *world, const float center[3],
    const float orientation[4], float radius, float half_height,
    const float displacement[3], uint32_t layer_mask,
    int hit_triggers, uint32_t exclude_slot,
    le_shape_hit *out_hit) {
    float dims[3] = { radius, half_height, 0.0f };

    return le_physics_shape_cast(
        world, LE_CAST_CAPSULE, center, orientation, dims,
        displacement, layer_mask, hit_triggers, exclude_slot,
        out_hit, NULL);
}

int le_physics_box_cast(
    le_world *world, const float center[3],
    const float orientation[4], const float half_extents[3],
    const float displacement[3], uint32_t layer_mask,
    int hit_triggers, uint32_t exclude_slot,
    le_shape_hit *out_hit) {
    float dims[3] = { 0.0f, 0.0f, 0.0f };

    if (half_extents != NULL) {
        dims[0] = half_extents[0];
        dims[1] = half_extents[1];
        dims[2] = half_extents[2];
    }
    return le_physics_shape_cast(
        world, LE_CAST_BOX, center, orientation, dims,
        displacement, layer_mask, hit_triggers, exclude_slot,
        out_hit, NULL);
}
