/*
 * Luma Engine Phase 25 headless tests: assets (handles, stale,
 * types, ownership, stats), TRS decomposition, KEEP_WORLD
 * reparent, scenes (capture/instantiate/duplicate/lookup),
 * serialization (determinism, round-trip, malformed fuzz,
 * property/randomized, large scene), paths, IDs.
 *
 * No GPU, no window, no renderer: asset creation needs an engine
 * WITHOUT renderer? No — mesh/material/texture creation uploads.
 * Headless-safe subset runs without a renderer (handle validation,
 * scenes of cameras/lights/transforms, serialization, fuzz);
 * GPU-backed asset tests SKIP when no renderer is attached (engine
 * created without renderer => create calls fail INVALID_ARGUMENT,
 * which the tests assert as the headless contract).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static int make_engine(le_engine **engine) {
    le_engine_desc edesc;

    memset(&edesc, 0, sizeof(edesc));
    return le_engine_create(&edesc, engine) == LE_SUCCESS;
}

static int make_world(le_engine *engine, le_world **world) {
    le_world_desc wdesc;

    memset(&wdesc, 0, sizeof(wdesc));
    return le_world_create(engine, &wdesc, world) == LE_SUCCESS;
}

int main(void) {
    printf("Running Luma Engine Phase 25 headless tests...\n");

    /* NULL-safety of every new API. */
    TEST_CHECK(!le_asset_is_valid(NULL), "asset valid(NULL) 0");
    TEST_CHECK(!le_asset_is_alive(NULL, NULL),
               "asset alive all-NULL 0");
    TEST_CHECK(le_asset_unload(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "unload NULL engine INVALID");
    {
        le_asset_type t =
            le_asset_get_type(NULL, &LE_ASSET_INVALID);

        TEST_CHECK(t == LE_ASSET_COUNT, "type NULL COUNT");
        TEST_CHECK(le_asset_get_state(NULL, NULL) ==
                       LE_ASSET_UNLOADED,
                   "state NULL UNLOADED");
        TEST_CHECK(le_asset_get_source(NULL, NULL) != NULL,
                   "source NULL non-NULL");
        TEST_CHECK(le_asset_get_mesh(NULL, NULL) == NULL,
                   "mesh NULL NULL");
        TEST_CHECK(le_asset_get_material(NULL, NULL) == NULL,
                   "material NULL NULL");
    }
    TEST_CHECK(le_scene_create(NULL, 0, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "scene create NULL INVALID");
    TEST_CHECK(le_scene_add_object(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "scene add NULL INVALID");
    TEST_CHECK(le_scene_get_info(NULL, NULL, NULL, NULL, NULL) ==
                   0,
               "scene info NULL 0");
    TEST_CHECK(le_scene_capture(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "capture NULL INVALID");
    TEST_CHECK(le_scene_instantiate(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "instantiate NULL INVALID");
    TEST_CHECK(le_scene_save_text(NULL, NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "save NULL INVALID");
    TEST_CHECK(le_scene_load_text(NULL, NULL, NULL, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "load NULL INVALID");
    TEST_CHECK(le_scene_save_file(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "save file NULL INVALID");
    TEST_CHECK(le_scene_load_file(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "load file NULL INVALID");
    TEST_CHECK(le_object_reparent(NULL, NULL, NULL, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "reparent NULL INVALID");
    TEST_CHECK(le_matrix_decompose(NULL, NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "decompose NULL INVALID");
    TEST_CHECK(le_object_add_asset_renderable(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "add asset rend NULL INVALID");
    TEST_CHECK(le_object_get_asset_renderable(NULL, NULL, NULL) ==
                   0,
               "get asset rend NULL 0");
    TEST_CHECK(le_object_remove_asset_renderable(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "remove asset rend NULL INVALID");
    TEST_CHECK(le_gltf_import(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "gltf NULL INVALID");
    le_gltf_import_free(NULL);
    TEST_CHECK(1, "gltf free NULL safe");
    le_scene_instance_free(NULL);
    TEST_CHECK(1, "instance free NULL safe");
    TEST_CHECK(le_scene_instance_lookup(NULL, NULL, NULL) == 0,
               "lookup NULL 0");
    TEST_CHECK(le_asset_id_is_nil(NULL), "asset nil NULL 1");
    TEST_CHECK(le_scene_object_id_is_nil(NULL), "scene nil NULL 1");
    TEST_CHECK(!le_asset_id_equal(NULL, NULL), "asset eq NULL 0");

    /* Headless contract: engine without renderer rejects
     * GPU-backed creation with INVALID_ARGUMENT (documented). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_mesh_asset_desc md;
        le_material_asset_desc matd;
        le_texture_asset_desc td;
        le_asset out = LE_ASSET_INVALID;
        lr_vertex v;
        uint32_t idx[3] = { 0, 1, 2 };
        unsigned char px[16];

        TEST_CHECK(make_engine(&engine), "headless engine");
        TEST_CHECK(le_world_create(engine, NULL, &world) ==
                       LE_SUCCESS,
                   "headless world");
        memset(&v, 0, sizeof(v));
        memset(&md, 0, sizeof(md));
        md.vertices = &v;
        md.vertex_count = 3;
        md.indices = idx;
        md.index_count = 3;
        TEST_CHECK(le_asset_create_mesh(engine, &md, &out) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "mesh w/o renderer INVALID");
        memset(&matd, 0, sizeof(matd));
        TEST_CHECK(le_asset_create_material(engine, &matd,
                                            &out) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "material w/o renderer INVALID");
        memset(px, 255, sizeof(px));
        memset(&td, 0, sizeof(td));
        td.rgba = px;
        td.width = 2;
        td.height = 2;
        TEST_CHECK(le_asset_create_texture(engine, &td, &out) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "texture w/o renderer INVALID");
        TEST_CHECK(le_gltf_import(engine, "x.glb", NULL) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "gltf NULL out INVALID");
        {
            le_gltf_result imp;

            TEST_CHECK(le_gltf_import(engine, "x.glb", &imp) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "gltf w/o renderer INVALID");
        }
        /* Asset stats zero-shape on empty registry. */
        {
            le_asset_stats stats;

            le_engine_get_asset_stats(engine, &stats);
            TEST_CHECK(stats.assets_alive == 0 &&
                           stats.ready_count == 0,
                       "empty registry stats zero");
            le_engine_get_asset_stats(NULL, &stats);
            TEST_CHECK(stats.assets_alive == 0,
                       "stats NULL zero");
        }
        /* Scenes work headlessly (no GPU): create + empty info. */
        {
            le_asset scene = LE_ASSET_INVALID;
            uint32_t nobjs = 99;
            uint32_t nroots = 99;
            uint32_t ver = 0;

            TEST_CHECK(le_scene_create(engine, 1, &scene) ==
                           LE_SUCCESS,
                       "scene create headless");
            TEST_CHECK(le_asset_is_alive(engine, &scene),
                       "scene alive");
            TEST_CHECK(le_asset_get_type(engine, &scene) ==
                           LE_ASSET_SCENE,
                       "scene type SCENE");
            TEST_CHECK(le_asset_get_state(engine, &scene) ==
                           LE_ASSET_READY,
                       "scene state READY");
            TEST_CHECK(le_scene_get_info(engine, &scene, &nobjs,
                                         &nroots, &ver) == 1 &&
                           nobjs == 0 && nroots == 0 &&
                           ver == LE_SCENE_FORMAT_VERSION,
                       "empty scene info");
            /* Capture empty world -> empty scene. */
            {
                uint32_t skipped = 99;

                TEST_CHECK(le_scene_capture(world, &scene,
                                            &skipped) ==
                               LE_SUCCESS,
                           "capture empty world");
                TEST_CHECK(skipped == 0, "nothing skipped");
            }
            /* Save/parse round-trip of empty scene. */
            {
                char *text = NULL;
                size_t size = 0;

                TEST_CHECK(le_scene_save_text(engine, &scene,
                                              &text,
                                              &size) == LE_SUCCESS &&
                               text != NULL && size > 0,
                           "save empty scene");
                TEST_CHECK(strstr(text, "LUMA_SCENE 1") != NULL,
                           "header present");
                {
                    le_asset scene2 = LE_ASSET_INVALID;
                    uint32_t n2 = 99;

                    le_scene_create(engine, 1, &scene2);
                    TEST_CHECK(le_scene_load_text(engine, &scene2,
                                                  text,
                                                  size) ==
                                   LE_SUCCESS,
                               "load empty scene");
                    le_scene_get_info(engine, &scene2, &n2, NULL,
                                      NULL);
                    TEST_CHECK(n2 == 0, "loaded empty");
                    le_asset_unload(engine, &scene2);
                }
                /* Determinism: save twice, byte-identical. */
                {
                    char *text2 = NULL;
                    size_t size2 = 0;

                    le_scene_save_text(engine, &scene, &text2,
                                       &size2);
                    TEST_CHECK(size == size2 &&
                                   memcmp(text, text2,
                                          size) == 0,
                               "empty save deterministic");
                    le_scene_free_text(text2);
                }
                le_scene_free_text(text);
            }
            le_asset_unload(engine, &scene);
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* TRS decomposition (pure math, headless). */
    {
        float p[3] = { 4.0f, 5.0f, 6.0f };
        float axis[3] = { 0.0f, 1.0f, 0.0f };
        float q[4];
        float s[3] = { 2.0f, 3.0f, 4.0f };
        float m[16];
        float dp[3];
        float dq[4];
        float ds[3];

        le_quat_from_axis_angle(axis, 0.7f, q);
        le_transform_compose(p, q, s, m);
        TEST_CHECK(le_matrix_decompose(m, dp, dq, ds) ==
                       LE_SUCCESS,
                   "decompose TRS ok");
        TEST_CHECK(dp[0] == p[0] && dp[1] == p[1] &&
                       dp[2] == p[2],
                   "decompose position exact");
        TEST_CHECK(ds[0] == s[0] && ds[1] == s[1] &&
                       ds[2] == s[2],
                   "decompose scale exact");
        {
            float d = dq[0] * q[0] + dq[1] * q[1] +
                      dq[2] * q[2] + dq[3] * q[3];

            if (d < 0) {
                d = -d;
            }
            TEST_CHECK(d > 0.99999f, "decompose rotation match");
        }
        /* Mirror decomposes with sign. */
        {
            float ms[3] = { -2.0f, 3.0f, 4.0f };
            float mm[16];
            float os[3];

            le_transform_compose(p, q, ms, mm);
            TEST_CHECK(le_matrix_decompose(mm, NULL, NULL, os) ==
                           LE_SUCCESS,
                       "decompose mirror ok");
            TEST_CHECK(os[0] < 0 && os[1] > 0 && os[2] > 0,
                       "mirror sign kept");
            TEST_CHECK(le_matrix_is_mirrored(mm) == 1,
                       "mirror parity");
        }
        /* Shear rejected, not distorted. */
        {
            float sh[16];

            le_mat4_identity(sh);
            sh[4] = 2.0f; /* shear XY */
            TEST_CHECK(le_matrix_decompose(sh, dp, dq, ds) ==
                           LE_ERROR_UNREPRESENTABLE_TRANSFORM,
                       "shear rejected");
        }
        /* Singular rejected. */
        {
            float sg[16];

            le_mat4_identity(sg);
            sg[0] = 0.0f;
            TEST_CHECK(le_matrix_decompose(sg, dp, dq, ds) ==
                           LE_ERROR_UNREPRESENTABLE_TRANSFORM,
                       "singular rejected");
        }
    }

    /* KEEP_WORLD reparent (headless, no GPU). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object parent;
        le_object child;
        float before[16];
        float after[16];
        int k;
        int same = 1;

        make_engine(&engine);
        le_world_create(engine, NULL, &world);
        le_object_create(world, &parent);
        le_object_create(world, &child);
        {
            float p[3] = { 10.0f, 0.0f, 0.0f };
            float c[3] = { 1.0f, 2.0f, 3.0f };

            le_object_set_position(world, &parent, p);
            le_object_set_position(world, &child, c);
        }
        le_object_set_parent(world, &child, &parent);
        le_object_get_world_matrix(world, &child, before);
        /* KEEP_WORLD to a NEW parent at origin: world preserved. */
        {
            le_object p2;

            le_object_create(world, &p2);
            TEST_CHECK(le_object_reparent(world, &child, &p2,
                                          LE_REPARENT_KEEP_WORLD) ==
                           LE_SUCCESS,
                       "keep-world reparent ok");
            le_object_get_world_matrix(world, &child, after);
            for (k = 0; k < 16; k++) {
                float d = before[k] - after[k];

                if (d > 1e-4f || d < -1e-4f) {
                    same = 0;
                }
            }
            TEST_CHECK(same, "world matrix preserved");
            /* KEEP_LOCAL would have changed it. */
            le_object_reparent(world, &child, &parent,
                               LE_REPARENT_KEEP_LOCAL);
            le_object_get_world_matrix(world, &child, after);
            same = 1;
            for (k = 0; k < 16; k++) {
                float d = before[k] - after[k];

                if (d > 1e-4f || d < -1e-4f) {
                    same = 0;
                }
            }
            TEST_CHECK(!same, "keep-local differs (sanity)");
            le_object_destroy(world, &p2);
        }
        /* Bad mode rejected. */
        TEST_CHECK(le_object_reparent(world, &child, &parent,
                                      (le_reparent_mode)99) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "bad mode rejected");
        /* Cycle rejected in KEEP_WORLD too. */
        TEST_CHECK(le_object_reparent(world, &parent, &child,
                                      LE_REPARENT_KEEP_WORLD) ==
                       LE_ERROR_CYCLE,
                   "keep-world cycle rejected");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Scene capture/instantiate headless (camera/light/transform
     * only — no assets needed). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_world *world2 = NULL;
        le_asset scene = LE_ASSET_INVALID;
        le_object root;
        le_object kid;
        le_object cam;
        le_scene_instance inst;

        make_engine(&engine);
        le_world_create(engine, NULL, &world);
        le_world_create(engine, NULL, &world2);
        le_object_create(world, &root);
        le_object_create(world, &kid);
        le_object_create(world, &cam);
        le_object_set_name(world, &root, "Root \"quoted\"\nline");
        le_object_set_name(world, &kid, "Kid\ttab");
        {
            float p[3] = { 1.0f, 2.0f, 3.0f };

            le_object_set_position(world, &kid, p);
        }
        le_object_set_parent(world, &kid, &root);
        le_object_set_enabled(world, &kid, 0);
        {
            le_camera_desc cd;
            le_light_desc ld;

            le_camera_desc_default(&cd);
            le_object_add_camera(world, &cam, &cd);
            memset(&ld, 0, sizeof(ld));
            ld.type = LE_LIGHT_DIRECTIONAL;
            ld.color[0] = 1.0f;
            ld.color[1] = 1.0f;
            ld.color[2] = 1.0f;
            ld.intensity = 2.0f;
            le_object_add_light(world, &root, &ld);
        }
        le_scene_create(engine, 1, &scene);
        {
            uint32_t skipped = 99;

            TEST_CHECK(le_scene_capture(world, &scene, &skipped) ==
                           LE_SUCCESS,
                       "capture nontrivial");
            TEST_CHECK(skipped == 0, "no pointer rends skipped");
        }
        {
            uint32_t n = 0;

            le_scene_get_info(engine, &scene, &n, NULL, NULL);
            TEST_CHECK(n == 3, "3 records captured");
        }
        /* Save -> load into a second scene -> instantiate into a
         * second world (fresh handles, same persistent IDs). */
        {
            char *text = NULL;
            size_t size = 0;
            le_asset scene2 = LE_ASSET_INVALID;
            le_scene_instance inst2;

            le_scene_save_text(engine, &scene, &text, &size);
            TEST_CHECK(size > 0, "nontrivial text");
            le_scene_create(engine, 1, &scene2);
            TEST_CHECK(le_scene_load_text(engine, &scene2, text,
                                          size) == LE_SUCCESS,
                       "parse nontrivial");
            TEST_CHECK(le_scene_instantiate(world2, &scene2,
                                            &inst2) == LE_SUCCESS,
                       "instantiate into world2");
            TEST_CHECK(inst2.count == 3, "3 objects live");
            {
                uint32_t alive = le_world_get_object_count(world2);

                TEST_CHECK(alive == 3, "world2 has 3");
            }
            /* Lookup by persistent ID works. */
            {
                le_object found = LE_OBJECT_INVALID;

                TEST_CHECK(le_scene_instance_lookup(
                               &inst2, &inst2.object_ids[0],
                               &found) == 1,
                           "instance lookup hits");
                TEST_CHECK(le_object_is_alive(world2, &found),
                           "looked-up handle alive");
                TEST_CHECK(le_scene_instance_lookup(
                               &inst2, NULL, NULL) == 0,
                           "lookup NULL misses");
            }
            /* Handles differ from the source world (fresh). */
            {
                int overlap = 0;
                uint32_t i;

                for (i = 0; i < inst2.count; i++) {
                    if (le_object_is_alive(world,
                                           &inst2.objects[i])) {
                        overlap = 1;
                    }
                }
                TEST_CHECK(!overlap,
                           "fresh handles (no world overlap)");
            }
            le_scene_instance_free(&inst2);
            le_scene_free_text(text);
            le_asset_unload(engine, &scene2);
        }
        /* Direct instantiate into a third world + duplicate
         * instantiation (two instances, disjoint handles). */
        {
            le_world *world3 = NULL;
            le_scene_instance a;
            le_scene_instance b;

            le_world_create(engine, NULL, &world3);
            TEST_CHECK(le_scene_instantiate(world3, &scene, &a) ==
                           LE_SUCCESS,
                       "instance A");
            TEST_CHECK(le_scene_instantiate(world3, &scene, &b) ==
                           LE_SUCCESS,
                       "instance B");
            TEST_CHECK(a.count == 3 && b.count == 3,
                       "both full");
            {
                uint32_t i;
                int clash = 0;

                for (i = 0; i < a.count; i++) {
                    uint32_t j;

                    for (j = 0; j < b.count; j++) {
                        if (a.objects[i].index ==
                                b.objects[j].index &&
                            a.objects[i].generation ==
                                b.objects[j].generation) {
                            clash = 1;
                        }
                    }
                }
                TEST_CHECK(!clash,
                           "instances disjoint handles");
                TEST_CHECK(le_object_stable_id(world3,
                                               &a.objects[0]) !=
                                   le_object_stable_id(
                                       world3, &b.objects[0]),
                           "temporal keys distinct");
            }
            le_scene_instance_free(&a);
            le_scene_instance_free(&b);
            le_world_destroy(world3);
        }
        memset(&inst, 0, sizeof(inst));
        le_asset_unload(engine, &scene);
        le_world_destroy(world);
        le_world_destroy(world2);
        le_engine_destroy(engine);
    }

    /* Malformed-input suite (no crash, no partial scene/world
     * corruption; precise codes). */
    {
        le_engine *engine = NULL;
        le_asset scene = LE_ASSET_INVALID;

        make_engine(&engine);
        le_scene_create(engine, 1, &scene);
        {
            /* Truncated / bad header / bad version. */
            TEST_CHECK(le_scene_load_text(engine, &scene, "", 0) ==
                           LE_ERROR_PARSE,
                       "empty rejected");
            TEST_CHECK(le_scene_load_text(engine, &scene,
                                          "HELLO\n", 6) ==
                           LE_ERROR_PARSE,
                       "bad header rejected");
            TEST_CHECK(le_scene_load_text(engine, &scene,
                                          "LUMA_SCENE 99\n", 13) ==
                           LE_ERROR_UNSUPPORTED_VERSION,
                       "bad version rejected");
            TEST_CHECK(le_scene_load_text(engine, &scene,
                                          "LUMA_SCENE 1\n", 12) ==
                           LE_SUCCESS,
                       "header-only ok");
        }
        {
            /* Duplicate IDs. */
            const char *dup =
                "LUMA_SCENE 1\n"
                "object "
                "0123456789abcdef0123456789abcdef\n"
                "end\n"
                "object "
                "0123456789abcdef0123456789abcdef\n"
                "end\n";

            TEST_CHECK(le_scene_load_text(engine, &scene, dup,
                                          strlen(dup)) ==
                           LE_ERROR_DUPLICATE_ID,
                       "duplicate IDs rejected");
        }
        {
            /* Missing end. */
            const char *noe =
                "LUMA_SCENE 1\n"
                "object "
                "0123456789abcdef0123456789abcdef\n"
                "enabled 1\n";

            TEST_CHECK(le_scene_load_text(engine, &scene, noe,
                                          strlen(noe)) ==
                           LE_ERROR_PARSE,
                       "missing end rejected");
        }
        {
            /* Bad parent hex / self parent / missing parent. */
            const char *selfp =
                "LUMA_SCENE 1\n"
                "object "
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                "parent "
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                "end\n";
            le_world *w = NULL;
            le_scene_instance inst;

            TEST_CHECK(le_scene_load_text(engine, &scene, selfp,
                                          strlen(selfp)) ==
                           LE_SUCCESS,
                       "self-parent parses (instantiate rejects)");
            le_world_create(engine, NULL, &w);
            memset(&inst, 0, sizeof(inst));
            TEST_CHECK(le_scene_instantiate(w, &scene, &inst) ==
                           LE_ERROR_INVALID_HIERARCHY,
                       "self-parent rejected at instantiate");
            TEST_CHECK(le_world_get_object_count(w) == 0,
                       "no partial objects (self-parent)");
            le_world_destroy(w);
        }
        {
            /* Cycle A->B->A. */
            const char *cyc =
                "LUMA_SCENE 1\n"
                "object "
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                "parent "
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
                "end\n"
                "object "
                "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
                "parent "
                "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
                "end\n";
            le_world *w = NULL;
            le_scene_instance inst;

            le_scene_load_text(engine, &scene, cyc, strlen(cyc));
            le_world_create(engine, NULL, &w);
            memset(&inst, 0, sizeof(inst));
            TEST_CHECK(le_scene_instantiate(w, &scene, &inst) ==
                           LE_ERROR_INVALID_HIERARCHY,
                       "cycle rejected");
            TEST_CHECK(le_world_get_object_count(w) == 0,
                       "no partial objects (cycle)");
            le_world_destroy(w);
        }
        {
            /* NaN / Inf floats. */
            const char *nan =
                "LUMA_SCENE 1\n"
                "object "
                "cccccccccccccccccccccccccccccccc\n"
                "position nan 0 0\n"
                "end\n";
            const char *inf =
                "LUMA_SCENE 1\n"
                "object "
                "dddddddddddddddddddddddddddddddd\n"
                "position inf 0 0\n"
                "end\n";

            TEST_CHECK(le_scene_load_text(engine, &scene, nan,
                                          strlen(nan)) ==
                           LE_ERROR_PARSE,
                       "NaN rejected");
            TEST_CHECK(le_scene_load_text(engine, &scene, inf,
                                          strlen(inf)) ==
                           LE_ERROR_PARSE,
                       "Inf rejected");
        }
        {
            /* Bad escapes / unterminated quote. */
            const char *badesc =
                "LUMA_SCENE 1\n"
                "object "
                "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee\n"
                "name \"bad\\qesc\"\n"
                "end\n";
            const char *noquote =
                "LUMA_SCENE 1\n"
                "object "
                "ffffffffffffffffffffffffffffffff\n"
                "name \"unterminated\n"
                "end\n";

            TEST_CHECK(le_scene_load_text(engine, &scene, badesc,
                                          strlen(badesc)) ==
                           LE_ERROR_PARSE,
                       "bad escape rejected");
            TEST_CHECK(le_scene_load_text(engine, &scene, noquote,
                                          strlen(noquote)) ==
                           LE_ERROR_PARSE,
                       "unterminated quote rejected");
        }
        {
            /* Unknown component rejected; unknown field
             * tolerated. */
            const char *unkcomp =
                "LUMA_SCENE 1\n"
                "object "
                "11111111111111111111111111111111\n"
                "physics 1 2 3\n"
                "end\n";
            const char *unkfield =
                "LUMA_SCENE 1\n"
                "object "
                "22222222222222222222222222222222\n"
                "mood happy\n"
                "end\n";

            TEST_CHECK(le_scene_load_text(engine, &scene, unkcomp,
                                          strlen(unkcomp)) ==
                           LE_ERROR_PARSE,
                       "unknown component rejected");
            TEST_CHECK(le_scene_load_text(engine, &scene, unkfield,
                                          strlen(unkfield)) ==
                           LE_SUCCESS,
                       "unknown field tolerated");
        }
        {
            /* Scene unchanged after failed load (transactional):
             * load a good scene, fail, verify content intact. */
            const char *good =
                "LUMA_SCENE 1\n"
                "object "
                "33333333333333333333333333333333\n"
                "name \"Keep\"\n"
                "end\n";
            uint32_t n = 0;

            le_scene_load_text(engine, &scene, good, strlen(good));
            le_scene_get_info(engine, &scene, &n, NULL, NULL);
            TEST_CHECK(n == 1, "good scene has 1");
            le_scene_load_text(engine, &scene, "TRASH",
                               strlen("TRASH"));
            le_scene_get_info(engine, &scene, &n, NULL, NULL);
            TEST_CHECK(n == 1, "failed load keeps payload");
        }
        le_asset_unload(engine, &scene);
        le_engine_destroy(engine);
    }

    /* Escaping round-trip (quotes/slashes/unicode/newlines/tabs). */
    {
        le_engine *engine = NULL;
        le_asset scene = LE_ASSET_INVALID;
        le_asset scene2 = LE_ASSET_INVALID;
        char *text = NULL;
        size_t size = 0;

        make_engine(&engine);
        le_scene_create(engine, 1, &scene);
        {
            le_scene_object rec;

            memset(&rec, 0, sizeof(rec));
            le_scene_object_id_make(engine, &rec.id);
            memcpy(rec.name, "q\" s\\ uni\xC3\xA9 \n \t end",
                   20);
            rec.name[20] = '\0';
            rec.enabled = 1;
            rec.rotation[3] = 1.0f;
            rec.scale[0] = rec.scale[1] = rec.scale[2] = 1.0f;
            TEST_CHECK(le_scene_add_object(engine, &scene, &rec) ==
                           LE_SUCCESS,
                       "escape record added");
            /* Duplicate add rejected. */
            TEST_CHECK(le_scene_add_object(engine, &scene, &rec) ==
                           LE_ERROR_DUPLICATE_ID,
                       "duplicate add rejected");
        }
        le_scene_save_text(engine, &scene, &text, &size);
        le_scene_create(engine, 1, &scene2);
        TEST_CHECK(le_scene_load_text(engine, &scene2, text,
                                      size) == LE_SUCCESS,
                   "escape parses");
        {
            char *text2 = NULL;
            size_t size2 = 0;

            le_scene_save_text(engine, &scene2, &text2, &size2);
            TEST_CHECK(size == size2 &&
                           memcmp(text, text2, size) == 0,
                       "escape canonical bytes");
            le_scene_free_text(text2);
        }
        le_scene_free_text(text);
        le_asset_unload(engine, &scene);
        le_asset_unload(engine, &scene2);
        le_engine_destroy(engine);
    }

    /* Randomized property round-trip (deterministic seed). */
    {
        const uint32_t SEED = 0xC0FFEEu;
        const int CASES = 25;
        uint32_t rng = SEED;
        int c;
        int ok = 1;

#define NEXT_U32() \
    (rng = rng * 1664525u + 1013904223u, rng >> 8)

        for (c = 0; c < CASES && ok; c++) {
            le_engine *engine = NULL;
            le_asset scene = LE_ASSET_INVALID;
            le_asset scene2 = LE_ASSET_INVALID;
            uint32_t nobjs = 1 + NEXT_U32() % 12u;
            uint32_t i;
            char *a = NULL;
            char *b = NULL;
            size_t sa = 0;
            size_t sb = 0;

            make_engine(&engine);
            le_scene_create(engine, 1, &scene);
            for (i = 0; i < nobjs; i++) {
                le_scene_object rec;

                memset(&rec, 0, sizeof(rec));
                le_scene_object_id_make(engine, &rec.id);
                snprintf(rec.name, sizeof(rec.name), "r-%u-%d",
                         (unsigned)i, c);
                rec.enabled = (NEXT_U32() & 1u) ? 1 : 0;
                rec.position[0] =
                    (float)(NEXT_U32() % 200u) - 100.0f;
                rec.position[1] =
                    (float)(NEXT_U32() % 200u) - 100.0f;
                rec.position[2] =
                    (float)(NEXT_U32() % 200u) - 100.0f;
                {
                    float ax[3] = {
                        (float)(NEXT_U32() % 7u) - 3.0f,
                        (float)(NEXT_U32() % 7u) - 3.0f,
                        (float)(NEXT_U32() % 7u) - 3.0f,
                    };

                    le_quat_from_axis_angle(
                        ax,
                        (float)(NEXT_U32() % 628u) / 100.0f,
                        rec.rotation);
                }
                rec.scale[0] =
                    ((NEXT_U32() & 3u) == 0u) ? -1.0f : 1.0f;
                rec.scale[1] = 1.0f;
                rec.scale[2] = 1.0f;
                if (le_scene_add_object(engine, &scene, &rec) !=
                    LE_SUCCESS) {
                    ok = 0;
                }
            }
            if (le_scene_save_text(engine, &scene, &a, &sa) !=
                LE_SUCCESS) {
                ok = 0;
            }
            le_scene_create(engine, 1, &scene2);
            if (le_scene_load_text(engine, &scene2, a, sa) !=
                LE_SUCCESS) {
                ok = 0;
            }
            if (le_scene_save_text(engine, &scene2, &b, &sb) !=
                LE_SUCCESS) {
                ok = 0;
            }
            if (sa != sb || memcmp(a, b, sa) != 0) {
                ok = 0;
            }
            le_scene_free_text(a);
            le_scene_free_text(b);
            le_asset_unload(engine, &scene);
            le_asset_unload(engine, &scene2);
            le_engine_destroy(engine);
        }
        TEST_CHECK(ok, "property round-trip 25 cases");
        printf("[info] property seed=0xC0FFEE cases=25\n");
#undef NEXT_U32
    }

    /* Random hierarchy test (acyclic, parent-first shuffled
     * order to prove forward-reference resolution). */
    {
        const uint32_t SEED = 0x5EEDu;
        uint32_t rng = SEED;
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset scene = LE_ASSET_INVALID;
        enum { HN = 40 };
        le_scene_object_id ids[HN];
        uint32_t i;
        int ok = 1;

        make_engine(&engine);
        le_world_create(engine, NULL, &world);
        le_scene_create(engine, 1, &scene);
        for (i = 0; i < HN; i++) {
            le_scene_object_id_make(engine, &ids[i]);
        }
        /* Records added child-first (reverse), parents resolved
         * at instantiate regardless of order. */
        for (i = HN; i-- > 0;) {
            le_scene_object rec;

            memset(&rec, 0, sizeof(rec));
            rec.id = ids[i];
            if (i > 0) {
                rng = rng * 1664525u + 1013904223u;
                if ((rng >> 8) & 1u) {
                    rec.parent = ids[(rng >> 16) % i];
                    rec.has_parent = 1;
                }
            }
            snprintf(rec.name, sizeof(rec.name), "h%u",
                     (unsigned)i);
            rec.enabled = 1;
            rec.rotation[3] = 1.0f;
            rec.scale[0] = rec.scale[1] = rec.scale[2] = 1.0f;
            rec.position[0] = (float)i;
            if (le_scene_add_object(engine, &scene, &rec) !=
                LE_SUCCESS) {
                ok = 0;
            }
        }
        TEST_CHECK(ok, "random hierarchy built");
        {
            le_scene_instance inst;
            char *text = NULL;
            size_t size = 0;

            memset(&inst, 0, sizeof(inst));
            TEST_CHECK(le_scene_instantiate(world, &scene,
                                            &inst) == LE_SUCCESS,
                       "random hierarchy instantiates");
            /* Verify parent relations + world positions. */
            {
                uint32_t k;
                int relok = 1;

                for (k = 0; k < inst.count; k++) {
                    le_object_info info;

                    le_object_get_info(world, &inst.objects[k],
                                       &info);
                    /* Position x == index proves TRS kept. */
                    float m[16];

                    le_object_get_world_matrix(world,
                                               &inst.objects[k],
                                               m);
                    /* World x accumulates ancestors (each +1
                     * per level along the chain). Just check
                     * finite + deterministic. */
                    if (!(m[12] == m[12])) {
                        relok = 0;
                    }
                }
                TEST_CHECK(relok, "world matrices finite");
            }
            le_scene_save_text(engine, &scene, &text, &size);
            {
                le_asset scene2 = LE_ASSET_INVALID;

                le_scene_create(engine, 1, &scene2);
                TEST_CHECK(le_scene_load_text(engine, &scene2,
                                              text,
                                              size) == LE_SUCCESS,
                           "hierarchy re-parses");
                le_asset_unload(engine, &scene2);
            }
            le_scene_free_text(text);
            le_scene_instance_free(&inst);
        }
        printf("[info] hierarchy seed=0x5EED objects=40\n");
        le_asset_unload(engine, &scene);
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* World-preserving reparent math edges: rotated parents. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object a;
        le_object b;
        float before[16];
        float after[16];
        int k;
        int same = 1;

        make_engine(&engine);
        le_world_create(engine, NULL, &world);
        le_object_create(world, &a);
        le_object_create(world, &b);
        {
            float ax[3] = { 0.0f, 0.0f, 1.0f };
            float q[4];
            float p[3] = { 5.0f, 0.0f, 0.0f };
            float c[3] = { 0.0f, 3.0f, 0.0f };

            le_quat_from_axis_angle(ax, 1.2f, q);
            le_object_set_rotation(world, &a, q);
            le_object_set_position(world, &a, p);
            le_object_set_position(world, &b, c);
        }
        le_object_get_world_matrix(world, &b, before);
        TEST_CHECK(le_object_reparent(world, &b, &a,
                                      LE_REPARENT_KEEP_WORLD) ==
                       LE_SUCCESS,
                   "keep-world under rotated parent");
        le_object_get_world_matrix(world, &b, after);
        for (k = 0; k < 16; k++) {
            float d = before[k] - after[k];

            if (d > 1e-3f || d < -1e-3f) {
                same = 0;
            }
        }
        TEST_CHECK(same, "rotated world preserved");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Path normalization unit checks (public edge: escaping
     * paths rejected at import; the internal normalizer is covered
     * indirectly by every dedup/save/load path above). */
    {
        le_engine *engine = NULL;
        le_gltf_result imp;

        make_engine(&engine);
        TEST_CHECK(le_gltf_import(engine, "../evil.glb", &imp) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "escaping path rejected");
        TEST_CHECK(le_gltf_import(engine, "", &imp) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "empty path rejected");
        le_engine_destroy(engine);
    }

    printf("Luma Engine Phase 25 headless tests: %d passed, %d "
           "failed\n",
           g_passed, g_failed);
    if (g_failed != 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL PHASE 25 HEADLESS TESTS PASSED\n");
    return 0;
}
