/* Phase 33 editor demo builder: creates editor/demo/Phase33Demo/
 * (a portable Luma Project: manifest + Assets/ + Scenes/Main)
 * through the PUBLIC led_* project/scene/prefab API — the same
 * workflow the desktop app drives via GUI (open project -> browse
 * assets -> open scene -> create -> prefab -> save). Headless (no
 * window/GPU); run from the repo root:
 *
 *   build/editor/Debug/editor_demo_phase33.exe
 *
 * Re-running is idempotent (wipes + rebuilds the demo dir).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <luma_engine/luma_engine.h>
#include <luma_editor/luma_editor.h>

#ifdef _WIN32
    #include <direct.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>
#endif

static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("[PASS] %s\n", msg); \
    } else { \
        printf("[FAIL] %s\n", msg); \
        g_fail = 1; \
    } \
} while (0)

static void make_dir(const char *path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
    (void)path;
}

static int write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        return 0;
    }
    fputs(text, f);
    return fclose(f) == 0;
}

/* A tiny orbit script (edit-authored, runtime-ticked): proves the
 * demo scene's scripted object from the Play side. */
static const char kOrbitScript[] =
    "local M = {}\n"
    "export('n', 0)\n"
    "function M.update(self, dt)\n"
    "  self.n = self.n + 1\n"
    "end\n"
    "return M\n";

