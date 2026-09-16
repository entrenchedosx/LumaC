# Object Identity Architecture (Phase 24)

Engine objects are **generational handles**, never pointers,
indices, submission positions, or GPU slots:

```c
typedef struct le_object {
    uint32_t index;       // world slot
    uint32_t generation;  // slot lifetime
    uint32_t world_tag;   // owning world
} le_object;
```

Field order is stable ABI. `LE_OBJECT_INVALID`
(`{0xFFFFFFFF, 0, 0}`) never names a live object; `{0,0,0}` is
equally invalid (generation 0 and tag 0 never validate).

## Slot lifecycle

- Creation pops the free-list head (O(1), no scan), bumps 0 → 1 on
  first use (0 never validates), and returns
  `{slot, generation, world_tag}`.
- Destruction retires the slot: generation bumps (wrapping 0 → 1,
  never 0), slot returns to the free-list. 32-bit generations admit
  4 billion reuses before an ancient handle could alias — and only
  with a simultaneous live-slot + tag collision.
- A handle is live iff its tag matches the world named in the call
  AND its slot is occupied AND generations match. Every operation
  through a stale handle fails with `LE_ERROR_STALE_HANDLE` and
  never addresses a different live object.
- Storage grows geometrically (×2 from `max(64, initial_capacity)`,
  capped at 16M slots, overflow-checked); every growth path
  allocates first and swaps in on full success (world unchanged on
  OOM).

## Cross-world safety

Tags come from a process-wide counter (never 0; 0 means "no
world"). A handle from world A presented to world B fails with
`LE_ERROR_WRONG_WORLD` — even when B holds a live object at the
same `{index, generation}` (likely: both worlds allocate slot 0
first). Tags make confusion structurally impossible, not
probabilistically unlikely. Salt+tag also decorrelate renderer keys
across worlds (below).

## Component-pointer rule

Dense per-type arrays (`renderables[]`, `cameras[]`, `lights[]`)
with slot↔entry maps relocate on growth (swap-remove on delete),
so the API **never exposes component addresses**: access is always
`(world, object) → values`. Public identity never depends on
component storage staying put — required for future Lua and editor
tooling.

## Stable renderer temporal identity (the Phase 24 audit debt)

Renderer temporal systems (LOD hysteresis today; motion vectors,
TAA, upscaling, animation history tomorrow) must key on **stable
object identity, never submission position**. The engine packs each
live handle into a 64-bit key:

```
stable_id = mix(world_salt, world_tag, index, generation)
```

- Submitted as `lr_draw_item.instance_id`; the visibility shader
  keys LOD hysteresis by it (per-slot owner tags; a slot reused by
  a different key restarts UNKNOWN instead of inheriting history).
- Slot reuse bumps the generation ⇒ the key changes by
  construction ⇒ **no separate retirement table**: a successor
  never inherits a predecessor's history.
- Destroyed handles report key 0 ("no stable identity": the
  renderer runs that submission without hysteresis — deterministic,
  always safe).
- `0` as `instance_id` keeps the metric path for renderer-direct
  users; the engine always submits real keys.

Canonical proofs: `test_engine_vulkan` PART 7 (reorder keeps totals
+ LOD mix; destroy/reuse retires keys), `test_lod_vulkan`
hysteresis oracle (keyed history accumulates across frames).
