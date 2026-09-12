# LumaC Architecture

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
  they survive swapchain recreation.
- **Images** (`src/graphics/image.c`, `vulkan_image.c`): 1D/2D/3D
  GPU images with mips, array layers, and cube-compatible structure;
  default full-resource views; whole-image layout tracking (documented
  limitation vs future per-subresource state); staging uploads and GPU
  mipmap generation through the shared upload context.
- **Samplers** (`src/graphics/sampler.c`): standalone sampling
  configuration (filters, mipmap modes, address modes, LODs,
  capability-gated anisotropy). No bindings yet by design.
- **Shaders** (`src/graphics/shader.c`): SPIR-V (Vulkan) / bytecode
  modules. Independent after pipeline creation.
- **Pipelines** (`src/graphics/pipeline.c`): vertex+fragment pair
  bound to a color format. Compatibility rule is format equality
  against a fixed render-pass recipe — no render-pass handles leak.
- **Commands**: per-swapchain command pools/buffers today; a small
  device-level immediate-submit context serves staging uploads.
  No public command API yet by design.
- **Rendering** (`src/graphics/frame.c` + swapchain render pass):
  acquire, clear/draw, submit, present. Exactly one open frame per
  swapchain; fixed frames in flight.
- **Presentation** (`src/graphics/surface.c`, `swapchain.c`):
  surfaces link device+window; swapchains own images, views,
  framebuffers, and per-image present semaphores.
- **Synchronization**: per-flight fences, per-image owner fences and
  present semaphores. Out-of-date/suboptimal are recoverable results,
  not errors.
- **Utilities**: capability queries (`lc_device_get_limits`),
  backend-neutral format translation.

## Ownership rules

- Caller-created handles are caller-owned (`lc_*_destroy`).
- A handle borrows what it was built from; dependents die first,
  enforced by internal tracking lists (no reference counting):
  `pipelines -> shaders -> samplers -> images -> buffers ->
  swapchains -> surfaces -> devices -> windows -> core`. (Shaders are
  independent of pipelines post-creation but still die before their
  device; images/samplers are device children like buffers.)
- Destroying a parent first auto-destroys dependents; shutdown
  follows the same order.

## Backend strategy

- One graphics backend today (Vulkan). Public concepts are chosen to
  map onto D3D12 as well: buffers/usages/memory-model, formats,
  vertex bindings/attributes, fences/semaphores-behind-frames.
- Format translation and memory-type selection are centralized
  helpers, not scattered switches.
- Future backends (D3D12, possibly Metal/WebGPU) reuse the public
  API; only `src/graphics/<backend>/` grows.

## Known architectural debt (pre-1.0, allowed to change)

1. **Adapter/device fusion.** There is no `lc_adapter` enumeration;
   device creation picks one GPU. A future `lc_adapter` + explicit
   device-from-adapter split is the planned stabilization step, and
   multi-GPU selection depends on it.
2. **Pipeline anchored to a swapchain.** Creation takes
   `lc_swapchain*` although only its device + color format matter.
   The anchor doubles as lifetime tracking. A future render-target
   description (for offscreen rendering) should replace the anchor;
   compatibility stays format-based.
3. **Shader bytecode wording is SPIR-V-centric.** `lc_shader_desc.code`
   is shaped (`void*` + size) to also carry DXIL later, but docs say
   SPIR-V. Rename the concept to "native shader bytecode" when the
   second backend lands.
4. **Fixed frames in flight (2).** Not queryable or configurable yet;
   tuning belongs with a future performance pass.
5. **Single global tracking registry.** Fine for explicit teardown
   ordering today; multithreaded recording will need per-thread
   command contexts and clearer thread-affinity rules.
6. **No device-lost recovery.** Submit failure is fatal for the
   swapchain by documented policy; recovery is roadmap work.
7. **Coarse `vkDeviceWaitIdle` on teardown/recreate paths.** Correct
   and rare (never per-frame); finer-grained sync is future work.
