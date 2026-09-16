# Lua Scripting (Phase 26; Input/Time Phase 27; Physics 28; Animation 29)

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

`le_time_set_fixed_delta(engine, fixed_dt)` owns the schedule
(Phase 27; the per-world `le_script_set_fixed_step` mirror is
kept for compatibility): `fixed_update` runs on an accumulator;
`fixed_dt == 0` disables it while `update()` still runs;
catch-up is capped (spiral-of-death guard; backlog is dropped).
`fixed_dt` is clamped to `<= 1.0`. `fixed_update(self, dt)`
receives exactly `Time.fixed_delta()`. See `orbiter.lua`.

## Input (Phase 27)

```lua
function update(self, dt)
    local x = Input.axis("move_x")
    local y = Input.axis("move_z")
    self:translate(x * dt, 0, y * dt)
    if Input.action_pressed("pause") then
        if Time.scale() == 0 then
            Time.set_scale(1)
        else
            Time.set_scale(0)
        end
    end
end
```

Raw: `Input.key_down/pressed/released(Key.W)`,
`Input.mouse_down/pressed/released(Mouse.Left)`,
`Input.mouse_position()`, `Input.mouse_delta()`,
`Input.scroll_delta()` (each delta returns two numbers).
Actions: `Input.action_down/pressed/released("jump")`.
Axes: `Input.axis("move_x")`. Keys are readable `Key.*`
constants (`Key.Space`, `Key.W`, `Key.Escape` — never magic
integers); mouse buttons are `Mouse.*`. All scripts in one
frame see the same finalized snapshot; Lua holds no input
state. See `examples/lua_input/scripts/player.lua`.

## Time (Phase 27)

`Time.delta()` (== the `update(self, dt)` argument, exactly),
`Time.unscaled_delta()`, `Time.elapsed()`,
`Time.unscaled_elapsed()`, `Time.frame()`, `Time.scale()`,
`Time.set_scale(s)` (NaN/Inf/negative rejected),
`Time.fixed_delta()`. Pause (`scale 0`) yields `dt == 0` in
`update` and stops `fixed_update`; rendering and input
continue. See `docs/TIME_ARCHITECTURE.md`.

## Physics (Phase 28)

```lua
function fixed_update(self, dt)
  self:add_force(0, -10 * self:gravity_pull(), 0)
  local vx, vy, vz = self:linear_velocity()
end
function collision_enter(self, other, contact)
  print(contact.penetration) -- table; nil on EXIT
end
function trigger_enter(self, other, contact) end
-- + collision_stay/exit, trigger_stay/exit
```

- `self:linear_velocity()/set_linear_velocity()`,
  `angular_velocity/set_angular_velocity`, `add_force/torque`,
  `apply_impulse[/at_point]`, `add_rigid_body{...}`,
  `add_collider{...}` (thin over `le_physics_*`; see
  `SCRIPT_BINDING_API.md`).
- `Physics.gravity()/set_gravity()`, `Physics.raycast(...)`.
- Character + sweeps (Phase 30): `self:character_move/
  is_grounded/ground_normal/ground_object/velocity/speed/
  set_vertical_velocity/vertical_velocity/teleport/gravity`,
  `Physics.sphere_cast/capsule_cast/box_cast/set_collision_mode`
  — see `LUA_CHARACTER_API.md` (walk/idle select by
  `character_speed()` composes Phases 29 + 30).
- Physics always steps on the fixed schedule (configured rate
  or 60 Hz default) — even with zero scripts. `fixed_update`
  callbacks run before each physics sub-step (forces first).
- Contact tables carry `normal/point/penetration` (A=self ->
  B=other); EXIT events pass nil. Errors mark the instance
  failed per the error policy. See `examples/lua_physics/` and
  `docs/PHYSICS_ARCHITECTURE.md`.

## Animation (Phase 29)

```lua
-- Clips arrive via Assets.find_by_id (persistent IDs):
function start(self)
  local clip = Assets.find_by_id("9f2c...") -- hex, 32 chars
  self:animation_play(clip)              -- restart at 0
  self:animation_play(clip, false)       -- continue same-clip time
end
function update(self, dt)
  if not self:animation_is_playing() then
    self:animation_resume()
  end
  print(self:animation_time(), self:animation_duration())
end
-- + animation_pause/resume/stop(reset?), animation_seek(t),
--   animation_speed([s]), animation_crossfade(clip, seconds)
```

- Thin over `le_anim_*` (see `SCRIPT_BINDING_API.md`); no
  Lua-side animation state exists. Stale clip handles error.
- Animators are usually attached in C or via scenes; Lua
  drives playback (menus, cutscenes, combat timing).
  See `examples/lua_animation/` and
  `docs/ANIMATION_ARCHITECTURE.md`.

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
