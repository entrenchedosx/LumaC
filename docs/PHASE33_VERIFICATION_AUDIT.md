# Phase 33 Verification Audit (Phase 33V) — Test-Trust Audit

Scope: `505992b` (Phase 32 baseline) → `66a8561` (Phase 33 HEAD). 91/91
executables reported success. This document treats that as
**"91 executables returned 0"** — nothing more — until each test's
actual exercised path is inspected.

Phase attribution (`git diff --name-only 505992b..66a8561` + `git log
--oneline -- <file>`): Phase 33 touched, in the editor, only
`editor/app/main.c` (new), `editor/demo/phase33_builder.c` (new),
`editor/src/gui/*` (new), `editor/tests/test_editor_gui.c` (new),
`editor/tests/test_editor_gui_gpu.c` (new), plus LumaC blend/scissor
state and vendored ImGui. **All other editor test files are Phase 32
and were untouched by Phase 33.**

Ratings: TRUSTED / PARTIAL / WEAK / FALSE POSITIVE /
NOT ACTUALLY TESTING CLAIM.

Global rule found across ALL 15 editor binaries: **none launches the
headed editor app (`luma_editor_app`), none drives a real `leg_*`
panel with real OS input events, none performs a real mouse
drag/drop, none asserts pixels of the real viewport.** "Browser",
"drag", "drop", "workflow", "end-to-end" claims are exercised through
headless `led_browser_*` / `led_drag_begin` / `led_drop_*` struct APIs
against TEMP dirs and throwaway in-memory sessions. There are **no
mocks/stubs anywhere** (good) — the false-positive risk is
*integration gaps*, not stubs. Every binary stops at the `led_*` +
`le_*` model layer, except the two `test_editor_gui*` binaries.

---

## 1. test_editor_core (Phase 31, unchanged in 33) — PARTIAL

- Claims: session lifecycle, selection incl. stale + slot-reuse
  pruning, hierarchy snapshot vs engine truth, inspector rows, dirty
  transitions, console ring, shortcuts.
- Actually executes: real `led_session_*`, real `le_engine/le_world`
  (renderer-less), real `led_selection_*/led_hierarchy_*/led_inspect/
  led_console_*/led_shortcut_match/led_focus_*/led_dispatch_action`.
  Synthetic: 1–3 anonymous objects per block.
- Real app / GUI panels / renderer / user interaction: NO on all axes
  (shortcuts tested as pure `led_shortcut_match`, never via the
  window event queue).
- Passes-while-broken: outliner renders stale names; inspector panel
  mis-binds rows; console panel never polls; Ctrl+Z swallowed by a
  text field; GUI-cached row + engine slot reuse highlights the wrong
  object. All green here.
- Strongest claim: `ALL PHASE 31 CORE HEADLESS TESTS PASSED` banner +
  `"hierarchy 3 nodes"`. Weakest actual: ~15 NULL-plumbing passes
  (`"contains NULL 0"`, `"prune NULL 0"`). Inverse brittleness:
  `"slot reused (same index expected)"` asserts engine allocator
  recycling, not editor logic.
- Rating **PARTIAL**: real model logic, zero evidence any pixel,
  panel, or keypress works.

## 2. test_editor_history (Phase 31) — PARTIAL

- Claims: commands, undo/redo, inverses, bounds eviction, coalescing,
  failed-execute-no-push, redo-clear, 10k stress with memory stats.
- Actually executes: real `led_execute/led_undo/led_redo/history_*` +
  real world reads. Synthetic hand-built `led_command`s; the 10k test
  hammers **one object's position** 10k times, not 10k distinct edits.
- Passes-while-broken: inspector writes bypassing `led_execute`
  diverge history/dirty silently; Undo toolbar button wired to the
  wrong stack; `KEEP_WORLD` reparent asserts only undo success, never
  the preserved transform numerically.
- Strongest: `"10k pushed/undone/redone"`. Weakest: `"bytes estimate"`
  is just `> 0`; `"byte-exact final"` is exactly-representable floats,
  not serialization equality.
- Rating **PARTIAL**: strongest file at its layer (real inverses,
  delete-subtree restore, cycle-rejection-pushes-nothing), but proves
  the stack, not GUI wiring or geometric correctness.

## 3. test_editor_play (Phase 31) — PARTIAL

- Claims: play isolation, canonical edit-before/after equality,
  runtime-only stepping, pause/resume/step, scene round-trip; "Script
  isolation is proven with a counter script".
