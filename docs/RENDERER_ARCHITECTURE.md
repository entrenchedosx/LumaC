# Luma Renderer Architecture

Luma Renderer (`lr_*`, Phase 13) is a small scene renderer on top of
**public LumaC only**. It owns no GPU backend code, includes no
LumaC internals, and LumaC never depends on it:

```
Luma Engine                  (future, le_*)
    |  scenes, entities, scripting, physics, audio, gameplay
    v
Luma Renderer                (this layer, lr_*)
    |  cameras, meshes, materials, draw lists, culling
    v
LumaC                        (lc_*)
    |  GPU resources, pipelines, commands, render targets
    v
Vulkan  (future: D3D12)
```

Dependency rules (strict, audited by source grep — no `Vk*`,
`vulkan/`, `windows.h`, or X11 symbols anywhere under
`renderer/`):

- Luma Renderer may use LumaC. LumaC must never depend on Luma
  Renderer.
- Luma Engine may use Luma Renderer. Luma Renderer must never
  depend on Luma Engine.

## Ownership

```
application
    |
    +-- lr_renderer  (pipelines cache, layouts, camera UBO,
    |                  fallback texture/sampler, queue)
    |     |
    |     +-- lr_mesh  (GPU vertex + index buffers, bounds)
    |     |
    |     +-- lr_material  (descriptor set: camera + texture + sampler)
    |
    +-- lc_device / lc_swapchain / lc_render_target (LumaC-owned)
```

- The application keeps the renderer, its meshes, and its materials
  alive while queued or recording; everything is NULL-safe, and
  cross-renderer use is rejected.
- Meshes own GPU buffers; materials own descriptor sets; the
  renderer owns pipelines, layouts, shaders, the camera buffer, and
  fallback resources — never the device, swapchain, or application
  render targets.
- Entries that die after submission are skipped defensively at
  render time (never dereferenced); the contract is still
  keep-alive-until-render-completes.
- Destroy renderers before their LumaC device shuts down
  (dependents-first, exactly like LumaC itself).

## Frame flow

```
lc_begin_frame(sc) -> lc_swapchain_get_encoder(sc, &enc)
lc_encoder_begin_*_pass(enc, ...)        // caller owns passes
lr_renderer_begin(renderer, &camera)     // latch camera, reset queue
lr_renderer_submit(renderer, &item) x N  // cull + queue
lr_renderer_render(renderer, enc, target) // sort, bind, push, draw
lr_renderer_end(renderer)
lc_encoder_end_render_pass(enc)
lc_end_frame(sc)
```

The renderer never begins or ends passes: it records draws into the
caller's open pass. That keeps editor compositing free (scene pass,
gizmo pass, UI sampling pass can interleave) and lets one frame mix
renderer draws with hand-written LumaC commands.

## Environment frame flow (Phase 17)

```
begin -> submits -> render_shadows(enc)
      -> render_scene(enc, w, h)  // HDR target (auto-fit) + sky first
      -> output pass -> render_output(enc, target)  // exposure + tonemap
      -> end
```

`render_scene` records PBR draws then the sky where no geometry
wrote depth; `render_output` tonemaps the HDR scene into any LDR
target (swapchain or offscreen — the tonemap pipeline is keyed by
the full target signature including depth format, since swapchain
passes always carry depth). Legacy `render()` is unchanged.
Stats gain `environment_passes`, `sky_draw_calls`,
`tonemap_passes`, `ibl_enabled`, `environment_rebuilds`
(per-frame: 1 on the building frame, 0 after); lifetime state via
`lr_renderer_get_environment_info`. Full detail:
`ENVIRONMENT_ARCHITECTURE.md`.

## Post chain, profiling, capture (Phase 18)

`render_scene` runs the optional post chain (ping-pong RGBA16F,
extent-keyed, default OFF) between items and chain head;
`render_output` tonemaps from the chain head. One verification
stage (`LR_POST_TINT_VERIFY`, identity = no-op). The tonemap set
uses an identity-guarded write-skip so two outputs (swapchain +
screenshot) share one recording. CPU profile
(`lr_frame_profile`: prepare/shadow/main/sky/post/tonemap/total
ms) + one-call diagnostics (`lr_frame_diagnostics`: frame,
viewport, draws, tris, shadows, IBL, post, CPU ms, cache status).
Public capture: `lr_renderer_capture_hdr`,
`lr_renderer_capture_output`, `lr_renderer_get_brdf_view` (all via
public LumaC readback; read only after `end_frame`). Binding
caches compare resource IDs, never pointers. Full detail:
`POST_PROCESS_ARCHITECTURE.md` and
`READBACK_ARCHITECTURE.md`.

