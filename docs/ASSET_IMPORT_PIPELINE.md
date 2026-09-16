# Asset Import Pipeline (Phase 32)

Importer **registry**, not a switch: `led_importer{id
("luma.gltf"), version, extensions}` dispatched by lowercase
extension. Unknown extensions are IGNORED (`UNSUPPORTED`), never
failures.

## Importers (all over EXISTING runtime pipelines — no
## editor-only parsers)

| ID v1 | Extensions | Mechanism |
| --- | --- | --- |
| `luma.gltf` | glb, gltf | `le_gltf_import` (+ animated where relevant) |
| `luma.texture` | png, jpg, jpeg | magic-gated validate, lazy upload (GPU on first use) |
| `luma.lua` | lua | `le_asset_load_script` |
| `luma.scene` | luma_scene | discovery only (magic gate; engine loads on open) |
| `luma.prefab` | luprefab | discovery only (magic gate; prefab loader on demand) |

Exact supported sets are reported; nothing is claimed beyond
them (no HDR texture assets — HDR lives only in the `la_hdr_*`
env path).

## Sub-asset stable keys (glTF)

- meshes: `mesh<mi>:prim<pi>` (matches the engine's stable mesh
  IDs `{fnv(src), (mi<<32|pi)^fnv}` — stable under reorder).
- materials: `mat<mi>:<sanitized-name>` (index fallback until
  the model layer exposes material names).
- textures: `tex<ti>:<sanitized-name>`; skeletons: `skin<si>`;
  clips: `clip<ai>:<sanitized-name>`.
- Sanitized names: lowercase alnum, others -> `_`, capped 64.

Model records publish the first mesh as representative runtime
handle; the full table rides the sidecar (`sub <key> <hex>`).

## Import execution

Synchronous in Phase 32 (`{pending,running,completed,failed}`
counters — worker-ready shape, no fake percents). Diagnostics
feed `led_console_push` (no second logger). Per-record
transactional: failure keeps prior good state + FAILED
diagnostics (never half-imported). Import-while-playing is
rejected (conservative policy).

## Known debt (tracked, not hidden)

- Engine material IDs are factor-hashes — reorder-fragile (the
  `gltf_bridge.c` positional fallback). Project material keys
  are index-derived until model material names are bridged.
  Identical-byte reimport identity IS proven stable
  (`test_gltf_identity`); reorder-robustness is future work.
- Texture GPU upload is lazy (records validate + stay READY
  with "upload on first use" diagnostics until a renderer
  consumes them).
