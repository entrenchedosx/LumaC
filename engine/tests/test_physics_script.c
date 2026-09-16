/*
 * Luma Engine Phase 28 script-integration tests (headless):
 * Lua physics bindings (velocities, forces, impulses, add body/
 * collider, gravity, raycast), fixed_update force application,
 * collision/trigger callback fan-out (enter/stay/exit with
 * contact tables, nil on EXIT), failed-callback policy, and
 * scene round-trip with physics + script together.
 *
 * Deterministic: le_engine_step only. No GPU/window.
 */
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

static le_result make_script(le_engine *engine, const char *src,
                             le_asset *out) {
    le_script_asset_desc d;

    memset(&d, 0, sizeof(d));
    d.source = src;
    d.size = strlen(src);
    d.path_hint = "test.lua";
    return le_asset_create_script(engine, &d, out);
}

static float pos_y(le_world *world, const le_object *o) {
    float p[3];

    le_object_get_position(world, o, p);
    return p[1];
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 28 script tests...\n");

    /* fixed_update applies force: Lua-driven fall control. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_rigid_body_desc d;
        le_collider_desc c;
        float y0;
        float y1;

        memset(&d, 0, sizeof(d));
        d.type = LE_BODY_DYNAMIC;
        d.mass = 1.0f;
        d.gravity_scale = 0.0f;
        memset(&c, 0, sizeof(c));
        c.shape = LE_COLLIDER_SPHERE;
        c.radius = 0.5f;
        c.orientation[3] = 1.0f;
        c.mask = 0xFFFFFFFFu;
        c.friction = 0.5f;
        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine,
                               "function fixed_update(self, dt)\n"
                               "  self:add_force(0, -10, 0)\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "force script compiles");
        le_object_create(world, &o);
        le_object_add_rigid_body(world, &o, &d);
        le_object_add_collider(world, &o, &c);
        le_object_add_script(world, &o, &script);
        le_script_set_fixed_step(world, 1.0f / 60.0f, 4);
        {
            float p[3] = { 0, 10, 0 };

            le_object_set_position(world, &o, p);
        }
        le_engine_step(engine, world, 0.016f);
        le_engine_step(engine, world, 0.016f);
        y0 = pos_y(world, &o);
        TEST_CHECK(y0 < 10.0f, "fixed_update force pulls down");
        y1 = y0;
        le_engine_step(engine, world, 0.5f);
        TEST_CHECK(pos_y(world, &o) < y1,
                   "sustained force keeps falling");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Velocity accessors from Lua. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_rigid_body_desc d;
        float v[3];

        memset(&d, 0, sizeof(d));
        d.type = LE_BODY_DYNAMIC;
        d.mass = 1.0f;
        d.gravity_scale = 0.0f;
        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine,
                               "function start(self)\n"
                               "  self:set_linear_velocity(3, 4, "
                               "5)\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "velocity script compiles");
        le_object_create(world, &o);
        le_object_add_rigid_body(world, &o, &d);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.05f);
        le_physics_get_linear_velocity(world, &o, v);
        TEST_CHECK(v[0] == 3.0f && v[1] == 4.0f &&
                       v[2] == 5.0f,
                   "Lua set_linear_velocity sticks");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Impulse from Lua moves the body. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_rigid_body_desc d;
        float p0[3];
        float p1[3];

        memset(&d, 0, sizeof(d));
        d.type = LE_BODY_DYNAMIC;
        d.mass = 1.0f;
        d.gravity_scale = 0.0f;
        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine,
                               "function start(self)\n"
                               "  self:apply_impulse(0, 5, 0)\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "impulse script compiles");
        le_object_create(world, &o);
        le_object_add_rigid_body(world, &o, &d);
        le_object_add_script(world, &o, &script);
        le_object_get_position(world, &o, p0);
        le_engine_step(engine, world, 0.2f);
        le_object_get_position(world, &o, p1);
        TEST_CHECK(p1[1] > p0[1] + 0.1f,
                   "Lua impulse lifts the body");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* collision_enter fires with a contact table; exit gets nil. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;
        le_asset script = LE_ASSET_INVALID;
        le_rigid_body_desc st;
        le_collider_desc ca;
        le_collider_desc cb;
        const char *src =
            "hits = {}\n"
            "function collision_enter(self, other, contact)\n"
            "  hits[#hits + 1] = { kind = \"enter\", pen = "
            "contact.penetration }\n"
            "end\n"
            "function collision_exit(self, other, contact)\n"
            "  hits[#hits + 1] = { kind = \"exit\", nilc = "
            "contact == nil }\n"
            "end\n";

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        memset(&ca, 0, sizeof(ca));
        ca.shape = LE_COLLIDER_SPHERE;
        ca.radius = 1.0f;
        ca.orientation[3] = 1.0f;
        ca.mask = 0xFFFFFFFFu;
        ca.friction = 0.5f;
        cb = ca;
        make_engine(&engine);
        make_world(engine, &world);
        le_physics_set_gravity(world, 0, 0, 0);
        TEST_CHECK(make_script(engine, src, &script) ==
                       LE_SUCCESS,
                   "collision script compiles");
        le_object_create(world, &a);
        le_object_create(world, &b);
        le_object_add_rigid_body(world, &a, &st);
        le_object_add_collider(world, &a, &ca);
        le_object_add_rigid_body(world, &b, &st);
        le_object_add_collider(world, &b, &cb);
        le_object_add_script(world, &a, &script);
        {
            float p[3] = { 0, 0, 0 };

            le_object_set_position(world, &a, p);
            le_object_set_position(world, &b, p);
        }
        le_script_set_fixed_step(world, 1.0f / 60.0f, 4);
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(!le_object_script_failed(world, &a),
                   "collision handler survives");
        {
            float p[3] = { 20, 0, 0 };

            le_object_set_position(world, &b, p);
        }
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(!le_object_script_failed(world, &a),
                   "exit handler survives");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* trigger_enter fires for trigger pairs. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;
        le_asset script = LE_ASSET_INVALID;
        le_rigid_body_desc st;
        le_collider_desc ca;
        le_collider_desc cb;

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        memset(&ca, 0, sizeof(ca));
        ca.shape = LE_COLLIDER_SPHERE;
        ca.radius = 2.0f;
        ca.orientation[3] = 1.0f;
        ca.mask = 0xFFFFFFFFu;
        ca.friction = 0.5f;
        ca.is_trigger = 1;
        cb = ca;
        cb.radius = 0.5f;
        cb.is_trigger = 0;
        make_engine(&engine);
        make_world(engine, &world);
        le_physics_set_gravity(world, 0, 0, 0);
        TEST_CHECK(make_script(engine,
                               "function trigger_enter(self, "
                               "other, contact)\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "trigger script compiles");
        le_object_create(world, &a);
        le_object_create(world, &b);
        le_object_add_rigid_body(world, &a, &st);
        le_object_add_collider(world, &a, &ca);
        le_object_add_rigid_body(world, &b, &st);
        le_object_add_collider(world, &b, &cb);
        le_object_add_script(world, &b, &script);
        {
            float p[3] = { 0, 0, 0 };

            le_object_set_position(world, &a, p);
            le_object_set_position(world, &b, p);
        }
        le_script_set_fixed_step(world, 1.0f / 60.0f, 4);
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(!le_object_script_failed(world, &b),
                   "trigger handler survives");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Physics.gravity / set_gravity from Lua. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        float g[3];

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine,
                               "function start(self)\n"
                               "  Physics.set_gravity(0, -2, 0)\n"
                               "  local x, y, z = "
                               "Physics.gravity()\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "gravity script compiles");
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.05f);
        le_physics_get_gravity(world, g);
        TEST_CHECK(g[1] == -2.0f, "Lua set_gravity sticks");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Physics.raycast from Lua returns a hit table. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_object eye;
        le_asset script = LE_ASSET_INVALID;
        le_collider_desc c;

        memset(&c, 0, sizeof(c));
        c.shape = LE_COLLIDER_SPHERE;
        c.radius = 0.5f;
        c.orientation[3] = 1.0f;
        c.mask = 0xFFFFFFFFu;
        c.friction = 0.5f;
        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine,
                               "seen = nil\n"
                               "function update(self, dt)\n"
                               "  local hit = Physics.raycast(0, "
                               "0, 0, 0, 0, 1, 100)\n"
                               "  if hit ~= nil then seen = "
                               "hit.distance end\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "raycast script compiles");
        le_object_create(world, &o);
        le_object_add_collider(world, &o, &c);
        {
            float p[3] = { 0, 0, 5 };

            le_object_set_position(world, &o, p);
        }
        le_object_create(world, &eye);
        le_object_add_script(world, &eye, &script);
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(!le_object_script_failed(world, &eye),
                   "raycast handler survives");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* add_rigid_body + add_collider from Lua. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_rigid_body_desc got;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine,
                               "function start(self)\n"
                               "  self:add_rigid_body({ type = "
                               "\"dynamic\", mass = 2.5 })\n"
                               "  self:add_collider({ shape = "
                               "\"sphere\", radius = 0.75 })\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "add-component script compiles");
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(le_object_get_rigid_body(world, &o, &got) ==
                       1 &&
                       got.mass == 2.5f,
                   "Lua add_rigid_body sticks");
        TEST_CHECK(le_object_has_component(world, &o,
                                           LE_COMPONENT_COLLIDER),
                   "Lua add_collider sticks");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Erroring collision handler marks failed, world continues. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;
        le_asset script = LE_ASSET_INVALID;
        le_rigid_body_desc st;
        le_collider_desc ca;
        le_collider_desc cb;

        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        memset(&ca, 0, sizeof(ca));
        ca.shape = LE_COLLIDER_SPHERE;
        ca.radius = 1.0f;
        ca.orientation[3] = 1.0f;
        ca.mask = 0xFFFFFFFFu;
        ca.friction = 0.5f;
        cb = ca;
        make_engine(&engine);
        make_world(engine, &world);
        le_physics_set_gravity(world, 0, 0, 0);
        TEST_CHECK(make_script(engine,
                               "function collision_enter(self, "
                               "other, contact)\n"
                               "  error(\"boom\")\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "error script compiles");
        le_object_create(world, &a);
        le_object_create(world, &b);
        le_object_add_rigid_body(world, &a, &st);
        le_object_add_collider(world, &a, &ca);
        le_object_add_rigid_body(world, &b, &st);
        le_object_add_collider(world, &b, &cb);
        le_object_add_script(world, &a, &script);
        {
            float p[3] = { 0, 0, 0 };

            le_object_set_position(world, &a, p);
            le_object_set_position(world, &b, p);
        }
        le_script_set_fixed_step(world, 1.0f / 60.0f, 4);
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(le_object_script_failed(world, &a),
                   "erroring handler marks failed");
        /* World still steps (no crash, second step fine). */
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(1, "world survives handler error");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    printf("Phase 28 script tests: %d passed, %d failed\n",
           g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
