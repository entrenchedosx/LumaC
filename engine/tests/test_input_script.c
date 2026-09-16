/*
 * Luma Engine Phase 27 script-integration tests (headless):
 * injected input -> Lua Input/Time API -> engine transforms;
 * update dt == Time.delta(); fixed_update dt == Time.fixed_delta();
 * same-frame snapshot consistency across scripts; mutation/error/
 * budget/scene regressions after the lifecycle refactor.
 *
 * Deterministic: injection + le_engine_step only.
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

static float pos_x(le_world *world, const le_object *o) {
    float p[3];

    le_object_get_position(world, o, p);
    return p[0];
}

static le_input_binding key_bind(le_key key) {
    le_input_binding b;

    memset(&b, 0, sizeof(b));
    b.kind = LE_BINDING_KEY;
    b.key = key;
    b.scale = 1.0f;
    return b;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 27 script tests...\n");

    /* Input-driven transform: W down -> Lua moves object via
     * Input.key_down; W up -> movement stops. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        float x0;
        float x1;
        float x2;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine,
                               "function update(self, dt)\n"
                               "  if Input.key_down(Key.W) then\n"
                               "    local x, y, z = "
                               "self:position()\n"
                               "    self:set_position(x + 5.0 * "
                               "dt, y, z)\n"
                               "  end\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "input script compiles");
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        x0 = pos_x(world, &o);
        le_input_inject_key(engine, LE_KEY_W, 1);
        le_engine_step(engine, world, 0.1f);
        x1 = pos_x(world, &o);
        TEST_CHECK(x1 - x0 > 0.49f && x1 - x0 < 0.51f,
                   "W down moves +5*dt");
        le_input_inject_key(engine, LE_KEY_W, 0);
        le_engine_step(engine, world, 0.1f);
        x2 = pos_x(world, &o);
        TEST_CHECK(x2 == x1, "W up stops movement");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Action-driven transform via Input.action_pressed. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_input_action jump = LE_INPUT_ACTION_INVALID;
        le_input_binding b;
        float x0;
        float x1;

        make_engine(&engine);
        make_world(engine, &world);
        le_input_create_action(engine, "jump", &jump);
        b = key_bind(LE_KEY_SPACE);
        le_input_add_action_binding(engine, &jump, &b);
        TEST_CHECK(make_script(engine,
                               "function update(self, dt)\n"
                               "  if Input.action_pressed("
                               "\"jump\") then\n"
                               "    local x, y, z = "
                               "self:position()\n"
                               "    self:set_position(x + 1.0, "
                               "y, z)\n"
                               "  end\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "action script compiles");
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        x0 = pos_x(world, &o);
        le_input_inject_key(engine, LE_KEY_SPACE, 1);
        le_engine_step(engine, world, 0.016f);
        x1 = pos_x(world, &o);
        TEST_CHECK(x1 - x0 == 1.0f, "action press hops +1");
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(pos_x(world, &o) == x1,
                   "held space does not re-hop");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Axis-driven transform via Input.axis. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_input_axis mx = LE_INPUT_AXIS_INVALID;
        le_axis_desc dd;
        le_input_binding b;
        float x0;
        float x1;

        make_engine(&engine);
        make_world(engine, &world);
        memset(&dd, 0, sizeof(dd));
        dd.name = "move_x";
        dd.scale = 1.0f;
        le_input_create_axis(engine, &dd, &mx);
        memset(&b, 0, sizeof(b));
        b.kind = LE_BINDING_KEY;
        b.key = LE_KEY_D;
        b.scale = 1.0f;
        le_input_add_axis_binding(engine, &mx, &b);
        TEST_CHECK(make_script(engine,
                               "function update(self, dt)\n"
                               "  local v = Input.axis("
                               "\"move_x\")\n"
                               "  local x, y, z = "
                               "self:position()\n"
                               "  self:set_position(x + v * dt, "
                               "y, z)\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "axis script compiles");
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        x0 = pos_x(world, &o);
        le_input_inject_key(engine, LE_KEY_D, 1);
        le_engine_step(engine, world, 0.5f);
        x1 = pos_x(world, &o);
        /* max_delta clamps 0.5 -> 0.25: v*dt = 0.25. */
        TEST_CHECK(x1 - x0 > 0.249f && x1 - x0 < 0.251f,
                   "axis drives v*dt (clamped)");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* dt consistency: update(self,dt) == Time.delta(). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_script_property p;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine,
                               "export('got', -1.0)\n"
                               "export('want', -2.0)\n"
                               "function update(self, dt)\n"
                               "  self.got = dt\n"
                               "  self.want = Time.delta()\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "dt probe compiles");
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_time_set_scale(engine, 0.5f);
        le_engine_step(engine, world, 0.04f);
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_NUMBER;
        TEST_CHECK(le_script_get_property(world, &o, "got", &p) &&
                   p.number > 0.019 && p.number < 0.021,
                   "update dt is scaled (0.02)");
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_NUMBER;
        TEST_CHECK(le_script_get_property(world, &o, "want",
                                          &p) &&
                   p.number > 0.019 && p.number < 0.021,
                   "Time.delta matches dt");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* fixed-dt consistency: fixed_update dt == fixed_delta. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_script_property p;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine,
                               "export('fdt', -1.0)\n"
                               "export('fwant', -2.0)\n"
                               "function fixed_update(self, "
                               "dt)\n"
                               "  self.fdt = dt\n"
                               "  self.fwant = "
                               "Time.fixed_delta()\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "fixed probe compiles");
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.05f);
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_NUMBER;
        TEST_CHECK(le_script_get_property(world, &o, "fdt", &p) &&
                   p.number > 0.0166 && p.number < 0.0167,
                   "fixed dt is 1/60");
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_NUMBER;
        TEST_CHECK(le_script_get_property(world, &o, "fwant",
                                          &p) &&
                   p.number > 0.0166 && p.number < 0.0167,
                   "Time.fixed_delta matches");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Snapshot consistency: two scripts in one frame see the
     * same pressed edge (no consumption between scripts). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;
        le_asset sa = LE_ASSET_INVALID;
        le_asset sb = LE_ASSET_INVALID;
        le_script_property p;
        const char *src =
            "export('saw', 0)\n"
            "function update(self, dt)\n"
            "  if Input.key_pressed(Key.Q) then\n"
            "    self.saw = 1\n"
            "  end\n"
            "end\n";

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine, src, &sa);
        make_script(engine, src, &sb);
        le_object_create(world, &a);
        le_object_create(world, &b);
        le_object_add_script(world, &a, &sa);
        le_object_add_script(world, &b, &sb);
        le_input_inject_key(engine, LE_KEY_Q, 1);
        le_engine_step(engine, world, 0.016f);
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_INT;
        TEST_CHECK(le_script_get_property(world, &a, "saw", &p) &&
                   p.integer == 1,
                   "script A saw edge");
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_INT;
        TEST_CHECK(le_script_get_property(world, &b, "saw", &p) &&
                   p.integer == 1,
                   "script B saw same edge");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Pause: update runs with dt=0, fixed stops, render valid. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_script_property p;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine,
                               "export('udt', -1.0)\n"
                               "export('ticks', 0)\n"
                               "function update(self, dt)\n"
                               "  self.udt = dt\n"
                               "end\n"
                               "function fixed_update(self, "
                               "dt)\n"
                               "  self.ticks = self.ticks + 1\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "pause probe compiles");
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_time_set_scale(engine, 0.0f);
        le_engine_step(engine, world, 0.1f);
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_NUMBER;
        TEST_CHECK(le_script_get_property(world, &o, "udt", &p) &&
                   p.number == 0.0,
                   "paused update dt 0");
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_INT;
        TEST_CHECK(le_script_get_property(world, &o, "ticks",
                                          &p) &&
                   p.integer == 0,
                   "paused fixed stops");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Mutation regression after lifecycle refactor. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object spawner;
        le_object victim;
        le_asset script = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine,
                               "function start(self)\n"
                               "  local v = World.find("
                               "\"victim\")\n"
                               "  if v ~= nil then\n"
                               "    World.destroy(v)\n"
                               "  end\n"
                               "  World.create(\"born\")\n"
                               "end\n"
                               "function update(self, dt)\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "mutation script compiles");
        le_object_create(world, &spawner);
        le_object_create(world, &victim);
        le_object_set_name(world, &victim, "victim");
        le_object_add_script(world, &spawner, &script);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(!le_object_is_alive(world, &victim),
                   "victim destroyed by script");
        {
            le_object born = LE_OBJECT_INVALID;

            TEST_CHECK(le_world_find_by_name(world, "born",
                                             &born),
                       "spawned object found");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Error regression: failing script disables, world runs. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object bad;
        le_object good;
        le_asset bads = LE_ASSET_INVALID;
        le_asset goods = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine,
                    "function update(self, dt)\n"
                    "  if Input.key_down(Key.W) then boom() end\n"
                    "end\n",
                    &bads);
        make_script(engine,
                    "function update(self, dt)\n"
                    "  local x, y, z = self:position()\n"
                    "  self:set_position(x + 1.0, y, z)\n"
                    "end\n",
                    &goods);
        le_object_create(world, &bad);
        le_object_create(world, &good);
        le_object_add_script(world, &bad, &bads);
        le_object_add_script(world, &good, &goods);
        le_input_inject_key(engine, LE_KEY_W, 1);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_object_script_failed(world, &bad),
                   "input error instance failed");
        TEST_CHECK(!le_object_script_failed(world, &good),
                   "healthy instance unaffected");
        TEST_CHECK(pos_x(world, &good) == 1.0f,
                   "healthy instance moved");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Runaway budget still enforced after refactor. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_script_config cfg;

        make_engine(&engine);
        make_world(engine, &world);
        memset(&cfg, 0, sizeof(cfg));
        cfg.max_instructions_per_callback = 10000;
        le_script_configure(engine, &cfg);
        make_script(engine,
                    "function update(self, dt)\n"
                    "  local s = 0\n"
                    "  while true do s = s + 1 end\n"
                    "end\n",
                    &script);
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_object_script_failed(world, &o),
                   "runaway budgeted out");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Scene round-trip with script still works via step(). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_world *world2 = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset scene = LE_ASSET_INVALID;
        le_asset scene2 = LE_ASSET_INVALID;
        char *text = NULL;
        size_t size = 0;
        le_scene_instance inst;

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine,
                    "export('speed', 7.0)\n"
                    "function update(self, dt)\n"
                    "  local x, y, z = self:position()\n"
                    "  self:set_position(x + self.speed * dt, "
                    "y, z)\n"
                    "end\n",
                    &script);
        le_object_create(world, &o);
        le_object_set_name(world, &o, "runner");
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.016f);
        le_scene_create(engine, 1, &scene);
        TEST_CHECK(le_scene_capture(world, &scene, NULL) ==
                       LE_SUCCESS,
                   "capture ok");
        TEST_CHECK(le_scene_save_text(engine, &scene, &text,
                                      &size) == LE_SUCCESS &&
                   text != NULL,
                   "save ok");
        le_scene_create(engine, 1, &scene2);
        TEST_CHECK(le_scene_load_text(engine, &scene2, text,
                                      size) == LE_SUCCESS,
                   "re-parse ok");
        le_scene_free_text(text);
        make_world(engine, &world2);
        memset(&inst, 0, sizeof(inst));
        TEST_CHECK(le_scene_instantiate(world2, &scene2,
                                        &inst) == LE_SUCCESS &&
                   inst.count == 1,
                   "instantiate ok");
        le_engine_step(engine, world2, 0.1f);
        le_engine_step(engine, world2, 0.1f);
        TEST_CHECK(pos_x(world2, &inst.objects[0]) > 1.39f,
                   "instantiated script runs (7*0.2)");
        le_scene_instance_free(&inst);
        le_world_destroy(world);
        le_world_destroy(world2);
        le_engine_destroy(engine);
    }

    printf("Luma Engine Phase 27 script tests: %d passed, %d "
           "failed\n",
           g_passed, g_failed);
    if (g_failed == 0) {
        printf("ALL PHASE 27 SCRIPT TESTS PASSED\n");
    } else {
        printf("TESTS FAILED\n");
    }
    return (g_failed == 0) ? 0 : 1;
}
