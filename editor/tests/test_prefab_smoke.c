/* Phase 32 prefab smoke: create -> load -> instantiate (x2) ->
 * undo/redo -> assign-asset guards. Headless: engine WITHOUT
 * renderer (script-free scene: no assets needed beyond prefab
 * text itself).
 *
 * Temp layout (CTest-safe, no CWD dependence):
 *   %TEMP%/luma32_prefab_smoke/luma.project
 *   %TEMP%/luma32_prefab_smoke/Assets/hero.luprefab (+ .luma)
 */

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

int main(void) {
    led_session *s = NULL;
    le_engine *e = NULL;
    le_world *w = NULL;
    char root[1024];
    char prefab_rel[] = "Assets/hero.luprefab";
    le_object hero = LE_OBJECT_INVALID;
    le_object sword = LE_OBJECT_INVALID;
    le_asset prefab = LE_ASSET_INVALID;

    memset(root, 0, sizeof(root));
    TEST_CHECK(make_session(&s, &e, &w), "make session");
    {
        const char *tmp = getenv("TEMP");

        if (tmp == NULL || tmp[0] == '\0') {
            tmp = getenv("TMP");
        }
        if (tmp == NULL || tmp[0] == '\0') {
            tmp = ".";
        }
        snprintf(root, sizeof(root),
                 "%s/luma32_prefab_smoke", tmp);
    }
    /* Fresh dir, shell-free (CTest PATH-proof): remove known
     * prior-run files, create the layout. */
    {
        static const char *kPrior[] = {
            "luma.project",
            "Assets/hero.luprefab",
            "Assets/hero.luprefab.luma",
            "Assets/bad.luprefab",
            "Assets/bad.luprefab.luma",
            NULL,
        };
        int i;
        char p[2048];

        for (i = 0; kPrior[i] != NULL; i++) {
            snprintf(p, sizeof(p), "%s/%s", root, kPrior[i]);
            remove(p);
        }
#ifdef _WIN32
        _mkdir(root);
        snprintf(p, sizeof(p), "%s/Assets", root);
        _mkdir(p);
        snprintf(p, sizeof(p), "%s/Scenes", root);
        _mkdir(p);
#else
        {
            char cmd[2048];

            snprintf(cmd, sizeof(cmd),
                     "mkdir -p \"%s/Assets\" \"%s/Scenes\"", root,
                     root);
            (void)system(cmd);
        }
#endif
    }
    TEST_CHECK(led_project_create(root, "smoke") == LED_SUCCESS,
               "project create");
    TEST_CHECK(led_project_open(s, root) == LED_SUCCESS,
               "project open");
    TEST_CHECK(led_project_is_open(s), "project is open");

    /* Build a 2-object subtree: hero (named, moved) + sword. */
    TEST_CHECK(le_object_create(w, &hero) == LE_SUCCESS,
               "hero create");
    TEST_CHECK(le_object_set_name(w, &hero, "hero") == LE_SUCCESS,
               "hero name");
    {
        float p[3] = {1.0f, 2.0f, 3.0f};

        TEST_CHECK(le_object_set_position(w, &hero, p) ==
                       LE_SUCCESS,
                   "hero move");
    }
    TEST_CHECK(le_object_create(w, &sword) == LE_SUCCESS,
               "sword create");
    TEST_CHECK(le_object_set_name(w, &sword, "sword") ==
                   LE_SUCCESS,
               "sword name");
    TEST_CHECK(le_object_set_parent(w, &sword, &hero) ==
                   LE_SUCCESS,
               "sword parent hero");

    /* Create the prefab (authoring snapshot, no runtime state). */
    TEST_CHECK(led_prefab_create(s, &hero, prefab_rel) ==
                   LED_SUCCESS,
               "prefab create");
    TEST_CHECK(led_assetdb_count(s) >= 1, "db has prefab");
    {
        led_asset_record rec;

        memset(&rec, 0, sizeof(rec));
        TEST_CHECK(led_assetdb_find_by_path(s, prefab_rel, &rec),
                   "db find prefab by path");
        TEST_CHECK(rec.type == LED_PROJECT_ASSET_PREFAB,
                   "prefab type in db");
    }

    /* Load into the registry (validates through scene parser). */
    TEST_CHECK(led_prefab_load(s, prefab_rel, &prefab) ==
                   LED_SUCCESS,
               "prefab load");
    TEST_CHECK(le_asset_is_alive(e, &prefab), "prefab alive");
    TEST_CHECK(le_asset_get_type(e, &prefab) == LE_ASSET_PREFAB,
               "prefab registry type");
    {
        size_t sz = 0;
        const char *txt =
            le_asset_get_prefab_text(e, &prefab, &sz);

        TEST_CHECK(txt != NULL && sz > 0, "prefab text stored");
        TEST_CHECK(strstr(txt, "LUMA_PREFAB 1") != NULL,
                   "prefab magic in text");
        TEST_CHECK(strstr(txt, "prefab_root") != NULL,
                   "prefab root in text");
    }

    /* Instantiate twice: disjoint handles, shared nothing. */
    {
        led_prefab_instance a;
        led_prefab_instance b;

        memset(&a, 0, sizeof(a));
        memset(&b, 0, sizeof(b));
        TEST_CHECK(led_prefab_instantiate(s, &prefab, &a) ==
                       LED_SUCCESS,
                   "instantiate #1");
        TEST_CHECK(a.count == 2, "instance #1 has 2 objects");
        TEST_CHECK(led_prefab_instantiate(s, &prefab, &b) ==
                       LED_SUCCESS,
                   "instantiate #2");
        TEST_CHECK(b.count == 2, "instance #2 has 2 objects");
        TEST_CHECK(
            (a.root.index != b.root.index ||
             a.root.generation != b.root.generation),
            "instance roots disjoint");
        /* Names round-tripped. */
        {
            le_object f = LE_OBJECT_INVALID;

            TEST_CHECK(le_world_find_by_name(w, "hero", &f),
                       "hero name resolves");
        }
        led_prefab_instance_free(&a);
        led_prefab_instance_free(&b);
    }

    /* Undoable instantiate via command: undo removes the whole
     * instance, redo restores it. */
    {
        uint32_t before = le_world_get_object_count(w);
        led_command c;

        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_INSTANTIATE_PREFAB;
        snprintf(c.label, sizeof(c.label), "Spawn hero");
        c.prefab.prefab_asset = prefab;
        TEST_CHECK(led_execute(s, &c) == LED_SUCCESS,
                   "execute instantiate");
        TEST_CHECK(le_world_get_object_count(w) == before + 2,
                   "world grew by 2");
        TEST_CHECK(led_undo(s), "undo instantiate");
        TEST_CHECK(le_world_get_object_count(w) == before,
                   "undo removed instance");
        TEST_CHECK(led_redo(s), "redo instantiate");
        TEST_CHECK(le_world_get_object_count(w) == before + 2,
                   "redo restored instance");
    }

    /* Malformed prefab: rejected transactionally, world kept. */
    {
        char bad_abs[2048];
        FILE *f = NULL;
        uint32_t before = le_world_get_object_count(w);
        le_asset bad = LE_ASSET_INVALID;

        snprintf(bad_abs, sizeof(bad_abs), "%s/Assets/bad.luprefab",
                 root);
        f = fopen(bad_abs, "w");
        TEST_CHECK(f != NULL, "bad prefab file open");
        if (f != NULL) {
            fputs("LUMA_PREFAB 1\nprefab_root bogus\n", f);
            fclose(f);
        }
        TEST_CHECK(led_prefab_load(s, "Assets/bad.luprefab",
                                   &bad) == LED_ERROR_PARSE,
                   "malformed prefab rejected");
        TEST_CHECK(le_world_get_object_count(w) == before,
                   "world untouched by bad load");
    }

    /* Play isolation: prefab ops rejected while playing. Enter a
     * FRESH world for play (the hero/sword scene carries scriptless
     * plain objects, but play-enter needs a capturable world — use
     * a clean scene_new first so enter succeeds deterministically;
     * the rejection checks don't depend on world content). */
    {
        led_prefab_instance pi;
        led_result erc;

        memset(&pi, 0, sizeof(pi));
        TEST_CHECK(led_scene_new(s) == LED_SUCCESS,
                   "scene new before play");
        erc = led_play_enter(s);
        TEST_CHECK(erc == LED_SUCCESS, "play start");
        TEST_CHECK(led_prefab_instantiate(s, &prefab, &pi) ==
                       LED_ERROR_ALREADY_PLAYING,
                   "instantiate rejected while playing");
        {
            led_command c;

            memset(&c, 0, sizeof(c));
            c.kind = LED_CMD_INSTANTIATE_PREFAB;
            c.prefab.prefab_asset = prefab;
            TEST_CHECK(led_execute(s, &c) ==
                           LED_ERROR_ALREADY_PLAYING,
                       "cmd instantiate rejected while playing");
        }
        TEST_CHECK(led_play_exit(s) == LED_SUCCESS, "play stop");
        led_prefab_instance_free(&pi);
    }

    kill_session(s, e, w);
    printf("prefab_smoke: %d passed, %d failed\n", g_passed,
           g_failed);
    return (g_failed == 0) ? 0 : 1;
}
