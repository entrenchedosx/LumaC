# Script Runtime Internals (Phase 26)

One `lua_State` per `le_engine`, created lazily on first script
use (`le_script_runtime_ensure`), destroyed at engine shutdown
after every world released its instance refs. The runtime is
driven exclusively from the world's owning thread
(`le_world_update`); states are never shared across threads.
Public surface: `le_script_*` in `luma_engine.h`; internals in
`engine/src/script/script_internal.h` (never included outside
`src/script/`).

## World isolation via world_tag

Lua userdata carries `{engine ptr, world_tag, index, generation}`
(objects) or `{engine ptr, index, generation}` (assets/scenes).
Bindings resolve the tag to a live world/slot on every call and
reject cross-world misuse (`"cross-world object misuse"`;
mutations additionally require the firing world — see
`le_obj_mutable`). One state serves all worlds on the engine;
isolation is by tag checks, not by separate states.

## Allocator wrapper + budget accounting

`le_lua_alloc` wraps `realloc`/`free`, tracking `lua_bytes`
(live), `lua_peak`, and `alloc_count` (visible in
`le_script_stats`). `memory_budget_bytes > 0` enforces a hard
cap: over-budget growth returns NULL (Lua raises OOM inside the
callback, caught by pcall). Failures never unwind past the
engine: every entry point uses pcall.

## LUA_MASKCOUNT hook

Per-callback instruction budget (`max_instructions`, default
`LE_SCRIPT_DEFAULT_INSTRUCTIONS` = 2M), armed by
`le_script_hook_begin` around chunk runs, `require()` loads,
and every `fire()`. Fires every `LE_SCRIPT_HOOK_GRANULE` (1024)
instructions; over-budget aborts via `lua_error`
(`"instruction budget exceeded"`), recorded once per the error
policy. Count-based, never wall-clock — deterministic.

## VM-independent le_script_backend_ops table

`script_internal.h` defines 8 ops; Lua implements backend #1
(`le_lua_backend_ops` in `script_lua.c`):

```
compile / release_chunk
instantiate / release_instance
fire (0=start, 1=update, 2=fixed, 3=destroy)
get_prop / set_prop / list_props
```

General engine sources reach Lua only through three hooks in
`engine_internal.h` (`le_script_step_world`,
`le_script_fire_slot_destroy`, `le_script_fire_world_destroy`
plus capture/apply/release helpers). A future native backend
implements the same table without Lua (see `SCRIPT_AOT.md`).

## instantiate / fire / get / set / list_props flow

- `compile`: `luaL_loadbufferx(..., "t")` (text-only chunks);
  failures fill `out_error`, registry unchanged.
- `instantiate`: builds the state table, runs the chunk once
  with the per-instance `_ENV` (collects `export()` metadata +
  lifecycle funcs), stores `state_ref` (registry ref). Failures
  disable the instance transactionally.
- `fire`: fetches `funcs[name]` from the state table; absent
  callback is a no-op (returns 0). Calls as `fn(self, dt?)`
  with `self` also installed globally (nesting-safe registry
  stack). Nonzero return = instance failed (caller disables).
- `get_prop`/`set_prop`/`list_props` convert between the state
  table's values and `le_script_property` (int/number coerce;
  NaN rejected; asset values must be live handles).

## Per-instance _ENV + state table

State table: `{values, exports, funcs, env}` (registry-ref'd).
`env = {self, export, require, print, _G}` with `__index -> _G`
(shared stdlib view). `export()` records into registry anchors
(`le_inst_exports`/`le_inst_values`) during instantiation only;
calling it elsewhere errors. Chunks with no `_ENV` upvalue
(empty scripts) run unisolated — fine, nothing to isolate.

## Snapshot dispatch (starts → fixed → update, slot order)

`le_world_update` (finite `dt > 0` only) refreshes matrices,
then `le_script_step_world`:

1. PASS 1 — pending starts in ascending slot order,
   effectively-enabled only (disabled stay pending).
2. PASS 2 — fixed steps (`script_accum`, capped at
   `max_steps`; backlog dropped).
3. PASS 3 — `update(dt)` in slot order.

Iteration runs over a snapshot of (slot, generation) copies —
never raw entry pointers — revalidating every row, so
structural mutation inside callbacks (create/destroy/reparent/
attach/remove/disable) is immediate and safe: new objects start
next frame; destroyed ones resolve stale. Nested dispatch depth
is capped (64). Destroy-other before its turn skips its update;
destroy-self may finish the current callback.

## Teardown guards

World destroy sets `scripts_tearing_down`, fires `destroy()` in
reverse slot order (iff `start()` ran; errors recorded, never
fatal), then releases backend state. Structural script ops
during teardown fail safely. Object destroy / remove-script fire
`destroy()` first with the started-flag cleared upfront
(reentrant destroy-self is a no-op, not recursion).

## Rebind-on-reload

`le_script_reload` recompiles + swaps the chunk, then rebinds
started healthy instances: snapshot values, rebuild state from
the new chunk, restore values for still-declared names, keep new
funcs. `start()` is NOT re-run. Pending/failed instances adopt
the new code on next start. Failure keeps old state.

## Determinism notes

Ascending-slot order, count-based (never clock-based) budgets,
text-only cached chunks, project-rooted `require()`. Identical
worlds + identical `dt` sequences agree bit-for-bit (tested:
two engines, 4 movers, 5 frames, `x == 5`). `float`↔`double`
conversions at the boundary are exact for gameplay magnitudes;
`%.17g` round-trips doubles through scene text.
