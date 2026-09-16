# GPU LOD Architecture (Phase 23)

Meshes carry up to four levels sharing one vertex buffer; each
simplified level holds its own index list plus a strictly
descending switch threshold (projected bounding-sphere diameter,
pixels). Level 0 is always the base mesh. LumaC owns only the
mechanisms (buffers, compute, indirect); LOD semantics live in
the renderer.

```c
#define LR_MESH_MAX_LODS 4u
lr_result lr_mesh_add_lod(lr_mesh *mesh, const uint32_t *indices,
                          uint32_t index_count, float min_pixels);
uint32_t lr_mesh_get_lod_count(const lr_mesh *mesh);
```

The metric is projected diameter, so FOV, resolution, object
scale, and distance all respond correctly; raw camera distance is
never the selector. Culling order is frustum -> occlusion -> LOD
(expensive selection never runs for rejected instances).
Hysteresis uses per-slot GPU history (0xFFFFFFFF = unknown):
coarser applies immediately, finer applies past
`switch * (1 + margin)` (default 0.15). New instances select
deterministically from the metric. History is a stability hint,
never correctness: capacity regrowth restarts it UNKNOWN.

Visible instances compact into per-LOD segments; finalize emits
one indexed indirect command per level with `firstInstance`
pointing at its segment, so the vertex shader stays LOD-unaware.
Production frames never read LOD data back (per-LOD counts ride
GPU buffers into indirect-count draws).

## Canonical proofs

`test_lod_vulkan` (99+ checks): misuse matrix, ten settled
threshold sizes incl. exact boundaries (200/80/25), hysteresis
sequences against an independent CPU oracle, non-uniform and
negative scales, 128->1 triangle reduction with distinct-pixel
proof, native/fallback indirect-count equivalence,
graph/manual equivalence with diagnostics. 100k-instance city
(settled frame 4, Hi-Z on): 471350 triangles without LOD vs
385958 with LOD at equal visibility (27245 visible) — measured
via `visibility_scene --instances 100000` with/without `--no-lod`.
No per-object buffers or descriptor sets exist at any LOD count.

## Futures

Continuous (unpopped) LOD blending, screen-space-error metrics,
automatic simplifier import, LOD debug tinting (explicitly
deferred), per-LOD material variants without state duplication.

## Phase 24 addendum: stable temporal identity

Hysteresis history is keyed by **stable instance identity, never
submission position** (closes the pre-Phase-24 P2 debt where LOD
history could attach to the wrong object after reordering).
`lr_draw_item.instance_id` carries a caller-owned 64-bit key (the
engine packs `{world salt+tag, index, generation}` via
`le_object_stable_id`); each history slot stores an owner tag, and
a slot reused by a different key restarts UNKNOWN instead of
inheriting hysteresis. `0` means "no stable identity" (deterministic
metric path, always safe). See `OBJECT_IDENTITY_ARCHITECTURE.md`;
proven by `test_engine_vulkan` PART 7 and the keyed oracle in
`test_lod_vulkan`.
