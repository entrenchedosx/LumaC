# Editor Input Routing (Phase 33V — audited)

Exactly one owner per event stream. No leaks between GUI and
gameplay.

## Ownership

- The GUI owns the `lc_window_read_event` drain:
  `leg_frame_begin` drains the queue ONCE per frame (host must
  not double-drain). The engine NEVER attaches the window
  (`le_engine_attach_window` is never called by the app): no
  engine event pump competes with the GUI.
- `leg_feed_event` NEVER drains (synthetic/headless injection
  only — used by tests; positive mapping matrix in
  `test_editor_gui_gpu` since 33V).
- `leg_wants_keyboard` / `leg_wants_mouse` mirror capture
  state: when the GUI captures (typing in a field, dragging a
  gizmo, hovering a panel), Play injection is suppressed for
  that stream. No input leak in either direction.

## Mapping (`editor/src/gui/gui_input.cpp`)

`lc_keycode` -> GUI keys: full A-Z / 0-9 / modifiers / arrows /
nav / F-keys / punctuation / numpad (unmapped -> none).
Modifier mirror follows the event's own side (33V fix: Phase 33
forced every RIGHT modifier false, so right-side modifiers could
never register). UTF-8 CHAR decode for text fields (control chars
refused), mouse buttons + wheel (both axes). Host-owned events
(focus/close/resize) are never consumed as GUI input.

## Play contract (`app_inject_play_input`, `editor/app/main.c`)

Honest 33V statement (the Phase 33 doc overstated this):
`app_inject_play_input` is a STUB — per-key gameplay injection
from the OS queue while the GUI owns the drain is a documented
non-goal. Phase 33V Play = runtime world ticks (scripts run,
physics integrates), edit world stays paused + byte-identical
(canonical oracle in tests), viewport composites the RUNTIME
world (33V fix: the active camera is carried into the runtime
world; before, Play rendered black). Text fields never leak
keystrokes into gameplay (`leg_wants_keyboard` gate); the engine
never starves the GUI (one queue, one drainer).

## Focus

- Focus loss releases held keys: the frame drain calls
  `io.ClearInputKeys()` on `LC_EVENT_FOCUS_LOST` (33V fix: Phase
  33 documented the release but no-op'd it).
- Shortcuts fire only when no text field is active
  (`leg_wants_keyboard` gate, fresh presses, no auto-repeat):
  `Ctrl+Z/Y/D/S` undo/redo/duplicate/save, `Delete` delete,
  `F` focus selection, `F5` play / `Shift+F5` stop, `F10` step.
  (`Ctrl+N` was listed here in Phase 33 but has never been bound
  — removed from the list rather than left as a false promise.)
- `W/E/R` switch translate/rotate/scale when the viewport is
  hovered, no modifiers held, not playing (33V: implemented to
  match this doc's long-standing promise; the overlay fly block
  skips the switch frame for the switched key so a tap doesn't
  also fly). `WASD/QE` fly, `RMB` orbit, `MMB/Shift+RMB` pan,
  wheel dolly, `F` focus — all hover-gated to the viewport.
- Play-mode guards stay engine-side (`ALREADY_PLAYING` for
  enter/new/open/revert; save allowed mid-play on the edit
  world; reimport/prefab authoring rejected during play).
