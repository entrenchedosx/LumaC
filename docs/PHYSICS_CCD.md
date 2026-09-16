# Continuous Collision Detection (Phase 30)

Deliberate subset foundation: CONTINUOUS dynamic sphere/capsule
bodies sweep along `v * dt` each fixed sub-step vs
static/kinematic geometry. Default remains DISCRETE (Phase 28
behavior, unchanged).

## Scope (deferred, documented)

- Dynamic-vs-dynamic CCD: deferred (discrete response still
  applies; the sweep stops at the first dynamic hit and leaves
  the rest to the solver).
- Box CCD: deferred (boxes integrate discretely even in
  CONTINUOUS mode — no silent fallback damage).
- Sphere/capsule vs static/kinematic only.

## Algorithm (per body per sub-step, bounded `LE_CCD_MAX_ITERS` 4)

```
remaining = dt
loop (<= 4):
    cast shape along v * remaining (blocking only, self excluded)
    on miss: integrate fully, done
    on hit at fraction f:
        advance (f * remaining) minus LE_CCD_MARGIN (0.001) along v
        kill inward-normal velocity (slide; restitution stays
            with the discrete solver on the resulting contact)
        remaining *= (1 - f)
```

Never moves beyond first contact (unlike discrete
correct-after). The margin keeps t=0 re-hits impossible. A
resting-contact shortcut hands tiny remainders (`< 2 * margin`)
to the discrete solver (prevents sweep-backoff fighting
Baumgarte hover); a fraction-~0 hit with velocity already
pointing away (or negligibly inward) likewise defers to the
contact response instead of freezing the fall.

## API

`le_collision_mode` (`LE_COLLISION_DISCRETE` = 0 default,
`LE_COLLISION_CONTINUOUS` = 1); `le_physics_set_collision_mode`
/ `le_physics_get_collision_mode` (stored per body; the stepper
only sweeps dynamics). TOI in [0, dt].

## Files

`engine/src/physics/ccd.c` (`le_ccd_sweep_body`, called from
`step.c` after the shared velocity update); stats `ccd_casts` /
`ccd_impacts`; tests `engine/tests/test_ccd.c` (11 checks:
tunnel/stop pairs for sphere + capsule at 50 m/s vs a 0.1 wall,
rest stability, dyn-vs-dyn finiteness).