- Actually executes: real `led_play_enter/tick/exit/set_paused/step/
  get_world`, real `le_engine_step`, real canonical oracle
  (`le_scene_capture/save_text` → `strcmp`), real Lua counter, real
  TEMP file save/open/revert. Synthetic: 1-object worlds, fixed dt.
- Passes-while-broken: Play button never calls `led_play_enter`, or
  the viewport keeps compositing the **edit** world during play (only
  pointer inequality `get_world()!=w` is checked) — frozen scene,
  green test. Pause is flag-only: `"tick while paused ok"` asserts a
  return code, **never that the world froze** — a pause that doesn't
  pause passes. A comment claims mid-play save coverage; the block
  never calls save mid-play.
- Strongest: `"edit byte-identical after play"`. Weakest: `"negative
  dt w/o play still NOT_PLAYING"` (error-precedence trivia);
  isolation thresholds (`>=5` runtime vs `<=1` edit) pass at any wrong
  rate.
- Rating **PARTIAL**: enter/tick/exit + canonical equality +
  freeze-counter is real; pause/step semantics, mid-play save, and
  every GUI/render switch unproven; "proven" overstates.

## 4. test_editor_viewport (Phase 31) — WEAK

- Claims: orbit math, camera derivation, unproject round-trip, picking
  hit+miss, AABB, frame-selection, gizmo intents + line soup.
- Actually executes: real `led_viewport_*/led_frame_selection/
  led_gizmo_*`, one real physics pick (unit box collider, hardcoded
  `320,240`). `lr_camera` as output struct only. Synthetic: sky-miss
  faked by moving target y=100; hand-filled gizmo world deltas;
  line-soup asserted by exact float count.
- Passes-while-broken (largest gap of the five): viewport black /
  wrong aspect / panel owns its own camera math — green. Clicking a
  visible **mesh without a collider** selects nothing (documented
  unsupported; only the collider case is tested). Broken HiDPI /
  y-flip / screen→world mapping — pre-resolved world deltas are fed
  in, so `"gizmo moved x only"` passes regardless. Garbage panel with
  the right float count passes `"gizmo lines need 90"`.
- Strongest: `"center pick hits"`, `"ray passes origin"`. Weakest:
  `"camera NULL-out validates"` (`out=NULL` still returns 1!);
  `"gizmo lines need 90"` (count, not geometry). Only
  TRANSLATE/x-axis exercised; rotate/scale untested.
- Rating **WEAK**.

## 5. test_editor_reflect (Phase 31) — PARTIAL

- Claims: reflection coverage per component, validation matrix, Euler
  round-trips, script props live enumeration, 100k enumeration timing.
- Actually executes: real `led_describe/list/find/read/write_property`
  + `led_inspect`, real component adds, genuine validation matrix
  (wrong-type, NaN, FOV 500, bad enums, read-only, bogus path,
  stale). Synthetic: one object per block, identity + 90°-X Euler.
- Passes-while-broken: inspector UI wrong labels / dropped script
  props / no refresh after add — direct calls pass. 100k outliner
  freeze — only engine-side count asserted. Euler poles, asset-ID
  types, script-row contents (only `n>=8` count) unexamined.
- The timing block is **NOT ACTUALLY TESTING CLAIM**: `t0=0; …; t1=1;
  (void)t0; (void)t1;` with `"(timing reported, not asserted)"` — a
  fake clock asserting a count, not time.
- Rating **PARTIAL** (schema-drift protection real; panel binding
  unproven; "timing" asserts no timing).

## 6. test_project (Phase 32) — PARTIAL

- Claims: manifest create/open/close/switch, path-normalization +
  escape rejection, scan + sidecar identity, rename/move preserving
  UUID, MISSING/restored, unknown files ignored, whole-project copy
  portability.
- Actually executes: real `led_project_create/open/scan/close`,
  `led_assetdb_count/find_by_path`, `led_project_rename/resolve`
  against `%TEMP%/luma32_proj*`. BUT `copy_tree()` is a test helper
  copying a hardcoded 4-file list — not the product path, not a real
  Explorer copy.
- Passes-while-broken: Browser panel never calls `led_project_scan`
  (stale view); Open-Project dialog chokes on spaces/unicode (roots
  are ASCII, no spaces); GUI rename forgets view refresh. Real copies
  carrying types outside the 4-file list (`.glb`, prefab sidecars)
  untested by construction.
