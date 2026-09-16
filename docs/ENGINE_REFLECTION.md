# Engine Reflection (Phase 31)

Reflection for the editor is **editor-side static tables over
validated engine APIs** — no engine internals, no RTTI, no stringly
code generation. Two small engine additions close the enumeration
gap; everything else composes existing getters.

## New engine APIs (append-only, Phase 31)

```c
le_result le_world_get_roots(le_world *world, le_object *out_roots,
                             uint32_t capacity, uint32_t *out_count);
uint32_t le_world_get_live_count(const le_world *world);
uint32_t le_world_get_all_objects(le_world *world,
                                  le_object *out_objects,
                                  uint32_t capacity);
void le_object_get_info2(const le_world *world,
                         const le_object *object,
                         le_object_info2 *out_info);
```

- `le_world_get_roots`: ascending-slot deterministic, counting query
  when `out_roots == NULL`, full count always in `out_count`.
- `le_world_get_all_objects`: ascending-slot census; counting query
  when `out_objects == NULL` (returns live count).
- `le_object_info2` (`LE_OBJECT_INFO2_VERSION == 1`): `version`,
  `alive`, `enabled`, `effectively_enabled`, `parent`+`has_parent`,
  `child_count`, `has_transform`, `has_renderable` (either spelling),
  `has_asset_renderable`, `has_camera`, `has_light`, `has_script` +
  `script_failed`, `has_rigid_body`, `has_collider`, `has_animator`,
  `has_character`, borrowed `name`. Stale-safe (zeros, version 0).
- `le_object_info` is unchanged (stable contract).

## Property vocabulary (stable paths)

```
object.name / object.enabled
transform.position / transform.rotation_quat
transform.rotation_euler_deg / transform.scale / transform.parent (ro)
renderable.visible / casts_shadow / receives_shadow / mesh_asset (ro)
camera.projection / fov_y_deg / ortho_height / aspect / near / far
light.type / color / intensity / range / spot_inner_deg / spot_outer_deg
rigidbody.type / mass / linear_damping / angular_damping /
  gravity_scale / linear_velocity / angular_velocity
collider.shape / radius / half_extents / capsule_radius /
  capsule_half_height / offset / is_trigger / layer / mask /
  friction / restitution
animator.autoplay / loop / speed / start_time / playing (ro) / time (ro)
character.radius / height / skin_width / max_slope_deg / step_height /
  gravity / terminal_velocity / snap_distance / push_strength /
  layer / mask
script.<export> (dynamic; type from le_script_get_property)
```

Types: `LED_DATA_BOOL/INT/UINT/FLOAT/VEC3/QUAT/EULER_DEG/STRING/ENUM/
ASSET_ID/COLOR3/UNAVAILABLE`. Static table entries carry `{path,
label, component, type, range?, enum labels?, read-only?}`; script
props synthesize descriptors from live metadata.

## Validation

Every write validates liveness (`le_object_is_alive`) + static type
+ range + finiteness before touching the engine; failures return
`LED_ERROR_*` without side effects. Enum writes bounds-check;
asset writes are rejected through this path (use commands). Reads of
absent components return 0 (presence query safe with NULL out).

## Euler policy (normative)

Storage is always quaternion (`le_object_set_rotation`, normalized
on store). The inspector presents ZYX-Euler degrees: pitch(X),
yaw(Y), roll(Z); canonical branch |pitch| ≤ 90°; gimbal lock yields
yaw-from-Y with roll 0. Writes convert deg→quat, normalize, store.
NaN/non-finite rejected pre-conversion. Round-trip oracles:
quat→euler→quat within 1e-5 (quat space); euler→quat→euler within
1e-2°. Verified by `test_editor_reflect` (90° pitch case).

## Asset-backed renderables

`renderable.mesh_asset` reports `LED_DATA_UNAVAILABLE`: persistent
asset IDs are not renderer pointers and are never guessed through
this path. Presence is still reflected (`has_asset_renderable`),
and full scene capture/instantiate round-trips them by ID.
