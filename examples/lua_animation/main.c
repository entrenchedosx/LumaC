/* Phase 29 Lua animation example (public engine, renderer and
 * LumaC APIs only).
 *
 * Scene: a two-joint procedural arm (skinned bar mesh) waving
 * and nodding under a conductor script (crossfades every 4 s),
 * plus a door object driven by an object-target clip (no
 * skeleton). The engine owns sampling, blending, and skin
 * palettes; the renderer skins on the GPU; Lua only conducts.
 *
 * Usage: lua_animation [--frames N] [--no-validation] [--stats]
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

#ifndef LE_PI_F
#define LE_PI_F 3.14159265358979323846f
#endif

static const char kConductorFallback[] =
    "local wave = nil\n"
    "local nod = nil\n"
    "local timer = 0.0\n"
    "local showing_wave = true\n"
    "function start(self)\n"
    "  wave = Assets.find_by_id(\"WAVE_HEX\")\n"
    "  nod = Assets.find_by_id(\"NOD_HEX\")\n"
    "  if wave == nil or nod == nil then\n"
    "    print('[conductor] missing clips')\n"
    "    return\n"
    "  end\n"
    "  self:animation_play(wave)\n"
    "  print('[conductor] playing wave')\n"
    "end\n"
    "function update(self, dt)\n"
    "  if wave == nil or nod == nil then return end\n"
    "  timer = timer + dt\n"
    "  if timer >= 4.0 then\n"
    "    timer = 0.0\n"
    "    showing_wave = not showing_wave\n"
    "    if showing_wave then\n"
    "      self:animation_crossfade(wave, 0.5)\n"
    "      print('[conductor] crossfade -> wave')\n"
    "    else\n"
    "      self:animation_crossfade(nod, 0.5)\n"
    "      print('[conductor] crossfade -> nod')\n"
    "    end\n"
    "  end\n"
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

/* Bake the two clip ID hex strings into the conductor source
 * (replaces the WAVE_HEX / NOD_HEX placeholders). Two passes:
 * measure exactly, then fill (never overflows). */
static char *bake_clips(const char *src, const char *wave_hex,
                         const char *nod_hex) {
    size_t wave_n = strlen(wave_hex);
    size_t nod_n = strlen(nod_hex);
    size_t total = 1u;
    const char *p = src;
    char *out = NULL;
    char *w = NULL;

    while (*p != '\0') {
        if (strncmp(p, "WAVE_HEX", 8) == 0) {
            total += wave_n;
            p += 8;
        } else if (strncmp(p, "NOD_HEX", 7) == 0) {
            total += nod_n;
            p += 7;
        } else {
            total += 1u;
            p += 1;
        }
    }
    out = (char *)malloc(total);
    if (out == NULL) {
        return NULL;
    }
    w = out;
    p = src;
    while (*p != '\0') {
        if (strncmp(p, "WAVE_HEX", 8) == 0) {
            memcpy(w, wave_hex, wave_n);
            w += wave_n;
            p += 8;
        } else if (strncmp(p, "NOD_HEX", 7) == 0) {
            memcpy(w, nod_hex, nod_n);
            w += nod_n;
            p += 7;
        } else {
            *w++ = *p++;
        }
    }
    *w = '\0';
    return out;
}

/* Two-joint arm: root at origin, elbow at (0,1,0). Inverse bind
 * = inverse of the bind globals (identity / translate back). */
