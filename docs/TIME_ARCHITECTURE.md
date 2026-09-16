# Time Architecture (Phase 27)

Engine-owned monotonic time. Lua and future native scripts
consume it; only the engine advances it.

## Ownership

- Time state (`struct le_time_state`) lives on the engine,
  created with the engine and destroyed with it.
- Two engines advance independently with separate scales,
  clamps, fixed intervals, and frame indices.
- `le_world` keeps its legacy `time` accumulator for direct
  `le_world_update` callers; the frame path reads engine time.

## Clock source

- Real runtime advances from `lc_clock_now`: nanoseconds,
  monotonic, differences only. Frequency is 1e9 by contract.
- Tests advance with explicit deltas (`le_engine_step`)
  through the same state machine: same clamp, same scale,
  same accumulators, same frame indexing.
- Backward clock steps read as zero and keep the newer
  stamp. This is defensive only; monotonic sources never
  step back.

## Advance order

Each advance applies, in order:

1. Measure raw delta (clock difference) or take the supplied
   delta (explicit path).
2. Sanitize: negative, NaN, and Inf become 0. Absurd values
   above 86400 seconds clamp hard so stats stay finite.
3. Clamp to `max_delta` when nonzero.
4. Scale: `scaled = clamped * time_scale`.
5. Accumulate: `elapsed += scaled`,
   `unscaled_elapsed += clamped`.
6. Increment `frame_index`.

## Fields

- `raw_delta`: measured or supplied delta before the
  configured clamp. Stays visible in stats for diagnostics.
- `scaled_delta`: the simulation delta (`le_time_delta`).
  This is the `dt` scripts receive in `update(dt)`.
- `unscaled_delta`: clamped raw before scale. Recomputed
  from raw plus the max clamp, exact, with no extra field.
- `elapsed`: scaled accumulation consumed by gameplay.
- `unscaled_elapsed`: clamped accumulation consumed by UI,
  profiling, and anything that must run through pause.
- `frame_index`: counts advances from 1. NULL engines read 0.

## Scale validation

- `le_time_set_scale` accepts 0 and up. Scale 1 is normal,
  0.5 is slow motion, 0 is paused.
- NaN, negative, and Inf (above 1e30) are rejected with
  `LE_ERROR_INVALID_ARGUMENT`. The previous scale is kept.
- Lua `Time.set_scale` surfaces the same rejection as an
  `invalid time scale` error.

## Max delta clamp

- Default `max_delta` is 0.25 seconds. Zero disables the
  clamp. Negative, NaN, and values above 86400 are rejected.
- The clamp bounds simulation steps after hitch, breakpoint,
  or tab-out. Raw stays visible so tooling can tell a 5
  second stall from a 0.25 second frame.

## Fixed delta and spiral guard

- Default `fixed_delta` is 1/60. Zero disables `fixed_update`.
  Values above 1.0 clamp to 1.0, matching the Phase 26 rule.
  NaN and negative values are rejected.
- The engine owns the schedule: the interval lives in engine
  time state and is mirrored into each world's Phase 26
  script fields every update.
- Per-world accumulators keep the backlog. Catch-up per
  frame caps at `script_max_steps` (default 4 when unset).
  The catch-up window is `fixed_delta * max_steps * 2`; extra
  backlog drops to zero instead of freezing the frame.
- Engine stats carry `fixed_steps` and `fixed_steps_dropped`
  per frame for tooling. World dispatch skips dead, failed,
  and effectively-disabled instances per step.

## Pause

- Pause is scale 0: `scaled_delta` reads 0, `fixed_update`
  takes no steps, `frame_index` and unscaled time keep going.
- Engine-frame dispatch still runs `update(dt)` with dt 0 so
  pause-aware scripts observe the pause. Direct legacy
  `le_world_update` with dt 0 runs matrices only.
- World pause (`le_world_set_paused`) is independent of
  global scale; see `FRAME_LIFECYCLE.md`.

## Single step

- `le_time_request_single_step` queues exactly one fixed
  interval while paused. The next engine update adds one
  `fixed_delta` to the world accumulator through the normal
  dispatch path, then clears the latch.
- Requesting outside pause records the latch; it fires when
  an update runs at scale 0 with a nonzero fixed interval.

## First frame

- The first advance has a defined delta of 0: no previous
  timestamp exists. Both clock and explicit paths mark
  started and apply 0.
- The explicit path stamps the clock at start so a later
  switch to the clock path measures from step time, not
  from engine creation.

## Suspend

- Clock gaps above one day clamp to 86400 before the max
  clamp, so hibernate or suspend never injects Inf or
  garbage into accumulators.
- Normal hitches land on `max_delta` instead: simulation
  slows through the stall while raw records its length.

## Precision

- Elapsed accumulators are `double`. Long sessions do not
  lose whole frames to float rounding.
- Per-frame `dt` handed to scripts is `float` (Phase 26
  dispatch contract). Queries (`le_time_delta`,
  `le_time_unscaled_delta`, `le_time_elapsed`,
  `le_time_unscaled_elapsed`) report `double`.
- Getters on NULL engines return 0 (deltas, elapsed, frame
  index) or defaults (scale 1, max delta 0.25, fixed 1/60).
  Stats out-pointers may be NULL; NULL engines zero them.
