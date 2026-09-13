/*
 * Luma Assets headless test (Phase 14).
 *
 * No GPU, no window, no display: pure geometry utilities, argument
 * validation, sampler/file helpers, and negative import cases. The
 * negative imports use a non-dereferenced placeholder renderer: every
 * case fails during parse/validate/decode — before any upload — so
 * the pointer is compared but never touched (la_model_load rejects a
 * NULL renderer up front, hence the placeholder). Successful loads
 * live in test_assets_vulkan.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <luma_renderer/luma_renderer.h>
#include <luma_assets/luma_assets.h>

#include "internal/assets_internal.h"

#ifndef LA_MALFORMED_DIR
#define LA_MALFORMED_DIR "."
#endif

#ifndef LA_FIXTURE_DIR
#define LA_FIXTURE_DIR "."
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

static int feq(float a, float b) {
    return fabsf(a - b) < 1e-5f;
}

/* A placeholder renderer: compared, never dereferenced (all imports
 * below fail before upload). */
static lr_renderer *placeholder_renderer(void) {
    return (lr_renderer *)(uintptr_t)0x1A55E7;
}

static la_asset_manager *make_manager(void) {
    la_asset_manager_desc desc;
    la_asset_manager *manager = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.renderer = placeholder_renderer();
    if (la_asset_manager_create(&desc, &manager) != LA_SUCCESS) {
        return NULL;
    }
    return manager;
}

static void test_normals(void) {
    /* CCW triangle in XY: +Z. */
    static const float pos[9] = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                                  0.0f, 1.0f, 0.0f };
    static const uint32_t idx[3] = { 0, 1, 2 };
    static const float dpos[9] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                                   0.0f, 0.0f, 0.0f };
    static const uint32_t bad[3] = { 0, 1, 9 };
    float n[9];

    TEST_CHECK(la_compute_normals(pos, 3, idx, 3, n) == LA_SUCCESS &&
               feq(n[0], 0.0f) && feq(n[1], 0.0f) && feq(n[2], 1.0f) &&
               feq(n[8], 1.0f), "normals: triangle normal is +Z");
    TEST_CHECK(la_compute_normals(dpos, 3, idx, 3, n) == LA_SUCCESS &&
               feq(n[0], 0.0f) && feq(n[1], 1.0f) && feq(n[2], 0.0f),
               "normals: degenerate fan falls back to +Y");
    TEST_CHECK(la_compute_normals(NULL, 3, idx, 3, n) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "normals: NULL positions rejected");
    TEST_CHECK(la_compute_normals(pos, 3, idx, 3, NULL) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "normals: NULL output rejected");
    TEST_CHECK(la_compute_normals(pos, 0, idx, 3, n) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "normals: zero vertices rejected");
    TEST_CHECK(la_compute_normals(pos, 3, idx, 4, n) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "normals: non-multiple-of-3 rejected");
    TEST_CHECK(la_compute_normals(pos, 3, bad, 3, n) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "normals: out-of-range index rejected");
}

static void test_tangents(void) {
    static const float pos[12] = { -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f,
                                   1.0f, 1.0f, 0.0f, -1.0f, 1.0f, 0.0f };
    static const float nrm[12] = { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                                   0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f };
    static const float uv[8] = { 0.0f, 0.0f, 1.0f, 0.0f,
                                 1.0f, 1.0f, 0.0f, 1.0f };
    static const float flat_uv[8] = { 0.0f, 0.0f, 0.0f, 0.0f,
                                      0.0f, 0.0f, 0.0f, 0.0f };
    static const uint32_t idx[6] = { 0, 1, 2, 0, 2, 3 };
    float t[16];

    TEST_CHECK(la_compute_tangents(pos, nrm, uv, 4, idx, 6, t) ==
                   LA_SUCCESS &&
               feq(t[0], 1.0f) && feq(t[1], 0.0f) && feq(t[2], 0.0f) &&
               feq(t[3], 1.0f) && feq(t[15], 1.0f),
               "tangents: axis-aligned quad gives +X/w=+1");
    TEST_CHECK(la_compute_tangents(pos, nrm, flat_uv, 4, idx, 6, t) ==
                   LA_SUCCESS &&
               feq(t[0], 1.0f) && feq(t[1], 0.0f) && feq(t[2], 0.0f) &&
               feq(t[3], 1.0f),
               "tangents: degenerate UVs fall back to +X/w=+1");
    TEST_CHECK(la_compute_tangents(NULL, nrm, uv, 4, idx, 6, t) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "tangents: NULL positions rejected");
    TEST_CHECK(la_compute_tangents(pos, nrm, uv, 4, idx, 6, NULL) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "tangents: NULL output rejected");
}

