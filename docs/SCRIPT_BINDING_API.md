# Script Binding API (Phase 26; Input/Time Phase 27; Physics 28; Animation 29)

Lua-visible surface registered at runtime creation
(`script_bind_world.c`, `script_bind_object.c`,
`script_bind_input.c`, `script_bind_physics.c`,
`script_bind_anim.c`). All bindings
are thin over `le_*`; failures raise catchable script errors
(caught by dispatch per the error policy), never crashes.
`World.*` / `Assets.*` / `Input.*` / `Time.*` / `Physics.*`
require dispatch context (`firing_world`); calling them outside
a callback errors.

## World.*

- `World.create(name?) -> object` — create; optional name
  (nil/omitted = unnamed). Errors outside dispatch, while
  tearing down, or on engine failure.
- `World.destroy(obj)` — destroy; stale/cross-world handles
  error. Cross-world-vs-firing check is strict.
- `World.find(name) -> object | nil` — first live exact-name
  match in slot order (nil when absent).
- `World.is_alive(obj) -> bool` — false for stale handles
  (never errors).
- `World.instantiate(scene) -> instance` — transactional scene
  instantiate; tracks the record (registry serial) for the
  Lua handle. Accepts scene or asset userdata
  (layout-identical). Cross-engine scenes rejected.

## instance methods

- `inst:find_by_id(hex) -> object | nil` — persistent-ID lookup
  within the instance (nil when dead/absent).
- `inst:object_count() -> int` — live record size.

## Assets.*

- `Assets.load(path) -> {meshes, materials, node_count}` —
  glTF import (`le_gltf_import`); `meshes`/`materials` are
  1-based asset arrays. Import failure errors.
- `Assets.load_scene(path) -> scene` — create + load a scene
  file (unloads the scene asset on load failure).
- `Assets.find_by_id(hex) -> asset | nil` — READY-asset lookup
  by persistent-ID hex (nil when absent/unparseable).

## Object methods (self:method(...))

- `name() -> string` / `set_name(s)`
- `is_alive() -> bool` (false for stale; never errors) /
  `is_enabled() -> bool` / `set_enabled(bool)`
- `parent() -> object | nil` / `set_parent(p?, mode?)` — nil
  detaches; `mode` is `"keep_local"` (default) or
  `"keep_world"` (shear/singular errors); stale/cross-world
  parents rejected.
- `position() -> x, y, z` (local) /
  `set_position(x, y, z)` / `translate(dx, dy, dz)` /
  `world_position() -> x, y, z` (read-only, from world matrix)
- `set_scale(x, y, z)` / `rotate_axis_angle(x, y, z, angle)` /
  `rotate_y(angle)` (radians; premultiplied onto local rotation)
- `has_component(kind) -> bool` — `transform`, `renderable`,
  `camera`, `light`, `script` (unknown kind errors).
- `set_renderable(mesh, mat)` — asset-backed mesh + material
  handles (stale/cross-engine rejected; visible + shadowed
  defaults) / `remove_renderable()`
- `get(name) -> value` / `set(name, value)` — explicit export
  access (unknown property errors; int/number coerce).

(For exact numeric semantics — e.g. rotation premultiplication
order — verify against `engine/src/script/script_bind_*.c`.)

## Input.* (Phase 27; `script_bind_input.c`, thin over `le_input_*`)

- `Input.key_down/pressed/released(key) -> bool` — raw edges
  from the finalized snapshot (repeat never presses).
- `Input.mouse_down/pressed/released(btn) -> bool`
- `Input.mouse_position() -> x, y` /
  `Input.mouse_delta() -> dx, dy` /
  `Input.scroll_delta() -> dx, dy` (two returns each).
- `Input.action_down/pressed/released("name") -> bool` —
  aggregate over bindings (unknown action errors).
- `Input.axis("name") -> number` (unknown axis errors).
- `Key.*` constants (`Key.W`, `Key.Space`, `Key.Escape`, …) and
  `Mouse.*` (`Mouse.Left`, …) — readable, no magic integers.

## Time.* (Phase 27; thin over `le_time_*`)

- `Time.delta()` (== `update` dt, exactly) /
  `Time.unscaled_delta()` / `Time.elapsed()` /
  `Time.unscaled_elapsed()` / `Time.frame()` /
  `Time.scale()` / `Time.fixed_delta()`.
- `Time.set_scale(s)` — invalid scales error; permission is
  gameplay-controlled (example scripts toggle pause with it).

## Physics (Phase 28; `script_bind_physics.c`, thin over `le_physics_*`)

