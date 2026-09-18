# Engine Reality Ledger — LUMA RECOVERY PHASE (live verification)

Method: every LIVE verdict below comes from running the REAL
production path (real editor binary, real GPU, real project open,
real scene open) and inspecting pixel output. Automated suites are
SECONDARY (noted per row, never the headline).

Environment: Windows 11, NVIDIA RTX 5060, Vulkan 1.4.357,
1280×800 headed frames, Debug + Release builds `build/recovery`.

Acceptance project: `LumaRealityTest/` (built ONLY through the
public led/le API via `editor/demo/recovery_project.c` → target
`editor_recovery_project`; never hand-written scene bytes):
CameraRig/Camera (aimed: pitch −0.35 rad) + Sun (dir + shadows) +
PointLight + Ground (static) + StaticCube + DynamicCube (dynamic,
starts 3 m up) + Player (character + CharDrive) + Npc
(skinned.glb) + TrigZone (trigger + Trig.lua) + Mover (box.glb +
Mover.lua). 11 objects, `report submitted=4 dis=0 inv=0 dead=0
draws=4 tris=38`.

## Reality matrix

| System | Live | Automated | Visual | Status |
|---|---|---|---|---|
| LumaC window/device/swapchain | app + examples open real windows | test_window etc pass | window visible on screen | LIVE PASS |
| LumaC triangle | R-005 offscreen pixel proof (center 152,116,102 vs clear 0,0,0; 11858 non-clear px) | test_triangle_vulkan PASS (frame counts only — WEAK) | `05-triangle-rt.ppm` dumped + censused | LIVE PASS |
| Textured rendering | textured checker crates render in editor viewport (red 255,8,38 pixels in shot) | test_textured_vulkan PASS | `17-game-aimed.png` viewbox | LIVE PASS |
| 3D/depth | crates at 3 depths (−2/0/+2, y 0.5/3/1) composite with depth | test_cube_vulkan + test_3d PASS | same shot | LIVE PASS |
| glTF import (v2 keyed) | box.glb + skinned.glb import in real editor, sidecars `mat0:checkerred` / `mat0:skinflat`, stable across launches | test_portable_identity + test_gltf_identity PASS | sidecar bytes inspected | LIVE PASS |
| Scene open (Game) | `import-all: 6 ok`, `objects=11`, report 4/0/0/0 | test_scene + test_scene_vulkan PASS | `17-game-aimed.png` | LIVE PASS |
| Scene open (Visual fixture) | regenerated Visual opens in probe AND app (`import-all: 3 ok`, `objects=5`) | — | `15-visual-report.png` | LIVE PASS |
| Commit `16afba6` Phase33V fixture | scene open FAILED (`LED_ERROR_PARSE`) on every machine incl. author's | — | stale v1 hexes match no import | BROKEN (fixture, not engine) — replaced |
| Model drop into scene | `led_drop_model_into_scene` via real payload (Static/Dynamic/Mover/Npc drops in builder) | H-drop headed 114-suite | drops in reopened scene | LIVE PASS |
| Directional light toggle | Game vs Game.nosun: viewbox diff 779/3268 px, mean R −34 (sun removal darkens) | covered in scene suites | `17-game-aimed.png` vs `19-game-nosun.png` | LIVE PASS |
| Point light | intensity-10x close light visibly warms crates (598/4141 px diff, red 255,8,38 cluster grows); small lateral moves at range 12 are sub-threshold in this dark scene (0 px) — honest sensitivity note, not a failure | — | `pass2/point-close.png` vs `pass2/point-left.png` | LIVE PASS (with noted sensitivity floor) |
| Shadows | Shadow scene ON vs OFF: footprint-only delta (viewport diffrac 0.0095, OFF +0.39); zero-bias mutation darkens whole ground (0.2266, +8.13); caster/light moves reshape; lab edge 1px at 512/1024/2048; ON repeat 0.0308 | test_shadow + test_shadow_vulkan + test_r011_probe PASS | `pass2/r011/editor-shadow-on.png` vs `-off.png`/`-zero.png`, `after/r011-on.ppm`, full log `pass2/r011/R-011-LOG.md` | LIVE PASS |
| Shadow mutation | — | MUT_COMPOSITE arm kills H-comp leg (verified in matrix); shadow-caster disarm proven live by R-009 ON/OFF-identical pair | R-009 pair | LIVE PASS (by live disarm + arm) |
| PBR/materials | readbacks 20/20 + grid shot banked (`pass2/pbr-grid.png`); endurance camera orbits away by frame 500 so the banked frame is mostly clear — readback legs are the proof, grid framing is a known-weak capture | test_pbr + test_pbr_vulkan PASS | `pass2/pbr-grid.png` (weak framing, noted) | LIVE PASS (readbacks) / PARTIAL (framed grid capture) |
| IBL/environment | IBL-off A/B: attached mean (172,147,131) → detached pure black (0,0,0, 1 color), stats `ibl=1→0 sky=1→0` | test_ibl + test_ibl_vulkan PASS | `pass2/tonemap-aces-ev0.png` vs `pass2/ibl-off.png` | LIVE PASS |
| Tonemapping | `--exposure`/`--tonemap` A/B on HDR sky: EV0 mean (172,147,131) → EV+2 (236,226,218); NONE (173,139,102, 4017 colors) vs ACES (3258) | test_ibl_vulkan ACES-CPU + EV-monotonic legs PASS | `pass2/tonemap-*.png` | LIVE PASS (renderer-public path; NO editor/exposure UI exists — NOT IMPLEMENTED there, not broken) |
| Scene graph/transforms | rig→camera parenting persists (`parent a4b9…`), positions/rotations round-trip byte-identical | test_scene PASS | scene text inspected | LIVE PASS |
| Save/reload round-trip | open never mutates the scene file (diff empty); builder save→reopen `objects=11`; Shadow reopen byte-identical hash | H-id headed legs | file diff + hash | LIVE PASS |
| Import-all convergence | clean launch: all 7 UNIMPORTED (fp match disk), `import-all: 7 ok` is CORRECT warmup; sidecars byte-stable across launches E/F | — | probe dump + sidecar diff | LIVE PASS (with CRLF-checkout footnote below) |
| Components | renderable/script/character/collider/rigid_body/light/shadow/camera rows persist + instantiate | test_scene PASS | scene text | LIVE PASS |
| Lua scripts | lua_scene live: 4 scripts tick, error_demo fails frame 3 while spin/move/orbit keep running | test_script + test_script_vulkan + animation/input/physics_script PASS | console error text | LIVE PASS |
| Lua in Game | Mover.orbit + CharDrive W/S + Trig.on_trigger_enter attached via real drop path | — | attach PASS lines in builder log | LIVE PASS (attach) |
| Input | W flies camera (H10), real inject_event queue | test_input + test_input_vulkan + test_input_script PASS; lua_input example | headed H10 | LIVE PASS |
| Play/Stop isolation | `--play` enters play, play shot differs 584/2800 px in viewbox (Mover moved), exit clean | H1/H2 headed + test_editor_play | `18-game-play.png` | LIVE PASS |
| Editor viewport environment | EDIT paints slate sky gradient + infinite grid (top-strip luma 34.4 vs PLAY 0.0); headed env-on records sky+grid (stats 1/0, luma 18), env-off skips (stats 0/1), play forces off with prefs ON (stats 0/1, luma 0); full suite 94/94 | test_editor_headed R-012 legs PASS | `pass2/r012/02-shadow.png` vs `06-play.png`, `01-empty.png`, full log `pass2/r012/R-012-LOG.md` | LIVE PASS |
| Physics fall/collide | lua_physics live: 5 submitted, 6 bodies, contacts 1→3 over 60 frames, `[drop] first contact` | test_physics + test_physics_vulkan + test_physics_script PASS | stats + script print | LIVE PASS |
| Colliders (box/sphere/capsule) | Game carries static/dynamic/trigger boxes; trigger flag persists | test_physics + test_cast + test_capsule PASS | scene text | LIVE PASS |
| Triggers | TrigZone `is_trigger=1` + Trig.lua counter attached; lua_physics zone counts overlaps | test_physics_script PASS | builder log | LIVE PASS |
| Raycast/shape-cast/CCD | picking core shared with raycast; headed H5 selects BoxA by pixel | test_cast + test_ccd PASS | headed H5 | LIVE PASS |
| Character | Player row persists (radius 0.4, height 1.7, slope 45°); CharDrive attached | test_character + test_character_platform + test_character_script PASS | scene text + builder log | LIVE PASS (persist+attach) |
| Character visual | Npc skinned.glb renders (4th draw); stairs/slope/platform NOT staged in Game | test_character_platform PASS | — | PARTIAL (movement proofs live in suites, not in Game) |
| Animation | skinned.glb imports (mesh0:prim0/prim1:meshnode); lua_animation live: 2 animators playing, joints=2, `[conductor] playing wave` | test_animation + test_animation_vulkan + test_animation_script PASS | stats + script print | LIVE PASS |
| Animation crossfade/char-integration | — | covered in animation suites | NOT staged in Game | PARTIAL |
| Browser/assets panel | asset rows probe live (H-drop row @(323,628)) | test_assetdb + test_import PASS | headed | LIVE PASS |
| Prefabs | — | test_prefab + test_prefab_smoke PASS | NOT staged live | PARTIAL |
| Undo/redo | Ctrl+Z/Y + menu Undo/Redo click all rename BoxB through real queue | H3/H4 + menu legs, test_editor_history PASS | headed `06-save-clean.ppm` | LIVE PASS |
| Gizmos | translate drag moves BoxA (H6), rotate/scale legs | headed H6/H7 legs PASS | headed | LIVE PASS |
| Picking | H5 click selects BoxA by pixel; `--select StaticCube` prints `selected` | headed H5 PASS | app stdout | LIVE PASS |
| Camera | orbit/pan/dolly + fly (H10 W-flies), frame-selection, active-camera carry into play | test_editor_viewport PASS | headed | LIVE PASS |
| Console | script errors mirror to console + stderr (`attempt to call nil 'boom'`) | test_editor_gui PASS | lua_scene output | LIVE PASS |
| Integrated game flow | Game: 11 objects, play ticks scripts+physics, Mover visibly displaces (edit-vs-play diff) | headed 114/114 | `17` vs `18` | LIVE PASS (basic) |
| Clean Debug suite | 94/94 PASS (`build/recovery`, 75 s) | — | — | LIVE PASS |
| Clean Release proofs | 8/8 targeted (headed, scene, physics, animation, script, shadow, pbr, ibl) | — | — | LIVE PASS |

