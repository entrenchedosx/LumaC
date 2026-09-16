# Phase 31 — Editor Foundation, Reflection, Undo/Redo & Play Mode: Design

Status: design (implementation follows). Baseline: `98f2a30` (HEAD == origin/main,
clean tree), Debug 75/75 PASS (67.66 s), Release 75/75 PASS (67.19 s), rebuilt
both configs from a clean tree state before designing.

## 1. Goals

Build the Godot-like editor **architectural foundation** — not a cosmetic GUI:
a headless `EditorCore` (selection, commands, undo/redo, dirty tracking, scene
I/O, play-mode isolation) plus an `EditorGUI` support layer (hierarchy model,
inspector model, viewport math, gizmo intents, console, shortcuts) that is
fully drivable without a window. Every behavior is proven by headless CTest
suites; no GUI framework is introduced (no ImGui/nuklear; repo grep confirms
zero matches).

STOP after Phase 31. Phase 32 (full editor, asset browser, shader/timeline/
graph editors, physics/navmesh/terrain editors, material graph, visual
scripting, profiler, packages, C#, MCP server, networking, prefab UI, docking,
themes, exporter) is explicitly NOT implemented.

## 2. Non-goals (Phase 32+)

No asset browser, shader editor, timeline editor, graph editors, physics/
navmesh/terrain editors, material graph, visual scripting, profiler, package
manager, C# scripting, MCP server, networking/multiplayer, prefab-variant UI,
docking layout, theming, or scene exporter. No new renderer submission path
(gizmo rendering reuses `le_physics_extract_debug_lines` data + viewport math;
no `lr_*` line API is added).

## 3. Layering invariants (mandatory, CI-audited)

- `editor → engine → assets → renderer → LumaC`. No `engine→editor`,
  `renderer→editor`, or `LumaC→editor` back-edge.
- Editor sources use ONLY public headers (`luma_editor.h`,
  `luma_engine/luma_engine.h`, `luma_renderer/luma_renderer.h`, `lumac.h`).
  Never `engine/src/internal/*`, `renderer/src/*`, backend or `*_internal.h`.
