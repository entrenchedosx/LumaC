# Animation, Skeleton & GPU Skinning Foundation (Phase 29)

Engine-owned keyframe animation over the existing asset
registry, transform hierarchy, time system, and renderer
resource model. No external library; no editor, state machines,
IK, retargeting, root-motion controllers, ragdoll, cloth, morph,
or facial systems (all deferred). Lua controls playback through
thin `le_anim_*` bindings; a future native/AOT backend uses the
same APIs.

## Units and conventions

- Joint TRS is LOCAL (parent space); time is seconds; angles
  are radians; quaternions `(x,y,z,w)`, normalized on store.
- Matrices are column-major 4x4, Y-up right-handed (same as the
  engine transform layer).
- Skin matrices are `joint_global * inverse_bind` in the
  animated object's local frame. The model matrix carries the
  object root; joints are relative to it (reconciled in
  `GPU_SKINNING.md`, proven by the CPU/GPU oracle test).
- Determinism: same assets + dt sequence reproduces poses.
  Cross-platform bit-identical floats are NOT promised.
- Threading: single-threaded (owning thread, like scripts and
  physics). No thread safety is claimed.

## Layers (immutable vs mutable, strictly separated)

- Asset layer (immutable, registry-owned):
  `LE_ASSET_SKELETON` — joint hierarchy + bind pose + inverse
  bind matrices. Runtime joint identity is the compact array
  index; names are import/debug/Lua sugar (duplicates allowed,
  first match wins lookups).
  `LE_ASSET_ANIMATION_CLIP` — duration + TRS tracks (per-joint
  and/or per-object), key times/values, interpolation.
- World layer (mutable, per-animator runtime):
  `LE_COMPONENT_ANIMATOR` — skeleton/clip handles, playback
  time/speed/loop/playing, crossfade state, evaluated pose
  (local + global + skin palette scratch).
- Renderer: skin palette (joint matrices) + GPU-skinned mesh
  (`GPU_SKINNING.md`). The renderer never sees skeletons,
  clips, or playback state — only matrices + vertices.

## Skeletons

- Joints: `name[64]`, `parent` (-1 = root, else joint index),
  bind TRS, `inverse_bind[16]` (column-major, finite).
- Validation (malformed input creates NOTHING): parents in
  range, no self-parent, no cycles, every joint reachable from
  a root, at least one root, joint count in
  `[1, LE_ANIM_MAX_JOINTS]` (4096), finite transforms, nonzero
  scale (zero scale collapses subtrees — rejected at
  ingestion), normalizable bind quats. Multiple roots are LEGAL
  (forests animate each root from identity). Cycle checks are
  iterative (1000-deep chains are safe — no recursion
  anywhere).
- glTF skins map: joints = skin joints, parents from the node
  hierarchy, bind TRS from node local transforms, inverse bind
  from `inverseBindMatrices` (see `GLTF_ANIMATION_IMPORT.md`).

## Clips

- Duration: finite, > 0. Tracks: at most `LE_ANIM_MAX_TRACKS`
  (65536) per clip, `LE_ANIM_MAX_KEYS_PER_TRACK` (1048576)
  keys each (checked size arithmetic throughout).
- Targets: `LE_ANIM_TARGET_JOINT` (index = joint index; stale
  indices past the skeleton length are SKIPPED, never OOB) or
  `LE_ANIM_TARGET_OBJECT` (the animator owner's local TRS —
  doors/props with no skeleton).
- Channels: translation / rotation / scale (glTF vocabulary).
- Interpolation: STEP (previous value; exact next key reads
  the next value), LINEAR (lerp for T/S, shortest-path SLERP
  with hemisphere flip for R), CUBICSPLINE (glTF Hermite
  in/value/out triples, tangents scaled by the key interval;
  quaternion results normalized).
- Key times: finite, >= 0, non-decreasing. Duplicates are
  allowed with explicit last-wins sampling (upper-bound
  search). Rotation values (or the value slot of a cubic
  triple) must be normalizable.
- Sampling is a pure function of `(clip, time)`: no Lua, no
  playback state. Loop/wrap policy belongs to the caller.
  Segment location uses a cached cursor with a binary-search
  fallback (seeks stay correct; sequential playback reuses).

## Playback