static void test_matrix_roundtrip(const lr_transform *tr, const char *msg,
                                    int exact) {
    float m[16];
    float rt[16];
    lr_transform back;
    int i;
    int ok = 1;

    lr_transform_to_matrix(tr, m);
    if (la_matrix_to_transform(m, &back) != LA_SUCCESS) {
        TEST_CHECK(0, msg);
        return;
    }
    lr_transform_to_matrix(&back, rt);
    for (i = 0; i < 16; i++) {
        if (exact ? rt[i] != m[i] : !feq(rt[i], m[i])) {
            ok = 0;
        }
    }
    TEST_CHECK(ok, msg);
}

static void test_matrices(void) {
    lr_transform tr;
    float m[16];
    lr_transform back;

    lr_transform_identity(&tr);
    test_matrix_roundtrip(&tr, "matrix: identity round-trips exactly", 1);
    tr.position[0] = 3.0f;
    tr.position[1] = -2.0f;
    tr.position[2] = 5.0f;
    tr.scale[0] = 2.0f;
    tr.scale[1] = 3.0f;
    tr.scale[2] = 4.0f;
    test_matrix_roundtrip(&tr, "matrix: TRS round-trips exactly", 1);
    /* 90 degrees about Z: (x, y, z, w) = (0, 0, sin45, cos45).
     * Quaternion recovery is unique only up to float32 rounding, so
     * rotation cases compare with epsilon (the stored matrix stays
     * the source of truth; error never accumulates). */
    tr.rotation[0] = 0.0f;
    tr.rotation[1] = 0.0f;
    tr.rotation[2] = 0.7071068f;
    tr.rotation[3] = 0.7071068f;
    test_matrix_roundtrip(&tr, "matrix: rotated TRS round-trips", 0);
    /* Mirror: negative determinant folds into scale.x. */
    tr.scale[0] = -2.0f;
    lr_transform_to_matrix(&tr, m);
    TEST_CHECK(la_matrix_to_transform(m, &back) == LA_SUCCESS &&
               back.scale[0] < 0.0f,
               "matrix: mirror absorbs sign into scale.x");
    test_matrix_roundtrip(&tr, "matrix: mirrored TRS round-trips", 0);
    TEST_CHECK(la_matrix_to_transform(NULL, &back) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "matrix: NULL matrix rejected");
    TEST_CHECK(la_matrix_to_transform(m, NULL) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "matrix: NULL output rejected");
}

