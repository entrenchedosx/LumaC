# Editor Gizmos (Phase 33)

Screen-space TRS handles over the Phase 31 gizmo math. The
overlay (`editor/src/gui/gui_gizmo_overlay.cpp`) projects
through `led_viewport_camera`, picks rays with
`led_viewport_ray`, anchors on `led_selection_aabb`, and
commits through `led_gizmo_begin` / `led_gizmo_apply` —
the same funnel the headless tests prove.

## Behavior

- Translate / Rotate / Scale modes (`W/E/R`), axis handles +
  plane handles, drawn in the viewport window's draw list
  (screen-space lines + quads, never scene geometry).
- Drag math: ray-plane intersect from `led_viewport_ray`;
  per-axis projection for axis drags; snap increments when
  enabled (Ctrl stepping); Escape cancels the in-flight drag
  (no command issued).
- Commit: `led_gizmo_begin(session, mode, axis)` on mouse-down
  (requires exactly one live selection), `led_gizmo_apply`
  with the staged drag on mouse-move, one undoable command on
  release. Consecutive same-target same-kind TRS drags
  coalesce (history setting, proven in `test_editor_history`).
- Undo/redo restores exact TRS (GPU suite: +1X then undo
  returns to `p0`).

## Selection interplay

Click-pick (`led_viewport_pick`, colliders required) selects;
marquee is deferred. Hierarchy `SET_PARENT` / `REPARENT_KEEP_
WORLD` context actions route through undoable commands.
The gizmo anchors on the selection AABB center; `F` frames it.

## Euler policy (unchanged from Phase 31)

ZYX (yaw Y, pitch X, roll Z), wire `[pitch, yaw, roll]` in
degrees; `led_euler_deg_to_quat` / `led_quat_to_euler_deg` in
`editor_reflect.c` are canonical. Rotation gizmo drags stage
Euler degrees, commit converts once.
