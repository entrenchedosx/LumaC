# LumaC Architecture

Phase 27 adds the engine-owned gameplay runtime foundation —
input, time, and frame lifecycle (see `INPUT_ARCHITECTURE.md`,
`INPUT_ACTIONS.md`, `TIME_ARCHITECTURE.md`,
`FRAME_LIFECYCLE.md`). Phase 26 adds the Lua gameplay scripting
runtime (interpreted
only) on top of Phase 25 assets/scenes/serialization (see
`LUA_SCRIPTING.md`, `SCRIPT_RUNTIME.md`, `SCRIPT_BINDING_API.md`,
`SCRIPT_AOT.md`, `SCRIPTING_ARCHITECTURE.md`). Phase 25 adds
engine assets, scenes, and serialization on top of
the Phase 24 engine (see `ENGINE_ASSET_ARCHITECTURE.md`,
`SCENE_ARCHITECTURE.md`, `SERIALIZATION_ARCHITECTURE.md`,
`PERSISTENT_IDENTITY_ARCHITECTURE.md`). Phase 24 added the Luma
Engine (see `ENGINE_ARCHITECTURE.md` and companions; Lua is the
permanent scripting choice, implemented in Phase 26 as an
interpreted runtime with the AOT compiler deferred).

LumaC is a backend-neutral graphics abstraction layer in C11. The
public API (`include/lumac/lumac.h`) exposes only opaque handles,
plain enums, and explicit result codes. Every Vulkan, Win32, or X11
type lives in `src/` and never crosses the public boundary.

```
Applications / Games / Editors / UI / Engines / Visualization
        |
        v
      LumaC  (opaque handles, explicit ownership, lc_result)
        |
   +----+----+
   |         |
 Vulkan    D3D12 (planned)
```

## Subsystems

- **Core** (`src/lumac.c`): `lc_init` / `lc_shutdown`, global tracking
  registry. Shutdown drains dependents-first; nothing else owns
  process-global state.
- **Platform** (`src/window.c`, `src/platform/`): native windows,
  event polling, client-area size tracking. No rendering knowledge.
- **Device** (`src/graphics/graphics.c`, `src/graphics/vulkan/`):
  adapter choice is currently fused into device creation (one GPU is
  picked deterministically). Queues: one retrieved queue per family.
- **Queues**: graphics queue drives recording/submission/upload;
  present queue drives presentation. Families may coincide.
- **Resources** (`src/graphics/buffer.c`, `vulkan_buffer.c`): generic
  GPU buffers with a three-way memory model (GPU-only, CPU-to-GPU,
  GPU-to-CPU). Buffers belong to a device, never to a swapchain, so
  they survive swapchain recreation. Every resource also carries a
  stable never-reused `lc_resource_id` (Phase 18; pointer equality
  is not identity). Vertex buffers feed per-vertex
  and per-instance bindings; index buffers (16/32-bit) feed indexed
  draws with bounds-checked ranges.
- **Push constants** (pipeline layouts): small per-draw root data
  (e.g. MVP matrices) with validated offset/size/visibility ranges;
  recorded openly per frame without descriptor work.
- **Images** (`src/graphics/image.c`, `vulkan_image.c`): 1D/2D/3D
  GPU images with mips, array layers, and cube-compatible structure;
  default full-resource views plus explicit subresource views;
  per-subresource SEMANTIC STATE tracking (Phase 19:
  `lc_resource_state`, mip/layer ranges, explicit transitions via
  `lc_encoder_transition_image`); staging uploads and GPU mipmap
  generation through the shared upload context; public readback
  (`src/graphics/readback.c`: tight deterministic CPU rows, sync
  only, TRANSFER_SRC-gated — see READBACK_ARCHITECTURE.md).
- **Synchronization** (`src/graphics/vulkan/vulkan_sync.c`, Phase
  19): states convert centrally to layouts/stages/access; render
  passes carry write→fragment-shader-read visibility (closes the
  Phase 18 debt); see SYNCHRONIZATION_ARCHITECTURE.md.
- **GPU memory** (`src/graphics/vulkan/vulkan_memory.c`, Phase 19):
  block pools per class (device images/buffers, upload, readback),
  best-fit suballocation + coalescing, dedicated policy,
  persistent mapped host blocks, non-coherent flush/invalidate,
  stats/budget/introspection; see GPU_MEMORY_ARCHITECTURE.md.
- **Pipeline cache** (device-level, lazy, Phase 18): one shared
  `VkPipelineCache` for every pipeline, optionally seeded/saved
  through `lc_device_desc.pipeline_cache_path` (atomic save,
  corrupt-safe fallback, disabled mode) — see
  PIPELINE_CACHE_ARCHITECTURE.md. Plus `src/clock.c` monotonic
  ticks for profiling.
- **Image views** (`src/graphics/image_view.c`): mip/layer ranges,
  cube/depth aspects, format rules; borrowed by images.
- **Samplers** (`src/graphics/sampler.c`): standalone sampling
  configuration (filters, mipmap modes, address modes, LODs,
  capability-gated anisotropy).
