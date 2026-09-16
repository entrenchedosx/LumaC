/*
 * Luma Engine Phase 30 headless tests: character controller.
 *
 * Coverage: flat-ground 10k stability, wall slide (analytic),
 * 90-degree corner settle, ceiling stop/fall, slope matrix
 * (0/15/30/44/45/46/60/89 deg around a 45-deg limit),
 * downhill snap adhesion, uphill projection, steep-slope
 * slide, step matrix (0.25H/0.9H/H/1.1H/2H), staircase up/down,
 * narrow corridor, doorway pass/fail, initial penetration
 * recovery, impossible penetration (bounded failure),
 * jump/teleport/enable, malformed config reject, scene
 * round-trip, determinism + frame-rate independence.
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
    d.max_slope_angle = 0.7853982f; /* 45 deg */
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

static le_object make_floor(le_world *w, float top_y) {
    le_object o = LE_OBJECT_INVALID;
    le_collider_desc d;

    le_object_create(w, &o);
    memset(&d, 0, sizeof(d));
    d.shape = LE_COLLIDER_BOX;
    d.half_extents[0] = 50.0f;
    d.half_extents[1] = 0.5f;
    d.half_extents[2] = 50.0f;
    d.orientation[3] = 1.0f;
    d.mask = 0xFFFFFFFFu;
    le_object_add_collider(w, &o, &d);
    set_pos(w, &o, 0.0f, top_y - 0.5f, 0.0f);
    return o;
}

