/*
 * Physics queries (Phase 28): raycast (closest + all, near->far),
 * sphere/box overlap, debug line extraction, stats.
 *
 * Read-only against step-fresh collider poses (AABBs + narrow
 * phase); never mutates solver arrays. Ray-vs-AABB uses the slab
 * method; ray-vs-shape reuses narrow-phase witnesses (sphere
 * analytic, box slab in the shape frame). Debug geometry emits
 * collider wireframes (box 12 edges, sphere 3 rings), contact
 * segments, and optional AABBs as xyz line soup.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

/* Slab ray vs AABB (entry distance or -1). */
static float le_ray_aabb(float ox, float oy, float oz, float dx,
                         float dy, float dz,
                         const float mn[3], const float mx[3],
                         float max_t) {
    float tmin = 0.0f;
    float tmax = max_t;
    float o;
    float d;
    float mnn;
    float mxx;
    float t1;
    float t2;
    int i;

    for (i = 0; i < 3; i++) {
        o = (i == 0) ? ox : (i == 1) ? oy : oz;
        d = (i == 0) ? dx : (i == 1) ? dy : dz;
        mnn = (i == 0) ? mn[0] : (i == 1) ? mn[1] : mn[2];
        mxx = (i == 0) ? mx[0] : (i == 1) ? mx[1] : mx[2];
        if (d > -1e-9f && d < 1e-9f) {
            if (o < mnn || o > mxx) {
                return -1.0f;
            }
            continue;
        }
        t1 = (mnn - o) / d;
        t2 = (mxx - o) / d;
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
            return -1.0f;
        }
    }
    return tmin;
}

