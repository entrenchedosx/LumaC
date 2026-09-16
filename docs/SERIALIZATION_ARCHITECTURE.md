# Serialization Architecture (Phase 25; script lines Phase 26)

Canonical versioned TEXT format (UTF-8, LF). Memory-first
(`le_scene_save_text` / `le_scene_load_text`); file helpers
(`le_scene_save_file` / `le_scene_load_file`) layer above with
explicit paths (no CWD dependence). No_bindings to the filesystem
inside the parser (helps tests, archives, networking, editor, MCP).

## Layout

```
LUMA_SCENE 1
asset mesh|material <32hex> <escaped path hint>
object <32hex>
name "..."              (omitted when empty)
enabled 0|1
parent <32hex>|nil
position x y z
rotation x y z w
scale x y z
renderable <meshhex> <mathex> cast recv visible
camera persp|ortho lens aspect near far
light dir|point|spot r g b intensity range inner outer
shadow 0 | shadow 1 res dbias nbias near far dist
script <scriphex>              (Phase 26: at most one per object)
sprop bool|int <name> <value>  (Phase 26: exported values)
sprop number|string|vec3|asset <name> <value>
end
```

Phase 26 notes: `script` resolves the asset ID to a READY
`LE_ASSET_SCRIPT` handle (unready/missing fails the
instantiate); `sprop` values restore after attach (unknown names
from newer files are skipped, not fatal). Numbers use `%.17g`
(double round-trip); strings reuse the name escaper; asset
values are persistent hex IDs. The lines carry IDs + values
only — never VM state.

## Versioning

`format_version = 1` in the header. Unknown versions →
`LE_ERROR_UNSUPPORTED_VERSION` (future migrations possible).
Unknown COMPONENT kinds (`physics`, …) → `LE_ERROR_PARSE`
(silently dropping a component would corrupt meaning).
(`script` is a KNOWN component since Phase 26; duplicate `script`
lines on one object are rejected.) Unknown
FIELDS (`mood happy`) → tolerated (forward compat). Enums serialize
by stable NAMES (`persp`, `dir`), never raw ordinals.

## Determinism

Records sorted by persistent ID bytes; asset table sorted by hex;
`%.9g` floats (exact float32 round-trip); fixed key order. Save
twice → byte-identical (tested incl. escape-heavy names and
25-case randomized property suites with fixed seeds).

## Floats

`%.9g` write; strict `strtof` full-consumption read; NaN/Inf
spellings REJECTED (records must be finite; lens/range rules add
domain checks at instantiate).

## Escaping

`\" \\ \n \t \r \uXXXX` (writer) with strict `\u` validation
(BMP, no surrogates → UTF-8 encode); malformed escapes,
unterminated quotes, embedded NULs all fail. UTF-8 passes through.

## Security (untrusted input)

Bounded everything: 64 MB text cap, 16M object cap, 4 KB lines,
127-char names, 1 KB paths; counts pre-checked before allocation
(`realloc` growth guarded); duplicate IDs, self/missing parents,
cycles, bad enums, NaN/Inf, broken escapes, unexpected EOF all
fail with precise codes. No `strtok`, no recursion, no hash tables
(determinism + auditability over speed; instantiate duplicate scan
is O(n²) worst-case, O(n) typical).

## Transactionality

Parse stages into temp records; the scene payload swaps in ONLY on
full success (failed loads keep the old payload). Instantiation
validates/resolves everything BEFORE creating objects, and rolls
back partial commits. Malformed scenes never half-corrupt scenes
or worlds (fuzz suite: truncation, versions, counts, IDs, parents,
cycles, quaternions, NaN/Inf, asset IDs, component kinds,
escapes, EOF).
