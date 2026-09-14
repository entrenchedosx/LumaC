# Luma GPU Memory Architecture (Phase 19, PARTs K–AD, AL, 13–18)

Phase 20 retains the block allocator and adds a live-byte cap for async staging,
completion-keyed reclamation, oldest-upload pressure waits, and a one-request
oversized policy. Retired bindings return only after native destruction is safe.

One-allocation-per-resource was the bring-up model. The Phase 19
allocator replaces it with block suballocation behind unchanged
`lc_buffer` / `lc_image` handles. Users never see heaps, memory
types, blocks, or offsets.

## Block allocator (PARTs K–Q)

Large `VkDeviceMemory` blocks per class, best-fit free-list
suballocation with splitting and neighbor coalescing, dedicated
allocations by policy. `vkAllocateMemory`/`vkFreeMemory` happen
only on block grow/reclaim and dedicated paths — 10,000 buffers
back onto 1 block, 600 images onto 2.

Memory type selection is centralized (required bits + ordered
preferences over `memoryTypeBits`); no fixed heap or type-index
assumptions (PART M).

## Classes (PARTs L, T)

- DEVICE_IMAGES: device-local optimal images.
- DEVICE_BUFFERS: device-local GPU-only buffers.
- UPLOAD: host-visible CPU→GPU (buffers + staging).
- READBACK: host-visible GPU→CPU (buffers + staging).

Buffers and images never share blocks (buffer-image granularity
hazards avoided structurally; correctness over clever packing).
Host classes stay persistently mapped; suballocations address
`mapped_base + offset`.

## Policy (PARTs N–O, R)

- Lazy growth; 32 MiB device / 8 MiB host defaults that always
  stretch to fit the first request (no 256 MiB block for a 1 KiB
  app).
- Dedicated when the aligned request exceeds half the class
  default (64 MiB image → dedicated, reported).
- Empty blocks: one warm reserve per class, excess released.
- Debug builds abort loudly on double-free, overlap, and leaks
  at teardown (PART 13); OOM returns `LC_ERROR_OUT_OF_MEMORY`
  with stats unmoved and nothing leaked (PARTs 14–15, including
  post-alloc-failure rollback in creation paths).

## Alignment (PART S) and mapping (PARTs U–Y)

`VkMemoryRequirements.alignment` honored per suballocation
(plus Debug overlap scans). Coherent memory preferred, never
required: non-coherent ranges flush on host write and invalidate
before CPU access, aligned outward to `nonCoherentAtomSize`.
`lc_buffer_map/unmap/write` semantics preserved; internally they
operate on allocator subregions (block addresses never escape).

## Staging (PART Z)

Uploads, GPU-only writes, and readbacks stage through pool
suballocations (`lc_vk_stage_acquire/release`), not per-op
dedicated `VkDeviceMemory`. Callers guarantee GPU completion
before release (all staging uses are immediate-submit + waited).

## Diagnostics (PARTs AA–AD, 19)

```c
lc_device_get_memory_stats(...);   /* committed/used/free per
  device-local/host-visible, live/block/dedicated counts,
  largest free range */
lc_device_get_memory_budget(...);  /* VK_EXT_memory_budget when
  present (RTX: 2 real heaps); never faked */
lc_buffer_get_memory_info(...);    /* requested/aligned/
  dedicated/class */
lc_image_get_memory_info(...);     /* mip-0 footprint + same */
```

Measured (RTX 5060): 10k×256 B buffers create in ~190 ms /
destroy in ~140 ms; 4312-resource workload backs onto 6 blocks
(~151 MB committed, ~90 MB used, 24 MB largest free).

## Lifetimes and aliasing (PARTs Q, 7)

Destroying a buffer/image returns its region; backing blocks live
until empty-and-excess or allocator/device teardown. Normal
suballocation NEVER overlaps (PART 7): intentional transient
aliasing for a future render graph is a separate, explicit
mechanism — not an accident of packing.

## Threading (PART AL)

One allocator mutex guards block selection, suballocation, reclamation, and
statistics. Resource IDs are atomic and buffer-registry mutations are guarded,
so independent threads may create/map/destroy buffers on one device. Phase 20's
four-thread stress performs 800 such cycles and is also run under TSan. There
is no giant recording lock.

## Streaming readiness (PARTs 22, AM/AO)

What exists for streaming/transient futures: suballocation with
stable IDs across offset reuse, lazily grown classes, range-exact
transitions, transfer-capable pools, mapped persistent upload
memory, budget queries. Deliberately NOT built: streaming
policies, defragmentation moves, render-graph aliasing,
defragmentation moves, render-graph aliasing, bindless, D3D12.
Compute queues are discovered (Phase 21) but not scheduled.
Asynchronous transfer-queue uploads are described in
`ASYNC_TRANSFER_ARCHITECTURE.md`.
