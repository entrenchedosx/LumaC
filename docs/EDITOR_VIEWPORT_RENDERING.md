# Editor Viewport Rendering (Phase 33)

How the 3D scene reaches the viewport panel: an offscreen
engine-trio composite sampled as a GUI texture. The swapchain
never carries the 3D scene directly.

## Pipeline

```
leg_viewport_composite(gui, vt, enc, w, h)   [no open pass]
  1. leg_viewport_ensure: offscreen RGBA8 target (+ D32 depth)
     sized to the panel extent (recreated on resize; the target
     SET retires through deferred retirement, never destroyed
     mid-flight).
  2. Play ? runtime world : edit world (Play isolation: the
     viewport shows the RUNTIME fork while playing).
  3. render_scene (engine trio) + output pass + render_end
     into the target.
  4. Primer lc_image_write BEFORE the texture-set update (a
     fresh image is UNDEFINED; the set must be created+updated
     AFTER the SHADER_READ transition, or the walk fails at
     step 2 with UNAVAILABLE).
  5. Return TexID = the target's TEXTURE-set pointer (round-
     tripped through the GUI texture table).

leg_panels_frame: viewport panel Image(scene_tex)  [gated on
  generation > 0 so the first frame never samples a stale set]
```

## Two-layout discipline

The GUI pipeline binds TWO sets: slot 0 = sampled image
(`tex_layout`, one entry — the per-texture set round-tripped
as TexID), slot 1 = sampler (`samp_layout`, one shared linear
sampler set). The viewport target mirrors the font-atlas walk
exactly: a texture set (created against `tex_layout`) + the
shared sampler set bound at slot 1. A single combined layout
(two entries, one set) FAILS at step 18 (PIPELINE_INCOMPATIBLE)
— this was a real bring-up bug, kept documented.

## Sizing

Frame 0 composites at a window fraction (panel extent unknown
before the first panels frame); later frames use the extent
the viewport panel wrote into `led_viewport`. The target
recreates on extent change; the panel `Image()` scales the
composited texture to the available region.

## Camera

RMB orbit / MMB(+Shift) pan / wheel dolly / WASDQE fly / F
frame-selection, via `led_viewport_camera` + `led_viewport_ray`
(the Phase 31 math, driven by real mouse deltas in
`leg_viewport_camera_update`). While playing, the viewport
camera follows the runtime world.

## GUI draw walk (`leg_record_gui`, `editor/src/gui/gui_draw.cpp`)

- Caller opens ONE `lc_*` pass with a matching
  `(color, depth-or-UNDEFINED)` signature; the GUI pipeline is
  rebuilt per-(color, depth) pair (swapchain passes carry
  depth; offscreen GUI passes may not — depth participates in
  the pipeline signature).
- Blended overlay pipeline (standard alpha blend), scissor per
  draw command (`lc_encoder_set_scissor`, full-target restore
  after the walk — step 0..18 diagnostics + last-texture probe
  on failure).
- Font atlas: RGBA8 upload, set created+updated AFTER
  `lc_image_write` (same SHADER_READ discipline as viewport).
- Draw budget: 20M verts / 2M indices / 64MiB cap
  (`LED_ERROR_OVERFLOW` past it). User callbacks are
  skipped/counted, never executed. Empty frames succeed.
