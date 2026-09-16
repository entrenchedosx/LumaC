# LumaC API Design Principles

Phase 20 abstractions remain backend-neutral: encoders/lists map to D3D12 command
allocators/lists, queue classes map to direct/copy/compute queues, completion
values map to D3D12 fences, and staging maps to upload/readback heaps. No Vulkan
handle, family index, stage, access mask, or layout enters the public API.

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

## Render targets (Phase 12)

- A swapchain presents; a render target renders. Presentation is one
  possible destination, never the definition of rendering.
- Targets borrow views (non-owning); views outlive targets, enforced
  by tracking hooks. Destroying a view first invalidates dependents,
  rejected where detectable, never dereferenced.
- Formats are inferred from views, never duplicated in the create
  call. Compatibility is structural (counts, formats, samples) with
  a hash fast-path and structural resolve — never pointer identity —
  so equivalent targets share pipelines.
- Pipelines hold only the signature, never the target object.

## Command recording (Phase 12)

- Recording flows through a borrowed per-frame encoder with an
  explicit state machine: exactly one pass open max, no nesting, no
  submit while open, no draws outside a pass. Misuse returns
  `LC_ERROR_INVALID_ARGUMENT` (or `LC_ERROR_PIPELINE_INCOMPATIBLE`
  for signature drift) without emitting backend commands.
- Legacy swapchain-bound recording remains as mutually-exclusive
  convenience while the codebase migrates; new recording must use
  encoders. Two renderers never coexist: both paths record into the
  same frame command buffer through shared backend helpers.
- Load/store are backend-neutral concepts (`lc_load_op`,
  `lc_store_op`); UNDEFINED is never a final layout and swap images
  always start UNDEFINED (LOAD there is rejected, not silently
  miscompiled).

## Renderer layering (Phase 13)

- Luma Renderer (`lr_*`) depends only on the public LumaC API. No
  `Vk*`, `vulkan/`, `windows.h`, or X11 symbols may appear under
  `renderer/` (audited by source grep); the renderer does not know
  which backend LumaC selected.
- Renderer objects are plain owned handles with NULL-safe destroys;
  cross-renderer use is rejected; per-frame queues are caller-visible
  only through statistics. No global renderer state (multiple
  instances share one device freely).
- LumaC validates recording; the renderer validates scene data. Both
  layers fail safely (explicit result codes, never crashes, never
  blind dereferences) and never allocate per-frame on hot paths.

## Engine layering (Phase 24)

- Luma Engine (`le_*`) depends only on the public Luma Renderer +
  LumaC APIs (audited at configure time by `engine/CMakeLists.txt`).
  Generational handles (`{index, generation, world_tag}`) are the
  only gameplay identity — never pointers, indices, or queue/GPU
  slots. Stale handles fail safely; cross-world use is rejected.
- The engine owns scene storage; the application owns the engine
  and keeps borrowed `lr_mesh`/`lr_material` alive. Descriptors are
  zero-initializable; counts accompany arrays; sizes are
  overflow-checked. The ABI is script-bound in Phase 26 (see
  `SCRIPTING_ARCHITECTURE.md`, `SCRIPT_BINDING_API.md`).

## Engine scripting (Phase 26)

- One `lua_State` per engine, worlds isolated by `world_tag`; no
  Lua spelling crosses the public C API (configure-time
  VM-confinement audit; only `engine/src/script/` touches Lua).
- Script-facing engine calls funnel through the VM-independent
  `le_script_backend_ops` table so a future native backend can
  implement the same operations without the VM.
- Persistence carries script asset IDs + exported values (`script`
  / `sprop` scene lines) — never VM state.

## Engine input/time/lifecycle (Phase 27)

- Input is engine-owned per `le_engine` (worlds share one
  finalized snapshot); the LumaC layer contributes only the
  backend-neutral `lc_window_event` queue (Win32/X11 translate,
  never gameplay). No `VK_*`/`WM_*`/`XKeyEvent` outside
  `src/platform/`.
- Time is engine-owned (monotonic ns source, explicit-delta test
  path through the same state machine); fixed-step is an engine
  schedule mirrored into world accumulators. `le_world_update`
  keeps its legacy contract; the engine path dispatches paused
  dt=0 via `le_world_simulate_engine`.
- Lua `Input.*`/`Time.*` are thin over `le_input_*`/`le_time_*`
  (no VM-side state), so future native scripts use the same
  services with identical semantics.

## Engine assets & scenes (Phase 25)

- Gameplay references assets by generational `le_asset` handles
  (registry-owned backing, never renderer pointers); persistent
  `le_asset_id` / `le_scene_object_id` values are the only things
  stored on disk (Lua must never persist raw runtime indices).
- Scenes are versioned canonical text (memory-first, file helpers
  above); unknown fields tolerated, unknown components/versions
  rejected; malformed input fails transactionally (world/scene
  untouched). New `le_result` codes follow the same switch-safety
  contract.

## Editor foundation (Phase 31)

