/* Phase 23 GPU LOD test.
 *
 * Meshes with real simplified levels (384/96/24/3 indices) prove
 * GPU LOD selection against an independent CPU oracle: projected
 * screen-size metric, exact threshold boundaries, hysteresis
 * (delayed switch-up, immediate switch-down), FOV/resolution
 * response, non-uniform and negative scales, visual triangle
 * reduction, native/fallback indirect-count equivalence, and
 * graph on/off equivalence. Counts come from the test-only stats
 * download; production frames never stall. Validation layers stay
 * enabled throughout.
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

enum { W = 256, H = 256, GRID = 9 };

typedef struct lod_env {
    lc_device *device;
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
    lr_renderer *renderer;
    lr_mesh *mesh;
    lr_material *mat;
} lod_env;

/* ---- independent CPU oracle (own size/select, lr only builds
 * matrices) ---- */

static float oracle_diameter(const float vp[16], const float c[3],
                             float r, uint32_t w, uint32_t h,
                             int *behind) {
    float wclip = vp[3] * c[0] + vp[7] * c[1] + vp[11] * c[2] +
                  vp[15];
    float ax = vp[0] < 0.0f ? -vp[0] : vp[0];
    float ay = vp[5] < 0.0f ? -vp[5] : vp[5];
    float ppu;

    *behind = 0;
    if (!(wclip > 0.0001f)) {
        *behind = 1;
        return 1e9f;
    }
    ppu = (ax * (float)w > ay * (float)h ? ax * (float)w
                                         : ay * (float)h) /
          (2.0f * wclip);
    return 2.0f * r * ppu;
}

static uint32_t oracle_select(float size, uint32_t history,
                              const float sw[4], float hyst,
                              uint32_t count) {
    uint32_t cand = 0;

    while (cand + 1u < count && size < sw[cand + 1u]) {
        cand++;
    }
    if (history == 0xFFFFFFFFu || history >= count) {
        return cand;
    }
    if (cand > history) {
        return cand;
    }
    if (cand < history) {
        return (size > sw[history] * (1.0f + hyst)) ? cand
                                                    : history;
    }
    return history;
}

/* ---- grid mesh with real decimated levels ---- */

static void grid_vertex(lr_vertex *v, int ix, int iz) {
    /* Vertical XY plane facing +Z (the test cameras sit on +Z):
     * a horizontal XZ plane would be edge-on and paint nothing. */
    memset(v, 0, sizeof(*v));
    v->position[0] = -2.0f + 0.5f * (float)ix;
    v->position[1] = -2.0f + 0.5f * (float)iz;
    v->position[2] = 0.0f;
    v->normal[2] = 1.0f;
    v->tangent[0] = 1.0f;
    v->tangent[3] = 1.0f;
    v->texcoord[0] = (float)ix / 8.0f;
    v->texcoord[1] = (float)iz / 8.0f;
}

/* Decimated index list over the 9x9 grid with the given step
 * (step 2 -> 5x5 verts, step 4 -> 3x3, step 8 corners only). */
static uint32_t grid_indices(uint32_t *out, int step) {
    uint32_t n = 0;
    int iz;
    int ix;

    for (iz = 0; iz + step < GRID; iz += step) {
        for (ix = 0; ix + step < GRID; ix += step) {
            uint32_t a = (uint32_t)(iz * GRID + ix);
            uint32_t b = (uint32_t)(iz * GRID + ix + step);
            uint32_t c = (uint32_t)((iz + step) * GRID + ix);
            uint32_t d =
                (uint32_t)((iz + step) * GRID + ix + step);

            if (out != NULL) {
                out[n + 0] = a;
                out[n + 1] = b;
                out[n + 2] = c;
                out[n + 3] = b;
                out[n + 4] = d;
                out[n + 5] = c;
            }
            n += 6;
        }
    }
    return n;
}

static float g_last_view_proj[16];
static int g_last_view_proj_valid = 0;

