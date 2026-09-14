# Asynchronous Transfer Architecture

Uploads copy caller bytes into LumaC-owned, persistently mapped staging before
returning, so source memory may be reused immediately. Every operation retains
its staging buffer, allocator binding, command buffer, synchronization objects,
and completion value until safe reclamation.

The staging budget defaults to 256 MiB and is configurable through
`lc_device_desc.upload_staging_cap`. Allocation first reclaims completed work;
under pressure it waits for the oldest upload instead of growing without bound.
One request larger than the cap is admitted after older reclaimable work drains.
Readback staging remains pinned until request destruction, and capacity pressure
fails explicitly rather than waiting forever on application-owned pins.

Async buffer uploads use a dedicated transfer queue when active. Image uploads
support mip, layer, and 3D extent selection and currently submit on graphics for
valid shader-layout transitions on transfer-only hardware. Both return
`lc_gpu_signal`. Async readback requests support poll, wait, map/copy, and
destroy. (Correction: synchronous `lc_image_readback` keeps its own immediate
implementation; it is not a wrapper over the async path.)
Phase 21 adds the transfer→compute→draw ordering the frame submit
already provides (in-flight transfer wait), plus a sync
`lc_buffer_read` download for tests and debugging (documented
stall, never in steady frames).

