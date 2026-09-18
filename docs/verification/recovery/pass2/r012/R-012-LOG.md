# R-012 — PROFESSIONAL EDITOR VIEWPORT ENVIRONMENT (recovery hotfix log)

Verdict: **R-012 COMPLETE — EDITOR VIEWPORT READY FOR HUMAN REVIEW**, then STOP.

## 01. Complaint

The edit viewport rendered a black void: the HDR scene pass cleared to
black, no authored environment exists in the test scenes, and the GUI
composite clear (0.04/0.05/0.09) only shows where no scene pixels land.
An empty scene read as a dead window, not a workspace. Requirement:
`EDIT = Editor Environment + Authored Scene + Overlays` vs
`PLAY = Actual Game Scene` — editor-only tooling, never scene content.

## 02. What was built (renderer-owned, editor-toggled)

Procedural editor environment, backend-neutral (public LumaC only), two
fullscreen-triangle draws, zero textures, zero lights, zero IBL:

- `renderer/shaders/editor_sky.vert` — fullscreen ray vertex shader
  (world = invVP * clip; ray = world - camPos).
- `renderer/shaders/editor_sky.frag` — 3-zone linear-HDR gradient over
  ray elevation: zenith slate / horizon band / nadir ground haze.
  Softness rides `zoneC.w`, clamped 0.02..0.6.
- `renderer/shaders/editor_grid.frag` — infinite y=0 grid: ray→plane
  intersect, 1 m minor / 10 m major cells, fwidth AA, 60 m distance
  fade, restrained red-X / blue-Z axes near origin. Discards above
  horizon, below-plane cameras, and at zero fade.
- Sky draws FIRST in `lr_renderer_render_scene` when no authored env
  is active; grid draws LAST (depth LESS, writes OFF, SRC_ALPHA
  blend) so authored floors/models obscure it. Authored sky takes
  precedence; the editor never covers game content.
- State `lr_editor_env {enabled, grid_enabled}`, default OFF —
  play/headless keep the black clear unless the bridge enables it.
- Public API: `lr_renderer_set_editor_environment` /
  `lr_renderer_get_editor_environment` /
  `lr_renderer_get_editor_environment_stats` (observe-only drawn /
  skipped counters for the headed legs).
- Palette (linear HDR, `renderer_internal.h`): zenith
  (0.045,0.062,0.105) → horizon (0.155,0.185,0.240) → nadir
  (0.030,0.032,0.038), softness 0.18; minor
  (0.145,0.165,0.200,a0.30), major (0.210,0.240,0.290,a0.45);
  axis strength 0.55, falloff 9 m.
- GUI bridge (`gui_viewport_tex.cpp`): enables env ONLY around the
  EDIT-world composite; the play world forces (0,0) every frame
  (belt-and-braces against stale state — the renderer is shared).
- View menu: `Editor Environment` + `Grid` checkboxes, defaults ON/ON
  (grid implies the sky context). Prefs are GUI-only: never
  serialized, never scene bytes, never assets.
- Headed-test hooks (public `luma_editor.h`, test-only):
  `leg_test_set_viewport_env` (staged-pending so pre-first-frame
  forces survive `leg_ui_ensure` defaults), `leg_viewport_sky_pixel`
  (single-pixel target readback, tight-stride-correct),
  `leg_viewport_panel_rect` (panel→target 1:1 mapping).

## 03. Screenshot-driven iteration (numeric probes; this model cannot view images)

1. First capture: sky too dark — top-strip luma 16.9 vs panel chrome
   (26,28,33); zenith/horizon ~2x darker than the GUI chrome.
2. Brightened 2x (zenith 0.023→0.045, horizon 0.075→0.155):
   top-strip luma 34.4, sky column (100→180) (14,18,29)→(39,47,61),
   chrome-adjacent but still quieter than panels. Horizon band peaks
   (39,47,61) at y=180, nadir haze below.
3. Grid verified on the ground plane: row oscillation stdev 80–91
   (lines) vs 0.3–0.4 (clear); checkerboard scene keeps the hard
   R-011 shadow readable (cube 182,61,82 vs ground 183,123,140;
   shadow report identical to R-011: submitted=2 draws=2 tris=24
   shadow_maps=1).

## 04. Proofs

App captures (1280x800 headed, `--import-all`, 90 frames):

