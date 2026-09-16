/*
 * Script dispatch (Phase 26): snapshot iteration, deterministic
 * order, fixed-step schedule, teardown guards, tracked scene
 * instances for script instance tables, and rebind-on-reload.
 *
 * Policy (documented, tested):
 * - le_world_update(dt) with finite dt > 0 drives: pending starts
 *   (slot order, effectively-enabled only — start delayed, never
 *   skipped), then fixed steps (accumulator, capped), then
 *   update(dt) in slot order. dt <= 0 / NaN / Inf: matrices only,
 *   no callbacks (render_scene's update(0) never runs scripts).
 * - Iteration runs over a SNAPSHOT of (slot, generation) pairs, so
 *   structural mutation inside callbacks (create/destroy/reparent/
 *   attach/remove/disable) is immediately executed AND safe: new
 *   objects start next frame; destroyed objects resolve stale for
 *   the rest of the frame. Deterministic, no queue.
 * - Destroy-other before its turn => it does NOT update (liveness
 *   re-checked per callback). Destroy-self => current callback may
 *   finish; further self ops fail safely.
 * - Nested destroy depth is capped (64) against adversarial
 *   destroy chains.
 * - Errors disable the failing instance (failed=1, no repeat spam);
 *   the world/engine continue. destroy() errors never corrupt
 *   teardown.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "script/script_internal.h"

#define LE_SCRIPT_MAX_NEST 64

/* Tracked scene instances for script instance tables (world-local,
 * serial-keyed; freed with the world). */
typedef struct le_tracked_instance {
    uint32_t serial;
    le_scene_instance inst;
} le_tracked_instance;

typedef struct le_instance_registry {
    le_tracked_instance *items;
    uint32_t count;
    uint32_t capacity;
    uint32_t next_serial;
} le_instance_registry;

/* Registry hangs off... worlds have no spare pointer; keep a
 * process-side list keyed by world pointer with teardown cleanup.
 * Worlds are heap objects; entries removed by le_script_free_world
 * (called from le_world_destroy). */
static le_instance_registry *le_reg_for(le_world *world, int create);

struct le_reg_node {
    le_world *world;
    le_instance_registry reg;
    struct le_reg_node *next;
};

static struct le_reg_node *le_reg_head = NULL;

static le_instance_registry *le_reg_for(le_world *world, int create) {
    struct le_reg_node *n;

    if (world == NULL) {
        return NULL;
    }
    for (n = le_reg_head; n != NULL; n = n->next) {
        if (n->world == world) {
            return &n->reg;
        }
    }
    if (!create) {
        return NULL;
    }
    n = (struct le_reg_node *)calloc(1, sizeof(*n));
    if (n == NULL) {
        return NULL;
    }
    n->world = world;
    n->reg.next_serial = 1;
    n->next = le_reg_head;
    le_reg_head = n;
    return &n->reg;
}

void le_script_free_world(le_world *world) {
    struct le_reg_node **link = &le_reg_head;

    if (world == NULL) {
        return;
    }
    while (*link != NULL) {
        if ((*link)->world == world) {
            struct le_reg_node *dead = *link;
            uint32_t i;

            for (i = 0; i < dead->reg.count; i++) {
                le_scene_instance_free(
                    &dead->reg.items[i].inst);
            }
            free(dead->reg.items);
            *link = dead->next;
            free(dead);
            return;
        }
        link = &(*link)->next;
    }
}

uint32_t le_script_track_instance(le_world *world,
                                  le_scene_instance *inst) {
    le_instance_registry *reg = le_reg_for(world, 1);
    le_tracked_instance *fresh;
    uint32_t grown;

    if (reg == NULL || inst == NULL) {
        return 0;
    }
    if (reg->count >= reg->capacity) {
        grown = (reg->capacity == 0) ? 8u : reg->capacity * 2u;
        fresh = (le_tracked_instance *)realloc(
            reg->items, (size_t)grown * sizeof(*fresh));
        if (fresh == NULL) {
            return 0;
        }
        reg->items = fresh;
        reg->capacity = grown;
    }
    reg->items[reg->count].serial = reg->next_serial++;
    if (reg->next_serial == 0) {
        reg->next_serial = 1;
    }
    reg->items[reg->count].inst = *inst;
    reg->count++;
    return reg->items[reg->count - 1].serial;
}