static void test_manager_args(void) {
    la_asset_manager_desc desc;
    la_asset_manager *manager = NULL;

    TEST_CHECK(la_asset_manager_create(NULL, &manager) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "manager: NULL desc rejected");
    TEST_CHECK(la_asset_manager_create(&desc, NULL) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "manager: NULL out rejected");
    memset(&desc, 0, sizeof(desc));
    TEST_CHECK(la_asset_manager_create(&desc, &manager) ==
                   LA_ERROR_INVALID_ARGUMENT && manager == NULL,
               "manager: NULL renderer rejected");
    desc.renderer = placeholder_renderer();
    TEST_CHECK(la_asset_manager_create(&desc, &manager) == LA_SUCCESS &&
               manager != NULL,
               "manager: placeholder renderer accepted");
    TEST_CHECK(la_asset_manager_get_texture_count(manager) == 0 &&
               la_asset_manager_get_sampler_count(manager) == 0,
               "manager: fresh caches are empty");
    TEST_CHECK(la_asset_manager_get_last_error(manager) != NULL &&
               la_asset_manager_get_last_error(manager)[0] == '\0',
               "manager: fresh error string is empty");
    TEST_CHECK(la_asset_manager_get_last_error(NULL) != NULL &&
               la_asset_manager_get_texture_count(NULL) == 0 &&
               la_asset_manager_get_sampler_count(NULL) == 0,
               "manager: NULL-safe queries");
    la_asset_manager_destroy(manager);
    la_asset_manager_destroy(NULL);

    /* NULL-safe model inspection with no model at all. */
    TEST_CHECK(la_model_get_node_count(NULL) == 0 &&
               la_model_get_mesh_count(NULL) == 0 &&
               la_model_get_material_count(NULL) == 0 &&
               la_model_get_texture_count(NULL) == 0 &&
               la_model_get_sampler_count(NULL) == 0 &&
               la_model_get_source_primitive_count(NULL) == 0 &&
               la_model_get_source_texture_count(NULL) == 0 &&
               la_model_get_source_sampler_count(NULL) == 0 &&
               la_model_get_instance_count(NULL) == 0 &&
               la_model_get_node(NULL, 0) == NULL &&
               la_model_get_material_data(NULL, 0) == NULL &&
               la_model_get_source_path(NULL) == NULL,
               "model: NULL-safe inspection");
    {
        lr_bounds b;
        la_texture_info ti;
        la_sampler_info si;

        la_model_get_bounds(NULL, &b);
        TEST_CHECK(b.min[0] == 0.0f && b.max[0] == 0.0f &&
                   b.radius == 0.0f,
                   "model: NULL bounds are zero");
        la_model_get_texture_info(NULL, 0, &ti);
        la_model_get_sampler_info(NULL, 0, &si);
        la_model_get_bounds(NULL, NULL);
        la_model_get_texture_info(NULL, 0, NULL);
        la_model_get_sampler_info(NULL, 0, NULL);
        TEST_CHECK(1, "model: NULL-output inspection is a no-op");
    }
    la_model_destroy(NULL);
    TEST_CHECK(la_model_load(NULL, "x", NULL) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "model: NULL manager rejected");
    {
        la_asset_manager *m = make_manager();
        la_model *model = (la_model *)(uintptr_t)0xDEAD;

        TEST_CHECK(m != NULL, "model: manager ready");
        TEST_CHECK(la_model_load(m, NULL, &model) ==
                       LA_ERROR_INVALID_ARGUMENT && model == NULL,
                   "model: NULL path rejected");
        TEST_CHECK(la_model_load(m, "x", NULL) ==
                       LA_ERROR_INVALID_ARGUMENT,
                   "model: NULL out rejected");
        TEST_CHECK(la_model_load(m, "no/such/file.glb", &model) ==
                       LA_ERROR_NOT_FOUND && model == NULL,
                   "model: missing file is NOT_FOUND");
        TEST_CHECK(la_model_submit(NULL, placeholder_renderer(), NULL) ==
                       LA_ERROR_INVALID_ARGUMENT,
                   "model: NULL submit rejected");
        la_asset_manager_destroy(m);
    }
}

static void test_sampler_mapping(void) {
    lc_sampler_desc p;

    TEST_CHECK(la_sampler_from_gltf(NULL, NULL) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "sampler: NULL out rejected");
    TEST_CHECK(la_sampler_from_gltf(NULL, &p) == LA_SUCCESS &&
               p.mag_filter == LC_FILTER_LINEAR &&
               p.min_filter == LC_FILTER_LINEAR &&
               p.mipmap_mode == LC_MIPMAP_MODE_LINEAR &&
               p.address_u == LC_ADDRESS_REPEAT &&
               p.address_v == LC_ADDRESS_REPEAT,
               "sampler: glTF defaults map to linear/repeat");
}