## Defects found live (recovery log, pass 2 appends R-008+)

### R-008: model drop attached to the wrong object once a parented child exists (P1, FIXED)
- Subsystem: editor drop path (`editor/src/project/browser.c`,
  `led_drop_model_into_scene`).
- How discovered: Shadow scene built via the builder staged
  ShadowGround/ShadowCube drops AFTER the ShadowCam child
  existed — the saved scene had the renderable + camera on ONE
  object, the rig rotation on the cube, and two empty objects.
- User-visible symptom: any GUI drag of a model into a scene
  with a parented object (cameras always) lands the mesh on
  the wrong object.
- Root cause: newborn found by "live tail"
  (`all[got-1u]`); children sort with all live objects, so the
  camera child (higher slot) shadowed later root drops.
- Regression: headed H-drop leg still passes (no-parent case);
  needs a parented-child drop leg (queued).
- Mutation sensitivity: n/a (new path mirrors the
  census-diff discipline `led_execute` already uses).
- Fix: snapshot the live set BEFORE the CREATE, take the
  census diff after (exact under slot recycling).
- Live re-verification: Shadow scene regenerates correct
  (rig/cam/cube/ground/sun, `1 1 1` flags), reopens, renders.

### R-009: dropped models opted out of shadows (P1, FIXED)
- Subsystem: same drop path (desc defaults).
- How discovered: Shadow ON vs OFF screenshots byte-identical
  (0/21931 px) with a shadow light + `shadow 1` in the scene.
