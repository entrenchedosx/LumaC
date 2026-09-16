# Prefab Architecture (Phase 32)

Reusable authored object-subtree assets (`.luprefab`,
`LUMA_PREFAB 1`): create from a live subtree, load through the
scene parser, instantiate through the scene commit path.

## Format (reuses the scene vocabulary — same writer discipline)

```
LUMA_PREFAB 1
prefab <uuid>              (prefab asset UUID, authoring identity)
object <local-uuid>
  name/enabled/parent/position/rotation/scale/renderable/
  camera/light/shadow/script/sprop/rigid_body/collider/
  animator/character/end   (same shapes as LUMA_SCENE 1)
prefab_root <local-uuid>
```

Rules: exactly one `prefab` line (first, after header), >= 1
object, exactly one `prefab_root` naming a known local ID
(after all objects), parent links resolve within the payload,
no second `prefab` line (nested prefabs deferred -> PARSE), no
`asset` table (resolution is purely by ID), duplicate local IDs
rejected, unknown components rejected (same fail-closed list as
scenes), unknown FIELDS tolerated. Canonical bytes: ID-sorted
records, fixed field order, `%.9g` (`%.17g` sprop doubles),
C-escapes, 32-hex IDs.

## Identity levels (three, never conflated)

```
prefab asset UUID  (which prefab — the `prefab` line)
prefab-local UUID  (which object within the prefab)
runtime le_object  (which live instance object — fresh handles
                    per instantiation, salted renderer keys)
```

Local IDs are fresh UUIDs minted at creation (stable across
save/load, distinct per prefab). Instantiation maps
prefab-local -> fresh scene IDs through the SAME commit path as
scenes (validate-first + rollback, staged asset resolution).

## Implementation (no second parser)

- **Create**: collect subtree (authoring snapshots; pointer-backed
  renderables refuse loudly — never silent data loss) -> resolve
  asset-sprop hexes while the engine is available -> emit
  canonical text -> transactional file write -> DB record adopt
  (UUID, fingerprint, importer, dep edges via the runtime-ID
  bridge) -> sidecar -> sort/reindex/view refresh.
- **Load**: structural line check (shape rules the scene parser
  cannot express) -> translate in memory to `LUMA_SCENE 1`
  (`prefab`/`prefab_root` lines drop, everything else verbatim)
  -> `le_scene_load_text` into a scratch scene asset (full
  fail-closed semantics) -> `le_asset_create_prefab` over the
  ORIGINAL canonical bytes. World untouched on failure.
- **Instantiate**: re-translate stored text -> scratch scene ->
  scratch-WORLD validation first (missing assets/hierarchy fail
  before the edit world is touched) -> commit into the edit
  world (fresh handles, ID stamping) -> editor-side instance
  record (prefab asset + project UUID bridged from the DB +
  local->runtime map; roots attach as scene roots).
- **Instance tracking** is editor-side only, never in gameplay
  components. Overrides deferred except instance-root TRS
  (natural). Nested prefabs deferred.

## Commands (all through `led_execute` history)

- `LED_CMD_INSTANTIATE_PREFAB`: undo destroys the whole
  instance (root cascade), redo re-instantiates. Instance roots
  resolve by census-diff (before/after newcomer with no parent)
  — never tail rules (slot recycling defeats them; regression
  covered: source object at a higher slot survives undo/redo).
- `LED_CMD_CREATE_PREFAB`: filesystem op — undo/redo are
  no-ops by design (documented split: no fake filesystem undo;
  explicit project-delete removes the file).
- `LED_CMD_ASSIGN_ASSET`: typed material/script assignment with
  before-images (material restores mesh+material IDs, script
  restores presence/handle). Type mismatches rejected.

## Isolation + policy

- N instances: disjoint handles, per-instance TRS/scripts/anim
  state, shared asset payloads. 10-instance and 1k-instance
  (0.025 s) proofs in `test_prefab`.
- Play: instantiate/create/assign rejected while playing;
  prefab-expanded capture keeps edit byte-identical across
  play (`test_project_editor`).
- Malformed battery (8 shapes): PARSE/VALIDATION, world kept.
- Known deferred namespace caveat (from Phase 31, applies to
  prefab instances equally): two instantiations stamp the same
  local IDs; a later whole-world capture emits duplicate IDs.
  Instance-aware capture namespacing is future work.
