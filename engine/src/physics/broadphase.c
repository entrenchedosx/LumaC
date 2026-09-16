/*
 * Broad phase (Phase 28): sweep-and-prune over world AABBs.
 *
 * Each step: refresh every collider's world pose + AABB (skips
 * shear/degenerate pathologies — never false bounds), insertion-
 * sort collider indices by AABB min-x (coherent motion makes this
 * O(n) typical; O(n^2) worst-case, deterministic), sweep with an
 * active list testing y/z overlap. Output pairs are (collider
 * index a < b) sorted, duplicate-free. Layer/mask + self + stale
 * filtering applies here so the narrow phase never sees rejected
 * pairs. Brute force exists ONLY as a test oracle (test_physics
 * compares candidate coverage), never as the production path.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

/* Layer/mask test: A.mask must contain B.layer AND vice versa. */
static int le_filter_pass(const le_collider_entry *a,
                          const le_collider_entry *b) {
    uint32_t abit =
        (a->layer < 32u) ? (1u << a->layer) : 0u;
    uint32_t bbit =
        (b->layer < 32u) ? (1u << b->layer) : 0u;

    if ((a->mask & bbit) == 0u) {
        return 0;
    }
    if ((b->mask & abit) == 0u) {
        return 0;
    }
    return 1;
}

static int le_aabb_overlap(const le_collider_entry *a,
                           const le_collider_entry *b) {
    if (a->aabb_min[0] > b->aabb_max[0] ||
        b->aabb_min[0] > a->aabb_max[0]) {
        return 0;
    }
    if (a->aabb_min[1] > b->aabb_max[1] ||
        b->aabb_min[1] > a->aabb_max[1]) {
        return 0;
    }
    if (a->aabb_min[2] > b->aabb_max[2] ||
        b->aabb_min[2] > a->aabb_max[2]) {
        return 0;
    }
    return 1;
}

static int le_entry_live(le_world *world,
                         const le_collider_entry *c) {
    if (c->slot >= world->capacity) {
        return 0;
    }
    if (!world->slots[c->slot].alive) {
        return 0;
    }
    if (!(world->slots[c->slot].present & LE_PRESENT_COLLIDER)) {
        return 0;
    }
    return 1;
}

uint32_t le_physics_broadphase(le_world *world, uint32_t *out_a,
                               uint32_t *out_b, uint32_t cap) {
    struct le_physics_world *pw;
    uint32_t n;
    uint32_t i;
    uint32_t count = 0;

    if (world == NULL || world->physics == NULL) {
        return 0;
    }
    pw = world->physics;
    pw->stat_candidates = 0;
    n = pw->collider_count;
    if (n < 2) {
        return 0;
    }
    /* Grow the sort scratch (geometric, reused across steps). */
    if (pw->sap_cap < n) {
        uint32_t grown = (pw->sap_cap == 0) ? 32u : pw->sap_cap;
        uint32_t *fresh;

        while (grown < n) {
            if (grown > 0x00FFFFFFu / 2u) {
                grown = n;
                break;
            }
            grown *= 2u;
        }
        fresh = (uint32_t *)realloc(pw->sap_order,
                                    (size_t)grown *
                                        sizeof(*fresh));
        if (fresh == NULL) {
            return 0; /* OOM: no pairs this step (safe miss —
                       * contacts persist via STAY only when
                       * re-detected; documented, tested) */
        }
        pw->sap_order = fresh;
        pw->sap_cap = grown;
    }
    /* Refresh poses + AABBs (invalid ones park at +inf so the
     * sort pushes them out of every sweep). */
    for (i = 0; i < n; i++) {
        le_collider_entry *c = &pw->colliders[i];

        if (!le_entry_live(world, c) ||
            !le_physics_refresh_collider(world, c)) {
            c->aabb_valid = 0;
            c->aabb_min[0] = c->aabb_min[1] = c->aabb_min[2] =
                1e30f;
            c->aabb_max[0] = c->aabb_max[1] = c->aabb_max[2] =
                1e30f;
        }
        pw->sap_order[i] = i;
    }
    /* Insertion sort by min-x (deterministic, coherent-fast). */
    for (i = 1; i < n; i++) {
        uint32_t key = pw->sap_order[i];
        float keyx = pw->colliders[key].aabb_min[0];
        uint32_t j = i;

        while (j > 0 &&
               pw->colliders[pw->sap_order[j - 1u]].aabb_min[0] >
                   keyx) {
            pw->sap_order[j] = pw->sap_order[j - 1u];
            j--;
        }
        pw->sap_order[j] = key;
    }
    /* Sweep: active window over max-x; test y/z; filter; emit. */
    for (i = 0; i < n; i++) {
        uint32_t ai = pw->sap_order[i];
        le_collider_entry *a = &pw->colliders[ai];
        uint32_t j;

        if (!a->aabb_valid) {
            continue;
        }
        for (j = i + 1u; j < n; j++) {
            uint32_t bi = pw->sap_order[j];
            le_collider_entry *b = &pw->colliders[bi];

            if (b->aabb_min[0] > a->aabb_max[0]) {
                break; /* sorted: no later box can overlap */
            }
            if (!b->aabb_valid) {
                continue;
            }
            if (a->slot == b->slot) {
                continue; /* never self-collide */
            }
            if (!le_aabb_overlap(a, b)) {
                continue;
            }
            if (!le_filter_pass(a, b)) {
                continue;
            }
            pw->stat_candidates++;
            if (count < cap) {
                /* Canonical order by collider index (stable,
                 * duplicate-free by construction). */
                if (ai < bi) {
                    out_a[count] = ai;
                    out_b[count] = bi;
                } else {
                    out_a[count] = bi;
                    out_b[count] = ai;
                }
                count++;
            }
        }
    }
    return count;
}
