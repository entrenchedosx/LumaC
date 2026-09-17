# Editor Frontend Decision (Phase 33V)

## Current problems (verified by headed run 2026-09-17, not by opinion)

Headed evidence (`docs/verification/phase33v/`, probe screenshots +
`[vpdbg]` composite log + 16×10 region census):

1. First-launch center is blank: rows 3–8 of the census read uniform
   grey 51 with zero text — hierarchy/inspector/assets/console bodies
   draw nothing visible; only menu bar, toolbar strip, a few left-
   panel rows, and the status bar render.
2. The viewport panel squeezes to ~64×132 px (`vpw=64 vph=132` logged
   from the panel's own content region) while the window is
   1280×800. The scene composite itself WORKS (nonzero TexID every
   60 frames) — the layout starves it.
3. No default layout is ever built (no `DockBuilder*` /
   `SetNextWindowDockID` anywhere) and layout never persists
   (`IniFilename = NULL`, `leg_set_ini_path(gui, NULL)`; the stored
   path is never assigned to `io`).
4. Zero theming: no `StyleColors*`, no `PushStyleColor`, default font
   — stock ImGui grey, ad-hoc hardcoded `TextColored` reds/greens.
5. `BeginMenu("Add component")` with no surrounding menu bar can never
   open — Camera/Light/RigidBody/etc. are unreachable from the
   inspector panel (author's own comment admits the missing bar).
6. Documented W/E/R gizmo hotkeys don't exist; mode switching is a
   Combo; WASD/Q/E fly the camera instead. Toolbar is one overflowing
   text strip with fixed widths; Pause/Step/Stop render greyed in edit
   mode; red `[PLAY]` text shouts inline.
7. Assets panel is a one-column path list (`Selectable("%s [%s] %s")`
   + `BulletText` folders), no grid/thumbnails; hierarchy is manual
   Indent-by-depth with no icons/rename; 16-row silent metadata cap.
8. No icons anywhere (grep-clean); hardcoded widths everywhere
   (110/90/70/120/140/160/220/180); `DisplayFramebufferScale = 1.0`
   hardcoded (no DPI path); status bar is a diagnostics format string.
9. Two real input bugs in the bridge: right-side modifiers forced
   `false` (`gui_input.cpp`), focus-loss path is a no-op despite its
   comment (`gui_loop.cpp`).

What is NOT broken: the custom `luma_gui_lumac` bridge (blended
pipeline + per-draw scissor under validation layers, font upload
ordering, viewport offscreen target + engine trio) — the GPU proof
suite is green and the headed composite returns valid TexIDs. The
write-path confinement (`led_execute`/`led_write_property` funnel)
holds; panels reach one layer down only for reads/descs/render-trio.

## Options considered

A. Repair/restyle existing GUI (keep vendored Dear ImGui docking
   branch + custom LumaC bridge; fix layout, theme, panels).
B. Replace frontend with C# (WinUI/WPF/Avalonia) + narrow native
   bridge (handles/IDs, never pointers).
C. Replace frontend with Rust (egui/iced) + narrow FFI.
D. Replace frontend with Electron/web for "easy CSS".

## Chosen technology

**A: repair/restyle the existing Dear ImGui frontend.**
Framework: vendored Dear ImGui `v1.92.9b-docking` (pinned tarball in
`third_party/imgui/`, core subset compiled, no upstream backends).
Language: C++ isolated to `editor/src/gui/` behind the `leg_*` C ABI
(unchanged boundary). Version/license: ImGui MIT (LICENSE.txt
vendored); no new dependencies — zero packaging change.

Reason chosen:

- The bridge is the hard part and it already works (validation-clean
  GPU walk, font/viewport texture management, input mapping). Options
  B/C/D all re-pay that cost plus a new texture-embedding/input-
  latency/integration bill for zero proven visual gain — ImGui can
  look professional (see its own styled demos and shipped tools);
  ours looks raw because no styling or layout work was ever done, not
  because the framework can't.
- Sunk cost is not the argument: the argument is that every observed
  defect (layout, theme, unreachable menu, hotkeys, icons, empty
  states, DPI) is fixable above the bridge without touching it.
- B/C remain possible later WITHOUT engine changes precisely because
  the `leg_*` C ABI + `led_*` core boundary is preserved; this
  decision does not close them. D is rejected: memory/packaging cost,
  native viewport texture embedding friction, and input latency for a
  GPU-composited viewport, for styling ease we don't need.

## Integration model (unchanged)

```
Beautiful Editor Frontend (ImGui, editor/src/gui/)
          │  leg_* C ABI (opaque handles)
Stable Editor Bridge/API (luma_editor.h leg_*)
          │  led_* session/commands
Editor Core (editor/src/*.c, GUI-framework-free)
          │  le_*
Luma Engine → Luma Renderer → LumaC
```

One convention amendment (documented, narrow): `imgui_internal.h`
`DockBuilder*` layout functions are permitted in ONE new TU
(`editor/src/gui/gui_layout.cpp`) for first-run default-layout
construction only. Rationale: the public header itself says "there is
no public API yet other than the very limited SetNextWindowDockID()";
every docking-branch app builds its default layout with the builder;
the vendored copy is version-pinned so the "WIP API" risk is
contained to upgrade time (re-verify on bump). No other internal API
is used; `BeginViewportSideBar`-style internals stay banned. The
CMake configure audit already permits this (it bans backends, Lua,
engine internals, and upstream impl backends — not the internal
header); the convention comment in `gui_internal.h` is updated to
record the single-TU exception.

## Packaging / platform implications

- Windows: unchanged (same static libs, same `luma_editor_app.exe`,
  same Win32 backend in LumaC). Font loading uses `%SystemRoot%`
  Segoe UI with ImGui-default fallback (no shipped font files, no
  licensing exposure; Segoe UI is a system font, not redistributed).
- Linux: unchanged (same X11 path). Fonts resolve via
  `/usr/share/fonts` (DejaVu Sans) with the same fallback chain.
- Performance: theme/layout/panel work adds no per-frame cost beyond
  what ImGui already does; icon drawing uses `ImDrawList` primitives
  (no texture, no font dependency); no animations are added.
- DPI: `DisplayFramebufferScale` stays 1.0 this phase (documented
  limitation); the rebuild uses `ImGui::GetIO().FontGlobalScale`-safe
  spacing (no new hardcoded-pixel assumptions beyond ImGui's own)
  and records the 100/125/150% matrix as NOT VERIFIED.

## Why the editor core remains independent

No `led_*`/`le_*` signature changes in this phase (the `--import-all`
app flag and any verified core bug fixes excepted — each committed
and justified separately). All visual work is confined to
`editor/src/gui/` + the app's frame assembly; the GUI-confinement and
backend-independence configure audits keep passing. A future C#/Rust
frontend would target the same `leg_*`/`led_*` surface this rebuild
validates.
