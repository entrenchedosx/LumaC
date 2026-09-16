# Asset Reimport (Phase 32)

Transactional reimport = **candidate-then-swap**:

```
old usable asset
     |
     +----> import candidate (fresh handles)
     |           |
     |        validate
     |        /      \
     |     fail    success
     |      |         |
     |      v         v
     +-- keep old  atomic replace (project ID stable,
                       new runtime handle published)
```

## Staleness

Fingerprint change OR importer-version change OR settings-digest
change -> STALE (with reason: "source/importer/settings
changed"). Unchanged reimport is a no-op success. MISSING
sources are not stale (scan owns MISSING).

## Paths

- **Scripts**: handle-preserving first (`le_script_asset_set_source`
  + `le_script_reload` — PARSE keeps old code live, instances
  adopt code and keep values, no re-start). Failure records
  FAILED diagnostics with last-known-good live.
- **Mesh/material/texture + discovery types**: generic path via
  `led_import_one_record` (candidate import), rollback to the
  snapshotted good state on failure, old-handle retire
  (`le_asset_unload`, `IN_USE`-safe) on success. New-handle
  policy: the old handle is detectably stale afterwards.
- **Reimport-all**: every STALE record, transactional per
  record; reports the fixed count; one failure never blocks the
  rest.

## Guarantees (proven in `test_import`)

- Broken source -> FAILED status + diagnostics + old handle
  still alive and resolvable; project UUID unchanged.
- Repair + forced reimport recovers to READY.
- Touched-but-valid source -> STALE -> reimport-all -> READY.
- Dependent invalidation: prefab/scene records carry dep edges;
  delete ref-checks scan them (see `PROJECT_PATHS.md` for the
  filesystem side).
- Reimport while playing is rejected (`ALREADY_PLAYING`).