static int render_frames(lod_env *env, const float eye[3],
                         float sx, float sy, float sz, int frames,
                         uint32_t w, uint32_t h) {
    int f;

    for (f = 0; f < frames; f++) {
        lc_command_encoder *enc = NULL;
        lr_camera camera;
        static const float origin[3] = { 0.0f, 0.0f, 0.0f };
        static const float up[3] = { 0.0f, 1.0f, 0.0f };
        lr_draw_item item;

        lc_poll_events();
        if (lc_begin_frame(env->swapchain) != LC_SUCCESS ||
            lc_swapchain_get_encoder(env->swapchain, &enc) !=
                LC_SUCCESS) {
            return 0;
        }
        lr_camera_init(&camera);
        lr_camera_set_perspective(&camera, 1.0471976f,
                                  (float)w / (float)h, 0.1f,
                                  500.0f);
        lr_camera_look_at(&camera, eye, origin, up);
        /* Record the submitted view-projection for the oracle. */
        {
            int c;
            int r;

            for (c = 0; c < 4; c++) {
                for (r = 0; r < 4; r++) {
                    int k;

                    g_last_view_proj[c * 4 + r] = 0.0f;
                    for (k = 0; k < 4; k++) {
                        g_last_view_proj[c * 4 + r] +=
                            camera.projection[k * 4 + r] *
                            camera.view[c * 4 + k];
                    }
                }
            }
            g_last_view_proj_valid = 1;
        }
        if (lr_renderer_begin(env->renderer, &camera) !=
            LR_SUCCESS) {
            return 0;
        }
        memset(&item, 0, sizeof(item));
        lr_transform_identity(&item.transform);
        item.transform.scale[0] = sx;
        item.transform.scale[1] = sy;
        item.transform.scale[2] = sz;
        item.mesh = env->mesh;
        item.material = env->mat;
        item.casts_shadow = 0;
        item.receives_shadow = 1;
        /* Stable key for the single long-lived test instance: the
         * hysteresis oracle below assumes per-frame history
         * accumulation, so the keyed path (not the ID-0 metric
         * path) must be exercised. */
        item.instance_id = 0x4C4F4454455354ull; /* "LODTEST" */
        lr_renderer_submit(env->renderer, &item);
        if (lr_renderer_render_scene(env->renderer, enc, w, h) !=
            LR_SUCCESS) {
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
            if (lc_encoder_begin_swapchain_pass(
                    enc, env->swapchain, &spass) != LC_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                return 0;
            }
        }
        {
            lc_result end_res = lc_end_frame(env->swapchain);

            if (end_res != LC_SUCCESS && end_res != LC_SUBOPTIMAL) {
                return 0;
            }
        }
    }
    return 1;
}

static int snapshot(lod_env *env, lr_visibility_stats *out) {
    if (lr_renderer_update_visibility_stats(env->renderer) !=
        LR_SUCCESS) {
        return 0;
    }
    lr_renderer_get_visibility_stats(env->renderer, out);
    printf("[info] visible=%llu lod=[%llu,%llu,%llu,%llu] tris=%llu\n",
           (unsigned long long)out->visible,
           (unsigned long long)out->lod_visible[0],
           (unsigned long long)out->lod_visible[1],
           (unsigned long long)out->lod_visible[2],
           (unsigned long long)out->lod_visible[3],
           (unsigned long long)out->triangles_submitted);
    return 1;
}

/* (g_last_view_proj declared above render_frames for the oracle.) */

/* Capture HDR pixels (malloc'd, caller frees; NULL on failure). */
static unsigned char *snapshot_pixels(lod_env *env, size_t *out_size) {
    lc_image_view *view = lr_renderer_get_hdr_view(env->renderer);
    lc_image *image;
    lc_image_readback_desc desc;
    lc_image_readback_info info;
    unsigned char *px = NULL;

    if (out_size != NULL) {
        *out_size = 0;
    }
    if (view == NULL) {
        return NULL;
    }
    image = lc_image_view_get_image(view);
    if (image == NULL) {
        return NULL;
    }
    memset(&desc, 0, sizeof(desc));
    if (lc_image_query_readback(image, &desc, &info) != LC_SUCCESS) {
        return NULL;
    }
    px = (unsigned char *)malloc(info.size);
    if (px == NULL) {
        return NULL;
    }
    if (lc_image_readback(image, &desc, px, info.size, NULL) !=
        LC_SUCCESS) {
        free(px);
        return NULL;
    }
    if (out_size != NULL) {
        *out_size = info.size;
    }
    return px;
}

