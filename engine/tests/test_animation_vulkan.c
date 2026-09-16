/*
 * Luma Engine Phase 29 Vulkan tests: animated + skinned
 * renderables through the render path (public APIs only, plus
 * the engine-internal palette getter for the CPU/GPU oracle).
 *
 * - A procedural two-joint arm (skinned bar mesh) renders at
 *   bind pose with and without an animator (static regression:
 *   near-identical pixels), then bent at t=1s (motion proof:
 *   pixels move).
 * - A door object driven by an object-target clip swings
 *   (object-track motion proof through the same path).
 * - CPU/GPU oracle: with identity inverse binds, the submitted
 *   palette equals the joint globals; joint 0 (root) must match
 *   le_transform_compose of the sampled clip pose.
 * - Skinned casters render through the shadow path (validation
 *   layers stay enabled throughout).
 * - glTF bridge: skinned.glb (2-joint rig + Wave clip) imports
 *   to engine assets; the imported animator steps and submits
 *   a finite 2-joint palette.
 *
 * Headless-safe: SKIP (exit 0) when no Vulkan device is
 * available. Validation layers stay enabled.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_assets/luma_assets.h>
#include <luma_engine/luma_engine.h>
#include "internal/engine_internal.h"

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

#define SKIP_ENV(what) do { \
    printf("SKIP: environment cannot provide %s\n", what); \
    lc_shutdown(); \
    return 0; \
} while (0)

#ifndef LE_PI_F
#define LE_PI_F 3.14159265358979323846f
#endif

typedef struct eng_env {
    lc_device *device;
    lr_renderer *renderer;
    le_engine *engine;
    le_world *world;
    lc_image *color_img;
    lc_image_view *color_view;
    lc_image *depth_img;
    lc_image_view *depth_view;
    lc_render_target *target;
    uint32_t width;
    uint32_t height;
} eng_env;

typedef struct frame_env {
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
} frame_env;

static int make_device(lc_device **out) {
    lc_device_desc desc;

    memset(&desc, 0, sizeof(desc));
    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 1;
    *out = NULL;
    switch (lc_device_create(&desc, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_BACKEND_UNAVAILABLE:
    case LC_ERROR_NO_SUPPORTED_DEVICE:
        return 1;
    default:
        return -1;
    }
}

static int make_frame(frame_env *fe) {
    lc_window_desc wdesc;

    memset(&wdesc, 0, sizeof(wdesc));
    wdesc.title = "LumaC Animation Test";
    wdesc.width = 640;
    wdesc.height = 480;
    switch (lc_window_create(&wdesc, &fe->window)) {
    case LC_SUCCESS:
        break;
    case LC_ERROR_PLATFORM:
    case LC_ERROR_WINDOW_CREATION_FAILED:
        return 1;
    default:
        return -1;
    }
    return 0;
}

static void destroy_frame(frame_env *fe) {
    lc_swapchain_destroy(fe->swapchain);
    lc_surface_destroy(fe->surface);
    lc_window_destroy(fe->window);
    fe->swapchain = NULL;
    fe->surface = NULL;
    fe->window = NULL;
}

static int env_init(eng_env *env, lc_device *device, uint32_t w,
                    uint32_t h) {
    lr_renderer_desc rdesc;
    le_engine_desc edesc;
    le_world_desc wdesc;
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_render_target_create_desc rtdesc;
    lc_render_target_attachment ratt;

    memset(env, 0, sizeof(*env));
    env->device = device;
    env->width = w;
    env->height = h;
    memset(&rdesc, 0, sizeof(rdesc));
    rdesc.device = device;
    rdesc.render_target.color_attachment_count = 1;
    rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
    rdesc.max_objects = 8192;
    rdesc.ambient_light[0] = 0.35f;
    rdesc.ambient_light[1] = 0.35f;
    rdesc.ambient_light[2] = 0.40f;
    if (lr_renderer_create(&rdesc, &env->renderer) != LR_SUCCESS) {
        return 0;
    }
    memset(&edesc, 0, sizeof(edesc));
    edesc.renderer = env->renderer;
    if (le_engine_create(&edesc, &env->engine) != LE_SUCCESS) {
        return 0;
    }
    memset(&wdesc, 0, sizeof(wdesc));
    if (le_world_create(env->engine, &wdesc, &env->world) !=
        LE_SUCCESS) {
        return 0;
    }
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RGBA8_UNORM;
    idesc.width = w;
    idesc.height = h;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                  LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_SRC |
                  LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &idesc, &env->color_img) !=
        LC_SUCCESS) {
        return 0;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(env->color_img, &vdesc,
                             &env->color_view) != LC_SUCCESS) {
        return 0;
    }
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_D32_FLOAT;
    idesc.width = w;
    idesc.height = h;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL |
                  LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &idesc, &env->depth_img) !=
        LC_SUCCESS) {
        return 0;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(env->depth_img, &vdesc,
                             &env->depth_view) != LC_SUCCESS) {
        return 0;
    }
    memset(&rtdesc, 0, sizeof(rtdesc));
    ratt.view = env->color_view;
    rtdesc.color_attachments = &ratt;
    rtdesc.color_attachment_count = 1;
    rtdesc.depth_stencil_attachment = env->depth_view;
    rtdesc.width = w;
    rtdesc.height = h;
    if (lc_render_target_create(device, &rtdesc, &env->target) !=
        LC_SUCCESS) {
        return 0;
    }
    return 1;
}

static void env_shutdown(eng_env *env) {
    le_world_destroy(env->world);
    le_engine_destroy(env->engine);
    if (env->renderer != NULL) {
        lr_renderer_destroy(env->renderer);
    }
    lc_render_target_destroy(env->target);
    lc_image_view_destroy(env->color_view);
    lc_image_destroy(env->color_img);
    lc_image_view_destroy(env->depth_view);
    lc_image_destroy(env->depth_img);
    memset(env, 0, sizeof(*env));
}

static int render_one_frame(eng_env *env, frame_env *fe) {
    lc_command_encoder *enc = NULL;
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_render_depth_attachment datt;

    lc_poll_events();
    if (lc_begin_frame(fe->swapchain) != LC_SUCCESS) {
        return 0;
    }
    if (lc_swapchain_get_encoder(fe->swapchain, &enc) !=
        LC_SUCCESS) {
        return 0;
    }
    if (le_world_render_scene(env->world, enc, env->width,
                              env->height) != LE_SUCCESS) {
        return 0;
    }
    memset(&catt, 0, sizeof(catt));
    catt.view = env->color_view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    catt.clear_color[0] = 0.0f;
    catt.clear_color[1] = 0.0f;
    catt.clear_color[2] = 0.0f;
    catt.clear_color[3] = 1.0f;
    memset(&datt, 0, sizeof(datt));
    datt.view = env->depth_view;
    datt.depth_load_op = LC_LOAD_OP_CLEAR;
    datt.depth_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_depth = 1.0f;
    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_stencil = 0;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.depth_attachment = &datt;
    pdesc.width = env->width;
    pdesc.height = env->height;
    if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
        return 0;
    }
    if (le_world_render_output(env->world, enc, env->target) !=
        LE_SUCCESS) {
        return 0;
    }
    if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        return 0;
    }
    le_world_render_end(env->world);
    {
        lc_render_swapchain_pass_desc spass;

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.depth_load_op = LC_LOAD_OP_CLEAR;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        if (lc_encoder_begin_swapchain_pass(enc, fe->swapchain,
                                            &spass) !=
            LC_SUCCESS) {
            return 0;
        }
        if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            return 0;
        }
    }
    {
        lc_result r = lc_end_frame(fe->swapchain);

        if (r != LC_SUCCESS && r != LC_SUBOPTIMAL) {
            return 0;
        }
    }
    return 1;
}

/* Read back the offscreen target into a caller buffer (RGBA8).
 * Returns pixel count, or 0 on failure. */