- Luma Editor (`led_*`, `editor/`) sits above the engine
  (`editor → engine → assets → renderer → LumaC`, audited at
  configure time by `editor/CMakeLists.txt`). C11, no C++ in
  library sources, no Lua spellings (script access via
  `le_script_*` only), no engine/renderer internals.
- New engine enumeration APIs are append-only and stale-safe:
  `le_world_get_roots` / `le_world_get_all_objects` /
  `le_world_get_live_count` / `le_object_info2` (v1). `le_object_info`
  is unchanged.
- Editor errors are `led_result` (1000+ range; engine failures
  surface as `LED_ERROR_ENGINE` with the code preserved). Commands
  are plain structs (MCP-ready); undo/redo is bounded with stats;
  Euler storage stays quaternion with ZYX-degree inspector mirror.

## Phase 18 additions

- Readback (`lc_image_query_readback` / `lc_image_readback`,
  `lc_image_readback_desc/_info`): tight deterministic CPU rows,
  no reinterpretation, TRANSFER_SRC-gated loudly, sync-only with a
  reserved async path (`lc_readback_request/poll/map`).
- Identity (`lc_resource_id` + per-type getters, 0 = none):
  pointer equality is not identity; caches compare IDs.
- View→image borrower (`lc_image_view_get_image`) so capture
  needs no backend access.
- Pipeline cache (`lc_device_desc.pipeline_cache_path`,
  `disable_pipeline_cache`, `lc_pipeline_cache_info`): backend owns
  bytes, API owns policy. Descriptors must be zero-initialized.
- Clock (`lc_clock_now` / `lc_clock_frequency`): monotonic ticks
  for profiling; GPU timestamps deferred.
- Renderer: `lr_frame_profile`, `lr_frame_diagnostics`,
  `lr_post_stage` (+ tint verify), `lr_renderer_capture_hdr`,
  `lr_renderer_capture_output`, `lr_renderer_get_brdf_view`.

## Pre-1.0 API debt list (PART AY audit)

Deliberately NOT fixed in Phase 18 (no breaking rewrite); clean up
before 1.0:

- `lc_image_query_readback` vs `lc_device_get_*` naming: `query_`
  is new; decide one convention (`get_readback_info`?).
- `LC_CACHE_SPV_DIR` duplicates `LC_TRIANGLE_SPV_DIR`/`LC_SPV_DIR`
  for the same shader staging dir (test-only macros).
- Legacy swapchain-era recording (`lc_bind_pipeline`, `lc_draw`,
  `lc_clear_color`, …) still sits beside the encoder path; the
  encoder is preferred but the legacy path is un-deprecated.
- `lc_device_desc` grows by appending (Vulkan create-info
  discipline); consider a version/size field before 1.0 if more
  platform policy accrues.
- `lr_renderer_capture_output` reads attachment 0 only; MRT
  capture wants an index parameter later.
- `lc_pipeline_cache_info.saved_*` are set during destroy (freed
  struct); a future explicit flush would make them observable.
- Test-only `fopen`/`snprintf` usage is fine, but any promotion
  of file helpers into the library must use the MSVC-safe
  wrappers (C4996).
- `lr_frame_profile` doubles `cpu_prepare_ms`/`cpu_shadow_ms`
  today (split reserved for a future prepare/record division).

## Phase 19 additions

- States (`lc_resource_state`, `lc_image_subresource_range`,
  `lc_encoder_transition_image`): destination-only transitions;
  buffer states named but untracked until compute/indirect work.
- Memory (`lc_memory_stats`, `lc_memory_budget`,
  `lc_resource_memory_info`, `lc_memory_class`,
  `lc_device_get_memory_stats/budget`,
  `lc_buffer/image_get_memory_info`): committed/used/free,
  counts, largest free, real-or-unknown budgets, no handles.
- Allocator stays fully private (no block/heap/type APIs).

## Pre-1.0 API review, Phase 19 pass (PART 20)

- States map cleanly both ways: Vulkan (layouts/stages/access
  derived centrally) and D3D12 (per-subresource barriers with the
  same granularity) need nothing else.
- Memory classes (DEVICE_LOCAL/UPLOAD/READBACK) mirror D3D12
  heaps (DEFAULT/UPLOAD/READBACK); stats/budget/info carry no
  Vulkan types.
- Small apps unaffected: buffer/image creation signatures
  unchanged; zero-init discipline already applied tree-wide.
- Resolved in Phase 21: `lc_encoder_transition_buffer` now covers
  buffer states (TRANSFER/STORAGE/INDIRECT/vertex/index/uniform).
- New debt (do not fix now): `lc_memory_stats` has no
  fragmentation percentage (largest-free suffices); OOM has no
  retry-with-smaller-blocks policy (callers size explicitly);
  `drawIndirectCount` is reported but has no public draw call
  (arrives with the async-compute scheduler).

Phase 23: indirect-count draws are public (lc_encoder_draw_indirect_count / indexed form); STORAGE_READ_WRITE covers compute read-modify-write history buffers.
