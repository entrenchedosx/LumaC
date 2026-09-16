# Changelog

## Phase 32

- Added the Luma Project system (`editor/src/project/`, same
  `luma_editor` lib): portable roots (`luma.project` manifest,
  lexical normalize + escape-rejecting resolve, no CWD
  dependence), sidecar asset database (`<source>.luma`: stable
  project UUIDs across rename/move/reimport/close-reopen, UUID +
  path hash indexes, deterministic path-sorted enumeration,
  case-insensitive search, duplicate-UUID conflict marking,
  corrupt-sidecar isolation, explicit incremental scan with
  STALE/MISSING/restored states), importer registry
  (`luma.gltf/lua/scene/prefab/texture` over existing runtime
  pipelines; unknown extensions ignored; name-keyed glTF
  sub-asset tables with full-table publish on reimport),
  transactional candidate-then-swap reimport (script
  handle-preserving path + generic new-handle path; broken
  source keeps last-known-good live), headless browser model
  (folder tree, filter/search/sort, selection by UUID, drag
  payloads, drops through undoable commands), project
  rename/move/delete (UUID-preserving moves with sidecar
  rewrite; ref-checked delete with DB-edge + hex text scan, no
  force-delete), prefab foundation (`.luprefab` reusing the
  scene vocabulary + scene parser + scene commit path; three
  identity levels; editor-side instance tracking; three new
  undoable command kinds with census-diff instance roots and
  conservative play rejection). One engine addition:
  `LE_ASSET_PREFAB` + `le_asset_create_prefab`/
  `le_asset_get_prefab_text` (opaque payload ownership).
- Fixed along the way: scan sweep-bit clearing fresh/restored
  records to MISSING; rename leaving a stale sidecar `source`
  line; prefab payloads written in text mode (CRLF drift —
  binary now, loads tolerate CR); tail-rule instance-root
  resolution destroying the wrong subtree (census-diff).
- Tests (7 new suites, all headless): `test_prefab_smoke` (39),
  `test_project` (43: paths/portability/identity/switch),
  `test_import` (60: import/reimport-txn/browser/delete
  ref-check), `test_assetdb` (35: determinism/conflicts/
  isolation), `test_project_editor` (40: drops/assign/play/E2E),
  `test_gltf_identity` (21, Vulkan-gated: stable identical
  reimport + sidecar round-trip), `test_prefab` (51:
  determinism/isolation/8-malformed/1k-stress/registry-vs-
  project identity).
- Docs: `docs/{PROJECT_ARCHITECTURE,ASSET_DATABASE,
  ASSET_IMPORT_PIPELINE,ASSET_REIMPORT,PREFAB_ARCHITECTURE,
  PROJECT_PATHS,PHASE32_DESIGN}.md`; updated
  `EDITOR_COMMANDS.md`, `README.md`, `CHANGELOG.md`.
- Deferred (tracked, not hidden): engine material-ID
  reorder-fragility (factor-hash debt); texture GPU upload
  laziness; double-instantiate capture namespace; nested
  prefabs; prefab overrides; 100k-record stress timings;
  force-delete.

## Phase 31

- Added Luma Editor foundation (`editor/`, `led_*`, static C11
  library on public engine APIs only; `editor → engine → assets →
  renderer → LumaC`, configure-time backend/Lua-confinement audit):
  headless `EditorCore` (session attach/detach/tick/stats,
  liveness-filtered selection with stale/slot-reuse pruning,
  subtree + by-name select), static reflection tables
  (describe/list/find/read/write over validated engine getters,
  ZYX-Euler-degree inspector mirror over quaternion storage,
  script exports enumerated live), hierarchy + inspector models,
  undoable commands (18 kinds incl. subtree-snapshot delete,
  validate-then-apply, bounded history with eviction stats +
  TRS-drag coalescing), dirty-tracked scene new/open/save/revert
  (transactional open, failed save keeps dirty, canonical
  save→load→save oracle), play-mode isolation (capture edit →
  instantiate runtime fork → step runtime only → destroy on exit;
  edit byte-identical oracle; runtime-only script-stepping proof),
  orbit viewport (unproject round-trip, physics picking, AABB
  compose/frame, math-only gizmo intents + line-soup preview),
  console ring + script-error mirror, shortcut table + focus
  policy, versioned project sidecar. No GUI framework, no editor
  renderer submission (Phase 32 work explicitly deferred).