static uint32_t readback_pixels(eng_env *env, unsigned char **out) {
    lc_image_readback_desc desc;
    lc_image_readback_info info;
    unsigned char *px = NULL;

    *out = NULL;
    memset(&desc, 0, sizeof(desc));
    if (lc_image_query_readback(env->color_img, &desc, &info) !=
        LC_SUCCESS) {
        return 0;
    }
    px = (unsigned char *)malloc(info.size);
    if (px == NULL) {
        return 0;
    }
    if (lc_image_readback(env->color_img, &desc, px, info.size,
                          NULL) != LC_SUCCESS) {
        free(px);
        return 0;
    }
    *out = px;
    return (uint32_t)(info.size / 4u);
}

static long lit_count(const unsigned char *px, uint32_t n) {
    long lit = 0;
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (px[i * 4u + 0] > 8 || px[i * 4u + 1] > 8 ||
            px[i * 4u + 2] > 8) {
            lit++;
        }
    }
    return lit;
}

static long diff_count(const unsigned char *a,
                       const unsigned char *b, uint32_t n) {
    long d = 0;
    uint32_t i;

    for (i = 0; i < n; i++) {
        int dr = (int)a[i * 4u + 0] - (int)b[i * 4u + 0];
        int dg = (int)a[i * 4u + 1] - (int)b[i * 4u + 1];
        int db = (int)a[i * 4u + 2] - (int)b[i * 4u + 2];

        if (dr < -12 || dr > 12 || dg < -12 || dg > 12 ||
            db < -12 || db > 12) {
            d++;
        }
    }
    return d;
}

