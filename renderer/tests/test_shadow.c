/*
 * Luma shadow headless test (Phase 16, PART Z).
 *
 * No GPU: light-space math goldens (matrix inverse, ortho mapping,
 * frustum corners + roundtrip, directional fit + texel-snap
 * properties, spot projection, depth mapping) plus NULL-path
 * validation. GPU pixels live in test_shadow_vulkan.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <luma_renderer/luma_renderer.h>

#include "internal/renderer_internal.h"

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

static int feq(float a, float b, float tol) {
    return fabsf(a - b) <= tol;
}

static void test_inverse(void) {
    /* M = T(3,-2,5) * Ry(30deg) * S(2,0.5,1.5), column-major. */
    static const float m[16] = {
        1.7320508f, 0.0f, -1.0f, 0.0f,
        0.0f, 0.5f, 0.0f, 0.0f,
        0.75f, 0.0f, 1.2990381f, 0.0f,
        3.0f, -2.0f, 5.0f, 1.0f
    };
    static const float want[16] = {
        0.4330127f, 0.0f, 0.3333333f, 0.0f,
        0.0f, 2.0f, 0.0f, 0.0f,
        -0.25f, 0.0f, 0.5773503f, 0.0f,
        -0.0490381f, 4.0f, -3.8867513f, 1.0f
    };
    float got[16];
    int i;
    int ok;

    TEST_CHECK(lr_mat4_inverse(m, got) != 0, "inverse: accepted");
    ok = 1;
    for (i = 0; i < 16; i++) {
        if (!feq(got[i], want[i], 1e-5f)) {
            ok = 0;
        }
    }
    TEST_CHECK(ok, "inverse: matches independent computation");
    /* Self-consistency: M * inv = I. */
    {
        float p[16];
        int r;
        int c;

        for (r = 0; r < 4; r++) {
            for (c = 0; c < 4; c++) {
                float s = 0.0f;
                int k;

                for (k = 0; k < 4; k++) {
                    s += m[k * 4 + r] * got[c * 4 + k];
                }
                p[c * 4 + r] = s;
            }
        }
        ok = 1;
        for (i = 0; i < 16; i++) {
            float e = (i == 0 || i == 5 || i == 10 || i == 15) ? 1.0f : 0.0f;

            if (!feq(p[i], e, 1e-5f)) {
                ok = 0;
            }
        }
        TEST_CHECK(ok, "inverse: M * inv = I");
    }
    {
        static const float singular[16] = { 0.0f, 0.0f, 0.0f, 0.0f,
                                            0.0f, 1.0f, 0.0f, 0.0f,
                                            0.0f, 0.0f, 1.0f, 0.0f,
                                            0.0f, 0.0f, 0.0f, 1.0f };
        float out[16];

        TEST_CHECK(lr_mat4_inverse(singular, out) == 0,
                   "inverse: singular rejected");
        TEST_CHECK(lr_mat4_inverse(NULL, out) == 0 &&
                   lr_mat4_inverse(m, NULL) == 0,
                   "inverse: NULL rejected");
    }
}

static void test_ortho(void) {
    /* Flipped-Y ortho (matches perspective sign conventions):
     * left->-1, right->+1, bottom->+1, top->-1, near->0, far->1. */
    float m[16];
    float v[4];
    float o[4];

    lr_mat4_ortho(m, -24.940833f, 4.825383f, -24.308015f, 3.977675f,
                  0.1f, 35.48954f);
#define MAP_PT(x, y, z)                                                  \
    do {                                                                 \
        v[0] = (x);                                                      \
        v[1] = (y);                                                      \
        v[2] = (z);                                                      \
        v[3] = 1.0f;                                                     \
        o[0] = m[0] * v[0] + m[4] * v[1] + m[8] * v[2] + m[12] * v[3];   \
        o[1] = m[1] * v[0] + m[5] * v[1] + m[9] * v[2] + m[13] * v[3];   \
        o[2] = m[2] * v[0] + m[6] * v[1] + m[10] * v[2] + m[14] * v[3];  \
        o[3] = m[3] * v[0] + m[7] * v[1] + m[11] * v[2] + m[15] * v[3];  \
    } while (0)
    MAP_PT(-24.940833f, -24.308015f, -0.1f);
    TEST_CHECK(feq(o[0] / o[3], -1.0f, 1e-5f) &&
               feq(o[1] / o[3], 1.0f, 1e-5f) &&
               feq(o[2] / o[3], 0.0f, 1e-5f),
               "ortho: (left,bottom,near) maps to (-1,+1,0)");
    MAP_PT(4.825383f, 3.977675f, -35.48954f);
    TEST_CHECK(feq(o[0] / o[3], 1.0f, 1e-5f) &&
               feq(o[1] / o[3], -1.0f, 1e-5f) &&
               feq(o[2] / o[3], 1.0f, 1e-5f),
               "ortho: (right,top,far) maps to (+1,-1,1)");
#undef MAP_PT
    lr_mat4_ortho(NULL, 0.0f, 1.0f, 0.0f, 1.0f, 0.1f, 2.0f);
    TEST_CHECK(1, "ortho: NULL matrix is a no-op");
}

