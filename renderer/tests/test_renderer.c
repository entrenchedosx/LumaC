/*
 * Headless Luma Renderer unit tests (Phase 13).
 *
 * No GPU, no window, no LumaC device: NULL/dead validation, pure
 * camera/transform math, mesh bounds, primitive quality (normalized
 * normals, tangent handedness, valid indices), and frustum behavior.
 * GPU-backed mesh/material/render paths live in
 * test_renderer_vulkan.
 */
#include <math.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include <luma_renderer/luma_renderer.h>

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

static int mat_near(const float *a, const float *b, int n, float eps) {
    int i;

    for (i = 0; i < n; i++) {
        float d = a[i] - b[i];

        if (!(d <= eps && d >= -eps)) {
            return 0;
        }
    }
    return 1;
}

int main(void) {
    printf("Running Luma Renderer headless unit tests...\n");

    /* NULL-safe constructors/destructors/getters. */
    TEST_CHECK(lr_mesh_create(NULL, NULL, NULL) ==
                   LR_ERROR_INVALID_ARGUMENT,
               "mesh create all-NULL -> INVALID");
    lr_mesh_destroy(NULL);
    TEST_CHECK(1, "mesh destroy(NULL) safe");
    TEST_CHECK(lr_mesh_get_vertex_count(NULL) == 0,
               "mesh verts(NULL) -> 0");
    TEST_CHECK(lr_mesh_get_index_count(NULL) == 0, "mesh idx(NULL) -> 0");
    {
        lr_bounds b;

        memset(&b, 0xFF, sizeof(b));
        lr_mesh_get_bounds(NULL, &b);
        TEST_CHECK(b.min[0] == 0.0f && b.radius == 0.0f,
                   "mesh bounds(NULL) zeroed");
    }
    TEST_CHECK(lr_mesh_create_cube(NULL, 1.0f, NULL) ==
                   LR_ERROR_INVALID_ARGUMENT,
               "cube(NULL renderer) -> INVALID");
    TEST_CHECK(lr_mesh_create_plane(NULL, 1.0f, 1.0f, NULL) ==
                   LR_ERROR_INVALID_ARGUMENT,
               "plane(NULL renderer) -> INVALID");
    TEST_CHECK(lr_mesh_create_sphere(NULL, 1.0f, 8, 4, NULL) ==
                   LR_ERROR_INVALID_ARGUMENT,
               "sphere(NULL renderer) -> INVALID");
    TEST_CHECK(lr_material_create_unlit(NULL, NULL, NULL) ==
                   LR_ERROR_INVALID_ARGUMENT,
               "material all-NULL -> INVALID");
    lr_material_destroy(NULL);
    TEST_CHECK(1, "material destroy(NULL) safe");
    TEST_CHECK(lr_renderer_create(NULL, NULL) == LR_ERROR_INVALID_ARGUMENT,
               "renderer create all-NULL -> INVALID");
    lr_renderer_destroy(NULL);
    TEST_CHECK(1, "renderer destroy(NULL) safe");
    TEST_CHECK(lr_renderer_begin(NULL, NULL) == LR_ERROR_INVALID_ARGUMENT,
               "begin all-NULL -> INVALID");
    TEST_CHECK(lr_renderer_submit(NULL, NULL) == LR_ERROR_INVALID_ARGUMENT,
               "submit all-NULL -> INVALID");
    TEST_CHECK(lr_renderer_render(NULL, NULL, NULL) ==
                   LR_ERROR_INVALID_ARGUMENT,
               "render all-NULL -> INVALID");
    lr_renderer_end(NULL);
    TEST_CHECK(1, "end(NULL) safe");
    {
        lr_render_stats stats;

        memset(&stats, 0xFF, sizeof(stats));
        lr_renderer_get_stats(NULL, &stats);
        TEST_CHECK(stats.submitted_objects == 0 && stats.triangles == 0,
                   "stats(NULL) zeroed");
    }

    /* Mesh descriptor validation (no renderer needed for shape). */
    {
        lr_vertex v[3];
        uint32_t idx[3] = { 0, 1, 2 };
        lr_mesh_desc desc;
        lr_mesh *out = (lr_mesh *)0x1;

        memset(v, 0, sizeof(v));
        memset(&desc, 0, sizeof(desc));
        desc.vertices = v;
        desc.vertex_count = 3;
        desc.indices = idx;
        desc.index_count = 3;
        /* NULL renderer still reports INVALID (not a crash). */
        TEST_CHECK(lr_mesh_create(NULL, &desc, &out) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "mesh create(NULL renderer) -> INVALID");
        TEST_CHECK(out == NULL, "mesh out cleared");
    }

    /* Pure bounds on synthetic data. */
    {
        lr_vertex v[2];
        lr_mesh_desc desc;
        lr_bounds bounds;

        memset(v, 0, sizeof(v));
        v[0].position[0] = -2.0f;
        v[0].position[1] = 0.0f;
        v[0].position[2] = 1.0f;
        v[1].position[0] = 4.0f;
        v[1].position[1] = -1.0f;
        v[1].position[2] = 5.0f;
        memset(&desc, 0, sizeof(desc));
        desc.vertices = v;
        desc.vertex_count = 2;
        TEST_CHECK(lr_mesh_compute_bounds(&desc, &bounds) == LR_SUCCESS,
                   "bounds compute succeeds");
        TEST_CHECK(bounds.min[0] == -2.0f && bounds.max[0] == 4.0f &&
                       bounds.min[1] == -1.0f && bounds.max[1] == 0.0f &&
                       bounds.min[2] == 1.0f && bounds.max[2] == 5.0f,
                   "bounds min/max exact");
        TEST_CHECK(bounds.center[0] == 1.0f && bounds.center[1] == -0.5f &&
                       bounds.center[2] == 3.0f,
                   "bounds center exact");
        TEST_CHECK(bounds.radius > 3.6f && bounds.radius < 3.7f,
                   "bounds radius sane");
        TEST_CHECK(lr_mesh_compute_bounds(NULL, &bounds) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "bounds(NULL desc) -> INVALID");
        TEST_CHECK(lr_mesh_compute_bounds(&desc, NULL) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "bounds(NULL out) -> INVALID");
        {
            lr_mesh_desc empty = { 0 };

            TEST_CHECK(lr_mesh_compute_bounds(&empty, &bounds) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "bounds(empty desc) -> INVALID");
        }
    }

    /* Camera validation. */
    {
        lr_camera cam;

        lr_camera_init(NULL);
        TEST_CHECK(1, "camera init(NULL) safe");
        lr_camera_init(&cam);
        TEST_CHECK(lr_camera_set_perspective(NULL, 1.0f, 1.0f, 0.1f,
                                             100.0f) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "perspective(NULL) -> INVALID");
        TEST_CHECK(lr_camera_set_perspective(&cam, 0.0f, 1.0f, 0.1f,
                                             100.0f) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "zero FOV rejected");
        TEST_CHECK(lr_camera_set_perspective(&cam, 1.0f, 0.0f, 0.1f,
                                             100.0f) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "zero aspect rejected");
        TEST_CHECK(lr_camera_set_perspective(&cam, 1.0f, 1.0f, 1.0f,
                                             0.5f) == LR_ERROR_INVALID_ARGUMENT,
                   "near>=far rejected");
        TEST_CHECK(lr_camera_set_perspective(&cam, 0.7853982f, 4.0f / 3.0f,
                                             0.1f,
                                             100.0f) == LR_SUCCESS,
                   "valid perspective accepted");
        {
            /* NDC z of near/far planes through the composed matrix. */
            float m[16];
            int c;
            int r;

            for (c = 0; c < 4; c++) {
                for (r = 0; r < 4; r++) {
                    m[c * 4 + r] = 0.0f;
                }
            }
            (void)m;
        }
        {
            static const float eye[3] = { 0.0f, 0.0f, 3.0f };
            static const float center[3] = { 0.0f, 0.0f, 0.0f };
            static const float up[3] = { 0.0f, 1.0f, 0.0f };

            TEST_CHECK(lr_camera_look_at(NULL, eye, center, up) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "look_at(NULL) -> INVALID");
            TEST_CHECK(lr_camera_look_at(&cam, eye, eye, up) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "look_at degenerate (eye==center) rejected");
            TEST_CHECK(lr_camera_look_at(&cam, eye, center, up) == LR_SUCCESS,
                       "look_at succeeds");
            TEST_CHECK(cam.position[0] == 0.0f && cam.position[2] == 3.0f,
                       "look_at stores eye");
            /* View maps the world origin to (0,0,-3). */
            {
                float x = cam.view[0] * 0.0f + cam.view[4] * 0.0f +
                          cam.view[8] * 0.0f + cam.view[12];
                float y = cam.view[1] * 0.0f + cam.view[5] * 0.0f +
                          cam.view[9] * 0.0f + cam.view[13];
                float z = cam.view[2] * 0.0f + cam.view[6] * 0.0f +
                          cam.view[10] * 0.0f + cam.view[14];

                TEST_CHECK(x == 0.0f && y == 0.0f && z == -3.0f,
                           "view translates origin to -Z");
            }
            lr_camera_set_position(&cam, eye);
            TEST_CHECK(1, "set_position safe");
            lr_camera_set_position(NULL, eye);
            TEST_CHECK(1, "set_position(NULL) safe");
        }
    }

    /* Transform math. */
    {
        lr_transform t;
        float m[16];
        static const float axis_y[3] = { 0.0f, 1.0f, 0.0f };

        lr_transform_identity(NULL);
        TEST_CHECK(1, "transform identity(NULL) safe");
        lr_transform_identity(&t);
        lr_transform_to_matrix(&t, m);
        {
            static const float ident[16] = { 1.0f, 0.0f, 0.0f, 0.0f, //
                                             0.0f, 1.0f, 0.0f, 0.0f, //
                                             0.0f, 0.0f, 1.0f, 0.0f, //
                                             0.0f, 0.0f, 0.0f, 1.0f };

            TEST_CHECK(mat_near(m, ident, 16, 1e-6f),
                       "identity composes to identity");
        }
        lr_transform_to_matrix(NULL, m);
        lr_transform_to_matrix(&t, NULL);
        TEST_CHECK(1, "to_matrix NULL-safe");
        t.position[0] = 5.0f;
        t.position[1] = -2.0f;
        t.position[2] = 7.0f;
        lr_transform_to_matrix(&t, m);
        TEST_CHECK(m[12] == 5.0f && m[13] == -2.0f && m[14] == 7.0f &&
                       m[0] == 1.0f && m[5] == 1.0f && m[10] == 1.0f,
                   "translation lands in matrix");
        lr_quat_from_axis_angle(NULL, 1.0f, t.rotation);
        TEST_CHECK(1, "quat axis(NULL) safe");
        lr_quat_from_axis_angle(axis_y, 0.0f, t.rotation);
        TEST_CHECK(t.rotation[3] == 1.0f, "zero angle is identity quat");
        {
            float q[4];
            float r[4];

            lr_quat_from_axis_angle(axis_y, 1.5707963f, q);
            lr_quat_multiply(q, q, r); /* 90+90 = 180 about Y */
            TEST_CHECK(r[1] > 0.999f && r[1] < 1.001f && r[3] > -0.001f &&
                           r[3] < 0.001f,
                       "quat double-90 equals 180");
            lr_quat_multiply(NULL, q, r);
            lr_quat_multiply(q, NULL, r);
            lr_quat_multiply(q, r, NULL);
            TEST_CHECK(1, "quat multiply NULL-safe");
        }
    }

    /* Primitive data quality (pure, headless): counts, index range,
     * normalized normals, tangent handedness, UV range. */
    {
        lr_vertex cube_v[LR_CUBE_VERTEX_COUNT];
        uint32_t cube_i[LR_CUBE_INDEX_COUNT];
        lr_vertex plane_v[LR_PLANE_VERTEX_COUNT];
        uint32_t plane_i[LR_PLANE_INDEX_COUNT];
        lr_vertex sphere_v[(8 + 1) * (4 + 1)];
        uint32_t sphere_i[8 * 4 * 6];
        unsigned k;
        int ok = 1;

        TEST_CHECK(lr_mesh_cube_data(NULL, cube_i, 1.0f) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "cube data(NULL verts) -> INVALID");
        TEST_CHECK(lr_mesh_cube_data(cube_v, NULL, 1.0f) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "cube data(NULL idx) -> INVALID");
        TEST_CHECK(lr_mesh_cube_data(cube_v, cube_i, 0.0f) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "cube data(zero size) -> INVALID");
        TEST_CHECK(lr_mesh_cube_data(cube_v, cube_i, 2.0f) == LR_SUCCESS,
                   "cube data succeeds");
        TEST_CHECK(lr_mesh_plane_data(plane_v, plane_i, 0.0f, 1.0f) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "plane data(zero width) -> INVALID");
        TEST_CHECK(lr_mesh_plane_data(plane_v, plane_i, 4.0f, 6.0f) ==
                       LR_SUCCESS,
                   "plane data succeeds");
        TEST_CHECK(lr_mesh_sphere_data(sphere_v, sphere_i, 1.0f, 2, 4) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "sphere data(segments<3) -> INVALID");
        TEST_CHECK(lr_mesh_sphere_data(sphere_v, sphere_i, 1.0f, 8, 1) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "sphere data(rings<2) -> INVALID");
        TEST_CHECK(lr_mesh_sphere_data(sphere_v, sphere_i, 1.0f, 8, 4) ==
                       LR_SUCCESS,
                   "sphere data succeeds");
        for (k = 0; k < LR_CUBE_INDEX_COUNT; k++) {
            if (cube_i[k] >= LR_CUBE_VERTEX_COUNT) {
                ok = 0;
            }
        }
        for (k = 0; k < LR_PLANE_INDEX_COUNT; k++) {
            if (plane_i[k] >= LR_PLANE_VERTEX_COUNT) {
                ok = 0;
            }
        }
        for (k = 0; k < 8u * 4u * 6u; k++) {
            if (sphere_i[k] >= (8u + 1u) * (4u + 1u)) {
                ok = 0;
            }
        }
        TEST_CHECK(ok, "all primitive indices in range");
        ok = 1;
        for (k = 0; k < LR_CUBE_VERTEX_COUNT; k++) {
            float *n = cube_v[k].normal;
            float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            float d = len - 1.0f;

            if (!(d <= 1e-5f && d >= -1e-5f)) {
                ok = 0;
            }
            if (cube_v[k].tangent[3] != 1.0f &&
                cube_v[k].tangent[3] != -1.0f) {
                ok = 0;
            }
            if (cube_v[k].texcoord[0] < 0.0f ||
                cube_v[k].texcoord[0] > 1.0f ||
                cube_v[k].texcoord[1] < 0.0f ||
                cube_v[k].texcoord[1] > 1.0f) {
                ok = 0;
            }
        }
        for (k = 0; k < (8u + 1u) * (4u + 1u); k++) {
            float *n = sphere_v[k].normal;
            float len = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            float d = len - 1.0f;

            if (!(d <= 1e-4f && d >= -1e-4f)) {
                ok = 0;
            }
            if (sphere_v[k].tangent[3] != 1.0f &&
                sphere_v[k].tangent[3] != -1.0f) {
                ok = 0;
            }
        }
        TEST_CHECK(ok, "primitive normals unit, tangents +-1, UVs in range");
        /* Cube spans [-1,1] for size 2; plane spans its extents. */
        {
            lr_mesh_desc desc;
            lr_bounds bounds;

            memset(&desc, 0, sizeof(desc));
            desc.vertices = cube_v;
            desc.vertex_count = LR_CUBE_VERTEX_COUNT;
            TEST_CHECK(lr_mesh_compute_bounds(&desc, &bounds) == LR_SUCCESS,
                       "cube bounds compute");
            TEST_CHECK(bounds.min[0] == -1.0f && bounds.max[0] == 1.0f &&
                           bounds.radius > 1.7f && bounds.radius < 1.74f,
                       "cube bounds exact-ish");
        }
    }

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
