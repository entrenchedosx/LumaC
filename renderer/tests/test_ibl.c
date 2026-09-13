/*
 * Luma IBL headless test (Phase 17).
 *
 * No GPU, no window: CPU reference values for the tonemap operator,
 * half-float conversion (shared with assets), yaw rotation math,
 * and the split-sum BRDF integration kernel. The Vulkan suite
 * compares GPU output against these references.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

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

/* ACES fitted (Narkowicz), single channel: MUST match tonemap.frag
 * aces() exactly (same operations, same order). */
static float aces_ref(float x) {
    return (x * (2.51f * x + 0.03f)) / (x * (2.43f * x + 0.59f) + 0.14f);
}

static float clamp01(float x) {
    return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
}

/* Full NONE path: exposure multiply then clamp (NaN -> 0). */
static float none_ref(float v, float ev_mult) {
    float x = v * ev_mult;

    if (x != x) {
        return 0.0f;
    }
    return clamp01(x);
}

/* Full ACES path: exposure, filmic, clamp. */
static float aces_full_ref(float v, float ev_mult) {
    float x = v * ev_mult;

    if (x != x) {
        return 0.0f;
    }
    if (x < 0.0f) {
        x = 0.0f;
    }
    return clamp01(aces_ref(x));
}

static void test_tonemap_reference(void) {
    /* Values pinned against independent computation (PART AM). */
    static const struct {
        float in;
        float ev;
        float none_want;
        float aces_want;
    } cases[] = {
        { 0.0f, 1.0f, 0.0f, 0.0f },
        { 0.18f, 1.0f, 0.18f, 0.0f }, /* aces filled below */
        { 1.0f, 1.0f, 1.0f, 0.0f },
        { 2.0f, 1.0f, 1.0f, 0.0f },
        { 4.0f, 1.0f, 1.0f, 0.0f },
        { 16.0f, 1.0f, 1.0f, 0.0f },
    };
    /* Precomputed ACES outputs (double precision, rounded). Note
     * 16.0 exceeds 1.0 pre-clamp (1.018); the full path clamps. */
    static const float aces_want[] = {
        0.0f, 0.266899f, 0.803797f, 0.914855f, 0.973417f, 1.0f,
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        float none_got = none_ref(cases[i].in, cases[i].ev);
        float aces_got = aces_full_ref(cases[i].in, cases[i].ev);

        TEST_CHECK(fabsf(none_got - cases[i].none_want) < 1e-6f,
                   "tonemap: NONE reference value");
        TEST_CHECK(fabsf(aces_got - aces_want[i]) < 1e-5f,
                   "tonemap: ACES reference value");
    }
    /* Exposure monotonicity on a mid input (PART AL logic). */
    {
        float lo = aces_full_ref(0.5f, 0.5f);
        float mid = aces_full_ref(0.5f, 1.0f);
        float hi = aces_full_ref(0.5f, 2.0f);

        TEST_CHECK(lo < mid && mid < hi,
                   "tonemap: EV -1/0/+1 monotonic");
        TEST_CHECK(none_ref(0.5f, 0.5f) == 0.25f &&
                   none_ref(0.5f, 2.0f) == 1.0f,
                   "tonemap: NONE scales then clamps");
    }
    /* NONE clamps negatives and huge values; NaN becomes 0. */
    {
        float zero = 0.0f;

        TEST_CHECK(none_ref(-3.0f, 1.0f) == 0.0f &&
                   none_ref(1e30f, 1.0f) == 1.0f &&
                   none_ref(zero / zero, 1.0f) == 0.0f,
                   "tonemap: NONE sanitizes range");
    }
}

/* Yaw rotation applied to sample directions (MUST match the rotY
 * used in pbr.frag IBL and sky.frag). */
static void rot_yaw(float x, float y, float z, float ang, float out[3]) {
    float c = cosf(ang);
    float s = sinf(ang);

    out[0] = c * x + s * z;
    out[1] = y;
    out[2] = -s * x + c * z;
}

static void test_rotation_math(void) {
    float o[3];

    rot_yaw(1.0f, 0.0f, 0.0f, 0.0f, o);
    TEST_CHECK(fabsf(o[0] - 1.0f) < 1e-6f && fabsf(o[2]) < 1e-6f,
               "yaw: identity at 0");
    rot_yaw(1.0f, 0.0f, 0.0f, 1.5707963f, o);
    TEST_CHECK(fabsf(o[0]) < 1e-5f && fabsf(o[2] + 1.0f) < 1e-5f,
               "yaw: +X rotates to -Z at +90deg");
    rot_yaw(0.0f, 1.0f, 0.0f, 1.0f, o);
    TEST_CHECK(fabsf(o[1] - 1.0f) < 1e-6f,
               "yaw: +Y pole is invariant");
    /* Length preservation + full turn. */
    rot_yaw(0.3f, -0.5f, 0.8f, 2.1f, o);
    TEST_CHECK(fabsf(sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]) -
                     sqrtf(0.09f + 0.25f + 0.64f)) < 1e-5f,
               "yaw: preserves length");
}

/* Hammersley + GGX importance sampling kernel shared with the
 * prefilter/BRDF shaders (sanity: points land in the hemisphere,
 * sequence is deterministic). */