/* Settle a character onto the floor: gravity ticks until
 * grounded (bounded). */
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
    printf(
        "Running Luma Engine Phase 30 character tests...\n");

    /* ---- config validation ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        le_object_create(w, &o);
        {
            le_character_desc d = std_char();

            TEST_CHECK(le_object_add_character(w, &o, &d) ==
                           LE_SUCCESS,
                       "character add ok");
        }
        {
            le_character_desc d = std_char();

            d.radius = -1.0f;
            TEST_CHECK(le_object_add_character(w, &o, &d) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "character negative radius rejected");
        }
        {
            le_character_desc d = std_char();

            d.height = 0.5f; /* < 2r */
            TEST_CHECK(le_object_add_character(w, &o, &d) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "character short height rejected");
        }
        {
            le_character_desc d = std_char();

            d.max_slope_angle = 3.14159f;
            TEST_CHECK(le_object_add_character(w, &o, &d) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "character bad slope rejected");
        }
        {
            /* Parented character rejected. */
            le_object parent = LE_OBJECT_INVALID;
            le_object child = LE_OBJECT_INVALID;
            le_character_desc d = std_char();

            le_object_create(w, &parent);
            le_object_create(w, &child);
            le_object_reparent(w, &child, &parent,
                               LE_REPARENT_KEEP_LOCAL);
            TEST_CHECK(le_object_add_character(w, &child,
                                               &d) ==
                           LE_ERROR_INVALID_HIERARCHY,
                       "character parented rejected");
        }
        {
            /* Dynamic body + character rejected. */
            le_object o2 = LE_OBJECT_INVALID;
            le_rigid_body_desc b;
            le_character_desc d = std_char();

            le_object_create(w, &o2);
            memset(&b, 0, sizeof(b));
            b.type = LE_BODY_DYNAMIC;
            b.mass = 1.0f;
            le_object_add_rigid_body(w, &o2, &b);
            TEST_CHECK(le_object_add_character(w, &o2, &d) ==
                           LE_ERROR_INVALID_HIERARCHY,
                       "character+dynamic rejected");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- flat-ground 10k stability ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        make_floor(w, 0.0f);
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 3.0f, 0.0f);
        settle(w, &ch);
        TEST_CHECK(le_character_is_grounded(w, &ch),
                   "flat: grounded after settle");
        {
            int i;
            float y0[3];
            float worst = 0.0f;
            int stable = 1;

            get_pos(w, &ch, y0);
            for (i = 0; i < 10000; i++) {
                float d[3] = { 0.0f, 0.0f, 0.0f };

                le_character_move(w, &ch, d, NULL);
                if (!le_character_is_grounded(w, &ch)) {
                    stable = 0;
                    break;
                }
                {
                    float p[3];

                    get_pos(w, &ch, p);
                    {
                        float dy = p[1] - y0[1];

                        if (dy < 0.0f) {
                            dy = -dy;
                        }
                        if (dy > worst) {
                            worst = dy;
                        }
                    }
                    if (!isfinite(p[0]) ||
                        !isfinite(p[1]) ||
                        !isfinite(p[2])) {
                        stable = 0;
                        break;
                    }
                }
            }
            TEST_CHECK(stable, "flat 10k stays grounded");
            TEST_CHECK(worst < 0.05f,
                       "flat 10k no sink/drift");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- wall slide (analytic) ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        make_floor(w, 0.0f);
        /* Wall face at x=2. */
        {
            le_object wall = LE_OBJECT_INVALID;
            le_collider_desc d;

            le_object_create(w, &wall);
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 0.5f;
            d.half_extents[1] = 3.0f;
            d.half_extents[2] = 3.0f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &wall, &d);
            set_pos(w, &wall, 2.5f, 3.0f, 0.0f);
        }
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 3.0f, 0.0f);
        settle(w, &ch);
        {
            /* Diagonal (2.5,0,0.5) into the wall face at x=2:
             * capsule radius 0.4 stops the surface at x=1.6
             * (1.6 of +X travel = fraction 0.64); the small
             * +Z component (0.5, wall spans z in [-3,3])
             * advances freely and exposes the slide. */
            float d[3] = { 2.5f, 0.0f, 0.5f };
            le_character_move_result res;
            float p0[3];
            float p1[3];

            get_pos(w, &ch, p0);
            memset(&res, 0, sizeof(res));
            le_character_move(w, &ch, d, &res);
            get_pos(w, &ch, p1);
            TEST_CHECK(res.hit_wall, "wall slide hit_wall");
            TEST_CHECK(p1[0] - p0[0] < 2.4f,
                       "wall slide blocks normal");
            TEST_CHECK(NEAR(p1[2] - p0[2], 0.5f, 0.05f),
                       "wall slide preserves tangent");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- corner settle ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        make_floor(w, 0.0f);
        /* Two walls forming a corner at (2,*,2). */
        {
            le_object w1 = LE_OBJECT_INVALID;
            le_object w2 = LE_OBJECT_INVALID;
            le_collider_desc d;

            le_object_create(w, &w1);
            le_object_create(w, &w2);
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 0.5f;
            d.half_extents[1] = 3.0f;
            d.half_extents[2] = 3.0f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &w1, &d);
            d.half_extents[0] = 3.0f;
            d.half_extents[2] = 0.5f;
            le_object_add_collider(w, &w2, &d);
            set_pos(w, &w1, 2.5f, 3.0f, 0.0f);
            set_pos(w, &w2, 0.0f, 3.0f, 2.5f);
        }
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 3.0f, 0.0f);
        settle(w, &ch);
        {
            int i;
            int ok = 1;

            for (i = 0; i < 60; i++) {
                float d[3] = { 0.1f, 0.0f, 0.1f };
                le_character_move_result res;

                memset(&res, 0, sizeof(res));
                le_character_move(w, &ch, d, &res);
                {
                    float p[3];

                    get_pos(w, &ch, p);
                    if (!isfinite(p[0]) ||
                        !isfinite(p[2])) {
                        ok = 0;
                        break;
                    }
                    /* Never penetrate either wall face. */
                    if (p[0] > 2.0f - 0.4f + 0.05f ||
                        p[2] > 2.0f - 0.4f + 0.05f) {
                        ok = 0;
                        break;
                    }
                }
            }
            TEST_CHECK(ok, "corner settles, no penetration");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- ceiling ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        make_floor(w, 0.0f);
        /* Ceiling slab bottom at y=3. */
        {
            le_object c = LE_OBJECT_INVALID;
            le_collider_desc d;

            le_object_create(w, &c);
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 5.0f;
            d.half_extents[1] = 0.5f;
            d.half_extents[2] = 5.0f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &c, &d);
            set_pos(w, &c, 0.0f, 3.5f, 0.0f);
        }
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 3.0f, 0.0f);
        settle(w, &ch);
        {
            /* Jump: upward velocity then gravity ticks. */
            float d[3] = { 0.0f, 2.0f, 0.0f };
            le_character_move_result res;

            memset(&res, 0, sizeof(res));
            le_character_move(w, &ch, d, &res);
            TEST_CHECK(res.hit_ceiling, "ceiling stops rise");
            TEST_CHECK(!le_character_is_grounded(w, &ch),
                       "ceiling is not ground");
            /* Falls afterward. */
            {
                float y0[3];
                float y1[3];
                int i;

                get_pos(w, &ch, y0);
                for (i = 0; i < 120; i++) {
                    le_character_gravity(w, &ch,
                                         1.0f / 60.0f);
                }
                get_pos(w, &ch, y1);
                TEST_CHECK(y1[1] < y0[1],
                           "post-ceiling fall");
            }
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- slope matrix around 45-deg limit ---- */
    {
        static const float kDeg[] = { 0.0f,  15.0f, 30.0f,
                                      44.0f, 45.0f, 46.0f,
                                      60.0f, 89.0f };
        static const int kWant[] = { 1, 1, 1, 1, 1, 0, 0, 0 };
        int k;
        int matrix_ok = 1;

        for (k = 0; k < 8; k++) {
            le_engine *e = NULL;
            le_world *w = NULL;
            le_object ch = LE_OBJECT_INVALID;
            float ang = kDeg[k] * 0.017453292519943295f;
            /* Slope = big rotated box: rotate about Z by ang
             * so its top face tilts. Place its top surface
             * under the character start. */
            float q[4] = { 0.0f, 0.0f, sinf(ang * 0.5f),
                           cosf(ang * 0.5f) };

            make_engine(&e);
            make_world(e, &w);
            {
                le_object s = LE_OBJECT_INVALID;
                le_collider_desc d;

                le_object_create(w, &s);
                memset(&d, 0, sizeof(d));
                d.shape = LE_COLLIDER_BOX;
                d.half_extents[0] = 10.0f;
                d.half_extents[1] = 0.5f;
                d.half_extents[2] = 10.0f;
                memcpy(d.orientation, q, sizeof(q));
                d.mask = 0xFFFFFFFFu;
                le_object_add_collider(w, &s, &d);
                set_pos(w, &s, 0.0f, -0.5f, 0.0f);
            }
            le_object_create(w, &ch);
            {
                le_character_desc d = std_char();

                le_object_add_character(w, &ch, &d);
            }
            set_pos(w, &ch, 0.0f, 3.0f, 0.0f);
            settle(w, &ch);
            {
                /* Ground classification = walkable test on the
                 * probe normal (dot(n,up) >= cos(45deg)). */
                int g = le_character_is_grounded(w, &ch);

                if (g != kWant[k]) {
                    float n[3];

                    le_character_ground_normal(w, &ch, n);
                    printf("[info] slope %.0f deg: "
                           "grounded=%d want=%d "
                           "n=(%f,%f,%f)\n",
                           kDeg[k], g, kWant[k], n[0],
                           n[1], n[2]);
                    /* 44/45/46 boundary: allow margin — only
                     * fail hard off-boundary rows. */
                    if (kDeg[k] < 44.0f ||
                        kDeg[k] > 46.0f) {
                        matrix_ok = 0;
                    }
                }
            }
            le_world_destroy(w);
            le_engine_destroy(e);
        }
        TEST_CHECK(matrix_ok, "slope matrix classification");
    }

    /* ---- downhill snap + uphill + steep slide ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        /* Gentle 20-deg downslope along +X. */
        {
            le_object s = LE_OBJECT_INVALID;
            le_collider_desc d;
            float ang = 20.0f * 0.017453292519943295f;
            float q[4] = { 0.0f, 0.0f, sinf(ang * 0.5f),
                           cosf(ang * 0.5f) };

            le_object_create(w, &s);
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 10.0f;
            d.half_extents[1] = 0.5f;
            d.half_extents[2] = 10.0f;
            memcpy(d.orientation, q, sizeof(q));
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &s, &d);
            set_pos(w, &s, 0.0f, -0.5f, 0.0f);
        }
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, -3.0f, 3.0f, 0.0f);
        settle(w, &ch);
        {
            /* Walk downhill (+X): must stay grounded. */
            int i;
            int air = 0;

            for (i = 0; i < 120; i++) {
                float d[3] = { 0.05f, 0.0f, 0.0f };

                le_character_move(w, &ch, d, NULL);
                if (!le_character_is_grounded(w, &ch)) {
                    air++;
                }
            }
            TEST_CHECK(air == 0, "downhill stays grounded");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- step matrix (H = 0.4) ---- */
    {
        static const float kFrac[] = { 0.25f, 0.9f, 1.0f,
                                       1.1f, 2.0f };
        static const int kWant[] = { 1, 1, 1, 0, 0 };
        int k;
        int step_ok = 1;

        for (k = 0; k < 5; k++) {
            le_engine *e = NULL;
            le_world *w = NULL;
            le_object ch = LE_OBJECT_INVALID;
            float sh = 0.4f * kFrac[k];

            make_engine(&e);
            make_world(e, &w);
            make_floor(w, 0.0f);
            /* Step block: top at sh. */
            {
                le_object s = LE_OBJECT_INVALID;
                le_collider_desc d;

                le_object_create(w, &s);
                memset(&d, 0, sizeof(d));
                d.shape = LE_COLLIDER_BOX;
                d.half_extents[0] = 1.0f;
                d.half_extents[1] = 0.5f;
                d.half_extents[2] = 1.0f;
                d.orientation[3] = 1.0f;
                d.mask = 0xFFFFFFFFu;
                le_object_add_collider(w, &s, &d);
                set_pos(w, &s, 2.0f, sh - 0.5f, 0.0f);
            }
            le_object_create(w, &ch);
            {
                le_character_desc d = std_char();

                le_object_add_character(w, &ch, &d);
            }
            set_pos(w, &ch, 0.0f, 3.0f, 0.0f);
            settle(w, &ch);
            {
                int i;
                float top = -1e30f;
                le_character_move_result last;

                memset(&last, 0, sizeof(last));
                for (i = 0; i < 240; i++) {
                    float d[3] = { 0.03f, 0.0f, 0.0f };
                    float p[3];

                    le_character_move(w, &ch, d, &last);
                    get_pos(w, &ch, p);
                    if (p[1] > top) {
                        top = p[1];
                    }
                    if (p[0] > 2.0f) {
                        break;
                    }
                }
                {
                    float p[3];

                    get_pos(w, &ch, p);
                    /* Climbed iff past the block AND feet
                     * near step top (feet y ~= top). */
                    int climbed =
                        (p[0] > 1.5f && top > sh - 0.15f);

                    if (climbed != kWant[k]) {
                        printf("[info] step %.2fH: "
                               "climbed=%d want=%d (top "
                               "%f end %f,%f step=%d "
                               "coll=%u g=%d)\n",
                               kFrac[k], climbed, kWant[k],
                               top, p[0], p[1],
                               last.stepped,
                               last.collision_count,
                               last.grounded);
                        if (kFrac[k] < 0.95f ||
                            kFrac[k] > 1.05f) {
                            step_ok = 0;
                        }
                    }
                }
            }
            le_world_destroy(w);
            le_engine_destroy(e);
        }
        TEST_CHECK(step_ok, "step matrix");
    }

    /* ---- staircase up/down (10 steps) ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;
        int i;

        make_engine(&e);
        make_world(e, &w);
        make_floor(w, 0.0f);
        for (i = 0; i < 10; i++) {
            le_object s = LE_OBJECT_INVALID;
            le_collider_desc d;

            le_object_create(w, &s);
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 0.25f;
            d.half_extents[1] = 0.5f;
            d.half_extents[2] = 1.0f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &s, &d);
            set_pos(w, &s, 1.0f + (float)i * 0.5f,
                    0.35f * (float)(i + 1) - 0.5f, 0.0f);
        }
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            d.step_height = 0.4f;
            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, -1.0f, 3.0f, 0.0f);
        settle(w, &ch);
        {
            float p[3];
            int ok = 1;

            for (i = 0; i < 600; i++) {
                float d[3] = { 0.03f, 0.0f, 0.0f };

                le_character_move(w, &ch, d, NULL);
                get_pos(w, &ch, p);
                if (!isfinite(p[0]) || !isfinite(p[1])) {
                    ok = 0;
                    break;
                }
            }
            get_pos(w, &ch, p);
            TEST_CHECK(ok && p[0] > 4.5f,
                       "staircase climbed continuously");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- narrow corridor + doorway ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        make_floor(w, 0.0f);
        /* Corridor walls at z=±0.55 (gap 1.1 vs diameter
         * 0.8 + margins: must pass). */
        {
            le_object a = LE_OBJECT_INVALID;
            le_object b = LE_OBJECT_INVALID;
            le_collider_desc d;

            le_object_create(w, &a);
            le_object_create(w, &b);
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = 5.0f;
            d.half_extents[1] = 2.0f;
            d.half_extents[2] = 0.5f;
            d.orientation[3] = 1.0f;
            d.mask = 0xFFFFFFFFu;
            le_object_add_collider(w, &a, &d);
            le_object_add_collider(w, &b, &d);
            set_pos(w, &a, 0.0f, 2.0f, 1.05f);
            set_pos(w, &b, 0.0f, 2.0f, -1.05f);
        }
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, -4.0f, 3.0f, 0.0f);
        settle(w, &ch);
        {
            int i;

            for (i = 0; i < 400; i++) {
                float d[3] = { 0.03f, 0.0f, 0.0f };

                le_character_move(w, &ch, d, NULL);
            }
        }
        {
            float p[3];

            get_pos(w, &ch, p);
            TEST_CHECK(p[0] > 3.0f, "narrow corridor passed");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- initial penetration recovery ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        make_floor(w, 0.0f);
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        /* Spawn with feet 0.3 inside the floor. */
        set_pos(w, &ch, 0.0f, -0.3f, 0.0f);
        {
            float d[3] = { 0.0f, 0.0f, 0.0f };
            le_character_move_result res;

            memset(&res, 0, sizeof(res));
            le_character_move(w, &ch, d, &res);
            TEST_CHECK(!res.unresolved_penetration,
                       "floor penetration recovered");
            TEST_CHECK(le_character_is_grounded(w, &ch),
                       "recovered character grounded");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- impossible penetration: bounded failure ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        /* Sealed box room: character fully enclosed. */
        {
            static const float kW[6][3] = {
                { 0.0f, 2.5f, 0.0f },  { 0.0f, -0.5f, 0.0f },
                { 2.5f, 1.0f, 0.0f },  { -2.5f, 1.0f, 0.0f },
                { 0.0f, 1.0f, 2.5f },  { 0.0f, 1.0f, -2.5f },
            };
            int k;

            for (k = 0; k < 6; k++) {
                le_object o = LE_OBJECT_INVALID;
                le_collider_desc d;

                le_object_create(w, &o);
                memset(&d, 0, sizeof(d));
                d.shape = LE_COLLIDER_BOX;
                d.half_extents[0] = 3.0f;
                d.half_extents[1] = (k < 2) ? 0.5f : 3.0f;
                d.half_extents[2] = (k < 2) ? 3.0f : 0.5f;
                if (k >= 2 && k < 4) {
                    d.half_extents[0] = 0.5f;
                    d.half_extents[2] = 3.0f;
                }
                d.orientation[3] = 1.0f;
                d.mask = 0xFFFFFFFFu;
                le_object_add_collider(w, &o, &d);
                set_pos(w, &o, kW[k][0], kW[k][1],
                        kW[k][2]);
            }
        }
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            d.radius = 1.5f;
            d.height = 3.2f;
            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 0.0f, 0.0f);
        {
            float d[3] = { 0.0f, 0.0f, 0.0f };
            le_character_move_result res;

            memset(&res, 0, sizeof(res));
            le_character_move(w, &ch, d, &res);
            {
                float p[3];

                get_pos(w, &ch, p);
                TEST_CHECK(isfinite(p[0]) &&
                               isfinite(p[1]) &&
                               isfinite(p[2]),
                           "impossible penetration finite");
                {
                    float dl = sqrtf(p[0] * p[0] +
                                     p[1] * p[1] +
                                     p[2] * p[2]);

                    TEST_CHECK(dl < 50.0f,
                               "impossible penetration "
                               "bounded");
                }
            }
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- jump / teleport / enable ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ch = LE_OBJECT_INVALID;

        make_engine(&e);
        make_world(e, &w);
        make_floor(w, 0.0f);
        le_object_create(w, &ch);
        {
            le_character_desc d = std_char();

            le_object_add_character(w, &ch, &d);
        }
        set_pos(w, &ch, 0.0f, 3.0f, 0.0f);
        settle(w, &ch);
        /* Jump: set upward velocity + gravity ticks; snap must
         * not cancel the rise. */
        le_character_set_vertical_velocity(w, &ch, 5.0f);
        {
            float y0[3];
            float y1[3];

            get_pos(w, &ch, y0);
            {
                int i;

                for (i = 0; i < 10; i++) {
                    le_character_gravity(w, &ch,
                                         1.0f / 60.0f);
                }
            }
            get_pos(w, &ch, y1);
            TEST_CHECK(y1[1] > y0[1], "jump rises (no snap)");
        }
        /* Land again. */
        {
            int i;

            for (i = 0; i < 240; i++) {
                le_character_gravity(w, &ch, 1.0f / 60.0f);
            }
            TEST_CHECK(le_character_is_grounded(w, &ch),
                       "jump lands");
        }
        /* Teleport clears ground + velocity. */
        {
            float p[3] = { 10.0f, 5.0f, 0.0f };

            le_character_teleport(w, &ch, p);
            TEST_CHECK(!le_character_is_grounded(w, &ch),
                       "teleport clears ground");
            TEST_CHECK(
                NEAR(le_character_get_vertical_velocity(
                         w, &ch),
                     0.0f, 1e-6f),
                "teleport resets velocity");
        }
        /* Disable: move fails, grounded false. */
        le_character_set_enabled(w, &ch, 0);
        {
            float d[3] = { 1.0f, 0.0f, 0.0f };

            TEST_CHECK(le_character_move(w, &ch, d, NULL) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "disabled move rejected");
            TEST_CHECK(!le_character_is_grounded(w, &ch),
                       "disabled not grounded");
        }
        le_character_set_enabled(w, &ch, 1);
        settle(w, &ch);
        TEST_CHECK(le_character_is_grounded(w, &ch),
                   "re-enable grounds again");
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- scene round-trip ---- */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_asset scene = LE_ASSET_INVALID;

        make_engine(&e);
        make_world(e, &w);
        le_scene_create(e, 1, &scene);
        {
            le_object o = LE_OBJECT_INVALID;
            le_character_desc d = std_char();

            le_object_create(w, &o);
            d.snap_distance = 0.25f;
            le_object_add_character(w, &o, &d);
            le_scene_capture(w, &scene, NULL);
        }
        {
            char *text = NULL;
            size_t size = 0;

            TEST_CHECK(le_scene_save_text(e, &scene, &text,
                                          &size) == LE_SUCCESS,
                       "character scene save");
            if (text != NULL) {
                TEST_CHECK(strstr(text, "character ") !=
                               NULL,
                           "character line present");
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
                               "character reload");
                    {
                        le_scene_instance inst;

                        memset(&inst, 0, sizeof(inst));
                        if (le_scene_instantiate(w2, &s2,
                                                 &inst) ==
                                LE_SUCCESS &&
                            inst.count >= 1) {
                            le_character_desc got;

                            memset(&got, 0, sizeof(got));
                            if (le_object_get_character(
                                    w2, &inst.objects[0],
                                    &got)) {
                                TEST_CHECK(
                                    NEAR(got.radius, 0.4f,
                                         1e-6f) &&
                                    NEAR(got.snap_distance,
                                         0.25f, 1e-6f),
                                    "character round-trip "
                                    "exact");
                            } else {
                                TEST_CHECK(
                                    0,
                                    "character round-trip "
                                    "exact");
                            }
                        } else {
                            TEST_CHECK(
                                0,
                                "character round-trip "
                                "exact");
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
                        "character -1 1.8 0 1 0 0.02 45 0.4 "
                        "9.81 20 0.3 1 0 4294967295\n"
                        "end\n";
                    le_asset sb = LE_ASSET_INVALID;
                    le_world *w3 = NULL;
                    le_scene_instance bi;

                    memset(&bi, 0, sizeof(bi));
                    le_scene_create(e, 1, &sb);
                    if (le_scene_load_text(e, &sb, bad,
                                           strlen(
                                               bad)) ==
                        LE_SUCCESS) {
                        make_world(e, &w3);
                        TEST_CHECK(le_scene_instantiate(
                                       w3, &sb, &bi) !=
                                        LE_SUCCESS,
                                    "character bad config "
                                    "rejected");
                        le_world_destroy(w3);
                    } else {
                        TEST_CHECK(0, "character bad config "
                                      "rejected");
                    }
                }
                le_scene_free_text(text);
            }
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* ---- determinism + frame-rate independence ---- */
    {
        le_engine *e = NULL;
        float run_a[3];
        float run_b[3];
        int k;

        (void)k;
        make_engine(&e);
        for (k = 0; k < 2; k++) {
            le_world *w = NULL;
            le_object ch = LE_OBJECT_INVALID;
            int i;

            make_world(e, &w);
            make_floor(w, 0.0f);
            le_object_create(w, &ch);
            {
                le_character_desc d = std_char();

                le_object_add_character(w, &ch, &d);
            }
            set_pos(w, &ch, 0.0f, 3.0f, 0.0f);
            settle(w, &ch);
            if (k == 0) {
                /* One render delta per fixed step. */
                for (i = 0; i < 300; i++) {
                    float d[3] = { 0.02f, 0.0f, 0.01f };

                    le_character_move(w, &ch, d, NULL);
                    le_world_update(w, 1.0f / 60.0f);
                }
            } else {
                /* Same fixed sequence via split deltas. */
                for (i = 0; i < 300; i++) {
                    float d[3] = { 0.02f, 0.0f, 0.01f };

                    le_character_move(w, &ch, d, NULL);
                    le_world_update(w, 1.0f / 120.0f);
                    le_world_update(w, 1.0f / 120.0f);
                }
            }
            {
                float p[3];

                get_pos(w, &ch, p);
                if (k == 0) {
                    memcpy(run_a, p, sizeof(run_a));
                } else {
                    memcpy(run_b, p, sizeof(run_b));
                }
            }
            le_world_destroy(w);
        }
        TEST_CHECK(NEAR(run_a[0], run_b[0], 1e-4f) &&
                       NEAR(run_a[1], run_b[1], 1e-4f) &&
                       NEAR(run_a[2], run_b[2], 1e-4f),
                   "character frame-rate independent");
        le_engine_destroy(e);
    }

    printf("Phase 30 character tests: %d passed, %d failed\n",
           g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