int le_script_instance_lookup_serial(le_world *world, uint32_t serial,
                                     const char *hex,
                                     le_object *out_obj) {
    le_instance_registry *reg = le_reg_for(world, 0);
    uint32_t i;
    le_asset_id id;

    if (out_obj != NULL) {
        *out_obj = LE_OBJECT_INVALID;
    }
    if (reg == NULL || hex == NULL) {
        return 0;
    }
    if (!le_asset_id_from_string(hex, &id)) {
        /* Scene object IDs print as 32 hex too; reuse the asset
         * hex parser (same 128-bit layout). */
        return 0;
    }
    for (i = 0; i < reg->count; i++) {
        if (reg->items[i].serial == serial) {
            le_scene_instance *inst = &reg->items[i].inst;
            uint32_t k;

            for (k = 0; k < inst->count; k++) {
                /* Compare via the instance's stored IDs: the
                 * le_scene_object_id layout matches le_asset_id. */
                le_asset_id *cand =
                    (le_asset_id *)&inst->object_ids[k];

                if (cand->hi == id.hi && cand->lo == id.lo) {
                    /* Liveness re-check (objects may have died
                     * since instantiation). */
                    if (le_object_is_alive(world,
                                           &inst->objects[k])) {
                        if (out_obj != NULL) {
                            *out_obj = inst->objects[k];
                        }
                        return 1;
                    }
                    return 0;
                }
            }
            return 0;
        }
    }
    return 0;
}

uint32_t le_script_instance_count(le_world *world, uint32_t serial) {
    le_instance_registry *reg = le_reg_for(world, 0);
    uint32_t i;

    if (reg == NULL) {
        return 0;
    }
    for (i = 0; i < reg->count; i++) {
        if (reg->items[i].serial == serial) {
            return reg->items[i].inst.count;
        }
    }
    return 0;
}

/* Rebind one live instance to its asset's CURRENT chunk (reload
 * path): re-fetch funcs, keep state values + exports. Implemented
 * via backend re-instantiate into a FRESH state table seeded from
 * the old values? Simpler honest semantics: reload re-runs the
 * chunk to rebuild funcs while PRESERVING the values table. The
 * script backend supports this by re-running instantiate with a
 * preserved values table. */
void le_script_rebind_instance(le_world *world,
                               le_script_entry *entry) {
    le_script_runtime *rt;

    if (world == NULL || entry == NULL ||
        world->engine == NULL) {
        return;
    }
    rt = world->engine->script_runtime;
    if (rt == NULL || rt->backend == NULL) {
        return;
    }
    /* Backend-agnostic rebind: release instance state, then
     * re-instantiate (values preserved by the backend: the script
     * backend copies old values over the fresh export defaults).
     * Only for started, healthy instances; pending/failed ones
     * pick the new code up naturally on next start. */
    if (!entry->started || entry->failed) {
        return;
    }
    /* Implemented inside script_lua.c (needs the backend): re-runs
     * the chunk with preserved values (no start() rerun). */
    le_lua_rebind(rt, world, entry);
}

/* Snapshot one world's script dispatch list (slot order). The
 * snapshot COPIES slot+generation+asset (never raw entry pointers):
 * dispatch revalidates every row against live storage (entries move
 * via swap-remove under nested dispatch). */
