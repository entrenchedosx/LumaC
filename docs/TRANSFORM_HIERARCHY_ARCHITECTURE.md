# Transform & Hierarchy Architecture (Phase 24)

Every object carries exactly one **universal transform component**:
position (vec3) + rotation (unit quaternion x,y,z,w) + scale (vec3,
negative axes legal for mirroring). New objects are identity
(p=0, q=identity, s=1). Setters normalize quaternions (zero-length
or non-finite input restores identity) and reject non-finite
position/scale.

## Convention (project-wide, same as the renderer)

- Column-major 4×4, meters, Y-up right-handed, camera looking along
  −Z in view space, Vulkan NDC (depth 0…1).
- Local matrices compose **T × R × S**; world matrices compose
  **world(child) = world(parent) × local(child)**.

## Hierarchy

- Links are parent index + first-child / next-sibling chains over
  **stable handles** (never pointers). Roots are parentless live
  objects.
- Reparenting **preserves the local transform** (documented Phase 24
  policy): local bytes untouched, world matrix follows the new
  parent. A world-preserving variant is future work. Detach via
  `NULL` or `LE_OBJECT_INVALID` parent.
- Cycles rejected with `LE_ERROR_CYCLE`, world unchanged:
  self-parenting (`A → A`) and ancestor loops (`A → B → A`,
  `A → B → C → A`) via an iterative ancestor walk.
- Destruction default: **destroy subtree** (depth-first, iterative
  post-order with an explicit heap stack — no recursion). No
  "orphan to root" mode exists in Phase 24. Children never dangle.

## Dirty propagation

Any local change (position/rotation/scale/reparent) marks the
object **and its whole descendant subtree** dirty via iterative DFS
(heap stack). World matrices refresh two ways:

- `le_world_update`: iterative root-first walk, dirty-driven
  (O(1) when clean via a dirty-hint short-circuit).
- `le_object_get_world_matrix`: ancestors-first refresh along one
  object's chain only (O(depth), heap-backed for absurd depths).

Ascending slot order is NOT topological (a parent may sit above its
child), so refresh never assumes index order. Whole-world
recomputation per setter never happens. 10k-deep chains and 100k
objects are stack-safe by construction (proven in `test_engine`).

## Mirrored transforms (audit fix, do not regress)

Negative-determinant world matrices flip winding. Parity is decided
once per submission (`le_matrix_is_mirrored` upper-3×3 determinant
sign → `lr_draw_item` mirrored path → CW front-face variant,
parity-grouped GPU batches). Engine decomposition
(`le_world_to_transform`) keeps the mirror sign on the
smallest-magnitude scale axis so the renderer's determinant-keyed
flip agrees. Test matrix: −X, −Y, −Z, double-mirror (even parity ⇒
not mirrored) — pixel-proven in `test_engine_vulkan` PART 4 and the
renderer `test_mirror_vulkan`.

## Iteration determinism

For unchanged world state, extraction and child queries walk
**ascending slot order** — never dense-component order (which
churns on swap-remove). Deterministic iteration helps future
serialization, testing, Lua, networking, editor. Physics/simulation
determinism is NOT promised.