int le_physics_ray_narrow(le_world *world,
                          const le_collider_entry *c, float ox,
                          float oy, float oz, float dx, float dy,
                          float dz, float max_t, float *out_t,
                          float out_n[3]) {
    (void)world;
    if (c == NULL || !c->aabb_valid) {
        return 0;
    }
    if (c->shape == LE_COLLIDER_CAPSULE) {
        /* Ray vs capsule: ray vs the segment's cylinder clipped
         * to the segment range, plus ray vs the two cap spheres.
         * Earliest t in [0, max_t] wins. Direction is unit
         * (callers normalize). */
        float best_t = max_t + 1.0f;
        int found = 0;
        float rr = c->world_cap_radius;

        {
            float abx = c->world_p1[0] - c->world_p0[0];
            float aby = c->world_p1[1] - c->world_p0[1];
            float abz = c->world_p1[2] - c->world_p0[2];
            float len2 = abx * abx + aby * aby + abz * abz;

            if (len2 > 1e-18f) {
                float ul = sqrtf(len2);
                float ux = abx / ul;
                float uy = aby / ul;
                float uz = abz / ul;
                float ocx = ox - c->world_center[0];
                float ocy = oy - c->world_center[1];
                float ocz = oz - c->world_center[2];
                float d_u = dx * ux + dy * uy + dz * uz;
                float o_u = ocx * ux + ocy * uy + ocz * uz;
                float ex = dx - ux * d_u;
                float ey = dy - uy * d_u;
                float ez = dz - uz * d_u;
                float fx = ocx - ux * o_u;
                float fy = ocy - uy * o_u;
                float fz = ocz - uz * o_u;
                float A = ex * ex + ey * ey + ez * ez;
                float B = ex * fx + ey * fy + ez * fz;
                float C =
                    fx * fx + fy * fy + fz * fz - rr * rr;

                if (A > 1e-18f) {
                    float disc = B * B - A * C;

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

                            if (t < 0.0f || t > max_t) {
                                continue;
                            }
                            hx = ox + dx * t -
                                 c->world_center[0];
                            hy = oy + dy * t -
                                 c->world_center[1];
                            hz = oz + dz * t -
                                 c->world_center[2];
                            s = hx * ux + hy * uy + hz * uz;
                            if (s < -c->world_cap_half ||
                                s > c->world_cap_half) {
                                continue;
                            }
                            if (!found || t < best_t) {
                                found = 1;
                                best_t = t;
                            }
                        }
                    }
                }
            }
        }
        {
            const float *caps[2] = { c->world_p0,
                                     c->world_p1 };
            int k;

            for (k = 0; k < 2; k++) {
                float ocx = ox - caps[k][0];
                float ocy = oy - caps[k][1];
                float ocz = oz - caps[k][2];
                float b = ocx * dx + ocy * dy + ocz * dz;
                float cc = ocx * ocx + ocy * ocy + ocz * ocz -
                           rr * rr;
                float disc = b * b - cc;
                float t;

                if (disc < 0.0f) {
                    continue;
                }
                disc = sqrtf(disc);
                t = -b - disc;
                if (t < 0.0f) {
                    t = -b + disc;
                }
                if (t < 0.0f || t > max_t) {
                    continue;
                }
                if (!found || t < best_t) {
                    found = 1;
                    best_t = t;
                }
            }
        }
        if (!found) {
            return 0;
        }
        if (out_t != NULL) {
            *out_t = best_t;
        }
        if (out_n != NULL) {
            /* Normal = (hit - closest segment point); axis-hit
             * fallback is -direction. */
            float hx = ox + dx * best_t;
            float hy = oy + dy * best_t;
            float hz = oz + dz * best_t;
            float abx = c->world_p1[0] - c->world_p0[0];
            float aby = c->world_p1[1] - c->world_p0[1];
            float abz = c->world_p1[2] - c->world_p0[2];
            float len2 =
                abx * abx + aby * aby + abz * abz;
            float tseg = 0.0f;
            float qx;
            float qy;
            float qz;
            float nx;
            float ny;
            float nz;
            float nl;

            if (len2 >= 1e-18f) {
                tseg = ((hx - c->world_p0[0]) * abx +
                        (hy - c->world_p0[1]) * aby +
                        (hz - c->world_p0[2]) * abz) /
                       len2;
                if (tseg < 0.0f) {
                    tseg = 0.0f;
                } else if (tseg > 1.0f) {
                    tseg = 1.0f;
                }
            }
            qx = c->world_p0[0] + abx * tseg;
            qy = c->world_p0[1] + aby * tseg;
            qz = c->world_p0[2] + abz * tseg;
            nx = hx - qx;
            ny = hy - qy;
            nz = hz - qz;
            nl = sqrtf(nx * nx + ny * ny + nz * nz);
            if (nl < 1e-9f) {
                out_n[0] = -dx;
                out_n[1] = -dy;
                out_n[2] = -dz;
            } else {
                out_n[0] = nx / nl;
                out_n[1] = ny / nl;
                out_n[2] = nz / nl;
            }
        }
        return 1;
    }
    if (c->shape == LE_COLLIDER_SPHERE) {
        /* Analytic ray-sphere. */
        float ocx = ox - c->world_center[0];
        float ocy = oy - c->world_center[1];
        float ocz = oz - c->world_center[2];
        float b = ocx * dx + ocy * dy + ocz * dz;
        float c2 = ocx * ocx + ocy * ocy + ocz * ocz -
                   c->world_radius * c->world_radius;
        float disc = b * b - c2;
        float t;

        if (disc < 0.0f) {
            return 0;
        }
        disc = sqrtf(disc);
        t = -b - disc;
        if (t < 0.0f) {
            t = -b + disc; /* origin inside: exit point */
        }
        if (t < 0.0f || t > max_t) {
            return 0;
        }
        if (out_t != NULL) {
            *out_t = t;
        }
        if (out_n != NULL) {
            float nx = ocx + dx * t;
            float ny = ocy + dy * t;
            float nz = ocz + dz * t;
            float l = sqrtf(nx * nx + ny * ny + nz * nz);

            if (l < 1e-9f) {
                out_n[0] = -dx;
                out_n[1] = -dy;
                out_n[2] = -dz;
            } else {
                out_n[0] = nx / l;
                out_n[1] = ny / l;
                out_n[2] = nz / l;
            }
        }
        return 1;
    }
    /* Ray vs OBB: transform to the shape frame, slab-test the
     * local AABB, map the hit normal back. */
    {
        float lx = ox - c->world_center[0];
        float ly = oy - c->world_center[1];
        float lz = oz - c->world_center[2];
        float lo[3];
        float ld[3];
        float mn[3];
        float mx[3];
        int i;

        for (i = 0; i < 3; i++) {
            lo[i] = lx * c->world_basis[0][i] +
                    ly * c->world_basis[1][i] +
                    lz * c->world_basis[2][i];
            ld[i] = dx * c->world_basis[0][i] +
                    dy * c->world_basis[1][i] +
                    dz * c->world_basis[2][i];
            mn[i] = -c->world_half[i];
            mx[i] = c->world_half[i];
        }
        {
            float t = le_ray_aabb(lo[0], lo[1], lo[2], ld[0],
                                  ld[1], ld[2], mn, mx, max_t);

            if (t < 0.0f) {
                return 0;
            }
            if (out_t != NULL) {
                *out_t = t;
            }
            if (out_n != NULL) {
                /* Hit face = axis of max local penetration. */
                float hx = lo[0] + ld[0] * t;
                float hy = lo[1] + ld[1] * t;
                float hz = lo[2] + ld[2] * t;
                float ex = c->world_half[0] -
                           (hx < 0.0f ? -hx : hx);
                float ey = c->world_half[1] -
                           (hy < 0.0f ? -hy : hy);
                float ez = c->world_half[2] -
                           (hz < 0.0f ? -hz : hz);
                float ln[3] = { 0.0f, 0.0f, 0.0f };

                if (ex <= ey && ex <= ez) {
                    ln[0] = (hx < 0.0f) ? -1.0f : 1.0f;
                } else if (ey <= ez) {
                    ln[1] = (hy < 0.0f) ? -1.0f : 1.0f;
                } else {
                    ln[2] = (hz < 0.0f) ? -1.0f : 1.0f;
                }
                /* Back to world: n = basis * ln. */
                out_n[0] = c->world_basis[0][0] * ln[0] +
                           c->world_basis[0][1] * ln[1] +
                           c->world_basis[0][2] * ln[2];
                out_n[1] = c->world_basis[1][0] * ln[0] +
                           c->world_basis[1][1] * ln[1] +
                           c->world_basis[1][2] * ln[2];
                out_n[2] = c->world_basis[2][0] * ln[0] +
                           c->world_basis[2][1] * ln[1] +
                           c->world_basis[2][2] * ln[2];
            }
            return 1;
        }
    }
}

