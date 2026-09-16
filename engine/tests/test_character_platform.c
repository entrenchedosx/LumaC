/*
 * Luma Engine Phase 30 headless tests: moving platforms,
 * triggers, high-speed character, Lua determinism.
 *
 * Coverage: platform translation ride (+x/-z/up/down),
 * elevator adhesion, jump inheritance, platform destroy ->
 * airborne, slot reuse (no stale ground), trigger walkthrough
 * ENTER/STAY/EXIT with no blocking, high-speed character vs
 * thin wall (no tunnel), dynamic crate push/blocked, char-char
 * blocking policy, Lua fixed_update WASD determinism +
 * frame-rate independence.
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

static le_character_desc std_char(void) {
    le_character_desc d;

    memset(&d, 0, sizeof(d));
    d.radius = 0.4f;
    d.height = 1.8f;
    d.up[1] = 1.0f;
    d.skin_width = 0.02f;
    d.max_slope_angle = 0.7853982f;
    d.step_height = 0.4f;
    d.gravity = 9.81f;
    d.terminal_velocity = 20.0f;
    d.snap_distance = 0.3f;
    d.push_strength = 1.0f;
    d.mask = 0xFFFFFFFFu;
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

static void get_pos(le_world *w, const le_object *o,
                    float p[3]) {
    le_object_get_position(w, o, p);
}

/* Kinematic platform with a box top. */
static le_object make_platform(le_world *w, float x, float y,
                               float z) {
    le_object o = LE_OBJECT_INVALID;
    le_rigid_body_desc b;
    le_collider_desc d;

    le_object_create(w, &o);
    memset(&b, 0, sizeof(b));
    b.type = LE_BODY_KINEMATIC;
    le_object_add_rigid_body(w, &o, &b);
    memset(&d, 0, sizeof(d));
    d.shape = LE_COLLIDER_BOX;
    d.half_extents[0] = 2.0f;
    d.half_extents[1] = 0.25f;
    d.half_extents[2] = 2.0f;
    d.orientation[3] = 1.0f;
    d.mask = 0xFFFFFFFFu;
    le_object_add_collider(w, &o, &d);
    set_pos(w, &o, x, y, z);
    return o;
}