- Added append-only engine enumeration APIs: `le_world_get_roots`
  (ascending-slot root listing), `le_world_get_all_objects` +
  `le_world_get_live_count` (bulk census), `le_object_info2` v1
  (full presence bits incl. script/physics/animator/character +
  asset-spelling flag + script-failed; `le_object_info` unchanged).
- Tests: `test_enumeration` (55), `test_editor_core` (136),
  `test_editor_history` (145), `test_editor_play` (121),
  `test_editor_viewport` (69), `test_editor_reflect` (101);
  10k-command undo/redo stress (byte-exact), 100k-object
  enumeration (reported), ASan/UBSan clean (one real
  strncpy-overlap fix in scene save), Linux + shared matrices.
- Docs: `docs/{EDITOR_ARCHITECTURE,ENGINE_REFLECTION,
  EDITOR_COMMANDS,EDITOR_UNDO_REDO,EDITOR_PLAY_MODE,
  EDITOR_VIEWPORT,PHASE31_DESIGN}.md`; updated `ARCHITECTURE.md`,
  `ENGINE_ARCHITECTURE.md`, `API_DESIGN.md`, `FRAME_LIFECYCLE.md`,
  `README.md`.

## Phase 27

- Added engine-owned input (per `le_engine`, worlds share one
  finalized snapshot): backend-neutral `le_key` (A–Z, 0–9,
  control, modifiers L/R, arrows, nav, F1–F12, numpad,
  punctuation), mouse buttons/position/delta/wheel, per-frame
  pressed/released edges (auto-repeat never edges; same-frame
  press+release yields both edges, held clear), UTF-8 text queue,
  focus-loss held-state clearing, cursor-mode requests
  (best-effort), injection feeding the SAME pending list as
  platform events (`le_input_inject_*`).
- Added LumaC event queue (`lc_keycode`, `lc_window_event`,
  `lc_window_read_event`/`drain`/`pending`, bounded ring): Win32
  (VK/scan translation, repeat bit, WM_CHAR UTF-8, XBUTTON,
  wheel H+V, focus) + X11 (KeySym map, buttons 4–7 as wheel,
  motion, FocusIn/Out, widened masks) backends translate; engine
  drains attached windows (`le_engine_attach/detach_window`).
- Added actions (FNV-1a names, multi-binding aggregates with
  aggregate edges), axes (digital cancel-to-0, analog
  deadzone/scale/invert, trigger handling), contexts (priority,
  consume masks), full rebind/query APIs; input-map disk format
  deferred (data model documented).
- Added engine-owned time (`lc_clock_now` ns monotonic source;
  explicit-delta test path, same state machine): scaled/unscaled
  delta + elapsed (double), uint64 frame index, validated scale
  (NaN/Inf/negative rejected), max-delta clamp (raw visible),
  fixed_delta schedule, pause (dt 0, fixed stops, frame+unscaled
  run), single-step, first-frame 0, suspend clamping.
- Added explicit frame lifecycle (`begin_frame`/`update`/
  `end_frame`/`frame`/`step`; host owns the loop, no
  run-forever); quit requests (never `exit()`); resize/minimize
  observation; per-world pause; fixed-step ownership moved to
  the engine schedule (world fields mirrored;
  `le_world_update` legacy contract preserved via the
  `le_world_simulate_engine` seam for paused dt=0 dispatch).
- Added `Input`/`Time`/`Key`/`Mouse` Lua bindings (thin over C;
  no Lua-side state): update dt == `Time.delta()`,
  fixed_update dt == `Time.fixed_delta()` (tested); same-frame
  snapshot shared across scripts.
- Added `examples/lua_input` (WASD + mouse-look camera rig, jump
  action, Escape pause, fixed counter, default map, explicit
  lifecycle loop) and
  `docs/{INPUT_ARCHITECTURE,INPUT_ACTIONS,TIME_ARCHITECTURE,
  FRAME_LIFECYCLE}.md`.
- Tests: `test_input` (118), `test_input_script` (31),
  `test_input_vulkan` (25 incl. inject→Lua→GPU pixel proof).
  Gamepad: backend-neutral API real, OS backends honestly
  PARTIAL (slots driven by injection; no faked devices).
- Fixed: aggregate action latch must update lazily at advance
  start (end-of-advance latching killed just-computed edges
  before script/test reads); `le_engine_step` leaves the
  snapshot readable (edges die at next advance/end_frame).

## Phase 26

- Added Lua gameplay scripting runtime (interpreted only; native
  AOT compiler explicitly NOT implemented): Lua 5.4.8 vendored
  unmodified in `third_party/lua/` (32 lib sources, `lua.c`/`luac.c`
  excluded, own static `luma_lua`), one `lua_State` per `le_engine`
  with worlds isolated by `world_tag` (no global engine state).