- `self:linear_velocity() -> x, y, z` /
  `self:set_linear_velocity(x, y, z)` (dynamics; missing body
  errors; non-finite errors).
- `self:angular_velocity() -> x, y, z` /
  `self:set_angular_velocity(x, y, z)` (world axes, rad/s).
- `self:add_force(fx, fy, fz)` / `self:add_torque(tx, ty, tz)` —
  accumulate for the next step (static/kinematic/missing ignore
  with success at the C layer; bindings still require a body).
- `self:apply_impulse(jx, jy, jz)` /
  `self:apply_impulse_at_point(jx, jy, jz, px, py, pz)` —
  immediate velocity change (point form adds `dw`).
- `self:add_rigid_body{type=, mass=, linear_damping=,
  angular_damping=, gravity_scale=}` — type is `"static"` /
  `"dynamic"` (default) / `"kinematic"`; parented dynamics error.
- `self:add_collider{shape=, radius=, half_extents=,
  is_trigger=, friction=, restitution=}` — shape is `"box"`
  (default, 0.5 half extents) / `"sphere"` / `"capsule"`
  (radius + `capsule_half_height`, Phase 30).
- `Physics.gravity() -> x, y, z` /
  `Physics.set_gravity(x, y, z)` — per-world gravity.
- `Physics.raycast(ox, oy, oz, dx, dy, dz, max_dist[, mask,
  triggers]) -> hit | nil` — hit is `{object, point={x,y,z},
  normal={x,y,z}, distance}`.
- Collision callbacks (absent = no-op; errors mark failed):
  `collision_enter/stay/exit(self, other, contact)` and
  `trigger_enter/stay/exit(self, other, contact)` — `contact` is
  `{normal={x,y,z}, point={x,y,z}, penetration=n}` (A=self ->
  B=other sense), **nil for EXIT** events.

## Character + shape casts (Phase 30; `script_bind_character.c`,
thin over `le_character_*` / `le_physics_*cast`)

- `self:character_move(dx, dy, dz)` -> grounded;
  `self:character_is_grounded()`, `self:character_ground_normal()`,
  `self:character_ground_object()`, `self:character_velocity()`,
  `self:character_speed()`,
  `self:character_set_vertical_velocity(v)` /
  `self:character_vertical_velocity()`,
  `self:character_teleport(x, y, z)`,
  `self:character_gravity(dt)`.
- `Physics.sphere_cast/capsule_cast/box_cast(...)` -> hit table
  (`{object, fraction, distance, point, normal,
  started_overlapping, penetration}`) or nil;
  `Physics.set_collision_mode(self, "discrete"|"continuous")`.
- Full semantics in `LUA_CHARACTER_API.md`.

## Animation (Phase 29; `script_bind_anim.c`, thin over `le_anim_*`)

- `self:animation_play([clip[, restart]])` — nil clip keeps the
  current clip (continue); restart nonzero forces time 0.
- `self:animation_pause()` / `self:animation_resume()` /
  `self:animation_stop([reset])` — reset nonzero returns to
  bind pose + time 0.
- `self:animation_seek(t)` — clamped into `[0, duration]`.
- `self:animation_speed([s])` — no arg returns current speed.
- `self:animation_crossfade(clip, seconds)` — 0 = immediate.
- `self:animation_is_playing() -> bool`,
  `self:animation_time() -> number`,
  `self:animation_duration() -> number`.
- Clips are asset userdata via `Assets.find_by_id(hex)`; stale
  handles error. Mutating calls require dispatch context and a
  live animator (missing animator errors); queries on missing
  animators return false/0.

## Identity userdata model

Four userdata kinds: object (`le_object`: engine + world_tag +
index + generation), asset / scene (engine + index +
generation; scene is a tag-swapped asset), instance (plain table
`{serial, world_tag}` with metatable methods). Handles are
values: safe to store across frames; resolution is live-checked
per call.

## Equality + staleness errors

- `objA == objB`: engine + world_tag + index + generation all
  equal. Asset/scene `__eq`: engine + index + generation.
  Objects print as `Object(index:generation)`.
- Live reads (`le_obj_live`): stale handle errors
  (`"stale object handle"`). Mutations (`le_obj_mutable`):
  additionally require the firing world and a non-tearing-down
  world (`"cross-world object misuse"` /
  `"world is shutting down"`). `is_alive` / `World.is_alive`
  return false instead of erroring.

## __index / __newindex export sugar

`__index`: method table first, then declared-export read
(`self.speed`), else nil. `__newindex`: declared-export write
only (inline conversion, no reentry); anything else errors with
`"undeclared property (use export())"` — typos fail loudly,
never silently. Asset-typed exports push/check asset userdata.
