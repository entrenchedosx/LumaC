# Lua Character + Cast API (Phase 30)

Thin bindings over `le_character_*` and `le_physics_*cast` —
no Lua-side gameplay state; all queries return plain values.
AOT-compatible (same `le_*` calls from native code).

## Object methods (self)

- `self:character_move(dx, dy, dz)` -> grounded (bool). Sweeps
  the controller capsule; deterministic per fixed step.
- `self:character_is_grounded()` -> bool.
- `self:character_ground_normal()` -> x, y, z.
- `self:character_ground_object()` -> object or nil
  (generation-safe; nil when airborne/stale).
- `self:character_velocity()` -> x, y, z (last horizontal +
  vertical fall velocity along up).
- `self:character_speed()` -> horizontal speed scalar (drive
  walk/idle animation selection — Phase 29 + 30 compose here).
- `self:character_set_vertical_velocity(v)` (jump = positive).
- `self:character_vertical_velocity()` -> v.
- `self:character_teleport(x, y, z)` (clears ground + velocity).
- `self:character_gravity(dt)` -> grounded (integrate + move;
  call per fixed step).

All mutable methods require dispatch context (owning world,
live handle); stale/cross-world misuse errors like every other
binding.

## Global `Physics` queries

- `Physics.sphere_cast(cx,cy,cz, r, dx,dy,dz[, mask])`
  -> hit table or nil.
- `Physics.capsule_cast(cx,cy,cz, qx,qy,qz,qw, r, half,
  dx,dy,dz[, mask])` -> hit or nil.
- `Physics.box_cast(cx,cy,cz, qx,qy,qz,qw, hx,hy,hz,
  dx,dy,dz[, mask])` -> hit or nil.
- `Physics.set_collision_mode(self, "discrete"|"continuous")`
  (method-style with self; bad modes error).

Hit table: `{object, fraction, distance, point={x,y,z},
normal={x,y,z}, started_overlapping, penetration}`.

## Script state discipline

Mutable per-instance script fields must be declared with
`export()` (the runtime rejects undeclared `self.*` writes).
The walker pattern: `character_move` + `character_gravity` per
`fixed_update`, jump via `character_set_vertical_velocity`,
locomotion speed via `character_speed`.

## Files

`engine/src/script/script_bind_character.c`
(`le_lua_register_character_methods`,
`le_lua_register_cast_queries`); test
`engine/tests/test_character_script.c` (fixed WASD walker:
advances +X, deterministic across runs, grounded at end).
