# Compute Architecture

Compute shaders are created through the same `lc_shader_create` path
with `LC_SHADER_STAGE_COMPUTE`. Compute pipelines bind binding
layouts and push ranges exactly like graphics pipelines (no
render-target signature) and participate in the same binary
pipeline cache under the same cache shard — no second cache
system. Push ranges admit `COMPUTE` visibility alongside the
graphics bits.

Dispatch records on frame encoders (outside any open pass;
dispatch inside a render-pass instance is rejected) and on
compute worker lists (secondaries without render-pass
inheritance, executed outside passes). Graphics worker lists
cannot dispatch. On hardware without a separate compute queue,
dispatch executes on the graphics queue (baseline compatibility
path, always available).

Queue discovery prefers a compute-only family, then
compute-without-graphics, then any compute-capable family, else
aliases graphics. `lc_device_get_queue_info(LC_QUEUE_COMPUTE)` and
`lc_device_get_compute_capabilities` report the honest topology
(supported vs enabled multi-draw/indirect-count, workgroup
limits, subgroup size or 0). Production dispatch uses
frame/worker buffers; a minimal isolated cross-queue submit
(`lc_vk_compute_dispatch_once`, test path only) proves dedicated
queues execute with correct visibility. Overlapping async compute
is explicitly deferred: no scheduler, no automatic overlap, no
`async_supported` promise on the compute queue.

Storage buffers use the existing binding types (`STORAGE_BUFFER`
read-only/read-write); storage images use `STORAGE_IMAGE` with
`SHADER_READ_WRITE` state and `GENERAL` layout (sampled images
keep `SHADER_READ`/`READ_ONLY`). Buffer state tracking
(`TRANSFER_SRC/DST`, `VERTEX/INDEX/UNIFORM/STORAGE_READ`,
`STORAGE_WRITE`, `INDIRECT_READ`) drives execution/access
barriers; descriptor-set updates accept untracked (`UNDEFINED`)
and transfer-ordered (`TRANSFER_DST`) buffers leniently and
reject wrong-slot reuse loudly.

D3D12 mapping: compute PSO, `Dispatch`, UAV barriers, one
fence-value domain. No Vulkan names cross the public API.

Phase 23: compute drives Hi-Z copy/reduce, frustum+occlusion+LOD culling, and indirect finalize dispatches; indirect-count capability consumed natively where offered.