- C++ editor code over the C engine ABI is NOT used: the editor ships as C11
  static library `luma_editor` like every other layer (CONTRIBUTING: "C11, no
  C++ in library sources"). No C++ types in `led_*` surface, no
  `typeid`/`dynamic_cast`/RTTI (verified zero hits in `engine/`).
- No Lua spellings (`lua_*`, `LUA_*`) outside `engine/src/script/` — the
  editor talks to scripts only via `le_script_*` C API.
- Reflection routes through validated engine APIs (`le_object_is_alive`,
  `le_object_has_component`, per-component getters); the editor never
  dereferences opaque handles.
- Commands own heap memory explicitly (`malloc`/`free`); no raw-pointer
  borrowing across undo boundaries — payloads are deep-copied values.
- Never run gameplay on the edit world: edit world stays paused with scripts
  stepped only via explicit matrices-only updates; runtime stepping applies
  ONLY to the play world.

## 4. Gaps found in the engine API (verbatim survey results)

1. **No object iterator / root list** — only `le_world_get_root_count`.
   Children enumerable per object, roots unobtainable → add
   `le_world_get_roots` (new engine API, Appendix A of the user spec).
2. **No generic reflection** — only `le_object_has_component`,
   script-only `le_script_list_properties`, and stale `le_object_info`
   (predates script/physics/anim/character) → editor-side tables +
   `led_describe_object` / `led_read_property` / `led_write_property`.
3. **No world/object AABB** — compose from `lr_mesh_get_bounds` ×
   `le_object_get_world_matrix` → `led_compute_world_aabb` (pointer
   renderables only; asset-backed renderables report
   `LE_DATA_UNAVAILABLE`, never guessed).
4. **No unproject/screen-ray** — invert `le_world_get_render_camera`
   output → `led_viewport_ray`; mesh picking without colliders
   unsupported (documented; physics raycasts need colliders).
5. **No debug-line/grid API** — only physics line soup → gizmo layer
   emits *intents* (`led_gizmo_*`), rendering deferred to Phase 32.

## 5. New engine APIs (minimal, append-only, in `luma_engine.h` style)

```c
le_result le_world_get_roots(le_world *world, le_object *out_roots,
                             uint32_t capacity, uint32_t *out_count);
void le_object_get_info2(const le_world *world, const le_object *object,
                         le_object_info2 *out_info); /* LE_OBJECT_INFO2_VERSION=1 */
uint32_t le_world_get_all_objects(le_world *world, le_object *out_objects,
                                  uint32_t capacity); /* optional census */
```

`le_world_get_roots`: ascending-slot deterministic order, counting query when
`out_roots == NULL`, `*out_count` always full count. `le_object_info2` carries
`version`, full presence bits incl. script/rigidbody/collider/animator/
character + `asset_backed_renderable` flag; `le_object_info` is NOT modified.
All NULL/stale-safe (zeros), never crash. These are the ONLY engine changes.

## 6. Editor module layout (`editor/`)

```
editor/
  include/luma_editor/luma_editor.h   # whole public surface (LE_API, C++-guard)
  src/editor_core.c       # session create/destroy, attach, tick(dt), stats
  src/editor_selection.c  # selection set (stable handles, liveness-filtered)
  src/editor_history.c    # bounded undo/redo stacks, coalescing, memory stats
  src/editor_commands.c   # command vtables: create/delete/set-name/enabled/
                          #   TRS/reparent/add-remove-component/script-prop
  src/editor_reflect.c    # static reflection tables + describe/read/write
  src/editor_hierarchy.c  # hierarchy model (roots→children snapshot)
  src/editor_inspector.c  # inspector rows over reflection (headless model)
  src/editor_scene.c      # new/open/save/save-as/revert, dirty tracking
  src/editor_play.c       # play/stop/pause/step, edit↔play isolation
  src/editor_viewport.c   # orbit camera math, unproject, picking, framing
  src/editor_gizmo.c      # translate/rotate/scale intents (math only)
  src/editor_console.c    # log ring + error capture
  src/editor_shortcuts.c  # shortcut table + focus policy
  src/editor_serialize.c  # editor project/prefs text format (self-contained)
  tests/test_editor_core.c      # session, selection, hierarchy, inspector
  tests/test_editor_history.c   # commands, undo/redo, coalescing, bounds
  tests/test_editor_play.c      # play isolation, pause/step, failure paths
  tests/test_editor_viewport.c  # camera, unproject, picking, AABB, framing
  tests/test_editor_reflect.c   # reflection coverage, validation, round-trips
  CMakeLists.txt                  # luma_editor static lib + audit + 5 tests
```

`CMakeLists.txt` mirrors `engine/CMakeLists.txt`: backend-independence audit
over `include/*.h + src/*.c` (forbids `Vk*`, `vulkan/`, `windows.h`, `X11/`,
`HWND`, `graphics_internal`, `lumac_internal`, `renderer_internal`,
`engine/src/internal`, `renderer/src`, `lua_*`/`LUA_*`), C11, `/W3` or
`-Wall -Wextra -Wpedantic`, `enable_testing()`, five `add_test()` entries,
Windows `PATH` extension for shared builds.

## 7. Core architecture

### 7.1 EditorCore vs EditorGUI split

- `EditorCore` (headless, fully tested): session, selection, command history,
  dirty flag, scene open/save, play-mode controller, reflection access.
  Zero window/renderer/windowing calls; operates on `le_engine`/`le_world`
  via public API only. Owns NOTHING engine-side (engine/world lifetime stays
  with the host; `led_session_attach(engine, edit_world)` borrows).
- `EditorGUI` (headless models, Phase-32-renderable): hierarchy snapshot,
  inspector row list, viewport state (camera + size + ray), gizmo intent
  list, console ring, shortcut table. Produces plain data; a future GUI
  renders it. All models refreshable via `led_*_refresh()` and unit-tested
  without a window.

### 7.2 Selection

`led_selection` = ordered unique array of `le_object` handles (cap 256,
configurable), validated by `le_object_is_alive` on every read path:
`set`, `add`, `remove`, `clear`, `toggle`, `contains`, `get_all` (returns only
live), `prune` (drops stale/slot-reused), `select_subtree`, `select_by_name`.
Stale handles never crash; slot-reuse (same index, bumped generation) does NOT
alias — `le_object_is_alive` fails the old generation. Selection changes do
NOT mark dirty (view state, like camera).

### 7.3 Commands + undo/redo

Every mutation flows UI → `led_command` → engine → history → dirty flag.
Command record: `{kind, label, target (le_object snapshot), before (bytes),
after (bytes), applied}`. Kinds: `create`, `delete` (subtree snapshot for
restore), `set_name`, `set_enabled`, `set_position/rotation/scale`,
`set_parent`/`reparent` (mode recorded), `add_component`/`remove_component`
(full desc bytes), `set_script_property` (typed `le_script_property` pair).

- `execute`: validate-then-apply; engine failure → command NOT pushed,
  history and dirty untouched.
- `undo`: apply inverse (delete→recreate-subtree from snapshot; set→before
  bytes; add-component→remove; remove-component→re-add with bytes).
- Redo: re-apply after bytes. Redo stack cleared on new execute (standard).
- Bounded: default cap 256 commands; eviction drops oldest, counted in
  `led_history_stats{undo_depth, redo_depth, evicted, bytes_estimate}`.
- Coalescing: consecutive same-target same-kind TRS drags merge within a
  500 ms window (configurable) to keep drags at one undo step; coalesced
  count reported.
- GUI-independent: commands constructible from values alone (MCP-ready);
  `led_execute_new/edit` take plain structs, no window/input state.
- 10k-command stress: push 10k set-position commands, undo all, redo all,
  assert byte-exact final state + bounded memory stat.

### 7.4 Reflection (editor-side tables over validated engine APIs)

Static const tables per component: `{component, property_name, type, offset,
size, min, max, flags, enum_labels?}`. Types: `LE_DATA_BOOL/INT/UINT/FLOAT/
VEC3/QUAT/STRING/ENUM/ASSET_ID/COLOR3`. Coverage: transform (pos/rot-quat+
Euler mirror policy §9/enabled/name/parent), renderable flags, camera lens
(projection enum, fov/aspect/near/far/ortho_height with renderer-mirrored
validation), light (type enum, color3, intensity, range, cones, shadow passthru
opaque), script (enumerated live via `le_script_list_properties`; typed read/
write via `le_script_get/set_property`), rigidbody, collider (shape enum +
per-shape dims + frames + layers), animator (desc + playback readouts),
character (full config).

`led_describe_object` → presence bits (via `has_component` × 9) + row count.
`led_read_property` / `led_write_property` validate liveness + type + range
and return `LED_ERROR_*` codes (never crash on stale). Euler policy: inspector
shows degrees; storage stays quaternion; write converts with normalization;
singular/NaN rejected. Asset-backed renderables: reflection reports
`LE_DATA_UNAVAILABLE` for mesh/material identity (persistent IDs are not
renderer pointers); no guessing.

### 7.5 Scene I/O + dirty tracking

`led_scene_new/open/save/save_as/revert`: thin over `le_scene_create/capture/
instantiate/save_file/load_file` + canonical-text oracle (save→load→save is
byte-identical). Dirty flag: set ONLY by successful command execute; cleared
by save/load/new/revert/undo-to-clean? (No — undo does NOT auto-clear; dirty
clears on save/load/new only. Documented.) Failed save keeps dirty; failed
load preserves the edit world (load into temp scene asset first, instantiate
into scratch validation, swap only on success).

### 7.6 Play mode (edit↔play isolation)

- `led_play_enter`: requires clean validation (world alive, scene asset
  writable). Capture edit world → `le_scene_capture` → instantiate into a
  FRESH runtime world on the SAME engine (`le_world_create` +
  `le_scene_instantiate`). Edit world `le_world_set_paused(edit, 1)`; runtime
  world unpaused. Selection maps via persistent IDs where possible (best
  effort; unmapped → cleared, reported).
- `led_play_tick(dt)`: steps ONLY the runtime world (`le_engine_step(engine,
  play_world, dt)` or explicit `le_world_update`); edit world receives
  matrices-only refresh so the outliner stays valid but scripts NEVER run on
  edit (`le_world_update(edit, 0)` path — matrices only per sync.c).
- `led_play_pause/resume/step`: `le_world_set_paused` + `le_time_request_single_step`
  on the runtime world.
- `led_play_exit`: destroys the runtime world (`le_world_destroy`), unpauses
  edit, restores pre-play selection by handle (pruned). Edit world is
  byte-identical to pre-play: proven by canonical capture before enter vs
  after exit (oracle test).
- Failed play (e.g. unready asset): `LED_ERROR_PLAY_FAILED`, edit world
  untouched, no runtime world leaked.
- Scripts: play world scripts start fresh (`pending_start`); edit-world
  scripts never fire during play. Physics determinism NOT promised across
  platforms (documented, matches engine); same-build explicit-dt replay is
  deterministic.

### 7.7 Viewport (math only, headless)

`led_viewport` = `{width, height, orbit_target[3], yaw, pitch, distance,
fov_y, near, far}`. Orbit math (yaw/pitch clamp, distance clamp) → eye →
`lr_camera_look_at` + `lr_camera_set_perspective` → `le_world_get_render_camera`
override for framing. `led_viewport_ray(x, y)` inverts view-proj
(column-major, Vulkan NDC 0..1) → world ray → `le_physics_raycast` picking
(colliders required; mesh-without-collider picking documented unsupported).
`led_compute_world_aabb` (pointer renderables: mesh bounds × world matrix;
mirrored basis handled via absolute-axis expansion). `led_frame_selection`
fits orbit distance to selection AABB union. Viewport NEVER submits to a
renderer in Phase 31 (no window/GPU in tests); render trio stays engine-side.

### 7.8 Gizmos

Math-only intents: `led_gizmo_translate/rotate/scale {axis, start, current,
delta, snap}` → TRS delta application via commands (undoable). No rendering
path; `led_gizmo_lines` emits a line-soup preview buffer (editor-owned) for a
future Phase-32 overlay pass. Snap increments configurable (default 0.1 m,
15°). Screen-axis projection uses the viewport camera.

### 7.9 Console, shortcuts, focus

- Console: ring buffer (cap 1024 entries, `{level, tag, message[256]}`),
  `led_console_push/clear/drain`; last script error mirrored via
  `le_script_get_last_error` on play tick failure.
- Shortcuts: static table `{key, mods, action}` for duplicate/delete/undo/
  redo/play/stop/step/save; `led_shortcut_match(key, mods)` pure function;
  focus policy: viewport shortcuts suppressed when console/field focused
  (`led_focus_set/get`, headless flag).

## 8. Error model + stats

`led_result` mirrors `le_result` numbering above 1000 (`LED_SUCCESS=0`,
`LED_ERROR_INVALID_ARGUMENT=1001`, `STALE_HANDLE=1002`, `WRONG_WORLD=1003`,
`NOT_ATTACHED=1004`, `NO_EDIT_WORLD=1005`, `ALREADY_PLAYING=1006`,
`NOT_PLAYING=1007`, `PLAY_FAILED=1008`, `VALIDATION=1009`, `OUT_OF_MEMORY=1010`,
`OVERFLOW=1011`, `UNAVAILABLE=1012`, `IO=1013`, `PARSE=1014`, plus
`le_result` passthrough for engine failures). `led_session_stats` +
`led_history_stats` + `led_play_stats` plain-data structs; zeros for NULL.

## 9. Euler policy (normative)

Storage is always quaternion (`le_object_set_rotation`, normalized on store).
The inspector presents ZYX-Euler degrees (pitch clamp ±89.9° display). Write
path converts degrees→quat, normalizes, stores; read path converts quat→
degrees deterministically. Gimbal-flip ambiguity resolved by canonical branch
(|pitch| ≤ 90°). NaN/non-finite rejected before conversion. Round-trip oracle:
quat→euler→quat agrees within 1e-5; euler→quat→euler within 1e-4°.

## 10. Headless test plan (5 suites, all no-GPU/no-window)

- `test_editor_core` (~120 checks): session attach/detach, selection
  (incl. stale/slot-reuse pruning), hierarchy snapshot vs engine truth,
  inspector row counts, dirty transitions, console ring, shortcuts.
- `test_editor_history` (~150): every command kind execute/undo/redo,
  inverse correctness, failed-execute-no-push, redo-clear, bounds eviction,
  coalescing, 10k stress + memory stats.
- `test_editor_play` (~100): enter/tick/exit, edit-before/after canonical
  equality, runtime-only stepping proof (edit script counter frozen),
  pause/resume/step, failed-play-no-touch, double-enter/double-exit errors.
- `test_editor_viewport` (~100): orbit math, unproject round-trip (project→
  unproject within epsilon), raycast picking incl. miss, AABB composition +
  mirror expansion, frame-selection distance, asset-backed UNAVAILABLE.
- `test_editor_reflect` (~130): per-component describe/read/write coverage,
  type/range/enum validation matrix, Euler round-trips, script props live
  enumeration, 100k-object enumeration timing (reported, never asserted).

Proofs beyond suites: reparent round-trip oracle, serialization oracle
(save→load→save byte-identical), ASan/UBSan + TSan (Linux), Vulkan validation
(engine suites), Windows Debug/Release + shared + Linux matrices.

## 11. Docs to write

`docs/EDITOR_ARCHITECTURE.md` (core/GUI split, layering, session model),
`docs/ENGINE_REFLECTION.md` (tables, types, validation, Euler policy),
`docs/EDITOR_COMMANDS.md` (kinds, payloads, MCP-readiness),
`docs/EDITOR_UNDO_REDO.md` (inverses, bounds, coalescing),
`docs/EDITOR_PLAY_MODE.md` (isolation recipe, failure semantics),
`docs/EDITOR_VIEWPORT.md` (orbit, unproject, picking, AABB, framing).
Update `ARCHITECTURE.md`, `ENGINE_ARCHITECTURE.md`, `SCENE_ARCHITECTURE.md`,
`SERIALIZATION_ARCHITECTURE.md`, `FRAME_LIFECYCLE.md`, `API_DESIGN.md`,
`SCRIPTING_ARCHITECTURE.md` (or scripting companions), `README.md`,
`CHANGELOG.md` (Phase 31 entry).

## 12. Implementation order

1. `editor/include/luma_editor/luma_editor.h` (full surface first).
2. New engine APIs (`le_world_get_roots`, `le_object_info2`) + engine tests.
3. `editor_core/selection/hierarchy` → `test_editor_core` green.
4. `reflect/inspector` → `test_editor_reflect` green.
5. `commands/history/scene(dirty)` → `test_editor_history` green.
6. `play` → `test_editor_play` green.
7. `viewport/gizmo/console/shortcuts/serialize` → `test_editor_viewport` green.
8. Root `CMakeLists.txt` wiring (`add_subdirectory(editor)` behind
   `LUMA_BUILD_EDITOR=ON`, default ON, requires `LUMA_BUILD_ENGINE`).
9. Proofs (stress, oracles, sanitizers, validation, matrices).
10. Docs + CHANGELOG + final report. STOP.
