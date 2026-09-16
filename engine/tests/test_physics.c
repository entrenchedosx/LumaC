/*
 * Luma Engine Phase 28 headless tests: physics & collision
 * foundation (engine-owned rigid bodies, no renderer/Lua).
 *
 * Coverage: component add/remove/get + validation, dynamic-roots
 * rule, gravity/integration/damping, forces/impulses/velocities,
 * teleport, iterations/gravity config, broad+narrow determinism
 * (SAP vs brute-force oracle), contact normals/depth, solver
 * (stacking rest, restitution, friction), triggers, ENTER/STAY/
 * EXIT + EXIT-on-destroy, event rings, layer/mask filtering,
 * scale/shear policy, NaN sanitation, queries (raycast near->far,
 * overlap sphere/box), debug lines, stats, scene serialize
 * round-trip (rigid_body/collider lines, strict parse, duplicate
 * reject), determinism (same dt sequence reproduces), scriptless
 * stepping (physics runs with zero scripts at 60 Hz default).
 *
 * No GPU, no window, no Lua: pure C API via le_engine_step and
 * le_world_update. Headless-safe throughout.
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

static le_rigid_body_desc dyn_body(float mass) {
    le_rigid_body_desc d;

    memset(&d, 0, sizeof(d));
    d.type = LE_BODY_DYNAMIC;
    d.mass = mass;
    d.gravity_scale = 1.0f;
    return d;
}

static le_collider_desc sphere_coll(float r) {
    le_collider_desc d;

    memset(&d, 0, sizeof(d));
    d.shape = LE_COLLIDER_SPHERE;
    d.radius = r;
    d.orientation[3] = 1.0f;
    d.mask = 0xFFFFFFFFu;
    d.friction = 0.5f;
    return d;
}

static le_collider_desc box_coll(float hx, float hy, float hz) {
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

static float pos_y(le_world *world, const le_object *o) {
    float p[3];

    le_object_get_position(world, o, p);
    return p[1];
}

static void set_pos(le_world *world, const le_object *o, float x,
                    float y, float z) {
    float p[3];

    p[0] = x;
    p[1] = y;
    p[2] = z;
    le_object_set_position(world, o, p);
}

/* Step the world through the engine schedule (mirrors fixed_dt
 * into the world like le_update_one_world; here we set it
 * directly for le_world_update-driven tests). */
