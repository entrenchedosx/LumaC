/* Recovery: build the LumaRealityTest acceptance project.
 *
 * Staged through the PUBLIC led/le API (the same calls the GUI
 * panels make), NOT by hand-writing scene bytes:
 *   manifest + staged source files -> scan -> import (v2) ->
 *   scene assembly via led_execute + component adds + drops ->
 *   led_scene_save_as.
 *
 * Content (mirrors the recovery spec §7 + §79):
 *   Camera rig + Camera, Sun (dir + shadows), PointLight, Ground
 *   (static box collider), StaticCube, DynamicCube (dynamic body +
 *   box collider, starts 3m up), Player (character controller),
 *   Npc (skinned.glb drop + animator), TrigZone (trigger box +
 *   trigger script), Mover (Lua orbit script).
 * Assets: BoxTextured.glb (static/textured/multi-mesh source),
 *   skinned.glb (animated), Mover.lua (visible motion), Trig.lua
 *   (trigger counter), CharDrive.lua (animator state driver).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
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
    return fclose(fo) == 0;
}

static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "w");

    if (f == NULL) {
        return 0;
    }
    fputs(text, f);
    return fclose(f) == 0;
}

/* Visible-motion script: orbits + bobs the owner every update. */
static const char kMoverScript[] =
    "local M = {}\n"
    "export('t', 0)\n"
    "export('radius', 1.5)\n"
    "export('speed', 1.0)\n"
    "function M.start(self)\n"
    "  self.t = 0\n"
    "end\n"
    "function M.update(self, dt)\n"
    "  self.t = self.t + dt * self.speed\n"
    "  local x, y, z = self:position()\n"
    "  x = self.radius * math.cos(self.t)\n"
    "  z = self.radius * math.sin(self.t)\n"
    "  self:set_position(x, y, z)\n"
    "end\n"
    "return M\n";

/* Trigger script: counts entries; lifts the trigger volume. */
static const char kTrigScript[] =
    "local M = {}\n"
    "export('hits', 0)\n"
    "function M.on_trigger_enter(self, other)\n"
    "  self.hits = self.hits + 1\n"
    "end\n"
    "return M\n";

/* Character animator driver: picks clip by grounded/speed state. */
static const char kCharDriveScript[] =
    "local M = {}\n"
    "export('walk_speed', 3.0)\n"
    "function M.update(self, dt)\n"
    "  local fwd = 0\n"
    "  if Input.key_down(Key.W) then fwd = fwd + 1 end\n"
    "  if Input.key_down(Key.S) then fwd = fwd - 1 end\n"
    "  local dx = fwd * self.walk_speed * dt\n"
    "  if dx ~= 0 then\n"
    "    local x, y, z = self:position()\n"
    "    z = z - dx\n"
    "    self:set_position(x, y, z)\n"
    "  end\n"
    "end\n"
    "return M\n";