- Added VM-independent `le_script_backend_ops` ABI
  (`compile`/`release_chunk`/`instantiate`/`release_instance`/
  `fire`/`get`/`set`/`list_props`) behind the rule
  `Script semantics ─┤├→ Engine operations`: the VM never defines
  gameplay semantics; a future native backend implements the same
  8 ops. Arg-form `start(self)`/`update(self,dt)`/
  `fixed_update(self,dt)`/`destroy(self)` is canonical for AOT.
- Added lifecycle dispatch: snapshot iteration in slot order,
  pending starts delayed until effectively enabled, `destroy` fires
  iff `started`, fixed-step accumulator (`script_fixed_dt`,
  `script_max_steps`, `script_accum`, spiral guard), nest cap 64,
  teardown guards; structural mutation inside callbacks executes
  immediately and safely (new objects start next frame).
- Added `World`/`Object`/`Assets` Lua bindings over the Phase 25
  public API only (create/destroy/find/is_alive/instantiate,
  transforms, hierarchy, components, properties, script-aware scene
  instantiate); identity userdata (object/asset/scene/instance)
  with generation-checked equality; stale/cross-world use raises
  catchable Lua errors.
- Added `export(name, default)` properties
  (bool/int/number/string/vec3/asset) with typed C get/set/list;
  sandboxed `require("scripts.x")` modules (engine roots, `..`
  escapes rejected); `print` routed to `le_script_log_fn`;
  deterministic `LUA_MASKCOUNT` hook + allocator wrapper with
  `memory_budget`; transactional reload preserving values/state
  without re-running `start()`.
- Added scene persistence for scripts: `script <hex>` +
  `sprop <kind> <name> <value>` lines (asset refs by persistent
  ID); never VM state. Per-object single script
  (`LE_COMPONENT_SCRIPT`, `LE_ASSET_SCRIPT` appended, no
  reordering).
- Added `examples/lua_scene` (spinner/mover/orbiter + error demo
  with embedded fallbacks) and
  `docs/{LUA_SCRIPTING,SCRIPT_RUNTIME,SCRIPT_BINDING_API,
  SCRIPT_AOT}.md` plus the `SCRIPTING_ARCHITECTURE.md` Phase 26
  update.
- Tests: `test_script` (75 headless checks: lifecycle, mover,
  delayed-start, error policy, syntax PARSE, exports, spawn+
  destroy, cross-world, serialization round-trip, reload,
  fixed-step, budgets, sandbox, print, stats/refcount/unload,
  teardown, determinism), `test_script_vulkan` (31 checks incl.
  render-driven pixel proofs). Fixed: `_ENV`-fallback harvest
  stack bug (`lua_pushvalue(L,-2)` copied the funcs table instead
  of the function — callbacks silently never ran); Linux
  `errno_t` portability fix (`script_asset.c`).
- Explicitly NOT in Phase 26: Lua→C/AOT compiler, LLVM/JIT,
  physics, editor, MCP.

## Phase 25

- Added engine assets (`le_asset` generational handles on an
  engine-owned registry; mesh/material/texture/scene types;
  FNV-1a/UUID persistent IDs; normalized source paths; READY-gated
  submission; type-safe access; `ASSET_IN_USE` unload policy;
  stats/inspection) and asset-backed renderables (handles resolve
  to renderer backing per frame; unready races skip as dead, never
  dangle). Worlds share assets; world death drops references only.
- Added scenes (`le_scene` payloads distinct from worlds;
  persistent 128-bit object IDs; world capture with ID
  preservation; transactional instantiate with forward refs,
  cycle/duplicate/missing-asset rejection and rollback; duplicate
  instantiation with disjoint handles + temporal keys; instance
  lookup records).
- Added canonical versioned scene text format (memory-first +
  file helpers; deterministic bytes; `%.9g` floats; strict
  escaping; unknown fields tolerated, unknown components/versions
  rejected; 64 MB/16M-object bounds; fuzz suite).
- Added world-preserving reparent (`LE_REPARENT_KEEP_WORLD` via
  TRS inverse + decomposition + commit-time verification) and
  public `le_matrix_decompose` (shear/singular fail honestly).
- Added glTF bridge (`le_gltf_import`: reuses `la_model_load`,
  adopts meshes/materials via new `la_model_adopt_*` API,
  canonical-path dedup returning identical handles).
