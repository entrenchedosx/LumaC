# Editor Input Routing (Phase 33)

Exactly one owner per event stream. No leaks between GUI and
gameplay.

## Ownership

- The GUI owns the `lc_window_read_event` drain:
  `leg_frame_begin` drains the queue ONCE per frame (host must
  not double-drain). The engine NEVER attaches the window
  (`le_engine_attach_window` is never called by the app): no
  engine event pump competes with the GUI.
- `leg_feed_event` NEVER drains (synthetic/headless injection
  only — used by tests).
- `leg_wants_keyboard` / `leg_wants_mouse` mirror capture
  state: when the GUI captures (typing in a field, dragging a
  gizmo, hovering a panel), Play injection is suppressed for
  that stream. No input leak in either direction.

## Mapping (`editor/src/gui/gui_input.cpp`)

`lc_keycode` -> GUI keys: full A-Z / 0-9 / modifiers / arrows /
nav / F-keys / punctuation / numpad (unmapped -> none).
Modifier mirror (Shift/Ctrl/Alt/Super both sides), UTF-8 CHAR
decode for text fields, mouse buttons + wheel (both axes).

## Play contract (`app_inject_play_input`, `editor/app/main.c`)

While `led_is_playing`, each frame maps the CURRENT `lc_*`
key state to `le_input_inject_*` on the runtime engine —
but ONLY for keys the GUI does not want
(`leg_wants_keyboard` gate per key class; mouse injection
likewise gated on `leg_wants_mouse`). Per-key injection of
arbitrary GUI key events into the runtime is deferred to
Phase 34; Phase 33 Play = runtime tick + isolation (scripts
run, edit stays byte-identical).

## Focus

Panels mirror GUI focus into `led_shortcut_focus_*` so the
shortcut table (`Ctrl+Z/Y/S`, `F5` play toggle, `F10` step,
`Delete`, `Ctrl+D` duplicate, `Ctrl+N` new, `W/E/R` gizmo
modes) fires only when no text field is active. Play-mode
guards stay engine-side (`ALREADY_PLAYING` for
new/open/revert; save allowed mid-play on the edit world).
