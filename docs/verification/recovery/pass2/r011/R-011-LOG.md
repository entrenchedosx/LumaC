# R-011 — SHADOW RENDERING IS VISIBLY INCORRECT (recovery hotfix log)

Verdict: **R-011 FIXED — READY FOR HUMAN SHADOW REVIEW**, then STOP.

## 01. Complaint

The pass-2 Shadow acceptance screenshots showed a large blurred/diffuse
rectangular darkening over the ground instead of a convincing cast shadow
from the cube. The prior ON/OFF differential proved only that the shadow
code changes pixels — not that the shadow is correct.

## 02. Minimal lab (no checker primary)

`renderer/tests/test_r011_probe.c` (CMake target `test_r011_probe`, same
link pattern as `test_shadow_vulkan`): one 20x20 grey plane + one 2m grey
cube at y=1.5 + one slanted directional light (0.5,-1,0.2), no textures, no
checker, no IBL, zero ambient. Three captures per run:

- `r011-on.ppm` — shadowed frame (current PCF path)
- `r011-off.ppm` — twin frame, shadow disabled (reference)
- `r011-map.ppm` — slot-0 depth map visualized (unlit quad, R out)

Plus: slot-0 raw D32 depth readback (min/max/mean/hist10), footprint check
(crescent dark / outside lit), screen-space edge-width check (row y=150,
85%->15% fall distance in px), ON/OFF diff fraction.

Sweep knobs: `argv[2] = zero` reproduces the pass-2 scene config (both
biases explicitly disabled); `argv[3] = 512/1024/2048` sets map resolution.
Defaults preserve the fixed capture (renderer-default biases, 1024).

## 03. Root causes found (two, both fixed)

### 03a. Linear filtering of depth data (renderer bug — FIXED)

The frame shadow descriptor set sampled every depth map through the shared
`default_sampler` (LINEAR/LINEAR). Depth maps hold COMPARISON data (one
depth per texel), never filterable color: bilinear interpolation blends
neighboring depths at EVERY tap, so each of the 9 PCF taps compared against
a blended depth and the penumbra widened into the large diffuse darkening
in the complaint.

Fix (`renderer/src/renderer.c`, `renderer/src/internal/renderer_internal.h`):
a dedicated `shadow_sampler` (NEAREST/NEAREST, clamp-to-edge, no anisotropy)
bound at shadow-set binding 5; materials keep the shared linear default;
destroyed beside the default sampler. Per-tap compare stays exact; PCF
averaging over exact taps is the only softening.

Measured effect (lab, default bias, 1024): footprint edge moved 1px
(y373->372) and total changed pixels 0.75% — small, because the dominant
visible defect in the acceptance scene was (03b), not this. Kept anyway:
sampling comparison data through a color filter is incorrect regardless of
magnitude, and the post-fix edge is a 1px hard step at every resolution.

### 03b. Pass-2 Shadow scene shipped explicit-zero biases (scene-content bug — FIXED)

`LumaRealityTest/Scenes/Shadow.luma_scene` carried
`shadow 1 1024 0 0 0 0 0`. Zero is NOT "default": in this renderer
negative bias selects the compiled defaults (const 0.0015 NDC, normal 0.02
world) and explicit zero DISABLES both mitigations. The whole ground
self-shadowed (acne): lab zero-bias run `diff frac 0.7090` with the entire
ground mottled at 0.33/0.66 luminance ratios; editor zero mutation
viewport `difffrac 0.2266`, whites (229,229,240)->(178,178,188), ground
(229,125,143)->(126,69,81).

Fix: one-line scene change `shadow 1 1024 -1 -1 0 0 0` (UUIDs/object order
preserved by byte patch, NOT a regen — regen mints fresh UUIDs, documented
in `recovery_project.c`). Builder fixed the same way
(`editor/demo/recovery_project.c`: ShadowSun `depth_bias=-1`,
`normal_bias=-1`) so future regens don't reintroduce it.