- User-visible symptom: NO editor-assembled scene can ever
  show cast shadows (every GUI drop zeroes the flags the
  format documents as opt-in).
- Root cause: zeroed `le_asset_renderable_desc` leaves
  `casts_shadow=receives_shadow=0`; the renderer honors the
  flags, so nothing submits to the shadow map.
- Regression: shadow ON/OFF pair IS the regression
  (`pass2/shadow-on.png` vs `pass2/shadow-off.png`:
  16957/21931 px, OFF +39 brightness).
- Mutation sensitivity: proven by the pre-fix identical pair
  (disarmed pipeline) vs post-fix differential.
- Fix: drops set `casts_shadow=receives_shadow=1` (opt-out
  stays per-object via inspector).
- Live re-verification: ON/OFF + caster-move + light-move +
  reopen proofs, all green.

### R-011: shadow rendering visibly incorrect — linear-filtered depth + zeroed scene biases (P0 hotfix, FIXED — READY FOR HUMAN SHADOW REVIEW)
- Subsystem: renderer shadow sampling (`renderer/src/renderer.c`) + Shadow
  acceptance scene content (`LumaRealityTest/Scenes/Shadow.luma_scene`) +
  uninspectable shadow config (no reflect rows, no report columns).
- How discovered: R-011 hotfix order — prior ON/OFF differential proved
  only that shadow code changes pixels, NOT correctness; the complaint
  (large blurred/diffuse rectangular darkening, no convincing cast shadow)
  reproduced in tenue.
