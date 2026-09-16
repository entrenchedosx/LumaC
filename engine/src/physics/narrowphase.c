/*
 * Narrow phase (Phase 28): exact manifolds for sphere/box pairs.
 *
 * Normal convention: A -> B everywhere. Swapped order reverses
 * the normal with identical depth (symmetry-tested). Contact
 * point = deepest representative point (single-point manifolds
 * in Phase 28; stacks stabilize via iteration, documented).
 *
 * Tolerances: 1e-6 parallel-axis epsilon, 1e-9 zero-length
 * guards, deterministic +X fallback normal for degenerate
 * coincidence (same-center spheres). No NaN normals possible —
 * every path normalizes a checked vector or falls back.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

static float le_dot3(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static float le_len3(const float a[3]) {
    return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

/* Sphere-sphere: trivial, with deterministic fallback normal. */
static int le_sphere_sphere(const le_collider_entry *a,
                            const le_collider_entry *b,
                            le_contact_point *out) {
    float d[3];
    float dist;
    float rr;

    /* Probes carry no world_half/world_basis (memset-zero
     * builders); only world_center + world_radius are valid
     * for sphere probes. This path uses exactly those. */
    d[0] = b->world_center[0] - a->world_center[0];
    d[1] = b->world_center[1] - a->world_center[1];
    d[2] = b->world_center[2] - a->world_center[2];
    dist = le_len3(d);
    rr = a->world_radius + b->world_radius;
    /* Touching is NOT a hit (penetration > 0 required):
     * dist < rr hits, dist >= rr misses. */
    if (dist >= rr) {
        return 0; /* separated or touching */
    }
    if (dist > 1e-9f) {
        out->normal[0] = d[0] / dist;
        out->normal[1] = d[1] / dist;
        out->normal[2] = d[2] / dist;
    } else {
        /* Coincident centers: deterministic +X fallback. */
        out->normal[0] = 1.0f;
        out->normal[1] = 0.0f;
        out->normal[2] = 0.0f;
        dist = 0.0f;
    }
    out->penetration = rr - dist;
    /* Point on A's surface toward B. */
    out->point[0] =
        a->world_center[0] + out->normal[0] * a->world_radius;
    out->point[1] =
        a->world_center[1] + out->normal[1] * a->world_radius;
    out->point[2] =
        a->world_center[2] + out->normal[2] * a->world_radius;
    return 1;
}

/* Closest point on an OBB (center C, orthonormal basis columns
 * U/V/W, half extents h) to point P; returns squared distance
 * and writes the closest point. */
static float le_closest_on_obb(const float c[3],
                               const float u[3], const float v[3],
                               const float w[3], const float h[3],
                               const float p[3], float out_q[3]) {
    float d[3];
    float q[3];
    float s;

    d[0] = p[0] - c[0];
    d[1] = p[1] - c[1];
    d[2] = p[2] - c[2];
    q[0] = c[0];
    q[1] = c[1];
    q[2] = c[2];
    s = le_dot3(d, u);
    if (s < -h[0]) {
        s = -h[0];
    } else if (s > h[0]) {
        s = h[0];
    }
    q[0] += u[0] * s;
    q[1] += u[1] * s;
    q[2] += u[2] * s;
    s = le_dot3(d, v);
    if (s < -h[1]) {
        s = -h[1];
    } else if (s > h[1]) {
        s = h[1];
    }
    q[0] += v[0] * s;
    q[1] += v[1] * s;
    q[2] += v[2] * s;
    s = le_dot3(d, w);
    if (s < -h[2]) {
        s = -h[2];
    } else if (s > h[2]) {
        s = h[2];
    }
    q[0] += w[0] * s;
    q[1] += w[1] * s;
    q[2] += w[2] * s;
    out_q[0] = q[0];
    out_q[1] = q[1];
    out_q[2] = q[2];
    {
        float e[3];

        e[0] = p[0] - q[0];
        e[1] = p[1] - q[1];
        e[2] = p[2] - q[2];
        return e[0] * e[0] + e[1] * e[1] + e[2] * e[2];
    }
}

