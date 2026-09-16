# Editor Commands (Phase 31)

Every mutation flows **UI → `led_command` → engine → history →
dirty**. Commands are plain structs (GUI-independent, MCP-ready):
constructible from values alone, no window/input state required.

## Kinds

```
CREATE / DELETE (subtree snapshot)
SET_NAME / SET_ENABLED
SET_POSITION / SET_ROTATION / SET_SCALE
SET_PARENT / REPARENT (KEEP_LOCAL / KEEP_WORLD)
ADD_COMPONENT / REMOVE_COMPONENT (full le_*_desc bytes)
SET_CAMERA / SET_LIGHT / SET_RIGID_BODY / SET_COLLIDER /
  SET_ANIMATOR / SET_CHARACTER (typed desc shortcuts)
SET_SCRIPT_PROPERTY (le_script_property before/after pair)
```

Payload: `{kind, label, target, parent?, mode?, name?, enabled?,
vec?, component?, comp_bytes[512]+size?, script_prop?}`. Component
bytes are full `le_*_desc` snapshots (size-validated per kind).

## Semantics

- **Validate-then-apply**: engine failure pushes NOTHING — history
  and dirty stay untouched. Stale targets fail `STALE_HANDLE`.
- **Success**: pushes the inverse onto undo, clears redo, sets
  dirty, bumps `commands_executed`.
- **CREATE**: records the newborn handle (tail census) for undo.
- **DELETE**: snapshots the subtree (names, enabled, TRS, parent
  links, component descs, script assets + export values) BEFORE
  destroying. Undo recreates with fresh handles (generations bump
  by engine contract — exact slots are NOT restorable) and restores
  names/TRS/hierarchy/components; verified by name/TRS equality.
  Redo re-resolves the restored root by snapshot name.
- **REPARENT**: mode recorded; `KEEP_WORLD` failures
  (`UNREPRESENTABLE_TRANSFORM`, `CYCLE`) push nothing.
- **Component inverses**: add↔remove with before-image re-add;
  scalar `SET_*` kinds without a before-image remove on undo.
- **Script property**: type must match the export declaration
  (checked by the engine); unknown names fail.

## Coalescing

Consecutive same-target same-kind TRS commands merge within the
configured window (default 500 ms, `led_history_set_coalesce`):
original before-image kept, after-image refreshed, `coalesced`
counted. Drags collapse to one undo step. Disabled in tests for
exact step counting; proven by the drag-merge test.
