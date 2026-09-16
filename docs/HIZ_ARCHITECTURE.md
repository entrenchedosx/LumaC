# Hi-Z Architecture (Phase 23)

The renderer keeps a dedicated R32F depth pyramid shadowing the
main-scene depth buffer. Mip 0 is seeded from the previous frame's
scene depth by a compute copy; higher mips are MAX reductions. The
pyramid persists across frames (one frame of occlusion latency, by
design) and rebuilds only on extent change through normal
retirement (no global idle).

Depth convention (derived, not assumed): standard Z, near maps to
0, far maps to 1, depth compare LESS, clear 1.0 (`camera.c`,
`vulkan_pipeline.c`). MAX reduction is therefore the conservative
choice: a stored texel is the farthest depth of its region, so an
object nearer than the stored value may be visible, and an object
behind it is behind everything there.

```c
uint32_t lr_hiz_mip_count(uint32_t width, uint32_t height);
lr_result lr_hiz_ensure(lr_renderer *renderer, uint32_t w, uint32_t h);
lr_result lr_hiz_generate(lr_renderer *renderer, lc_command_encoder *enc);
lc_image_view *lr_renderer_get_hiz_view(lr_renderer *r, uint32_t mip);
uint32_t lr_renderer_get_hiz_mip_count(const lr_renderer *r);
```

The image is never a render target (no attachment aliasing with
scene depth): STORAGE for production, SAMPLED for reads,
TRANSFER_SRC for test/debug readback. Mip count is
floor(log2(max(w,h)))+1, complete down to 1x1; odd extents are
covered, never dropped (out-of-range source texels are skipped,
the last row/column is included). Per-mip views serve storage
writes; one full-chain view serves occlusion reads (fetching
lod > 0 through a single-level view is undefined).

## Semantic states

Production runs whole-image SHADER_READ_WRITE, then drops each
mip to SHADER_READ as it finishes (per-mip subresource
transitions, all outside passes). Bypass frames adopt SHADER_READ
without sampling distrusted depth. The main HDR pass stores depth
(STORE + SAMPLED final) only while Hi-Z needs the previous frame;
legacy frames keep DISCARD. The pass key joins SAMPLED usage with
the STORE op so layout tracking stays truthful in both modes.

## Canonical proofs

`test_hiz_vulkan`: 8x8 diagnostic (near/far/gradients/odd edge),
7x5, 1x8, 8x1, 1x1, plus 800x600 -> 321x179 -> 1920x1080 ->
800x600 resize fixtures; every mip bit-exact against the CPU
MAX reference (D32->R32F is exact; no tolerance needed).
`--debug-hiz` captures mip 2 (`docs/images/hiz-debug.png` comes
from this path). No Vulkan names cross the renderer API. D3D12
maps the pyramid onto an R32_FLOAT mip chain with UAV writes.
GPU time is NOT MEASURED, never estimated.

## Futures

Reversed-Z (flip the reduction), async-compute pyramid overlap,
transient-aliased pyramid storage once the graph proves
lifetimes, sampler-based (filtered) min/max variants behind the
same state model.
