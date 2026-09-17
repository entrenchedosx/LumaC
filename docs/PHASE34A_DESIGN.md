# Phase 34A Design — Trust Closure, Real GUI Automation & Portable Asset Identity

Status: DESIGN (this doc is the plan; code follows it).
Baseline: `16afba6`, clean tree, Debug 92/92 green.

## 1. Portable identity — root cause (verified in source)

`le_gltf_import(e, abs, ...)` (`engine/src/gltf_bridge.c:54`) mints
mesh IDs as `fnv(abs_normalized)` / `((mi<<32)|pi)^fnv(abs)` (:539-545)
and material IDs as `fnv(factors)+fnv(abs)+i*salt` (:385-416). The
project layer passes the root-joined ABSOLUTE path
(`editor/src/project/import_impl.c:532`). Scene files persist those
IDs (`renderable <hex> <hex>`, `engine/src/serialize.c:674`), and
`le_scene_instantiate` resolves them by exact ID match
(`engine/src/scene.c:606-615`, MISSING_ASSET → LED_ERROR_PARSE at
`editor/src/editor_scene.c:128`). New absolute root ⇒ new IDs ⇒ saved
scenes orphaned. Scripts embed `fnv(abs_path)` too
(`engine/src/script/script_asset.c:144-145`). Skeleton/clip IDs are
fresh UUIDs per import (`engine/src/animation/anim_api.c:34,65`) —
stable nowhere. `led_sub_id_for_key` (import_impl.c:220) is dead code;
the sub table republishes live engine IDs.

## 2. Identity model (after)

Persistent vs runtime, never conflated:

- **Project UUID** (`led_project_asset_id`, sidecar `id`): authoring
  identity. Minted once at discovery, travels in the sidecar.
  UNCHANGED semantics (distinct files = distinct assets even with
  identical bytes).
- **Source locator**: canonical PROJECT-RELATIVE path (`source` line,
  `/`-separated, `.`/`..` resolved, escapes rejected). Locator, not
  identity. Case/separator/dot-segment normalized by the existing
  `led_project_normalize` (backslash fold, collapse, resolve).
- **Sub-asset key** (new, stable vocabulary): per model source,
  `mesh<mi>:prim<pi>`, `mat<mi>[:<sanitized-name>]`,
  `skin<si>[:<name>]`, `clip<ai>[:<name>]`, `tex<ti>[:<name>]`, where
  names come from new `la_model` name queries (mesh/material/skin/
  animation/image names; NULL/empty/dup → index-only key, documented).
  Persisted in sidecar `sub <key> <engine-hex>` lines.
- **Engine persistent ID** (`le_asset_id`): runtime+file identity.
  Derived as `FNV(project-UUID-bytes || sub-key)` — location-free,
  stable across relocation/reimport/rename-moves. Scene/prefab files
  store THESE hexes (format unchanged, values change).
- **Runtime handle** (`le_asset`): generational, never persisted
  (unchanged prohibition).

Migration: sidecars/prefabs/scenes written by old builds carry
location-derived hexes. Bump a marker: sidecar gains
`idmodel luma-portable-1` line; glTF importer version 1→2 (stale ⇒
reimport on next scan, which rewrites sub/engine hexes AND rewrites
dependent scene/prefab refs... — NO. Rewriting scene bytes silently
is forbidden. Instead: scene/prefab LOAD gains a project-aware
remap: `led_scene_open` resolves stale refs through the DB
(project-UUID + sub-key → current engine ID) BEFORE engine parse.
Concretely: on MISSING_ASSET-shaped failure OR proactively, the
project layer rewrites the in-memory text's hexes via the sub-key
map, then loads. Old scenes open after reimport without byte
rewrites. New saves persist portable hexes directly.)

Hmm — engine `le_scene_load_text` is project-blind. The remap must
live in the editor: `led_scene_open` (and prefab instantiate) first
ensure the referenced model records are imported (import-on-demand
through the existing lazy path), build map old-hex→new-hex from
sidecar `sub` lines, patch a COPY of the file text, load the copy.
Deterministic, no format change, old projects open.

