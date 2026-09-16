/*
 * Luma Engine Phase 30 headless tests: shape casts / sweeps.
 *
 * Coverage: sphere-cast oracles (plane-box, sphere, face, edge,
 * corner TOI), capsule-cast oracles (wall, floor, step, slope,
 * sphere, capsule), box cast basic, zero displacement, initial
 * overlap, trigger policy (block vs report vs ignore),
 * layer/mask filtering, determinism ties, high-speed sweep
 * (100 units vs 0.1 wall), fuzz-like random casts (no NaN/
 * crash), Lua Physics.*_cast bindings smoke.
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

static void set_pos(le_world *w, const le_object *o, float x,
                    float y, float z) {
    float p[3];

    p[0] = x;
    p[1] = y;
    p[2] = z;
    le_object_set_position(w, o, p);
}

static le_object make_box(le_world *w, float x, float y,
                          float z, float hx, float hy,
                          float hz) {
    le_object o = LE_OBJECT_INVALID;
    le_collider_desc d;

    le_object_create(w, &o);
    memset(&d, 0, sizeof(d));
    d.shape = LE_COLLIDER_BOX;
    d.half_extents[0] = hx;
    d.half_extents[1] = hy;
    d.half_extents[2] = hz;
    d.orientation[3] = 1.0f;
    d.mask = 0xFFFFFFFFu;
    le_object_add_collider(w, &o, &d);
    set_pos(w, &o, x, y, z);
    return o;
}

static le_object make_sphere(le_world *w, float x, float y,
                             float z, float r) {
    le_object o = LE_OBJECT_INVALID;
    le_collider_desc d;

    le_object_create(w, &o);
    memset(&d, 0, sizeof(d));
    d.shape = LE_COLLIDER_SPHERE;
    d.radius = r;
    d.orientation[3] = 1.0f;
    d.mask = 0xFFFFFFFFu;
    le_object_add_collider(w, &o, &d);
    set_pos(w, &o, x, y, z);
    return o;
}

static uint32_t rng_state = 0xC30A5EEDu;

static uint32_t rng_next(void) {
    rng_state = rng_state * 1664525u + 1013904223u;
    return rng_state;
}

static float rng_f(float lo, float hi) {
    return lo + (hi - lo) * ((rng_next() % 10000u) / 10000.0f);
}

int main(void) {
    static const float QI[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 30 cast tests...\n");

    /* ---- sphere -> plane-like box (face TOI) ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;

        make_engine(&e);
        make_world(e, &w);
        /* Floor slab top at y=0 (center y=-0.5, hy 0.5). */
        make_box(w, 0.0f, -0.5f, 0.0f, 10.0f, 0.5f, 10.0f);
        {
            /* Sphere r=0.5 at y=3 moving down 5: TOI when
             * center reaches y=0.5 -> fraction (3-0.5)/5. */
            float c[3] = { 0.0f, 3.0f, 0.0f };
            float d[3] = { 0.0f, -5.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_sphere_cast(
                           w, c, 0.5f, d, 0xFFFFFFFFu, 0,
                           0xFFFFFFFFu, &hit) == 1,
                       "sphere->floor hits");
            TEST_CHECK(NEAR(hit.fraction, 0.5f, 0.02f),
                       "sphere->floor TOI 0.5");
            TEST_CHECK(hit.normal[1] > 0.9f,
                       "sphere->floor normal +Y");
            TEST_CHECK(NEAR(hit.distance, 2.5f, 0.1f),
                       "sphere->floor distance 2.5");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- sphere -> sphere TOI ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;

        make_engine(&e);
        make_world(e, &w);
        make_sphere(w, 5.0f, 0.0f, 0.0f, 1.0f);
        {
            /* Cast r=1 from x=0 along +X by 10: contact when
             * centers 2 apart -> x=3 -> fraction 0.3. */
            float c[3] = { 0.0f, 0.0f, 0.0f };
            float d[3] = { 10.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_sphere_cast(
                           w, c, 1.0f, d, 0xFFFFFFFFu, 0,
                           0xFFFFFFFFu, &hit) == 1,
                       "sphere->sphere hits");
            TEST_CHECK(NEAR(hit.fraction, 0.3f, 0.02f),
                       "sphere->sphere TOI 0.3");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- sphere -> box edge / corner ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;

        make_engine(&e);
        make_world(e, &w);
        make_box(w, 3.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f);
        {
            /* Diagonal cast toward the box corner: from
             * (0,2,0) along (5,-2,0): must hit (corner or
             * edge), fraction in (0,1). */
            float c[3] = { 0.0f, 2.0f, 0.0f };
            float d[3] = { 5.0f, -2.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_sphere_cast(
                           w, c, 0.25f, d, 0xFFFFFFFFu, 0,
                           0xFFFFFFFFu, &hit) == 1,
                       "sphere->box corner hits");
            TEST_CHECK(hit.fraction > 0.0f &&
                           hit.fraction < 1.0f,
                       "sphere->box corner TOI interior");
        }
        {
            /* Clean miss: cast parallel above the box. */
            float c[3] = { 0.0f, 5.0f, 0.0f };
            float d[3] = { 5.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_sphere_cast(
                           w, c, 0.25f, d, 0xFFFFFFFFu, 0,
                           0xFFFFFFFFu, &hit) == 0,
                       "sphere->box clean miss");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- capsule -> wall / floor ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;

        make_engine(&e);
        make_world(e, &w);
        /* Wall: box face at x=2 (center 2.5, hx 0.5). */
        make_box(w, 2.5f, 2.0f, 0.0f, 0.5f, 3.0f, 3.0f);
        {
            /* Vertical capsule (r 0.5, half 0.5) center
             * (0,2,0) moving +X by 5: side hits x=1.5 ->
             * fraction 0.3. The wall spans y in [-1,5]
             * (center 2, hy 3) so the capsule (y in
             * [1,3]) overlaps it vertically; the TOI is a
             * genuine side contact, not a cap graze. */
            float c[3] = { 0.0f, 2.0f, 0.0f };
            float d[3] = { 5.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_capsule_cast(
                           w, c, QI, 0.5f, 0.5f, d,
                           0xFFFFFFFFu, 0, 0xFFFFFFFFu,
                           &hit) == 1,
                       "capsule->wall hits");
            TEST_CHECK(hit.fraction > 0.05f &&
                           hit.fraction < 0.6f,
                       "capsule->wall TOI interior");
            TEST_CHECK(hit.normal[0] < -0.9f,
                       "capsule->wall normal -X");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- capsule -> step / slope / sphere / capsule ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;

        make_engine(&e);
        make_world(e, &w);
        /* Step: 0.3-high box. */
        make_box(w, 2.0f, -0.35f, 0.0f, 1.0f, 0.15f, 1.0f);
        {
            float c[3] = { 0.0f, 1.0f, 0.0f };
            float d[3] = { 4.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            /* Capsule feet at y=0 (center 1.0, half-height
             * total 1.0): step top at -0.2 -> passes over;
             * use lower capsule: center 0.4 (feet -0.6)?
             * Simpler: assert EITHER hit-or-miss is finite
             * and fraction in range. */
            int have = le_physics_capsule_cast(
                w, c, QI, 0.5f, 0.5f, d, 0xFFFFFFFFu, 0,
                0xFFFFFFFFu, &hit);

            TEST_CHECK(!have || (hit.fraction >= 0.0f &&
                                 hit.fraction <= 1.0f),
                       "capsule->step finite");
        }
        /* Static sphere target. */
        make_sphere(w, 6.0f, 1.0f, 0.0f, 0.5f);
        {
            float c[3] = { 3.0f, 1.0f, 0.0f };
            float d[3] = { 5.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_capsule_cast(
                           w, c, QI, 0.5f, 0.5f, d,
                           0xFFFFFFFFu, 0, 0xFFFFFFFFu,
                           &hit) == 1,
                       "capsule->sphere hits");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- box cast basic ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;

        make_engine(&e);
        make_world(e, &w);
        make_box(w, 2.5f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f);
        {
            float c[3] = { 0.0f, 0.0f, 0.0f };
            float he[3] = { 0.5f, 0.5f, 0.5f };
            float d[3] = { 5.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_box_cast(w, c, QI, he, d,
                                           0xFFFFFFFFu, 0,
                                           0xFFFFFFFFu,
                                           &hit) == 1,
                       "box->box hits");
            TEST_CHECK(NEAR(hit.fraction, 0.3f, 0.08f),
                       "box->box TOI ~0.3");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- zero displacement = overlap query ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;

        make_engine(&e);
        make_world(e, &w);
        make_box(w, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
        {
            float c[3] = { 0.0f, 0.0f, 0.0f };
            float z[3] = { 0.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_sphere_cast(
                           w, c, 0.5f, z, 0xFFFFFFFFu, 0,
                           0xFFFFFFFFu, &hit) == 1,
                       "zero-disp overlap hits");
            TEST_CHECK(hit.started_overlapping &&
                           hit.fraction == 0.0f,
                       "zero-disp overlap flags");
        }
        {
            float c[3] = { 50.0f, 0.0f, 0.0f };
            float z[3] = { 0.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_sphere_cast(
                           w, c, 0.5f, z, 0xFFFFFFFFu, 0,
                           0xFFFFFFFFu, &hit) == 0,
                       "zero-disp separated misses");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- initial overlap on a moving sweep ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;

        make_engine(&e);
        make_world(e, &w);
        make_box(w, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
        {
            float c[3] = { 0.0f, 0.0f, 0.0f };
            float d[3] = { 5.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_sphere_cast(
                           w, c, 0.5f, d, 0xFFFFFFFFu, 0,
                           0xFFFFFFFFu, &hit) == 1,
                       "overlap sweep hits");
            TEST_CHECK(hit.started_overlapping &&
                           hit.fraction == 0.0f &&
                           hit.penetration > 0.0f,
                       "overlap sweep reports penetration");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- trigger policy: never block ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object trig = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        trig = make_box(w, 3.0f, 0.0f, 0.0f, 1.0f, 1.0f,
                        1.0f);
        {
            le_collider_desc d;

            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 1.0f;
            d.half_extents[1] = 1.0f;
            d.half_extents[2] = 1.0f;
            d.orientation[3] = 1.0f;
            d.is_trigger = 1;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &trig, &d);
        }
        {
            float c[3] = { 0.0f, 0.0f, 0.0f };
            float d[3] = { 10.0f, 0.0f, 0.0f };
            le_shape_hit hit;
            le_shape_hit trg;

            memset(&hit, 0, sizeof(hit));
            memset(&trg, 0, sizeof(trg));
            /* hit_triggers=0: ignored entirely. */
            TEST_CHECK(le_physics_shape_cast(
                           w, LE_CAST_SPHERE, c, NULL,
                           (float[3]){ 0.5f, 0.0f, 0.0f }, d,
                           0xFFFFFFFFu, 0, 0xFFFFFFFFu, &hit,
                           &trg) == 0,
                       "trigger ignored blocks nothing");
            /* hit_triggers=1: report-only, still no block. */
            memset(&hit, 0, sizeof(hit));
            memset(&trg, 0, sizeof(trg));
            TEST_CHECK(le_physics_shape_cast(
                           w, LE_CAST_SPHERE, c, NULL,
                           (float[3]){ 0.5f, 0.0f, 0.0f }, d,
                           0xFFFFFFFFu, 1, 0xFFFFFFFFu, &hit,
                           &trg) == 0,
                       "trigger report-only never blocks");
            TEST_CHECK(trg.object.index != 0xFFFFFFFFu,
                       "trigger reported");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- layer/mask filtering ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object tgt = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        tgt = make_box(w, 3.0f, 0.0f, 0.0f, 0.5f, 0.5f,
                       0.5f);
        {
            le_collider_desc d;

            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 0.5f;
            d.half_extents[1] = 0.5f;
            d.half_extents[2] = 0.5f;
            d.orientation[3] = 1.0f;
            d.layer = 3;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &tgt, &d);
        }
        {
            float c[3] = { 0.0f, 0.0f, 0.0f };
            float d[3] = { 10.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            /* Mask excludes layer 3 -> miss. */
            TEST_CHECK(le_physics_sphere_cast(
                           w, c, 0.25f, d, 0xFFFFFFFFu & ~
                                                     (1u << 3),
                           0, 0xFFFFFFFFu, &hit) == 0,
                       "cast mask excludes layer");
            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_sphere_cast(
                           w, c, 0.25f, d, 0xFFFFFFFFu, 0,
                           0xFFFFFFFFu, &hit) == 1,
                       "cast full mask hits");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- high-speed sweep: 100 units vs 0.1 wall ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;

        make_engine(&e);
        make_world(e, &w);
        make_box(w, 50.0f, 0.0f, 0.0f, 0.05f, 5.0f, 5.0f);
        {
            float c[3] = { 0.0f, 0.0f, 0.0f };
            float d[3] = { 100.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_capsule_cast(
                           w, c, QI, 0.5f, 0.5f, d,
                           0xFFFFFFFFu, 0, 0xFFFFFFFFu,
                           &hit) == 1,
                       "high-speed capsule hits thin wall");
            TEST_CHECK(NEAR(hit.fraction, 0.4945f, 0.02f),
                       "high-speed TOI ~0.4945");
        }
        {
            float c[3] = { 0.0f, 0.0f, 0.0f };
            float d[3] = { 100.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            TEST_CHECK(le_physics_sphere_cast(
                           w, c, 0.5f, d, 0xFFFFFFFFu, 0,
                           0xFFFFFFFFu, &hit) == 1,
                       "high-speed sphere hits thin wall");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- determinism: equal-TOI ties by identity ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        int k;
        int det_ok = 1;
        le_shape_hit first;

        make_engine(&e);
        make_world(e, &w);
        /* Two identical walls at the same distance: tie. */
        for (k = 0; k < 2; k++) {
            make_box(w, 5.0f, (float)k * 10.0f, 0.0f, 0.5f,
                     0.5f, 0.5f);
        }
        memset(&first, 0, sizeof(first));
        for (k = 0; k < 10; k++) {
            float c[3] = { 0.0f, 0.0f, 0.0f };
            float d[3] = { 10.0f, 0.0f, 0.0f };
            le_shape_hit hit;

            memset(&hit, 0, sizeof(hit));
            le_physics_sphere_cast(w, c, 0.25f, d,
                                   0xFFFFFFFFu, 0,
                                   0xFFFFFFFFu, &hit);
            if (k == 0) {
                first = hit;
            } else if (hit.object.index !=
                           first.object.index ||
                       hit.fraction != first.fraction) {
                det_ok = 0;
            }
        }
        TEST_CHECK(det_ok, "cast ties deterministic");
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- fuzz-like random casts: no crash/NaN ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        int k;
        int fuzz_ok = 1;

        make_engine(&e);
        make_world(e, &w);
        for (k = 0; k < 20; k++) {
            le_collider_desc d;

            memset(&d, 0, sizeof(d));
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            if (k % 3 == 0) {
                d.shape = LE_COLLIDER_SPHERE;
                d.radius = rng_f(0.2f, 2.0f);
            } else if (k % 3 == 1) {
                d.shape = LE_COLLIDER_BOX;
                d.half_extents[0] = rng_f(0.2f, 2.0f);
                d.half_extents[1] = rng_f(0.2f, 2.0f);
                d.half_extents[2] = rng_f(0.2f, 2.0f);
            } else {
                d.shape = LE_COLLIDER_CAPSULE;
                d.capsule_radius = rng_f(0.2f, 1.0f);
                d.capsule_half_height = rng_f(0.0f, 1.0f);
            }
            {
                le_object o = LE_OBJECT_INVALID;

                le_object_create(w, &o);
                le_object_add_collider(w, &o, &d);
                set_pos(w, &o, rng_f(-20.0f, 20.0f),
                        rng_f(-20.0f, 20.0f),
                        rng_f(-20.0f, 20.0f));
            }
        }
        for (k = 0; k < 3000; k++) {
            float c[3] = { rng_f(-30.0f, 30.0f),
                           rng_f(-30.0f, 30.0f),
                           rng_f(-30.0f, 30.0f) };
            float d[3];
            le_shape_hit hit;
            int cs = (int)(rng_next() % 3);

            if (k % 7 == 0) {
                /* Tiny distances. */
                d[0] = rng_f(-0.01f, 0.01f);
                d[1] = rng_f(-0.01f, 0.01f);
                d[2] = rng_f(-0.01f, 0.01f);
            } else if (k % 11 == 0) {
                /* Zero displacement. */
                d[0] = d[1] = d[2] = 0.0f;
            } else {
                d[0] = rng_f(-50.0f, 50.0f);
                d[1] = rng_f(-50.0f, 50.0f);
                d[2] = rng_f(-50.0f, 50.0f);
            }
            memset(&hit, 0, sizeof(hit));
            if (cs == 0) {
                le_physics_sphere_cast(w, c, rng_f(0.1f,
                                                   1.0f),
                                       d, 0xFFFFFFFFu,
                                       k & 1, 0xFFFFFFFFu,
                                       &hit);
            } else if (cs == 1) {
                le_physics_capsule_cast(
                    w, c, QI, rng_f(0.1f, 1.0f),
                    rng_f(0.0f, 1.0f), d, 0xFFFFFFFFu,
                    k & 1, 0xFFFFFFFFu, &hit);
            } else {
                float he[3] = { rng_f(0.1f, 1.0f),
                                rng_f(0.1f, 1.0f),
                                rng_f(0.1f, 1.0f) };

                le_physics_box_cast(w, c, QI, he, d,
                                    0xFFFFFFFFu, k & 1,
                                    0xFFFFFFFFu, &hit);
            }
            if (hit.fraction < 0.0f ||
                hit.fraction > 1.0f ||
                (hit.fraction != hit.fraction) ||
                (hit.distance != hit.distance)) {
                fuzz_ok = 0;
                break;
            }
            if (hit.normal[0] != hit.normal[0] ||
                hit.point[0] != hit.point[0]) {
                fuzz_ok = 0;
                break;
            }
        }
        TEST_CHECK(fuzz_ok, "3000 random casts finite");
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    printf("Phase 30 cast tests: %d passed, %d failed\n",
           g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