static int le_query_visible(le_world *world,
                            const le_collider_entry *c,
                            uint32_t layer_mask, int hit_triggers) {
    uint32_t bit;

    (void)world;
    if (c->layer > 31u) {
        return 0;
    }
    bit = 1u << c->layer;
    if ((layer_mask & bit) == 0u) {
        return 0;
    }
    if (c->is_trigger && !hit_triggers) {
        return 0;
    }
    if (!c->aabb_valid) {
        return 0;
    }
    return 1;
}

/* Refresh poses on demand so queries work even before the first
 * fixed step (authoring-time raycasts in tests/editors). */
static void le_query_refresh(le_world *world) {
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

int le_physics_raycast(le_world *world, float ox, float oy,
                       float oz, float dx, float dy, float dz,
                       float max_distance, uint32_t layer_mask,
                       int hit_triggers, le_ray_hit *out_hit) {
    float len;
    uint32_t i;
    float best_t = 0.0f;
    int found = 0;
    le_ray_hit best;

    if (out_hit != NULL) {
        memset(out_hit, 0, sizeof(*out_hit));
        out_hit->object = LE_OBJECT_INVALID;
    }
    if (world == NULL || world->physics == NULL) {
        return 0;
    }
    len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!isfinite(ox) || !isfinite(oy) || !isfinite(oz) ||
        !isfinite(dx) || !isfinite(dy) || !isfinite(dz) ||
        !isfinite(max_distance) || max_distance <= 0.0f ||
        len < 1e-9f || !isfinite(len)) {
        return 0;
    }
    dx /= len;
    dy /= len;
    dz /= len;
    memset(&best, 0, sizeof(best));
    best.object = LE_OBJECT_INVALID;
    le_query_refresh(world);
    {
        struct le_physics_world *pw = world->physics;

        for (i = 0; i < pw->collider_count; i++) {
            le_collider_entry *c = &pw->colliders[i];
            float t;
            float n[3];

            if (!le_query_visible(world, c, layer_mask,
                                  hit_triggers)) {
                continue;
            }
            if (le_ray_aabb(ox, oy, oz, dx, dy, dz,
                            c->aabb_min, c->aabb_max,
                            max_distance) < 0.0f) {
                continue;
            }
            if (!le_physics_ray_narrow(world, c, ox, oy, oz, dx,
                                       dy, dz, max_distance, &t,
                                       n)) {
                continue;
            }
            if (!found || t < best_t) {
                found = 1;
                best_t = t;
                best.object.index = c->slot;
                best.object.generation =
                    world->slots[c->slot].generation;
                best.object.world_tag = world->tag;
                best.point[0] = ox + dx * t;
                best.point[1] = oy + dy * t;
                best.point[2] = oz + dz * t;
                best.normal[0] = n[0];
                best.normal[1] = n[1];
                best.normal[2] = n[2];
                best.distance = t;
            }
        }
    }
    if (found && out_hit != NULL) {
        *out_hit = best;
    }
    world->physics->stat_ray_queries++;
    return found;
}