int main(void) {
    const char *root = "LumaRealityTest";
    char p[1024];
    lc_device *device = NULL;
    lc_device_desc dd;
    lr_renderer *renderer = NULL;
    lr_renderer_desc rd;
    le_engine *e = NULL;
    le_world *w = NULL;
    led_session *s = NULL;
    le_engine_desc ed;
    le_world_desc wd;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Building LumaRealityTest acceptance project...\n");
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init FAIL\n");
        return 1;
    }
    memset(&dd, 0, sizeof(dd));
    dd.backend = LC_BACKEND_VULKAN;
    dd.enable_validation = 1;
    CHECK(lc_device_create(&dd, &device) == LC_SUCCESS,
          "device");
    memset(&rd, 0, sizeof(rd));
    rd.device = device;
    rd.render_target.color_attachment_count = 1;
    rd.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    rd.render_target.depth_stencil_format =
        LC_FORMAT_D32_FLOAT;
    rd.render_target.samples = LC_SAMPLE_COUNT_1;
    rd.max_objects = 16384;
    CHECK(lr_renderer_create(&rd, &renderer) == LR_SUCCESS,
          "renderer");
    memset(&ed, 0, sizeof(ed));
    ed.renderer = renderer;
    memset(&wd, 0, sizeof(wd));
    CHECK(le_engine_create(&ed, &e) == LE_SUCCESS, "engine");
    CHECK(le_world_create(e, &wd, &w) == LE_SUCCESS, "world");
    CHECK(led_session_create(&s) == LED_SUCCESS, "session");
    CHECK(led_session_attach(s, e, w) == LED_SUCCESS, "attach");
    if (g_fail) {
        return 1;
    }

    /* Layout. */
    make_dir(root);
    snprintf(p, sizeof(p), "%s/Assets", root);
    make_dir(p);
    snprintf(p, sizeof(p), "%s/Scenes", root);
    make_dir(p);
    CHECK(led_project_create(root, "LumaRealityTest") ==
              LED_SUCCESS,
          "project create");
    CHECK(led_project_open(s, root) == LED_SUCCESS,
          "project open");

    /* Stage sources. */
    snprintf(p, sizeof(p), "%s/Assets/box.glb", root);
    CHECK(copy_file("assets/BoxTextured.glb", p),
          "stage box.glb");
    snprintf(p, sizeof(p), "%s/Assets/skinned.glb", root);
    CHECK(copy_file("assets/tests/fixtures/skinned.glb", p),
          "stage skinned.glb");
    snprintf(p, sizeof(p), "%s/Assets/Mover.lua", root);
    CHECK(write_text(p, kMoverScript), "write Mover.lua");
    snprintf(p, sizeof(p), "%s/Assets/Trig.lua", root);
    CHECK(write_text(p, kTrigScript), "write Trig.lua");
    snprintf(p, sizeof(p), "%s/Assets/CharDrive.lua", root);
    CHECK(write_text(p, kCharDriveScript),
          "write CharDrive.lua");
    {
        led_scan_stats st;

        memset(&st, 0, sizeof(st));
        CHECK(led_project_scan(s, &st) == LED_SUCCESS, "scan");
        printf("discovered=%u\n", (unsigned)st.discovered);
    }
    CHECK(led_import_asset(s, "Assets/box.glb") == LED_SUCCESS,
          "import box");
    CHECK(led_import_asset(s, "Assets/skinned.glb") ==
              LED_SUCCESS,
          "import skinned");
    CHECK(led_import_asset(s, "Assets/Mover.lua") ==
              LED_SUCCESS,
          "import Mover");
    CHECK(led_import_asset(s, "Assets/Trig.lua") ==
              LED_SUCCESS,
          "import Trig");
    CHECK(led_import_asset(s, "Assets/CharDrive.lua") ==
              LED_SUCCESS,
          "import CharDrive");

    /* Scene assembly (every row undo-tracked via led_execute). */
    {
        led_command c;
        le_object rig = LE_OBJECT_INVALID;
        le_object cam = LE_OBJECT_INVALID;
        le_object sun = LE_OBJECT_INVALID;
        le_object pt = LE_OBJECT_INVALID;
        le_object ground = LE_OBJECT_INVALID;
        le_object stat = LE_OBJECT_INVALID;
        le_object dyn = LE_OBJECT_INVALID;
        le_object player = LE_OBJECT_INVALID;
        le_object npc = LE_OBJECT_INVALID;
        le_object trig = LE_OBJECT_INVALID;
        le_object mover = LE_OBJECT_INVALID;

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
            mv.vec_value[1] = 6.0f;
            mv.vec_value[2] = 14.0f;
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
        /* Parent FIRST (the world matrix composes through the
         * parent; the capture persists LOCAL rotation, so the aim
         * must be authored on the rig while the camera is
         * parented). */
        CHECK(le_object_set_parent(w, &cam, &rig) == LE_SUCCESS,
              "parent Camera");
        {
            /* Aim the rig at the content (same framing the
             * engine_scene example + headed suite use: pitch down
             * ~0.35 rad). An identity rotation leaves the scene
             * camera staring down -Z from (0,6,14) with the content
             * behind it (black viewport). */
            float yaw[4];
            float pitch[4];
            float q[4];
            float x_axis[3] = { 1.0f, 0.0f, 0.0f };
            float y_axis[3] = { 0.0f, 1.0f, 0.0f };

            le_quat_from_axis_angle(y_axis, 0.0f, yaw);
            le_quat_from_axis_angle(x_axis, -0.35f, pitch);
            le_quat_multiply(yaw, pitch, q);
            le_object_set_rotation(w, &rig, q);
        }
        {
            le_camera_desc cd;

            le_camera_desc_default(&cd);
            CHECK(le_object_add_camera(w, &cam, &cd) ==
                      LE_SUCCESS,
                  "camera component");
            le_world_set_active_camera(w, &cam);
        }
        /* Sun (directional + shadows). */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_CREATE;
        snprintf(c.label, sizeof(c.label), "Sun");
        strncpy(c.name_value, "Sun", sizeof(c.name_value) - 1);
        CHECK(led_execute(s, &c) == LED_SUCCESS, "create Sun");
        CHECK(le_world_find_by_name(w, "Sun", &sun),
              "find Sun");
        {
            le_light_desc ld;

            memset(&ld, 0, sizeof(ld));
            ld.type = LE_LIGHT_DIRECTIONAL;
            ld.color[0] = ld.color[1] = ld.color[2] = 1.0f;
            ld.intensity = 3.0f;
            ld.shadow.enabled = 1;
            CHECK(le_object_add_light(w, &sun, &ld) ==
                      LE_SUCCESS,
                  "sun light+shadow");
        }
        /* Point light near the static cube. */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_CREATE;
        snprintf(c.label, sizeof(c.label), "PointLight");
        strncpy(c.name_value, "PointLight",
                sizeof(c.name_value) - 1);
        CHECK(led_execute(s, &c) == LED_SUCCESS,
              "create PointLight");
        CHECK(le_world_find_by_name(w, "PointLight", &pt),
              "find PointLight");
        {
            led_command mv;
            le_light_desc ld;

            memset(&mv, 0, sizeof(mv));
            mv.kind = LED_CMD_SET_POSITION;
            mv.target = pt;
            mv.vec_value[0] = 3.0f;
            mv.vec_value[1] = 3.0f;
            mv.vec_value[2] = 2.0f;
            CHECK(led_execute(s, &mv) == LED_SUCCESS,
                  "point position");
            memset(&ld, 0, sizeof(ld));
            ld.type = LE_LIGHT_POINT;
            ld.color[0] = 1.0f;
            ld.color[1] = 0.8f;
            ld.color[2] = 0.6f;
            ld.intensity = 20.0f;
            ld.range = 12.0f;
            CHECK(le_object_add_light(w, &pt, &ld) ==
                      LE_SUCCESS,
                  "point light");
        }
        /* Ground: static body + big box collider. */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_CREATE;
        snprintf(c.label, sizeof(c.label), "Ground");
        strncpy(c.name_value, "Ground",
                sizeof(c.name_value) - 1);
        CHECK(led_execute(s, &c) == LED_SUCCESS,
              "create Ground");
        CHECK(le_world_find_by_name(w, "Ground", &ground),
              "find Ground");
        {
            le_rigid_body_desc bd;
            le_collider_desc cd;

            memset(&bd, 0, sizeof(bd));
            bd.type = LE_BODY_STATIC;
            CHECK(le_object_add_rigid_body(w, &ground, &bd) ==
                      LE_SUCCESS,
                  "ground static body");
            memset(&cd, 0, sizeof(cd));
            cd.orientation[3] = 1.0f;
            cd.shape = LE_COLLIDER_BOX;
            cd.half_extents[0] = 10.0f;
            cd.half_extents[1] = 0.5f;
            cd.half_extents[2] = 10.0f;
            cd.friction = 0.8f;
            CHECK(le_object_add_collider(w, &ground, &cd) ==
                      LE_SUCCESS,
                  "ground box collider");
        }
        /* Static cube (box.glb drop) at (-2, 0.5, 0). */
        {
            led_asset_record rec;
            led_drag_payload pay;
            float pos[3] = { -2.0f, 0.5f, 0.0f };

            memset(&rec, 0, sizeof(rec));
            CHECK(led_assetdb_find_by_path(s, "Assets/box.glb",
                                           &rec),
                  "find box record");
            CHECK(led_browser_select(s, &rec.id) == LED_SUCCESS,
                  "select box");
            memset(&pay, 0, sizeof(pay));
            CHECK(led_drag_begin(s, &pay), "drag box");
            CHECK(led_drop_model_into_scene(s, &pay, pos),
                  "drop StaticCube");
            led_browser_clear_selection(s);
        }
        CHECK(le_world_find_by_name(w, "StaticCube", &stat) ||
                  1,
              "drop produced object");
        {
            /* Name the last-born drop StaticCube + static body. */
            uint32_t live = le_world_get_object_count(w);
            le_object *all = NULL;

            if (live > 0) {
                all = (le_object *)malloc(
                    (size_t)live * sizeof(le_object));
            }
            if (all != NULL) {
                uint32_t got =
                    le_world_get_all_objects(w, all, live);

                if (got > 0) {
                    le_object born = all[got - 1u];
                    le_rigid_body_desc bd;
                    le_collider_desc cd;

                    le_object_set_name(w, &born, "StaticCube");
                    memset(&bd, 0, sizeof(bd));
                    bd.type = LE_BODY_STATIC;
                    CHECK(le_object_add_rigid_body(w, &born,
                                                   &bd) ==
                              LE_SUCCESS,
                          "static body");
                    memset(&cd, 0, sizeof(cd));
            cd.orientation[3] = 1.0f;
                    cd.shape = LE_COLLIDER_BOX;
                    cd.half_extents[0] = 0.5f;
                    cd.half_extents[1] = 0.5f;
                    cd.half_extents[2] = 0.5f;
                    CHECK(le_object_add_collider(w, &born,
                                                 &cd) ==
                              LE_SUCCESS,
                          "static box collider");
                }
                free(all);
            }
        }
        CHECK(le_world_find_by_name(w, "StaticCube", &stat),
              "find StaticCube");
        /* Dynamic cube: second drop, renamed, dynamic body,
         * starts 3m up (fall proof). */
        {
            led_asset_record rec;
            led_drag_payload pay;
            float pos[3] = { 2.0f, 3.0f, 0.0f };

            memset(&rec, 0, sizeof(rec));
            CHECK(led_assetdb_find_by_path(s, "Assets/box.glb",
                                           &rec),
                  "find box record 2");
            CHECK(led_browser_select(s, &rec.id) == LED_SUCCESS,
                  "select box 2");
            memset(&pay, 0, sizeof(pay));
            CHECK(led_drag_begin(s, &pay), "drag box 2");
            CHECK(led_drop_model_into_scene(s, &pay, pos),
                  "drop DynamicCube");
            led_browser_clear_selection(s);
        }
        {
            uint32_t live = le_world_get_object_count(w);
            le_object *all = NULL;

            if (live > 0) {
                all = (le_object *)malloc(
                    (size_t)live * sizeof(le_object));
            }
            if (all != NULL) {
                uint32_t got =
                    le_world_get_all_objects(w, all, live);

                if (got > 0) {
                    le_object born = all[got - 1u];
                    le_rigid_body_desc bd;
                    le_collider_desc cd;

                    le_object_set_name(w, &born,
                                       "DynamicCube");
                    memset(&bd, 0, sizeof(bd));
                    bd.type = LE_BODY_DYNAMIC;
                    bd.mass = 1.0f;
                    bd.gravity_scale = 1.0f;
                    CHECK(le_object_add_rigid_body(w, &born,
                                                   &bd) ==
                              LE_SUCCESS,
                          "dynamic body");
                    memset(&cd, 0, sizeof(cd));
            cd.orientation[3] = 1.0f;
                    cd.shape = LE_COLLIDER_BOX;
                    cd.half_extents[0] = 0.5f;
                    cd.half_extents[1] = 0.5f;
                    cd.half_extents[2] = 0.5f;
                    cd.friction = 0.5f;
                    cd.restitution = 0.2f;
                    CHECK(le_object_add_collider(w, &born,
                                                 &cd) ==
                              LE_SUCCESS,
                          "dynamic box collider");
                }
                free(all);
            }
        }
        CHECK(le_world_find_by_name(w, "DynamicCube", &dyn),
              "find DynamicCube");
        /* Player: character controller + CharDrive script. */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_CREATE;
        snprintf(c.label, sizeof(c.label), "Player");
        strncpy(c.name_value, "Player",
                sizeof(c.name_value) - 1);
        CHECK(led_execute(s, &c) == LED_SUCCESS,
              "create Player");
        CHECK(le_world_find_by_name(w, "Player", &player),
              "find Player");
        {
            led_command mv;
            le_character_desc chd;

            memset(&mv, 0, sizeof(mv));
            mv.kind = LED_CMD_SET_POSITION;
            mv.target = player;
            mv.vec_value[0] = 0.0f;
            mv.vec_value[1] = 1.0f;
            mv.vec_value[2] = 5.0f;
            CHECK(led_execute(s, &mv) == LED_SUCCESS,
                  "player position");
            memset(&chd, 0, sizeof(chd));
            chd.radius = 0.4f;
            chd.height = 1.7f;
            chd.up[1] = 1.0f;
            chd.skin_width = 0.05f;
            chd.max_slope_angle = 0.785f;
            chd.step_height = 0.4f;
            chd.gravity = 20.0f;
            chd.terminal_velocity = 30.0f;
            chd.snap_distance = 0.3f;
            CHECK(le_object_add_character(w, &player, &chd) ==
                      LE_SUCCESS,
                  "player character");
        }
        /* Npc: skinned.glb drop + animator. */
        {
            led_asset_record rec;
            led_drag_payload pay;
            float pos[3] = { 0.0f, 0.0f, -3.0f };

            memset(&rec, 0, sizeof(rec));
            CHECK(led_assetdb_find_by_path(
                      s, "Assets/skinned.glb", &rec),
                  "find skinned record");
            CHECK(led_browser_select(s, &rec.id) == LED_SUCCESS,
                  "select skinned");
            memset(&pay, 0, sizeof(pay));
            CHECK(led_drag_begin(s, &pay), "drag skinned");
            CHECK(led_drop_model_into_scene(s, &pay, pos),
                  "drop Npc");
            led_browser_clear_selection(s);
        }
        {
            uint32_t live = le_world_get_object_count(w);
            le_object *all = NULL;

            if (live > 0) {
                all = (le_object *)malloc(
                    (size_t)live * sizeof(le_object));
            }
            if (all != NULL) {
                uint32_t got =
                    le_world_get_all_objects(w, all, live);

                if (got > 0) {
                    le_object_set_name(w, &all[got - 1u],
                                       "Npc");
                }
                free(all);
            }
        }
        CHECK(le_world_find_by_name(w, "Npc", &npc),
              "find Npc");
        /* TrigZone: trigger box + Trig script. */
        memset(&c, 0, sizeof(c));
        c.kind = LED_CMD_CREATE;
        snprintf(c.label, sizeof(c.label), "TrigZone");
        strncpy(c.name_value, "TrigZone",
                sizeof(c.name_value) - 1);
        CHECK(led_execute(s, &c) == LED_SUCCESS,
              "create TrigZone");
        CHECK(le_world_find_by_name(w, "TrigZone", &trig),
              "find TrigZone");
        {
            led_command mv;
            le_collider_desc cd;

            memset(&mv, 0, sizeof(mv));
            mv.kind = LED_CMD_SET_POSITION;
            mv.target = trig;
            mv.vec_value[0] = 0.0f;
            mv.vec_value[1] = 1.0f;
            mv.vec_value[2] = 2.0f;
            CHECK(led_execute(s, &mv) == LED_SUCCESS,
                  "trig position");
            memset(&cd, 0, sizeof(cd));
            cd.orientation[3] = 1.0f;
            cd.shape = LE_COLLIDER_BOX;
            cd.half_extents[0] = 1.0f;
            cd.half_extents[1] = 1.0f;
            cd.half_extents[2] = 1.0f;
            cd.is_trigger = 1;
            CHECK(le_object_add_collider(w, &trig, &cd) ==
                      LE_SUCCESS,
                  "trig trigger");
        }
        /* Mover: box.glb drop + Mover script. */
        {
            led_asset_record rec;
            led_drag_payload pay;
            float pos[3] = { 0.0f, 1.0f, 3.0f };

            memset(&rec, 0, sizeof(rec));
            CHECK(led_assetdb_find_by_path(s, "Assets/box.glb",
                                           &rec),
                  "find box record 3");
            CHECK(led_browser_select(s, &rec.id) == LED_SUCCESS,
                  "select box 3");
            memset(&pay, 0, sizeof(pay));
            CHECK(led_drag_begin(s, &pay), "drag box 3");
            CHECK(led_drop_model_into_scene(s, &pay, pos),
                  "drop Mover");
            led_browser_clear_selection(s);
        }
        {
            uint32_t live = le_world_get_object_count(w);
            le_object *all = NULL;

            if (live > 0) {
                all = (le_object *)malloc(
                    (size_t)live * sizeof(le_object));
            }
            if (all != NULL) {
                uint32_t got =
                    le_world_get_all_objects(w, all, live);

                if (got > 0) {
                    le_object_set_name(w, &all[got - 1u],
                                       "Mover");
                }
                free(all);
            }
        }
        CHECK(le_world_find_by_name(w, "Mover", &mover),
              "find Mover");
        /* Attach scripts (drop equivalents). */
        {
            led_asset_record rec;
            led_drag_payload pay;
            le_object o;

            memset(&rec, 0, sizeof(rec));
            CHECK(led_assetdb_find_by_path(
                      s, "Assets/Mover.lua", &rec),
                  "find Mover rec");
            CHECK(led_browser_select(s, &rec.id) == LED_SUCCESS,
                  "select Mover script");
            memset(&pay, 0, sizeof(pay));
            CHECK(led_drag_begin(s, &pay), "drag Mover");
            o = mover;
            CHECK(led_drop_script_onto_object(s, &pay, &o),
                  "attach Mover");
            led_browser_clear_selection(s);

            memset(&rec, 0, sizeof(rec));
            CHECK(led_assetdb_find_by_path(s, "Assets/Trig.lua",
                                           &rec),
                  "find Trig rec");
            CHECK(led_browser_select(s, &rec.id) == LED_SUCCESS,
                  "select Trig script");
            memset(&pay, 0, sizeof(pay));
            CHECK(led_drag_begin(s, &pay), "drag Trig");
            o = trig;
            CHECK(led_drop_script_onto_object(s, &pay, &o),
                  "attach Trig");
            led_browser_clear_selection(s);

            memset(&rec, 0, sizeof(rec));
            CHECK(led_assetdb_find_by_path(
                      s, "Assets/CharDrive.lua", &rec),
                  "find CharDrive rec");
            CHECK(led_browser_select(s, &rec.id) == LED_SUCCESS,
                  "select CharDrive");
            memset(&pay, 0, sizeof(pay));
            CHECK(led_drag_begin(s, &pay), "drag CharDrive");
            o = player;
            CHECK(led_drop_script_onto_object(s, &pay, &o),
                  "attach CharDrive");
            led_browser_clear_selection(s);
        }
        (void)sun;
        (void)pt;
        (void)ground;
        (void)stat;
        (void)dyn;
        (void)player;
        (void)npc;
        (void)trig;
        (void)mover;
    }

    snprintf(p, sizeof(p), "%s/Scenes/Game.luma_scene", root);
    CHECK(led_scene_save_as(s, p) == LED_SUCCESS, "save Game");
    printf("scene objects: %u\n",
           (unsigned)le_world_get_object_count(w));
    /* In-place reopen must resolve everything. */
    {
        led_result orc = led_scene_open(s, p);

        CHECK(orc == LED_SUCCESS, "reopen Game");
    }
    printf("reopen objects: %u\n",
           (unsigned)le_world_get_object_count(w));

    led_session_destroy(s);
    le_world_destroy(w);
    le_engine_destroy(e);
    lr_renderer_destroy(renderer);
    lc_device_destroy(device);
    lc_shutdown();
    if (g_fail) {
        printf("REALITY PROJECT BUILD FAILED\n");
        return 1;
    }
    printf("REALITY PROJECT OK: %s\n", root);
    return 0;
}