static void camera_fixture(lr_camera *camera) {
    static const float eye[3] = { 0.0f, 2.0f, 6.0f };
    static const float center[3] = { 0.0f, 0.5f, 0.0f };
    static const float up[3] = { 0.0f, 1.0f, 0.0f };

    lr_camera_init(camera);
    lr_camera_set_perspective(camera, 0.7853982f, 4.0f / 3.0f, 0.1f,
                              100.0f);
    lr_camera_look_at(camera, eye, center, up);
}

static void test_corners(void) {
    /* Golden corners (independent computation) for the fixture
     * camera at NDC depths 0 and z_slice=0.9969970. */
    static const float want[8][3] = {
        { -0.05523f, 2.01593f, 5.89294f },
        { 0.05523f, 2.01593f, 5.89294f },
        { -0.05523f, 1.93556f, 5.91303f },
        { 0.05523f, 1.93556f, 5.91303f },
        { -13.80712f, 5.98276f, -20.76510f },
        { 13.80712f, 5.98276f, -20.76510f },
        { -13.80712f, -14.10955f, -15.74202f },
        { 13.80712f, -14.10955f, -15.74202f }
    };
    lr_camera camera;
    float vp[16];
    float corners[8][3];
    int i;
    int ok;

    camera_fixture(&camera);
    lr_mat4_multiply(vp, camera.projection, camera.view);
    lr_shadow_frustum_corners(vp, 0.0f, 0.9969970f, corners);
    ok = 1;
    for (i = 0; i < 8; i++) {
        if (!feq(corners[i][0], want[i][0], 1e-3f) ||
            !feq(corners[i][1], want[i][1], 1e-3f) ||
            !feq(corners[i][2], want[i][2], 1e-3f)) {
            ok = 0;
        }
    }
    TEST_CHECK(ok, "corners: match independent computation");
    /* Roundtrip: every corner reprojects to its NDC address. */
    ok = 1;
    for (i = 0; i < 8; i++) {
        float x = (i & 1) ? 1.0f : -1.0f;
        float y = (i & 2) ? 1.0f : -1.0f;
        float z = (i & 4) ? 0.9969970f : 0.0f;
        float p[4];
        float w;

        p[0] = vp[0] * corners[i][0] + vp[4] * corners[i][1] +
               vp[8] * corners[i][2] + vp[12];
        p[1] = vp[1] * corners[i][0] + vp[5] * corners[i][1] +
               vp[9] * corners[i][2] + vp[13];
        p[2] = vp[2] * corners[i][0] + vp[6] * corners[i][1] +
               vp[10] * corners[i][2] + vp[14];
        p[3] = vp[3] * corners[i][0] + vp[7] * corners[i][1] +
               vp[11] * corners[i][2] + vp[15];
        w = p[3];
        if (w == 0.0f || !feq(p[0] / w, x, 1e-4f) ||
            !feq(p[1] / w, y, 1e-4f) || !feq(p[2] / w, z, 1e-3f)) {
            ok = 0;
        }
    }
    TEST_CHECK(ok, "corners: reproject to their NDC addresses");
    {
        /* Singular VP zeroes output (no NaNs downstream). */
        static const float zero_vp[16] = { 0 };
        float zc[8][3];
        int k;
        int allzero = 1;

        lr_shadow_frustum_corners(zero_vp, 0.0f, 1.0f, zc);
        for (k = 0; k < 8; k++) {
            if (zc[k][0] != 0.0f || zc[k][1] != 0.0f ||
                zc[k][2] != 0.0f) {
                allzero = 0;
            }
        }
        TEST_CHECK(allzero, "corners: singular VP zeroes output");
    }
    lr_shadow_frustum_corners(NULL, 0.0f, 1.0f, corners);
    lr_shadow_frustum_corners(vp, 0.0f, 1.0f, NULL);
    TEST_CHECK(1, "corners: NULL args are no-ops");
}