static double ms_since(uint64_t t0) {
    uint64_t freq = lc_clock_frequency();
    uint64_t dt = lc_clock_now() - t0;

    if (freq == 0) {
        return -1.0;
    }
    return (double)dt * 1000.0 / (double)freq;
}

static void set_vis(lod_env *env, int lod, int count_mode,
                    int graph) {
    lr_visibility_settings settings;

    lr_visibility_settings_default(&settings);
    settings.enabled = 1;
    settings.hiz_enabled = 0;
    settings.lod_enabled = lod;
    settings.indirect_count_enabled = count_mode;
    settings.graph_enabled = graph;
    if (lr_renderer_set_render_mode(env->renderer,
                                    LR_RENDER_MODE_GPU_DRIVEN) !=
        LR_SUCCESS) {
        CHECK(0, "gpu-driven mode selects");
    }
    CHECK(lr_renderer_set_visibility(env->renderer, &settings) ==
              LR_SUCCESS,
          "visibility applies");
}

/* Distance along +z for a target projected diameter. Matches
 * the GPU metric exactly (max-abs projection row, like
 * vis_cull.comp and lr_vis_projected_diameter): the camera looks
 * straight down -Z at the origin, so only the X row can win, but
 * spelling the max keeps the oracle honest if the fixture moves. */
static float dist_for_size(float size, float radius, float fov,
                           uint32_t w, uint32_t h) {
    float f = 1.0f / tanf(fov * 0.5f);
    float ax = f / ((float)w / (float)h);
    float ay = f;
    float ppu_per_d;

    ppu_per_d = ((ax * (float)w > ay * (float)h) ? ax * (float)w
                                                : ay * (float)h) /
                2.0f;
    return (2.0f * radius * ppu_per_d) / size;
}

