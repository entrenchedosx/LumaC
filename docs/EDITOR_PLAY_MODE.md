# Editor Play Mode (Phase 31)

Edit↔runtime isolation over one engine + two worlds. The edit world
is never stepped with gameplay; the runtime world is a disposable
capture→instantiate fork.

## Flow

```
enter: capture(edit) -> create(runtime) -> instantiate -> pause(edit)
tick(dt): le_engine_step(runtime, dt) ONLY; edit gets update(0)
pause/resume/step: le_world_set_paused / le_time_request_single_step
exit: destroy(runtime) -> unpause(edit) -> restore selection
```

- **Enter** (`led_play_enter`): stashes selection, captures the edit
  world into a session scene asset, creates a fresh runtime world on
  the same engine, instantiates (transactional — any failure returns
  `PLAY_FAILED` with the edit untouched and no runtime leaked),
  pauses edit, unpauses runtime, clears selection. Double-enter is
  `ALREADY_PLAYING`.
- **Tick** (`led_play_tick`): explicit-dt `le_engine_step` on the
  runtime world only (deterministic, same-build replay), then
  `le_world_update(edit, 0)` — matrices-only refresh so the outliner
  stays valid while edit scripts NEVER run. Negative/NaN dt is
  `INVALID_ARGUMENT`. Proven: a counter script advances ≥5 ticks on
  the runtime while the edit instance stays frozen (≤1, primed).
- **Pause/step**: per-world pause on the runtime; edit stays paused
  throughout. Single-step queues one fixed interval.
- **Exit** (`led_play_exit`): destroys the runtime world, unloads
  the scene asset, restores the edit pause flag, restores pre-play
  selection by handle (pruned — runtime handles never leak back).
  The edit world is byte-identical to pre-play: proven by the
  canonical capture→serialize oracle (before == after, `test_editor_play`).
- **Stats**: `{playing, runtime_paused, ticks, runtime_elapsed,
  runtime_objects}`.

## Failure semantics

Failed enter (unready asset, OOM, instantiate error) leaves the
edit world untouched and leaks nothing. Scene new/open/revert are
blocked while playing (`ALREADY_PLAYING`); saves are allowed (edit
capture is safe mid-play).

## Determinism note

Same-build explicit-dt replay is deterministic. Cross-platform
bit-identical physics is NOT promised (matches the engine
contract). Input routing during play is host-owned (injection
drives the shared snapshot; contexts/consume masks disambiguate
editor vs game).