- Added `examples/scene_roundtrip` (capture→save→destroy→load→
  duplicate instantiate→render; 1+1 shared assets) and
  `docs/{ENGINE_ASSET,SCENE,SERIALIZATION,PERSISTENT_IDENTITY}_
  ARCHITECTURE.md`.
- Tests: `test_scene` (114 headless checks), `test_scene_vulkan`
  (92 checks incl. pixel-exact round-trip, 5000-object scene,
  two-world/two-instance isolation, glTF import).
- Fixed: TRS matrix inverse scale distribution (rows, not columns)
  found by the rotated-parent KEEP_WORLD test.

## Phase 24

- Added Luma Engine (`engine/`, `le_*` static library on public
  Luma Renderer + LumaC only; configure-time backend-independence
  audit): generational objects (`{index, generation, world_tag}`),
  world ownership (geometric growth, free-list reuse, overflow/OOM
  safety), names, enabled state (effective = self && ancestors),
  lightweight components (transform universal; renderable/camera/
  light dense + swap-remove, no exposed addresses), T*R*S
  transforms with iterative dirty propagation, parent/child
  hierarchy (cycle rejection, local-preserving reparent, destroy-
  subtree, 10k-deep stack-safe), deterministic ascending-slot
  iteration, extraction + submission (`render_scene`/`render_output`
  /`render_end` trio + one-call offscreen helper), structured
  stats/memory accounting, honest single-thread contract.
- Added stable renderer temporal identity (closes the pre-Phase-24
  P2 debt): `le_object_stable_id` packs `{salt, tag, index,
  generation}` into `lr_draw_item.instance_id`; renderer LOD
  hysteresis keys per-slot owner tags (slot reuse restarts UNKNOWN;
  ID 0 = metric path). Reorder preserves history; destroy/reuse
  retires it.
- Added `examples/engine_scene` (CameraRig→Camera, Sun, SceneRoot
  incl. mirrored object, MovingParent→ChildA/ChildB; GPU-driven,
  public APIs only) and
  `docs/{ENGINE,OBJECT_IDENTITY,TRANSFORM_HIERARCHY,
  ENGINE_RENDERER_INTEGRATION,SCRIPTING}_ARCHITECTURE.md`.
- Recorded the permanent Lua scripting decision (VM/AOT explicitly
  NOT implemented; engine ABI kept script-friendly).
- Tests: `test_engine` (292 CPU checks), `test_engine_stress`
  (100k lifecycle + 5k churn + 20k world-destroy), expanded
  `test_engine_vulkan` (109 checks: pixel proofs, parented
  camera/renderable, lights, mirror parities, reorder + LOD-mix
  agreement, destroy/reuse retirement, GPU + CPU paths, small +
  5000-object accounting invariants, multi-world). Fixed
  `test_lod_vulkan` forward declaration + keyed-hysteresis stable
  ID (105/105).
- Fixed: `le_object_is_alive` funnels through the single resolve
  validator; render-camera derivation revalidates slots post-
  resolve; `le_ensure_object_capacity` free-list fast path
  simplified (no dead branches).

## Pre-Phase-24 audit

- Fixed mirrored (negative-determinant) transforms rendering
  inside-out: per-item winding parity (`lr_matrix_is_mirrored`) now
  selects a CW front-face pipeline variant and parity-groups GPU
  batches, instead of relying on callers to mark materials
  double-sided. New `renderer/tests/test_mirror_vulkan` proves
  lit-pixel coverage for mirrored cubes on the CPU and GPU-driven
  paths; the shadow test's "known debt" case now asserts lit tops.
- Fixed legacy GPU visibility stats reading stale per-group
  counters for frame-skipped groups (`count == 0` groups contribute
  zero, matching the extended-visibility path).
- Fixed allocator host-OOM accounting corruption (live record is
  pre-allocated; free-list mutations roll back) and grow-path fresh
  block leak on OOM.
- Fixed render-graph transient reuse keying on the full descriptor
  (usage/samples/type/flags), not just format/extent.
- Fixed worker `execute` burning single-shot state on rejected
  mixed compute/graphics batches; added swapchain-primary guard.
- Fixed recorded image transitions racing transfer/worker state
  marks (old states now snapshotted under the state shard).
- Fixed `lr_mesh_create_sphere`/`lr_mesh_sphere_data` integer
  overflow on huge segment/ring counts (fail cleanly pre-alloc).
- Fixed shared (DLL) builds: white-box compute test symbols
  exported (`LC_API` convention) and all renderer tests given the
  DLL search path (was `STATUS_DLL_NOT_FOUND` for Phase 23 tests).
