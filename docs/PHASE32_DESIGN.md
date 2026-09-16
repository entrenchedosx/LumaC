# Phase 32 — Project System, Asset Browser, Import/Reimport & Prefabs: Design

Status: design (implementation follows). Freeze: Phase 31 committed as
`6cad41c` (HEAD == origin/main, clean tree), full Debug 81/81 PASS
(69.78 s), representative editor suites 6/6 PASS. Phase 32 starts clean.

## 1. Survey synthesis (what exists)

- Asset registry (`engine/src/asset.c`, `luma_engine.h:1124-1510`):
  `le_asset{index,generation}` runtime handles, `le_asset_id{hi,lo}`
  persistent IDs (FNV-1a content hash for imports/scripts, UUID for
  procedurals), normalized source locator (NOT identity,
  `le_normalize_source` lexical: `/` folding, `.`/`..`, drive roots,
  above-root clamps). Types MESH/MATERIAL/TEXTURE/SCENE/SCRIPT/
  SKELETON/CLIP (COUNT=7). Unload-while-referenced → ASSET_IN_USE.
  Extraction resolves handles per frame, skips non-READY (never
  dangles). `le_script_reload` + `le_script_asset_set_source` exist
  (transactional recompile; instances adopt code, keep values, no
  re-start). NO generic reload API for mesh/material/texture.
- glTF bridge (`engine/src/gltf_bridge.c`): dedup by canonical path;
  mesh IDs `{fnv(src), (mi<<32|pi)^fnv}` — STABLE under reorder.
  Material IDs = factor-hash — REORDER-FRAGILE (same factors at a new
  index collide; the `gltf_bridge.c:276-299` comment admits the
  positional fallback). Node→material mapping is positional
  (`matidx < nmat` → `material_asset = matidx`). Animated import is
  index-based (joints/channels by file order).
- Assets layer: `la_model_load` (.glb/.gltf), stb_image PNG/JPEG→RGBA8
  (+ Radiance .hdr float path), textures manager-cached, adopt
  mesh/material out of models. Renderer has NO texture API of its own
  (LumaC `lc_image/view/sampler` borrowed by materials).
- Serialization (`engine/src/serialize.c`, 2752 lines): `LUMA_SCENE 1`
  header, per-line records (`object/name/enabled/parent/position/
  rotation/scale/renderable/camera/light/shadow/script/sprop×6/
  rigid_body/collider×3/animator/character/end`), `%.9g` floats
  (`%.17g` for sprop doubles, read back float-precise), C-escape
  names, 32-hex IDs, asset table (mesh/material ONLY). Unknown
  fields tolerated, unknown components/versions rejected.
  Transactional load (stages temp, swaps on success).
- Capture (`scene.c:262`): WHOLE-WORLD only, ascending slots, wholesale
  replace (OOM-partial caveat). Instantiate (`scene.c:493`): append,
  fresh handles, ID stamping, validate-first + rollback, fixed
  component order, empty = no-op. Instance record is "prefab-ready"
  but same-scene-twice-then-capture yields DUPLICATE IDs (instance
  namespace documented-not-implemented).
- Editor (`luma_editor.h`, 668 lines): session/selection/reflection/
  hierarchy/inspector/18 command kinds/history/scene-IO/play/viewport/
  gizmo/console/shortcuts + `led_project_save/load_sidecar` (editor
  state only, NOT a project system). No `LE_ASSET_PREFAB`, no project,
  no asset DB, no importers (grep-verified zero).

## 2. Architecture decisions

### D1. Project system lives in `editor/` (new `project/` sources, same
### `luma_editor` lib)
Rationale: spec §3 prefers editor/project tooling ownership over
contaminating LumaC/renderer. The engine gains exactly ONE appended
asset type (`LE_ASSET_PREFAB = 7`, COUNT=8) so prefab payloads ride the
registry (dependents-first teardown, handle discipline) — everything
else (manifest, paths, DB, importers, browser, prefab logic) is
editor-side over public APIs.

