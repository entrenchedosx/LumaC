# Changelog

## Phase 21

- Added backend-neutral compute: `LC_SHADER_STAGE_COMPUTE`,
  `lc_compute_pipeline` (shared binary pipeline cache, warm/cold/
  corrupt/disabled coverage), dispatch on frame encoders and
  compute worker lists, graphics-queue fallback, compute queue
  discovery with honest capabilities, and an isolated
  cross-queue dispatch proof (overlapping async compute
  explicitly deferred).
- Added buffer state tracking (`TRANSFER_SRC/DST`, `VERTEX/INDEX/
  UNIFORM/STORAGE_READ`, `STORAGE_WRITE`, `INDIRECT_READ`) with
  execution/access barriers, `lc_encoder_transition_buffer`,
  storage-image bindings (`SHADER_READ_WRITE`/`GENERAL`), and a
  sync `lc_buffer_read` test/debug path.
- Added indirect drawing (`lc_encoder_draw_indirect`,
  `lc_encoder_draw_indexed_indirect`): explicit-width commands,
  native multi-draw when enabled else an honest loop,
  `INDIRECT_READ` gating (barriers live outside passes),
  `LC_BUFFER_USAGE_INDIRECT`; indirect-count deferred with the
  device capability reported.
- Added renderer GPU-driven mode: per-(mesh, material,
  shadow-flag) groups with shared instance buffers, per-flight
  visible/counter/indirect resources, atomic-append compaction
  with capacity guards, compute frustum culling (CPU oracle
  agreement required), one indexed indirect draw per group
  through the PBR-compatible instanced pipeline, CPU fallback,
  per-frame diagnostics, and a 100k-instance benchmark plus
  1000-frame endurance.
- Added `tests/test_phase21_compute_vulkan` (127 checks),
  `renderer/tests/test_gpu_driven_vulkan` (94 checks), and
  `examples/gpu_driven_scene` (`--frames/--instances/
  --cpu-culling/--gpu-culling/--no-validation/--screenshot`,
  `docs/images/gpu-driven-scene.png`).
- Fixed: GPU-only buffers gain transfer-source for download
  copies (VUID); secondary buffers always carry inheritance info
  (VUID); multi-draw/indirect-count features enabled when
  offered (VUID); compute-compatible outside-pass dispatch next
  to pending clears; group regrow preserving filled instances.

## Phase 20

- Added worker recording contexts and immutable single-use command lists with
  ordered batch execution and submission-time state validation.
- Added queue classes, dedicated-transfer diagnostics, timeline-style GPU
  completion, and configurable frames in flight.
- Added bounded async buffer/image uploads, async image readback, and
  completion-keyed deferred destruction.
- Added validation-backed tests for 1,000 uploads, oversized staging, and
  50,000 real draw recordings across four CPU threads with pixel proof, plus
  concurrent allocator churn and ThreadSanitizer coverage.

## Unreleased

- Luma AAA memory + synchronization foundation (Phase 19):
  semantic resource states (`lc_resource_state`) with per-mip/layer
  tracking, explicit `lc_encoder_transition_image`, write→read
  visibility fix in render-pass dependencies (closes Phase 18
  debt), block suballocator (4 classes, best-fit + coalescing,
  dedicated policy, warm-reserve reclamation, internal lock),
  non-coherent flush/invalidate with persistent mapping, pool
  staging (no per-op dedicated allocations), memory stats/budget/
  per-resource introspection, `tests/test_sync_vulkan` (1000-
  transition ping-pong, depth, upload/copy paths, mixed states),
  `tests/test_memory` (10k buffers→1 block, fragmentation,
  dedicated, OOM rollback, benchmarks, scale workload),
  renderer endurance + churn + asset-cycle memory proofs, new docs
  (SYNCHRONIZATION/GPU_MEMORY), pre-1.0 API review notes
- Fixed: depth/non-sampled images settling to SHADER_READ (VUID);
  legacy swapchain pass dependencies vs cached passes
  (framebuffer incompatibility); mid-test teardown races
  (drain before destroy); UNORM .5 rounding assumption in tests
- Luma infrastructure hardening (Phase 18): public image readback
  (`lc_image_query_readback`/`lc_image_readback`: tight rows,
  mip/layer/depth, loud TRANSFER_SRC gating, sync with reserved
  async names), stable resource IDs + identity-compared binding
  caches (+ ABA stress), persistent Vulkan pipeline cache (app
  path, atomic save, corrupt-safe, disabled mode, introspection),
  backend-neutral monotonic clock, renderer CPU profiling
  (`lr_frame_profile`) + diagnostics snapshot, internal
  post-process chain (ping-pong intermediates, tint verification
  stage), renderer HDR/LDR capture + BRDF view borrower,
  `ibl_scene --screenshot` via Luma-authored `tools/png_mini.h`,
  `docs/images/ibl-scene.png`, 500-frame endurance, new docs
  (READBACK/PIPELINE_CACHE/POST_PROCESS), pre-1.0 API debt list
