# Physics & Collision Foundation (Phases 28 + 30)

Engine-owned rigid-body subsystem: one physics world per
`le_world`, driven by the Phase 27 fixed-step schedule. No
external library (no PhysX/Jolt/Bullet/Havok); small, correct,
and extensible. Lua consumes thin bindings; a future native/AOT
backend uses the same `le_*` APIs (collision dispatch goes
through the VM-independent `le_script_fire_collision`).

Phase 30 adds: capsule colliders (segment + radius, all pairs),
shape casts / sweeps (sphere analytic, capsule/box conservative
advancement), continuous collision (`CONTINUOUS` opt-in subset
for spheres/capsules vs static/kinematic), and the dedicated
kinematic character controller. Details:
`CHARACTER_CONTROLLER.md`, `PHYSICS_SHAPE_CASTS.md`,
`PHYSICS_CCD.md`, `LUA_CHARACTER_API.md`.

## Units and conventions

- 1 distance unit = 1 meter, time = seconds, mass = kilograms,
  angles = radians, force = newtons, impulse = newton-seconds.
- Y-up right-handed, Vulkan NDC, quaternions `(x,y,z,w)`.
- Contact normals point **A -> B** (sorted pair order).
- Determinism: same initial world + input + explicit dt sequence
  on the same build/arch reproduces results. Cross-platform
  bit-identical floats are NOT promised.

## Fixed-step order (no second timer)

Per fixed interval (`le_script_set_fixed_step` rate; physics
always runs even with zero scripts, at the configured rate or
the 60 Hz default):

1. `fixed_update` scripts run (apply forces/impulses first)
2. gravity + force accumulators -> velocities (dynamics only)
3. integrate velocities -> positions + orientations
   (semi-implicit Euler; quaternions normalized)
4. kinematic follow (transform -> body, capped velocity estimate)
5. broad phase (sweep-and-prune) + narrow phase -> contacts
6. sequential-impulse solve (normal + friction + restitution,
   Baumgarte/slop correction)
7. physics -> engine transform sync (dynamic roots only)
8. ENTER/STAY/EXIT (+ trigger) events -> script fan-out

`le_world_update(dt)` with `dt <= 0`/NaN/Inf runs matrices only
(never physics). Paused worlds skip simulation but still render.

## Bodies and colliders

- `LE_BODY_STATIC` — infinite mass, never integrated (may parent
  freely; collider-only objects behave as static geometry).
- `LE_BODY_DYNAMIC` — mass > 0 finite, integrated, collides.
  **Must be a world root** (`LE_ERROR_INVALID_HIERARCHY` when
  parented — no valid local inversion under parents).
- `LE_BODY_KINEMATIC` — script/app-driven via transforms; the
  engine estimates its velocity (capped at 50 m/s) so it pushes
  dynamics with infinite mass.
- Body-only objects integrate but never collide; trigger bodies
  never respond (events only).
- Sphere (`radius`), box (`half extents`), capsule
  (`capsule_radius` + `capsule_half_height` = HALF the cylinder
  length, 0 = sphere; all narrow-phase pairs incl. capsule-
  sphere/capsule-capsule/capsule-box with exact segment
  distances, touch-is-miss everywhere). Capsule inertia:
  axial `1/2 m r^2`, transverse composite.
  Local offset + orientation frame per collider.
- Scale policy: max `|axis|` multiplies sphere radius and box
  extents (exact for uniform/axis-aligned, conservative
  otherwise; negatives never negate). Capsules require uniform
  scale (non-uniform skips the collider — no closed-form rigid
  shape). Sheared or degenerate (near-zero axis, NaN) world
  matrices skip the collider for the step — never a silently
  invalid shape.
- Layers: `layer` 0..31 (single bit), `mask` 32-bit; a pair
  collides only when **both** directions pass.
- Pair friction = geometric mean; pair restitution = max
  (bounce only above 1 m/s approach).

## Detection and solver

- Broad phase: sweep-and-prune over world AABBs (insertion sort
  by min-x, active sweep on y/z). Canonical, sorted,
  duplicate-free pairs. A brute-force oracle exists only in
  tests, never in production.
- Narrow phase: sphere-sphere (+X fallback normal on
  coincidence), sphere-box (closest-point, least-penetration
  inside; center strictly inside required — face touch is a
  miss), box-box 15-axis SAT, capsule-sphere/capsule (segment
  closest points), capsule-box (endpoint spheres first, then
  exact segment-vs-OBB incl. face planes + 12 edges; graze =
  miss). Single deepest point per pair; swapped order reverses
  the normal.
- Solver: sequential impulse (default 8 velocity / 3 position
  iterations, each clamped to [1,64]). Restitution targets come
  from **pre-solve** approach velocities (computed once per step
  — recomputing per iteration unwinds the bounce). Baumgarte
  0.2 + slop 0.005 m (cap 0.5 m/step). Position correction
  splits by inverse mass.
- Inertia: sphere `2/5 m r^2`, box cuboid form (local
  dimensions); triggers zero. Point impulses use `r x J`.
- NaN discipline: one bad body is sanitized, never poisons the
  world; all public inputs validate finiteness.

## Events, queries, debug

- ENTER (first overlap), STAY (continued), EXIT (first clear);
  trigger variants likewise. Destroying an overlapping object
  emits EXIT to survivors. Pair identity is
  `(slot_a <= slot_b, gen_a, gen_b)` — slot reuse never inherits
  stale state. Per-object 64-event rings (oldest drops counted
  in stats); `le_physics_drain_events` consumes.
- Queries are read-only on step-fresh poses (auto-refresh, so
  they work before the first step): closest/all raycasts
  (near -> far), sphere/box overlaps, debug xyz line soup
  (collider wireframes + AABBs + contact segments), stats.
- Serialization: `rigid_body <type> <mass> <lindamp> <angdamp>
  <gscale> <lv*3> <av*3>` (12 tokens), `collider sphere|box|
  capsule ...` (15/17/16 tokens), `character ...` (15 tokens,
  slope in degrees), `%.9g` floats, strict transactional
  parse (duplicates/malformed rejected, scene untouched;
  value validation at commit/instantiate).

## Files

`engine/src/physics/`: `physics.c` (lifecycle), `body.c`
(add/remove/get), `dynamics.c` (forces/impulses/velocities +
collision modes), `collider_pose.c` (world pose + inertia),
`broadphase.c`, `narrowphase.c`, `solver.c`, `step.c`
(fixed-step driver), `events.c`, `query.c`,
`scene_physics.c` (capture/apply), `cast.c` (shape sweeps),
`ccd.c` (continuous sweep), `character.c` (controller).
Bindings: `engine/src/script/script_bind_physics.c`,
`script_bind_character.c`;
dispatch: `le_script_fire_collision` in `script_lua.c`.
