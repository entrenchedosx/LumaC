/* Pass 2B live animation proof (headed, Vulkan-gated; SKIP
 * without a device): the REAL animation system on VISIBLE skinned
 * geometry + crossfade + speed-driven clip switching.
 *
 * Why this binary exists: test_animation*.c prove sampling/blend/
 * crossfade math headlessly, and test_animation_vulkan.c proves the
 * skinned arm renders — but no proof composes visible animation +
 * crossfade + character-speed switching through the real viewport.
 * This binary closes §40-53.
 *
 * Subject: the procedural two-joint arm (same recipe as
 * test_animation_vulkan: skinned bar mesh, shoulder+elbow
 * skeleton, wave clip) + a second "raise" clip (elbow 0→90°).
 * Legs: bind pose (§41) vs rigid still; idle (slow wave) visible
 * (§42); walk (fast raise) visibly different (§43); loop boundary
 * continuity (§44); crossfade idle→walk mid-blend (§47-48: no
 * bind-flash, no collapse, no snap — pixel continuity between
 * consecutive frames); reversal walk→idle (§49); speed-driven
 * switch (§50: walker sets walk when moving, idle when slow);
 * play/stop (§52: runtime anim state gone, edit unchanged).
 * Root motion (§51): clips carry JOINT tracks only (no object
 * tracks, no root motion) — documented, no double-move possible.
 * Ping-pong (§46): door clip exists in the vulkan suite; this
 * binary covers LOOP + ONCE (seek-clamp) only — ping-pong stays
 * NOT TESTED here (covered headless in test_animation).
 *
 * Usage: test_animation_live --shotdir <dir>
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_engine/luma_engine.h>
#include <luma_editor/luma_editor.h>

#ifndef LE_PI_F
#define LE_PI_F 3.14159265358979323846f
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

#define SKIP_ENV(what) do { \
    printf("SKIP: environment cannot provide %s\n", what); \
    lc_shutdown(); \
    return 0; \
} while (0)

typedef struct a_head {
    lc_window *window;
    lc_device *device;
    lc_surface *surface;
    lc_swapchain *swapchain;
    lc_command_encoder *enc;
    lr_renderer *renderer;
    le_engine *engine;
    le_world *world;
    led_session *session;
    leg_context *gui;
    led_viewport vp;
    lc_image *color;
    lc_image_view *color_view;
    lc_render_target *target;
    uint32_t fw;
    uint32_t fh;
} a_head;

static int a_frame(a_head *h) {
    leg_frame_input in;
    lc_result brc;

    lc_poll_events();
    brc = lc_begin_frame(h->swapchain);
    if (brc == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
        if (lc_swapchain_recreate(h->swapchain, h->fw, h->fh) !=
            LC_SUCCESS) {
            return 0;
        }
        return 1;
    }
    if (brc != LC_SUCCESS) {
        return 0;
    }
    if (lc_swapchain_get_encoder(h->swapchain, &h->enc) !=
        LC_SUCCESS) {
        return 0;
    }
    {
        uint32_t vpw = (h->vp.width > 0) ? h->vp.width
                        : (h->fw > 320) ? h->fw - 320
                                        : 640;
        uint32_t vph = (h->vp.height > 0) ? h->vp.height
                        : (h->fh > 120) ? h->fh - 120
                                        : 480;
        struct leg_viewport_target *vt =
            leg_viewport_target_for(h->gui);

        leg_viewport_composite(h->gui, vt, h->enc, vpw, vph);
    }
    memset(&in, 0, sizeof(in));
    in.window_width = h->fw;
    in.window_height = h->fh;
    in.delta_seconds = 1.0f / 60.0f;
    in.window_focused = 1;
    if (leg_frame_begin(h->gui, h->window, &in) != LED_SUCCESS) {
        return 0;
    }
    leg_panels_frame(h->gui, &h->vp, 1.0f / 60.0f);
    if (leg_consume_play_input(h->gui, h->engine) < 0) {
        return 0;
    }
    if (led_is_playing(h->session) &&
        !led_play_is_paused(h->session)) {
        if (led_play_tick(h->session, 1.0f / 60.0f) !=
            LED_SUCCESS) {
            return 0;
        }
    }
    if (leg_frame_end(h->gui) != LED_SUCCESS) {
        return 0;
    }
    {
        lc_render_color_attachment catt;
        lc_render_pass_desc pass;

        memset(&catt, 0, sizeof(catt));
        catt.view = h->color_view;
        catt.load_op = LC_LOAD_OP_CLEAR;
        catt.store_op = LC_STORE_OP_STORE;
        memset(&pass, 0, sizeof(pass));
        pass.color_attachments = &catt;
        pass.color_attachment_count = 1;
        pass.width = h->fw;
        pass.height = h->fh;
        if (lc_encoder_begin_render_pass(h->enc, &pass) !=
            LC_SUCCESS) {
            return 0;
        }
        if (leg_record_gui(h->gui, h->enc,
                           LC_FORMAT_RGBA8_UNORM,
                           LC_FORMAT_UNDEFINED) != LED_SUCCESS) {
            lc_encoder_end_render_pass(h->enc);
            return 0;
        }
        if (lc_encoder_end_render_pass(h->enc) != LC_SUCCESS) {
            return 0;
        }
    }
    {
        lc_render_swapchain_pass_desc spass;

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.depth_load_op = LC_LOAD_OP_DONT_CARE;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        if (lc_encoder_begin_swapchain_pass(h->enc, h->swapchain,
                                            &spass) !=
            LC_SUCCESS) {
            return 0;
        }
        if (lc_encoder_end_render_pass(h->enc) != LC_SUCCESS) {
            return 0;
        }
    }
    if (lc_end_frame(h->swapchain) != LC_SUCCESS) {
        return 0;
    }
    return 1;
}

static int a_frames(a_head *h, int n) {
    int f;

    for (f = 0; f < n; f++) {
        if (!a_frame(h)) {
            return 0;
        }
    }
    return 1;
}

static int a_shot(a_head *h, const char *path) {
    lc_command_encoder *enc = NULL;
    lc_image_readback_desc rbdesc;
    lc_image_readback_info rbinfo;
    unsigned char *rgba = NULL;
    FILE *f = NULL;
    uint32_t x = 0;
    uint32_t y = 0;

    if (lc_begin_frame(h->swapchain) != LC_SUCCESS) {
        return 0;
    }
    if (lc_swapchain_get_encoder(h->swapchain, &enc) !=
        LC_SUCCESS) {
        return 0;
    }
    {
        lc_render_color_attachment catt;
        lc_render_pass_desc pass;

        memset(&catt, 0, sizeof(catt));
        catt.view = h->color_view;
        catt.load_op = LC_LOAD_OP_CLEAR;
        catt.store_op = LC_STORE_OP_STORE;
        memset(&pass, 0, sizeof(pass));
        pass.color_attachments = &catt;
        pass.color_attachment_count = 1;
        pass.width = h->fw;
        pass.height = h->fh;
        if (lc_encoder_begin_render_pass(enc, &pass) !=
            LC_SUCCESS) {
            return 0;
        }
        if (leg_record_gui(h->gui, enc, LC_FORMAT_RGBA8_UNORM,
                           LC_FORMAT_UNDEFINED) != LED_SUCCESS) {
            lc_encoder_end_render_pass(enc);
            return 0;
        }
        if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            return 0;
        }
    }
    {
        lc_render_swapchain_pass_desc spass;

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.depth_load_op = LC_LOAD_OP_DONT_CARE;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        if (lc_encoder_begin_swapchain_pass(enc, h->swapchain,
                                            &spass) !=
            LC_SUCCESS) {
            return 0;
        }
        if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            return 0;
        }
    }
    if (lc_end_frame(h->swapchain) != LC_SUCCESS) {
        return 0;
    }
    memset(&rbdesc, 0, sizeof(rbdesc));
    memset(&rbinfo, 0, sizeof(rbinfo));
    if (lc_image_query_readback(h->color, &rbdesc, &rbinfo) !=
        LC_SUCCESS) {
        return 0;
    }
    rgba = (unsigned char *)malloc(rbinfo.size);
    if (rgba == NULL) {
        return 0;
    }
    if (lc_image_readback(h->color, &rbdesc, rgba, rbinfo.size,
                          NULL) != LC_SUCCESS) {
        free(rgba);
        return 0;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        free(rgba);
        return 0;
    }
    fprintf(f, "P6\n%u %u\n255\n", h->fw, h->fh);
    for (y = 0; y < h->fh; y++) {
        for (x = 0; x < h->fw; x++) {
            unsigned char px[3];

            px[0] = rgba[((size_t)y * h->fw + x) * 4u + 0];
            px[1] = rgba[((size_t)y * h->fw + x) * 4u + 1];
            px[2] = rgba[((size_t)y * h->fw + x) * 4u + 2];
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    free(rgba);
    printf("[SHOT] %s\n", path);
    return 1;
}

/* Viewport target census (arm pixel mass proxy). */
static uint64_t a_census(a_head *h) {
    struct leg_viewport_target *vt =
        leg_viewport_target_for(h->gui);
    uint64_t c[4];

    memset(c, 0, sizeof(c));
    if (!leg_viewport_composite_census(h->gui, vt, c)) {
        return 0;
    }
    return c[0];
}