- Fixed: example freed upload pixels before `lc_image_write`
  (use-after-free → negative HDR); tonemap set rewrite between
  binds invalidated shared recordings (identity-guarded skip);
  non-SAMPLED depth settling to SHADER_READ (VUID); all in-tree
  `lc_device_desc` zero-initialized
- Luma Renderer environment lighting (`lr_*`, Phase 17): HDR
  equirect sources become split-sum IBL (128-cube with full mip
  chain, 32 irradiance, 64 prefilter x7 mips, 256 BRDF LUT, fixed
  tap counts), sky pass, auto-fit HDR scene target, exposure EV +
  NONE/ACES tonemap output pass, params-only intensity/yaw (lighting
  and sky rotate together), ambient fallback iff no env
  (bit-identical legacy), render_scene/render_output frame flow,
  extended stats + read-only environment introspection,
  `examples/ibl_scene`, `docs/ENVIRONMENT_ARCHITECTURE.md`
- Luma Assets HDR (`la_hdr_*`): strict RGBE decode, RNE
  float/half conversion, R16F GPU upload with path cache,
  procedural `env_gradient.hdr` fixture
- Tonemap pipeline keyed by full target signature incl. depth
  format (swapchain passes always carry depth)
- Luma Renderer shadows (`lr_*`, Phase 16): directional (fitted
  ortho) + spot (cone perspective) shadow maps onto PBR (3x3 manual
  PCF, constant + normal bias, opt-in cast/receive, LR_MAX_SHADOWS
  4 with unshadowed over-capacity), per-light resolution/biases,
  single-prepare frame flow with loud flow guards, extended stats +
  read-only slot/light introspection, `examples/shadow_scene`,
  `docs/SHADOW_ARCHITECTURE.md`, `docs/images/shadow-scene.png`
- LumaC generic depth plumbing (no shadow semantics): optional
  fragment stage for depth-only pipelines, sampled-depth final
  layout, `lc_device_wait_idle`
- Test-harness depth-tested main targets (painter order is
  heap-address noise); renderer examples drain the device before
  teardown; renderer shaders compile with `glslc -O`
- Luma Renderer PBR (`lr_*`, direct lighting only): explicit
  UNLIT/PBR material kinds, PBR metallic-roughness materials (own
  parameter buffer, five texture roles with neutral fallbacks,
  double-sided variants), Cook-Torrance BRDF (GGX/Smith/Schlick/
  Lambert, metallic workflow, 0.05 roughness floor), directional +
  point lights (LR_MAX_LIGHTS 64, uniform buffer, mapped updates),
  temporary ambient fallback, normal mapping with handedness +
  inverse-transpose normal matrices, occlusion-on-ambient,
  factor-or-texture emissive, per-kind pipeline variants, extended
  stats + read-only introspection, `examples/pbr_scene`,
  `docs/PBR_ARCHITECTURE.md`, `docs/images/pbr-scene.png`
- Luma Assets imports glTF materials as real PBR (all roles,
  scales/strengths, double-sided, alpha metadata); `model_viewer`
  lights its model
- Renderer gains `lr_renderer_submit_light`, `lr_renderer_set_ambient`,
  `lr_renderer_get_ambient`, `lr_renderer_get_pipeline_count`,
  `lr_material_create_pbr`, `lr_material_get_type`,
  `lr_material_get_pbr_info` (additive, pre-1.0)
- Luma Assets (`la_*`, separate `luma_assets` library on public Luma
  Renderer + LumaC only): glTF 2.0 ingestion (vendored cgltf v1.15,
  stb_image) with strict validation, generated normals/tangents,
  hierarchy with raw + decomposed transforms, full PBR metadata with
  PBR material upload, role-qualified shared texture/sampler
  caches, model-space bounds, deterministic file-order submission via
  `la_model_submit`, `examples/model_viewer`, `docs/ASSET_ARCHITECTURE.md`
- Renderer gains `lr_renderer_get_device` (borrower for upper layers;
  additive, pre-1.0)
- Luma Renderer (`lr_*`, separate `luma_renderer` library on public
  LumaC only): reusable GPU meshes (PBR-ready vertices, bounds,
  cube/plane/sphere), unlit materials with fallback texture/sampler,
  cameras, quaternion transforms, frustum-culled draw lists with
  opaque sorting and statistics, per-target pipeline cache,
  offscreen/multi-viewport rendering, `examples/renderer_scene`
- Pipeline creation no longer takes a swapchain (breaking, pre-1.0):
  `lc_graphics_pipeline_create(device, desc, out)` with a mandatory
  structural render-target description, plus
  `lc_swapchain_get_render_target_desc` for presentation pipelines
- Render-target view getters, pass-cache count debug accounting;
  copy-to-buffer rejects sourceless images loudly instead of
  driver-dependent behavior
  (non-owning color/depth views, up to 8 colors, depth-optional,
  sample-aware), structural `lc_render_target_desc` compatibility
  (hash fast-path, structural resolve), borrowed swapchain targets,
  `max_color_attachments` device limit