static le_script_entry *le_snapshot_scripts(le_world *world,
                                            uint32_t *out_n) {
    le_script_entry *snap = NULL;

    if (out_n != NULL) {
        *out_n = 0;
    }
    if (world == NULL || world->script_count == 0) {
        return NULL;
    }
    snap = (le_script_entry *)malloc(world->script_count *
                                     sizeof(*snap));
    if (snap == NULL) {
        return NULL;
    }
    memcpy(snap, world->scripts,
           world->script_count * sizeof(*snap));
    /* Deterministic slot order (dense order churns on remove). */
    {
        uint32_t i;
        uint32_t j;

        for (i = 0; i < world->script_count; i++) {
            for (j = i + 1u; j < world->script_count; j++) {
                if (snap[j].slot < snap[i].slot) {
                    le_script_entry t = snap[i];

                    snap[i] = snap[j];
                    snap[j] = t;
                }
            }
        }
    }
    if (out_n != NULL) {
        *out_n = world->script_count;
    }
    return snap;
}

/* Find the LIVE entry for a snapshot row (validates slot +
 * generation drift: entry may have moved via swap-remove). */
static le_script_entry *le_find_live_entry(le_world *world,
                                           uint32_t slot) {
    uint32_t i;

    if (world == NULL || slot >= world->capacity) {
        return NULL;
    }
    if (!(world->slots[slot].alive) ||
        !(world->slots[slot].present & LE_PRESENT_SCRIPT)) {
        return NULL;
    }
    i = (uint32_t)world->slots[slot].script_index;
    if (i >= world->script_count) {
        return NULL;
    }
    if (world->scripts[i].slot != slot) {
        return NULL;
    }
    return &world->scripts[i];
}

static int le_entry_effective(le_world *world,
                              le_script_entry *entry) {
    le_object h;

    if (world == NULL || entry == NULL) {
        return 0;
    }
    h.index = entry->slot;
    h.generation = world->slots[entry->slot].generation;
    h.world_tag = world->tag;
    return le_object_is_effectively_enabled(world, &h);
}

/* Phase 28 fixed-step physics driver for worlds WITHOUT script
 * dispatch (no runtime, or zero scripts). Runs the SAME schedule
 * as the PASS2 loop inline in le_script_step_world (accumulator
 * + capped catch-up + backlog drop on the shared script_fixed_dt
 * / script_max_steps / script_accum fields — one schedule, two
 * entry points, never double-stepped: callers take exactly one
 * path per frame). No fixed_update callbacks exist here by
 * construction; each interval is a straight physics sub-step.
 * A world that never opted into a fixed dt still simulates at
 * the default 60 Hz physics rate (physics always runs; only
 * fixed_update SCRIPT callbacks are opt-in). */
void le_script_step_physics(le_world *world, float dt) {
    double cap;
    uint32_t steps = 0;
    float fixed_dt;
    uint32_t max_steps;

    if (world == NULL) {
        return;
    }
    if (!(dt == dt) || dt <= 0.0f) {
        return;
    }
    fixed_dt = world->script_fixed_dt;
    max_steps = world->script_max_steps;
    if (!(fixed_dt > 0.0f)) {
        fixed_dt = 1.0f / 60.0f; /* physics default rate */
    }
    if (max_steps == 0) {
        max_steps = 4u;
    }
    cap = (double)fixed_dt * (double)max_steps * 2.0;
    world->script_accum += (dt > cap) ? cap : (double)dt;
    while (world->script_accum >= (double)fixed_dt &&
           steps < max_steps) {
        world->script_accum -= (double)fixed_dt;
        steps++;
        le_physics_step(world, fixed_dt);
    }
    if (steps == max_steps &&
        world->script_accum >= (double)fixed_dt) {
        world->script_accum = 0.0; /* drop backlog */
    }
}

