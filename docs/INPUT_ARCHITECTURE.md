# Input Architecture (Phase 27)

Engine-owned gameplay input. The engine owns all input state per
`le_engine`; platform code translates OS events; Lua and future
native scripts consume one finalized per-frame snapshot.

```
Win32 / X11 backend
        |
        v
lc_window_event queue (per window, backend-neutral)
        |
        v
le_engine_begin_frame: lc_poll_events + drain + advance
        |
        v
finalized snapshot (held + edges + deltas, shared by worlds)
        |
        v
le_engine_update (scripts) ... le_engine_end_frame (clear edges)
```

## Ownership

- Input state (`struct le_input_state`) lives on the engine, created
  with `le_engine_create`, destroyed with the engine.
- Worlds share one snapshot. Worlds keep independent object and
  script state but observe identical input in a frame.
- No gameplay input logic lives in Lua, the renderer, or any
  graphics backend. Lua bindings are thin queries.
- Two engines are fully isolated: separate held, edges, time,
  actions, and gamepad slots.

## Platform boundary

- Backends translate OS events into `lc_window_event` and queue
  them per window (`LC_EVENT_KEY_DOWN/UP`, `CHAR`, `MOUSE_*`,
  `WHEEL`, `FOCUS_*`, `RESIZE`, `CLOSE`).
- The engine drains every attached window with
  `lc_window_read_event` during `le_engine_begin_frame`, after
  `lc_poll_events`. The queue is drained, never implicitly
  cleared; unread events persist until read.
- The engine never names backend symbols and never includes
  backend headers. It consumes only the neutral queue.

## Conventions

- Key identity is physical position, not text: `LE_KEY_W` is the W
  position on a US layout regardless of layout or modifiers.
  Values mirror `lc_keycode` ordering and are Luma-owned.
- Mouse position is client-area pixels of the focus window:
  origin top-left, +x right, +y down.
- Mouse deltas are pixels of relative motion accumulated since
  the previous input frame.
- Wheel is detents (lines), up and right positive.
- Modifiers are a `le_key_mod` bitmask mirroring `lc_key_mod`,
  carried on key, char, and mouse events.

## Frame model

- Pending events (platform plus injection) land in `pending[]`.
- `le_input_advance_frame` folds pending into `held[]`, computes
  `pressed` and `released` edges, accumulates motion and wheel
  deltas, appends text, applies focus, then clears pending.
- Scripts observe the snapshot until the next advance.
- `le_input_end_frame` clears edges and per-frame deltas. Held
  state, absolute position, and the text queue persist.
- Press plus release in one frame sets both edges with held
  clear. Release always edges, even without a recorded press.

## Repeat policy

- OS auto-repeat never sets `pressed`. First press only.
- A held key stays down across repeats without re-arming the edge.
- Injection always enters with repeat clear, so tests exercise
  the same first-press path.

## Focus loss

- Focus loss clears held keys and buttons immediately. No stuck
  movement.
- No release edge is synthesized. Motion already stopped; a
  phantom release would re-trigger tap detection.

## Multi-window policy

- An engine observes zero or more windows via
  `le_engine_attach_window` and `le_engine_detach_window`.
- The host attaches each window whose queue feeds the engine.
  Duplicate attach is success. Detach of an unknown window
  fails with `LE_ERROR_INVALID_ARGUMENT`.
- The engine never destroys windows. Detach on window destroy.
- Close on any attached window raises the quit request.
- Resize to zero marks minimized; nonzero clears it.
- The most recently gained focus wins for mouse position.

## Injection

- Injection feeds the same pending list through the identical
  state machine and edge rules. Deterministic: no clock, no
  devices, no windows needed.
- APIs: key, mouse button, mouse move, scroll, text, focus,
  gamepad button, gamepad axis. Bad enums and NULL engines
  fail with `LE_ERROR_INVALID_ARGUMENT`.
- Pending holds 1024 events. Flood drops the oldest and keeps
  counting ingestion in stats.

## Cursor modes

- `LE_CURSOR_NORMAL`, `LE_CURSOR_HIDDEN`, `LE_CURSOR_CAPTURED`.
- `le_input_set_cursor_mode` is a best-effort request and
  returns the previous mode. Invalid modes leave state alone.
- Phase 27 records the mode for bindings and future backends.
  Platform capture is deferred; no backend support is faked.

## Stats

`le_input_stats` reports keys down, mouse buttons down, events
ingested, action and axis counts, active contexts, connected
gamepads, and pending events. NULL engine or NULL out yields
zeros or no-op; value getters zero-fill on bad input.

## Error model

- Fallible functions return `le_result`. NULL and out-of-range
  inputs return `LE_ERROR_INVALID_ARGUMENT` or zero, never crash.
- Stale action, axis, and context handles query as idle or zero.
- Destroys are NULL-safe.

## Thread contract

- Poll, advance, query, and inject on the owning thread only.
  No thread safety is claimed.

## Platform neutrality and Wayland

- Public key, button, modifier, and event values are
  backend-neutral. `Win32 VK_*` and X11 `KeySym` never cross
  the engine boundary.
- Wayland is planned, not present. Linux runs on X11, verified
  under XWayland. A Wayland backend only needs to emit the same
  `lc_window_event` shapes; engine code does not change.

## Gamepad honesty

- Gamepad API is real and backend-neutral: 8 slots, 14 buttons,
  6 axes. Sticks report `[-1,+1]`; triggers report `[0,1]`.
- Platform reporting is PARTIAL in Phase 27: slots exist,
  injection drives them, OS gamepads report disconnected until
  a platform backend lands. Presence is never faked.
- Queries on NULL engines, bad slots, bad enums, or
  disconnected slots return zero.
