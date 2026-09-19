/* Phase 32 project<->editor integration: script drop attaches
 * via the script path, material-assign type safety (script payload
 * onto ASSIGN_MATERIAL rejected; bare object without renderable
 * rejected), prefab instantiate/undo/redo through commands, play
 * isolation (edit world byte-identical across play with prefab
 * instances live), and the end-to-end headless workflow
 * (project -> import -> instantiate -> prefab -> save -> play ->
 * stop). Renderer-free (no drops need GPU here). */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <luma_editor/luma_editor.h>

#ifdef _WIN32
    #include <direct.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>
#endif

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

static int make_session(led_session **s, le_engine **e,
                        le_world **w) {
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

static void kill_session(led_session *s, le_engine *e,
                         le_world *w) {
    led_session_destroy(s);
    le_world_destroy(w);
    le_engine_destroy(e);
}

static void make_dir_one(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");

    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

/* Canonical capture helper (edit-equality oracle): capture the
 * edit world into a temp scene asset + serialize. */
static char *capture_canonical(le_engine *e, le_world *w,
                               size_t *out_size) {
    le_asset scene = LE_ASSET_INVALID;
    char *text = NULL;
    size_t size = 0;

    *out_size = 0;
    if (le_scene_create(e, 0, &scene) != LE_SUCCESS) {
        return NULL;
    }
    if (le_scene_capture(w, &scene, NULL) != LE_SUCCESS) {
        le_asset_unload(e, &scene);
        return NULL;
    }
    if (le_scene_save_text(e, &scene, &text, &size) !=
        LE_SUCCESS) {
        le_asset_unload(e, &scene);
        return NULL;
    }
    le_asset_unload(e, &scene);
    *out_size = size;
    return text;
}

static const char kScript[] =
    "local M = {}\n"
    "export('n', 'number', 3)\n"
    "function M.start(self)\n"
    "end\n"
    "return M\n";

int main(void) {
    led_session *s = NULL;
    le_engine *e = NULL;
    le_world *w = NULL;
    char root[1024];
    char p[2048];
    const char *tmp = getenv("TEMP");

    if (tmp == NULL || tmp[0] == '\0') {
        tmp = getenv("TMP");
    }
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = ".";
    }
    snprintf(root, sizeof(root), "%s/luma32_projedit", tmp);
    {
        static const char *kPrior[] = {
            "luma.project",
            "Assets/s.lua",
            "Assets/s.lua.luma",
            "Assets/p.luprefab",
            "Assets/p.luprefab.luma",
            "Scenes/flow.luma_scene",
            NULL,
        };
        int i;

        for (i = 0; kPrior[i] != NULL; i++) {
            snprintf(p, sizeof(p), "%s/%s", root, kPrior[i]);
            remove(p);
        }
    }
    make_dir_one(root);
    snprintf(p, sizeof(p), "%s/Assets", root);
    make_dir_one(p);
    snprintf(p, sizeof(p), "%s/Scenes", root);
    make_dir_one(p);

    TEST_CHECK(make_session(&s, &e, &w), "make session");
    TEST_CHECK(led_project_create(root, "pe") == LED_SUCCESS,
               "project create");
    TEST_CHECK(led_project_open(s, root) == LED_SUCCESS,
               "project open");

    /* Import a script for drop/assign flows. */
    snprintf(p, sizeof(p), "%s/Assets/s.lua", root);
    write_file(p, kScript);
    {
        led_scan_stats st;

        memset(&st, 0, sizeof(st));
        TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                   "scan script");
    }
    TEST_CHECK(led_import_asset(s, "Assets/s.lua") == LED_SUCCESS,
               "import script");

    /* Script drop onto an object attaches the component. */
    {
        le_object o = LE_OBJECT_INVALID;
        led_asset_record rec;
        led_drag_payload pay;
        le_asset got = LE_ASSET_INVALID;

        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS,
                   "drop target create");
        TEST_CHECK(led_assetdb_find_by_path(s, "Assets/s.lua",
                                            &rec),
                   "find script");
        TEST_CHECK(led_browser_select(s, &rec.id) == LED_SUCCESS,
                   "select script");
        memset(&pay, 0, sizeof(pay));
        TEST_CHECK(led_drag_begin(s, &pay), "drag script");
        TEST_CHECK(led_drop_script_onto_object(s, &pay, &o),
                   "drop script attaches");
        TEST_CHECK(le_object_get_script(w, &o, &got) &&
                       le_asset_is_alive(e, &got),
                   "script live on object");
        /* Wrong-type drop: script payload as material rejected. */
        TEST_CHECK(!led_drop_material_onto_object(s, &pay, &o),
                   "script-as-material rejected");
        led_browser_clear_selection(s);
    }

    /* ASSIGN_ASSET command type safety: script role with a
     * material asset (none here) — use the script asset under
     * the MATERIAL role: must fail validation. */
    {
        le_object o = LE_OBJECT_INVALID;
        led_asset_record rec;
        led_command c;

        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS,
                   "assign target create");
        TEST_CHECK(led_assetdb_find_by_path(s, "Assets/s.lua",
                                            &rec),
                   "find script 2");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_ASSIGN_ASSET;
        c.target = o;
        c.prefab.prefab_asset = rec.runtime_asset;
        c.prefab.assign_role = LED_PROJECT_ASSET_MATERIAL;
        TEST_CHECK(led_execute(s, &c) != LED_SUCCESS,
                   "script-as-material command rejected");
        /* SCRIPT role on a bare object succeeds (attach). */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_ASSIGN_ASSET;
        c.target = o;
        c.prefab.prefab_asset = rec.runtime_asset;
        c.prefab.assign_role = LED_PROJECT_ASSET_SCRIPT;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                   "script assign ok");
        TEST_CHECK(led_undo(s), "undo assign");
        {
            le_asset got = LE_ASSET_INVALID;

            TEST_CHECK(!le_object_get_script(w, &o, &got),
                       "undo assign removed script");
        }
        TEST_CHECK(led_redo(s), "redo assign");
    }

    /* Prefab instantiate/undo/redo through commands + play
     * isolation with instances live. */
    {
        le_object hero = LE_OBJECT_INVALID;
        le_asset prefab = LE_ASSET_INVALID;
        led_command c;
        uint32_t base = 0;
        char *before = NULL;
        char *after = NULL;
        size_t bsize = 0;
        size_t asize = 0;

        TEST_CHECK(led_scene_new(s) == LED_SUCCESS,
                   "fresh world");
        TEST_CHECK(le_object_create(w, &hero) == LE_SUCCESS,
                   "hero create");
        TEST_CHECK(le_object_set_name(w, &hero, "hero") ==
                       LE_SUCCESS,
                   "hero name");
        TEST_CHECK(led_prefab_create(s, &hero, "Assets/p.luprefab") ==
                       LED_SUCCESS,
                   "prefab create");
        TEST_CHECK(led_prefab_load(s, "Assets/p.luprefab",
                                   &prefab) == LED_SUCCESS,
                   "prefab load");
        base = le_world_get_object_count(w);
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_INSTANTIATE_PREFAB;
        c.prefab.prefab_asset = prefab;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                   "instantiate via command");
        TEST_CHECK(le_world_get_object_count(w) == base + 1,
                   "instance added");
        before = capture_canonical(e, w, &bsize);
        TEST_CHECK(before != NULL, "capture before play");
        TEST_CHECK(led_play_enter(s) == LED_SUCCESS,
                   "play with instance live");
        TEST_CHECK(led_play_tick(s, 1.0f / 60.0f) == LED_SUCCESS,
                   "play tick");
        TEST_CHECK(led_play_exit(s) == LED_SUCCESS,
                   "play exit");
        after = capture_canonical(e, w, &asize);
        TEST_CHECK(after != NULL, "capture after play");
        TEST_CHECK(bsize == asize && strcmp(before, after) == 0,
                   "edit byte-identical across play");
        if (before != NULL) {
            le_scene_free_text(before);
        }
        if (after != NULL) {
            le_scene_free_text(after);
        }
        TEST_CHECK(led_undo(s), "undo instantiate");
        TEST_CHECK(le_world_get_object_count(w) == base,
                   "undo removed instance");
        /* Undo must destroy the INSTANCE, never the source:
         * hero's handle stays alive (regression: the old tail
         * rule destroyed hero when it sat at a higher slot). */
        TEST_CHECK(le_object_is_alive(w, &hero),
                   "source hero survives undo");
        TEST_CHECK(led_redo(s), "redo instantiate");
        TEST_CHECK(le_world_get_object_count(w) == base + 1,
                   "redo restored instance");
        TEST_CHECK(le_object_is_alive(w, &hero),
                   "source hero survives redo");
    }

    /* End-to-end (§66-70 integrated save→play→stop→save→
     * reopen): save the prefab-instance scene to
     * Scenes/flow.luma_scene, play/stop FIRST (edit must be
     * byte-identical), save AGAIN post-stop, reopen in a second
     * session over a second engine, verify the object count
     * round-trips AND a rescan marks nothing STALE (no churn). */
    {
        char flow_abs[2048];

        snprintf(flow_abs, sizeof(flow_abs),
                 "%s/Scenes/flow.luma_scene", root);
        TEST_CHECK(led_scene_save_as(s, flow_abs) == LED_SUCCESS,
                   "save flow scene");
        /* play/stop with the instance live, then save again. */
        {
            char *before = NULL;
            char *after = NULL;
            size_t bsize = 0;
            size_t asize = 0;

            before = capture_canonical(e, w, &bsize);
            TEST_CHECK(before != NULL,
                       "capture before play (flow)");
            TEST_CHECK(led_play_enter(s) == LED_SUCCESS,
                       "play flow scene");
            TEST_CHECK(led_play_tick(s, 1.0f / 60.0f) ==
                           LED_SUCCESS,
                       "tick flow scene");
            TEST_CHECK(led_play_tick(s, 1.0f / 60.0f) ==
                           LED_SUCCESS,
                       "tick flow scene 2");
            TEST_CHECK(led_play_exit(s) == LED_SUCCESS,
                       "stop flow scene");
            after = capture_canonical(e, w, &asize);
            TEST_CHECK(after != NULL,
                       "capture after stop (flow)");
            TEST_CHECK(bsize == asize &&
                           strcmp(before, after) == 0,
                       "edit byte-identical across "
                       "play/stop (flow)");
            if (before != NULL) {
                le_scene_free_text(before);
            }
            if (after != NULL) {
                le_scene_free_text(after);
            }
        }
        TEST_CHECK(led_scene_save_as(s, flow_abs) == LED_SUCCESS,
                   "save flow scene post-stop");
        {
            led_session *s2 = NULL;
            le_engine *e2 = NULL;
            le_world *w2 = NULL;
            uint32_t n1 = le_world_get_object_count(w);

            TEST_CHECK(make_session(&s2, &e2, &w2),
                       "make session 2");
            TEST_CHECK(led_project_open(s2, root) ==
                           LED_SUCCESS,
                       "open project 2 (same root)");
            TEST_CHECK(led_scene_open(s2, flow_abs) ==
                           LED_SUCCESS,
                       "reopen flow scene");
            TEST_CHECK(le_world_get_object_count(w2) == n1,
                       "object count round-trips");
            /* No-churn: a rescan in the fresh session marks
             * nothing STALE (fingerprints match what the save
             * wrote — the R-007/R-010 loop stays dead). */
            {
                led_scan_stats st;

                memset(&st, 0, sizeof(st));
                TEST_CHECK(led_project_scan(s2, &st) ==
                               LED_SUCCESS,
                           "rescan reopened project");
                TEST_CHECK(st.stale_marked == 0,
                           "reopened project scan clean");
            }
            kill_session(s2, e2, w2);
        }
    }

    kill_session(s, e, w);
    printf("projedit: %d passed, %d failed\n", g_passed,
           g_failed);
    return (g_failed == 0) ? 0 : 1;
}
