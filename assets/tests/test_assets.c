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
static void test_skin_decode_units(void);
static void test_skin_anim_files(void);

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
    test_skin_decode_units();
    test_skin_anim_files();
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

/* ------------------------------------------------------------------
 * Phase 29 skin/animation import (headless-safe subset).
 *
 * Pure decode helpers are unit-tested directly; file-level skin
 * and animation validation runs through meshless temp .glb files
 * (no meshes/materials/images, so the placeholder renderer is
 * never touched — every success AND every failure lands before
 * any upload). Vertex-upload positives live in
 * test_assets_vulkan.c.
 * ------------------------------------------------------------------ */

/* Hand-rolled accessor over a stack buffer (mirrors what cgltf
 * produces for a dense non-interleaved accessor). */
typedef struct la_test_span {
    cgltf_buffer buf;
    cgltf_buffer_view view;
    cgltf_accessor acc;
} la_test_span;

static void la_test_span_init(la_test_span *s, void *bytes, size_t len,
                              cgltf_component_type ctype,
                              cgltf_type type, cgltf_size count,
                              int normalized, int sparse) {
    memset(s, 0, sizeof(*s));
    s->buf.data = bytes;
    s->buf.size = (cgltf_size)len;
    s->view.buffer = &s->buf;
    s->view.offset = 0;
    s->view.size = (cgltf_size)len;
    s->acc.buffer_view = &s->view;
    s->acc.offset = 0;
    s->acc.stride = 0;
    s->acc.component_type = ctype;
    s->acc.type = type;
    s->acc.count = count;
    s->acc.normalized = normalized;
    s->acc.is_sparse = sparse;
}