uint32_t le_physics_raycast_all(
    le_world *world, float ox, float oy, float oz, float dx,
    float dy, float dz, float max_distance, uint32_t layer_mask,
    int hit_triggers, le_ray_hit *out, uint32_t cap) {
    float len;
    uint32_t i;
    uint32_t total = 0;

    if (world == NULL || world->physics == NULL) {
        return 0;
    }
    len = sqrtf(dx * dx + dy * dy + dz * dz);
    if (!isfinite(ox) || !isfinite(oy) || !isfinite(oz) ||
        !isfinite(dx) || !isfinite(dy) || !isfinite(dz) ||
        !isfinite(max_distance) || max_distance <= 0.0f ||
        len < 1e-9f || !isfinite(len)) {
        return 0;
    }
    dx /= len;
    dy /= len;
    dz /= len;
    le_query_refresh(world);
    {
        struct le_physics_world *pw = world->physics;

        for (i = 0; i < pw->collider_count; i++) {
            le_collider_entry *c = &pw->colliders[i];
            float t;
            float n[3];

            if (!le_query_visible(world, c, layer_mask,
                                  hit_triggers)) {
                continue;
            }
            if (le_ray_aabb(ox, oy, oz, dx, dy, dz,
                            c->aabb_min, c->aabb_max,
                            max_distance) < 0.0f) {
                continue;
            }
            if (!le_physics_ray_narrow(world, c, ox, oy, oz, dx,
                                       dy, dz, max_distance, &t,
                                       n)) {
                continue;
            }
            /* Insertion-sort into out by distance (near->far).
             * O(hits^2) worst — hit counts are small; total is
             * exact regardless of cap. */
            if (out != NULL && total < cap) {
                uint32_t k = total;

                out[k].object.index = c->slot;
                out[k].object.generation =
                    world->slots[c->slot].generation;
                out[k].object.world_tag = world->tag;
                out[k].point[0] = ox + dx * t;
                out[k].point[1] = oy + dy * t;
                out[k].point[2] = oz + dz * t;
                out[k].normal[0] = n[0];
                out[k].normal[1] = n[1];
                out[k].normal[2] = n[2];
                out[k].distance = t;
                while (k > 0 &&
                       out[k].distance < out[k - 1u].distance) {
                    le_ray_hit tmp = out[k];

                    out[k] = out[k - 1u];
                    out[k - 1u] = tmp;
                    k--;
                }
            }
            total++;
        }
    }
    world->physics->stat_ray_queries++;
    return total;
}

