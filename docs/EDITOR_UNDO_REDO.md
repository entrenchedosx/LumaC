# Editor Undo/Redo (Phase 31)

Bounded, in-memory, session-owned. No disk persistence of history
in Phase 31 (project sidecar stores tuning, not stacks).

## Stacks

- **Undo** (default cap 256, `led_history_set_capacity`, clamped
  [1, 65536]): push on successful execute; pop on undo.
- **Redo**: cleared on every new execute (standard); populated on
  undo; consumed on redo.
- **Eviction**: pushing past capacity drops the oldest entry
  (subtree blob freed), counted in `commands_evicted`.
- **`clear()`**: empties both stacks (never touches dirty).

## Inverses

| Command | Undo | Redo |
| --- | --- | --- |
| CREATE | destroy newborn | recreate (fresh handle) |
| DELETE | restore subtree blob | re-resolve root by name + destroy |
| SET_* value | write before-bytes | write after-bytes |
| ADD_COMPONENT | remove | re-add after-bytes |
| REMOVE_COMPONENT | re-add before-bytes | remove |
| SET_SCRIPT_PROPERTY | write before prop | write after prop |

Undo applies the inverse; on inverse failure the entry is restored
(the engine guarantees validated calls never half-apply, so the
world stays consistent). Undo/redo prune stale selection but never
clear dirty (dirty clears on save/load/new only — see scene docs).

## Stats

`led_history_stats{undo_depth, redo_depth, capacity,
coalesce_enabled, commands_pushed, commands_evicted, undos, redos,
coalesced, bytes_estimate}` — `bytes_estimate` sums entry structs +
live subtree blobs (ordinary malloc backing, no custom allocator
claims). The 10k-command stress (push 10k → undo 10k → redo 10k,
byte-exact final) reports it; proven in `test_editor_history`.