static void fixture_corners(float corners[8][3]) {
    static const float want[8][3] = {
        { -0.05523f, 2.01593f, 5.89294f },
        { 0.05523f, 2.01593f, 5.89294f },
        { -0.05523f, 1.93556f, 5.91303f },
        { 0.05523f, 1.93556f, 5.91303f },
        { -13.80712f, 5.98276f, -20.76510f },
        { 13.80712f, 5.98276f, -20.76510f },
        { -13.80712f, -14.10955f, -15.74202f },
        { 13.80712f, -14.10955f, -15.74202f }
    };

    memcpy(corners, want, sizeof(want));
}

static void test_fit(void) {    /* Travel direction is intentionally UNNORMALIZED here (the fit
     * normalizes defensively, like submit does). */
    static const float dir[3] = { 0.35f, -1.0f, 0.25f };
    static const float want_bounds[6] = { -14.883108f, 14.883108f,
                                          -14.142845f, 14.142845f,
                                          0.1f, 35.48954f };
    static const float want_eye[3] = { -8.09371f, 14.08695f,
                                       -18.11865f };
    float corners[8][3];
    float bounds[6];
    float eye[3];
    float view[16];
    int i;
    int ok;

    fixture_corners(corners);
    lr_shadow_fit_directional(corners, dir, bounds, eye, view);
    ok = 1;
    for (i = 0; i < 6; i++) {
        if (!feq(bounds[i], want_bounds[i], 1e-2f)) {
            ok = 0;
        }
    }
    TEST_CHECK(ok, "fit: bounds match independent computation");
    ok = 1;
    for (i = 0; i < 3; i++) {
        if (!feq(eye[i], want_eye[i], 1e-2f)) {
            ok = 0;
        }
    }
    TEST_CHECK(ok, "fit: eye matches independent computation");
    /* Centered by construction (the stability mechanism). */
    TEST_CHECK(feq(bounds[0] + bounds[1], 0.0f, 1e-3f) &&
               feq(bounds[2] + bounds[3], 0.0f, 1e-3f),
               "fit: bounds are eye-frame centered");
    /* Self-consistency: every corner lands inside the bounds under
     * the fitted view. */
    ok = 1;
    for (i = 0; i < 8; i++) {
        float lx = view[0] * corners[i][0] + view[4] * corners[i][1] +
                   view[8] * corners[i][2] + view[12];
        float ly = view[1] * corners[i][0] + view[5] * corners[i][1] +
                   view[9] * corners[i][2] + view[13];
        float lz = view[2] * corners[i][0] + view[6] * corners[i][1] +
                   view[10] * corners[i][2] + view[14];

        if (lx < bounds[0] - 1e-2f || lx > bounds[1] + 1e-2f ||
            ly < bounds[2] - 1e-2f || ly > bounds[3] + 1e-2f) {
            ok = 0;
        }
        (void)lz;
    }
    TEST_CHECK(ok, "fit: fitted view contains all corners");
    lr_shadow_fit_directional(NULL, dir, bounds, eye, view);
    lr_shadow_fit_directional(corners, NULL, bounds, eye, view);
    lr_shadow_fit_directional(corners, dir, NULL, eye, view);
    TEST_CHECK(1, "fit: NULL args are no-ops");
}

/* Degenerate-up branch (|travel.y| > 0.99 takes +X as up):
 * golden values plus the corners-inside self-check. */
static void test_fit_degenerate(void) {
    static const float dir[3] = { 0.0f, -1.0f, 0.0f };
    static const float want_bounds[6] = { -13.339067f, 13.339067f,
                                          -13.807119f, 13.807119f,
                                          0.1f, 27.092310f };
    static const float want_eye[3] = { 0.0f, 10.98276f, -7.42603f };
    float corners[8][3];
    float bounds[6];
    float eye[3];
    float view[16];
    int i;
    int ok;

    fixture_corners(corners);
    lr_shadow_fit_directional(corners, dir, bounds, eye, view);
    ok = 1;
    for (i = 0; i < 6; i++) {
        if (!feq(bounds[i], want_bounds[i], 1e-2f)) {
            ok = 0;
        }
    }
    TEST_CHECK(ok, "fit-degenerate: bounds match");
    ok = 1;
    for (i = 0; i < 3; i++) {
        if (!feq(eye[i], want_eye[i], 1e-2f)) {
            ok = 0;
        }
    }
    TEST_CHECK(ok, "fit-degenerate: eye matches");
    ok = 1;
    for (i = 0; i < 8; i++) {
        float lx = view[0] * corners[i][0] + view[4] * corners[i][1] +
                   view[8] * corners[i][2] + view[12];
        float ly = view[1] * corners[i][0] + view[5] * corners[i][1] +
                   view[9] * corners[i][2] + view[13];

        if (lx < bounds[0] - 1e-2f || lx > bounds[1] + 1e-2f ||
            ly < bounds[2] - 1e-2f || ly > bounds[3] + 1e-2f) {
            ok = 0;
        }
    }
    TEST_CHECK(ok, "fit-degenerate: view contains corners");
}

