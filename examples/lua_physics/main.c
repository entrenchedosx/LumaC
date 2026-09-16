/* Phase 28 Lua physics example (public engine, renderer and
 * LumaC APIs only).
 *
 * Scene: a static floor, three dynamic crates (one scripted with
 * collision callbacks), a kinematic pusher patrolling on
 * fixed_update, and a trigger zone counting overlaps. Scripts
 * apply forces, read velocities, and raycast; the engine owns
 * every rigid-body decision.
 *
 * Usage: lua_physics [--frames N] [--no-validation] [--stats]
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

static const char kDropFallback[] =
    "function collision_enter(self, other, contact)\n"
    "  print('[drop] first contact')\n"
    "end\n";

static const char kPusherFallback[] =
    "local t = 0.0\n"
    "function fixed_update(self, dt)\n"
    "  t = t + dt\n"
    "  local _, y, z = self:position()\n"
    "  self:set_position(3.0 * math.sin(t), y, z)\n"
    "end\n";

static const char kZoneFallback[] =
    "function trigger_enter(self, other, contact)\n"
    "  print('[zone] enter')\n"
    "end\n"
    "function trigger_exit(self, other, contact)\n"
    "  print('[zone] exit')\n"
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

static void add_box_body(le_world *world, le_object o,
                         le_body_type type, float mass, float hx,
                         float hy, float hz, int trigger) {
    le_rigid_body_desc bd;
    le_collider_desc cd;

    memset(&bd, 0, sizeof(bd));
    bd.type = type;
    bd.mass = (type == LE_BODY_DYNAMIC) ? mass : 0.0f;
    bd.gravity_scale = 1.0f;
    le_object_add_rigid_body(world, &o, &bd);
    memset(&cd, 0, sizeof(cd));
    cd.shape = LE_COLLIDER_BOX;
    cd.half_extents[0] = hx;
    cd.half_extents[1] = hy;
    cd.half_extents[2] = hz;
    cd.orientation[3] = 1.0f;
    cd.is_trigger = trigger;
    cd.mask = 0xFFFFFFFFu;
    cd.friction = 0.5f;
    cd.restitution = (type == LE_BODY_DYNAMIC) ? 0.3f : 0.1f;
    le_object_add_collider(world, &o, &cd);
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
    le_asset drop = LE_ASSET_INVALID;
    le_asset pusher = LE_ASSET_INVALID;
    le_asset zone = LE_ASSET_INVALID;
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
                    "usage: lua_physics [--frames N] "
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
    window_desc.title = "Luma Physics";
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
    if (!load_script(engine, scripts_dir, "drop.lua",
                     kDropFallback, &drop)) {
        FAIL_CLEANUP("drop script");
    }
    if (!load_script(engine, scripts_dir, "pusher.lua",
                     kPusherFallback, &pusher)) {
        FAIL_CLEANUP("pusher script");
    }
    if (!load_script(engine, scripts_dir, "zone.lua",
                     kZoneFallback, &zone)) {
        FAIL_CLEANUP("zone script");
    }
    /* Camera + sun. */
    {
        le_object rig;
        le_object cam;
        le_object sun;
        le_camera_desc cd;
        le_light_desc ld;
        float q[4] = { 0.3f, 0.0f, 0.0f, 0.95f };
        float sq[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float nq[4];

        le_object_create(world, &rig);
        le_object_create(world, &cam);
        le_object_set_parent(world, &cam, &rig);
        set_pos(world, rig, 0.0f, 5.0f, 12.0f);
        le_quat_normalize(q, nq);
        le_object_set_rotation(world, &rig, nq);
        le_camera_desc_default(&cd);
        cd.far_plane = 200.0f;
        le_object_add_camera(world, &cam, &cd);
        le_world_set_active_camera(world, &cam);

        le_object_create(world, &sun);
        le_object_set_rotation(world, &sun, sq);
        memset(&ld, 0, sizeof(ld));
        ld.type = LE_LIGHT_DIRECTIONAL;
        ld.color[0] = 1.0f;
        ld.color[1] = 1.0f;
        ld.color[2] = 1.0f;
        ld.intensity = 3.0f;
        le_object_add_light(world, &sun, &ld);
    }
    /* Floor (static), crates (dynamic), pusher (kinematic),
     * zone (trigger). */
    {
        le_object floor_o;
        le_object crate;
        le_object push;
        le_object zone_o;
        int k;

        le_object_create(world, &floor_o);
        le_object_set_name(world, &floor_o, "Floor");
        set_pos(world, floor_o, 0.0f, -0.5f, 0.0f);
        attach_cube(world, floor_o, cube, mat);
        add_box_body(world, floor_o, LE_BODY_STATIC, 0.0f, 8.0f,
                     0.5f, 8.0f, 0);

        for (k = 0; k < 3; k++) {
            le_object_create(world, &crate);
            if (k == 0) {
                le_object_set_name(world, &crate, "DropCrate");
            }
            set_pos(world, crate, -2.0f + 2.0f * (float)k,
                    4.0f + (float)k, 0.0f);
            attach_cube(world, crate, cube, mat);
            add_box_body(world, crate, LE_BODY_DYNAMIC, 1.0f,
                         0.5f, 0.5f, 0.5f, 0);
            if (k == 0 &&
                le_object_add_script(world, &crate, &drop) !=
                    LE_SUCCESS) {
                FAIL_CLEANUP("attach drop");
            }
        }

        le_object_create(world, &push);
        le_object_set_name(world, &push, "Pusher");
        set_pos(world, push, 0.0f, 0.5f, 3.0f);
        attach_cube(world, push, cube, mat);
        add_box_body(world, push, LE_BODY_KINEMATIC, 0.0f, 0.5f,
                     0.5f, 0.5f, 0);
        if (le_object_add_script(world, &push, &pusher) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("attach pusher");
        }

        le_object_create(world, &zone_o);
        le_object_set_name(world, &zone_o, "Zone");
        set_pos(world, zone_o, 0.0f, 3.0f, -4.0f);
        add_box_body(world, zone_o, LE_BODY_STATIC, 0.0f, 2.0f,
                     2.0f, 2.0f, 1);
        if (le_object_add_script(world, &zone_o, &zone) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("attach zone");
        }
    }
    if (le_script_set_fixed_step(world, 1.0f / 60.0f, 4) !=
        LE_SUCCESS) {
        FAIL_CLEANUP("fixed step");
    }
    printf("lua_physics: crates fall, the pusher patrols, the "
           "zone counts overlaps.\n");
    {
        unsigned long frame = 0;

        for (frame = 0; frame < max_frames; frame++) {
            lc_command_encoder *enc = NULL;
            float dt = 1.0f / 60.0f;

            lc_poll_events();
            if (lc_window_should_close(window)) {
                break;
            }
            if (le_world_update(world, dt) != LE_SUCCESS) {
                FAIL_CLEANUP("world update");
            }
            if (lc_begin_frame(swapchain) != LC_SUCCESS) {
                FAIL_CLEANUP("begin_frame");
            }
            if (lc_swapchain_get_encoder(swapchain, &enc) !=
                LC_SUCCESS) {
                FAIL_CLEANUP("get_encoder");
            }
            {
                uint32_t w = lc_window_get_width(window);
                uint32_t h = lc_window_get_height(window);

                if (le_world_render_scene(world, enc, w, h) !=
                    LE_SUCCESS) {
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
                    FAIL_CLEANUP("end_frame");
                }
            }
            if (show_stats && (frame % 60u) == 0u) {
                le_render_report rep;
                le_physics_stats pstats;

                le_world_get_last_render_report(world, &rep);
                le_physics_get_stats(world, &pstats);
                printf("frame %lu: submitted=%u "
                       "bodies=%u colliders=%u contacts=%u\n",
                       frame, rep.submitted, pstats.body_count,
                       pstats.collider_count,
                       pstats.contact_count);
            }
        }
    }
    ok = 1;

cleanup:
    le_world_destroy(world);
    if (engine != NULL) {
        le_asset_unload(engine, &drop);
        le_asset_unload(engine, &pusher);
        le_asset_unload(engine, &zone);
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