### D2. SIDECAR metadata (`<source>.luma`), not a central DB file
Rationale (§14): rename/move = filesystem move of source + sidecar
preserves identity with zero DB surgery; copy = new identity unless
the sidecar is explicitly preserved (then conflict state); corrupt
one sidecar degrades one asset, never the whole project. Sidecar
stores: project asset UUID, importer ID + version, settings digest,
source fingerprint (size + FNV-1a-64 of bytes), sub-asset key table
(glTF), dependency IDs, diagnostics. Cache stays rebuildable under
`.luma/` (never beside source except the sidecar).

### D3. Identity stack (four distinct concepts, never conflated)
- **Project asset ID**: counter-seeded UUID minted at discovery,
  stored in the sidecar. Survives rename/move (sidecar travels),
  reimport (bytes change, UUID stable), cache delete. Two copies of
  identical bytes get DISTINCT IDs (authoring identity ≠ content).
- **Content fingerprint**: `{size, fnv1a64(bytes)}` — change
  detection only, never identity.
- **Runtime `le_asset` handle**: engine registry; reimport creates a
  NEW generation/handle (old handle detectably stale), project ID
  stable. Documents §32 as new-handle policy.
- **glTF sub-asset keys**: `kind + stable source token`:
  meshes `mesh<m>:prim<p>` (matches existing stable mesh IDs);
  materials `mat<m>:<sanitized-name>` with index fallback
  (FIXES the positional debt: name-keyed, factor-hash retired —
  §24); textures `tex<t>:<sanitized-name>`; skeletons
  `skin<s>`; clips `clip<a>:<sanitized-name>`. Reordered
  enumeration with equivalent semantics → same keys → same IDs.
- Scene files keep storing `le_asset_id` hex (unchanged wire
  format); the project DB bridges project-ID ↔ `le_asset_id` ↔
  runtime handle (§162).

### D4. Importers: registry, not a switch
`led_importer{ id ("luma.gltf"), version, extensions[], import(),
settings digests }` table, dispatch by lowercase extension:
`.glb/.gltf → luma.gltf` (reuses `le_gltf_import` +
`le_gltf_import_animated`, NO editor parser), `.lua → luma.lua`
(`le_asset_create_script`/`le_asset_load_script`), `.luma_scene →
luma.scene` (discovery only — engine loads), `.png/.jpg/.jpeg →
luma.texture` (stb via `le_asset_create_texture`; EXACT supported
set reported, nothing claimed beyond it), `.luprefab → luma.prefab`.
Unknown extensions = IGNORED with `UNIMPORTED` status (never a
failure). Importer version + settings digest join the cache key and
staleness check (§19, §60).

### D5. Transactional reimport = candidate-then-swap
Import into CANDIDATE runtime assets (fresh handles), validate, then
atomically repoint the project record (project ID unchanged, new
runtime handle published, old handle unloaded when unreferenced or
left stale-detectable). Failure → record keeps old handle + `FAILED`
diagnostics, last-known-good live (§30/§189). Scripts reuse the
existing `le_script_reload` path where the handle can be preserved;
mesh/material/texture take the new-handle path (§32 documented).

### D6. Prefab = `LE_ASSET_PREFAB` + `.luprefab` text reusing the scene
### line vocabulary
`LUMA_PREFAB 1` header + `prefab <uuid>` + the SAME object-record
lines as scenes (fail-closed components, tolerant fields, `%.9g`,
escapes, hex IDs) + `prefab_root <localid>` + local-ID remap table.
Prefab-local IDs are fresh UUIDs minted at creation (stable across
save/load, distinct from scene IDs). Instantiation: parse+validate
transactionally → map prefab-local → fresh scene IDs → instantiate
through the SAME commit path as scenes (fresh runtime handles,
per-instance renderer keys). Instance tracking is editor-side
(prefab asset + root handle + local→runtime map), never in gameplay
components. Overrides DEFERRED except instance-root TRS (natural).
Nested prefabs DEFERRED (a prefab record referencing another prefab
is rejected with diagnostics).