- Fixed unlit "vertex attribute not consumed" validation noise:
  unlit shader declares only locations 0+3 and its pipeline binds
  only those two (PBR keeps four).
- Added `LR_API` visibility macro (renderer builds static today;
  the macro reserves a clean shared future).
- Clarified `visibility_scene --instances N` semantics: N counts
  generation candidates; frame 0 prints
  candidates/submitted/street-skipped/submit-failed with the exact
  accounting equation.
- Added `tools/tsan-lavapipe.suppr` (Mesa lavapipe driver-internal
  TSan noise suppressions; no LumaC frame implicated).
- 100k accounting root cause: street-grid filtering intentionally
  rejects ~half the candidates before submit (verified exact:
  5000 = 2458 + 2542 street-skipped; submitted =
  frustum + occlusion + visible every frame).

## Phase 23

- Added GPU Hi-Z depth pyramid (dedicated R32F mip chain, MAX
  reduction, per-mip semantic transitions, CPU MAX reference,
  `renderer/tests/test_hiz_vulkan` with 8x8/7x5/1xN/Nx1/1x1 plus
  resize fixtures, all GPU mips bit-exact).
- Added GPU Hi-Z occlusion culling (frustum -> occlusion -> LOD ->
  compaction -> indirect generation, conservative-visible policy,
  previous-frame depth with teleport/first-frame bypass,
  `test_occlusion_vulkan` with wall/pixel/teleport/rotation/
  moving-occluder proofs and Hi-Z ON/OFF pixel equivalence).
- Added renderer mesh LODs (up to 4 levels, projected-diameter
  metric, GPU selection with hysteresis + UNKNOWN first-frame
  history, `test_lod_vulkan` against an independent CPU oracle,
  measured triangle reduction, graph/manual equivalence).
- Added backend-neutral indirect-count draws
  (`lc_encoder_draw_indirect_count` /
  `lc_encoder_draw_indexed_indirect_count`,
  `lc_compute_capabilities.indirect_count`, zero-instance
  fallback, `tests/test_indirect_count_vulkan` count 0/partial/
  max pixel proofs).
- Added minimal renderer-owned render graph (pass/resource/use
  declarations, derived edges, topological sort, cycle and
  read-before-write rejection, transient lifetimes + reuse,
  diagnostics/dump/stats, visibility pipeline migrated,
  `renderer/tests/test_render_graph`).
- Added `examples/visibility_scene` (`--frames/--instances/
  --no-hiz/--no-lod/--debug-hiz`, counters, screenshots,
  `docs/images/visibility-scene.png`) and
  `docs/{HIZ,OCCLUSION_CULLING,GPU_LOD,RENDER_GRAPH}_ARCHITECTURE.md`.
- Fixed: transfer release/acquire barriers use IGNORED families
  under CONCURRENT sharing (validation-clean cross-queue uploads).

## Phase 21

- Added backend-neutral compute: `LC_SHADER_STAGE_COMPUTE`,
  `lc_compute_pipeline` (shared binary pipeline cache, warm/cold/
  corrupt/disabled coverage), dispatch on frame encoders and
  compute worker lists, graphics-queue fallback, compute queue
  discovery with honest capabilities, and an isolated
  cross-queue dispatch proof (overlapping async compute
  explicitly deferred).
- Added buffer state tracking (`TRANSFER_SRC/DST`, `VERTEX/INDEX/
  UNIFORM/STORAGE_READ`, `STORAGE_WRITE`, `INDIRECT_READ`) with
  execution/access barriers, `lc_encoder_transition_buffer`,
  storage-image bindings (`SHADER_READ_WRITE`/`GENERAL`), and a
  sync `lc_buffer_read` test/debug path.
- Added indirect drawing (`lc_encoder_draw_indirect`,
  `lc_encoder_draw_indexed_indirect`): explicit-width commands,
  native multi-draw when enabled else an honest loop,
  `INDIRECT_READ` gating (barriers live outside passes),
  `LC_BUFFER_USAGE_INDIRECT`; indirect-count deferred with the
  device capability reported.
- Added renderer GPU-driven mode: per-(mesh, material,
  shadow-flag) groups with shared instance buffers, per-flight
  visible/counter/indirect resources, atomic-append compaction
  with capacity guards, compute frustum culling (CPU oracle
  agreement required), one indexed indirect draw per group
  through the PBR-compatible instanced pipeline, CPU fallback,
  per-frame diagnostics, and a 100k-instance benchmark plus
  1000-frame endurance.
