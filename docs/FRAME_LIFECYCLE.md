# Frame Lifecycle (Phase 27)

Host-driven frames over engine-owned input and time. The
engine never runs its own loop and never calls `exit()`.

## Contract

Explicit lifecycle, in order:

```
le_engine_begin_frame(engine)   // poll + ingest input, advance clock
le_engine_update(engine, world) // one world: sim + scripts
... render trio (le_world_render_*, unchanged) ...
le_engine_end_frame(engine)     // clear edges + per-frame deltas
```

Convenience and test spellings:

- `le_engine_frame(engine, worlds, count)`: begin, update each
  world in order, end. NULL worlds entries are skipped. NULL
  worlds with count 0 still advances input and time.
- `le_engine_step(engine, world, dt)`: deterministic
  explicit-delta variant. Folds pending injection without
  touching the OS pump, advances the test clock, runs one
  world. Edges stay readable after step returns until the
  next advance or end.
- `le_engine_update(engine, NULL)` is success: input and time
  already advanced at begin.

## Host loop

- The host owns the loop: windowing, frame pacing, and the
  render trio stay in application code.
- `begin_frame` fails with `LE_ERROR_NOT_INITIALIZED` when
  input or time state is missing, and with
  `LE_ERROR_INVALID_ARGUMENT` on NULL engines.
- `frame` stops at the first failing world update, still ends
  the frame to clear edges, and returns the error.
- `step` sanitizes NaN to 0 and clamps negative deltas to 0
  before advancing.

## Quit

- `le_engine_request_quit` sets the flag; `le_engine_cancel_quit`
  clears it; `le_engine_app_state` reports `LE_APP_RUNNING`
  or `LE_APP_QUIT_REQUESTED`. NULL engines report running.
- Window close on any attached window feeds the same flag.
- The engine never calls `exit()`. The host decides when the
  loop ends, including after cancel.

## Resize and minimize

- Resize events update the minimized flag: zero extent sets
  it, nonzero clears it. Observable via
  `le_engine_is_minimized`.
- Focus events update both the input focus flag and the
  engine focus flag. Observable via `le_engine_has_focus`
  and `le_input_has_focus`.
- No resize handling, swapchain work, or rendering policy
  lives here; this layer only observes.

## World pause vs global scale

- `le_world_set_paused` holds one world: updates skip
  simulation but still refresh matrices, so rendering stays
  valid. Pause is per world; the engine clock keeps going.
- Global scale (`le_time_set_scale`) slows or stops scaled
  time for every world on the engine. Unscaled time and the
  frame index keep advancing either way.
- The two compose: a paused world holds still while other
  worlds simulate, including under slow motion.

## Multi-world

- All worlds on one engine share the finalized input
  snapshot and the engine clock in a frame.
- Worlds keep independent storage, tags, scripts, pause
  flags, and fixed-step accumulators. Destroying one world
  leaves survivors untouched.
- Two engines are fully isolated: different input, scales,
  and frame indices can advance side by side in tests.

## Fixed-step ownership

- The engine owns the fixed-step schedule: the interval
  lives in engine time state.
- Each update mirrors the engine interval into the world's
  Phase 26 script fields (`script_fixed_dt`, default
  `script_max_steps` of 4 when unset) before dispatch.
- Worlds updated directly via `le_world_update` use their
  own stored schedule instead. Worlds without an engine
  tick are unaffected by engine fixed-step changes.
- Dispatch order per world: pending starts, fixed steps
  (accumulator, capped catch-up, backlog drop), then
  `update(dt)` in slot order.

## Legacy update contract

- `le_world_update(world, dt)` keeps its Phase 24 to 26
  contract: finite dt above 0 advances the world clock,
  refreshes matrices, and dispatches scripts; NaN, Inf, and
  dt at or below 0 refresh matrices only.
- `le_world_simulate_engine(world, dt)` is the engine seam:
  like the dt-positive path but always dispatches scripts,
  even at dt 0 (pause semantics). NaN and Inf stay
  matrices-only and never reach the VM.
- The render trio calls the legacy path with 0 and never
  runs scripts; script dispatch belongs to the update step.

## Future editor, MCP, and AOT notes

- Injection plus explicit-delta stepping is the replay and
  tooling foundation: editors, MCP harnesses, and tests
  drive identical state machines without windows, devices,
  or sleeps.
- The per-engine snapshot means a future editor can attach
  one engine per viewport or share one engine across
  viewports with documented sharing semantics.
- Lua bindings stay thin over the C API so a future
  native or AOT backend implements the same contract:
  same snapshot visibility, same `update(dt)` value as
  `Time.delta()`, same dispatch order.

## Text input

- Key identity stays physical; text arrives separately as
  UTF-8 scalars, one per injection or `LC_EVENT_CHAR`.
- The engine queues up to 256 scalars. Reads pop one scalar
  per successful `le_input_read_text` call: bytes exclude
  NUL, the buffer is always NUL-terminated on success.
- A short caller buffer reports the needed length and keeps
  the scalar queued. Nothing truncates, nothing is lost.
- No UI system is built on text yet. The queue exists for
  the future console, editor fields, and UI text entry,
  which will consume these scalars without changing the
  key-identity path.