- User-visible symptom: whole ground half-shadowed with dimmed checker
  whites instead of a hard cast shadow under the cube.
- Root causes (two): (1) depth maps sampled through the shared LINEAR
  sampler — bilinear blending of comparison data at every PCF tap widened
  the penumbra (fixed: dedicated NEAREST `shadow_sampler` at shadow-set
  binding 5; lab edge moves 1px, kept as correctness hygiene). (2) The
  committed scene shipped explicit-zero biases (`shadow 1 1024 0 0 ...`),
  which DISABLE both mitigations (negative = compiled defaults 0.0015/0.02)
  — whole-ground acne (lab zero run `diff frac 0.7090`; editor zero
  mutation `difffrac 0.2266`, whites 229->178). Fixed with the one-line
  scene change `-1 -1` (byte patch preserving UUIDs; builder fixed to match).
- Audit kept as-is: PCF is per-tap compare-then-average (correct),
  `compareEnable` always FALSE (manual PCF only), UV 0..1 / depth 0..1 /
  outside-frustum-lit conventions correct, 3x3 kernel over 1-texel steps,
  25 m default frustum, depth-map stats real content (min 0.202774, max 1.0).
- Regression: lab probe `test_r011_probe` (screen edge 1px at 512/1024/2048,
  footprint ratio 0.0, depth 0..1) + full Debug suite 94/94 PASS (75.19 s).
- Mutation sensitivity: zero-bias (0.2266), caster-move (0.0251, cube visibly
  displaces), light-move (0.2038) — every mutation moves pixels in the
  expected direction.
- Reopen anomaly closed: mid-work 10.5% reopen-vs-on delta was the stale
  committed `0 0` bias line re-read by the "reopen" run (reopen matched the
  zero mutation pixel-for-pixel, checker phase identical) — NOT an
  exposure/ambient/texture race. Post-fix ON vs repeat `0.0308`.
- Evidence + full per-section log: `docs/verification/recovery/pass2/r011/`
  (`R-011-LOG.md`, lab `after/` + `sweep-*/`, six editor PNGs).
- Human review asked: open `-on.png` vs `-zero.png` (hard shadow + clean
  whites vs half-shadowed ground); `-off.png` differs from ON only inside
  the cast footprint; lab `after/r011-on.ppm` shows the 1px edge on
  untextured grey. Remaining shape/placement questions are artistic
  direction (light angle, intensity 3, 1024 res), not pipeline correctness.
### R-012: professional editor viewport environment — black-void edit viewport (P1 hotfix, COMPLETE — READY FOR HUMAN REVIEW)
- Subsystem: renderer scene pass (`renderer/src/renderer.c`,
  `renderer/src/environment.c`, `renderer/shaders/editor_*`) + GUI
  viewport bridge (`editor/src/gui/gui_viewport_tex.cpp`) + View menu
  prefs (`editor/src/gui/gui_panels.cpp`).
- How discovered: R-012 hotfix order — the edit viewport cleared to
  black (HDR clear) with no authored environment in the test scenes;
  an empty scene read as a dead window, not a workspace.
- User-visible symptom: black void behind/around all edit content;
  empty starter scene (rig + cam + sun + Ground cue) showed nothing
  but chrome.
- Fix: renderer-owned procedural sky (3-zone linear-HDR gradient,
  fullscreen triangle, depth off, first in pass) + infinite grid
  (y=0 plane, 1 m / 10 m, fwidth AA, 60 m fade, depth-tested writes-off
  blended, last in pass). `lr_editor_env` default OFF; the GUI bridge
  enables it ONLY around the EDIT-world composite and forces (0,0)
  for every play composite (shared renderer). Authored sky takes
  precedence. Prefs are GUI-only (View menu, ON/ON), never serialized.