- **Resource binding** (`src/graphics/binding.c`,
  `vulkan_binding.c`): binding layouts (slots with types, counts,
  visibility) map to set layouts; binding sets allocate from a
  private growable device descriptor allocator; validated batched
  updates; pipelines take ordered layout slots shared with D3D12
  root-signature slots; per-frame set binds are recorded openly.
  Pipelines and sets hold canonical signature copies (sorted slot
  contents), so binds compare signatures, never raw layout pointers:
  destroying a layout first no longer invalidates matching pipelines
  or sets.
- **Shaders** (`src/graphics/shader.c`): SPIR-V (Vulkan) / bytecode
  modules. Independent after pipeline creation.
- **Pipelines** (`src/graphics/pipeline.c`): vertex+fragment pair
  with a structural render-target signature (color count/formats,
  depth format, samples; extent-independent). Compatibility is
  content-based (hash fast-path, structural resolve) — never target
  or swapchain pointer identity. Raster state (cull, front face),
  depth test/write (LESS), and push-constant ranges included.
- **Render targets** (`src/graphics/render_target.c`,
  `vulkan_render_target.c`): offscreen color (up to 8) + optional
  depth with non-owning views (formats/dimensions/samples inferred,
  never duplicated). One lazily created framebuffer per target,
  reused across load/store variants. Swapchains expose a borrowed
  target snapshot (same signature machinery, presentation final).
- **Commands** (`src/graphics/encoder.c`, `vulkan_encoder.c`):
  borrowed per-frame encoders with an explicit state machine (one
  pass max, no nesting, no submit while open). Explicit offscreen
  and swapchain passes share the frame command buffer with the
  legacy implicit pass (mutually exclusive). Indexed/instanced draws
  map directly to Vulkan/D3D12 instanced draws. No public command
  API beyond encoders yet by design.
- **Render-pass cache** (device-level, lazy): one VkRenderPass per
  structure + load/store + presentation-final key, shared by
  pipelines (canonical policy at creation) and passes. Never
  evicted; destroyed with the device. No per-frame allocation
  anywhere on the record path.
- **Rendering** (`src/graphics/frame.c` + passes): acquire, one or
  more passes (offscreen scene work, then presentation), submit,
  present. Exactly one open frame (and at most one open pass) per
  swapchain; fixed frames in flight. Offscreen color finals land
  sampled-readable so second passes validate with no extra barriers;
  swapchain finals land present-ready.
- **Presentation** (`src/graphics/surface.c`, `swapchain.c`):
  surfaces link device+window; swapchains own color images, views, a
  depth image/view, legacy framebuffers, and per-image present
  semaphores. A small device-level immediate-submit context serves
  staging uploads outside frames.
- **Synchronization**: per-flight fences, per-image owner fences and
  present semaphores. Out-of-date/suboptimal are recoverable results,
  not errors.
- **Utilities**: capability queries (`lc_device_get_limits`),
  backend-neutral format translation.

## Ownership rules

- Caller-created handles are caller-owned (`lc_*_destroy`), except
  borrowed handles which must never be destroyed
  (`lc_swapchain_get_render_target`, `lc_swapchain_get_encoder`).
- A handle borrows what it was built from; dependents die first,
  enforced by internal tracking lists (no reference counting):
  `pipelines -> binding sets -> binding layouts -> shaders ->
  samplers -> render targets -> image views -> images -> buffers ->
  swapchains -> surfaces -> devices -> windows -> core`. (Shaders are
  independent of pipelines post-creation but still die before their
  device; binding anchors are compared, never dereferenced, so dead
  layouts fail closed. Pipelines hold only structural target
  signatures and survive any swapchain; targets borrow views, so a
  destroyed view takes its targets with it. Images/samplers are
  device children like buffers.)
- Destroying a parent first auto-destroys dependents; shutdown
  follows the same order.

## Backend strategy

- One graphics backend today (Vulkan). Public concepts are chosen to
  map onto D3D12 as well: buffers/usages/memory-model, formats,
  vertex bindings/attributes, binding layouts/sets (root
  signatures/tables, CBV/SRV/UAV/samplers),
  fences/semaphores-behind-frames.
- Format translation and memory-type selection are centralized
  helpers, not scattered switches.
- Future backends (D3D12, possibly Metal/WebGPU) reuse the public
  API; only `src/graphics/<backend>/` grows.

## Known architectural debt (pre-1.0, allowed to change)

1. **Adapter/device fusion.** There is no `lc_adapter` enumeration;
   device creation picks one GPU. A future `lc_adapter` + explicit
   device-from-adapter split is the planned stabilization step, and
   multi-GPU selection depends on it.
2. **Pipeline creation anchor (removed Phase 13).** Creation used
   to take `lc_swapchain*`; it now takes only a device plus a
   mandatory structural render-target description
   (`lc_swapchain_get_render_target_desc` builds presentation ones
   deliberately). Compatibility and lifetime were already structural.
3. **Shader bytecode wording is SPIR-V-centric.** `lc_shader_desc.code`
   is shaped (`void*` + size) to also carry DXIL later, but docs say
   SPIR-V. Rename the concept to "native shader bytecode" when the
   second backend lands.