/* Sphere-box (either order; wrapper fixes the convention). */
static int le_sphere_box_raw(const le_collider_entry *s,
                             const le_collider_entry *bx,
                             float n_out[3], float p_out[3],
                             float *dep_out) {
    const float *c = bx->world_center;
    const float *u = bx->world_basis[0];
    const float *v = bx->world_basis[1];
    const float *w = bx->world_basis[2];
    float q[3];
    float dist2;
    float dist;

    dist2 = le_closest_on_obb(c, u, v, w, bx->world_half,
                              s->world_center, q);
    dist = sqrtf(dist2);
    /* Touching (dist == radius) is NOT a hit (solver needs
     * penetration > 0). Exact comparison: dist < r hits,
     * dist >= r misses (no epsilon band — the sweep paths
     * share this convention exactly). */
    if (dist >= s->world_radius) {
        return 0;
    }
    if (dist > 1e-9f) {
        /* Normal s -> bx is (q - center)/dist... convention is
         * A -> B; the caller passes (a=sphere, b=box) or swaps.
         * Here: normal points sphere -> box = (q - s)/dist. */
        n_out[0] = (q[0] - s->world_center[0]) / dist;
        n_out[1] = (q[1] - s->world_center[1]) / dist;
        n_out[2] = (q[2] - s->world_center[2]) / dist;
        p_out[0] = q[0];
        p_out[1] = q[1];
        p_out[2] = q[2];
        *dep_out = s->world_radius - dist;
        return 1;
    }
    /* Sphere center strictly inside the box (dist == 0 with
     * all |coords| < half): push out along the
     * least-penetration face axis. A face-touch (dist == 0 via
     * clamping with the center outside, e.g. endpoint resting
     * exactly on the face) is NOT inside: it is a miss with
     * depth 0 (touching convention). */
    {
        float d[3];
        float best;
        int axis = 0;
        int sign = 1;
        float s0;
        float s1;
        float s2;

        d[0] = s->world_center[0] - c[0];
        d[1] = s->world_center[1] - c[1];
        d[2] = s->world_center[2] - c[2];
        s0 = le_dot3(d, u);
        s1 = le_dot3(d, v);
        s2 = le_dot3(d, w);
        if ((s0 < 0.0f ? -s0 : s0) >= bx->world_half[0] ||
            (s1 < 0.0f ? -s1 : s1) >= bx->world_half[1] ||
            (s2 < 0.0f ? -s2 : s2) >= bx->world_half[2]) {
            return 0; /* face-touch from outside: miss */
        }
        /* Penetration per face = half - |coord|; min wins. */
        best = bx->world_half[0] -
               (s0 < 0.0f ? -s0 : s0);
        axis = 0;
        sign = (s0 < 0.0f) ? -1 : 1;
        {
            float c1 = bx->world_half[1] -
                       (s1 < 0.0f ? -s1 : s1);

            if (c1 < best) {
                best = c1;
                axis = 1;
                sign = (s1 < 0.0f) ? -1 : 1;
            }
        }
        {
            float c2 = bx->world_half[2] -
                       (s2 < 0.0f ? -s2 : s2);

            if (c2 < best) {
                best = c2;
                axis = 2;
                sign = (s2 < 0.0f) ? -1 : 1;
            }
        }
        {
            const float *ax =
                (axis == 0) ? u : (axis == 1) ? v : w;

            /* Sphere->box normal opposes the exit direction...
             * exit dir pushes the sphere OUT of the box; the
             * contact normal (A=sphere -> B=box) points INTO
             * the box = -exit. */
            n_out[0] = -(float)sign * ax[0];
            n_out[1] = -(float)sign * ax[1];
            n_out[2] = -(float)sign * ax[2];
        }
        p_out[0] = s->world_center[0];
        p_out[1] = s->world_center[1];
        p_out[2] = s->world_center[2];
        *dep_out = best + s->world_radius;
        return 1;
    }
}

/* Box-box via SAT (15 axes: 3+3 face + 9 edge-cross). Returns the
 * deepest single-point manifold. Face-face reports the witness
 * corner; edge-edge the closest pair midpoint. */
