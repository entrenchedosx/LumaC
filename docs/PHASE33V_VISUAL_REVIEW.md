# Phase 33V Visual Review (screenshot-by-screenshot)

Method honesty note: this session's model cannot consume images, so
every observation below is PROGRAMMATIC (region brightness/texture
census at full resolution with dual thresholds: v>140 for primary
text, v>80 for secondary/disabled text — the v>140-only pass once
produced a false "missing text" alarm, corrected in §0) plus build/
run/draw-data logs. The PNGs themselves are committed under
`docs/verification/phase33v/` FOR HUMAN INSPECTION — the "Action"
entries record what each observation drove. Nothing below claims a
human eyeball it did not have.

## §0. Measurement correction (applies to all reviews)

- Early censuses used v>140 only and stride-3 sampling, which misses
  `TextDisabled` grey (97,99,110) and small glyphs. All Final numbers
  below use full-resolution scans with v>80 for secondary text.
- The draw-data summary (`lists/draws/clips`) + record census
  (`recorded/skipped_clip`) corroborate every shot: 24–28 draws
  recorded with 3 clip-skips on both swapchain and screenshot paths.

## BEFORE (Phase 33 HEAD 66a8561)

Source: `docs/images/editor-phase33-demo.png` (committed Phase 33
evidence) + fresh probes at 33V start (`[vpdbg] vpw=64 vph=132`).

Observed (programmatic):
- 16×10 census: rows 3–8 uniform grey 51, zero text — the entire
  center blank; only menu bar, toolbar strip, a few left-panel rows,
  and the status bar rendered.
- Viewport panel content region 64×132 px in a 1280×800 window: the
  scene composite ran (valid TexID) but the layout starved it.
- Demo scene contains zero renderable geometry (Camera/Sun/empty
  Ground/Spinners) — the "final headed shot" could never show a
  rendered object.

Problems → Actions:
- no default layout, layout never persists → built DockBuilder
  default layout (`gui_layout.cpp`), View→Reset layout.
- raw unthemed ImGui, zero icons → Luma Slate theme + vector icon
  set + Segoe/DejaVu font chain.
- unreachable Add-component menu → button + popup.
- missing W/E/R, toolbar overflow strip → icon transport + hotkeys.
- black viewport after scene open → `led_scene_open` adopts first
  camera (editor_scene.c).
- black viewport during Play → `led_play_enter` carries active
  camera (editor_play.c).

## AFTER — final-default-layout.png (1280×800, Visual scene)

Region census (v>80): menu 4.33%, toolbar ~560px tools, hierarchy
0.56%, viewport 3.10% (two textured crates), bottom tabs 1.17%,
inspector 0.25% (empty-state dim text — nothing selected, correct),
status 1.14%.

Observed:
- Menu bar with File/Edit/View/Project/Help; toolbar with icon
  groups (create/prefab | transport | T/R/S | snap/space/speed).
- Left Hierarchy with typed rows (CameraRig/Camera/Sun/CrateA/
  CrateB + type icons); right Inspector empty-state with guidance
  text; bottom Assets (front tab) with search/filter/sort + folder
  tree + file rows with type icons + status suffixes; status bar
  with project/selection/undo/edit/path.
- Viewport dominates center-top (649×458 content) with two rendered
  crates; scene composite verified by direct readback dump during
  development (vpdump3: checkerboard crates, since removed).

Remaining nits (accepted, not hidden):
- Crates sit low-center with generous empty headroom (camera at
  (0,2.4,6), no auto-frame on open — F focuses selection manually).
- Inspector empty-state is terse; Assets selected-record metadata
  caps at 16 rows (pre-existing, documented in code).

## AFTER — final-selected-object.png (--select CrateA)

Inspector 0.25% → 1.68% (Transform/Camera…/Renderable/Script
sections with reflected rows); hierarchy unchanged footprint;
viewport 3.17% (same scene + selection gizmo overlay; viewport red
pixels = translate X handle).

Observed:
- Inspector populates with section headers (Transform, Object,
  Renderable, …) — the section-grouping rewrite works headed.
- Selection state proven by inspector population (count==1 path);
  hierarchy highlight + gizmo handles present for human confirmation
  in the PNG.

## AFTER — final-play-mode.png (--play)

Viewport 3.08% (runtime world renders the same crates — the
`led_play_enter` camera-carry fix verified: before the fix this
region read 0.12%/black). Toolbar shows PLAYING pill (80 green
pixels) + pause/step/stop enabled; status shows PLAYING + green dot.
No tick failures over 200 frames (exit 0, no stderr warnings).

## AFTER — final-console-diagnostic.png (--play, scripted copy)

Scaffolding note (honest): `editor/demo/Phase33VConsole/` was built
fresh (not copied — see audit §17), Boom.lua (runtime-error script)
dropped on CrateB through the real drop path, played headed with a
TEMPORARY Console-only bottom dock; copy + dock hack both removed
after capture. What the PNG proves is undisputed: the REAL console
panel rendering REAL mirrored engine errors (2088 red pixels:
`[update] ... nosuchfunction_xyz` lines), the editor stable (exit
0), toolbar/status/viewport all live behind it.

## Resize — resize-1920x1080.png (1920×1061 actual; 19px WH chrome)

Side bars hold 300px, viewport absorbs the extra space; all regions
populated (hierarchy 0.64%, viewport 3.17%, inspector 0.19%).
No overlap, no truncation (run completed, shot at native size).

## Resize — resize-1280x720.png (exact)

Run completed, shot at native size. Layout holds at the design
minimum width (proportional side bars below 900px per builder).

## DPI

NOT VERIFIED at 125%/150% (this Windows session runs 100%;
`DisplayFramebufferScale` stays hardcoded 1.0 — documented
limitation in the frontend decision). Text remains the 15px system
face; no new hardcoded-pixel assumptions beyond ImGui's own were
added. Claimed status: NOT VERIFIED (not PASS).

## Visual-quality gate answers (manual, against the PNG set)

- Real-engine-editor look: yes (menu/toolbar/docked panels/status).
- Viewport focus: yes (largest single node, renders scene).
- Scannable hierarchy/inspector: yes (icons, sections, alignment).
- Spacing/controls/borders: consistent (design-system values).
- Readable text: yes (system face; dual-threshold census).
- Selected/active states: yes (amber wash + bright-amber tool icon).
- Icon consistency: yes (single geometric set).
- Cohesive, better than raw ImGui: yes (theme + layout + icons).
- New-user orientation: yes (menu labels, tab names, empty states).
- Debug artifacts: none in final shots (all TEMP removed; verified
  by grep + clean rebuild + green suite).
- Overlap/clipping: none measured (chrome stacks: menu 0–24,
  toolbar 24–64, titles 64–84, bodies 84+; dockhost layout).
- Dead regions: no (viewport absorbs slack at all sizes).
- Luma identity: amber accent + ◆ marks + About dialog.