- `play(clip, restart)`: a different clip (or restart != 0)
  restarts at 0; same clip with restart == 0 continues time.
  Always sets playing. `pause` holds time; `resume`
  continues; `stop(reset=0)` holds the last pose,
  `stop(reset!=0)` returns to bind pose + time 0.
- `seek` clamps into `[0, duration]` (cursors self-correct).
  `speed` is finite, >= 0 (negative rejected, unchanged).
- Loop modes: ONCE (clamp at end, stop playing, hold pose),
  LOOP (bounded wrap, no drift), PING_PONG (bounded direction
  alternation).
- Crossfade `(to, duration)`: blends the CURRENT blended pose
  to the destination over `duration` (0 = immediate).
  Interrupting a fade snapshots what's on screen (no snap
  back to either endpoint). Positions/scales lerp, rotations
  slerp (`le_anim_blend_pose` is also public for tests/tools).
- Disabled objects/components hold time and resume on
  re-enable. Missing animator = `LE_ERROR_INVALID_ARGUMENT`
  (queries return zeros/false).

## Frame order (no second animation clock)

Phase 27 time owns the only clock. Per frame
(`le_world_update(dt)` / engine `le_world_simulate_engine`):

1. matrices -> PASS1 starts -> PASS2 fixed (fixed_update +
   physics) -> PASS3 update scripts
2. ANIM visual advance with the same scaled dt (enabled
   animators only; `dt <= 0`/NaN/Inf holds pose)
3. matrices (object-track writes land here)
4. extraction reads evaluated poses + borrows palettes
   (no re-evaluation: `pose_version` gating; static poses
   evaluate on demand so extraction-before-first-step skins)

Paused worlds never advance animation (but still render).

## Transform ownership (physics reconciliation)

- OBJECT tracks write the owner's LOCAL transform.
- Animator + no/static/kinematic body: allowed.
- Animator + DYNAMIC body + OBJECT-target tracks: REJECTED at
  add-time with `LE_ERROR_INVALID_HIERARCHY` (physics owns
  the root transform; no tug-of-war).
- JOINT-target tracks under a dynamic root are VALID (physics
  drives the root, animation drives the joints).

## CPU skinning oracle

`le_anim_skin_vertex` (tests + tools; NOT the render path):
`p' = sum weight_i * (joint_matrix_i * p)`, normals via the
upper 3x3 (renormalized). Weights normalize (divide by sum);
zero-weight vertices hold position (never NaN); out-of-range
joints and non-finite weights are rejected before use.

## Serialization

`animator <skelhex|nil> <cliphex|nil> <autoplay 0|1>
<once|loop|pingpong> <speed> <start>` (7 tokens, `%.9g`
floats). Authoring/playback state only (never poses or
palettes). IDs resolve READY-only at instantiate (missing =
`LE_ERROR_MISSING_ASSET`); value ranges re-validate at
commit; duplicates/malformed lines fail the load
transactionally. Bare `animator`/`animation` lines outside
the dedicated branch are rejected (never silently dropped).

## Lua surface (thin; same `le_*` for AOT)

Object methods (no global table): `animation_play(clip?),
animation_pause/resume/stop(reset?), animation_seek(t),
animation_speed([s]), animation_crossfade(clip, dur),
animation_is_playing/time/duration`. Clips resolve via
`Assets.find_by_id(hex)` (same handle discipline as every
other binding; stale handles error, instances fail per the
error policy). No Lua-side animation state exists.

## Files

`engine/src/animation/`: `anim_asset.c` (immutable
construction + validation), `anim_sample.c` (sampler),
`anim_pose.c` (bind/local->global/skin/blend/oracle),
`animator.c` (component add/remove/get), `anim_playback.c`
(play/pause/.../crossfade/queries), `anim_step.c` (visual
advance + evaluation + stats), `anim_api.c` (asset
create/getters/sample/bind), `anim_gltf.c` (decoded builders),
`anim_serialize.c` (capture/validate/apply/palette query).
Bindings: `engine/src/script/script_bind_anim.c`.
Renderer: `GPU_SKINNING.md`. Import: `GLTF_ANIMATION_IMPORT.md`
+ `engine/src/gltf_bridge_anim.c`
(`le_gltf_import_animated`: skin -> skeleton, animations ->
clips over a caller-loaded `la_model`; non-skeleton channel
targets skip with a loud count; transactional).
