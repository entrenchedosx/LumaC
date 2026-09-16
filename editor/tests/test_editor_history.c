/*
 * Luma Editor Phase 31 headless tests: commands, undo/redo,
 * inverses, bounds eviction, coalescing, failed-execute-no-push,
 * redo-clear, 10k stress with memory stats.
 *
 * No GPU, no window, no renderer.
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

static void make_create_cmd(led_command *c, const char *name) {
    memset(c, 0, sizeof(*c));
    c->kind = LED_CMD_CREATE;
    snprintf(c->label, sizeof(c->label), "Create");
    if (name != NULL) {
        strncpy(c->name_value, name, sizeof(c->name_value) - 1);
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Luma Editor Phase 31 history headless tests...\n");

    /* NULL + bad-kind guards. */
    TEST_CHECK(led_execute(NULL, NULL) == LED_ERROR_INVALID_ARGUMENT,
               "execute NULL INVALID");
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        led_command bad;

        TEST_CHECK(make_session(&s, &e, &w), "make session");
        memset(&bad, 0, sizeof(bad));
        bad.kind = LED_CMD_KIND_COUNT;
        TEST_CHECK(led_execute(s, &bad) == LED_ERROR_INVALID_ARGUMENT,
                   "bad kind INVALID");
        TEST_CHECK(led_execute(s, NULL) == LED_ERROR_INVALID_ARGUMENT,
                   "NULL command INVALID");
        TEST_CHECK(led_history_set_capacity(s, 0) ==
                       LED_ERROR_INVALID_ARGUMENT,
                   "cap 0 INVALID");
        TEST_CHECK(led_history_set_capacity(NULL, 8) ==
                       LED_ERROR_INVALID_ARGUMENT,
                   "cap NULL INVALID");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Create execute/undo/redo + dirty transitions. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        led_command c;

        TEST_CHECK(make_session(&s, &e, &w), "make session 2");
        TEST_CHECK(!led_is_dirty(s), "clean at start");
        make_create_cmd(&c, "hero");
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS, "create hero");
        TEST_CHECK(led_is_dirty(s), "dirty after execute");
        TEST_CHECK(le_world_get_object_count(w) == 1, "one object");
        TEST_CHECK(led_undo(s) == 1, "undo create");
        TEST_CHECK(le_world_get_object_count(w) == 0,
                   "zero after undo");
        TEST_CHECK(led_redo(s) == 1, "redo create");
        TEST_CHECK(le_world_get_object_count(w) == 1,
                   "one after redo");
        TEST_CHECK(!led_undo(NULL), "undo NULL 0");
        TEST_CHECK(!led_redo(NULL), "redo NULL 0");
        /* Undo then new execute clears redo. */
        TEST_CHECK(led_undo(s) == 1, "undo again");
        TEST_CHECK(led_redo(s) == 1, "redo again");
        TEST_CHECK(led_undo(s) == 1, "undo third");
        make_create_cmd(&c, "second");
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                   "new execute clears redo");
        TEST_CHECK(!led_redo(s), "redo empty after new execute");
        TEST_CHECK(!led_undo(NULL), "undo NULL 0");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Set-name/enabled/position/rotation/scale round-trips. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;
        led_command c;

        TEST_CHECK(make_session(&s, &e, &w), "make session 3");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "create o");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_SET_NAME;
        snprintf(c.label, sizeof(c.label), "Rename");
        c.target = o;
        strncpy(c.name_value, "renamed", sizeof(c.name_value) - 1);
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS, "rename");
        TEST_CHECK(strcmp(le_object_get_name(w, &o), "renamed") == 0,
                   "name applied");
        TEST_CHECK(led_undo(s) == 1, "undo rename");
        TEST_CHECK(strcmp(le_object_get_name(w, &o), "") == 0,
                   "name reverted");
        TEST_CHECK(led_redo(s) == 1, "redo rename");
        TEST_CHECK(strcmp(le_object_get_name(w, &o), "renamed") == 0,
                   "name redone");
        /* Enabled. */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_SET_ENABLED;
        c.target = o;
        c.enabled_value = 0;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS, "disable");
        TEST_CHECK(!le_object_is_enabled(w, &o), "disabled");
        TEST_CHECK(led_undo(s) == 1, "undo disable");
        TEST_CHECK(le_object_is_enabled(w, &o), "reenabled");
        /* Position. */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_SET_POSITION;
        c.target = o;
        c.vec_value[0] = 1.0f;
        c.vec_value[1] = 2.0f;
        c.vec_value[2] = 3.0f;
        TEST_CHECK(led_history_set_coalesce(s, 0, 0) == LED_SUCCESS,
                   "coalesce off");
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS, "move");
        {
            float p[3];

            le_object_get_position(w, &o, p);
            TEST_CHECK(p[0] == 1.0f && p[1] == 2.0f &&
                           p[2] == 3.0f,
                       "position applied");
        }
        TEST_CHECK(led_undo(s) == 1, "undo move");
        {
            float p[3];

            le_object_get_position(w, &o, p);
            TEST_CHECK(p[0] == 0.0f && p[1] == 0.0f &&
                           p[2] == 0.0f,
                       "position reverted");
        }
        TEST_CHECK(led_redo(s) == 1, "redo move");
        /* Rotation + scale. */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_SET_ROTATION;
        c.target = o;
        c.vec_value[0] = 0.0f;
        c.vec_value[1] = 0.0f;
        c.vec_value[2] = 0.0f;
        c.vec_value[3] = 1.0f;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS, "rotate");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_SET_SCALE;
        c.target = o;
        c.vec_value[0] = 2.0f;
        c.vec_value[1] = 2.0f;
        c.vec_value[2] = 2.0f;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS, "scale");
        {
            float sc[3];

            le_object_get_scale(w, &o, sc);
            TEST_CHECK(sc[0] == 2.0f, "scale applied");
        }
        TEST_CHECK(led_undo(s) == 1, "undo scale");
        TEST_CHECK(led_undo(s) == 1, "undo rotate");
        TEST_CHECK(led_redo(s) == 1, "redo rotate");
        TEST_CHECK(led_redo(s) == 1, "redo scale");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Failed execute pushes nothing (stale target). */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        led_command c;
        led_history_stats st;

        memset(&st, 0, sizeof(st));
        TEST_CHECK(make_session(&s, &e, &w), "make session 4");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_SET_NAME;
        c.target = LE_OBJECT_INVALID;
        strncpy(c.name_value, "x", sizeof(c.name_value) - 1);
        TEST_CHECK(led_execute(s, &c) == LED_ERROR_STALE_HANDLE,
                   "stale execute STALE");
        led_history_get_stats(s, &st);
        TEST_CHECK(st.undo_depth == 0 && st.commands_pushed == 0,
                   "nothing pushed on failure");
        TEST_CHECK(!led_is_dirty(s), "not dirty after failure");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Delete subtree undo restores name/TRS (fresh handles). */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object root = LE_OBJECT_INVALID;
        le_object kid = LE_OBJECT_INVALID;
        led_command c;

        TEST_CHECK(make_session(&s, &e, &w), "make session 5");
        TEST_CHECK(le_object_create(w, &root) == LE_SUCCESS,
                   "root");
        TEST_CHECK(le_object_create(w, &kid) == LE_SUCCESS, "kid");
        TEST_CHECK(le_object_set_name(w, &root, "delroot") ==
                       LE_SUCCESS,
                   "name root");
        {
            float p[3] = { 4.0f, 5.0f, 6.0f };

            TEST_CHECK(le_object_set_position(w, &kid, p) ==
                           LE_SUCCESS,
                       "kid pos");
        }
        TEST_CHECK(le_object_set_parent(w, &kid, &root) ==
                       LE_SUCCESS,
                   "kid under root");
        TEST_CHECK(led_history_set_coalesce(s, 0, 0) == LED_SUCCESS,
                   "coalesce off 2");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_DELETE;
        snprintf(c.label, sizeof(c.label), "Delete");
        c.target = root;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS, "delete root");
        TEST_CHECK(le_world_get_object_count(w) == 0,
                   "both gone");
        TEST_CHECK(led_undo(s) == 1, "undo delete");
        TEST_CHECK(le_world_get_object_count(w) == 2,
                   "both restored");
        {
            le_object found = LE_OBJECT_INVALID;

            TEST_CHECK(le_world_find_by_name(w, "delroot", &found),
                       "restored name searchable");
            TEST_CHECK(le_object_get_child_count(w, &found) == 1,
                       "restored hierarchy");
            {
                le_object kk = LE_OBJECT_INVALID;
                float p[3];

                TEST_CHECK(le_object_get_children(w, &found, &kk,
                                                  1, NULL) ==
                               LE_SUCCESS,
                           "get restored kid");
                le_object_get_position(w, &kk, p);
                TEST_CHECK(p[0] == 4.0f && p[1] == 5.0f &&
                               p[2] == 6.0f,
                           "restored TRS");
            }
        }
        TEST_CHECK(led_redo(s) == 1, "redo delete");
        TEST_CHECK(le_world_get_object_count(w) == 0,
                   "gone again");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Reparent undo/redo + cycle rejection pushes nothing. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object a = LE_OBJECT_INVALID;
        le_object b = LE_OBJECT_INVALID;
        led_command c;
        led_history_stats st;

        memset(&st, 0, sizeof(st));
        TEST_CHECK(make_session(&s, &e, &w), "make session 6");
        TEST_CHECK(le_object_create(w, &a) == LE_SUCCESS, "a");
        TEST_CHECK(le_object_create(w, &b) == LE_SUCCESS, "b");
        TEST_CHECK(led_history_set_coalesce(s, 0, 0) == LED_SUCCESS,
                   "coalesce off 3");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_SET_PARENT;
        c.target = b;
        c.parent = a;
        c.has_parent = 1;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS, "parent b->a");
        {
            le_object p = LE_OBJECT_INVALID;

            TEST_CHECK(le_object_get_parent(w, &b, &p), "has parent");
            TEST_CHECK(p.index == a.index, "parent is a");
        }
        TEST_CHECK(led_undo(s) == 1, "undo reparent");
        {
            le_object p = LE_OBJECT_INVALID;

            TEST_CHECK(!le_object_get_parent(w, &b, &p),
                       "root again");
        }
        /* Cycle: a under b ok, then b under... build a->b then try
         * b as parent of a (a is ancestor of b? no: a is parent of
         * b after redo). Redo first, then attempt cycle a<-b<-a. */
        TEST_CHECK(led_redo(s) == 1, "redo reparent");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_SET_PARENT;
        c.target = a;
        c.parent = b;
        c.has_parent = 1;
        led_history_get_stats(s, &st);
        {
            uint32_t pushed = (uint32_t)st.commands_pushed;

            TEST_CHECK(led_execute(s, &c) != LED_SUCCESS,
                       "cycle rejected");
            led_history_get_stats(s, &st);
            TEST_CHECK(st.commands_pushed == pushed,
                       "cycle pushed nothing");
        }
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Component add/remove inverse. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;
        led_command c;
        le_camera_desc cd;

        memset(&cd, 0, sizeof(cd));
        TEST_CHECK(make_session(&s, &e, &w), "make session 7");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "o");
        le_camera_desc_default(&cd);
        TEST_CHECK(led_history_set_coalesce(s, 0, 0) == LED_SUCCESS,
                   "coalesce off 4");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_ADD_COMPONENT;
        c.target = o;
        c.component = LE_COMPONENT_CAMERA;
        memcpy(c.comp_bytes, &cd, sizeof(cd));
        c.comp_size = (uint32_t)sizeof(cd);
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS, "add camera");
        TEST_CHECK(le_object_has_component(w, &o,
                                           LE_COMPONENT_CAMERA),
                   "camera present");
        TEST_CHECK(led_undo(s) == 1, "undo add camera");
        TEST_CHECK(!le_object_has_component(w, &o,
                                            LE_COMPONENT_CAMERA),
                   "camera gone");
        TEST_CHECK(led_redo(s) == 1, "redo add camera");
        TEST_CHECK(le_object_has_component(w, &o,
                                           LE_COMPONENT_CAMERA),
                   "camera back");
        /* Remove with before-image restore. */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_REMOVE_COMPONENT;
        c.target = o;
        c.component = LE_COMPONENT_CAMERA;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                   "remove camera");
        TEST_CHECK(!le_object_has_component(w, &o,
                                            LE_COMPONENT_CAMERA),
                   "camera removed");
        TEST_CHECK(led_undo(s) == 1, "undo remove camera");
        TEST_CHECK(le_object_has_component(w, &o,
                                           LE_COMPONENT_CAMERA),
                   "camera restored");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Bounds: small cap evicts oldest. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        led_command c;
        led_history_stats st;

        memset(&st, 0, sizeof(st));
        TEST_CHECK(make_session(&s, &e, &w), "make session 8");
        TEST_CHECK(led_history_set_capacity(s, 4) == LED_SUCCESS,
                   "cap 4");
        TEST_CHECK(led_history_set_coalesce(s, 0, 0) == LED_SUCCESS,
                   "coalesce off 5");
        {
            int i;

            for (i = 0; i < 6; i++) {
                char nm[32];

                snprintf(nm, sizeof(nm), "n%d", i);
                make_create_cmd(&c, nm);
                TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                           "create burst");
            }
        }
        led_history_get_stats(s, &st);
        TEST_CHECK(st.undo_depth == 4, "depth capped at 4");
        TEST_CHECK(st.commands_evicted == 2, "evicted 2");
        TEST_CHECK(st.bytes_estimate > 0, "bytes estimate");
        TEST_CHECK(le_world_get_object_count(w) == 6, "6 objects");
        /* Undo 4 (all available), 5th fails. */
        TEST_CHECK(led_undo(s) == 1, "undo b1");
        TEST_CHECK(led_undo(s) == 1, "undo b2");
        TEST_CHECK(led_undo(s) == 1, "undo b3");
        TEST_CHECK(led_undo(s) == 1, "undo b4");
        TEST_CHECK(!led_undo(s), "undo empty 0");
        TEST_CHECK(le_world_get_object_count(w) == 2, "2 remain");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Coalescing: rapid same-target TRS merges to one step. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;
        led_command c;
        led_history_stats st;

        memset(&st, 0, sizeof(st));
        TEST_CHECK(make_session(&s, &e, &w), "make session 9");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "o");
        TEST_CHECK(led_history_set_coalesce(s, 1, 60000) ==
                       LED_SUCCESS,
                   "coalesce on wide window");
        {
            int i;

            for (i = 1; i <= 5; i++) {
                memset(&c, 0, sizeof(c));
                c.kind = LED_CMD_SET_POSITION;
                c.target = o;
                c.vec_value[0] = (float)i;
                TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                           "drag step");
            }
        }
        led_history_get_stats(s, &st);
        TEST_CHECK(st.undo_depth == 1, "drag coalesced to 1");
        TEST_CHECK(st.coalesced == 4, "coalesced count 4");
        {
            float p[3];

            le_object_get_position(w, &o, p);
            TEST_CHECK(p[0] == 5.0f, "final drag value");
        }
        TEST_CHECK(led_undo(s) == 1, "undo drag");
        {
            float p[3];

            le_object_get_position(w, &o, p);
            TEST_CHECK(p[0] == 0.0f, "drag fully reverted");
        }
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* 10k command stress: push, undo all, redo all, byte-exact. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;
        led_command c;
        led_history_stats st;

        memset(&st, 0, sizeof(st));
        TEST_CHECK(make_session(&s, &e, &w), "make session 10");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "o");
        TEST_CHECK(led_history_set_capacity(s, 20000) ==
                       LED_SUCCESS,
                   "cap 20k");
        TEST_CHECK(led_history_set_coalesce(s, 0, 0) == LED_SUCCESS,
                   "coalesce off stress");
        {
            int i;

            for (i = 1; i <= 10000; i++) {
                memset(&c, 0, sizeof(c));
                c.kind = LED_CMD_SET_POSITION;
                c.target = o;
                c.vec_value[0] = (float)i;
                c.vec_value[1] = (float)(i * 2);
                c.vec_value[2] = 0.0f;
                if (led_execute(s, &c) != LED_SUCCESS) {
                    break;
                }
            }
        }
        led_history_get_stats(s, &st);
        TEST_CHECK(st.undo_depth == 10000, "10k pushed");
        TEST_CHECK(st.bytes_estimate > 0, "stress bytes");
        {
            int i;

            for (i = 0; i < 10000; i++) {
                if (!led_undo(s)) {
                    break;
                }
            }
            TEST_CHECK(i == 10000, "10k undone");
        }
        {
            float p[3];

            le_object_get_position(w, &o, p);
            TEST_CHECK(p[0] == 0.0f && p[1] == 0.0f,
                       "back at origin");
        }
        {
            int i;

            for (i = 0; i < 10000; i++) {
                if (!led_redo(s)) {
                    break;
                }
            }
            TEST_CHECK(i == 10000, "10k redone");
        }
        {
            float p[3];

            le_object_get_position(w, &o, p);
            TEST_CHECK(p[0] == 10000.0f && p[1] == 20000.0f,
                       "byte-exact final");
        }
        led_history_get_stats(s, &st);
        TEST_CHECK(st.undos == 10000 && st.redos == 10000,
                   "undo/redo counters");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* history_clear + KEEP_WORLD reparent command. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object a = LE_OBJECT_INVALID;
        le_object b = LE_OBJECT_INVALID;
        led_command c;
        led_history_stats st;

        memset(&st, 0, sizeof(st));
        TEST_CHECK(make_session(&s, &e, &w), "make session 11");
        TEST_CHECK(le_object_create(w, &a) == LE_SUCCESS, "a");
        TEST_CHECK(le_object_create(w, &b) == LE_SUCCESS, "b");
        TEST_CHECK(led_history_set_coalesce(s, 0, 0) == LED_SUCCESS,
                   "coalesce off 6");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_REPARENT;
        c.target = b;
        c.parent = a;
        c.has_parent = 1;
        c.reparent_mode = LED_REPARENT_KEEP_WORLD;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                   "keep-world reparent");
        TEST_CHECK(led_undo(s) == 1, "undo keep-world");
        led_history_clear(s);
        led_history_get_stats(s, &st);
        TEST_CHECK(st.undo_depth == 0 && st.redo_depth == 0,
                   "history cleared");
        TEST_CHECK(!led_undo(s), "undo after clear 0");
        led_history_clear(NULL);
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    printf("Luma Editor Phase 31 history headless tests: %d passed, "
           "%d failed\n",
           g_passed, g_failed);
    if (g_failed == 0) {
        printf("ALL PHASE 31 HISTORY HEADLESS TESTS PASSED\n");
    } else {
        printf("TESTS FAILED\n");
    }
    return (g_failed == 0) ? 0 : 1;
}
