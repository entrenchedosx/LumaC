/*
 * Luma Engine Phase 30 headless tests: CCD foundation.
 *
 * Coverage: discrete sphere tunnels through a thin wall at
 * high speed; CONTINUOUS sphere stops; discrete capsule
 * tunnels; CONTINUOUS capsule stops; mode set/get + invalid
 * mode reject; default DISCRETE preserved (existing behavior);
 * CCD margin (no t=0 re-hit loop); iteration cap terminates;
 * dynamic-vs-dynamic deferred (no crash, discrete fallback).
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <luma_engine/luma_engine.h>

#include "internal/engine_internal.h"

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

/* Fast sphere (50 m/s) at a 0.1-thick wall, one fixed step of
 * dt=1/60: motion 0.83 m >> wall + diameter. Returns final x. */
static float fire_sphere(int continuous) {
    le_engine *e = NULL;
    le_world *w = NULL;
    le_object ball = LE_OBJECT_INVALID;
    le_object wall = LE_OBJECT_INVALID;
    float fx = 0.0f;

    make_engine(&e);
    make_world(e, &w);
    le_script_set_fixed_step(w, 1.0f / 60.0f, 4);
    le_object_create(w, &ball);
    le_object_create(w, &wall);
    {
        le_rigid_body_desc b;

        memset(&b, 0, sizeof(b));
        b.type = LE_BODY_DYNAMIC;
        b.mass = 1.0f;
        b.gravity_scale = 0.0f;
        le_object_add_rigid_body(w, &ball, &b);
    }
    {
        le_collider_desc d;

        memset(&d, 0, sizeof(d));
        d.shape = LE_COLLIDER_SPHERE;
        d.radius = 0.25f;
        d.orientation[3] = 1.0f;
        d.mask = 0xFFFFFFFFu;
        le_object_add_collider(w, &ball, &d);
    }
    {
        le_collider_desc d;

        memset(&d, 0, sizeof(d));
        d.shape = LE_COLLIDER_BOX;
        d.half_extents[0] = 0.05f;
        d.half_extents[1] = 5.0f;
        d.half_extents[2] = 5.0f;
        d.orientation[3] = 1.0f;
        d.mask = 0xFFFFFFFFu;
        le_object_add_collider(w, &wall, &d);
    }
    set_pos(w, &ball, -2.0f, 0.0f, 0.0f);
    set_pos(w, &wall, 0.0f, 0.0f, 0.0f);
    le_physics_set_linear_velocity(w, &ball, 50.0f, 0.0f,
                                   0.0f);
    if (continuous) {
        le_physics_set_collision_mode(w, &ball,
                                      LE_COLLISION_CONTINUOUS);
    }
    {
        /* 30 fixed steps = 0.5 s -> 25 m without a wall. */
        int i;

        for (i = 0; i < 30; i++) {
            le_world_update(w, 1.0f / 60.0f);
        }
    }
    {
        float p[3];

        le_object_get_position(w, &ball, p);
        fx = p[0];
    }
    le_world_destroy(w);
    le_engine_destroy(e);
    return fx;
}