- Strongest: `"copy preserves UUID"` (real close/open in a second
  session). Weakest: `"second starts empty"` (count==0) / single
  `"../escape"` string as the whole traversal proof.
- Rating **PARTIAL**.

## 7. test_import (Phase 32) — PARTIAL

- Claims: script import + transactional failed-reimport (last-good
  live), scene/prefab discovery, STALE fingerprint, reimport-all,
  browser filter/search/select/inspect/drag, delete ref-check.
- Actually executes: real `led_import_asset/led_reimport_asset/
  led_project_reimport_all/led_project_delete/led_prefab_create` +
  headless `led_browser_*` model queries; `led_drag_begin` struct
  fill; `le_object_add_script`. The "browser" is a model query, not a
  widget; the "drag" is a struct, not a gesture.
- Passes-while-broken: browser ImGui panel renders empty/wrong order
  (owns its own sorting); real GUI drag builds the payload on a
  different path (stale UUID) so `led_drop_*` fails in-app; Delete
  dialog bypasses the ref-check. `kBrokenLua` ("this is not lua")
  proves one failure shape only.
- Strongest: `"referenced script delete refused"`
  (`LED_ERROR_VALIDATION` — real dep edge) + `"good handle live"`.
  Weakest: `"inspect rows"` — `> 0`, any positive count passes.
  Runner-up: `"5 importers"` (brittle count, zero semantics).
- Rating **PARTIAL**.

## 8. test_assetdb (Phase 32) — PARTIAL

- Claims: deterministic enumeration, UUID/path lookup, type filters,
  case-insensitive search, dep edges, stats, duplicate-UUID conflict,
  malformed-sidecar isolation.
- Actually executes: real `led_assetdb_*/led_project_scan` on TEMP.
  The "dependency edge" section asserts the DB is *empty* (`== 0`) —
  proves the absence case only; a regression breaking dep
  *recording* passes this file cleanly.
- Strongest: `"duplicate UUID marks both FAILED"` (genuine corruption
  handling). Weakest: `"search order deterministic"` (only `ids[0] <
  ids[1]` on a capacity-2 query); `"unrelated record intact"` (find
  succeeds, status unchecked).
- Rating **PARTIAL**.

## 9. test_project_editor (Phase 32) — WEAK

- Claims: script drop "via the script path", material-assign type
  safety, prefab instantiate/undo/redo, play isolation, "end-to-end
  headless workflow (project → import → instantiate → prefab → save →
  play → stop)".
- Actually executes: real `led_drop_script_onto_object` with a
  **synthetic** `led_drag_payload` (no mouse/widget); "material type
  safety" stuffs the *script* asset under the MATERIAL role — a
  genuine material-asset path is never tested. The "end-to-end"
  second-session check asserts **only object-count equality** — not
  names, hierarchy, scripts, materials, or TRS.
- Passes-while-broken: GUI drag constructs the payload differently →
  real drops fail, test passes. Viewport renders garbage during play
  while serialized text stays byte-identical. Saved scene fails to
  reopen in the headed app (renderer-dependent assets) while count
  equality passes.
- Strongest: `"edit byte-identical across play"` (strict `strcmp`
  oracle, text-only). Weakest: `"object count round-trips"` as a
  scene-format round-trip proof.
- Rating **WEAK**: "drop" and "end-to-end" dramatically overstated.

## 10. test_prefab (Phase 32) — WEAK

- Claims: canonical determinism, 10-instance isolation + TRS
  independence, 8-way malformed battery, nested rejection, 1k stress,
  duplicate-load identity.
- Actually executes: real `led_prefab_create/load/instantiate` on
  TEMP. BUT determinism is gutted: strips **all** identity lines,
  splits blocks at `end`, **`qsort`s the blocks** — proves "same
  multiset of de-identified blocks", hiding ordering bugs, ID
  collisions, sibling-order nondeterminism *by construction*. The 1k
  "stress" frees each instance inside the loop yet asserts `count ==
  base + 1000*2` live — 1000 tracked-live instances never coexist;
  `secs < 30.0` as "non-blocking" is a joke threshold for a UI claim
  with no UI thread measured.
- Strongest: malformed battery 8/8 + `"world kept across malformed
  battery"` (genuine transactional safety — trusted-grade).
  Weakest: `"canonical shapes deterministic"` (after strip-all-IDs +
  sort, nearly tautological).
- Rating **WEAK** (malformed battery excluded: that section is
  trusted-grade).

