/* Phase 27 Lua input example (public engine, renderer and
 * LumaC APIs only).
 *
 * Workflow:
 *   attach window -> default input map (WASD + Space + Escape)
 *     -> build world (mouse-look camera rig, sun, WASD cube,
 *        fixed-step counter cube)
 *     -> run: le_engine_begin_frame (input+time) ->
 *        le_engine_update (scripts) -> render trio ->
 *        le_engine_end_frame; Escape toggles pause via the pause
 *        action, Space hops the mover via the jump action
 *     -> render + periodic stats (input + time + scripts)
 *
 * Controls: WASD move the cube, mouse moves the camera rig yaw
 * (captured cursor requested best-effort), Space hop, Escape
 * pause/unpause.
 *
 * Usage: lua_input [--frames N] [--no-validation] [--stats]
 *   [--scripts DIR]
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_engine/luma_engine.h>

#define FAIL_CLEANUP(what) do { \
    fprintf(stderr, "%s failed\n", what); \
    goto cleanup; \
} while (0)

static const char kCameraFallback[] =
    "export('sensitivity', 0.003)\n"
    "function update(self, dt)\n"
    "  local dx, dy = Input.mouse_delta()\n"
    "  if dx ~= 0 then\n"
    "    self:rotate_y(-dx * self.sensitivity)\n"
    "  end\n"
    "end\n";

static const char kPlayerFallback[] =
    "export('speed', 6.0)\n"
    "function update(self, dt)\n"
    "  local mx = Input.axis(\"move_x\")\n"
    "  local mz = Input.axis(\"move_z\")\n"
    "  if mx ~= 0 or mz ~= 0 then\n"
    "    local x, y, z = self:position()\n"
    "    self:set_position(x + mx * self.speed * dt, y,\n"
    "                      z + mz * self.speed * dt)\n"
    "  end\n"
    "  if Input.action_pressed(\"jump\") then\n"
    "    local x, y, z = self:position()\n"
    "    self:set_position(x, y + 1.0, z)\n"
    "  end\n"
    "  if Input.action_pressed(\"pause\") then\n"
    "    if Time.scale() == 0 then\n"
    "      Time.set_scale(1)\n"
    "    else\n"
    "      Time.set_scale(0)\n"
    "    end\n"
    "  end\n"
    "end\n";

static const char kCounterFallback[] =
    "export('ticks', 0)\n"
    "function fixed_update(self, dt)\n"
    "  self.ticks = self.ticks + 1\n"
    "end\n";

static char *read_file(const char *path, size_t *out_size) {
    FILE *f = NULL;
    long n = 0;
    char *buf = NULL;
    size_t got = 0;

    if (out_size != NULL) {
        *out_size = 0;
    }
    if (path == NULL) {
        return NULL;
    }
#if defined(_MSC_VER)
    if (fopen_s(&f, path, "rb") != 0 || f == NULL) {
        return NULL;
    }
#else
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
#endif
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (char *)malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    if (out_size != NULL) {
        *out_size = got;
    }
    return buf;
}

static int load_script(le_engine *engine, const char *dir,
                       const char *file, const char *fallback,
                       le_asset *out) {
    le_script_asset_desc d;
    char path[1024];
    char *text = NULL;
    size_t size = 0;
    le_result rc;

    memset(&d, 0, sizeof(d));
    if (dir != NULL && dir[0] != '\0') {
        snprintf(path, sizeof(path), "%s/%s", dir, file);
        text = read_file(path, &size);
    }
    if (text != NULL) {
        d.source = text;
        d.size = size;
    } else {
        d.source = fallback;
        d.size = strlen(fallback);
    }
    d.path_hint = file;
    rc = le_asset_create_script(engine, &d, out);
    free(text);
    return rc == LE_SUCCESS;
}

static void set_pos(le_world *world, le_object obj, float x,
                    float y, float z) {
    float p[3];

    p[0] = x;
    p[1] = y;
    p[2] = z;
    le_object_set_position(world, &obj, p);
}

static int make_cube_asset(le_engine *engine, le_asset *out) {
    lr_vertex v[8];
    static const uint32_t idx[36] = {
        0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
        2, 6, 7, 2, 7, 3, 0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2,
    };
    static const float pos[8][3] = {
        { -1.0f, -1.0f, -1.0f }, { 1.0f, -1.0f, -1.0f },
        { 1.0f, 1.0f, -1.0f }, { -1.0f, 1.0f, -1.0f },
        { -1.0f, -1.0f, 1.0f }, { 1.0f, -1.0f, 1.0f },
        { 1.0f, 1.0f, 1.0f }, { -1.0f, 1.0f, 1.0f },
    };
    le_mesh_asset_desc md;
    int i;

    for (i = 0; i < 8; i++) {
        memset(&v[i], 0, sizeof(v[i]));
        v[i].position[0] = pos[i][0];
        v[i].position[1] = pos[i][1];
        v[i].position[2] = pos[i][2];
        v[i].normal[2] = 1.0f;
        v[i].tangent[0] = 1.0f;
        v[i].tangent[3] = 1.0f;
    }
    memset(&md, 0, sizeof(md));
    md.vertices = v;
    md.vertex_count = 8;
    md.indices = idx;
    md.index_count = 36;
    return le_asset_create_mesh(engine, &md, out) == LE_SUCCESS;
}

static void attach_cube(le_world *world, le_object o, le_asset mesh,
                        le_asset material) {
    le_asset_renderable_desc rd;

    memset(&rd, 0, sizeof(rd));
    rd.mesh = mesh;
    rd.material = material;
    rd.casts_shadow = 0;
    rd.receives_shadow = 1;
    rd.visible = 1;
    le_object_add_asset_renderable(world, &o, &rd);
}

/* Default gameplay map (example-only, not engine-global rules):
 * move_forward=W, back=S, left=A, right=D, jump=Space,
 * pause=Escape; move_x = A(-1)/D(+1), move_z = W(-1)/S(+1). */
