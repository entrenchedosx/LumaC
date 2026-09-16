/*
 * Luma Editor Phase 31 headless tests: session, selection (stale +
 * slot-reuse pruning), hierarchy snapshot vs engine truth,
 * inspector rows, dirty transitions, console ring, shortcuts.
 *
 * No GPU, no window, no renderer: engine created WITHOUT renderer.
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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Luma Editor Phase 31 core headless tests...\n");

    /* NULL-safety across the session/selection surface. */
    TEST_CHECK(led_session_create(NULL) == LED_ERROR_INVALID_ARGUMENT,
               "session create NULL INVALID");
    TEST_CHECK(led_session_attach(NULL, NULL, NULL) ==
                   LED_ERROR_INVALID_ARGUMENT,
               "attach all-NULL INVALID");
    TEST_CHECK(led_selection_set(NULL, NULL, 0) ==
                   LED_ERROR_INVALID_ARGUMENT,
               "selection set NULL INVALID");
    TEST_CHECK(led_selection_contains(NULL, NULL) == 0,
               "contains NULL 0");
    TEST_CHECK(led_selection_get(NULL, NULL, 0) == 0,
               "selection get NULL 0");
    TEST_CHECK(led_selection_prune(NULL) == 0, "prune NULL 0");
    TEST_CHECK(led_is_dirty(NULL) == 0, "dirty NULL 0");
    TEST_CHECK(led_is_playing(NULL) == 0, "playing NULL 0");
    TEST_CHECK(led_hierarchy_refresh(NULL) == 0,
               "hierarchy NULL 0");
    TEST_CHECK(led_hierarchy_count(NULL) == 0, "hier count NULL 0");
    TEST_CHECK(led_inspect(NULL, NULL) == 0, "inspect NULL 0");
    TEST_CHECK(led_undo(NULL) == 0, "undo NULL 0");
    TEST_CHECK(led_redo(NULL) == 0, "redo NULL 0");
    TEST_CHECK(led_console_count(NULL) == 0, "console NULL 0");
    TEST_CHECK(led_focus_get(NULL) == 0, "focus NULL 0");

    /* Session lifecycle + stats. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        led_session_stats st;

        memset(&st, 0, sizeof(st));
        TEST_CHECK(make_session(&s, &e, &w), "make session");
        led_session_get_stats(s, &st);
        TEST_CHECK(st.attached && !st.playing && !st.dirty,
                   "stats attached clean");
        TEST_CHECK(led_session_get_engine(s) == e, "borrow engine");
        TEST_CHECK(led_session_get_edit_world(s) == w,
                   "borrow world");
        TEST_CHECK(led_session_tick(s, 0.016f) == LED_SUCCESS,
                   "tick ok");
        TEST_CHECK(led_session_tick(NULL, 0.0f) ==
                       LED_ERROR_INVALID_ARGUMENT,
                   "tick NULL INVALID");
        led_session_detach(s);
        led_session_get_stats(s, &st);
        TEST_CHECK(!st.attached, "detached");
        TEST_CHECK(led_session_tick(s, 0.0f) ==
                       LED_ERROR_NOT_ATTACHED,
                   "tick detached NOT_ATTACHED");
        TEST_CHECK(led_session_attach(s, NULL, NULL) ==
                       LED_ERROR_INVALID_ARGUMENT,
                   "reattach NULL INVALID");
        TEST_CHECK(led_session_attach(s, e, w) == LED_SUCCESS,
                   "reattach ok");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
        led_session_destroy(NULL);
        led_session_detach(NULL);
    }

    /* Selection basics + dedup + ordering. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object a = LE_OBJECT_INVALID;
        le_object b = LE_OBJECT_INVALID;
        le_object c = LE_OBJECT_INVALID;

        TEST_CHECK(make_session(&s, &e, &w), "make session 2");
        TEST_CHECK(le_object_create(w, &a) == LE_SUCCESS, "create a");
        TEST_CHECK(le_object_create(w, &b) == LE_SUCCESS, "create b");
        TEST_CHECK(le_object_create(w, &c) == LE_SUCCESS, "create c");
        {
            /* Set reversed; expect ascending-slot order back. */
            le_object rev[3] = { c, a, b };
            le_object got[3];

            memset(got, 0, sizeof(got));
            TEST_CHECK(led_selection_set(s, rev, 3) == LED_SUCCESS,
                       "selection set 3");
            TEST_CHECK(led_selection_get(s, got, 3) == 3,
                       "selection get 3");
            TEST_CHECK(got[0].index == a.index &&
                           got[1].index == b.index &&
                           got[2].index == c.index,
                       "selection ascending order");
            TEST_CHECK(led_selection_contains(s, &b),
                       "contains b");
            TEST_CHECK(!led_selection_contains(s, NULL),
                       "contains NULL false");
        }
        {
            /* Dedup + stale filter. */
            le_object dup[3] = { a, a, LE_OBJECT_INVALID };

            TEST_CHECK(led_selection_set(s, dup, 3) == LED_SUCCESS,
                       "set with dup+invalid");
            TEST_CHECK(led_selection_get(s, NULL, 0) == 1,
                       "dedup to 1 live");
        }
        TEST_CHECK(led_selection_add(s, &b) == LED_SUCCESS,
                   "add b");
        TEST_CHECK(led_selection_add(s, &b) == LED_SUCCESS,
                   "add b again idempotent");
        TEST_CHECK(led_selection_get(s, NULL, 0) == 2,
                   "count 2");
        TEST_CHECK(led_selection_toggle(s, &b) == LED_SUCCESS,
                   "toggle b off");
        TEST_CHECK(!led_selection_contains(s, &b), "b gone");
        TEST_CHECK(led_selection_toggle(s, &b) == LED_SUCCESS,
                   "toggle b on");
        TEST_CHECK(led_selection_contains(s, &b), "b back");
        TEST_CHECK(led_selection_remove(s, &b) == LED_SUCCESS,
                   "remove b");
        TEST_CHECK(led_selection_clear(s) == LED_SUCCESS,
                   "clear");
        TEST_CHECK(led_selection_get(s, NULL, 0) == 0,
                   "empty after clear");
        /* Stale add rejected. */
        {
            le_object stale = LE_OBJECT_INVALID;

            TEST_CHECK(led_selection_add(s, &stale) ==
                           LED_ERROR_STALE_HANDLE,
                       "add invalid STALE");
            TEST_CHECK(led_selection_add(s, NULL) ==
                           LED_ERROR_INVALID_ARGUMENT,
                       "add NULL INVALID");
        }
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Stale/slot-reuse pruning proof. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object a = LE_OBJECT_INVALID;
        le_object b = LE_OBJECT_INVALID;
        le_object ghost;

        TEST_CHECK(make_session(&s, &e, &w), "make session 3");
        TEST_CHECK(le_object_create(w, &a) == LE_SUCCESS, "create a");
        TEST_CHECK(le_object_create(w, &b) == LE_SUCCESS, "create b");
        ghost = a;
        TEST_CHECK(led_selection_set(s, &a, 1) == LED_SUCCESS,
                   "select a");
        TEST_CHECK(le_object_destroy(w, &a) == LE_SUCCESS,
                   "destroy a");
        /* ghost handle is stale: contains false, get filters. */
        TEST_CHECK(!led_selection_contains(s, &ghost),
                   "stale not contained");
        TEST_CHECK(led_selection_get(s, NULL, 0) == 0,
                   "stale filtered from get");
        /* Slot reuse: successor must NOT alias the selection. */
        {
            le_object next = LE_OBJECT_INVALID;

            TEST_CHECK(le_object_create(w, &next) == LE_SUCCESS,
                       "slot reuse create");
            TEST_CHECK(next.index == ghost.index,
                       "slot reused (same index expected)");
            TEST_CHECK(next.generation != ghost.generation,
                       "generation bumped");
            TEST_CHECK(!led_selection_contains(s, &ghost),
                       "ghost still not contained");
            TEST_CHECK(led_selection_prune(s) == 0,
                       "prune drops ghost");
        }
        TEST_CHECK(led_selection_set(s, &b, 1) == LED_SUCCESS,
                   "select b");
        TEST_CHECK(le_object_destroy(w, &b) == LE_SUCCESS,
                   "destroy b");
        TEST_CHECK(led_session_tick(s, 0.016f) == LED_SUCCESS,
                   "tick prunes");
        TEST_CHECK(led_selection_get(s, NULL, 0) == 0,
                   "tick pruned stale");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Subtree + by-name selection. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object root = LE_OBJECT_INVALID;
        le_object k1 = LE_OBJECT_INVALID;
        le_object k2 = LE_OBJECT_INVALID;
        le_object gk = LE_OBJECT_INVALID;

        TEST_CHECK(make_session(&s, &e, &w), "make session 4");
        TEST_CHECK(le_object_create(w, &root) == LE_SUCCESS,
                   "root");
        TEST_CHECK(le_object_create(w, &k1) == LE_SUCCESS, "k1");
        TEST_CHECK(le_object_create(w, &k2) == LE_SUCCESS, "k2");
        TEST_CHECK(le_object_create(w, &gk) == LE_SUCCESS, "gk");
        TEST_CHECK(le_object_set_parent(w, &k1, &root) == LE_SUCCESS,
                   "k1 under root");
        TEST_CHECK(le_object_set_parent(w, &k2, &root) == LE_SUCCESS,
                   "k2 under root");
        TEST_CHECK(le_object_set_parent(w, &gk, &k1) == LE_SUCCESS,
                   "gk under k1");
        TEST_CHECK(le_object_set_name(w, &gk, "grandkid") ==
                       LE_SUCCESS,
                   "name gk");
        TEST_CHECK(led_selection_select_subtree(s, &root) ==
                       LED_SUCCESS,
                   "select subtree");
        TEST_CHECK(led_selection_get(s, NULL, 0) == 4,
                   "subtree size 4");
        TEST_CHECK(led_selection_select_by_name(s, "grandkid") ==
                       LED_SUCCESS,
                   "select by name");
        TEST_CHECK(led_selection_get(s, NULL, 0) == 1,
                   "by-name size 1");
        TEST_CHECK(led_selection_select_by_name(s, "nope") !=
                       LED_SUCCESS,
                   "by-name missing fails");
        TEST_CHECK(led_selection_select_by_name(s, NULL) ==
                       LED_ERROR_INVALID_ARGUMENT,
                   "by-name NULL INVALID");
        TEST_CHECK(led_selection_select_subtree(s, NULL) ==
                       LED_ERROR_INVALID_ARGUMENT,
                   "subtree NULL INVALID");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Hierarchy snapshot vs engine truth. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object r0 = LE_OBJECT_INVALID;
        le_object r1 = LE_OBJECT_INVALID;
        le_object ch = LE_OBJECT_INVALID;

        TEST_CHECK(make_session(&s, &e, &w), "make session 5");
        TEST_CHECK(led_hierarchy_refresh(s) == 0,
                   "empty hierarchy 0");
        TEST_CHECK(le_object_create(w, &r0) == LE_SUCCESS, "r0");
        TEST_CHECK(le_object_create(w, &r1) == LE_SUCCESS, "r1");
        TEST_CHECK(le_object_create(w, &ch) == LE_SUCCESS, "ch");
        TEST_CHECK(le_object_set_name(w, &r0, "alpha") == LE_SUCCESS,
                   "name alpha");
        TEST_CHECK(le_object_set_parent(w, &ch, &r0) == LE_SUCCESS,
                   "ch under r0");
        {
            uint32_t n = led_hierarchy_refresh(s);
            const led_hierarchy_node *nodes =
                led_hierarchy_nodes(s);

            TEST_CHECK(n == 3, "hierarchy 3 nodes");
            TEST_CHECK(nodes != NULL, "nodes non-NULL");
            TEST_CHECK(led_hierarchy_count(s) == 3,
                       "hier count 3");
            if (nodes != NULL && n == 3) {
                TEST_CHECK(nodes[0].depth == 0, "first root depth 0");
                TEST_CHECK(nodes[0].has_children, "r0 has kids");
                TEST_CHECK(strcmp(nodes[0].name, "alpha") == 0,
                           "borrowed name alpha");
                TEST_CHECK(nodes[1].depth == 1, "child depth 1");
                TEST_CHECK(nodes[2].depth == 0,
                           "second root depth 0");
            }
        }
        /* Destroy the child: refresh must shrink. */
        TEST_CHECK(le_object_destroy(w, &ch) == LE_SUCCESS,
                   "destroy ch");
        TEST_CHECK(led_hierarchy_refresh(s) == 2, "hier 2 after");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Inspector rows for plain + camera objects. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;
        le_object cam = LE_OBJECT_INVALID;
        le_camera_desc cd;

        memset(&cd, 0, sizeof(cd));
        TEST_CHECK(make_session(&s, &e, &w), "make session 6");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "obj");
        {
            uint32_t n = led_inspect(s, &o);

            TEST_CHECK(n >= 5, "plain rows >= 5");
            TEST_CHECK(led_inspector_count(s) == n,
                       "inspector count");
            TEST_CHECK(led_inspector_rows(s) != NULL,
                       "rows non-NULL");
        }
        TEST_CHECK(le_object_create(w, &cam) == LE_SUCCESS, "cam");
        le_camera_desc_default(&cd);
        TEST_CHECK(le_object_add_camera(w, &cam, &cd) == LE_SUCCESS,
                   "add camera");
        {
            uint32_t n = led_inspect(s, &cam);
            const led_inspector_row *rows =
                led_inspector_rows(s);
            int saw_fov = 0;
            uint32_t i;

            TEST_CHECK(n >= 10, "camera rows >= 10");
            for (i = 0; i < n; i++) {
                if (strcmp(rows[i].desc.path,
                           "camera.fov_y_deg") == 0 &&
                    rows[i].has_value) {
                    saw_fov = 1;
                }
            }
            TEST_CHECK(saw_fov, "camera fov row with value");
        }
        TEST_CHECK(led_inspect(s, NULL) == 0, "inspect NULL 0");
        {
            le_object stale = LE_OBJECT_INVALID;

            TEST_CHECK(led_inspect(s, &stale) == 0,
                       "inspect stale 0");
        }
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Console ring + overflow + script-error mirror (no error). */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;

        TEST_CHECK(make_session(&s, &e, &w), "make session 7");
        TEST_CHECK(led_console_push(s, LED_LOG_INFO, "test",
                                    "hello") == LED_SUCCESS,
                   "console push");
        TEST_CHECK(led_console_count(s) == 1, "console count 1");
        {
            led_log_level lv = LED_LOG_INFO;
            const char *tag = NULL;
            const char *msg = led_console_message(s, 0, &lv, &tag);

            TEST_CHECK(msg != NULL && strcmp(msg, "hello") == 0,
                       "console message");
            TEST_CHECK(lv == LED_LOG_INFO, "console level");
            TEST_CHECK(tag != NULL && strcmp(tag, "test") == 0,
                       "console tag");
        }
        TEST_CHECK(led_console_message(s, 7, NULL, NULL) == NULL,
                   "console oob NULL");
        TEST_CHECK(led_console_push(s, 99, "t", "m") ==
                       LED_ERROR_INVALID_ARGUMENT,
                   "console bad level INVALID");
        /* Overflow: push 1030 more; cap 1024 holds. */
        {
            uint32_t i;

            for (i = 0; i < 1030; i++) {
                led_console_push(s, LED_LOG_WARNING, "spam",
                                 "x");
            }
            TEST_CHECK(led_console_count(s) ==
                           LED_CONSOLE_CAPACITY,
                       "console capped");
        }
        led_console_clear(s);
        TEST_CHECK(led_console_count(s) == 0, "console cleared");
        TEST_CHECK(led_console_mirror_script_error(s) == 0,
                   "no script error to mirror");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Shortcuts table + focus policy. */
    {
        led_action a = LED_ACTION_COUNT;

        TEST_CHECK(led_shortcut_match(LE_KEY_Z, LE_MOD_CONTROL, &a) &&
                       a == LED_ACTION_UNDO,
                   "ctrl+z undo");
        TEST_CHECK(led_shortcut_match(LE_KEY_Y, LE_MOD_CONTROL, &a) &&
                       a == LED_ACTION_REDO,
                   "ctrl+y redo");
        TEST_CHECK(led_shortcut_match(LE_KEY_F5, LE_MOD_NONE, &a) &&
                       a == LED_ACTION_PLAY,
                   "f5 play");
        TEST_CHECK(led_shortcut_match(LE_KEY_F5, LE_MOD_SHIFT, &a) &&
                       a == LED_ACTION_STOP,
                   "shift+f5 stop");
        TEST_CHECK(led_shortcut_match(LE_KEY_S, LE_MOD_CONTROL, &a) &&
                       a == LED_ACTION_SAVE,
                   "ctrl+s save");
        TEST_CHECK(!led_shortcut_match(LE_KEY_Q, LE_MOD_NONE, &a),
                   "q unmatched");
        TEST_CHECK(!led_shortcut_match(LE_KEY_Z, LE_MOD_NONE, NULL),
                   "z alone unmatched");
    }
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;

        TEST_CHECK(make_session(&s, &e, &w), "make session 8");
        TEST_CHECK(led_focus_get(s) == 0, "focus default 0");
        led_focus_set(s, 1);
        TEST_CHECK(led_focus_get(s) == 1, "focus set 1");
        led_focus_set(NULL, 1);
        led_focus_set(s, 0);
        TEST_CHECK(led_focus_get(s) == 0, "focus back 0");
        /* Dispatch undo/redo on empty history: not handled. */
        TEST_CHECK(!led_dispatch_action(s, LED_ACTION_UNDO, NULL),
                   "dispatch undo empty 0");
        TEST_CHECK(!led_dispatch_action(s, LED_ACTION_COUNT, NULL),
                   "dispatch bad action 0");
        TEST_CHECK(!led_dispatch_action(NULL, LED_ACTION_UNDO,
                                        NULL),
                   "dispatch NULL 0");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Selection cap overflow. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;

        TEST_CHECK(make_session(&s, &e, &w), "make session 9");
        {
            /* Fill to cap via direct adds. */
            uint32_t i;
            int capped = 0;

            for (i = 0; i < LED_SELECTION_MAX + 4; i++) {
                le_object o = LE_OBJECT_INVALID;

                if (le_object_create(w, &o) != LE_SUCCESS) {
                    break;
                }
                if (led_selection_add(s, &o) ==
                    LED_ERROR_OVERFLOW) {
                    capped = 1;
                    break;
                }
            }
            TEST_CHECK(capped, "selection overflow reported");
            TEST_CHECK(led_selection_get(s, NULL, 0) ==
                           LED_SELECTION_MAX,
                       "selection at cap");
        }
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    printf("Luma Editor Phase 31 core headless tests: %d passed, %d "
           "failed\n",
           g_passed, g_failed);
    if (g_failed == 0) {
        printf("ALL PHASE 31 CORE HEADLESS TESTS PASSED\n");
    } else {
        printf("TESTS FAILED\n");
    }
    return (g_failed == 0) ? 0 : 1;
}
