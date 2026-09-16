/*
 * Headless Luma Engine unit tests (Phase 24).
 *
 * No GPU, no window, no LumaC device, no renderer: engine/world/
 * object/handle/name/enabled/component/transform/hierarchy math,
 * validation, stats, and inspection. Renderer-backed submission
 * lives in test_engine_vulkan; bulk stress in test_engine_stress.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <luma_engine/luma_engine.h>

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

static int float_near(float a, float b, float eps) {
    float d = a - b;

    return (d <= eps && d >= -eps) ? 1 : 0;
}

static int mat_near(const float *a, const float *b, int n, float eps) {
    int i;

    for (i = 0; i < n; i++) {
        if (!float_near(a[i], b[i], eps)) {
            return 0;
        }
    }
    return 1;
}

static int make_engine_world(le_engine **engine, le_world **world) {
    le_engine_desc edesc;
    le_world_desc wdesc;

    memset(&edesc, 0, sizeof(edesc));
    memset(&wdesc, 0, sizeof(wdesc));
    if (le_engine_create(&edesc, engine) != LE_SUCCESS) {
        return 0;
    }
    if (le_world_create(*engine, &wdesc, world) != LE_SUCCESS) {
        le_engine_destroy(*engine);
        *engine = NULL;
        return 0;
    }
    return 1;
}

int main(void) {
    printf("Running Luma Engine headless unit tests...\n");

    /* NULL-safety first. */
    TEST_CHECK(le_engine_create(NULL, NULL) == LE_ERROR_INVALID_ARGUMENT,
               "engine create all-NULL -> INVALID");
    {
        le_engine *out = (le_engine *)0x1;

        TEST_CHECK(le_engine_create(NULL, &out) == LE_SUCCESS &&
                       out != NULL,
                   "engine create NULL desc succeeds");
        TEST_CHECK(le_engine_get_renderer(NULL) == NULL,
                   "engine renderer(NULL) -> NULL");
        TEST_CHECK(le_engine_get_renderer(out) == NULL,
                   "engine without renderer borrows NULL");
        le_engine_destroy(out);
    }
    le_engine_destroy(NULL);
    TEST_CHECK(1, "engine destroy(NULL) safe");
    TEST_CHECK(le_world_create(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "world create all-NULL -> INVALID");
    le_world_destroy(NULL);
    TEST_CHECK(1, "world destroy(NULL) safe");
    TEST_CHECK(le_world_get_engine(NULL) == NULL,
               "world engine(NULL) -> NULL");
    TEST_CHECK(le_world_get_object_count(NULL) == 0,
               "object count(NULL) -> 0");
    TEST_CHECK(le_world_get_object_capacity(NULL) == 0,
               "object capacity(NULL) -> 0");
    TEST_CHECK(le_object_create(NULL, NULL) == LE_ERROR_INVALID_ARGUMENT,
               "object create all-NULL -> INVALID");
    TEST_CHECK(le_object_destroy(NULL, NULL) == LE_ERROR_INVALID_ARGUMENT,
               "object destroy NULL world -> INVALID");
    TEST_CHECK(le_object_destroy(NULL, &LE_OBJECT_INVALID) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "object destroy NULL world + invalid -> INVALID");
    TEST_CHECK(!le_object_is_valid(NULL), "is_valid(NULL) -> 0");
    TEST_CHECK(!le_object_is_valid(&LE_OBJECT_INVALID),
               "invalid encoding not valid");
    {
        le_object zero = { 0, 0, 0 };

        TEST_CHECK(!le_object_is_valid(&zero), "{0,0,0} not valid");
        TEST_CHECK(!le_object_is_alive(NULL, NULL),
                   "is_alive all-NULL -> 0");
        TEST_CHECK(le_object_stable_id(NULL, NULL) == 0,
                   "stable_id all-NULL -> 0");
    }
    TEST_CHECK(le_object_set_name(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "set_name all-NULL -> INVALID");
    TEST_CHECK(le_object_get_name(NULL, NULL) == NULL,
               "get_name all-NULL -> NULL");
    TEST_CHECK(le_object_set_enabled(NULL, NULL, 1) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "set_enabled all-NULL -> INVALID");
    TEST_CHECK(le_object_is_enabled(NULL, NULL) == 0,
               "is_enabled all-NULL -> 0");
    TEST_CHECK(le_object_is_effectively_enabled(NULL, NULL) == 0,
               "effective all-NULL -> 0");
    {
        float v3[3] = { 9.0f, 9.0f, 9.0f };
        float q[4] = { 9.0f, 9.0f, 9.0f, 9.0f };
        float m[16];

        for (int i = 0; i < 16; i++) {
            m[i] = 9.0f;
        }
        le_object_get_position(NULL, NULL, v3);
        TEST_CHECK(v3[0] == 0.0f && v3[1] == 0.0f && v3[2] == 0.0f,
                   "get_position NULL zeroes");
        le_object_get_position(NULL, NULL, NULL);
        TEST_CHECK(le_object_set_position(NULL, NULL, NULL) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "set_position all-NULL -> INVALID");
        le_object_get_rotation(NULL, NULL, q);
        TEST_CHECK(q[3] == 1.0f && q[0] == 0.0f,
                   "get_rotation NULL identity");
        TEST_CHECK(le_object_set_rotation(NULL, NULL, NULL) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "set_rotation all-NULL -> INVALID");
        le_object_get_scale(NULL, NULL, v3);
        TEST_CHECK(v3[0] == 1.0f && v3[1] == 1.0f && v3[2] == 1.0f,
                   "get_scale NULL ones");
        TEST_CHECK(le_object_set_scale(NULL, NULL, NULL) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "set_scale all-NULL -> INVALID");
        le_object_get_local_matrix(NULL, NULL, m);
        {
            float ident[16];

            le_mat4_identity(ident);
            TEST_CHECK(mat_near(m, ident, 16, 0.0f),
                       "get_local_matrix NULL identity");
        }
        le_object_get_world_matrix(NULL, NULL, m);
        {
            float ident[16];

            le_mat4_identity(ident);
            TEST_CHECK(mat_near(m, ident, 16, 0.0f),
                       "get_world_matrix NULL identity");
        }
        le_mat4_identity(NULL);
        le_mat4_multiply(NULL, NULL, NULL);
        le_transform_compose(NULL, NULL, NULL, NULL);
        le_quat_normalize(NULL, NULL);
        le_quat_multiply(NULL, NULL, NULL);
        le_quat_from_axis_angle(NULL, 0.0f, NULL);
        TEST_CHECK(le_matrix_is_mirrored(NULL) == 0,
                   "mirrored(NULL) -> 0");
        TEST_CHECK(1, "math NULL-safe no-ops");
    }
    TEST_CHECK(le_object_set_parent(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "set_parent all-NULL -> INVALID");
    TEST_CHECK(le_object_get_parent(NULL, NULL, NULL) == 0,
               "get_parent all-NULL -> 0");
    TEST_CHECK(le_object_get_child_count(NULL, NULL) == 0,
               "child count all-NULL -> 0");
    TEST_CHECK(le_object_get_children(NULL, NULL, NULL, 0, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "get_children all-NULL -> INVALID");
    TEST_CHECK(le_world_get_root_count(NULL) == 0,
               "root count(NULL) -> 0");
    TEST_CHECK(le_object_has_component(NULL, NULL, LE_COMPONENT_TRANSFORM) ==
                   0,
               "has_component all-NULL -> 0");
    TEST_CHECK(le_object_add_renderable(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "add_renderable all-NULL -> INVALID");
    TEST_CHECK(le_object_remove_renderable(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "remove_renderable all-NULL -> INVALID");
    TEST_CHECK(le_object_get_renderable(NULL, NULL, NULL) == 0,
               "get_renderable all-NULL -> 0");
    le_camera_desc_default(NULL);
    TEST_CHECK(1, "camera_desc_default(NULL) safe");
    TEST_CHECK(le_object_add_camera(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "add_camera all-NULL -> INVALID");
    TEST_CHECK(le_object_remove_camera(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "remove_camera all-NULL -> INVALID");
    TEST_CHECK(le_object_get_camera(NULL, NULL, NULL) == 0,
               "get_camera all-NULL -> 0");
    TEST_CHECK(le_world_set_active_camera(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "set_active_camera NULL world -> INVALID");
    TEST_CHECK(le_world_get_active_camera(NULL, NULL) == 0,
               "get_active_camera all-NULL -> 0");
    TEST_CHECK(le_object_add_light(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "add_light all-NULL -> INVALID");
    TEST_CHECK(le_object_remove_light(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "remove_light all-NULL -> INVALID");
    TEST_CHECK(le_object_get_light(NULL, NULL, NULL) == 0,
               "get_light all-NULL -> 0");
    TEST_CHECK(le_world_update(NULL, 0.0f) == LE_ERROR_INVALID_ARGUMENT,
               "update NULL world -> INVALID");
    {
        le_extraction_counts counts;

        memset(&counts, 0xFF, sizeof(counts));
        le_world_get_extraction_counts(NULL, &counts);
        TEST_CHECK(counts.renderables == 0 && counts.lights == 0 &&
                       counts.has_camera == 0,
                   "extraction counts(NULL) zeroed");
        le_world_get_extraction_counts(NULL, NULL);
        TEST_CHECK(1, "extraction counts(NULL,NULL) safe");
    }
    TEST_CHECK(le_world_extract_renderables(NULL, NULL, 0, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "extract NULL world -> INVALID");
    TEST_CHECK(le_world_render(NULL, NULL, NULL, 0, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "render all-NULL -> INVALID");
    {
        le_render_report report;

        memset(&report, 0xFF, sizeof(report));
        le_world_get_last_render_report(NULL, &report);
        TEST_CHECK(report.submitted == 0,
                   "last report(NULL) zeroed");
        le_world_get_last_render_report(NULL, NULL);
        TEST_CHECK(1, "last report(NULL,NULL) safe");
    }
    le_object_get_info(NULL, NULL, NULL);
    TEST_CHECK(1, "get_info all-NULL safe");
    {
        le_object_info info;

        memset(&info, 0xFF, sizeof(info));
        le_object_get_info(NULL, NULL, &info);
        TEST_CHECK(info.alive == 0 && info.name != NULL &&
                       info.name[0] == '\0',
                   "get_info(NULL) zeroed with empty name");
    }
    {
        le_world_stats stats;

        memset(&stats, 0xFF, sizeof(stats));
        le_world_get_stats(NULL, &stats);
        TEST_CHECK(stats.objects_alive == 0 && stats.object_capacity == 0,
                   "world stats(NULL) zeroed");
        le_world_get_stats(NULL, NULL);
        TEST_CHECK(1, "world stats(NULL,NULL) safe");
    }
    {
        le_memory_stats mem;

        memset(&mem, 0xFF, sizeof(mem));
        le_world_get_memory_stats(NULL, &mem);
        TEST_CHECK(mem.total_bytes == 0, "memory stats(NULL) zeroed");
        le_world_get_memory_stats(NULL, NULL);
        TEST_CHECK(1, "memory stats(NULL,NULL) safe");
    }

    /* Engine/world lifecycle. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;

        TEST_CHECK(make_engine_world(&engine, &world),
                   "engine + world create");
        TEST_CHECK(le_world_get_engine(world) == engine,
                   "world borrows engine");
        TEST_CHECK(le_world_get_object_count(world) == 0,
                   "new world empty");
        TEST_CHECK(le_world_get_object_capacity(world) >= 64u,
                   "new world default capacity");
        TEST_CHECK(le_world_get_root_count(world) == 0,
                   "new world no roots");
        {
            le_world_stats stats;

            le_world_get_stats(world, &stats);
            TEST_CHECK(stats.objects_alive == 0 && stats.time == 0.0,
                       "new world stats sane");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
        TEST_CHECK(1, "engine/world destroy clean");
    }

    /* Object creation: defaults (identity transform, enabled,
     * root, unnamed, transform-only). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;

        TEST_CHECK(make_engine_world(&engine, &world), "setup");
        TEST_CHECK(le_object_create(world, &a) == LE_SUCCESS,
                   "object create succeeds");
        TEST_CHECK(le_object_is_valid(&a), "handle well-formed");
        TEST_CHECK(le_object_is_alive(world, &a), "handle alive");
        TEST_CHECK(le_world_get_object_count(world) == 1,
                   "count 1 after create");
        TEST_CHECK(le_world_get_root_count(world) == 1,
                   "new object is a root");
        TEST_CHECK(le_object_is_enabled(world, &a) == 1,
                   "new object enabled");
        TEST_CHECK(le_object_is_effectively_enabled(world, &a) == 1,
                   "new object effectively enabled");
        TEST_CHECK(le_object_get_name(world, &a) != NULL &&
                       le_object_get_name(world, &a)[0] == '\0',
                   "new object unnamed");
        TEST_CHECK(le_object_has_component(world, &a,
                                           LE_COMPONENT_TRANSFORM) == 1,
                   "universal transform present");
        TEST_CHECK(le_object_has_component(world, &a,
                                           LE_COMPONENT_RENDERABLE) == 0,
                   "no renderable initially");
        {
            float p[3];
            float q[4];
            float s[3];
            float m[16];
            float ident[16];

            le_object_get_position(world, &a, p);
            le_object_get_rotation(world, &a, q);
            le_object_get_scale(world, &a, s);
            TEST_CHECK(p[0] == 0.0f && p[1] == 0.0f && p[2] == 0.0f,
                       "default position zero");
            TEST_CHECK(q[0] == 0.0f && q[1] == 0.0f && q[2] == 0.0f &&
                           q[3] == 1.0f,
                       "default rotation identity");
            TEST_CHECK(s[0] == 1.0f && s[1] == 1.0f && s[2] == 1.0f,
                       "default scale one");
            le_mat4_identity(ident);
            le_object_get_local_matrix(world, &a, m);
            TEST_CHECK(mat_near(m, ident, 16, 0.0f),
                       "default local matrix identity");
            le_object_get_world_matrix(world, &a, m);
            TEST_CHECK(mat_near(m, ident, 16, 0.0f),
                       "default world matrix identity");
        }
        {
            le_object_info info;

            le_object_get_info(world, &a, &info);
            TEST_CHECK(info.alive && info.enabled &&
                           info.effectively_enabled &&
                           info.has_transform && !info.has_parent &&
                           info.child_count == 0,
                       "object info snapshot sane");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Slot reuse + stale handles (REQUIRED Part 9 semantics). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;
        float p[3] = { 1.0f, 2.0f, 3.0f };

        TEST_CHECK(make_engine_world(&engine, &world), "setup reuse");
        TEST_CHECK(le_object_create(world, &a) == LE_SUCCESS, "create A");
        TEST_CHECK(le_object_set_position(world, &a, p) == LE_SUCCESS,
                   "position A");
        TEST_CHECK(le_object_destroy(world, &a) == LE_SUCCESS,
                   "destroy A");
        TEST_CHECK(!le_object_is_alive(world, &a), "A dead after destroy");
        TEST_CHECK(le_object_stable_id(world, &a) == 0,
                   "A stable_id retired");
        TEST_CHECK(le_object_create(world, &b) == LE_SUCCESS, "create B");
        TEST_CHECK(b.index == a.index, "B reuses A slot");
        TEST_CHECK(b.generation != a.generation, "generation bumped");
        TEST_CHECK(le_object_is_alive(world, &b), "B alive");
        TEST_CHECK(!le_object_is_alive(world, &a), "A still dead");
        /* Every operation using A fails (stale), none touches B. */
        TEST_CHECK(le_object_destroy(world, &a) == LE_ERROR_STALE_HANDLE,
                   "destroy stale A -> STALE");
        TEST_CHECK(le_object_set_position(world, &a, p) ==
                       LE_ERROR_STALE_HANDLE,
                   "set_position stale A -> STALE");
        {
            float m[16];
            float ident[16];

            le_mat4_identity(ident);
            le_object_get_world_matrix(world, &a, m);
            TEST_CHECK(mat_near(m, ident, 16, 0.0f),
                       "stale world matrix reads identity");
        }
        TEST_CHECK(le_object_set_enabled(world, &a, 0) ==
                       LE_ERROR_STALE_HANDLE,
                   "set_enabled stale A -> STALE");
        TEST_CHECK(le_object_is_enabled(world, &a) == 0,
                   "is_enabled stale A -> 0");
        TEST_CHECK(le_object_set_name(world, &a, "ghost") ==
                       LE_ERROR_STALE_HANDLE,
                   "set_name stale A -> STALE");
        TEST_CHECK(le_object_get_name(world, &a) == NULL,
                   "get_name stale A -> NULL");
        TEST_CHECK(le_object_set_parent(world, &a, &b) ==
                       LE_ERROR_STALE_HANDLE,
                   "set_parent stale child -> STALE");
        TEST_CHECK(le_object_add_renderable(world, &a, NULL) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "add_renderable stale+NULL desc -> INVALID first");
        {
            le_renderable_desc rd;

            memset(&rd, 0, sizeof(rd));
            TEST_CHECK(le_object_add_renderable(world, &a, &rd) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "add_renderable NULL mesh -> INVALID");
        }
        TEST_CHECK(le_object_get_child_count(world, &a) == 0,
                   "child count stale -> 0");
        /* B is unaffected and usable. */
        TEST_CHECK(le_object_set_position(world, &b, p) == LE_SUCCESS,
                   "B usable after reuse");
        TEST_CHECK(le_object_destroy(world, &b) == LE_SUCCESS,
                   "destroy B clean");
        TEST_CHECK(le_world_get_object_count(world) == 0,
                   "world empty after destroys");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Generation wrap policy: 0 skipped forever. */
    {
        le_object invalid = LE_OBJECT_INVALID;

        TEST_CHECK(invalid.generation == 0 && invalid.world_tag == 0,
                   "invalid encoding uses generation 0 + tag 0");
        TEST_CHECK(!le_object_is_valid(&invalid),
                   "invalid encoding rejected by is_valid");
    }

    /* Cross-world misuse (Part 66): world tags make confusion
     * STRUCTURAL (not probabilistic). World B is forced to hold a
     * live object at the same {index,generation} — then a handle
     * from A must STILL fail in B with WRONG_WORLD. */
    {
        le_engine *engine = NULL;
        le_world *wa = NULL;
        le_world *wb = NULL;
        le_engine_desc edesc;
        le_world_desc wdesc;
        le_object a;
        le_object b;
        float p[3] = { 5.0f, 0.0f, 0.0f };

        memset(&edesc, 0, sizeof(edesc));
        memset(&wdesc, 0, sizeof(wdesc));
        TEST_CHECK(le_engine_create(&edesc, &engine) == LE_SUCCESS,
                   "engine for xworld");
        TEST_CHECK(le_world_create(engine, &wdesc, &wa) == LE_SUCCESS,
                   "world A");
        TEST_CHECK(le_world_create(engine, &wdesc, &wb) == LE_SUCCESS,
                   "world B");
        TEST_CHECK(le_object_create(wa, &a) == LE_SUCCESS, "create in A");
        TEST_CHECK(le_object_create(wb, &b) == LE_SUCCESS, "create in B");
        /* Force the alias: both worlds allocate slot 0 first, so
         * {index,generation} collide — only the tag differs. */
        TEST_CHECK(a.index == b.index && a.generation == b.generation,
                   "xworld slot collision forced");
        TEST_CHECK(a.world_tag != b.world_tag,
                   "world tags differ");
        TEST_CHECK(le_object_is_alive(wa, &a) && le_object_is_alive(wb, &b),
                   "both handles live at home");
        TEST_CHECK(!le_object_is_alive(wb, &a),
                   "A handle not alive in B despite slot collision");
        TEST_CHECK(!le_object_is_alive(wa, &b),
                   "B handle not alive in A despite slot collision");
        TEST_CHECK(le_object_set_position(wb, &a, p) ==
                       LE_ERROR_WRONG_WORLD,
                   "xworld set_position -> WRONG_WORLD");
        TEST_CHECK(le_object_destroy(wb, &a) == LE_ERROR_WRONG_WORLD,
                   "xworld destroy -> WRONG_WORLD");
        TEST_CHECK(le_object_set_enabled(wb, &a, 0) ==
                       LE_ERROR_WRONG_WORLD,
                   "xworld set_enabled -> WRONG_WORLD");
        TEST_CHECK(le_object_set_name(wb, &a, "x") ==
                       LE_ERROR_WRONG_WORLD,
                   "xworld set_name -> WRONG_WORLD");
        TEST_CHECK(le_object_set_parent(wb, &b, &a) ==
                       LE_ERROR_WRONG_WORLD,
                   "xworld set_parent foreign parent -> WRONG_WORLD");
        TEST_CHECK(le_object_set_parent(wb, &a, &b) ==
                       LE_ERROR_WRONG_WORLD,
                   "xworld set_parent foreign child -> WRONG_WORLD");
        /* World-local ops on the right world still work. */
        TEST_CHECK(le_object_set_position(wa, &a, p) == LE_SUCCESS,
                   "home world op succeeds");
        TEST_CHECK(le_object_set_position(wb, &b, p) == LE_SUCCESS,
                   "other home world op succeeds");
        /* Stable IDs decorrelate across worlds via salt+tag. */
        {
            uint64_t ida = le_object_stable_id(wa, &a);
            uint64_t idb = le_object_stable_id(wb, &b);

            TEST_CHECK(ida != 0 && idb != 0 && ida != idb,
                       "stable IDs nonzero + cross-world distinct");
            /* A foreign handle yields no key in the wrong world. */
            TEST_CHECK(le_object_stable_id(wb, &a) == 0,
                       "foreign stable_id -> 0");
        }
        le_world_destroy(wa);
        le_world_destroy(wb);
        le_engine_destroy(engine);
    }

    /* Names: ownership, duplicates, clearing. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;

        TEST_CHECK(make_engine_world(&engine, &world), "setup names");
        TEST_CHECK(le_object_create(world, &a) == LE_SUCCESS, "create a");
        TEST_CHECK(le_object_create(world, &b) == LE_SUCCESS, "create b");
        TEST_CHECK(le_object_set_name(world, &a, "Player") == LE_SUCCESS,
                   "set name a");
        TEST_CHECK(strcmp(le_object_get_name(world, &a), "Player") == 0,
                   "get name a exact");
        /* Caller bytes may be transient (world copies). */
        {
            char transient[16];

            strcpy(transient, "Transient");
            TEST_CHECK(le_object_set_name(world, &b, transient) ==
                           LE_SUCCESS,
                       "set transient name");
            memset(transient, 0, sizeof(transient));
            TEST_CHECK(strcmp(le_object_get_name(world, &b),
                              "Transient") == 0,
                       "world kept its own copy");
        }
        /* Duplicates allowed: identity never depends on names. */
        TEST_CHECK(le_object_set_name(world, &b, "Player") == LE_SUCCESS,
                   "duplicate name allowed");
        TEST_CHECK(le_object_is_alive(world, &a) &&
                       le_object_is_alive(world, &b),
                   "duplicates stay distinct objects");
        /* Clearing. */
        TEST_CHECK(le_object_set_name(world, &a, NULL) == LE_SUCCESS,
                   "clear name with NULL");
        TEST_CHECK(le_object_get_name(world, &a)[0] == '\0',
                   "cleared name reads empty");
        TEST_CHECK(le_object_set_name(world, &b, "") == LE_SUCCESS,
                   "clear name with empty");
        {
            le_world_stats stats;

            le_world_get_stats(world, &stats);
            TEST_CHECK(stats.named_objects == 0,
                       "named count tracks clearing");
        }
        TEST_CHECK(le_object_set_name(world, &b, "Kept") == LE_SUCCESS,
                   "rename b");
        TEST_CHECK(le_object_destroy(world, &b) == LE_SUCCESS,
                   "destroy named object");
        {
            le_world_stats stats;

            le_world_get_stats(world, &stats);
            TEST_CHECK(stats.named_objects == 0,
                       "named count tracks destruction");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Enabled state + hierarchical (Godot-like) effective state. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object parent;
        le_object child;

        TEST_CHECK(make_engine_world(&engine, &world), "setup enabled");
        TEST_CHECK(le_object_create(world, &parent) == LE_SUCCESS,
                   "create parent");
        TEST_CHECK(le_object_create(world, &child) == LE_SUCCESS,
                   "create child");
        TEST_CHECK(le_object_set_parent(world, &child, &parent) ==
                       LE_SUCCESS,
                   "attach child");
        TEST_CHECK(le_object_is_effectively_enabled(world, &child) == 1,
                   "child effectively enabled");
        TEST_CHECK(le_object_set_enabled(world, &parent, 0) == LE_SUCCESS,
                   "disable parent");
        TEST_CHECK(le_object_is_enabled(world, &parent) == 0,
                   "parent stored flag off");
        TEST_CHECK(le_object_is_enabled(world, &child) == 1,
                   "child stored flag still on");
        TEST_CHECK(le_object_is_effectively_enabled(world, &child) == 0,
                   "child effectively disabled via parent");
        TEST_CHECK(le_object_set_enabled(world, &child, 0) == LE_SUCCESS,
                   "disable child directly");
        TEST_CHECK(le_object_set_enabled(world, &parent, 1) == LE_SUCCESS,
                   "re-enable parent");
        TEST_CHECK(le_object_is_effectively_enabled(world, &child) == 0,
                   "child stays off via own flag");
        TEST_CHECK(le_object_set_enabled(world, &child, 1) == LE_SUCCESS,
                   "re-enable child");
        TEST_CHECK(le_object_is_effectively_enabled(world, &child) == 1,
                   "subtree enabled again");
        {
            le_world_stats stats;

            le_world_get_stats(world, &stats);
            TEST_CHECK(stats.enabled_objects == 2 &&
                           stats.disabled_objects == 0,
                       "enabled counters exact");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Transform setters/getters, quaternion normalization, matrix
     * convention (T*R*S column-major, renderer-compatible). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;

        TEST_CHECK(make_engine_world(&engine, &world), "setup xform");
        TEST_CHECK(le_object_create(world, &o) == LE_SUCCESS, "create o");
        {
            float p[3] = { 1.0f, -2.0f, 3.5f };

            TEST_CHECK(le_object_set_position(world, &o, p) == LE_SUCCESS,
                       "set position");
            {
                float got[3];

                le_object_get_position(world, &o, got);
                TEST_CHECK(mat_near(got, p, 3, 0.0f),
                           "position round-trips");
            }
        }
        {
            float bad[3] = { 1.0f, 1.0f, (float)INFINITY };

            TEST_CHECK(le_object_set_position(world, &o, bad) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "non-finite position rejected");
        }
        {
            /* 90 deg about Y: (0, sin45, 0, cos45). */
            float q[4] = { 0.0f, 0.7071068f, 0.0f, 0.7071068f };

            TEST_CHECK(le_object_set_rotation(world, &o, q) == LE_SUCCESS,
                       "set rotation");
            {
                float got[4];
                float len;

                le_object_get_rotation(world, &o, got);
                len = sqrtf(got[0] * got[0] + got[1] * got[1] +
                            got[2] * got[2] + got[3] * got[3]);
                TEST_CHECK(float_near(len, 1.0f, 1e-5f),
                           "stored rotation unit length");
            }
        }
        {
            /* Non-unit quaternions normalize on store. */
            float big[4] = { 0.0f, 2.0f, 0.0f, 0.0f };
            float got[4];

            TEST_CHECK(le_object_set_rotation(world, &o, big) ==
                           LE_SUCCESS,
                       "set non-unit rotation");
            le_object_get_rotation(world, &o, got);
            TEST_CHECK(float_near(got[1], 1.0f, 1e-5f) &&
                           float_near(got[3], 0.0f, 1e-5f),
                       "rotation normalized on store");
        }
        {
            /* Zero-length restores identity. */
            float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            float got[4];

            TEST_CHECK(le_object_set_rotation(world, &o, zero) ==
                           LE_SUCCESS,
                       "set zero rotation");
            le_object_get_rotation(world, &o, got);
            TEST_CHECK(got[3] == 1.0f && got[0] == 0.0f,
                       "zero rotation -> identity");
        }
        {
            float s[3] = { 2.0f, -1.0f, 0.5f };

            TEST_CHECK(le_object_set_scale(world, &o, s) == LE_SUCCESS,
                       "set scale incl. mirror");
            {
                float got[3];

                le_object_get_scale(world, &o, got);
                TEST_CHECK(mat_near(got, s, 3, 0.0f),
                           "scale round-trips");
            }
        }
        /* Local matrix: pure translation composes exactly. */
        {
            float p[3] = { 4.0f, 5.0f, 6.0f };
            float ident_q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            float one[3] = { 1.0f, 1.0f, 1.0f };
            float m[16];

            le_object_set_position(world, &o, p);
            le_object_set_rotation(world, &o, ident_q);
            le_object_set_scale(world, &o, one);
            le_object_get_local_matrix(world, &o, m);
            TEST_CHECK(m[12] == 4.0f && m[13] == 5.0f && m[14] == 6.0f &&
                           m[0] == 1.0f && m[5] == 1.0f &&
                           m[10] == 1.0f && m[15] == 1.0f,
                       "translation-only local matrix exact");
        }
        /* Local matrix: 90-deg Y rotation maps +X to -Z
         * (column-major T*R*S, renderer convention). */
        {
            float q[4] = { 0.0f, 0.7071068f, 0.0f, 0.7071068f };
            float zero[3] = { 0.0f, 0.0f, 0.0f };
            float one[3] = { 1.0f, 1.0f, 1.0f };
            float m[16];

            le_object_set_position(world, &o, zero);
            le_object_set_rotation(world, &o, q);
            le_object_set_scale(world, &o, one);
            le_object_get_local_matrix(world, &o, m);
            /* Column 0 (image of +X) ~ (0,0,-1). */
            TEST_CHECK(float_near(m[0], 0.0f, 1e-5f) &&
                           float_near(m[1], 0.0f, 1e-5f) &&
                           float_near(m[2], -1.0f, 1e-5f),
                       "Y-90 rotation maps +X to -Z");
        }
        /* Mirrored scale reports negative determinant. */
        {
            float ident_q[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            float mir[3] = { -1.0f, 1.0f, 1.0f };
            float m[16];

            le_object_set_rotation(world, &o, ident_q);
            le_object_set_scale(world, &o, mir);
            le_object_get_local_matrix(world, &o, m);
            TEST_CHECK(le_matrix_is_mirrored(m) == 1,
                       "negative scale mirrors");
            {
                float one[3] = { 1.0f, 1.0f, 1.0f };

                le_object_set_scale(world, &o, one);
                le_object_get_local_matrix(world, &o, m);
                TEST_CHECK(le_matrix_is_mirrored(m) == 0,
                           "unit scale not mirrored");
            }
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Quaternion math unit coverage. */
    {
        float q[4];
        float id[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        float x_axis[3] = { 1.0f, 0.0f, 0.0f };

        le_quat_from_axis_angle(x_axis, 0.0f, q);
        TEST_CHECK(mat_near(q, id, 4, 1e-6f),
                   "axis-angle 0 -> identity");
        {
            float y_axis[3] = { 0.0f, 1.0f, 0.0f };
            float half = 3.14159265f * 0.5f;

            le_quat_from_axis_angle(y_axis, half, q);
            TEST_CHECK(float_near(q[1], 0.7071068f, 1e-5f) &&
                           float_near(q[3], 0.7071068f, 1e-5f),
                       "axis-angle Y-90 exact");
        }
        {
            float a[4] = { 0.0f, 0.7071068f, 0.0f, 0.7071068f };
            float out[4];

            /* q * identity == q. */
            le_quat_multiply(a, id, out);
            TEST_CHECK(mat_near(out, a, 4, 1e-5f),
                       "quat * identity == q");
            /* q * q == 180-deg Y == (0,1,0,0). */
            le_quat_multiply(a, a, out);
            TEST_CHECK(float_near(out[1], 1.0f, 1e-4f) &&
                           float_near(out[3], 0.0f, 1e-4f),
                       "quat Y-90 squared == Y-180");
        }
        {
            float m[16];
            float p[3] = { 0.0f, 0.0f, 0.0f };
            float rot[4] = { 0.0f, 0.7071068f, 0.0f, 0.7071068f };
            float s[3] = { 1.0f, 1.0f, 1.0f };
            float m2[16];
            float composed[16];

            le_transform_compose(p, rot, s, m);
            le_object_get_local_matrix(NULL, NULL, m2);
            TEST_CHECK(m[15] == 1.0f && m[3] == 0.0f,
                       "compose homogeneous row sane");
            /* compose then multiply by identity == itself. */
            le_mat4_identity(m2);
            le_mat4_multiply(composed, m, m2);
            TEST_CHECK(mat_near(composed, m, 16, 1e-6f),
                       "mat * identity == mat");
        }
    }

    /* Hierarchy: attach, world composition, reparenting (local
     * preserved), cycle rejection. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object parent;
        le_object child;
        le_object grandchild;

        TEST_CHECK(make_engine_world(&engine, &world), "setup hier");
        TEST_CHECK(le_object_create(world, &parent) == LE_SUCCESS,
                   "create parent");
        TEST_CHECK(le_object_create(world, &child) == LE_SUCCESS,
                   "create child");
        TEST_CHECK(le_object_create(world, &grandchild) == LE_SUCCESS,
                   "create grandchild");
        {
            float pp[3] = { 10.0f, 0.0f, 0.0f };
            float cp[3] = { 0.0f, 5.0f, 0.0f };

            le_object_set_position(world, &parent, pp);
            le_object_set_position(world, &child, cp);
        }
        TEST_CHECK(le_object_set_parent(world, &child, &parent) ==
                       LE_SUCCESS,
                   "attach child");
        TEST_CHECK(le_object_set_parent(world, &grandchild, &child) ==
                       LE_SUCCESS,
                   "attach grandchild");
        TEST_CHECK(le_object_get_child_count(world, &parent) == 1,
                   "parent has 1 child");
        TEST_CHECK(le_object_get_child_count(world, &child) == 1,
                   "child has 1 child");
        TEST_CHECK(le_world_get_root_count(world) == 1,
                   "one root in chain");
        {
            le_object got;

            TEST_CHECK(le_object_get_parent(world, &child, &got) == 1 &&
                           got.index == parent.index &&
                           got.generation == parent.generation,
                       "get_parent returns parent handle");
            TEST_CHECK(le_object_get_parent(world, &parent, &got) == 0,
                       "root has no parent");
        }
        {
            /* world(child) = world(parent) * local(child):
             * (10,0,0) + (0,5,0) = (10,5,0). */
            float m[16];

            le_object_get_world_matrix(world, &child, m);
            TEST_CHECK(float_near(m[12], 10.0f, 1e-5f) &&
                           float_near(m[13], 5.0f, 1e-5f) &&
                           float_near(m[14], 0.0f, 1e-5f),
                       "world(child) = parent * local");
            le_object_get_world_matrix(world, &grandchild, m);
            TEST_CHECK(float_near(m[12], 10.0f, 1e-5f) &&
                           float_near(m[13], 5.0f, 1e-5f),
                       "grandchild inherits chain");
        }
        /* Moving the parent moves the child's world matrix
         * (dirty propagation) without touching local bytes. */
        {
            float pp2[3] = { 20.0f, 0.0f, 0.0f };
            float local[16];
            float before[16];
            float after[16];

            le_object_get_local_matrix(world, &child, before);
            le_object_set_position(world, &parent, pp2);
            le_object_get_local_matrix(world, &child, local);
            TEST_CHECK(mat_near(local, before, 16, 0.0f),
                       "parent move preserves child local");
            le_object_get_world_matrix(world, &child, after);
            TEST_CHECK(float_near(after[12], 20.0f, 1e-5f),
                       "child world follows parent");
        }
        /* Reparenting preserves LOCAL (documented policy). */
        {
            le_object other;

            le_object_create(world, &other);
            {
                float op[3] = { 100.0f, 100.0f, 100.0f };

                le_object_set_position(world, &other, op);
            }
            {
                float local_before[16];
                float local_after[16];

                le_object_get_local_matrix(world, &child, local_before);
                TEST_CHECK(le_object_set_parent(world, &child, &other) ==
                               LE_SUCCESS,
                           "reparent child");
                le_object_get_local_matrix(world, &child, local_after);
                TEST_CHECK(mat_near(local_before, local_after, 16, 0.0f),
                           "reparent preserves local");
            }
            le_object_destroy(world, &other);
            /* other is destroyed with its subtree (child +
             * grandchild). Recreate the chain for cycle tests. */
            TEST_CHECK(le_object_create(world, &child) == LE_SUCCESS,
                       "recreate child");
            TEST_CHECK(le_object_create(world, &grandchild) == LE_SUCCESS,
                       "recreate grandchild");
            TEST_CHECK(le_object_set_parent(world, &child, &parent) ==
                           LE_SUCCESS,
                       "reattach child");
            TEST_CHECK(le_object_set_parent(world, &grandchild, &child) ==
                           LE_SUCCESS,
                       "reattach grandchild");
        }
        /* Cycle rejection: self, 2-cycle, 3-cycle. */
        TEST_CHECK(le_object_set_parent(world, &parent, &parent) ==
                       LE_ERROR_CYCLE,
                   "self-parent rejected");
        TEST_CHECK(le_object_set_parent(world, &parent, &child) ==
                       LE_ERROR_CYCLE,
                   "2-cycle rejected");
        TEST_CHECK(le_object_set_parent(world, &parent, &grandchild) ==
                       LE_ERROR_CYCLE,
                   "3-cycle rejected");
        /* Failed reparent leaves the world unchanged. */
        {
            le_object got;

            TEST_CHECK(le_object_get_parent(world, &child, &got) == 1 &&
                           got.index == parent.index,
                       "failed cycle keeps old parent");
        }
        /* Detach to root via NULL / LE_OBJECT_INVALID. */
        TEST_CHECK(le_object_set_parent(world, &child, NULL) == LE_SUCCESS,
                   "detach via NULL");
        TEST_CHECK(le_object_get_child_count(world, &parent) == 0,
                   "parent childless after detach");
        TEST_CHECK(le_object_set_parent(world, &child, &parent) ==
                       LE_SUCCESS,
                   "reattach");
        TEST_CHECK(le_object_set_parent(world, &child,
                                        &LE_OBJECT_INVALID) == LE_SUCCESS,
                   "detach via INVALID");
        TEST_CHECK(le_world_get_root_count(world) == 2,
                   "two roots after detach");
        /* Children listing is deterministic (ascending slot). */
        {
            le_object k1;
            le_object k2;
            le_object kids[4];
            uint32_t count = 0;

            le_object_create(world, &k1);
            le_object_create(world, &k2);
            le_object_set_parent(world, &k1, &parent);
            le_object_set_parent(world, &k2, &parent);
            TEST_CHECK(le_object_get_children(world, &parent, kids, 4,
                                              &count) == LE_SUCCESS &&
                           count == 2,
                       "children count exact");
            TEST_CHECK(kids[0].index < kids[1].index,
                       "children ascending slot order");
            le_object_destroy(world, &k1);
            le_object_destroy(world, &k2);
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Hierarchy destruction default: destroying a parent destroys
     * the whole subtree. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object parent;
        le_object child;
        le_object grandchild;

        TEST_CHECK(make_engine_world(&engine, &world), "setup destroy");
        le_object_create(world, &parent);
        le_object_create(world, &child);
        le_object_create(world, &grandchild);
        le_object_set_parent(world, &child, &parent);
        le_object_set_parent(world, &grandchild, &child);
        TEST_CHECK(le_object_destroy(world, &parent) == LE_SUCCESS,
                   "destroy parent");
        TEST_CHECK(!le_object_is_alive(world, &parent),
                   "parent dead");
        TEST_CHECK(!le_object_is_alive(world, &child),
                   "child dead via subtree");
        TEST_CHECK(!le_object_is_alive(world, &grandchild),
                   "grandchild dead via subtree");
        TEST_CHECK(le_world_get_object_count(world) == 0,
                   "world empty after subtree destroy");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Deep hierarchy is stack-safe (10k chain) with exact tip. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object prev;
        le_object cur;
        int ok = 1;
        const uint32_t depth = 10000u;
        uint32_t i;

        TEST_CHECK(make_engine_world(&engine, &world), "setup deep");
        if (le_object_create(world, &prev) != LE_SUCCESS) {
            ok = 0;
        }
        {
            float step[3] = { 1.0f, 0.0f, 0.0f };

            for (i = 1; i < depth && ok; i++) {
                if (le_object_create(world, &cur) != LE_SUCCESS) {
                    ok = 0;
                    break;
                }
                if (le_object_set_position(world, &cur, step) !=
                        LE_SUCCESS ||
                    le_object_set_parent(world, &cur, &prev) !=
                        LE_SUCCESS) {
                    ok = 0;
                    break;
                }
                prev = cur;
            }
        }
        TEST_CHECK(ok, "10k chain built");
        TEST_CHECK(le_world_update(world, 0.016f) == LE_SUCCESS,
                   "deep update succeeds");
        {
            float m[16];

            le_object_get_world_matrix(world, &prev, m);
            /* Each link offsets +1 in X in its parent frame:
             * tip at x = depth - 1 (root contributes 0). */
            TEST_CHECK(float_near(m[12], (float)(depth - 1u), 0.5f),
                       "deep tip world position exact");
        }
        /* Destroying the root retires the whole chain iteratively. */
        {
            le_object root = prev;

            /* Find the root by walking up. */
            for (i = 0; i < depth; i++) {
                le_object p;

                if (!le_object_get_parent(world, &root, &p)) {
                    break;
                }
                root = p;
            }
            TEST_CHECK(le_object_destroy(world, &root) == LE_SUCCESS,
                       "deep subtree destroy succeeds");
            TEST_CHECK(le_world_get_object_count(world) == 0,
                       "deep destroy retires all");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Components: renderable/camera/light add/remove/get +
     * validation + active camera + deterministic iteration. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;

        TEST_CHECK(make_engine_world(&engine, &world), "setup comps");
        le_object_create(world, &o);
        /* Renderable validation (no renderer needed: mesh/material
         * are borrowed handles validated at sync). */
        {
            le_renderable_desc rd;

            memset(&rd, 0, sizeof(rd));
            TEST_CHECK(le_object_add_renderable(world, &o, &rd) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "renderable NULL mesh rejected");
            rd.mesh = (lr_mesh *)(uintptr_t)0x1234;
            TEST_CHECK(le_object_add_renderable(world, &o, &rd) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "renderable NULL material rejected");
            rd.material = (lr_material *)(uintptr_t)0x5678;
            rd.visible = 1;
            rd.casts_shadow = 1;
            rd.receives_shadow = 1;
            TEST_CHECK(le_object_add_renderable(world, &o, &rd) ==
                           LE_SUCCESS,
                       "renderable add succeeds");
            TEST_CHECK(le_object_has_component(world, &o,
                                               LE_COMPONENT_RENDERABLE) ==
                           1,
                       "renderable presence set");
            {
                le_renderable_desc got;

                TEST_CHECK(le_object_get_renderable(world, &o, &got) ==
                               1,
                           "renderable get present");
                TEST_CHECK(got.mesh == rd.mesh &&
                               got.material == rd.material &&
                               got.visible == 1 && got.casts_shadow == 1,
                           "renderable round-trips");
            }
            /* Replace in place. */
            rd.visible = 0;
            TEST_CHECK(le_object_add_renderable(world, &o, &rd) ==
                           LE_SUCCESS,
                       "renderable replace succeeds");
            {
                le_world_stats stats;

                le_world_get_stats(world, &stats);
                TEST_CHECK(stats.renderables == 1,
                           "replace keeps count 1");
            }
            TEST_CHECK(le_object_remove_renderable(world, &o) ==
                           LE_SUCCESS,
                       "renderable remove succeeds");
            TEST_CHECK(le_object_has_component(world, &o,
                                               LE_COMPONENT_RENDERABLE) ==
                           0,
                       "renderable presence cleared");
            TEST_CHECK(le_object_remove_renderable(world, &o) ==
                           LE_SUCCESS,
                       "double remove is no-op success");
        }
        /* Camera validation + active camera. */
        {
            le_camera_desc cd;

            le_camera_desc_default(&cd);
            TEST_CHECK(le_object_add_camera(world, &o, &cd) == LE_SUCCESS,
                       "camera add succeeds");
            TEST_CHECK(le_object_has_component(world, &o,
                                               LE_COMPONENT_CAMERA) == 1,
                       "camera presence set");
            {
                le_camera_desc bad = cd;

                bad.fov_y_rad = 0.0f;
                TEST_CHECK(le_object_add_camera(world, &o, &bad) ==
                               LE_ERROR_INVALID_ARGUMENT,
                       "bad FOV rejected");
                bad = cd;
                bad.near_plane = 5.0f;
                bad.far_plane = 1.0f;
                TEST_CHECK(le_object_add_camera(world, &o, &bad) ==
                               LE_ERROR_INVALID_ARGUMENT,
                       "near>=far rejected");
            }
            {
                le_camera_desc ortho;

                le_camera_desc_default(&ortho);
                ortho.projection = LE_PROJECTION_ORTHOGRAPHIC;
                TEST_CHECK(le_object_add_camera(world, &o, &ortho) ==
                               LE_SUCCESS,
                       "ortho camera accepted");
                le_object_add_camera(world, &o, &cd);
            }
            {
                le_camera_desc got;

                TEST_CHECK(le_object_get_camera(world, &o, &got) == 1,
                           "camera get present");
                TEST_CHECK(float_near(got.fov_y_rad, cd.fov_y_rad, 1e-6f),
                           "camera round-trips");
            }
            TEST_CHECK(le_world_set_active_camera(world, &o) == LE_SUCCESS,
                       "set active camera");
            {
                le_object active;

                TEST_CHECK(le_world_get_active_camera(world, &active) ==
                               1,
                           "get active camera");
                TEST_CHECK(active.index == o.index &&
                               active.generation == o.generation,
                           "active camera handle exact");
            }
            /* Object without camera cannot become active. */
            {
                le_object plain;

                le_object_create(world, &plain);
                TEST_CHECK(le_world_set_active_camera(world, &plain) ==
                               LE_ERROR_MISSING_COMPONENT,
                           "camera-less active rejected");
                le_object_destroy(world, &plain);
            }
            TEST_CHECK(le_world_set_active_camera(world, NULL) ==
                           LE_SUCCESS,
                       "clear active camera");
            TEST_CHECK(le_world_get_active_camera(world, NULL) == 0,
                       "no active camera after clear");
            TEST_CHECK(le_world_set_active_camera(world, &o) == LE_SUCCESS,
                       "re-set active camera");
            TEST_CHECK(le_object_remove_camera(world, &o) == LE_SUCCESS,
                       "camera remove succeeds");
            TEST_CHECK(le_world_get_active_camera(world, &o) == 0,
                       "active auto-clears on remove");
        }
        /* Light validation (fresh object: prior blocks leave camera
         * state behind on the shared fixture object). */
        {
            le_light_desc ld;
            le_object lo;

            TEST_CHECK(le_object_create(world, &lo) == LE_SUCCESS,
                       "light fixture create");

            memset(&ld, 0, sizeof(ld));
            ld.type = LE_LIGHT_DIRECTIONAL;
            ld.color[0] = 1.0f;
            ld.color[1] = 1.0f;
            ld.color[2] = 1.0f;
            ld.intensity = 3.0f;
            TEST_CHECK(le_object_add_light(world, &lo, &ld) == LE_SUCCESS,
                       "directional add succeeds");
            {
                le_light_desc bad = ld;

                bad.type = (le_light_type)99;
                TEST_CHECK(le_object_add_light(world, &lo, &bad) ==
                               LE_ERROR_INVALID_ARGUMENT,
                       "bad light kind rejected");
            }
            {
                le_light_desc pt;

                memset(&pt, 0, sizeof(pt));
                pt.type = LE_LIGHT_POINT;
                pt.intensity = 1.0f;
                pt.range = 0.0f;
                TEST_CHECK(le_object_add_light(world, &lo, &pt) ==
                               LE_ERROR_INVALID_ARGUMENT,
                       "zero-range point rejected");
                pt.range = 10.0f;
                pt.shadow.enabled = 1;
                TEST_CHECK(le_object_add_light(world, &lo, &pt) ==
                               LE_ERROR_INVALID_ARGUMENT,
                       "point shadow rejected");
                pt.shadow.enabled = 0;
                TEST_CHECK(le_object_add_light(world, &lo, &pt) ==
                               LE_SUCCESS,
                       "point add succeeds");
            }
            {
                le_light_desc sp;

                memset(&sp, 0, sizeof(sp));
                sp.type = LE_LIGHT_SPOT;
                sp.intensity = 1.0f;
                sp.range = 10.0f;
                sp.spot_inner = 0.5f;
                sp.spot_outer = 0.2f;
                TEST_CHECK(le_object_add_light(world, &lo, &sp) ==
                               LE_ERROR_INVALID_ARGUMENT,
                       "inverted spot cone rejected");
                sp.spot_inner = 0.2f;
                sp.spot_outer = 0.5f;
                TEST_CHECK(le_object_add_light(world, &lo, &sp) ==
                               LE_SUCCESS,
                       "spot add succeeds");
            }
            {
                le_light_desc got;

                TEST_CHECK(le_object_get_light(world, &lo, &got) == 1,
                           "light get present");
                TEST_CHECK(got.type == LE_LIGHT_SPOT,
                           "light replace keeps last");
            }
            TEST_CHECK(le_object_remove_light(world, &lo) == LE_SUCCESS,
                       "light remove succeeds");
            TEST_CHECK(le_object_get_light(world, &lo, NULL) == 0,
                       "light get after remove -> 0");
                    le_object_destroy(world, &lo);
}
        /* Destroy removes all components (counters return). */
        {
            le_object victim;
            le_renderable_desc rd;
            le_camera_desc cd;
            le_light_desc ld;

            le_object_create(world, &victim);
            memset(&rd, 0, sizeof(rd));
            rd.mesh = (lr_mesh *)(uintptr_t)0x1;
            rd.material = (lr_material *)(uintptr_t)0x2;
            rd.visible = 1;
            le_object_add_renderable(world, &victim, &rd);
            le_camera_desc_default(&cd);
            le_object_add_camera(world, &victim, &cd);
            memset(&ld, 0, sizeof(ld));
            ld.type = LE_LIGHT_DIRECTIONAL;
            ld.intensity = 1.0f;
            le_object_add_light(world, &victim, &ld);
            le_object_destroy(world, &victim);
            {
                le_world_stats stats;

                le_world_get_stats(world, &stats);
                TEST_CHECK(stats.renderables == 0 && stats.cameras == 0 &&
                               stats.lights == 0,
                           "destroy removes all components");
            }
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Extraction (headless): deterministic order, counts, disabled
     * and invisible filtering. Mesh/material are fake borrowed
     * handles (extraction never dereferences them). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;
        le_object c;

        TEST_CHECK(make_engine_world(&engine, &world), "setup extract");
        le_object_create(world, &a);
        le_object_create(world, &b);
        le_object_create(world, &c);
        {
            le_renderable_desc rd;

            memset(&rd, 0, sizeof(rd));
            rd.mesh = (lr_mesh *)(uintptr_t)0x100;
            rd.material = (lr_material *)(uintptr_t)0x200;
            rd.visible = 1;
            le_object_add_renderable(world, &a, &rd);
            le_object_add_renderable(world, &b, &rd);
            le_object_add_renderable(world, &c, &rd);
        }
        {
            le_extraction_counts counts;

            le_world_get_extraction_counts(world, &counts);
            TEST_CHECK(counts.renderables == 3 && counts.lights == 0 &&
                           counts.has_camera == 0,
                       "extraction counts 3 renderables");
        }
        {
            le_extracted_renderable items[4];
            uint32_t count = 0;

            TEST_CHECK(le_world_extract_renderables(world, items, 4,
                                                    &count) == LE_SUCCESS &&
                           count == 3,
                       "extract 3 items");
            TEST_CHECK(items[0].object.index < items[1].object.index &&
                           items[1].object.index < items[2].object.index,
                       "extraction ascending-slot order");
            TEST_CHECK(items[0].stable_id != 0 &&
                           items[0].stable_id != items[1].stable_id,
                       "stable IDs nonzero + distinct");
            /* Deterministic: extract twice, same order + keys. */
            {
                le_extracted_renderable again[4];
                uint32_t count2 = 0;

                le_world_extract_renderables(world, again, 4, &count2);
                TEST_CHECK(count2 == 3 &&
                               again[0].stable_id ==
                                   items[0].stable_id &&
                               again[2].object.index ==
                                   items[2].object.index,
                           "extraction deterministic");
            }
        }
        /* Disable one: extraction drops it. */
        le_object_set_enabled(world, &b, 0);
        {
            le_extraction_counts counts;

            le_world_get_extraction_counts(world, &counts);
            TEST_CHECK(counts.renderables == 2,
                       "disabled excluded from counts");
        }
        le_object_set_enabled(world, &b, 1);
        /* Invisible flag: extraction drops it. */
        {
            le_renderable_desc rd;

            le_object_get_renderable(world, &c, &rd);
            rd.visible = 0;
            le_object_add_renderable(world, &c, &rd);
        }
        {
            uint32_t count = 0;

            le_world_extract_renderables(world, NULL, 0, &count);
            TEST_CHECK(count == 2, "invisible excluded from extract");
        }
        /* Capacity query: NULL buffer reports full count. */
        {
            uint32_t count = 0;

            le_world_extract_renderables(world, NULL, 0, &count);
            TEST_CHECK(count == 2, "counting query exact");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* World update advances time; world stats + memory sane. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;

        TEST_CHECK(make_engine_world(&engine, &world), "setup stats");
        TEST_CHECK(le_world_update(world, 0.5f) == LE_SUCCESS,
                   "update advances");
        TEST_CHECK(le_world_update(world, -1.0f) == LE_SUCCESS,
                   "negative dt clamps (success)");
        {
            le_world_stats stats;

            le_world_get_stats(world, &stats);
            TEST_CHECK(float_near((float)stats.time, 0.5f, 1e-5f),
                       "time accumulator exact");
        }
        {
            le_memory_stats mem;

            le_world_get_memory_stats(world, &mem);
            TEST_CHECK(mem.object_slots >= 64u && mem.total_bytes > 0 &&
                           mem.total_bytes >= mem.object_slot_bytes,
                       "memory accounting sane");
            printf("[info] slots=%llu slot_bytes=%llu total=%llu\n",
                   (unsigned long long)mem.object_slots,
                   (unsigned long long)mem.object_slot_bytes,
                   (unsigned long long)mem.total_bytes);
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Capacity growth is geometric (no tiny fixed limit). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_world_desc wdesc;
        le_engine_desc edesc;
        uint32_t cap0;
        uint32_t i;
        int ok = 1;

        memset(&edesc, 0, sizeof(edesc));
        memset(&wdesc, 0, sizeof(wdesc));
        wdesc.initial_capacity = 8;
        le_engine_create(&edesc, &engine);
        le_world_create(engine, &wdesc, &world);
        cap0 = le_world_get_object_capacity(world);
        TEST_CHECK(cap0 == 8, "initial capacity honored");
        for (i = 0; i < 100; i++) {
            le_object o;

            if (le_object_create(world, &o) != LE_SUCCESS) {
                ok = 0;
                break;
            }
        }
        TEST_CHECK(ok, "100 objects past initial capacity");
        TEST_CHECK(le_world_get_object_capacity(world) > cap0,
                   "capacity grew geometrically");
        TEST_CHECK(le_world_get_object_count(world) == 100,
                   "count exact after growth");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    printf("Luma Engine headless tests: %d passed, %d failed\n", g_passed,
           g_failed);
    if (g_failed != 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL ENGINE TESTS PASSED\n");
    return 0;
}
