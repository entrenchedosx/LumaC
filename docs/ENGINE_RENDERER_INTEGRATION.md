# Engine → Renderer Integration (Phase 24)

The engine orchestrates renderer systems; it duplicates none of
them. Meshes, materials, cameras, lights, PBR, shadows, IBL, HDR,
tonemapping, frustum/Hi-Z culling, GPU LOD, compaction, indirect
generation, and the render graph all stay renderer-owned.

## Extraction (the boundary)

`le_world_extract_renderables` builds a flat snapshot — plain data,
no renderer pointers in object identity:

```c
typedef struct le_extracted_renderable {
    le_object object;        // source handle (debugging)
    float world_matrix[16];  // refreshed, column-major
    uint64_t stable_id;      // renderer temporal key
    lr_mesh *mesh;           // borrowed renderer handles
    lr_material *material;
    int casts_shadow, receives_shadow, mirrored;
} le_extracted_renderable;
```

Counts (`le_world_get_extraction_counts`) respect
effective-enabled, visibility flags, and component presence.
Mesh/material liveness is NOT pre-filtered: the renderer owns those
registries and is the final authority at submit (rejections count
as `skipped_dead`).

## Submission

`le_world_render_scene(world, encoder, w, h)`:

1. `le_world_update(0)` (refresh dirty matrices),
2. derive the active camera (below) → `lr_renderer_begin`,
3. submit lights (travel direction = −Z axis for
   directional/spot; world position for point/spot),
4. submit renderables in ascending-slot order — engine world matrix
   → decomposed `lr_transform` (exact for T×R×S chains incl.
   mirrors/non-uniform scales; closest rigid fit for residual shear)
   + mesh/material + flags + `stable_id`. TWO SPELLINGS: direct
   (Phase 24 borrowed `lr_mesh`/`lr_material`, app-kept-alive) and
   asset-backed (Phase 25 `le_asset` handles resolved through the
   registry per frame; unready/unresolvable → `skipped_dead`, frame
   stays valid). At most one spelling per object.
5. `lr_renderer_render_shadows` + `lr_renderer_render_scene` into
   the HDR target. Scene pass only — no output/present.

`le_world_render_output` tonemaps into the caller's target (caller
begins/ends the pass; any LDR target works). `le_world_render_end`
closes the renderer frame. The legacy one-call `le_world_render`
wraps the trio for offscreen targets (swapchain targets rejected —
presentation uses the explicit trio).

The frame contract: the encoder comes from an open frame with NO
open pass; the call leaves no pass open and the renderer frame OPEN
until `le_world_render_end`.

## Cameras

The object's world transform IS the camera transform (single
authority). Derivation: position = translation column; basis
columns normalized (non-uniform-scale tolerant, mirror-preserving);
view = rotation-transpose + eye translation; projection from the
lens (perspective/orthographic, renderer-validated ranges). No live
/ effectively-enabled / non-singular camera ⇒ documented default
60° origin camera. Disabled-camera objects submit nothing.

## Lights

Engine mirrors the renderer-supported kinds (directional, point,
spot) with renderer validation ranges; shadow config rides through
verbatim (point shadows rejected, as in the renderer). Disabled (or
effectively-disabled) light objects submit nothing.

## Enabled semantics

```
effective_enabled = self_enabled && parent_effective_enabled
```

A disabled parent hides its whole subtree from renderer AND light
submission. Render-time accounting distinguishes
`submitted / skipped_disabled / skipped_invisible / skipped_dead`,
mirroring the renderer frame stats in `le_render_report`.

## Accounting invariant

For renderer-submitted instances (GPU path):

```
submitted = frustum_rejected + occlusion_rejected + visible
```

proven at 3 instances and at 5000 engine renderables in
`test_engine_vulkan` (PART 8/8B). Engine-side: created, enabled,
renderable, submitted, frustum/occlusion rejected, visible are
reported separately — never conflated (the Phase 23 100k lesson:
scene-generation filtering is documented separately, not counted
as renderer loss).

## Mode agnosticism

Engine submissions work identically under GPU-driven (frustum →
Hi-Z → LOD → compaction → indirect finalize → draw) and CPU-
fallback paths — the engine never selects, detects, or branches on
the mode. Proven both ways every run in `test_engine_vulkan`.
