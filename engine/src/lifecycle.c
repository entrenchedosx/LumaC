/*
 * Frame + application + world lifecycle (Phase 27).
 *
 * Explicit contract (host owns the loop):
 *   begin_frame: poll platform -> ingest -> finalize edges -> time
 *   update:      per-world sim (pause-aware) + scripts via dt
 *   [render trio: existing le_world_render_* API, unchanged]
 *   end_frame:   edge cleanup (edges + deltas die at boundary)
 * frame(): begin + update(each world) + end.
 * step():  deterministic explicit-delta variant for tests.
 *
 * Fixed-step ownership: the ENGINE owns the schedule (interval
 * lives in le_time_state); per-world accumulators (Phase 26
 * fields) keep the backlog. le_world_update(world, dt) keeps its
 * contract as a thin wrapper: explicit dt -> time advance (test
 * clock semantics) + world sim with the engine schedule.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "input/input_internal.h"
#include "time/time_internal.h"

le_result le_engine_attach_window(le_engine *engine,
                                  lc_window *window) {
    uint32_t i;

    if (engine == NULL || window == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < engine->window_count; i++) {
        if (engine->windows[i] == window) {
            return LE_SUCCESS;
        }
    }
    if (engine->window_count >= engine->window_cap) {
        uint32_t grown =
            (engine->window_cap == 0) ? 4u : engine->window_cap * 2u;
        lc_window **fresh = (lc_window **)realloc(
            engine->windows, (size_t)grown * sizeof(*fresh));

        if (fresh == NULL) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
        engine->windows = fresh;
        engine->window_cap = grown;
    }
    engine->windows[engine->window_count++] = window;
    if (engine->focus_window == NULL) {
        engine->focus_window = window;
    }
    return LE_SUCCESS;
}

le_result le_engine_detach_window(le_engine *engine,
                                  lc_window *window) {
    uint32_t i;

    if (engine == NULL || window == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < engine->window_count; i++) {
        if (engine->windows[i] == window) {
            engine->windows[i] =
                engine->windows[engine->window_count - 1u];
            engine->window_count--;
            if (engine->focus_window == window) {
                engine->focus_window =
                    (engine->window_count > 0)
                        ? engine->windows[0]
                        : NULL;
            }
            return LE_SUCCESS;
        }
    }
    return LE_ERROR_INVALID_ARGUMENT;
}

/* Run one world's simulation for the engine frame's scaled dt:
 * matrices + script dispatch (which runs fixed steps internally
 * per the engine schedule mirrored into world fields). Paused
 * worlds skip everything (but still refresh matrices so render
 * stays valid). */
static le_result le_update_one_world(le_engine *engine,
                                     le_world *world, float dt) {
    struct le_time_state *t;
    float engine_fixed;

    if (engine == NULL || world == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->engine != engine) {
        return LE_ERROR_WRONG_WORLD;
    }
    if (world->paused) {
        le_refresh_world_matrices(world);
        return LE_SUCCESS;
    }
    t = engine->time;
    engine_fixed = (t != NULL) ? t->fixed_delta : 0.0f;
    /* Mirror the engine schedule into the world fields the
     * Phase 26 dispatcher reads (single source of truth stays
     * the engine; worlds without an engine tick use their own
     * values via le_world_update directly). */
    world->script_fixed_dt = engine_fixed;
    if (world->script_max_steps == 0) {
        world->script_max_steps = 4;
    }
    /* Single-step while paused (time_scale == 0): run exactly
     * one fixed interval through the normal path. */
    if (t != NULL && t->single_step_queued &&
        t->time_scale == 0.0f && engine_fixed > 0.0f) {
        t->single_step_queued = 0;
        world->script_accum += (double)engine_fixed;
    }
    /* Engine-contract dispatch (scripts run even at dt==0 while
     * paused; direct le_world_update callers keep the legacy
     * matrices-only dt<=0 path). */
    le_world_simulate_engine(world, dt);
    return LE_SUCCESS;
}

le_result le_engine_begin_frame(le_engine *engine) {
    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (engine->input == NULL || engine->time == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    le_input_poll_platform(engine);
    le_input_advance_frame(engine);
    /* Focus-window tracking: most recent FOCUS_GAINED wins (the
     * pending list already applied focus state; mirror it). */
    le_time_advance_clock(engine->time);
    return LE_SUCCESS;
}

le_result le_engine_update(le_engine *engine, le_world *world) {
    float dt;

    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (engine->time == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    if (world == NULL) {
        return LE_SUCCESS; /* input+time already advanced */
    }
    dt = (float)engine->time->scaled_delta;
    return le_update_one_world(engine, world, dt);
}

le_result le_engine_end_frame(le_engine *engine) {
    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (engine->input == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    le_input_end_frame(engine);
    return LE_SUCCESS;
}

le_result le_engine_frame(le_engine *engine, le_world **worlds,
                          uint32_t world_count) {
    le_result rc;
    uint32_t i;

    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (worlds == NULL && world_count != 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    rc = le_engine_begin_frame(engine);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    for (i = 0; i < world_count; i++) {
        if (worlds[i] == NULL) {
            continue;
        }
        rc = le_engine_update(engine, worlds[i]);
        if (rc != LE_SUCCESS) {
            le_engine_end_frame(engine);
            return rc;
        }
    }
    return le_engine_end_frame(engine);
}

le_result le_engine_step(le_engine *engine, le_world *world,
                         float dt) {
    double raw;

    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (engine->input == NULL || engine->time == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    if (dt != dt) {
        dt = 0.0f;
    }
    if (dt < 0.0f) {
        dt = 0.0f;
    }
    raw = (double)dt;
    /* Deterministic path: fold pending (injection) WITHOUT
     * touching the OS pump, then advance the test clock. Edges
     * stay readable after step returns (the NEXT advance or
     * end_frame clears them) so tests/scripts observe the same
     * snapshot the update just ran. */
    le_input_advance_frame(engine);
    le_time_advance_delta(engine->time, raw);
    if (world != NULL) {
        return le_update_one_world(
            engine, world, (float)engine->time->scaled_delta);
    }
    return LE_SUCCESS;
}

le_result le_engine_request_quit(le_engine *engine) {
    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    engine->quit_requested = 1;
    return LE_SUCCESS;
}

le_result le_engine_cancel_quit(le_engine *engine) {
    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    engine->quit_requested = 0;
    return LE_SUCCESS;
}

le_app_state le_engine_app_state(le_engine *engine) {
    if (engine == NULL) {
        return LE_APP_RUNNING;
    }
    return engine->quit_requested ? LE_APP_QUIT_REQUESTED
                                  : LE_APP_RUNNING;
}

int le_engine_has_focus(le_engine *engine) {
    if (engine == NULL) {
        return 0;
    }
    return engine->has_focus ? 1 : 0;
}

int le_engine_is_minimized(le_engine *engine) {
    if (engine == NULL) {
        return 0;
    }
    return engine->minimized ? 1 : 0;
}

le_result le_world_set_paused(le_world *world, int paused) {
    if (world == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    world->paused = paused ? 1 : 0;
    return LE_SUCCESS;
}

int le_world_is_paused(const le_world *world) {
    if (world == NULL) {
        return 0;
    }
    return world->paused ? 1 : 0;
}