### D7. Filesystem ops are project operations, NOT scene-undo
Rename/move/delete go through `led_project_*` APIs with reference
checking (scene/prefab reference scan by persistent ID); scene
mutations (instantiate/drop/assign/attach) go through Phase-31
`led_execute` history. Documented split (§52, §129). No fake
filesystem undo. No watcher (explicit Refresh/Reimport/Reimport-All).

### D8. Play policy: conservative
Reimport + prefab authoring ops are rejected while playing
(`LED_ERROR_ALREADY_PLAYING`, matching scene-new/open/revert).
Play content = prefab-expanded scene capture (no prefab awareness
in the runtime).

## 3. New surfaces

### 3.1 Engine (append-only)
- `LE_ASSET_PREFAB = 7`, `LE_ASSET_COUNT = 8` + payload slot
  (`prefab_text` bytes, engine-agnostic) + `le_asset_create_prefab`
  / `le_asset_get_prefab_text` accessors. No other engine changes.

### 3.2 Editor: project (`led_project_*`, new `project/` sources)
- Open/close/switch (singleton per session, §95), manifest parse/
  save/validate, path canon (`led_project_normalize`,
  `led_project_resolve` with escape rejection), explicit
  `led_project_scan` (incremental: fingerprint compare, no reimport
  when unchanged), rename/move/delete with ref-check, startup scene
  resolve, stats.
- Manifest `luma.project`: `name`, `format_version (=1)`,
  `startup_scene` (project-asset UUID or path, resolved via DB),
  `asset_roots` (default `Assets/`, `Scenes/`), window defaults
  (w/h/title, optional). Unknown future versions fail loudly.
  Hand parser (reuse the line discipline, no new dependency).

### 3.3 Editor: asset DB (`led_assetdb_*`)
- Records `{project_id UUID, type, source_path rel, status
  (UNIMPORTED/READY/STALE/FAILED/MISSING/UNSUPPORTED), fingerprint,
  importer id+version, settings digest, sub-asset table, deps[],
  diagnostics, runtime handle}`.
- Indexed by UUID (hash) + normalized path (hash); deterministic
  enumeration (sorted); count/enumerate/lookup-by-ID/lookup-by-path/
  filter-by-type/search (name/path/type, case policy documented)/
  sort; duplicate-UUID conflict state (both marked, neither silently
  picked); malformed sidecar → per-record diagnostic, scan continues.
- Scan policy: ignore `.git`, `build*`, `.luma`, `*.luma` outputs;
  no directory-symlink following outside root; case-collision
  detection; long/UTF-8 paths via dynamic buffers.

### 3.4 Editor: import (`led_import_*`)
- Importer registry + per-type import over existing pipelines;
  explicit queue `{pending,running,completed,failed}` executed
  synchronously (worker-ready shape); progress counters, no fake
  percents; diagnostics feed `led_console_push` (no second logger).
- Staleness: fingerprint change OR settings change OR importer
  version change → STALE. Reimport: candidate-then-swap (§D5).
  Dependencies: scene→asset, prefab→asset, material→texture DAG
  with cycle detection; dependent invalidation marks STALE.
- Lazy runtime policy: scan/discovery never uploads GPU resources;
  import creates runtime assets on demand (browser metadata ≠ GPU).

### 3.5 Editor: browser (`led_browser_*`)
- Headless model over the DB: folder tree, asset list, filter,
  search, sort, selection BY PROJECT UUID (never row index).
- Drag payload `{project_id, type}` (never raw paths): model→scene
  = CREATE + asset-renderable assign via `led_execute`; material→
  renderable via property/command with type check; script→object =
  add-script command; scene = open (never accidental instantiate).
- Asset inspector: ID/type/path/status/importer/deps/diagnostics
  rows through the headless inspector shape.

### 3.6 Editor: prefabs (`led_prefab_*` + 3 command kinds)
- `led_prefab_create` (subtree → local IDs → transactional write),
  `led_prefab_load/save` (canonical bytes stable across
  load→save→load→save), `led_prefab_instantiate` (fresh scene IDs +
  engine instantiate; returns local→runtime map + root).
