# Project Architecture (Phase 32)

Turns `engine + scene files + editor core` into a **Luma Project**:
a portable directory with a small manifest, source assets under
`Assets/` + `Scenes/`, and editor tooling (browser, import,
prefabs) over the same Project/Asset APIs a future human GUI and
future MCP/AI tooling share. The GUI is a later view; Phase 32 is
the model + pipelines, all headless-proven.

## Layout

```
MyGame/
  luma.project          (manifest: LUMA_PROJECT 1 + name/startup/
                         asset_roots/window; unknown fields tolerated,
                         unknown versions fail loudly)
  Assets/               (source models/textures/scripts/prefabs)
   Maiden.luprefabMaiden.luprefab.luma   (sidecar: UUID + importer +
                         fingerprint + deps + sub-assets)
  Scenes/
    Main.luma_scene
```

The manifest is hand-parsed (line discipline, no new dependency).
`asset_roots` defaults to `Assets/` + `Scenes/` when absent.

## Layering (strict, CI-audited)

```
editor/project (led_project_*, led_assetdb_*, led_import_*,
                led_browser_*, led_prefab_*, led_*_payload)
  |
  v
engine (le_*) — plus exactly ONE appended type: LE_ASSET_PREFAB
  |
  v
assets -> renderer -> LumaC
```

The project layer lives in `editor/` (same `luma_editor` lib, new
`project/` sources). The engine gains `LE_ASSET_PREFAB = 7`
(`COUNT = 8`) so prefab payloads ride the registry (handle
discipline, dependents-first teardown) — everything else
(manifest, paths, DB, importers, browser, prefab logic) is
editor-side over public APIs. No `engine -> editor`, no
`renderer -> editor`, no `LumaC -> editor` back-edge. No Lua
spellings outside `engine/src/script/` (prefab `sprop` lines go
through `le_script_*` values only).

## Rules (mandatory, tested)

- The browser is a VIEW:
  `Filesystem -> Project Asset Database -> Stable Asset Records
  -> Engine Asset Registry -> Editor Asset Browser`.
  The DB describes; the REGISTRY owns resources.
- One canonical root (the stored absolute manifest dir); never
  CWD-dependent identity. All joins use the stored root.
- Small manifest; explicit version (unknown versions fail loudly
  with `LED_ERROR_PARSE`).
- One path normalization (`led_project_normalize`) + escape
  rejection (`led_project_resolve` rejects `..` breakout,
  absolute and drive-absolute paths).
- `le_asset` handle != `le_asset_id` != source path — four
  identity levels, never conflated (see `ASSET_DATABASE.md`).
- Source files are never modified by import (sidecars +
  `.luma/`-style derived state only).
- Content fingerprint = `{size, FNV-1a-64(bytes)}`, never
  timestamps.
- Explicit scan (`led_project_scan`); no file watcher. Rescan is
  incremental (fingerprint compare; unchanged projects import
  nothing).
- Filesystem ops are PROJECT ops, not scene undo: rename/move
  preserve UUID (source + sidecar travel), delete ref-checks
  (no force-delete). No fake filesystem undo.
- Play isolation (conservative): reimport + prefab authoring are
  rejected while playing (`LED_ERROR_ALREADY_PLAYING`). Play
  content = prefab-expanded scene capture (no prefab awareness
  in the runtime).

## Module map

| File | Responsibility |
| --- | --- |
| `project/project_core.c` | Manifest, path canon, open/close/switch |
| `project/asset_db.c` | Records, sidecars, UUID/path indexes, search |
| `project/project_scan.c` | Explicit walk, ignore policy, mark+sweep |
| `project/import_impl.c` | Importer registry + per-type import |
| `project/reimport.c` | Candidate-then-swap reimport + reimport-all |
| `project/browser.c` | Folder tree, view, selection, drag payloads |
| `project/project_files.c` | Rename/move/delete + ref-check |
| `project/prefab_core.c` | Prefab collect/emit/create/load/instantiate |

## Proofs

- Portability: copy/move the whole project to a new absolute
  path, reopen, resolve + load + instantiate (`test_project`).
- Identity stability across rename/move/reimport/close-reopen
  (`test_project`, `test_import`, `test_gltf_identity`).
- Transactional reimport: broken source keeps last-known-good
  live (`test_import`).
- Prefab isolation: N instances, disjoint handles, per-instance
  state (`test_prefab`, `test_project_editor`).
- Edit/play isolation with prefab instances live
  (`test_project_editor`).