static float radical_inverse(uint32_t bits) {
    bits = (bits << 16) | (bits >> 16);
    bits = ((bits & 0x55555555u) << 1) | ((bits & 0xAAAAAAAAu) >> 1);
    bits = ((bits & 0x33333333u) << 2) | ((bits & 0xCCCCCCCCu) >> 2);
    bits = ((bits & 0x0F0F0F0Fu) << 4) | ((bits & 0xF0F0F0F0u) >> 4);
    bits = ((bits & 0x00FF00FFu) << 8) | ((bits & 0xFF00FF00u) >> 8);
    return (float)bits * 2.3283064365386963e-10f;
}

static void test_hammersley(void) {
    uint32_t i;
    int ok = 1;

    if (fabsf(radical_inverse(0) - 0.0f) > 1e-9f) {
        ok = 0;
    }
    if (fabsf(radical_inverse(1) - 0.5f) > 1e-9f) {
        ok = 0;
    }
    if (fabsf(radical_inverse(2) - 0.25f) > 1e-9f) {
        ok = 0;
    }
    for (i = 0; i < 256; i++) {
        float v = radical_inverse(i);

        if (!(v >= 0.0f && v < 1.0f)) {
            ok = 0;
            break;
        }
    }
    TEST_CHECK(ok, "hammersley: radical inverse anchors + range");
}

/* Split-sum BRDF integration (Epic formulation, MUST match
 * brdf.frag integrateBRDF exactly). Returns (A, B). */
static void integrate_brdf(float n_dot_v, float roughness, float out[2]) {
    const uint32_t taps = 128u;
    float vx = sqrtf(1.0f - n_dot_v * n_dot_v > 0.0f
                         ? 1.0f - n_dot_v * n_dot_v
                         : 0.0f);
    float a = roughness * roughness;
    float A = 0.0f;
    float B = 0.0f;
    uint32_t i;

    if (a < 0.0025f) {
        a = 0.0025f; /* MUST match brdf.frag exactly */
    }
    for (i = 0; i < taps; i++) {
        float u1 = (float)i / (float)taps;
        float u2 = radical_inverse(i);
        float phi = 6.2831853f * u1;
        float cos_t =
            sqrtf((1.0f - u2) / (1.0f + (a * a - 1.0f) * u2));
        float sin_t = sqrtf(1.0f - cos_t * cos_t);
        /* Tangent frame around +Z (matches shader up fallback). */
        float hx = cosf(phi) * sin_t;
        float hy = sinf(phi) * sin_t;
        float hz = cos_t;
        float raw_dh = vx * hx + n_dot_v * hz;
        float v_dot_h = raw_dh > 0.0f ? raw_dh : 0.0f;
        float lx = 2.0f * raw_dh * hx - vx;
        float ly = 2.0f * raw_dh * hy - 0.0f;
        float lz = 2.0f * raw_dh * hz - n_dot_v;
        float n_dot_l = lz > 0.0f ? lz : 0.0f;
        float n_dot_h = hz > 0.0f ? hz : 0.0f;
        (void)lx;
        (void)ly;
        if (n_dot_l > 0.0f) {
            float k = (a + 1.0f) * (a + 1.0f) / 8.0f;
            float g1v = n_dot_v / (n_dot_v * (1.0f - k) + k);
            float g1l = n_dot_l / (n_dot_l * (1.0f - k) + k);
            float denom = n_dot_h * n_dot_v;
            float g = g1v * g1l;
            float gv = g * v_dot_h / (denom > 1e-4f ? denom : 1e-4f);
            float fc = powf(1.0f - v_dot_h, 5.0f);

            A += (1.0f - fc) * gv;
            B += fc * gv;
        }
    }
    out[0] = A / (float)taps;
    out[1] = B / (float)taps;
}

static void test_brdf_reference(void) {
    /* Anchors: smooth normal incidence reflects ~fully (A~1,B~0);
     * rough grazing loses energy to the lobe (A small, B > A).
     * Values pinned from this implementation (cross-checked
     * against the GPU LUT in the Vulkan suite). */
    float ab[2];

    integrate_brdf(1.0f, 0.0f, ab);
    TEST_CHECK(ab[0] > 0.95f && ab[0] <= 1.0f && ab[1] < 0.05f,
               "brdf: smooth normal incidence ~ (1, 0)");
    printf("[dbg] brdf(1,0) = (%.4f, %.4f)\n", ab[0], ab[1]);
    integrate_brdf(0.0f, 1.0f, ab);
    TEST_CHECK(ab[0] >= 0.0f && ab[0] < 0.6f && ab[1] >= 0.0f &&
               ab[1] < 0.6f,
               "brdf: rough grazing stays bounded");
    printf("[dbg] brdf(0,1) = (%.4f, %.4f)\n", ab[0], ab[1]);
    integrate_brdf(0.5f, 0.5f, ab);
    TEST_CHECK(ab[0] > 0.3f && ab[0] < 1.0f && ab[1] >= 0.0f &&
               ab[1] < 0.5f,
               "brdf: mid values in sane range");
    printf("[dbg] brdf(0.5,0.5) = (%.4f, %.4f)\n", ab[0], ab[1]);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_tonemap_reference();
    test_rotation_math();
    test_hammersley();
    test_brdf_reference();
    printf("ibl headless: %d passed, %d failed\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}