## 11. test_prefab_smoke (Phase 32) — WEAK

- Claims: create → load → instantiate (×2) → undo/redo → assign
  guards; "engine WITHOUT renderer (script-free scene)".
- Narrowest honest scope of the set. `"hero name resolves"` calls
  `find_by_name("hero")` when **three** heroes exist — any match
  passes; instance integrity unverified.
- Strongest: `"world grew by 2" / "undo removed instance" / "redo
  restored instance"`. Weakest: `"prefab magic in text"`
  (`strstr(txt,"LUMA_PREFAB 1")` — true if anything was written).
- Rating **WEAK**: honest smoke, but smoke is all it is.

## 12. test_gltf_identity (Phase 32) — FALSE POSITIVE (by SKIP) / WEAK (when run)

- Claims: stable reimport identity; sub-asset key vocabulary round-
  tripping through the sidecar.
- Actually: **on any machine without a Vulkan device it executes ZERO
  assertions and exits 0** — `SKIP_ENV` prints `SKIP:` and `return
  0`, which CTest scores as pass. When a GPU exists: real reimport of
  one staged `BoxTextured.glb`, UUID/sub-count/runtime-ID stability.
  The advertised "key vocabulary" check **does not exist** — no
  assertion inspects a single sub-asset key string; only
  `sub_asset_count >= 2` and count equality.
- Passes-while-broken: headless CI green with no GPU while glTF import
  is 100% broken; material reorder scrambles IDs (documented debt);
  GUI Import fails; any non-Box model fails; viewport renders wrong.
- Strongest: `"representative runtime ID stable"` after forced
  reimport. Weakest: `"mesh+material sub-assets"` (`>= 2`) — or the
  SKIP line, a pass with zero checks.
- Rating **FALSE POSITIVE** via the SKIP path; WEAK even on-GPU.
- Repair (Phase 33V, done): `LUMA_REQUIRE_GPU=1` env makes SKIP a
  hard failure; CI must set it on GPU runners. Header comment fixed
  to state the key-vocabulary gap.

## 13. editor_demo_phase33 (Phase 33) — NOT ACTUALLY TESTING CLAIM

- Claims (comments): "the same workflow the desktop app drives via
  GUI"; "the GUI Save path"; "drag-drop equivalent"; "proves the demo
  scene's scripted object from the Play side."
- Actually: a **demo-content builder**, not a test — headless `led_*`
  calls writing CWD-relative `editor/demo/Phase33Demo/`. Fatal to its
  own story: it detaches the script (`le_object_remove_script`) "for
  portable save" — **the shipped demo scene contains no script**, so
  the "Play oracle" (one 1/60 s tick + byte-identical on a
  now-scriptless world) passes trivially and proves nothing about the
  "tiny orbit script".
- Passes-while-broken: app fails to open Phase33Demo; viewport black;
  GUI Save broken; Orbit never ticks in real play — builder still
  prints `DEMO BUILD OK`.
- Strongest: `"save Main scene"` (produces a portable fixture).
  Weakest: `"drop Orbit onto Spinner"` followed by detaching it; the
  de-scripted "oracle".
- Rating **NOT ACTUALLY TESTING CLAIM** — fixture generator misread
  as workflow/GUI proof.
- Repair (Phase 33V, done): header/output relabeled FIXTURE; CTest
  name kept for continuity but documented as fixture, not proof.

## 14. test_editor_gui (Phase 33) — PARTIAL, with one NOT-TESTED section

- Claims (header): "input ownership defaults, clip->scissor clamping,
  draw-walk budget caps" + NULL-context refusal.
- Actually: pure math/NULL guards — `leg_context_create` NULL
  refusal, `leg_clip_to_scissor` full/partial/negative/overhang/
  outside/inverted/zero/NaN/Inf, `leg_draw_budget` exact + 64 MiB
  caps. **There is NO input-ownership test at all**: no positive
  `leg_feed_event` case, no `leg_wants_keyboard/mouse` TRUE case —
  only NULL probes. The header's first claim ("input mapping") is
  untested.
- Passes-while-broken: any `leg_feed_event` mapping bug (wrong key,
  dropped modifiers), any `wants_keyboard` inversion breaking typing-
  vs-play routing — all invisible. `leg_clip_to_scissor` with NULL
  out "still reports nonempty" is a probe of nothing.
- Strongest: NaN/Inf clip rejection + 64 MiB budget caps (real,
  would catch GPU-buffer overrun math). Weakest: NULL-probe padding.