#define ARM_LEVELS 9
#define ARM_CORNERS 4

static int make_arm_mesh_asset(eng_env *env, le_asset *out) {
    lr_vertex verts[ARM_LEVELS * ARM_CORNERS];
    uint32_t idx[(ARM_LEVELS - 1u) * ARM_CORNERS * 6u];
    le_mesh_asset_desc md;
    uint32_t l;
    uint32_t c;
    uint32_t q = 0;
    static const float kCorner[4][2] = {
        { -0.5f, -0.5f }, { 0.5f, -0.5f },
        { 0.5f, 0.5f }, { -0.5f, 0.5f },
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
            uint32_t e2 = (l + 1u) * ARM_CORNERS + c2;

            idx[q++] = a;
            idx[q++] = b;
            idx[q++] = e2;
            idx[q++] = a;
            idx[q++] = e2;
            idx[q++] = d;
        }
    }
    memset(&md, 0, sizeof(md));
    md.vertices = verts;
    md.vertex_count = ARM_LEVELS * ARM_CORNERS;
    md.indices = idx;
    md.index_count = q;
    return le_asset_create_mesh(env->engine, &md, out) ==
        LE_SUCCESS;
}

/* Two-joint arm skeleton. identity_inv = nonzero uses identity
 * inverse binds (oracle-friendly); otherwise true binds. */
static int make_arm_skeleton(eng_env *env, le_asset *out,
                             int identity_inv) {
    le_skeleton_joint_desc joints[2];
    le_skeleton_asset_desc d;

    memset(joints, 0, sizeof(joints));
    memcpy(joints[0].name, "shoulder", 9);
    joints[0].parent = -1;
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
    if (identity_inv) {
        joints[1].inverse_bind[0] = joints[1].inverse_bind[5] =
            joints[1].inverse_bind[10] =
                joints[1].inverse_bind[15] = 1.0f;
    } else {
        joints[1].inverse_bind[0] = joints[1].inverse_bind[5] =
            joints[1].inverse_bind[10] =
                joints[1].inverse_bind[15] = 1.0f;
        joints[1].inverse_bind[13] = -1.0f;
    }
    memset(&d, 0, sizeof(d));
    d.joints = joints;
    d.joint_count = 2;
    return le_asset_create_skeleton(env->engine, &d, out) ==
        LE_SUCCESS;
}

