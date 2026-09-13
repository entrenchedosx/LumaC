# Luma Post-Process Architecture (Phase 18, PARTs AB–AI)

A small internal chain abstraction between the HDR scene and the
tonemap/output pass. NOT a render graph (explicitly deferred):
one optional stage today, structured so bloom/SSAO/TAA slot in
without rewriting frame flow.

## Model (PARTs AB–AC)

```
HDR scene -> [post stage 0] -> ... -> tonemap/output
```

`render_scene` records sky + items into HDR, then each enabled
stage reading the chain head and writing a ping-pong intermediate;
`render_output` tonemaps from the chain head (post output, or the
HDR view when the chain is empty). Internal representation per
stage: input view, output target, pipeline, rewritten bindings,
push parameters (`renderer/src/post.c`, public lumac.h only).
No user-shader post-processing is exposed yet.

## Intermediates (PARTs AD, AG–AH)

Renderer-owned RGBA16F ping-pong pairs, per output-extent slot
(two slots cover multiview with differing extents; environments
stay shared). Allocated lazily, reused across frames, recreated
only on extent change — never per frame, never on material/mesh/
shadow/BRDF/cache changes. TRANSFER_SRC included (publicly
readable for debugging).

## Verification effect (PARTs AE–AF)

Exactly one stage: `LR_POST_TINT_VERIFY` (color multiply, default
identity = byte-exact no-op when enabled). Default OFF. Bloom and
everything else stay deferred to Phase 19+.

## Frame flow with post

`render_scene` runs `lr_post_record` (slot 0 today) after items;
`record_tonemap` samples the chain head. Two outputs may share
one recording (swapchain + screenshot): the tonemap set uses an
identity-guarded write-skip (same view+sampler IDs rebind
without rewriting — rewriting between binds invalidates the
command buffer).

## Profiling (PARTs Y–AA, AK)

CPU-side recording timings (monotonic clock via backend-neutral
`lc_clock_now`/`lc_clock_frequency`; GPU timestamps deferred —
no query pools in the renderer):

```c
lr_renderer_get_frame_profile(&profile); /* prepare/shadow/main/
  sky/post/tonemap/total ms, post_passes, frame_number */
lr_renderer_get_frame_diagnostics(&diag); /* one-call snapshot:
  frame, viewport, draws, tris, shadows, IBL, post, CPU ms,
  cache status. No native handles. */
```

`lr_render_stats` keeps its integer counters untouched.

## Multiview + screenshots (PART AI)

Two orientations through public readback must differ (pinned in
P18-AS/AI). Future `capture_game_view`/`capture_editor_view`/
`capture_depth`/`capture_shadow_map`/`capture_material_preview`
MCP verbs build on the capture APIs + diagnostics, never on
backend-private access (PART AJ).

## Debt and next steps

GPU timestamps (`lc_query_pool` shape sketched, not built);
per-view post slots beyond two; async readback; bloom (deferred
past Phase 19) as the first real multi-pass consumer (blur needs
≥2 stages + a second ping-pong turn — the abstraction already
supports it).

## Phase 19 notes

Intermediates allocate through the block allocator (suballocated,
TRANSFER_SRC-capable, publicly readable); extent-change rebuilds
return regions to the pools. No API change.
