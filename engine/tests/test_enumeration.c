/*
 * Luma Engine Phase 31 headless tests: le_world_get_roots,
 * le_world_get_all_objects, le_world_get_live_count,
 * le_object_get_info2 (full component coverage incl. script,
 * physics, animator, character).
 *
 * No GPU, no window, no renderer.
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
        return 0;
    }
    return 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 31 enumeration headless tests...\n");

    /* NULL guards. */
    TEST_CHECK(le_world_get_roots(NULL, NULL, 0, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "roots NULL INVALID");
    TEST_CHECK(le_world_get_live_count(NULL) == 0,
               "live NULL 0");
    TEST_CHECK(le_world_get_all_objects(NULL, NULL, 0) == 0,
               "all NULL 0");
    {
        le_object_info2 info;

        memset(&info, 0, sizeof(info));
        le_object_get_info2(NULL, NULL, NULL);
        le_object_get_info2(NULL, NULL, &info);
        TEST_CHECK(info.version == 0 && !info.alive,
                   "info2 NULL zeroed");
    }

    /* Roots: counting, fill, determinism, roots-not-children. */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object r0 = LE_OBJECT_INVALID;
        le_object r1 = LE_OBJECT_INVALID;
        le_object ch = LE_OBJECT_INVALID;

        TEST_CHECK(make_engine_world(&e, &w), "make world");
        {
            uint32_t n = 0;

            TEST_CHECK(le_world_get_roots(w, NULL, 0, &n) ==
                           LE_SUCCESS,
                       "roots counting ok");
            TEST_CHECK(n == 0, "no roots yet");
            TEST_CHECK(le_world_get_root_count(w) == 0,
                       "root count 0");
            TEST_CHECK(le_world_get_live_count(w) == 0,
                       "live 0");
            TEST_CHECK(le_world_get_all_objects(w, NULL, 0) == 0,
                       "all counting 0");
        }
        TEST_CHECK(le_object_create(w, &r0) == LE_SUCCESS, "r0");
        TEST_CHECK(le_object_create(w, &r1) == LE_SUCCESS, "r1");
        TEST_CHECK(le_object_create(w, &ch) == LE_SUCCESS, "ch");
        TEST_CHECK(le_object_set_parent(w, &ch, &r0) == LE_SUCCESS,
                   "ch under r0");
        {
            uint32_t n = 0;
            le_object roots[4];

            memset(roots, 0, sizeof(roots));
            TEST_CHECK(le_world_get_roots(w, NULL, 0, &n) ==
                           LE_SUCCESS,
                       "roots counting 2");
            TEST_CHECK(n == 2, "two roots");
            TEST_CHECK(le_world_get_root_count(w) == 2,
                       "root count 2");
            TEST_CHECK(le_world_get_live_count(w) == 3, "live 3");
            TEST_CHECK(le_world_get_all_objects(w, NULL, 0) == 3,
                       "all counting 3");
            TEST_CHECK(le_world_get_roots(w, roots, 4, &n) ==
                           LE_SUCCESS,
                       "roots fill ok");
            TEST_CHECK(n == 2, "full count still 2");
            TEST_CHECK(roots[0].index < roots[1].index,
                       "roots ascending");
            TEST_CHECK(roots[0].index == r0.index &&
                           roots[1].index == r1.index,
                       "roots are r0 r1");
            /* Truncated fill still reports full count. */
            {
                le_object one[1];
                uint32_t n2 = 0;

                memset(one, 0, sizeof(one));
                TEST_CHECK(le_world_get_roots(w, one, 1, &n2) ==
                               LE_SUCCESS,
                           "truncated ok");
                TEST_CHECK(n2 == 2, "truncated full count");
                TEST_CHECK(one[0].index == r0.index,
                           "truncated first root");
            }
            /* All-objects ascending. */
            {
                le_object all[4];
                uint32_t got =
                    le_world_get_all_objects(w, all, 4);

                TEST_CHECK(got == 3, "all 3");
                TEST_CHECK(all[0].index < all[1].index &&
                               all[1].index < all[2].index,
                           "all ascending");
            }
        }
        /* Destroy a root: its subtree goes, roots shrink. */
        TEST_CHECK(le_object_destroy(w, &r0) == LE_SUCCESS,
                   "destroy r0 subtree");
        {
            uint32_t n = 0;

            TEST_CHECK(le_world_get_roots(w, NULL, 0, &n) ==
                           LE_SUCCESS,
                       "roots after destroy");
            TEST_CHECK(n == 1, "one root left");
            TEST_CHECK(le_world_get_live_count(w) == 1, "live 1");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* info2: plain object + full component coverage. */
    {
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;
        le_object_info2 info;

        memset(&info, 0, sizeof(info));
        TEST_CHECK(make_engine_world(&e, &w), "make world 2");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "obj");
        TEST_CHECK(le_object_set_name(w, &o, "info2") == LE_SUCCESS,
                   "name");
        le_object_get_info2(w, &o, &info);
        TEST_CHECK(info.version == LE_OBJECT_INFO2_VERSION,
                   "version 1");
        TEST_CHECK(info.alive && info.has_transform, "alive+trs");
        TEST_CHECK(!info.has_camera && !info.has_script &&
                       !info.has_rigid_body && !info.has_collider &&
                       !info.has_animator && !info.has_character,
                   "no components yet");
        TEST_CHECK(strcmp(info.name, "info2") == 0, "name borrowed");
        TEST_CHECK(info.child_count == 0 && !info.has_parent,
                   "root no kids");
        /* Camera + light bits. */
        {
            le_camera_desc cd;
            le_light_desc ld;

            memset(&cd, 0, sizeof(cd));
            memset(&ld, 0, sizeof(ld));
            le_camera_desc_default(&cd);
            TEST_CHECK(le_object_add_camera(w, &o, &cd) ==
                           LE_SUCCESS,
                       "add camera");
            ld.type = LE_LIGHT_POINT;
            ld.color[0] = ld.color[1] = ld.color[2] = 1.0f;
            ld.intensity = 5.0f;
            ld.range = 10.0f;
            TEST_CHECK(le_object_add_light(w, &o, &ld) ==
                           LE_SUCCESS,
                       "add light");
            memset(&info, 0, sizeof(info));
            le_object_get_info2(w, &o, &info);
            TEST_CHECK(info.has_camera && info.has_light,
                       "camera+light bits");
            TEST_CHECK(info.has_renderable == 0,
                       "no renderable bit");
        }
        /* Physics bits. */
        {
            le_rigid_body_desc bd;
            le_collider_desc cd;

            memset(&bd, 0, sizeof(bd));
            memset(&cd, 0, sizeof(cd));
            bd.type = LE_BODY_STATIC;
            TEST_CHECK(le_object_add_rigid_body(w, &o, &bd) ==
                           LE_SUCCESS,
                       "add body");
            cd.shape = LE_COLLIDER_SPHERE;
            cd.radius = 1.0f;
            cd.orientation[3] = 1.0f;
            cd.layer = 0;
            cd.mask = 0xFFFFFFFFu;
            cd.friction = 0.5f;
            TEST_CHECK(le_object_add_collider(w, &o, &cd) ==
                           LE_SUCCESS,
                       "add collider");
            memset(&info, 0, sizeof(info));
            le_object_get_info2(w, &o, &info);
            TEST_CHECK(info.has_rigid_body && info.has_collider,
                       "physics bits");
        }
        /* Character bit (static body allowed). */
        {
            le_character_desc chd;

            memset(&chd, 0, sizeof(chd));
            chd.radius = 0.4f;
            chd.height = 1.8f;
            chd.up[1] = 1.0f;
            chd.max_slope_angle = 0.7f;
            chd.layer = 0;
            chd.mask = 0xFFFFFFFFu;
            TEST_CHECK(le_object_add_character(w, &o, &chd) ==
                           LE_SUCCESS,
                       "add character");
            memset(&info, 0, sizeof(info));
            le_object_get_info2(w, &o, &info);
            TEST_CHECK(info.has_character, "character bit");
        }
        /* Script bit + failed flag. */
        {
            le_script_asset_desc sd;
            le_asset script;
            static const char src[] = "export('x', 1)\n";

            memset(&sd, 0, sizeof(sd));
            sd.source = src;
            sd.size = strlen(src);
            sd.path_hint = "info2.lua";
            TEST_CHECK(le_asset_create_script(e, &sd, &script) ==
                           LE_SUCCESS,
                       "create script");
            TEST_CHECK(le_object_add_script(w, &o, &script) ==
                           LE_SUCCESS,
                       "attach script");
            memset(&info, 0, sizeof(info));
            le_object_get_info2(w, &o, &info);
            TEST_CHECK(info.has_script, "script bit");
            TEST_CHECK(!info.script_failed, "script healthy");
            le_asset_unload(e, &script);
        }
        /* Stale handle zeroes with version 0. */
        {
            le_object stale = o;

            TEST_CHECK(le_object_destroy(w, &o) == LE_SUCCESS,
                       "destroy obj");
            memset(&info, 0xAA, sizeof(info));
            le_object_get_info2(w, &stale, &info);
            TEST_CHECK(info.version == 0 && !info.alive,
                       "stale zeroed");
        }
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    printf("Luma Engine Phase 31 enumeration headless tests: %d "
           "passed, %d failed\n",
           g_passed, g_failed);
    if (g_failed == 0) {
        printf("ALL PHASE 31 ENUMERATION HEADLESS TESTS PASSED\n");
    } else {
        printf("TESTS FAILED\n");
    }
    return (g_failed == 0) ? 0 : 1;
}
