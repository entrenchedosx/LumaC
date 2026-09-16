# GPU Skinning (Phase 29, renderer-owned)

Renderer-side linear-blend skinning over the shared `lr_vertex`
stride. The renderer never sees skeletons, clips, or playback
state — only joint matrices + vertices. The engine owns
animation (see `ANIMATION_ARCHITECTURE.md`); this document is
the reconciliation contract between the two.

## Frame reconciliation (who computes what)

- Engine (per frame, per animator): evaluates joint globals,
  then builds the skin palette:
  `skin[j] = joint_global[j] * inverse_bind[j]`, expressed in
  the animated object's LOCAL frame (see
  `ANIMATION_ARCHITECTURE.md`).
- Engine submit: borrows the palette on `lr_draw_item`
  (`skin_palette` + `skin_joint_count`).
- Renderer submit (`lr_renderer_submit`): copies the palette
  synchronously into a renderer-owned per-frame arena (never
  retains the caller pointer — free/mutate immediately after
  submit returns).
- Renderer draw: uploads the arena once per frame into one GPU
  storage buffer, binds ONE descriptor set for every skinned
  draw, and passes the per-draw `(skin_offset, skin_joints)`
  window through the push block.
- Shader: blends in the mesh local frame, then applies the
  item's model matrix exactly like the rigid path:
  `world = model * skinned_local`. The model matrix carries
  the object root; joints are relative to it.

## Vertex format (unchanged)

`lr_vertex` is 88 bytes and the stride NEVER changes:
position (0), normal (12), tangent (24), texcoord (40),
joints uvec4 (48), weights vec4 (64). Rigid and skinned
pipelines bind the same buffers; only the pipeline's
attribute declarations differ (rigid pipelines never declare
locations 4/5, so no unconsumed-attribute validation noise).

## Weight policy

- Weights normalize in-shader: `w_i' = w_i / sum(w)`.
- Zero-weight vertices (`sum <= 1e-9`) hold bind position
  (never NaN) — same fallback as the CPU oracle
  `le_anim_skin_vertex` (which additionally renormalizes
  normals; the GPU path transforms normals by the push-block
  normal matrix after blending, matching the rigid path).
- Normals/tangents blend by the joint upper-3x3 (rigid-ish
  assumption, documented in the engine oracle), then ride the
  model normal matrix like rigid vertices.

## Zero-weight fallback

A vertex with all-zero weights renders at its authored bind
position. Joint 0 at weight 0 never moves a vertex: the loop
accumulates `w * (m * p)` and `w = 0` contributes nothing
(the rigid convention joints `{0,0,0,0}` + weights
`{1,0,0,0}` therefore renders identically skinned or rigid).

## Influence limit

Exactly 4 influences per vertex (`JOINTS_0`/`WEIGHTS_0` in
glTF terms). Importers REJECT `JOINTS_1`/`WEIGHTS_1` (and any
second set): meshes with > 4 influences must be quantized to
4 at import (keep the 4 largest, renormalize). The renderer
has no second-set path by design.

## OOB joint clamp

Joint indices clamp to `[0, skin_joints - 1]` in-shader
(per influence, before the arena read). Out-of-range indices
(stale meshes, short palettes) render with the last joint —
wrong but bounded, never an OOB buffer read. The CPU oracle
instead REJECTS OOB joints (tooling strictness); the GPU
clamps (frame robustness). Both agree on valid inputs.

## Bounds policy (conservative)

Skinned items SKIP frustum culling: `main_visible` is forced
at submit. The authored static bounds (AABB + sphere) cannot
bound the animated pose — culling against them would pop as
joints move. The static bounds still feed the shadow pass and
the (unused-by-skinned) GPU-driven grouping key. Tightening
this (per-frame joint-AABB upload) is deferred: correctness
first, one forced-visible draw per skinned item.

## GPU-driven / indirect exclusion

Skinned items NEVER join GPU-driven (`gpu_driven.c`) or
extended-visibility (`visibility.c`) groups: grouping keys
(mesh, material, shadow-flag) cannot express per-draw
palettes, and the instanced shader has no skin path. Skinned
items stay queued; the CPU loop draws them after the
indirect draws. In GPU-driven mode a frame with skinned PBR
items records BOTH indirect draws (rigid groups) and direct
draws (skinned items) — proven by the skin test's GPU-mode
leg.

## Shadow path

Skinned casters render through a skinned depth variant
(position + joints + weights attributes, skin window in the
72B push) with cull NONE like the rigid depth pipeline. The
same arena upload covers shadow and main passes (one upload
per frame, idempotent per `frame_number`).

## Static-mesh regression guarantee

- Rigid draws (no palette, or palette on a rigid mesh) take
  the pre-existing pipelines with `skin_offset = 0` /
  `skin_joints = 0` pushed (the rigid shaders ignore the
  tail bytes; push size grew but offsets of model/color did
  not move).
- A skinned mesh with an identity palette renders
  pixel-identical (near-identical under float rounding) to
  the same geometry drawn rigid — proven by
  `renderer/tests/test_skin_vulkan.c` (bind-pose leg).
- Push blocks stay under the 128B limit: PBR 124B, unlit
  88B, shadow 72B (static-asserted in `material.c` for PBR).

## Limits

- `LR_SKIN_MAX_JOINTS` = 4096 joints per submit (matches the
  engine `LE_ANIM_MAX_JOINTS` ceiling); larger palettes fall
  back to rigid at submit (bounded arena, loud only on OOM).
- Arena grows geometrically in 64-joint chunks (CPU + GPU
  sides independently); capacities persist across frames.
- `skin_offset`/`skin_joints` are `uint32_t` push fields;
  `skin_offset + skin_joints` never exceeds the frame arena
  (both derive from the same submit-time copy).

## Files

- `renderer/src/skin.c`: arena, GPU mirror, one set, three
  skinned pipeline mini-caches, `lr_mesh_is_skinned`.
- `renderer/shaders/{pbr,unlit,shadow}_skinned.vert` (+
  committed `.spv`, recompiled by glslc when available).
- Test: `renderer/tests/test_skin_vulkan.c`.