- Added `tests/test_phase21_compute_vulkan` (127 checks),
  `renderer/tests/test_gpu_driven_vulkan` (94 checks), and
  `examples/gpu_driven_scene` (`--frames/--instances/
  --cpu-culling/--gpu-culling/--no-validation/--screenshot`,
  `docs/images/gpu-driven-scene.png`).
- Fixed: GPU-only buffers gain transfer-source for download
  copies (VUID); secondary buffers always carry inheritance info
  (VUID); multi-draw/indirect-count features enabled when
  offered (VUID); compute-compatible outside-pass dispatch next
  to pending clears; group regrow preserving filled instances.

## Phase 20

- Added worker recording contexts and immutable single-use command lists with
  ordered batch execution and submission-time state validation.
- Added queue classes, dedicated-transfer diagnostics, timeline-style GPU
  completion, and configurable frames in flight.
- Added bounded async buffer/image uploads, async image readback, and
  completion-keyed deferred destruction.
- Added validation-backed tests for 1,000 uploads, oversized staging, and
  50,000 real draw recordings across four CPU threads with pixel proof, plus
  concurrent allocator churn and ThreadSanitizer coverage.

## Unreleased

- Luma AAA memory + synchronization foundation (Phase 19):
  semantic resource states (`lc_resource_state`) with per-mip/layer
  tracking, explicit `lc_encoder_transition_image`, write→read
  visibility fix in render-pass dependencies (closes Phase 18
  debt), block suballocator (4 classes, best-fit + coalescing,
  dedicated policy, warm-reserve reclamation, internal lock),
  non-coherent flush/invalidate with persistent mapping, pool
  staging (no per-op dedicated allocations), memory stats/budget/
  per-resource introspection, `tests/test_sync_vulkan` (1000-
  transition ping-pong, depth, upload/copy paths, mixed states),
  `tests/test_memory` (10k buffers→1 block, fragmentation,
  dedicated, OOM rollback, benchmarks, scale workload),
  renderer endurance + churn + asset-cycle memory proofs, new docs
  (SYNCHRONIZATION/GPU_MEMORY), pre-1.0 API review notes
- Fixed: depth/non-sampled images settling to SHADER_READ (VUID);
  legacy swapchain pass dependencies vs cached passes
  (framebuffer incompatibility); mid-test teardown races
  (drain before destroy); UNORM .5 rounding assumption in tests
- Luma infrastructure hardening (Phase 18): public image readback
  (`lc_image_query_readback`/`lc_image_readback`: tight rows,
  mip/layer/depth, loud TRANSFER_SRC gating, sync with reserved
  async names), stable resource IDs + identity-compared binding
  caches (+ ABA stress), persistent Vulkan pipeline cache (app
  path, atomic save, corrupt-safe, disabled mode, introspection),
  backend-neutral monotonic clock, renderer CPU profiling
  (`lr_frame_profile`) + diagnostics snapshot, internal
  post-process chain (ping-pong intermediates, tint verification
  stage), renderer HDR/LDR capture + BRDF view borrower,
  `ibl_scene --screenshot` via Luma-authored `tools/png_mini.h`,
  `docs/images/ibl-scene.png`, 500-frame endurance, new docs
  (READBACK/PIPELINE_CACHE/POST_PROCESS), pre-1.0 API debt list
- Fixed: example freed upload pixels before `lc_image_write`
  (use-after-free → negative HDR); tonemap set rewrite between
  binds invalidated shared recordings (identity-guarded skip);
  non-SAMPLED depth settling to SHADER_READ (VUID); all in-tree
  `lc_device_desc` zero-initialized
- Luma Renderer environment lighting (`lr_*`, Phase 17): HDR
  equirect sources become split-sum IBL (128-cube with full mip
  chain, 32 irradiance, 64 prefilter x7 mips, 256 BRDF LUT, fixed
  tap counts), sky pass, auto-fit HDR scene target, exposure EV +
  NONE/ACES tonemap output pass, params-only intensity/yaw (lighting
  and sky rotate together), ambient fallback iff no env
  (bit-identical legacy), render_scene/render_output frame flow,
  extended stats + read-only environment introspection,
  `examples/ibl_scene`, `docs/ENVIRONMENT_ARCHITECTURE.md`
- Luma Assets HDR (`la_hdr_*`): strict RGBE decode, RNE
  float/half conversion, R16F GPU upload with path cache,
  procedural `env_gradient.hdr` fixture
- Tonemap pipeline keyed by full target signature incl. depth
  format (swapchain passes always carry depth)
