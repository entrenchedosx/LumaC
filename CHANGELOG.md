# Changelog

## Unreleased

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
