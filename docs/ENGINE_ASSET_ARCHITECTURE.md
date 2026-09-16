# Engine Asset Architecture (Phase 25)

Engine-level assets (`le_asset`) wrap renderer resources for
gameplay code. Gameplay deals in asset handles, never renderer
pointers. (The `la_*` glTF importer is a separate layer below —
see `ASSET_ARCHITECTURE.md`; the engine bridges it, never
duplicates it.)

Layering: `le_*` → `la_*` → `lr_*` → `lc_*` (downward only).

## Identity: three concepts

| Concept | Form | Lifetime | Example |
|---|---|---|---|
| Runtime handle (`le_asset`) | `{index, generation}` | Until unload | `{3, 7}` |
| Persistent ID (`le_asset_id`) | 128-bit `{hi, lo}` | Forever (content) | `9f2c…` (32 hex) |
| Source path | Normalized string | Locator only | `meshes/crate.glb` |

- Runtime handles are generational (stale use → `LE_ERROR_STALE_ASSET`).
- Persistent IDs are content hashes (FNV-1a over imported bytes /
  PBR factors + source) for imports, minted UUIDs for procedural
  assets. The SAME file re-imported after unload yields the SAME
  persistent ID but a FRESH handle.
- Paths are NOT identity: separators normalized to `/`, `.`/`..`
  resolved lexically, leading `..` escapes rejected. Two spellings
  of one file deduplicate.

## Domain

Assets live on the ENGINE (`le_engine`), never in a world. Every
world references every READY asset. Destroying a world never
destroys assets. Engine shutdown drains worlds → registry → glTF
manager (adopted materials borrow cached views).

## Types

`LE_ASSET_MESH` (owns `lr_mesh`), `LE_ASSET_MATERIAL` (owns
`lr_material` + texture dependency pins), `LE_ASSET_TEXTURE` (owns
`lc_image` + view, full mip chain), `LE_ASSET_SCENE` (owns object
records). Wrong-type use → `LE_ERROR_WRONG_ASSET_TYPE` (never
silent reinterpretation).

## States

`UNLOADED / LOADING / READY / FAILED`. Phase 25 loads
synchronously (no fake async); the model permits a future loader.
Only READY assets submit; anything else skips as `skipped_dead`.

## Ownership

The registry OWNS renderer backing and destroys it at unload via
public APIs (renderer retirement covers GPU safety; the engine owns
no sync). Applications must not destroy backing resources directly
(the renderer liveness guard fails such misuse safely).

## Lifetime

Registry-owned until explicit unload (no GC). Unload scans all
worlds for referencing renderables + material texture pins
(O(objects)); referenced assets fail `LE_ERROR_ASSET_IN_USE`.
Renderables hold HANDLES, so world destruction merely drops
references.

## Dependencies

Materials pin textures (procedural `le_material_asset_desc`
texture fields). glTF-adopted materials borrow the bridge
manager's cached views (manager outlives the registry). Texture
assets from glTF images are deferred (duplicating GPU images or
holding borrows are both worse — documented, not silent).

## Duplicate loads

Deduplicate by canonical source: repeat `le_gltf_import` returns
the SAME handles, zero new GPU resources (proven: assets 2→2).

## Failure

Malformed input creates nothing (validate indices/factors/deps
BEFORE allocating; abandon reverses partial slots). Missing files
map to `LE_ERROR_MISSING_ASSET`, malformed content to
`LE_ERROR_PARSE`, GPU failure to `LE_ERROR_RENDERER`.

## Stats

`le_asset_stats` (alive/capacity/by-type/ready/failed/name bytes)
+ `le_asset_info` (type/state/ID/source/live reference count).

## Futures

Async loading (state model ready), explicit reload (deferred:
failure must leave READY usable — transactional replacement, not
destroy-first), streaming (out of scope).