uint32_t le_physics_overlap_sphere(le_world *world, float cx,
                                   float cy, float cz,
                                   float radius,
                                   uint32_t layer_mask,
                                   int hit_triggers,
                                   le_object *out, uint32_t cap) {
    uint32_t i;
    uint32_t total = 0;
    le_collider_entry probe;

    if (world == NULL || world->physics == NULL) {
        return 0;
    }
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(cz) ||
        !isfinite(radius) || radius <= 0.0f ||
        radius > 1e6f) {
        return 0;
    }
    memset(&probe, 0, sizeof(probe));
    probe.shape = LE_COLLIDER_SPHERE;
    probe.world_radius = radius;
    probe.world_center[0] = cx;
    probe.world_center[1] = cy;
    probe.world_center[2] = cz;
    le_query_refresh(world);
    {
        struct le_physics_world *pw = world->physics;

        for (i = 0; i < pw->collider_count; i++) {
            le_collider_entry *c = &pw->colliders[i];
            le_contact_point cp;

            if (!le_query_visible(world, c, layer_mask,
                                  hit_triggers)) {
                continue;
            }
            /* AABB reject first (broad-phase discipline). */
            if (cx + radius < c->aabb_min[0] ||
                cx - radius > c->aabb_max[0] ||
                cy + radius < c->aabb_min[1] ||
                cy - radius > c->aabb_max[1] ||
                cz + radius < c->aabb_min[2] ||
                cz - radius > c->aabb_max[2]) {
                continue;
            }
            pw->stat_narrow_tests++;
            memset(&cp, 0, sizeof(cp));
            {
                int hit = le_physics_narrow_pair(world, &probe,
                                                 c, &cp);
                if (hit == 0) {
                    /* Touching (dist == rr) is overlap for a
                     * query even though the solver wants
                     * penetration > 0. Exact-touch counts
                     * when centers are within rr + 1e-6. Box
                     * members rely on the AABB pre-test above
                     * (conservative, documented) since an
                     * exact-touch sphere probe can graze a
                     * face with dist == radius. */
                    float dx =
                        c->world_center[0] - cx;
                    float dy =
                        c->world_center[1] - cy;
                    float dz =
                        c->world_center[2] - cz;

                    if (c->shape == LE_COLLIDER_SPHERE) {
                        float rr =
                            radius + c->world_radius;

                        if (dx * dx + dy * dy + dz * dz <=
                            rr * rr + 1e-6f) {
                            hit = 1;
                        }
                    } else {
                        float ex = dx < 0.0f ? -dx : dx;
                        float ey = dy < 0.0f ? -dy : dy;
                        float ez = dz < 0.0f ? -dz : dz;

                        if (ex <= c->world_half[0] + radius &&
                            ey <= c->world_half[1] + radius &&
                            ez <= c->world_half[2] + radius) {
                            /* Axis-aligned conservative cover
                             * for rotated boxes (AABB pre-test
                             * already passed exactly). */
                            hit = 1;
                        }
                    }
                }
                if (hit == 0) {
                    continue;
                }
            }
            if (out != NULL && total < cap) {
                out[total].index = c->slot;
                out[total].generation =
                    world->slots[c->slot].generation;
                out[total].world_tag = world->tag;
            }
            total++;
        }
    }
    return total;
}

uint32_t le_physics_overlap_box(le_world *world, float cx,
                                float cy, float cz, float hx,
                                float hy, float hz,
                                uint32_t layer_mask,
                                int hit_triggers, le_object *out,
                                uint32_t cap) {
    uint32_t i;
    uint32_t total = 0;
    float mn[3];
    float mx[3];

    if (world == NULL || world->physics == NULL) {
        return 0;
    }
    if (!isfinite(cx) || !isfinite(cy) || !isfinite(cz) ||
        !isfinite(hx) || !isfinite(hy) || !isfinite(hz) ||
        hx <= 0.0f || hy <= 0.0f || hz <= 0.0f || hx > 1e6f ||
        hy > 1e6f || hz > 1e6f) {
        return 0;
    }
    mn[0] = cx - hx;
    mn[1] = cy - hy;
    mn[2] = cz - hz;
    mx[0] = cx + hx;
    mx[1] = cy + hy;
    mx[2] = cz + hz;
    le_query_refresh(world);
    {
        struct le_physics_world *pw = world->physics;

        for (i = 0; i < pw->collider_count; i++) {
            le_collider_entry *c = &pw->colliders[i];

            if (!le_query_visible(world, c, layer_mask,
                                  hit_triggers)) {
                continue;
            }
            if (mn[0] > c->aabb_max[0] ||
                c->aabb_min[0] > mx[0] ||
                mn[1] > c->aabb_max[1] ||
                c->aabb_min[1] > mx[1] ||
                mn[2] > c->aabb_max[2] ||
                c->aabb_min[2] > mx[2]) {
                continue;
            }
            /* AABB overlap is the overlap answer for the query
             * box (conservative, documented); narrow-phase
             * confirmation runs for sphere members via the
             * sphere path above. OBB members confirm by SAT
             * when the AABBs merely touch... AABB-level is
             * exact for axis-aligned members and conservative
             * otherwise (no false negatives, ever). */
            if (out != NULL && total < cap) {
                out[total].index = c->slot;
                out[total].generation =
                    world->slots[c->slot].generation;
                out[total].world_tag = world->tag;
            }
            total++;
        }
    }
    return total;
}

