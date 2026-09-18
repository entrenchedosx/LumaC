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
| Point light | PointLight (1.0,0.8,0.6, int 20, range 12) submits alongside sun (lights=2/2 in shadow_scene) | — | nosun shot still lit (point survives) | LIVE PASS |
| Shadows | `shadow_scene` runs (shadow_passes=1, shadow_draws=3, lights=2/2); Game Sun carries `shadow 1` | test_shadow + test_shadow_vulkan PASS | stats (example has no --screenshot; differential proof queued) | LIVE PASS (stats) / PARTIAL (visual) |
| PBR/materials | test_pbr_vulkan readback asserts: rough-vs-smooth, metal-vs-dielectric, emissive, occlusion, sRGB (20/20 PASS lines) | test_pbr + test_pbr_vulkan PASS | checker crates distinguishable in viewport | LIVE PASS |
| IBL/environment | `ibl_scene --frames 5 --screenshot`: sky + env objects, 3258 unique colors, `ibl=1 sky=1 tonemap=2` | test_ibl + test_ibl_vulkan PASS | `12-ibl-repeat.png` | LIVE PASS |
| Tonemapping | stats report tonemap=2 in IBL run | — | exposure control comparison NOT done | PARTIAL |
| Scene graph/transforms | rig→camera parenting persists (`parent a4b9…`), positions/rotations round-trip byte-identical | test_scene PASS | scene text inspected | LIVE PASS |
| Save/reload round-trip | open never mutates the scene file (diff empty); builder save→reopen `objects=11` | H-id headed legs | file diff | LIVE PASS |
| Components | renderable/script/character/collider/rigid_body/light/shadow/camera rows persist + instantiate | test_scene PASS | scene text | LIVE PASS |
| Lua scripts | lua_scene live: 4 scripts tick, error_demo fails frame 3 while spin/move/orbit keep running | test_script + test_script_vulkan + animation/input/physics_script PASS | console error text | LIVE PASS |
| Lua in Game | Mover.orbit + CharDrive W/S + Trig.on_trigger_enter attached via real drop path | — | attach PASS lines in builder log | LIVE PASS (attach) |
| Input | W flies camera (H10), real inject_event queue | test_input + test_input_vulkan + test_input_script PASS; lua_input example | headed H10 | LIVE PASS |
| Play/Stop isolation | `--play` enters play, play shot differs 584/2800 px in viewbox (Mover moved), exit clean | H1/H2 headed + test_editor_play | `18-game-play.png` | LIVE PASS |
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

## Defects found live (recovery log)

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

## What was NOT live-verified (honest gaps)
- Shadow-map visual differential (stats green; no shadow
  on/off screenshot pair — example lacks --screenshot).
- Tonemap/exposure control comparison.
- Prefab instantiate live in-editor (suites green only).
- Character stairs/slope/platform traversal inside Game
  (platform suite green; not staged in the acceptance scene).
- Animation crossfade inside Game (animation suites green).
- Linux build + sanitizers (validation runs are Windows
  Vulkan; Linux/sanitizer legs from the 95-step spec did not
  run in this session).

## Verdict

LUMA RECOVERY INCOMPLETE — ENGINE STILL HAS BROKEN CORE WORKFLOWS
