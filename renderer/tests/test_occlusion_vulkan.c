/* Phase 23 occlusion-culling test.
 *
 * A deterministic wall scene proves conservative GPU Hi-Z
 * occlusion: fully hidden objects are culled, visible/partial
 * objects are retained, and Hi-Z ON/OFF final pixels agree with
 * LOD disabled. Near-plane intersection, camera-inside-bound,
 * screen-edge, teleport, 180-degree rotation, moving-occluder,
 * and an open-scene yaw sweep (false-occlusion stress) are pinned.
 * Counts come from the test-only stats download; production frames
 * never stall. Validation layers stay enabled throughout.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>

#include "graphics/graphics_internal.h"
#include "internal/renderer_internal.h"

static int g_passed;
static int g_failed;

#define CHECK(c, m) do {                                                \
    if (c) { printf("[PASS] %s\n", m); g_passed++; }                    \
    else { printf("[FAIL] %s\n", m); g_failed++; }                     \
} while (0)

#define SKIP_ENV(what) do {                                             \
    printf("SKIP: environment cannot provide %s\n", what);              \
    lc_shutdown();                                                      \
    return 0;                                                           \
} while (0)

enum { W = 256, H = 256 };

typedef struct occ_env {
    lc_device *device;
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
    lr_renderer *renderer;
    lr_mesh *cube;
    lr_material *mats[5];
} occ_env;

typedef struct placed {
    int mat;
    float x, y, z, s;
} placed;

static void submit_one(occ_env *env, int mat, float x, float y,
                       float z, float s) {
    lr_draw_item item;

    memset(&item, 0, sizeof(item));
    lr_transform_identity(&item.transform);
    item.transform.position[0] = x;
    item.transform.position[1] = y;
    item.transform.position[2] = z;
    item.transform.scale[0] = s;
    item.transform.scale[1] = s;
    item.transform.scale[2] = s;
    item.mesh = env->cube;
    item.material = env->mats[mat];
    item.casts_shadow = 0;
    item.receives_shadow = 1;
    lr_renderer_submit(env->renderer, &item);
}

/* Wall scene: 0 gray wall, 1 red hidden, 2 green beside,
 * 3 yellow partial, 4 cyan above. */
static const placed k_wall_scene[] = {
    { 0, 0.0f, 0.0f, -6.0f, 12.0f },
    { 1, -3.0f, 0.0f, -12.0f, 1.0f },
    { 1, 0.0f, 1.0f, -13.0f, 1.5f },
    { 1, 3.0f, -1.0f, -12.0f, 1.0f },
    { 2, -10.0f, 0.0f, -10.0f, 1.0f },
    { 2, 10.0f, 0.0f, -10.0f, 1.0f },
    { 3, 6.2f, 0.0f, -8.0f, 2.0f },
    { 4, 0.0f, 6.0f, -8.0f, 1.5f },
};

static void submit_wall_scene(occ_env *env) {
    size_t i;

    /* The wall cube scaled (12,8,0.5): submit_one only does
     * uniform scale, so emulate with a wide flat placement the
     * uniform scaler still covers: scale 12 would be a giant
     * cube; instead place the wall via a dedicated non-uniform
     * transform below. */
    for (i = 1; i < sizeof(k_wall_scene) / sizeof(k_wall_scene[0]);
         i++) {
        submit_one(env, k_wall_scene[i].mat, k_wall_scene[i].x,
                   k_wall_scene[i].y, k_wall_scene[i].z,
                   k_wall_scene[i].s);
    }
    {
        lr_draw_item item;

        memset(&item, 0, sizeof(item));
        lr_transform_identity(&item.transform);
        item.transform.position[2] = -6.0f;
        item.transform.scale[0] = 6.0f;
        item.transform.scale[1] = 4.0f;
        item.transform.scale[2] = 0.25f;
        item.mesh = env->cube;
        item.material = env->mats[0];
        item.casts_shadow = 0;
        item.receives_shadow = 1;
        lr_renderer_submit(env->renderer, &item);
    }
}

