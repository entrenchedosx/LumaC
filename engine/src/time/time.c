/*
 * Engine time (Phase 27): monotonic, engine-owned, Lua-consumed.
 *
 * Order of operations per advance: raw delta measured (clock) or
 * supplied (explicit) -> clamp to max_delta (when nonzero) ->
 * scaled = clamped * scale -> elapsed += scaled, unscaled +=
 * clamped -> frame_index++. Negative supplied deltas clamp to 0;
 * NaN/Inf supplied deltas clamp to 0 (never corrupt the
 * accumulators). First frame: delta 0 (no previous timestamp).
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "time/time_internal.h"

#define LE_TIME_DEFAULT_MAX_DELTA 0.25f
#define LE_TIME_DEFAULT_FIXED_DELTA (1.0f / 60.0f)

struct le_time_state *le_time_create(void) {
    struct le_time_state *t =
        (struct le_time_state *)calloc(1, sizeof(*t));

    if (t == NULL) {
        return NULL;
    }
    t->started = 0;
    t->frame_index = 0;
    t->time_scale = 1.0f;
    t->max_delta = LE_TIME_DEFAULT_MAX_DELTA;
    t->fixed_delta = LE_TIME_DEFAULT_FIXED_DELTA;
    return t;
}

void le_time_destroy(struct le_time_state *t) {
    free(t);
}

static void le_time_apply(struct le_time_state *t, double raw) {
    double clamped;
    double scaled;

    if (t == NULL) {
        return;
    }
    if (raw != raw || raw < 0.0) {
        raw = 0.0;
    }
    if (raw > 86400.0) {
        /* Absurd deltas (suspend/hibernate): clamp hard before
         * the configured max so stats stay finite. */
        raw = 86400.0;
    }
    clamped = raw;
    if (t->max_delta > 0.0f && clamped > (double)t->max_delta) {
        clamped = (double)t->max_delta;
    }
    scaled = clamped * (double)t->time_scale;
    t->raw_delta = raw;
    t->scaled_delta = scaled;
    t->elapsed += scaled;
    t->unscaled_elapsed += clamped;
    t->frame_index++;
    t->fixed_steps = 0;
    t->fixed_steps_dropped = 0;
}

void le_time_advance_clock(struct le_time_state *t) {
    uint64_t now;

    if (t == NULL) {
        return;
    }
    now = lc_clock_now();
    if (!t->started) {
        /* First frame: defined delta 0 (no previous stamp). */
        t->started = 1;
        t->last_ticks = now;
        le_time_apply(t, 0.0);
        return;
    }
    {
        uint64_t prev = t->last_ticks;
        double dt;

        t->last_ticks = now;
        if (now >= prev) {
            /* ns -> s (frequency is 1e9 by contract). */
            dt = (double)(now - prev) / 1000000000.0;
        } else {
            /* Clock stepped backwards: treat as zero, keep the
             * newer stamp (monotonic sources never do this;
             * defensive only). */
            dt = 0.0;
        }
        le_time_apply(t, dt);
    }
}

void le_time_advance_delta(struct le_time_state *t, double dt) {
    if (t == NULL) {
        return;
    }
    if (!t->started) {
        t->started = 1;
        t->last_ticks = lc_clock_now();
    }
    le_time_apply(t, dt);
}

void le_time_get_stats(le_engine *engine, le_time_stats *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (engine == NULL || engine->time == NULL) {
        return;
    }
    {
        struct le_time_state *t = engine->time;

        out->frame_index = t->frame_index;
        out->raw_delta = t->raw_delta;
        out->scaled_delta = t->scaled_delta;
        out->elapsed = t->elapsed;
        out->unscaled_elapsed = t->unscaled_elapsed;
        out->fixed_delta = t->fixed_delta;
        out->fixed_steps = t->fixed_steps;
        out->fixed_steps_dropped = t->fixed_steps_dropped;
        out->time_scale = t->time_scale;
    }
}

double le_time_delta(le_engine *engine) {
    if (engine == NULL || engine->time == NULL) {
        return 0.0;
    }
    return engine->time->scaled_delta;
}

double le_time_unscaled_delta(le_engine *engine) {
    if (engine == NULL || engine->time == NULL) {
        return 0.0;
    }
    /* Unscaled = clamped raw (pre-scale). Recompute from the
     * latched raw + max clamp (cheap, exact, no extra field). */
    {
        struct le_time_state *t = engine->time;
        double c = t->raw_delta;

        if (t->max_delta > 0.0f && c > (double)t->max_delta) {
            c = (double)t->max_delta;
        }
        return c;
    }
}

double le_time_elapsed(le_engine *engine) {
    if (engine == NULL || engine->time == NULL) {
        return 0.0;
    }
    return engine->time->elapsed;
}

double le_time_unscaled_elapsed(le_engine *engine) {
    if (engine == NULL || engine->time == NULL) {
        return 0.0;
    }
    return engine->time->unscaled_elapsed;
}

uint64_t le_time_frame_index(le_engine *engine) {
    if (engine == NULL || engine->time == NULL) {
        return 0;
    }
    return engine->time->frame_index;
}

le_result le_time_set_scale(le_engine *engine, float scale) {
    if (engine == NULL || engine->time == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (scale != scale || scale < 0.0f ||
        scale > 1e30f) {
        /* NaN/negative/Inf rejected (scale unchanged). */
        return LE_ERROR_INVALID_ARGUMENT;
    }
    engine->time->time_scale = scale;
    return LE_SUCCESS;
}

float le_time_get_scale(le_engine *engine) {
    if (engine == NULL || engine->time == NULL) {
        return 1.0f;
    }
    return engine->time->time_scale;
}

le_result le_time_set_max_delta(le_engine *engine, float max_delta) {
    if (engine == NULL || engine->time == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (max_delta != max_delta || max_delta < 0.0f ||
        max_delta > 86400.0f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    engine->time->max_delta = max_delta;
    return LE_SUCCESS;
}

float le_time_get_max_delta(le_engine *engine) {
    if (engine == NULL || engine->time == NULL) {
        return LE_TIME_DEFAULT_MAX_DELTA;
    }
    return engine->time->max_delta;
}

le_result le_time_set_fixed_delta(le_engine *engine,
                                  float fixed_delta) {
    if (engine == NULL || engine->time == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (fixed_delta != fixed_delta || fixed_delta < 0.0f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (fixed_delta > 1.0f) {
        fixed_delta = 1.0f; /* same clamp as Phase 26 config */
    }
    engine->time->fixed_delta = fixed_delta;
    return LE_SUCCESS;
}

float le_time_get_fixed_delta(le_engine *engine) {
    if (engine == NULL || engine->time == NULL) {
        return LE_TIME_DEFAULT_FIXED_DELTA;
    }
    return engine->time->fixed_delta;
}

le_result le_time_request_single_step(le_engine *engine) {
    if (engine == NULL || engine->time == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    engine->time->single_step_queued = 1;
    return LE_SUCCESS;
}