static float fire_capsule(int continuous) {
    le_engine *e = NULL;
    le_world *w = NULL;
    le_object cap = LE_OBJECT_INVALID;
    le_object wall = LE_OBJECT_INVALID;
    float fx = 0.0f;

    make_engine(&e);
    make_world(e, &w);
    le_script_set_fixed_step(w, 1.0f / 60.0f, 4);
    le_object_create(w, &cap);
    le_object_create(w, &wall);
    {
        le_rigid_body_desc b;

        memset(&b, 0, sizeof(b));
        b.type = LE_BODY_DYNAMIC;
        b.mass = 1.0f;
        b.gravity_scale = 0.0f;
        le_object_add_rigid_body(w, &cap, &b);
    }
    {
        le_collider_desc d;

        memset(&d, 0, sizeof(d));
        d.shape = LE_COLLIDER_CAPSULE;
        d.capsule_radius = 0.25f;
        d.capsule_half_height = 0.5f;
        d.orientation[3] = 1.0f;
        d.mask = 0xFFFFFFFFu;
        le_object_add_collider(w, &cap, &d);
    }
    {
        le_collider_desc d;

        memset(&d, 0, sizeof(d));
        d.shape = LE_COLLIDER_BOX;
        d.half_extents[0] = 0.05f;
        d.half_extents[1] = 5.0f;
        d.half_extents[2] = 5.0f;
        d.orientation[3] = 1.0f;
        d.mask = 0xFFFFFFFFu;
        le_object_add_collider(w, &wall, &d);
    }
    set_pos(w, &cap, -2.0f, 0.0f, 0.0f);
    set_pos(w, &wall, 0.0f, 0.0f, 0.0f);
    le_physics_set_linear_velocity(w, &cap, 50.0f, 0.0f,
                                   0.0f);
    if (continuous) {
        le_physics_set_collision_mode(w, &cap,
                                      LE_COLLISION_CONTINUOUS);
    }
    {
        int i;

        for (i = 0; i < 30; i++) {
            le_world_update(w, 1.0f / 60.0f);
        }
    }
    {
        float p[3];

        le_object_get_position(w, &cap, p);
        fx = p[0];
    }
    le_world_destroy(w);
    le_engine_destroy(e);
    return fx;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 30 CCD tests...\n");

    /* ---- mode API ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        le_object_create(w, &o);
        TEST_CHECK(le_physics_get_collision_mode(w, &o) ==
                       LE_COLLISION_DISCRETE,
                   "default mode DISCRETE (missing body)");
        {
            le_rigid_body_desc b;

            memset(&b, 0, sizeof(b));
            b.type = LE_BODY_DYNAMIC;
            b.mass = 1.0f;
            le_object_add_rigid_body(w, &o, &b);
        }
        TEST_CHECK(le_physics_get_collision_mode(w, &o) ==
                       LE_COLLISION_DISCRETE,
                   "default mode DISCRETE (new body)");
        TEST_CHECK(le_physics_set_collision_mode(
                       w, &o, LE_COLLISION_CONTINUOUS) ==
                       LE_SUCCESS,
                   "set CONTINUOUS ok");
        TEST_CHECK(le_physics_get_collision_mode(w, &o) ==
                       LE_COLLISION_CONTINUOUS,
                   "get CONTINUOUS");
        TEST_CHECK(le_physics_set_collision_mode(
                       w, &o, (le_collision_mode)99) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "invalid mode rejected");
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- fast sphere: discrete tunnels, continuous stops ----
     * NOTE: scriptless worlds fall back to the 60 Hz default
     * rate. 30 frames x 1/60 = 0.5 s -> 25 m unobstructed. */
    {
        float xd = fire_sphere(0);

        printf("[info] discrete sphere final x = %f\n", xd);
        TEST_CHECK(xd > 5.0f, "discrete sphere tunnels");
    }
    {
        float xc = fire_sphere(1);

        printf("[info] continuous sphere final x = %f\n", xc);
        TEST_CHECK(xc < 0.5f && xc > -3.0f,
                   "continuous sphere stopped at wall");
    }

    /* ---- fast capsule ---- */
    {
        float xd = fire_capsule(0);

        printf("[info] discrete capsule final x = %f\n", xd);
        TEST_CHECK(xd > 5.0f, "discrete capsule tunnels");
    }
    {
        float xc = fire_capsule(1);

        printf("[info] continuous capsule final x = %f\n",
               xc);
        TEST_CHECK(xc < 0.5f && xc > -3.5f,
                   "continuous capsule stopped at wall");
    }

    /* ---- CCD margin: resting continuous body stays put ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ball = LE_OBJECT_INVALID;
        le_object floor = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        le_script_set_fixed_step(w, 1.0f / 60.0f, 4);
        le_object_create(w, &ball);
        le_object_create(w, &floor);
        {
            le_rigid_body_desc b;

            memset(&b, 0, sizeof(b));
            b.type = LE_BODY_DYNAMIC;
            b.mass = 1.0f;
            b.gravity_scale = 1.0f;
            le_object_add_rigid_body(w, &ball, &b);
            le_physics_set_collision_mode(
                w, &ball, LE_COLLISION_CONTINUOUS);
        }
        {
            le_rigid_body_desc b;

            memset(&b, 0, sizeof(b));
            b.type = LE_BODY_STATIC;
            le_object_add_rigid_body(w, &floor, &b);
        }
        {
            le_collider_desc d;

            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_SPHERE;
            d.radius = 0.5f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &ball, &d);
        }
        {
            le_collider_desc d;

            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 10.0f;
            d.half_extents[1] = 0.5f;
            d.half_extents[2] = 10.0f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &floor, &d);
        }
        set_pos(w, &ball, 0.0f, 3.0f, 0.0f);
        set_pos(w, &floor, 0.0f, -0.5f, 0.0f);
        {
            /* Direct sweep sanity: cast the ball shape down
             * from y=3 (gap 2.5 to the surface at y=0, r=0.5:
             * TOI fraction must be 0.5, NOT 0). Also dump the
             * floor world pose to prove refresh works. The
             * ball itself is excluded by slot (else the cast
             * starts inside its own collider: self-hit t=0). */
            float bc[3] = { 0.0f, 3.0f, 0.0f };
            float bd[3] = { 0.0f, -5.0f, 0.0f };
            le_shape_hit bh;
            le_collider_desc fd;
            le_object me = ball;

            memset(&bh, 0, sizeof(bh));
            memset(&fd, 0, sizeof(fd));
            printf("[info] direct cast=%d\n",
                   le_physics_sphere_cast(w, bc, 0.5f, bd,
                                          0xFFFFFFFFu, 0,
                                          me.index,
                                          &bh));
            printf("[info] cast frac=%f dist=%f n=(%f,%f,%f) "
                   "overlap=%d pen=%f\n",
                   bh.fraction, bh.distance, bh.normal[0],
                   bh.normal[1], bh.normal[2],
                   bh.started_overlapping, bh.penetration);
            {
                le_object f2 = floor;
                float fp[3];

                le_object_get_position(w, &f2, fp);
                printf("[info] floor pos=(%f,%f,%f)\n",
                       fp[0], fp[1], fp[2]);
            }
            /* One manual physics sub-step: does velocity move? */
            {
                float v0[3];

                le_physics_get_linear_velocity(w, &ball, v0);
                printf("[info] pre-step vy=%f\n", v0[1]);
                le_physics_step(w, 1.0f / 60.0f);
                le_physics_get_linear_velocity(w, &ball, v0);
                {
                    float p[3];

                    le_object_get_position(w, &ball, p);
                    printf("[info] post-1-substep y=%f vy=%f\n",
                           p[1], v0[1]);
                }
                le_physics_step(w, 1.0f / 60.0f);
                {
                    float p[3];

                    le_object_get_position(w, &ball, p);
                    printf("[info] post-2-substep y=%f\n",
                           p[1]);
                }
            }
        }
        {
            int i;

            for (i = 0; i < 600; i++) {
                le_world_update(w, 1.0f / 60.0f);
                if (i == 0 || i == 120) {
                    float p[3];
                    float v[3];

                    le_object_get_position(w, &ball, p);
                    le_physics_get_linear_velocity(w, &ball,
                                                   v);
                    printf("[info] rest step %d: y=%f vy=%f\n",
                           i, p[1], v[1]);
                }
            }
        }
        {
            float p[3];

            le_object_get_position(w, &ball, p);
            printf("[info] ccd rest y = %f\n", p[1]);
            TEST_CHECK(p[1] > 0.3f && p[1] < 1.2f,
                       "continuous body rests (no sink/loop)");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- dynamic-vs-dynamic deferred: no crash ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object a = LE_OBJECT_INVALID;
        le_object b = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        le_script_set_fixed_step(w, 1.0f / 60.0f, 4);
        le_object_create(w, &a);
        le_object_create(w, &b);
        {
            le_rigid_body_desc bd;

            memset(&bd, 0, sizeof(bd));
            bd.type = LE_BODY_DYNAMIC;
            bd.mass = 1.0f;
            bd.gravity_scale = 0.0f;
            le_object_add_rigid_body(w, &a, &bd);
            le_object_add_rigid_body(w, &b, &bd);
            le_physics_set_collision_mode(
                w, &a, LE_COLLISION_CONTINUOUS);
            le_physics_set_collision_mode(
                w, &b, LE_COLLISION_CONTINUOUS);
        }
        {
            le_collider_desc d;

            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_SPHERE;
            d.radius = 0.5f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &a, &d);
            le_object_add_collider(w, &b, &d);
        }
        set_pos(w, &a, -3.0f, 0.0f, 0.0f);
        set_pos(w, &b, 3.0f, 0.0f, 0.0f);
        le_physics_set_linear_velocity(w, &a, 20.0f, 0.0f,
                                       0.0f);
        le_physics_set_linear_velocity(w, &b, -20.0f, 0.0f,
                                       0.0f);
        {
            int i;

            for (i = 0; i < 60; i++) {
                le_world_update(w, 1.0f / 60.0f);
            }
        }
        {
            float pa[3];
            float pb[3];

            le_object_get_position(w, &a, pa);
            le_object_get_position(w, &b, pb);
            TEST_CHECK(isfinite(pa[0]) && isfinite(pb[0]),
                       "dyn-vs-dyn deferred, world finite");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    printf("Phase 30 CCD tests: %d passed, %d failed\n",
           g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
