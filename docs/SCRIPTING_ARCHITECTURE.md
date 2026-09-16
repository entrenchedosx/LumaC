# Scripting Architecture (Phase 24 decision record, Phase 25 update, Phase 26 implementation)

**Lua is the permanent primary gameplay scripting
language.** Lua is IMPLEMENTED in Phase 26 as an interpreted
runtime (Lua 5.4.8 vendored unmodified in `third_party/lua/` +
engine bindings + AOT-compatible script ABI); the AOT/native
compiler is explicitly deferred. Phases 24–25 laid the
groundwork (no VM, no bindings then): Phase 24 built the
stable engine object/component API; Phase 25 adds stable assets,
scenes, and serialization for Lua to bind to.

## Future shapes (both preserved)

```
Lua source → Lua VM / bytecode          (development, hot reload)
```

and optionally:

```
Lua-compatible source → Luma scripting frontend → IR →
generated C/native code                 (future AOT/native builds)
```

No compiler is implemented now, and no promise is made that
arbitrary dynamic Lua trivially becomes standalone C: some Lua
semantics will require runtime support. The requirement ON Phase 24
is narrower: **the engine API and object model must not depend on
Lua VM internals**, so both interpreted and future native/AOT
scripts can eventually call equivalent engine operations.

## Script-friendly ABI (what Phase 24 delivers)

- Plain C handles (`le_object` = three uint32s, `le_world *`
  opaque), plain C structs, explicit `le_result` codes, explicit
  ownership, simple property getters/setters, stable IDs.
- No persistent raw pointers as gameplay identity (component
  storage relocates; identity is the generational handle).
- Future Lua interacts with engine objects/components/assets/worlds
  — never `VkBuffer`/`VkImage`, `lc_*` encoders, renderer
  internals, or GPU pointers.

A later Lua binding should naturally express (IMPLEMENTED in
Phase 26 — this exact shape works, minus `Input` which is still
a future hook):

```lua
function update(dt)
    local t = self:get_transform()
    if Input.key_down("W") then
        t:translate(0, 0, -5 * dt)
    end
end
```

(`Input` still future; transforms use `position()` /
`set_position()` / `translate()` — see `SCRIPT_BINDING_API.md`).

Phase 25 asset/scene shape (IMPLEMENTED in Phase 26, adjusted:
`Assets.load` is glTF import returning `{meshes, materials,
node_count}`; scenes load via `Assets.load_scene`; renderables
attach via `set_renderable(mesh, mat)`):

```lua
local scene = Assets.load("levels/test.scene")
local instance = World.instantiate(scene)

local mesh = Assets.load("meshes/crate.glb")

local obj = World.create("Crate")
obj:set_mesh(mesh)
```

## Readiness map

- Stable identity for scripts: `le_object` + `le_object_is_alive`
  + stale/foreign rejection (scripts can hold handles safely).
- Stable assets: `le_asset` handles + `le_asset_id` persistent IDs
  + registry stats/inspection (scripts reference assets, never
  renderer pointers).
- Scenes: `le_scene` + instance records + persistent→runtime
  mapping (save-games, prefab instances, editor selection).
- Persistence rule: Lua must NEVER persist raw runtime indices to
  disk — asset IDs + scene object IDs only
  (`PERSISTENT_IDENTITY_ARCHITECTURE.md`).
- Introspection for editor/MCP/serialization futures:
  `le_object_info`, `le_world_stats`, `le_memory_stats`,
  `le_asset_stats/info`, `le_scene_get_info`, deterministic
  iteration, optional non-identity names.
- Explicitly deferred at Phase 24/25: script components, hot reload, fixed
  timestep, physics/animation hooks — IMPLEMENTED in Phase 26
  except physics/animation hooks (still future):
  `le_object_add/remove_script`, `le_script_reload`,
  `le_script_set_fixed_step`; error policy (failed instance
  disables, world continues); scene `script` + `sprop` lines.
  See `LUA_SCRIPTING.md`, `SCRIPT_RUNTIME.md`,
  `SCRIPT_BINDING_API.md`, `SCRIPT_AOT.md`.