static int le_box_box(const le_collider_entry *a,
                      const le_collider_entry *b,
                      le_contact_point *out) {
    const float *ca = a->world_center;
    const float *cb = b->world_center;
    float t[3]; /* cb - ca in A's frame */
    float d[3];
    float ra;
    float rb;
    float best;
    float best_axis[3];
    int best_is_edge = 0;
    int i;

    d[0] = cb[0] - ca[0];
    d[1] = cb[1] - ca[1];
    d[2] = cb[2] - ca[2];
    /* Express d in A's frame. */
    for (i = 0; i < 3; i++) {
        t[i] = d[0] * a->world_basis[0][i] +
               d[1] * a->world_basis[1][i] +
               d[2] * a->world_basis[2][i];
    }
    /* Rotation B -> A frame: R[i][j] = Ai . Bj. */
    {
        float r[3][3];
        float absr[3][3];
        int m;
        int n;

        for (m = 0; m < 3; m++) {
            for (n = 0; n < 3; n++) {
                r[m][n] =
                    a->world_basis[0][m] * b->world_basis[0][n] +
                    a->world_basis[1][m] * b->world_basis[1][n] +
                    a->world_basis[2][m] * b->world_basis[2][n];
                {
                    float v = r[m][n];

                    absr[m][n] = (v < 0.0f) ? -v : v;
                }
            }
        }
        best = 1e30f;
        /* A's face axes. */
        for (m = 0; m < 3; m++) {
            ra = a->world_half[m];
            rb = b->world_half[0] * absr[m][0] +
                 b->world_half[1] * absr[m][1] +
                 b->world_half[2] * absr[m][2];
            {
                float tt = (t[m] < 0.0f) ? -t[m] : t[m];
                float overlap = ra + rb - tt;

                if (overlap < 0.0f) {
                    return 0; /* separating axis */
                }
                if (overlap < best) {
                    float s = (t[m] < 0.0f) ? -1.0f : 1.0f;

                    best = overlap;
                    best_axis[0] = s * a->world_basis[0][m];
                    best_axis[1] = s * a->world_basis[1][m];
                    best_axis[2] = s * a->world_basis[2][m];
                    best_is_edge = 0;
                }
            }
        }
        /* B's face axes. */
        for (n = 0; n < 3; n++) {
            ra = a->world_half[0] * absr[0][n] +
                 a->world_half[1] * absr[1][n] +
                 a->world_half[2] * absr[2][n];
            rb = b->world_half[n];
            {
                float tt = t[0] * r[0][n] + t[1] * r[1][n] +
                           t[2] * r[2][n];

                if (tt < 0.0f) {
                    tt = -tt;
                }
                {
                    float overlap = ra + rb - tt;

                    if (overlap < 0.0f) {
                        return 0;
                    }
                    if (overlap < best) {
                        float s = (t[0] * r[0][n] +
                                   t[1] * r[1][n] +
                                   t[2] * r[2][n]) < 0.0f
                                      ? -1.0f
                                      : 1.0f;

                        best = overlap;
                        best_axis[0] = s * b->world_basis[0][n];
                        best_axis[1] = s * b->world_basis[1][n];
                        best_axis[2] = s * b->world_basis[2][n];
                        best_is_edge = 0;
                    }
                }
            }
        }
        /* Edge-cross axes Ai x Bj. */
        for (m = 0; m < 3; m++) {
            int m1 = (m + 1) % 3;
            int m2 = (m + 2) % 3;

            for (n = 0; n < 3; n++) {
                int n1 = (n + 1) % 3;
                int n2 = (n + 2) % 3;
                float overlap;

                ra = a->world_half[m1] * absr[m2][n] +
                     a->world_half[m2] * absr[m1][n];
                rb = b->world_half[n1] * absr[m][n2] +
                     b->world_half[n2] * absr[m][n1];
                {
                    float tt = t[m2] * r[m1][n] - t[m1] * r[m2][n];

                    if (tt < 0.0f) {
                        tt = -tt;
                    }
                    overlap = ra + rb - tt;
                }
                if (overlap < 0.0f) {
                    return 0;
                }
                /* Skip near-parallel (degenerate cross). */
                {
                    float cx = a->world_basis[1][m] *
                                   b->world_basis[2][n] -
                               a->world_basis[2][m] *
                                   b->world_basis[1][n];
                    float cy = a->world_basis[2][m] *
                                   b->world_basis[0][n] -
                               a->world_basis[0][m] *
                                   b->world_basis[2][n];
                    float cz = a->world_basis[0][m] *
                                   b->world_basis[1][n] -
                               a->world_basis[1][m] *
                                   b->world_basis[0][n];
                    float cl =
                        sqrtf(cx * cx + cy * cy + cz * cz);

                    if (cl < 1e-6f) {
                        continue;
                    }
                    if (overlap < best) {
                        /* Orient along center delta. */
                        float s = cx * d[0] + cy * d[1] +
                                          cz * d[2] <
                                  0.0f
                                      ? -1.0f
                                      : 1.0f;

                        best = overlap;
                        best_axis[0] = s * cx / cl;
                        best_axis[1] = s * cy / cl;
                        best_axis[2] = s * cz / cl;
                        best_is_edge = 1;
                    }
                }
            }
        }
        (void)best_is_edge;
    }
    /* Deepest axis found with best > 0 (touching = overlap 0 on
     * every axis but none negative -> contact with ~0 depth;
     * report it so resting contact persists). */
    {
        float nl = le_len3(best_axis);

        if (nl < 1e-9f) {
            best_axis[0] = 1.0f;
            best_axis[1] = 0.0f;
            best_axis[2] = 0.0f;
        } else {
            best_axis[0] /= nl;
            best_axis[1] /= nl;
            best_axis[2] /= nl;
        }
    }
    out->normal[0] = best_axis[0];
    out->normal[1] = best_axis[1];
    out->normal[2] = best_axis[2];
    out->penetration = best;
    /* Witness: support point of A along -normal, averaged with
     * support of B along +normal (stable midpoint). */
    {
        float pa[3];
        float pb[3];
        int k;

        pa[0] = ca[0];
        pa[1] = ca[1];
        pa[2] = ca[2];
        pb[0] = cb[0];
        pb[1] = cb[1];
        pb[2] = cb[2];
        for (k = 0; k < 3; k++) {
            float sa = -(out->normal[0] * a->world_basis[0][k] +
                         out->normal[1] * a->world_basis[1][k] +
                         out->normal[2] * a->world_basis[2][k]);
            float sb = (out->normal[0] * b->world_basis[0][k] +
                        out->normal[1] * b->world_basis[1][k] +
                        out->normal[2] * b->world_basis[2][k]);

            sa = (sa < 0.0f) ? -a->world_half[k] : a->world_half[k];
            sb = (sb < 0.0f) ? -b->world_half[k] : b->world_half[k];
            pa[0] += a->world_basis[0][k] * sa;
            pa[1] += a->world_basis[1][k] * sa;
            pa[2] += a->world_basis[2][k] * sa;
            pb[0] += b->world_basis[0][k] * sb;
            pb[1] += b->world_basis[1][k] * sb;
            pb[2] += b->world_basis[2][k] * sb;
        }
        out->point[0] = 0.5f * (pa[0] + pb[0]);
        out->point[1] = 0.5f * (pa[1] + pb[1]);
        out->point[2] = 0.5f * (pa[2] + pb[2]);
    }
    return 1;
}