static int make_arm_skeleton(le_engine *engine, le_asset *out) {
    le_skeleton_joint_desc joints[2];
    le_skeleton_asset_desc d;

    memset(joints, 0, sizeof(joints));
    memcpy(joints[0].name, "shoulder", 9);
    joints[0].parent = -1;
    joints[0].translation[1] = 0.0f;
    joints[0].rotation[3] = 1.0f;
    joints[0].scale[0] = joints[0].scale[1] =
        joints[0].scale[2] = 1.0f;
    joints[0].inverse_bind[0] = joints[0].inverse_bind[5] =
        joints[0].inverse_bind[10] =
            joints[0].inverse_bind[15] = 1.0f;
    memcpy(joints[1].name, "elbow", 6);
    joints[1].parent = 0;
    joints[1].translation[1] = 1.0f;
    joints[1].rotation[3] = 1.0f;
    joints[1].scale[0] = joints[1].scale[1] =
        joints[1].scale[2] = 1.0f;
    /* inverse of translate(0,1,0): translate(0,-1,0). */
    joints[1].inverse_bind[0] = joints[1].inverse_bind[5] =
        joints[1].inverse_bind[10] =
            joints[1].inverse_bind[15] = 1.0f;
    joints[1].inverse_bind[13] = -1.0f;
    memset(&d, 0, sizeof(d));
    d.joints = joints;
    d.joint_count = 2;
    return le_asset_create_skeleton(engine, &d, out) ==
        LE_SUCCESS;
}

static void z_rot(float degrees, float out_q[4]) {
    float half = degrees * LE_PI_F / 360.0f;

    out_q[0] = 0.0f;
    out_q[1] = 0.0f;
    out_q[2] = sinf(half);
    out_q[3] = cosf(half);
}

static void x_rot(float degrees, float out_q[4]) {
    float half = degrees * LE_PI_F / 360.0f;

    out_q[0] = sinf(half);
    out_q[1] = 0.0f;
    out_q[2] = 0.0f;
    out_q[3] = cosf(half);
}

/* Elbow wave: joint 1 rotation.z keys over 2 s (loops). */
static int make_wave_clip(le_engine *engine, le_asset *out) {
    float times[5] = { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f };
    float values[5][4];
    le_anim_track_desc track;
    le_animation_clip_desc d;
    float q[4];
    int k;
    static const float kDeg[5] = { 0.0f, 50.0f, 0.0f,
                                   -50.0f, 0.0f };

    for (k = 0; k < 5; k++) {
        z_rot(kDeg[k], q);
        memcpy(values[k], q, sizeof(q));
    }
    memset(&track, 0, sizeof(track));
    track.target_kind = LE_ANIM_TARGET_JOINT;
    track.target_index = 1;
    track.channel = LE_ANIM_CHANNEL_ROTATION;
    track.interpolation = LE_ANIM_INTERP_LINEAR;
    track.times = times;
    track.values = &values[0][0];
    track.key_count = 5;
    memset(&d, 0, sizeof(d));
    d.duration = 2.0f;
    d.tracks = &track;
    d.track_count = 1;
    return le_asset_create_clip(engine, &d, out) == LE_SUCCESS;
}

/* Shoulder nod: joint 0 rotation.x keys over 2 s (loops). */
static int make_nod_clip(le_engine *engine, le_asset *out) {
    float times[3] = { 0.0f, 1.0f, 2.0f };
    float values[3][4];
    le_anim_track_desc track;
    le_animation_clip_desc d;
    float q[4];
    int k;
    static const float kDeg[3] = { 0.0f, 25.0f, 0.0f };

    for (k = 0; k < 3; k++) {
        x_rot(kDeg[k], q);
        memcpy(values[k], q, sizeof(q));
    }
    memset(&track, 0, sizeof(track));
    track.target_kind = LE_ANIM_TARGET_JOINT;
    track.target_index = 0;
    track.channel = LE_ANIM_CHANNEL_ROTATION;
    track.interpolation = LE_ANIM_INTERP_LINEAR;
    track.times = times;
    track.values = &values[0][0];
    track.key_count = 3;
    memset(&d, 0, sizeof(d));
    d.duration = 2.0f;
    d.tracks = &track;
    d.track_count = 1;
    return le_asset_create_clip(engine, &d, out) == LE_SUCCESS;
}

