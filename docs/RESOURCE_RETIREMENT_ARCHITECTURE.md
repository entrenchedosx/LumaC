# Resource Retirement Architecture

Public destruction is logical: the handle leaves the live registry immediately,
and finished command lists referencing it are poisoned so execution fails
loudly. Native objects and allocator bindings enter a device retirement queue
behind the newest submitted completion value. Polling, submission, and signal
waits reclaim due entries.

Retirement covers buffers, images/views, samplers, pipelines/layouts, descriptor
sets/layouts, and offscreen framebuffers. Image/buffer bindings travel with the
native object, preventing block reuse before native destruction. Entries with no
GPU dependency reclaim immediately, preserving allocator churn and coalescing.

Device shutdown is the intentional global-idle exception: wait once, free live
readback requests, drain transfers and retirements, then destroy pools, allocator
blocks, descriptors, caches, and the device in dependency order. Swapchain
recreation retains documented device-idle debt. Destroying an object concurrently
with active recording of that same object is outside the contract; destruction
after list completion is detected through the list registry.


Phase 23: Hi-Z images, visibility buffers, and transient graph resources retire through the existing deferred path (no global idle); shutdown retirement ordering fix retained.
