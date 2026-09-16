# glTF Skin + Animation Import (Phase 29)

Decoded, engine-ready skin and animation import in the Luma Assets
module (`assets/src/gltf_import.c`, queries in `assets/src/model.c`,
surface in `assets/include/luma_assets/luma_assets.h`). This is the
import half of the animation pipeline; the engine half
(`le_gltf_import_animated`: skin -> skeleton asset, animations ->
clip assets) lives in `engine/src/gltf_bridge_anim.c` and consumes
only the query API documented here.

Conventions inherited from the base importer: file order is
preserved everywhere (skins, joints, animations, channels);
caches are insertion-ordered; matrices are column-major;
quaternions are `(x, y, z, w)`; Y-up right-handed; UVs top-left.

## Vertex skinning (JOINTS_0 / WEIGHTS_0)

- `JOINTS_0` must be `VEC4` of `UNSIGNED_BYTE` or `UNSIGNED_SHORT`,
  UNnormalized (scalar-per-component indices, per glTF 2.0).
  Any other component type, or a normalized flag, fails the import
  with `LA_ERROR_UNSUPPORTED`.
- `WEIGHTS_0` must be `VEC4` of `FLOAT`, or normalized `u8`/`u16`
  (decoded through the same normalized-float path as `TEXCOORD_0`).
  Anything else fails with `LA_ERROR_UNSUPPORTED`. Non-finite
  weights (NaN/Inf) fail with `LA_ERROR_IMPORT` — they would
  poison normalization and are never stored.
- **Weight normalize rule:** each vertex's 4 weights are divided
  by their sum so they total 1. An all-zero (or cancelling) sum
  falls back to rigid `{1,0,0,0}` — never NaN.
- **Missing streams:** absent `JOINTS_0`/`WEIGHTS_0` selects rigid
  defaults (`joints {0,0,0,0}`, `weights {1,0,0,0}`). When exactly
  one of the pair is present, the other defaults; both absent is
  the common unskinned case.
- **Four-influence limit:** any primitive carrying `JOINTS_1`,
  `WEIGHTS_1` (or any higher joint/weight set) fails the WHOLE
  import with `LA_ERROR_UNSUPPORTED` ("four-influence limit") —
  never silently dropped.
- Sparse `JOINTS_0`/`WEIGHTS_0` are rejected per the existing
  sparse-accessor policy (`LA_ERROR_UNSUPPORTED`).
- Joint values are stored verbatim (widened to `uint32`); they are
  NOT range-checked against any skin at the primitive level — a
  mesh is shared across nodes while skins bind per node, so range
  validation belongs to the skeleton builder at use time
  (out-of-range joints are rejected there, before use).

## Skins

- Each file skin becomes one model skin (file order); skins own
  their joint-node list (`int32_t` model node indices) and their
  inverse-bind matrices, decoded ONCE at import.
- **Joint validation at import:** every joint must reference a
  node in range, and joint nodes must be UNIQUE within a skin.
  Duplicates fail with `LA_ERROR_IMPORT`: the engine keys
  skeleton joints by node, so a duplicate would collapse two
  joints into one. (Duplicate joint *names* downstream are fine —
  that is a lookup-sugar concern, not identity.)
- **Zero joints:** a skin with no joints is malformed
  (`LA_ERROR_IMPORT`).
- **Inverse bind:** decoded from the skin's `inverseBindMatrices`
  accessor (`MAT4` float, count == joint count, every float
  finite-checked) into model-owned column-major floats. A missing
  accessor defaults every joint to identity (glTF default).
  Malformed data fails the import (`LA_ERROR_IMPORT`; sparse
  gives `LA_ERROR_UNSUPPORTED`); nothing partial is kept.
- **Node link:** `la_model_node.skin_index` is the file-order skin
  used by that node, or -1 when unskinned (filled exactly like
  `mesh_index`: pointer-to-index, range-checked).