static void test_fs_helpers(void) {
    unsigned char *bytes = NULL;
    size_t size = 0;
    char *joined = NULL;
    char *dir = NULL;

    TEST_CHECK(la_fs_read(LA_MALFORMED_DIR "/nope.bin", &bytes, &size) ==
                   LA_ERROR_NOT_FOUND,
               "fs: missing file is NOT_FOUND");
    TEST_CHECK(la_fs_read("", &bytes, &size) == LA_ERROR_NOT_FOUND,
               "fs: empty path is NOT_FOUND");
    TEST_CHECK(la_fs_read(NULL, &bytes, &size) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "fs: NULL path rejected");
    TEST_CHECK(la_fs_join("a/b", "c.png", &joined) == LA_SUCCESS &&
               strcmp(joined, "a/b/c.png") == 0,
               "fs: join inserts separator");
    free(joined);
    joined = NULL;
    TEST_CHECK(la_fs_join("", "x.glb", &joined) == LA_SUCCESS &&
               strcmp(joined, "x.glb") == 0,
               "fs: empty dir passes through");
    free(joined);
    TEST_CHECK(la_fs_dirname("a/b/c.glb", &dir) == LA_SUCCESS &&
               strcmp(dir, "a/b") == 0,
               "fs: dirname strips leaf");
    free(dir);
    dir = NULL;
    TEST_CHECK(la_fs_dirname("file.glb", &dir) == LA_SUCCESS &&
               strcmp(dir, "") == 0,
               "fs: bare leaf has empty dir");
    free(dir);
    la_fs_free(NULL);
}

static void test_negative_imports(void) {
    /* Pre-upload failures only: every case below fails during
     * parse/validate/decode, so the placeholder renderer is never
     * touched. missing_image/corrupt_image have valid geometry and
     * reach upload — they live in test_assets_vulkan.c. */
    static const struct {
        const char *name;
        la_result want;
    } cases[] = {
        { "invalid_json.gltf", LA_ERROR_IMPORT },
        { "truncated.glb", LA_ERROR_IMPORT },
        { "points_mode.glb", LA_ERROR_UNSUPPORTED },
        { "no_position.glb", LA_ERROR_IMPORT },
        { "index_oob.glb", LA_ERROR_IMPORT },
        { "accessor_oob.glb", LA_ERROR_IMPORT },
        { "missing_buffer.gltf", LA_ERROR_NOT_FOUND },
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char path[1024];
        la_asset_manager *m = make_manager();
        la_model *model = NULL;
        la_result res;

        snprintf(path, sizeof(path), "%s/%s", LA_MALFORMED_DIR,
                 cases[i].name);
        res = la_model_load(m, path, &model);
        TEST_CHECK(res == cases[i].want && model == NULL &&
                   la_asset_manager_get_last_error(m)[0] != '\0',
                   cases[i].name);
        la_asset_manager_destroy(m);
    }
}

static void test_hdr_decode(void);
static void test_half_conversion(void);

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    test_normals();
    test_tangents();
    test_matrices();
    test_manager_args();
    test_sampler_mapping();
    test_fs_helpers();
    test_negative_imports();
    test_hdr_decode();
    test_half_conversion();
    printf("assets headless: %d passed, %d failed\n", g_passed, g_failed);
    return (g_failed == 0) ? 0 : 1;
}

static int fbetween(float v, float lo, float hi) {
    return v >= lo && v <= hi;
}

/* Radiance .hdr decode (Phase 17): the committed procedural fixture
 * (vertical gradient + gaussian spot) decodes to RGBA float32 with
 * documented values; non-HDR bytes are rejected, never regraded. */
