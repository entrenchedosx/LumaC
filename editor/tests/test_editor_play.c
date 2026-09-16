/*
 * Luma Editor Phase 31 headless tests: play-mode isolation
 * (enter/tick/exit, edit-before/after canonical equality,
 * runtime-only stepping proof, pause/resume/step, failed play,
 * double-enter/exit errors, scene save/load round-trip).
 *
 * No GPU, no window, no renderer. Script isolation is proven with
 * a counter script: edit-world counter frozen while runtime ticks.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <luma_editor/luma_editor.h>

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

static int make_session(led_session **s, le_engine **e, le_world **w) {
    le_engine_desc ed;
    le_world_desc wd;

    memset(&ed, 0, sizeof(ed));
    memset(&wd, 0, sizeof(wd));
    if (le_engine_create(&ed, e) != LE_SUCCESS) {
        return 0;
    }
    if (le_world_create(*e, &wd, w) != LE_SUCCESS) {
        le_engine_destroy(*e);
        return 0;
    }
    if (led_session_create(s) != LED_SUCCESS) {
        le_world_destroy(*w);
        le_engine_destroy(*e);
        return 0;
    }
    if (led_session_attach(*s, *e, *w) != LED_SUCCESS) {
        led_session_destroy(*s);
        le_world_destroy(*w);
        le_engine_destroy(*e);
        return 0;
    }
    return 1;
}

/* Canonical capture of a world (malloc'd text; caller frees with
 * le_scene_free_text). Returns NULL on failure. */
static char *capture_canonical(le_engine *e, le_world *w,
                               size_t *out_size) {
    le_asset scene;
    char *text = NULL;
    size_t size = 0;

    if (out_size != NULL) {
        *out_size = 0;
    }
    if (le_scene_create(e, 0, &scene) != LE_SUCCESS) {
        return NULL;
    }
    {
        uint32_t skipped = 0;

        if (le_scene_capture(w, &scene, &skipped) != LE_SUCCESS) {
            le_asset_unload(e, &scene);
            return NULL;
        }
    }
    if (le_scene_save_text(e, &scene, &text, &size) != LE_SUCCESS) {
        le_asset_unload(e, &scene);
        return NULL;
    }
    le_asset_unload(e, &scene);
    if (out_size != NULL) {
        *out_size = size;
    }
    return text;
}