static void make_camera(lr_camera *camera, const float eye[3],
                        const float center[3]) {
    static const float up[3] = { 0.0f, 1.0f, 0.0f };

    lr_camera_init(camera);
    lr_camera_set_perspective(camera, 1.5707963f, 1.0f, 0.1f,
                              100.0f);
    lr_camera_look_at(camera, eye, center, up);
}

/* Edge-case submitters (single cubes). */
static void submit_near_plane(occ_env *env) {
    /* Spans the 0.1 near plane (eye z=5 -> plane z=4.9). */
    submit_one(env, 1, 0.0f, 0.0f, 4.5f, 1.0f);
}

static void submit_inside(occ_env *env) {
    /* Camera (0,0,5) sits inside this radius-10 cube. */
    submit_one(env, 2, 0.0f, 0.0f, -5.0f, 10.0f);
}

static void submit_edge(occ_env *env) {
    /* Center near the right frustum edge, half offscreen. */
    submit_one(env, 3, 11.0f, 0.0f, -8.0f, 3.0f);
}

static void submit_wall_scene_nomwall(occ_env *env) {
    size_t i;

    for (i = 1; i < sizeof(k_wall_scene) / sizeof(k_wall_scene[0]);
         i++) {
        submit_one(env, k_wall_scene[i].mat, k_wall_scene[i].x,
                   k_wall_scene[i].y, k_wall_scene[i].z,
                   k_wall_scene[i].s);
    }
}

/* Open arc: five cubes 20 degrees apart at radius 15 (no mutual
 * occlusion from the origin at any yaw). */
static void submit_arc(occ_env *env) {
    static const float angles[5] = { -0.6981317f, -0.3490659f,
                                     0.0f, 0.3490659f,
                                     0.6981317f };
    int k;

    for (k = 0; k < 5; k++) {
        submit_one(env, 2, 15.0f * sinf(angles[k]), 0.0f,
                   -15.0f * cosf(angles[k]), 1.0f);
    }
}

/* Render one frame with the given eye/center; returns 1 on full
 * success (CHECKs fire inside). */
