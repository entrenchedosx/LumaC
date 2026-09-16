/* Phase 32 project-system tests: manifest create/open/close/
 * switch, path normalization matrix + escape rejection, scan
 * discovery + sidecar identity, rename/move preserving UUID,
 * MISSING/restored, unknown files ignored, portability (copy the
 * whole project to a new absolute path -> reopen -> identity
 * stable). Headless, TEMP-backed, no CWD dependence. */

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

static void temp_root(char out[1024], const char *leaf) {
    const char *tmp = getenv("TEMP");

    if (tmp == NULL || tmp[0] == '\0') {
        tmp = getenv("TMP");
    }
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = ".";
    }
    snprintf(out, 1024, "%s/%s", tmp, leaf);
}

#ifdef _WIN32
    #include <direct.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>
#endif

static void make_dir_one(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

static void fresh_dir(const char *root) {
    /* Shell-free reset: remove known prior-run files explicitly
     * (the suites own their TEMP leaves), then create the dirs. */
    static const char *kPrior[] = {
        "luma.project",
        "Assets/hero.luprefab",
        "Assets/hero.luprefab.luma",
        "Assets/bad.luprefab",
        "Assets/bad.luprefab.luma",
        "Scenes/main.luma_scene",
        "Scenes/main.luma_scene.luma",
        "Scenes/renamed.luma_scene",
        "Scenes/renamed.luma_scene.luma",
        "Scenes/readme.txt",
        NULL,
    };
    int i;

    for (i = 0; kPrior[i] != NULL; i++) {
        char p[2048];

        snprintf(p, sizeof(p), "%s/%s", root, kPrior[i]);
        remove(p);
    }
    make_dir_one(root);
    {
        char sub[2048];

        snprintf(sub, sizeof(sub), "%s/Assets", root);
        make_dir_one(sub);
        snprintf(sub, sizeof(sub), "%s/Scenes", root);
        make_dir_one(sub);
    }
}

/* Shell-free file copy (CTest PATH-proof). */
static int copy_file(const char *from, const char *to) {
    FILE *fi = fopen(from, "rb");
    FILE *fo = NULL;
    unsigned char buf[4096];
    size_t n = 0;

    if (fi == NULL) {
        return 0;
    }
    fo = fopen(to, "wb");
    if (fo == NULL) {
        fclose(fi);
        return 0;
    }
    while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) {
        if (fwrite(buf, 1, n, fo) != n) {
            fclose(fi);
            fclose(fo);
            return 0;
        }
    }
    fclose(fi);
    if (fclose(fo) != 0) {
        return 0;
    }
    return 1;
}

