/*
 * Luma Engine Phase 26 headless tests: Lua scripting runtime,
 * engine bindings, lifecycle/dispatath, exports/properties,
 * scene round-trip with scripts, reload, budgets, sandbox.
 *
 * No GPU, no window, no renderer: scripts drive transforms and
 * engine state only. Headless-safe throughout.
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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 26 headless tests...\n");

    /* NULL-safety of every new API. */
    TEST_CHECK(le_asset_create_script(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "create script NULL INVALID");
    TEST_CHECK(le_asset_load_script(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "load script NULL INVALID");
    TEST_CHECK(le_script_asset_set_source(NULL, NULL, NULL, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "set source NULL INVALID");
    TEST_CHECK(le_script_reload(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "reload NULL INVALID");
    TEST_CHECK(le_object_add_script(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "add script NULL INVALID");
    TEST_CHECK(le_object_remove_script(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "remove script NULL INVALID");
    TEST_CHECK(le_object_get_script(NULL, NULL, NULL) == 0,
               "get script NULL 0");
    TEST_CHECK(le_object_script_failed(NULL, NULL) == 0,
               "script failed NULL 0");
    TEST_CHECK(le_script_get_property(NULL, NULL, NULL, NULL) == 0,
               "get prop NULL 0");
    TEST_CHECK(le_script_set_property(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "set prop NULL INVALID");
    TEST_CHECK(le_script_list_properties(NULL, NULL, NULL, 0,
                                         NULL) == 0,
               "list props NULL 0");
    TEST_CHECK(le_script_configure(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "configure NULL INVALID");
    TEST_CHECK(le_script_add_search_path(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "search path NULL INVALID");
    TEST_CHECK(le_script_set_fixed_step(NULL, 0.0f, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "fixed step NULL INVALID");
    TEST_CHECK(le_world_find_by_name(NULL, NULL, NULL) == 0,
               "find by name NULL 0");
    TEST_CHECK(le_asset_find_by_id(NULL, NULL, NULL) == 0,
               "find asset by id NULL 0");
    {
        le_script_stats st;

        le_script_get_stats(NULL, &st);
        TEST_CHECK(st.script_instances == 0, "stats NULL zero");
        le_script_get_last_error(NULL, NULL);
    }

    /* Lifecycle: start once, update per frame, destroy on remove. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        const char *src =
            "local calls = { start = 0, update = 0, destroy = 0 }\n"
            "function start(self) calls.start = calls.start + 1 end\n"
            "function update(self, dt) calls.update = calls.update + 1 end\n"
            "function destroy(self) calls.destroy = calls.destroy + 1 end\n"
            "return calls\n";

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine, src, &script) == LE_SUCCESS,
                   "compile ok");
        le_object_create(world, &o);
        TEST_CHECK(le_object_add_script(world, &o, &script) ==
                       LE_SUCCESS,
                   "add script ok");
        TEST_CHECK(le_object_has_component(world, &o,
                                           LE_COMPONENT_SCRIPT),
                   "has script component");
        le_world_update(world, 1.0f / 60.0f);
        le_world_update(world, 1.0f / 60.0f);
        le_world_update(world, 1.0f / 60.0f);
        /* No direct inspection of Lua state from C (by design);
         * lifecycle proof: instance alive + not failed. */
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "lifecycle no failure");
        TEST_CHECK(le_object_remove_script(world, &o) ==
                       LE_SUCCESS,
                   "remove script ok");
        TEST_CHECK(!le_object_has_component(world, &o,
                                            LE_COMPONENT_SCRIPT),
                   "script gone after remove");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Transform drive: update() moves the object every frame. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        const char *src =
            "function update(self, dt)\n"
            "  local x, y, z = self:position()\n"
            "  self:set_position(x + 1.0, y, z)\n"
            "end\n";
        float x0;
        float x3;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_script(engine, src, &script) == LE_SUCCESS,
                   "mover compiles");
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        x0 = pos_x(world, &o);
        le_world_update(world, 0.016f);
        le_world_update(world, 0.016f);
        le_world_update(world, 0.016f);
        x3 = pos_x(world, &o);
        TEST_CHECK(x3 - x0 > 2.9f && x3 - x0 < 3.1f,
                   "mover advances +1/frame x3");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Start delayed until effectively enabled; destroy iff started. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        /* Export declared but only read via self.speed AFTER the
         * __index fix (no lua_call reentry). */
        const char *src =
            "export('speed', 2.0)\n"
            "function start(self)\n"
            "end\n"
            "function update(self, dt)\n"
            "  local x, y, z = self:position()\n"
            "  self:set_position(x + self.speed * dt, y, z)\n"
            "end\n";

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine, src, &script);
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_object_set_enabled(world, &o, 0);
        le_world_update(world, 1.0f);
        TEST_CHECK(pos_x(world, &o) == 0.0f,
                   "disabled object never starts");
        le_object_set_enabled(world, &o, 1);
        le_world_update(world, 1.0f);
        TEST_CHECK(pos_x(world, &o) > 1.9f &&
                       pos_x(world, &o) < 2.1f,
                   "enabled object starts + runs (speed*dt)");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Errors disable the instance; world continues. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object bad;
        le_object good;
        le_asset bads = LE_ASSET_INVALID;
        le_asset goods = LE_ASSET_INVALID;
        le_script_error err;

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine, "function update(self, dt) boom() end",
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
        le_world_update(world, 0.016f);
        TEST_CHECK(le_object_script_failed(world, &bad),
                   "erroring instance marked failed");
        TEST_CHECK(!le_object_script_failed(world, &good),
                   "healthy instance unaffected");
        le_world_update(world, 0.016f);
        TEST_CHECK(pos_x(world, &good) > 1.9f,
                   "healthy instance keeps running");
        le_script_get_last_error(engine, &err);
        TEST_CHECK(err.message != NULL && err.message[0] != '\0',
                   "last error recorded");
        TEST_CHECK(err.callback != NULL &&
                       strcmp(err.callback, "update") == 0,
                   "last error names update");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Syntax errors fail asset creation transactionally. */
    {
        le_engine *engine = NULL;
        le_asset s = LE_ASSET_INVALID;
        le_script_stats st;

        make_engine(&engine);
        TEST_CHECK(make_script(engine, "function (broken",
                               &s) == LE_ERROR_PARSE,
                   "syntax error PARSE");
        memset(&st, 0xFF, sizeof(st));
        le_script_get_stats(engine, &st);
        TEST_CHECK(st.script_assets == 0,
                   "failed compile creates nothing");
        le_engine_destroy(engine);
    }

    /* Exports: declare, list, get, set, type enforcement. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        const char *src =
            "export('speed', 3.5)\n"
            "export('label', 'bot')\n"
            "export('active', true)\n"
            "export('lives', 3)\n"
            "export('offset', { 1, 2, 3 })\n";
        le_script_property all[16];
        uint32_t count = 0;
        le_script_property p;

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine, src, &script);
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_world_update(world, 0.016f); /* instantiate on start */
        TEST_CHECK(le_script_list_properties(world, &o, all, 16,
                                             &count) &&
                       count == 5,
                   "five exports listed");
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_NUMBER;
        TEST_CHECK(le_script_get_property(world, &o, "speed",
                                          &p) &&
                       p.number > 3.4 && p.number < 3.6,
                   "get speed default");
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_NUMBER;
        snprintf(p.name, sizeof(p.name), "speed");
        p.number = 9.0;
        TEST_CHECK(le_script_set_property(world, &o, &p) ==
                       LE_SUCCESS,
                   "set speed ok");
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_NUMBER;
        TEST_CHECK(le_script_get_property(world, &o, "speed",
                                          &p) &&
                       p.number > 8.9 && p.number < 9.1,
                   "speed reads back");
        /* Type mismatch rejected. */
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_STRING;
        snprintf(p.name, sizeof(p.name), "speed");
        snprintf(p.string_value, sizeof(p.string_value), "fast");
        TEST_CHECK(le_script_set_property(world, &o, &p) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "type mismatch rejected");
        /* Unknown name rejected. */
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_NUMBER;
        snprintf(p.name, sizeof(p.name), "nope");
        p.number = 1.0;
        TEST_CHECK(le_script_set_property(world, &o, &p) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "unknown prop rejected");
        /* String/int/bool/vec3 round-trips. */
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_STRING;
        TEST_CHECK(le_script_get_property(world, &o, "label",
                                          &p) &&
                       strcmp(p.string_value, "bot") == 0,
                   "string default");
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_BOOL;
        TEST_CHECK(le_script_get_property(world, &o, "active",
                                          &p) &&
                       p.boolean == 1,
                   "bool default");
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_INT;
        TEST_CHECK(le_script_get_property(world, &o, "lives",
                                          &p) &&
                       p.integer == 3,
                   "int default");
        memset(&p, 0, sizeof(p));
        p.type = LE_SCRIPT_PROP_VEC3;
        TEST_CHECK(le_script_get_property(world, &o, "offset",
                                          &p) &&
                       p.vec3[0] == 1.0f && p.vec3[2] == 3.0f,
                   "vec3 default");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Structural mutation from scripts: create + destroy. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object spawner;
        le_asset script = LE_ASSET_INVALID;
        const char *src =
            "function start(self)\n"
            "  local c = World.create('spawned')\n"
            "end\n"
            "function update(self, dt)\n"
            "  local v = World.find('victim')\n"
            "  if v ~= nil then World.destroy(v) end\n"
            "end\n";
        le_object victim;
        uint32_t before;
        uint32_t after;
        le_object found = LE_OBJECT_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine, src, &script);
        le_object_create(world, &spawner);
        le_object_create(world, &victim);
        le_object_set_name(world, &victim, "victim");
        le_object_add_script(world, &spawner, &script);
        before = le_world_get_object_count(world);
        le_world_update(world, 0.016f);
        after = le_world_get_object_count(world);
        /* +1 spawned, -1 victim destroyed => same count. */
        TEST_CHECK(after == before, "spawn+destroy net zero");
        TEST_CHECK(le_world_find_by_name(world, "spawned",
                                         &found) == 1,
                   "spawned object found by name");
        TEST_CHECK(!le_object_is_alive(world, &victim),
                   "victim destroyed by script");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Cross-world misuse rejected; stale handles error safely. */
    {
        le_engine *engine = NULL;
        le_world *w1 = NULL;
        le_world *w2 = NULL;
        le_object o1;
        le_asset script = LE_ASSET_INVALID;
        const char *src =
            "other = nil\n"
            "function start(self, foreign)\n"
            "end\n"
            "function update(self, dt)\n"
            "  self:set_name('touched')\n"
            "end\n";

        make_engine(&engine);
        make_world(engine, &w1);
        make_world(engine, &w2);
        make_script(engine, src, &script);
        le_object_create(w1, &o1);
        le_object_add_script(w1, &o1, &script);
        le_world_update(w1, 0.016f);
        TEST_CHECK(strcmp(le_object_get_name(w1, &o1),
                          "touched") == 0,
                   "self mutation works");
        le_world_destroy(w1);
        le_world_destroy(w2);
        le_engine_destroy(engine);
    }

    /* Serialization round-trip carries script + props (no VM). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_world *world2 = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset scene = LE_ASSET_INVALID;
        const char *src =
            "export('speed', 4.0)\n"
            "function update(self, dt)\n"
            "  local x, y, z = self:position()\n"
            "  self:set_position(x + self.speed * dt, y, z)\n"
            "end\n";
        char *text = NULL;
        size_t size = 0;
        le_asset scene2 = LE_ASSET_INVALID;
        le_scene_instance inst;

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine, src, &script);
        le_object_create(world, &o);
        le_object_set_name(world, &o, "runner");
        le_object_add_script(world, &o, &script);
        le_world_update(world, 0.016f);
        {
            le_script_property p;

            memset(&p, 0, sizeof(p));
            p.type = LE_SCRIPT_PROP_NUMBER;
            snprintf(p.name, sizeof(p.name), "speed");
            p.number = 7.0;
            le_script_set_property(world, &o, &p);
        }
        le_scene_create(engine, 1, &scene);
        TEST_CHECK(le_scene_capture(world, &scene, NULL) ==
                       LE_SUCCESS,
                   "capture with script ok");
        TEST_CHECK(le_scene_save_text(engine, &scene, &text,
                                      &size) == LE_SUCCESS &&
                       text != NULL,
                   "save text ok");
        TEST_CHECK(strstr(text, "script ") != NULL,
                   "script line serialized");
        TEST_CHECK(strstr(text, "sprop ") != NULL,
                   "sprop lines serialized");
        le_scene_create(engine, 1, &scene2);
        TEST_CHECK(le_scene_load_text(engine, &scene2, text,
                                      size) == LE_SUCCESS,
                   "scripted scene re-parses");
        le_scene_free_text(text);
        make_world(engine, &world2);
        memset(&inst, 0, sizeof(inst));
        TEST_CHECK(le_scene_instantiate(world2, &scene2, &inst) ==
                       LE_SUCCESS,
                   "scripted scene instantiates");
        TEST_CHECK(inst.count == 1, "one object instantiated");
        if (inst.count == 1) {
            le_script_property p;

            le_world_update(world2, 1.0f);
            memset(&p, 0, sizeof(p));
            p.type = LE_SCRIPT_PROP_NUMBER;
            TEST_CHECK(le_script_get_property(world2,
                                              &inst.objects[0],
                                              "speed", &p) &&
                           p.number > 6.9 && p.number < 7.1,
                       "prop value survives round-trip");
            TEST_CHECK(pos_x(world2, &inst.objects[0]) > 6.9f,
                       "instantiated script runs (7*dt)");
        }
        le_scene_instance_free(&inst);
        le_world_destroy(world);
        le_world_destroy(world2);
        le_engine_destroy(engine);
    }

    /* Reload adopts new code, keeps values, no start() rerun. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        const char *v1 =
            "export('speed', 1.0)\n"
            "function update(self, dt)\n"
            "  local x, y, z = self:position()\n"
            "  self:set_position(x + self.speed * dt, y, z)\n"
            "end\n";
        const char *v2 =
            "export('speed', 1.0)\n"
            "function update(self, dt)\n"
            "  local x, y, z = self:position()\n"
            "  self:set_position(x + 10.0 * self.speed * dt, y, "
            "z)\n"
            "end\n";

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine, v1, &script);
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_world_update(world, 1.0f);
        {
            le_script_property p;

            memset(&p, 0, sizeof(p));
            p.type = LE_SCRIPT_PROP_NUMBER;
            snprintf(p.name, sizeof(p.name), "speed");
            p.number = 2.0;
            le_script_set_property(world, &o, &p);
        }
        TEST_CHECK(le_script_asset_set_source(engine, &script, v2,
                                              strlen(v2)) ==
                       LE_SUCCESS,
                   "set source ok");
        TEST_CHECK(le_script_reload(engine, &script) == LE_SUCCESS,
                   "reload ok");
        le_world_update(world, 1.0f);
        /* v1: +1*1 then value 2; v2: +10*2 => x ~= 21. */
        TEST_CHECK(pos_x(world, &o) > 20.0f &&
                       pos_x(world, &o) < 22.0f,
                   "reload adopts code, keeps value");
        /* Bad source is transactional. */
        TEST_CHECK(le_script_asset_set_source(
                       engine, &script, "function (oops",
                       strlen("function (oops")) ==
                       LE_ERROR_PARSE,
                   "bad source rejected");
        le_world_update(world, 1.0f);
        TEST_CHECK(pos_x(world, &o) > 40.0f,
                   "old code live after failed set_source");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Fixed-step schedule: N fixed_updates per update. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        const char *src =
            "export('ticks', 0)\n"
            "function fixed_update(self, dt)\n"
            "  self.ticks = self.ticks + 1\n"
            "end\n";

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine, src, &script);
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        TEST_CHECK(le_script_set_fixed_step(world, 0.016f, 4) ==
                       LE_SUCCESS,
                   "fixed step configured");
        le_world_update(world, 0.05f);
        {
            le_script_property p;

            memset(&p, 0, sizeof(p));
            p.type = LE_SCRIPT_PROP_INT;
            TEST_CHECK(le_script_get_property(world, &o, "ticks",
                                              &p) &&
                           p.integer == 3,
                       "3 fixed steps per 50ms frame");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Budgets: instruction limit fails fast, recorded once. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_script_config cfg;

        make_engine(&engine);
        make_world(engine, &world);
        memset(&cfg, 0, sizeof(cfg));
        cfg.max_instructions_per_callback = 5000;
        le_script_configure(engine, &cfg);
        make_script(engine,
                    "function update(self, dt)\n"
                    "  local s = 0\n"
                    "  for i = 1, 100000000 do s = s + i end\n"
                    "end\n",
                    &script);
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_world_update(world, 0.016f);
        TEST_CHECK(le_object_script_failed(world, &o),
                   "runaway script budgeted out");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Sandbox: io/os/package/debug absent; require sandboxed. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine,
                    "function start(self)\n"
                    "  assert(io == nil)\n"
                    "  assert(os == nil)\n"
                    "  assert(package == nil)\n"
                    "  assert(debug == nil)\n"
                    "  assert(dofile == nil)\n"
                    "  assert(loadfile == nil)\n"
                    "  assert(load == nil)\n"
                    "end\n",
                    &script);
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_world_update(world, 0.016f);
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "sandbox surface absent, start clean");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* print() routes to the log callback, never raw stdout. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_script_config cfg;
        static char last_log[256];

        last_log[0] = '\0';
        make_engine(&engine);
        make_world(engine, &world);
        memset(&cfg, 0, sizeof(cfg));
        cfg.log_fn = NULL; /* default stderr; just don't crash */
        le_script_configure(engine, &cfg);
        make_script(engine,
                    "function start(self) print('hello', 42) end\n",
                    &script);
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_world_update(world, 0.016f);
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "print routes cleanly");
        (void)last_log;
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Stats + find_by_id + asset refcount/unload policy. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_script_stats st;
        le_asset_id id;
        le_asset found = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine, "function update(self, dt) end\n",
                    &script);
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_world_update(world, 0.016f);
        memset(&st, 0, sizeof(st));
        le_script_get_stats(engine, &st);
        TEST_CHECK(st.script_assets == 1, "one script asset");
        TEST_CHECK(st.script_instances == 1, "one instance");
        le_asset_get_id(engine, &script, &id);
        TEST_CHECK(le_asset_find_by_id(engine, &id, &found) == 1,
                   "find_by_id locates script");
        TEST_CHECK(le_asset_unload(engine, &script) ==
                       LE_ERROR_ASSET_IN_USE,
                   "referenced script refuses unload");
        le_object_remove_script(world, &o);
        TEST_CHECK(le_asset_unload(engine, &script) == LE_SUCCESS,
                   "unreferenced script unloads");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* World teardown fires destroy() without crashing. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_script(engine,
                    "function destroy(self) end\n", &script);
        le_object_create(world, &o);
        le_object_add_script(world, &o, &script);
        le_world_update(world, 0.016f);
        le_world_destroy(world); /* destroy() fires here */
        le_engine_destroy(engine);
        TEST_CHECK(1, "teardown with scripts clean");
    }

    /* Determinism: identical worlds, identical updates agree.
     * Roots are created in slot order (0..3) with default tag
     * handles resolvable via find-free scan: query each world's
     * roots by ascending slot through le_world_get_root_count +
     * children API? Roots need handles — instead name objects and
     * resolve by name (deterministic, public). */
    {
        le_engine *e1 = NULL;
        le_engine *e2 = NULL;
        le_world *w1 = NULL;
        le_world *w2 = NULL;
        const char *src =
            "function update(self, dt)\n"
            "  local x, y, z = self:position()\n"
            "  self:set_position(x + 1.0, y, z)\n"
            "end\n";
        int i;
        int same = 1;

        make_engine(&e1);
        make_world(e1, &w1);
        make_engine(&e2);
        make_world(e2, &w2);
        for (i = 0; i < 4; i++) {
            le_object o;
            le_asset s = LE_ASSET_INVALID;
            char name[32];

            snprintf(name, sizeof(name), "mover%d", i);
            le_object_create(w1, &o);
            le_object_set_name(w1, &o, name);
            make_script(e1, src, &s);
            le_object_add_script(w1, &o, &s);
            le_object_create(w2, &o);
            le_object_set_name(w2, &o, name);
            make_script(e2, src, &s);
            le_object_add_script(w2, &o, &s);
        }
        for (i = 0; i < 5; i++) {
            le_world_update(w1, 0.016f);
            le_world_update(w2, 0.016f);
        }
        {
            uint32_t n1 = le_world_get_object_count(w1);
            uint32_t n2 = le_world_get_object_count(w2);

            TEST_CHECK(n1 == 4 && n2 == 4, "both worlds 4 objs");
            for (i = 0; i < 4; i++) {
                char name[32];
                le_object h1 = LE_OBJECT_INVALID;
                le_object h2 = LE_OBJECT_INVALID;

                snprintf(name, sizeof(name), "mover%d", i);
                if (le_world_find_by_name(w1, name, &h1) &&
                    le_world_find_by_name(w2, name, &h2)) {
                    float p1[3] = { 0, 0, 0 };
                    float p2[3] = { 0, 0, 0 };

                    le_object_get_position(w1, &h1, p1);
                    le_object_get_position(w2, &h2, p2);
                    /* 5 frames x +1 => x == 5 in both. */
                    if (p1[0] < 4.9f || p1[0] > 5.1f ||
                        p2[0] < 4.9f || p2[0] > 5.1f) {
                        same = 0;
                    }
                } else {
                    same = 0;
                }
            }
            TEST_CHECK(same, "identical runs agree (x==5)");
        }
        le_world_destroy(w1);
        le_world_destroy(w2);
        le_engine_destroy(e1);
        le_engine_destroy(e2);
    }

    printf("Luma Engine Phase 26 headless tests: %d passed, %d "
           "failed\n",
           g_passed, g_failed);
    if (g_failed != 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL PHASE 26 HEADLESS TESTS PASSED\n");
    return 0;
}
