# Indirect Rendering Architecture

Indirect draws take backend-neutral commands with explicit-width
fields (`lc_indirect_draw_command`,
`lc_indirect_draw_indexed_command`); the backend statically
asserts structural equivalence with `VkDrawIndirectCommand` /
`VkDrawIndexedIndirectCommand`. D3D12 maps the same fields onto
its indirect-argument structures.

`lc_encoder_draw_indirect` / `lc_encoder_draw_indexed_indirect`
record inside open passes (frame encoders and graphics worker
lists). `draw_count` executes natively when the enabled
`multiDrawIndirect` feature allows, else as a compatibility loop
of single indirect draws (honest fallback, counted the same way).
`stride` must cover the command size; ranges are validated
against the buffer.

Indirect buffers carry `LC_BUFFER_USAGE_INDIRECT` and the
`INDIRECT_READ` state. The draw call strictly requires that
state — barriers are illegal inside render passes (no
subpass self-dependency), so producers transition before the
pass opens (renderer prepare does this) and a missing
transition fails loudly instead of recording an illegal
barrier. The canonical chain is compute `STORAGE_WRITE` into
the command buffer, one barrier to `INDIRECT_READ`, then draw.

`drawIndirectCount` (GPU-written count selects the draw count)
is deliberately deferred: the device capability is reported
honestly via `lc_compute_capabilities.indirect_count`, and the
fixed-count path (compute writes `instanceCount`, one indirect
call) covers all Phase 21 work. A public count API arrives with
the async-compute scheduler, not before.
