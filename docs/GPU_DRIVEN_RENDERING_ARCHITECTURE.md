# GPU-Driven Rendering Architecture

CPU mode stays the default: one draw per item with push-constant
transforms. GPU mode (`LR_RENDER_MODE_GPU_DRIVEN`, PBR items)
groups submissions by (mesh, material, shadow-flag), uploads one
shared instance buffer per group with async uploads, runs
frustum-culling + indirect-finalize compute dispatches outside
any pass, then renders one indexed indirect draw per group
through the PBR-compatible instanced pipeline. Shadows keep the
CPU depth path; unlit items keep per-draw CPU submission.

Per group: one instance buffer (96-byte `lr_gpu_instance`:
model, local bounds, mesh/object ids), per-flight visible-index,
counter `{count, capacity}`, and indirect-command buffers, plus
three descriptor sets per flight (cull, finalize, instance).
Flights rotate with the frame slot (2 and 3 deep verified); no
per-object buffers, sets, or command buffers, ever. Descriptors
are created once per group/flight, never per frame.

Culling is frustum-only (six inward planes by push constant,
bounding spheres world-transformed in-shader with max-axis
scaling: safe for non-uniform and negative scales). Visible
indices compact via atomic append with capacity guards (overflow
clamps the draw and reports loudly through the test-only
counter download; out-of-range writes are impossible).
Counters reset with small async uploads (no CPU stall, no
readback in the frame path). Zero visible emits
`instanceCount 0`: a valid no-op, never stale data.

The CPU frustum oracle (`lr_frustum_*` + independent sphere
math) must agree with every GPU visible set exactly; CPU and
GPU submission of the same scene must produce identical pixels.
`lr_gpu_driven_stats` reports submitted/visible (visible via an
explicit test-only download stall), dispatches, indirect
draws/commands, batches, descriptor counts, overflows, and
CPU-side prepare time. GPU timestamps are deferred, stated as
NOT MEASURED, never estimated.

CPU fallback is always available (mode setter rejects
unsupported hardware without changing state). Future work this
enables without redesign: Hi-Z occlusion (extra cull input),
GPU LOD (per-instance level in the instance record), GPU
material sorting (richer batch keys), meshlets (denser
compaction output), and overlapping async compute (queue model
already exposes the class; scheduler deferred).

Phase 23: extended visibility (renderer/src/visibility.c) runs frustum -> Hi-Z occlusion -> GPU LOD -> per-LOD compaction -> indirect finalize per group; draws consume GPU-written counts; proofs in test_hiz/occlusion/lod_vulkan and examples/visibility_scene.
