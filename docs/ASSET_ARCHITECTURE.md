# Luma Assets Architecture

Luma Assets (`la_*`, Phase 14) is glTF 2.0 model ingestion on top of
**public Luma Renderer + LumaC only**. Neither LumaC nor Luma Renderer
depends on it, and the renderer stays usable without this module:

```
Luma Engine                  (future, le_*)
    |  scenes, entities, scripting, physics, audio, gameplay
    v
Luma Assets                  (this layer, la_*)
    |  glTF parsing, hierarchy, PBR metadata, shared caches
    v
Luma Renderer                (lr_*)
    |  cameras, meshes, materials, draw lists, culling
    v
LumaC                        (lc_*)
    |  GPU resources, pipelines, commands, render targets
    v
Vulkan  (future: D3D12)
```

Dependency rules (strict, audited by source grep over
`assets/include` + `assets/src` — no `Vk*`, `vulkan/`,
`windows.h`, X11, `graphics_internal`, `lumac_internal`, or
`renderer_internal`; `tests/` takes the same white-box privilege as
LumaC's own integration tests):

- Luma Assets may use Luma Renderer and LumaC. Neither may depend on
  Luma Assets.
- The only renderer addition Phase 14 needed is the
  `lr_renderer_get_device` borrower (upper layers allocate through
  public LumaC APIs without reaching into internals).

## HDR environment sources (Phase 17)

`la_hdr_decode` (strict HDR gate over `stbi_loadf`),
`la_float_to_half` (round-to-nearest-even; Inf/NaN/subnormal
correct) + `la_half_to_float`, `la_hdr_load` (path cache, R16F GPU
image, TRANSFER_SRC for readback), `la_hdr_get_view`. No lighting
semantics live here — decode and upload only; split-sum, sky, and
tonemap are renderer work (`ENVIRONMENT_ARCHITECTURE.md`).

## Ownership

```
application
    |
    +-- la_asset_manager  (shared texture/sampler caches, model list)
    |     |
    |     +-- la_model  (meshes, materials, nodes, metadata;
    |     |              borrows cache entries by ref)
    |     |
    |     +-- GPU textures + samplers (manager-owned, ref-counted)
    |
    +-- lr_renderer / lc_device  (borrowed by the manager; outlive it)
```

- Models are caller-owned and must be destroyed before their
  manager; managers before their renderer/device
  (dependents-first, like every Luma layer).
- Cache entries are insertion-ordered lists, never hash iteration:
  file order is preserved everywhere (nodes, meshes, primitives,
  materials, textures), so sharing reports and submissions are
  deterministic.
- CPU vertex/index/texel copies are freed after upload; only
  lightweight metadata is kept (hierarchy, PBR data, bounds,
  source counts).

## Import pipeline

```
.glb / .gltf on disk
  -> parse + load buffers (vendored cgltf v1.15, centralized file
     callbacks so missing externals map to NOT_FOUND precisely)
  -> strict accessor reads (stride/offset/normalized aware,
     bounds-checked per element; sparse rejected as UNSUPPORTED)
  -> missing normals generated (area-weighted), missing tangents
     generated (Lengyel-style + Gram-Schmidt + handedness in w;
     MikkTSpace conformance explicitly deferred)
  -> hierarchy validated (double parenting, cycles, reachability)
     with raw + decomposed transforms kept side by side
  -> materials resolved (Phase 15: full PBR metadata uploaded as
     real `lr_material` PBR objects — all five texture roles with
     role-correct color spaces, scales/strengths, double-sided,
     alpha mode stored-but-OPAQUE; one shared sampler per material:
     base-color view's, else first available) with role-qualified
     texture keys
  -> meshes/materials uploaded through public renderer/LumaC APIs
  -> model-space bounds over instances; model linked on success
     only (partial state unwinds, failed imports leave no residue)
```

## Conventions (no-conversion list)

glTF 2.0 already matches Luma's world convention, so the importer
converts nothing and documents that once:

- Y-up right-handed, column-major matrices copied **untransposed**;
  quaternions are `(x, y, z, w)` on both sides.
- Negative-determinant node matrices absorb one mirror into
  `scale.x` (rotation stays proper; recovery holds up to float32
  rounding — the stored matrix stays the source of truth).
- UV origin is top-left in glTF and Vulkan: no V flip; images
  upload row-major exactly as decoded.
- Winding is preserved (fixtures reuse the proven renderer cube
  corner orders for CCW-outward faces).
- Color-space role is carried by the GPU format, never gamma math:
  base-color/emissive upload `RGBA8_SRGB`, data (normal,
  metallic-roughness, occlusion) uploads `RGBA8_UNORM`. The same
  source image in both roles is two GPU resources (key suffix
  `|srgb` / `|lin`); identical sampler parameters share one
  sampler.

## Errors

`lc_result` / `lr_result` values never leak (`la_map_lc/lr`
translates). `la_asset_manager_get_last_error` keeps the detail:

| situation                              | code            |
|----------------------------------------|-----------------|
| NULL/dead arguments, foreign renderer  | INVALID_ARGUMENT|
| missing file or external reference     | NOT_FOUND       |
| non-triangle mode, sparse accessor     | UNSUPPORTED     |
| malformed JSON/GLB, bad accessor, bad hierarchy, corrupt image | IMPORT |
| GPU upload failure                     | RENDER          |

## Deferred (not Phase 14)

Skinning/animation, `KHR_*` material/lighting extensions, sparse
accessors, non-triangle primitive modes, MikkTSpace tangents,
alpha-blend rendering (modes parsed, rendered as OPAQUE),
interleaved-or-indexed anything beyond stride/offset accessors.

## Fixtures and tests

Deterministic, authored-from-scratch inputs (no license
encumbrance), regenerated with `py -3 assets/generate_fixtures.py`:

- `assets/BoxTextured.glb` — example asset (indexed box, normals +
  UVs, embedded checker, one node).
- `assets/tests/fixtures/fixture.glb` — sharing/hierarchy/texture
  roles/interleaved-stride/generated-attribute coverage (5 nodes,
  4 primitives, 1 image in 2 roles, 2 samplers, 7 instances).
- `fixture.gltf` / `.bin` / `.png` — same scene via external
  references.
- `malformed/*` — nine negative inputs with documented codes.
- `env_gradient.hdr` (Phase 17) — procedural RLE RGBE: vertical
  gradient 0.05-4.0 plus a 25x spot; validated by decode-back.
  Public domain by construction (generator documented in-file).

`test_assets` (headless: geometry, decomposed transforms, arg
validation, sampler/file helpers, pre-upload negatives),
`test_assets_vulkan` (live loads, sharing counts, hierarchy,
metadata, bounds, submission stats, negatives that reach upload, a
short validated render run). `examples/model_viewer` loads any
`.glb`/`.gltf` (default: the example asset) through the three-line
frame body: begin, `la_model_submit`, render.