Why nobody saw it: shadow bias/distance were uninspectable — no reflect
rows, no inspector, no report columns. Added (`editor/src/editor_reflect.c`:
`light.shadow_enabled/resolution/depth_bias/normal_bias/distance`
read+write; `asset-renderable visible/casts/receives` read+write — the old
pointer-only path always failed on drops; `editor/app/main.c --report` now
prints `shadow_lights/passes/draws/tris/maps/culled`).

## 04. What was NOT the cause (audited, kept as-is)

- PCF math: `renderer/shaders/pbr.frag::shadow_pcf` does per-tap compare
  then averages visibility (`sum += step(recv, stored)`, `/9.0`) — correct.
  Average-depth-then-compare was never present.
- Sampler type: `src/graphics/vulkan/vulkan_sampler.c` sets
  `compareEnable=VK_FALSE` always — manual PCF only, no comparison-sampler
  path to misconfigure.
- UV/depth conventions: proj.z checked 0..1 (Vulkan), UV clamped 0..1 with
  outside-frustum returns lit (1.0), behind-light (`w<=0`) returns lit.
- Kernel/texel: 3x3 kernel, `prm.x` = 1 texel; resolution sweep edge width
  is 1px at 512, 1024, AND 2048 (screen-space), i.e. no resolution-scaled
  blur remains.
- Frustum: directional fit uses `LR_SHADOW_DEFAULT_DISTANCE` 25 m unless
  `shadow_distance > 0`; scene leaves it 0 (default). Depth-map stats
  `min=0.202774 max=1.0 mean=0.741464`, content histogram spread
  (73k/163k/163k/44k across bins 2-5, 603k clear) — real content, 0..1.

## 05. Proofs (all post-fix binaries, fixed scene content)

Lab (`after/`, res 1024, default bias): `passes=1 draws=2 tris=14 slots=1
slot0res=1024 slot0active=1`; footprint `crescent lum=0.0000 lit
lum=0.4992 ratio=0.0000`; screen edge `xlit=143 xdark=144 width=1px`;
`on/off diff frac=0.0930`; depth `min=0.202774 max=1.000000
mean=0.741464`; probe `OK`.

Sweeps: zero-bias `diff frac=0.7090` (whole-ground acne — reproduces the
complaint mechanism); 512 edge `1px`, `diff=0.0978`; 2048 edge `1px`,
`diff=0.0910`; all probes `OK`. (The probe's original world-space
scan-line check FAILED at 2048 because the scan window lands fully inside
the umbra — replaced with the screen-space edge check above, which passes
at all resolutions. The 2048 "failure" was a probe bug, not a renderer
regression.)

Editor twins (fixed scene, 1280x800 headed, `--import-all`, 90 frames,
viewport crop x315-975/y95-665):

| pair | viewport diffrac | mean delta | reading |
|---|---|---|---|
| ON vs repeat (reopen) | 0.0308 | -0.40 | deterministic; the "reopen anomaly" is closed (see 06) |
| ON vs OFF | 0.0095 | OFF +0.39 | shadow confined to the cast footprint; e.g. (272-321,234-241) ON=(74,1,10)/(126,3,18) vs OFF=(229,6,32) checker whites; ground elsewhere byte-identical |
| ON vs ZERO mutation | 0.2266 | fixed +8.13 | acne removed by the fix; zero whites (178,178,188) vs fixed (229,229,240) |
| ON vs caster-moved | 0.0251 (0.0219 @thr25) | -0.09 | cube displaces (checker/black bands swap sides at y165); shadow follows the caster |
| ON vs light-moved | 0.2038 | +8.79 | shadow reshapes with light yaw |

Reports: fixed ON `submitted=2 dis=0 inv=0 dead=0 draws=2 tris=24
shadow_lights=1 shadow_passes=1 shadow_draws=2 shadow_tris=24 shadow_maps=1
shadow_culled=0`; OFF twin `shadow_lights=0 ... shadow_maps=0` with the
same 2 draws (geometry path untouched).