- Luma Renderer shadows (`lr_*`, Phase 16): directional (fitted
  ortho) + spot (cone perspective) shadow maps onto PBR (3x3 manual
  PCF, constant + normal bias, opt-in cast/receive, LR_MAX_SHADOWS
  4 with unshadowed over-capacity), per-light resolution/biases,
  single-prepare frame flow with loud flow guards, extended stats +
  read-only slot/light introspection, `examples/shadow_scene`,
  `docs/SHADOW_ARCHITECTURE.md`, `docs/images/shadow-scene.png`
- LumaC generic depth plumbing (no shadow semantics): optional
  fragment stage for depth-only pipelines, sampled-depth final
  layout, `lc_device_wait_idle`
- Test-harness depth-tested main targets (painter order is
  heap-address noise); renderer examples drain the device before
  teardown; renderer shaders compile with `glslc -O`
- Luma Renderer PBR (`lr_*`, direct lighting only): explicit
  UNLIT/PBR material kinds, PBR metallic-roughness materials (own
  parameter buffer, five texture roles with neutral fallbacks,
  double-sided variants), Cook-Torrance BRDF (GGX/Smith/Schlick/
  Lambert, metallic workflow, 0.05 roughness floor), directional +
  point lights (LR_MAX_LIGHTS 64, uniform buffer, mapped updates),
  temporary ambient fallback, normal mapping with handedness +
  inverse-transpose normal matrices, occlusion-on-ambient,
  factor-or-texture emissive, per-kind pipeline variants, extended
  stats + read-only introspection, `examples/pbr_scene`,
  `docs/PBR_ARCHITECTURE.md`, `docs/images/pbr-scene.png`
- Luma Assets imports glTF materials as real PBR (all roles,
  scales/strengths, double-sided, alpha metadata); `model_viewer`
  lights its model
- Renderer gains `lr_renderer_submit_light`, `lr_renderer_set_ambient`,
  `lr_renderer_get_ambient`, `lr_renderer_get_pipeline_count`,
  `lr_material_create_pbr`, `lr_material_get_type`,
  `lr_material_get_pbr_info` (additive, pre-1.0)
- Luma Assets (`la_*`, separate `luma_assets` library on public Luma
  Renderer + LumaC only): glTF 2.0 ingestion (vendored cgltf v1.15,
  stb_image) with strict validation, generated normals/tangents,
  hierarchy with raw + decomposed transforms, full PBR metadata with
  PBR material upload, role-qualified shared texture/sampler
  caches, model-space bounds, deterministic file-order submission via
  `la_model_submit`, `examples/model_viewer`, `docs/ASSET_ARCHITECTURE.md`
- Renderer gains `lr_renderer_get_device` (borrower for upper layers;
  additive, pre-1.0)
- Luma Renderer (`lr_*`, separate `luma_renderer` library on public
  LumaC only): reusable GPU meshes (PBR-ready vertices, bounds,
  cube/plane/sphere), unlit materials with fallback texture/sampler,
  cameras, quaternion transforms, frustum-culled draw lists with
  opaque sorting and statistics, per-target pipeline cache,
  offscreen/multi-viewport rendering, `examples/renderer_scene`
- Pipeline creation no longer takes a swapchain (breaking, pre-1.0):
  `lc_graphics_pipeline_create(device, desc, out)` with a mandatory
  structural render-target description, plus
  `lc_swapchain_get_render_target_desc` for presentation pipelines
- Render-target view getters, pass-cache count debug accounting;
  copy-to-buffer rejects sourceless images loudly instead of
  driver-dependent behavior
  (non-owning color/depth views, up to 8 colors, depth-optional,
  sample-aware), structural `lc_render_target_desc` compatibility
  (hash fast-path, structural resolve), borrowed swapchain targets,
  `max_color_attachments` device limit
- Command encoders: borrowed `lc_command_encoder` per open frame with
  an explicit state machine (one pass max, no nesting, no submit
  while open); explicit offscreen (`lc_encoder_begin_render_pass`)
  and swapchain (`lc_encoder_begin_swapchain_pass`) passes with
  backend-neutral load/store ops; full generic record path (bind
  pipeline/sets/vertex/index, push constants, indexed/instanced
  draws); legacy swapchain-bound recording kept as mutually-exclusive
  convenience
- Pipelines decoupled from swapchain identity: structural target
  signature in the desc (legacy all-zero infers from the anchor),
  device-only lifetime (survive swapchain destroy), multi-attachment
  blending, sample-count-aware creation against the shared
  render-pass cache (no per-frame allocation)
