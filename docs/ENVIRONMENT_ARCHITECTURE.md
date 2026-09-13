# Luma Environment Architecture

Real-time environment lighting for PBR (Phase 17): HDR equirect
sources become split-sum IBL (irradiance + prefiltered specular +
BRDF LUT), a sky pass, an HDR scene target, and an exposure +
tonemap output pass. Environment work lives in **Luma Renderer**;
LumaC gained no environment types (audited): only the generic
machinery any pass needs was already there (render targets with
depth, binding sets, command recording). Assets gained an HDR
decode/upload path (`la_hdr_*`) with no lighting semantics.

## Objects and API

`lr_environment` is opaque, renderer-owned, and cheap to create
(preprocessing is lazy on first use). The descriptor borrows its
source texture + sampler (they must outlive the environment);
`intensity >= 0` scales IBL (0 removes IBL while direct
lights/emissive remain) and `rotation` is a horizontal yaw in
radians applied to lighting AND sky together.

- `lr_environment_create / destroy / update` — `update` with a new
  source texture/sampler marks the environment dirty (reprocessed
  on next use); intensity/rotation-only updates touch runtime
  parameters, never the derived maps.
- `lr_environment_set_intensity / set_rotation` — params-only, no
  reprocessing (pinned: rebuild counters stay 0 across yaw churn).
- `lr_renderer_set_environment` (NULL detaches and restores the
  ambient fallback); destroying the active environment without
  detaching first is an app error.
- `lr_renderer_set_exposure` (EV, `mult = 2^EV`, NaN/Inf rejected)
  and `lr_renderer_set_tonemap_operator` (`NONE` default, `ACES`).
- `render_scene(enc, w, h)` + `render_output(enc, target)` — the
  legacy `render()` direct path is unchanged.
- `lr_renderer_get_hdr_view` (read-only diagnostics) and
  `lr_renderer_get_environment_info` (active, intensity, rotation,
  exposure, tonemap, HDR format/extent, preprocess state,
  lifetime rebuilds, generation, last build ms).

## Preprocessing (fixed kernels, lazy, once)

Sizes and taps are fixed defines (`LR_ENV_*`): base cube 128 with
all 8 mips rendered analytically, irradiance 32x32x6 single mip,
prefilter 64 with the full 7-mip chain, BRDF LUT 256x256 RG16F
(renderer-owned singleton, environment-independent). Tap counts:
irradiance 128, prefilter 256, BRDF 128. RGBA16F is preferred for
derived maps; no new LumaC formats were needed.

Every mip face gets its own ordered pass — fully analytic, no
mipmap blits mid-frame: immediate-submit blits jump the queue ahead
of recorded flight work and sample half-built levels (fault found
during development, fixed by construction). Every level ends
STORE-marked sampled-readable for the passes that sample it.
A build that fails midway leaves ready=0 + dirty=1 so the next
frame retries instead of sampling half-built maps. Builds drain
the device first (derived views may still be sampled by in-flight
work being replaced); builds are rare, so the coarse wait is cheap.

Dirty tracking is generation-checked: create marks dirty, a new
source marks dirty, everything else is parameters. Per-frame stats
reset in `begin`, so `environment_rebuilds == 1` on the building
frame and 0 after — the lifetime total lives in
`lr_environment_info`.

## Frame flow

```
begin -> submits -> render_shadows(enc)
      -> render_scene(enc, w, h)   // HDR target + sky first, no depth test/write
      -> output pass -> render_output(enc, target)  // exposure + tonemap
      -> end
```

Exactly one lights upload happens per frame (single-submit mapped
buffer); there is no second renderer leg. The HDR target auto-fits
the scene extent (recreated only on extent change — resizing the
scene never reprocesses the environment). The sky is a fullscreen
triangle drawn first with depth test/write off; the ray comes from
the inverse view-projection (`world - camPos`, translation
invariant) and samples the base cube, so the sky is HDR scene
content and tonemaps with everything else.

## Sampling (split-sum)

```
diffIBL = irradiance(N) * albedo * (1 - metal) / pi
specIBL = prefilter(R, lod = rough * (mips - 1)) * (F0 * A + B)
indirect = (diffIBL * occ + specIBL * mix(1, occ, 0.5)) * intensity
```

The ambient fallback applies if and only if no environment is
active (`* (1 - iblActive)`): with no env the output is
bit-identical to the legacy path (pinned by 92 checks). Direct
light and emissive are never AO-gated; AO scales diffuse fully and
specular half. `F0` is the metal tint signal (dielectrics neutral,
metals reflect tinted) — pinned by a copper-vs-dielectric grid.

