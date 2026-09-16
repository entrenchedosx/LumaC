# Luma Synchronization Architecture (Phase 19, PARTs A–J, AM–AO)

Phase 20 keeps semantic states and adds recording-local intents for worker
lists. Global state advances only in list execution order. Device completion
values order transfer, frames, staging reclamation, and native retirement.

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

- Parallel recording uses per-list state-intent logs and reconciles them in
  explicit execution order; see `COMMAND_RECORDING_ARCHITECTURE.md`.
- Graphics and transfer queues now share a backend-neutral timeline. GPU-only
  buffers use concurrent family sharing where a dedicated transfer family is
  active; image transfers remain on graphics until split copy/finalize support.
- Public `lc_gpu_signal` values expose poll/wait completion without leaking
  Vulkan fence or semaphore types.
- Compute: SHADER_READ_WRITE maps to GENERAL; storage states are
  already named.
- Phase 21 buffer states: whole-resource tracked states
  (TRANSFER_SRC/DST, VERTEX/INDEX/UNIFORM/STORAGE_READ,
  STORAGE_WRITE, INDIRECT_READ) with stage/access mapping and
  explicit transitions; barriers are illegal inside passes
  (no subpass self-dependency), so buffer transitions record
  outside passes and indirect draws strictly require
  INDIRECT_READ. Descriptor updates accept untracked and
  transfer-ordered buffers, rejecting wrong-slot reuse.

Phase 23: STORAGE_READ_WRITE added for compute read-modify-write buffers (LOD history); Hi-Z pyramid uses per-mip SHADER_READ_WRITE -> SHADER_READ subresource transitions; indirect/count buffers transition to INDIRECT_READ before count draws.