- **Skeleton derivation rule (engine side):** joints = the skin's
  joint nodes in file order; parents = the nearest joint ancestor
  in the NODE hierarchy (non-joint intermediates bake into the
  bind pose); bind TRS decomposes from node `local_matrix`
  chains (relative matrices, lossless source); inverse bind comes
  from this import. The skin's `skeleton` root hint is
  intentionally ignored — parents always derive from the node
  hierarchy.

## Animations

- **Morph skip rule:** channels targeting `weights` (morph
  targets, a deferred feature) are SKIPPED at import — skipped,
  not failed. An animation left with zero importable channels is
  EXCLUDED from the model entirely: it contributes no index, no
  count, and no duration. The engine additionally reports
  surviving-track counts per clip (loud, never silent).
- **Channel mapping:** `translation` -> path 0 (vec3), `rotation`
  -> path 1 (quat `xyzw`, vec4), `scale` -> path 2 (vec3);
  `STEP` -> 0, `LINEAR` -> 1, `CUBICSPLINE` -> 2 (glTF's default
  interpolation is `LINEAR`). Unknown paths fail the import.
- **Times:** sampler inputs must be scalar float, count > 0,
  finite, `>= 0`, non-decreasing. Negative or NaN/Inf times fail
  the import. Strict increase is NOT required — duplicates are
  accepted (the engine samples them last-wins).
- **Values:** outputs must be float `vec3` (T/S) or `vec4` (R)
  with element count == key count (x3 under CUBICSPLINE),
  finite-checked. Rotation quats are finite-checked ONLY —
  normalization is the engine's job at store time.
- **CUBICSPLINE triple layout:** under `CUBICSPLINE` each key
  expands to an in-tangent / value / out-tangent Hermite triple
  in FILE order (`[in, value, out]`, offsets `0/comps/2*comps`
  per key). `key_count` always equals the input count while
  `values` holds `key_count * 3` vectors; the engine scales
  tangents by the key interval at sample time.
- **Targets:** the target node must exist (in range), else the
  import fails. Channels targeting non-skeleton nodes still
  import (the engine skips those tracks loudly per clip).
- **Duration:** max last-key input time across the animation's
  KEPT channels (0 for empty); morph-skipped channels do not
  contribute.
- Counts are range-checked with overflow-safe arithmetic
  throughout; sampler `input`/`output` must both be present.

## Lifetimes, errors, transactions

- All decoded skin/animation arrays are model-owned and freed in
  `la_model_destroy`; query pointers borrow model storage (valid
  while the model lives).
- Every query is NULL-safe; out-of-range indices yield zeros
  (counts), -1 (joint-node / node-skin links), identity (bad
  inverse bind), or failure/0.0 (bad channel/duration).
- Transactional: malformed skin/animation data fails
  `la_model_load` with no partial state (no half-decoded skins,
  no half-kept animations, no GPU residue — animations decode
  before any mesh uploads so track failures precede GPU work).
- Files that imported before this change import identically:
  unskinned primitives now write the documented rigid defaults
  instead of leaving joints/weights uninitialized, which is the
  only vertex-level difference, and no new errors trigger on
  files without skins, animations, or extra joint sets
  (proven by the unmodified pre-existing test expectations).

## Test fixtures

- `assets/tests/fixtures/skinned.glb` (from
  `assets/generate_fixtures.py::make_skinned`): 4 nodes
  (Root/JointA/JointB/MeshNode+skin), 1 skin (joints [1,2],
  joint1 inverse bind = translation (1,2,3)), 2 primitives (u8
  joints + float weights incl. a normalize case and an
  all-zero rigid case; u16 joints incl. index 300 + normalized
  u16 weights), 1 `Wave` animation (T LINEAR + R CUBICSPLINE)
  plus 1 morph-only animation (excluded from the count).
- Headless negatives (crafted temp `.glb`s, placeholder
  renderer): JOINTS_1, zero-joint skin, duplicate joints,
  inverse-bind mismatch, negative times, output mismatch,
  unknown path. Meshless skin/animation positives run headless;
  upload positives (vertex readback) run in
  `test_assets_vulkan.c`.
