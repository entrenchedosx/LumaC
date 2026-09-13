# Luma Synchronization Architecture (Phase 19, PARTs A–J, AM–AO)

Backend-neutral resource states with subresource tracking,
automatic transitions where safe, and one explicit transition
API. LumaC converts states to backend synchronization (Vulkan
layouts, stages, access); those never cross the public header.

## Semantic states (PARTs A–B)

```c
lc_resource_state // UNDEFINED, COLOR_ATTACHMENT_WRITE,
  // DEPTH_ATTACHMENT_WRITE, SHADER_READ, SHADER_READ_WRITE,
  // TRANSFER_SRC, TRANSFER_DST, PRESENT, VERTEX/INDEX/UNIFORM/
  // STORAGE_READ, STORAGE_WRITE, INDIRECT_READ
```

Image states are tracked and enforced today. Buffer states are
valid enum values with a total backend mapping, reserved for
compute/indirect work — the abstraction never needs replacing
for them. No state names any queue (PART AN): graphics, compute,
and transfer scheduling fit underneath later.

## Subresource tracking (PARTs C–D)

Every image tracks one state per mip per layer
(`states[layer * mips + mip]`). Ranges (`base_mip`, `level_count`,
`base_layer`, `layer_count`) address subsets; whole-image callers
pass the full range explicitly. mip 0 can be SHADER_READ while
mip 1 is COLOR_ATTACHMENT_WRITE — pinned by the mixed-state test
(one mip binds sampled while its sibling sits in TRANSFER_DST).

## Automatic vs explicit (PART E–F)

High-level operations infer transitions: render-pass begin/end
adopt finals (STORE → SHADER_READ, DONT_CARE → UNDEFINED),
uploads/readbacks/mipgens transition their ranges, binding writes
validate sampled-readability. Advanced use names only the
destination — never old state, stages, or access:

```c
lc_encoder_transition_image(enc, image, &range, NEW_STATE);
```

Recorded into the frame command buffer (usable between passes);
tracking updates immediately (recorded-not-executed discipline).

## Visibility (PARTs G–I, the Phase 18 debt)

Closed explicitly: every render pass carries an EXTERNAL→0 and a
0→EXTERNAL dependency whose dst is FRAGMENT_SHADER +
SHADER_READ, so attachment writes (color STORE and sampled-usage
depth STORE alike) are *visible* to later shader reads on strict
hardware — not merely layout-correct. Legacy swapchain passes use
the identical dependency so shared framebuffers stay compatible.

Canonical proofs (all pixel-exact, all validation-clean):
- G: 500-iteration A↔B ping-pong (clear A, copy A→B, alternate),
  every 25th copy byte-exact, final exact.
- H: depth-only clear → sampled bind accepted → raw depth
  readback equals the clear value.
- I: upload→sample, render→readback, staging-copy→vertex-fetch.
- J: ~1000 explicit + ~1000 pass-boundary transitions in the G
  loop; deterministic final pixels.

## Futures (PARTs AM–AO)

- Parallel recording: tracking is per-image today; per-queue or
  per-recording epochs slot in without changing states or calls.
- Multi-queue: barriers use IGNORED families now; ownership
  transfers land in the sync layer only.
- Timelines: a future submit/wait API references the same states;
  no Vulkan fence/semaphore types leak today.
- Compute: SHADER_READ_WRITE maps to GENERAL; storage states are
  already named.
