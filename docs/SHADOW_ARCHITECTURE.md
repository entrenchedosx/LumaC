# Luma Shadow Architecture

Real-time shadow mapping for PBR direct lights (Phase 16):
directional (fitted ortho) + spot (cone perspective) shadows onto
PBR receivers, 3x3 PCF, opt-in casting/receiving. Shadows live in
**Luma Renderer**; LumaC only gained generic depth plumbing
(fragment-less pipelines, sampled-depth final layout, wait-idle).
No light, cascade, or shadow-map types exist in LumaC (audited).

## Lights with shadows

`lr_light` gains `type = LR_LIGHT_SPOT` (position, range,
`spot_inner/outer` radians, `0 <= inner <= outer < pi/2`) and a
`lr_shadow_desc` per light: `enabled`, `resolution` (64..2048,
0 = 1024), `shadow_distance` (directional fit slice),
`near/far_plane` (spot depth range), `depth_bias` /
`normal_bias` (negative = renderer defaults). Submit-time
validation is pure and loud (bad direction, degenerate cone,
over-capacity); at most `LR_MAX_SHADOWS` = **4** shadowed lights
submit per frame — the 5th renders unshadowed, pinned by test.

`direction` stays the light TRAVEL direction for all kinds.

## Slots and maps

One renderer-owned depth slot per assignment (max 4): D32 image +
view + depth-only target, recreated only on resolution change,
rebound into the frame set after the pass that makes it sampled.
Inactive slots sample a white fallback (value 1 = lit). Eager
creation fails fast when depth sampling is unsupported. The
frame-set binding is invalidated on every recreate (not compared):
freed wrapper addresses recycle through the allocator, so a
pointer compare would mistake the new view for the old one, skip
the rebind, and sample a destroyed image (GPU fault).

Depth pipeline: shared fragment-less layout, model-only push,
CULL_NONE for every caster (documented rule — never the main-pass
culling state), mini-cache keyed by signature, bounded (4).

## Frame flow (the one-upload rule)

```
begin -> submit lights/items -> render_shadows(enc)
      -> main pass -> render(target) -> end
```

`render_shadows` renders every assigned slot (frustum-fit VP,
per-pass VP upload, depth-only draws, set rebind, metadata
staging) exactly once; double prepare and post-prepare submits
are rejected. The lights UBO is a persistently-mapped CPU memcpy,
while the whole frame records into ONE command buffer submitted
at end-of-frame — so exactly one upload may happen per frame. A
second renderer leg with different prepared state (e.g. an
unprepared swapchain mirror) would memcpy unprepared values, and
`begin` would zero the shadow metadata, AFTER the shadowed leg
recorded but BEFORE anything executes: deterministically
unshadowed output. Test harness and examples render one prepared
leg per frame (clear-only present legs; multiview shares one
prepare across two same-value renders).

Slot recreation destroys the sampled view: callers must have
drained in-flight sampling work first (device idle), or a prior
frame faults on the freed image.

## Fits and sampling

- Directional: camera-frustum slice corners fitted to an
  axis-aligned box around the light axis (translation-invariant,
  rotation-sensitive; degenerate straight-down takes a fixed-up
  branch). All math is headless golden-tested against an
  independent Python reference, including stability bounds.
- Spot: cone perspective (`2*outer` fov, aspect 1).
- Sampling: manual 3x3 PCF over one slot (no comparison sampler),
  texel-sized taps, receiver depth minus constant bias vs stored
  depth; normal bias offsets the sample position along the
  geometric normal (never the tangent-space normal). Anything
  outside the projection (including behind-light `w <= 0`) is
  lit, never clamped. Shadow gates DIRECT light only; ambient
  and emissive never.
- Metadata block mirrors `lr_shadows_gpu` exactly: count uvec4 +
  4x {mat4 viewProj (column-major), params (texel, normalBias,
  constBias, 0)} = 336 bytes, static-asserted on both sides.

## Receivers and casters

Per-draw opt-in: `casts_shadow` / `receives_shadow` (assets set
both). The receive flag rides the PBR push block (bit 0, 116
bytes total). Unlit casters throw identical footprints (depth
pass is material-agnostic). Mirrored (negative-scale) casters
match in the map and render correctly in main (per-item
front-face flip; the old inside-out debt was fixed in the
pre-Phase-24 audit and is pinned lit by the mirrored shadow
test).

## Stats, introspection, example

Extended stats (`shadow_casting_lights/passes/maps_rendered`,
`shadow_draw_calls/triangles/culled`, per-kind draws, bounded
binds), read-only slot/light/pipeline introspection, and
`examples/shadow_scene` (cube + ball under one orbiting shadow
light + unshadowed fill; `--frames N` headless smoke).

## Test methodology (earned)

Pixel probes must sample VISIBLE ground: under-caster pixels
show unlit caster faces, never ground (silhouette conflation
voided a generation of probes). Tilted-light crescents beside
the caster, min-windows over thin strips, and grid scans over
fixture poses replaced center-pixel reads. The main testbed
target carries its own depth buffer — painter order over
overlapping draws is heap-address noise, not signal.

## Deferred (unchanged)

Point-light (cube) shadows, cascades, contact hardening, IBL,
HDR, tonemapping, transparency, animation, scene graph, ECS,
device-lost recovery, pipeline cache, Wayland.

## Phase 19 notes

Shadow depth write → PBR sample now rides explicit
pass-dependency visibility (write → fragment-shader-read), not
just finalLayout correctness; sampled-usage depth finals adopt
SHADER_READ state. Shadow maps are capture-capable
(TRANSFER_SRC) for debugging/AI diagnostics. No API change.