- Command encoders: borrowed `lc_command_encoder` per open frame with
  an explicit state machine (one pass max, no nesting, no submit
  while open); explicit offscreen (`lc_encoder_begin_render_pass`)
  and swapchain (`lc_encoder_begin_swapchain_pass`) passes with
  backend-neutral load/store ops; full generic record path (bind
  pipeline/sets/vertex/index, push constants, indexed/instanced
  draws); legacy swapchain-bound recording kept as mutually-exclusive
  convenience
- Pipelines decoupled from swapchain identity: structural target
  signature in the desc (legacy all-zero infers from the anchor),
  device-only lifetime (survive swapchain destroy), multi-attachment
  blending, sample-count-aware creation against the shared
  render-pass cache (no per-frame allocation)
- Render-to-texture proven: `examples/render_to_texture` (cube scene
  into 512x512, sampled to swapchain; offscreen survives resizes)
- Image layouts extended (color/depth attachment endpoints);
  attachment finals land sampled-readable so second passes validate
  with no extra barriers
- First 3D foundation: indexed drawing (
  (`lc_draw_instanced`, per-instance vertex input, instanced indexed
  draws), push constants (`lc_push_constant_range`,
  `lc_push_constants`), raster state (cull mode, front face),
  depth testing (`lc_clear_depth`, swapchain-owned depth buffer,
  `lc_swapchain_get_depth_format`), `examples/cube_3d` (indexed
  textured cube x2 instances, rotating push-constant MVP)
- Pipelines carry raster/depth/push state with depth-compatible render
  passes; binding matches by canonical signatures (layout destruction
  no longer invalidates matching pipelines/sets)
- Resource bindings: `lc_image_view` (mip/layer ranges, cube/depth
  aspects), `lc_shader_visibility`, binding layouts/sets with
  uniform/storage/sampled/storage-image/sampler slots, descriptor
  arrays, validated batched updates, device-level descriptor
  allocator, `lc_bind_binding_set`
- Per-subresource image layout tracking; sampled-state-gated binding
  with vertex/fragment/compute stage mapping
- Pipelines take ordered binding-layout slots; `examples/textured_quad`
  (mipmapped checkerboard, animated uniform, no descriptor rebuilds)
- `LC_ERROR_PIPELINE_INCOMPATIBLE` now also covers layout mismatch
- Image/texture foundation: `lc_image` (1D/2D/3D, mips incl. full
  chain, array layers, cube-compatible flag, multisample field),
  default full-resource views, whole-image layout tracking
- Staging uploads via `lc_image_write` (descriptor-based regions),
  GPU linear-blit mipmap generation, exact upload round-trips
- `lc_sampler` (filters, mipmap modes, address modes, LODs,
  capability-gated anisotropy); device negotiates samplerAnisotropy
- Depth formats in `lc_format` with color/depth/stencil metadata;
  `examples/texture_upload`
- `LC_ERROR_IMAGE_CREATION_FAILED/SAMPLER_CREATION_FAILED/UNSUPPORTED`
- Production resource foundation: backend-neutral `lc_format` system
  with centralized Vulkan translation
- Generic `lc_buffer` abstraction (GPU-only / CPU-to-GPU / GPU-to-CPU
  memory model), persistent coherent mapping, bounds-checked
  `lc_buffer_write` with staging uploads via a device upload context
- Backend-neutral vertex bindings/attributes, pipeline vertex-input
  state, `lc_bind_vertex_buffer`
- `examples/vertex_triangle`: identical triangle from a real GPU
  vertex buffer; exact staging round-trip verified in tests
- `lc_device_get_limits`, `lc_swapchain_get_format`
- `docs/ARCHITECTURE.md` (subsystems, ownership, backend strategy,
  known pre-1.0 debt) and `docs/API_DESIGN.md` (binding principles)
- First triangle: `lc_shader` (SPIR-V modules), `lc_pipeline`
  (empty layout, dynamic viewport/scissor, no culling), minimal
  render pass owned by the swapchain, `lc_bind_pipeline` + `lc_draw`
- Clears now flow through render-pass load ops (deferred color,
  `vkCmdClearAttachments` once recording); submit waits at
  color-attachment output
- Swapchain images require transfer-destination usage for clears
- `examples/triangle` with committed and build-time-compiled SPIR-V
- Recoverable `LC_ERROR_PIPELINE_INCOMPATIBLE` for format drift
- Frame lifecycle: `lc_begin_frame` / `lc_clear_color` / `lc_end_frame`
  with acquire, submit, and present
- Two frames in flight, per-image present semaphores and ownership
  tracking
- Recoverable `LC_ERROR_SWAPCHAIN_OUT_OF_DATE` and `LC_SUBOPTIMAL`
  results; resize/minimize workflow in `examples/clear_screen`

## 0.1.0-dev

- Core `lc_init` / `lc_shutdown` lifecycle and version API
- Native Win32 windowing; X11 backend implemented
- Vulkan device foundation: instance, validation, GPU selection,
  logical device with per-family graphics queues
- Vulkan surfaces with present-queue discovery
- Swapchains with capability-driven selection, image views, and
  transactional recreation
- Static/shared CMake builds, headless unit tests, Vulkan integration
  tests, Windows verified, Linux compile verified