/* Capsule helpers (Phase 30): a capsule is the set of points
 * within radius r of segment [p0,p1]. Zero-length segments are
 * spheres (handled naturally by the same code). */

/* Closest point on segment [a,b] to p; writes q and returns
 * the segment parameter t in [0,1]. */
float le_seg_closest_point(const float a[3],
                           const float b[3],
                           const float p[3], float q[3]) {
    float ab[3];
    float t;

    ab[0] = b[0] - a[0];
    ab[1] = b[1] - a[1];
    ab[2] = b[2] - a[2];
    {
        float len2 = ab[0] * ab[0] + ab[1] * ab[1] +
                     ab[2] * ab[2];

        if (len2 < 1e-18f) {
            q[0] = a[0];
            q[1] = a[1];
            q[2] = a[2];
            return 0.0f;
        }
        t = ((p[0] - a[0]) * ab[0] + (p[1] - a[1]) * ab[1] +
             (p[2] - a[2]) * ab[2]) /
            len2;
    }
    if (t < 0.0f) {
        t = 0.0f;
    } else if (t > 1.0f) {
        t = 1.0f;
    }
    q[0] = a[0] + ab[0] * t;
    q[1] = a[1] + ab[1] * t;
    q[2] = a[2] + ab[2] * t;
    return t;
}

/* Closest points between segments [p1,q1] and [p2,q2] (Ericson
 * 5.1.9, robust for parallel/near-parallel/crossing/coincident/
 * zero-length). Writes c1/c2; returns squared distance. No
 * divide-by-zero: every denominator is guarded. Exported for
 * sweep clearance (cast.c). */
float le_seg_seg_closest(const float p1[3],
                         const float q1[3],
                         const float p2[3],
                         const float q2[3], float c1[3],
                         float c2[3]) {
    float d1[3];
    float d2[3];
    float r[3];
    float a;
    float e;
    float f;
    float s;
    float t;

    d1[0] = q1[0] - p1[0];
    d1[1] = q1[1] - p1[1];
    d1[2] = q1[2] - p1[2];
    d2[0] = q2[0] - p2[0];
    d2[1] = q2[1] - p2[1];
    d2[2] = q2[2] - p2[2];
    r[0] = p1[0] - p2[0];
    r[1] = p1[1] - p2[1];
    r[2] = p1[2] - p2[2];
    a = le_dot3(d1, d1);
    e = le_dot3(d2, d2);
    f = le_dot3(d2, r);
    if (a < 1e-18f && e < 1e-18f) {
        /* Both degenerate: point-point. */
        c1[0] = p1[0];
        c1[1] = p1[1];
        c1[2] = p1[2];
        c2[0] = p2[0];
        c2[1] = p2[1];
        c2[2] = p2[2];
        {
            float dd[3];

            dd[0] = c1[0] - c2[0];
            dd[1] = c1[1] - c2[1];
            dd[2] = c1[2] - c2[2];
            return le_dot3(dd, dd);
        }
    }
    if (a < 1e-18f) {
        s = 0.0f;
        t = (e > 1e-18f) ? f / e : 0.0f;
        if (t < 0.0f) {
            t = 0.0f;
        } else if (t > 1.0f) {
            t = 1.0f;
        }
    } else {
        float c = le_dot3(d1, r);

        if (e < 1e-18f) {
            t = 0.0f;
            s = (c < 0.0f) ? 0.0f : ((c > a) ? 1.0f : c / a);
        } else {
            float b = le_dot3(d1, d2);
            float denom = a * e - b * b;

            s = (denom > 1e-18f)
                    ? ((b * f - c * e) / denom)
                    : 0.0f;
            if (s < 0.0f) {
                s = 0.0f;
            } else if (s > 1.0f) {
                s = 1.0f;
            }
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = (c < 0.0f) ? 0.0f
                               : ((c > a) ? 1.0f : c / a);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = ((b - c) < 0.0f)
                        ? 0.0f
                        : (((b - c) > a) ? 1.0f : (b - c) / a);
            }
        }
    }
    c1[0] = p1[0] + d1[0] * s;
    c1[1] = p1[1] + d1[1] * s;
    c1[2] = p1[2] + d1[2] * s;
    c2[0] = p2[0] + d2[0] * t;
    c2[1] = p2[1] + d2[1] * t;
    c2[2] = p2[2] + d2[2] * t;
    {
        float dd[3];

        dd[0] = c1[0] - c2[0];
        dd[1] = c1[1] - c2[1];
        dd[2] = c1[2] - c2[2];
        return le_dot3(dd, dd);
    }
}

