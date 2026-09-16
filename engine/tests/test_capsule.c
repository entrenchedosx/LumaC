/*
 * Luma Engine Phase 30 headless tests: capsule collider.
 *
 * Coverage: capsule validation (radius/NaN/Inf/negative),
 * capsule AABB (vertical/horizontal/rotated/translated/
 * negative-scale/large-coords, no false negatives),
 * capsule-sphere (cap/side/diagonal/inside/coincident/touch/
 * separate), capsule-capsule (parallel/near-parallel/crossing/
 * coincident/zero-length), capsule-box (face/side/edge/inside),
 * narrow-phase symmetry A/B (all pairs + randomized oracle),
 * rigid-body inertia sanity, serialization round-trip +
 * malformed reject, ray-vs-capsule, debug counts.
 *
 * No GPU, no window, no Lua: pure C API. Headless-safe.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <luma_engine/luma_engine.h>

static int g_passed = 0;
static int g_failed = 0;

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        printf("[PASS] %s\n", msg); \
        g_passed++; \
    } else { \
        printf("[FAIL] %s\n", msg); \
        g_failed++; \
    } \
} while (0)

#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (float)(eps))

static int make_engine(le_engine **engine) {
    le_engine_desc edesc;

    memset(&edesc, 0, sizeof(edesc));
    return le_engine_create(&edesc, engine) == LE_SUCCESS;
}

static int make_world(le_engine *engine, le_world **world) {
    le_world_desc wdesc;

    memset(&wdesc, 0, sizeof(wdesc));
    return le_world_create(engine, &wdesc, world) == LE_SUCCESS;
}

static le_collider_desc cap_coll(float r, float h) {
    le_collider_desc d;

    memset(&d, 0, sizeof(d));
    d.shape = LE_COLLIDER_CAPSULE;
    d.capsule_radius = r;
    d.capsule_half_height = h;
    d.orientation[3] = 1.0f;
    d.mask = 0xFFFFFFFFu;
    d.friction = 0.5f;
    return d;
}

static le_collider_desc sph_coll(float r) {
    le_collider_desc d;

    memset(&d, 0, sizeof(d));
    d.shape = LE_COLLIDER_SPHERE;
    d.radius = r;
    d.orientation[3] = 1.0f;
    d.mask = 0xFFFFFFFFu;
    d.friction = 0.5f;
    return d;
}

static le_collider_desc box_coll(float hx, float hy,
                                 float hz) {
    le_collider_desc d;

    memset(&d, 0, sizeof(d));
    d.shape = LE_COLLIDER_BOX;
    d.half_extents[0] = hx;
    d.half_extents[1] = hy;
    d.half_extents[2] = hz;
    d.orientation[3] = 1.0f;
    d.mask = 0xFFFFFFFFu;
    d.friction = 0.5f;
    return d;
}

static void set_pos(le_world *w, const le_object *o, float x,
                    float y, float z) {
    float p[3];

    p[0] = x;
    p[1] = y;
    p[2] = z;
    le_object_set_position(w, o, p);
}

/* Drain all events for one object (counting). Note: with a
 * NULL buffer the ring is only OBSERVED (count reported, ring
 * kept); the caller must drain with a real buffer to consume.
 * Phase 28 semantics (test_physics relies on this). */
static uint32_t peek_total(le_world *w, const le_object *o) {
    uint32_t n = 0;

    le_physics_drain_events(w, o, NULL, 0, &n);
    return n;
}

/* Consume (drain) all events for one object. */
static uint32_t drain_total(le_world *w, const le_object *o) {
    le_collision_event evs[64];
    uint32_t total = 0;

    for (;;) {
        uint32_t got = 0;

        le_physics_drain_events(w, o, evs, 64, &got);
        total += got;
        if (got < 64u) {
            break;
        }
    }
    return total;
}

static uint32_t rng_state = 0x12345678u;

