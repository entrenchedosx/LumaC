# Script AOT Contract (Phase 26)

Rule: **Script semantics ─┤├→ Engine operations.** The VM (or
any future native backend) must not define gameplay semantics:
identity, ordering, lifecycle, persistence, and error policy
live in the engine (`le_*`). The backend is an execution
frontend only — it fetches callbacks, converts values, and
calls back into the engine.

## Arg-form canonical calling

`fire()` calls `fn(self, dt?)`: `self` as explicit first arg
(`dt` second for `update`/`fixed_update` only) is the canonical
form a native backend maps to a context parameter. The global
`self` (installed per call, nesting-safe) is convenience for
the same value — backends must provide both, with arg form
authoritative.

## Backend ops table (porting surface)

`le_script_backend_ops` (`engine/src/script/script_internal.h`)
is the port for a future native backend — implement these 8,
no Lua involved:

```
compile(source, size, chunkname) -> chunk token | error
release_chunk(chunk)
instantiate(rt, world, entry)    -> 0 ok | nonzero (diagnostics recorded)
release_instance(rt, world, entry)
fire(rt, world, entry, cb, dt)   -> 0 ok | nonzero (instance failed)
get_prop / set_prop / list_props (le_script_property values)
```

Callback ids: 0=start, 1=update, 2=fixed_update, 3=destroy.
Absent callbacks are no-ops. General engine code must keep
reaching scripts only through the `engine_internal.h` hooks
(step/fire/destroy/capture/apply/release) — never backend
types (`lua_State *` never crosses `luma_engine.h`).

## What a native backend must implement

1. Lifecycle: pending-start gating (effectively-enabled only),
   starts→fixed→update slot order, destroy-iff-started —
   driven by the existing snapshot dispatcher, not the
   backend.
2. `export()` metadata: name/type/default + live values
   converting losslessly to/from `le_script_property`
   (bool/int/number/string/vec3/asset; max 16).
3. `self` identity: generational handle checks (world_tag +
   index + generation) with stale/cross-world rejection.
4. Error policy: failing callback disables the instance,
   records once, world continues; syntax errors fail
   compile transactionally.
5. Persistence: scene capture/apply of asset IDs + export
   values (below) — backend state never serialized.

## Persistence rule

Scenes store `script <asset-hex>` + one `sprop <kind> <name>
<value...>` line per export (asset props persist registry IDs).
Reload re-applies values onto fresh instance state. Never
persist VM state: no bytecode, no closures, no upvalues, no
registry refs, no raw indices. Unknown `sprop` names on load
are skipped (forward compat).

## Explicitly NOT implemented

No Lua→C compiler, no JIT, no ahead-of-time codegen of any
kind — Phase 26 ships the interpreted backend only. No physics,
no animation, no input overhaul hooks. The ops table reserves
the porting surface; nothing on the native side exists yet.
(AOT consumers also inherit the sandbox: no filesystem, no OS
clocks, no dynamic code loading beyond the asset pipeline.)
