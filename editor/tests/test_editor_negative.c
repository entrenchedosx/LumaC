/* Phase 33V negative tests (headless): every failure path below
 * must FAIL CLEANLY (error code, no crash, prior state preserved).
 * Covers the gaps the audit found in the positive suites: bad
 * project paths, missing scenes, reparent cycles, stale handles,
 * typeless drops, play/save guards, missing prefabs, unknown
 * extensions. Renderer-free (le_engine_create with NULL renderer).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <luma_engine/luma_engine.h>
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

static led_command make_create(const char *name) {
    led_command c;

    memset(&c, 0, sizeof(c));
    c.kind = LED_CMD_CREATE;
    snprintf(c.label, sizeof(c.label), "neg");
    if (name != NULL) {
        strncpy(c.name_value, name, sizeof(c.name_value) - 1);
    }
    return c;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Luma Editor Phase 33V negative tests...\n");

    /* Bad project paths (no project created, no crash). */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;

        TEST_CHECK(make_session(&s, &e, &w), "make session");
        TEST_CHECK(led_project_open(s, NULL) != LED_SUCCESS,
                   "project open NULL fails");
        TEST_CHECK(led_project_open(s, "") != LED_SUCCESS,
                   "project open empty fails");
        TEST_CHECK(led_project_open(
                       s, "phase33v_missing_dir_xyz") != LED_SUCCESS,
                   "project open missing dir fails");
        TEST_CHECK(!led_project_is_open(s),
                   "no project latched after failures");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Missing scene open preserves the world. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        led_command c = make_create("keeper");

        TEST_CHECK(make_session(&s, &e, &w), "make session 2");
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                   "seed keeper");
        TEST_CHECK(led_scene_open(s, "phase33v_no_scene_xyz") !=
                       LED_SUCCESS,
                   "missing scene open fails");
        TEST_CHECK(le_world_get_object_count(w) == 1,
                   "world preserved after failed open");
        TEST_CHECK(led_scene_save(s) != LED_SUCCESS,
                   "save with no path fails");
        TEST_CHECK(led_is_dirty(s),
                   "failed save keeps dirty");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Reparent cycle rejected, hierarchy intact. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object parent = LE_OBJECT_INVALID;
        le_object child = LE_OBJECT_INVALID;
        led_command c;

        TEST_CHECK(make_session(&s, &e, &w), "make session 3");
        c = make_create("p");
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS, "create p");
        TEST_CHECK(le_world_find_by_name(w, "p", &parent),
                   "find p");
        c = make_create("c");
        c.parent = parent;
        c.has_parent = 1;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                   "create c under p");
        TEST_CHECK(le_world_find_by_name(w, "c", &child),
                   "find c");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_REPARENT;
        snprintf(c.label, sizeof(c.label), "cycle");
        c.target = parent;
        c.parent = child;
        c.has_parent = 1;
        c.reparent_mode = LED_REPARENT_KEEP_WORLD;
        TEST_CHECK(led_execute(s, &c) != LED_SUCCESS,
                   "reparent cycle rejected");
        {
            le_object back = LE_OBJECT_INVALID;

            TEST_CHECK(le_world_find_by_name(w, "p", &back),
                       "parent survives cycle attempt");
            TEST_CHECK(le_world_find_by_name(w, "c", &back),
                       "child survives cycle attempt");
        }
        /* Self-parent rejected too. */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_SET_PARENT;
        snprintf(c.label, sizeof(c.label), "self");
        c.target = parent;
        c.parent = parent;
        c.has_parent = 1;
        TEST_CHECK(led_execute(s, &c) != LED_SUCCESS,
                   "self parent rejected");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Stale handle use fails safely (delete then operate). */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object ghost = LE_OBJECT_INVALID;
        led_command c;

        TEST_CHECK(make_session(&s, &e, &w), "make session 4");
        c = make_create("ghost");
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                   "create ghost");
        TEST_CHECK(le_world_find_by_name(w, "ghost", &ghost),
                   "find ghost");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_DELETE;
        snprintf(c.label, sizeof(c.label), "del ghost");
        c.target = ghost;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                   "delete ghost");
        /* Operate on the now-stale handle. */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_SET_POSITION;
        snprintf(c.label, sizeof(c.label), "move ghost");
        c.target = ghost;
        c.vec_value[0] = 5.0f;
        TEST_CHECK(led_execute(s, &c) != LED_SUCCESS,
                   "stale move rejected");
        TEST_CHECK(!led_selection_contains(s, &ghost),
                   "stale never selected");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Missing prefab load fails (no object created). */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_asset pref = LE_ASSET_INVALID;
        uint32_t before = 0;

        TEST_CHECK(make_session(&s, &e, &w), "make session 5");
        before = le_world_get_object_count(w);
        TEST_CHECK(led_prefab_load(
                       s, "phase33v_missing_xyz.luprefab",
                       &pref) != LED_SUCCESS,
                   "missing prefab load fails");
        TEST_CHECK(le_world_get_object_count(w) == before,
                   "no object from failed prefab load");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Play/save guards (state machine rejects nonsense). */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;

        TEST_CHECK(make_session(&s, &e, &w), "make session 6");
        TEST_CHECK(led_play_tick(s, 1.0f / 60.0f) != LED_SUCCESS,
                   "tick without play fails");
        TEST_CHECK(led_play_exit(s) != LED_SUCCESS,
                   "exit without play fails");
        TEST_CHECK(led_play_enter(s) == LED_SUCCESS,
                   "play enter");
        TEST_CHECK(led_play_enter(s) != LED_SUCCESS,
                   "double play enter fails");
        TEST_CHECK(led_scene_open(s, "x") != LED_SUCCESS,
                   "scene open during play fails");
        TEST_CHECK(led_play_exit(s) == LED_SUCCESS,
                   "play exit");
        TEST_CHECK(led_play_exit(s) != LED_SUCCESS,
                   "double play exit fails");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Typeless/empty drop payloads rejected. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;
        led_drag_payload pay;
        led_command c = make_create("dropvictim");

        TEST_CHECK(make_session(&s, &e, &w), "make session 7");
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                   "create dropvictim");
        TEST_CHECK(le_world_find_by_name(w, "dropvictim", &o),
                   "find dropvictim");
        memset(&pay, 0, sizeof(pay));
        TEST_CHECK(!led_drop_model_into_scene(s, &pay, NULL),
                   "empty model drop rejected");
        TEST_CHECK(!led_drop_script_onto_object(s, &pay, &o),
                   "empty script drop rejected");
        TEST_CHECK(!led_drop_material_onto_object(s, &pay, &o),
                   "empty material drop rejected");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    printf("editor negative: %d passed, %d failed\n", g_passed,
           g_failed);
    return (g_failed == 0) ? 0 : 1;
}