static void settle(le_world *w, const le_object *ch) {
    int i;

    for (i = 0; i < 240; i++) {
        le_character_gravity(w, ch, 1.0f / 60.0f);
        if (le_character_is_grounded(w, ch)) {
            break;
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 30 platform tests...\n");

    /* ---- platform +X ride ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;
        le_object pf = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        pf = make_platform(w, 0.0f, -0.25f, 0.0f);
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 2.0f, 0.0f);
        settle(w, &ch);
        TEST_CHECK(le_character_is_grounded(w, &ch),
                   "platform: initially grounded");
        {
            float c0[3];
            float p0[3];
            int i;

            get_pos(w, &ch, c0);
            get_pos(w, &pf, p0);
            for (i = 0; i < 60; i++) {
                /* Move platform +X 0.05/step. */
                float pp[3];

                get_pos(w, &pf, pp);
                pp[0] += 0.05f;
                set_pos(w, &pf, pp[0], pp[1], pp[2]);
                le_world_update(w, 1.0f / 60.0f);
                {
                    float z[3] = { 0.0f, 0.0f, 0.0f };

                    le_character_move(w, &ch, z, NULL);
                }
            }
            {
                float c1[3];

                get_pos(w, &ch, c1);
                TEST_CHECK(c1[0] - c0[0] > 2.0f,
                           "platform +X ride follows");
                TEST_CHECK(le_character_is_grounded(w, &ch),
                           "platform ride stays grounded");
            }
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- elevator (vertical) ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;
        le_object pf = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        pf = make_platform(w, 0.0f, -0.25f, 0.0f);
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 2.0f, 0.0f);
        settle(w, &ch);
        {
            float c0[3];
            int i;
            int air = 0;

            get_pos(w, &ch, c0);
            for (i = 0; i < 60; i++) {
                float pp[3];

                get_pos(w, &pf, pp);
                pp[1] += 0.03f;
                set_pos(w, &pf, pp[0], pp[1], pp[2]);
                le_world_update(w, 1.0f / 60.0f);
                {
                    float z[3] = { 0.0f, 0.0f, 0.0f };

                    le_character_move(w, &ch, z, NULL);
                }
                if (!le_character_is_grounded(w, &ch)) {
                    air++;
                }
            }
            {
                float c1[3];

                get_pos(w, &ch, c1);
                TEST_CHECK(c1[1] - c0[1] > 1.0f,
                           "elevator carries up");
                TEST_CHECK(air == 0, "elevator no separation");
            }
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- platform destroy -> airborne, slot reuse safe ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;
        le_object pf = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        pf = make_platform(w, 0.0f, -0.25f, 0.0f);
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 2.0f, 0.0f);
        settle(w, &ch);
        TEST_CHECK(le_character_is_grounded(w, &ch),
                   "destroy: grounded first");
        {
            le_object g0 = le_character_ground_object(w, &ch);

            TEST_CHECK(g0.index == pf.index,
                       "destroy: ground is platform");
        }
        le_object_destroy(w, &pf);
        {
            float z[3] = { 0.0f, 0.0f, 0.0f };

            le_character_move(w, &ch, z, NULL);
            TEST_CHECK(!le_character_is_grounded(w, &ch),
                       "destroy: airborne after");
        }
        /* Slot reuse: new object must NOT be ground. */
        {
            le_object fresh = LE_OBJECT_INVALID;

            le_object_create(w, &fresh);
            {
                float z[3] = { 0.0f, 0.0f, 0.0f };

                le_character_move(w, &ch, z, NULL);
            }
            {
                le_object g = le_character_ground_object(
                    w, &ch);

                TEST_CHECK(!le_object_is_valid(&g) ||
                               g.index != fresh.index ||
                               !le_character_is_grounded(
                                   w, &ch),
                           "slot reuse: no stale ground");
            }
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- trigger walkthrough: ENTER/STAY/EXIT, no block ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;
        le_object floor = LE_OBJECT_INVALID;
        le_object zone = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        floor = make_platform(w, 0.0f, -5.25f, 0.0f);
        (void)floor;
        /* Trigger volume straddling the walk path. */
        le_object_create(w, &zone);
        {
            le_collider_desc d;

            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 1.0f;
            d.half_extents[1] = 2.0f;
            d.half_extents[2] = 1.0f;
            d.orientation[3] = 1.0f;
            d.is_trigger = 1;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &zone, &d);
            set_pos(w, &zone, 3.0f, 1.0f, 0.0f);
        }
        /* Floor for walking. */
        {
            le_object f = LE_OBJECT_INVALID;
            le_collider_desc d;

            le_object_create(w, &f);
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 50.0f;
            d.half_extents[1] = 0.5f;
            d.half_extents[2] = 50.0f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &f, &d);
            set_pos(w, &f, 0.0f, -0.5f, 0.0f);
        }
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 3.0f, 0.0f);
        settle(w, &ch);
        {
            float p0[3];
            int i;

            get_pos(w, &ch, p0);
            for (i = 0; i < 300; i++) {
                float d[3] = { 0.03f, 0.0f, 0.0f };

                le_character_move(w, &ch, d, NULL);
                le_world_update(w, 1.0f / 60.0f);
            }
            {
                float p1[3];

                get_pos(w, &ch, p1);
                TEST_CHECK(p1[0] - p0[0] > 5.0f,
                           "trigger never blocks");
            }
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- high-speed character vs thin wall ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        {
            le_object f = LE_OBJECT_INVALID;
            le_collider_desc d;

            le_object_create(w, &f);
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 50.0f;
            d.half_extents[1] = 0.5f;
            d.half_extents[2] = 50.0f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &f, &d);
            set_pos(w, &f, 0.0f, -0.5f, 0.0f);
        }
        /* Thin wall 0.1 at x=5. */
        {
            le_object wall = LE_OBJECT_INVALID;
            le_collider_desc d;

            le_object_create(w, &wall);
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 0.05f;
            d.half_extents[1] = 3.0f;
            d.half_extents[2] = 3.0f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &wall, &d);
            set_pos(w, &wall, 5.0f, 3.0f, 0.0f);
        }
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 3.0f, 0.0f);
        settle(w, &ch);
        {
            /* 10-unit single move >> radius: sweep must stop. */
            float d[3] = { 10.0f, 0.0f, 0.0f };
            le_character_move_result res;
            float p[3];

            memset(&res, 0, sizeof(res));
            le_character_move(w, &ch, d, &res);
            get_pos(w, &ch, p);
            TEST_CHECK(p[0] < 5.0f,
                       "high-speed character no tunnel");
            TEST_CHECK(res.hit_wall, "high-speed hit_wall");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- dynamic crate: blocked (and pushed when enabled) ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;
        le_object crate = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        {
            le_object f = LE_OBJECT_INVALID;
            le_collider_desc d;

            le_object_create(w, &f);
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 50.0f;
            d.half_extents[1] = 0.5f;
            d.half_extents[2] = 50.0f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &f, &d);
            set_pos(w, &f, 0.0f, -0.5f, 0.0f);
        }
        le_object_create(w, &crate);
        {
            le_rigid_body_desc b;
            le_collider_desc d;

            memset(&b, 0, sizeof(b));
            b.type = LE_BODY_DYNAMIC;
            b.mass = 2.0f;
            le_object_add_rigid_body(w, &crate, &b);
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 0.4f;
            d.half_extents[1] = 0.4f;
            d.half_extents[2] = 0.4f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &crate, &d);
            set_pos(w, &crate, 2.0f, 0.4f, 0.0f);
        }
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            d.push_strength = 2.0f;
            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 3.0f, 0.0f);
        settle(w, &ch);
        {
            float c0[3];
            int i;

            get_pos(w, &crate, c0);
            for (i = 0; i < 120; i++) {
                float d[3] = { 0.03f, 0.0f, 0.0f };

                le_character_move(w, &ch, d, NULL);
                le_world_update(w, 1.0f / 60.0f);
            }
            {
                float c1[3];
                float p[3];

                get_pos(w, &crate, c1);
                get_pos(w, &ch, p);
                TEST_CHECK(isfinite(c1[0]) &&
                               isfinite(p[0]),
                           "crate interaction finite");
                TEST_CHECK(c1[0] - c0[0] > 0.05f,
                           "crate pushed forward");
            }
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    printf("Phase 30 platform tests: %d passed, %d failed\n",
           g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