static void test_hdr_decode(void) {
    char path[1024];
    unsigned char *bytes = NULL;
    size_t size = 0;
    float *rgba = NULL;
    uint32_t w = 0;
    uint32_t h = 0;
    /* A minimal PNG (non-HDR) must be rejected as UNSUPPORTED. */
    static const unsigned char not_hdr[] = {
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00
    };

    snprintf(path, sizeof(path), "%s/env_gradient.hdr", LA_FIXTURE_DIR);
    TEST_CHECK(la_fs_read(path, &bytes, &size) == LA_SUCCESS && size > 0,
               "hdr: fixture reads");
    if (bytes != NULL) {
        TEST_CHECK(la_hdr_decode(bytes, size, &rgba, &w, &h) ==
                       LA_SUCCESS &&
                   w == 64 && h == 32 && rgba != NULL,
                   "hdr: 64x32 RGBA float decodes");
        if (rgba != NULL) {
            const float *top = rgba + (0u * 64u + 0u) * 4u;
            const float *spot = rgba + (11u * 64u + 32u) * 4u;
            const float *bot = rgba + (31u * 64u + 0u) * 4u;

            TEST_CHECK(fbetween(top[0], 3.7f, 4.0f) &&
                       fbetween(top[1], 3.5f, 3.8f) &&
                       fbetween(top[2], 3.3f, 3.6f) &&
                       top[3] == 1.0f,
                       "hdr: gradient top values + opaque alpha");
            TEST_CHECK(spot[0] > 20.0f && spot[0] < 30.0f,
                       "hdr: gaussian spot peak is HDR");
            TEST_CHECK(fbetween(bot[0], 0.04f, 0.06f) &&
                       bot[3] == 1.0f,
                       "hdr: gradient bottom is dim LDR");
            la_hdr_decode_free(rgba);
            rgba = NULL;
        }
        la_fs_free(bytes);
    }
    TEST_CHECK(la_hdr_decode(not_hdr, sizeof(not_hdr), &rgba, &w,
                             &h) == LA_ERROR_UNSUPPORTED &&
               rgba == NULL,
               "hdr: non-HDR bytes rejected");
    TEST_CHECK(la_hdr_decode(NULL, 0, &rgba, &w, &h) ==
                   LA_ERROR_INVALID_ARGUMENT,
               "hdr: NULL args rejected");
    la_hdr_decode_free(NULL);
}

/* binary32 <-> binary16 conversion incl. RNE, subnormals, Inf/NaN. */
static void test_half_conversion(void) {
    static const struct {
        float f;
        uint16_t h;
    } exact[] = {
        { 0.0f, 0x0000u },   { -0.0f, 0x8000u }, { 1.0f, 0x3C00u },
        { -1.0f, 0xBC00u },  { 0.5f, 0x3800u },  { 2.0f, 0x4000u },
        { 65504.0f, 0x7BFFu },
    };
    static const float roundtrip[] = {
        0.1f, 0.33333334f, 3.1415927f, 100.0f, 0.00006103515625f,
        123.456f, -17.25f, 1e-5f, 60000.0f,
    };
    size_t i;

    for (i = 0; i < sizeof(exact) / sizeof(exact[0]); i++) {
        TEST_CHECK(la_float_to_half(exact[i].f) == exact[i].h,
                   "half: exact value converts");
        TEST_CHECK(la_half_to_float(exact[i].h) == exact[i].f,
                   "half: exact value round-trips");
    }
    /* Halfway between 1.0 and the next half (1.00048828125) rounds
     * to even (0x3C00); overflow goes to Inf, never wraps. */
    TEST_CHECK(la_float_to_half(1.00048828125f) == 0x3C00u,
               "half: halfway rounds to even");
    TEST_CHECK(la_float_to_half(100000.0f) == 0x7C00u,
               "half: finite overflow becomes Inf");
    {
        /* Runtime zeros: constant division by zero is rejected. */
        float zero = 0.0f;
        float inf = 1.0f / zero;
        float nan = zero / zero;
        float back;

        TEST_CHECK(la_float_to_half(inf) == 0x7C00u,
                   "half: Inf stays Inf");
        back = la_half_to_float(la_float_to_half(nan));
        TEST_CHECK(back != back, "half: NaN stays NaN");
    }
    TEST_CHECK(la_half_to_float(0x0400u) == 6.103515625e-05f,
               "half: min normal exact");
    TEST_CHECK(la_half_to_float(0x0001u) > 0.0f &&
               la_half_to_float(0x0001u) < 6.103515625e-05f,
               "half: min subnormal is tiny but nonzero");
    for (i = 0; i < sizeof(roundtrip) / sizeof(roundtrip[0]); i++) {
        float back =
            la_half_to_float(la_float_to_half(roundtrip[i]));

        TEST_CHECK(fabsf(back - roundtrip[i]) <=
                       fabsf(roundtrip[i]) * 0.0011f + 1e-7f,
                   "half: value round-trips within half epsilon");
    }
}