void le_physics_get_debug_counts(const le_world *world,
                                 le_physics_debug_counts *out) {
    uint32_t i;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (world == NULL || world->physics == NULL) {
        return;
    }
    {
        const struct le_physics_world *pw = world->physics;

        for (i = 0; i < pw->collider_count; i++) {
            if (pw->colliders[i].shape == LE_COLLIDER_SPHERE) {
                out->spheres++;
            } else if (pw->colliders[i].shape ==
                       LE_COLLIDER_CAPSULE) {
                out->capsules++;
            } else {
                out->boxes++;
            }
        }
        out->contacts = pw->contact_count;
        out->aabbs = pw->collider_count;
    }
}

uint32_t le_physics_extract_debug_lines(const le_world *world,
                                        float *out_xyz,
                                        uint32_t float_cap,
                                        int include_aabbs,
                                        int include_contacts) {
    /* Segment sink: appends xyz pairs, counts floats. */
    uint32_t used = 0;
    uint32_t i;

    if (world == NULL || world->physics == NULL) {
        return 0;
    }
    /* Poses may be stale (no fixed step ran yet, or the caller
     * teleported since the last step): refresh like every other
     * query so debug views never show an empty world. Const-
     * correct via the same mutable-world idiom as extraction
     * counts (le_scan_renderables). */
    le_query_refresh((le_world *)world);
#define LE_PUSH_SEG(ax, ay, az, bx, by, bz)                 \
    do {                                                    \
        if (out_xyz != NULL) {                              \
            if (used + 6u <= float_cap) {                   \
                out_xyz[used + 0u] = (ax);                  \
                out_xyz[used + 1u] = (ay);                  \
                out_xyz[used + 2u] = (az);                  \
                out_xyz[used + 3u] = (bx);                  \
                out_xyz[used + 4u] = (by);                  \
                out_xyz[used + 5u] = (bz);                  \
            }                                               \
        }                                                   \
        used += 6u;                                         \
    } while (0)
    {
        const struct le_physics_world *pw = world->physics;

        for (i = 0; i < pw->collider_count; i++) {
            const le_collider_entry *c = &pw->colliders[i];

            if (!c->aabb_valid) {
                continue;
            }
            if (c->shape == LE_COLLIDER_BOX) {
                float v[8][3];
                int e;
                static const int kEdges[12][2] = {
                    { 0, 1 }, { 1, 3 }, { 3, 2 }, { 2, 0 },
                    { 4, 5 }, { 5, 7 }, { 7, 6 }, { 6, 4 },
                    { 0, 4 }, { 1, 5 }, { 2, 6 }, { 3, 7 },
                };

                for (e = 0; e < 8; e++) {
                    float sx = (e & 1) ? 1.0f : -1.0f;
                    float sy = (e & 2) ? 1.0f : -1.0f;
                    float sz = (e & 4) ? 1.0f : -1.0f;

                    v[e][0] = c->world_center[0] +
                              (c->world_basis[0][0] * sx *
                                   c->world_half[0] +
                               c->world_basis[0][1] * sy *
                                   c->world_half[1] +
                               c->world_basis[0][2] * sz *
                                   c->world_half[2]);
                    v[e][1] = c->world_center[1] +
                              (c->world_basis[1][0] * sx *
                                   c->world_half[0] +
                               c->world_basis[1][1] * sy *
                                   c->world_half[1] +
                               c->world_basis[1][2] * sz *
                                   c->world_half[2]);
                    v[e][2] = c->world_center[2] +
                              (c->world_basis[2][0] * sx *
                                   c->world_half[0] +
                               c->world_basis[2][1] * sy *
                                   c->world_half[1] +
                               c->world_basis[2][2] * sz *
                                   c->world_half[2]);
                }
                for (e = 0; e < 12; e++) {
                    LE_PUSH_SEG(v[kEdges[e][0]][0],
                                v[kEdges[e][0]][1],
                                v[kEdges[e][0]][2],
                                v[kEdges[e][1]][0],
                                v[kEdges[e][1]][1],
                                v[kEdges[e][1]][2]);
                }
            } else if (c->shape == LE_COLLIDER_CAPSULE) {
                /* Capsule wireframe: 2 cap rings (in the plane
                 * perpendicular to the segment axis) + 4 axial
                 * rails connecting the ring vertices. */
                float ax0 = c->world_axis[0];
                float ax1 = c->world_axis[1];
                float ax2 = c->world_axis[2];
                float t0[3];
                float t1[3];
                float px;
                float py;
                float pz;
                float pl;
                int s;

                /* Pick a stable perpendicular. */
                if (ax0 * ax0 + ax2 * ax2 > 1e-12f) {
                    px = -ax2;
                    py = 0.0f;
                    pz = ax0;
                } else {
                    px = 0.0f;
                    py = -ax2;
                    pz = ax1;
                }
                pl = sqrtf(px * px + py * py + pz * pz);
                if (pl < 1e-9f) {
                    px = 1.0f;
                    py = 0.0f;
                    pz = 0.0f;
                    pl = 1.0f;
                }
                t0[0] = px / pl;
                t0[1] = py / pl;
                t0[2] = pz / pl;
                /* t1 = axis x t0. */
                t1[0] = ax1 * t0[2] - ax2 * t0[1];
                t1[1] = ax2 * t0[0] - ax0 * t0[2];
                t1[2] = ax0 * t0[1] - ax1 * t0[0];
                for (s = 0; s < 8; s++) {
                    float a0 = 6.2831853f * (float)s / 8.0f;
                    float b0 = 6.2831853f * (float)(s + 1) /
                               8.0f;
                    float c0 = cosf(a0);
                    float s0 = sinf(a0);
                    float c1 = cosf(b0);
                    float s1 = sinf(b0);
                    float r = c->world_cap_radius;
                    int cap;

                    for (cap = 0; cap < 2; cap++) {
                        const float *ctr =
                            (cap == 0) ? c->world_p0
                                       : c->world_p1;
                        float q0[3];
                        float q1[3];

                        q0[0] = ctr[0] +
                                (t0[0] * c0 + t1[0] * s0) * r;
                        q0[1] = ctr[1] +
                                (t0[1] * c0 + t1[1] * s0) * r;
                        q0[2] = ctr[2] +
                                (t0[2] * c0 + t1[2] * s0) * r;
                        q1[0] = ctr[0] +
                                (t0[0] * c1 + t1[0] * s1) * r;
                        q1[1] = ctr[1] +
                                (t0[1] * c1 + t1[1] * s1) * r;
                        q1[2] = ctr[2] +
                                (t0[2] * c1 + t1[2] * s1) * r;
                        LE_PUSH_SEG(q0[0], q0[1], q0[2],
                                    q1[0], q1[1], q1[2]);
                    }
                    if ((s % 2) == 0) {
                        float dx0 = t0[0] * c0 + t1[0] * s0;
                        float dy0 = t0[1] * c0 + t1[1] * s0;
                        float dz0 = t0[2] * c0 + t1[2] * s0;
                        float r = c->world_cap_radius;

                        LE_PUSH_SEG(
                            c->world_p0[0] + dx0 * r,
                            c->world_p0[1] + dy0 * r,
                            c->world_p0[2] + dz0 * r,
                            c->world_p1[0] + dx0 * r,
                            c->world_p1[1] + dy0 * r,
                            c->world_p1[2] + dz0 * r);
                    }
                }
            } else {
                /* 3 axis rings, 8 segments each. */
                int ring;
                int s;

                for (ring = 0; ring < 3; ring++) {
                    float px = 0.0f;
                    float py = 0.0f;

                    for (s = 0; s < 8; s++) {
                        /* Angle on the ring plane. */
                        float a0 =
                            6.2831853f * (float)s / 8.0f;
                        float c0 = cosf(a0);
                        float s0 = sinf(a0);
                        float a1 = 6.2831853f *
                                   (float)(s + 1) / 8.0f;
                        float c1 = cosf(a1);
                        float s1 = sinf(a1);
                        float p0[3];
                        float p1[3];
                        int u = (ring + 1) % 3;
                        int v = (ring + 2) % 3;

                        (void)px;
                        (void)py;
                        p0[0] = c->world_center[0];
                        p0[1] = c->world_center[1];
                        p0[2] = c->world_center[2];
                        p1[0] = p0[0];
                        p1[1] = p0[1];
                        p1[2] = p0[2];
                        p0[ring] += 0.0f;
                        {
                            float *pu0 =
                                (u == 0)
                                    ? &p0[0]
                                    : (u == 1) ? &p0[1] : &p0[2];
                            float *pv0 =
                                (v == 0)
                                    ? &p0[0]
                                    : (v == 1) ? &p0[1] : &p0[2];
                            float *pu1 =
                                (u == 0)
                                    ? &p1[0]
                                    : (u == 1) ? &p1[1] : &p1[2];
                            float *pv1 =
                                (v == 0)
                                    ? &p1[0]
                                    : (v == 1) ? &p1[1] : &p1[2];

                            *pu0 += c0 * c->world_radius;
                            *pv0 += s0 * c->world_radius;
                            *pu1 += c1 * c->world_radius;
                            *pv1 += s1 * c->world_radius;
                        }
                        LE_PUSH_SEG(p0[0], p0[1], p0[2], p1[0],
                                    p1[1], p1[2]);
                    }
                }
            }
            if (include_aabbs) {
                float mnx = c->aabb_min[0];
                float mny = c->aabb_min[1];
                float mnz = c->aabb_min[2];
                float mxx = c->aabb_max[0];
                float mxy = c->aabb_max[1];
                float mxz = c->aabb_max[2];

                LE_PUSH_SEG(mnx, mny, mnz, mxx, mny, mnz);
                LE_PUSH_SEG(mxx, mny, mnz, mxx, mny, mxz);
                LE_PUSH_SEG(mxx, mny, mxz, mnx, mny, mxz);
                LE_PUSH_SEG(mnx, mny, mxz, mnx, mny, mnz);
                LE_PUSH_SEG(mnx, mxy, mnz, mxx, mxy, mnz);
                LE_PUSH_SEG(mxx, mxy, mnz, mxx, mxy, mxz);
                LE_PUSH_SEG(mxx, mxy, mxz, mnx, mxy, mxz);
                LE_PUSH_SEG(mnx, mxy, mxz, mnx, mxy, mnz);
                LE_PUSH_SEG(mnx, mny, mnz, mnx, mxy, mnz);
                LE_PUSH_SEG(mxx, mny, mnz, mxx, mxy, mnz);
                LE_PUSH_SEG(mxx, mny, mxz, mxx, mxy, mxz);
                LE_PUSH_SEG(mnx, mny, mxz, mnx, mxy, mxz);
            }
        }
        if (include_contacts) {
            for (i = 0; i < pw->contact_count; i++) {
                const le_contact_point *cp = &pw->contacts[i];

                LE_PUSH_SEG(cp->point[0], cp->point[1],
                            cp->point[2],
                            cp->point[0] + cp->normal[0],
                            cp->point[1] + cp->normal[1],
                            cp->point[2] + cp->normal[2]);
            }
        }
    }
#undef LE_PUSH_SEG
    return used;
}