- Proof: EDIT top-strip luma 34.4 (gradient (14,18,29)→(39,47,61))
  vs PLAY 0.0 (game black); grid row-oscillation stdev 80–91 over
  ground vs 0.3 clear; headed stats env-on sky 1/0 grid 1/0, env-off
  0/1, play-with-prefs-ON 0/1; app reports unchanged (Shadow
  `submitted=2 draws=2 tris=24 shadow_maps=1` — R-011 proof
  uncontaminated); full Debug suite 94/94 PASS.
- Evidence + full per-section log: `docs/verification/recovery/pass2/r012/`
  (`R-012-LOG.md`, `01-empty/02-shadow/03-game/06-play` app PNGs,
  `04/05/07` headed PNGs).
- Human review asked: open `02-shadow.png` vs `06-play.png` (slate
  gradient + grid under the checkerboard with the hard R-011 shadow
  vs game black over the same geometry); `01-empty.png` should read
  as a quiet workspace. View → Editor Environment / Grid toggle the
  tooling (defaults ON/ON).
- R-010 (withdrawn — CRLF checkout artifact, NOT an engine defect)
- Symptom seen: `Game.luma_scene.luma` fingerprint flipped
  `2778 b323…` → `2891 1cd9…` on every open; looked like the
  R-007 loop persisting.
- Investigation: working file is 2891 bytes / 113 CR pairs;
  committed blob is 2778 bytes (113 = line count). FNV over
  LF-normalized bytes = EXACTLY `b3236c164a2d37b1`.
- Cause: `* text=auto` checks the scene out CRLF on Windows
  while the committed sidecar fingerprints LF bytes. The scan
  correctly reports drift; the "loop" is git inflating the
  file under the engine, one flip per fresh checkout (stable
  afterwards — launches E/F byte-identical).
- Action: NO production change (the stashed scan-write was
  reverted). Follow-up: `.gitattributes` LF pinning for
  `*.luma_scene` (+ regen sidecars) so fingerprints are
  checkout-stable. Queued, not done in this pass.
- Lesson kept in the verdict: user-visible state must match
  the bytes the engine actually hashed.

### R-001: dedup import path drops sub-asset key strings (P1, FIXED)
- Subsystem: engine glTF bridge (`engine/src/gltf_bridge.c`).
- Claimed previous status: portable identity "59/59 green".
- Actual observation: first import persists `mat0:checkerred`;
  any reimport through a live registry (the dedup
  `load_nodes_only` path) returned handles with NULL key arrays,
  so the project layer fell back to `mat0`. Key vocabulary
  NOT stable across reimport — the 59-check suite never
  reimported through a warm registry twice in one process.
- Root cause: `load_nodes_only` populated mesh/material/node
  arrays but never `mesh_keys`/`material_keys`.
- Fix: re-derive key strings from the metadata model in the
  dedup path (same sanitize vocabulary as full import).
- Regression proof: recovery probe (first `mat0:checkerred`,
  reimport `mat0:checkerred`); needs a permanent test
  (queued: warm-registry reimport key-stability test).
- Live proof: regenerated sidecar bytes + reopen shots.

### R-002: `--import-all` skipped STALE records (P1, FIXED)
- Subsystem: editor app (`editor/app/main.c`).
- Claimed previous status: batch warmup path (untested claim).
- Actual observation: project whose sidecars all read STALE
  (v1→v2 importer bump) printed `import-all: 0 ok`; scene open
  then failed with `LED_ERROR_PARSE` (empty registry).
- Root cause: sweep filtered `status != UNIMPORTED`, ignoring
  STALE (fresh process = empty registry = STALE must upload).
- Fix: sweep UNIMPORTED + STALE (+ READY for dead-handle
  demotion case).
- Live proof: `import-all: 3 ok` (Visual), `import-all: 6 ok`
  (Game), scenes open.

### R-003: Phase33V fixture committed stale (P1, REPAIRED)
- The `Visual.luma_scene` scene refs (`3e9f…`, `4dd6…`) match NO
  import on ANY machine (v1 location-bound IDs from the
  committer's box; the sidecar's own `sub` lines disagree).
- The scene-open failure is CORRECT engine behavior
  (unresolvable refs must fail, not invent geometry).
- Repair: regenerated the fixture through the v2 pipeline
  (same layout: Sun + CameraRig/Camera + CrateA/CrateB) —
  new scene refs match minted IDs, reopen verified in-probe
  AND in-app.