static int render_frame(occ_env *env, const float eye[3],
                        const float center[3],
                        void (*submit)(occ_env *), const char *msg) {
    lc_command_encoder *enc = NULL;
    lr_camera camera;
    lr_light light;

    lc_poll_events();
    if (lc_begin_frame(env->swapchain) != LC_SUCCESS ||
        lc_swapchain_get_encoder(env->swapchain, &enc) !=
            LC_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    make_camera(&camera, eye, center);
    if (lr_renderer_begin(env->renderer, &camera) != LR_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    /* One unshadowed key light (PBR needs direct light for
     * meaningful pixel proofs; ambient alone renders dark). */
    memset(&light, 0, sizeof(light));
    light.type = LR_LIGHT_DIRECTIONAL;
    light.color[0] = 1.0f;
    light.color[1] = 1.0f;
    light.color[2] = 1.0f;
    light.intensity = 2.0f;
    light.direction[0] = 0.3f;
    light.direction[1] = -1.0f;
    light.direction[2] = 0.4f;
    lr_renderer_submit_light(env->renderer, &light);
    if (submit != NULL) {
        submit(env);
    }
    if (lr_renderer_render_scene(env->renderer, enc, W, H) !=
        LR_SUCCESS) {
        CHECK(0, msg);
        return 0;
    }
    lr_renderer_end(env->renderer);
    {
        lc_render_swapchain_pass_desc spass;

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.depth_load_op = LC_LOAD_OP_CLEAR;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        if (lc_encoder_begin_swapchain_pass(enc, env->swapchain,
                                            &spass) != LC_SUCCESS ||
            lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            CHECK(0, msg);
            return 0;
        }
    }
    {
        lc_result end_res = lc_end_frame(env->swapchain);

        if (end_res != LC_SUCCESS && end_res != LC_SUBOPTIMAL) {
            CHECK(0, msg);
            return 0;
        }
    }
    return 1;
}

/* Snapshot stats + HDR pixels (malloc'd, caller frees). */
static int snapshot(occ_env *env, lr_visibility_stats *out_stats,
                    unsigned char **out_px, size_t *out_size) {
    lc_image_view *view;
    lc_image *image;
    lc_image_readback_desc desc;
    lc_image_readback_info info;
    unsigned char *px = NULL;

    if (lr_renderer_update_visibility_stats(env->renderer) !=
        LR_SUCCESS) {
        return 0;
    }
    lr_renderer_get_visibility_stats(env->renderer, out_stats);
    printf("[info] total=%llu frustum=%llu occlusion=%llu visible=%llu\n",
           (unsigned long long)out_stats->total_instances,
           (unsigned long long)out_stats->frustum_rejected,
           (unsigned long long)out_stats->occlusion_rejected,
           (unsigned long long)out_stats->visible);
    view = lr_renderer_get_hdr_view(env->renderer);
    if (view == NULL) {
        return 0;
    }
    image = lc_image_view_get_image(view);
    if (image == NULL) {
        return 0;
    }
    memset(&desc, 0, sizeof(desc));
    if (lc_image_query_readback(image, &desc, &info) != LC_SUCCESS) {
        return 0;
    }
    px = (unsigned char *)malloc(info.size);
    if (px == NULL) {
        return 0;
    }
    if (lc_image_readback(image, &desc, px, info.size, NULL) !=
        LC_SUCCESS) {
        free(px);
        return 0;
    }
    *out_px = px;
    *out_size = info.size;
    return 1;
}

static void set_vis(occ_env *env, int hiz) {
    lr_visibility_settings settings;
    lr_visibility_settings_default(&settings);
    settings.enabled = 1;
    settings.hiz_enabled = hiz;
    settings.lod_enabled = 0;
    settings.indirect_count_enabled = 1;
    /* Graph schedules the visibility passes (derived order must
     * match the manual path exactly). */
    settings.graph_enabled = 1;
    if (lr_renderer_set_render_mode(env->renderer,
                                    LR_RENDER_MODE_GPU_DRIVEN) !=
        LR_SUCCESS) {
        CHECK(0, "gpu-driven mode selects");
    }
    CHECK(lr_renderer_set_visibility(env->renderer, &settings) ==
              LR_SUCCESS,
          hiz ? "visibility hiz-on applies"
              : "visibility hiz-off applies");
}

int main(void) {
    occ_env env;
    lc_device_desc ddesc;
    lc_window_desc wdesc;
    lc_swapchain_desc sdesc;
    lr_renderer_desc rdesc;
    lr_pbr_material_desc matdesc;
    static const float colors[5][4] = {
        { 0.5f, 0.5f, 0.5f, 1.0f },
        { 1.0f, 0.1f, 0.1f, 1.0f },
        { 0.1f, 1.0f, 0.1f, 1.0f },
        { 1.0f, 1.0f, 0.1f, 1.0f },
        { 0.1f, 1.0f, 1.0f, 1.0f },
    };
    static const float eye0[3] = { 0.0f, 0.0f, 5.0f };
    static const float center0[3] = { 0.0f, 0.0f, 0.0f };
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running LumaC occlusion (Phase 23) test...\n");
    memset(&env, 0, sizeof(env));
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.backend = LC_BACKEND_VULKAN;
    ddesc.enable_validation = 1;
    if (lc_device_create(&ddesc, &env.device) != LC_SUCCESS) {
        SKIP_ENV("Vulkan device");
    }
    memset(&wdesc, 0, sizeof(wdesc));
    wdesc.title = "LumaC Occlusion";
    wdesc.width = 320;
    wdesc.height = 320;
    if (lc_window_create(&wdesc, &env.window) != LC_SUCCESS ||
        lc_surface_create(env.device, env.window, &env.surface) !=
            LC_SUCCESS) {
        SKIP_ENV("windowed Vulkan");
    }
    memset(&sdesc, 0, sizeof(sdesc));
    sdesc.width = 320;
    sdesc.height = 320;
    sdesc.vsync = 1;
    if (lc_swapchain_create(env.device, env.surface, &sdesc,
                            &env.swapchain) != LC_SUCCESS) {
        SKIP_ENV("windowed Vulkan");
    }
    memset(&rdesc, 0, sizeof(rdesc));
    rdesc.device = env.device;
    rdesc.render_target.color_attachment_count = 1;
    rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
    rdesc.max_objects = 64;
    rdesc.ambient_light[0] = 0.6f;
    rdesc.ambient_light[1] = 0.6f;
    rdesc.ambient_light[2] = 0.6f;
    if (lr_renderer_create(&rdesc, &env.renderer) != LR_SUCCESS) {
        printf("renderer create failed: FAIL\n");
        return 1;
    }
    if (lr_mesh_create_cube(env.renderer, 2.0f, &env.cube) !=
        LR_SUCCESS) {
        printf("cube create failed: FAIL\n");
        return 1;
    }
    for (i = 0; i < 5; i++) {
        memset(&matdesc, 0, sizeof(matdesc));
        memcpy(matdesc.base_color_factor, colors[i],
               sizeof(colors[i]));
        matdesc.metallic_factor = 0.0f;
        matdesc.roughness_factor = 0.6f;
        matdesc.normal_scale = 1.0f;
        matdesc.occlusion_strength = 1.0f;
        matdesc.alpha_mode = LR_ALPHA_OPAQUE;
        if (lr_material_create_pbr(env.renderer, &matdesc,
                                   &env.mats[i]) != LR_SUCCESS) {
            printf("material create failed: FAIL\n");
            return 1;
        }
    }
    /* ---- hiz OFF baseline: nothing culled by occlusion ---- */
    set_vis(&env, 0);
    CHECK(render_frame(&env, eye0, center0, submit_wall_scene,
                       "warm off 1"),
          "warm off 1 renders");
    CHECK(render_frame(&env, eye0, center0, submit_wall_scene,
                       "warm off 2"),
          "warm off 2 renders");
    {
        lr_visibility_stats off;
        unsigned char *off_px = NULL;
        size_t off_size = 0;
        lr_visibility_stats on;
        unsigned char *on_px = NULL;
        size_t on_size = 0;

        CHECK(snapshot(&env, &off, &off_px, &off_size),
              "off stats+pixels captured");
        CHECK(off.frustum_rejected == 0, "off frustum rejects none");
        CHECK(off.occlusion_rejected == 0, "off occludes none");
        CHECK(off.visible == 8, "off keeps all eight");
        /* ---- hiz ON: bypass frame keeps all, settled frame
         * culls exactly the three hidden cubes ---- */
        set_vis(&env, 1);
        CHECK(render_frame(&env, eye0, center0, submit_wall_scene,
                           "on bypass frame"),
              "on bypass frame renders");
        CHECK(snapshot(&env, &on, &on_px, &on_size),
              "bypass stats captured");
        CHECK(on.occlusion_rejected == 0, "bypass occludes none");
        CHECK(on.visible == 8, "bypass keeps all eight");
        free(on_px);
        CHECK(render_frame(&env, eye0, center0, submit_wall_scene,
                           "on settled frame"),
              "on settled frame renders");
        CHECK(snapshot(&env, &on, &on_px, &on_size),
              "on stats+pixels captured");
        CHECK(on.frustum_rejected == 0,
              "settled frustum rejects none");
        CHECK(on.occlusion_rejected == 3,
              "settled occludes three hidden");
        CHECK(on.visible == 5,
              "settled keeps wall/beside/partial/above");
        CHECK(off_size == on_size, "pixel buffers match in size");
        if (off_size == on_size && off_px != NULL &&
            on_px != NULL) {
            CHECK(memcmp(off_px, on_px, off_size) == 0,
                  "hiz on/off pixels identical");
            /* The wall fills the frame center: its pixels must be
             * lit (guards a vacuously-clear false pass). HDR16F
             * center texel, any channel well above black. */
            {
                size_t center =
                    ((size_t)(H / 2) * (size_t)W + (size_t)(W / 2)) *
                    4u * sizeof(uint16_t);
                const uint16_t *c16 = (const uint16_t *)(off_px +
                                                         center);

                CHECK(center + 8u <= off_size &&
                          (c16[0] > 0x2000u || c16[1] > 0x2000u ||
                           c16[2] > 0x2000u),
                      "wall center pixel is lit");
            }
        } else {
            CHECK(0, "hiz on/off pixels identical");
        }
        free(off_px);
        free(on_px);
    }
    /* ---- near-plane intersection stays visible ---- */
    set_vis(&env, 1);
    CHECK(render_frame(&env, eye0, center0, submit_near_plane,
                       "near warm"),
          "near warm renders");
    CHECK(render_frame(&env, eye0, center0, submit_near_plane,
                       "near settled"),
          "near settled renders");
    {
        lr_visibility_stats st;
        unsigned char *px = NULL;
        size_t size = 0;

        CHECK(snapshot(&env, &st, &px, &size), "near captured");
        CHECK(st.visible == 1, "near-plane cube stays visible");
        free(px);
    }
    /* ---- camera inside the bound stays visible ---- */
    CHECK(render_frame(&env, eye0, center0, submit_inside,
                       "inside warm"),
          "inside warm renders");
    CHECK(render_frame(&env, eye0, center0, submit_inside,
                       "inside settled"),
          "inside settled renders");
    {
        lr_visibility_stats st;
        unsigned char *px = NULL;
        size_t size = 0;

        CHECK(snapshot(&env, &st, &px, &size), "inside captured");
        CHECK(st.visible == 1, "camera-inside cube stays visible");
        free(px);
    }
    /* ---- screen-edge cube stays visible ---- */
    CHECK(render_frame(&env, eye0, center0, submit_edge,
                       "edge warm"),
          "edge warm renders");
    CHECK(render_frame(&env, eye0, center0, submit_edge,
                       "edge settled"),
          "edge settled renders");
    {
        lr_visibility_stats st;
        unsigned char *px = NULL;
        size_t size = 0;

        CHECK(snapshot(&env, &st, &px, &size), "edge captured");
        CHECK(st.visible == 1, "screen-edge cube stays visible");
        free(px);
    }
    /* ---- teleport bypasses occlusion for one frame ---- */
    {
        static const float far_eye[3] = { 40.0f, 0.0f, 5.0f };
        lr_visibility_stats st;
        unsigned char *px = NULL;
        size_t size = 0;

        set_vis(&env, 1);
        CHECK(render_frame(&env, eye0, center0, submit_wall_scene,
                           "pre-teleport"),
              "pre-teleport renders");
        CHECK(render_frame(&env, far_eye, center0,
                           submit_wall_scene, "teleport frame"),
              "teleport frame renders");
        CHECK(snapshot(&env, &st, &px, &size), "teleport captured");
        CHECK(st.occlusion_rejected == 0,
              "teleport occludes none");
        CHECK(st.visible == 8, "teleport keeps all eight");
        free(px);
        CHECK(render_frame(&env, far_eye, center0,
                           submit_wall_scene, "post-teleport"),
              "post-teleport renders");
        CHECK(snapshot(&env, &st, &px, &size),
              "post-teleport captured");
        CHECK(st.visible > 0, "post-teleport draws survivors");
        free(px);
    }
    /* ---- 180-degree rotation: everything behind ---- */
    {
        static const float behind[3] = { 0.0f, 0.0f, 20.0f };
        lr_visibility_stats st;
        unsigned char *px = NULL;
        size_t size = 0;

        set_vis(&env, 1);
        CHECK(render_frame(&env, eye0, behind, submit_wall_scene,
                           "rotated warm"),
              "rotated warm renders");
        CHECK(render_frame(&env, eye0, behind, submit_wall_scene,
                           "rotated settled"),
              "rotated settled renders");
        CHECK(snapshot(&env, &st, &px, &size), "rotated captured");
        CHECK(st.visible == 0, "rotated draws none");
        CHECK(st.frustum_rejected == 8,
              "rotated frustum rejects all eight");
        free(px);
    }
    /* ---- moving the occluder away reveals the hidden ---- */
    {
        lr_visibility_stats st;
        unsigned char *px = NULL;
        size_t size = 0;

        set_vis(&env, 1);
        CHECK(render_frame(&env, eye0, center0, submit_wall_scene,
                           "occluder warm"),
              "occluder warm renders");
        CHECK(render_frame(&env, eye0, center0,
                           submit_wall_scene_nomwall,
                           "occluder moved"),
              "occluder moved renders");
        CHECK(render_frame(&env, eye0, center0,
                           submit_wall_scene_nomwall,
                           "occluder settled"),
              "occluder settled renders");
        CHECK(snapshot(&env, &st, &px, &size),
              "revealed captured");
        CHECK(st.occlusion_rejected == 0,
              "revealed occludes none");
        CHECK(st.visible == 7, "revealed keeps all seven");
        free(px);
    }
    /* ---- open-scene yaw sweep: on must equal off ---- */
    {
        int step;

        set_vis(&env, 0);
        for (step = 0; step < 12; step++) {
            float yaw = (float)step * 0.5235988f;
            float eye[3] = { 15.0f * sinf(yaw), 0.0f,
                             15.0f * cosf(yaw) };
            static const float origin[3] = { 0.0f, 0.0f, 0.0f };
            char msg[64];

            snprintf(msg, sizeof(msg), "open off yaw %d renders",
                     step);
            if (!render_frame(&env, eye, origin, submit_arc,
                              msg)) {
                CHECK(0, msg);
                break;
            }
        }
        {
            lr_visibility_stats off;
            unsigned char *px = NULL;
            size_t size = 0;

            CHECK(snapshot(&env, &off, &px, &size),
                  "open off captured");
            free(px);
            set_vis(&env, 1);
            for (step = 0; step < 12; step++) {
                float yaw = (float)step * 0.5235988f;
                float eye[3] = { 15.0f * sinf(yaw), 0.0f,
                                 15.0f * cosf(yaw) };
                static const float origin[3] = { 0.0f, 0.0f,
                                                 0.0f };
                char msg[64];

                snprintf(msg, sizeof(msg), "open on yaw %d renders",
                         step);
                if (!render_frame(&env, eye, origin, submit_arc,
                                  msg)) {
                    CHECK(0, msg);
                    break;
                }
            }
            {
                lr_visibility_stats on;
                unsigned char *opx = NULL;
                size_t osize = 0;

                CHECK(snapshot(&env, &on, &opx, &osize),
                      "open on captured");
                CHECK(on.visible == off.visible,
                      "sweep on/off visibility agrees");
                free(opx);
            }
        }
    }
    for (i = 0; i < 5; i++) {
        lr_material_destroy(env.mats[i]);
    }
    lr_mesh_destroy(env.cube);
    lr_renderer_destroy(env.renderer);
    lc_swapchain_destroy(env.swapchain);
    lc_surface_destroy(env.surface);
    lc_device_destroy(env.device);
    lc_window_destroy(env.window);
    lc_shutdown();
    printf("occlusion: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}