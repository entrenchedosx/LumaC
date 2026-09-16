/* Phase 32 import/reimport/browser/delete tests: script import +
 * transactional failed reimport (last-known-good live), scene
 * discovery import, prefab discovery import, STALE via fingerprint
 * change, reimport-all, browser filter/search/select/inspect/drag,
 * delete ref-check (prefab referencing a script blocks deletion;
 * clean asset deletes). Headless, TEMP-backed. */

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

static void fresh_dir(const char *root) {
    static const char *kPrior[] = {
        "luma.project",
        "Assets/counter.lua",
        "Assets/counter.lua.luma",
        "Assets/extra.lua",
        "Assets/extra.lua.luma",
        "Assets/blob.bin",
        "Assets/withscript.luprefab",
        "Assets/withscript.luprefab.luma",
        "Scenes/mini.luma_scene",
        "Scenes/mini.luma_scene.luma",
        NULL,
    };
    int i;
    char p[2048];

    for (i = 0; kPrior[i] != NULL; i++) {
        snprintf(p, sizeof(p), "%s/%s", root, kPrior[i]);
        remove(p);
    }
    make_dir_one(root);
    snprintf(p, sizeof(p), "%s/Assets", root);
    make_dir_one(p);
    snprintf(p, sizeof(p), "%s/Scenes", root);
    make_dir_one(p);
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");

    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

static const char kCounterLua[] =
    "local M = {}\n"
    "export('n', 'number', 1)\n"
    "function M.start(self)\n"
    "end\n"
    "return M\n";

static const char kBrokenLua[] =
    "local M = {}\n"
    "function M.start(self)\n"
    "  this is not lua\n"
    "end\n"
    "return M\n";

int main(void) {
    led_session *s = NULL;
    le_engine *e = NULL;
    le_world *w = NULL;
    char root[1024];
    char spath[2048];
    const char *tmp = getenv("TEMP");

    if (tmp == NULL || tmp[0] == '\0') {
        tmp = getenv("TMP");
    }
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = ".";
    }
    snprintf(root, sizeof(root), "%s/luma32_import", tmp);
    fresh_dir(root);
    TEST_CHECK(make_session(&s, &e, &w), "make session");
    TEST_CHECK(led_project_create(root, "imp") == LED_SUCCESS,
               "project create");
    TEST_CHECK(led_project_open(s, root) == LED_SUCCESS,
               "project open");

    /* Importer registry sanity (pure surface). */
    TEST_CHECK(led_importer_count() == 5, "5 importers");
    TEST_CHECK(led_importer_for_extension("lua") != NULL,
               "lua importer known");
    TEST_CHECK(led_importer_for_extension("luprefab") != NULL,
               "prefab importer known");
    TEST_CHECK(led_importer_for_extension("zzz") == NULL,
               "unknown ext unmapped");

    /* Script source + scene source + unknown binary. */
    snprintf(spath, sizeof(spath), "%s/Assets/counter.lua", root);
    write_file(spath, kCounterLua);
    snprintf(spath, sizeof(spath), "%s/Scenes/mini.luma_scene",
             root);
    write_file(spath, "LUMA_SCENE 1\n");
    snprintf(spath, sizeof(spath), "%s/Assets/blob.bin", root);
    write_file(spath, "binary?");
    {
        led_scan_stats st;

        memset(&st, 0, sizeof(st));
        TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                   "scan sources");
        TEST_CHECK(led_assetdb_count(s) == 2,
                   "scene+script discovered (bin ignored)");
    }

    /* Import the script: READY + runtime handle. */
    TEST_CHECK(led_import_asset(s, "Assets/counter.lua") ==
                   LED_SUCCESS,
               "import script");
    {
        led_asset_record rec;

        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(led_assetdb_find_by_path(
                       s, "Assets/counter.lua", &rec),
                   "find script");
        TEST_CHECK(rec.status == LED_IMPORT_READY,
                   "script READY");
        TEST_CHECK(rec.has_runtime_asset, "script has handle");
        TEST_CHECK(rec.has_runtime_id, "script has runtime id");
    }

    /* Import the scene (discovery): READY, no runtime handle. */
    TEST_CHECK(led_import_asset(s, "Scenes/mini.luma_scene") ==
                   LED_SUCCESS,
               "import scene");
    {
        led_asset_record rec;

        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(led_assetdb_find_by_path(
                       s, "Scenes/mini.luma_scene", &rec),
                   "find scene");
        TEST_CHECK(rec.status == LED_IMPORT_READY,
                   "scene READY");
    }

    /* Failed reimport preserves last-known-good: break the
     * script source, force reimport, expect FAILED + old handle
     * still alive + READY-capable after repair. */
    {
        led_asset_record before;
        led_asset_record after;

        memset(&before, 0, sizeof(before));
        TEST_CHECK(led_assetdb_find_by_path(
                       s, "Assets/counter.lua", &before),
                   "snapshot good script");
        snprintf(spath, sizeof(spath), "%s/Assets/counter.lua",
                 root);
        write_file(spath, kBrokenLua);
        TEST_CHECK(led_reimport_asset(s, &before.id, 1) !=
                       LED_SUCCESS,
                   "broken reimport fails");
        memset(&after, 0, sizeof(after));
        TEST_CHECK(led_assetdb_find_by_path(
                       s, "Assets/counter.lua", &after),
                   "record survives failed reimport");
        TEST_CHECK(after.status == LED_IMPORT_FAILED,
                   "FAILED status");
        TEST_CHECK(after.has_runtime_asset,
                   "good handle preserved");
        TEST_CHECK(le_asset_is_alive(e, &after.runtime_asset),
                   "good handle live");
        TEST_CHECK(strcmp(after.id_hex, before.id_hex) == 0,
                   "project UUID stable across failure");
        /* Repair + forced reimport recovers. */
        write_file(spath, kCounterLua);
        TEST_CHECK(led_reimport_asset(s, &before.id, 1) ==
                       LED_SUCCESS,
                   "repaired reimport ok");
        memset(&after, 0, sizeof(after));
        TEST_CHECK(led_assetdb_find_by_path(
                       s, "Assets/counter.lua", &after),
                   "find after repair");
        TEST_CHECK(after.status == LED_IMPORT_READY,
                   "READY after repair");
    }

    /* STALE via fingerprint change + reimport-all. */
    {
        led_asset_record rec;

        snprintf(spath, sizeof(spath), "%s/Assets/extra.lua",
                 root);
        write_file(spath, kCounterLua);
        {
            led_scan_stats st;

            memset(&st, 0, sizeof(st));
            TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                       "scan extra");
        }
        TEST_CHECK(led_import_asset(s, "Assets/extra.lua") ==
                       LED_SUCCESS,
                   "import extra");
        /* Touch the source (comment append keeps it valid). */
        {
            FILE *f = fopen(spath, "a");

            if (f != NULL) {
                fputs("-- touch\n", f);
                fclose(f);
            }
        }
        {
            led_scan_stats st;

            memset(&st, 0, sizeof(st));
            TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                       "rescan touched");
        }
        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(led_assetdb_find_by_path(s, "Assets/extra.lua",
                                            &rec),
                   "find extra");
        TEST_CHECK(rec.status == LED_IMPORT_STALE,
                   "touched source STALE");
        {
            uint32_t ok = 0;

            TEST_CHECK(led_project_reimport_all(s, &ok) ==
                           LED_SUCCESS,
                       "reimport all ok");
            TEST_CHECK(ok >= 1, "reimport-all fixed stale");
        }
        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(led_assetdb_find_by_path(s, "Assets/extra.lua",
                                            &rec),
                   "find extra after");
        TEST_CHECK(rec.status == LED_IMPORT_READY,
                   "extra READY after reimport-all");
    }

    /* Browser: refresh + filter + search + select + inspect +
     * drag payload. */
    {
        uint32_t n = led_browser_refresh(s);

        TEST_CHECK(n >= 3, "browser has assets");
        TEST_CHECK(led_browser_folder_count(s) >= 1,
                   "browser has folders");
        TEST_CHECK(led_browser_set_filter(
                       s, LED_PROJECT_ASSET_SCRIPT, NULL, 0) ==
                       LED_SUCCESS,
                   "filter scripts");
        TEST_CHECK(led_browser_asset_count(s) == 2,
                   "two scripts in view");
        TEST_CHECK(led_browser_set_filter(
                       s, LED_PROJECT_ASSET_TYPE_COUNT,
                       "mini", 0) == LED_SUCCESS,
                   "search mini");
        TEST_CHECK(led_browser_asset_count(s) == 1,
                   "one search hit");
        TEST_CHECK(led_browser_set_filter(
                       s, LED_PROJECT_ASSET_TYPE_COUNT, NULL,
                       0) == LED_SUCCESS,
                   "clear filter");
        {
            led_asset_record rec;
            led_drag_payload pay;

            memset(&rec, 0, sizeof(rec));
            TEST_CHECK(led_assetdb_find_by_path(
                           s, "Assets/counter.lua", &rec),
                       "find for select");
            TEST_CHECK(led_browser_select(s, &rec.id) ==
                           LED_SUCCESS,
                       "select by UUID");
            TEST_CHECK(led_browser_get_selection(s, NULL, 0) ==
                           1,
                       "selection count 1");
            memset(&pay, 0, sizeof(pay));
            TEST_CHECK(led_drag_begin(s, &pay),
                       "drag begin single-select");
            TEST_CHECK(pay.type == LED_PROJECT_ASSET_SCRIPT,
                       "drag type script");
            TEST_CHECK(led_browser_inspect(s, &rec.id) > 0,
                       "inspect rows");
            TEST_CHECK(led_browser_clear_selection(s) ==
                           LED_SUCCESS,
                       "clear selection");
            TEST_CHECK(led_browser_get_selection(s, NULL, 0) ==
                           0,
                       "selection empty");
        }
    }

    /* Delete: unreferenced script deletes; then a prefab
     * referencing counter.lua blocks its deletion. */
    {
        /* Build prefab referencing the script: attach the
         * imported script to an object, create prefab. */
        le_object o = LE_OBJECT_INVALID;
        led_asset_record srec;

        memset(&srec, 0, sizeof(srec));
        TEST_CHECK(led_assetdb_find_by_path(
                       s, "Assets/counter.lua", &srec),
                   "find counter");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS,
                   "obj for prefab");
        TEST_CHECK(le_object_add_script(w, &o,
                                        &srec.runtime_asset) ==
                       LE_SUCCESS,
                   "attach script");
        TEST_CHECK(led_prefab_create(s, &o,
                                     "Assets/withscript.luprefab") ==
                       LED_SUCCESS,
                   "prefab with script");
        /* The prefab record must carry a dep edge to counter.lua
         * (create-time runtime-ID bridge). */
        {
            led_asset_record prec;
            uint32_t ndep = 0;

            memset(&prec, 0, sizeof(prec));
            TEST_CHECK(led_assetdb_find_by_path(
                           s, "Assets/withscript.luprefab", &prec),
                       "find prefab record");
            ndep = prec.dependency_count;
            TEST_CHECK(ndep >= 1, "prefab depends on script");
        }
        /* counter.lua is now referenced by the prefab file text
         * (script asset hex): deletion must be refused. */
        {
            led_result drc = led_project_delete(
                s, "Assets/counter.lua");

            TEST_CHECK(drc == LED_ERROR_VALIDATION,
                       "referenced script delete refused");
        }
        /* Unreferenced extra.lua deletes cleanly. */
        TEST_CHECK(led_project_delete(s, "Assets/extra.lua") ==
                       LED_SUCCESS,
                   "unreferenced delete ok");
        {
            led_asset_record gone;

            memset(&gone, 0, sizeof(gone));
            TEST_CHECK(!led_assetdb_find_by_path(
                           s, "Assets/extra.lua", &gone),
                       "deleted record gone");
        }
    }

    kill_session(s, e, w);
    printf("import: %d passed, %d failed\n", g_passed,
           g_failed);
    return (g_failed == 0) ? 0 : 1;
}
