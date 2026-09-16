# Physics Solver Notes (Phase 28)

Companion to `PHYSICS_ARCHITECTURE.md`: constants, models, and
the rationale a future Phase 29+ needs before touching the
solver.

## Iteration model

- Velocity iterations (default 8): per contact in deterministic
  order — normal impulse (accumulated, clamped >= 0, no pulling)
  + restitution target + Coulomb friction (two tangents,
  `|jt| <= mu * jn` against the accumulated normal impulse).
- Position iterations (default 3): split residual penetration
  beyond slop by inverse-mass share, capped per contact. Writes
  go directly to root slots with batched dirty marks.
- Both counts set via `le_physics_set_iterations` (each clamped
  to [1,64]; zeros leave the value unchanged).

## Constants

| Symbol | Value | Meaning |
|---|---|---|
| `LE_PHYS_SLOP` | 0.005 m | penetration below this is rest |
| `LE_PHYS_BAUMGARTE` | 0.2 | velocity-bias stabilization |
| `LE_PHYS_MAX_CORRECTION` | 0.5 m/step | per-contact correction cap |
| `LE_PHYS_RESTITUTION_SLOP` | 1.0 m/s | bounce only above this |
| `LE_PHYS_KINEMATIC_VELOCITY_CAP` | 50 m/s | kinematic estimate cap |
| ring | 64/object | oldest drops, counted |
| contacts/pairs | 8192 | step caps (pairs evict past cap) |

## Restitution (read before modifying)

Restitution targets are computed in a **pre-pass** from
pre-solve approach velocities (`rest_bias = e * |vn0|`, pair
`e = max`) and held constant across all velocity iterations.
Recomputing the bias per iteration is a verified bounce-killer:
iteration 1 produces separating velocity, iteration 2 reads it
as "no approach" and removes iteration 1's impulse (see Phase
28 bring-up: `vn=-4.4, e=0.9` yielded `vy=+0.3` instead of
`+4.0`). The loop converges to `vn = +rest_bias`; Baumgarte adds
on top. Single-point manifolds re-bounce across steps, not
within one step — stacks stabilize via iteration as documented.

## Friction

Pair `mu = sqrt(fa * fb)` (geometric mean). Tangent basis from
the normal via a stable axis pick; impulse clamped to
`mu * accumulated_normal`. Zero on either side (or zero normal
impulse) disables friction for the pair.

## Damping and gravity

- Damping: `v *= 1/(1+d*dt)` (unconditionally stable).
- Gravity: per-world vector (default `(0,-9.81,0)`), per-body
  `gravity_scale`; validated to `|g| <= 1e6`, finite.
- Forces/torques accumulate until the next step, then clear.
  Impulses apply immediately (`dv = J*inv_mass`,
  `dw = I^-1 (r x J)` for point impulses).

## Known limits (explicitly out of scope)

No CCD (fast bodies tunnel — restitution tests drop from 1.5 m,
not 5 m, for this reason), no sleeping, no joints, no character
controller, no mesh colliders. Single-point manifolds only
(stacks of boxes settle; tall pyramids may jitter without
multi-point support — accepted for Phase 28).
