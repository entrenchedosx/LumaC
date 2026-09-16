# Queue Architecture

The public model exposes backend-neutral `GRAPHICS`, `TRANSFER`, and reserved
`COMPUTE` classes. Queue-family indices, Vulkan semaphores, command buffers, and
stage masks remain private. `lc_queue_info` reports availability and whether
transfer is dedicated.

Vulkan device creation prefers a transfer-only family, then a family without
graphics, then another transfer family, and finally graphics fallback. GPU-only
buffers use concurrent graphics/transfer sharing to avoid ownership churn.
Image uploads/readbacks currently remain asynchronous on graphics because their
final layouts target graphics shader stages; splitting copy and graphics-finalize
submissions is explicit follow-up work.

One device timeline semaphore provides monotonically increasing completion
values where supported, with binary-fence fallback. Queue submits are protected
by a narrow submission lock; command recording is not. Polling queries the
counter and waits use Vulkan's blocking wait, never busy-spinning. The model maps
to D3D12 direct/copy queues and one fence-value domain without changing public
types. Phase 21 discovers the compute topology (compute-only >
compute-without-graphics > any compute > graphics alias) and
reports it honestly; production dispatch records into
frame/worker buffers while a minimal isolated submit proves
cross-queue execution. Overlapping async compute stays deferred:
no scheduler, no overlap promise.


Phase 23: visibility passes declare graphics/compute classes and execute dependency-ordered (serialized); no async overlap yet, architecture permits it.
