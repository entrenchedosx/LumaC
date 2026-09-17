# Editor Viewport (Phase 31; rendering in Phase 33)

Headless orbit/pick/frame math. No window, no GPU, no renderer
submission — the engine render trio stays engine-side; a Phase-32
GUI renders these models. Phase 33 did exactly that; see
`EDITOR_VIEWPORT_RENDERING.md` for the composite + camera update.

## Orbit camera

State `{width, height, target, yaw, pitch, distance, fov_y, near,
far}` (defaults 640×480, origin target, yaw 0, pitch −0.35,
distance 8, 60° FOV, 0.1/1000 planes). `orbit` wraps yaw to
[−π, π] and clamps pitch to [−1.55, 1.55]; `dolly` multiplies
distance (clamp [0.05, 1e5]); `pan` moves target in the camera
frame scaled by distance. `led_viewport_camera` derives `lr_camera`
via `lr_camera_look_at` + `lr_camera_set_perspective` (polar guard
tilts up-vector at extreme pitch). NaN inputs are ignored.

## Unproject

`led_viewport_ray(px, py)` maps client px (origin top-left, LumaC
convention) → NDC (Vulkan depth 0..1) → world ray by inverting
view-proj (column-major Cramer inverse; singular → 0). Proven by
the project→unproject round-trip: the camera-target ray passes
within 1e-3 of the target (`test_editor_viewport`).

## Picking

`led_viewport_pick` casts the viewport ray through
`le_physics_raycast` on the edit world (runtime world while
playing). **Colliders required**: mesh-accurate picking without
physics colliders is unsupported (no engine mesh-raycast exists).
`max_distance <= 0` selects `far_plane`. `pick_select` replaces the
selection on hit, clears on miss (edit world only; no-op in play).

## AABB + framing

`led_compute_world_aabb` = `lr_mesh_get_bounds` × world matrix over
8 corners (mirrored bases expand conservatively — corner mapping is
sign-agnostic). Pointer renderables only; asset-backed report 0
(never guessed); missing/stale/asset-backed → 0 with zeroed outs.
`led_selection_aabb` unions live selection (position fallback for
non-mesh members); `led_frame_selection` fits target + distance to
the selection (whole-scene position union when empty).

## Gizmos

Modeled as drag intents `{mode, axis, start_world, current_world,
snap}` applied through coalesced undoable commands: translate adds
the axis-constrained delta to position; scale adds 0.1×delta;
rotate premultiplies an axis-angle delta (free = yaw). Snap rounds
deltas (meters / degrees / fraction). `led_gizmo_lines` emits the
90-float preview soup (12 AABB edges + 3 axis rays) for a future
overlay pass.