/* Door swing: OBJECT-target rotation.y 0 -> 80 deg ping-pong. */
static int make_door_clip(le_engine *engine, le_asset *out) {
    float times[2] = { 0.0f, 3.0f };
    float values[2][4];
    le_anim_track_desc track;
    le_animation_clip_desc d;
    float half = 80.0f * LE_PI_F / 360.0f;

    values[0][0] = values[0][1] = values[0][2] = 0.0f;
    values[0][3] = 1.0f;
    values[1][0] = 0.0f;
    values[1][1] = sinf(half);
    values[1][2] = 0.0f;
    values[1][3] = cosf(half);
    memset(&track, 0, sizeof(track));
    track.target_kind = LE_ANIM_TARGET_OBJECT;
    track.target_index = 0;
    track.channel = LE_ANIM_CHANNEL_ROTATION;
    track.interpolation = LE_ANIM_INTERP_LINEAR;
    track.times = times;
    track.values = &values[0][0];
    track.key_count = 2;
    memset(&d, 0, sizeof(d));
    d.duration = 3.0f;
    d.tracks = &track;
    d.track_count = 1;
    return le_asset_create_clip(engine, &d, out) == LE_SUCCESS;
}

/* Skinned bar: 9 levels x 4 corners (y 0..2, half-width 0.15).
 * Weights blend joint 0 -> 1 across y in [0.7, 1.3]. */
#define ARM_LEVELS 9
#define ARM_CORNERS 4

static int make_arm_mesh(le_engine *engine, le_asset *out) {
    lr_vertex verts[ARM_LEVELS * ARM_CORNERS];
    uint32_t idx[(ARM_LEVELS - 1u) * ARM_CORNERS * 6u];
    le_mesh_asset_desc md;
    uint32_t l;
    uint32_t c;
    uint32_t q = 0;
    static const float kCorner[4][2] = {
        { -0.15f, -0.15f }, { 0.15f, -0.15f },
        { 0.15f, 0.15f }, { -0.15f, 0.15f },
    };

    for (l = 0; l < ARM_LEVELS; l++) {
        float y = 2.0f * (float)l / (float)(ARM_LEVELS - 1u);
        float w1 = (y - 0.7f) / 0.6f;

        if (w1 < 0.0f) {
            w1 = 0.0f;
        } else if (w1 > 1.0f) {
            w1 = 1.0f;
        }
        for (c = 0; c < ARM_CORNERS; c++) {
            lr_vertex *v = &verts[l * ARM_CORNERS + c];

            memset(v, 0, sizeof(*v));
            v->position[0] = kCorner[c][0];
            v->position[1] = y;
            v->position[2] = kCorner[c][1];
            v->normal[2] = 1.0f;
            v->tangent[0] = 1.0f;
            v->tangent[3] = 1.0f;
            v->joints[0] = 0;
            v->joints[1] = 1;
            v->joints[2] = 0;
            v->joints[3] = 0;
            v->weights[0] = 1.0f - w1;
            v->weights[1] = w1;
            v->weights[2] = 0.0f;
            v->weights[3] = 0.0f;
        }
    }
    for (l = 0; l < ARM_LEVELS - 1u; l++) {
        for (c = 0; c < ARM_CORNERS; c++) {
            uint32_t c2 = (c + 1u) % ARM_CORNERS;
            uint32_t a = l * ARM_CORNERS + c;
            uint32_t b = l * ARM_CORNERS + c2;
            uint32_t d = (l + 1u) * ARM_CORNERS + c;
            uint32_t e = (l + 1u) * ARM_CORNERS + c2;

            idx[q++] = a;
            idx[q++] = b;
            idx[q++] = e;
            idx[q++] = a;
            idx[q++] = e;
            idx[q++] = d;
        }
    }
    memset(&md, 0, sizeof(md));
    md.vertices = verts;
    md.vertex_count = ARM_LEVELS * ARM_CORNERS;
    md.indices = idx;
    md.index_count = q;
    return le_asset_create_mesh(engine, &md, out) == LE_SUCCESS;
}

