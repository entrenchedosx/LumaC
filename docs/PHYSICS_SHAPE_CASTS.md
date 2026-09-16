# Physics Shape Casts (Phase 30)

Sweeps (shape casts) against step-fresh collider poses: sphere
(analytic), capsule and box (conservative advancement). Read-only
queries — never move anything, never allocate per cast (bounded
stack scratch + world scratch).

## Semantics

- `fraction` in [0,1]: 0 = immediate hit at start, 1 = full
  movement free. `distance = fraction * |displacement|`.
- Zero displacement = overlap query at the start pose (hit iff
  genuinely penetrating: fraction 0 + `started_overlapping`).
- Initial overlap never pretends collision-free: fraction 0,
  `started_overlapping = 1`, depenetration normal/depth.
- Touching is a MISS everywhere (narrow phase, sweeps, initial
  overlap): penetration > 0 required. A zero-depth "hit" through
  any fallback path is still a miss for sweep purposes.
- Swept AABB = union(start, start + displacement) + margin
  selects candidates (layer/mask unilateral query convention —
  see below; self skipped by slot; triggers never block).
- Blocking pass + trigger report-only pass (nearest trigger
  overlap; never affects the blocking result).
- Ties break by stable `(fraction, slot, generation)` identity —
  never traversal order. Deterministic across runs.

## Narrow TOI paths

- Sphere vs sphere: ray-sphere with `R + r`.
- Sphere vs box: ray vs expanded OBB (slab test in the box
  frame, shrunk by a touch epsilon so exact-touch agrees with
  the narrow-phase miss convention).
- Sphere vs capsule: ray vs expanded capsule (cylinder +
  cap spheres).
- Capsule/box probes: conservative advancement, bounded 32
  iterations on exact segment-vs-shape clearance (segment vs
  sphere-center / segment vs OBB / segment vs segment — tight,
  so sub-radius sweeps converge). The advance stops at
  clearance <= 0; a t=0 graze (touching) is a miss (the
  initial-overlap branch already handled true overlap).
- Contact at TOI: re-run narrow phase at (start + d*t); normal
  points from the static collider toward the probe (against
  motion). Exact-touch TOI uses a hair-inflated retry (1e-4) to
  recover the TRUE geometric normal instead of a motion-aligned
  guess (steep slopes otherwise report +up and read walkable).

## Layer/mask convention

Unlike the discrete broad phase (both directions must pass),
sweeps use the Phase 28 raycast/overlap query convention: the
caller's `layer_mask` selects target layers unilaterally, and
the target's own mask does not veto. `exclude_slot` skips one
object slot (typically the caster; `0xFFFFFFFFu` for none).

## API

`le_physics_shape_cast` (generic) + `le_physics_sphere_cast` /
`le_physics_capsule_cast` / `le_physics_box_cast` conveniences.
Capsule dims convention matches colliders (radius + half cylinder
length). `le_shape_hit`: object (generational), fraction,
distance, point, normal, `started_overlapping`, penetration.

## Files

`engine/src/physics/cast.c`; stats `shape_casts` /
`cast_candidates`; tests `engine/tests/test_cast.c` (31 checks
incl. oracles, tie determinism, 3000-case fuzz).