## Meshes

- `lr_vertex`: position, normal, tangent (+handedness w), UV —
  48 bytes, PBR-ready now so normal mapping never reshapes it.
- Static GPU-only vertex (UINT32 indices) + index buffers uploaded
  through public LumaC staging; local AABB + bounding sphere.
- Primitives (cube/plane/sphere) share one attribute convention;
  pure data fillers (`lr_mesh_*_data`) stay headless-testable for
  unit tests and future glTF import.

## Materials

- Unlit foundation: base-color texture modulated by a push-constant
  color. NULL texture/sampler select the renderer-owned 1x1 white
  fallback + default sampler — no special shader branch.
- One descriptor set per material over the standard layout
  (camera UBO, sampled image, sampler); camera updates rewrite
  mapped memory, never descriptors.
- Phase 15 PBR (`LR_MATERIAL_PBR_METALLIC_ROUGHNESS`): own 64-byte
  parameter UBO (written once), 9-binding layout (camera both
  stages, lights, material, five maps, one sampler), push carries
  model + inverse-transpose normal matrices (112 bytes,
  vertex-only), cull NONE iff double-sided. Full equations, light
  model, and color-space rules live in `PBR_ARCHITECTURE.md`;
  the unlit path above is untouched.

## Lights (Phase 15)

- Submitted per-frame data (`lr_renderer_submit_light`, reset every
  begin), NOT scene entities: directional (travel direction,
  normalized on submit) + point (inverse-square-style with smooth
  range cutoff). Max 64; 65th rejected.
- One renderer-owned uniform buffer (ambient + count + 64 lights),
  persistently mapped; per-render content updates, descriptor sets
  never rebuilt. Ambient is an explicitly temporary non-physical
  fallback until IBL.

## Camera data

- CPU side: `lr_camera` value (position, view, projection, planes)
  via perspective/look_at/position helpers (renderer-local math:
  column-major, Y-up RH world, Vulkan NDC).
- GPU side: one shared 208-byte uniform buffer
  (view, proj, viewProj, camera position), rewritten once per
  `lr_renderer_begin` from the mapped pointer.

## Culling

- Per-mesh local sphere, transformed to world with a
  max-scale-conservative radius; six normalized inward frustum
  planes from the latched view-projection; touching counts as
  visible. Culled items count as submitted, never draw.
- Statistics expose submitted/visible/draws/triangles/pipeline and
  material binds plus per-kind draws (`pbr/unlit_draw_calls`),
  submitted/active lights, and the live pipeline-variant count for
  the future editor profiler.

## Object data today

- Per-object model matrix (+ unlit color) travel by push constants
  (80 bytes); PBR pushes model + normal matrix (112 bytes) with
  material data in the per-material UBO. Correct for small scenes;
  documented ceiling before storage/instance buffers and
  GPU-driven tables take over.

## Offscreen and editor use

- The renderer accepts any compatible `lc_render_target` — swapchain
  snapshots and offscreen textures share pipelines by signature, so
  scene/game/material-preview/thumbnail viewports reuse one setup.
- The editor-viewport primitive is: renderer scene into an offscreen
  target, read back or sample for UI (sampling composition itself is
  Phase 12 machinery, proven there).

## Future engine integration

```
Engine scene graph
    |  (cull, LOD, animation -> transforms + material picks)
    v
lr_draw_item stream  ->  lr_renderer_submit
    v
lc_command_encoder draws (this renderer + custom passes)
```

No scene graph, ECS, assets, or scripting live here by design —
Phase 14 (glTF) feeds materials (`lr_material_create_pbr`) and
geometry; Phase 15 lights are submitted data, not entities.

## Phase 19 notes

No renderer code changes were needed for the allocator or the
state model (both live behind public LumaC): meshes, materials,
shadow maps, HDR/post targets, and environment maps allocate
through pools transparently. New proofs live in
`test_ibl_vulkan` PARTs P19-MEM (zero per-frame allocation
churn), P19-CHURN (bounded under resource churn), and P19-AR
(imported-asset GPU memory fully returns).
