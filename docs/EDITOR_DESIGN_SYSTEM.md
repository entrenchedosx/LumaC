# Luma Editor Design System (Phase 33V)

Small, deliberate, no one-off values in panel code. All constants live
in `editor/src/gui/gui_theme.cpp` (`LegTheme`) and are applied once in
`leg_theme_apply()`.

## Spacing scale (px @ 100%)

- `pad_xs = 2`, `pad_s = 4`, `pad_m = 8`, `pad_l = 12`, `pad_xl = 16`
- Panel padding: `(8, 6)` (`WindowPadding`)
- Item spacing: `(8, 4)`; inner spacing `(4, 4)`; indent `16`
- Control height: default frame `22` (`FramePadding = (6, 3)` @ 15px
  font); toolbar buttons `28×28`; status bar `26`; toolbar `40`

## Corner radius / borders

- `WindowRounding = 0` for chrome (toolbar/status/viewport frame);
  `FrameRounding = 3`, `GrabRounding = 3`, `ChildRounding = 3`,
  `PopupRounding = 4`, `ScrollbarRounding = 4`
- `WindowBorderSize = 1`, `FrameBorderSize = 0` (frames read via fill,
  not outline); `SeparatorTextBorderSize = 1`
- Panel borders: 1px `border` color; selected rows use filled
  `selection` bg, never outline-only

## Typography hierarchy

- UI font: Segoe UI 15px (Windows `%SystemRoot%/Fonts/segoeui.ttf`),
  DejaVu Sans 15px (Linux `/usr/share/fonts/.../DejaVuSans.ttf`),
  fallback: ImGui default raster (always works; logged).
- Panel titles: ImGui window titles, same font, `titlebar` bg —
  hierarchy comes from position/size, not from extra title fonts.
- Secondary metadata: `ImGui::TextDisabled` (secondary text color).
- Monospace/code data: console asset hex + diagnostics use the same
  UI font (no second font shipped this phase — documented; a mono
  face for the console is future work, not a second raster today).

## Color roles (linear 0..1, dark theme "Luma Slate")

| role | value | use |
|---|---|---|
| window_bg | (0.10, 0.11, 0.13) | main + panels |
| child_bg | (0.08, 0.09, 0.11) | scroll regions, viewport letterbox |
| titlebar_bg / active | (0.13,0.14,0.17) / (0.16,0.17,0.21) | panel titles |
| menu_bg | (0.12, 0.13, 0.16) | menu bar, toolbar, status |
| border | (0.23, 0.24, 0.28) | panel/frame borders |
| text | (0.88, 0.89, 0.92) | primary text |
| text_2nd | (0.55, 0.57, 0.62) | metadata, empty states, readouts |
| text_dis | (0.38, 0.39, 0.43) | disabled |
| accent | (0.95, 0.62, 0.18) | Luma amber — logo dot, active tool, links |
| selection | (0.72, 0.46, 0.13, 0.55) | selected rows (amber wash, readable text) |
| hover | (1.00, 1.00, 1.00, 0.06) | header/row hover |
| active_tool | (0.95, 0.62, 0.18, 0.30) | active transform-mode button fill |
| play | (0.35, 0.75, 0.40) | play pill + playing accents |
| warning | (1.00, 0.80, 0.35) | warnings |
| error | (1.00, 0.42, 0.38) | errors |
| success | (0.55, 0.85, 0.60) | ok confirmations |

Rules: one accent only (amber); no gradients/glow/neon; severity
colors appear ONLY on severity content; play state reads via the
status pill + toolbar pause-state, never a full-UI tint.

## States

- hover: `HeaderHovered` white 6% wash; buttons use frame-hovered lift
  (slightly lighter fill, no border flash).
- selected: amber wash + primary text (never inverted/black text).
- active (tool/mode): `active_tool` fill + accent icon.
- warning/error/disabled: roles above; disabled = `text_dis` + no
  hover response (`BeginDisabled` stays, but labels explain WHY where
  space allows, e.g. "Stop (not playing)" tooltip).
- focus: keyboard focus ring = `NavHighlight` accent @ 60%.

## Luma identity

Amber accent + "Luma" wordmark in the About dialog + a small amber
diamond/dot drawn before the project name in the status bar and in
the viewport overlay when no scene is open. No logo image files, no
splash screen this phase (documented non-goal).

## Layout contract (1280×800 default)

```
menu bar (system)
toolbar 40px: [create][prefab] | [play][pause][step][stop] [mode pill]
              | [T][R][S] gizmo | snap | speed        scene dirty dot
left 300: Hierarchy (full height)
right 300: Inspector (full height)
center: Viewport (top ~70%) / bottom 240: Assets | Console (tabs)
status 26px: dot project | sel n | undo n redo n | edit/PLAY | path
```

Viewport dominates by construction (center-top, largest single node).
Panels degrade by docking (user-resizable; Reset layout in View menu
restores the contract). Minimum sane window: 1024×640 (below that,
docking scrolls — documented, verified at 1280×720 + 1920×1080).