static void test_skin_decode_units(void) {
    /* la_normalize_weights: rescale, rigid fallback, NULL-safe. */
    {
        float w[4] = { 0.25f, 0.25f, 0.0f, 0.0f };

        la_normalize_weights(w);
        TEST_CHECK(feq(w[0], 0.5f) && feq(w[1], 0.5f) &&
                   w[2] == 0.0f && w[3] == 0.0f,
                   "skin: partial weights rescale to sum 1");
    }
    {
        float w[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

        la_normalize_weights(w);
        TEST_CHECK(w[0] == 1.0f && w[1] == 0.0f && w[2] == 0.0f &&
                   w[3] == 0.0f,
                   "skin: all-zero weights fall back to rigid");
    }
    {
        float w[4] = { 2.0f, 0.0f, 0.0f, 0.0f };

        la_normalize_weights(w);
        TEST_CHECK(w[0] == 1.0f, "skin: oversum weights normalize");
        la_normalize_weights(NULL);
        TEST_CHECK(1, "skin: NULL weights are a no-op");
    }
    /* la_decode_joints: u8/u16 widening + rejections. */
    {
        la_test_span s;
        unsigned char j8[8] = { 0, 0, 0, 0, 5, 6, 7, 8 };
        uint32_t jout[8];

        la_test_span_init(&s, j8, sizeof(j8),
                          cgltf_component_type_r_8u,
                          cgltf_type_vec4, 2, 0, 0);
        TEST_CHECK(la_decode_joints(&s.acc, 2, jout) == LA_SUCCESS &&
                   jout[0] == 0 && jout[4] == 5 && jout[7] == 8,
                   "skin: u8 joints widen");
    }
    {
        la_test_span s;
        unsigned char j16[16] = { 0, 0, 0, 0, 0, 0, 0, 0,
                                  0x2C, 0x01, 0, 0, 0, 0, 0, 0 };
        uint32_t jout[8];

        la_test_span_init(&s, j16, sizeof(j16),
                          cgltf_component_type_r_16u,
                          cgltf_type_vec4, 2, 0, 0);
        TEST_CHECK(la_decode_joints(&s.acc, 2, jout) == LA_SUCCESS &&
                   jout[0] == 0 && jout[4] == 300,
                   "skin: u16 joints widen past 255");
    }
    {
        la_test_span s;
        unsigned char j8[4] = { 1, 2, 3, 4 };
        uint32_t jout[4];

        la_test_span_init(&s, j8, sizeof(j8),
                          cgltf_component_type_r_8u,
                          cgltf_type_vec4, 1, 1, 0);
        TEST_CHECK(la_decode_joints(&s.acc, 1, jout) ==
                       LA_ERROR_UNSUPPORTED,
                   "skin: normalized JOINTS rejected");
        la_test_span_init(&s, j8, sizeof(j8),
                          cgltf_component_type_r_32f,
                          cgltf_type_vec4, 1, 0, 0);
        TEST_CHECK(la_decode_joints(&s.acc, 1, jout) ==
                       LA_ERROR_UNSUPPORTED,
                   "skin: float JOINTS rejected");
        la_test_span_init(&s, j8, sizeof(j8),
                          cgltf_component_type_r_8u,
                          cgltf_type_vec4, 1, 0, 1);
        TEST_CHECK(la_decode_joints(&s.acc, 1, jout) ==
                       LA_ERROR_UNSUPPORTED,
                   "skin: sparse JOINTS rejected");
        TEST_CHECK(la_decode_joints(NULL, 1, jout) ==
                       LA_ERROR_INVALID_ARGUMENT,
                   "skin: NULL joints accessor rejected");
    }
    /* la_decode_skin_vertex: defaults, normalization, NaN guard. */
    {
        uint32_t j[4];
        float w[4];

        TEST_CHECK(la_decode_skin_vertex(NULL, NULL, 1, j, w) ==
                       LA_SUCCESS &&
                   j[0] == 0 && j[3] == 0 && w[0] == 1.0f &&
                   w[1] == 0.0f,
                   "skin: missing streams select rigid defaults");
        TEST_CHECK(la_decode_skin_vertex(NULL, NULL, 0, j, w) ==
                       LA_ERROR_INVALID_ARGUMENT,
                   "skin: zero vertices rejected");
    }
    {
        /* Normalized u16 {32768,32768,0,0} -> ~{.5,.5,0,0}. */
        unsigned char w16[8] = { 0x00, 0x80, 0x00, 0x80,
                                 0x00, 0x00, 0x00, 0x00 };
        la_test_span s;
        uint32_t j[4];
        float w[4];

        la_test_span_init(&s, w16, sizeof(w16),
                          cgltf_component_type_r_16u,
                          cgltf_type_vec4, 1, 1, 0);
        TEST_CHECK(la_decode_skin_vertex(NULL, &s.acc, 1, j, w) ==
                       LA_SUCCESS &&
                   fabsf(w[0] - 0.5f) < 1e-4f &&
                   fabsf(w[1] - 0.5f) < 1e-4f &&
                   fabsf(w[0] + w[1] - 1.0f) < 1e-5f,
                   "skin: normalized u16 weights decode + normalize");
    }
    {
        /* NaN/Inf weights poison normalization: IMPORT errors. */
        float bad[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        uint32_t bits = 0x7FC00000u;
        la_test_span s;
        uint32_t j[4];
        float w[4];

        memcpy(&bad[1], &bits, 4u);
        la_test_span_init(&s, bad, sizeof(bad),
                          cgltf_component_type_r_32f,
                          cgltf_type_vec4, 1, 0, 0);
        TEST_CHECK(la_decode_skin_vertex(NULL, &s.acc, 1, j, w) ==
                       LA_ERROR_IMPORT,
                   "skin: NaN weight is an import error");
        bits = 0x7F800000u; /* +Inf */
        memcpy(&bad[1], &bits, 4u);
        TEST_CHECK(la_decode_skin_vertex(NULL, &s.acc, 1, j, w) ==
                       LA_ERROR_IMPORT,
                   "skin: Inf weight is an import error");
    }
}

/* Tiny GLB writer for crafted file-level cases (JSON + optional
 * BIN; single JSON chunk when there is no binary payload). */
static int la_write_tmp_glb(const char *name, const char *json,
                            const unsigned char *bin, size_t bin_len) {
    FILE *f = NULL;
    size_t jlen;
    size_t jpad;
    size_t bpad;
    size_t i;
    uint32_t total;
    uint32_t magic = 0x46546C67u;
    uint32_t version = 2u;
    uint32_t jtype = 0x4E4F534Au;
    uint32_t btype = 0x004E4942u;
    uint32_t jlen32;
    uint32_t blen32;

    if (name == NULL || json == NULL) {
        return 0;
    }
    if (bin == NULL) {
        bin_len = 0;
    }
    jlen = strlen(json);
    if (jlen == 0 || jlen > 0xFFFFFFu || bin_len > 0xFFFFFFu) {
        return 0;
    }
    jpad = (4u - (jlen % 4u)) % 4u;
    bpad = (4u - (bin_len % 4u)) % 4u;
    /* Chunk lengths INCLUDE padding (matches generate_fixtures.py
     * write_glb; this cgltf locates the BIN chunk at
     * json_data + json_chunk_length with no extra skip). */
    jlen32 = (uint32_t)jlen + (uint32_t)jpad;
    blen32 = (uint32_t)bin_len + (uint32_t)bpad;
    total = 12u + 8u + jlen32;
    if (bin_len > 0) {
        total += 8u + blen32;
    }
    f = fopen(name, "wb");
    if (f == NULL) {
        return 0;
    }
    if (fwrite(&magic, 4u, 1u, f) != 1u ||
        fwrite(&version, 4u, 1u, f) != 1u ||
        fwrite(&total, 4u, 1u, f) != 1u ||
        fwrite(&jlen32, 4u, 1u, f) != 1u ||
        fwrite(&jtype, 4u, 1u, f) != 1u ||
        fwrite(json, 1u, jlen, f) != jlen) {
        fclose(f);
        remove(name);
        return 0;
    }
    for (i = 0; i < jpad; i++) {
        if (fputc(0x20, f) == EOF) {
            fclose(f);
            remove(name);
            return 0;
        }
    }
    if (bin_len > 0) {
        if (fwrite(&blen32, 4u, 1u, f) != 1u ||
            fwrite(&btype, 4u, 1u, f) != 1u ||
            fwrite(bin, 1u, bin_len, f) != bin_len) {
            fclose(f);
            remove(name);
            return 0;
        }
        for (i = 0; i < bpad; i++) {
            if (fputc(0x00, f) == EOF) {
                fclose(f);
                remove(name);
                return 0;
            }
        }
    }
    if (fclose(f) != 0) {
        remove(name);
        return 0;
    }
    return 1;
}

/* Float BIN builder (little-endian host matches GLB order on
 * every target this suite runs on). */
typedef struct la_bin {
    unsigned char data[1024];
    size_t len;
} la_bin;

static void la_bin_f32(la_bin *b, float v) {
    if (b->len + 4u <= sizeof(b->data)) {
        memcpy(b->data + b->len, &v, 4u);
        b->len += 4u;
    }
}

static void la_bin_bytes(la_bin *b, const void *src, size_t n) {
    if (b->len + n <= sizeof(b->data)) {
        memcpy(b->data + b->len, src, n);
        b->len += n;
    }
}

/* Load one crafted file; the temp file is always removed. */
static la_result la_load_tmp(la_asset_manager *m, const char *name,
                             const char *json, la_bin *b,
                             la_model **out_model) {
    la_result res;

    if (la_write_tmp_glb(name, json, (b != NULL) ? b->data : NULL,
                         (b != NULL) ? b->len : 0) == 0) {
        return LA_ERROR_OUT_OF_MEMORY;
    }
    res = la_model_load(m, name, out_model);
    remove(name);
    return res;
}

static void test_skin_anim_files(void) {
    /* Meshless skin + IBM: full query surface, headless. */
    static const char k_skin_ok[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0,1]}],"
        "\"nodes\":[{\"name\":\"A\"},{\"name\":\"B\",\"skin\":0}],"
        "\"skins\":[{\"name\":\"Rig\",\"joints\":[0,1],"
        "\"inverseBindMatrices\":0}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
        "\"count\":2,\"type\":\"MAT4\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,"
        "\"byteLength\":128}],"
        "\"buffers\":[{\"byteLength\":128}]}";
    /* Meshless animation: T LINEAR (3 keys) + R CUBICSPLINE. */
    static const char k_anim_ok[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0,1]}],"
        "\"nodes\":[{\"name\":\"A\"},{\"name\":\"B\"}],"
        "\"animations\":[{\"name\":\"Move\",\"samplers\":["
        "{\"input\":0,\"output\":1,\"interpolation\":\"LINEAR\"},"
        "{\"input\":2,\"output\":3,"
        "\"interpolation\":\"CUBICSPLINE\"}],"
        "\"channels\":["
        "{\"sampler\":0,\"target\":{\"node\":0,"
        "\"path\":\"translation\"}},"
        "{\"sampler\":1,\"target\":{\"node\":1,"
        "\"path\":\"rotation\"}}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
        "\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":2,"
        "\"type\":\"SCALAR\"},"
        "{\"bufferView\":3,\"componentType\":5126,\"count\":6,"
        "\"type\":\"VEC4\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":12},"
        "{\"buffer\":0,\"byteOffset\":12,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":48,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":56,\"byteLength\":96}],"
        "\"buffers\":[{\"byteLength\":152}]}";
    /* Mixed morph + translation: morph skipped, T kept. */
    static const char k_morph_mixed[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"A\"}],"
        "\"animations\":[{\"name\":\"Mix\",\"samplers\":["
        "{\"input\":0,\"output\":1},"
        "{\"input\":0,\"output\":2}],"
        "\"channels\":["
        "{\"sampler\":0,\"target\":{\"node\":0,"
        "\"path\":\"weights\"}},"
        "{\"sampler\":1,\"target\":{\"node\":0,"
        "\"path\":\"translation\"}}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
        "\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,"
        "\"type\":\"SCALAR\"},"
        "{\"bufferView\":2,\"componentType\":5126,\"count\":2,"
        "\"type\":\"VEC3\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":16,\"byteLength\":24}],"
        "\"buffers\":[{\"byteLength\":40}]}";
    /* Morph-only animation: excluded from the count. */
    static const char k_morph_only[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"A\"}],"
        "\"animations\":[{\"name\":\"MorphOnly\",\"samplers\":["
        "{\"input\":0,\"output\":1}],"
        "\"channels\":["
        "{\"sampler\":0,\"target\":{\"node\":0,"
        "\"path\":\"weights\"}}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
        "\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,"
        "\"type\":\"SCALAR\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":8}],"
        "\"buffers\":[{\"byteLength\":16}]}";
    /* Zero-joint skin: malformed. */
    static const char k_zero_joints[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"A\"}],"
        "\"skins\":[{\"joints\":[]}]}";
    /* Duplicate joint nodes: malformed (skeleton keys by node). */
    static const char k_dup_joints[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"A\"}],"
        "\"skins\":[{\"joints\":[0,0]}]}";
    /* IBM count mismatch (1 matrix, 2 joints): malformed. */
    static const char k_ibm_count[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0,1]}],"
        "\"nodes\":[{\"name\":\"A\"},{\"name\":\"B\"}],"
        "\"skins\":[{\"joints\":[0,1],"
        "\"inverseBindMatrices\":0}],"
        "\"accessors\":[{\"bufferView\":0,\"componentType\":5126,"
        "\"count\":1,\"type\":\"MAT4\"}],"
        "\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,"
        "\"byteLength\":64}],"
        "\"buffers\":[{\"byteLength\":64}]}";
    /* Negative key time: malformed. */
    static const char k_neg_time[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"A\"}],"
        "\"animations\":[{\"samplers\":[{\"input\":0,\"output\":1}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,"
        "\"path\":\"translation\"}}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
        "\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,"
        "\"type\":\"VEC3\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":24}],"
        "\"buffers\":[{\"byteLength\":32}]}";
    /* Output count mismatch (3 vec3 vs 2 keys): malformed. */
    static const char k_out_count[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"A\"}],"
        "\"animations\":[{\"samplers\":[{\"input\":0,\"output\":1}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,"
        "\"path\":\"translation\"}}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
        "\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":36}],"
        "\"buffers\":[{\"byteLength\":44}]}";
    /* Unknown animation path: malformed. */
    static const char k_bad_path[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],\"nodes\":[{\"name\":\"A\"}],"
        "\"animations\":[{\"samplers\":[{\"input\":0,\"output\":1}],"
        "\"channels\":[{\"sampler\":0,\"target\":{\"node\":0,"
        "\"path\":\"notapath\"}}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":2,"
        "\"type\":\"SCALAR\"},"
        "{\"bufferView\":1,\"componentType\":5126,\"count\":2,"
        "\"type\":\"VEC3\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":8},"
        "{\"buffer\":0,\"byteOffset\":8,\"byteLength\":24}],"
        "\"buffers\":[{\"byteLength\":32}]}";
    /* JOINTS_1 presence: four-influence limit (UNSUPPORTED).
     * Fires before any upload, so the placeholder renderer
     * stays untouched. */
    static const char k_joints1[] =
        "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,"
        "\"scenes\":[{\"nodes\":[0]}],"
        "\"nodes\":[{\"mesh\":0}],"
        "\"meshes\":[{\"primitives\":[{\"attributes\":{"
        "\"POSITION\":0,\"JOINTS_1\":1},\"mode\":4}]}],"
        "\"accessors\":["
        "{\"bufferView\":0,\"componentType\":5126,\"count\":3,"
        "\"type\":\"VEC3\"},"
        "{\"bufferView\":1,\"componentType\":5121,\"count\":3,"
        "\"type\":\"VEC4\"}],"
        "\"bufferViews\":["
        "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36},"
        "{\"buffer\":0,\"byteOffset\":36,\"byteLength\":12}],"
        "\"buffers\":[{\"byteLength\":48}]}";
    {
        /* Skin queries over a meshless file (headless). */
        la_asset_manager *m = make_manager();
        la_model *model = NULL;
        la_bin b;
        float ibm[16];
        size_t k;
        int ok = 0;

        memset(&b, 0, sizeof(b));
        /* Joint 0: identity. Joint 1: translation (1,2,3). */
        for (k = 0; k < 16; k++) {
            la_bin_f32(&b, (k % 5u == 0) ? 1.0f : 0.0f);
        }
        for (k = 0; k < 16; k++) {
            float v = (k % 5u == 0) ? 1.0f : 0.0f;

            if (k == 12) {
                v = 1.0f;
            } else if (k == 13) {
                v = 2.0f;
            } else if (k == 14) {
                v = 3.0f;
            }
            la_bin_f32(&b, v);
        }
        TEST_CHECK(m != NULL, "skinfile: manager ready");
        if (m != NULL) {
            la_anim_channel ch;
            const la_model_node *n1;

            memset(&ch, 0, sizeof(ch));
            TEST_CHECK(la_load_tmp(m, "la_tmp_skin.glb", k_skin_ok,
                                   &b, &model) == LA_SUCCESS &&
                       model != NULL,
                       "skinfile: meshless skin loads headless");
            if (model != NULL) {
                la_model_get_skin_inverse_bind(model, 0, 0, ibm);
                ok = (la_model_get_skin_count(model) == 1 &&
                      la_model_get_skin_joint_count(model, 0) == 2 &&
                      la_model_get_skin_joint_node(model, 0, 0) == 0 &&
                      la_model_get_skin_joint_node(model, 0, 1) == 1 &&
                      feq(ibm[0], 1.0f) && feq(ibm[5], 1.0f) &&
                      feq(ibm[15], 1.0f));
                TEST_CHECK(ok, "skinfile: joint nodes + identity bind");
                la_model_get_skin_inverse_bind(model, 0, 1, ibm);
                TEST_CHECK(feq(ibm[12], 1.0f) &&
                           feq(ibm[13], 2.0f) &&
                           feq(ibm[14], 3.0f) && feq(ibm[0], 1.0f),
                           "skinfile: inverse-bind values decode");
                TEST_CHECK(la_model_get_node_skin(model, 1) == 0 &&
                           la_model_get_node_skin(model, 0) == -1 &&
                           la_model_get_node_skin(model, 9) == -1 &&
                           la_model_get_node_skin(NULL, 0) == -1,
                           "skinfile: node skin links (+OOB/NULL)");
                n1 = la_model_get_node(model, 1);
                TEST_CHECK(n1 != NULL && n1->skin_index == 0,
                           "skinfile: node struct carries skin_index");
                TEST_CHECK(la_model_get_skin_count(NULL) == 0 &&
                           la_model_get_skin_joint_count(NULL, 0) ==
                               0 &&
                           la_model_get_skin_joint_count(model, 7) ==
                               0 &&
                           la_model_get_skin_joint_node(NULL, 0, 0) ==
                               -1 &&
                           la_model_get_skin_joint_node(model, 0, 7) ==
                               -1 &&
                           la_model_get_animation_count(model) == 0 &&
                           la_model_get_animation_channel_count(
                               model, 0) == 0 &&
                           la_model_get_animation_channel(model, 0, 0,
                                                          &ch) == 0 &&
                           la_model_get_animation_duration(model, 0) ==
                               0.0f,
                           "skinfile: OOB/NULL queries are zeros");
                la_model_get_skin_inverse_bind(model, 0, 7, ibm);
                TEST_CHECK(feq(ibm[0], 1.0f) && feq(ibm[5], 1.0f) &&
                           feq(ibm[10], 1.0f) &&
                           feq(ibm[15], 1.0f) && ibm[12] == 0.0f,
                           "skinfile: bad joint bind is identity");
                la_model_get_skin_inverse_bind(NULL, 0, 0, ibm);
                la_model_get_skin_inverse_bind(model, 0, 0, NULL);
                TEST_CHECK(feq(ibm[0], 1.0f),
                           "skinfile: NULL-model bind is identity");
                la_model_destroy(model);
                model = NULL;
            }
            la_asset_manager_destroy(m);
        }
    }
    {
        /* Animation queries over a meshless file (headless). */
        la_asset_manager *m = make_manager();
        la_model *model = NULL;
        la_bin b;
        int ok = 0;

        memset(&b, 0, sizeof(b));
        /* Times [0,1,2] (12B); T values (0,0,0),(1,0,0),(2,0,0)
         * (36B); cubic times [0,3] (8B); cubic rotation output
         * (2 keys x in/value/out x VEC4 = 24 floats = 96B).
         * Total 152B. */
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 1.0f);
        la_bin_f32(&b, 2.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 1.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 2.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        /* Cubic rotation times [0,3]. */
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 3.0f);
        /* Key 0 triple: in/quat(identity)/out. */
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 1.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        /* Key 1 triple: 90 deg about Z. */
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.7071068f);
        la_bin_f32(&b, 0.7071068f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        TEST_CHECK(m != NULL, "animfile: manager ready");
        TEST_CHECK(b.len == 152, "animfile: BIN length matches views");
        if (m != NULL && b.len == 152) {
            la_anim_channel ch0;
            la_anim_channel ch1;

            memset(&ch0, 0, sizeof(ch0));
            memset(&ch1, 0, sizeof(ch1));
            TEST_CHECK(la_load_tmp(m, "la_tmp_anim.glb", k_anim_ok,
                                   &b, &model) == LA_SUCCESS &&
                       model != NULL,
                       "animfile: meshless animation loads headless");
            if (model != NULL) {
                ok =
                    (la_model_get_animation_count(model) == 1 &&
                     la_model_get_animation_channel_count(model, 0) ==
                         2 &&
                     la_model_get_animation_channel(model, 0, 0,
                                                    &ch0) == 1 &&
                     la_model_get_animation_channel(model, 0, 1,
                                                    &ch1) == 1);
                TEST_CHECK(ok, "animfile: 1 anim, 2 channels borrow");
                TEST_CHECK(ch0.target_node == 0 && ch0.path == 0 &&
                           ch0.interpolation == 1 &&
                           ch0.key_count == 3 && ch0.times != NULL &&
                           ch0.values != NULL &&
                           feq(ch0.times[0], 0.0f) &&
                           feq(ch0.times[2], 2.0f) &&
                           feq(ch0.values[3], 1.0f),
                           "animfile: translation track decodes");
                TEST_CHECK(ch1.target_node == 1 && ch1.path == 1 &&
                           ch1.interpolation == 2 &&
                           ch1.key_count == 2 &&
                           feq(ch1.times[1], 3.0f) &&
                           feq(ch1.values[7], 1.0f) &&
                           feq(ch1.values[18], 0.7071068f) &&
                           feq(ch1.values[19], 0.7071068f),
                           "animfile: cubic rotation triples decode");
                TEST_CHECK(feq(la_model_get_animation_duration(
                                   model, 0),
                               3.0f) &&
                           la_model_get_animation_duration(model,
                                                           5) == 0.0f &&
                           la_model_get_animation_duration(NULL, 0) ==
                               0.0f,
                           "animfile: duration is max last-key time");
                TEST_CHECK(la_model_get_animation_count(NULL) == 0 &&
                           la_model_get_animation_channel_count(
                               NULL, 0) == 0 &&
                           la_model_get_animation_channel(
                               NULL, 0, 0, &ch0) == 0 &&
                           la_model_get_animation_channel(
                               model, 0, 9, &ch0) == 0 &&
                           la_model_get_animation_channel(
                               model, 0, 0, NULL) == 0,
                           "animfile: bad channel queries fail clean");
                la_model_destroy(model);
                model = NULL;
            }
            la_asset_manager_destroy(m);
        }
    }
    {
        /* Morph skip rules: mixed keeps T, morph-only excluded. */
        la_asset_manager *m = make_manager();
        la_model *model = NULL;
        la_bin b;

        memset(&b, 0, sizeof(b));
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.5f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 1.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 1.0f);
        la_bin_f32(&b, 0.0f);
        la_bin_f32(&b, 0.0f);
        if (m != NULL) {
            la_anim_channel ch;

            memset(&ch, 0, sizeof(ch));
            TEST_CHECK(la_load_tmp(m, "la_tmp_mix.glb",
                                   k_morph_mixed, &b,
                                   &model) == LA_SUCCESS &&
                       model != NULL,
                       "morph: mixed animation loads");
            if (model != NULL) {
                TEST_CHECK(la_model_get_animation_count(model) == 1 &&
                           la_model_get_animation_channel_count(
                               model, 0) == 1 &&
                           la_model_get_animation_channel(model, 0, 0,
                                                          &ch) == 1 &&
                           ch.path == 0 && ch.target_node == 0,
                           "morph: weights skipped, translation kept");
                la_model_destroy(model);
                model = NULL;
            }
            TEST_CHECK(la_load_tmp(m, "la_tmp_morph.glb",
                                   k_morph_only, &b,
                                   &model) == LA_SUCCESS &&
                       model != NULL,
                       "morph: morph-only file still loads");
            if (model != NULL) {
                TEST_CHECK(la_model_get_animation_count(model) == 0,
                           "morph: excluded count is zero");
                la_model_destroy(model);
                model = NULL;
            }
            la_asset_manager_destroy(m);
        }
    }
    {
        /* File-level negatives: every case fails with a loud
         * error and no model. */
        static const struct {
            const char *file;
            const char *json;
            la_result want;
            const char *msg;
        } cases[] = {
            { "la_tmp_zj.glb", k_zero_joints, LA_ERROR_IMPORT,
              "neg: zero-joint skin is IMPORT" },
            { "la_tmp_dup.glb", k_dup_joints, LA_ERROR_IMPORT,
              "neg: duplicate joint nodes are IMPORT" },
            { "la_tmp_ibmc.glb", k_ibm_count, LA_ERROR_IMPORT,
              "neg: inverse-bind count mismatch is IMPORT" },
            { "la_tmp_negt.glb", k_neg_time, LA_ERROR_IMPORT,
              "neg: negative key time is IMPORT" },
            { "la_tmp_outc.glb", k_out_count, LA_ERROR_IMPORT,
              "neg: output count mismatch is IMPORT" },
            { "la_tmp_path.glb", k_bad_path, LA_ERROR_IMPORT,
              "neg: unknown animation path is IMPORT" },
            { "la_tmp_j1.glb", k_joints1, LA_ERROR_UNSUPPORTED,
              "neg: JOINTS_1 is UNSUPPORTED (four-influence)" },
        };
        size_t i;

        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            la_asset_manager *m = make_manager();
            la_model *model = NULL;
            la_bin b;
            la_result res;

            memset(&b, 0, sizeof(b));
            if (strcmp(cases[i].file, "la_tmp_negt.glb") == 0) {
                la_bin_f32(&b, -1.0f);
                la_bin_f32(&b, 1.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
            } else if (strcmp(cases[i].file, "la_tmp_outc.glb") == 0) {
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 1.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
            } else if (strcmp(cases[i].file, "la_tmp_path.glb") == 0) {
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 1.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
                la_bin_f32(&b, 0.0f);
            } else if (strcmp(cases[i].file, "la_tmp_ibmc.glb") == 0) {
                size_t k;

                for (k = 0; k < 16; k++) {
                    la_bin_f32(&b, (k % 5u == 0) ? 1.0f : 0.0f);
                }
            } else if (strcmp(cases[i].file, "la_tmp_j1.glb") == 0) {
                static const float tri[9] = { 0.0f, 0.0f, 0.0f,
                                              1.0f, 0.0f, 0.0f,
                                              0.0f, 1.0f, 0.0f };
                static const unsigned char j1[12] = { 0 };
                size_t k;

                for (k = 0; k < 9; k++) {
                    la_bin_f32(&b, tri[k]);
                }
                la_bin_bytes(&b, j1, sizeof(j1));
            }
            res = la_load_tmp(m, cases[i].file, cases[i].json,
                              (b.len > 0) ? &b : NULL, &model);
            TEST_CHECK(res == cases[i].want && model == NULL &&
                       la_asset_manager_get_last_error(m)[0] != '\0',
                       cases[i].msg);
            la_asset_manager_destroy(m);
        }
    }
}
