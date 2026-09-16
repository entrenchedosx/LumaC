/*
 * Luma Engine Phase 29 script-integration tests (headless):
 * Lua animation playback bindings (play/pause/resume/stop/
 * seek/speed/is_playing/time/duration + crossfade via
 * Assets.find_by_id with a clip-ID hex STRING property),
 * failed-callback policy, stale-object error path, and scene
 * round-trip with animator + script together.
 *
 * Deterministic: le_engine_step only. No GPU/window.
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

static le_result make_script(le_engine *engine, const char *src,
                             le_asset *out) {
    le_script_asset_desc d;

    memset(&d, 0, sizeof(d));
    d.source = src;
    d.size = strlen(src);
    d.path_hint = "test.lua";
    return le_asset_create_script(engine, &d, out);
}

/* OBJECT translation ramp clip (owner x: 0 -> x1 over dur). */
static le_result make_ramp(le_engine *engine, float dur, float x1,
                           le_asset *out) {
    float times[2] = { 0.0f, 0.0f };
    float values[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    le_anim_track_desc tr;
    le_animation_clip_desc d;

    times[1] = dur;
    values[3] = x1;
    memset(&tr, 0, sizeof(tr));
    tr.target_kind = LE_ANIM_TARGET_OBJECT;
    tr.target_index = 0;
    tr.channel = LE_ANIM_CHANNEL_TRANSLATION;
    tr.interpolation = LE_ANIM_INTERP_LINEAR;
    tr.times = times;
    tr.values = values;
    tr.key_count = 2;
    memset(&d, 0, sizeof(d));
    d.duration = dur;
    d.tracks = &tr;
    d.track_count = 1;
    return le_asset_create_clip(engine, &d, out);
}

/* Attach a paused looping animator to an object. */
static le_result attach(le_world *world, const le_object *o,
                        const le_asset *clip) {
    le_animator_desc d;

    memset(&d, 0, sizeof(d));
    d.skeleton = LE_ASSET_INVALID;
    d.clip = *clip;
    d.autoplay = 0;
    d.loop_mode = LE_ANIM_LOOP;
    d.speed = 1.0f;
    d.start_time = 0.0f;
    return le_object_add_animator(world, o, &d);
}

static float pos_x(le_world *world, const le_object *o) {
    float p[3];

    le_object_get_position(world, o, p);
    return p[0];
}

/* Pass a clip-ID hex string to Lua as an exported STRING prop. */
static le_result set_clip_hex(le_engine *engine, le_world *world,
                              const le_object *o,
                              const le_asset *clip) {
    le_asset_id id;
    char hex[33];
    le_script_property p;

    le_asset_get_id(engine, clip, &id);
    le_asset_id_to_string(&id, hex);
    memset(&p, 0, sizeof(p));
    p.type = LE_SCRIPT_PROP_STRING;
    snprintf(p.name, sizeof(p.name), "clip_hex");
    snprintf(p.string_value, sizeof(p.string_value), "%s", hex);
    return le_script_set_property(world, o, &p);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 29 script tests...\n");

    /* Lua play (no-arg continue) + seek + queries. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset clip = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_ramp(engine, 2.0f, 4.0f, &clip) ==
                       LE_SUCCESS,
                   "ramp clip creates");
        TEST_CHECK(make_script(engine,
                               "function start(self)\n"
                               "  self:animation_play(nil)\n"
                               "  self:animation_seek(0.5)\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "playback script compiles");
        le_object_create(world, &o);
        attach(world, &o, &clip);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "playback script survives");
        TEST_CHECK(le_anim_is_playing(world, &o) == 1,
                   "Lua play starts playback");
        TEST_CHECK(le_anim_get_time(world, &o) > 0.5f,
                   "Lua seek + step advance");
        TEST_CHECK(le_anim_get_time(world, &o) < 0.6f,
                   "Lua time advances from seek point");
        TEST_CHECK(NEAR(le_anim_get_duration(world, &o), 2.0f,
                        1e-6f),
                   "Lua duration query matches clip");
        TEST_CHECK(pos_x(world, &o) > 1.0f,
                   "Lua-driven pose writes transform");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Lua pause holds time exactly. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset clip = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_ramp(engine, 2.0f, 4.0f, &clip);
        TEST_CHECK(make_script(engine,
                               "function start(self)\n"
                               "  self:animation_play(nil)\n"
                               "  self:animation_seek(0.4)\n"
                               "end\n"
                               "function update(self, dt)\n"
                               "  self:animation_pause()\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "pause script compiles");
        le_object_create(world, &o);
        attach(world, &o, &clip);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.1f);
        le_engine_step(engine, world, 0.5f);
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "pause script survives");
        TEST_CHECK(le_anim_is_playing(world, &o) == 0 &&
                       le_anim_get_time(world, &o) == 0.4f,
                   "Lua pause holds time exactly");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Lua resume continues a paused animator (paused from C so
     * start() races cannot interfere). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset clip = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_ramp(engine, 2.0f, 4.0f, &clip);
        TEST_CHECK(make_script(engine,
                               "function update(self, dt)\n"
                               "  self:animation_resume()\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "resume script compiles");
        le_object_create(world, &o);
        attach(world, &o, &clip);
        le_anim_pause(world, &o);
        le_object_add_script(world, &o, &script);
        le_world_update(world, 0.016f); /* start() only */
        le_anim_pause(world, &o);       /* re-hold after start */
        le_anim_seek(world, &o, 0.0f);
        le_engine_step(engine, world, 0.2f);
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "resume script survives");
        TEST_CHECK(le_anim_is_playing(world, &o) == 1 &&
                       NEAR(le_anim_get_time(world, &o), 0.2f,
                            1e-5f),
                   "Lua resume advances playback");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Lua stop(reset) returns to time 0. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset clip = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_ramp(engine, 2.0f, 4.0f, &clip);
        TEST_CHECK(make_script(engine,
                               "function start(self)\n"
                               "  self:animation_play(nil)\n"
                               "  self:animation_seek(0.5)\n"
                               "end\n"
                               "function update(self, dt)\n"
                               "  self:animation_stop(true)\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "stop script compiles");
        le_object_create(world, &o);
        attach(world, &o, &clip);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.1f);
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "stop script survives");
        TEST_CHECK(le_anim_is_playing(world, &o) == 0 &&
                       le_anim_get_time(world, &o) == 0.0f,
                   "Lua stop reset returns to 0");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Lua speed scales playback (read back from C). The speed
     * set lands in start() before the first advance; the 0.25s
     * step then integrates at 2x exactly. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset clip = LE_ASSET_INVALID;
        le_animator_desc got;

        make_engine(&engine);
        make_world(engine, &world);
        make_ramp(engine, 4.0f, 8.0f, &clip);
        TEST_CHECK(make_script(engine,
                               "function start(self)\n"
                               "  self:animation_play(nil)\n"
                               "  self:animation_speed(2.0)\n"
                               "  spd = self:animation_speed()\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "speed script compiles");
        le_object_create(world, &o);
        attach(world, &o, &clip);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.0f);
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "speed script survives");
        memset(&got, 0, sizeof(got));
        le_object_get_animator(world, &o, &got);
        TEST_CHECK(NEAR(got.speed, 2.0f, 1e-7f),
                   "Lua speed sticks");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Speed scales the visual advance (separate world: step a
     * fixed dt and compare times at 1x vs 2x). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset clip = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_ramp(engine, 4.0f, 8.0f, &clip);
        TEST_CHECK(make_script(engine,
                               "function start(self)\n"
                               "  self:animation_play(nil)\n"
                               "  self:animation_speed(2.0)\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "speed advance script compiles");
        le_object_create(world, &o);
        attach(world, &o, &clip);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.25f);
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "speed advance survives");
        TEST_CHECK(NEAR(le_anim_get_time(world, &o), 0.5f, 1e-5f),
                   "Lua speed scales advance");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Crossfade from Lua via Assets.find_by_id(hex STRING
     * prop). NOTE: the prop default is "" at the first start()
     * (C sets the real hex only after instantiation), so the
     * script MUST tolerate a nil lookup: find_by_id returns nil
     * for malformed hex (never an error), and crossfade(nil)
     * raises a script error (le_asset expected). The armed flag
     * defers the fade until the prop is set. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset clipA = LE_ASSET_INVALID;
        le_asset clipB = LE_ASSET_INVALID;
        le_anim_stats st;

        make_engine(&engine);
        make_world(engine, &world);
        make_ramp(engine, 1.0f, 0.0f, &clipA);
        TEST_CHECK(make_ramp(engine, 1.0f, 10.0f, &clipB) ==
                       LE_SUCCESS,
                   "fade clips create");
        TEST_CHECK(make_script(engine,
                               "export('clip_hex', '')\n"
                               "export('armed', 0)\n"
                               "function start(self)\n"
                               "  self:animation_play(nil)\n"
                               "end\n"
                               "function update(self, dt)\n"
                               "  local a = self:get('armed')\n"
                               "  if a == 1 then\n"
                               "    local hx = "
                               "self:get('clip_hex')\n"
                               "    local c = "
                               "Assets.find_by_id(hx)\n"
                               "    if c ~= nil then\n"
                               "      self:animation_crossfade(c, "
                               "0.5)\n"
                               "      self:set('armed', 2)\n"
                               "    end\n"
                               "  end\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "crossfade script compiles");
        le_object_create(world, &o);
        attach(world, &o, &clipA);
        le_object_add_script(world, &o, &script);
        le_world_update(world, 0.05f);
        /* Export defaults are seeded into instance values at
         * instantiate, so self:get reads 0 (disarmed) and the
         * script survives without firing. */
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "unattached prop read returns default");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Crossfade from Lua via Assets.find_by_id(hex STRING prop).
     * Attach-timing discipline (verified against the script
     * backend): instance values exist only after instantiate at
     * start(), so C-side writes land only after the first update;
     * and Lua-side self:get reads the LIVE values table, which is
     * seeded at that same instantiate. Both sides agree after one
     * le_world_update. The armed flag defers the fade until the
     * C side has published the real clip hex. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset clipA = LE_ASSET_INVALID;
        le_asset clipB = LE_ASSET_INVALID;
        le_anim_stats st;

        make_engine(&engine);
        make_world(engine, &world);
        make_ramp(engine, 1.0f, 0.0f, &clipA);
        TEST_CHECK(make_ramp(engine, 1.0f, 10.0f, &clipB) ==
                       LE_SUCCESS,
                   "fade clips create");
        TEST_CHECK(make_script(engine,
                               "export('clip_hex', 'unset')\n"
                               "export('armed', 0.0)\n"
                               "function start(self)\n"
                               "  self:animation_play(nil)\n"
                               "end\n"
                               "function update(self, dt)\n"
                               "  if self.armed == 1 then\n"
                               "    local c = "
                               "Assets.find_by_id(self.clip_hex)\n"
                               "    if c ~= nil then\n"
                               "      self:animation_crossfade(c, "
                               "0.5)\n"
                               "      self:set('armed', 2.0)\n"
                               "    end\n"
                               "  end\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "crossfade script compiles");
        le_object_create(world, &o);
        attach(world, &o, &clipA);
        le_object_add_script(world, &o, &script);
        le_world_update(world, 0.05f);
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "crossfade script starts clean");
        TEST_CHECK(set_clip_hex(engine, world, &o, &clipB) ==
                       LE_SUCCESS,
                   "clip hex prop set");
        {
            le_script_property p;
            le_script_property got;

            memset(&p, 0, sizeof(p));
            p.type = LE_SCRIPT_PROP_NUMBER;
            snprintf(p.name, sizeof(p.name), "armed");
            p.number = 1.0;
            TEST_CHECK(le_script_set_property(world, &o, &p) ==
                           LE_SUCCESS,
                       "fade armed");
            memset(&got, 0, sizeof(got));
            got.type = LE_SCRIPT_PROP_NUMBER;
            TEST_CHECK(le_script_get_property(world, &o, "armed",
                                              &got) == 1 &&
                           got.number == 1.0,
                       "armed reads back from C");
        }
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(!le_object_script_failed(world, &o),
                   "Lua crossfade survives");
        memset(&st, 0, sizeof(st));
        le_anim_get_stats(world, &st);
        TEST_CHECK(st.active_crossfades == 1,
                   "Lua crossfade activates fade");
        /* NOTE: le_engine_step clamps dt to the engine max_delta
         * (0.25s), so settle with several small steps instead of
         * one big one. */
        for (int si = 0; si < 12; si++) {
            le_engine_step(engine, world, 0.05f);
        }
        memset(&st, 0, sizeof(st));
        le_anim_get_stats(world, &st);
        TEST_CHECK(st.active_crossfades == 0,
                   "Lua fade settles");
        /* Fade destination is the 0 -> 10 ramp; after ~0.65s of
         * LOOP playback the owner sits past mid-ramp. */
        TEST_CHECK(pos_x(world, &o) > 5.0f,
                   "Lua fade lands toward B");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Erroring anim callback marks failed, world continues. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset clip = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_ramp(engine, 1.0f, 2.0f, &clip);
        TEST_CHECK(make_script(engine,
                               "function update(self, dt)\n"
                               "  self:animation_seek(\"bogus\")\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "error script compiles");
        le_object_create(world, &o);
        attach(world, &o, &clip);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(le_object_script_failed(world, &o),
                   "erroring anim call marks failed");
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(1, "world survives anim handler error");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Stale object: Lua keeps a handle across destroy; the next
     * method call errors (no crash) and marks failed. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset clip = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_ramp(engine, 1.0f, 2.0f, &clip);
        TEST_CHECK(make_script(engine,
                               "victim = nil\n"
                               "function start(self)\n"
                               "  victim = World.create('doomed')\n"
                               "  World.destroy(victim)\n"
                               "  victim:animation_pause()\n"
                               "end\n",
                               &script) == LE_SUCCESS,
                   "stale script compiles");
        le_object_create(world, &o);
        attach(world, &o, &clip);
        le_object_add_script(world, &o, &script);
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(le_object_script_failed(world, &o),
                   "stale anim call marks failed");
        le_engine_step(engine, world, 0.05f);
        TEST_CHECK(1, "world survives stale call");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Animator + script scene round-trip together. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_world *world2 = NULL;
        le_object o;
        le_asset script = LE_ASSET_INVALID;
        le_asset clip = LE_ASSET_INVALID;
        le_asset scene = LE_ASSET_INVALID;
        le_asset scene2 = LE_ASSET_INVALID;
        const char *src =
            "export('speed', 4.0)\n"
            "function update(self, dt)\n"
            "  self:animation_seek(0.25)\n"
            "end\n";
        char *text = NULL;
        size_t size = 0;
        le_scene_instance inst;

        make_engine(&engine);
        make_world(engine, &world);
        make_ramp(engine, 2.0f, 4.0f, &clip);
        make_script(engine, src, &script);
        le_object_create(world, &o);
        attach(world, &o, &clip);
        le_object_add_script(world, &o, &script);
        le_world_update(world, 0.016f);
        le_scene_create(engine, 1, &scene);
        TEST_CHECK(le_scene_capture(world, &scene, NULL) ==
                       LE_SUCCESS,
                   "capture anim+script world");
        TEST_CHECK(le_scene_save_text(engine, &scene, &text,
                                      &size) == LE_SUCCESS &&
                       text != NULL,
                   "save anim+script scene");
        TEST_CHECK(strstr(text, "animator") != NULL &&
                       strstr(text, "script ") != NULL,
                   "save emits animator + script lines");
        le_scene_create(engine, 1, &scene2);
        TEST_CHECK(le_scene_load_text(engine, &scene2, text,
                                      size) == LE_SUCCESS,
                   "anim+script scene re-parses");
        le_scene_free_text(text);
        make_world(engine, &world2);
        memset(&inst, 0, sizeof(inst));
        TEST_CHECK(le_scene_instantiate(world2, &scene2, &inst) ==
                       LE_SUCCESS,
                   "anim+script scene instantiates");
        TEST_CHECK(inst.count == 1, "one object instantiated");
        if (inst.count == 1) {
            le_world_update(world2, 0.05f);
            TEST_CHECK(le_anim_get_time(world2,
                                        &inst.objects[0]) >= 0.25f,
                       "instantiated script drives seek");
            TEST_CHECK(!le_object_script_failed(world2,
                                                &inst.objects[0]),
                       "instantiated script healthy");
        }
        le_scene_instance_free(&inst);
        le_world_destroy(world);
        le_world_destroy(world2);
        le_engine_destroy(engine);
    }

    printf("Phase 29 script tests: %d passed, %d failed\n",
           g_passed, g_failed);
    return g_failed == 0 ? 0 : 1;
}
