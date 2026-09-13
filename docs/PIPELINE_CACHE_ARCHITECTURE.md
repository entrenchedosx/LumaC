# Luma Pipeline-Cache Architecture (Phase 18, PARTs P–X, AT–AV)

Persistent Vulkan pipeline caching, owned entirely by the LumaC
backend. No Vulkan types leak publicly; the renderer only observes
backend-neutral load/save facts.

## Ownership (PARTs P, AW)

`VkPipelineCache` lives in `lc_device` (one per device, shared by
every graphics pipeline via `vkCreateGraphicsPipelines`). File I/O
lives in `src/graphics/vulkan/vulkan_backend.c` (load at create,
save at destroy). The renderer, assets, examples, and tests only
pass a path and read `lc_pipeline_cache_info`. Screenshot/PNG
tooling stays in `tools/` (never LumaC, never the renderer).

## Lifetime (PART Q)

device creation → load blob if compatible → `vkCreatePipelineCache`
(empty on any failure) → pipelines created through it →
`lc_device_destroy` → `vkGetPipelineCacheData` → atomic save.
Creation NEVER fails because of cache trouble.

## Path policy (PART R)

`lc_device_desc.pipeline_cache_path` (backend-neutral name;
D3D12 may reuse it for its own representation). NULL/empty =
in-memory cache only. `disable_pipeline_cache` removes even the
in-memory cache (tests, sandboxes, consoles). The path is copied
at creation; parent directories must exist; unwritable paths fall
back silently to memory-only. No cache files ever land in the
caller's CWD unless the caller points there. Descriptors must be
zero-initialized (`= { 0 }`) — an uninitialized path pointer is
undefined behavior (Vulkan create-info discipline); all in-tree
call sites were hardened.

## Compatibility (PART S)

Vulkan validates vendor/device/driver UUIDs in the blob header at
`vkCreatePipelineCache`. Incompatible/corrupt input → stderr
notice → empty cache → device continues. Oversized files
(> 64 MiB) are refused at load.

## Atomic save (PART T) and size limit (PART U)

Temp sibling → flush/close → `remove` + `rename` over the
destination (Windows rename needs the remove-first step; a crash
between them loses the old cache but never corrupts it — the tmp
remains). Blobs over 64 MiB stay in-memory only (never truncated;
truncation corrupts blobs).

## Introspection (PART AL)

```c
lc_device_get_pipeline_cache_info(device, &info); /* enabled,
  loaded_from_file, saved_to_file, bytes_loaded, bytes_saved */
```

No Vulkan UUIDs exposed. `lr_frame_diagnostics` mirrors
enabled/loaded/bytes for frame tooling. Note: `saved_*` are set
during destroy (freed struct), so post-shutdown verification
stats the file itself.

## Evidence (PARTs V, W, AT)

`tests/test_pipeline_cache` (22 checks): missing file, two-run
load/save/reuse, junk/truncated/0xFF blobs (all create safely,
then self-repair on next shutdown), unwritable path, disabled
mode. Measured on RTX 5060: true-cold pipeline 87.64 ms, warm
0.58 ms (same binary, same driver); lavapipe: 0.63 ms / 0.41 ms
(header-only 32-byte blob). Timings are reported, never asserted.

## TDR position (PARTs X, BB)

Phase 18 introduces NO TDR requirement and does not modify the
registry (no elevation ever). The persistent cache removes the
repeat cold-JIT cost that motivated the Phase 17 operational
workaround: steady-state launches compile nothing. Default-TDR
single-launch verification was NOT performed (the Phase 17
`TdrDelay` value remains in place on this machine); reported
honestly as NOT VERIFIED, not as solved.
