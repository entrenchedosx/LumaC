# Scene Architecture (Phase 25)

## scene != world

- `le_scene` is SERIALIZED PROJECT CONTENT: object records with
  persistent IDs, hierarchy by persistent parent ID, canonical TRS,
  components, asset references by persistent asset ID. Never runtime
  pointers, never runtime indices. Stored as an `LE_ASSET_SCENE`
  payload on the engine (so scenes load by asset ID like everything
  else).
- `le_world` is RUNTIME STATE: `le_object` handles, cached matrices,
  live components.

```
Scene asset ──instantiate──▶ World objects (fresh handles)
World ──capture──▶ Scene asset (IDs preserved, deletions stick)
```

## Persistent scene object IDs

`le_scene_object_id` (128-bit, counter-seeded UUID, 32-hex). Minted
at capture for objects lacking mapping, preserved across
save/close/reload, stamped back onto instantiated slots so
re-capture keeps them. New post-instantiation objects mint fresh
IDs at next capture (collision: negligible by construction —
64-bit counter space per process + address fold).

## Records

`le_scene_object`: ID, parent ID + has_parent, name[128], enabled,
position/quaternion/scale, optional renderable (mesh/material
persistent IDs + shadow/visible flags), optional camera (full lens),
optional light (full params + shadow config), optional script
(Phase 26: script asset ID + up to 16 exported values).
Pointer-backed Phase 24
renderables are SKIPPED at capture (counted) — scenes never store
renderer pointers.

## Instantiation (transactional)

parse → validate (TRS, IDs) → resolve (parents by ID incl. forward
refs, assets to READY handles) → cycle-check → commit (create +
stamp IDs + names + TRS, then hierarchy, then components). ANY
failure leaves the world untouched (rollback destroys partial
objects). Missing/self parents, cycles, duplicate IDs, unready
assets all fail cleanly.

## Instances (prefab-ready)

`le_scene_instance`: owning scene handle, first-root handle,
parallel `object_ids[]`/`objects[]` mapping (for Lua/editor/
save-games), count. Two instantiations yield DISJOINT handle sets;
renderer temporal keys mix world salt + slot + generation, so LOD
history never collides across instances. Persistent local IDs may
later gain an instance namespace (documented, not implemented).

## Lookup

`le_scene_instance_lookup` maps persistent → runtime within an
instance (linear scan; instance sizes are small).

## Reparent modes

`LE_REPARENT_KEEP_LOCAL` (Phase 24 behavior) and
`LE_REPARENT_KEEP_WORLD` (new: `inverse(new_parent_world) ×
old_world` + TRS decomposition + commit-time verification
`parent × new_local ≈ old` or `LE_ERROR_UNREPRESENTABLE_TRANSFORM`).
Shear-inducing reparents fail honestly — never silent distortion.

## glTF bridge

`le_gltf_import` reuses `la_model_load` and ADOPTS mesh/material
resources into engine assets (new `la_model_adopt_*` API; model
teardown NULL-skips adopted slots). Dedup by canonical path.
Textures ride adopted materials via the engine-owned bridge manager
(long-lived; outlives the registry). Procedural texture assets are
first-class; glTF-image texture assets deferred (see asset doc).
