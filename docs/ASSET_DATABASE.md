# Asset Database (Phase 32)

The project asset database is a **GUI-independent model** over
sidecar metadata. It describes; the engine registry owns.

## Identity stack (four concepts, never conflated)

```
project asset UUID  (led_project_asset_id)
  authoring identity. Counter-seeded UUID minted at discovery,
  stored in <source>.luma. Stable across rename/move (sidecar
  travels), reimport (bytes change, UUID stable), cache delete.
  Two copies of identical bytes get DISTINCT IDs.

content fingerprint ({size, FNV-1a-64(bytes)})
  change detection ONLY. Never identity.

runtime le_asset handle ({index, generation})
  engine registry. Reimport publishes a NEW handle (old one
  detectably stale); project ID unchanged.

le_asset_id persistent ID ({hi, lo})
  what scene/prefab files store. The DB bridges
  project-ID <-> le_asset_id <-> runtime handle.
```

```
Scene/prefab payload          Project DB              Engine registry
 hex le_asset_id  ---bridge--> project UUID +   --->  le_asset handle
                               runtime le_asset_id     (READY resources)
                               + sub-asset table
```

## Records

`{project_id UUID, type, source_path (rel, normalized), status
(UNIMPORTED/READY/STALE/FAILED/MISSING/UNSUPPORTED), fingerprint,
importer id+version, settings digest, sub-asset table (glTF),
deps[] (project UUIDs), diagnostics, runtime handle + runtime ID}`.

## Sidecars, not a central DB (`<source>.luma`)

`LUMA_ASSET 1` + `id/type/source/fingerprint/importer/settings/
dep/sub/runtime` lines. Rationale: rename/move = filesystem move
of source + sidecar with zero DB surgery; copy = new identity
unless the sidecar is preserved (then duplicate-UUID conflict
state); one corrupt sidecar degrades one record, never the
project. Unknown fields tolerated (forward compat); missing ID
or bad magic = per-record diagnostic, scan continues.

## Indexes + enumeration

- Open-addressing UUID hash + path hash (rebuilt after every
  structural change).
- Deterministic path-sorted order for enumeration, search, and
  the browser view (sort modes: path / name / type-then-path).
- Search: case-insensitive substring over basename + full path +
  type name, with optional type filter; counting-query contract
  (always reports the full count).
- Duplicate-UUID conflict: two records claiming one project ID
  are BOTH marked FAILED with diagnostics (never a silent pick).
- Dependency edges (`deps[]`) + reverse lookup (dependents);
  prefab create extracts edges via the runtime-ID bridge.

## Scan policy

Explicit `led_project_scan` only (no watcher). Ignore: `.git`,
`.svn`, `.hg`, `.luma`, `build*`, `cache`, `__pycache__`,
`node_modules`, hidden dotfiles, `*.luma` sidecars,
`luma.project`. Directory symlinks never followed (lstat gate;
MSVC backend lists without following). Depth cap 64, entries
cap 65536 per directory, deterministic (sorted) traversal.
Mark (visited high-bit) + sweep (unvisited -> MISSING, identity
retained for restore). Restored files recover identity via
sidecar (`restored` counter); changed fingerprint/importer/
settings -> STALE with reason.

## Status lattice

`UNIMPORTED -> READY <-> STALE -> READY` (reimport),
`ANY -> MISSING -> UNIMPORTED/STALE` (restore),
`ANY -> FAILED` (import/reimport failure, conflict).
Fresh discovery mints the UUID + writes the sidecar. Live
runtime handles revalidate (dead handle -> UNIMPORTED); runtime
assets survive rescans (lazy policy: scan never uploads GPU).

## Stress + proofs

100k-record stress is a documented gate (hash lookup, sorted
enumeration); the current suites prove determinism, conflict
marking, MISSING/restore, and corrupt-sidecar isolation at
project scale (`test_assetdb`). Portability (copy/move) and
rename/move UUID stability are proven in `test_project`.