/* Capsule-sphere (either order via the wrapper): closest point
 * on the capsule segment to the sphere center. Normal convention
 * A -> B handled by the caller. */
static int le_capsule_sphere_raw(const le_collider_entry *cap,
                                 const le_collider_entry *sph,
                                 float n_out[3], float p_out[3],
                                 float *dep_out) {
    float q[3];
    float d[3];
    float dist;
    float rr = cap->world_cap_radius + sph->world_radius;

    (void)le_seg_closest_point(cap->world_p0, cap->world_p1,
                               sph->world_center, q);
    d[0] = sph->world_center[0] - q[0];
    d[1] = sph->world_center[1] - q[1];
    d[2] = sph->world_center[2] - q[2];
    dist = le_len3(d);
    /* Touching is NOT a hit (penetration > 0 required). */
    if (dist >= rr) {
        return 0;
    }
    if (dist > 1e-9f) {
        /* Normal cap -> sphere = (center - q)/dist. */
        n_out[0] = d[0] / dist;
        n_out[1] = d[1] / dist;
        n_out[2] = d[2] / dist;
        /* Contact point on the capsule surface toward sphere. */
        p_out[0] = q[0] + n_out[0] * cap->world_cap_radius;
        p_out[1] = q[1] + n_out[1] * cap->world_cap_radius;
        p_out[2] = q[2] + n_out[2] * cap->world_cap_radius;
        *dep_out = rr - dist;
        return 1;
    }
    /* Sphere center on (or at the endpoint of) the segment:
     * push along the capsule axis (or +X fallback for a
     * zero-length capsule = coincident spheres). */
    {
        float ax[3];

        ax[0] = cap->world_p1[0] - cap->world_p0[0];
        ax[1] = cap->world_p1[1] - cap->world_p0[1];
        ax[2] = cap->world_p1[2] - cap->world_p0[2];
        {
            float al = le_len3(ax);

            if (al > 1e-9f) {
                n_out[0] = ax[0] / al;
                n_out[1] = ax[1] / al;
                n_out[2] = ax[2] / al;
            } else {
                n_out[0] = 1.0f;
                n_out[1] = 0.0f;
                n_out[2] = 0.0f;
            }
        }
        p_out[0] = q[0] + n_out[0] * cap->world_cap_radius;
        p_out[1] = q[1] + n_out[1] * cap->world_cap_radius;
        p_out[2] = q[2] + n_out[2] * cap->world_cap_radius;
        *dep_out = rr - dist;
        return 1;
    }
}

/* Capsule-capsule via segment-segment closest points. */
static int le_capsule_capsule(const le_collider_entry *a,
                              const le_collider_entry *b,
                              le_contact_point *out) {
    float c1[3];
    float c2[3];
    float dist2;
    float dist;
    float rr = a->world_cap_radius + b->world_cap_radius;

    dist2 = le_seg_seg_closest(a->world_p0, a->world_p1,
                               b->world_p0, b->world_p1, c1, c2);
    dist = sqrtf(dist2);
    /* Touching is NOT a hit (penetration > 0 required). */
    if (dist >= rr) {
        return 0;
    }
    if (dist > 1e-9f) {
        out->normal[0] = (c2[0] - c1[0]) / dist;
        out->normal[1] = (c2[1] - c1[1]) / dist;
        out->normal[2] = (c2[2] - c1[2]) / dist;
    } else {
        /* Coincident/parallel-touching segments: use the
         * center delta, else deterministic +X. */
        float d[3];

        d[0] = b->world_center[0] - a->world_center[0];
        d[1] = b->world_center[1] - a->world_center[1];
        d[2] = b->world_center[2] - a->world_center[2];
        {
            float dl = le_len3(d);

            if (dl > 1e-9f) {
                out->normal[0] = d[0] / dl;
                out->normal[1] = d[1] / dl;
                out->normal[2] = d[2] / dl;
            } else {
                out->normal[0] = 1.0f;
                out->normal[1] = 0.0f;
                out->normal[2] = 0.0f;
            }
        }
        dist = 0.0f;
    }
    out->penetration = rr - dist;
    out->point[0] = c1[0] + out->normal[0] * a->world_cap_radius;
    out->point[1] = c1[1] + out->normal[1] * a->world_cap_radius;
    out->point[2] = c1[2] + out->normal[2] * a->world_cap_radius;
    return 1;
}

