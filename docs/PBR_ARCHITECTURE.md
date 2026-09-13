# Luma PBR Architecture

Direct-lighting metallic-roughness PBR (Phase 15). PBR lives in
**Luma Renderer**; material metadata comes from **Luma Assets**;
GPU primitives come from **LumaC**. No lights, BRDF, normal
mapping, or material semantics exist in LumaC (audited).

## Material model

Two explicit kinds (`lr_material_type`) share one registry, one
queue, and one material-sorted bind path; pipelines vary by kind:

- `LR_MATERIAL_UNLIT` — Phase 13 path, untouched (push color,
  3-binding layout, BACK culling always).
- `LR_MATERIAL_PBR_METALLIC_ROUGHNESS` — `lr_pbr_material_desc`,
  own 64-byte GPU parameter block (written once, never per frame),
  9-binding layout, per-kind push (model + normal matrix, 112 bytes
  vertex-only), cull NONE iff `double_sided`.

`lr_alpha_mode` (OPAQUE/MASK/BLEND) is stored in the parameter
block for forward use; Phase 15 renders OPAQUE only.

Fallbacks (bound for every missing map — shaders never branch):
white base, neutral metal/rough texel `(0,1,1,1)` in G/B so
factors pass through, flat tangent normal `(128,128,255)`,
white occlusion, **white emissive** (deliberate: emissive =
factor x texel, so factor-only emission — legal glTF — needs the
identity; the zero default factor keeps it dark).

UNORM8 honesty note: `(128,128,255)` decodes to a normal tilted
~0.32 deg off flat. At very low roughness the GGX lobe is narrower
than that tilt, so absolute facing highlights shift slightly;
the CPU reference models the tilt and pixel tests pin it. This is
physical (real normal maps quantize the same way), not a bug.

## BRDF (Cook-Torrance metallic-roughness, direct lighting only)

Per light, with `N` (possibly normal-mapped), `L`, `V`, `H`:

```
rough = clamp(rough, 0.05, 1.0);  a = rough^2
F0 = mix(vec3(0.04), baseColor, metallic)
D = a^2 / (pi * (NdotH^2 * (a^2 - 1) + 1)^2)          // GGX
k = (rough + 1)^2 / 8   (Schlick-GGX direct-light form)
G = G1(NdotV) * G1(NdotL), G1(x) = x / (x*(1-k) + k)  // Smith
F = F0 + (1 - F0) * (1 - VdotH)^5                     // Schlick
spec = D * F * G / max(4*NdotV*NdotL, 1e-4)
kD = (1 - F) * (1 - metallic)                         // metals: ~0
Lo += (kD * baseColor / pi + spec) * NdotL * atten * radiance
```

Energy: metals lose diffuse via `(1-metallic)`; dielectrics use
F0 = 0.04. Roughness floors at **0.05** (documented minimum —
avoids D singularities; `r=0.0` shades byte-identical to 0.05,
pinned by test). `NdotL <= 0` skips before `H` is built (the
half-vector degenerates for exact back-lighting).

Channels (glTF, no guessing): metal = factor x texel.**B**,
rough = factor x texel.**G**, occlusion = texel.**R**.

## IBL (Phase 17, split-sum; direct lighting above unchanged)

With an environment attached, one uniform branch adds indirect
light (no env = bit-identical legacy, pinned by 92 checks):

```
diffIBL = irradiance(N) * albedo * (1 - metal) / pi
specIBL = prefilter(R, lod = rough * (mips - 1)) * (F0 * A + B)
indirect = (diffIBL * occ + specIBL * mix(1, occ, 0.5)) * intensity
```

AO scales diffuse fully, specular half; direct/emissive never.
`F0` carries the metal tint (dielectrics neutral, metals tinted —
pinned by a copper grid). Full preprocessing, sky, HDR target,
exposure/ACES, and frame flow live in
`ENVIRONMENT_ARCHITECTURE.md`; the tonemap pass is separate
from this shader by rule.

## Lights

Submitted per-frame data (`lr_renderer_submit_light`), reset every
`lr_renderer_begin`, max `LR_MAX_LIGHTS` = **64** (65th rejected).
No entities/nodes yet.

- Direction: `direction` is the light TRAVEL direction (L = -dir),
  normalized on submit; zero rejected.
- Point: inverse-square-style with smooth cutoff —
  `att = win^2 / (d^2 + 1)`, `win = saturate(1 - (d/range)^4)`;
  range must be > 0. Documented approximation (unit softening
  avoids the d=0 singularity); no arbitrary linear/quadratic
  falloff constants.
- Storage: one renderer-owned uniform buffer (ambient + count +
  64 lights, 3104 bytes, persistently mapped). Contents update per
  render; descriptor sets are NEVER rebuilt for lights.
- Ambient: temporary non-physical fallback (default 0.03 gray,
  settable, readable). Occlusion damps **ambient only**
  (`mix(1, texel.R, strength)`) — darkening direct specular with
  it would be wrong with no IBL to gate. Emissive adds after.
- Camera position comes from the existing camera buffer (now
  visible to both stages in the PBR layout).

## Spaces and transforms

World-space interpolants (position, normal, tangent+handedness,
uv); no reconstruction in fragment. Normals/tangents use the
CPU-side **normal matrix** (inverse-transpose 3x3, pushed per
draw) — never the raw model matrix — so rotation + non-uniform
scale shade correctly (mirrors fold sign into cofactors;
negative-scale winding stays a documented limitation shared with
the cull state). TBN = `[T, w*(N x T), N]`; backfaces flip the
shading normal when culling is disabled.

## Color spaces

sRGB: base color, emissive. Linear: metallic-roughness, normal,
occlusion. No manual gamma anywhere — GPU formats convert, and an
sRGB-target pixel test pins encode behavior against the CPU
reference (linear targets compare raw).

## glTF mapping (assets bridge)

`baseColorFactor`, `metallic/roughnessFactor`, all five texture
roles with role-correct color spaces, `normalTexture.scale`,
`occlusionTexture.strength` (cgltf scale), `emissiveFactor`,
`doubleSided`, alpha mode stored-but-OPAQUE. One shared sampler
per material (base-color view's, else first available) —
documented approximation until per-map samplers land. Default
(no-source) materials are PBR white (metallic/roughness per glTF
defaults 1.0/1.0).

## Pipeline variants and binds

Cache key: target signature + material type + cull mode; bounded
(16). Material sorting still dedups binds across kinds (sets are
rebound on pipeline switches since layouts differ); stats split
`pbr/unlit_draw_calls`, `submitted/active_lights`, plus a live
variant count for endurance/profiling. Introspection
(`lr_material_get_type/pbr_info`, stats, pipeline count) is
read-only and MCP-ready; no MCP itself.

## Deferred (unchanged)

Shadows, IBL/environment/skybox, HDR, tonemapping, bloom, SSAO,
SSR, transparency, deferred/clustered, animation, scene graph,
ECS, physics, audio, scripting, editor, MCP, compute, indirect,
bindless, GPU-driven, render graph, D3D12, Wayland.