void le_script_step_world(le_world *world, float dt) {
    le_engine *engine;
    le_script_runtime *rt;
    uint32_t n = 0;
    le_script_entry *snap = NULL;
    uint32_t i;

    if (world == NULL) {
        return;
    }
    engine = world->engine;
    if (engine == NULL) {
        /* Engine-less worlds never exist in practice, but keep
         * physics alive anyway (script dispatch needs the
         * runtime, physics does not). */
        le_script_step_physics(world, dt);
        return;
    }
    if (engine->script_runtime == NULL ||
        world->script_count == 0) {
        /* Phase 28: scriptless (or runtime-less) worlds still
         * simulate physics on the fixed schedule. The physics
         * accumulator is independent of script presence. */
        le_script_step_physics(world, dt);
        return;
    }
    rt = engine->script_runtime;
    if (world->scripts_firing >= LE_SCRIPT_MAX_NEST) {
        return;
    }
    if (world->scripts == NULL) {
        return;
    }
    if (rt->backend == NULL || rt->backend->instantiate == NULL ||
        rt->backend->fire == NULL) {
        return;
    }
    rt->callbacks_this_frame = 0;
    snap = le_snapshot_scripts(world, &n);
    if (snap == NULL || n == 0) {
        /* No instances (script_count was 0) — or OOM. Either way
         * nothing to dispatch; OOM simply skips this frame. */
        free(snap);
        return;
    }
    if (n != world->script_count || world->script_count == 0) {
        /* Paranoia: snapshot drift means storage moved under us
         * (should be impossible — single-threaded dispatch). */
        free(snap);
        return;
    }
    world->scripts_firing++;
    /* PASS 1: pending starts (effectively-enabled only; disabled
     * instances stay pending — start delayed until activation).
     * NOTE: `e` is re-resolved AFTER instantiate (backend calls may
     * grow the scripts array via nested World.create+add_script;
     * realloc would dangle the pre-call pointer). */
    for (i = 0; i < n; i++) {
        le_script_entry *e = NULL;
        uint32_t snapslot = 0;
        int is_eff = 0;
        int irc = 0;
        int frc = 0;

        if (snap == NULL || i >= n) {
            break;
        }
        snapslot = snap[i].slot;
        if (snapslot >= world->capacity) {
            continue;
        }
        e = le_find_live_entry(world, snapslot);

        if (e == NULL || !e->pending_start || e->failed ||
            e->started) {
            continue;
        }
        is_eff = le_entry_effective(world, e);
        if (!is_eff) {
            continue;
        }
        /* Instantiate backend state on first start. The backend
         * balances the stack exactly (entry depth on entry AND
         * exit). Re-resolve `e` after the call: nested dispatch
         * (World.create + add_script from start()) may realloc
         * the scripts array and dangle the pointer. */
        if (e->state_ref == LE_SCRIPT_NOREF) {
            irc = rt->backend->instantiate(rt, world, e);
            e = le_find_live_entry(world, snapslot);
            if (e == NULL) {
                continue;
            }
            if (irc == 0 && e->state_ref == LE_SCRIPT_NOREF) {
                /* Backend reported success but left no state
                 * (must never happen) — fail loudly. */
                irc = -1;
            }
            if (irc != 0) {
                e->failed = 1;
                e->pending_start = 0;
                continue;
            }
        }
        frc = rt->backend->fire(rt, world, e, 0, 0.0f);
        e = le_find_live_entry(world, snapslot);
        if (e == NULL) {
            continue;
        }
        if (frc != 0) {
            e->failed = 1;
            e->pending_start = 0;
            continue;
        }
        e->started = 1;
        e->pending_start = 0;
    }
    /* PASS 2: fixed steps (accumulator, capped catch-up). One
     * physics sub-step runs per fixed interval AFTER the
     * fixed_update script fires for that interval (scripts apply
     * forces/impulses first) — documented order, no second
     * timer. Scriptless worlds take the same path via
     * le_script_step_physics (called above); this loop handles
     * scripted worlds inline so fixed_update and physics stay
     * in lockstep per interval. */
    if (world->script_fixed_dt > 0.0f) {
        double cap =
            (double)world->script_fixed_dt *
            (double)world->script_max_steps * 2.0;

        world->script_accum += (dt > cap) ? cap : (double)dt;
        {
            uint32_t steps = 0;

            while (world->script_accum >=
                       (double)world->script_fixed_dt &&
                   steps < world->script_max_steps) {
                uint32_t k;

                world->script_accum -=
                    (double)world->script_fixed_dt;
                steps++;
                for (k = 0; k < n; k++) {
                    le_script_entry *e = le_find_live_entry(
                        world, snap[k].slot);

                    if (e == NULL || !e->started || e->failed) {
                        continue;
                    }
                    if (!le_entry_effective(world, e)) {
                        continue;
                    }
                    if (rt->backend->fire(rt, world, e, 2,
                                          world->script_fixed_dt) !=
                        0) {
                        e->failed = 1;
                    }
                }
                le_physics_step(world, world->script_fixed_dt);
            }
            if (steps == world->script_max_steps &&
                world->script_accum >=
                    (double)world->script_fixed_dt) {
                world->script_accum = 0.0; /* drop backlog */
            }
        }
    }
    /* PASS 3: update(dt) in slot order. */
    for (i = 0; i < n; i++) {
        le_script_entry *e = NULL;
        uint32_t uslot = 0;
        int ueff = 0;
        int ufrc = 0;

        if (snap == NULL || i >= n) {
            break;
        }
        uslot = snap[i].slot;
        if (uslot >= world->capacity) {
            continue;
        }
        e = le_find_live_entry(world, uslot);
        if (e == NULL || !e->started || e->failed) {
            continue;
        }
        ueff = le_entry_effective(world, e);
        if (!ueff) {
            continue;
        }
        /* Re-resolve after fire (nested dispatch may realloc). */
        ufrc = rt->backend->fire(rt, world, e, 1, dt);
        e = le_find_live_entry(world, uslot);
        if (e == NULL) {
            continue;
        }
        if (ufrc != 0) {
            e->failed = 1;
        }
    }
    world->scripts_firing--;
    free(snap);
}

