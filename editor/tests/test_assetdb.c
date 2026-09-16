/* Phase 32 asset-database tests: deterministic enumeration
 * order, lookup by UUID/path, type filter counts, case-insensitive
 * search + type filter, dependency/dependent edges, DB stats,
 * duplicate-UUID conflict marking, malformed sidecar isolation
 * (one bad sidecar degrades one record, scan continues).
 * Headless, TEMP-backed. */

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
    snprintf(root, sizeof(root), "%s/luma32_assetdb", tmp);
    /* Reset known files. */
    {
        static const char *kPrior[] = {
            "luma.project",
            "Scenes/zeta.luma_scene",
            "Scenes/zeta.luma_scene.luma",
            "Scenes/alpha.luma_scene",
            "Scenes/alpha.luma_scene.luma",
            "Scenes/mid.luma_scene",
            "Scenes/mid.luma_scene.luma",
            "Scenes/victim.luma_scene",
            "Scenes/victim.luma_scene.luma",
            "Assets/a.lua",
            "Assets/a.lua.luma",
            "Assets/b.lua",
            "Assets/b.lua.luma",
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
    TEST_CHECK(led_project_create(root, "db") == LED_SUCCESS,
               "project create");
    TEST_CHECK(led_project_open(s, root) == LED_SUCCESS,
               "project open");

    /* Create out of order: zeta, alpha, mid (scenes) + a/b
     * (scripts). Enumeration must be path-sorted regardless. */
    snprintf(p, sizeof(p), "%s/Scenes/zeta.luma_scene", root);
    write_file(p, "LUMA_SCENE 1\n");
    snprintf(p, sizeof(p), "%s/Scenes/alpha.luma_scene", root);
    write_file(p, "LUMA_SCENE 1\n");
    snprintf(p, sizeof(p), "%s/Scenes/mid.luma_scene", root);
    write_file(p, "LUMA_SCENE 1\n");
    snprintf(p, sizeof(p), "%s/Assets/a.lua", root);
    write_file(p, "return {}\n");
    snprintf(p, sizeof(p), "%s/Assets/b.lua", root);
    write_file(p, "return {}\n");
    {
        led_scan_stats st;

        memset(&st, 0, sizeof(st));
        TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                   "scan 5 sources");
        TEST_CHECK(led_assetdb_count(s) == 5, "5 records");
    }

    /* Deterministic path-sorted enumeration. */
    {
        static const char *kWant[] = {
            "Assets/a.lua",
            "Assets/b.lua",
            "Scenes/alpha.luma_scene",
            "Scenes/mid.luma_scene",
            "Scenes/zeta.luma_scene",
        };
        uint32_t i;
        int ordered = 1;

        for (i = 0; i < 5; i++) {
            led_asset_record rec;

            memset(&rec, 0, sizeof(rec));
            if (!led_assetdb_get(s, i, &rec) ||
                strcmp(rec.source_path, kWant[i]) != 0) {
                ordered = 0;
                break;
            }
        }
        TEST_CHECK(ordered, "enumeration path-sorted");
        TEST_CHECK(!led_assetdb_get(s, 5, &(led_asset_record){
                                       0 }),
                   "get out-of-range fails");
    }

    /* Lookup by UUID round-trips to the same path. */
    {
        led_asset_record a;
        led_asset_record b;

        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        TEST_CHECK(led_assetdb_find_by_path(s, "Scenes/mid.luma_scene",
                                            &a),
                   "find mid by path");
        TEST_CHECK(led_assetdb_find_by_id(s, &a.id, &b),
                   "find mid by UUID");
        TEST_CHECK(strcmp(b.source_path,
                          "Scenes/mid.luma_scene") == 0,
                   "UUID round-trips to path");
    }

    /* Type filter counts. */
    TEST_CHECK(led_assetdb_filter(s, LED_PROJECT_ASSET_SCENE) ==
                   3,
               "3 scenes");
    TEST_CHECK(led_assetdb_filter(s, LED_PROJECT_ASSET_SCRIPT) ==
                   2,
               "2 scripts");
    TEST_CHECK(led_assetdb_filter(
                   s, LED_PROJECT_ASSET_TYPE_COUNT) == 5,
               "COUNT counts all");

    /* Case-insensitive search + type filter + counting query. */
    {
        uint32_t total = 0;
        led_project_asset_id ids[8];
        uint32_t n = 0;

        n = led_assetdb_search(s, "ALPHA",
                               LED_PROJECT_ASSET_TYPE_COUNT, ids,
                               8, &total);
        TEST_CHECK(n == 1 && total == 1,
                   "case-insensitive search hits");
        n = led_assetdb_search(s, ".lua", LED_PROJECT_ASSET_SCRIPT,
                               ids, 8, &total);
        TEST_CHECK(n == 2 && total == 2,
                   "search + script filter");
        n = led_assetdb_search(s, ".lua", LED_PROJECT_ASSET_SCENE,
                               ids, 8, &total);
        TEST_CHECK(n == 0 && total == 0,
                   "search + scene filter empty");
        /* Capacity clamp: reports full count, fills capacity. */
        n = led_assetdb_search(s, "", LED_PROJECT_ASSET_TYPE_COUNT,
                               ids, 2, &total);
        TEST_CHECK(n == 5 && total == 5,
                   "empty query matches all (counting)");
        /* Deterministic order: path-sorted UUIDs. */
        {
            led_asset_record r0;
            led_asset_record r1;

            memset(&r0, 0, sizeof(r0));
            memset(&r1, 0, sizeof(r1));
            led_assetdb_find_by_id(s, &ids[0], &r0);
            led_assetdb_find_by_id(s, &ids[1], &r1);
            TEST_CHECK(strcmp(r0.source_path,
                              r1.source_path) < 0,
                       "search order deterministic");
        }
    }

    /* Dependency edges: none yet (no prefab) — empty but valid. */
    {
        led_asset_record a;

        memset(&a, 0, sizeof(a));
        TEST_CHECK(led_assetdb_find_by_path(s, "Assets/a.lua", &a),
                   "find a.lua");
        TEST_CHECK(led_assetdb_dependencies(s, &a.id, NULL, 0) ==
                       0,
                   "no dependencies yet");
        TEST_CHECK(led_assetdb_dependents(s, &a.id, NULL, 0) ==
                       0,
                   "no dependents yet");
    }

    /* DB stats shape. */
    {
        led_assetdb_stats st;

        memset(&st, 0, sizeof(st));
        led_assetdb_get_stats(s, &st);
        TEST_CHECK(st.records == 5, "stats record count");
        TEST_CHECK(st.by_type[LED_PROJECT_ASSET_SCENE] == 3,
                   "stats scenes");
        TEST_CHECK(st.by_type[LED_PROJECT_ASSET_SCRIPT] == 2,
                   "stats scripts");
    }

    /* Duplicate-UUID conflict: clone alpha's sidecar UUID onto a
     * victim file's sidecar -> next scan marks BOTH failed. */
    {
        char alpha_side[2048];
        char victim_side[2048];
        char idline[128] = {0};
        FILE *f = NULL;

        snprintf(p, sizeof(p), "%s/Scenes/victim.luma_scene",
                 root);
        write_file(p, "LUMA_SCENE 1\n");
        {
            led_scan_stats st;

            memset(&st, 0, sizeof(st));
            TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                       "scan victim");
        }
        snprintf(alpha_side, sizeof(alpha_side),
                 "%s/Scenes/alpha.luma_scene.luma", root);
        snprintf(victim_side, sizeof(victim_side),
                 "%s/Scenes/victim.luma_scene.luma", root);
        f = fopen(alpha_side, "r");
        TEST_CHECK(f != NULL, "alpha sidecar readable");
        if (f != NULL) {
            char line[256];

            while (fgets(line, sizeof(line), f) != NULL) {
                if (strncmp(line, "id ", 3) == 0) {
                    strncpy(idline, line, sizeof(idline) - 1);
                    break;
                }
            }
            fclose(f);
        }
        TEST_CHECK(idline[0] != '\0', "alpha UUID line");
        /* Rewrite victim sidecar with alpha's UUID. */
        {
            FILE *vf = fopen(victim_side, "r");
            char rest[4096];
            size_t rn = 0;

            memset(rest, 0, sizeof(rest));
            if (vf != NULL) {
                char line[256];
                int first = 1;

                while (fgets(line, sizeof(line), vf) != NULL) {
                    if (first) {
                        first = 0;
                        continue; /* magic re-emitted below */
                    }
                    if (strncmp(line, "id ", 3) == 0) {
                        continue; /* replaced */
                    }
                    if (rn + strlen(line) + 1 < sizeof(rest)) {
                        memcpy(rest + rn, line, strlen(line));
                        rn += strlen(line);
                    }
                }
                fclose(vf);
            }
            vf = fopen(victim_side, "w");
            TEST_CHECK(vf != NULL, "victim sidecar writable");
            if (vf != NULL) {
                fputs("LUMA_ASSET 1\n", vf);
                fputs(idline, vf);
                fputs(rest, vf);
                fclose(vf);
            }
        }
        {
            led_scan_stats st;

            memset(&st, 0, sizeof(st));
            TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                       "scan with duplicate UUID");
        }
        {
            led_asset_record ra;
            led_asset_record rv;

            memset(&ra, 0, sizeof(ra));
            memset(&rv, 0, sizeof(rv));
            TEST_CHECK(led_assetdb_find_by_path(
                           s, "Scenes/alpha.luma_scene", &ra),
                       "find alpha");
            TEST_CHECK(led_assetdb_find_by_path(
                           s, "Scenes/victim.luma_scene", &rv),
                       "find victim");
            TEST_CHECK(ra.status == LED_IMPORT_FAILED &&
                           rv.status == LED_IMPORT_FAILED,
                       "duplicate UUID marks both FAILED");
        }
    }

    /* Malformed sidecar isolation: corrupt mid's sidecar magic;
     * scan continues (zeta still fine), mid gets a diagnostic
     * (corrupt -> keeps discovery identity, error counted). */
    {
        char mid_side[2048];

        snprintf(mid_side, sizeof(mid_side),
                 "%s/Scenes/mid.luma_scene.luma", root);
        write_file(mid_side, "GARBAGE\n");
        {
            led_scan_stats st;

            memset(&st, 0, sizeof(st));
            TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                       "scan with corrupt sidecar");
            TEST_CHECK(st.errors >= 1,
                       "corrupt sidecar counted");
        }
        {
            led_asset_record rz;

            memset(&rz, 0, sizeof(rz));
            TEST_CHECK(led_assetdb_find_by_path(
                           s, "Scenes/zeta.luma_scene", &rz),
                       "unrelated record intact");
        }
    }

    kill_session(s, e, w);
    printf("assetdb: %d passed, %d failed\n", g_passed,
           g_failed);
    return (g_failed == 0) ? 0 : 1;
}
