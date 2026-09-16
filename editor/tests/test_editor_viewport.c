/*
 * Luma Editor Phase 31 headless tests: viewport orbit math, camera
 * derivation, unproject round-trip, raycast picking (hit + miss),
 * AABB composition, frame-selection, gizmo intents + line soup.
 *
 * No GPU, no window, no renderer: picking uses physics colliders.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string.h>

#include <luma_editor/luma_editor.h>

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

static int make_session(led_session **s, le_engine **e, le_world **w) {
    le_engine_desc ed;
    le_world_desc wd;

    memset(&ed, 0, sizeof(ed));
    memset(&wd, 0, sizeof(wd));
    if (le_engine_create(&ed, e) != LE_SUCCESS) {
        return 0;
    }
    if (le_world_create(*e, &wd, w) != LE_SUCCESS) {
        le_engine_destroy(*e);
        return 0;
    }
    if (led_session_create(s) != LED_SUCCESS) {
        le_world_destroy(*w);
        le_engine_destroy(*e);
        return 0;
    }
    if (led_session_attach(*s, *e, *w) != LED_SUCCESS) {
        led_session_destroy(*s);
        le_world_destroy(*w);
        le_engine_destroy(*e);
        return 0;
    }
    return 1;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Luma Editor Phase 31 viewport headless tests...\n");

    /* Defaults + NULL guards. */
    {
        led_viewport v;

        led_viewport_default(NULL);
        led_viewport_orbit(NULL, 0.1f, 0.1f);
        led_viewport_dolly(NULL, 2.0f);
        led_viewport_pan(NULL, 1.0f, 1.0f);
        TEST_CHECK(!led_viewport_camera(NULL, NULL),
                   "camera NULL 0");
        TEST_CHECK(!led_viewport_ray(NULL, 0, 0, NULL, NULL),
                   "ray NULL 0");
        led_viewport_default(&v);
        TEST_CHECK(v.width == 640 && v.height == 480,
                   "default extent");
        TEST_CHECK(v.distance == 8.0f, "default distance");
        TEST_CHECK(led_viewport_camera(&v, NULL),
                   "camera NULL-out validates");
        TEST_CHECK(!led_viewport_pick(NULL, &v, 0, 0, 0, 0, NULL),
                   "pick NULL session 0");
    }
    {
        led_viewport bad;

        memset(&bad, 0, sizeof(bad));
        TEST_CHECK(!led_viewport_camera(&bad, NULL),
                   "zero viewport invalid");
        TEST_CHECK(!led_viewport_ray(&bad, 10, 10, NULL, NULL),
                   "zero ray invalid");
    }

    /* Orbit math: yaw wrap, pitch/distance clamps. */
    {
        led_viewport v;

        led_viewport_default(&v);
        led_viewport_orbit(&v, 100.0f, 0.0f);
        TEST_CHECK(v.yaw_rad <= 3.14159265f &&
                       v.yaw_rad >= -3.14159265f,
                   "yaw wrapped");
        led_viewport_orbit(&v, 0.0f, 10.0f);
        TEST_CHECK(v.pitch_rad <= 1.55f, "pitch clamped hi");
        led_viewport_orbit(&v, 0.0f, -10.0f);
        TEST_CHECK(v.pitch_rad >= -1.55f, "pitch clamped lo");
        led_viewport_dolly(&v, 1e9f);
        TEST_CHECK(v.distance <= 1e5f, "distance clamped hi");
        led_viewport_dolly(&v, 1e-9f);
        TEST_CHECK(v.distance >= 0.05f, "distance clamped lo");
        led_viewport_dolly(&v, -2.0f);
        TEST_CHECK(v.distance >= 0.05f, "negative dolly ignored");
        {
            float nanf = strtof("nan", NULL);

            led_viewport_orbit(&v, nanf, 0.0f);
            TEST_CHECK(v.yaw_rad <= 3.14159265f, "NaN orbit ignored");
        }
    }

    /* Camera derivation: position/planes mirror state. */
    {
        led_viewport v;
        lr_camera cam;

        memset(&cam, 0, sizeof(cam));
        led_viewport_default(&v);
        TEST_CHECK(led_viewport_camera(&v, &cam), "camera ok");
        TEST_CHECK(cam.near_plane == v.near_plane, "near mirrors");
        TEST_CHECK(cam.far_plane == v.far_plane, "far mirrors");
        TEST_CHECK(cam.aspect_ratio ==
                       (float)v.width / (float)v.height,
                   "aspect mirrors");
    }

    /* Unproject round-trip: NDC-project a known point through the
     * camera, unproject the pixel, verify the ray passes near it. */
    {
        led_viewport v;
        lr_camera cam;

        memset(&cam, 0, sizeof(cam));
        led_viewport_default(&v);
        v.target[0] = 0.0f;
        v.target[1] = 0.0f;
        v.target[2] = 0.0f;
        TEST_CHECK(led_viewport_camera(&v, &cam), "camera rt");
        {
            /* Project world origin: clip = P*V*pos. */
            float clip[4] = { 0, 0, 0, 1 };
            int r;
            float ndc[3];
            float px;
            float py;
            float org[3];
            float dir[3];

            for (r = 0; r < 4; r++) {
                clip[r] = cam.view[0 * 4 + r] * 0.0f +
                          cam.view[1 * 4 + r] * 0.0f +
                          cam.view[2 * 4 + r] * 0.0f +
                          cam.view[3 * 4 + r] * 1.0f;
            }
            {
                float c2[4] = { 0, 0, 0, 1 };
                int rr;

                for (rr = 0; rr < 4; rr++) {
                    c2[rr] =
                        cam.projection[0 * 4 + rr] * clip[0] +
                        cam.projection[1 * 4 + rr] * clip[1] +
                        cam.projection[2 * 4 + rr] * clip[2] +
                        cam.projection[3 * 4 + rr] * clip[3];
                }
                memcpy(clip, c2, sizeof(clip));
            }
            TEST_CHECK(clip[3] != 0.0f, "clip w nonzero");
            ndc[0] = clip[0] / clip[3];
            ndc[1] = clip[1] / clip[3];
            ndc[2] = clip[2] / clip[3];
            px = (ndc[0] * 0.5f + 0.5f) * (float)v.width;
            py = (1.0f - (ndc[1] * 0.5f + 0.5f)) *
                 (float)v.height;
            TEST_CHECK(led_viewport_ray(&v, px, py, org, dir),
                       "unproject center ray");
            /* Ray-to-origin distance < 1e-3 (round-trip). */
            {
                float t = -(org[0] * dir[0] + org[1] * dir[1] +
                            org[2] * dir[2]);
                float q[3] = { org[0] + dir[0] * t,
                               org[1] + dir[1] * t,
                               org[2] + dir[2] * t };
                float d = sqrtf(q[0] * q[0] + q[1] * q[1] +
                                q[2] * q[2]);

                TEST_CHECK(d < 1e-3f, "ray passes origin");
            }
        }
        /* Corner rays are well-formed unit vectors. */
        {
            float org[3];
            float dir[3];
            float n;

            TEST_CHECK(led_viewport_ray(&v, 0, 0, org, dir),
                       "corner ray");
            n = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] +
                      dir[2] * dir[2]);
            TEST_CHECK(n > 0.999f && n < 1.001f, "ray unit");
        }
    }

    /* Picking: collider hit + miss + pick-select. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object box = LE_OBJECT_INVALID;
        le_collider_desc cd;
        led_viewport v;
        le_ray_hit hit;

        memset(&cd, 0, sizeof(cd));
        memset(&hit, 0, sizeof(hit));
        TEST_CHECK(make_session(&s, &e, &w), "make session");
        TEST_CHECK(le_object_create(w, &box) == LE_SUCCESS,
                   "pick box");
        cd.shape = LE_COLLIDER_BOX;
        cd.half_extents[0] = 1.0f;
        cd.half_extents[1] = 1.0f;
        cd.half_extents[2] = 1.0f;
        cd.orientation[3] = 1.0f;
        cd.layer = 0;
        cd.mask = 0xFFFFFFFFu;
        cd.friction = 0.5f;
        cd.restitution = 0.0f;
        TEST_CHECK(le_object_add_collider(w, &box, &cd) ==
                       LE_SUCCESS,
                   "add box collider");
        le_world_update(w, 0.0f);
        led_viewport_default(&v);
        v.target[0] = 0.0f;
        v.target[1] = 0.0f;
        v.target[2] = 0.0f;
        v.distance = 8.0f;
        /* Center pixel looks at the box: expect a hit. */
        TEST_CHECK(led_viewport_pick(s, &v, 320.0f, 240.0f, 0.0f,
                                     0xFFFFFFFFu, &hit),
                   "center pick hits");
        TEST_CHECK(hit.object.index == box.index, "hit the box");
        /* Far corner against empty sky... aim above the box: pitch
         * the camera target away so the corner misses. */
        {
            led_viewport sky = v;

            sky.target[0] = 0.0f;
            sky.target[1] = 100.0f;
            sky.target[2] = 0.0f;
            TEST_CHECK(!led_viewport_pick(s, &sky, 639.0f, 479.0f,
                                          0.0f, 0xFFFFFFFFu, NULL),
                       "sky corner misses");
        }
        /* Pick-select replaces selection on hit. */
        TEST_CHECK(led_viewport_pick_select(s, &v, 320.0f, 240.0f),
                   "pick-select hit");
        TEST_CHECK(led_selection_contains(s, &box),
                   "box selected");
        {
            led_viewport sky = v;

            sky.target[0] = 0.0f;
            sky.target[1] = 100.0f;
            sky.target[2] = 0.0f;
            TEST_CHECK(!led_viewport_pick_select(s, &sky, 639.0f,
                                                 479.0f),
                       "pick-select miss clears");
            TEST_CHECK(led_selection_get(s, NULL, 0) == 0,
                       "selection cleared");
        }
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* AABB without renderable: 0 (documented, never guessed). */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;
        float mn[3];
        float mx[3];

        TEST_CHECK(make_session(&s, &e, &w), "make session 2");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "obj");
        TEST_CHECK(!led_compute_world_aabb(s, &o, mn, mx),
                   "no renderable no AABB");
        TEST_CHECK(!led_compute_world_aabb(s, NULL, mn, mx),
                   "NULL no AABB");
        TEST_CHECK(!led_compute_world_aabb(NULL, &o, mn, mx),
                   "NULL session no AABB");
        TEST_CHECK(!led_selection_aabb(s, mn, mx),
                   "empty selection no AABB");
        TEST_CHECK(!led_frame_selection(NULL, NULL),
                   "frame NULL 0");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Frame selection moves target + distance. */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object a = LE_OBJECT_INVALID;
        le_object b = LE_OBJECT_INVALID;
        led_viewport v;

        TEST_CHECK(make_session(&s, &e, &w), "make session 3");
        led_viewport_default(&v);
        TEST_CHECK(le_object_create(w, &a) == LE_SUCCESS, "a");
        TEST_CHECK(le_object_create(w, &b) == LE_SUCCESS, "b");
        {
            float pa[3] = { -5.0f, 0.0f, 0.0f };
            float pb[3] = { 5.0f, 0.0f, 0.0f };

            TEST_CHECK(le_object_set_position(w, &a, pa) ==
                           LE_SUCCESS,
                       "pos a");
            TEST_CHECK(le_object_set_position(w, &b, pb) ==
                           LE_SUCCESS,
                       "pos b");
        }
        TEST_CHECK(led_selection_set(s, &a, 1) == LED_SUCCESS,
                   "select a");
        TEST_CHECK(led_frame_selection(s, &v), "frame a");
        TEST_CHECK(v.target[0] > -6.0f && v.target[0] < -4.0f,
                   "target near a");
        {
            le_object both[2] = { a, b };

            TEST_CHECK(led_selection_set(s, both, 2) == LED_SUCCESS,
                       "select both");
            TEST_CHECK(led_frame_selection(s, &v), "frame both");
            TEST_CHECK(v.target[0] > -1.0f && v.target[0] < 1.0f,
                       "target centered");
            TEST_CHECK(v.distance > 8.0f, "distance widened");
        }
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    /* Gizmo begin/apply/lines (translate single selection). */
    {
        led_session *s = NULL;
        le_engine *e = NULL;
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;
        led_gizmo_drag drag;

        memset(&drag, 0, sizeof(drag));
        TEST_CHECK(make_session(&s, &e, &w), "make session 4");
        TEST_CHECK(!led_gizmo_begin(s, LED_GIZMO_TRANSLATE, 0),
                   "gizmo w/o selection 0");
        TEST_CHECK(le_object_create(w, &o) == LE_SUCCESS, "obj");
        TEST_CHECK(led_selection_set(s, &o, 1) == LED_SUCCESS,
                   "select");
        TEST_CHECK(led_history_set_coalesce(s, 0, 0) == LED_SUCCESS,
                   "coalesce off");
        TEST_CHECK(led_gizmo_begin(s, LED_GIZMO_TRANSLATE, 0),
                   "gizmo begin x");
        drag.mode = LED_GIZMO_TRANSLATE;
        drag.axis = 0;
        drag.start_world[0] = 0.0f;
        drag.current_world[0] = 2.0f;
        drag.snap = 0.0f;
        TEST_CHECK(led_gizmo_apply(s, &drag) == LED_SUCCESS,
                   "gizmo apply");
        {
            float p[3];

            le_object_get_position(w, &o, p);
            TEST_CHECK(p[0] == 2.0f && p[1] == 0.0f,
                       "gizmo moved x only");
        }
        TEST_CHECK(led_undo(s) == 1, "undo gizmo");
        {
            float p[3];

            le_object_get_position(w, &o, p);
            TEST_CHECK(p[0] == 0.0f, "gizmo undone");
        }
        /* Snap: 2.3 with 0.5 snap -> 2.5. */
        drag.current_world[0] = 2.3f;
        drag.snap = 0.5f;
        TEST_CHECK(led_gizmo_apply(s, &drag) == LED_SUCCESS,
                   "gizmo snap apply");
        {
            float p[3];

            le_object_get_position(w, &o, p);
            TEST_CHECK(p[0] == 2.5f, "gizmo snapped");
        }
        /* Lines: counting query then fill (needs selection AABB;
         * plain object has no AABB -> position fallback path in
         * selection_aabb gives a degenerate box — lines still
         * emit. */
        {
            led_viewport v;
            uint32_t need;

            led_viewport_default(&v);
            need = led_gizmo_lines(s, &v, NULL, 0);
            TEST_CHECK(need == 90, "gizmo lines need 90");
            {
                float buf[90];

                TEST_CHECK(led_gizmo_lines(s, &v, buf, 90) == 90,
                           "gizmo lines fill");
            }
        }
        TEST_CHECK(led_gizmo_apply(NULL, NULL) ==
                       LED_ERROR_INVALID_ARGUMENT,
                   "gizmo apply NULL INVALID");
        led_session_destroy(s);
        le_world_destroy(w);
        le_engine_destroy(e);
    }

    printf("Luma Editor Phase 31 viewport headless tests: %d passed, "
           "%d failed\n",
           g_passed, g_failed);
    if (g_failed == 0) {
        printf("ALL PHASE 31 VIEWPORT HEADLESS TESTS PASSED\n");
    } else {
        printf("TESTS FAILED\n");
    }
    return (g_failed == 0) ? 0 : 1;
}