void le_script_fire_slot_destroy(le_world *world, uint32_t slot) {
    le_script_entry *e;
    le_engine *engine;
    le_script_runtime *rt;

    if (world == NULL || slot >= world->capacity) {
        return;
    }
    if (!(world->slots[slot].present & LE_PRESENT_SCRIPT)) {
        return;
    }
    e = le_find_live_entry(world, slot);
    if (e == NULL || !e->started) {
        return;
    }
    /* started flag clears FIRST: reentrant destroy-self is a
     * no-op instead of infinite recursion. */
    e->started = 0;
    engine = world->engine;
    if (engine == NULL || engine->script_runtime == NULL) {
        return;
    }
    rt = engine->script_runtime;
    if (world->scripts_firing >= LE_SCRIPT_MAX_NEST) {
        return;
    }
    world->scripts_firing++;
    if (rt->backend->fire(rt, world, e, 3, 0.0f) != 0) {
        e->failed = 1;
    }
    world->scripts_firing--;
}

void le_script_fire_world_destroy(le_world *world) {
    uint32_t n = 0;
    le_script_entry *snap = NULL;
    uint32_t i;

    if (world == NULL || world->script_count == 0) {
        if (world != NULL) {
            world->scripts_tearing_down = 1;
        }
        return;
    }
    world->scripts_tearing_down = 1;
    snap = le_snapshot_scripts(world, &n);
    /* Teardown order: reverse slot order (children usually sit
     * above parents; deterministic either way). */
    for (i = n; i-- > 0;) {
        le_script_fire_slot_destroy(world, snap[i].slot);
    }
    free(snap);
}

void le_script_release_entry(le_world *world, le_script_entry *entry) {
    le_engine *engine;

    if (world == NULL || entry == NULL) {
        return;
    }
    engine = world->engine;
    if (engine == NULL || engine->script_runtime == NULL) {
        entry->state_ref = LE_SCRIPT_NOREF;
        return;
    }
    engine->script_runtime->backend->release_instance(
        engine->script_runtime, world, entry);
    entry->state_ref = LE_SCRIPT_NOREF;
}

/* --- VM-independent property C API (delegates to backend) --- */

