# Lua Scripting (Phase 26)

Lua 5.4.8 (vendored in `third_party/lua/`) is the gameplay
scripting language. Scripts are engine assets
(`LE_ASSET_SCRIPT`), attach one-per-object, and run from
`le_world_update`. See `SCRIPT_BINDING_API.md` (reference),
`SCRIPT_RUNTIME.md` (internals), `SCRIPT_AOT.md` (contract).

## Lifecycle

```lua
export("speed", 2.0)          -- optional exported properties

function start(self) end      -- once, when effectively enabled
function update(self, dt) end -- every frame (variable dt)
function fixed_update(self, dt) end -- fixed-step clock (opt-in)
function destroy(self) end    -- on remove/destroy, iff start() ran
```

- Attach with `le_object_add_script` (replaces any existing
  script); `start()` runs on the next `le_world_update` once the
  object is effectively enabled (disabled objects stay pending —
  start is delayed, never skipped).
- `dt <= 0`, NaN, or Inf runs matrices only: no callbacks fire
  (`render_scene`'s internal `update(0)` never runs scripts).
- Absent callbacks are no-ops, not errors.
- `le_object_remove_script` fires `destroy()` iff `start()` ran;
  a missing component is success/no-op.
- `self` is the owning object: passed as the first arg AND
  installed as the global `self` for the call (arg form is
  canonical — see `SCRIPT_AOT.md`). Object methods plus export
  sugar (`self.speed`); see `SCRIPT_BINDING_API.md`.

## export() + property types

`export(name, default)` at chunk top level declares a
VM-independent property (max 16 per script, `LE_SCRIPT_MAX_PROPS`;
names must be `[A-Za-z_][A-Za-z0-9_]*`, under 64 chars):

| Lua default | Type | Notes |
|---|---|---|
| `true` | bool | |
| `3` (integer) | int | 64-bit |
| `3.5` (float) | number | double; NaN rejected |
| `"bot"` | string | truncated to 255 bytes |
| `{1, 2, 3}` | vec3 | 3-number array table |
| asset userdata | asset | live handle on this engine |

Read/write from Lua via `self.speed` sugar or
`self:get(name)` / `self:set(name, value)`; from C via
`le_script_get/set_property` (type must match; int/number coerce
across the boundary in Lua). `le_script_list_properties` reports
the full export list. Unknown names and type mismatches fail
(`LE_ERROR_INVALID_ARGUMENT` from C, script error from Lua).

## World / Object / Assets APIs

```lua
local c = World.create("spawned")   -- create (+ optional name)
World.destroy(v)                    -- destroy (stale-safe)
local o = World.find("victim")      -- object or nil
if World.is_alive(o) then ... end
local inst = World.instantiate(scene)
local o2 = inst:find_by_id("32hex...") -- persistent-ID lookup
local n = inst:object_count()

local x, y, z = self:position()
self:set_position(x + self.speed * dt, y, z)
self:translate(0, 1, 0)
self:rotate_y(angle)
self:rotate_axis_angle(0, 1, 0, angle)
self:set_scale(1, 1, 1)
local wx, wy, wz = self:world_position() -- read-only
self:set_name("Bot") / local n = self:name()
self:set_enabled(false) / self:is_enabled()
self:set_parent(other)  -- nil detaches; "keep_world"|"keep_local"
self:has_component("renderable")     -- transform/renderable/
                                     -- camera/light/script
self:set_renderable(mesh, mat) / self:remove_renderable()

local imp = Assets.load("models/crate.glb") -- {meshes, materials, node_count}
local scene = Assets.load_scene("levels/a.scene")
local a = Assets.find_by_id("32hex...")     -- asset or nil
```

`World.*` / `Assets.*` require dispatch context (calling them
outside a callback errors). `set_renderable` takes asset-backed
mesh + material handles. Full signatures in
`SCRIPT_BINDING_API.md`.

## require("scripts.x") modules

Dots become path separators (`scripts.ai` →
`scripts/ai.lua`), resolved beneath project roots only:
`le_script_config.script_root` plus up to 16
`le_script_add_search_path` roots. `..` escapes and illegal
characters are rejected; modules are cached per engine. Sources
are capped at 4 MB (`LE_SCRIPT_MAX_SOURCE`).

## print routing

Global `print(...)` routes to `le_script_config.log_fn`
(`log_user` context); default is stderr with a `[script]` tag.
Scripts never write stdout directly.

## Error policy

A callback error disables that instance (`failed=1`, visible via
`le_object_script_failed`) — the world and all healthy instances
continue. The error (message + traceback + callback + object) is
recorded once and readable via `le_script_get_last_error`;
`le_script_stats` counts errors. Syntax errors fail asset
creation with `LE_ERROR_PARSE` and create nothing
(transactional). See `examples/lua_scene/scripts/error_demo.lua`.

## Fixed-step config

`le_script_set_fixed_step(world, fixed_dt, max_steps)`:
`fixed_update` runs on an accumulator; `fixed_dt == 0` disables
it (default) while `update()` still runs; `max_steps` caps
catch-up (default 4, spiral-of-death guard; backlog is dropped).
`fixed_dt` is clamped to `<= 1.0`. See `orbiter.lua`.

## Reload semantics

`le_script_asset_set_source` (recompile first; failure keeps the
old source) + `le_script_reload`: live instances adopt the new
code, keep exported values and instance state, and do NOT re-run
`start()`. Pending/failed instances pick the new code up on next
start. Failure leaves old code live (transactional).

## Sandbox

Available: `table`, `string`, `math`, `utf8`, `coroutine`, plus a
base subset. Absent: `io`, `os`, `package`, `debug`,
`dofile`/`loadfile`/`load`, `collectgarbage` (GC policy is
engine-owned). Per-callback instruction budget: 2M default
(`max_instructions_per_callback`), enforced by a count hook —
deterministic, never wall-clock.

## Memory budget config

`le_script_config.memory_budget_bytes` sets a hard Lua cap
(`0` = unlimited). Live/peak bytes are in `le_script_stats`.

## Persistence

Scripts persist as `script <hex>` + one `sprop <kind> <name>
<value...>` line per export (asset IDs + values only — never
VM state). Unknown `sprop` names on load are skipped.