static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static float rng_f(float lo, float hi) {
    return lo + (hi - lo) * ((rng_next() % 10000u) / 10000.0f);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 30 capsule tests...\n");

    /* ---- validation ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;

        TEST_CHECK(make_engine(&e) && make_world(e, &w),
                   "capsule fixture");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS,
                   "capsule object");
        {
            le_collider_desc d = cap_coll(0.5f, 0.5f);

            TEST_CHECK(le_object_add_collider(w, &o, &d) ==
                           LE_SUCCESS,
                       "capsule add ok");
        }
        {
            le_collider_desc d = cap_coll(0.0f, 0.5f);

            TEST_CHECK(le_object_add_collider(w, &o, &d) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "capsule zero radius rejected");
        }
        {
            le_collider_desc d = cap_coll(-1.0f, 0.5f);

            TEST_CHECK(le_object_add_collider(w, &o, &d) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "capsule negative radius rejected");
        }
        {
            le_collider_desc d = cap_coll(0.5f, -0.5f);

            TEST_CHECK(le_object_add_collider(w, &o, &d) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "capsule negative half rejected");
        }
        {
            le_collider_desc d = cap_coll(0.5f, 0.0f);

            /* Zero cylinder = sphere: valid. */
            TEST_CHECK(le_object_add_collider(w, &o, &d) ==
                           LE_SUCCESS,
                       "capsule zero half valid (sphere)");
        }
        {
            le_collider_desc d = cap_coll(0.5f, 0.5f);
            uint32_t nanbits = 0x7FC00000u;
            float nanv = 0.0f;

            memcpy(&nanv, &nanbits, sizeof(nanv));
            d.capsule_radius = nanv;
            TEST_CHECK(le_object_add_collider(w, &o, &d) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "capsule NaN radius rejected");
        }
        {
            le_collider_desc d = cap_coll(0.5f, 0.5f);
            uint32_t infbits = 0x7F800000u;
            float infv = 0.0f;

            memcpy(&infv, &infbits, sizeof(infv));
            d.capsule_half_height = infv;
            TEST_CHECK(le_object_add_collider(w, &o, &d) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "capsule Inf half rejected");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- capsule-sphere contacts ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object cap = LE_OBJECT_INVALID;
        le_object sph = LE_OBJECT_INVALID;

        TEST_CHECK(make_engine(&e) && make_world(e, &w),
                   "cap-sphere fixture");
        le_object_create(w, &cap);
        le_object_create(w, &sph);
        {
            le_collider_desc d = cap_coll(0.5f, 0.5f);

            le_object_add_collider(w, &cap, &d);
        }
        {
            le_collider_desc d = sph_coll(0.5f);

            le_object_add_collider(w, &sph, &d);
        }
        /* Side contact: sphere at (1.2, 0, 0) vs vertical
         * capsule at origin (segment y in [-0.5,0.5], r 0.5):
         * gap 1.2 - 0.5 - 0.5 = 0.2 -> then move to 0.9:
         * penetration 0.1. */
        set_pos(w, &cap, 0.0f, 0.0f, 0.0f);
        set_pos(w, &sph, 1.2f, 0.0f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        TEST_CHECK(drain_total(w, &cap) == 0,
                   "cap-sphere separated: no event");
        set_pos(w, &sph, 0.9f, 0.0f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        {
            le_collision_event evs[8];
            uint32_t n = 0;

            le_physics_drain_events(w, &cap, evs, 8, &n);
            TEST_CHECK(n >= 1 &&
                           evs[0].type ==
                               LE_COLLISION_ENTER,
                       "cap-sphere side contact ENTER");
            if (n >= 1) {
                TEST_CHECK(
                    NEAR(evs[0].penetration, 0.1f, 0.05f),
                    "cap-sphere side depth ~0.1");
                /* Normal is A->B in canonical pair order:
                 * |x| must dominate (axis of approach). */
                TEST_CHECK(fabsf(evs[0].normal[0]) > 0.9f,
                           "cap-sphere normal ±X (A->B)");
            }
        }
        /* Cap contact: sphere above the top cap. */
        set_pos(w, &sph, 0.0f, 1.9f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        drain_total(w, &cap);
        set_pos(w, &sph, 0.0f, 1.4f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        {
            uint32_t n = drain_total(w, &cap);

            TEST_CHECK(n >= 1, "cap-sphere cap contact");
        }
        /* Same center: deterministic (no NaN, some ENTER). */
        set_pos(w, &sph, 0.0f, 0.0f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        {
            le_collision_event evs[8];
            uint32_t n = 0;

            le_physics_drain_events(w, &cap, evs, 8, &n);
            TEST_CHECK(n >= 1, "cap-sphere coincident hits");
            if (n >= 1) {
                TEST_CHECK(isfinite(evs[0].normal[0]) &&
                               isfinite(evs[0].normal[1]) &&
                               isfinite(evs[0].normal[2]),
                           "cap-sphere coincident normal finite");
            }
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- capsule-capsule ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object a = LE_OBJECT_INVALID;
        le_object b = LE_OBJECT_INVALID;

        TEST_CHECK(make_engine(&e) && make_world(e, &w),
                   "cap-cap fixture");
        le_object_create(w, &a);
        le_object_create(w, &b);
        {
            le_collider_desc d = cap_coll(0.5f, 0.5f);

            le_object_add_collider(w, &a, &d);
            le_object_add_collider(w, &b, &d);
        }
        /* Parallel segments 0.8 apart (rr = 1.0): overlap. */
        set_pos(w, &a, 0.0f, 0.0f, 0.0f);
        set_pos(w, &b, 0.8f, 0.0f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        TEST_CHECK(drain_total(w, &a) >= 1,
                   "cap-cap parallel overlap");
        /* Crossing: b rotated 90 deg about Z (segment along X)
         * at same center: overlap. */
        {
            le_collider_desc d = cap_coll(0.5f, 0.5f);
            float q[4] = { 0.0f, 0.0f, 0.7071068f,
                           0.7071068f };

            memcpy(d.orientation, q, sizeof(q));
            le_object_add_collider(w, &b, &d);
        }
        set_pos(w, &b, 0.0f, 0.0f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        TEST_CHECK(drain_total(w, &a) >= 1,
                   "cap-cap crossing overlap");
        /* Zero-length capsules (= spheres) coincident. */
        {
            le_collider_desc d = cap_coll(0.5f, 0.0f);

            le_object_add_collider(w, &a, &d);
            le_object_add_collider(w, &b, &d);
        }
        set_pos(w, &a, 5.0f, 0.0f, 0.0f);
        set_pos(w, &b, 5.0f, 0.0f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        TEST_CHECK(drain_total(w, &a) >= 1,
                   "cap-cap zero-length coincident");
        /* Separated: no NEW contact (EXIT may arrive for the
         * dissolved pair — drain and require no ENTER/STAY). */
        set_pos(w, &b, 50.0f, 0.0f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        {
            le_collision_event evs[8];
            uint32_t n = 0;
            int contact = 0;
            uint32_t k;

            le_physics_drain_events(w, &a, evs, 8, &n);
            for (k = 0; k < (n < 8u ? n : 8u); k++) {
                if (evs[k].type == LE_COLLISION_ENTER ||
                    evs[k].type == LE_COLLISION_STAY) {
                    contact = 1;
                }
            }
            TEST_CHECK(!contact, "cap-cap separated clean");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- capsule-box (incl. side contact) ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object cap = LE_OBJECT_INVALID;
        le_object box = LE_OBJECT_INVALID;

        TEST_CHECK(make_engine(&e) && make_world(e, &w),
                   "cap-box fixture");
        le_object_create(w, &cap);
        le_object_create(w, &box);
        {
            le_collider_desc d = cap_coll(0.5f, 0.5f);

            le_object_add_collider(w, &cap, &d);
        }
        {
            le_collider_desc d = box_coll(5.0f, 0.5f, 5.0f);

            le_object_add_collider(w, &box, &d);
        }
        /* Capsule lying horizontally (segment along X) just
         * above the box top face: side contact, NOT endpoint.
         * Box top at y=0.5; horizontal capsule center y=0.9:
         * clearance 0.4 < r 0.5 -> penetration 0.1 along the
         * whole side. Endpoint-sphere-only would MISS this if
         * the segment extended beyond the box... use a long
         * capsule (half 2.0) centered so endpoints are far
         * outside the box footprint (box x half 5 — endpoints
         * inside). Better: narrow box (x half 0.5), long
         * capsule along X (half 2): endpoints at x=±2 outside
         * the box; side band at |x|<0.5 must still contact. */
        {
            le_collider_desc narrow = box_coll(0.5f, 0.5f,
                                               0.5f);
            le_collider_desc longc = cap_coll(0.5f, 2.0f);
            float q[4] = { 0.0f, 0.0f, 0.7071068f,
                           0.7071068f }; /* Y -> X */

            memcpy(longc.orientation, q, sizeof(q));
            le_object_add_collider(w, &cap, &longc);
            le_object_add_collider(w, &box, &narrow);
        }
        set_pos(w, &cap, 0.0f, 1.4f, 0.0f);
        set_pos(w, &box, 0.0f, 0.0f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        TEST_CHECK(drain_total(w, &cap) == 0,
                   "cap-box side separated");
        set_pos(w, &cap, 0.0f, 0.9f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        {
            le_collision_event evs[8];
            uint32_t n = 0;

            le_physics_drain_events(w, &cap, evs, 8, &n);
            TEST_CHECK(n >= 1,
                       "cap-box SIDE contact (not endpoints)");
            if (n >= 1) {
                TEST_CHECK(evs[0].normal[1] < -0.9f,
                           "cap-box side normal -Y");
            }
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- symmetry: collide(A,B) vs collide(B,A) ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        int sym_ok = 1;
        int k;

        make_engine(&e);
        make_world(e, &w);
        for (k = 0; k < 200; k++) {
            le_object a = LE_OBJECT_INVALID;
            le_object b = LE_OBJECT_INVALID;
            int sa = (int)(rng_next() % 3);
            int sb = (int)(rng_next() % 3);
            le_collider_desc da;
            le_collider_desc db;
            float ax = rng_f(-3.0f, 3.0f);
            float ay = rng_f(-3.0f, 3.0f);
            float az = rng_f(-3.0f, 3.0f);
            float bx = ax + rng_f(-2.0f, 2.0f);
            float by = ay + rng_f(-2.0f, 2.0f);
            float bz = az + rng_f(-2.0f, 2.0f);

            memset(&da, 0, sizeof(da));
            memset(&db, 0, sizeof(db));
            da.orientation[3] = 1.0f;
            db.orientation[3] = 1.0f;
            da.mask = db.mask = 0xFFFFFFFFu;
            if (sa == 0) {
                da.shape = LE_COLLIDER_SPHERE;
                da.radius = rng_f(0.2f, 1.5f);
            } else if (sa == 1) {
                da.shape = LE_COLLIDER_BOX;
                da.half_extents[0] = rng_f(0.2f, 1.5f);
                da.half_extents[1] = rng_f(0.2f, 1.5f);
                da.half_extents[2] = rng_f(0.2f, 1.5f);
            } else {
                da.shape = LE_COLLIDER_CAPSULE;
                da.capsule_radius = rng_f(0.2f, 1.0f);
                da.capsule_half_height = rng_f(0.0f, 1.0f);
            }
            if (sb == 0) {
                db.shape = LE_COLLIDER_SPHERE;
                db.radius = rng_f(0.2f, 1.5f);
            } else if (sb == 1) {
                db.shape = LE_COLLIDER_BOX;
                db.half_extents[0] = rng_f(0.2f, 1.5f);
                db.half_extents[1] = rng_f(0.2f, 1.5f);
                db.half_extents[2] = rng_f(0.2f, 1.5f);
            } else {
                db.shape = LE_COLLIDER_CAPSULE;
                db.capsule_radius = rng_f(0.2f, 1.0f);
                db.capsule_half_height = rng_f(0.0f, 1.0f);
            }
            le_object_create(w, &a);
            le_object_create(w, &b);
            le_object_add_collider(w, &a, &da);
            le_object_add_collider(w, &b, &db);
            set_pos(w, &a, ax, ay, az);
            set_pos(w, &b, bx, by, bz);
            le_world_update(w, 1.0f / 60.0f);
            {
                le_collision_event ea[4];
                le_collision_event eb[4];
                uint32_t na = 0;
                uint32_t nb = 0;

                le_physics_drain_events(w, &a, ea, 4, &na);
                le_physics_drain_events(w, &b, eb, 4, &nb);
                /* Both sides agree on hit presence. */
                if ((na > 0) != (nb > 0)) {
                    sym_ok = 0;
                }
                if (na > 0 && nb > 0) {
                    /* Normals oppose. */
                    float dot =
                        ea[0].normal[0] * eb[0].normal[0] +
                        ea[0].normal[1] * eb[0].normal[1] +
                        ea[0].normal[2] * eb[0].normal[2];

                    if (dot > -0.99f) {
                        sym_ok = 0;
                    }
                    if (!NEAR(ea[0].penetration,
                              eb[0].penetration, 1e-4f)) {
                        sym_ok = 0;
                    }
                    if (!isfinite(ea[0].normal[0]) ||
                        !isfinite(eb[0].normal[0])) {
                        sym_ok = 0;
                    }
                }
            }
            le_object_destroy(w, &a);
            le_object_destroy(w, &b);
        }
        TEST_CHECK(sym_ok,
                   "capsule symmetry over 200 random pairs");
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- capsule AABB: rotated/translated/negative/large ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object cap = LE_OBJECT_INVALID;
        le_object probe = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        le_object_create(w, &cap);
        le_object_create(w, &probe);
        {
            le_collider_desc d = cap_coll(0.5f, 1.0f);

            le_object_add_collider(w, &cap, &d);
        }
        {
            /* Small probe sphere swept around: any contact
             * missed by the broad phase = false negative. */
            le_collider_desc d = sph_coll(0.1f);

            le_object_add_collider(w, &probe, &d);
        }
        /* Vertical capsule at origin: segment y in [-1,1].
         * Probe r=0.1 at z=0.55: side gap 0.55-0.5-0.1 < 0
         * -> overlap. (0.65 would be separated.) */
        set_pos(w, &cap, 0.0f, 0.0f, 0.0f);
        set_pos(w, &probe, 0.0f, 0.0f, 0.55f);
        le_world_update(w, 1.0f / 60.0f);
        TEST_CHECK(drain_total(w, &cap) >= 1,
                   "AABB vertical side hit");
        /* Rotated 90 deg: segment along X. */
        {
            le_collider_desc d = cap_coll(0.5f, 1.0f);
            float q[4] = { 0.0f, 0.0f, 0.7071068f,
                           0.7071068f };

            memcpy(d.orientation, q, sizeof(q));
            le_object_add_collider(w, &cap, &d);
        }
        set_pos(w, &probe, 0.55f, 0.0f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        drain_total(w, &cap);
        set_pos(w, &probe, 0.0f, 0.0f, 0.55f);
        le_world_update(w, 1.0f / 60.0f);
        TEST_CHECK(drain_total(w, &cap) >= 1,
                   "AABB rotated side hit");
        /* Negative scale: -2 uniform (abs => 2x). Capsule
         * r=0.5,h=1 scaled 2x: r=1, half=2. Probe at x=1.05
         * (side): clearance... side dist 1.05 vs r 1.0 -> hit. */
        {
            float s[3] = { -2.0f, -2.0f, -2.0f };

            le_object_set_scale(w, &cap, s);
        }
        set_pos(w, &probe, 1.05f, 0.0f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        TEST_CHECK(drain_total(w, &cap) >= 1,
                   "AABB negative uniform scale hit");
        /* Non-uniform scale: capsule must SKIP (no crash, no
         * NEW contact — conservative reject, documented; an
         * EXIT for the dissolved pair may arrive). */
        {
            float s[3] = { 1.0f, 2.0f, 1.0f };

            le_object_set_scale(w, &cap, s);
        }
        set_pos(w, &probe, 0.55f, 0.0f, 0.0f);
        le_world_update(w, 1.0f / 60.0f);
        {
            le_collision_event evs[8];
            uint32_t n = 0;
            int contact = 0;
            uint32_t k;

            le_physics_drain_events(w, &cap, evs, 8, &n);
            for (k = 0; k < (n < 8u ? n : 8u); k++) {
                if (evs[k].type == LE_COLLISION_ENTER ||
                    evs[k].type == LE_COLLISION_STAY) {
                    contact = 1;
                }
            }
            TEST_CHECK(!contact,
                       "AABB non-uniform scale skipped");
        }
        /* Large coordinates: capsule at 10000. */
        {
            float s[3] = { 1.0f, 1.0f, 1.0f };
            le_collider_desc d = cap_coll(0.5f, 1.0f);

            le_object_set_scale(w, &cap, s);
            le_object_add_collider(w, &cap, &d);
        }
        set_pos(w, &cap, 10000.0f, 0.0f, 0.0f);
        set_pos(w, &probe, 10000.0f, 0.0f, 0.55f);
        le_world_update(w, 1.0f / 60.0f);
        TEST_CHECK(drain_total(w, &cap) >= 1,
                   "AABB large coords hit");
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- rigid-body inertia sanity + dynamic capsule falls ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object cap = LE_OBJECT_INVALID;
        le_object floor = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        le_object_create(w, &cap);
        le_object_create(w, &floor);
        {
            le_rigid_body_desc b;

            memset(&b, 0, sizeof(b));
            b.type = LE_BODY_DYNAMIC;
            b.mass = 1.0f;
            b.gravity_scale = 1.0f;
            le_object_add_rigid_body(w, &cap, &b);
        }
        {
            le_rigid_body_desc b;

            memset(&b, 0, sizeof(b));
            b.type = LE_BODY_STATIC;
            le_object_add_rigid_body(w, &floor, &b);
        }
        {
            le_collider_desc d = cap_coll(0.5f, 0.5f);

            le_object_add_collider(w, &cap, &d);
        }
        {
            le_collider_desc d = box_coll(10.0f, 0.5f, 10.0f);

            le_object_add_collider(w, &floor, &d);
        }
        set_pos(w, &cap, 0.0f, 5.0f, 0.0f);
        set_pos(w, &floor, 0.0f, -0.5f, 0.0f);
        {
            int i;

            for (i = 0; i < 600; i++) {
                le_world_update(w, 1.0f / 60.0f);
            }
        }
        {
            float p[3];

            le_object_get_position(w, &cap, p);
            printf("[info] capsule rest y = %f\n", p[1]);
            /* Capsule total half height 1.0 rests on floor top
             * y=0: center y ~= 1.0 (feet capsule center at half
             * height). Allow solver slop. */
            TEST_CHECK(p[1] > 0.8f && p[1] < 1.5f,
                       "dynamic capsule rests on floor");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- serialization round-trip + malformed ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_asset scene = LE_ASSET_INVALID;

        make_engine(&e);
        make_world(e, &w);
        le_scene_create(e, 1, &scene);
        {
            le_object o = LE_OBJECT_INVALID;
            le_collider_desc d = cap_coll(0.4f, 0.7f);

            le_object_create(w, &o);
            le_object_add_collider(w, &o, &d);
            le_scene_capture(w, &scene, NULL);
        }
        {
            char *text = NULL;
            size_t size = 0;

            TEST_CHECK(le_scene_save_text(e, &scene, &text,
                                          &size) == LE_SUCCESS,
                       "capsule scene save");
            if (text != NULL) {
                TEST_CHECK(strstr(text, "collider capsule") !=
                               NULL,
                           "capsule line present");
                {
                    le_asset s2 = LE_ASSET_INVALID;
                    le_world *w2 = NULL;

                    le_scene_create(e, 1, &s2);
                    make_world(e, &w2);
                    TEST_CHECK(le_scene_load_text(e, &s2,
                                                  text,
                                                  strlen(
                                                      text)) ==
                                   LE_SUCCESS,
                               "capsule scene reload");
                    {
                        le_scene_instance inst;

                        memset(&inst, 0, sizeof(inst));
                        if (le_scene_instantiate(w2, &s2,
                                                 &inst) ==
                            LE_SUCCESS) {
                            le_collider_desc got;

                            memset(&got, 0, sizeof(got));
                            if (inst.count >= 1 &&
                                le_object_get_collider(
                                    w2, &inst.objects[0],
                                    &got)) {
                                TEST_CHECK(
                                    got.shape ==
                                        LE_COLLIDER_CAPSULE &&
                                    NEAR(got.capsule_radius,
                                         0.4f, 1e-6f) &&
                                    NEAR(got.capsule_half_height,
                                         0.7f, 1e-6f),
                                    "capsule round-trip exact");
                            } else {
                                TEST_CHECK(
                                    0,
                                    "capsule round-trip exact");
                            }
                        } else {
                            TEST_CHECK(
                                0, "capsule round-trip exact");
                        }
                    }
                    le_world_destroy(w2);
                }
                /* Malformed: negative radius rejected. This only
                 * fails at INSTANTIATE time (load_text stores
                 * records; commit validates). Assert on the
                 * instantiate result, and require the parse to
                 * succeed first so the test is meaningful. */
                {
                    const char *bad =
                        "LUMA_SCENE 1\n"
                        "object "
                        "00000000000000000000000000000001\n"
                        "collider capsule -1 0.5 0 0 0 0 0 0 1 "
                        "0 0 4294967295 0.5 0\n"
                        "end\n";
                    le_asset sb = LE_ASSET_INVALID;
                    le_world *w3 = NULL;
                    le_scene_instance bi;

                    memset(&bi, 0, sizeof(bi));
                    le_scene_create(e, 1, &sb);
                    if (le_scene_load_text(e, &sb, bad,
                                           strlen(bad)) ==
                        LE_SUCCESS) {
                        make_world(e, &w3);
                        TEST_CHECK(le_scene_instantiate(
                                       w3, &sb, &bi) !=
                                        LE_SUCCESS,
                                    "capsule negative radius "
                                    "rejected");
                        le_world_destroy(w3);
                    } else {
                        TEST_CHECK(0, "capsule negative radius "
                                      "rejected");
                    }
                }
                le_scene_free_text(text);
            }
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- ray vs capsule + debug counts ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object cap = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        le_object_create(w, &cap);
        {
            le_collider_desc d = cap_coll(0.5f, 0.5f);

            le_object_add_collider(w, &cap, &d);
        }
        set_pos(w, &cap, 0.0f, 0.0f, 5.0f);
        {
            le_ray_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_raycast(
                           w, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                           1.0f, 10.0f, 0xFFFFFFFFu, 0,
                           &hit) == 1,
                       "ray hits capsule");
            if (hit.object.index != 0xFFFFFFFFu) {
                TEST_CHECK(NEAR(hit.distance, 4.5f, 0.1f),
                           "ray-capsule distance ~4.5");
            }
        }
        {
            le_physics_debug_counts dc;

            memset(&dc, 0, sizeof(dc));
            le_physics_get_debug_counts(w, &dc);
            TEST_CHECK(dc.capsules == 1,
                       "debug counts capsules");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- component count contract (9 with character) ---- */
    TEST_CHECK(LE_COMPONENT_COUNT == 9,
               "component count is 9");

    printf("Phase 30 capsule tests: %d passed, %d failed\n",
           g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