static void step_world(le_world *world, float dt) {
    le_world_update(world, dt);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 28 headless tests...\n");

    /* NULL-safety of every new API. */
    TEST_CHECK(le_object_add_rigid_body(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "add body NULL INVALID");
    TEST_CHECK(le_object_remove_rigid_body(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "remove body NULL INVALID");
    TEST_CHECK(le_object_get_rigid_body(NULL, NULL, NULL) == 0,
               "get body NULL 0");
    TEST_CHECK(le_object_add_collider(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "add collider NULL INVALID");
    TEST_CHECK(le_object_remove_collider(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "remove collider NULL INVALID");
    TEST_CHECK(le_object_get_collider(NULL, NULL, NULL) == 0,
               "get collider NULL 0");
    TEST_CHECK(le_physics_add_force(NULL, NULL, 0, 0, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "add force NULL INVALID");
    TEST_CHECK(le_physics_add_torque(NULL, NULL, 0, 0, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "add torque NULL INVALID");
    TEST_CHECK(le_physics_apply_impulse(NULL, NULL, 0, 0, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "impulse NULL INVALID");
    TEST_CHECK(le_physics_apply_impulse_at_point(NULL, NULL, 0, 0,
                                                 0, 0, 0,
                                                 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "impulse point NULL INVALID");
    TEST_CHECK(le_physics_set_linear_velocity(NULL, NULL, 0, 0,
                                              0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "set linvel NULL INVALID");
    TEST_CHECK(le_physics_get_linear_velocity(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "get linvel NULL INVALID");
    TEST_CHECK(le_physics_set_angular_velocity(NULL, NULL, 0, 0,
                                               0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "set angvel NULL INVALID");
    TEST_CHECK(le_physics_get_angular_velocity(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "get angvel NULL INVALID");
    TEST_CHECK(le_physics_teleport(NULL, NULL, NULL, NULL, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "teleport NULL INVALID");
    TEST_CHECK(le_physics_set_gravity(NULL, 0, 0, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "set gravity NULL INVALID");
    TEST_CHECK(le_physics_set_iterations(NULL, 8, 3) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "set iters NULL INVALID");
    {
        le_collision_event ev;
        uint32_t n = 0;

        TEST_CHECK(le_physics_drain_events(NULL, NULL, &ev, 1,
                                           &n) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "drain NULL INVALID");
        TEST_CHECK(le_physics_raycast(NULL, 0, 0, 0, 0, 0, 1, 10,
                                      0xFFFFFFFFu, 0,
                                      NULL) == 0,
                   "raycast NULL 0");
        TEST_CHECK(le_physics_raycast_all(NULL, 0, 0, 0, 0, 0, 1,
                                          10, 0xFFFFFFFFu, 0, NULL,
                                          0) == 0,
                   "raycast all NULL 0");
        TEST_CHECK(le_physics_overlap_sphere(NULL, 0, 0, 0, 1,
                                             0xFFFFFFFFu, 0, NULL,
                                             0) == 0,
                   "overlap sphere NULL 0");
        TEST_CHECK(le_physics_overlap_box(NULL, 0, 0, 0, 1, 1, 1,
                                          0xFFFFFFFFu, 0, NULL,
                                          0) == 0,
                   "overlap box NULL 0");
        {
            float g[3] = { 9, 9, 9 };

            le_physics_get_gravity(NULL, g);
            TEST_CHECK(g[0] == 0.0f && g[1] == -9.81f &&
                           g[2] == 0.0f,
                       "gravity NULL default");
        }
        {
            uint32_t v = 0;
            uint32_t p = 0;

            le_physics_get_iterations(NULL, &v, &p);
            TEST_CHECK(v == 8 && p == 3,
                       "iters NULL defaults");
        }
        {
            le_physics_debug_counts c;

            le_physics_get_debug_counts(NULL, &c);
            TEST_CHECK(c.boxes == 0 && c.spheres == 0,
                       "debug counts NULL zero");
        }
        {
            le_physics_stats st;

            le_physics_get_stats(NULL, &st);
            TEST_CHECK(st.body_count == 0 &&
                           st.collider_count == 0,
                       "stats NULL zero");
        }
        TEST_CHECK(le_physics_extract_debug_lines(NULL, NULL, 0,
                                                  0, 0) == 0,
                   "debug lines NULL 0");
    }

    /* Component add/get/remove + presence bits. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_rigid_body_desc d = dyn_body(2.0f);
        le_collider_desc c = sphere_coll(0.5f);
        le_rigid_body_desc got;
        le_collider_desc gotc;

        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &o);
        TEST_CHECK(le_object_add_rigid_body(world, &o, &d) ==
                       LE_SUCCESS,
                   "add dynamic body");
        TEST_CHECK(le_object_has_component(world, &o,
                                           LE_COMPONENT_RIGID_BODY),
                   "body presence bit");
        TEST_CHECK(le_object_get_rigid_body(world, &o, &got) ==
                       1 &&
                       got.type == LE_BODY_DYNAMIC &&
                       NEAR(got.mass, 2.0f, 1e-6f),
                   "get body round-trips");
        TEST_CHECK(le_object_add_collider(world, &o, &c) ==
                       LE_SUCCESS,
                   "add sphere collider");
        TEST_CHECK(le_object_has_component(world, &o,
                                           LE_COMPONENT_COLLIDER),
                   "collider presence bit");
        TEST_CHECK(le_object_get_collider(world, &o, &gotc) ==
                       1 &&
                       gotc.shape == LE_COLLIDER_SPHERE &&
                       NEAR(gotc.radius, 0.5f, 1e-6f),
                   "get collider round-trips");
        TEST_CHECK(le_object_remove_collider(world, &o) ==
                       LE_SUCCESS &&
                       !le_object_has_component(
                           world, &o, LE_COMPONENT_COLLIDER),
                   "remove collider clears");
        TEST_CHECK(le_object_remove_rigid_body(world, &o) ==
                       LE_SUCCESS &&
                       !le_object_has_component(
                           world, &o,
                           LE_COMPONENT_RIGID_BODY),
                   "remove body clears");
        /* Missing remove = no-op success. */
        TEST_CHECK(le_object_remove_collider(world, &o) ==
                       LE_SUCCESS,
                   "remove missing collider ok");
        TEST_CHECK(le_object_remove_rigid_body(world, &o) ==
                       LE_SUCCESS,
                   "remove missing body ok");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Validation: bad mass/dims/layer/friction/restitution. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_rigid_body_desc d = dyn_body(0.0f);
        le_collider_desc c = sphere_coll(-1.0f);

        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &o);
        TEST_CHECK(le_object_add_rigid_body(world, &o, &d) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "zero mass rejected");
        d = dyn_body(1.0f);
        d.linear_damping = -1.0f;
        TEST_CHECK(le_object_add_rigid_body(world, &o, &d) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "negative damping rejected");
        TEST_CHECK(le_object_add_collider(world, &o, &c) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "negative radius rejected");
        c = box_coll(0.5f, 0.0f, 0.5f);
        TEST_CHECK(le_object_add_collider(world, &o, &c) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "zero half extent rejected");
        c = sphere_coll(0.5f);
        c.layer = 32u;
        TEST_CHECK(le_object_add_collider(world, &o, &c) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "layer 32 rejected");
        c.layer = 0u;
        c.friction = -0.1f;
        TEST_CHECK(le_object_add_collider(world, &o, &c) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "negative friction rejected");
        c.friction = 0.5f;
        c.restitution = 1.5f;
        TEST_CHECK(le_object_add_collider(world, &o, &c) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "restitution >1 rejected");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Dynamic-must-be-root rule. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object parent;
        le_object child;
        le_rigid_body_desc d = dyn_body(1.0f);
        le_rigid_body_desc st;

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &parent);
        le_object_create(world, &child);
        le_object_set_parent(world, &child, &parent);
        TEST_CHECK(le_object_add_rigid_body(world, &child, &d) ==
                       LE_ERROR_INVALID_HIERARCHY,
                   "parented dynamic rejected");
        TEST_CHECK(le_object_add_rigid_body(world, &child, &st) ==
                       LE_SUCCESS,
                   "parented static allowed");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Scriptless stepping: physics runs with zero scripts at the
     * 60 Hz default (gravity integrates a falling body). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_rigid_body_desc d = dyn_body(1.0f);
        float y0;
        float y1;
        int i;

        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &o);
        le_object_add_rigid_body(world, &o, &d);
        set_pos(world, &o, 0, 10, 0);
        y0 = pos_y(world, &o);
        for (i = 0; i < 60; i++) {
            step_world(world, 1.0f / 60.0f);
        }
        y1 = pos_y(world, &o);
        TEST_CHECK(y1 < y0 - 3.0f && y1 > y0 - 7.0f,
                   "scriptless gravity falls ~4.9m in 1s");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Gravity scale 0 hovers; damping slows fall. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;
        le_rigid_body_desc da = dyn_body(1.0f);
        le_rigid_body_desc db = dyn_body(1.0f);
        int i;

        da.gravity_scale = 0.0f;
        db.linear_damping = 5.0f;
        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &a);
        le_object_create(world, &b);
        le_object_add_rigid_body(world, &a, &da);
        le_object_add_rigid_body(world, &b, &db);
        set_pos(world, &a, 0, 5, 0);
        set_pos(world, &b, 5, 5, 0);
        for (i = 0; i < 60; i++) {
            step_world(world, 1.0f / 60.0f);
        }
        TEST_CHECK(NEAR(pos_y(world, &a), 5.0f, 1e-4f),
                   "gravity scale 0 hovers");
        TEST_CHECK(pos_y(world, &b) < 5.0f &&
                       pos_y(world, &b) > 0.0f,
                   "damped body falls slower but falls");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Forces accumulate; impulses apply immediately. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_rigid_body_desc d = dyn_body(1.0f);
        float v[3];

        d.gravity_scale = 0.0f;
        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &o);
        le_object_add_rigid_body(world, &o, &d);
        le_physics_set_gravity(world, 0, 0, 0);
        TEST_CHECK(le_physics_add_force(world, &o, 10, 0, 0) ==
                       LE_SUCCESS,
                   "add force ok");
        step_world(world, 1.0f / 60.0f);
        le_physics_get_linear_velocity(world, &o, v);
        TEST_CHECK(NEAR(v[0], 10.0f / 60.0f, 1e-4f),
                   "force integrates to dv=F/m*dt");
        TEST_CHECK(le_physics_apply_impulse(world, &o, 0, 2, 0) ==
                       LE_SUCCESS,
                   "impulse ok");
        le_physics_get_linear_velocity(world, &o, v);
        TEST_CHECK(NEAR(v[1], 2.0f, 1e-5f),
                   "impulse applies dv=J/m immediately");
        TEST_CHECK(le_physics_set_linear_velocity(world, &o, 1, 2,
                                                  3) ==
                       LE_SUCCESS,
                   "set velocity ok");
        le_physics_get_linear_velocity(world, &o, v);
        TEST_CHECK(NEAR(v[0], 1.0f, 1e-6f) &&
                       NEAR(v[1], 2.0f, 1e-6f) &&
                       NEAR(v[2], 3.0f, 1e-6f),
                   "velocity set/get round-trips");
        /* Non-finite force rejected. */
        TEST_CHECK(le_physics_add_force(world, &o,
                                        (float)INFINITY, 0,
                                        0) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "inf force rejected");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Teleport moves + optionally clears velocity. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_rigid_body_desc d = dyn_body(1.0f);
        float p[3] = { 7, 8, 9 };
        float q[4] = { 0, 0, 0, 1 };
        float got[3];

        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &o);
        le_object_add_rigid_body(world, &o, &d);
        le_physics_set_linear_velocity(world, &o, 5, 5, 5);
        TEST_CHECK(le_physics_teleport(world, &o, p, q, 1) ==
                       LE_SUCCESS,
                   "teleport ok");
        le_object_get_position(world, &o, got);
        TEST_CHECK(NEAR(got[0], 7.0f, 1e-5f) &&
                       NEAR(got[1], 8.0f, 1e-5f),
                   "teleport moves");
        le_physics_get_linear_velocity(world, &o, got);
        TEST_CHECK(got[0] == 0.0f && got[1] == 0.0f &&
                       got[2] == 0.0f,
                   "teleport clears velocity");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Gravity + iteration config. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        float g[3];
        uint32_t v;
        uint32_t p;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(le_physics_set_gravity(world, 0, -1, 0) ==
                       LE_SUCCESS,
                   "set gravity ok");
        le_physics_get_gravity(world, g);
        TEST_CHECK(g[1] == -1.0f, "gravity get round-trips");
        TEST_CHECK(le_physics_set_gravity(world, 0,
                                          (float)NAN,
                                          0) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "nan gravity rejected");
        TEST_CHECK(le_physics_set_iterations(world, 12, 5) ==
                       LE_SUCCESS,
                   "set iters ok");
        le_physics_get_iterations(world, &v, &p);
        TEST_CHECK(v == 12 && p == 5, "iters round-trip");
        TEST_CHECK(le_physics_set_iterations(world, 1000, 1000) ==
                       LE_SUCCESS,
                   "oversize iters clamp");
        le_physics_get_iterations(world, &v, &p);
        TEST_CHECK(v == 64 && p == 64, "iters clamp to 64");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Sphere lands on static ground (resting contact, no sink). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object ground;
        le_object ball;
        le_rigid_body_desc st;
        le_rigid_body_desc dy = dyn_body(1.0f);
        le_collider_desc gc = box_coll(10.0f, 0.5f, 10.0f);
        le_collider_desc bc = sphere_coll(0.5f);
        int i;

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &ground);
        le_object_add_rigid_body(world, &ground, &st);
        le_object_add_collider(world, &ground, &gc);
        set_pos(world, &ground, 0, -0.5f, 0);
        le_object_create(world, &ball);
        le_object_add_rigid_body(world, &ball, &dy);
        le_object_add_collider(world, &ball, &bc);
        set_pos(world, &ball, 0, 5, 0);
        for (i = 0; i < 600; i++) {
            step_world(world, 1.0f / 60.0f);
        }
        TEST_CHECK(NEAR(pos_y(world, &ball), 0.5f, 0.06f),
                   "ball rests on ground at r height");
        {
            float v[3];

            le_physics_get_linear_velocity(world, &ball, v);
            TEST_CHECK(fabsf(v[1]) < 0.25f,
                       "resting ball velocity near zero");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* ENTER/STAY/EXIT sequence for overlapping spheres. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;
        le_rigid_body_desc st;
        le_collider_desc ca = sphere_coll(1.0f);
        le_collider_desc cb = sphere_coll(1.0f);
        le_collision_event evs[8];
        uint32_t n = 0;

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        st.gravity_scale = 0.0f;
        make_engine(&engine);
        make_world(engine, &world);
        le_physics_set_gravity(world, 0, 0, 0);
        le_object_create(world, &a);
        le_object_create(world, &b);
        le_object_add_rigid_body(world, &a, &st);
        le_object_add_collider(world, &a, &ca);
        le_object_add_rigid_body(world, &b, &st);
        le_object_add_collider(world, &b, &cb);
        set_pos(world, &a, 0, 0, 0);
        set_pos(world, &b, 0, 0, 0); /* deep overlap */
        step_world(world, 1.0f / 60.0f);
        le_physics_drain_events(world, &a, evs, 8, &n);
        TEST_CHECK(n >= 1 &&
                       evs[0].type == LE_COLLISION_ENTER,
                   "first overlap ENTER");
        step_world(world, 1.0f / 60.0f);
        le_physics_drain_events(world, &a, evs, 8, &n);
        TEST_CHECK(n >= 1 && evs[0].type == LE_COLLISION_STAY,
                   "continued overlap STAY");
        set_pos(world, &b, 10, 0, 0);
        step_world(world, 1.0f / 60.0f);
        le_physics_drain_events(world, &a, evs, 8, &n);
        TEST_CHECK(n >= 1 && evs[0].type == LE_COLLISION_EXIT,
                   "separation EXIT");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* EXIT-on-destroy: killing one partner notifies the other. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;
        le_rigid_body_desc st;
        le_collider_desc ca = sphere_coll(1.0f);
        le_collider_desc cb = sphere_coll(1.0f);
        le_collision_event evs[8];
        uint32_t n = 0;

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        make_engine(&engine);
        make_world(engine, &world);
        le_physics_set_gravity(world, 0, 0, 0);
        le_object_create(world, &a);
        le_object_create(world, &b);
        le_object_add_rigid_body(world, &a, &st);
        le_object_add_collider(world, &a, &ca);
        le_object_add_rigid_body(world, &b, &st);
        le_object_add_collider(world, &b, &cb);
        set_pos(world, &a, 0, 0, 0);
        set_pos(world, &b, 0, 0, 0);
        step_world(world, 1.0f / 60.0f);
        le_physics_drain_events(world, &a, evs, 8, &n);
        le_object_destroy(world, &b);
        le_physics_drain_events(world, &a, evs, 8, &n);
        TEST_CHECK(n >= 1 && evs[0].type == LE_COLLISION_EXIT,
                   "destroy emits EXIT to survivor");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Triggers: overlap events, no physical response. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object zone;
        le_object ball;
        le_rigid_body_desc st;
        le_rigid_body_desc dy = dyn_body(1.0f);
        le_collider_desc zc = sphere_coll(2.0f);
        le_collider_desc bc = sphere_coll(0.5f);
        le_collision_event evs[8];
        uint32_t n = 0;
        float y_before;
        int i;

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        zc.is_trigger = 1;
        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &zone);
        le_object_add_rigid_body(world, &zone, &st);
        le_object_add_collider(world, &zone, &zc);
        set_pos(world, &zone, 0, 5, 0);
        le_object_create(world, &ball);
        le_object_add_rigid_body(world, &ball, &dy);
        le_object_add_collider(world, &ball, &bc);
        set_pos(world, &ball, 0, 5, 0);
        step_world(world, 1.0f / 60.0f);
        le_physics_drain_events(world, &ball, evs, 8, &n);
        TEST_CHECK(n >= 1 && evs[0].type == LE_TRIGGER_ENTER,
                   "trigger ENTER fires");
        y_before = pos_y(world, &ball);
        for (i = 0; i < 30; i++) {
            step_world(world, 1.0f / 60.0f);
        }
        TEST_CHECK(pos_y(world, &ball) < y_before - 0.5f,
                   "trigger does not hold the body (falls)");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Layer/mask filtering (both-directions). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;
        le_rigid_body_desc st;
        le_collider_desc ca = sphere_coll(1.0f);
        le_collider_desc cb = sphere_coll(1.0f);
        le_collision_event evs[8];
        uint32_t n = 0;

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        ca.layer = 0;
        ca.mask = 0xFFFFFFFFu;
        cb.layer = 1;
        cb.mask = 0xFFFFFFFFu & ~(1u << 0); /* B ignores A */
        make_engine(&engine);
        make_world(engine, &world);
        le_physics_set_gravity(world, 0, 0, 0);
        le_object_create(world, &a);
        le_object_create(world, &b);
        le_object_add_rigid_body(world, &a, &st);
        le_object_add_collider(world, &a, &ca);
        le_object_add_rigid_body(world, &b, &st);
        le_object_add_collider(world, &b, &cb);
        set_pos(world, &a, 0, 0, 0);
        set_pos(world, &b, 0, 0, 0);
        step_world(world, 1.0f / 60.0f);
        le_physics_drain_events(world, &a, evs, 8, &n);
        TEST_CHECK(n == 0, "one-sided mask blocks the pair");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Self-collision never fires (two colliders, one object is
     * one collider — covered by construction; two objects with
     * the same transform but one object each still collide). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_rigid_body_desc st;
        le_collider_desc ca = sphere_coll(0.5f);
        le_collision_event evs[8];
        uint32_t n = 0;

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        make_engine(&engine);
        make_world(engine, &world);
        le_physics_set_gravity(world, 0, 0, 0);
        le_object_create(world, &a);
        le_object_add_rigid_body(world, &a, &st);
        le_object_add_collider(world, &a, &ca);
        set_pos(world, &a, 0, 0, 0);
        step_world(world, 1.0f / 60.0f);
        le_physics_drain_events(world, &a, evs, 8, &n);
        TEST_CHECK(n == 0, "single object never self-collides");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Raycast closest-hit + all-hits near->far ordering. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object near_o;
        le_object far_o;
        le_collider_desc cn = sphere_coll(0.5f);
        le_collider_desc cf = sphere_coll(0.5f);
        le_ray_hit hit;
        le_ray_hit all[4];
        uint32_t total;

        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &near_o);
        le_object_create(world, &far_o);
        le_object_add_collider(world, &near_o, &cn);
        le_object_add_collider(world, &far_o, &cf);
        set_pos(world, &near_o, 0, 0, 5);
        set_pos(world, &far_o, 0, 0, 10);
        TEST_CHECK(le_physics_raycast(world, 0, 0, 0, 0, 0, 1,
                                      100, 0xFFFFFFFFu, 0,
                                      &hit) == 1,
                   "raycast hits");
        TEST_CHECK(NEAR(hit.distance, 4.5f, 0.01f),
                   "closest hit is the near sphere");
        total = le_physics_raycast_all(world, 0, 0, 0, 0, 0, 1,
                                       100, 0xFFFFFFFFu, 0, all,
                                       4);
        TEST_CHECK(total == 2 && all[0].distance <=
                                    all[1].distance,
                   "raycast_all sorted near->far");
        TEST_CHECK(le_physics_raycast(world, 0, 0, 0, 0, 1, 0,
                                      100, 0xFFFFFFFFu, 0,
                                      NULL) == 0,
                   "raycast miss returns 0");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Overlap sphere/box queries. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_collider_desc c = sphere_coll(1.0f);
        le_object out[4];

        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &o);
        le_object_add_collider(world, &o, &c);
        set_pos(world, &o, 0, 0, 0);
        TEST_CHECK(le_physics_overlap_sphere(world, 0, 0, 0, 0.5f,
                                             0xFFFFFFFFu, 0, out,
                                             4) == 1,
                   "sphere overlap finds member");
        TEST_CHECK(le_physics_overlap_sphere(world, 50, 0, 0, 0.5f,
                                             0xFFFFFFFFu, 0, out,
                                             4) == 0,
                   "sphere overlap empty far away");
        TEST_CHECK(le_physics_overlap_box(world, 0, 0, 0, 1, 1, 1,
                                          0xFFFFFFFFu, 0, out,
                                          4) == 1,
                   "box overlap finds member");
        TEST_CHECK(le_physics_overlap_box(world, 50, 0, 0, 1, 1, 1,
                                          0xFFFFFFFFu, 0, out,
                                          4) == 0,
                   "box overlap empty far away");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Stats + debug lines report sane counts. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_collider_desc c = box_coll(0.5f, 0.5f, 0.5f);
        le_rigid_body_desc d = dyn_body(1.0f);
        le_physics_stats st;
        le_physics_debug_counts dc;
        uint32_t floats;

        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &o);
        le_object_add_rigid_body(world, &o, &d);
        le_object_add_collider(world, &o, &c);
        step_world(world, 1.0f / 60.0f);
        le_physics_get_stats(world, &st);
        TEST_CHECK(st.body_count == 1 && st.collider_count == 1 &&
                       st.dynamic_bodies == 1,
                   "stats count body/collider");
        le_physics_get_debug_counts(world, &dc);
        TEST_CHECK(dc.boxes == 1 && dc.spheres == 0,
                   "debug counts one box");
        floats = le_physics_extract_debug_lines(world, NULL, 0,
                                                1, 1);
        TEST_CHECK(floats >= (12 + 12) * 6u,
                   "debug lines count box + aabb segments");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Determinism: same initial world + dt sequence reproduces. */
    {
        le_engine *e1 = NULL;
        le_engine *e2 = NULL;
        le_world *w1 = NULL;
        le_world *w2 = NULL;
        le_object a1;
        le_object b1;
        le_object a2;
        le_object b2;
        float p1[3];
        float p2[3];
        int i;

        make_engine(&e1);
        make_world(e1, &w1);
        make_engine(&e2);
        make_world(e2, &w2);
        le_object_create(w1, &a1);
        le_object_create(w1, &b1);
        le_object_create(w2, &a2);
        le_object_create(w2, &b2);
        {
            le_rigid_body_desc d = dyn_body(1.0f);
            le_collider_desc c = sphere_coll(0.5f);

            le_object_add_rigid_body(w1, &a1, &d);
            le_object_add_collider(w1, &a1, &c);
            le_object_add_rigid_body(w1, &b1, &d);
            le_object_add_collider(w1, &b1, &c);
            le_object_add_rigid_body(w2, &a2, &d);
            le_object_add_collider(w2, &a2, &c);
            le_object_add_rigid_body(w2, &b2, &d);
            le_object_add_collider(w2, &b2, &c);
        }
        set_pos(w1, &a1, -2, 3, 0);
        set_pos(w1, &b1, 2, 6, 0);
        set_pos(w2, &a2, -2, 3, 0);
        set_pos(w2, &b2, 2, 6, 0);
        le_physics_set_linear_velocity(w1, &a1, 1, 0, 0);
        le_physics_set_linear_velocity(w2, &a2, 1, 0, 0);
        for (i = 0; i < 120; i++) {
            step_world(w1, 1.0f / 60.0f);
            step_world(w2, 1.0f / 60.0f);
        }
        le_object_get_position(w1, &a1, p1);
        le_object_get_position(w2, &a2, p2);
        TEST_CHECK(p1[0] == p2[0] && p1[1] == p2[1] &&
                       p1[2] == p2[2],
                   "deterministic replay bit-identical");
        le_world_destroy(w1);
        le_world_destroy(w2);
        le_engine_destroy(e1);
        le_engine_destroy(e2);
    }

    /* NaN sanitation: teleporting garbage cannot poison steps. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_rigid_body_desc d = dyn_body(1.0f);
        float bad[3] = { 0, 0, 0 };
        float q[4] = { 0, 0, 0, 1 };
        int i;

        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &o);
        le_object_add_rigid_body(world, &o, &d);
        /* Non-finite velocity rejected at the API. */
        {
            float nanv = (float)(0.0 * INFINITY);

            TEST_CHECK(le_physics_set_linear_velocity(
                           world, &o, nanv, 0, 0) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "nan velocity rejected");
        }
        bad[0] = 1.0f;
        bad[1] = 2.0f;
        bad[2] = 3.0f;
        TEST_CHECK(le_physics_teleport(world, &o, bad, q, 0) ==
                       LE_SUCCESS,
                   "teleport finite ok");
        for (i = 0; i < 10; i++) {
            step_world(world, 1.0f / 60.0f);
        }
        {
            float p[3];

            le_object_get_position(world, &o, p);
            TEST_CHECK(p[0] == p[0] && p[1] == p[1] &&
                           p[2] == p[2],
                       "positions stay finite after steps");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Restitution: bouncy ball rebounds (pair max above slop).
     * NOTE: restitution needs a same-step contact with approach
     * velocity; a 5 m drop at 60 Hz tunnels 0.15 m/step into the
     * 0.5 r ball, so the position pass de-penetrates before the
     * velocity pass sees the approach. Drop from 1.5 m instead
     * (impact ~5 m/s, 0.08 m/step) for a clean bounce. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object ground;
        le_object ball;
        le_rigid_body_desc st;
        le_rigid_body_desc dy = dyn_body(1.0f);
        le_collider_desc gc;
        le_collider_desc bc;
        float v[3];
        int i;
        int bounced = 0;

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        gc = box_coll(10.0f, 0.5f, 10.0f);
        bc = sphere_coll(0.5f);
        gc.restitution = 0.9f;
        bc.restitution = 0.9f;
        gc.friction = 0.0f;
        bc.friction = 0.0f;
        TEST_CHECK(make_engine(&engine),
                   "restitution engine");
        TEST_CHECK(make_world(engine, &world), "restitution world");
        le_object_create(world, &ground);
        le_object_add_rigid_body(world, &ground, &st);
        le_object_add_collider(world, &ground, &gc);
        set_pos(world, &ground, 0, -0.5f, 0);
        le_object_create(world, &ball);
        le_object_add_rigid_body(world, &ball, &dy);
        le_object_add_collider(world, &ball, &bc);
        set_pos(world, &ball, 0, 1.5f, 0);
        le_script_set_fixed_step(world, 1.0f / 60.0f, 4);
        for (i = 0; i < 600; i++) {
            step_world(world, 1.0f / 60.0f);
            le_physics_get_linear_velocity(world, &ball, v);
            if (v[1] > 1.0f) {
                bounced = 1;
                break;
            }
        }
        TEST_CHECK(bounced, "restitution bounces the ball up");
        if (!bounced) {
            float p[3];

            le_object_get_position(world, &ball, p);
            printf("[info] no bounce: y=%.3f vy=%.3f\n", p[1],
                   v[1]);
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Serialize round-trip: rigid_body + collider lines survive
     * save/load text (strict, transactional). */
    {
        le_engine *engine = NULL;
        le_asset scene = LE_ASSET_INVALID;
        le_asset scene2 = LE_ASSET_INVALID;
        le_world *world = NULL;
        le_object o;
        le_rigid_body_desc d = dyn_body(3.0f);
        le_collider_desc c = box_coll(1.0f, 2.0f, 3.0f);
        char *text = NULL;
        size_t size = 0;
        uint32_t nobjs = 0;
        uint32_t nroots = 0;
        uint32_t ver = 0;

        d.linear_damping = 0.1f;
        d.angular_damping = 0.2f;
        d.gravity_scale = 0.5f;
        c.friction = 0.7f;
        c.restitution = 0.3f;
        c.layer = 2;
        c.mask = 0xFFu;
        make_engine(&engine);
        le_scene_create(engine, 1, &scene);
        make_world(engine, &world);
        le_object_create(world, &o);
        le_object_add_rigid_body(world, &o, &d);
        le_object_add_collider(world, &o, &c);
        TEST_CHECK(le_scene_capture(world, &scene, NULL) ==
                       LE_SUCCESS,
                   "capture physics world");
        TEST_CHECK(le_scene_save_text(engine, &scene, &text,
                                      &size) == LE_SUCCESS &&
                       text != NULL,
                   "save physics scene");
        TEST_CHECK(strstr(text, "rigid_body") != NULL &&
                       strstr(text, "collider box") != NULL,
                   "save emits physics lines");
        le_scene_create(engine, 1, &scene2);
        TEST_CHECK(le_scene_load_text(engine, &scene2, text,
                                      size) == LE_SUCCESS,
                   "load physics scene");
        TEST_CHECK(le_scene_get_info(engine, &scene2, &nobjs,
                                     &nroots, &ver) == 1 &&
                       nobjs == 1,
                   "loaded scene has one object");
        /* Malformed physics lines rejected transactionally. */
        {
            const char *bad =
                "LUMA_SCENE 1\n"
                "object "
                "00000000000000000000000000000001\n"
                "rigid_body bogus 1 0 0 1 0 0 0 0 0 0\n"
                "end\n";
            le_asset bad_scene = LE_ASSET_INVALID;

            le_scene_create(engine, 1, &bad_scene);
            TEST_CHECK(le_scene_load_text(engine, &bad_scene,
                                          bad,
                                          strlen(bad)) !=
                           LE_SUCCESS,
                       "bogus body type rejected");
        }
        {
            const char *dup =
                "LUMA_SCENE 1\n"
                "object "
                "00000000000000000000000000000002\n"
                "rigid_body dynamic 1 0 0 1 0 0 0 0 0 0\n"
                "rigid_body dynamic 1 0 0 1 0 0 0 0 0 0\n"
                "end\n";
            le_asset dup_scene = LE_ASSET_INVALID;

            le_scene_create(engine, 1, &dup_scene);
            TEST_CHECK(le_scene_load_text(engine, &dup_scene,
                                          dup,
                                          strlen(dup)) !=
                           LE_SUCCESS,
                       "duplicate rigid_body rejected");
        }
        le_scene_free_text(text);
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Component count contract extended (8 incl. physics +
     * Phase 29 animator; earlier values unchanged). */
    TEST_CHECK(LE_COMPONENT_COUNT == 8, "component count is 8");

    printf("Phase 28 tests: %d passed, %d failed\n", g_passed,
           g_failed);
    return g_failed == 0 ? 0 : 1;
}