- Render-to-texture proven: `examples/render_to_texture` (cube scene
  into 512x512, sampled to swapchain; offscreen survives resizes)
- Image layouts extended (color/depth attachment endpoints);
  attachment finals land sampled-readable so second passes validate
  with no extra barriers
- First 3D foundation: indexed drawing (
  (`lc_draw_instanced`, per-instance vertex input, instanced indexed
  draws), push constants (`lc_push_constant_range`,
  `lc_push_constants`), raster state (cull mode, front face),
  depth testing (`lc_clear_depth`, swapchain-owned depth buffer,
  `lc_swapchain_get_depth_format`), `examples/cube_3d` (indexed
  textured cube x2 instances, rotating push-constant MVP)
- Pipelines carry raster/depth/push state with depth-compatible render
  passes; binding matches by canonical signatures (layout destruction
  no longer invalidates matching pipelines/sets)
- Resource bindings: `lc_image_view` (mip/layer ranges, cube/depth
  aspects), `lc_shader_visibility`, binding layouts/sets with
  uniform/storage/sampled/storage-image/sampler slots, descriptor
  arrays, validated batched updates, device-level descriptor
  allocator, `lc_bind_binding_set`
- Per-subresource image layout tracking; sampled-state-gated binding
  with vertex/fragment/compute stage mapping
- Pipelines take ordered binding-layout slots; `examples/textured_quad`
  (mipmapped checkerboard, animated uniform, no descriptor rebuilds)
- `LC_ERROR_PIPELINE_INCOMPATIBLE` now also covers layout mismatch
- Image/texture foundation: `lc_image` (1D/2D/3D, mips incl. full
  chain, array layers, cube-compatible flag, multisample field),
  default full-resource views, whole-image layout tracking
- Staging uploads via `lc_image_write` (descriptor-based regions),
  GPU linear-blit mipmap generation, exact upload round-trips
- `lc_sampler` (filters, mipmap modes, address modes, LODs,
  capability-gated anisotropy); device negotiates samplerAnisotropy
- Depth formats in `lc_format` with color/depth/stencil metadata;
  `examples/texture_upload`
- `LC_ERROR_IMAGE_CREATION_FAILED/SAMPLER_CREATION_FAILED/UNSUPPORTED`
- Production resource foundation: backend-neutral `lc_format` system
  with centralized Vulkan translation
- Generic `lc_buffer` abstraction (GPU-only / CPU-to-GPU / GPU-to-CPU
  memory model), persistent coherent mapping, bounds-checked
  `lc_buffer_write` with staging uploads via a device upload context
- Backend-neutral vertex bindings/attributes, pipeline vertex-input
  state, `lc_bind_vertex_buffer`
- `examples/vertex_triangle`: identical triangle from a real GPU
  vertex buffer; exact staging round-trip verified in tests
- `lc_device_get_limits`, `lc_swapchain_get_format`
- `docs/ARCHITECTURE.md` (subsystems, ownership, backend strategy,
  known pre-1.0 debt) and `docs/API_DESIGN.md` (binding principles)
- First triangle: `lc_shader` (SPIR-V modules), `lc_pipeline`
  (empty layout, dynamic viewport/scissor, no culling), minimal
  render pass owned by the swapchain, `lc_bind_pipeline` + `lc_draw`
- Clears now flow through render-pass load ops (deferred color,
  `vkCmdClearAttachments` once recording); submit waits at
  color-attachment output
- Swapchain images require transfer-destination usage for clears
- `examples/triangle` with committed and build-time-compiled SPIR-V
- Recoverable `LC_ERROR_PIPELINE_INCOMPATIBLE` for format drift
- Frame lifecycle: `lc_begin_frame` / `lc_clear_color` / `lc_end_frame`
  with acquire, submit, and present
- Two frames in flight, per-image present semaphores and ownership
  tracking
- Recoverable `LC_ERROR_SWAPCHAIN_OUT_OF_DATE` and `LC_SUBOPTIMAL`
  results; resize/minimize workflow in `examples/clear_screen`

## 0.1.0-dev

- Core `lc_init` / `lc_shutdown` lifecycle and version API
- Native Win32 windowing; X11 backend implemented
- Vulkan device foundation: instance, validation, GPU selection,
  logical device with per-family graphics queues
- Vulkan surfaces with present-queue discovery
- Swapchains with capability-driven selection, image views, and
  transactional recreation
- Static/shared CMake builds, headless unit tests, Vulkan integration
  tests, Windows verified, Linux compile verified