static le_script_entry *le_prop_entry(le_world *world,
                                      const le_object *object,
                                      le_result *code) {
    uint32_t slot;

    if (world == NULL || object == NULL) {
        if (code != NULL) {
            *code = LE_ERROR_INVALID_ARGUMENT;
        }
        return NULL;
    }
    if (!le_resolve_live(world, object, &slot, code)) {
        return NULL;
    }
    if (!(world->slots[slot].present & LE_PRESENT_SCRIPT)) {
        if (code != NULL) {
            *code = LE_ERROR_MISSING_COMPONENT;
        }
        return NULL;
    }
    {
        uint32_t idx = (uint32_t)world->slots[slot].script_index;

        if (idx >= world->script_count ||
            world->scripts[idx].slot != slot) {
            if (code != NULL) {
                *code = LE_ERROR_INVALID_ARGUMENT;
            }
            return NULL;
        }
        if (code != NULL) {
            *code = LE_SUCCESS;
        }
        return &world->scripts[idx];
    }
}

int le_script_get_property(le_world *world, const le_object *object,
                           const char *name,
                           le_script_property *out_prop) {
    le_result code = LE_SUCCESS;
    le_script_entry *e;
    /* The lookup name may alias out_prop->name (callers pass a
     * field of their output struct as the key). Copy it aside
     * FIRST so the memset below cannot wipe the key. */
    char key[64];

    memset(key, 0, sizeof(key));
    if (name != NULL) {
        size_t n = strlen(name);

        if (n >= sizeof(key)) {
            n = sizeof(key) - 1u;
        }
        memcpy(key, name, n);
    }
    if (out_prop != NULL) {
        memset(out_prop, 0, sizeof(*out_prop));
    }
    if (key[0] == '\0' || out_prop == NULL) {
        return 0;
    }
    e = le_prop_entry(world, object, &code);
    if (e == NULL || world->engine == NULL ||
        world->engine->script_runtime == NULL) {
        return 0;
    }
    snprintf(out_prop->name, sizeof(out_prop->name), "%s", key);
    /* Discover type via list, then typed read. */
    {
        le_script_property all[16];
        uint32_t count = 0;
        uint32_t i;

        if (!world->engine->script_runtime->backend->list_props(
                world->engine->script_runtime, world, e, all, 16,
                &count)) {
            return 0;
        }
        for (i = 0; i < count; i++) {
            if (strcmp(all[i].name, key) == 0) {
                out_prop->type = all[i].type;
                return world->engine->script_runtime->backend
                    ->get_prop(world->engine->script_runtime,
                               world, e, out_prop);
            }
        }
    }
    return 0;
}