/* Closest distance between segment [p0,p1] and an OBB; writes
 * the segment point (sq) and box point (bq). Correct for side
 * contacts (not just endpoint spheres): tests the segment
 * against all Voronoi regions via ternary-free bisection-free
 * sampling? NO — exact approach: minimize |S(t) - Q(u,v,w)|
 * by checking (a) endpoint-vs-box distances and (b) segment vs
 * each box face plane (segment-face intersection when the
 * crossing point lies inside the face rect). The minimum over
 * all candidates is the exact segment-box distance for convex
 * boxes. Exported for sweep clearance (cast.c). */
float le_seg_obb_closest(const float p0[3],
                         const float p1[3],
                         const float bc[3],
                         const float bu[3],
                         const float bv[3],
                         const float bw[3],
                         const float hh[3], float sq[3],
                         float bq[3]) {
    float best_d2 = -1.0f;
    float cand_sq[3];
    float cand_bq[3];
    int has = 0;
    /* Helper: consider candidate pair. */
#define LE_SEG_OBB_CAND(sx, sy, sz, bx, by, bz)          \
    do {                                                 \
        float dx = (sx) - (bx);                          \
        float dy = (sy) - (by);                          \
        float dz = (sz) - (bz);                          \
        float d2 = dx * dx + dy * dy + dz * dz;          \
        if (!has || d2 < best_d2) {                      \
            has = 1;                                     \
            best_d2 = d2;                                \
            cand_sq[0] = (sx);                           \
            cand_sq[1] = (sy);                           \
            cand_sq[2] = (sz);                           \
            cand_bq[0] = (bx);                           \
            cand_bq[1] = (by);                           \
            cand_bq[2] = (bz);                           \
        }                                                \
    } while (0)
    /* (a) Endpoint-vs-box closest points. */
    {
        float q0[3];
        float q1[3];

        le_closest_on_obb(bc, bu, bv, bw, hh, p0, q0);
        le_closest_on_obb(bc, bu, bv, bw, hh, p1, q1);
        LE_SEG_OBB_CAND(p0[0], p0[1], p0[2], q0[0], q0[1],
                        q0[2]);
        LE_SEG_OBB_CAND(p1[0], p1[1], p1[2], q1[0], q1[1],
                        q1[2]);
    }
    /* (b) Segment vs each of the 6 face planes: intersect the
     * segment with the plane; if the hit lies inside the face
     * rect (with the segment parameter in [0,1]), it is a
     * candidate (distance 0 in the overlapping case is found
     * here when the segment pierces a face). */
    {
        const float *axes[3] = { bu, bv, bw };
        int f;

        for (f = 0; f < 3; f++) {
            int u = (f + 1) % 3;
            int v = (f + 2) % 3;
            float h0 = hh[f];
            float h1 = hh[u];
            float h2 = hh[v];
            int side;

            for (side = -1; side <= 1; side += 2) {
                float denom;
                float numer;
                float t;
                float hp[3];
                float lu;
                float lv;

                denom = (p1[0] - p0[0]) * axes[f][0] +
                        (p1[1] - p0[1]) * axes[f][1] +
                        (p1[2] - p0[2]) * axes[f][2];
                numer = ((bc[0] + axes[f][0] * h0 *
                                       (float)side -
                          p0[0]) * axes[f][0] +
                         (bc[1] + axes[f][1] * h0 *
                                       (float)side -
                          p0[1]) * axes[f][1] +
                         (bc[2] + axes[f][2] * h0 *
                                       (float)side -
                          p0[2]) * axes[f][2]);
                if (denom > -1e-12f && denom < 1e-12f) {
                    continue; /* parallel to the face */
                }
                t = numer / denom;
                if (t < 0.0f || t > 1.0f) {
                    continue;
                }
                hp[0] = p0[0] + (p1[0] - p0[0]) * t;
                hp[1] = p0[1] + (p1[1] - p0[1]) * t;
                hp[2] = p0[2] + (p1[2] - p0[2]) * t;
                {
                    float rel[3];

                    rel[0] = hp[0] - bc[0];
                    rel[1] = hp[1] - bc[1];
                    rel[2] = hp[2] - bc[2];
                    lu = rel[0] * axes[u][0] +
                         rel[1] * axes[u][1] +
                         rel[2] * axes[u][2];
                    lv = rel[0] * axes[v][0] +
                         rel[1] * axes[v][1] +
                         rel[2] * axes[v][2];
                }
                if (lu < -h1 || lu > h1 || lv < -h2 ||
                    lv > h2) {
                    continue;
                }
                LE_SEG_OBB_CAND(hp[0], hp[1], hp[2], hp[0],
                                hp[1], hp[2]);
            }
        }
    }
    /* (c) Box edges vs segment: covered by (a)+(b) for convex
     * boxes EXCEPT segment-grazing-edge cases where the closest
     * box point is edge-interior and neither endpoint projects
     * there. Handle by segment-segment closest for the 12 box
     * edges vs the capsule segment. */
    {
        float corn[8][3];
        static const int kE[12][2] = {
            { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
            { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
            { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
        };
        int e;
        int k;

        for (k = 0; k < 8; k++) {
            float su = (k & 1) ? 1.0f : -1.0f;
            float sv = (k & 2) ? 1.0f : -1.0f;
            float sw = (k & 4) ? 1.0f : -1.0f;

            corn[k][0] =
                bc[0] + bu[0] * su * hh[0] + bv[0] * sv * hh[1] +
                bw[0] * sw * hh[2];
            corn[k][1] =
                bc[1] + bu[1] * su * hh[0] + bv[1] * sv * hh[1] +
                bw[1] * sw * hh[2];
            corn[k][2] =
                bc[2] + bu[2] * su * hh[0] + bv[2] * sv * hh[1] +
                bw[2] * sw * hh[2];
        }
        for (e = 0; e < 12; e++) {
            float s1[3];
            float s2[3];
            float dd[3];
            float d2;

            d2 = le_seg_seg_closest(
                p0, p1, corn[kE[e][0]], corn[kE[e][1]], s1, s2);
            dd[0] = s1[0] - s2[0];
            dd[1] = s1[1] - s2[1];
            dd[2] = s1[2] - s2[2];
            (void)dd;
            if (!has || d2 < best_d2) {
                has = 1;
                best_d2 = d2;
                cand_sq[0] = s1[0];
                cand_sq[1] = s1[1];
                cand_sq[2] = s1[2];
                cand_bq[0] = s2[0];
                cand_bq[1] = s2[1];
                cand_bq[2] = s2[2];
            }
        }
    }
#undef LE_SEG_OBB_CAND
    if (!has) {
        sq[0] = p0[0];
        sq[1] = p0[1];
        sq[2] = p0[2];
        le_closest_on_obb(bc, bu, bv, bw, hh, p0, bq);
        {
            float dx = sq[0] - bq[0];
            float dy = sq[1] - bq[1];
            float dz = sq[2] - bq[2];

            return dx * dx + dy * dy + dz * dz;
        }
    }
    sq[0] = cand_sq[0];
    sq[1] = cand_sq[1];
    sq[2] = cand_sq[2];
    bq[0] = cand_bq[0];
    bq[1] = cand_bq[1];
    bq[2] = cand_bq[2];
    return best_d2;
}

/* Capsule-box (either order via the wrapper): exact segment vs
 * OBB closest distance. Normal convention A -> B. */
static int le_capsule_box_raw(const le_collider_entry *cap,
                              const le_collider_entry *bx,
                              float n_out[3], float p_out[3],
                              float *dep_out) {
    const float *bc = bx->world_center;
    const float *bu = bx->world_basis[0];
    const float *bv = bx->world_basis[1];
    const float *bw = bx->world_basis[2];
    float sq[3];
    float bq[3];
    float dist2;
    float dist;
    float r = cap->world_cap_radius;

    /* Endpoint spheres first: either endpoint within r of the
     * box is a genuine hit (exact sphere path, no segment
     * machinery). This runs BEFORE the segment distance so a
     * grazing segment (dist == 0 via clamping, both centers
     * outside) can never claim a hit the spheres deny. */
    {
        float n0[3];
        float p0[3];
        float d0 = 0.0f;
        float n1[3];
        float p1[3];
        float d1 = 0.0f;
        le_collider_entry probe;
        int h0;
        int h1;

        memset(&probe, 0, sizeof(probe));
        probe.shape = LE_COLLIDER_SPHERE;
        probe.world_radius = r;
        memcpy(probe.world_center, cap->world_p0,
               sizeof(probe.world_center));
        h0 = le_sphere_box_raw(&probe, bx, n0, p0, &d0);
        memcpy(probe.world_center, cap->world_p1,
               sizeof(probe.world_center));
        h1 = le_sphere_box_raw(&probe, bx, n1, p1, &d1);
        if (h0 && (!h1 || d0 >= d1)) {
            memcpy(n_out, n0, sizeof(n0));
            memcpy(p_out, p0, sizeof(p0));
            *dep_out = d0;
            return 1;
        }
        if (h1) {
            memcpy(n_out, n1, sizeof(n1));
            memcpy(p_out, p1, sizeof(p1));
            *dep_out = d1;
            return 1;
        }
    }
    dist2 = le_seg_obb_closest(cap->world_p0, cap->world_p1, bc,
                               bu, bv, bw, bx->world_half, sq,
                               bq);
    dist = sqrtf(dist2);
    /* Touching is NOT a hit (penetration > 0 required). Both
     * endpoint spheres missed, so dist == 0 here means the
     * segment grazes the box (face/edge touch from outside):
     * a MISS. A true side contact (segment outside, surface
     * within r) has dist in (0, r) and hits below. */
    if (dist >= r) {
        return 0;
    }
    if (dist > 1e-9f) {
        /* Segment strictly outside the box (dist > 0): the
         * capsule surface is (r - dist) away — a hit iff
         * dist < r (already established). */
        /* Normal cap -> box = (bq - sq)/dist. */
        n_out[0] = (bq[0] - sq[0]) / dist;
        n_out[1] = (bq[1] - sq[1]) / dist;
        n_out[2] = (bq[2] - sq[2]) / dist;
        p_out[0] = sq[0] + n_out[0] * r;
        p_out[1] = sq[1] + n_out[1] * r;
        p_out[2] = sq[2] + n_out[2] * r;
        *dep_out = r - dist;
        return 1;
    }
    /* dist == 0 with both endpoint spheres missing: the segment
     * grazes the box from outside (face/edge touch) — a MISS.
     * (A segment with an endpoint strictly inside always hits
     * via the sphere fast path above, so this branch is
     * graze-only.) */
    return 0;
}

int le_physics_narrow_pair(le_world *world,
                           const le_collider_entry *a,
                           const le_collider_entry *b,
                           le_contact_point *out) {
    int sa;
    int sb;

    (void)world;
    if (a == NULL || b == NULL || out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    out->slot_a = 0;
    out->slot_b = 0;
    {
        int ca =
            (a->shape == LE_COLLIDER_CAPSULE) ? 2 : 0;
        int cb =
            (b->shape == LE_COLLIDER_CAPSULE) ? 2 : 0;

        sa = (a->shape == LE_COLLIDER_SPHERE) ? 1 : ca;
        sb = (b->shape == LE_COLLIDER_SPHERE) ? 1 : cb;
    }
    if (sa == 2 && sb == 2) {
        return le_capsule_capsule(a, b, out);
    }
    if (sa == 2 && sb == 1) {
        float n[3];
        float p[3];
        float dep = 0.0f;

        if (!le_capsule_sphere_raw(a, b, n, p, &dep)) {
            return 0;
        }
        memcpy(out->normal, n, sizeof(n));
        memcpy(out->point, p, sizeof(p));
        out->penetration = dep;
        return 1;
    }
    if (sa == 1 && sb == 2) {
        float n[3];
        float p[3];
        float dep = 0.0f;

        if (!le_capsule_sphere_raw(b, a, n, p, &dep)) {
            return 0;
        }
        out->normal[0] = -n[0];
        out->normal[1] = -n[1];
        out->normal[2] = -n[2];
        memcpy(out->point, p, sizeof(p));
        out->penetration = dep;
        return 1;
    }
    if (sa == 2 && sb == 0) {
        float n[3];
        float p[3];
        float dep = 0.0f;

        if (!le_capsule_box_raw(a, b, n, p, &dep)) {
            return 0;
        }
        memcpy(out->normal, n, sizeof(n));
        memcpy(out->point, p, sizeof(p));
        out->penetration = dep;
        return 1;
    }
    if (sa == 0 && sb == 2) {
        float n[3];
        float p[3];
        float dep = 0.0f;

        if (!le_capsule_box_raw(b, a, n, p, &dep)) {
            return 0;
        }
        out->normal[0] = -n[0];
        out->normal[1] = -n[1];
        out->normal[2] = -n[2];
        memcpy(out->point, p, sizeof(p));
        out->penetration = dep;
        return 1;
    }
    if (sa && sb) {
        return le_sphere_sphere(a, b, out);
    }
    if (sa && !sb) {
        float n[3];
        float p[3];
        float dep = 0.0f;

        if (!le_sphere_box_raw(a, b, n, p, &dep)) {
            return 0;
        }
        memcpy(out->normal, n, sizeof(n));
        memcpy(out->point, p, sizeof(p));
        out->penetration = dep;
        return 1;
    }
    if (!sa && sb) {
        float n[3];
        float p[3];
        float dep = 0.0f;

        /* Raw computes sphere->box; flip for A=box,B=sphere. */
        if (!le_sphere_box_raw(b, a, n, p, &dep)) {
            return 0;
        }
        out->normal[0] = -n[0];
        out->normal[1] = -n[1];
        out->normal[2] = -n[2];
        /* Contact point stays the box-face witness (valid for
         * either order). */
        memcpy(out->point, p, sizeof(p));
        out->penetration = dep;
        return 1;
    }
    return le_box_box(a, b, out);
}