- Rating **PARTIAL**; the "input ownership defaults" section is
  **NOT ACTUALLY TESTING CLAIM**.
- Repair (Phase 33V, planned): positive feed-event/wants-keyboard
  cases; see mutation §16.

## 15. test_editor_gui_gpu (Phase 33) — PARTIAL (strongest Phase-33 file, still not a GUI proof)

- Claims: font/blend/scissor draw-walk, gizmo funnel, play/undo/redo
  "through the GUI command funnel", 1k Play/Stop stress, scene-switch
  oracles — with validation layers ON, 0 ERROR tolerance.
- Actually (verified by reading): real device + real native window +
  real `leg_context`, `leg_panels_frame` + `leg_frame_end` with
  nonzero draw stats, `leg_record_gui` into an offscreen pass with
  scissor-restore assertion, synthetic `led_gizmo_begin/apply`
  (+1 X, undo), `led_execute` CREATE + renderer-free play + canonical
  equality + undo/redo, 1k enter/tick/exit, save-as/new/open/missing/
  revert oracles. The debug note about the present-legality bug is
  evidence of a REAL bug caught here — this test does genuine work.
- Gaps (all verified): (a) "gizmo funnel" bypasses the overlay mouse
  path — pre-resolved world deltas via `led_gizmo_apply`, so overlay
  screen-mapping bugs pass. (b) "GUI command funnel" is `led_execute`
  directly, not panel buttons. (c) `leg_panels_frame` runs with **no
  input events ever fed** — no click, key, or drag is exercised.
  (d) nonzero draw stats prove panels emitted *something*, not
  correct layout. (e) engine created WITHOUT renderer, so
  `leg_viewport_composite` — the actual viewport path — **never runs
  in any test** (header admits "SKIPPED here").
- Passes-while-broken: overlay screen→world mapping wrong; toolbar
  Play/Undo buttons disconnected; panel layout garbage but nonzero;
  viewport composite black — all green.
- Strongest: blended+scissor walk under validation + scissor restore
  + 1k isolation oracle. Weakest: `"draw stats nonzero (panels
  emitted)"` as a panels proof.
- Rating **PARTIAL** — genuine GPU-walk + isolation proof; not a GUI
  interaction proof; viewport composite untested anywhere.

## 16. Mutation verification (Phase 33V) — all performed live 2026-09-17

Each row was executed for real: source mutated → rebuilt via
`cmake --build build --config Debug --target <t>` → test binary run
→ observed FAILs recorded → source restored from backup → touched,
rebuilt, re-run green → `git status` clean of mutants. (One process
lesson: MSBuild skips recompile when a restored file's mtime predates
outputs, so every restore was followed by an explicit timestamp touch
+ rebuild + green re-run.)

| # | Feature broken | Test | Observed result |
|---|---------------|------|-----------------|
| M1 | `leg_clip_to_scissor` forced full-rect passthrough (floor/ceil/clamp removed) | test_editor_gui | **FAILED as expected** (6 FAIL: "clip partial floor/ceil", "clip negative clamped", "clip overhang clamped", "clip outside 0", "clip outside zeroed", "clip inverted 0"; 27 passed) → restored → 33/33 green |
| M2 | translate gizmo X delta doubled (`p[0]+delta[0]*2`) | test_editor_viewport, test_editor_gui_gpu | **FAILED as expected** in both: viewport 2 FAIL ("gizmo moved x only", "gizmo snapped"), gui_gpu 1 FAIL ("gizmo moved exactly +1 X") → restored → 69/69 and 52/52 green |
| M3 | `led_undo` pops the entry but never applies the inverse | test_editor_history | **FAILED as expected** (16 FAIL incl. "zero after undo", "name reverted", "position reverted", "both restored", "restored TRS", "drag fully reverted", "back at origin") → restored → 145/145 green |
| M4 | `led_viewport_orbit` yaw wrap removed | test_editor_viewport | **FAILED as expected** (2 FAIL: "yaw wrapped", "NaN orbit ignored") → restored → 69/69 green |
| M5 | `leg_draw_budget` cap halved (64→32 MiB) | test_editor_gui | **FAILED as expected** (2 FAIL: "budget vertex capped 64MiB", "budget index capped 64MiB") → restored → 33/33 green |
| M6 | `leg_viewport_composite` gutted to clear-only (no `le_world_render_scene/output`) | test_editor_gui_gpu (+ full suite) | **STILL PASSED 52/52 — confirmed hole**: no test covers composite output |
| M7 | toolbar Play button disconnected (status text only, no `led_play_enter`) | ALL suites | **STILL PASSED — confirmed hole**: no test drives panel callbacks |