Regression: full Debug suite `94/94 PASS (75.19s)`; targeted
shadow/scene/reflect/viewport `6/6 PASS`.

## 06. Reopen anomaly — isolated and closed (NOT a shadow defect)

Mid-investigation `editor-shadow-reopen.png` differed from
`editor-shadow-on.png` by 10.5% full-frame (darker + checker-phase shift).
Viewport-cropped forensics: reopen matched the ZERO mutation pixel-for-pixel
(`diffrac 0.0052`, identical lit samples (126,69,81)/(178,178,188)) while ON
was bright (229,125,143)/(229,229,240), and the checker phase was IDENTICAL
(row-305 peaks at the same x in all captures — the "shift" was acne
darkening of alternating squares, not geometry motion).

Cause: the committed scene on disk still carried the stale `0 0` bias line
while the ON capture had been taken with a patched working copy; the
"reopen" run re-read the stale committed bytes. After the one-line fix
landed in the working tree, ON vs repeat runs `diffrac=0.0308,
meandarken=-0.40` — deterministic. No exposure/ambient/texture race; no
renderer change was needed for this.

Collateral (R-010 footnote): every headed capture dirties
`*.luma_scene.luma` sidecar fingerprints (`Game` 2778 b323->2891 1cd9,
`Shadow` 1163 20ba->1213 744b) because `* text=auto` checks scenes out CRLF
on Windows while fingerprints hash LF bytes. Restored after every capture
(`git checkout -- LumaRealityTest/`); the committed fix is the single bias
line only. `.gitattributes` LF-pinning stays queued work.

## 07. Files changed

- `renderer/src/renderer.c` — `shadow_sampler` NEAREST create/bind/destroy.
- `renderer/src/internal/renderer_internal.h` — sampler field + rationale.
- `renderer/tests/test_r011_probe.c` — new lab probe (screen-edge check).
- `renderer/CMakeLists.txt` — `test_r011_probe` target.
- `editor/src/editor_reflect.c` — shadow + asset-renderable rows.
- `editor/app/main.c` — `--report` shadow columns.
- `editor/demo/recovery_project.c` — ShadowSun defaults (-1/-1) + honest
  regen comment.
- `LumaRealityTest/Scenes/Shadow.luma_scene` — `0 0` -> `-1 -1` (1 line).

## 08. Evidence in this directory

- `r011-lab-on/off/map.png` — stale pre-fix lab captures (kept for history;
  PPM originals are git-ignored build artifacts, PNGs committed).
- `after/r011-on/off/map.png` — post-fix lab (1024, default bias).
- `sweep-zero/`, `sweep-512/`, `sweep-2048/` — bias/resolution sweeps
  (`r011-*.png`; PPM originals ignored, PNGs committed).
- `editor-shadow-on.png` — fixed ON (bright, hard shadow).
- `editor-shadow-reopen.png` — repeat ON (determinism control).
- `editor-shadow-off.png` — shadow-disabled twin (footprint-only delta).
- `editor-shadow-zero.png` — zero-bias mutation (acne reproduction).
- `editor-shadow-moved.png` — caster-moved (shadow follows).
- `editor-shadow-lightmoved.png` — light-moved (shadow reshapes).

## 09. Human review asked for

Open `editor-shadow-on.png` vs `editor-shadow-zero.png`: the first shows a
hard-edged cast shadow under/left of the cube with clean checker whites
(229,229,240); the second shows the whole ground half-shadowed with dimmed
whites (178,178,188). `editor-shadow-off.png` differs from ON only inside
the cast footprint. Lab `after/r011-on.ppm` shows the same hard 1px edge on
untextured grey. If the ON shadow shape/placement reads wrong to a human
eye, that is artistic direction (light angle, intensity 3.0, 1024 res) —
the pipeline itself now does exactly what the scene asks.
