/*
 * Engine time internals (Phase 27). Engine-owned, Lua-consumed.
 * Real runtime advances from lc_clock_now (ns, monotonic); tests
 * advance with explicit deltas through the same state machine.
 */

#ifndef LE_TIME_INTERNAL_H
#define LE_TIME_INTERNAL_H

#include <stdint.h>

struct le_time_state {
    int started; /* nonzero once the first frame ran */
    uint64_t frame_index;
    uint64_t last_ticks; /* lc_clock_now ns at last advance */
    double raw_delta;
    double scaled_delta;
    double elapsed;
    double unscaled_elapsed;
    float time_scale;
    float max_delta;   /* 0 = unclamped */
    float fixed_delta; /* 0 = fixed_update disabled */
    uint32_t fixed_steps;
    uint32_t fixed_steps_dropped;
    int single_step_queued;
};

/* Lifecycle (time.c). */
struct le_time_state *le_time_create(void);
void le_time_destroy(struct le_time_state *t);
/* Advance from the host clock (begin_frame path). */
void le_time_advance_clock(struct le_time_state *t);
/* Advance with an explicit delta (deterministic tests + step). */
void le_time_advance_delta(struct le_time_state *t, double dt);

#endif /* LE_TIME_INTERNAL_H */