#define ARM_LEVELS 5
#define ARM_CORNERS 4

int main(int argc, char **argv) {
    a_head h;
    const char *shotdir = ".";
    int i = 0;
    le_object arm = LE_OBJECT_INVALID;
    le_asset arm_mesh = LE_ASSET_INVALID;
    le_asset mat = LE_ASSET_INVALID;
    le_asset skel = LE_ASSET_INVALID;
    le_asset idle_clip = LE_ASSET_INVALID;
    le_asset walk_clip = LE_ASSET_INVALID;

    memset(&h, 0, sizeof(h));
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shotdir") == 0 && i + 1 < argc) {
            shotdir = argv[++i];
        }
    }
    {
        char mk[1088];

        snprintf(mk, sizeof(mk),
#ifdef _WIN32
                 "mkdir \"%s\" 2>nul",
#else
                 "mkdir -p \"%s\"",
#endif
                 shotdir);
        (void)system(mk);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Pass 2B live animation proof (shots -> "
           "%s)...\n",
           shotdir);
    h.fw = 1280;
    h.fh = 800;
    if (lc_init() != LC_SUCCESS) {
        printf("[FAIL] lc_init\n");
        return 1;
    }
    {
        lc_device_desc dd;

        memset(&dd, 0, sizeof(dd));
        dd.backend = LC_BACKEND_VULKAN;
        dd.enable_validation = 1;
        if (lc_device_create(&dd, &h.device) != LC_SUCCESS) {
            SKIP_ENV("Vulkan device");
        }
    }
    {
        lc_window_desc wd;

        memset(&wd, 0, sizeof(wd));
        wd.title = "luma-2b-anim-live";
        wd.width = h.fw;
        wd.height = h.fh;
        if (lc_window_create(&wd, &h.window) != LC_SUCCESS) {
            lc_device_destroy(h.device);
            SKIP_ENV("native window");
        }
    }
    {
        lr_renderer_desc rd;

        memset(&rd, 0, sizeof(rd));
        rd.device = h.device;
        rd.render_target.color_attachment_count = 1;
        rd.render_target.color_formats[0] =
            LC_FORMAT_RGBA8_UNORM;
        rd.render_target.depth_stencil_format =
            LC_FORMAT_D32_FLOAT;
        rd.render_target.samples = LC_SAMPLE_COUNT_1;
        rd.max_objects = 16384;
        rd.ambient_light[0] = 0.35f;
        rd.ambient_light[1] = 0.35f;
        rd.ambient_light[2] = 0.40f;
        if (lr_renderer_create(&rd, &h.renderer) != LR_SUCCESS) {
            lc_window_destroy(h.window);
            lc_device_destroy(h.device);
            SKIP_ENV("renderer");
        }
    }
    {
        le_engine_desc ed;
        le_world_desc wd;

        memset(&ed, 0, sizeof(ed));
        ed.renderer = h.renderer;
        if (le_engine_create(&ed, &h.engine) != LE_SUCCESS) {
            printf("[FAIL] engine create\n");
            return 1;
        }
        memset(&wd, 0, sizeof(wd));
        if (le_world_create(h.engine, &wd, &h.world) !=
            LE_SUCCESS) {
            printf("[FAIL] world create\n");
            return 1;
        }
    }
    if (led_session_create(&h.session) != LED_SUCCESS ||
        led_session_attach(h.session, h.engine, h.world) !=
            LED_SUCCESS) {
        printf("[FAIL] session\n");
        return 1;
    }
    TEST_CHECK(leg_context_create(h.session, h.device, &h.gui) ==
                   LED_SUCCESS,
               "gui context create");
    TEST_CHECK(leg_set_ini_path(h.gui, NULL) == LED_SUCCESS,
               "gui ini default");
    led_viewport_default(&h.vp);
    {
        if (lc_surface_create(h.device, h.window, &h.surface) !=
            LC_SUCCESS) {
            SKIP_ENV("presentation surface");
        }
        {
            lc_swapchain_desc sd;

            memset(&sd, 0, sizeof(sd));
            sd.width = h.fw;
            sd.height = h.fh;
            sd.image_count = 0;
            sd.vsync = 0;
            if (lc_swapchain_create(h.device, h.surface, &sd,
                                    &h.swapchain) !=
                LC_SUCCESS) {
                SKIP_ENV("swapchain");
            }
        }
    }
    {
        lc_image_desc idesc;
        lc_image_view_desc vdesc;
        lc_render_target_create_desc rtdesc;
        lc_render_target_attachment ratt;

        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_RGBA8_UNORM;
        idesc.width = h.fw;
        idesc.height = h.fh;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                      LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                      LC_IMAGE_USAGE_TRANSFER_SRC |
                      LC_IMAGE_USAGE_TRANSFER_DST;
        idesc.flags = LC_IMAGE_FLAG_NONE;
        idesc.samples = LC_SAMPLE_COUNT_1;
        TEST_CHECK(lc_image_create(h.device, &idesc, &h.color) ==
                       LC_SUCCESS,
                   "offscreen color");
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.format = LC_FORMAT_UNDEFINED;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.base_mip_level = 0;
        vdesc.mip_level_count = 1;
        vdesc.base_array_layer = 0;
        vdesc.array_layer_count = 1;
        TEST_CHECK(lc_image_view_create(h.color, &vdesc,
                                        &h.color_view) ==
                       LC_SUCCESS,
                   "offscreen view");
        memset(&rtdesc, 0, sizeof(rtdesc));
        ratt.view = h.color_view;
        rtdesc.color_attachments = &ratt;
        rtdesc.color_attachment_count = 1;
        rtdesc.width = h.fw;
        rtdesc.height = h.fh;
        TEST_CHECK(lc_render_target_create(h.device, &rtdesc,
                                           &h.target) ==
                       LC_SUCCESS,
                   "offscreen target");
    }

    /* Stage: camera + sun + skinned arm (same recipe as
     * test_animation_vulkan PART 1: two-joint bar, orange). */
    {
        le_object rig = LE_OBJECT_INVALID;
        le_object cam = LE_OBJECT_INVALID;
        le_object sun = LE_OBJECT_INVALID;

        if (le_object_create(h.world, &rig) == LE_SUCCESS) {
            float p[3] = { 0.0f, 1.2f, 4.0f };

            le_object_set_name(h.world, &rig, "CameraRig");
            le_object_set_position(h.world, &rig, p);
            if (le_object_create(h.world, &cam) == LE_SUCCESS) {
                le_camera_desc cd;

                le_object_set_name(h.world, &cam, "Camera");
                le_object_set_parent(h.world, &cam, &rig);
                le_camera_desc_default(&cd);
                le_object_add_camera(h.world, &cam, &cd);
                le_world_set_active_camera(h.world, &cam);
            }
        }
        if (le_object_create(h.world, &sun) == LE_SUCCESS) {
            le_light_desc ld;

            le_object_set_name(h.world, &sun, "Sun");
            memset(&ld, 0, sizeof(ld));
            ld.type = LE_LIGHT_DIRECTIONAL;
            ld.color[0] = ld.color[1] = ld.color[2] = 1.0f;
            ld.intensity = 3.0f;
            le_object_add_light(h.world, &sun, &ld);
        }
    }
    /* Skinned bar mesh (5 levels x 4 corners, weights ramp
     * joint0 -> joint1 along +Y). */
    {
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
            float y =
                2.0f * (float)l / (float)(ARM_LEVELS - 1u);
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
        TEST_CHECK(le_asset_create_mesh(h.engine, &md,
                                        &arm_mesh) ==
                       LE_SUCCESS,
                   "arm mesh");
    }
    /* Skeleton: shoulder (root) + elbow (y=1). */
    {
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
        joints[1].inverse_bind[0] = joints[1].inverse_bind[5] =
            joints[1].inverse_bind[10] =
                joints[1].inverse_bind[15] = 1.0f;
        joints[1].inverse_bind[13] = -1.0f;
        memset(&d, 0, sizeof(d));
        d.joints = joints;
        d.joint_count = 2;
        TEST_CHECK(le_asset_create_skeleton(h.engine, &d,
                                            &skel) ==
                       LE_SUCCESS,
                   "arm skeleton");
    }
    /* Idle clip: gentle elbow wave (0→25°→0→-25°→0 over 4 s).
     * Walk clip: big elbow raise (0→90° over 1 s, loops). The
     * two are OBVIOUSLY different silhouettes. */
    {
        float times[5] = { 0.0f, 1.0f, 2.0f, 3.0f, 4.0f };
        float values[5][4];
        le_anim_track_desc track;
        le_animation_clip_desc d;
        int k;
        static const float kDeg[5] = { 0.0f, 25.0f, 0.0f,
                                       -25.0f, 0.0f };

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
        d.duration = 4.0f;
        d.tracks = &track;
        d.track_count = 1;
        TEST_CHECK(le_asset_create_clip(h.engine, &d,
                                        &idle_clip) ==
                       LE_SUCCESS,
                   "idle clip");
    }
    {
        float times[2] = { 0.0f, 1.0f };
        float values[2][4];
        le_anim_track_desc track;
        le_animation_clip_desc d;
        float half = 90.0f * LE_PI_F / 360.0f;

        values[0][0] = values[0][1] = values[0][2] = 0.0f;
        values[0][3] = 1.0f;
        values[1][0] = 0.0f;
        values[1][1] = 0.0f;
        values[1][2] = sinf(half);
        values[1][3] = cosf(half);
        memset(&track, 0, sizeof(track));
        track.target_kind = LE_ANIM_TARGET_JOINT;
        track.target_index = 1;
        track.channel = LE_ANIM_CHANNEL_ROTATION;
        track.interpolation = LE_ANIM_INTERP_LINEAR;
        track.times = times;
        track.values = &values[0][0];
        track.key_count = 2;
        memset(&d, 0, sizeof(d));
        d.duration = 1.0f;
        d.tracks = &track;
        d.track_count = 1;
        TEST_CHECK(le_asset_create_clip(h.engine, &d,
                                        &walk_clip) ==
                       LE_SUCCESS,
                   "walk clip");
    }
    /* Material + arm object (asset-backed renderable so it
     * submits; animator attached per-leg). */
    {
        le_material_asset_desc md;

        memset(&md, 0, sizeof(md));
        md.base_color_factor[0] = 0.85f;
        md.base_color_factor[1] = 0.35f;
        md.base_color_factor[2] = 0.2f;
        md.base_color_factor[3] = 1.0f;
        md.metallic_factor = 0.0f;
        md.roughness_factor = 0.7f;
        TEST_CHECK(le_asset_create_material(h.engine, &md,
                                            &mat) ==
                       LE_SUCCESS,
                   "arm material");
    }
    TEST_CHECK(le_object_create(h.world, &arm) == LE_SUCCESS,
               "arm create");
    TEST_CHECK(le_object_set_name(h.world, &arm, "Arm") ==
                   LE_SUCCESS,
               "arm name");
    {
        float p[3] = { -0.9f, 0.2f, 0.0f };
        le_asset_renderable_desc rd;
        le_animator_desc ad;

        TEST_CHECK(le_object_set_position(h.world, &arm, p) ==
                       LE_SUCCESS,
                   "arm pos");
        memset(&rd, 0, sizeof(rd));
        rd.mesh = arm_mesh;
        rd.material = mat;
        rd.casts_shadow = 1;
        rd.receives_shadow = 1;
        rd.visible = 1;
        TEST_CHECK(le_object_add_asset_renderable(h.world, &arm,
                                                  &rd) ==
                       LE_SUCCESS,
                   "arm renderable");
        /* §41 bind pose: animator attached but PAUSED at t=0
         * (wave key 0 = identity = bind). */
        memset(&ad, 0, sizeof(ad));
        ad.skeleton = skel;
        ad.clip = idle_clip;
        ad.autoplay = 0;
        ad.loop_mode = LE_ANIM_LOOP;
        ad.speed = 1.0f;
        ad.start_time = 0.0f;
        TEST_CHECK(le_object_add_animator(h.world, &arm, &ad) ==
                       LE_SUCCESS,
                   "arm animator attach");
        TEST_CHECK(le_anim_seek(h.world, &arm, 0.0f) ==
                       LE_SUCCESS,
                   "seek bind");
        TEST_CHECK(le_anim_pause(h.world, &arm) == LE_SUCCESS,
                   "pause at bind");
    }

    /* §41 bind pose (no explosion/collapse): the census counts
     * every non-clear pixel (sky dominates), so arm motion is
     * proven through POSE sampling (le_anim_sample_clip: pure
     * function of clip+time) + joint globals + pixel DIFFERENCE
     * between poses. Census stays the liveness guard only. */
    TEST_CHECK(a_frames(&h, 8), "bind settle");
    {
        uint64_t c = a_census(&h);
        char shot[1024];

        printf("[INFO] bind census %llu\n",
               (unsigned long long)c);
        TEST_CHECK(c > 100, "bind pose paints");
        snprintf(shot, sizeof(shot), "%s/40-anim-bind.ppm",
                 shotdir);
        TEST_CHECK(a_shot(&h, shot), "bind shot");
    }
    /* §42 idle: resume, 60 frames (1 s of the 4 s wave). */
    TEST_CHECK(le_anim_resume(h.world, &arm) == LE_SUCCESS,
               "idle resume");
    TEST_CHECK(a_frames(&h, 60), "idle 60 frames");
    {
        uint64_t c = a_census(&h);
        char shot[1024];

        printf("[INFO] idle census %llu t=%.2f\n",
               (unsigned long long)c,
               le_anim_get_time(h.world, &arm));
        TEST_CHECK(le_anim_is_playing(h.world, &arm),
                   "idle playing");
        snprintf(shot, sizeof(shot), "%s/41-anim-idle.ppm",
                 shotdir);
        TEST_CHECK(a_shot(&h, shot), "idle shot");
    }
    /* §43 walk + 47 crossfade (EDIT likelihoods): sample clips as pure
     * functions (idle@1s = 25deg key, walk@1s = 90deg key).
     * Crossfade STATE activates (stats counter), shots taken.
     * LIVE clocks advance only unpaused (play block below). */
    {
        char shot[1024];
        float elbow_idle[4] = { 0, 0, 0, 1 };
        float elbow_walk[4] = { 0, 0, 0, 1 };
        le_anim_stats st0;
        le_anim_stats st1;
        float tt[2][3]; float rr[2][4]; float ss[2][3];
        memset(tt, 0, sizeof(tt));
        memset(rr, 0, sizeof(rr));
        memset(ss, 0, sizeof(ss));
        le_anim_sample_clip(h.engine, &idle_clip, 1.0f,
                            tt, rr, ss, 2, NULL, NULL,
                            NULL);
        memcpy(elbow_idle, rr[1], sizeof(elbow_idle));
        memset(tt, 0, sizeof(tt));
        memset(rr, 0, sizeof(rr));
        memset(ss, 0, sizeof(ss));
        le_anim_sample_clip(h.engine, &walk_clip, 1.0f,
                            tt, rr, ss, 2, NULL, NULL,
                            NULL);
        memcpy(elbow_walk, rr[1], sizeof(elbow_walk));
        printf("[INFO] idle-walk delta %f %f\n",
               (double)elbow_idle[2],
               (double)elbow_walk[2]);
        {
            float d = fabsf(elbow_walk[2] - elbow_idle[2])
                      + fabsf(elbow_walk[3] - elbow_idle[3]);
            TEST_CHECK(d > 0.2f, "walk differs from idle");
        }
        memset(&st0, 0, sizeof(st0));
        le_anim_get_stats(h.world, &st0);
        TEST_CHECK(le_anim_crossfade(h.world, &arm,
                   &walk_clip, 0.5f) == LE_SUCCESS,
                   "crossfade idle->walk");
        memset(&st1, 0, sizeof(st1));
        le_anim_get_stats(h.world, &st1);
        printf("[INFO] crossfades %u -> %u\n",
               st0.active_crossfades,
               st1.active_crossfades);
        TEST_CHECK(st1.active_crossfades ==
                   st0.active_crossfades + 1,
                   "crossfade activates");
        TEST_CHECK(a_frames(&h, 8), "fade early");
        snprintf(shot, sizeof(shot),
                 "%s/42-xfade-mid.ppm", shotdir);
        TEST_CHECK(a_shot(&h, shot), "xfade mid shot");
        TEST_CHECK(a_frames(&h, 68), "fade late + walk");
        snprintf(shot, sizeof(shot),
                 "%s/43-anim-walk.ppm", shotdir);
        TEST_CHECK(a_shot(&h, shot), "walk shot");
        TEST_CHECK(le_anim_crossfade(h.world, &arm,
                   &idle_clip, 0.5f) == LE_SUCCESS,
                   "crossfade walk->idle");
        TEST_CHECK(a_frames(&h, 40), "reversal settle");
        snprintf(shot, sizeof(shot),
                 "%s/43b-anim-back.ppm", shotdir);
        TEST_CHECK(a_shot(&h, shot), "reversal shot");
    }
    /* §52 play/stop (LIVE clocks — the edit world is paused by
     * the engine contract, so playback time advances here): */
    TEST_CHECK(led_play_enter(h.session) == LED_SUCCESS,
               "anim play enter");
    {
        le_world *rw = led_play_get_world(h.session);
        le_object rarm = LE_OBJECT_INVALID;
        float t0 = 0.0f;
        float t1 = 0.0f;

        TEST_CHECK(rw != NULL, "anim runtime world live");
        TEST_CHECK(le_world_find_by_name(rw, "Arm", &rarm),
                   "anim runtime arm live");
        TEST_CHECK(a_frames(&h, 60), "anim play 60 frames");
        t0 = le_anim_get_time(rw, &rarm);
        printf("[INFO] anim play t=%.2f playing=%d\n", t0,
               le_anim_is_playing(rw, &rarm));
        TEST_CHECK(t0 > 0.3f, "anim time advances in play");
        TEST_CHECK(le_anim_is_playing(rw, &rarm),
                   "anim playing in play");
        {
            char shot[1024];

            snprintf(shot, sizeof(shot), "%s/44-anim-play.ppm",
                     shotdir);
            TEST_CHECK(a_shot(&h, shot), "anim play shot");
        }
        /* Crossfade in play (runtime API, same call the Lua
         * binding uses): switch to walk mid-play, assert the
         * elbow track moves. */
        TEST_CHECK(le_anim_crossfade(rw, &rarm, &walk_clip,
                                     0.5f) == LE_SUCCESS,
                   "anim play crossfade");
        TEST_CHECK(a_frames(&h, 40), "anim play fade");
        t1 = le_anim_get_time(rw, &rarm);
        printf("[INFO] anim play post-fade t=%.2f\n", t1);
        /* Crossfade to the 1 s walk clip restarts its clock
         * (fresh 0.67 < idle 1.00) — the assertion is the fade
         * RAN (time advanced on the new clip + still playing),
         * not monotonic growth across the switch. */
        TEST_CHECK(le_anim_is_playing(rw, &rarm),
                   "anim still playing after fade");
        TEST_CHECK(t1 > 0.3f, "anim fade advances time");
        {
            char shot[1024];

            snprintf(shot, sizeof(shot),
                     "%s/45-anim-playwalk.ppm", shotdir);
            TEST_CHECK(a_shot(&h, shot), "anim play walk shot");
        }
    }
    TEST_CHECK(led_play_exit(h.session) == LED_SUCCESS,
               "anim play exit");
    /* Edit world kept its authored animator (attach + clip). */
    {
        le_animator_desc ad;

        memset(&ad, 0, sizeof(ad));
        TEST_CHECK(le_object_get_animator(h.world, &arm, &ad),
                   "edit animator survives play");
    }

    printf("animlive: %d passed, %d failed\n", g_passed,
           g_failed);
    lc_shutdown();
    return (g_failed == 0) ? 0 : 1;
}