M6/M7 are kept as documented gaps with headed-screenshot coverage
instead of fake unit proofs (see docs/verification/phase33v/).

## 17. Known failure: project relocation breaks mesh identity (found live 2026-09-17)

Reproducer (executed, not theorized): `editor/demo/Phase33V/` copied
to `editor/demo/Phase33VConsole/` (static files only), opened,
`led_import_asset("Assets/Crate.glb")` → SUCCESS, then
`led_scene_open(.../Phase33VConsole/Scenes/Visual.luma_scene)` →
`LED_ERROR_PARSE` (1014), world empty. The identical bytes open fine
in place.

Root cause (verified in source): `le_gltf_import` mints mesh/material
runtime IDs as `fnv(normalized_source_path)` (`engine/src/
gltf_bridge.c`: `want.hi = le_fnv1a64(normalized, ...)`), and the
project layer passes the project-root-joined path (`editor/src/
project/import_impl.c`: `le_gltf_import(e, abs, &imp)`). Scene files
persist those IDs (`renderable <mesh-id> <material-id>`). Move, copy,
or rename the project directory (or open it via a differently-spelled
root/CWD) → different IDs → `MISSING_ASSET` at instantiate →
scene open fails. `test_project`'s "copy preserves UUID" covers only
the project UUID — no mesh-bearing project is ever copied in any
test, so the suite is green around a broken workflow.

Status: DOCUMENTED KNOWN FAILURE, not fixed in Phase 33V. A correct
fix (location-independent mesh identity: hash project-relative path
or content for the identity key, keeping the absolute path for file
access) changes engine asset identity + sidecar sub-ID + scene-ref
contracts — cross-layer redesign, Phase-34-or-later scope. No test
was added pinning either behavior: pinning the bug as PASS is
forbidden; a WILL_FAIL dance would obscure the suite. The console-
diagnostic evidence used a freshly-built (not copied) project for
exactly this reason.

## 18. Phase 33V test repairs (done)

- `test_gltf_identity`: `SKIP_ENV` now honors `LUMA_REQUIRE_GPU=1`
  (SKIP becomes a hard failure); header fixed (no "key vocabulary"
  claim — only count + representative-ID stability is asserted).
- `editor_demo_phase33` (`phase33_builder.c`): header relabeled
  FIXTURE (not a test); behavior unchanged (still builds the demo).
- `test_editor_gui`: header fixed (never tested input mapping; the
  positive `leg_feed_event` matrix now lives in
  `test_editor_gui_gpu`: 9 new checks — key/mouse/wheel/char
  acceptance, focus-lost + control-char refusal).
- `test_editor_reflect`: the fake-clock "timing" block now measures
  with `lc_clock_now` (100k enum: ~2.2 ms on this machine, reported
  not asserted).
- NEW `test_editor_negative` (43 checks): bad project/scene paths,
  reparent cycle + self-parent rejection, stale-handle ops, missing
  prefab, play/save guards, empty drop payloads — all must fail
  cleanly with state preserved.
- Core repairs the new tests + headed runs forced:
  `led_scene_open` adopts the first camera when none is active
  (black viewport after open); `led_play_enter` carries the edit
  world's active camera into the runtime world (black viewport
  during Play).

## 19. Summary counts (16 editor binaries + 1 fixture)

- TRUSTED: 0 (no binary is trusted as *visible-GUI* proof; headed
  evidence in docs/verification/phase33v/ covers that axis instead)
- PARTIAL: 10 (core, history, play, reflect, project, import,
  assetdb, editor_gui, editor_gui_gpu, editor_negative — the last is
  PARTIAL only in the GUI-proof sense; as failure-path coverage it
  is solid)
- WEAK: 4 (viewport, project_editor, prefab, prefab_smoke)
- FALSE POSITIVE: 1 (gltf_identity, via SKIP-exit-0 by default;
  WEAK on-GPU; hard-fails with LUMA_REQUIRE_GPU=1)
- NOT ACTUALLY TESTING CLAIM: 1 fixture (editor_demo_phase33)

No test is kept under a misleading name without a repair entry in
§18. Phase-31/32 headless suites are kept for their real
model-layer value and are no longer quoted as GUI/viewport proof.
