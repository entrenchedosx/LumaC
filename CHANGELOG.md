# Changelog

## Unreleased

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