### R-004: Game scene camera aimed at nothing (P2, FIXED)
- Was filed as "viewport clear is black, not (10,13,23)".
- Actual root cause (found via `--report` + bisect): the
  Game builder authored CameraRig with IDENTITY rotation at
  (0,6,14). The scene camera stares down −Z; all content sits
  at z ≤ 5 BEHIND it. The renderer correctly submitted
  (`submitted=4 dead=0`) and correctly painted nothing (clear
  color in the target). The headed suite + engine_scene never
  hit this because they pitch the rig −0.35 rad.
- Fix (acceptance project only, no engine change):
  `editor/demo/recovery_project.c` parents Camera→CameraRig
  then aims the rig (yaw 0, pitch −0.35, persisted as
  `rotation -0.174… 0 0 0.985…`). Red checker crates now
  visible in the viewport (`17-game-aimed.png` viewbox:
  255,8,38 red + 255,255,255 selection text).
- Residual note: the (10,13,23)-vs-(0,0,0) background question
  is answered — the screenshot path re-records the GUI over a
  cleared offscreen target; the composite census target (not
  the PNG) is where (10,13,23) applies. No engine defect.

### R-005: Lua scripts staged with a table idiom the bindings never supported (P1, FIXED)
- `self:position()` returns THREE numbers (x,y,z);
  `self:set_position(x,y,z)` takes three numbers. The
  recovery builder staged `local p = self:position(); p[1] =
  …; self:set_position(p)` — `p` is a number, so play ticks
  errored (`attempt to index a number value`).
- Fix: `local x,y,z = self:position()` … 
  `self:set_position(x,y,z)` in Mover.lua + CharDrive.lua
  (builder source AND acceptance project).
- Live proof: `--play` runs clean; edit-vs-play viewbox diff
  584/2800 px (Mover displaced); lua_scene error-isolation
  leg still green.

### R-006: default-shadow scenes failed to reopen (P1, FIXED)
- The writer emits `shadow 1 0 0 0 0 0 0` for a default shadow
  config (resolution 0 = renderer default 1024, negative bias
  = default); the reader demanded res ∈ [128,4096] pow2, so
  EVERY scene with a default shadow light failed
  `LED_ERROR_PARSE` on reopen (the Game scene: Sun).
- Fix (`engine/src/serialize.c`): accept `res == 0` (default).
- Live proof: Game saves, closes, reopens `objects=11`.

### R-007: sidecar fingerprints never persisted after import (P1, FIXED)
- `led_import_one_record` wrote the sidecar with the SCAN-time
  fingerprint; `led_reimport_asset`'s script fastpath refreshed
  fp in memory but never wrote the sidecar. Next open re-read
  stale fp → STALE → `import-all: N ok` every launch forever.
- Fix: fingerprint the bytes JUST imported before
  `led_sidecar_write_pub` (both paths).
- Live proof: sidecars byte-stable across launches
  (diff empty), scene file untouched by open.

## Test-trust findings
- `test_triangle_vulkan`: WEAK — counts presented frames, never
  reads a pixel (a blank-clear loop passes). R-005 probe
  (`build/recovery_tri.c`, offscreen target + center-pixel
  assert) is the replacement pattern; needs promoting to a real
  test.
- `test_portable_identity` 59/59: WEAK — never exercised
  reimport through a warm registry in one process (missed
  R-001). Replacement queued (warm-registry key-stability).
- `editor_demo_phase33` + old Phase33V shots: WERE MISLEADING
  as workflow proof (fixture never opened — R-003). Fixture
  regenerated; headed suite stays the workflow proof (114/114).
- New `--report` app flag (production
  `le_world_get_last_render_report`, observe-only) is what
  cracked R-004: `submitted=4 dead=0` proved the renderer was
  innocent and the camera guilty. Keep it as CI discipline.

## What was NOT live-verified (honest gaps, pass-2 update)
- Framed PBR grid capture (readbacks green; banked frame
  mis-framed by the orbiting endurance camera).
- Prefab instantiate live in-editor (suites green only).
- Character stairs/slope/platform traversal inside Game
  (platform suite green; not staged in the acceptance scene).
- Animation crossfade inside Game (animation suites green).
- `.gitattributes` LF pinning for scene files (R-010 follow-up).
- Linux build + sanitizers (Windows Vulkan validation runs
  only in this session).

## Verdict

LUMA RECOVERY INCOMPLETE — ENGINE STILL HAS BROKEN CORE WORKFLOWS