void le_physics_get_stats(le_world *world,
                          le_physics_stats *out_stats) {
    uint32_t i;

    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (world == NULL || world->physics == NULL) {
        return;
    }
    {
        struct le_physics_world *pw = world->physics;

        for (i = 0; i < pw->body_count; i++) {
            out_stats->body_count++;
            if (pw->bodies[i].type == LE_BODY_DYNAMIC) {
                out_stats->dynamic_bodies++;
            }
        }
        for (i = 0; i < pw->collider_count; i++) {
            out_stats->collider_count++;
            if (pw->colliders[i].is_trigger) {
                out_stats->trigger_count++;
            }
        }
        out_stats->broadphase_candidates = pw->stat_candidates;
        out_stats->narrowphase_tests = pw->stat_narrow_tests;
        out_stats->contact_count = pw->contact_count;
        out_stats->velocity_iterations = pw->velocity_iters;
        out_stats->position_iterations = pw->position_iters;
        out_stats->ray_queries = pw->stat_ray_queries;
        out_stats->events_dropped = pw->events_dropped_total;
        out_stats->shape_casts = pw->stat_shape_casts;
        out_stats->cast_candidates = pw->stat_cast_candidates;
        out_stats->ccd_casts = pw->stat_ccd_casts;
        out_stats->ccd_impacts = pw->stat_ccd_impacts;
        out_stats->character_sweeps = pw->stat_character_sweeps;
        out_stats->character_slides = pw->stat_character_slides;
        out_stats->depenetrations = pw->stat_depenetrations;
        out_stats->ground_probes = pw->stat_ground_probes;
        out_stats->step_attempts = pw->stat_step_attempts;
    }
}