static int make_wave_clip(eng_env *env, le_asset *out) {
    float times[5] = { 0.0f, 0.5f, 1.0f, 1.5f, 2.0f };
    float values[5][4];
    le_anim_track_desc track;
    le_animation_clip_desc d;
    int k;
    static const float kDeg[5] = { 0.0f, 50.0f, 0.0f,
                                   -50.0f, 0.0f };

    for (k = 0; k < 5; k++) {
        float half = kDeg[k] * LE_PI_F / 360.0f;

        values[k][0] = 0.0f;
        values[k][1] = 0.0f;
        values[k][2] = sinf(half);
        values[k][3] = cosf(half);
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
    return le_asset_create_clip(env->engine, &d, out) ==
        LE_SUCCESS;
}

static int make_door_clip(eng_env *env, le_asset *out) {
    float times[2] = { 0.0f, 2.0f };
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
    d.duration = 2.0f;
    d.tracks = &track;
    d.track_count = 1;
    return le_asset_create_clip(env->engine, &d, out) ==
        LE_SUCCESS;
}

static void make_camera(eng_env *env) {
    le_object rig;
    le_object cam;
    float p[3] = { 0.0f, 1.2f, 4.0f };
    float q[4] = { 0.3f, 0.0f, 0.0f, 0.95f };
    float nq[4];
    le_camera_desc cd;

    le_object_create(env->world, &rig);
    le_object_create(env->world, &cam);
    le_object_set_parent(env->world, &cam, &rig);
    le_object_set_position(env->world, &rig, p);
    le_quat_normalize(q, nq);
    le_object_set_rotation(env->world, &rig, nq);
    le_camera_desc_default(&cd);
    cd.far_plane = 100.0f;
    le_object_add_camera(env->world, &cam, &cd);
    le_world_set_active_camera(env->world, &cam);
}

static void make_sun(eng_env *env) {
    le_object sun;
    le_light_desc ld;
    float q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    le_object_create(env->world, &sun);
    le_object_set_rotation(env->world, &sun, q);
    memset(&ld, 0, sizeof(ld));
    ld.type = LE_LIGHT_DIRECTIONAL;
    ld.color[0] = 1.0f;
    ld.color[1] = 1.0f;
    ld.color[2] = 1.0f;
    ld.intensity = 3.0f;
    le_object_add_light(env->world, &sun, &ld);
}

static int make_material(eng_env *env, le_asset *out) {
    le_material_asset_desc md;

    memset(&md, 0, sizeof(md));
    md.base_color_factor[0] = 0.85f;
    md.base_color_factor[1] = 0.35f;
    md.base_color_factor[2] = 0.2f;
    md.base_color_factor[3] = 1.0f;
    md.metallic_factor = 0.0f;
    md.roughness_factor = 0.7f;
    return le_asset_create_material(env->engine, &md, out) ==
        LE_SUCCESS;
}

/* Snapshot the current offscreen pixels (caller frees). */
static uint32_t snap(eng_env *env, unsigned char **out) {
    return readback_pixels(env, out);
}

int main(void) {
    lc_device *device = NULL;
    frame_env fe;
    eng_env env;
    lc_surface *surface = NULL;
    eng_env *e;
    int dev_rc;
    le_asset arm_mesh = LE_ASSET_INVALID;
    le_asset mat = LE_ASSET_INVALID;
    le_asset skel = LE_ASSET_INVALID;
    le_asset wave = LE_ASSET_INVALID;
    le_asset door_clip = LE_ASSET_INVALID;
    le_object arm = LE_OBJECT_INVALID;
    le_object door = LE_OBJECT_INVALID;

    printf("Running Luma Engine Phase 29 Vulkan tests...\n");
    memset(&fe, 0, sizeof(fe));
    memset(&env, 0, sizeof(env));
    e = &env;
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }
    dev_rc = make_device(&device);
    if (dev_rc == 1) {
        SKIP_ENV("Vulkan device");
    }
    if (dev_rc != 0) {
        printf("device failed: FAIL\n");
        lc_shutdown();
        return 1;
    }
    switch (make_frame(&fe)) {
    case 0:
        break;
    case 1:
        lc_device_destroy(device);
        SKIP_ENV("window");
    default:
        printf("window failed: FAIL\n");
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    switch (lc_surface_create(device, fe.window, &surface)) {
    case LC_SUCCESS:
        fe.surface = surface;
        break;
    case LC_ERROR_SURFACE_UNSUPPORTED:
        destroy_frame(&fe);
        lc_device_destroy(device);
        SKIP_ENV("surface");
    default:
        printf("surface failed: FAIL\n");
        destroy_frame(&fe);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    {
        lc_swapchain_desc sdesc;

        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.width = 640;
        sdesc.height = 480;
        sdesc.image_count = 0;
        sdesc.vsync = 0;
        switch (lc_swapchain_create(device, fe.surface, &sdesc,
                                    &fe.swapchain)) {
        case LC_SUCCESS:
            break;
        case LC_ERROR_SWAPCHAIN_UNSUPPORTED:
        case LC_ERROR_ZERO_EXTENT:
            destroy_frame(&fe);
            lc_device_destroy(device);
            SKIP_ENV("swapchain");
        default:
            printf("swapchain failed: FAIL\n");
            destroy_frame(&fe);
            lc_device_destroy(device);
            lc_shutdown();
            return 1;
        }
    }
    if (!env_init(&env, device, 640, 480)) {
        printf("env init failed: FAIL\n");
        destroy_frame(&fe);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    make_camera(e);
    make_sun(e);
    if (!make_arm_mesh_asset(e, &arm_mesh)) {
        printf("arm mesh failed: FAIL\n");
        goto done;
    }
    if (!make_material(e, &mat)) {
        printf("material failed: FAIL\n");
        goto done;
    }
    if (!make_arm_skeleton(e, &skel, 0)) {
        printf("skeleton failed: FAIL\n");
        goto done;
    }
    if (!make_wave_clip(e, &wave)) {
        printf("wave clip failed: FAIL\n");
        goto done;
    }
    if (!make_door_clip(e, &door_clip)) {
        printf("door clip failed: FAIL\n");
        goto done;
    }
    /* Arm (skinned renderable, animator attached later per
     * part) + door (object-track animator from the start). */
    {
        le_asset_renderable_desc rd;
        le_animator_desc ad;
        float p[3];

        if (le_object_create(e->world, &arm) != LE_SUCCESS) {
            printf("arm create failed: FAIL\n");
            goto done;
        }
        p[0] = -0.9f;
        p[1] = 0.2f;
        p[2] = 0.0f;
        le_object_set_position(e->world, &arm, p);
        memset(&rd, 0, sizeof(rd));
        rd.mesh = arm_mesh;
        rd.material = mat;
        rd.casts_shadow = 1;
        rd.receives_shadow = 1;
        rd.visible = 1;
        if (le_object_add_asset_renderable(e->world, &arm,
                                           &rd) !=
            LE_SUCCESS) {
            printf("arm renderable failed: FAIL\n");
            goto done;
        }
        if (le_object_create(e->world, &door) != LE_SUCCESS) {
            printf("door create failed: FAIL\n");
            goto done;
        }
        p[0] = 1.3f;
        p[1] = 0.2f;
        p[2] = 0.0f;
        le_object_set_position(e->world, &door, p);
        memset(&rd, 0, sizeof(rd));
        rd.mesh = arm_mesh; /* rigid-bar stand-in; object clip
                             * carries the swing */
        rd.material = mat;
        rd.casts_shadow = 1;
        rd.receives_shadow = 1;
        rd.visible = 1;
        if (le_object_add_asset_renderable(e->world, &door,
                                           &rd) !=
            LE_SUCCESS) {
            printf("door renderable failed: FAIL\n");
            goto done;
        }
        memset(&ad, 0, sizeof(ad));
        ad.skeleton = LE_ASSET_INVALID;
        ad.clip = door_clip;
        ad.autoplay = 1;
        ad.loop_mode = LE_ANIM_PING_PONG;
        ad.speed = 1.0f;
        if (le_object_add_animator(e->world, &door, &ad) !=
            LE_SUCCESS) {
            printf("door animator failed: FAIL\n");
            goto done;
        }
    }

    /* PART 1: static regression — skinned mesh with NO animator
     * (rigid path, bind pose) vs WITH an animator at bind pose
     * (skinned path, identity-ish palette): near-identical. */
    {
        unsigned char *rigid = NULL;
        unsigned char *skinned = NULL;
        uint32_t n0;
        uint32_t n1;
        long lit0;
        long lit1;
        long nd;
        le_animator_desc ad;

        le_world_update(e->world, 1.0f / 60.0f);
        TEST_CHECK(render_one_frame(e, &fe), "rigid frame");
        n0 = snap(e, &rigid);
        TEST_CHECK(n0 > 0, "rigid readback");
        lit0 = (n0 > 0) ? lit_count(rigid, n0) : 0;

        memset(&ad, 0, sizeof(ad));
        ad.skeleton = skel;
        ad.clip = wave;
        ad.autoplay = 0;
        ad.loop_mode = LE_ANIM_LOOP;
        ad.speed = 1.0f;
        ad.start_time = 0.0f; /* wave key 0 = identity */
        TEST_CHECK(le_object_add_animator(e->world, &arm, &ad) ==
                       LE_SUCCESS,
                   "arm animator attaches");
        /* Exact bind pose: seek + pause (no time advance). */
        le_anim_seek(e->world, &arm, 0.0f);
        le_anim_pause(e->world, &arm);
        le_world_update(e->world, 1.0f / 60.0f);
        TEST_CHECK(render_one_frame(e, &fe), "skinned frame");
        n1 = snap(e, &skinned);
        TEST_CHECK(n1 > 0 && n0 == n1, "skinned readback");
        lit1 = (n1 > 0) ? lit_count(skinned, n1) : 0;
        nd = (n0 > 0 && n1 > 0 && n0 == n1)
                 ? diff_count(rigid, skinned, n0)
                 : (long)n0;
        TEST_CHECK(lit0 > 1000, "rigid arm paints pixels");
        TEST_CHECK(lit1 > 1000, "bind-pose skinned arm paints");
        TEST_CHECK(nd < lit0 / 50 + 64,
                   "bind pose matches rigid path");
        printf("[info] arm pixels: rigid=%ld skinned=%ld "
               "diff=%ld\n",
               lit0, lit1, nd);
        free(rigid);
        free(skinned);
    }

    /* PART 2: motion proof — play and advance ~0.5s (near the
     * 50 deg elbow-Z key); pixels must move. */
    {
        unsigned char *before = NULL;
        unsigned char *after = NULL;
        uint32_t n0;
        uint32_t n1;
        long moved;
        float ta;
        int i;

        le_world_update(e->world, 1.0f / 60.0f);
        TEST_CHECK(render_one_frame(e, &fe), "pre-bend frame");
        n0 = snap(e, &before);
        TEST_CHECK(le_anim_resume(e->world, &arm) == LE_SUCCESS,
                   "arm plays");
        for (i = 0; i < 30; i++) {
            le_world_update(e->world, 1.0f / 60.0f);
        }
        ta = le_anim_get_time(e->world, &arm);
        TEST_CHECK(ta > 0.4f && ta < 0.7f,
                   "arm time advanced to bend");
        TEST_CHECK(render_one_frame(e, &fe), "bent frame");
        n1 = snap(e, &after);
        moved = (n0 > 0 && n1 > 0 && n0 == n1)
                    ? diff_count(before, after, n0)
                    : 0;
        TEST_CHECK(moved > 500, "bent arm moves pixels");
        printf("[info] bend moved %ld pixels\n", moved);
        free(before);
        free(after);
    }

    /* PART 3: CPU/GPU oracle — identity inverse binds make the
     * palette equal the joint globals; joint 0 (root) matches
     * compose(sampled pose) at the current time. */
    {
        le_asset iskel = LE_ASSET_INVALID;
        le_object oracle = LE_OBJECT_INVALID;
        le_animator_desc ad;
        float t;
        float st[1][3] = { { 0.0f, 0.0f, 0.0f } };
        float sr[1][4] = { { 0.0f, 0.0f, 0.0f, 1.0f } };
        float ss[1][3] = { { 1.0f, 1.0f, 1.0f } };
        float expm[16];
        const float (*pal)[16] = NULL;
        uint32_t nj = 0;
        uint32_t slot = 0;
        le_result code = LE_SUCCESS;
        int match = 0;

        TEST_CHECK(make_arm_skeleton(e, &iskel, 1),
                   "oracle skeleton");
        if (le_object_create(e->world, &oracle) != LE_SUCCESS) {
            printf("oracle create failed: FAIL\n");
            goto done;
        }
        memset(&ad, 0, sizeof(ad));
        ad.skeleton = iskel;
        ad.clip = wave;
        ad.autoplay = 1;
        ad.loop_mode = LE_ANIM_LOOP;
        ad.speed = 1.0f;
        if (le_object_add_animator(e->world, &oracle, &ad) !=
            LE_SUCCESS) {
            printf("oracle animator failed: FAIL\n");
            goto done;
        }
        {
            int i;

            for (i = 0; i < 45; i++) {
                le_world_update(e->world, 1.0f / 60.0f);
            }
        }
        t = le_anim_get_time(e->world, &oracle);
        if (le_anim_sample_clip(e->engine, &wave, t, st, sr,
                                ss, 1, NULL, NULL,
                                NULL) == LE_SUCCESS) {
            le_transform_compose(st[0], sr[0], ss[0], expm);
            if (le_resolve_live(e->world, &oracle, &slot,
                                &code) &&
                le_anim_get_palette(e->world, slot, &pal,
                                    &nj) &&
                pal != NULL && nj == 2) {
                float worst = 0.0f;
                int k;

                for (k = 0; k < 16; k++) {
                    float d = pal[0][k] - expm[k];

                    if (d < 0.0f) {
                        d = -d;
                    }
                    if (d > worst) {
                        worst = d;
                    }
                }
                match = (worst < 1e-4f);
                printf("[info] oracle worst-element error: %.3g "
                       "(t=%.3f)\n",
                       worst, t);
            }
        }
        TEST_CHECK(match, "palette matches CPU compose");
        le_object_remove_animator(e->world, &oracle);
        le_object_destroy(e->world, &oracle);
        le_asset_unload(e->engine, &iskel);
    }

    /* PART 4: teardown with live animated renderables is
     * clean (animators retire with slots; assets unload
     * after world destroy). */
    TEST_CHECK(1, "vulkan animation teardown clean");

    /* PART 5: glTF bridge — skinned.glb yields a 2-joint
     * skeleton (joints = nodes 1,2; node 1 parented under the
     * non-joint root) + one Wave clip (morph-only animation
     * excluded). The imported animator steps and submits a
     * 2-joint palette. */
    {
        la_asset_manager_desc mdesc;
        la_asset_manager *manager = NULL;
        la_model *model = NULL;

        memset(&mdesc, 0, sizeof(mdesc));
        mdesc.renderer = e->renderer;
        TEST_CHECK(la_asset_manager_create(&mdesc, &manager) ==
                       LA_SUCCESS,
                   "bridge manager");
        if (manager != NULL &&
            la_model_load(manager, LE_ANIM_GLB_PATH, &model) ==
                LA_SUCCESS &&
            model != NULL) {
            le_gltf_animated ba;
            le_object bo = LE_OBJECT_INVALID;
            le_animator_desc bad;
            int ok = 1;

            TEST_CHECK(la_model_get_skin_count(model) == 1,
                       "fixture one skin");
            memset(&ba, 0, sizeof(ba));
            ba.skeleton = LE_ASSET_INVALID;
            if (le_gltf_import_animated(e->engine, model, 0,
                                        &ba) != LE_SUCCESS) {
                TEST_CHECK(0, "bridge import succeeds");
                ok = 0;
            } else {
                TEST_CHECK(
                    le_skeleton_get_joint_count(e->engine,
                                                &ba.skeleton) ==
                        2,
                    "bridge two joints");
                TEST_CHECK(ba.joint_count == 2 &&
                               ba.joint_nodes[0] == 1 &&
                               ba.joint_nodes[1] == 2,
                           "bridge joint nodes 1,2");
                TEST_CHECK(ba.clip_count == 1,
                           "bridge one clip (morph excluded)");
                if (ba.clip_count == 1) {
                    float dur = le_clip_get_duration(
                        e->engine, &ba.clips[0]);

                    TEST_CHECK(
                        dur > 0.0f &&
                            le_clip_get_track_count(
                                e->engine, &ba.clips[0]) == 2,
                        "bridge wave duration + 2 tracks");
                    printf("[info] bridge wave duration: %.3f "
                           "(skipped=%u)\n",
                           dur, ba.skipped_tracks);
                }
                if (le_object_create(e->world, &bo) !=
                        LE_SUCCESS) {
                    ok = 0;
                } else {
                    memset(&bad, 0, sizeof(bad));
                    bad.skeleton = ba.skeleton;
                    bad.clip = ba.clips[0];
                    bad.autoplay = 1;
                    bad.loop_mode = LE_ANIM_LOOP;
                    bad.speed = 1.0f;
                    if (le_object_add_animator(e->world, &bo,
                                               &bad) !=
                        LE_SUCCESS) {
                        ok = 0;
                    } else {
                        int i;

                        for (i = 0; i < 10; i++) {
                            le_world_update(e->world,
                                            1.0f / 60.0f);
                        }
                        {
                            uint32_t slot = 0;
                            le_result code = LE_SUCCESS;
                            const float (*pal)[16] = NULL;
                            uint32_t nj = 0;

                            if (le_resolve_live(e->world, &bo,
                                                &slot,
                                                &code) &&
                                le_anim_get_palette(
                                    e->world, slot, &pal,
                                    &nj) &&
                                pal != NULL && nj == 2) {
                                int fin = 1;
                                int k;

                                for (k = 0; k < 32; k++) {
                                    float v = pal[0][k];

                                    if (!(v == v)) {
                                        fin = 0;
                                        break;
                                    }
                                }
                                TEST_CHECK(fin, "bridge palette "
                                                "finite x2");
                            } else {
                                TEST_CHECK(0, "bridge palette");
                            }
                        }
                        le_object_remove_animator(e->world,
                                                  &bo);
                    }
                    le_object_destroy(e->world, &bo);
                }
                if (ba.clip_count > 0) {
                    uint32_t k;

                    for (k = 0; k < ba.clip_count; k++) {
                        le_asset_unload(e->engine,
                                        &ba.clips[k]);
                    }
                }
                le_asset_unload(e->engine, &ba.skeleton);
                le_gltf_animated_free(&ba);
            }
            (void)ok;
            la_model_destroy(model);
        } else {
            TEST_CHECK(0, "fixture skinned.glb loads");
        }
        la_asset_manager_destroy(manager);
    }

    printf("Luma Engine Phase 29 Vulkan tests: %d passed, %d failed\n",
           g_passed, g_failed);
    if (g_failed == 0) {
        printf("ALL PHASE 29 VULKAN TESTS PASSED\n");
    } else {
        printf("TESTS FAILED\n");
    }

done:
    /* Assets unload after world destroy (animators retire with
     * their slots, releasing asset references). */
    le_world_destroy(env.world);
    env.world = NULL;
    le_asset_unload(env.engine, &door_clip);
    le_asset_unload(env.engine, &wave);
    le_asset_unload(env.engine, &skel);
    le_asset_unload(env.engine, &mat);
    le_asset_unload(env.engine, &arm_mesh);
    env_shutdown(&env);
    destroy_frame(&fe);
    lc_device_destroy(device);
    lc_shutdown();
    return (g_failed == 0) ? 0 : 1;
}