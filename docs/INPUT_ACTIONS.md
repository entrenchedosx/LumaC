# Input Actions, Axes, and Contexts (Phase 27)

Named indirection over raw device state. Games bind gameplay
verbs once, query handles per frame, and route them through
priority contexts.

## Handles

- Actions, axes, and contexts are opaque `{index, generation}`
  handles. Stale handles fail safely and query as idle or zero.
- Handles resolve once via `le_input_find_*` and pass by value
  afterwards. Lua resolves by name per call; C hosts cache.
- Names are printable ASCII, 1 to 127 bytes for actions and
  axes (buffer 128), 1 to 63 bytes for contexts (buffer 64).

## Actions

- An action is a digital verb: down, pressed, released.
- Down means any visible binding is down. Edges run on the
  aggregate, not on individual bindings.
- Aggregate latch: `was_down` captures the previous frame's
  aggregate before pending folds. Pressed means up-then-down;
  released means down-then-up across that boundary.
- Example: `jump` bound to Space plus gamepad A. Holding Space
  then pressing pad A does not re-press. Releasing Space while
  pad A stays held keeps the aggregate down. Releasing both
  raises one release edge.
- Actions accept key, mouse button, and gamepad button
  bindings only. Motion and analog sources are axis-only and
  rejected with `LE_ERROR_INVALID_ARGUMENT`.
- Duplicate add is idempotent success. Remove swaps the last
  entry down and fails when absent. Clear empties the list.
- Repeat creation with a live same name returns the existing
  handle instead of creating a duplicate.

## Binding kinds

- `LE_BINDING_KEY`: physical `le_key`.
- `LE_BINDING_MOUSE_BUTTON`: mouse button index.
- `LE_BINDING_GAMEPAD_BUTTON`: button plus slot.
- `LE_BINDING_GAMEPAD_AXIS`: analog source, axis-only.
- `LE_BINDING_MOUSE_DELTA_X/Y`: pixels per frame, axis-only.
- `LE_BINDING_MOUSE_WHEEL_X/Y`: detents per frame, axis-only.
- Payloads validate by kind: bad keys, buttons, slots, axes,
  or unknown kinds fail binding calls.

## Axes

- An axis is a signed scalar sampled in active contexts, with
  deadzone, scale, and invert applied after binding mix.
- Digital bindings carry a pole scale: A at -1 plus D at +1.
  Equal opposite poles cancel to 0. Unequal poles resolve to
  the stronger pole. Results clamp to `[-1,1]`, then apply
  axis scale and optional sign flip.
- Gamepad sticks apply the axis deadzone; below it reads 0.
  Triggers bypass the stick deadzone and read 0 below a small
  rest threshold, then scale normally.
- Mouse delta and wheel bindings sample the current frame
  snapshot times their binding scale.
- Defaults: axis scale 0 means 1; deadzone must sit in
  `[0,1)`; NaN deadzone or NaN scale is rejected. Digital
  bindings with scale 0 default to +1.
- Unbound, invisible, stale, or NULL axes sample 0.

## Contexts

- A context is a named map with a priority and a consume mask.
  Higher priority wins on overlap; default priority is 0.
- Actions and axes start global: visible in every context.
  The first context bind narrows an entry to its owning
  contexts. Narrowed entries hide while none of their owners
  is active.
- Activate and deactivate are explicit. Repeat creation with
  a live same name updates priority and returns the handle.
- Consumption: the top active context by priority may consume
  keyboard, mouse, or both. Consumed domains read as released
  or zero to raw queries, action aggregates, and axis samples
  below it.
- Example: `gameplay` at priority 0 owns `fire` on F.
  `console` at priority 10 consumes keyboard while active.
  With console up, raw F reads quiet and gameplay stops.
  Deactivating console restores raw F without rebinding.
- Masks outside `LE_CONSUME_ALL` are rejected.

## Rebind and query APIs

- Create, find, add, remove, clear, and get-bindings exist for
  actions and axes. Contexts add activate, deactivate,
  active-query, bind-action, bind-axis, and set-consume.
- Get-bindings supports counting queries: NULL out or zero
  capacity still reports the full count.
- Removal order is not stable (swap-remove). Queries report
  counts; callers copy into their own memory.

## Capacity and allocation policy

- Caps: 4096 actions, 1024 axes, 64 contexts, 16 bindings per
  entry, 1024 pending events, 256 queued text scalars.
- Registries grow geometrically and fail with
  `LE_ERROR_OUT_OF_MEMORY` on allocation failure or
  `LE_ERROR_OVERFLOW` past the cap. Partial state is never
  left behind on failure.
- Pending flood drops the oldest event and keeps ingesting.
  Text past the queue cap drops the extra scalar.

## Per-frame string work

- Lua `Input.action_*` and `Input.axis` resolve by name with
  one hash lookup plus compare per call. No per-frame scans
  of unrelated entries.
- C and future native code avoid even that cost by caching
  handles after `le_input_find_*` and passing them by value.

## AOT mapping

- Lua `Input.*` maps directly onto `le_input_*` C calls with
  the same snapshot semantics:
  `key_down/pressed/released`, `mouse_down/pressed/released`,
  `mouse_position`, `mouse_delta`, `scroll_delta`,
  `action_down/pressed/released`, `axis(name)`.
- Key and mouse constants expose readable names (`Key.W`,
  `Key.Space`, `Mouse.Left`) over the same integer values.
- No Lua-side input state machine exists: scripts observe the
  engine snapshot advanced once per frame before dispatch.
  `update(dt)` receives exactly `Time.delta()`.

## Serialization

- Input-map file serialization is DEFERRED. No input-map
  format is specified in Phase 27.
- The data model that a future format must persist is:
  action names plus digital binding lists; axis names plus
  deadzone, scale, invert, and binding lists with pole
  scales; context names, priorities, consume masks, and
  narrowed membership lists.
- Runtime handles (`{index, generation}`) are never persisted.
  Names are the durable identity; handles re-resolve on load.
