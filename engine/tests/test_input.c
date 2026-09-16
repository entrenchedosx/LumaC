/*
 * Luma Engine Phase 27 headless tests: engine input (keys, mouse,
 * edges, focus, injection), actions/axes/contexts, gamepad
 * foundation, engine time (scale/pause/clamp/first-frame), fixed
 * determinism, frame/app/world lifecycle, two-world/two-engine
 * isolation.
 *
 * Deterministic throughout: injection + explicit-delta stepping
 * only (no sleeps, no devices, no windows, no renderer).
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

static le_input_binding key_bind(le_key key) {
    le_input_binding b;

    memset(&b, 0, sizeof(b));
    b.kind = LE_BINDING_KEY;
    b.key = key;
    b.scale = 1.0f;
    return b;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Engine Phase 27 headless tests...\n");

    /* NULL-safety across the new API. */
    {
        le_input_action bad_a = LE_INPUT_ACTION_INVALID;
        le_input_axis bad_x;
        le_input_stats ist;
        le_time_stats tst;

        memset(&bad_x, 0, sizeof(bad_x));
        TEST_CHECK(le_input_key_down(NULL, LE_KEY_W) == 0,
                   "key down NULL 0");
        TEST_CHECK(le_input_key_pressed(NULL, LE_KEY_W) == 0,
                   "key pressed NULL 0");
        TEST_CHECK(le_input_key_released(NULL, LE_KEY_W) == 0,
                   "key released NULL 0");
        TEST_CHECK(le_input_key_down(NULL, (le_key)9999) == 0,
                   "key down bad enum 0");
        TEST_CHECK(le_input_mouse_down(NULL, LE_MOUSE_LEFT) == 0,
                   "mouse down NULL 0");
        TEST_CHECK(le_input_action_down(NULL, &bad_a) == 0,
                   "action down NULL 0");
        TEST_CHECK(le_input_action_pressed(NULL, &bad_a) == 0,
                   "action pressed NULL 0");
        TEST_CHECK(le_input_axis_value(NULL, &bad_x) == 0.0f,
                   "axis NULL 0");
        TEST_CHECK(le_gamepad_is_connected(NULL, 0) == 0,
                   "gamepad NULL 0");
        TEST_CHECK(le_gamepad_axis_value(NULL, 0,
                                         LE_GAMEPAD_AXIS_LEFT_X) ==
                       0.0f,
                   "pad axis NULL 0");
        TEST_CHECK(le_time_delta(NULL) == 0.0,
                   "time delta NULL 0");
        TEST_CHECK(le_time_frame_index(NULL) == 0,
                   "frame index NULL 0");
        TEST_CHECK(le_time_set_scale(NULL, 1.0f) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "set scale NULL INVALID");
        TEST_CHECK(le_engine_begin_frame(NULL) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "begin NULL INVALID");
        TEST_CHECK(le_engine_step(NULL, NULL, 0.016f) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "step NULL INVALID");
        le_input_get_stats(NULL, &ist);
        TEST_CHECK(ist.keys_down == 0, "input stats NULL zero");
        le_time_get_stats(NULL, &tst);
        TEST_CHECK(tst.frame_index == 0, "time stats NULL zero");
        TEST_CHECK(le_world_is_paused(NULL) == 0,
                   "paused NULL 0");
    }

    /* Key edge contract: frame1 down+pressed, frame2 held,
     * frame3 released, frame4 clear. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;

        make_engine(&engine);
        make_world(engine, &world);
        le_input_inject_key(engine, LE_KEY_W, 1);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_key_down(engine, LE_KEY_W),
                   "f1 W down");
        TEST_CHECK(le_input_key_pressed(engine, LE_KEY_W),
                   "f1 W pressed");
        TEST_CHECK(!le_input_key_released(engine, LE_KEY_W),
                   "f1 W not released");
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_key_down(engine, LE_KEY_W),
                   "f2 W still down");
        TEST_CHECK(!le_input_key_pressed(engine, LE_KEY_W),
                   "f2 pressed cleared");
        TEST_CHECK(!le_input_key_released(engine, LE_KEY_W),
                   "f2 not released");
        le_input_inject_key(engine, LE_KEY_W, 0);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(!le_input_key_down(engine, LE_KEY_W),
                   "f3 W up");
        TEST_CHECK(!le_input_key_pressed(engine, LE_KEY_W),
                   "f3 not pressed");
        TEST_CHECK(le_input_key_released(engine, LE_KEY_W),
                   "f3 released");
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(!le_input_key_down(engine, LE_KEY_W) &&
                   !le_input_key_pressed(engine, LE_KEY_W) &&
                   !le_input_key_released(engine, LE_KEY_W),
                   "f4 all clear");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Press+release in the same frame: both edges, held clear. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;

        make_engine(&engine);
        make_world(engine, &world);
        le_input_inject_key(engine, LE_KEY_A, 1);
        le_input_inject_key(engine, LE_KEY_A, 0);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_key_pressed(engine, LE_KEY_A) &&
                   le_input_key_released(engine, LE_KEY_A) &&
                   !le_input_key_down(engine, LE_KEY_A),
                   "same-frame press+release edges, not held");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Invalid injection rejected. */
    {
        le_engine *engine = NULL;

        make_engine(&engine);
        TEST_CHECK(le_input_inject_key(engine, (le_key)9999, 1) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "inject bad key rejected");
        TEST_CHECK(le_input_inject_key(NULL, LE_KEY_W, 1) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "inject NULL rejected");
        TEST_CHECK(le_input_inject_text(engine, NULL) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "inject NULL text rejected");
        TEST_CHECK(le_input_inject_text(engine, "\xff") ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "inject bad utf8 rejected");
        le_engine_destroy(engine);
    }

    /* Mouse buttons share edge semantics; position/delta/wheel
     * accumulate within a frame and reset at the boundary. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        float x = 0.0f;
        float y = 0.0f;
        float dx = 0.0f;
        float dy = 0.0f;

        make_engine(&engine);
        make_world(engine, &world);
        le_input_inject_mouse_button(engine, LE_MOUSE_LEFT, 1);
        le_input_inject_mouse_move(engine, 100.0f, 50.0f, 4.0f,
                                   -2.0f);
        le_input_inject_mouse_move(engine, 104.0f, 48.0f, 4.0f,
                                   -2.0f);
        le_input_inject_scroll(engine, 0.0f, 3.0f);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_mouse_down(engine, LE_MOUSE_LEFT) &&
                   le_input_mouse_pressed(engine, LE_MOUSE_LEFT),
                   "mouse pressed edge");
        le_input_mouse_position(engine, &x, &y);
        TEST_CHECK(x == 104.0f && y == 48.0f,
                   "mouse position latest");
        le_input_mouse_delta(engine, &dx, &dy);
        TEST_CHECK(dx == 8.0f && dy == -4.0f,
                   "mouse delta accumulated");
        le_input_scroll_delta(engine, &dx, &dy);
        TEST_CHECK(dx == 0.0f && dy == 3.0f,
                   "wheel accumulated");
        le_engine_step(engine, world, 0.016f);
        le_input_mouse_delta(engine, &dx, &dy);
        TEST_CHECK(dx == 0.0f && dy == 0.0f,
                   "delta resets next frame");
        le_input_scroll_delta(engine, &dx, &dy);
        TEST_CHECK(dx == 0.0f && dy == 0.0f,
                   "wheel resets next frame");
        TEST_CHECK(!le_input_mouse_pressed(engine,
                                           LE_MOUSE_LEFT) &&
                   le_input_mouse_down(engine, LE_MOUSE_LEFT),
                   "mouse held, edge cleared");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Text input queues scalars; short buffers keep data. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        char buf[8];
        uint32_t len = 0;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(le_input_inject_text(engine, "h") ==
                       LE_SUCCESS,
                   "inject text ok");
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_pending_text(engine) == 1,
                   "one scalar pending");
        TEST_CHECK(le_input_read_text(engine, buf, 1, &len) == 0,
                   "short buffer keeps scalar");
        TEST_CHECK(le_input_pending_text(engine) == 1,
                   "scalar retained");
        TEST_CHECK(le_input_read_text(engine, buf, sizeof(buf),
                                      &len) == 1 &&
                   buf[0] == 'h' && len == 1,
                   "scalar reads back");
        TEST_CHECK(le_input_pending_text(engine) == 0,
                   "queue drained");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Focus loss clears held keys (no stuck movement). */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;

        make_engine(&engine);
        make_world(engine, &world);
        le_input_inject_key(engine, LE_KEY_W, 1);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_key_down(engine, LE_KEY_W),
                   "held before focus loss");
        le_input_inject_focus(engine, 0);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(!le_input_key_down(engine, LE_KEY_W),
                   "focus loss clears held");
        TEST_CHECK(!le_input_has_focus(engine),
                   "focus flag cleared");
        le_input_inject_focus(engine, 1);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_has_focus(engine),
                   "focus regained");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Actions: multi-binding aggregate (Space + pad A). Holding
     * one then pressing the other does NOT re-press. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_input_action jump = LE_INPUT_ACTION_INVALID;
        le_input_binding b;

        make_engine(&engine);
        make_world(engine, &world);
        TEST_CHECK(le_input_create_action(engine, "jump", &jump) ==
                       LE_SUCCESS,
                   "create jump");
        memset(&b, 0, sizeof(b));
        b.kind = LE_BINDING_KEY;
        b.key = LE_KEY_SPACE;
        b.scale = 1.0f;
        TEST_CHECK(le_input_add_action_binding(engine, &jump,
                                               &b) == LE_SUCCESS,
                   "bind space");
        memset(&b, 0, sizeof(b));
        b.kind = LE_BINDING_GAMEPAD_BUTTON;
        b.gamepad_button = LE_GAMEPAD_A;
        b.gamepad_slot = 0;
        TEST_CHECK(le_input_add_action_binding(engine, &jump,
                                               &b) == LE_SUCCESS,
                   "bind pad A");
        TEST_CHECK(le_input_find_action(engine, "jump", NULL),
                   "find jump");
        le_input_inject_key(engine, LE_KEY_SPACE, 1);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_action_down(engine, &jump) &&
                   le_input_action_pressed(engine, &jump),
                   "jump pressed via space");
        /* Second binding presses while held: no new edge. */
        le_input_inject_gamepad_button(engine, 0, LE_GAMEPAD_A,
                                       1);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_action_down(engine, &jump) &&
                   !le_input_action_pressed(engine, &jump),
                   "no re-press on second binding");
        le_input_inject_key(engine, LE_KEY_SPACE, 0);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_action_down(engine, &jump),
                   "pad still holds aggregate");
        le_input_inject_gamepad_button(engine, 0, LE_GAMEPAD_A,
                                       0);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(!le_input_action_down(engine, &jump) &&
                   le_input_action_released(engine, &jump),
                   "aggregate release edge");
        /* Rebind APIs. */
        TEST_CHECK(le_input_clear_action_bindings(engine,
                                                  &jump) ==
                       LE_SUCCESS,
                   "clear bindings");
        TEST_CHECK(!le_input_action_down(engine, &jump),
                   "cleared action idle");
        {
            uint32_t n = 99;

            memset(&b, 0, sizeof(b));
            b.kind = LE_BINDING_KEY;
            b.key = LE_KEY_J;
            b.scale = 1.0f;
            le_input_add_action_binding(engine, &jump, &b);
            le_input_get_action_bindings(engine, &jump, NULL, 0,
                                         &n);
            TEST_CHECK(n == 1, "one binding queried");
            TEST_CHECK(le_input_remove_action_binding(
                           engine, &jump, &b) == LE_SUCCESS,
                       "remove binding");
            le_input_get_action_bindings(engine, &jump, NULL, 0,
                                         &n);
            TEST_CHECK(n == 0, "zero bindings queried");
        }
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Action name validation + stale handles. */
    {
        le_engine *engine = NULL;
        le_input_action a = LE_INPUT_ACTION_INVALID;
        le_input_action stale = { 0, 0 };

        make_engine(&engine);
        TEST_CHECK(le_input_create_action(engine, "", &a) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "empty action rejected");
        TEST_CHECK(le_input_create_action(engine, NULL, &a) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "NULL action rejected");
        TEST_CHECK(!le_input_action_down(engine, &stale),
                   "stale action idle");
        TEST_CHECK(le_input_create_action(engine, "ok_name-1",
                                          &a) == LE_SUCCESS,
                   "valid name ok");
        TEST_CHECK(le_input_find_action(engine, "ok_name-1",
                                        NULL),
                   "find valid");
        le_engine_destroy(engine);
    }

    /* Digital axis: A(-1)/D(+1); both held cancel to 0. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_input_axis mx = LE_INPUT_AXIS_INVALID;
        le_axis_desc dd;
        le_input_binding b;

        make_engine(&engine);
        make_world(engine, &world);
        memset(&dd, 0, sizeof(dd));
        dd.name = "move_x";
        dd.deadzone = 0.0f;
        dd.scale = 1.0f;
        TEST_CHECK(le_input_create_axis(engine, &dd, &mx) ==
                       LE_SUCCESS,
                   "create move_x");
        memset(&b, 0, sizeof(b));
        b.kind = LE_BINDING_KEY;
        b.key = LE_KEY_A;
        b.scale = -1.0f;
        le_input_add_axis_binding(engine, &mx, &b);
        memset(&b, 0, sizeof(b));
        b.kind = LE_BINDING_KEY;
        b.key = LE_KEY_D;
        b.scale = 1.0f;
        le_input_add_axis_binding(engine, &mx, &b);
        le_input_inject_key(engine, LE_KEY_D, 1);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_axis_value(engine, &mx) == 1.0f,
                   "D gives +1");
        le_input_inject_key(engine, LE_KEY_A, 1);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_axis_value(engine, &mx) == 0.0f,
                   "A+D cancel to 0");
        le_input_inject_key(engine, LE_KEY_D, 0);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_axis_value(engine, &mx) == -1.0f,
                   "A alone gives -1");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Analog axis: gamepad stick with deadzone/scale/invert. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_input_axis lx = LE_INPUT_AXIS_INVALID;
        le_axis_desc dd;
        le_input_binding b;

        make_engine(&engine);
        make_world(engine, &world);
        memset(&dd, 0, sizeof(dd));
        dd.name = "look_x";
        dd.deadzone = 0.2f;
        dd.scale = 2.0f;
        TEST_CHECK(le_input_create_axis(engine, &dd, &lx) ==
                       LE_SUCCESS,
                   "create look_x");
        memset(&b, 0, sizeof(b));
        b.kind = LE_BINDING_GAMEPAD_AXIS;
        b.gamepad_axis = LE_GAMEPAD_AXIS_LEFT_X;
        b.gamepad_slot = 1;
        b.scale = 1.0f;
        le_input_add_axis_binding(engine, &lx, &b);
        le_input_inject_gamepad_axis(engine, 1,
                                     LE_GAMEPAD_AXIS_LEFT_X,
                                     0.1f);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_axis_value(engine, &lx) == 0.0f,
                   "deadzone swallows 0.1");
        le_input_inject_gamepad_axis(engine, 1,
                                     LE_GAMEPAD_AXIS_LEFT_X,
                                     0.5f);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_axis_value(engine, &lx) == 1.0f,
                   "0.5 x scale 2 clamps to 1");
        TEST_CHECK(le_gamepad_is_connected(engine, 1),
                   "pad slot connected via injection");
        TEST_CHECK(!le_gamepad_is_connected(engine, 2),
                   "pad slot 2 disconnected");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Contexts: narrowed actions hide when inactive; priority +
     * consume mask routes Console over Gameplay. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_input_action fire = LE_INPUT_ACTION_INVALID;
        le_input_context game = LE_INPUT_CONTEXT_INVALID;
        le_input_context console = LE_INPUT_CONTEXT_INVALID;
        le_input_binding b;

        make_engine(&engine);
        make_world(engine, &world);
        le_input_create_action(engine, "fire", &fire);
        memset(&b, 0, sizeof(b));
        b.kind = LE_BINDING_KEY;
        b.key = LE_KEY_F;
        b.scale = 1.0f;
        le_input_add_action_binding(engine, &fire, &b);
        le_input_create_context(engine, "gameplay", 0, &game);
        le_input_create_context(engine, "console", 10,
                                &console);
        le_input_context_bind_action(engine, &game, &fire);
        /* Narrowed but inactive: hidden. */
        le_input_inject_key(engine, LE_KEY_F, 1);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(!le_input_action_down(engine, &fire),
                   "narrowed action hidden while inactive");
        le_input_activate_context(engine, &game);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_action_down(engine, &fire),
                   "active context reveals action");
        TEST_CHECK(le_input_context_active(engine, &game),
                   "game active");
        /* Console consumes keyboard: gameplay raw goes quiet. */
        le_input_context_set_consume(engine, &console,
                                     LE_CONSUME_KEYBOARD);
        le_input_activate_context(engine, &console);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(!le_input_key_down(engine, LE_KEY_F),
                   "consumed key hidden from raw");
        le_input_deactivate_context(engine, &console);
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_input_key_down(engine, LE_KEY_F),
                   "unconsumed after console off");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Time: first frame delta 0; scale math; clamp; pause. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_time_stats ts;

        make_engine(&engine);
        make_world(engine, &world);
        le_engine_step(engine, world, 0.02f);
        TEST_CHECK(le_time_frame_index(engine) == 1,
                   "frame 1 indexed");
        le_time_get_stats(engine, &ts);
        TEST_CHECK(ts.scaled_delta > 0.019 &&
                   ts.scaled_delta < 0.021,
                   "raw 0.02 simulated");
        TEST_CHECK(le_time_set_scale(engine, 0.5f) == LE_SUCCESS,
                   "scale 0.5 ok");
        le_engine_step(engine, world, 0.02f);
        TEST_CHECK(le_time_delta(engine) > 0.009 &&
                   le_time_delta(engine) < 0.011,
                   "scaled 0.01");
        TEST_CHECK(le_time_unscaled_delta(engine) > 0.019,
                   "unscaled stays 0.02");
        TEST_CHECK(le_time_set_scale(engine, -1.0f) ==
                       LE_ERROR_INVALID_ARGUMENT,
                   "negative scale rejected");
        TEST_CHECK(le_time_get_scale(engine) == 0.5f,
                   "scale unchanged after reject");
        TEST_CHECK(le_time_set_scale(engine, 0.0f) == LE_SUCCESS,
                   "scale 0 ok");
        {
            double nan_scale = 0.0;

            nan_scale = nan_scale / nan_scale; /* quiet NaN */
            TEST_CHECK(le_time_set_scale(engine,
                                         (float)nan_scale) ==
                           LE_ERROR_INVALID_ARGUMENT,
                       "NaN scale rejected");
        }
        /* Pause: update dt 0, fixed stops, frame+unscaled run. */
        le_time_set_scale(engine, 0.0f);
        {
            uint64_t f0 = le_time_frame_index(engine);
            double u0 = le_time_unscaled_elapsed(engine);

            le_engine_step(engine, world, 0.05f);
            TEST_CHECK(le_time_delta(engine) == 0.0,
                       "paused dt 0");
            TEST_CHECK(le_time_frame_index(engine) == f0 + 1,
                       "frame advances while paused");
            TEST_CHECK(le_time_unscaled_elapsed(engine) > u0,
                       "unscaled advances while paused");
        }
        /* Clamp: huge delta caps at max_delta. */
        le_time_set_scale(engine, 1.0f);
        le_time_set_max_delta(engine, 0.25f);
        le_engine_step(engine, world, 5.0f);
        TEST_CHECK(le_time_unscaled_delta(engine) <= 0.25001,
                   "huge delta clamped");
        le_time_get_stats(engine, &ts);
        TEST_CHECK(ts.raw_delta > 4.9, "raw still visible");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Fixed determinism: 100x0.01 vs 50x0.02 agree on steps. */
    {
        le_engine *e1 = NULL;
        le_engine *e2 = NULL;
        le_world *w1 = NULL;
        le_world *w2 = NULL;
        int i;

        make_engine(&e1);
        make_world(e1, &w1);
        make_engine(&e2);
        make_world(e2, &w2);
        le_time_set_fixed_delta(e1, 1.0f / 60.0f);
        le_time_set_fixed_delta(e2, 1.0f / 60.0f);
        for (i = 0; i < 100; i++) {
            le_engine_step(e1, w1, 0.01f);
        }
        for (i = 0; i < 50; i++) {
            le_engine_step(e2, w2, 0.02f);
        }
        {
            double a = le_time_elapsed(e1);
            double b = le_time_elapsed(e2);

            TEST_CHECK(a > 0.99 && a < 1.01 && b > 0.99 &&
                       b < 1.01,
                       "equal time both sequences");
        }
        le_world_destroy(w1);
        le_world_destroy(w2);
        le_engine_destroy(e1);
        le_engine_destroy(e2);
    }

    /* Spiral guard: huge delta caps fixed work, no freeze. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;

        make_engine(&engine);
        make_world(engine, &world);
        le_engine_step(engine, world, 10.0f);
        TEST_CHECK(le_time_frame_index(engine) >= 1,
                   "survived huge delta");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Single-step while paused. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;

        make_engine(&engine);
        make_world(engine, &world);
        le_time_set_scale(engine, 0.0f);
        TEST_CHECK(le_time_request_single_step(engine) ==
                       LE_SUCCESS,
                   "single step queued");
        le_engine_step(engine, world, 0.016f);
        TEST_CHECK(le_time_frame_index(engine) >= 1,
                   "stepped while paused");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Frame-rate independence: pos += speed*dt over equal time. */
    {
        le_engine *e1 = NULL;
        le_engine *e2 = NULL;
        le_world *w1 = NULL;
        le_world *w2 = NULL;
        le_object o1;
        le_object o2;
        float p1[3] = { 0.0f, 0.0f, 0.0f };
        float p2[3] = { 0.0f, 0.0f, 0.0f };
        int i;

        make_engine(&e1);
        make_world(e1, &w1);
        make_engine(&e2);
        make_world(e2, &w2);
        le_object_create(w1, &o1);
        le_object_create(w2, &o2);
        for (i = 0; i < 100; i++) {
            le_engine_step(e1, w1, 0.01f);
            p1[0] += 2.0f * (float)le_time_delta(e1);
        }
        for (i = 0; i < 50; i++) {
            le_engine_step(e2, w2, 0.02f);
            p2[0] += 2.0f * (float)le_time_delta(e2);
        }
        TEST_CHECK(p1[0] > 1.99f && p1[0] < 2.01f &&
                   p2[0] > 1.99f && p2[0] < 2.01f,
                   "frame-rate independent integration");
        le_world_destroy(w1);
        le_world_destroy(w2);
        le_engine_destroy(e1);
        le_engine_destroy(e2);
    }

    /* App lifecycle: quit request/cancel, no exit() anywhere. */
    {
        le_engine *engine = NULL;

        make_engine(&engine);
        TEST_CHECK(le_engine_app_state(engine) == LE_APP_RUNNING,
                   "starts running");
        le_engine_request_quit(engine);
        TEST_CHECK(le_engine_app_state(engine) ==
                       LE_APP_QUIT_REQUESTED,
                   "quit requested");
        le_engine_cancel_quit(engine);
        TEST_CHECK(le_engine_app_state(engine) == LE_APP_RUNNING,
                   "quit cancelled");
        le_engine_destroy(engine);
    }

    /* World pause: paused world skips sim but renders valid. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_object o;
        float p0[3];
        float p1[3];

        make_engine(&engine);
        make_world(engine, &world);
        le_object_create(world, &o);
        p0[0] = 5.0f;
        p0[1] = 0.0f;
        p0[2] = 0.0f;
        le_object_set_position(world, &o, p0);
        le_world_set_paused(world, 1);
        TEST_CHECK(le_world_is_paused(world), "world paused");
        le_engine_step(engine, world, 1.0f);
        le_object_get_position(world, &o, p1);
        TEST_CHECK(p1[0] == 5.0f, "paused world holds still");
        le_world_set_paused(world, 0);
        TEST_CHECK(!le_world_is_paused(world), "world resumed");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Two worlds share the input snapshot, keep own state. */
    {
        le_engine *engine = NULL;
        le_world *wa = NULL;
        le_world *wb = NULL;
        le_object oa;
        le_object ob;
        float pa[3];
        float pb[3];

        make_engine(&engine);
        make_world(engine, &wa);
        make_world(engine, &wb);
        le_object_create(wa, &oa);
        le_object_create(wb, &ob);
        le_input_inject_key(engine, LE_KEY_W, 1);
        le_engine_step(engine, wa, 0.016f);
        le_engine_step(engine, wb, 0.016f);
        TEST_CHECK(le_input_key_down(engine, LE_KEY_W),
                   "shared snapshot down");
        pa[0] = 1.0f;
        pa[1] = 0.0f;
        pa[2] = 0.0f;
        pb[0] = 9.0f;
        pb[1] = 0.0f;
        pb[2] = 0.0f;
        le_object_set_position(wa, &oa, pa);
        le_object_set_position(wb, &ob, pb);
        le_world_destroy(wa);
        le_object_get_position(wb, &ob, pb);
        TEST_CHECK(pb[0] == 9.0f, "survivor keeps state");
        le_world_destroy(wb);
        le_engine_destroy(engine);
    }

    /* Two engines stay isolated under different input/time. */
    {
        le_engine *ea = NULL;
        le_engine *eb = NULL;
        le_world *wa = NULL;
        le_world *wb = NULL;

        make_engine(&ea);
        make_world(ea, &wa);
        make_engine(&eb);
        make_world(eb, &wb);
        le_input_inject_key(ea, LE_KEY_W, 1);
        le_input_inject_key(eb, LE_KEY_S, 1);
        le_time_set_scale(eb, 0.5f);
        le_engine_step(ea, wa, 0.02f);
        le_engine_step(eb, wb, 0.02f);
        TEST_CHECK(le_input_key_down(ea, LE_KEY_W) &&
                   !le_input_key_down(ea, LE_KEY_S),
                   "engine A sees W only");
        TEST_CHECK(le_input_key_down(eb, LE_KEY_S) &&
                   !le_input_key_down(eb, LE_KEY_W),
                   "engine B sees S only");
        TEST_CHECK(le_time_delta(ea) > 0.019 &&
                   le_time_delta(eb) > 0.009 &&
                   le_time_delta(eb) < 0.011,
                   "isolated time scales");
        le_world_destroy(wa);
        le_world_destroy(wb);
        le_engine_destroy(ea);
        le_engine_destroy(eb);
    }

    /* le_engine_frame convenience over explicit lifecycle. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_world *list[1];

        make_engine(&engine);
        make_world(engine, &world);
        list[0] = world;
        le_input_inject_key(engine, LE_KEY_W, 1);
        TEST_CHECK(le_engine_frame(engine, list, 1) == LE_SUCCESS,
                   "frame convenience ok");
        TEST_CHECK(le_input_key_down(engine, LE_KEY_W),
                   "frame snapshot visible");
        TEST_CHECK(le_time_frame_index(engine) == 1,
                   "frame indexed");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    /* Stats + cursor mode surface. */
    {
        le_engine *engine = NULL;
        le_world *world = NULL;
        le_input_stats st;

        make_engine(&engine);
        make_world(engine, &world);
        le_input_inject_key(engine, LE_KEY_W, 1);
        le_engine_step(engine, world, 0.016f);
        le_input_get_stats(engine, &st);
        TEST_CHECK(st.keys_down == 1 && st.events_ingested >= 1,
                   "input stats count");
        TEST_CHECK(le_input_set_cursor_mode(engine,
                                            LE_CURSOR_CAPTURED) ==
                       LE_CURSOR_NORMAL,
                   "cursor prev normal");
        TEST_CHECK(le_input_get_cursor_mode(engine) ==
                       LE_CURSOR_CAPTURED,
                   "cursor captured recorded");
        le_world_destroy(world);
        le_engine_destroy(engine);
    }

    printf("Luma Engine Phase 27 headless tests: %d passed, %d "
           "failed\n",
           g_passed, g_failed);
    if (g_failed == 0) {
        printf("ALL PHASE 27 HEADLESS TESTS PASSED\n");
    } else {
        printf("TESTS FAILED\n");
    }
    return (g_failed == 0) ? 0 : 1;
}