Orientation: cube face mapping is derived from the Vulkan cube
spec (`+X: (1, 1-2v, 1-2u)` etc.), verified by a 6-color face test
first try. Equirect convention: `u = atan2(z,x)/2pi + 0.5`,
`v = acos(y)/pi`, row 0 = +Y.

## Sky, tonemap, output

The output pass is separate from PBR by rule (tonemap never runs
in the PBR shader): `evMult` first, then ACES Narkowicz
`x*(2.51x+0.03)/(x*(2.43x+0.59)+0.14)`, NaN to 0, clamp to [0,1];
`NONE` clamps the exposure-scaled HDR. sRGB encoding happens
exactly once via the target format (linear in-shader, pinned
against a CPU reference: 0.1804/0.4627).

The tonemap pipeline mini-cache is keyed per encountered target
signature INCLUDING the depth format: swapchain passes always
carry depth while offscreen output passes usually carry none, and
a depth-less pipeline records INCOMPATIBLE into a depth pass
(fault found by `examples/ibl_scene`, pinned by a depth-carrying
output regression test).

## Rotation, intensity, replacement

One yaw rotates IBL sample directions AND the sky (same `rotY`);
rotation and intensity are runtime parameters — no reprocessing,
no new bindings (the params buffer never moves, so no binding ever
dangles on parameter change). Replacement swaps the source (dirty
rebuild on next use) or the whole object (the renderer survives
env churn); sequential views share one preprocessing.

## Assets: HDR

`la_hdr_decode` (strict HDR gate over `stbi_loadf`),
`la_float_to_half` (round-to-nearest-even, Inf/NaN/subnormal
correct) + `la_half_to_float`, `la_hdr_load` (path cache, R16F GPU
image, TRANSFER_SRC for readback), `la_hdr_get_view`,
`la_hdr_lookup`, teardown hookup. Fixture
`assets/tests/fixtures/env_gradient.hdr` is a procedural RLE RGBE
(gradient 0.05-4.0 + a 25x spot), validated by decode-back; it is
public domain by construction (generator documented in-file).

## Faults found this phase (kept as rules)

- Descriptor set updated between binds in one recording is a VUID:
  per-stage write-once sets.
- Immediate-submit mipmap blits jump recorded flight work: render
  all mips analytically instead.
- LumaC binding writes require full-range tracked readability
  (renders mark STORE ranges; DONT_CARE-store decays to UNDEFINED).
- Freed wrapper addresses recycle: pointer compares skip rebinds
  on recycled handles — invalidate on destroy + ptr/epoch tracking.
- Stale `.obj` files after internal-header struct growth write wild
  fields: nuke the renderer build subdirs and rebuild after header
  changes (serial builds; `$env:CMAKE_BUILD_PARALLEL_LEVEL=1`).
- Depthless targets + pointer-sorted draws = heap-address painter
  flicker (fixed in Phase 16 with depth-tested targets).
- Test stats reset every `begin`: per-frame `environment_rebuilds`
  is 1 on the building frame, 0 after; lifetime totals come from
  `lr_environment_info`.

## Tests and example

`renderer/tests/test_ibl.c` (headless CPU references: ACES values,
yaw math, Hammersley anchors, BRDF kernel matching the shader
op-for-op) and `renderer/tests/test_ibl_vulkan.c` (114 GPU checks:
API validation, orientation, irradiance/specular follows-light,
BRDF LUT, emissive ladder, HDR>1, NONE clamping, exposure,
intensity linearity, sRGB-once, shadow+IBL independence, metal
grid, rotation, sky, replacement, resize, multiview, info,
150-frame endurance, depth-pass output, Phase 18 post/tint,
public HDR+LDR capture, public derived-map reads, 500-frame
endurance, multiview divergence). `examples/ibl_scene`
renders a procedural HDR sky lighting a sphere trio with no direct
lights (`--frames N`, `--screenshot out.png`).

Phase 18 closures: `lc_image_readback` public readback (white-box
test helpers retired where public covers), persistent pipeline
cache (cold-JIT cost paid once; warm reuse measured), WSL runtime
for device-only tests (readback 48/0, cache 22/0 on lavapipe —
windowed tests still need a display). Default-TDR single-launch
verification remains NOT VERIFIED (no registry changes made).
A real example-app bug was caught by the new capture path: the
example freed its upload pixels before `lc_image_write` consumed
them (use-after-free → deterministic negative HDR); fixed, with
the mid-recording-readback rule documented from the same work.
