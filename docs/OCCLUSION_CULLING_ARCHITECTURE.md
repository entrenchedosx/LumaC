# Occlusion-Culling Architecture (Phase 23)

GPU frustum culling extends with previous-frame Hi-Z occlusion in
one dispatch per group: frustum test (current planes) ->
occlusion test (previous-frame matrices against the pyramid) ->
LOD selection -> per-LOD compaction. Temporal policy is
previous-frame depth with conservative bypass: first frame,
camera teleport (view-projection drift heuristic), extent change,
Hi-Z disabled, or a frame whose depth was not stored all keep
every frustum-visible instance. Stale depth can never hide
geometry; at most one frame of latency hides nothing.

The supreme rule is conservative correctness: false visibility is
acceptable, false occlusion is a bug. Uncertain projections
(near-plane intersection, camera inside the bound, samples outside
the viewport, depth outside [0,1]) stay visible. Bounding spheres
use max-axis scale (non-uniform and mirrored scales stay
conservative).

```c
lr_result lr_renderer_set_visibility(lr_renderer *r,
                                     const lr_visibility_settings *s);
lr_result lr_renderer_update_visibility_stats(lr_renderer *r); /* TEST-ONLY stall */
void lr_renderer_get_visibility_stats(const lr_renderer *r,
                                      lr_visibility_stats *out);
```

Projected bounds: the sphere center projects through the
previous-frame matrix; the nearest depth is the sphere point
closest to the previous camera. Screen extent derives from
pixels-per-unit at the center depth. Mip selection is
`clamp((mips-1) - floor(log2(max(extent,1))))`: tiny footprints
sample coarse (MAX over more area hides less), large footprints
sample fine. Five samples (corners + center, viewport-clipped;
fully outside stays visible) must all report behind before a
cull, with a small configurable NDC bias (`occlusion_depth_bias`,
default 0) that only ever favors visibility.

## Canonical proofs

`test_occlusion_vulkan` (60 checks): wall scene culls exactly the
hidden (3 occluded, 5 retained), Hi-Z on/off pixels bit-identical
with LOD off, lit-pixel guard against vacuous passes,
near-plane/camera-inside/screen-edge/teleport/180-degree/moving-
occluder cases, and an open-scene yaw sweep proving on/off
visibility agreement (no false occlusion). No CPU visibility
readback exists in the production path (stats download is
test-only and documented).

## Futures

Reversed-Z bias conventions, temporal reprojection of Hi-Z,
multi-sample patterns by footprint size, debug LOD/occlusion
tint modes (counters ship now; tinting is deferred, not designed
away).