4. **Fixed frames in flight (2).** Not queryable or configurable yet;
   tuning belongs with a future performance pass.
5. **Single global tracking registry.** Fine for explicit teardown
   ordering today; multithreaded recording will need per-thread
   command contexts and clearer thread-affinity rules. Encoders are
   frame-bound today (no headless/offscreen-only queues yet).
6. **No device-lost recovery.** Submit failure is fatal for the
   swapchain by documented policy; recovery is roadmap work.
7. **Coarse `vkDeviceWaitIdle` on teardown/recreate paths.** Correct
   and rare (never per-frame); finer-grained sync is future work.
8. **Binding-layout anchor discipline (fixed Phase 11).** Pipelines
   and sets hold canonical signature copies compared by contents at
   bind time; destroying a layout first no longer invalidates matching
   dependents. Revisit only if real use hits an edge.
9. **Render-pass cache never evicted (Phase 12).** Entries are tiny
   (one VkRenderPass per structure+policy key, shared); worst case is
   a handful per device. Eviction/LRU only if a future workload
   proves it matters.
10. **Stencil minimal (Phase 12).** Stencil aspects/ops validate but
    rendering stays depth-only (DONT_CARE stencil enforced); real
    stencil work arrives with shadows/picking.
11. **No MSAA resolve (Phase 12).** Sample counts participate in
    compatibility structurally, but multisampled rendering has no
    resolve path yet; only single-sampled rendering is exercised.
12. **Synchronous readback only (Phase 18).** `lc_image_readback`
    stalls by design; `lc_readback_request/poll/map` are reserved
    names, not implementations.
13. **No GPU timestamps (Phase 18).** Profiling is CPU-side
    recording time; `lc_clock_*` is the only clock API.
14. **Post chain is single-stage (Phase 18).** One optional tint
    stage proves chaining; bloom arrives as the first real
    multi-pass consumer.
15. **Synchronous uploads/readbacks only (Phase 19).** Transfers
    run on the graphics queue via immediate-submit; async copy and
    compute queues are roadmap work (states already queue-neutral).
16. **No defragmentation moves (Phase 19).** The allocator
    coalesces on free but never relocates live resources; moving
    compaction belongs with streaming/transient aliasing later.

## AAA roadmap note (Phase 19 preparation, Phase 22)

This phase prepares — without implementing — later
multithreaded command recording (per-image tracking needs
per-queue epochs), async copy/compute queues (barriers already
queue-family-parameterized internally), resource streaming
(suballocation + stable IDs + budgets exist), indirect
rendering and GPU-driven culling (states named, tracking
follows), bindless descriptors, LOD/virtualized geometry
systems, render graphs with transient aliasing (explicitly
distinct from normal suballocation), and a D3D12 backend (all
public memory/sync concepts map: classes, states, stats,
budget).

Phase 21 status: multithreaded recording, async transfer
queues, buffer-state tracking, compute dispatch, indirect
drawing, and renderer GPU-driven culling are implemented
(compute/indirect/GPU-driven docs); overlapping async compute,
Hi-Z occlusion, GPU LOD, meshlets, and render graphs remain
future layers over these boundaries.

## Renderer layering

LumaC core stays a low-level graphics API. Luma Renderer
(`renderer/`, `lr_*`, separate static library on public LumaC only —
see `RENDERER_ARCHITECTURE.md`) owns meshes, materials,
cameras, draw lists, and frustum culling today; lights, PBR,
shadows, IBL, post-processing, render graphs, and GPU-driven
rendering build on it next:

```
Luma Engine
    |
    v
Luma Renderer
    |
    +-- scene render target (HDR + depth)
    +-- shadow targets (depth-only)
    +-- picking target (MRT metadata)
    +-- editor viewports (SceneViewport, MaterialPreview,
    |      MeshPreview, GameViewport, ThumbnailRenderer)
    +-- post-process chain (sampled previous pass)
    |
    v
LumaC render-target / command system (this repository)
    |
    +-- offscreen targets + MRT + load/store
    +-- command encoders, one frame, many passes
    +-- presentation (swapchain only)
```

Likely resource pattern: one global set (camera + lights), one set
per material (textures + samplers), per-object uniform/storage data —
all expressible with today's binding layouts, sets, and arrays. A
future editor renders its scene into an offscreen image, exposes the
sampled image to its UI, and displays it inside a viewport panel:
`examples/render_to_texture` is the first primitive version of that
workflow (fixed 512x512 scene target, sampled fullscreen). No
high-level system is implemented here on purpose.

Phase 23: Hi-Z occlusion, GPU LOD, indirect-count, and the renderer-owned render graph are implemented (see HIZ/OCCLUSION_CULLING/GPU_LOD/RENDER_GRAPH_ARCHITECTURE.md); meshlets/mesh shaders/bindless/virtual geometry remain deferred.

Phase 24: Luma Engine foundation (`ENGINE_ARCHITECTURE.md` +
identity/hierarchy/integration/scripting docs). The renderer gains
only the stable temporal key (`lr_draw_item.instance_id` → LOD
hysteresis owner tags, `GPU_LOD_ARCHITECTURE.md` addendum below);
no renderer features are added.
