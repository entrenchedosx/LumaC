/*
 * Luma Engine Phase 29 headless tests: animation, skeletons &
 * GPU skinning foundation (engine-owned animation, no renderer/
 * GPU includes; engine public API only).
 *
 * Coverage: skeleton asset validation (hierarchy, transforms,
 * registry census), clip validation (times/values/quats, cubic
 * stride), sampling (clamp/STEP/duplicates/stale/OBJECT slots),
 * bind pose, blend pose, CPU skinning oracle, animator lifecycle,
 * playback advance (LOOP/ONCE/PING_PONG, OBJECT writes,
 * enable/disable), crossfades, physics ownership, scene
 * round-trip (strict parse), stats, seek determinism.
 *
 * No GPU, no window, no Lua: pure C API via le_world_update.
 * Headless-safe throughout.
 */
#include <math.h>
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

#define NEAR(a, b, eps) (fabsf((float)(a) - (float)(b)) <= (float)(eps))

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

static void step_world(le_world *world, float dt) {
    le_world_update(world, dt);
}

static void ident_mat(float m[16]) {
    memset(m, 0, 16u * sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

/* Fill one joint desc: identity rotation/scale-bind, identity
 * inverse bind, caller-chosen name/parent/translation. */
static void fill_joint(le_skeleton_joint_desc *d, const char *name,
                       int32_t parent, float tx, float ty,
                       float tz) {
    memset(d, 0, sizeof(*d));
    snprintf(d->name, sizeof(d->name), "%s", name);
    d->parent = parent;
    d->translation[0] = tx;
    d->translation[1] = ty;
    d->translation[2] = tz;
    d->rotation[3] = 1.0f;
    d->scale[0] = 1.0f;
    d->scale[1] = 1.0f;
    d->scale[2] = 1.0f;
    ident_mat(d->inverse_bind);
}

/* Valid 3-joint chain root->mid->tip (translations 1/2/3 on
 * x/y/z for bind-pose checks). */
static le_result make_chain(le_engine *engine, le_asset *out) {
    le_skeleton_joint_desc j[3];
    le_skeleton_asset_desc d;

    fill_joint(&j[0], "root", -1, 1.0f, 0.0f, 0.0f);
    fill_joint(&j[1], "mid", 0, 0.0f, 2.0f, 0.0f);
    fill_joint(&j[2], "tip", 1, 0.0f, 0.0f, 3.0f);
    memset(&d, 0, sizeof(d));
    d.joints = j;
    d.joint_count = 3;
    return le_asset_create_skeleton(engine, &d, out);
}

/* OBJECT translation ramp clip (owner x: 0 -> x1 over dur). */
static le_result make_obj_ramp(le_engine *engine, float dur,
                               float x1, le_asset *out) {
    float times[2] = { 0.0f, 0.0f };
    float values[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    le_anim_track_desc tr;
    le_animation_clip_desc d;

    times[1] = dur;
    values[3] = x1;
    memset(&tr, 0, sizeof(tr));
    tr.target_kind = LE_ANIM_TARGET_OBJECT;
    tr.target_index = 0;
    tr.channel = LE_ANIM_CHANNEL_TRANSLATION;
    tr.interpolation = LE_ANIM_INTERP_LINEAR;
    tr.times = times;
    tr.values = values;
    tr.key_count = 2;
    memset(&d, 0, sizeof(d));
    d.duration = dur;
    d.tracks = &tr;
    d.track_count = 1;
    return le_asset_create_clip(engine, &d, out);
}

/* OBJECT translation constant clip (single key). */
static le_result make_obj_const(le_engine *engine, float x,
                               le_asset *out) {
    float times[1] = { 0.0f };
    float values[3] = { 0.0f, 0.0f, 0.0f };
    le_anim_track_desc tr;
    le_animation_clip_desc d;

    values[0] = x;
    memset(&tr, 0, sizeof(tr));
    tr.target_kind = LE_ANIM_TARGET_OBJECT;
    tr.target_index = 0;
    tr.channel = LE_ANIM_CHANNEL_TRANSLATION;
    tr.interpolation = LE_ANIM_INTERP_LINEAR;
    tr.times = times;
    tr.values = values;
    tr.key_count = 1;
    memset(&d, 0, sizeof(d));
    d.duration = 1.0f;
    d.tracks = &tr;
    d.track_count = 1;
    return le_asset_create_clip(engine, &d, out);
}

/* Joint translation ramp clip (joint 0 x: 0 -> x1 over dur). */
static le_result make_joint_ramp(le_engine *engine, float dur,
                                 float x1, le_asset *out) {
    float times[2] = { 0.0f, 0.0f };
    float values[6] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    le_anim_track_desc tr;
    le_animation_clip_desc d;

    times[1] = dur;
    values[3] = x1;
    memset(&tr, 0, sizeof(tr));
    tr.target_kind = LE_ANIM_TARGET_JOINT;
    tr.target_index = 0;
    tr.channel = LE_ANIM_CHANNEL_TRANSLATION;
    tr.interpolation = LE_ANIM_INTERP_LINEAR;
    tr.times = times;
    tr.values = values;
    tr.key_count = 2;
    memset(&d, 0, sizeof(d));
    d.duration = dur;
    d.tracks = &tr;
    d.track_count = 1;
    return le_asset_create_clip(engine, &d, out);
}

static float pos_x(le_world *world, const le_object *o) {
    float p[3];

    le_object_get_position(world, o, p);
    return p[0];
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 29 headless tests...\n");

    TEST_CHECK(LE_COMPONENT_COUNT == 9, "component count is 9");
    TEST_CHECK(LE_COMPONENT_ANIMATOR == 7, "animator component is 7");

    /* NULL-safety of every new API. */
    TEST_CHECK(le_asset_create_skeleton(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "skeleton NULL INVALID");
    TEST_CHECK(le_asset_create_clip(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "clip NULL INVALID");
    TEST_CHECK(le_skeleton_get_joint_count(NULL, NULL) == 0,
               "joint count NULL 0");
    TEST_CHECK(le_skeleton_find_joint(NULL, NULL, NULL, NULL) == 0,
               "find joint NULL 0");
    TEST_CHECK(le_clip_get_duration(NULL, NULL) == 0.0f,
               "clip duration NULL 0");
    TEST_CHECK(le_clip_get_track_count(NULL, NULL) == 0,
               "clip tracks NULL 0");
    TEST_CHECK(le_anim_sample_clip(NULL, NULL, 0.0f, NULL, NULL,
                                   NULL, 0, NULL, NULL,
                                   NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "sample NULL INVALID");
    TEST_CHECK(le_anim_bind_pose(NULL, NULL, NULL, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "bind NULL INVALID");
    TEST_CHECK(le_anim_blend_pose(0, NULL, NULL, NULL, NULL, NULL,
                                  NULL, 0.0f, NULL, NULL,
                                  NULL) == LE_ERROR_INVALID_ARGUMENT,
               "blend NULL INVALID");
    TEST_CHECK(le_anim_skin_vertex(NULL, NULL, NULL, NULL, NULL, 0,
                                   NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "skin NULL INVALID");
    TEST_CHECK(le_object_add_animator(NULL, NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "add animator NULL INVALID");
    TEST_CHECK(le_object_remove_animator(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "remove animator NULL INVALID");
    TEST_CHECK(le_object_get_animator(NULL, NULL, NULL) == 0,
               "get animator NULL 0");
    TEST_CHECK(le_anim_play(NULL, NULL, NULL, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "play NULL INVALID");
    TEST_CHECK(le_anim_pause(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "pause NULL INVALID");
    TEST_CHECK(le_anim_resume(NULL, NULL) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "resume NULL INVALID");
    TEST_CHECK(le_anim_stop(NULL, NULL, 0) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "stop NULL INVALID");
    TEST_CHECK(le_anim_seek(NULL, NULL, 0.0f) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "seek NULL INVALID");
    TEST_CHECK(le_anim_set_speed(NULL, NULL, 1.0f) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "speed NULL INVALID");
    TEST_CHECK(le_anim_set_loop(NULL, NULL, LE_ANIM_LOOP) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "loop NULL INVALID");
    TEST_CHECK(le_anim_crossfade(NULL, NULL, NULL, 0.0f) ==
                   LE_ERROR_INVALID_ARGUMENT,
               "crossfade NULL INVALID");
    TEST_CHECK(le_anim_is_playing(NULL, NULL) == 0,
               "is_playing NULL 0");
    TEST_CHECK(le_anim_get_time(NULL, NULL) == 0.0f,
               "get time NULL 0");
    TEST_CHECK(le_anim_get_duration(NULL, NULL) == 0.0f,
               "get duration NULL 0");
    {
        le_anim_stats st;

        memset(&st, 0xAB, sizeof(st));
        le_anim_get_stats(NULL, &st);
        TEST_CHECK(st.animator_count == 0 &&
                       st.playing_count == 0 &&
                       st.active_crossfades == 0,
                   "stats NULL zero");
    }

    /* Skeleton assets: valid chain + lookups. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset skel = LE_ASSET_INVALID;
        uint32_t idx = 0;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(make_chain(engine, &skel) == LE_SUCCESS,
                   "chain skeleton creates");
        TEST_CHECK(le_skeleton_get_joint_count(engine, &skel) == 3,
                   "chain joint count 3");
        TEST_CHECK(le_skeleton_find_joint(engine, &skel, "root",
                                          &idx) == 1 &&
                       idx == 0,
                   "find root 0");
        TEST_CHECK(le_skeleton_find_joint(engine, &skel, "mid",
                                          &idx) == 1 &&
                       idx == 1,
                   "find mid 1");
        TEST_CHECK(le_skeleton_find_joint(engine, &skel, "tip",
                                          &idx) == 1 &&
                       idx == 2,
                   "find tip 2");
        TEST_CHECK(le_skeleton_find_joint(engine, &skel, "nope",
                                          NULL) == 0,
                   "find missing 0");
        TEST_CHECK(le_skeleton_find_joint(engine, &skel, NULL,
                                          NULL) == 0,
                   "find NULL name 0");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Duplicate names: allowed, first match wins. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_skeleton_joint_desc j[2];
        le_skeleton_asset_desc d;
        le_asset skel = LE_ASSET_INVALID;
        uint32_t idx = 99;

        make_engine(&engine);
        make_world(engine, &world);
        fill_joint(&j[0], "dup", -1, 0.0f, 0.0f, 0.0f);
        fill_joint(&j[1], "dup", 0, 0.0f, 1.0f, 0.0f);
        memset(&d, 0, sizeof(d));
        d.joints = j;
        d.joint_count = 2;
        TEST_CHECK(le_asset_create_skeleton(engine, &d, &skel) ==
                       LE_SUCCESS,
                   "duplicate names allowed");
        TEST_CHECK(le_skeleton_find_joint(engine, &skel, "dup",
                                          &idx) == 1 &&
                       idx == 0,
                   "duplicate first-wins");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Malformed skeletons: all rejected, registry unchanged. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset_stats st;
        le_asset bad = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        /* Zero joints. */
        {
            le_skeleton_asset_desc d;

            memset(&d, 0, sizeof(d));
            d.joints = NULL;
            d.joint_count = 0;
            TEST_CHECK(le_asset_create_skeleton(engine, &d,
                                                &bad) ==
                           LE_ERROR_INVALID_ARGUMENT &&
                           !le_asset_is_valid(&bad),
                       "zero joints rejected");
            le_engine_get_asset_stats(engine, &st);
            TEST_CHECK(st.skeleton_count == 0,
                       "census clean after zero joints");
        }
        /* Self-parent. */
        {
            le_skeleton_joint_desc j[1];
            le_skeleton_asset_desc d;

            fill_joint(&j[0], "s", 0, 0.0f, 0.0f, 0.0f);
            memset(&d, 0, sizeof(d));
            d.joints = j;
            d.joint_count = 1;
            TEST_CHECK(le_asset_create_skeleton(engine, &d,
                                                &bad) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "self-parent rejected");
            le_engine_get_asset_stats(engine, &st);
            TEST_CHECK(st.skeleton_count == 0,
                       "census clean after self-parent");
        }
        /* Parent out of range. */
        {
            le_skeleton_joint_desc j[1];
            le_skeleton_asset_desc d;

            fill_joint(&j[0], "o", 7, 0.0f, 0.0f, 0.0f);
            memset(&d, 0, sizeof(d));
            d.joints = j;
            d.joint_count = 1;
            TEST_CHECK(le_asset_create_skeleton(engine, &d,
                                                &bad) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "parent OOB rejected");
            le_engine_get_asset_stats(engine, &st);
            TEST_CHECK(st.skeleton_count == 0,
                       "census clean after OOB parent");
        }
        /* Cycle A<->B with no root. */
        {
            le_skeleton_joint_desc j[2];
            le_skeleton_asset_desc d;

            fill_joint(&j[0], "a", 1, 0.0f, 0.0f, 0.0f);
            fill_joint(&j[1], "b", 0, 0.0f, 0.0f, 0.0f);
            memset(&d, 0, sizeof(d));
            d.joints = j;
            d.joint_count = 2;
            TEST_CHECK(le_asset_create_skeleton(engine, &d,
                                                &bad) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "rootless cycle rejected");
            le_engine_get_asset_stats(engine, &st);
            TEST_CHECK(st.skeleton_count == 0,
                       "census clean after cycle");
        }
        /* Detached cycle beside a valid root (unreachable). */
        {
            le_skeleton_joint_desc j[3];
            le_skeleton_asset_desc d;

            fill_joint(&j[0], "r", -1, 0.0f, 0.0f, 0.0f);
            fill_joint(&j[1], "a", 2, 0.0f, 0.0f, 0.0f);
            fill_joint(&j[2], "b", 1, 0.0f, 0.0f, 0.0f);
            memset(&d, 0, sizeof(d));
            d.joints = j;
            d.joint_count = 3;
            TEST_CHECK(le_asset_create_skeleton(engine, &d,
                                                &bad) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "unreachable cycle rejected");
            le_engine_get_asset_stats(engine, &st);
            TEST_CHECK(st.skeleton_count == 0,
                       "census clean after unreachable");
        }
        /* Zero scale. */
        {
            le_skeleton_joint_desc j[1];
            le_skeleton_asset_desc d;

            fill_joint(&j[0], "z", -1, 0.0f, 0.0f, 0.0f);
            j[0].scale[1] = 0.0f;
            memset(&d, 0, sizeof(d));
            d.joints = j;
            d.joint_count = 1;
            TEST_CHECK(le_asset_create_skeleton(engine, &d,
                                                &bad) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "zero scale rejected");
            le_engine_get_asset_stats(engine, &st);
            TEST_CHECK(st.skeleton_count == 0,
                       "census clean after zero scale");
        }
        /* NaN translation. */
        {
            le_skeleton_joint_desc j[1];
            le_skeleton_asset_desc d;

            fill_joint(&j[0], "n", -1, 0.0f, 0.0f, 0.0f);
            j[0].translation[0] = (float)NAN;
            memset(&d, 0, sizeof(d));
            d.joints = j;
            d.joint_count = 1;
            TEST_CHECK(le_asset_create_skeleton(engine, &d,
                                                &bad) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "NaN translation rejected");
            le_engine_get_asset_stats(engine, &st);
            TEST_CHECK(st.skeleton_count == 0,
                       "census clean after NaN");
        }
        /* Zero-length bind quat. */
        {
            le_skeleton_joint_desc j[1];
            le_skeleton_asset_desc d;

            fill_joint(&j[0], "q", -1, 0.0f, 0.0f, 0.0f);
            j[0].rotation[0] = 0.0f;
            j[0].rotation[1] = 0.0f;
            j[0].rotation[2] = 0.0f;
            j[0].rotation[3] = 0.0f;
            memset(&d, 0, sizeof(d));
            d.joints = j;
            d.joint_count = 1;
            TEST_CHECK(le_asset_create_skeleton(engine, &d,
                                                &bad) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "zero quat rejected");
            le_engine_get_asset_stats(engine, &st);
            TEST_CHECK(st.skeleton_count == 0,
                       "census clean after zero quat");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Clip assets: T/R/S/cubic valid tracks. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset clipT = LE_ASSET_INVALID;
        le_asset clipR = LE_ASSET_INVALID;
        le_asset clipS = LE_ASSET_INVALID;
        le_asset clipC = LE_ASSET_INVALID;
        le_asset empty = LE_ASSET_INVALID;
        le_animation_clip_desc d;
        le_anim_track_desc tr;

        make_engine(&engine);
        make_world(engine, &world);
        /* T linear 0 -> (1,2,3) over 1s. */
        {
            float times[2] = { 0.0f, 1.0f };
            float values[6] = { 0.0f, 0.0f, 0.0f,
                                1.0f, 2.0f, 3.0f };

            memset(&tr, 0, sizeof(tr));
            tr.target_kind = LE_ANIM_TARGET_JOINT;
            tr.target_index = 0;
            tr.channel = LE_ANIM_CHANNEL_TRANSLATION;
            tr.interpolation = LE_ANIM_INTERP_LINEAR;
            tr.times = times;
            tr.values = values;
            tr.key_count = 2;
            memset(&d, 0, sizeof(d));
            d.duration = 1.0f;
            d.tracks = &tr;
            d.track_count = 1;
            TEST_CHECK(le_asset_create_clip(engine, &d, &clipT) ==
                           LE_SUCCESS,
                       "T linear clip creates");
        }
        /* R slerp identity -> 90deg Y over 2s. */
        {
            float times[2] = { 0.0f, 2.0f };
            float values[8] = { 0.0f, 0.0f, 0.0f, 1.0f,
                                0.0f, 0.70710678f, 0.0f,
                                0.70710678f };

            memset(&tr, 0, sizeof(tr));
            tr.target_kind = LE_ANIM_TARGET_JOINT;
            tr.target_index = 1;
            tr.channel = LE_ANIM_CHANNEL_ROTATION;
            tr.interpolation = LE_ANIM_INTERP_LINEAR;
            tr.times = times;
            tr.values = values;
            tr.key_count = 2;
            memset(&d, 0, sizeof(d));
            d.duration = 2.0f;
            d.tracks = &tr;
            d.track_count = 1;
            TEST_CHECK(le_asset_create_clip(engine, &d, &clipR) ==
                           LE_SUCCESS,
                       "R slerp clip creates");
        }
        /* S step 1 -> 2 -> 3. */
        {
            float times[3] = { 0.0f, 0.5f, 1.0f };
            float values[9] = { 1.0f, 1.0f, 1.0f,
                                2.0f, 2.0f, 2.0f,
                                3.0f, 3.0f, 3.0f };

            memset(&tr, 0, sizeof(tr));
            tr.target_kind = LE_ANIM_TARGET_JOINT;
            tr.target_index = 2;
            tr.channel = LE_ANIM_CHANNEL_SCALE;
            tr.interpolation = LE_ANIM_INTERP_STEP;
            tr.times = times;
            tr.values = values;
            tr.key_count = 3;
            memset(&d, 0, sizeof(d));
            d.duration = 1.0f;
            d.tracks = &tr;
            d.track_count = 1;
            TEST_CHECK(le_asset_create_clip(engine, &d, &clipS) ==
                           LE_SUCCESS,
                       "S step clip creates");
        }
        /* CUBICSPLINE: zero tangents, values 0 -> (2,4,6). */
        {
            float times[2] = { 0.0f, 1.0f };
            float values[18] = { 0.0f, 0.0f, 0.0f,
                                 0.0f, 0.0f, 0.0f,
                                 0.0f, 0.0f, 0.0f,
                                 0.0f, 0.0f, 0.0f,
                                 2.0f, 4.0f, 6.0f,
                                 0.0f, 0.0f, 0.0f };

            memset(&tr, 0, sizeof(tr));
            tr.target_kind = LE_ANIM_TARGET_JOINT;
            tr.target_index = 0;
            tr.channel = LE_ANIM_CHANNEL_TRANSLATION;
            tr.interpolation = LE_ANIM_INTERP_CUBICSPLINE;
            tr.times = times;
            tr.values = values;
            tr.key_count = 2;
            memset(&d, 0, sizeof(d));
            d.duration = 1.0f;
            d.tracks = &tr;
            d.track_count = 1;
            TEST_CHECK(le_asset_create_clip(engine, &d, &clipC) ==
                           LE_SUCCESS,
                       "cubic clip creates");
        }
        TEST_CHECK(le_clip_get_duration(engine, &clipT) == 1.0f &&
                       le_clip_get_duration(engine, &clipR) == 2.0f,
                   "clip durations read back");
        TEST_CHECK(le_clip_get_track_count(engine, &clipT) == 1 &&
                       le_clip_get_track_count(engine, &clipC) == 1,
                   "clip track counts read back");
        /* Empty clip (zero tracks) is legal. */
        {
            memset(&d, 0, sizeof(d));
            d.duration = 2.0f;
            d.tracks = NULL;
            d.track_count = 0;
            TEST_CHECK(le_asset_create_clip(engine, &d, &empty) ==
                           LE_SUCCESS &&
                           le_clip_get_track_count(engine,
                                                   &empty) == 0,
                       "empty clip creates");
        }
        /* Malformed clips: rejected, census unchanged (5 live). */
        {
            le_asset_stats st;
            le_asset badc = LE_ASSET_INVALID;
            le_animation_clip_desc dd;
            le_anim_track_desc tt;
            float times[2] = { 0.0f, 1.0f };
            float values[6] = { 0.0f, 0.0f, 0.0f,
                                1.0f, 2.0f, 3.0f };

            memset(&tt, 0, sizeof(tt));
            tt.target_kind = LE_ANIM_TARGET_JOINT;
            tt.target_index = 0;
            tt.channel = LE_ANIM_CHANNEL_TRANSLATION;
            tt.interpolation = LE_ANIM_INTERP_LINEAR;
            tt.times = times;
            tt.values = values;
            tt.key_count = 2;
            memset(&dd, 0, sizeof(dd));
            dd.duration = 0.0f;
            dd.tracks = &tt;
            dd.track_count = 1;
            TEST_CHECK(le_asset_create_clip(engine, &dd, &badc) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "zero duration rejected");
            dd.duration = (float)NAN;
            TEST_CHECK(le_asset_create_clip(engine, &dd, &badc) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "NaN duration rejected");
            dd.duration = 1.0f;
            times[0] = 1.0f;
            times[1] = 0.0f;
            TEST_CHECK(le_asset_create_clip(engine, &dd, &badc) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "unsorted times rejected");
            times[0] = 0.0f;
            times[1] = 1.0f;
            values[0] = (float)NAN;
            TEST_CHECK(le_asset_create_clip(engine, &dd, &badc) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "NaN values rejected");
            values[0] = 0.0f;
            tt.channel = LE_ANIM_CHANNEL_ROTATION;
            {
                float qv[8] = { 0.0f, 0.0f, 0.0f, 0.0f,
                                0.0f, 0.0f, 0.0f, 0.0f };

                tt.values = qv;
                TEST_CHECK(le_asset_create_clip(engine, &dd,
                                                &badc) ==
                               LE_ERROR_INVALID_ARGUMENT,
                           "zero quat rejected");
                tt.values = values;
            }
            tt.channel = LE_ANIM_CHANNEL_TRANSLATION;
            tt.key_count = 0;
            TEST_CHECK(le_asset_create_clip(engine, &dd, &badc) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "key_count 0 rejected");
            le_engine_get_asset_stats(engine, &st);
            TEST_CHECK(st.clip_count == 5,
                       "clip census unchanged after failures");
        }
        /* Sampling: T linear exact/between/past-end. */
        {
            float t[3][3];
            float r[3][4];
            float s[3][3];

            memset(t, 0, sizeof(t));
            memset(r, 0, sizeof(r));
            memset(s, 0, sizeof(s));
            for (int j = 0; j < 3; j++) {
                r[j][3] = 1.0f;
                s[j][0] = s[j][1] = s[j][2] = 1.0f;
            }
            TEST_CHECK(le_anim_sample_clip(engine, &clipT, 0.0f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           t[0][0] == 0.0f && t[0][1] == 0.0f,
                       "sample T at key 0");
            TEST_CHECK(le_anim_sample_clip(engine, &clipT, 1.0f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           NEAR(t[0][0], 1.0f, 1e-6f) &&
                           NEAR(t[0][2], 3.0f, 1e-6f),
                       "sample T at end key");
            TEST_CHECK(le_anim_sample_clip(engine, &clipT, 0.5f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           NEAR(t[0][0], 0.5f, 1e-6f) &&
                           NEAR(t[0][1], 1.0f, 1e-6f),
                       "sample T lerps between keys");
            TEST_CHECK(le_anim_sample_clip(engine, &clipT, 5.0f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           NEAR(t[0][0], 1.0f, 1e-6f),
                       "sample T past end clamps");
        }
        /* Sampling: STEP semantics. */
        {
            float t[3][3];
            float r[3][4];
            float s[3][3];

            memset(t, 0, sizeof(t));
            memset(r, 0, sizeof(r));
            memset(s, 0, sizeof(s));
            for (int j = 0; j < 3; j++) {
                r[j][3] = 1.0f;
            }
            TEST_CHECK(le_anim_sample_clip(engine, &clipS, 0.25f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           s[2][0] == 1.0f,
                       "STEP holds previous value");
            TEST_CHECK(le_anim_sample_clip(engine, &clipS, 0.5f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           s[2][0] == 2.0f,
                       "STEP exact key takes next value");
            TEST_CHECK(le_anim_sample_clip(engine, &clipS, 0.75f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           s[2][1] == 2.0f,
                       "STEP holds mid-segment");
        }
        /* Sampling: R slerp endpoints + midpoint. */
        {
            float t[3][3];
            float r[3][4];
            float s[3][3];

            memset(t, 0, sizeof(t));
            memset(r, 0, sizeof(r));
            memset(s, 0, sizeof(s));
            TEST_CHECK(le_anim_sample_clip(engine, &clipR, 0.0f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           NEAR(r[1][3], 1.0f, 1e-6f) &&
                           NEAR(r[1][1], 0.0f, 1e-6f),
                       "R at 0 is identity");
            TEST_CHECK(le_anim_sample_clip(engine, &clipR, 2.0f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           NEAR(r[1][1], 0.70710678f, 1e-5f) &&
                           NEAR(r[1][3], 0.70710678f, 1e-5f),
                       "R at end is 90deg Y");
            TEST_CHECK(le_anim_sample_clip(engine, &clipR, 1.0f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           NEAR(r[1][1], 0.38268343f, 1e-4f) &&
                           NEAR(r[1][3], 0.92387953f, 1e-4f),
                       "R midpoint is 45deg Y");
        }
        /* Sampling: cubic midpoint + endpoints. */
        {
            float t[3][3];
            float r[3][4];
            float s[3][3];

            memset(t, 0, sizeof(t));
            memset(r, 0, sizeof(r));
            memset(s, 0, sizeof(s));
            TEST_CHECK(le_anim_sample_clip(engine, &clipC, 0.5f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           NEAR(t[0][0], 1.0f, 1e-5f) &&
                           NEAR(t[0][1], 2.0f, 1e-5f) &&
                           NEAR(t[0][2], 3.0f, 1e-5f),
                       "cubic midpoint interpolates");
            TEST_CHECK(le_anim_sample_clip(engine, &clipC, 0.0f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           t[0][0] == 0.0f,
                       "cubic start exact");
        }
        /* Sampling: duplicate timestamps last-wins. */
        {
            float times[4] = { 0.0f, 0.5f, 0.5f, 1.0f };
            float values[12] = { 0.0f, 0.0f, 0.0f,
                                 1.0f, 0.0f, 0.0f,
                                 2.0f, 0.0f, 0.0f,
                                 3.0f, 0.0f, 0.0f };
            le_anim_track_desc dtr;
            le_animation_clip_desc dd;
            le_asset dup = LE_ASSET_INVALID;
            float t[3][3];
            float r[3][4];
            float s[3][3];

            memset(&dtr, 0, sizeof(dtr));
            dtr.target_kind = LE_ANIM_TARGET_JOINT;
            dtr.target_index = 0;
            dtr.channel = LE_ANIM_CHANNEL_TRANSLATION;
            dtr.interpolation = LE_ANIM_INTERP_LINEAR;
            dtr.times = times;
            dtr.values = values;
            dtr.key_count = 4;
            memset(&dd, 0, sizeof(dd));
            dd.duration = 1.0f;
            dd.tracks = &dtr;
            dd.track_count = 1;
            memset(t, 0, sizeof(t));
            memset(r, 0, sizeof(r));
            memset(s, 0, sizeof(s));
            TEST_CHECK(le_asset_create_clip(engine, &dd, &dup) ==
                           LE_SUCCESS,
                       "duplicate-time clip creates");
            TEST_CHECK(le_anim_sample_clip(engine, &dup, 0.5f, t, r,
                                           s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           NEAR(t[0][0], 2.0f, 1e-6f),
                       "duplicate timestamps last-wins");
        }
        /* Sampling: stale joint target skipped, no OOB. */
        {
            float times[2] = { 0.0f, 1.0f };
            float values[6] = { 9.0f, 9.0f, 9.0f,
                                9.0f, 9.0f, 9.0f };
            le_anim_track_desc dtr;
            le_animation_clip_desc dd;
            le_asset stale = LE_ASSET_INVALID;
            float t[3][3];
            float r[3][4];
            float s[3][3];

            memset(&dtr, 0, sizeof(dtr));
            dtr.target_kind = LE_ANIM_TARGET_JOINT;
            dtr.target_index = 99;
            dtr.channel = LE_ANIM_CHANNEL_TRANSLATION;
            dtr.interpolation = LE_ANIM_INTERP_LINEAR;
            dtr.times = times;
            dtr.values = values;
            dtr.key_count = 2;
            memset(&dd, 0, sizeof(dd));
            dd.duration = 1.0f;
            dd.tracks = &dtr;
            dd.track_count = 1;
            TEST_CHECK(le_asset_create_clip(engine, &dd, &stale) ==
                           LE_SUCCESS,
                       "stale-target clip creates");
            for (int j = 0; j < 3; j++) {
                t[j][0] = -1.0f;
                t[j][1] = -2.0f;
                t[j][2] = -3.0f;
                r[j][0] = r[j][1] = r[j][2] = 0.0f;
                r[j][3] = 1.0f;
                s[j][0] = s[j][1] = s[j][2] = 1.0f;
            }
            TEST_CHECK(le_anim_sample_clip(engine, &stale, 0.5f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) == LE_SUCCESS &&
                           t[0][0] == -1.0f && t[2][2] == -3.0f,
                       "stale target skipped without OOB");
        }
        /* Sampling: OBJECT tracks land in out_obj slots. */
        {
            le_asset objclip = LE_ASSET_INVALID;
            float t[3][3];
            float r[3][4];
            float s[3][3];
            float ot[3] = { -7.0f, -7.0f, -7.0f };
            float orr[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
            float os[3] = { 1.0f, 1.0f, 1.0f };

            TEST_CHECK(make_obj_ramp(engine, 2.0f, 4.0f,
                                     &objclip) == LE_SUCCESS,
                       "object ramp clip creates");
            for (int j = 0; j < 3; j++) {
                t[j][0] = -1.0f;
                t[j][1] = -1.0f;
                t[j][2] = -1.0f;
                r[j][0] = r[j][1] = r[j][2] = 0.0f;
                r[j][3] = 1.0f;
                s[j][0] = s[j][1] = s[j][2] = 1.0f;
            }
            TEST_CHECK(le_anim_sample_clip(engine, &objclip, 1.0f,
                                           t, r, s, 3, ot, orr,
                                           os) == LE_SUCCESS &&
                           NEAR(ot[0], 2.0f, 1e-6f) &&
                           t[0][0] == -1.0f,
                       "OBJECT track lands in obj slot");
        }
        /* Sampling: bad time / wrong type rejected. */
        {
            float t[3][3];
            float r[3][4];
            float s[3][3];
            le_asset skel = LE_ASSET_INVALID;

            memset(t, 0, sizeof(t));
            memset(r, 0, sizeof(r));
            memset(s, 0, sizeof(s));
            TEST_CHECK(le_anim_sample_clip(engine, &clipT, -1.0f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "sample negative time rejected");
            TEST_CHECK(le_anim_sample_clip(engine, &clipT,
                                           (float)NAN, t, r, s, 3,
                                           NULL, NULL,
                                           NULL) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "sample NaN time rejected");
            make_chain(engine, &skel);
            TEST_CHECK(le_anim_sample_clip(engine, &skel, 0.5f, t,
                                           r, s, 3, NULL, NULL,
                                           NULL) ==
                           LE_ERROR_WRONG_ASSET_TYPE,
                       "sample wrong asset type rejected");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Bind pose: chain composition into translation column. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset skel = LE_ASSET_INVALID;
        float g[3][16];
        le_asset clip = LE_ASSET_INVALID;

        make_engine(&engine);
        make_world(engine, &world);
        make_chain(engine, &skel);
        memset(g, 0, sizeof(g));
        TEST_CHECK(le_anim_bind_pose(engine, &skel, g, 3) ==
                       LE_SUCCESS,
                   "bind pose evaluates");
        TEST_CHECK(NEAR(g[0][12], 1.0f, 1e-6f) &&
                       NEAR(g[0][13], 0.0f, 1e-6f) &&
                       NEAR(g[0][14], 0.0f, 1e-6f),
                   "root global is bind translation");
        TEST_CHECK(NEAR(g[1][12], 1.0f, 1e-6f) &&
                       NEAR(g[1][13], 2.0f, 1e-6f) &&
                       NEAR(g[1][14], 0.0f, 1e-6f),
                   "mid global composes root*mid");
        TEST_CHECK(NEAR(g[2][12], 1.0f, 1e-6f) &&
                       NEAR(g[2][13], 2.0f, 1e-6f) &&
                       NEAR(g[2][14], 3.0f, 1e-6f),
                   "tip global composes root*mid*tip");
        TEST_CHECK(le_anim_bind_pose(engine, &skel, g, 2) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "bind undersized rejected");
        TEST_CHECK(le_anim_bind_pose(engine, &skel, NULL, 3) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "bind NULL out rejected");
        make_joint_ramp(engine, 1.0f, 1.0f, &clip);
        TEST_CHECK(le_anim_bind_pose(engine, &clip, g, 3) ==
                       LE_ERROR_WRONG_ASSET_TYPE,
                   "bind wrong asset type rejected");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Blend pose: weights 0/1/mid, clamp, NaN reject. */
    {
        float at[2][3] = { { 0.0f, 0.0f, 0.0f },
                           { 0.0f, 0.0f, 0.0f } };
        float ar[2][4] = { { 0.0f, 0.0f, 0.0f, 1.0f },
                           { 0.0f, 0.0f, 0.0f, 1.0f } };
        float as[2][3] = { { 1.0f, 1.0f, 1.0f },
                           { 1.0f, 1.0f, 1.0f } };
        float bt[2][3] = { { 10.0f, 0.0f, 0.0f },
                           { 0.0f, 20.0f, 0.0f } };
        float br[2][4] = { { 0.0f, 0.0f, 0.0f, 1.0f },
                           { 0.0f, 0.0f, 0.0f, 1.0f } };
        float bs[2][3] = { { 3.0f, 3.0f, 3.0f },
                           { 3.0f, 3.0f, 3.0f } };
        float ot[2][3];
        float orr[2][4];
        float os[2][3];

        memset(ot, 0, sizeof(ot));
        memset(orr, 0, sizeof(orr));
        memset(os, 0, sizeof(os));
        TEST_CHECK(le_anim_blend_pose(2, at, ar, as, bt, br, bs,
                                      0.0f, ot, orr,
                                      os) == LE_SUCCESS &&
                       ot[0][0] == 0.0f && os[1][1] == 1.0f,
                   "blend weight 0 is pose A");
        TEST_CHECK(le_anim_blend_pose(2, at, ar, as, bt, br, bs,
                                      1.0f, ot, orr,
                                      os) == LE_SUCCESS &&
                       NEAR(ot[0][0], 10.0f, 1e-6f) &&
                       NEAR(ot[1][1], 20.0f, 1e-6f),
                   "blend weight 1 is pose B");
        TEST_CHECK(le_anim_blend_pose(2, at, ar, as, bt, br, bs,
                                      0.5f, ot, orr,
                                      os) == LE_SUCCESS &&
                       NEAR(ot[0][0], 5.0f, 1e-6f) &&
                       NEAR(os[0][0], 2.0f, 1e-6f),
                   "blend weight 0.5 lerps");
        TEST_CHECK(le_anim_blend_pose(2, at, ar, as, bt, br, bs,
                                      -1.0f, ot, orr,
                                      os) == LE_SUCCESS &&
                       ot[0][0] == 0.0f,
                   "blend negative weight clamps to 0");
        TEST_CHECK(le_anim_blend_pose(2, at, ar, as, bt, br, bs,
                                      2.0f, ot, orr,
                                      os) == LE_SUCCESS &&
                       NEAR(ot[0][0], 10.0f, 1e-6f),
                   "blend overweight clamps to 1");
        TEST_CHECK(le_anim_blend_pose(2, at, ar, as, bt, br, bs,
                                      (float)NAN, ot, orr,
                                      os) == LE_ERROR_INVALID_ARGUMENT,
                   "blend NaN weight rejected");
    }

    /* Skin oracle. */
    {
        float ident[16];
        float tx5[16];
        float tx2[16];
        float mats[2][16];
        float p[3] = { 1.0f, 2.0f, 3.0f };
        float n[3] = { 0.0f, 1.0f, 0.0f };
        uint32_t j[4] = { 0, 0, 0, 0 };
        float w[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        float op[3] = { 0.0f, 0.0f, 0.0f };
        float on[3] = { 0.0f, 0.0f, 0.0f };

        ident_mat(ident);
        ident_mat(tx5);
        tx5[12] = 5.0f;
        ident_mat(tx2);
        tx2[12] = 2.0f;
        TEST_CHECK(le_anim_skin_vertex(p, n, j, w, &ident, 1, op,
                                       on) == LE_SUCCESS &&
                       NEAR(op[0], 1.0f, 1e-6f) &&
                       NEAR(op[1], 2.0f, 1e-6f) &&
                       NEAR(op[2], 3.0f, 1e-6f),
                   "skin identity holds point");
        TEST_CHECK(NEAR(on[0], 0.0f, 1e-6f) &&
                       NEAR(on[1], 1.0f, 1e-6f),
                   "skin identity holds normal");
        TEST_CHECK(le_anim_skin_vertex(p, n, j, w, &tx5, 1, op,
                                       on) == LE_SUCCESS &&
                       NEAR(op[0], 6.0f, 1e-6f) &&
                       NEAR(op[1], 2.0f, 1e-6f),
                   "skin translated joint offsets point");
        {
            uint32_t j2[4] = { 0, 1, 0, 0 };
            float w2[4] = { 0.5f, 0.5f, 0.0f, 0.0f };
            float p0[3] = { 0.0f, 0.0f, 0.0f };

            memcpy(mats[0], ident, sizeof(ident));
            memcpy(mats[1], tx2, sizeof(tx2));
            TEST_CHECK(le_anim_skin_vertex(p0, n, j2, w2, mats, 2,
                                           op,
                                           on) == LE_SUCCESS &&
                           NEAR(op[0], 1.0f, 1e-6f),
                       "skin 50/50 blend averages");
        }
        {
            float w0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

            TEST_CHECK(le_anim_skin_vertex(p, n, j, w0, &ident, 1,
                                           op,
                                           on) == LE_SUCCESS &&
                           op[0] == 1.0f && op[1] == 2.0f &&
                           op[2] == 3.0f,
                       "skin zero weights hold position");
        }
        {
            float wu[4] = { 2.0f, 0.0f, 0.0f, 0.0f };

            TEST_CHECK(le_anim_skin_vertex(p, n, j, wu, &tx5, 1,
                                           op,
                                           on) == LE_SUCCESS &&
                           NEAR(op[0], 6.0f, 1e-6f),
                       "skin unnormalized weights normalized");
        }
        {
            uint32_t jb[4] = { 99, 0, 0, 0 };

            TEST_CHECK(le_anim_skin_vertex(p, n, jb, w, &ident, 1,
                                           op,
                                           on) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "skin OOB joint rejected");
        }
        {
            uint32_t jb[4] = { 99, 0, 0, 0 };
            float w0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };

            TEST_CHECK(le_anim_skin_vertex(p, n, jb, w0, &ident, 1,
                                           op,
                                           on) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "skin OOB joint rejected even at 0 weight");
        }
        {
            float wn[4] = { (float)NAN, 0.0f, 0.0f, 0.0f };

            TEST_CHECK(le_anim_skin_vertex(p, n, j, wn, &ident, 1,
                                           op,
                                           on) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "skin NaN weight rejected");
            TEST_CHECK(le_anim_skin_vertex(NULL, n, j, w, &ident,
                                           1, op,
                                           on) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "skin NULL position rejected");
            TEST_CHECK(le_anim_skin_vertex(p, n, j, w, &ident, 0,
                                           op,
                                           on) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "skin zero joint_count rejected");
        }
    }

    /* Animator lifecycle. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset skel = LE_ASSET_INVALID;
        le_asset clipA = LE_ASSET_INVALID;
        le_asset clipB = LE_ASSET_INVALID;
        le_object o;
        le_animator_desc desc;
        le_animator_desc got;

        make_engine(&engine);
        make_world(engine, &world);
        make_chain(engine, &skel);
        make_joint_ramp(engine, 1.0f, 1.0f, &clipA);
        make_joint_ramp(engine, 2.0f, 4.0f, &clipB);
        le_object_create(world, &o);
        memset(&desc, 0, sizeof(desc));
        desc.skeleton = skel;
        desc.clip = clipA;
        desc.autoplay = 0;
        desc.loop_mode = LE_ANIM_ONCE;
        desc.speed = 1.0f;
        desc.start_time = 0.0f;
        TEST_CHECK(le_object_add_animator(world, &o, &desc) ==
                       LE_SUCCESS,
                   "add animator ok");
        TEST_CHECK(le_object_has_component(world, &o,
                                           LE_COMPONENT_ANIMATOR),
                   "animator presence bit");
        memset(&got, 0, sizeof(got));
        TEST_CHECK(le_object_get_animator(world, &o, &got) == 1 &&
                       NEAR(got.speed, 1.0f, 1e-7f) &&
                       got.loop_mode == LE_ANIM_ONCE,
                   "get animator round-trips");
        /* Busy assets refuse unload. */
        TEST_CHECK(le_asset_unload(engine, &clipA) ==
                       LE_ERROR_ASSET_IN_USE,
                   "busy clip unload IN_USE");
        /* Replace in place with new speed. */
        desc.speed = 3.0f;
        desc.loop_mode = LE_ANIM_LOOP;
        TEST_CHECK(le_object_add_animator(world, &o, &desc) ==
                       LE_SUCCESS,
                   "replace animator ok");
        memset(&got, 0, sizeof(got));
        TEST_CHECK(le_object_get_animator(world, &o, &got) == 1 &&
                       NEAR(got.speed, 3.0f, 1e-7f) &&
                       got.loop_mode == LE_ANIM_LOOP,
                   "replace adopts new fields");
        TEST_CHECK(le_object_remove_animator(world, &o) ==
                       LE_SUCCESS &&
                       le_object_get_animator(world, &o, &got) ==
                           0,
                   "remove animator clears");
        TEST_CHECK(le_object_remove_animator(world, &o) ==
                       LE_SUCCESS,
                   "remove missing animator ok");
        /* Autoplay starts playing immediately. */
        desc.speed = 1.0f;
        desc.loop_mode = LE_ANIM_LOOP;
        desc.autoplay = 1;
        le_object_add_animator(world, &o, &desc);
        TEST_CHECK(le_anim_is_playing(world, &o) == 1,
                   "autoplay starts playing");
        TEST_CHECK(NEAR(le_anim_get_duration(world, &o), 1.0f,
                        1e-6f),
                   "duration query matches clip");
        /* Same-clip play restart=0 continues; restart=1 resets. */
        le_anim_seek(world, &o, 0.3f);
        TEST_CHECK(le_anim_play(world, &o, &clipA, 0) ==
                       LE_SUCCESS &&
                       NEAR(le_anim_get_time(world, &o), 0.3f,
                            1e-6f),
                   "same-clip play continues time");
        TEST_CHECK(le_anim_play(world, &o, &clipA, 1) ==
                       LE_SUCCESS &&
                       le_anim_get_time(world, &o) == 0.0f,
                   "restart forces time 0");
        /* NULL clip continues current. */
        le_anim_seek(world, &o, 0.4f);
        TEST_CHECK(le_anim_play(world, &o, NULL, 0) ==
                       LE_SUCCESS &&
                       NEAR(le_anim_get_time(world, &o), 0.4f,
                            1e-6f),
                   "NULL clip continues");
        /* Different clip restarts + re-times duration. */
        TEST_CHECK(le_anim_play(world, &o, &clipB, 0) ==
                       LE_SUCCESS &&
                       le_anim_get_time(world, &o) == 0.0f &&
                       NEAR(le_anim_get_duration(world, &o), 2.0f,
                            1e-6f),
                   "different clip restarts");
        le_anim_play(world, &o, &clipA, 1);
        /* Pause holds; resume continues. */
        le_anim_seek(world, &o, 0.5f);
        TEST_CHECK(le_anim_pause(world, &o) == LE_SUCCESS &&
                       le_anim_is_playing(world, &o) == 0 &&
                       NEAR(le_anim_get_time(world, &o), 0.5f,
                            1e-6f),
                   "pause holds time");
        TEST_CHECK(le_anim_resume(world, &o) == LE_SUCCESS &&
                       le_anim_is_playing(world, &o) == 1,
                   "resume plays");
        /* Stop reset=0 holds; reset=1 returns to 0. */
        le_anim_seek(world, &o, 0.6f);
        TEST_CHECK(le_anim_stop(world, &o, 0) == LE_SUCCESS &&
                       le_anim_is_playing(world, &o) == 0 &&
                       NEAR(le_anim_get_time(world, &o), 0.6f,
                            1e-6f),
                   "stop holds pose+time");
        TEST_CHECK(le_anim_stop(world, &o, 1) == LE_SUCCESS &&
                       le_anim_get_time(world, &o) == 0.0f,
                   "stop reset returns to 0");
        /* Seek clamps; NaN rejected. */
        TEST_CHECK(le_anim_seek(world, &o, -5.0f) == LE_SUCCESS &&
                       le_anim_get_time(world, &o) == 0.0f,
                   "seek negative clamps to 0");
        TEST_CHECK(le_anim_seek(world, &o, 99.0f) == LE_SUCCESS &&
                       NEAR(le_anim_get_time(world, &o), 1.0f,
                            1e-6f),
                   "seek over duration clamps");
        le_anim_seek(world, &o, 0.25f);
        TEST_CHECK(le_anim_seek(world, &o, (float)NAN) ==
                       LE_ERROR_INVALID_ARGUMENT &&
                       NEAR(le_anim_get_time(world, &o), 0.25f,
                            1e-6f),
                   "seek NaN rejected, time kept");
        /* Speed: negative/NaN rejected, value kept. */
        TEST_CHECK(le_anim_set_speed(world, &o, -1.0f) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "negative speed rejected");
        memset(&got, 0, sizeof(got));
        le_object_get_animator(world, &o, &got);
        TEST_CHECK(NEAR(got.speed, 1.0f, 1e-7f),
                   "speed unchanged after reject");
        TEST_CHECK(le_anim_set_speed(world, &o, (float)NAN) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "NaN speed rejected");
        TEST_CHECK(le_anim_set_speed(world, &o, 2.0f) ==
                       LE_SUCCESS,
                   "speed set ok");
        /* Loop: bad values rejected. */
        TEST_CHECK(le_anim_set_loop(world, &o,
                                    (le_anim_loop_mode)99) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "bad loop rejected");
        TEST_CHECK(le_anim_set_loop(world, &o,
                                    LE_ANIM_LOOP_COUNT) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "LOOP_COUNT rejected");
        TEST_CHECK(le_anim_set_loop(world, &o, LE_ANIM_LOOP) ==
                       LE_SUCCESS,
                   "loop set ok");
        /* Missing animator: mutators INVALID, queries zero. */
        {
            le_object bare;

            le_object_create(world, &bare);
            TEST_CHECK(le_anim_play(world, &bare, &clipA, 0) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "play missing animator INVALID");
            TEST_CHECK(le_anim_pause(world, &bare) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "pause missing INVALID");
            TEST_CHECK(le_anim_resume(world, &bare) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "resume missing INVALID");
            TEST_CHECK(le_anim_stop(world, &bare, 0) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "stop missing INVALID");
            TEST_CHECK(le_anim_seek(world, &bare, 0.5f) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "seek missing INVALID");
            TEST_CHECK(le_anim_set_speed(world, &bare, 1.0f) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "speed missing INVALID");
            TEST_CHECK(le_anim_set_loop(world, &bare,
                                        LE_ANIM_LOOP) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "loop missing INVALID");
            TEST_CHECK(le_anim_crossfade(world, &bare, &clipA,
                                         0.25f) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "crossfade missing INVALID");
            TEST_CHECK(le_anim_is_playing(world, &bare) == 0 &&
                           le_anim_get_time(world, &bare) ==
                               0.0f &&
                           le_anim_get_duration(world, &bare) ==
                               0.0f,
                       "missing queries zero");
        }
        /* Unload works once animators are gone. */
        le_object_remove_animator(world, &o);
        TEST_CHECK(le_asset_unload(engine, &clipA) == LE_SUCCESS &&
                       le_asset_unload(engine, &clipB) ==
                           LE_SUCCESS &&
                       le_asset_unload(engine, &skel) ==
                           LE_SUCCESS,
                   "unload after remove succeeds");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Playback advance: LOOP wrap. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset clip = LE_ASSET_INVALID;
        le_object o;
        le_animator_desc desc;

        make_engine(&engine);
        make_world(engine, &world);
        make_obj_ramp(engine, 2.0f, 4.0f, &clip);
        le_object_create(world, &o);
        memset(&desc, 0, sizeof(desc));
        desc.skeleton = LE_ASSET_INVALID;
        desc.clip = clip;
        desc.autoplay = 1;
        desc.loop_mode = LE_ANIM_LOOP;
        desc.speed = 1.0f;
        desc.start_time = 0.0f;
        le_object_add_animator(world, &o, &desc);
        step_world(world, 0.6f);
        TEST_CHECK(NEAR(le_anim_get_time(world, &o), 0.6f, 1e-5f) &&
                       NEAR(pos_x(world, &o), 1.2f, 1e-4f),
                   "LOOP advances time+pose");
        step_world(world, 0.6f);
        TEST_CHECK(NEAR(le_anim_get_time(world, &o), 1.2f, 1e-5f),
                   "LOOP accumulates pre-wrap");
        step_world(world, 1.0f);
        TEST_CHECK(NEAR(le_anim_get_time(world, &o), 0.2f, 1e-4f) &&
                       NEAR(pos_x(world, &o), 0.4f, 1e-4f),
                   "LOOP wraps time mod duration");
        /* Speed scales advance. */
        le_anim_set_speed(world, &o, 2.0f);
        step_world(world, 0.25f);
        TEST_CHECK(NEAR(le_anim_get_time(world, &o), 0.7f, 1e-4f),
                   "speed scales advance");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Playback advance: ONCE stops at end holding pose. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset clip = LE_ASSET_INVALID;
        le_object o;
        le_animator_desc desc;

        make_engine(&engine);
        make_world(engine, &world);
        make_obj_ramp(engine, 1.0f, 10.0f, &clip);
        le_object_create(world, &o);
        memset(&desc, 0, sizeof(desc));
        desc.skeleton = LE_ASSET_INVALID;
        desc.clip = clip;
        desc.autoplay = 1;
        desc.loop_mode = LE_ANIM_ONCE;
        desc.speed = 1.0f;
        desc.start_time = 0.0f;
        le_object_add_animator(world, &o, &desc);
        step_world(world, 0.4f);
        TEST_CHECK(NEAR(pos_x(world, &o), 4.0f, 1e-4f),
                   "ONCE mid-pose follows track");
        step_world(world, 1.0f);
        TEST_CHECK(NEAR(le_anim_get_time(world, &o), 1.0f, 1e-5f) &&
                       le_anim_is_playing(world, &o) == 0,
                   "ONCE stops at end");
        TEST_CHECK(NEAR(pos_x(world, &o), 10.0f, 1e-4f),
                   "ONCE holds end pose");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Playback advance: PING_PONG reverses. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset clip = LE_ASSET_INVALID;
        le_object o;
        le_animator_desc desc;

        make_engine(&engine);
        make_world(engine, &world);
        make_obj_ramp(engine, 1.0f, 10.0f, &clip);
        le_object_create(world, &o);
        memset(&desc, 0, sizeof(desc));
        desc.skeleton = LE_ASSET_INVALID;
        desc.clip = clip;
        desc.autoplay = 1;
        desc.loop_mode = LE_ANIM_PING_PONG;
        desc.speed = 1.0f;
        desc.start_time = 0.0f;
        le_object_add_animator(world, &o, &desc);
        step_world(world, 0.6f);
        TEST_CHECK(NEAR(le_anim_get_time(world, &o), 0.6f, 1e-5f),
                   "PING_PONG forward pass");
        step_world(world, 0.6f);
        TEST_CHECK(NEAR(le_anim_get_time(world, &o), 0.8f, 1e-4f) &&
                       le_anim_is_playing(world, &o) == 1,
                   "PING_PONG reflects at end");
        step_world(world, 0.3f);
        TEST_CHECK(NEAR(le_anim_get_time(world, &o), 0.5f, 1e-4f) &&
                       NEAR(pos_x(world, &o), 5.0f, 1e-4f),
                   "PING_PONG reverses pose");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Disabled objects hold time, then resume. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset clip = LE_ASSET_INVALID;
        le_object o;
        le_animator_desc desc;
        float held;

        make_engine(&engine);
        make_world(engine, &world);
        make_obj_ramp(engine, 2.0f, 4.0f, &clip);
        le_object_create(world, &o);
        memset(&desc, 0, sizeof(desc));
        desc.skeleton = LE_ASSET_INVALID;
        desc.clip = clip;
        desc.autoplay = 1;
        desc.loop_mode = LE_ANIM_LOOP;
        desc.speed = 1.0f;
        desc.start_time = 0.0f;
        le_object_add_animator(world, &o, &desc);
        step_world(world, 0.5f);
        held = le_anim_get_time(world, &o);
        le_object_set_enabled(world, &o, 0);
        step_world(world, 1.0f);
        TEST_CHECK(le_anim_get_time(world, &o) == held &&
                       NEAR(pos_x(world, &o), 1.0f, 1e-4f),
                   "disabled holds time+pose");
        le_object_set_enabled(world, &o, 1);
        step_world(world, 0.5f);
        TEST_CHECK(NEAR(le_anim_get_time(world, &o), held + 0.5f,
                        1e-4f),
                   "re-enable resumes advance");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Crossfade: blend, interrupt continuity, immediate. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset skel = LE_ASSET_INVALID;
        le_asset clipA = LE_ASSET_INVALID;
        le_asset clipB = LE_ASSET_INVALID;
        le_asset clipC = LE_ASSET_INVALID;
        le_object o;
        le_animator_desc desc;
        le_anim_stats st;

        make_engine(&engine);
        make_world(engine, &world);
        make_chain(engine, &skel);
        make_obj_const(engine, 0.0f, &clipA);
        make_obj_const(engine, 10.0f, &clipB);
        make_obj_const(engine, 20.0f, &clipC);
        le_object_create(world, &o);
        memset(&desc, 0, sizeof(desc));
        desc.skeleton = skel;
        desc.clip = clipA;
        desc.autoplay = 1;
        desc.loop_mode = LE_ANIM_LOOP;
        desc.speed = 1.0f;
        desc.start_time = 0.0f;
        le_object_add_animator(world, &o, &desc);
        step_world(world, 0.1f);
        TEST_CHECK(le_anim_crossfade(world, &o, &clipB, 0.5f) ==
                       LE_SUCCESS,
                   "crossfade starts");
        memset(&st, 0, sizeof(st));
        le_anim_get_stats(world, &st);
        TEST_CHECK(st.active_crossfades == 1,
                   "fade active in stats");
        step_world(world, 0.25f);
        TEST_CHECK(le_anim_get_time(world, &o) > 0.3f &&
                       le_anim_get_time(world, &o) < 0.4f,
                   "fade advances destination time");
        /* The owner pose blends snapshot -> current: at w=0.5
         * the owner sits halfway between the A and B
         * constants (probe-verified: x=5.0). */
        TEST_CHECK(NEAR(pos_x(world, &o), 5.0f, 1e-3f),
                   "mid-fade pose blends A->B");
        /* Interrupt: captures current, no snap. */
        TEST_CHECK(le_anim_crossfade(world, &o, &clipC, 0.5f) ==
                       LE_SUCCESS,
                   "crossfade interrupt starts");
        step_world(world, 0.05f);
        TEST_CHECK(pos_x(world, &o) > 4.0f &&
                       pos_x(world, &o) < 8.0f,
                   "interrupt keeps pose continuity");
        /* Zero-duration = immediate. */
        TEST_CHECK(le_anim_crossfade(world, &o, &clipC, 0.0f) ==
                       LE_SUCCESS &&
                       le_anim_get_time(world, &o) == 0.0f,
                   "zero-duration fade immediate");
        step_world(world, 0.1f);
        TEST_CHECK(NEAR(pos_x(world, &o), 20.0f, 1e-4f),
                   "immediate fade lands on C");
        memset(&st, 0, sizeof(st));
        le_anim_get_stats(world, &st);
        TEST_CHECK(st.active_crossfades == 0,
                   "no active fades after settle");
        TEST_CHECK(le_anim_crossfade(world, &o, &clipC, -1.0f) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "negative fade rejected");
        TEST_CHECK(le_anim_crossfade(world, &o, NULL, 0.25f) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "NULL fade clip rejected");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Physics ownership: dynamic + OBJECT tracks rejected. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset objclip = LE_ASSET_INVALID;
        le_asset jointclip = LE_ASSET_INVALID;
        le_object o1;
        le_object o2;
        le_object o3;
        le_rigid_body_desc body;
        le_animator_desc desc;

        make_engine(&engine);
        make_world(engine, &world);
        make_obj_ramp(engine, 1.0f, 5.0f, &objclip);
        make_joint_ramp(engine, 1.0f, 5.0f, &jointclip);
        memset(&body, 0, sizeof(body));
        body.type = LE_BODY_DYNAMIC;
        body.mass = 1.0f;
        body.gravity_scale = 1.0f;
        le_object_create(world, &o1);
        TEST_CHECK(le_object_add_rigid_body(world, &o1, &body) ==
                       LE_SUCCESS,
                   "dynamic body added");
        memset(&desc, 0, sizeof(desc));
        desc.skeleton = LE_ASSET_INVALID;
        desc.clip = objclip;
        desc.autoplay = 0;
        desc.loop_mode = LE_ANIM_LOOP;
        desc.speed = 1.0f;
        desc.start_time = 0.0f;
        TEST_CHECK(le_object_add_animator(world, &o1, &desc) ==
                       LE_ERROR_INVALID_HIERARCHY,
                   "dynamic + OBJECT tracks rejected");
        desc.clip = jointclip;
        TEST_CHECK(le_object_add_animator(world, &o1, &desc) ==
                       LE_SUCCESS,
                   "dynamic + JOINT tracks allowed");
        memset(&body, 0, sizeof(body));
        body.type = LE_BODY_STATIC;
        le_object_create(world, &o2);
        le_object_add_rigid_body(world, &o2, &body);
        desc.clip = objclip;
        TEST_CHECK(le_object_add_animator(world, &o2, &desc) ==
                       LE_SUCCESS,
                   "static + OBJECT tracks allowed");
        memset(&body, 0, sizeof(body));
        body.type = LE_BODY_KINEMATIC;
        le_object_create(world, &o3);
        le_object_add_rigid_body(world, &o3, &body);
        TEST_CHECK(le_object_add_animator(world, &o3, &desc) ==
                       LE_SUCCESS,
                   "kinematic + OBJECT tracks allowed");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Scene round-trip: animator fields + playback survive. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_world *world2 = NULL;
        le_asset skel = LE_ASSET_INVALID;
        le_asset clip = LE_ASSET_INVALID;
        le_asset scene = LE_ASSET_INVALID;
        le_asset scene2 = LE_ASSET_INVALID;
        le_object o;
        le_animator_desc desc;
        char *text = NULL;
        size_t size = 0;
        uint32_t nobjs = 0;
        float captured = 0.0f;
        le_scene_instance inst;

        make_engine(&engine);
        make_world(engine, &world);
        make_chain(engine, &skel);
        make_obj_ramp(engine, 2.0f, 4.0f, &clip);
        le_object_create(world, &o);
        memset(&desc, 0, sizeof(desc));
        desc.skeleton = skel;
        desc.clip = clip;
        desc.autoplay = 1;
        desc.loop_mode = LE_ANIM_LOOP;
        desc.speed = 2.0f;
        desc.start_time = 0.0f;
        le_object_add_animator(world, &o, &desc);
        step_world(world, 0.3f);
        captured = le_anim_get_time(world, &o);
        le_scene_create(engine, 1, &scene);
        TEST_CHECK(le_scene_capture(world, &scene, NULL) ==
                       LE_SUCCESS,
                   "capture animated world");
        TEST_CHECK(le_scene_save_text(engine, &scene, &text,
                                      &size) == LE_SUCCESS &&
                       text != NULL,
                   "save animated scene");
        TEST_CHECK(strstr(text, "animator") != NULL,
                   "save emits animator line");
        le_scene_create(engine, 1, &scene2);
        TEST_CHECK(le_scene_load_text(engine, &scene2, text,
                                      size) == LE_SUCCESS,
                   "load animated scene");
        TEST_CHECK(le_scene_get_info(engine, &scene2, &nobjs,
                                     NULL, NULL) == 1 &&
                       nobjs == 1,
                   "loaded scene has one object");
        make_world(engine, &world2);
        memset(&inst, 0, sizeof(inst));
        TEST_CHECK(le_scene_instantiate(world2, &scene2, &inst) ==
                       LE_SUCCESS &&
                       inst.count == 1,
                   "animated scene instantiates");
        if (inst.count == 1) {
            le_animator_desc got;

            memset(&got, 0, sizeof(got));
            TEST_CHECK(le_object_get_animator(world2,
                                              &inst.objects[0],
                                              &got) == 1,
                       "instantiated animator present");
            TEST_CHECK(got.loop_mode == LE_ANIM_LOOP &&
                           NEAR(got.speed, 2.0f, 1e-6f) &&
                           got.autoplay == 1,
                       "animator fields survive");
            TEST_CHECK(NEAR(le_anim_get_time(world2,
                                             &inst.objects[0]),
                            captured, 1e-6f) &&
                           le_anim_is_playing(world2,
                                              &inst.objects[0]) ==
                               1,
                       "playback state survives");
        }
        le_scene_instance_free(&inst);
        /* Malformed animator lines rejected transactionally. */
        {
            const char *badloop =
                "LUMA_SCENE 1\n"
                "object "
                "0000000000000000000000000000000B\n"
                "animator nil nil 1 bogus 1 0\n"
                "end\n";
            le_asset bs = LE_ASSET_INVALID;

            le_scene_create(engine, 1, &bs);
            TEST_CHECK(le_scene_load_text(engine, &bs, badloop,
                                          strlen(badloop)) ==
                           LE_ERROR_PARSE,
                       "bad loop token rejected");
        }
        {
            const char *dup =
                "LUMA_SCENE 1\n"
                "object "
                "0000000000000000000000000000000C\n"
                "animator nil nil 1 loop 1 0\n"
                "animator nil nil 1 loop 1 0\n"
                "end\n";
            le_asset ds = LE_ASSET_INVALID;

            le_scene_create(engine, 1, &ds);
            TEST_CHECK(le_scene_load_text(engine, &ds, dup,
                                          strlen(dup)) ==
                           LE_ERROR_PARSE,
                       "duplicate animator rejected");
        }
        {
            const char *unk =
                "LUMA_SCENE 1\n"
                "object "
                "0000000000000000000000000000000D\n"
                "animation foo\n"
                "end\n";
            le_asset us = LE_ASSET_INVALID;

            le_scene_create(engine, 1, &us);
            TEST_CHECK(le_scene_load_text(engine, &us, unk,
                                          strlen(unk)) ==
                           LE_ERROR_PARSE,
                       "unknown animation line rejected");
        }
        le_scene_free_text(text);
        le_world_destroy(world);
        le_world_destroy(world2);
        le_engine_destroy(engine);
    }

    /* Stats census. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset skel = LE_ASSET_INVALID;
        le_asset clip1 = LE_ASSET_INVALID;
        le_asset clip2 = LE_ASSET_INVALID;
        le_object a;
        le_object b;
        le_animator_desc desc;
        le_anim_stats st;
        float times[2] = { 0.0f, 1.0f };
        float values[6] = { 0.0f, 0.0f, 0.0f,
                            1.0f, 0.0f, 0.0f };
        float values2[6] = { 0.0f, 0.0f, 0.0f,
                             0.0f, 1.0f, 0.0f };
        le_anim_track_desc trs[2];
        le_animation_clip_desc dd;

        make_engine(&engine);
        make_world(engine, &world);
        make_chain(engine, &skel);
        make_joint_ramp(engine, 1.0f, 1.0f, &clip1);
        memset(&trs[0], 0, sizeof(trs[0]));
        trs[0].target_kind = LE_ANIM_TARGET_JOINT;
        trs[0].target_index = 0;
        trs[0].channel = LE_ANIM_CHANNEL_TRANSLATION;
        trs[0].interpolation = LE_ANIM_INTERP_LINEAR;
        trs[0].times = times;
        trs[0].values = values;
        trs[0].key_count = 2;
        memset(&trs[1], 0, sizeof(trs[1]));
        trs[1].target_kind = LE_ANIM_TARGET_JOINT;
        trs[1].target_index = 1;
        trs[1].channel = LE_ANIM_CHANNEL_TRANSLATION;
        trs[1].interpolation = LE_ANIM_INTERP_LINEAR;
        trs[1].times = times;
        trs[1].values = values2;
        trs[1].key_count = 2;
        memset(&dd, 0, sizeof(dd));
        dd.duration = 1.0f;
        dd.tracks = trs;
        dd.track_count = 2;
        TEST_CHECK(le_asset_create_clip(engine, &dd, &clip2) ==
                       LE_SUCCESS,
                   "two-track clip creates");
        le_object_create(world, &a);
        le_object_create(world, &b);
        memset(&desc, 0, sizeof(desc));
        desc.skeleton = skel;
        desc.clip = clip1;
        desc.autoplay = 1;
        desc.loop_mode = LE_ANIM_LOOP;
        desc.speed = 1.0f;
        desc.start_time = 0.0f;
        le_object_add_animator(world, &a, &desc);
        desc.clip = clip2;
        desc.autoplay = 0;
        le_object_add_animator(world, &b, &desc);
        step_world(world, 0.1f);
        memset(&st, 0, sizeof(st));
        le_anim_get_stats(world, &st);
        TEST_CHECK(st.animator_count == 2, "stats two animators");
        TEST_CHECK(st.playing_count == 1, "stats one playing");
        TEST_CHECK(st.active_crossfades == 0,
                   "stats no fades yet");
        TEST_CHECK(st.sampled_tracks == 3,
                   "stats three sampled tracks");
        TEST_CHECK(st.evaluated_joints == 6,
                   "stats six evaluated joints");
        TEST_CHECK(st.frames_advanced > 0, "stats frames advance");
        TEST_CHECK(le_anim_crossfade(world, &b, &clip1, 0.5f) ==
                       LE_SUCCESS,
                   "stats fade starts");
        memset(&st, 0, sizeof(st));
        le_anim_get_stats(world, &st);
        TEST_CHECK(st.active_crossfades == 1 &&
                       st.playing_count == 2,
                   "stats fade active");
        step_world(world, 1.0f);
        memset(&st, 0, sizeof(st));
        le_anim_get_stats(world, &st);
        TEST_CHECK(st.active_crossfades == 0 &&
                       st.sampled_tracks == 2,
                   "stats fade settles, dest adopted");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Cursor/seek stress: random seeks stay exact + deterministic. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_asset clip = LE_ASSET_INVALID;
        le_object o;
        le_animator_desc desc;
        uint32_t rng = 0x12345678u;
        int i;
        int ok = 1;

        make_engine(&engine);
        make_world(engine, &world);
        make_obj_ramp(engine, 2.0f, 4.0f, &clip);
        le_object_create(world, &o);
        memset(&desc, 0, sizeof(desc));
        desc.skeleton = LE_ASSET_INVALID;
        desc.clip = clip;
        desc.autoplay = 1;
        desc.loop_mode = LE_ANIM_LOOP;
        desc.speed = 1.0f;
        desc.start_time = 0.0f;
        le_object_add_animator(world, &o, &desc);
        for (i = 0; i < 6; i++) {
            float t;

            rng = rng * 1664525u + 1013904223u;
            t = (float)(rng % 10000u) / 10000.0f * 1.9f;
            if (le_anim_seek(world, &o, t) != LE_SUCCESS ||
                le_anim_get_time(world, &o) != t) {
                ok = 0;
            }
            step_world(world, 0.05f);
            if (!NEAR(le_anim_get_time(world, &o), t + 0.05f,
                      1e-5f) ||
                !NEAR(pos_x(world, &o), (t + 0.05f) * 2.0f,
                      1e-3f)) {
                ok = 0;
            }
        }
        TEST_CHECK(ok, "random seeks advance exactly");
        {
            int exact = 0;

            for (i = 0; i < 8; i++) {
                float t;
                float t1[3][3];
                float r1[3][4];
                float s1[3][3];
                float t2[3][3];
                float r2[3][4];
                float s2[3][3];

                rng = rng * 1664525u + 1013904223u;
                t = (float)(rng % 10000u) / 10000.0f * 2.0f;
                memset(t1, 0, sizeof(t1));
                memset(r1, 0, sizeof(r1));
                memset(s1, 0, sizeof(s1));
                memset(t2, 0, sizeof(t2));
                memset(r2, 0, sizeof(r2));
                memset(s2, 0, sizeof(s2));
                if (le_anim_sample_clip(engine, &clip, t, t1, r1,
                                        s1, 3, NULL, NULL,
                                        NULL) == LE_SUCCESS &&
                    le_anim_sample_clip(engine, &clip, t, t2, r2,
                                        s2, 3, NULL, NULL,
                                        NULL) == LE_SUCCESS &&
                    memcmp(t1, t2, sizeof(t1)) == 0 &&
                    memcmp(r1, r2, sizeof(r1)) == 0 &&
                    memcmp(s1, s2, sizeof(s1)) == 0) {
                    exact++;
                }
            }
            TEST_CHECK(exact == 8, "sampling bit-exact repeats");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    printf("Phase 29 tests: %d passed, %d failed\n", g_passed,
           g_failed);
    return g_failed == 0 ? 0 : 1;
}