/* Stability (PART J): the centered fit is translation-invariant
 * by construction (rigid slice motion preserves AABB extents), so
 * sub-texel camera drift cannot crawl the projection. Rotation
 * genuinely refits (sensitivity control: the test is not vacuous). */
static void test_stability(void) {
    static const float dir[3] = { 0.35f, -1.0f, 0.25f };
    float corners[8][3];
    float b0[6];
    float eye[3];
    float view[16];
    float texel;
    int i;
    int ok;

    fixture_corners(corners);
    lr_shadow_fit_directional(corners, dir, b0, eye, view);
    texel = (b0[1] - b0[0]) / 1024.0f;
    /* Sub-texel world shift: bounds hold to float noise (a naive
     * camera-centered fit would move ~0.3 texels = 100x more). */
    {
        float moved[8][3];
        float b1[6];
        float e1[3];
        float v1[16];
        int k;

        for (k = 0; k < 8; k++) {
            moved[k][0] = corners[k][0] + 0.3f * texel;
            moved[k][1] = corners[k][1] - 0.2f * texel;
            moved[k][2] = corners[k][2] + 0.1f;
        }
        lr_shadow_fit_directional(moved, dir, b1, e1, v1);
        ok = 1;
        for (i = 0; i < 4; i++) {
            if (!feq(b1[i], b0[i], 1e-4f)) {
                ok = 0;
            }
        }
        TEST_CHECK(ok, "stability: sub-texel drift holds bounds");
    }
    /* Rotation refits (genuine coverage change is not frozen). */
    {
        lr_camera camera;
        float vp[16];
        float rcorners[8][3];
        float b1[6];
        float e1[3];
        float v1[16];
        static const float eye2[3] = { 2.0f, 2.0f, 6.0f };
        static const float center[3] = { 0.0f, 0.5f, 0.0f };
        static const float up[3] = { 0.0f, 1.0f, 0.0f };
        float moved = 0.0f;

        lr_camera_init(&camera);
        lr_camera_set_perspective(&camera, 0.7853982f, 4.0f / 3.0f, 0.1f,
                                  100.0f);
        lr_camera_look_at(&camera, eye2, center, up);
        lr_mat4_multiply(vp, camera.projection, camera.view);
        lr_shadow_frustum_corners(vp, 0.0f, 0.9969970f, rcorners);
        lr_shadow_fit_directional(rcorners, dir, b1, e1, v1);
        for (i = 0; i < 4; i++) {
            float d = b1[i] - b0[i];

            if (d < 0.0f) {
                d = -d;
            }
            if (d > moved) {
                moved = d;
            }
        }
        TEST_CHECK(moved > 1e-2f, "stability: rotation refits bounds");
    }
}