Wait — old hexes aren't in the sidecar (sidecar has only CURRENT
sub hexes). Old scene refs match old engine IDs which are gone. The
remap key must be (project-UUID of model? scenes don't store that
either — they store bare engine hexes). Dead end for automatic
remap of OLD scenes without extra data... UNLESS: old scene `asset`
hint lines carry the absolute source path (serialize.c:561-575).
Remap: hint path → relativize against old root? Old root unknown at
new location... but hint basename + DB scan can match: for each hint
(mesh-hex, path), find DB record whose source_path basename chain
matches the hint's trailing segments, then map mesh-hex→that
record's current sub-ID by ORDER (hint table is hex-sorted, sub
table order = adoption order = model order — fragile).

DECISION (honest, minimal, no silent corruption): NO automatic
migration of old scene bytes. Old location-derived scenes are
declared LEGACY: `led_scene_open` detects unresolvable refs AND a
legacy marker (missing `idmodel` in referenced sidecars... simpler:
refs unresolvable + sidecars present + sources present ⇒ report
LED_ERROR_VALIDATION with diagnostic "legacy location-bound scene:
reimport assets and reassign (see docs/PORTABLE_IDENTITY.md)").
Hmm, but that leaves the Phase33V fixture + user scenes broken...

Better: make the ENGINE IDs themselves recoverable: new ID scheme
`FNV(project-UUID || sub-key)` requires the engine to know the
project UUID at import. `le_gltf_import` signature keeps `path`
(file access) + new `le_gltf_import_with_key(engine, path,
key_bytes, key_len, out)` where key = project-UUID bytes + rel
locator. Project layer passes UUID+rel. Repo root / non-project
callers (tests, engine_scene?) pass NULL key ⇒ legacy abs-path
scheme (documented, unchanged behavior). Scene files then carry
UUID-derived hexes: relocation-proof AND rename-proof (UUID stable)
AND content-independent (identical bytes = distinct assets preserved
via distinct UUIDs). Reimport stability: same UUID + same sub-key ⇒
same engine ID ⇒ scene refs resolve WITHOUT scene rewrite. Rename/
move within project: source_path changes but UUID doesn't ⇒ IDs
stable ⇒ scenes keep resolving (fixes a second latent bug: today
rename breaks scenes too). Failed reimport: candidate-then-swap
unchanged; IDs recomputed identically ⇒ stable.

Legacy scenes (old hexes): provide a ONE-SHOT, EXPLICIT repair tool:
`led_scene_repair_legacy_refs(session, text)`: parse hint lines
(path + hex), match hints to current DB records by trailing-path
+ sub-order, rewrite renderable hexes, report count. Wired to a
console command / test helper — never automatic. Phase33V fixture:
regenerate via builder (it's a fixture generator, honest path).

Skeletons/clips: `le_gltf_import_animated` gains key params too
(skeleton/clip IDs = FNV(UUID||skin-key)/FNV(UUID||clip-key));
falls back to UUID when no key (public API compat).

Scripts: `le_asset_create_script` ID = FNV(content)||FNV(norm-path)
— path part breaks relocation. Change: project layer loads scripts
by rel path; engine keeps path_hint for dedup/file access (source
string stays normalized-abs — runtime-only, never persisted... wait,
scene files persist script hexes too). New scheme: script ID =
FNV(content) || FNV(project-key || rel)? Engine is project-blind;
add `path_id_hint` to `le_script_asset_desc`: when set, lo =
FNV(hint) ^ (hi|1) instead of norm-path hash. Project passes
UUID+rel as hint. set_source keeps content hash + existing source
string. NULL hint ⇒ legacy behavior.

Textures: no persistent ID in project flow today (deferred upload).
Out of scope for IDs; document (relocation-safe trivially — nothing
persisted). Prefab/scene/procedural UUIDs already location-free.

Chapter order fix: material IDs currently hash FACTORS (reorder =
new IDs AND identical factors in two models collide modulo src).
New: mat key = `mat<mi>[:name]` ⇒ reorder-robust for named mats,
index-stable otherwise. Factor-only collisions across DIFFERENT
models now impossible (UUID differs). Within one model, two
identical materials share... different mi ⇒ different keys ⇒
distinct IDs (dedup loss, correctness win — matches mesh policy).

## 3. Real input harness

- `lc_window_inject_event(lc_window*, const lc_window_event*)`
  (public, lumac.h + window.c): forwards to `lc_window_push_event`
  (the exact function the Win32/X11 backends call). Same queue,
  same drain (`leg_frame_begin`), same FOCUS_LOST branch. Documents
  itself as the test/automation path.
- Observation (NOT operation): new `leg_probe_*` queries in the
  GUI layer over the public ABI:
  `leg_probe_viewport_rect` (origin+size of `##vp-capture`),
  `leg_probe_tool_rect("##tb-play"|...)` (last GetItemRect),
  `leg_probe_gizmo_handle(int mode,int axis)` (projected handle px,
  mirrors `leg_world_to_panel` math),
  `leg_probe_end_frame_snapshot` (button rect table + viewport
  rect, refreshed each panels frame into context-owned plain data).
  Tests send mouse/keyboard via inject; probes only OBSERVE.
- Driver: new headed binary `test_editor_headed` (Vulkan-gated):
  real window + device + swapchain + renderer + engine + session +
  GUI context; per step: inject events → N `leg_frame_begin`/
  `leg_panels_frame`/`leg_frame_end` frames → assert `led_*` state
  → screenshot via offscreen pass (same discipline as
  test_editor_gui_gpu + app screenshot path). Each widget step
  followed by a mutation run (break callback → test MUST fail).

## 4. Play wiring (kill the stub)

GUI exposes key identity: `leg_consume_play_input(ctx, out_keys)`
  (public ABI): after panels frame, reports currently-down
  gameplay keys (WASD/arrows/space + mouse deltas/wheel) WHEN
  playing AND viewport hovered AND NOT `leg_wants_keyboard` —
  the exact policy the stub's comment demands. App calls it then
  `le_input_inject_*` before `led_play_tick` (le_engine_step folds
  pending WITHOUT the OS pump — deterministic, same state
  machine). No test backdoor: headed test presses keys via inject,
  reads Lua-observed movement. Focus loss: FOCUS_LOST clears ImGui
  keys (exists) + engine clears held (exists, input.c:311-323) —
  test both halves live.

## 5. Headed proof matrix (each: inject → frames → state assert → shot)

Play/Stop/Undo(menu+Ctrl+Z)/Redo/Save(File menu)/pick A-B-empty/
renderable-without-collider (document honestly: physics-only pick
⇒ extend? NO — §14 says document if unsupported; picking needs
colliders, fixture crates get colliders, one collider-less mesh
proves miss+documents)/translate/rotate/scale drags (20-move,
ONE undo entry, camera-excluded)/ESC-cancel (exists in overlay —
verify!)/camera orbit+WASD+wheel+F/ownership matrix/input
routing matrix/focus-loss/asset drag (MODEL via LUMA_ASSET payload
— real ImGui DnD needs BeginDragDropSource... synthetic payload
via io? HONEST LIMIT: ImGui DnD payloads can't be synthesized via
lc events; drive drop via `AcceptDragDropPayload` requires a real
source frame. Option: two-phase — mouse down on asset row, N
move frames to viewport, up: real ImGui DnD path end-to-end (it
IS the production path; the injected events are OS-level). Verify
it works; if ImGui drops it, report NOT VERIFIED honestly.)/
prefab ×2 independence/runtime key (Lua Input.key_down(W) moves
object; key up stops)/composite proof (deterministic scene,
viewport-target readback, region assertions + clear-only mutation)/
100k DB (synthetic records + headed browser search/sort/frame)/
DPI (env: report; code: plumb real scale into
DisplayFramebufferScale via DPI probe or document NOT VERIFIED).

## 6. Docs to write/update

- docs/PORTABLE_IDENTITY.md (identity model §30-35, locator rules
  §46-51, migration/legacy §53, no-handle-persistence §54)
- docs/PHASE33_VERIFICATION_AUDIT.md §17 → resolved entry
- docs/PHASE34A_REPORT.md (the §77 template, verbatim sections)
- docs/verification/phase34a/ (16 shots)
