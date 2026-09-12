# LumaC API Design Principles

The API is pre-1.0: breaking changes are allowed while the
architecture is being established. What follows is binding intent,
not marketing.

## Opaque handles

Every GPU or OS object is an opaque `lc_*` pointer. Callers never see
struct layouts, sizes, or backend fields. This keeps the ABI flexible
and makes invalid use (dead handles) detectable via tracking lists.

## Explicit ownership

- `lc_*_create` hands ownership to the caller; `lc_*_destroy`
  releases it and is always NULL-safe.
- Handles borrow what they were built from (a swapchain borrows its
  device and surface; a buffer borrows its device). Destroying a
  parent first auto-destroys dependents; shutdown drains
  dependents-first. No reference counting.
- Output pointers are always cleared to NULL on failure.

## Backend-neutral enums

Public enums describe concepts (`lc_format`, `lc_buffer_usage`,
`lc_memory_usage`, `lc_shader_stage`), never Vulkan flags or DXGI
values. Translation to backend constants is centralized in the
backend layer. A concept must make sense for Vulkan _and_ D3D12
before it enters the API.

## Predictable error handling

- Every fallible function returns `lc_result`; every value is
  documented on the function.
- `NULL` and dead handles fail with `LC_ERROR_INVALID_ARGUMENT`,
  never crash (verified by headless tests).
- Recoverable states (out-of-date, suboptimal, zero extent) are
  distinct results, not fatal errors and not hidden retries.
- Fatal backend failures use the narrowest fitting code; unknown
  situations use `LC_ERROR_UNKNOWN`, never a fabricated specific.

## Minimal hidden work

The library never spawns threads, never busy-waits, never recreates
resources behind the application's back (no auto-recreate in the
event pump), and never allocates per-frame. Coarse
`vkDeviceWaitIdle` appears only on teardown/recreate paths and is
documented as intentionally simple.

## Zero backend types publicly

`include/lumac/lumac.h` includes only `<stddef.h>`/`<stdint.h>`.
Any `Vk*`, `HWND`, or X11 symbol in the public header is a release-
blocking bug. Verify with grep before every milestone.

## Stable C ABI goal

Opaque pointers, fixed enums, no inline functions, no exposed
struct layouts: the ABI is designed to stay link-compatible once 1.0
lands. Until then, additive changes are preferred but breaking
changes are permitted with a changelog entry.

## Validation discipline

- New descriptors are zero-initializable; docs say so.
- Counts accompany every pointer array; zero means empty.
- Sizes are 64-bit (`uint64_t`) at the API boundary; overflow is
  checked, never assumed away.
- Tests cover: pre-init, NULLs, dead handles, bounds/overflow,
  misuse ordering (begin-twice, draw-unbound), and real GPU
  round-trips where behavior (not just `VkResult`) is asserted.
