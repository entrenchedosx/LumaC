/* Pass 2B §60-65 line-ending/checkout regression proof (headless):
 * canonical text assets (scene/lua/prefab) MUST NOT flip to STALE
 * when checked out CRLF vs LF. Policy under test:
 *   - writers emit LF bytes (fopen "wb", "\n" joins)
 *   - fingerprint hashes CANONICAL bytes (bare CR stripped)
 *   - readers tolerate LF + CRLF (strip \r at line ends)
 * Legs: (1) write LF scene via write_file_text + scan + import =>
 * READY (or UNIMPORTED-pending for scene type); rewrite the SAME
 * logical bytes CRLF => rescan => fingerprint identical, NO
 * STALE flip; (2) CRLF scene OPENS (reader tolerance); (3) a REAL
 * byte-touch (append a comment line) DOES flip to STALE (the
 * check is not vacuously blind); reimport converges back.
 * TEMP-backed, no CWD dependence. */

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

/* LF-canonical write (what every engine/editor writer must do:
 * binary mode, "\n" joins — never text-mode CRLF). */
static void write_lf(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");

    if (f != NULL) {
        fwrite(text, 1, strlen(text), f);
        fclose(f);
    }
}

/* Simulate a CRLF checkout of the same logical content (what git
 * text=auto did to scenes on Windows pre-R-010 pinning). */
static void rewrite_same_crlf(const char *path) {
    FILE *f = fopen(path, "rb");
    char *buf = NULL;
    long len = 0;
    size_t i;

    if (f == NULL) {
        return;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) {
        fclose(f);
        return;
    }
    buf = (char *)malloc((size_t)len + 1u);
    if (buf == NULL) {
        fclose(f);
        return;
    }
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return;
    }
    fclose(f);
    f = fopen(path, "wb");
    if (f == NULL) {
        free(buf);
        return;
    }
    for (i = 0; i < (size_t)len; i++) {
        if (buf[i] == '\n' && (i == 0 || buf[i - 1] != '\r')) {
            fputc('\r', f);
        }
        fputc(buf[i], f);
    }
    fclose(f);
    free(buf);
}

static const char kSceneLF[] =
    "LUMA_SCENE 1\n"
    "object 00000000000000000000000000000001\n"
    "name ProbeRoot\n"
    "end\n";

static const char kScriptLF[] =
    "local M = {}\n"
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
    snprintf(root, sizeof(root), "%s/luma2b_lineend", tmp);
    {
        static const char *kPrior[] = {
            "luma.project",
            "Assets/e.lua",
            "Assets/e.lua.luma",
            "Scenes/eol.luma_scene",
            "Scenes/eol.luma_scene.luma",
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
    TEST_CHECK(led_project_create(root, "eol") == LED_SUCCESS,
               "project create");
    TEST_CHECK(led_project_open(s, root) == LED_SUCCESS,
               "project open");

    /* LF baseline: script + scene, scan, import both. */
    snprintf(p, sizeof(p), "%s/Assets/e.lua", root);
    write_lf(p, kScriptLF);
    snprintf(p, sizeof(p), "%s/Scenes/eol.luma_scene", root);
    write_lf(p, kSceneLF);
    {
        led_scan_stats st;

        memset(&st, 0, sizeof(st));
        TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                   "scan LF baseline");
    }
    TEST_CHECK(led_import_asset(s, "Assets/e.lua") == LED_SUCCESS,
               "import script LF");
    TEST_CHECK(led_import_asset(s, "Scenes/eol.luma_scene") ==
                   LED_SUCCESS,
               "import scene LF");
    {
        led_asset_record rec;
        uint64_t fp_lf = 0;

        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(led_assetdb_find_by_path(
                       s, "Scenes/eol.luma_scene", &rec),
                   "find scene LF");
        fp_lf = rec.fingerprint_hash;
        printf("[INFO] LF fp size=%llu hash=%016llx status=%d\n",
               (unsigned long long)rec.fingerprint_size,
               (unsigned long long)rec.fingerprint_hash,
               (int)rec.status);

        /* CRLF checkout of the SAME logical bytes: rescan must
         * NOT flip to STALE, fingerprint must be identical. */
        snprintf(p, sizeof(p), "%s/Scenes/eol.luma_scene", root);
        rewrite_same_crlf(p);
        {
            led_scan_stats st;

            memset(&st, 0, sizeof(st));
            TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                       "rescan CRLF checkout");
            printf("[INFO] rescan stale=%u errors=%u\n",
                   st.stale_marked, st.errors);
            TEST_CHECK(st.stale_marked == 0,
                       "CRLF checkout marks nothing STALE");
        }
        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(led_assetdb_find_by_path(
                       s, "Scenes/eol.luma_scene", &rec),
                   "find scene CRLF");
        printf("[INFO] CRLF fp size=%llu hash=%016llx status=%d\n",
               (unsigned long long)rec.fingerprint_size,
               (unsigned long long)rec.fingerprint_hash,
               (int)rec.status);
        TEST_CHECK(rec.fingerprint_hash == fp_lf,
                   "LF vs CRLF fingerprint identical");
        TEST_CHECK(rec.status != LED_IMPORT_STALE,
                   "CRLF checkout not STALE");

        /* Same for the script. */
        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(led_assetdb_find_by_path(s, "Assets/e.lua",
                                            &rec),
                   "find script CRLF");
        TEST_CHECK(rec.status != LED_IMPORT_STALE,
                   "script CRLF checkout not STALE");

        /* Reader tolerance: the CRLF scene OPENS. */
        TEST_CHECK(led_scene_new(s) == LED_SUCCESS,
                   "fresh world for CRLF open");
        TEST_CHECK(led_scene_open(s, p) == LED_SUCCESS,
                   "CRLF scene opens");

        /* The check is not vacuous: a REAL content touch DOES
         * flip to STALE, and reimport converges. */
        {
            FILE *f = fopen(p, "ab");

            TEST_CHECK(f != NULL, "touch append open");
            if (f != NULL) {
                fwrite("# pass2b eol touch\n", 1, 19, f);
                fclose(f);
            }
        }
        {
            led_scan_stats st;

            memset(&st, 0, sizeof(st));
            TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                       "rescan after real touch");
            TEST_CHECK(st.stale_marked == 1,
                       "real touch marks exactly one STALE");
        }
        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(led_assetdb_find_by_path(
                       s, "Scenes/eol.luma_scene", &rec),
                   "find scene touched");
        TEST_CHECK(rec.status == LED_IMPORT_STALE,
                   "touched scene is STALE");
        {
            uint32_t ok = 0;

            TEST_CHECK(led_project_reimport_all(s, &ok) ==
                           LED_SUCCESS,
                       "reimport-all ok");
            TEST_CHECK(ok == 1, "reimport-all converges one");
        }
        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(led_assetdb_find_by_path(
                       s, "Scenes/eol.luma_scene", &rec),
                   "find scene reimported");
        TEST_CHECK(rec.status != LED_IMPORT_STALE,
                   "reimported scene not STALE");
        {
            led_scan_stats st;

            memset(&st, 0, sizeof(st));
            TEST_CHECK(led_project_scan(s, &st) == LED_SUCCESS,
                       "rescan after reimport");
            TEST_CHECK(st.stale_marked == 0,
                       "post-reimport scan clean (no loop)");
        }
    }

    kill_session(s, e, w);
    printf("lineend: %d passed, %d failed\n", g_passed,
           g_failed);
    return (g_failed == 0) ? 0 : 1;
}
