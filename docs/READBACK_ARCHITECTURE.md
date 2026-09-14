# Luma Readback Architecture (Phase 18, PARTs A–L, AM–AQ, AR)

Phase 20 adds async requests with poll, wait, map/copy, and destroy. The
synchronous API now schedules, waits, and maps through that same implementation.

Public CPU-visible capture of GPU images through LumaC only. No
Vulkan staging buffers, command buffers, fences, or query pools
are exposed; no backend-private access is needed by the renderer,
examples, tests, or future MCP capture tooling.

## API

```c
lc_image_query_readback(image, &desc, &info); /* sizing, no GPU work */
lc_image_readback(image, &desc, dst, dst_size, &out_required_size);
```

`lc_image_readback_desc` selects one mip level of one array layer
(cube faces are array layers 0–5 of a cube-compatible image).
`lc_image_readback_info` reports the deterministic CPU layout:
mip extent, format (never converted), `row_pitch`, `size`.

Renderer convenience (same dst rules, public route only):

```c
lr_renderer_capture_hdr(renderer, dst, dst_size, &need);    /* RGBA16F */
lr_renderer_capture_output(renderer, target, dst, ...);     /* LDR */
lr_renderer_get_brdf_view(renderer); /* + lc_image_view_get_image */
```

## Layout semantics (PART B)

CPU output is ALWAYS tightly packed deterministic rows
(`row_pitch == width * element_bytes`), even when backend staging
uses a larger pitch internally. Element bytes follow
`lc_format_byte_size`; sRGB stays encoded, float stays float, BGRA
stays BGRA, depth stays raw. The 4-quadrant pattern test pins
orientation (row 0 = top), channels (RGBA order), and pitch.

## Eligibility (PART C)

The image MUST carry `LC_IMAGE_USAGE_TRANSFER_SRC`; otherwise
`LC_ERROR_INVALID_ARGUMENT` is returned loudly — never silent
zeros (the Phase 14 bug class). Renderer-owned capture-capable
targets (HDR, post intermediates, environment maps, shadow maps,
HDR depth) include `TRANSFER_SRC` automatically. Multisampled
images are rejected.

Supported formats: all color formats plus `D16_UNORM`,
`D32_FLOAT`, `D24_UNORM_S8_UINT` (raw 4 bytes/texel). Depth
readback (PART F) is SUPPORTED over the transfer path (zeros
round-trip raw; rendering depth is older machinery).

## Synchronization (PART D) and the mid-frame rule

Synchronous convenience: drain prior device work → transfer to
staging → wait → copy to CPU. Documented stall: use for
screenshots, tests, editor thumbnails, debugging — never every
frame in gameplay. Normal rendering performs NO readback unless
explicitly requested (PART AR, audited: the only renderer call
sites are the two capture helpers).

Hard rule learned the expensive way: NEVER read an image written
by the still-open recording before `lc_end_frame`. Layout tracking
describes recorded (not yet executed) passes; an immediate-submit
copy issued mid-recording names layouts the GPU has not reached
(validation error + garbage). The test harness and `ibl_scene`
read back only after `end_frame`.

## Future async path (PART E, reserved)

`lc_readback_request` / `lc_readback_poll` / `lc_readback_map`
will build on `lc_image_readback_desc/_info` without breaking the
synchronous API. Not implemented.

## Identity (PARTs M–O)

Every resource carries a process-monotonic `lc_resource_id`
(assigned at creation, never 0, never reused — `next_resource_id`
lives in process state). Getters return 0 for NULL/dead handles.
Renderer binding caches compare IDs, never pointers (shadow-set
rebind, environment source-change detection, tonemap-set
write-skip). The ABA stress test cycles 500 image+view
create/destroy pairs and asserts all 1000 IDs unique.

## Screenshot flow (PARTs H–J, AM)

`ibl_scene --screenshot out.png`: extra output pass into an
offscreen RGBA8 target in the same recording (two outputs share
one tonemap set via the identity-guarded write-skip), `end_frame`,
public readback, RGBA→RGB, `tools/png_mini.h` (Luma-authored,
stored-deflate, public domain). BGRA/sRGB/vertical handling:
readback preserves target encoding; rows are top-first; HDR vs
LDR is the caller's format choice. `docs/images/ibl-scene.png`
was captured this way (800×600, copper center verified), then
recompressed losslessly for repo size (pixels identical).

## Tests

`tests/test_readback_vulkan` (56 checks): pattern, float32
bit-exact incl. 8.0, float16 tolerance, mip/layer faces, depth,
identity basics, 500-cycle ABA, full error matrix.
`renderer/tests/test_ibl_vulkan` PARTs P18-POST/CAP/IBL/AS/AI:
tint chaining, HDR>1.0 publicly, derived maps publicly,
500-frame endurance, multiview divergence.
`tests/test_pipeline_cache` covers cache file handling (see
PIPELINE_CACHE_ARCHITECTURE.md).

## Phase 19 notes

Staging now suballocates from upload/readback pools (no per-op
dedicated memory); non-coherent ranges flush/invalidate;
depth/non-sampled settles stay range-exact so mixed mip states
survive readback. No API change.