le_result le_script_set_property(le_world *world,
                                 const le_object *object,
                                 const le_script_property *prop) {
    le_result code = LE_SUCCESS;
    le_script_entry *e;

    if (world == NULL || object == NULL || prop == NULL ||
        prop->name[0] == '\0') {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    e = le_prop_entry(world, object, &code);
    if (e == NULL) {
        return (code == LE_ERROR_MISSING_COMPONENT)
                   ? LE_ERROR_INVALID_ARGUMENT
                   : code;
    }
    if (world->engine == NULL ||
        world->engine->script_runtime == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (prop->type < LE_SCRIPT_PROP_BOOL ||
        prop->type > LE_SCRIPT_PROP_ASSET) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (prop->type == LE_SCRIPT_PROP_ASSET) {
        uint32_t aslot;
        le_result ac = LE_SUCCESS;

        if (!le_resolve_asset_live(world->engine, &prop->asset,
                                   &aslot, &ac)) {
            return LE_ERROR_STALE_ASSET;
        }
    }
    if (world->engine->script_runtime->backend->set_prop(
            world->engine->script_runtime, world, e, prop) != 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    return LE_SUCCESS;
}

int le_script_list_properties(le_world *world, const le_object *object,
                              le_script_property *out_props,
                              uint32_t capacity,
                              uint32_t *out_count) {
    le_result code = LE_SUCCESS;
    le_script_entry *e;

    if (out_count != NULL) {
        *out_count = 0;
    }
    if (world == NULL || object == NULL) {
        return 0;
    }
    e = le_prop_entry(world, object, &code);
    if (e == NULL || world->engine == NULL ||
        world->engine->script_runtime == NULL) {
        return 0;
    }
    return world->engine->script_runtime->backend->list_props(
        world->engine->script_runtime, world, e, out_props,
        capacity, out_count);
}

/* --- scene capture/apply for script records --- */

void le_script_capture_for_record(le_world *world, uint32_t slot,
                                  le_scene_object *rec) {
    le_script_entry *e;
    uint32_t idx;

    if (world == NULL || rec == NULL || slot >= world->capacity) {
        return;
    }
    rec->has_script = 0;
    rec->script_prop_count = 0;
    if (!(world->slots[slot].present & LE_PRESENT_SCRIPT)) {
        return;
    }
    idx = (uint32_t)world->slots[slot].script_index;
    if (idx >= world->script_count ||
        world->scripts[idx].slot != slot) {
        return;
    }
    e = &world->scripts[idx];
    if (world->engine == NULL) {
        return;
    }
    {
        uint32_t aslot;
        le_result code = LE_SUCCESS;

        if (!le_resolve_asset_live(world->engine, &e->asset,
                                   &aslot, &code)) {
            return;
        }
        rec->has_script = 1;
        rec->script_id = world->engine->assets[aslot].id;
    }
    /* Exported values (up to LE_SCRIPT_MAX_PROPS). */
    if (world->engine->script_runtime != NULL &&
        e->state_ref != LE_SCRIPT_NOREF) {
        le_script_property all[16];
        uint32_t count = 0;
        uint32_t i;

        if (world->engine->script_runtime->backend->list_props(
                world->engine->script_runtime, world, e, all, 16,
                &count)) {
            for (i = 0; i < count &&
                        i < LE_SCRIPT_MAX_PROPS &&
                        i < 16u;
                 i++) {
                rec->script_props[rec->script_prop_count++] =
                    all[i];
            }
        }
    }
}

le_result le_script_apply_record(le_world *world, const le_object *obj,
                                 const le_scene_object *rec) {
    le_engine *engine;
    le_asset script = LE_ASSET_INVALID;
    uint32_t i;
    le_result rc;

    if (world == NULL || obj == NULL || rec == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!rec->has_script) {
        return LE_SUCCESS;
    }
    engine = world->engine;
    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_asset_find_by_id(engine, &rec->script_id, &script)) {
        return LE_ERROR_MISSING_ASSET;
    }
    rc = le_object_add_script(world, obj, &script);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    /* Property values restore AFTER attach (instance state exists
     * only after instantiate-on-start... state is created at
     * instantiate() during START. Values must wait: stash them as
     * pending overrides applied at start. Simplest honest path:
     * apply at start via the record? The entry doesn't retain the
     * record. Alternative: force-instantiate backend state NOW so
     * set_prop works, keeping pending_start for start(). */
    {
        uint32_t slot;
        le_result code = LE_SUCCESS;
        le_script_entry *e;

        if (!le_resolve_live(world, obj, &slot, &code)) {
            return code;
        }
        e = le_find_live_entry(world, slot);
        if (e == NULL) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (e->state_ref == LE_SCRIPT_NOREF &&
            engine->script_runtime != NULL) {
            if (engine->script_runtime->backend->instantiate(
                    engine->script_runtime, world, e) != 0) {
                le_object_remove_script(world, obj);
                return LE_ERROR_RENDERER;
            }
        }
        for (i = 0; i < rec->script_prop_count; i++) {
            /* Best-effort per property (unknown names from newer
             * files are SKIPPED, not fatal — forward compat). */
            if (engine->script_runtime != NULL) {
                engine->script_runtime->backend->set_prop(
                    engine->script_runtime, world, e,
                    &rec->script_props[i]);
            }
        }
    }
    return LE_SUCCESS;
}
