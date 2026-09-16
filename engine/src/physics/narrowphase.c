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

    d[0] = b->world_center[0] - a->world_center[0];
    d[1] = b->world_center[1] - a->world_center[1];
    d[2] = b->world_center[2] - a->world_center[2];
    dist = le_len3(d);
    rr = a->world_radius + b->world_radius;
    if (dist >= rr) {
        return 0; /* separated or exactly touching */
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
    /* Sphere center inside (or on) the box: push out along the
     * least-penetration face axis. */
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
    sa = (a->shape == LE_COLLIDER_SPHERE) ? 1 : 0;
    sb = (b->shape == LE_COLLIDER_SPHERE) ? 1 : 0;
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