static void test_spot(void) {
    static const float pos[3] = { 2.5f, 2.5f, 2.0f };
    static const float tgt[3] = { 0.0f, 0.8f, 0.0f };
    float dir[3];
    float sub[3];
    float len;
    float view[16];
    float proj[16];
    float vp[16];
    float p[4];
    float w;

    sub[0] = tgt[0] - pos[0];
    sub[1] = tgt[1] - pos[1];
    sub[2] = tgt[2] - pos[2];
    len = sqrtf(sub[0] * sub[0] + sub[1] * sub[1] + sub[2] * sub[2]);
    dir[0] = sub[0] / len;
    dir[1] = sub[1] / len;
    dir[2] = sub[2] / len;
    /* Golden travel direction (independent computation). */
    TEST_CHECK(feq(dir[0], -0.689672f, 1e-5f) &&
               feq(dir[1], -0.468977f, 1e-5f) &&
               feq(dir[2], -0.551737f, 1e-5f),
               "spot: travel direction matches");
    {
        float look_tgt[3] = { pos[0] + dir[0], pos[1] + dir[1],
                              pos[2] + dir[2] };

        lr_shadow_light_view(pos, look_tgt, view);
    }
    lr_mat4_perspective(proj, 0.5f * 2.0f, 1.0f, 0.5f, 12.0f);
    lr_mat4_multiply(vp, proj, view);
    p[0] = vp[0] * tgt[0] + vp[4] * tgt[1] + vp[8] * tgt[2] + vp[12];
    p[1] = vp[1] * tgt[0] + vp[5] * tgt[1] + vp[9] * tgt[2] + vp[13];
    p[2] = vp[2] * tgt[0] + vp[6] * tgt[1] + vp[10] * tgt[2] + vp[14];
    p[3] = vp[3] * tgt[0] + vp[7] * tgt[1] + vp[11] * tgt[2] + vp[15];
    w = p[3];
    TEST_CHECK(w > 0.0f && feq(p[0] / w, 0.0f, 1e-4f) &&
               feq(p[1] / w, 0.0f, 1e-4f) &&
               feq(p[2] / w, 0.899547f, 1e-4f),
               "spot: aim target projects to NDC center");
    {
        /* Straight-down spot (degenerate-up rule keeps working). */
        static const float dpos[3] = { 0.0f, 5.0f, 0.0f };
        static const float dtgt[3] = { 0.0f, 0.0f, 0.0f };
        float dview[16];
        float dp[4];
        float dw;

        lr_shadow_light_view(dpos, dtgt, dview);
        dp[0] = dview[0] * 0.0f + dview[4] * 2.5f + dview[8] * 0.0f +
                dview[12];
        dp[1] = dview[1] * 0.0f + dview[5] * 2.5f + dview[9] * 0.0f +
                dview[13];
        dp[2] = dview[2] * 0.0f + dview[6] * 2.5f + dview[10] * 0.0f +
                dview[14];
        dp[3] = dview[3] * 0.0f + dview[7] * 2.5f + dview[11] * 0.0f +
                dview[15];
        dw = dp[3];
        TEST_CHECK(dw > 0.0f && feq(dp[0] / dw, 0.0f, 1e-4f) &&
                   feq(dp[2] / dw, -2.5f, 1e-4f),
                   "spot: straight-down view stays valid");
    }
    lr_shadow_light_view(NULL, tgt, view);
    lr_shadow_light_view(pos, NULL, view);
    lr_shadow_light_view(pos, tgt, NULL);
    TEST_CHECK(1, "spot: NULL view args are no-ops");
}

static void test_api(void) {
    TEST_CHECK(LR_MAX_SHADOWS == 4u, "shadows: 4 slots");
    TEST_CHECK(LR_SHADOW_RESOLUTION_DEFAULT == 1024u,
               "shadows: 1024 default resolution");
    TEST_CHECK(lr_renderer_submit_light(NULL, NULL) ==
                   LR_ERROR_INVALID_ARGUMENT,
               "shadows: NULL submit rejected");
    TEST_CHECK(lr_renderer_render_shadows(NULL, NULL) ==
                   LR_ERROR_INVALID_ARGUMENT,
               "shadows: NULL prepare rejected");
    TEST_CHECK(lr_renderer_get_shadow_count(NULL) == 0,
               "shadows: NULL count is 0");
    TEST_CHECK(lr_renderer_get_light_count(NULL) == 0,
               "shadows: NULL light count is 0");
    {
        lr_shadow_slot_info info;

        lr_renderer_get_shadow_slot_info(NULL, 0, &info);
        TEST_CHECK(info.resolution == 0 && info.format == 0,
                   "shadows: NULL slot info zeroes");
        lr_renderer_get_shadow_slot_info(NULL, 0, NULL);
        TEST_CHECK(lr_renderer_get_shadow_view(NULL, 0) == NULL,
                   "shadows: NULL view is NULL");
    }
    {
        lr_light light;
        lr_shadow_desc shadow;

        lr_renderer_get_light(NULL, 0, &light, &shadow);
        TEST_CHECK(light.type == 0 && shadow.resolution == 0,
                   "shadows: NULL light get zeroes");
        lr_renderer_get_light(NULL, 0, NULL, NULL);
        TEST_CHECK(1, "shadows: NULL/NULL light get is a no-op");
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_inverse();
    test_ortho();
    test_corners();
    test_fit();
    test_fit_degenerate();
    test_stability();
    test_spot();
    test_api();
    printf("shadow headless: %d passed, %d failed\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