static int copy_tree(const char *from_root, const char *to_root,
                     const char *rel) {
    /* Known project layout copy: manifest + Assets/* + Scenes/*.
     * (A test helper, not a product copier: it mirrors the exact
     * files the test created instead of walking directories.) */
    static const char *kFiles[] = {
        "luma.project",
        "Scenes/renamed.luma_scene",
        "Scenes/renamed.luma_scene.luma",
        "Scenes/readme.txt",
        NULL,
    };
    int i;

    (void)rel;
    make_dir_one(to_root);
    {
        char sub[2048];

        snprintf(sub, sizeof(sub), "%s/Assets", to_root);
        make_dir_one(sub);
        snprintf(sub, sizeof(sub), "%s/Scenes", to_root);
        make_dir_one(sub);
    }
    for (i = 0; kFiles[i] != NULL; i++) {
        char from[2048];
        char to[2048];

        snprintf(from, sizeof(from), "%s/%s", from_root,
                 kFiles[i]);
        snprintf(to, sizeof(to), "%s/%s", to_root, kFiles[i]);
        {
            FILE *probe = fopen(from, "rb");

            if (probe == NULL) {
                continue; /* optional file absent: skip */
            }
            fclose(probe);
        }
        if (!copy_file(from, to)) {
            printf("[INFO] copy_tree failed: %s\n", kFiles[i]);
            return 0;
        }
    }
    return 1;
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");

    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

int main(void) {
    /* ---- path normalization matrix (no session needed) ---- */
    {
        char out[1024];

        TEST_CHECK(led_project_normalize("Assets/a.luprefab", out) &&
                       strcmp(out, "Assets/a.luprefab") == 0,
                   "normalize plain");
        TEST_CHECK(led_project_normalize("Assets\\a.luprefab", out) &&
                       strcmp(out, "Assets/a.luprefab") == 0,
                   "normalize backslash fold");
        TEST_CHECK(led_project_normalize("Assets//a.luprefab", out) &&
                       strcmp(out, "Assets/a.luprefab") == 0,
                   "normalize dup sep");
        TEST_CHECK(led_project_normalize("./Assets/a.luprefab",
                                         out) &&
                       strcmp(out, "Assets/a.luprefab") == 0,
                   "normalize dot");
        TEST_CHECK(led_project_normalize("Assets/x/../a.luprefab",
                                         out) &&
                       strcmp(out, "Assets/a.luprefab") == 0,
                   "normalize dotdot");
        TEST_CHECK(!led_project_normalize("", out),
                   "normalize empty rejected");
        TEST_CHECK(!led_project_normalize("/abs/path", out),
                   "normalize absolute rejected");
        TEST_CHECK(!led_project_normalize("C:/win/path", out),
                   "normalize drive rejected");
        TEST_CHECK(led_project_normalize("Assets/", out) &&
                       strcmp(out, "Assets") == 0,
                   "normalize trailing slash");
    }

    /* ---- manifest + open + discovery + identity ---- */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        char root[1024];
        char scene_path[2048];
        char note_path[2048];

        temp_root(root, "luma32_proj");
        fresh_dir(root);
        TEST_CHECK(make_session(&s, &e, &w), "make session");
        TEST_CHECK(led_project_create(root, "proj") ==
                       LED_SUCCESS,
                   "project create");
        TEST_CHECK(led_project_create(root, "proj") !=
                       LED_SUCCESS,
                   "re-create refuses (use open)");
        TEST_CHECK(led_project_open(s, root) == LED_SUCCESS,
                   "project open");
        TEST_CHECK(led_project_is_open(s), "is open");
        /* Empty project: no records. */
        TEST_CHECK(led_assetdb_count(s) == 0, "empty db");

        /* Drop a scene + an unknown file; rescan discovers the
         * scene, ignores the unknown. */
        snprintf(scene_path, sizeof(scene_path),
                 "%s/Scenes/main.luma_scene", root);
        snprintf(note_path, sizeof(note_path), "%s/Scenes/readme.txt",
                 root);
        write_file(scene_path, "LUMA_SCENE 1\n");
        write_file(note_path, "hello\n");
        {
            led_scan_stats st;

            memset(&st, 0, sizeof(st));
            TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                       "scan ok");
            TEST_CHECK(led_assetdb_count(s) == 1,
                       "one record (unknown ignored)");
        }
        {
            led_asset_record rec;
            char id_before[33];

            memset(&rec, 0, sizeof(rec));
            TEST_CHECK(led_assetdb_find_by_path(
                           s, "Scenes/main.luma_scene", &rec),
                       "find scene by path");
            TEST_CHECK(rec.type == LED_PROJECT_ASSET_SCENE,
                       "scene type");
            TEST_CHECK(rec.status == LED_IMPORT_UNIMPORTED,
                       "fresh discovery pending (not missing)");
            memcpy(id_before, rec.id_hex, 33);

            /* Rename preserves the project UUID. */
            TEST_CHECK(led_project_rename(
                           s, "Scenes/main.luma_scene",
                           "Scenes/renamed.luma_scene") ==
                           LED_SUCCESS,
                       "rename ok");
            memset(&rec, 0, sizeof(rec));
            TEST_CHECK(led_assetdb_find_by_path(
                           s, "Scenes/renamed.luma_scene", &rec),
                       "find by new path");
            TEST_CHECK(strcmp(rec.id_hex, id_before) == 0,
                       "rename preserves UUID");
            TEST_CHECK(!led_assetdb_find_by_path(
                           s, "Scenes/main.luma_scene", &rec),
                       "old path gone");

            /* Delete the source file behind the DB: rescan marks
             * MISSING with identity retained. */
            {
                char gone[2048];

                snprintf(gone, sizeof(gone),
                         "%s/Scenes/renamed.luma_scene", root);
                remove(gone);
            }
            {
                led_scan_stats st;

                memset(&st, 0, sizeof(st));
                TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                           "rescan after delete");
            }
            memset(&rec, 0, sizeof(rec));
            TEST_CHECK(led_assetdb_find_by_path(
                           s, "Scenes/renamed.luma_scene", &rec),
                       "missing record retained");
            TEST_CHECK(rec.status == LED_IMPORT_MISSING,
                       "missing status");
            TEST_CHECK(strcmp(rec.id_hex, id_before) == 0,
                       "missing keeps UUID");

            /* Restore the file: scan recovers (restored, same
             * UUID). */
            {
                char back[2048];

                snprintf(back, sizeof(back),
                         "%s/Scenes/renamed.luma_scene", root);
                write_file(back, "LUMA_SCENE 1\n");
            }
            {
                led_scan_stats st;

                memset(&st, 0, sizeof(st));
                TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                           "rescan after restore");
            }
            memset(&rec, 0, sizeof(rec));
            TEST_CHECK(led_assetdb_find_by_path(
                           s, "Scenes/renamed.luma_scene", &rec),
                       "restored record found");
            TEST_CHECK(rec.status == LED_IMPORT_UNIMPORTED ||
                           rec.status == LED_IMPORT_STALE,
                       "restored pending (not missing)");
            TEST_CHECK(strcmp(rec.id_hex, id_before) == 0,
                       "restore keeps UUID");

            /* Portability: copy the whole project dir to a new
             * absolute path; open there; the scene UUID is
             * identical (identity is root-independent).
             * Shell-free copy_tree (CTest PATH-proof). */
            {
                char root2[1024];
                led_session *s2 = NULL;
                le_engine *e2 = NULL;
                le_world *w2 = NULL;
                led_asset_record rec2;

                temp_root(root2, "luma32_proj_copy");
                TEST_CHECK(copy_tree(root, root2, NULL),
                           "copy project tree");
                TEST_CHECK(make_session(&s2, &e2, &w2),
                           "make session 2");
                TEST_CHECK(led_project_open(s2, root2) ==
                               LED_SUCCESS,
                           "open copied project");
                memset(&rec2, 0, sizeof(rec2));
                TEST_CHECK(led_assetdb_find_by_path(
                               s2, "Scenes/renamed.luma_scene",
                               &rec2),
                           "find scene in copy");
                TEST_CHECK(strcmp(rec2.id_hex, id_before) == 0,
                           "copy preserves UUID");
                kill_session(s2, e2, w2);
            }
        }

        /* Escape rejection: resolve refuses breakout. */
        {
            char abs[2048];

            TEST_CHECK(!led_project_resolve(s, "../escape", abs),
                       "resolve rejects ..");
            TEST_CHECK(!led_project_resolve(s, "/abs", abs),
                       "resolve rejects absolute");
        }

        /* Switch policy: opening a second project closes the
         * first (singleton). */
        {
            char root2[1024];

            temp_root(root2, "luma32_proj_b");
            fresh_dir(root2);
            TEST_CHECK(led_project_create(root2, "b") ==
                           LED_SUCCESS,
                       "create second");
            TEST_CHECK(led_project_open(s, root2) == LED_SUCCESS,
                       "switch opens second");
            TEST_CHECK(led_assetdb_count(s) == 0,
                       "second starts empty");
        }
        led_project_close(s);
        TEST_CHECK(!led_project_is_open(s), "closed");
        kill_session(s, e, w);
    }

    printf("project: %d passed, %d failed\n", g_passed,
           g_failed);
    return (g_failed == 0) ? 0 : 1;
}