| capture | viewport reading |
|---|---|
| `01-empty.png` (starter: rig+cam+sun+Ground) | sky gradient top (11,16,27) flat, horizon lift (37,44,57) at y340, dark field (14,17,22); empty reads as workspace |
| `02-shadow.png` (Shadow scene) | sky (14,18,29)→(39,47,61), grid stdev 80–91 on ground, R-011 shadow intact (report `submitted=2 draws=2 tris=24 shadow_maps=1`) |
| `03-game.png` (Game scene, Camera selected) | same sky, scene content (70,170,70) at y340, `submitted=4 draws=4 tris=38` |
| `06-play.png` (`--play`) | top-strip luma 0.0 (black, game camera), NO editor sky; scene pixels byte-comparable to EDIT below the sky region |

Headed legs (`test_editor_headed`, same seeded scene + production bridge):

- `R12 env-on recorded sky+grid`: stats sky 1/0 grid 1/0; sky pixel
  (11,16,27) luma 18 ≥ 15.
- `R12 env-off skipped sky+grid`: stats sky 0/1 grid 0/1; pixel
  (0,0,0) vs ON (11,16,27) — sky lift removed.
- `R12 play skipped sky+grid`: stats sky 0/1 grid 0/1 with prefs left
  ON (bridge forced off); pixel luma 0 < 15.
- H-composite determinism holds: census 302560 twice, identical.
- Full Debug suite: **94/94 PASS**.

Isolation arguments:

- EDIT vs PLAY: same Shadow scene — EDIT top-strip luma 34.4
  (gradient), PLAY top-strip luma 0.0 (game black); bridge forces
  (0,0) for every play composite (headed play leg leaves prefs ON
  and still skips).
- No serialization: prefs live on `leg_ui` only; `git status` shows
  zero `LumaRealityTest/` modifications after every capture
  (sidecars restored; the R-011 CRLF footnote still applies).
- No light emission: sky/grid write only the HDR color buffer in the
  scene pass; no light submit, no IBL params, no ambient change;
  the R-011 ON/OFF footprint logic is untouched (report columns
  identical).
- Validation: full headed run under validation enabled, zero VUID
  failures; the grid pipeline carries the live HDR signature
  (R16F + D32) with push-only layout.

## 05. Design notes for the human reviewer

- 1 unit = 1 metre (grid cells are 1 m / 10 m).
- Below-plane cameras: sky only (grid discards — clean fade rule).
- Distance fade 60 m: no hard grid edge; nadir haze carries the far field.
- Cost: sky + grid = 2 fullscreen draws, 0 textures, 0 buffers.
- What this is NOT: no Lit/Unlit/Wireframe modes, no ambient hacks
  (the R-011 Lit proof is uncontaminated), no `LE_COMPONENT_EDITOR_GRID`.

## 06. Files changed

- `renderer/shaders/editor_sky.vert` + `editor_sky.frag` +
  `editor_grid.frag` (new) + committed `.spv` binaries.
- `renderer/CMakeLists.txt` — copy + glslc + hex_to_c entries.
- `renderer/src/environment.c` — ray uniforms, pipeline singletons,
  `lr_renderer_record_editor_sky/grid`.
- `renderer/src/renderer.c` — sky-first/grid-last wiring, public
  setters/getters/stats.
- `renderer/src/internal/renderer_internal.h` — pushes, palette,
  `lr_editor_env`, stat flags.
- `renderer/include/luma_renderer/luma_renderer.h` — R-012 contract.
- `editor/src/gui/gui_viewport_tex.cpp` — EDIT-only bridge + play
  force-off + sky-pixel probe.
- `editor/src/gui/gui_panels.cpp` — View menu toggles + test hook
  (staged pending).
- `editor/src/gui/gui_probe.cpp` — panel-rect probe.
- `editor/src/gui/gui_internal.h` — pref fields + decls.
- `editor/include/luma_editor/luma_editor.h` — test-hook ABIs.
- `editor/tests/test_editor_headed.c` — R-012 env-on/off/play legs.

## 07. Evidence in this directory

- `01-empty.png` — starter scene (workspace read).
- `02-shadow.png` — Shadow scene (sky + grid + R-011 shadow).
- `03-game.png` — Game scene (sky + content).
- `04-headed-env-on.png` — headed seeded scene, env ON.
- `05-headed-env-off.png` — headed seeded scene, env OFF twin.
- `06-play.png` — app `--play` (no editor sky).
- `07-headed-play.png` — headed runtime world (no editor sky).

## 08. Human review asked for

Open `02-shadow.png` vs `06-play.png`: EDIT shows the slate gradient
+ grid under the checkerboard with the hard R-011 shadow; PLAY shows
game black above the same geometry. Open `01-empty.png`: the empty
scene should read as a quiet workspace, not a void. Toggle View →
Editor Environment / Grid to taste; defaults are ON/ON.
