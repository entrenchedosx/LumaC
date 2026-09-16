# Persistent Identity Architecture (Phase 25)

Runtime handles are TEMPORARY. Serialized references are FOREVER
(or at least content-stable). Never conflate them:

| Handle | Scope | Stable across runs? | Stored on disk? |
|---|---|---|---|
| `le_object` `{index,generation,world_tag}` | World runtime | No | NEVER |
| `le_scene_object_id` `{hi,lo}` | Scene content | Yes (UUID) | Yes |
| `le_asset` `{index,generation}` | Engine runtime | No | NEVER |
| `le_asset_id` `{hi,lo}` | Asset content | Yes (hash/UUID) | Yes |
| Source/scene path | Project layout | Human-stable | As hint only |

## Rules

- Scene files store persistent IDs + path HINTS. Instantiation
  resolves IDs through the live registry (paths never trusted for
  identity; relocation-safe by design).
- Lua (future) must NEVER persist raw runtime indices to disk —
  always map through instance records + persistent IDs
  (`SCRIPTING_ARCHITECTURE.md`).
- Two instances of one scene share persistent IDs LOGICALLY but own
  disjoint runtime handles AND disjoint renderer temporal keys
  (keys mix world salt: same local ID, different instances, zero
  shared LOD/motion history).
- Capture preserves IDs for previously-instantiated objects (slot
  stamps), mints for new ones, drops deleted ones. Re-capture is a
  snapshot, not a merge.
- Asset re-import after unload: SAME persistent ID (content hash),
  FRESH runtime handle. Procedural assets: fresh UUID per create
  (each create intentionally distinct; dedup is by import path).
- All IDs print/parse as 32 lowercase hex (`le_asset_id_to_string`
  / `_from_string`, strict 32-char validation).
