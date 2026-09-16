# Character Controller (Phase 30)

Dedicated kinematic capsule controller: sweep/slide/ground-probe/
slope/step — NOT a dynamic body with friction. The renderer and
LumaC layers are ignorant of character mechanics; all state lives
in engine-owned dense arrays with generational identity.

## Authoring

`le_character_desc`: `radius` (> 0), `height` (>= 2r), `up`
(normalized on store), `skin_width` ([0, radius] — contact
separation margin), `max_slope_angle` (radians, [0, PI/2) —
walkable limit), `step_height` (>= 0), `gravity` (>= 0, fall
acceleration along `-up`), `terminal_velocity` (>= 0),
`snap_distance` (>= 0, ground adhesion when descending),
`push_strength` (>= 0, 0 = block only), `layer`, `mask`.

Rules: world roots only (`LE_ERROR_INVALID_HIERARCHY` when
parented); no `LE_BODY_DYNAMIC` on the same slot (kinematic
controller vs dynamic integration would fight). Kinematic/static
bodies may coexist but are unnecessary.

Convention: object origin = FEET. Capsule center = origin + up *
(height/2); segment half length = height/2 - radius. The capsule
axis follows `up`, NOT object rotation (yaw-only convention).

## Pipeline (`le_character_move`)

1. **Depenetration** — zero-disp overlap probes, push out along
   min-penetration normal by (depth + skin); bounded 8 iterations,
   total travel capped at 4 * (radius + step + 0.1); reports
   `unresolved_penetration` when still overlapped.
2. **Platform ride** — when grounded on a live kinematic platform,
   inherit its frame translation delta (teleports > 2 m rebase
   instead). Rotation contribution deferred (translation only).
3. **Sweep** — capsule cast along the remainder; advance to
   (fraction - skin/|d|).
4. **Slide** — `v -= n * min(dot(v,n), 0)`, re-clipped against up
   to 3 recorded planes (corners settle, creases preserved);
   at most 4 slide iterations.
5. **Step attempt** (once per slide contact) — uses the
   PRE-advance horizontal remainder (per-frame moves are small;
   the post-advance sliver would rarely trigger): rise
   (step_height + skin, must be fully free) -> forward (remainder
   + bounded edge overhang <= r/2, swept) -> down
   (step + snap + 2*skin, must hit walkable). Accept iff the drop
   descends >= skin from the raised pose (rejects taller-than-
   budget obstacles that graze the raised feet) AND the landing
   gains height over the start (rejects no-op drops back to the
   current ground) AND the commit stops one skin short of drop
   contact (edge touches contact below the walkable surface;
   exact-contact would embed the flank and wedge). Landing sets
   ground state directly.
6. **Ground probe + snap** — cast down (snap + skin); walkable hit
   -> grounded (+ snap the gap, leaving skin; landing kills
   downward velocity). Disabled while rising (`fall_velocity >
   0`): jumps are never snapped down.
7. **Publish** — `le_character_move_result` (requested/actual,
   grounded + normal + generation-safe ground object, hit_wall /
   hit_ceiling / stepped / snapped / unresolved_penetration /
   collision_count).

Walkable: `dot(normal, up) >= cos(max_slope_angle)`. Wall vs
ceiling classification: `|upness| <= 0.7` -> wall, `< -0.7` ->
ceiling. No coyote time in core (script-layer policy).

## Vertical velocity and gravity

`fall_velocity` is stored signed along `+up` (positive = rising).
`le_character_gravity(dt)` integrates (`-= gravity * dt`, clamped
to terminal) and moves by `up * fall * dt`. Jump = positive
velocity (rises, snap disabled); landing zeroes it.
`le_character_set_vertical_velocity` clamps dives to terminal and
rises to `max(4 * terminal, 10)`. Teleport clears velocity +
ground. Disable makes move fail and grounded read false.

## Platforms, pushes, identity

Ground/platform objects are `(slot, generation, world_tag)` —
destroyed or slot-reused platforms invalidate automatically
(grounded reads false; ride detaches). Dynamic crates hit by the
controller receive a bounded impulse along the push-away normal
(`push_strength * approach / inv_mass`, capped) — crates block
and shove, never explode. Character-vs-character is
sweep-blocking only (no mutual resolution; each controller treats
the other as geometry).

## Budgets (no per-move heap)

4 slides, 3 planes, 8 depenetration iterations, 1 step attempt
per contact, bounded stack probes + world scratch only.
Counters in `le_physics_stats`: `character_sweeps`,
`character_slides`, `depenetrations`, `ground_probes`,
`step_attempts`.

## Serialization

`character <radius> <height> <upx> <upy> <upz> <skin>
<slope_deg> <step> <gravity> <terminal> <snap> <push> <layer>
<mask>` (15 tokens; authoring config only — never runtime
ground/velocities). Slope stored as DEGREES on the wire,
radians in memory. Strict transactional parse like physics.

## Files

`engine/src/physics/character.c` (controller); bindings
`engine/src/script/script_bind_character.c`; tests
`engine/tests/test_character.c` (38 checks),
`test_character_platform.c` (14), `test_character_script.c`
(Lua WASD determinism).
