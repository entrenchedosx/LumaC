# Luma Engine Architecture (Phase 24; scripting Phase 26)

Luma Engine (`engine/`, `le_*`, static library) is the scene layer:
generational objects, hierarchy, transforms, lightweight components,
and render extraction on top of **public Luma Renderer + LumaC only**.
Neither LumaC nor the renderer depends on it; the renderer stays
usable without this module.

```
Game / Application
        |
        v
Lua gameplay scripts            [Phase 26, interpreted]
        |
        v
Luma Engine       le_*          [this layer]
        |
        v
Luma Renderer     lr_*
        |
        v
LumaC             lc_*
        |
        v
Vulkan  (future: D3D12)
```

Dependency rules (strict, enforced by a configure-time source audit
in `engine/CMakeLists.txt` — library headers/sources must never name
`Vk*`, `vulkan/`, `windows.h`, X11, LumaC internals, or renderer
internals; only tests take the same white-box privilege as LumaC's
own integration tests):

- `le_*` may use public `lr_*` + `lc_*`. Never the reverse.
- The engine consumes renderer **mesh/material/camera/light**
  resources through public APIs; it never duplicates them.
- The engine allocates **no GPU resources** and owns **no GPU
  synchronization** (no barriers, fences, queues, semaphores —
  those stay in LumaC/renderer).

## Ownership

```
application
    |
    +-- le_engine  (world list + asset registry + borrowed renderer)
    |     |
    |     +-- le_world  (slots, transforms, hierarchy, components,
    |     |              names, active camera, time, last report)
    |     |     |
    |     |     +-- le_object handles (generational, world-local)
    |     |
    |     +-- le_world  (independent storage, own tag + salt)
    |     |
    |     +-- le_asset registry (generational handles, engine-owned
    |              renderer backing: meshes, materials, textures,
    |              scenes; every world references every READY asset)
    |
    +-- lr_renderer (owned backing for DIRECT pointer renderables;
    |     registry-owned backing for asset renderables — app keeps
    |     direct handles alive per the renderer contract, never
    |     registry backing)
    |
    +-- lc_device / lc_swapchain / lc_render_target (LumaC-owned)
```

- The application owns the engine. Direct (Phase 24 pointer)
  renderables still borrow `lr_mesh`/`lr_material` (app keeps them
  alive); asset-backed (Phase 25) renderables hold `le_asset`
  handles (registry keeps backing alive; worlds never destroy it).
- The engine owns world storage plus the asset registry; borrowed
  renderer handles are never destroyed by the engine, owned
  registry backing is (at unload/shutdown, via public APIs).
- Destroy worlds before their renderer/device shuts down
  (dependents-first, like every Luma layer). Destroying the engine
  destroys every world it still owns, then drains the registry,
  then the glTF bridge manager. See `ENGINE_ASSET_ARCHITECTURE.md`,
  `SCENE_ARCHITECTURE.md`.

## Update / render boundary

```
simulation (app mutates objects/components)
      |
      v
le_world_update(world, dt)   — dirty-driven world-matrix refresh
      |
      v
render extraction            — flat snapshot (matrices + stable IDs
      |                         + borrowed mesh/material + lights
      |                         + active camera); pure read, no renderer
      v
le_world_render_scene        — begin + light/renderable submits
(output/output/end trio)       + shadows + scene into HDR target
```

`le_world_update` costs O(1) when clean (dirty-hint scan short-
circuits) and O(alive) worst-case. Extraction scans ascending slot
order — deterministic for unchanged world state (helps future
serialization, Lua, networking, editor). Submission shares one call
path with extraction; the `le_extracted_renderable` snapshot struct
is the documented boundary future phases widen (multithreading,
editor, fixed timestep).

## Thread contract (honest, minimal)

- World mutation: single owning thread.
- Render extraction: same thread unless a future phase documents
  otherwise.
- Read-only inspection is NOT implicitly thread-safe.
- The engine spawns no threads.

## Stats

`le_world_get_stats` / `le_world_get_memory_stats` expose plain
counts (objects, capacity, roots, enabled/disabled, renderables,
cameras, lights, named, time; slot/transform/link/component/name
bytes) — editor/MCP/Lua-ready, no native handles.

## Futures (explicitly NOT Phase 24/25/26)

Full ECS/archetypes, Lua→C/AOT compiler, physics, audio,
animation, input overhaul, editor, MCP, networking, full prefabs,
bindless, meshlets, virtual geometry,
SSAO/SSR/TAA, D3D12, Wayland.

## Phase 25 additions

Engine assets (`ENGINE_ASSET_ARCHITECTURE.md`), scenes +
persistent identity (`SCENE_ARCHITECTURE.md`,
`PERSISTENT_IDENTITY_ARCHITECTURE.md`), canonical serialization
(`SERIALIZATION_ARCHITECTURE.md`), world-preserving reparent +
TRS decomposition, asset-backed renderables (extraction resolves
handles → renderer backing per frame), glTF bridge (adopt, dedup).
Reparent now takes an explicit mode (`LE_REPARENT_KEEP_LOCAL` /
`LE_REPARENT_KEEP_WORLD`); the modeless Phase 24 spelling keeps
local.

## Phase 26 additions

Lua scripting runtime (`LUA_SCRIPTING.md`, `SCRIPT_RUNTIME.md`,
`SCRIPT_BINDING_API.md`, `SCRIPT_AOT.md`): vendored Lua 5.4.8,
one state per engine, `le_script_backend_ops` ABI, lifecycle
dispatch with fixed-step accumulator, `World`/`Object`/`Assets`
bindings over the Phase 25 public API, `export()` properties,
`LE_COMPONENT_SCRIPT` / `LE_ASSET_SCRIPT`, `script` + `sprop`
scene lines (never VM state), transactional reload, budgets. The
native AOT compiler stays explicitly deferred.