int main(void) {
    const char *root = "editor/demo/Phase33Demo";
    char p[1024];
    led_session *s = NULL;
    le_engine *e = NULL;
    le_world *w = NULL;
    le_engine_desc ed;
    le_world_desc wd;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Building Phase 33 editor demo project...\n");

    make_dir("editor/demo");
    make_dir(root);
    snprintf(p, sizeof(p), "%s/Assets", root);
    make_dir(p);
    snprintf(p, sizeof(p), "%s/Scenes", root);
    make_dir(p);

    /* Idempotent: wipe the prior demo output (manifest + prior
     * import/prefab sidecars) so create always starts clean. */
    {
        static const char *kWipe[] = {
            "luma.project",
            "Assets/Orbit.lua",
            "Assets/Orbit.lua.luma",
            "Assets/Spinner.luprefab",
            "Assets/Spinner.luprefab.luma",
            "Scenes/Main.luma_scene",
            NULL,
        };
        int i;

        for (i = 0; kWipe[i] != NULL; i++) {
            char w[1024];

            snprintf(w, sizeof(w), "%s/%s", root, kWipe[i]);
            remove(w);
        }
    }

    memset(&ed, 0, sizeof(ed));
    memset(&wd, 0, sizeof(wd));
    CHECK((led_result)le_engine_create(&ed, &e) == LED_SUCCESS,
          "engine create");
    CHECK((led_result)le_world_create(e, &wd, &w) == LED_SUCCESS,
          "world create");
    CHECK(led_session_create(&s) == LED_SUCCESS,
          "session create");
    CHECK(led_session_attach(s, e, w) == LED_SUCCESS,
          "session attach");
    if (g_fail) {
        return 1;
    }

    /* Manifest (portable; created via the project API). */
    CHECK(led_project_create(root, "Phase33Demo") ==
              LED_SUCCESS,
          "project create");
    CHECK(led_project_open(s, root) == LED_SUCCESS,
          "project open");

    /* One authored script asset (import path = GUI Import). */
    snprintf(p, sizeof(p), "%s/Assets/Orbit.lua", root);
    CHECK(write_file(p, kOrbitScript), "write Orbit.lua");
    {
        led_scan_stats st;

        memset(&st, 0, sizeof(st));
        CHECK(led_project_scan(s, &st) == LED_SUCCESS,
              "scan assets");
        CHECK(led_import_asset(s, "Assets/Orbit.lua") ==
                  LED_SUCCESS,
              "import Orbit.lua");
    }

    /* Starter scene: camera rig + sun + ground + a scripted
     * spinner (every row through led_execute = undo-tracked). */
    {
        led_command c;
        le_object rig = LE_OBJECT_INVALID;
        le_object cam = LE_OBJECT_INVALID;
        le_object spinner = LE_OBJECT_INVALID;

        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_CREATE;
        snprintf(c.label, sizeof(c.label), "Rig");
        strncpy(c.name_value, "CameraRig",
                sizeof(c.name_value) - 1);
        CHECK(led_execute(s, &c) == LED_SUCCESS,
              "create CameraRig");
        CHECK(le_world_find_by_name(w, "CameraRig", &rig),
              "find CameraRig");
        {
            led_command mv;

            memset(&mv, 0, sizeof(mv));
            mv.kind = LED_CMD_SET_POSITION;
            mv.target = rig;
            mv.vec_value[0] = 0.0f;
            mv.vec_value[1] = 4.0f;
            mv.vec_value[2] = 10.0f;
            CHECK(led_execute(s, &mv) == LED_SUCCESS,
                  "rig position");
        }
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_CREATE;
        snprintf(c.label, sizeof(c.label), "Camera");
        strncpy(c.name_value, "Camera",
                sizeof(c.name_value) - 1);
        c.parent = rig;
        c.has_parent = 1;
        CHECK(led_execute(s, &c) == LED_SUCCESS,
              "create Camera");
        CHECK(le_world_find_by_name(w, "Camera", &cam),
              "find Camera");
        {
            le_camera_desc cd;

            le_camera_desc_default(&cd);
            CHECK(le_object_add_camera(w, &cam, &cd) ==
                      LE_SUCCESS,
                  "camera component");
            le_world_set_active_camera(w, &cam);
        }
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_CREATE;
        snprintf(c.label, sizeof(c.label), "Sun");
        strncpy(c.name_value, "Sun", sizeof(c.name_value) - 1);
        CHECK(led_execute(s, &c) == LED_SUCCESS,
              "create Sun");
        {
            le_object sun = LE_OBJECT_INVALID;
            le_light_desc ld;

            CHECK(le_world_find_by_name(w, "Sun", &sun),
                  "find Sun");
            memset(&ld, 0, sizeof(ld));
            ld.type = LE_LIGHT_DIRECTIONAL;
            ld.color[0] = ld.color[1] = ld.color[2] = 1.0f;
            ld.intensity = 3.0f;
            CHECK(le_object_add_light(w, &sun, &ld) ==
                      LE_SUCCESS,
                  "sun light");
        }
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_CREATE;
        snprintf(c.label, sizeof(c.label), "Ground");
        strncpy(c.name_value, "Ground",
                sizeof(c.name_value) - 1);
        CHECK(led_execute(s, &c) == LED_SUCCESS,
              "create Ground");
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_CREATE;
        snprintf(c.label, sizeof(c.label), "Spinner");
        strncpy(c.name_value, "Spinner",
                sizeof(c.name_value) - 1);
        CHECK(led_execute(s, &c) == LED_SUCCESS,
              "create Spinner");
        CHECK(le_world_find_by_name(w, "Spinner", &spinner),
              "find Spinner");
        /* Attach the imported script (drag-drop equivalent). */
        {
            led_asset_record rec;
            led_drag_payload pay;
            le_object o = spinner;

            memset(&rec, 0, sizeof(rec));
            CHECK(led_assetdb_find_by_path(
                      s, "Assets/Orbit.lua", &rec),
                  "find Orbit record");
            CHECK(led_browser_select(s, &rec.id) == LED_SUCCESS,
                  "select Orbit");
            memset(&pay, 0, sizeof(pay));
            CHECK(led_drag_begin(s, &pay), "drag Orbit");
            CHECK(led_drop_script_onto_object(s, &pay, &o),
                  "drop Orbit onto Spinner");
            led_browser_clear_selection(s);
        }
        /* The demo scene must REOPEN in a fresh process (portable
         * scene file): script refs are registry IDs, so detach the
         * session-local script before save — the prefab carries the
         * scripted authoring state (prefab file round-trips), the
         * scene stays dependency-free. */
        CHECK(le_object_remove_script(w, &spinner) == LE_SUCCESS,
              "detach session script for portable save");
        /* Prefab from the spinner (create/use-prefabs flow). */
        CHECK(led_prefab_create(s, &spinner,
                                "Assets/Spinner.luprefab") ==
                  LED_SUCCESS,
              "prefab Spinner");
        /* Instantiate once (second spinner proves reuse). */
        {
            led_command inst;
            le_asset pref = LE_ASSET_INVALID;

            CHECK(led_prefab_load(s,
                                  "Assets/Spinner.luprefab",
                                  &pref) == LED_SUCCESS,
                  "prefab load");
            memset(&inst, 0, sizeof(inst));
            inst.kind = LED_CMD_INSTANTIATE_PREFAB;
            snprintf(inst.label, sizeof(inst.label),
                     "Instantiate Spinner");
            inst.prefab.prefab_asset = pref;
            CHECK(led_execute(s, &inst) == LED_SUCCESS,
                  "prefab instantiate");
        }
    }

    /* Save (the GUI Save path). */
    snprintf(p, sizeof(p), "%s/Scenes/Main.luma_scene", root);
    CHECK(led_scene_save_as(s, p) == LED_SUCCESS,
          "save Main scene");
    printf("demo objects: %u\n",
           (unsigned)le_world_get_object_count(w));

    /* Play oracle on the demo content (tick + byte-identical). */
    {
        char *before = NULL;
        char *after = NULL;
        size_t bsize = 0;
        size_t asize = 0;
        le_asset scene = LE_ASSET_INVALID;

        CHECK(le_scene_create(e, 0, &scene) == LE_SUCCESS,
              "oracle scene");
        CHECK(le_scene_capture(w, &scene, NULL) == LE_SUCCESS,
              "oracle capture before");
        CHECK(le_scene_save_text(e, &scene, &before, &bsize) ==
                  LE_SUCCESS,
              "oracle text before");
        le_asset_unload(e, &scene);
        CHECK(led_play_enter(s) == LED_SUCCESS,
              "oracle play enter");
        CHECK(led_play_tick(s, 1.0f / 60.0f) == LED_SUCCESS,
              "oracle play tick");
        CHECK(led_play_exit(s) == LED_SUCCESS,
              "oracle play exit");
        CHECK(le_scene_create(e, 0, &scene) == LE_SUCCESS,
              "oracle scene 2");
        CHECK(le_scene_capture(w, &scene, NULL) == LE_SUCCESS,
              "oracle capture after");
        CHECK(le_scene_save_text(e, &scene, &after, &asize) ==
                  LE_SUCCESS,
              "oracle text after");
        le_asset_unload(e, &scene);
        CHECK(before != NULL && after != NULL &&
                  bsize == asize && strcmp(before, after) == 0,
              "oracle edit byte-identical across play");
        if (before != NULL) {
            le_scene_free_text(before);
        }
        if (after != NULL) {
            le_scene_free_text(after);
        }
    }

    led_history_clear(s);
    led_session_destroy(s);
    le_world_destroy(w);
    le_engine_destroy(e);
    if (g_fail) {
        printf("DEMO BUILD FAILED\n");
        return 1;
    }
    printf("DEMO BUILD OK: %s\n", root);
    return 0;
}