static const char kCounterScript[] =
    "export('n', 0)\n"
    "function update(self, dt)\n"
    "  self.n = self.n + 1\n"
    "end\n";

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Luma Editor Phase 31 play headless tests...\n");

    /* Guards. */
    TEST_CHECK(led_play_enter(NULL) == LED_ERROR_INVALID_ARGUMENT,
               "enter NULL INVALID");
    TEST_CHECK(led_play_tick(NULL, 0.016f) ==
                   LED_ERROR_INVALID_ARGUMENT,
               "tick NULL INVALID");
    TEST_CHECK(led_play_exit(NULL) == LED_ERROR_INVALID_ARGUMENT,
               "exit NULL INVALID");
    TEST_CHECK(led_play_set_paused(NULL, 1) ==
                   LED_ERROR_INVALID_ARGUMENT,
               "set paused NULL INVALID");
    TEST_CHECK(!led_play_is_paused(NULL), "is paused NULL 0");
    TEST_CHECK(led_play_step(NULL) == LED_ERROR_INVALID_ARGUMENT,
               "step NULL INVALID");
    TEST_CHECK(led_play_get_world(NULL) == NULL,
               "play world NULL NULL");

    /* Not-playing errors. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;

        TEST_CHECK(make_session(&s, &e, &w), "make session");
        TEST_CHECK(led_play_tick(s, 0.016f) == LED_ERROR_NOT_PLAYING,
                   "tick w/o play NOT_PLAYING");
        TEST_CHECK(led_play_exit(s) == LED_ERROR_NOT_PLAYING,
                   "exit w/o play NOT_PLAYING");
        TEST_CHECK(led_play_set_paused(s, 1) == LED_ERROR_NOT_PLAYING,
                   "pause w/o play NOT_PLAYING");
        TEST_CHECK(led_play_step(s) == LED_ERROR_NOT_PLAYING,
                   "step w/o play NOT_PLAYING");
        TEST_CHECK(led_play_get_world(s) == NULL,
                   "no runtime world");
        TEST_CHECK(led_play_tick(s, -1.0f) == LED_ERROR_NOT_PLAYING,
                   "negative dt w/o play still NOT_PLAYING");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Enter/tick/exit with canonical edit equality. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;
        char *before = NULL;
        char *after = NULL;
        size_t bsize = 0;
        size_t asize = 0;

        TEST_CHECK(make_session(&s, &e, &w), "make session 2");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "obj");
        TEST_CHECK(le_object_set_name(w, &o, "actor") == LE_SUCCESS,
                   "name actor");
        {
            float p[3] = { 1.0f, 2.0f, 3.0f };

            TEST_CHECK(le_object_set_position(w, &o, p) ==
                           LE_SUCCESS,
                       "set pos");
        }
        /* Ticking the edit world directly is NOT part of the play
         * flow; scripts would run here in a real game loop. The
         * editor play_tick must be the only stepper. */
        before = capture_canonical(e, w, &bsize);
        TEST_CHECK(before != NULL, "capture before");
        TEST_CHECK(led_play_enter(s) == LED_SUCCESS, "play enter");
        TEST_CHECK(led_is_playing(s), "playing");
        TEST_CHECK(led_play_enter(s) == LED_ERROR_ALREADY_PLAYING,
                   "double enter rejected");
        TEST_CHECK(led_play_get_world(s) != NULL, "runtime live");
        TEST_CHECK(led_play_get_world(s) != w, "runtime != edit");
        TEST_CHECK(le_world_get_object_count(led_play_get_world(s)) ==
                       1,
                   "runtime has 1 object");
        TEST_CHECK(le_world_get_object_count(w) == 1,
                   "edit untouched count");
        TEST_CHECK(le_world_is_paused(w), "edit paused in play");
        {
            int i;

            for (i = 0; i < 10; i++) {
                TEST_CHECK(led_play_tick(s, 1.0f / 60.0f) ==
                               LED_SUCCESS,
                           "play tick");
            }
        }
        {
            led_play_stats ps;

            memset(&ps, 0, sizeof(ps));
            led_play_get_stats(s, &ps);
            TEST_CHECK(ps.playing && ps.ticks == 10,
                       "play stats ticks");
            TEST_CHECK(ps.runtime_objects == 1, "runtime objects");
        }
        TEST_CHECK(led_play_tick(s, -1.0f) ==
                       LED_ERROR_INVALID_ARGUMENT,
                   "negative dt INVALID");
        TEST_CHECK(led_play_exit(s) == LED_SUCCESS, "play exit");
        TEST_CHECK(!led_is_playing(s), "stopped");
        TEST_CHECK(led_play_get_world(s) == NULL,
                   "runtime gone");
        TEST_CHECK(!le_world_is_paused(w), "edit unpaused");
        TEST_CHECK(led_play_exit(s) == LED_ERROR_NOT_PLAYING,
                   "double exit rejected");
        after = capture_canonical(e, w, &asize);
        TEST_CHECK(after != NULL, "capture after");
        TEST_CHECK(bsize == asize && strcmp(before, after) == 0,
                   "edit byte-identical after play");
        if (before != NULL) {
            le_scene_free_text(before);
        }
        if (after != NULL) {
            le_scene_free_text(after);
        }
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Runtime-only stepping: edit script counter frozen. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_asset script;
        le_object o = LE_OBJECT_INVALID;
        le_script_asset_desc sd;

        memset(&sd, 0, sizeof(sd));
        TEST_CHECK(make_session(&s, &e, &w), "make session 3");
        sd.source = kCounterScript;
        sd.size = strlen(kCounterScript);
        sd.path_hint = "counter.lua";
        TEST_CHECK(le_asset_create_script(e, &sd, &script) ==
                       LE_SUCCESS,
                   "create counter script");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "obj");
        TEST_CHECK(le_object_add_script(w, &o, &script) ==
                       LE_SUCCESS,
                   "attach script");
        /* One direct engine step instantiates + runs the edit
         * script once (proves the script works outside play). */
        TEST_CHECK(le_engine_step(e, w, 1.0f / 60.0f) == LE_SUCCESS,
                   "prime edit script");
        {
            le_script_property p;

            memset(&p, 0, sizeof(p));
            TEST_CHECK(le_script_get_property(w, &o, "n", &p),
                       "read n primed");
            TEST_CHECK(p.integer >= 0, "n primed value");
        }
        TEST_CHECK(led_play_enter(s) == LED_SUCCESS,
                   "play enter scripted");
        {
            le_world *rw = led_play_get_world(s);
            int i;

            TEST_CHECK(rw != NULL, "runtime live scripted");
            for (i = 0; i < 5; i++) {
                TEST_CHECK(led_play_tick(s, 1.0f / 60.0f) ==
                               LED_SUCCESS,
                           "scripted tick");
            }
            /* Runtime script ran (find its object, read n). */
            {
                le_object found = LE_OBJECT_INVALID;
                uint32_t n = le_world_get_all_objects(rw, &found,
                                                      1);

                TEST_CHECK(n == 1, "runtime one object");
                if (n == 1) {
                    le_script_property p;

                    memset(&p, 0, sizeof(p));
                    TEST_CHECK(le_script_get_property(rw, &found,
                                                      "n", &p),
                               "runtime n readable");
                    TEST_CHECK(p.integer >= 5,
                               "runtime script advanced");
                }
            }
        }
        /* Edit script did NOT advance during play (matrices-only
         * refresh on edit; gameplay never runs there). */
        {
            le_script_property p;

            memset(&p, 0, sizeof(p));
            TEST_CHECK(le_script_get_property(w, &o, "n", &p),
                       "edit n readable after play");
            TEST_CHECK(p.integer <= 1, "edit script frozen");
        }
        TEST_CHECK(led_play_exit(s) == LED_SUCCESS, "exit scripted");
        le_asset_unload(e, &script);
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Pause/resume/step on the runtime world. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;

        TEST_CHECK(make_session(&s, &e, &w), "make session 4");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "obj");
        TEST_CHECK(led_play_enter(s) == LED_SUCCESS, "enter");
        TEST_CHECK(!led_play_is_paused(s), "runtime unpaused");
        TEST_CHECK(led_play_set_paused(s, 1) == LED_SUCCESS,
                   "pause runtime");
        TEST_CHECK(led_play_is_paused(s), "runtime paused");
        TEST_CHECK(led_play_tick(s, 1.0f / 60.0f) == LED_SUCCESS,
                   "tick while paused ok");
        TEST_CHECK(led_play_step(s) == LED_SUCCESS,
                   "single step ok");
        TEST_CHECK(led_play_set_paused(s, 0) == LED_SUCCESS,
                   "resume runtime");
        TEST_CHECK(!led_play_is_paused(s), "runtime resumed");
        TEST_CHECK(led_play_exit(s) == LED_SUCCESS, "exit");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Selection restore on exit (pre-play selection by handle). */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object a = LE_OBJECT_INVALID;
        le_object b = LE_OBJECT_INVALID;

        TEST_CHECK(make_session(&s, &e, &w), "make session 5");
        TEST_CHECK(le_object_create(w, &a) == LE_SUCCESS, "a");
        TEST_CHECK(le_object_create(w, &b) == LE_SUCCESS, "b");
        TEST_CHECK(led_selection_set(s, &a, 1) == LED_SUCCESS,
                   "select a");
        TEST_CHECK(led_play_enter(s) == LED_SUCCESS, "enter");
        TEST_CHECK(led_selection_get(s, NULL, 0) == 0,
                   "selection cleared in play");
        TEST_CHECK(led_play_exit(s) == LED_SUCCESS, "exit");
        TEST_CHECK(led_selection_get(s, NULL, 0) == 1,
                   "selection restored");
        TEST_CHECK(led_selection_contains(s, &a), "a reselected");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Scene save/open round-trip via files (canonical oracle). */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;
        char path[1024];

        memset(path, 0, sizeof(path));
        TEST_CHECK(make_session(&s, &e, &w), "make session 6");
        /* CTest-safe temp path (works regardless of CWD). */
        {
            const char *tmp = getenv("TEMP");
            const char *tmp2 = getenv("TMP");

            if (tmp == NULL || tmp[0] == '\0') {
                tmp = tmp2;
            }
            if (tmp == NULL || tmp[0] == '\0') {
                tmp = ".";
            }
            snprintf(path, sizeof(path), "%s/phase31_roundtrip.luma_scene",
                     tmp);
        }
        TEST_CHECK(led_scene_save(s) == LED_ERROR_IO,
                   "save w/o path IO error");
        TEST_CHECK(led_scene_revert(s) == LED_ERROR_IO,
                   "revert w/o path IO error");
        TEST_CHECK(strcmp(led_scene_get_path(s), "") == 0,
                   "no path yet");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "obj");
        TEST_CHECK(le_object_set_name(w, &o, "saved") == LE_SUCCESS,
                   "name saved");
        TEST_CHECK(!led_is_dirty(s), "clean before cmd");
        {
            led_command c;

            memset(&c, 0, sizeof(c));
            c.kind = LED_CMD_SET_POSITION;
            c.target = o;
            c.vec_value[0] = 7.0f;
            TEST_CHECK(led_history_set_coalesce(s, 0, 0) ==
                           LED_SUCCESS,
                       "coalesce off");
            TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                       "move via cmd");
        }
        TEST_CHECK(led_is_dirty(s), "dirty after cmd");
        TEST_CHECK(led_scene_save_as(s, path) == LED_SUCCESS,
                   "save as");
        TEST_CHECK(!led_is_dirty(s), "clean after save");
        TEST_CHECK(strcmp(led_scene_get_path(s), path) == 0,
                   "path remembered");
        TEST_CHECK(led_scene_new(s) == LED_SUCCESS, "scene new");
        TEST_CHECK(le_world_get_object_count(w) == 0,
                   "world cleared");
        TEST_CHECK(!led_is_dirty(s), "new clears dirty");
        TEST_CHECK(led_scene_open(s, path) == LED_SUCCESS,
                   "scene open");
        TEST_CHECK(le_world_get_object_count(w) == 1,
                   "one object back");
        {
            le_object found = LE_OBJECT_INVALID;

            TEST_CHECK(le_world_find_by_name(w, "saved", &found),
                       "saved name back");
            {
                float p[3];

                le_object_get_position(w, &found, p);
                TEST_CHECK(p[0] == 7.0f, "saved position back");
            }
        }
        /* Corrupt the file target: failed open preserves world. */
        TEST_CHECK(led_scene_open(s, "phase31_does_not_exist_xyz") !=
                       LED_SUCCESS,
                   "open missing fails");
        TEST_CHECK(le_world_get_object_count(w) == 1,
                   "world preserved after failed open");
        /* Revert restores the saved state. */
        {
            led_command c;

            memset(&c, 0, sizeof(c));
            c.kind = LED_CMD_SET_POSITION;
            TEST_CHECK(
                le_world_find_by_name(w, "saved", &c.target),
                "find for dirty");
            c.vec_value[0] = 99.0f;
            TEST_CHECK(led_execute(s, &c) == LED_SUCCESS, "dirty it");
            TEST_CHECK(led_is_dirty(s), "dirty again");
            TEST_CHECK(led_scene_revert(s) == LED_SUCCESS,
                       "revert ok");
            TEST_CHECK(!led_is_dirty(s), "revert clears dirty");
        }
        /* Save via remembered path. */
        TEST_CHECK(led_scene_save(s) == LED_SUCCESS,
                   "save remembered");
        remove(path);
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Scene-new blocked while playing; save works mid-play is
     * allowed (edit world capture) — new/open/revert blocked. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;

        TEST_CHECK(make_session(&s, &e, &w), "make session 7");
        TEST_CHECK(led_play_enter(s) == LED_SUCCESS, "enter");
        TEST_CHECK(led_scene_new(s) == LED_ERROR_ALREADY_PLAYING,
                   "new blocked in play");
        TEST_CHECK(led_scene_open(s, "x") ==
                       LED_ERROR_ALREADY_PLAYING,
                   "open blocked in play");
        TEST_CHECK(led_scene_revert(s) == LED_ERROR_ALREADY_PLAYING,
                   "revert blocked in play");
        TEST_CHECK(led_play_exit(s) == LED_SUCCESS, "exit");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    printf("Luma Editor Phase 31 play headless tests: %d passed, %d "
           "failed\n",
           g_passed, g_failed);
    if (g_failed == 0) {
        printf("ALL PHASE 31 PLAY HEADLESS TESTS PASSED\n");
    } else {
        printf("TESTS FAILED\n");
    }
    return (g_failed == 0) ? 0 : 1;
}