- Commands `LED_CMD_INSTANTIATE_PREFAB` (undo removes whole
  instance, redo restores coherent identity), `LED_CMD_CREATE_PREFAB`
  (writes asset; undo unlinks), `LED_CMD_ASSIGN_ASSET` (typed
  material/script assignment with type check). All through
  `led_execute` history.
- Validation: duplicate local IDs, missing root, cycles, bad
  parents, bad components, missing required refs → reject
  transactionally. Malformed fuzz: no crash/UAF/loop.

## 4. Test plan (6 suites, all headless)
- `test_project` (~120): new/open/close/switch, manifest parse/
  version/escape/corrupt, path normalization matrix (/, \, ., ..,
  dup seps, absolute, UTF-8, long), escape attacks, startup scene,
  validation errors, copy/move portability.
- `test_asset_database` (~130): enumeration determinism, ID/path
  lookup (hash), filter/search/sort, duplicate-ID conflict, missing/
  restored/rename/move, unknown files ignored, hidden/build-dir
  ignore, case collision, 100k-record stress (lookup/search/filter/
  sort timings + memory at 1k/10k/100k).
- `test_import` (~120): initial import (glb+lua+png+scene), unchanged
  rescan (no reimport), source-changed, settings-changed, importer-
  version-changed, failed import preserves good, dependency
  invalidation + cycle detection, reload generation safety
  (old handle stale), missing/unload semantics.
- `test_gltf_identity` (~40): controlled fixture — import, record
  sub-asset IDs, reimport equivalent source (IDs stable), reorder
  non-semantic enumeration (name-keyed materials stable — kills the
  positional debt), texture/skeleton/clip keys.
- `test_prefab` (~140): create/load/save/instantiate, canonical
  determinism, multi-instance (1/10/100), destroy + slot reuse,
  malformed battery, duplicate IDs, scene round-trip, missing-asset
  diagnostics, 10k-object prefab (load/instantiate/destroy timed,
  non-blocking), 1k-instance stress.
- `test_project_editor` (~120): browser model (tree/list/filter/
  search/sort/selection-by-UUID), drag payloads (model/material/
  script/scene), assign type-safety, prefab instantiate/undo/redo,
  delete-with-refs, play isolation with prefabs (edit unchanged),
  player-prefab integration (character+script+animator+renderable ×
  2 instances: handles differ, script/anim state isolated, assets
  shared), end-to-end headless workflow (project→import→
  instantiate→prefab→save→play→stop), reimport E2E (broken source
  → good survives), portability E2E (copy/move project).

## 5. Proofs & matrices
- Portability: copy + move whole project, reopen at a different
  absolute path, resolve + load scene + instantiate prefab.
- ASan/UBSan (Linux) over project/DB/import/prefab/integration;
  TSan on CPU suites (sync paths stay single-threaded, no safety
  claimed); Vulkan validation on an imported+prefab scene render
  (zero ERROR/VUID); Windows Debug/Release static + shared,
  Linux compile + headless + Vulkan-where-possible.
- Fault injection where infra exists (open/scan/growth/parse/
  import/instantiate paths); malformed fuzz (manifest/sidecar/
  prefab); UTF-8/long/Windows/Linux path batteries; cache-delete
  rebuild; sidecar-delete re-identity; duplicated-source distinct
  identity.

## 6. Docs to write
`PROJECT_ARCHITECTURE.md`, `ASSET_DATABASE.md`,
`ASSET_IMPORT_PIPELINE.md`, `ASSET_REIMPORT.md`,
`PREFAB_ARCHITECTURE.md`, `PROJECT_PATHS.md` (+ identity diagram,
reimport-transaction diagram, prefab-identity levels, human+AI
tooling diagram, deferred-features list). Update `EDITOR_*
(4 files)`, `SCENE/SERIALIZATION/ENGINE/ARCHITECTURE`,
`API_DESIGN`, `README`, `CHANGELOG`.