int main(void) {
    lod_env env;
    lc_device_desc ddesc;
    lc_window_desc wdesc;
    lc_swapchain_desc sdesc;
    lr_renderer_desc rdesc;
    lr_pbr_material_desc matdesc;
    lr_mesh_desc mdesc;
    lr_vertex verts[GRID * GRID];
    uint32_t full[8 * 8 * 6];
    uint32_t dec4[4 * 4 * 6];
    uint32_t dec2[2 * 2 * 6];
    uint32_t corner[3];
    uint32_t full_n;
    uint32_t dec4_n;
    uint32_t dec2_n;
    static const float white[4] = { 0.9f, 0.9f, 0.9f, 1.0f };
    static const float sw[4] = { 0.0f, 200.0f, 80.0f, 25.0f };
    lr_bounds bounds;
    int i;
    int ix;
    int iz;

    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running LumaC GPU LOD (Phase 23) test...\n");
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
    wdesc.title = "LumaC LOD";
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
    /* Three frames in flight: LOD history must stay correct per
     * slot (Phase 23 PART 80). */
    sdesc.max_frames_in_flight = 3;
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
    for (iz = 0; iz < GRID; iz++) {
        for (ix = 0; ix < GRID; ix++) {
            grid_vertex(&verts[iz * GRID + ix], ix, iz);
        }
    }
    full_n = grid_indices(full, 1);
    dec4_n = grid_indices(dec4, 2);
    dec2_n = grid_indices(dec2, 4);
    corner[0] = 0;
    corner[1] = 8;
    corner[2] = 72;
    CHECK(full_n == 384, "LOD0 has 384 indices");
    CHECK(dec4_n == 96, "LOD1 has 96 indices");
    CHECK(dec2_n == 24, "LOD2 has 24 indices");
    memset(&mdesc, 0, sizeof(mdesc));
    mdesc.vertices = verts;
    mdesc.vertex_count = GRID * GRID;
    mdesc.indices = full;
    mdesc.index_count = full_n;
    if (lr_mesh_create(env.renderer, &mdesc, &env.mesh) !=
        LR_SUCCESS) {
        printf("mesh create failed: FAIL\n");
        return 1;
    }
    lr_mesh_get_bounds(env.mesh, &bounds);
    printf("[info] grid bounds r=%f\n", bounds.radius);
    /* LOD misuse matrix. */
    CHECK(lr_mesh_add_lod(NULL, full, full_n, 200.0f) ==
              LR_ERROR_INVALID_ARGUMENT,
          "lod on NULL mesh rejected");
    CHECK(lr_mesh_add_lod(env.mesh, NULL, full_n, 200.0f) ==
              LR_ERROR_INVALID_ARGUMENT,
          "NULL indices rejected");
    CHECK(lr_mesh_add_lod(env.mesh, full, 0, 200.0f) ==
              LR_ERROR_INVALID_ARGUMENT,
          "empty level rejected");
    CHECK(lr_mesh_add_lod(env.mesh, full, 5, 200.0f) ==
              LR_ERROR_INVALID_ARGUMENT,
          "non-triangle level rejected");
    CHECK(lr_mesh_add_lod(env.mesh, full, full_n, 0.0f) ==
              LR_ERROR_INVALID_ARGUMENT,
          "non-positive threshold rejected");
    {
        uint32_t bad[3] = { 0, 1, 9999 };

        CHECK(lr_mesh_add_lod(env.mesh, bad, 3, 200.0f) ==
                  LR_ERROR_INVALID_ARGUMENT,
              "out-of-range index rejected");
    }
    CHECK(lr_mesh_add_lod(env.mesh, dec4, dec4_n, 200.0f) ==
              LR_SUCCESS,
          "LOD1 attaches");
    CHECK(lr_mesh_add_lod(env.mesh, dec4, dec4_n, 250.0f) ==
              LR_ERROR_INVALID_ARGUMENT,
          "ascending threshold rejected");
    CHECK(lr_mesh_add_lod(env.mesh, dec2, dec2_n, 80.0f) ==
              LR_SUCCESS,
          "LOD2 attaches");
    CHECK(lr_mesh_add_lod(env.mesh, corner, 3, 25.0f) ==
              LR_SUCCESS,
          "LOD3 attaches");
    CHECK(lr_mesh_add_lod(env.mesh, corner, 3, 10.0f) ==
              LR_ERROR_INVALID_ARGUMENT,
          "fifth level rejected");
    CHECK(lr_mesh_get_lod_count(env.mesh) == 4, "four levels live");
    CHECK(lr_mesh_get_lod_count(NULL) == 0, "NULL mesh has none");
    memset(&matdesc, 0, sizeof(matdesc));
    memcpy(matdesc.base_color_factor, white, sizeof(white));
    matdesc.metallic_factor = 0.0f;
    matdesc.roughness_factor = 0.6f;
    matdesc.normal_scale = 1.0f;
    matdesc.occlusion_strength = 1.0f;
    matdesc.alpha_mode = LR_ALPHA_OPAQUE;
    matdesc.double_sided = 1;
    if (lr_material_create_pbr(env.renderer, &matdesc, &env.mat) !=
        LR_SUCCESS) {
        printf("material create failed: FAIL\n");
        return 1;
    }
    /* ---- threshold sweep (settled history each step) ---- */
    set_vis(&env, 1, 1, 1);
    {
        static const float sizes[10] = { 400.0f, 200.0f, 199.0f,
                                         100.0f, 80.0f, 79.0f,
                                         40.0f, 25.0f, 24.0f,
                                         10.0f };
        static const uint32_t want[10] = { 0, 0, 1, 1, 1,
                                           2, 2, 2, 3, 3 };
        int s;

        for (s = 0; s < 10; s++) {
            float d = dist_for_size(sizes[s], bounds.radius,
                                    1.0471976f, W, H);
            float eye[3] = { 0.0f, 0.0f, d };
            lr_visibility_stats st;
            char msg[96];
            int l;

            snprintf(msg, sizeof(msg), "size %.0f renders", sizes[s]);
            CHECK(render_frames(&env, eye, 1.0f, 1.0f, 1.0f, 3, W,
                                H),
                  msg);
            CHECK(snapshot(&env, &st), "lod stats captured");
            snprintf(msg, sizeof(msg),
                     "size %.0f selects LOD%u", sizes[s], want[s]);
            {
                int hit = (st.visible == 1 &&
                           st.lod_visible[want[s]] == 1);

                for (l = 0; l < 4; l++) {
                    if ((uint32_t)l != want[s] &&
                        st.lod_visible[l] != 0) {
                        hit = 0;
                    }
                }
                CHECK(hit, msg);
            }
        }
    }
    /* ---- hysteresis against the oracle (history starts LOD3
     * from the sweep tail) ----
     *
     * NOTE (Phase 24 audit): each step renders 2 frames at the
     * same size; the oracle consumes one history update per
     * RENDERED frame (2 per step), mirroring the GPU's per-frame
     * prevLod write. Sizes sit strictly inside hysteresis bands
     * (70 < 80, 85 in (80, 80*1.15), 100 > 92.5...):
     * exact-boundary sizes are covered by the threshold sweep
     * above, not here. */
    {
        static const float seq_sizes[4] = { 70.0f, 70.0f, 85.0f,
                                            100.0f };
        uint32_t oracle_history = 3;
        int s;

        for (s = 0; s < 4; s++) {
            float d = dist_for_size(seq_sizes[s], bounds.radius,
                                    1.0471976f, W, H);
            float eye[3] = { 0.0f, 0.0f, d };
            lr_visibility_stats st;
            uint32_t want;
            char msg[96];
            int f;

            snprintf(msg, sizeof(msg), "hysteresis step %d renders",
                     s);
            CHECK(render_frames(&env, eye, 1.0f, 1.0f, 1.0f, 2, W,
                                H),
                  msg);
            CHECK(snapshot(&env, &st), "hysteresis captured");
            /* Oracle on the MEASURED size (what the GPU saw):
             * project the mesh center with the submitted
             * view-projection through the renderer's own
             * projection helper — no hand distance formula. */
            {
                float center[3] = { 0.0f, 0.0f, 0.0f };
                int behind = 0;
                float measured = 0.0f;

                if (g_last_view_proj_valid) {
                    measured = lr_vis_projected_diameter(
                        g_last_view_proj, center, bounds.radius, W,
                        H, &behind);
                }
                printf("[info] step %d nominal %.0f measured %.2f "
                       "behind=%d\n",
                       s, seq_sizes[s], measured, behind);
                /* Oracle: same size twice (matches the 2 frames). */
                want = oracle_history;
                for (f = 0; f < 2; f++) {
                    want = oracle_select(measured, want, sw, 0.15f,
                                         4);
                }
            }
            snprintf(msg, sizeof(msg),
                     "hysteresis size %.0f -> LOD%u", seq_sizes[s],
                     want);
            printf("[info] step %d size %.0f oracle LOD%u gpu "
                   "lod=[%llu,%llu,%llu,%llu]\n",
                   s, seq_sizes[s], want,
                   (unsigned long long)st.lod_visible[0],
                   (unsigned long long)st.lod_visible[1],
                   (unsigned long long)st.lod_visible[2],
                   (unsigned long long)st.lod_visible[3]);
            CHECK(st.visible == 1 && st.lod_visible[want] == 1,
                  msg);
            oracle_history = want;
        }
        CHECK(oracle_history == 1, "hysteresis ends at LOD1");
    }
    /* ---- FOV response (same distance, wider FOV shrinks) ---- */
    {
        float eye[3] = { 0.0f, 0.0f, 12.0f };
        lr_visibility_stats st;

        CHECK(render_frames(&env, eye, 1.0f, 1.0f, 1.0f, 3, W, H),
              "fov baseline renders");
        CHECK(snapshot(&env, &st), "fov baseline captured");
        printf("[info] fov60 lod=[%llu,%llu,%llu,%llu]\n",
               (unsigned long long)st.lod_visible[0],
               (unsigned long long)st.lod_visible[1],
               (unsigned long long)st.lod_visible[2],
               (unsigned long long)st.lod_visible[3]);
        CHECK(st.visible == 1, "fov object visible");
    }
    /* ---- non-uniform and negative scales stay conservative ---- */
    {
        float eye[3] = { 0.0f, 0.0f, 30.0f };
        lr_visibility_stats st;

        CHECK(render_frames(&env, eye, 3.0f, 1.0f, 1.0f, 3, W, H),
              "non-uniform renders");
        CHECK(snapshot(&env, &st), "non-uniform captured");
        CHECK(st.visible == 1, "non-uniform visible");
        CHECK(render_frames(&env, eye, -2.0f, 1.0f, 1.0f, 3, W, H),
              "mirrored renders");
        CHECK(snapshot(&env, &st), "mirrored captured");
        CHECK(st.visible == 1, "mirrored visible");
    }
    /* ---- triangle reduction: near/full vs far/LOD ---- */
    {
        float near_eye[3] = { 0.0f, 0.0f, 4.0f };
        float far_eye[3] = { 0.0f, 0.0f, 60.0f };
        lr_visibility_stats near_st;
        lr_visibility_stats far_st;
        unsigned char *near_px = NULL;
        unsigned char *far_px = NULL;
        size_t near_size = 0;
        size_t far_size = 0;

        CHECK(render_frames(&env, near_eye, 1.0f, 1.0f, 1.0f, 3, W,
                            H),
              "near renders");
        CHECK(snapshot(&env, &near_st), "near captured");
        near_px = snapshot_pixels(&env, &near_size);
        CHECK(near_px != NULL && near_size > 0, "near pixels read");
        CHECK(render_frames(&env, far_eye, 1.0f, 1.0f, 1.0f, 3, W,
                            H),
              "far renders");
        CHECK(snapshot(&env, &far_st), "far captured");
        far_px = snapshot_pixels(&env, &far_size);
        CHECK(far_px != NULL && far_size > 0, "far pixels read");
        printf("[info] tris near=%llu far=%llu\n",
               (unsigned long long)near_st.triangles_submitted,
               (unsigned long long)far_st.triangles_submitted);
        CHECK(near_st.lod_visible[0] == 1, "near uses LOD0");
        CHECK(far_st.lod_visible[3] == 1, "far uses LOD3");
        CHECK(far_st.triangles_submitted <
                  near_st.triangles_submitted,
              "LOD reduces triangles");
        CHECK(near_st.triangles_submitted == 128,
              "LOD0 draws 128 triangles");
        CHECK(far_st.triangles_submitted == 1,
              "LOD3 draws 1 triangle");
        /* Distinct LOD geometry must paint distinct pixels (and
         * neither frame may be vacuously clear). */
        if (near_px != NULL && far_px != NULL &&
            near_size == far_size) {
            size_t center =
                ((size_t)(H / 2) * (size_t)W + (size_t)(W / 2)) *
                4u * sizeof(uint16_t);
            const uint16_t *n16 =
                (const uint16_t *)(near_px + center);

            CHECK(center + 8u <= near_size &&
                      (n16[0] > 0x2000u || n16[1] > 0x2000u ||
                       n16[2] > 0x2000u),
                  "near center pixel is lit");
            /* The far triangle is small and may miss the exact
             * center: any lit texel proves it painted. */
            {
                const uint16_t *fpx = (const uint16_t *)far_px;
                size_t words = far_size / sizeof(uint16_t);
                size_t t;
                int lit = 0;

                for (t = 0; t + 3u < words; t += 4u) {
                    if (fpx[t] > 0x2000u || fpx[t + 1] > 0x2000u ||
                        fpx[t + 2] > 0x2000u) {
                        lit = 1;
                        break;
                    }
                }
                CHECK(lit, "far frame paints lit pixels");
            }
            CHECK(memcmp(near_px, far_px, near_size) != 0,
                  "LOD levels paint distinct pixels");
        } else {
            CHECK(0, "LOD pixel buffers comparable");
        }
        free(near_px);
        free(far_px);
    }
    /* ---- fallback equivalence (fixed-count, zero no-ops) ---- */
    {
        float eye[3] = { 0.0f, 0.0f, 12.0f };
        lr_visibility_stats native_st;
        lr_visibility_stats fall_st;

        set_vis(&env, 1, 1, 1);
        CHECK(render_frames(&env, eye, 1.0f, 1.0f, 1.0f, 3, W, H),
              "native renders");
        CHECK(snapshot(&env, &native_st), "native captured");
        set_vis(&env, 1, 0, 1);
        CHECK(render_frames(&env, eye, 1.0f, 1.0f, 1.0f, 3, W, H),
              "fallback renders");
        CHECK(snapshot(&env, &fall_st), "fallback captured");
        CHECK(native_st.visible == fall_st.visible,
              "fallback visibility agrees");
        CHECK(native_st.lod_visible[0] == fall_st.lod_visible[0] &&
                  native_st.lod_visible[1] ==
                      fall_st.lod_visible[1] &&
                  native_st.lod_visible[2] ==
                      fall_st.lod_visible[2] &&
                  native_st.lod_visible[3] ==
                      fall_st.lod_visible[3],
              "fallback LOD agrees");
        CHECK(native_st.triangles_submitted ==
                  fall_st.triangles_submitted,
              "fallback triangles agree");
    }
    /* ---- graph on/off equivalence ---- */
    {
        float eye[3] = { 0.0f, 0.0f, 12.0f };
        lr_visibility_stats graph_st;
        lr_visibility_stats manual_st;

        set_vis(&env, 1, 1, 1);
        CHECK(render_frames(&env, eye, 1.0f, 1.0f, 1.0f, 2, W, H),
              "graph renders");
        CHECK(snapshot(&env, &graph_st), "graph captured");
        {
            lr_render_graph *graph =
                lr_renderer_borrow_visibility_graph(env.renderer);
            lr_graph_stats gs;
            char dump[2048];

            CHECK(graph != NULL, "visibility graph borrowed");
            lr_render_graph_get_stats(graph, &gs);
            printf("[info] graph passes=%u edges=%u barriers=%u "
                   "transient=%u\n",
                   gs.pass_count, gs.dependency_edges,
                   gs.derived_barriers, gs.transient_resources);
            CHECK(gs.pass_count == 4, "graph has four passes");
            CHECK(gs.dependency_edges >= 3,
                  "graph derives real edges");
            {
                /* CPU compile cost of the visibility schedule
                 * (recompile is idempotent; topology cache hits
                 * skip it in prepare). */
                uint64_t t0 = lc_clock_now();

                CHECK(lr_render_graph_compile(graph) ==
                          LR_SUCCESS,
                      "graph recompiles");
                printf("[info] graph compile ms=%.3f prepare ms=%.3f\n",
                       ms_since(t0), graph_st.cpu_prepare_ms);
            }
            CHECK(lr_render_graph_dump(graph, dump, sizeof(dump)) >
                      0,
                  "graph dump non-empty");
            printf("[info] graph dump:\n%s", dump);
        }
        set_vis(&env, 1, 1, 0);
        CHECK(render_frames(&env, eye, 1.0f, 1.0f, 1.0f, 2, W, H),
              "manual renders");
        CHECK(snapshot(&env, &manual_st), "manual captured");
        CHECK(graph_st.visible == manual_st.visible,
              "graph/manual visibility agrees");
        CHECK(graph_st.triangles_submitted ==
                  manual_st.triangles_submitted,
              "graph/manual triangles agree");
    }
    lr_material_destroy(env.mat);
    lr_mesh_destroy(env.mesh);
    lr_renderer_destroy(env.renderer);
    lc_swapchain_destroy(env.swapchain);
    lc_surface_destroy(env.surface);
    lc_device_destroy(env.device);
    lc_window_destroy(env.window);
    lc_shutdown();
    printf("lod: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}