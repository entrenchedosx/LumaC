# Project Paths (Phase 32)

One path normalization + escape rejection. No CWD dependence
anywhere in the project layer.

## Normalization (`led_project_normalize`, lexical only)

`/` separators, backslash folding, collapsed duplicates,
resolved `.`/`..` (leading `..` retained lexically so resolve
can reject escapes), no trailing slash, 1023-char cap. Rejects:
NULL/empty/overlong/absolute/drive-absolute inputs.

## Resolution (`led_project_resolve`)

Join the NORMALIZED rel against the STORED absolute root (the
manifest dir as opened — portability holds because joins never
consult the process CWD). Rejects leading `..` escapes,
absolute paths, drive paths, overlong joins. Returns the
absolute OS path (2048 cap).

## Matrix (proven in `test_project`)

| Input | Normalized | Resolves |
| --- | --- | --- |
| `Assets/a.luprefab` | same | yes |
| `Assets\a.luprefab` | `Assets/a.luprefab` | yes |
| `Assets//a.luprefab` | `Assets/a.luprefab` | yes |
| `./Assets/a.luprefab` | `Assets/a.luprefab` | yes |
| `Assets/x/../a.luprefab` | `Assets/a.luprefab` | yes |
| `Assets/` | `Assets` | yes |
| (empty) | — | no (INVALID) |
| `/abs/path` | — | no |
| `C:/win/path` | — | no |
| `../escape` | `../escape` (lexical) | no (resolve rejects) |

Long/UTF-8 paths: dynamic buffers throughout the scan (512-byte
entry names, 1024 rel cap, 2048 abs cap — overlong fails
cleanly, never truncates into an escape).

## Filesystem operations (project ops, NOT scene undo)

- **Rename/move** (`led_project_rename`): source + sidecar move
  together (portable move: `rename()` + cross-volume copy
  fallback with rollback); destination-exists and escapes fail;
  project UUID preserved -> references intact; sidecar `source`
  line rewritten; DB re-sorted + reindexed + view refreshed.
- **Delete** (`led_project_delete`): ref-check first — direct DB
  dep edges + runtime-ID hex text scan of scene/prefab sources
  (chunked 64KB window + 32B overlap for boundary hexes).
  Referenced -> `LED_ERROR_VALIDATION` + dependency listing in
  the console. No force-delete in Phase 32. Unreferenced ->
  source + sidecar removed, record dropped (order kept),
  reindex + refresh.
- Symlink policy: directory symlinks never descended (lstat
  gate; MSVC backend never follows); file symlinks hash as
  target bytes (documented).
- Case collisions: records are unique by normalized rel; the
  duplicate-UUID conflict check surfaces aliasing instead of
  silently picking.
