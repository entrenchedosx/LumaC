/*
 * Luma PBR headless test (Phase 15, PART V).
 *
 * No GPU: CPU reference of the exact shader BRDF equations plus
 * anchor checks (catches algebra/sign mistakes independently of any
 * render), normal-matrix vectors (verified against an independent
 * computation), and NULL-path validation of the new APIs. Pixels
 * live in test_pbr_vulkan.c.
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

#ifndef LR_TEST_PI
#define LR_TEST_PI 3.14159265358979323846
#endif
#define LR_TEST_MIN_ROUGH 0.05
#define LR_TEST_F0 0.04

/* ------------------------------------------------------------------
 * CPU reference: the pbr.frag equations in double precision.
 * ------------------------------------------------------------------ */

static double test_clamp(double x, double lo, double hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static double test_dot3(const double a[3], const double b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* NDF only (for the D anchor). */
static double test_ggx_d(double roughness, double n_dot_h) {
    double r = test_clamp(roughness, LR_TEST_MIN_ROUGH, 1.0);
    double a = r * r;
    double a2 = a * a;
    double denom = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;

    return a2 / (LR_TEST_PI * denom * denom);
}

/* Geometry only (for the G anchor). */
static double test_smith_g(double roughness, double n_dot_v,
                            double n_dot_l) {
    double r = test_clamp(roughness, LR_TEST_MIN_ROUGH, 1.0);
    double k = (r + 1.0) * (r + 1.0) / 8.0;
    double g1v = n_dot_v / (n_dot_v * (1.0 - k) + k);
    double g1l = n_dot_l / (n_dot_l * (1.0 - k) + k);

    return g1v * g1l;
}

/* Fresnel only (for the F anchor). */
static void test_schlick_f(const double f0[3], double v_dot_h,
                            double out[3]) {
    double t = pow(1.0 - v_dot_h, 5.0);
    int i;

    for (i = 0; i < 3; i++) {
        out[i] = f0[i] + (1.0 - f0[i]) * t;
    }
}

/* Full direct-lighting evaluation for one light (mirrors main()). */
static void test_brdf_eval(const double n[3], const double l[3],
                            const double v[3], const double albedo[3],
                            double metallic, double roughness,
                            const double light_color[3], double intensity,
                            double attenuation, double ambient[3],
                            double occlusion, const double emissive[3],
                            double out[3]) {
    double r = test_clamp(roughness, LR_TEST_MIN_ROUGH, 1.0);
    double f0[3];
    double a;
    double a2;
    double k;
    double n_dot_l;
    double n_dot_v;
    double h[3];
    double hl;
    double n_dot_h;
    double v_dot_h;
    double denom;
    double d;
    double g;
    double f[3];
    double spec[3];
    double kd[3];
    int i;

    for (i = 0; i < 3; i++) {
        f0[i] = LR_TEST_F0 + (albedo[i] - LR_TEST_F0) * metallic;
    }
    a = r * r;
    a2 = a * a;
    k = (r + 1.0) * (r + 1.0) / 8.0;
    n_dot_l = test_clamp(test_dot3(n, l), 0.0, 1.0);
    n_dot_v = test_clamp(test_dot3(n, v), 0.0, 1.0);
    for (i = 0; i < 3; i++) {
        out[i] = albedo[i] * (1.0 - metallic) * ambient[i] * occlusion +
                 emissive[i];
    }
    if (n_dot_l <= 0.0) {
        return; /* mirrors the shader early-out (h degenerates) */
    }
    for (i = 0; i < 3; i++) {
        h[i] = l[i] + v[i];
    }
    hl = sqrt(test_dot3(h, h));
    for (i = 0; i < 3; i++) {
        h[i] /= hl;
    }
    n_dot_h = test_clamp(test_dot3(n, h), 0.0, 1.0);
    v_dot_h = test_clamp(test_dot3(v, h), 0.0, 1.0);
    denom = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    d = a2 / (LR_TEST_PI * denom * denom);
    g = test_smith_g(r, n_dot_v, n_dot_l);
    test_schlick_f(f0, v_dot_h, f);
    for (i = 0; i < 3; i++) {
        double denom_spec = 4.0 * n_dot_v * n_dot_l;

        if (denom_spec < 1e-4) {
            denom_spec = 1e-4;
        }
        spec[i] = d * g * f[i] / denom_spec;
        kd[i] = (1.0 - f[i]) * (1.0 - metallic);
        out[i] += (kd[i] * albedo[i] / LR_TEST_PI + spec[i]) * n_dot_l *
                  attenuation * light_color[i] * intensity;
    }
}

static int deq(double a, double b, double tol) {
    return fabs(a - b) <= tol;
}

static void test_brdf_anchors(void) {
    /* D(rough=1, NdotH=1) = 1/pi. */
    TEST_CHECK(deq(test_ggx_d(1.0, 1.0), 1.0 / LR_TEST_PI, 1e-12),
               "brdf: D(1, 1) = 1/pi");
    /* D floor: roughness 0 clamps to 0.05. */
    TEST_CHECK(deq(test_ggx_d(0.0, 1.0), test_ggx_d(0.05, 1.0), 0.0),
               "brdf: roughness clamps at 0.05");
    /* G(1, 1) = 1 for any roughness. */
    TEST_CHECK(deq(test_smith_g(0.13, 1.0, 1.0), 1.0, 1e-12) &&
               deq(test_smith_g(1.0, 1.0, 1.0), 1.0, 1e-12),
               "brdf: G(1, 1) = 1");
    /* G(0, *) = 0 (grazing shuts down). */
    TEST_CHECK(deq(test_smith_g(0.5, 0.0, 1.0), 0.0, 1e-12),
               "brdf: G(0, L) = 0");
    /* F(VdotH=1) = F0; F(VdotH=0) = 1. */
    {
        double f0[3] = { 0.04, 0.04, 0.04 };
        double f[3];

        test_schlick_f(f0, 1.0, f);
        TEST_CHECK(deq(f[0], 0.04, 1e-12) && deq(f[2], 0.04, 1e-12),
                   "brdf: F(1) = F0");
        test_schlick_f(f0, 0.0, f);
        TEST_CHECK(deq(f[0], 1.0, 1e-12) && deq(f[1], 1.0, 1e-12),
                   "brdf: F(0) = 1");
    }
    /* Full eval, facing dielectric: hand-derived structure. */
    {
        double n[3] = { 0.0, 0.0, 1.0 };
        double l[3] = { 0.0, 0.0, 1.0 };
        double v[3] = { 0.0, 0.0, 1.0 };
        double albedo[3] = { 0.5, 0.5, 0.5 };
        double light[3] = { 1.0, 1.0, 1.0 };
        double amb[3] = { 0.0, 0.0, 0.0 };
        double em[3] = { 0.0, 0.0, 0.0 };
        double out[3];
        /* rough=1: D=1/pi, G=1, F=F0=lerp(0.04,0.5,0)=0.04;
         * spec = (1/pi)(0.04)/4 = 0.01/pi; kD=(0.96)(1)=0.96;
         * diff = 0.96*0.5/pi = 0.48/pi; total/1 = 0.49/pi. */
        double want = 0.49 / LR_TEST_PI;

        test_brdf_eval(n, l, v, albedo, 0.0, 1.0, light, 1.0, 1.0, amb,
                       1.0, em, out);
        TEST_CHECK(deq(out[0], want, 1e-9) && deq(out[1], want, 1e-9) &&
                   deq(out[2], want, 1e-9),
                   "brdf: facing dielectric rough=1 matches hand calc");
    }
    /* Metal kills diffuse: with NdotL=0 only ambient+emissive stay. */
    {
        double n[3] = { 0.0, 0.0, 1.0 };
        double l[3] = { 0.0, 0.0, -1.0 };
        double v[3] = { 0.0, 0.0, 1.0 };
        double albedo[3] = { 0.9, 0.8, 0.7 };
        double light[3] = { 1.0, 1.0, 1.0 };
        double amb[3] = { 0.03, 0.03, 0.03 };
        double em[3] = { 0.1, 0.2, 0.3 };
        double out[3];

        test_brdf_eval(n, l, v, albedo, 1.0, 0.4, light, 3.0, 1.0, amb,
                       1.0, em, out);
        TEST_CHECK(deq(out[0], em[0], 1e-12) &&
                   deq(out[1], em[1], 1e-12) && deq(out[2], em[2], 1e-12),
                   "brdf: backface metal = emissive only");
    }
    /* Energy: facing white dielectric, rough=1, unit light. */
    {
        double n[3] = { 0.0, 0.0, 1.0 };
        double l[3] = { 0.0, 0.0, 1.0 };
        double v[3] = { 0.0, 0.0, 1.0 };
        double albedo[3] = { 1.0, 1.0, 1.0 };
        double light[3] = { 1.0, 1.0, 1.0 };
        double amb[3] = { 0.0, 0.0, 0.0 };
        double em[3] = { 0.0, 0.0, 0.0 };
        double out[3];

        test_brdf_eval(n, l, v, albedo, 0.0, 1.0, light, 1.0, 1.0, amb,
                       1.0, em, out);
        TEST_CHECK(out[0] < 1.0 && out[0] > 0.0,
                   "brdf: dielectric conserves energy");
    }
}

static void test_normal_matrix(void) {
    /* M = Ry(30deg) * diag(2, 0.5, 1.5), column-major. Expected
     * columns computed independently (see Phase 15 notes). */
    static const float m[16] = {
        1.7320508f, 0.0f, -1.0f, 0.0f,  /* col 0 */
        0.0f, 0.5f, 0.0f, 0.0f,         /* col 1 */
        0.75f, 0.0f, 1.2990381f, 0.0f,  /* col 2 */
        0.0f, 0.0f, 0.0f, 1.0f
    };
    static const float want[12] = {
        0.4330127f, 0.0f, -0.25f, 0.0f, /* col 0 */
        0.0f, 2.0f, 0.0f, 0.0f,         /* col 1 */
        0.3333333f, 0.0f, 0.5773503f, 0.0f /* col 2 */
    };
    float got[12];
    int i;
    int ok;

    TEST_CHECK(lr_mat3_normal_from_mat4(m, got) != 0,
               "normalmat: non-singular accepted");
    ok = 1;
    for (i = 0; i < 12; i++) {
        if (fabsf(got[i] - want[i]) > 1e-5f) {
            ok = 0;
        }
    }
    TEST_CHECK(ok, "normalmat: matches independent computation");
    {
        /* Identity -> identity columns. */
        static const float ident[16] = { 1.0f, 0.0f, 0.0f, 0.0f,
                                         0.0f, 1.0f, 0.0f, 0.0f,
                                         0.0f, 0.0f, 1.0f, 0.0f,
                                         5.0f, -2.0f, 3.0f, 1.0f };
        float id[12];

        TEST_CHECK(lr_mat3_normal_from_mat4(ident, id) != 0 &&
                   fabsf(id[0] - 1.0f) < 1e-7f &&
                   fabsf(id[5] - 1.0f) < 1e-7f &&
                   fabsf(id[10] - 1.0f) < 1e-7f &&
                   fabsf(id[1]) < 1e-7f && fabsf(id[4]) < 1e-7f,
                   "normalmat: rigid transform is identity");
    }
    {
        /* Singular (zero scale) rejected. */
        static const float singular[16] = { 0.0f, 0.0f, 0.0f, 0.0f,
                                            0.0f, 1.0f, 0.0f, 0.0f,
                                            0.0f, 0.0f, 1.0f, 0.0f,
                                            0.0f, 0.0f, 0.0f, 1.0f };
        float out[12];

        TEST_CHECK(lr_mat3_normal_from_mat4(singular, out) == 0,
                   "normalmat: singular rejected");
    }
}

static void test_api_null_paths(void) {
    lr_pbr_material_info info;
    lr_material *dead = NULL;

    TEST_CHECK(LR_MAX_LIGHTS == 64u, "lights: capacity is 64");
    TEST_CHECK(lr_renderer_submit_light(NULL, NULL) ==
                   LR_ERROR_INVALID_ARGUMENT,
               "lights: NULL renderer rejected");
    TEST_CHECK(lr_material_create_pbr(NULL, NULL, &dead) ==
                   LR_ERROR_INVALID_ARGUMENT && dead == NULL,
               "material: NULL renderer rejected");
    TEST_CHECK(lr_material_create_unlit(NULL, NULL, &dead) ==
                   LR_ERROR_INVALID_ARGUMENT,
               "material: unlit NULL renderer rejected");
    lr_renderer_set_ambient(NULL, NULL);
    TEST_CHECK(1, "lights: NULL ambient set is a no-op");
    TEST_CHECK(lr_renderer_get_pipeline_count(NULL) == 0,
               "renderer: NULL pipeline count is 0");
    TEST_CHECK(lr_material_get_type(NULL) == LR_MATERIAL_UNKNOWN,
               "material: NULL type is UNKNOWN");
    lr_material_get_pbr_info(NULL, &info);
    TEST_CHECK(info.type == LR_MATERIAL_UNKNOWN,
               "material: NULL info is UNKNOWN");
    lr_material_get_pbr_info(NULL, NULL);
    TEST_CHECK(1, "material: NULL/NULL info is a no-op");
    lr_material_destroy(NULL);
    TEST_CHECK(1, "material: NULL destroy is safe");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_brdf_anchors();
    test_normal_matrix();
    test_api_null_paths();
    printf("pbr headless: %d passed, %d failed\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