static void set_pos(le_world *world, le_object obj, float x,
                    float y, float z) {
    float p[3];

    p[0] = x;
    p[1] = y;
    p[2] = z;
    le_object_set_position(world, &obj, p);
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
    le_asset arm_mesh = LE_ASSET_INVALID;
    le_asset door_mesh = LE_ASSET_INVALID;
    le_asset mat = LE_ASSET_INVALID;
    le_asset skel = LE_ASSET_INVALID;
    le_asset wave = LE_ASSET_INVALID;
    le_asset nod = LE_ASSET_INVALID;
    le_asset door_clip = LE_ASSET_INVALID;
    le_asset conductor = LE_ASSET_INVALID;
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
                    "usage: lua_animation [--frames N] "
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
    window_desc.title = "Luma Animation";
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
    if (!make_arm_mesh(engine, &arm_mesh)) {
        FAIL_CLEANUP("arm mesh");
    }
    /* Door: rigid box (object clip carries the swing). */
    {
        lr_vertex v[8];
        static const uint32_t idx[36] = {
            0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5,
            0, 5, 1, 2, 6, 7, 2, 7, 3, 0, 3, 7, 0, 7, 4,
            1, 5, 6, 1, 6, 2,
        };
        static const float pos[8][3] = {
            { -0.5f, 0.0f, -0.05f }, { 0.5f, 0.0f, -0.05f },
            { 0.5f, 2.0f, -0.05f }, { -0.5f, 2.0f, -0.05f },
            { -0.5f, 0.0f, 0.05f }, { 0.5f, 0.0f, 0.05f },
            { 0.5f, 2.0f, 0.05f }, { -0.5f, 2.0f, 0.05f },
        };
        le_mesh_asset_desc md;
        int k;

        for (k = 0; k < 8; k++) {
            memset(&v[k], 0, sizeof(v[k]));
            memcpy(v[k].position, pos[k], sizeof(pos[k]));
            v[k].normal[2] = 1.0f;
            v[k].tangent[0] = 1.0f;
            v[k].tangent[3] = 1.0f;
            v[k].weights[0] = 1.0f; /* rigid */
        }
        memset(&md, 0, sizeof(md));
        md.vertices = v;
        md.vertex_count = 8;
        md.indices = idx;
        md.index_count = 36;
        if (le_asset_create_mesh(engine, &md, &door_mesh) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("door mesh");
        }
    }
    {
        le_material_asset_desc md;

        memset(&md, 0, sizeof(md));
        md.base_color_factor[0] = 0.3f;
        md.base_color_factor[1] = 0.55f;
        md.base_color_factor[2] = 0.8f;
        md.base_color_factor[3] = 1.0f;
        md.metallic_factor = 0.1f;
        md.roughness_factor = 0.6f;
        if (le_asset_create_material(engine, &md, &mat) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("material asset");
        }
    }
    if (!make_arm_skeleton(engine, &skel)) {
        FAIL_CLEANUP("skeleton");
    }
    if (!make_wave_clip(engine, &wave)) {
        FAIL_CLEANUP("wave clip");
    }
    if (!make_nod_clip(engine, &nod)) {
        FAIL_CLEANUP("nod clip");
    }
    if (!make_door_clip(engine, &door_clip)) {
        FAIL_CLEANUP("door clip");
    }
    /* Conductor script (clip IDs baked in). */
    {
        le_asset_info wi;
        le_asset_info ni;
        char whex[33];
        char nhex[33];
        char path[1024];
        char *text = NULL;
        size_t size = 0;
        char *baked = NULL;
        le_script_asset_desc sd;

        le_asset_get_info(engine, &wave, &wi);
        le_asset_get_info(engine, &nod, &ni);
        le_asset_id_to_string(&wi.id, whex);
        le_asset_id_to_string(&ni.id, nhex);
        /* Conductor source (file or fallback) with the real
         * clip IDs baked in. */
        if (scripts_dir != NULL && scripts_dir[0] != '\0') {
            snprintf(path, sizeof(path), "%s/%s", scripts_dir,
                     "conductor.lua");
            text = read_file(path, &size);
        }
        baked = bake_clips((text != NULL) ? text
                                          : kConductorFallback,
                           whex, nhex);
        free(text);
        if (baked == NULL) {
            FAIL_CLEANUP("bake conductor");
        }
        if (baked == NULL) {
            FAIL_CLEANUP("bake conductor");
        }
        memset(&sd, 0, sizeof(sd));
        sd.source = baked;
        sd.size = strlen(baked);
        sd.path_hint = "conductor.lua";
        if (le_asset_create_script(engine, &sd, &conductor) !=
            LE_SUCCESS) {
            free(baked);
            FAIL_CLEANUP("conductor script");
        }
        free(baked);
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
        set_pos(world, rig, 0.0f, 1.6f, 6.0f);
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
    /* Arm (skinned, scripted conductor) + door (object clip). */
    {
        le_object arm;
        le_object door;
        le_asset_renderable_desc rd;
        le_animator_desc ad;

        le_object_create(world, &arm);
        le_object_set_name(world, &arm, "Arm");
        set_pos(world, arm, -1.2f, 0.0f, 0.0f);
        memset(&rd, 0, sizeof(rd));
        rd.mesh = arm_mesh;
        rd.material = mat;
        rd.casts_shadow = 1;
        rd.receives_shadow = 1;
        rd.visible = 1;
        if (le_object_add_asset_renderable(world, &arm, &rd) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("arm renderable");
        }
        memset(&ad, 0, sizeof(ad));
        ad.skeleton = skel;
        ad.clip = wave;
        ad.autoplay = 1;
        ad.loop_mode = LE_ANIM_LOOP;
        ad.speed = 1.0f;
        if (le_object_add_animator(world, &arm, &ad) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("arm animator");
        }
        if (le_object_add_script(world, &arm, &conductor) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("arm conductor");
        }

        le_object_create(world, &door);
        le_object_set_name(world, &door, "Door");
        set_pos(world, door, 1.5f, 0.0f, 0.0f);
        memset(&rd, 0, sizeof(rd));
        rd.mesh = door_mesh;
        rd.material = mat;
        rd.casts_shadow = 1;
        rd.receives_shadow = 1;
        rd.visible = 1;
        if (le_object_add_asset_renderable(world, &door, &rd) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("door renderable");
        }
        memset(&ad, 0, sizeof(ad));
        ad.skeleton = LE_ASSET_INVALID;
        ad.clip = door_clip;
        ad.autoplay = 1;
        ad.loop_mode = LE_ANIM_PING_PONG;
        ad.speed = 1.0f;
        if (le_object_add_animator(world, &door, &ad) !=
            LE_SUCCESS) {
            FAIL_CLEANUP("door animator");
        }
    }
    printf("lua_animation: the arm waves and nods (Lua conducts), "
           "the door swings.\n");
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
                le_anim_stats astats;

                le_world_get_last_render_report(world, &rep);
                le_anim_get_stats(world, &astats);
                printf("frame %lu: submitted=%u animators=%u "
                       "playing=%u joints=%u\n",
                       frame, rep.submitted,
                       astats.animator_count,
                       astats.playing_count,
                       astats.evaluated_joints);
            }
        }
    }
    ok = 1;

cleanup:
    le_world_destroy(world);
    if (engine != NULL) {
        le_asset_unload(engine, &conductor);
        le_asset_unload(engine, &door_clip);
        le_asset_unload(engine, &nod);
        le_asset_unload(engine, &wave);
        le_asset_unload(engine, &skel);
        le_asset_unload(engine, &mat);
        le_asset_unload(engine, &door_mesh);
        le_asset_unload(engine, &arm_mesh);
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