static int make_default_map(le_engine *engine) {
    le_input_action fwd;
    le_input_action back;
    le_input_action left;
    le_input_action right;
    le_input_action jump;
    le_input_action pause;
    le_input_axis mx;
    le_input_axis mz;
    le_axis_desc dd;
    le_input_binding b;

    if (le_input_create_action(engine, "move_forward", &fwd) !=
        LE_SUCCESS) {
        return 0;
    }
    if (le_input_create_action(engine, "move_backward", &back) !=
        LE_SUCCESS) {
        return 0;
    }
    if (le_input_create_action(engine, "move_left", &left) !=
        LE_SUCCESS) {
        return 0;
    }
    if (le_input_create_action(engine, "move_right", &right) !=
        LE_SUCCESS) {
        return 0;
    }
    if (le_input_create_action(engine, "jump", &jump) !=
        LE_SUCCESS) {
        return 0;
    }
    if (le_input_create_action(engine, "pause", &pause) !=
        LE_SUCCESS) {
        return 0;
    }
    memset(&b, 0, sizeof(b));
    b.kind = LE_BINDING_KEY;
    b.scale = 1.0f;
    b.key = LE_KEY_W;
    if (le_input_add_action_binding(engine, &fwd, &b) !=
        LE_SUCCESS) {
        return 0;
    }
    b.key = LE_KEY_S;
    if (le_input_add_action_binding(engine, &back, &b) !=
        LE_SUCCESS) {
        return 0;
    }
    b.key = LE_KEY_A;
    if (le_input_add_action_binding(engine, &left, &b) !=
        LE_SUCCESS) {
        return 0;
    }
    b.key = LE_KEY_D;
    if (le_input_add_action_binding(engine, &right, &b) !=
        LE_SUCCESS) {
        return 0;
    }
    b.key = LE_KEY_SPACE;
    if (le_input_add_action_binding(engine, &jump, &b) !=
        LE_SUCCESS) {
        return 0;
    }
    b.key = LE_KEY_ESCAPE;
    if (le_input_add_action_binding(engine, &pause, &b) !=
        LE_SUCCESS) {
        return 0;
    }
    memset(&dd, 0, sizeof(dd));
    dd.name = "move_x";
    dd.scale = 1.0f;
    if (le_input_create_axis(engine, &dd, &mx) != LE_SUCCESS) {
        return 0;
    }
    memset(&dd, 0, sizeof(dd));
    dd.name = "move_z";
    dd.scale = 1.0f;
    if (le_input_create_axis(engine, &dd, &mz) != LE_SUCCESS) {
        return 0;
    }
    memset(&b, 0, sizeof(b));
    b.kind = LE_BINDING_KEY;
    b.key = LE_KEY_A;
    b.scale = -1.0f;
    if (le_input_add_axis_binding(engine, &mx, &b) != LE_SUCCESS) {
        return 0;
    }
    b.key = LE_KEY_D;
    b.scale = 1.0f;
    if (le_input_add_axis_binding(engine, &mx, &b) != LE_SUCCESS) {
        return 0;
    }
    b.key = LE_KEY_W;
    b.scale = -1.0f;
    if (le_input_add_axis_binding(engine, &mz, &b) != LE_SUCCESS) {
        return 0;
    }
    b.key = LE_KEY_S;
    b.scale = 1.0f;
    if (le_input_add_axis_binding(engine, &mz, &b) != LE_SUCCESS) {
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    unsigned long max_frames = 600;
    int use_validation = 1;
    int show_stats = 0;
    const char *scripts_dir = "scripts";
    unsigned long i;
    lc_window_desc window_desc;
    lc_window *window = NULL;
    lc_device_desc device_desc;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain_desc swapchain_desc;
    lc_swapchain *swapchain = NULL;
    lr_renderer_desc rdesc;
    lr_renderer *renderer = NULL;
    le_engine *engine = NULL;
    le_world *world = NULL;
    le_asset cube = LE_ASSET_INVALID;
    le_asset mat = LE_ASSET_INVALID;
    le_asset cam_script = LE_ASSET_INVALID;
    le_asset player_script = LE_ASSET_INVALID;
    le_asset counter_script = LE_ASSET_INVALID;
    le_object player_obj = LE_OBJECT_INVALID;
    int ok = 0;

    for (i = 1; i < (unsigned long)argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--no-validation") == 0) {
            use_validation = 0;
        } else if (strcmp(argv[i], "--stats") == 0) {
            show_stats = 1;
        } else if (strcmp(argv[i], "--scripts") == 0 &&
                   i + 1 < argc) {
            scripts_dir = argv[++i];
        } else {
            fprintf(stderr,
                    "usage: lua_input [--frames N] "
                    "[--no-validation] [--stats] "
                    "[--scripts DIR]\n");
            return 1;
        }
    }
    if (max_frames < 1) {
        max_frames = 1;
    }
    memset(&window_desc, 0, sizeof(window_desc));
    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }
    window_desc.title = "Luma Lua Input";
    window_desc.width = 800;
    window_desc.height = 600;
    if (lc_window_create(&window_desc, &window) != LC_SUCCESS) {
        fprintf(stderr, "lc_window_create failed\n");
        lc_shutdown();
        return 1;
    }
    memset(&device_desc, 0, sizeof(device_desc));
    device_desc.backend = LC_BACKEND_VULKAN;
    device_desc.enable_validation = use_validation;
    if (lc_device_create(&device_desc, &device) != LC_SUCCESS) {
        FAIL_CLEANUP("lc_device_create");
    }
    if (lc_surface_create(device, window, &surface) != LC_SUCCESS) {
        FAIL_CLEANUP("lc_surface_create");
    }
    memset(&swapchain_desc, 0, sizeof(swapchain_desc));
    swapchain_desc.width = lc_window_get_width(window);
    swapchain_desc.height = lc_window_get_height(window);
    swapchain_desc.image_count = 0;
    swapchain_desc.vsync = 1;
    if (lc_swapchain_create(device, surface, &swapchain_desc,
                            &swapchain) != LC_SUCCESS) {
        FAIL_CLEANUP("lc_swapchain_create");
    }
    memset(&rdesc, 0, sizeof(rdesc));
    rdesc.device = device;
    rdesc.render_target.color_attachment_count = 1;
    rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
    rdesc.max_objects = 4096;
    rdesc.ambient_light[0] = 0.35f;
    rdesc.ambient_light[1] = 0.35f;
    rdesc.ambient_light[2] = 0.4f;
    if (lr_renderer_create(&rdesc, &renderer) != LR_SUCCESS) {
        FAIL_CLEANUP("lr_renderer_create");
    }
    {
        le_engine_desc edesc;
        le_world_desc wdesc;

        memset(&edesc, 0, sizeof(edesc));
        edesc.renderer = renderer;
        if (le_engine_create(&edesc, &engine) != LE_SUCCESS) {
            FAIL_CLEANUP("le_engine_create");
        }
        if (le_engine_attach_window(engine, window) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("attach window");
        }
        memset(&wdesc, 0, sizeof(wdesc));
        if (le_world_create(engine, &wdesc, &world) != LE_SUCCESS) {
            FAIL_CLEANUP("le_world_create");
        }
    }
    if (!make_cube_asset(engine, &cube)) {
        FAIL_CLEANUP("cube asset");
    }
    {
        le_material_asset_desc md;

        memset(&md, 0, sizeof(md));
        md.base_color_factor[0] = 0.75f;
        md.base_color_factor[1] = 0.45f;
        md.base_color_factor[2] = 0.2f;
        md.base_color_factor[3] = 1.0f;
        md.metallic_factor = 0.1f;
        md.roughness_factor = 0.6f;
        if (le_asset_create_material(engine, &md, &mat) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("material asset");
        }
    }
    if (!make_default_map(engine)) {
        FAIL_CLEANUP("default input map");
    }
    if (!load_script(engine, scripts_dir, "camera.lua",
                     kCameraFallback, &cam_script)) {
        FAIL_CLEANUP("camera script");
    }
    if (!load_script(engine, scripts_dir, "player.lua",
                     kPlayerFallback, &player_script)) {
        FAIL_CLEANUP("player script");
    }
    if (!load_script(engine, scripts_dir, "counter.lua",
                     kCounterFallback, &counter_script)) {
        FAIL_CLEANUP("counter script");
    }
    /* Camera rig + sun + player + counter. */
    {
        le_object rig;
        le_object cam;
        le_object sun;
        le_object player;
        le_object counter;
        le_camera_desc cd;
        le_light_desc ld;
        float yaw[4];
        float pitch[4];
        float q[4];
        float xa[3] = { 1.0f, 0.0f, 0.0f };
        float ya[3] = { 0.0f, 1.0f, 0.0f };
        float saxis[3] = { 1.0f, 0.0f, 0.0f };
        float sq[4];

        le_object_create(world, &rig);
        le_object_create(world, &cam);
        le_object_set_name(world, &rig, "CameraRig");
        le_object_set_name(world, &cam, "Camera");
        le_object_set_parent(world, &cam, &rig);
        set_pos(world, rig, 0.0f, 4.0f, 11.0f);
        le_quat_from_axis_angle(ya, 0.0f, yaw);
        le_quat_from_axis_angle(xa, -0.35f, pitch);
        le_quat_multiply(yaw, pitch, q);
        le_object_set_rotation(world, &rig, q);
        if (le_object_add_script(world, &rig, &cam_script) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("attach camera script");
        }
        le_camera_desc_default(&cd);
        cd.far_plane = 200.0f;
        le_object_add_camera(world, &cam, &cd);
        le_world_set_active_camera(world, &cam);

        le_object_create(world, &sun);
        le_object_set_name(world, &sun, "Sun");
        le_quat_from_axis_angle(saxis, -0.9f, sq);
        le_object_set_rotation(world, &sun, sq);
        memset(&ld, 0, sizeof(ld));
        ld.type = LE_LIGHT_DIRECTIONAL;
        ld.color[0] = 1.0f;
        ld.color[1] = 1.0f;
        ld.color[2] = 1.0f;
        ld.intensity = 3.0f;
        le_object_add_light(world, &sun, &ld);

        le_object_create(world, &player);
        le_object_set_name(world, &player, "Player");
        set_pos(world, player, 0.0f, 0.5f, 0.0f);
        attach_cube(world, player, cube, mat);
        if (le_object_add_script(world, &player,
                                 &player_script) != LE_SUCCESS) {
            FAIL_CLEANUP("attach player script");
        }
        player_obj = player;

        le_object_create(world, &counter);
        le_object_set_name(world, &counter, "Counter");
        set_pos(world, counter, 3.5f, 0.5f, 2.0f);
        attach_cube(world, counter, cube, mat);
        if (le_object_add_script(world, &counter,
                                 &counter_script) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("attach counter script");
        }
    }
    le_input_set_cursor_mode(engine, LE_CURSOR_CAPTURED);
    printf("lua_input: WASD move, mouse look, Space hop, Escape "
           "pause. Close the window or --frames N to stop.\n");
    /* Frame loop over the explicit lifecycle (host owns it). */
    {
        unsigned long frame = 0;

        for (frame = 0; frame < max_frames; frame++) {
            lc_command_encoder *enc = NULL;

            if (le_engine_begin_frame(engine) != LE_SUCCESS) {
                FAIL_CLEANUP("begin_frame");
            }
            if (le_engine_app_state(engine) ==
                LE_APP_QUIT_REQUESTED) {
                le_engine_end_frame(engine);
                break;
            }
            if (lc_window_should_close(window)) {
                le_engine_request_quit(engine);
                le_engine_end_frame(engine);
                break;
            }
            if (le_engine_update(engine, world) != LE_SUCCESS) {
                le_engine_end_frame(engine);
                FAIL_CLEANUP("engine update");
            }
            if (lc_begin_frame(swapchain) != LC_SUCCESS) {
                le_engine_end_frame(engine);
                FAIL_CLEANUP("begin_frame");
            }
            if (lc_swapchain_get_encoder(swapchain, &enc) !=
                LC_SUCCESS) {
                le_engine_end_frame(engine);
                FAIL_CLEANUP("get_encoder");
            }
            {
                uint32_t w = lc_window_get_width(window);
                uint32_t h = lc_window_get_height(window);

                if (le_world_render_scene(world, enc, w, h) !=
                    LE_SUCCESS) {
                    le_engine_end_frame(engine);
                    FAIL_CLEANUP("render_scene");
                }
            }
            {
                lc_render_swapchain_pass_desc spass;

                memset(&spass, 0, sizeof(spass));
                spass.color_load_op = LC_LOAD_OP_CLEAR;
                spass.color_store_op = LC_STORE_OP_STORE;
                spass.depth_load_op = LC_LOAD_OP_CLEAR;
                spass.depth_store_op = LC_STORE_OP_DONT_CARE;
                spass.clear_depth = 1.0f;
                if (lc_encoder_begin_swapchain_pass(enc, swapchain,
                                                    &spass) !=
                    LC_SUCCESS) {
                    le_engine_end_frame(engine);
                    FAIL_CLEANUP("swapchain pass");
                }
                if (le_world_render_output(
                        world, enc,
                        lc_swapchain_get_render_target(
                            swapchain)) != LE_SUCCESS) {
                    lc_encoder_end_render_pass(enc);
                    le_world_render_end(world);
                } else {
                    lc_encoder_end_render_pass(enc);
                    le_world_render_end(world);
                }
            }
            {
                lc_result r = lc_end_frame(swapchain);

                if (r != LC_SUCCESS && r != LC_SUBOPTIMAL) {
                    le_engine_end_frame(engine);
                    FAIL_CLEANUP("end_frame");
                }
            }
            if (le_engine_end_frame(engine) != LE_SUCCESS) {
                FAIL_CLEANUP("end_frame");
            }
            if (show_stats && (frame % 60u) == 0u) {
                le_render_report rep;
                le_script_stats sstats;
                le_input_stats istats;
                le_time_stats tstats;

                le_world_get_last_render_report(world, &rep);
                le_script_get_stats(engine, &sstats);
                le_input_get_stats(engine, &istats);
                le_time_get_stats(engine, &tstats);
                printf("frame %lu: submitted=%u scripts=%u "
                       "errors=%u keys=%u player_failed=%d "
                       "scale=%.2f fixed=%s\n",
                       frame, rep.submitted,
                       sstats.script_instances,
                       sstats.errors_total, istats.keys_down,
                       (int)le_object_script_failed(world,
                                                    &player_obj),
                       tstats.time_scale,
                       tstats.time_scale == 0.0f ? "paused"
                                                 : "running");
            }
        }
    }
    ok = 1;

cleanup:
    if (engine != NULL) {
        le_engine_detach_window(engine, window);
    }
    le_world_destroy(world);
    if (engine != NULL) {
        le_asset_unload(engine, &cam_script);
        le_asset_unload(engine, &player_script);
        le_asset_unload(engine, &counter_script);
        le_asset_unload(engine, &cube);
        le_asset_unload(engine, &mat);
    }
    le_engine_destroy(engine);
    lr_renderer_destroy(renderer);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return ok ? 0 : 1;
}
