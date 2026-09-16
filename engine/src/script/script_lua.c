/*
 * Lua backend #1 (Phase 26): userdata representations, chunk
 * compile/cache, instance state, export() metadata, lifecycle
 * dispatch, and the VM-independent property bridge.
 *
 * Identity on the Lua side is ALWAYS engine identity:
 * - object userdata: {engine ptr, world_tag, index, generation}
 * - asset userdata: {engine ptr, index, generation}
 * - scene userdata: {engine ptr, index, generation}
 * - instance userdata: {engine ptr, world tag, 즐겨찾기…} — no:
 *   {engine ptr, world_tag, scene asset handle, instance serial}.
 *   Instances are looked up through the world's live instance
 *   list; a serial guards use-after-free of the record.
 *
 * Equality: same kind + same engine + same index + same generation
 * (+ same world tag for objects) => equal. Slot reuse bumps the
 * generation, so a recreated object NEVER equals the old one.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "script/script_internal.h"

/* Forward declaration: instantiate is defined below the ops table
 * but used by le_lua_rebind above it. */
static int le_lua_instantiate(struct le_script_runtime *rt,
                              le_world *world, le_script_entry *entry);

/* Callback names (file scope so both instantiate-harvest and the
 * dispatcher share one table). */
static const char *const le_lua_callback_names[] = {
    "start", "update", "fixed_update", "destroy",
};

void le_lua_push_object(lua_State *L, le_world *world,
                        const le_object *obj) {
    le_lua_object *u;

    u = (le_lua_object *)lua_newuserdatauv(L, sizeof(*u), 0);
    u->engine = (world != NULL) ? world->engine : NULL;
    u->world_tag = (obj != NULL) ? obj->world_tag : 0;
    u->index = (obj != NULL) ? obj->index : LE_OBJECT_INDEX_INVALID;
    u->generation = (obj != NULL) ? obj->generation : 0;
    luaL_getmetatable(L, LE_LUA_OBJECT_MT);
    lua_setmetatable(L, -2);
}

/* Resolve userdata to a LIVE (world, slot). Returns 1 live with
 * *out_world/*out_slot set; 0 dead (out_result-style soft fail for
 * bindings, which raise script errors instead of crashing). */
static int le_resolve_lua_object(le_lua_object *u, le_world **out_world,
                                 uint32_t *out_slot) {
    le_world *w;
    le_object h;
    le_result code = LE_SUCCESS;
    uint32_t slot = 0;

    if (u == NULL || u->engine == NULL) {
        return 0;
    }
    for (w = u->engine->worlds; w != NULL; w = w->next) {
        if (w->tag == u->world_tag) {
            break;
        }
    }
    if (w == NULL) {
        return 0;
    }
    h.index = u->index;
    h.generation = u->generation;
    h.world_tag = u->world_tag;
    if (!le_resolve_live(w, &h, &slot, &code)) {
        return 0;
    }
    if (out_world != NULL) {
        *out_world = w;
    }
    if (out_slot != NULL) {
        *out_slot = slot;
    }
    return 1;
}

int le_lua_check_object(lua_State *L, int idx, le_world **out_world,
                        le_object *out_obj) {
    le_lua_object *u =
        (le_lua_object *)luaL_checkudata(L, idx, LE_LUA_OBJECT_MT);

    if (out_world != NULL) {
        *out_world = NULL;
    }
    if (out_obj != NULL) {
        *out_obj = LE_OBJECT_INVALID;
    }
    if (u == NULL) {
        return 0;
    }
    {
        le_world *w = NULL;
        uint32_t slot = 0;

        if (!le_resolve_lua_object(u, &w, &slot)) {
            return 0;
        }
        if (out_world != NULL) {
            *out_world = w;
        }
        if (out_obj != NULL) {
            out_obj->index = u->index;
            out_obj->generation = u->generation;
            out_obj->world_tag = u->world_tag;
        }
        return 1;
    }
}

void le_lua_push_asset(lua_State *L, le_engine *engine,
                       const le_asset *asset) {
    le_lua_asset *u;

    u = (le_lua_asset *)lua_newuserdatauv(L, sizeof(*u), 0);
    u->engine = engine;
    u->index = (asset != NULL) ? asset->index : LE_ASSET_INDEX_INVALID;
    u->generation = (asset != NULL) ? asset->generation : 0;
    luaL_getmetatable(L, LE_LUA_ASSET_MT);
    lua_setmetatable(L, -2);
}

int le_lua_check_asset(lua_State *L, int idx, le_engine **out_engine,
                       le_asset *out_asset) {
    le_lua_asset *u =
        (le_lua_asset *)luaL_checkudata(L, idx, LE_LUA_ASSET_MT);
    le_result code = LE_SUCCESS;
    uint32_t slot = 0;

    if (out_engine != NULL) {
        *out_engine = NULL;
    }
    if (out_asset != NULL) {
        *out_asset = LE_ASSET_INVALID;
    }
    if (u == NULL || u->engine == NULL) {
        return 0;
    }
    {
        le_asset h;

        h.index = u->index;
        h.generation = u->generation;
        if (!le_resolve_asset_live(u->engine, &h, &slot, &code)) {
            return 0;
        }
        if (out_engine != NULL) {
            *out_engine = u->engine;
        }
        if (out_asset != NULL) {
            *out_asset = h;
        }
        return 1;
    }
}

void le_lua_push_scene(lua_State *L, le_engine *engine,
                       const le_asset *scene) {
    le_lua_scene *u;

    u = (le_lua_scene *)lua_newuserdatauv(L, sizeof(*u), 0);
    u->engine = engine;
    u->index = (scene != NULL) ? scene->index : LE_ASSET_INDEX_INVALID;
    u->generation = (scene != NULL) ? scene->generation : 0;
    luaL_getmetatable(L, LE_LUA_SCENE_MT);
    lua_setmetatable(L, -2);
}

int le_lua_check_scene(lua_State *L, int idx, le_engine **out_engine,
                       le_asset *out_scene) {
    /* Scenes ride the asset registry: same validation as assets,
     * plus a type check by the caller. */
    return le_lua_check_asset(L, idx, out_engine, out_scene);
}

/* Scene-instance values cross into Lua as plain tracked tables
 * ({serial, world_tag} + le_scene_instance metatable; see
 * script_bind_world.c). No userdata form exists. */

/* ------------------------------------------------------------------
 * Property conversion (VM-independent le_script_property bridge).
 * ------------------------------------------------------------------ */

int le_script_prop_from_lua(lua_State *L, int idx, const char *name,
                            le_script_property *out) {
    int t;

    if (L == NULL || name == NULL || out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    t = lua_type(L, idx);
    if (t == LUA_TBOOLEAN) {
        out->type = LE_SCRIPT_PROP_BOOL;
        out->boolean = lua_toboolean(L, idx) ? 1 : 0;
        return 1;
    }
    if (t == LUA_TNUMBER) {
        if (lua_isinteger(L, idx)) {
            out->type = LE_SCRIPT_PROP_INT;
            out->integer = (int64_t)lua_tointeger(L, idx);
        } else {
            out->type = LE_SCRIPT_PROP_NUMBER;
            out->number = (double)lua_tonumber(L, idx);
            if (!(out->number == out->number)) {
                return 0;
            }
        }
        return 1;
    }
    if (t == LUA_TSTRING) {
        size_t n = 0;
        const char *s = lua_tolstring(L, idx, &n);

        out->type = LE_SCRIPT_PROP_STRING;
        if (n >= sizeof(out->string_value)) {
            n = sizeof(out->string_value) - 1;
        }
        memcpy(out->string_value, s, n);
        out->string_value[n] = '\0';
        return 1;
    }
    if (t == LUA_TTABLE) {
        /* vec3 table {x,y,z} or {1,2,3}. idx may be negative
         * (export() passes arg 2 — positive; keep it robust for
         * both by absolutizing). */
        float v[3];
        int ok = 1;
        int k;
        int absidx = (idx > 0) ? idx : (lua_gettop(L) + idx + 1);

        for (k = 0; k < 3; k++) {
            lua_geti(L, absidx, (lua_Integer)(k + 1));
            if (!lua_isnumber(L, -1)) {
                ok = 0;
            } else {
                v[k] = (float)lua_tonumber(L, -1);
                if (!(v[k] == v[k])) {
                    ok = 0;
                }
            }
            lua_pop(L, 1);
            if (!ok) {
                break;
            }
        }
        if (ok) {
            out->type = LE_SCRIPT_PROP_VEC3;
            memcpy(out->vec3, v, sizeof(v));
            return 1;
        }
        return 0;
    }
    if (t == LUA_TUSERDATA) {
        /* Asset userdata converts to an asset property. */
        le_engine *e = NULL;
        le_asset h = LE_ASSET_INVALID;

        /* Probe without raising: raw metatable compare. */
        if (lua_getmetatable(L, idx)) {
            luaL_getmetatable(L, LE_LUA_ASSET_MT);
            if (lua_rawequal(L, -1, -2)) {
                lua_pop(L, 2);
                if (le_lua_check_asset(L, idx, &e, &h)) {
                    (void)e;
                    out->type = LE_SCRIPT_PROP_ASSET;
                    out->asset = h;
                    return 1;
                }
                return 0;
            }
            lua_pop(L, 2);
        }
        return 0;
    }
    return 0;
}

void le_script_prop_to_lua(lua_State *L,
                           const le_script_property *prop) {
    if (L == NULL || prop == NULL) {
        if (L != NULL) {
            lua_pushnil(L);
        }
        return;
    }
    switch (prop->type) {
    case LE_SCRIPT_PROP_BOOL:
        lua_pushboolean(L, prop->boolean ? 1 : 0);
        break;
    case LE_SCRIPT_PROP_INT:
        lua_pushinteger(L, (lua_Integer)prop->integer);
        break;
    case LE_SCRIPT_PROP_NUMBER:
        lua_pushnumber(L, (lua_Number)prop->number);
        break;
    case LE_SCRIPT_PROP_STRING:
        lua_pushstring(L, prop->string_value);
        break;
    case LE_SCRIPT_PROP_VEC3:
        lua_newtable(L);
        lua_pushnumber(L, prop->vec3[0]);
        lua_seti(L, -2, 1);
        lua_pushnumber(L, prop->vec3[1]);
        lua_seti(L, -2, 2);
        lua_pushnumber(L, prop->vec3[2]);
        lua_seti(L, -2, 3);
        break;
    case LE_SCRIPT_PROP_ASSET:
        /* Pushed by the caller with engine context (asset needs
         * its engine for the userdata). Fallback: nil. */
        lua_pushnil(L);
        break;
    default:
        lua_pushnil(L);
        break;
    }
}

/* ------------------------------------------------------------------
 * Chunk compile (validates syntax; stores function in registry).
 * Returns registry ref (>= 0) or -1 on failure with *out_error
 * (luaL_ref never returns -1 for a function, so -1 is unambiguous;
 * LE_SCRIPT_NOREF is the parallel "no state" sentinel for
 * entry->state_ref only).
 * ------------------------------------------------------------------ */

static int le_compile_chunk(le_script_runtime *rt, const char *source,
                            size_t size, const char *chunkname,
                            char *out_error, size_t error_cap) {
    int rc;
    int ref = -1;

    if (rt == NULL || source == NULL || size == 0) {
        if (out_error != NULL && error_cap > 0) {
            snprintf(out_error, error_cap, "empty source");
        }
        return -1;
    }
    if (size > LE_SCRIPT_MAX_SOURCE) {
        if (out_error != NULL && error_cap > 0) {
            snprintf(out_error, error_cap, "source too large");
        }
        return -1;
    }
    /* Syntax check under the instruction budget (syntax bombs like
     * deeply nested tables compile in bounded steps... the parser
     * has no hook, but luaL_loadbufferx on 4MB caps the damage;
     * runaway PARSE-time is bounded by input size). */
    rc = luaL_loadbufferx(rt->L, source, size,
                          (chunkname != NULL) ? chunkname : "script",
                          "t");
    if (rc != LUA_OK) {
        const char *msg = lua_tostring(rt->L, -1);

        if (out_error != NULL && error_cap > 0) {
            snprintf(out_error, error_cap, "%s",
                     (msg != NULL) ? msg : "syntax error");
        }
        lua_pop(rt->L, 1);
        return -1;
    }
    ref = luaL_ref(rt->L, LUA_REGISTRYINDEX);
    return ref;
}

/* ------------------------------------------------------------------
 * Instance state: per-entry Lua table {exports={}, values={}}
 * plus a per-instance environment. Chunk runs ONCE per instance
 * (fresh globals view? No — chunks share the engine globals but
 * get a per-instance _ENV for isolation).
 * ------------------------------------------------------------------ */

/* export(name, default): declares VM-independent metadata. Called
 * from chunk top-level during instantiate(). */
static int le_lua_export(lua_State *L) {
    const char *name = luaL_checkstring(L, 1);
    le_script_runtime *rt = le_lua_current_runtime(L);
    size_t n;

    if (rt == NULL) {
        return luaL_error(L, "no script runtime");
    }
    if (name == NULL || name[0] == '\0') {
        return luaL_error(L, "bad export name");
    }
    n = strlen(name);
    if (n >= 64) {
        return luaL_error(L, "export name too long");
    }
    /* Validated identifier? [A-Za-z_][A-Za-z0-9_]* */
    {
        size_t i;

        for (i = 0; i < n; i++) {
            char c = name[i];
            int ok = (c == '_' || (c >= 'A' && c <= 'Z') ||
                      (c >= 'a' && c <= 'z') ||
                      (i > 0 && c >= '0' && c <= '9'));

            if (!ok) {
                return luaL_error(L, "bad export name");
            }
        }
    }
    if (lua_gettop(L) < 2) {
        return luaL_error(L, "export needs a default");
    }
    {
        le_script_property prop;

        if (!le_script_prop_from_lua(L, 2, name, &prop)) {
            return luaL_error(
                L, "unsupported export type (bool/int/number/"
                   "string/vec3/asset only)");
        }
        /* Record into the instantiating entry's export table:
         * registry "le_inst_exports" holds the CURRENT entry's
         * export list during instantiate(). Absolute indices:
         * [exports][record]; the default VALUE is at arg 2 (below
         * the call frame — stable across pushes). */
        lua_getfield(L, LUA_REGISTRYINDEX, "le_inst_exports");
        if (!lua_istable(L, -1)) {
            lua_pop(L, 1);
            return luaL_error(L, "export outside instantiation");
        }
        /* exports[name] = {type=..., default=...} */
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)prop.type);
        lua_setfield(L, -2, "type");
        lua_pushvalue(L, 2);
        lua_setfield(L, -2, "default");
        lua_setfield(L, -2, name); /* pops record, exports[name]= */
        lua_pop(L, 1);             /* exports anchor */
        /* Also seed the instance values table. */
        lua_getfield(L, LUA_REGISTRYINDEX, "le_inst_values");
        if (lua_istable(L, -1)) {
            lua_pushvalue(L, 2);
            lua_setfield(L, -2, name);
        }
        lua_pop(L, 1);
    }
    return 0;
}

/* Rebind a live instance to its asset's current chunk (reload):
 * snapshot values, rebuild state from the new chunk, restore values
 * for still-declared names, keep funcs from the new chunk. start()
 * is NOT re-run. Failure keeps the old state (transactional). */
void le_lua_rebind(le_script_runtime *rt, le_world *world,
                   le_script_entry *entry) {
    lua_State *L;
    le_asset h;
    le_asset_slot *slot = NULL;
    uint32_t aslot = 0;
    le_result code = LE_SUCCESS;
    int old_state;

    if (rt == NULL || world == NULL || entry == NULL) {
        return;
    }
    L = rt->L;
    h = entry->asset;
    if (!le_resolve_asset_live(world->engine, &h, &aslot, &code)) {
        return;
    }
    slot = &world->engine->assets[aslot];
    if (slot->type != LE_ASSET_SCRIPT ||
        slot->script_chunk_ref < 0) {
        return;
    }
    if (entry->state_ref == LE_SCRIPT_NOREF) {
        return;
    }
    old_state = entry->state_ref;
    entry->state_ref = LE_SCRIPT_NOREF;
    /* Snapshot old values (plain Lua values, no refs). Record the
     * base first: rebind may run with a NON-empty stack (called
     * from reload, outside dispatch — but never assume emptiness).
     * copy = shallow clone of values (iteration + per-key get/set
     * below never mutates it). */
    {
        int base = lua_gettop(L);

        lua_rawgeti(L, LUA_REGISTRYINDEX, old_state);
        lua_getfield(L, base + 1, "values");
        lua_newtable(L);
        /* copy = shallow clone of values. */
        lua_pushnil(L);
        while (lua_next(L, base + 2) != 0) {
            /* [..][old][vals][copy][key][val]: copy[key] = val. */
            lua_pushvalue(L, -2);
            lua_pushvalue(L, -2);
            lua_settable(L, base + 3);
            lua_pop(L, 1); /* val; key stays */
        }
        /* Rebuild: instantiate() hard-balances to entry depth (it
         * pops its own state table via luaL_ref), so after the call
         * the stack is back at [..][old][vals][copy].
         * entry->state_ref holds the fresh state. */
        if (le_lua_instantiate(rt, world, entry) != 0) {
            /* Keep the old state (transactional). */
            entry->state_ref = old_state;
            lua_settop(L, base);
            return;
        }
        /* Merge: for each old value whose name is still exported,
         * overwrite the fresh default. Stack:
         *   [..][old][vals][copy] at base+1..base+3.
         * Re-fetch NEW values + OLD exports; iterate the snapshot
         * copy (base+3). All absolute. */
        lua_rawgeti(L, LUA_REGISTRYINDEX, entry->state_ref);
        lua_getfield(L, base + 4, "values"); /* newvals=base+5 */
        lua_rawgeti(L, LUA_REGISTRYINDEX, old_state);
        lua_getfield(L, base + 6, "exports");
        lua_remove(L, base + 6); /* [..][old][vals][copy]
                                  * [newst][newvals][oldexp] */
        lua_pushnil(L);          /* first key */
        while (lua_next(L, base + 3) != 0) {
            /* [-2]=key [-1]=old value. Still exported? */
            lua_pushvalue(L, -2);      /* key copy */
            lua_gettable(L, base + 6); /* record | nil */
            if (!lua_isnil(L, -1)) {
                /* newvals[key] = old value. */
                lua_pushvalue(L, -3);  /* key */
                lua_pushvalue(L, -3);  /* old value */
                lua_settable(L, base + 5);
            }
            lua_pop(L, 2); /* record/nil + old value; key stays */
        }
        /* Terminating lua_next consumed the last key. Remaining
         * above base: [old][vals][copy][newst][newvals][oldexp]. */
        lua_settop(L, base);
        luaL_unref(L, LUA_REGISTRYINDEX, old_state);
    }
}

/* Forward declarations for the ops table. */
static int le_lua_compile(struct le_script_runtime *rt,
                          const char *source, size_t size,
                          const char *chunkname, char *out_error,
                          size_t error_cap);
static void le_lua_release_chunk(struct le_script_runtime *rt,
                                 int chunk);
static int le_lua_instantiate(struct le_script_runtime *rt,
                              le_world *world, le_script_entry *entry);
static void le_lua_release_instance(struct le_script_runtime *rt,
                                    le_world *world,
                                    le_script_entry *entry);
static int le_lua_fire(struct le_script_runtime *rt, le_world *world,
                       le_script_entry *entry, int callback, float dt);
static int le_lua_get_prop(struct le_script_runtime *rt,
                           le_world *world, le_script_entry *entry,
                           le_script_property *prop);
static int le_lua_set_prop(struct le_script_runtime *rt,
                           le_world *world, le_script_entry *entry,
                           const le_script_property *prop);
static int le_lua_list_props(struct le_script_runtime *rt,
                             le_world *world, le_script_entry *entry,
                             le_script_property *out, uint32_t cap,
                             uint32_t *out_count);

const le_script_backend_ops le_lua_backend_ops = {
    le_lua_compile,
    le_lua_release_chunk,
    le_lua_instantiate,
    le_lua_release_instance,
    le_lua_fire,
    le_lua_get_prop,
    le_lua_set_prop,
    le_lua_list_props,
};

static int le_lua_compile(struct le_script_runtime *rt,
                          const char *source, size_t size,
                          const char *chunkname, char *out_error,
                          size_t error_cap) {
    int ref = 0;

    if (rt == NULL || rt->L == NULL) {
        return -1;
    }
    ref = le_compile_chunk(rt, source, size, chunkname, out_error,
                           error_cap);
    return (ref < 0) ? -1 : ref;
}

static void le_lua_release_chunk(struct le_script_runtime *rt,
                                 int chunk) {
    if (rt == NULL || rt->L == NULL || chunk < 0) {
        return;
    }
    luaL_unref(rt->L, LUA_REGISTRYINDEX, chunk);
}

/* Build the per-instance environment: {self=obj, export=fn,
 * require=sandboxed, print=routed, __index=_G}. Chunk runs with
 * this _ENV; lifecycle functions are read from the chunk return
 * (table) or its _ENV globals.
 *
 * DIAGNOSTIC SCAFFOLD (triage only): define
 * LE_SCRIPT_TRACE_INSTANTIATE to fprintf each phase. Leave
 * undefined for normal builds. */
static int le_lua_instantiate(struct le_script_runtime *rt,
                              le_world *world, le_script_entry *entry) {
    lua_State *L;
    le_asset h;
    le_asset_slot *slot = NULL;
    uint32_t aslot = 0;
    le_result code = LE_SUCCESS;

    if (rt == NULL || rt->L == NULL || world == NULL ||
        entry == NULL) {
        return -1;
    }
    L = rt->L;
    h = entry->asset;
    if (h.index == LE_ASSET_INDEX_INVALID || h.generation == 0) {
        return -1;
    }
    if (world->engine == NULL) {
        return -1;
    }
    if (!le_resolve_asset_live(world->engine, &h, &aslot, &code)) {
        return -1;
    }
    slot = &world->engine->assets[aslot];
    if (slot->type != LE_ASSET_SCRIPT ||
        slot->state != LE_ASSET_READY ||
        slot->script_chunk_ref == LE_SCRIPT_NOREF) {
        return -1;
    }
    if (entry->slot >= world->capacity ||
        !world->slots[entry->slot].alive) {
        return -1;
    }
    /* Instance state table: {values={}, exports={}, funcs={}}.
     * Balance contract: `base` stack depth on entry, base+1 ([state])
     * at every exit below (success leaves it for luaL_ref; failures
     * restore base). One block per phase (C89-decl discipline for
     * MSVC /W3). */
    {
        int base = 0;
        int state = 0;
        le_object selfobj;
        int hook_rc = 0;
        const char *msg = NULL;
        char emsg[512];
        int k = 0;
        le_object errobj;

        base = lua_gettop(L);
        lua_newtable(L);
        state = lua_gettop(L); /* base+1 */
        if (state != base + 1) {
            lua_settop(L, base);
            return -1;
        }

        lua_newtable(L);
        lua_setfield(L, state, "values");
        lua_newtable(L);
        lua_setfield(L, state, "exports");
        lua_newtable(L);
        lua_setfield(L, state, "funcs");
        /* _ENV for the chunk: {self, export, require, print}
         * with __index -> _G (shared read-only-ish stdlib). */
        lua_newtable(L);
        selfobj.index = entry->slot;
        selfobj.generation = world->slots[entry->slot].generation;
        selfobj.world_tag = world->tag;
        le_lua_push_object(L, world, &selfobj);
        lua_setfield(L, -2, "self");
        lua_pushcfunction(L, le_lua_export);
        lua_setfield(L, -2, "export");
        lua_getglobal(L, "require");
        lua_setfield(L, -2, "require");
        lua_getglobal(L, "print");
        lua_setfield(L, -2, "print");
        lua_getglobal(L, "_G");
        lua_setfield(L, -2, "_G");
        lua_newtable(L);
        lua_getglobal(L, "_G");
        lua_setfield(L, -2, "__index");
        lua_setmetatable(L, -2);
        lua_setfield(L, state, "env");
        /* base/state stay in scope: close the outer block late. */
        /* Publish per-instantiate anchors for export(). Stack:
         * [..][state]; getfield pushes [..][state][exports]. */
        lua_getfield(L, state, "exports");
        lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_exports");
        lua_getfield(L, state, "values");
        lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_values");
    /* Load the chunk function fresh (re-entrant: same chunk runs
     * per instance) and run it with _ENV. Stack: [..][state].
     * Order: [state][fn][env]; setupvalue pops env and sets fn's
     * _ENV. Anchor everything at `state`. */
    lua_rawgeti(L, LUA_REGISTRYINDEX, slot->script_chunk_ref);
    if (lua_gettop(L) != state + 1) {
        lua_settop(L, base);
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_exports");
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_values");
        return -1;
    }
    if (!lua_isfunction(L, state + 1)) {
        /* Chunk ref died (unloaded/replaced under us) — treat as a
         * failed instantiate, stack-balanced. */
        lua_settop(L, base);
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_exports");
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_values");
        return -1;
    }
    lua_getfield(L, state, "env");
    if (lua_gettop(L) != state + 2) {
        lua_settop(L, base);
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_exports");
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_values");
        return -1;
    }
    if (!lua_istable(L, state + 2)) {
        lua_settop(L, base);
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_exports");
        lua_pushnil(L);
        lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_values");
        return -1;
    }
    /* upvalue _ENV is the first upvalue of a main chunk. When the
     * chunk has NO upvalues at all (empty script), setupvalue
     * returns NULL — that is FINE (nothing to isolate); only treat
     * a MISSING env table as fatal. Proceed to pcall either way:
     * [..][state][fn](+[env] when setupvalue consumed it). */
    {
        const char *upname = lua_setupvalue(L, state + 1, 1);

        if (upname == NULL) {
            /* No _ENV upvalue: the chunk ignores environments
             * (empty/no-global script). Pop the env table we
             * pushed: [..][state][fn][env] -> [..][state][fn]. */
            if (lua_gettop(L) == state + 2) {
                lua_pop(L, 1);
            }
        }
    }
    le_script_hook_begin(rt);
    hook_rc = lua_pcall(L, 0, 1, 0);

    le_script_hook_end(rt);
    /* The chunk ran with the per-instance _ENV: its globals AND
     * the funcs harvest below read state.env/state.funcs via
     * ABSOLUTE indices. pcall may leave the stack as
     * [..][state]([ret]|error) — verify before indexing. */
    if (hook_rc == LUA_OK && lua_gettop(L) != base + 2) {
        /* Unexpected arity (chunk returned >1 value?): collapse to
         * [..][state][ret0]. */
        lua_settop(L, base + 2);
    }
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_exports");
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "le_inst_values");
    if (hook_rc != LUA_OK) {
        msg = lua_tostring(L, -1);

        snprintf(emsg, sizeof(emsg), "%s",
                 (msg != NULL) ? msg : "chunk failed");
        lua_settop(L, base); /* error + state */
        errobj.index = entry->slot;
        errobj.generation =
            world->slots[entry->slot].generation;
        errobj.world_tag = world->tag;
        le_script_record_error(
            rt, "instantiate", world, &errobj,
            (slot->source != NULL) ? slot->source : "", emsg);
        lua_settop(L, base);
        return -1;
    }
    /* Chunk return: table of lifecycle fns (or nil). Stack:
     * [..][state][ret]. Move start/update/fixed_update/destroy
     * into state.funcs. Anchor at base/state. */
    if (lua_istable(L, base + 2)) {
        int ftop = 0;

        lua_getfield(L, state, "funcs"); /* [..][s][r][f] */
        ftop = lua_gettop(L);
        if (ftop != base + 3) {
            lua_settop(L, base + 2);
        } else {
            for (k = 0; k < 4; k++) {
                lua_getfield(L, base + 2,
                             le_lua_callback_names[k]); /* value */
                if (lua_isfunction(L, -1)) {
                    lua_setfield(L, base + 3,
                                 le_lua_callback_names[k]);
                } else {
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1); /* funcs -> [..][state][ret] */
        }
    }
    lua_pop(L, 1); /* chunk return -> [..][state] */
    /* Also harvest _ENV globals as fallback funcs (function
     * start() ... end style declares globals in _ENV).
     * Stack: [..][state][env][funcs]: env=base+2, funcs=base+3. */
    lua_getfield(L, state, "env");
    if (lua_gettop(L) != base + 2 || !lua_istable(L, base + 2)) {
        lua_settop(L, base + 1);
    } else {
        lua_getfield(L, state, "funcs");
        if (lua_gettop(L) != base + 3 ||
            !lua_istable(L, base + 3)) {
            lua_settop(L, base + 1);
        } else {
    for (k = 0; k < 4; k++) {
        lua_getfield(L, base + 2, le_lua_callback_names[k]);
        if (!lua_isnil(L, -1)) {
            lua_getfield(L, base + 3, le_lua_callback_names[k]);
            if (lua_isnil(L, -1)) {
                lua_pop(L, 1);
                lua_pushvalue(L, -1);
                lua_setfield(L, base + 3, le_lua_callback_names[k]);
            } else {
                lua_pop(L, 1);
            }
        }
        lua_pop(L, 1);
    }
        lua_settop(L, base + 1);
    }
    }
    entry->state_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_settop(L, base);
    return 0;
}
}

static void le_lua_release_instance(struct le_script_runtime *rt,
                                    le_world *world,
                                    le_script_entry *entry) {
    (void)world;
    if (rt == NULL || rt->L == NULL || entry == NULL) {
        return;
    }
    if (entry->state_ref != LE_SCRIPT_NOREF) {
        luaL_unref(rt->L, LUA_REGISTRYINDEX, entry->state_ref);
        entry->state_ref = LE_SCRIPT_NOREF;
    }
}

/* Fire one callback: funcs[name](self, dt?). self passed BOTH as
 * arg and as global `self` (spec example shape); arg form is
 * canonical (AOT maps to an explicit context parameter). Names
 * come from the file-scope le_lua_callback_names table. */
static const char *le_callback_name(int callback) {
    switch (callback) {
    case 0:
        return "start";
    case 1:
        return "update";
    case 2:
        return "fixed_update";
    case 3:
        return "destroy";
    default:
        return "?";
    }
}

static int le_lua_fire(struct le_script_runtime *rt, le_world *world,
                       le_script_entry *entry, int callback, float dt) {
    lua_State *L;
    const char *name;
    le_object obj;
    int nargs;
    int rc;

    if (rt == NULL || rt->L == NULL || world == NULL ||
        entry == NULL) {
        return -1;
    }
    if (callback < 0 || callback > 3) {
        return -1;
    }
    L = rt->L;
    if (L == NULL) {
        return -1;
    }
    if (rt->backend != &le_lua_backend_ops) {
        return -1;
    }
    name = le_callback_name(callback);
    if (name == NULL || name[0] == '\0' || name[0] == '?') {
        return -1;
    }
    if (entry->state_ref == LE_SCRIPT_NOREF) {
        return -1;
    }
    if (entry->slot >= world->capacity ||
        !world->slots[entry->slot].alive) {
        return -1;
    }
    /* Fetch fn + self: [..][fn][self]. Record the base first —
     * fire runs inside dispatch AND inside bindings (metamethods
     * call back into list/get/set), so the stack is NOT empty in
     * general. Absolute indices from `base` throughout. */
    {
        int base = 0;
        int stateref = 0;

        base = lua_gettop(L);
        stateref = entry->state_ref;
        /* [base+1]=state [base+2]=funcs [base+3]=fn? */
        lua_rawgeti(L, LUA_REGISTRYINDEX, stateref);
        lua_getfield(L, base + 1, "funcs");
        lua_getfield(L, base + 2, name);
        if (lua_gettop(L) < base + 3 ||
            !lua_isfunction(L, base + 3)) {
            lua_settop(L, base);
            return 0; /* absent callback is a no-op (not an error) */
        }
        lua_getfield(L, base + 1, "env");  /* base+4 */
        lua_getfield(L, base + 4, "self"); /* base+5 */
        /* [state][funcs][fn][env][self] -> [..][fn][self]. */
        lua_copy(L, base + 3, base + 1);
        lua_copy(L, base + 5, base + 2);
        lua_settop(L, base + 2); /* [..][fn][self] */
    }
    nargs = 1;
    if (callback == 1 || callback == 2) {
        lua_pushnumber(L, (lua_Number)dt);
        nargs = 2;
    }
    obj.index = entry->slot;
    obj.generation = world->slots[entry->slot].generation;
    obj.world_tag = world->tag;
    /* Global `self` for the spec's bare-self shape (save/restore).
     * Stack entering: [..][fn][self]([dt]). Save the old global
     * into the REGISTRY (no probe slot on the stack), install the
     * self arg, pcall [fn][args] plainly, then restore. The only
     * arithmetic is before the call; after pcall the stack holds
     * [..](results|error) which we pop explicitly.
     *
     * NESTED-DISPATCH NOTE: fire() reenters through bindings
     * (World.create/destroy, set_parent, instantiate...). Those
     * paths call le_lua_fire recursively on the SAME lua_State, so
     * the registry save slot "le_saved_self" MUST be a stack: push
     * the old value per nesting level, pop on unwind. A single
     * slot would clobber the outer self when a callback creates or
     * destroys objects mid-dispatch. */
    {
        int base = lua_gettop(L) - nargs - 1; /* .. top */
        int rc2;

        /* Save old global self (may be nil) on a registry STACK
         * (nested dispatch reenters fire() on the same state). All
         * absolute from the pre-push top (`savebase`). */
        {
            int savebase = lua_gettop(L);

            lua_getfield(L, LUA_REGISTRYINDEX, "le_saved_self");
            if (!lua_istable(L, savebase + 1)) {
                lua_pop(L, 1);
                lua_newtable(L);
                lua_pushvalue(L, savebase + 1);
                lua_setfield(L, LUA_REGISTRYINDEX,
                             "le_saved_self");
            }
            lua_getglobal(L, "self");
            lua_seti(L, savebase + 1,
                     (lua_Integer)(rt->firing_depth + 1));
            lua_settop(L, savebase);
        }
        /* Install the self arg as the global. */
        lua_pushvalue(L, base + 2); /* self arg */
        lua_setglobal(L, "self");
        rt->firing_world = world;
        rt->firing_depth++;
        le_script_hook_begin(rt);
        rc2 = lua_pcall(L, nargs, 0, 0);
        rc = rc2;
        le_script_hook_end(rt);
        rt->firing_depth--;
        /* firing_world restore: NULL at outermost, else keep the
         * (single-world) dispatch world. Nested fires always run
         * on the same world (bindings reject cross-world). */
        if (rt->firing_depth == 0) {
            rt->firing_world = NULL;
        }
        rt->callbacks_this_frame++;
        /* Restore the old global from the stack (absolute again —
         * the save table is fetched fresh, then indexed). */
        {
            int rbase = lua_gettop(L);

            lua_getfield(L, LUA_REGISTRYINDEX, "le_saved_self");
            lua_geti(L, rbase + 1,
                     (lua_Integer)(rt->firing_depth + 1));
            lua_setglobal(L, "self");
            lua_pushnil(L);
            lua_seti(L, rbase + 1,
                     (lua_Integer)(rt->firing_depth + 1));
            lua_settop(L, rbase);
        }
        if (rc != LUA_OK) {
            const char *msg = NULL;
            char emsg[512];
            le_asset h = entry->asset;
            le_asset_slot *as = NULL;
            uint32_t aslot = 0;
            le_result code = LE_SUCCESS;

            /* The error object sits on top. Copy its text BEFORE
             * anything else runs. */
            msg = lua_tostring(L, -1);
            snprintf(emsg, sizeof(emsg), "%s",
                     (msg != NULL) ? msg : "callback failed");
            lua_settop(L, base); /* error + .. -> base */
            if (le_resolve_asset_live(world->engine, &h, &aslot,
                                      &code)) {
                as = &world->engine->assets[aslot];
            }
            le_script_record_error(
                rt, name, world, &obj,
                (as != NULL && as->source != NULL) ? as->source
                                                  : "",
                emsg);
            return -1;
        }
        lua_settop(L, base); /* no results; balance to entry */
        return 0;
    }
}

static int le_lua_get_prop(struct le_script_runtime *rt,
                           le_world *world, le_script_entry *entry,
                           le_script_property *prop) {
    lua_State *L;
    int found = 0;
    int base;

    if (rt == NULL || world == NULL || entry == NULL ||
        prop == NULL) {
        return 0;
    }
    (void)world;
    L = rt->L;
    if (entry->state_ref == LE_SCRIPT_NOREF) {
        return 0;
    }
    base = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, entry->state_ref);
    lua_getfield(L, base + 1, "values");
    lua_getfield(L, base + 2, prop->name);
    if (!lua_isnil(L, base + 3)) {
        le_script_property got;

        if (le_script_prop_from_lua(L, base + 3, prop->name,
                                    &got) &&
            got.type == prop->type) {
            *prop = got;
            found = 1;
        } else if (le_script_prop_from_lua(L, base + 3, prop->name,
                                           &got)) {
            /* Type changed since export: report current. */
            *prop = got;
            found = 1;
        }
    }
    lua_settop(L, base);
    return found;
}

static int le_lua_set_prop(struct le_script_runtime *rt,
                           le_world *world, le_script_entry *entry,
                           const le_script_property *prop) {
    lua_State *L;

    if (rt == NULL || world == NULL || entry == NULL ||
        prop == NULL) {
        return -1;
    }
    (void)world;
    L = rt->L;
    if (entry->state_ref == LE_SCRIPT_NOREF) {
        return -1;
    }
    /* Must be a declared export (metadata is engine-meaningful).
     * Absolute indices from a recorded base (never relative
     * across pushes). */
    {
        int base = lua_gettop(L);
        int want;

        lua_rawgeti(L, LUA_REGISTRYINDEX, entry->state_ref);
        lua_getfield(L, base + 1, "exports");
        lua_getfield(L, base + 2, prop->name);
        if (lua_isnil(L, base + 3)) {
            lua_settop(L, base);
            return -1;
        }
        lua_getfield(L, base + 3, "type");
        want = (int)lua_tointeger(L, base + 4);
        if (want != (int)prop->type) {
            lua_settop(L, base);
            return -1;
        }
        /* [state][exports][record][want]; fetch values from state:
         * [state][exports][record][want][vals]. */
        lua_getfield(L, base + 1, "values");
        if (prop->type == LE_SCRIPT_PROP_ASSET) {
            /* Asset values need engine context for userdata. */
            le_lua_push_asset(L, world->engine, &prop->asset);
        } else {
            le_script_prop_to_lua(L, prop);
        }
        lua_setfield(L, base + 5, prop->name);
        lua_settop(L, base);
        return 0;
    }
}

static int le_lua_list_props(struct le_script_runtime *rt,
                             le_world *world, le_script_entry *entry,
                             le_script_property *out, uint32_t cap,
                             uint32_t *out_count) {
    lua_State *L;
    uint32_t n = 0;

    if (out_count != NULL) {
        *out_count = 0;
    }
    if (rt == NULL || world == NULL || entry == NULL) {
        return 0;
    }
    (void)world;
    L = rt->L;
    if (entry->state_ref == LE_SCRIPT_NOREF) {
        return 0;
    }
    /* Absolute indices from a recorded base (the stack is NOT
     * empty in general — property reads run inside bindings and
     * dispatch). */
    {
        int base = lua_gettop(L);

        lua_rawgeti(L, LUA_REGISTRYINDEX, entry->state_ref);
        lua_getfield(L, base + 1, "exports");
        lua_getfield(L, base + 1, "values");
        lua_pushnil(L);
        while (lua_next(L, base + 2) != 0) {
            /* key=name(base+4... precisely: -2), value=record. */
            const char *name = lua_tostring(L, -2);

            if (name != NULL) {
                n++;
                if (out != NULL && n <= cap) {
                    le_script_property *dst = &out[n - 1];

                    memset(dst, 0, sizeof(*dst));
                    snprintf(dst->name, sizeof(dst->name),
                             "%s", name);
                    lua_getfield(L, -1, "type");
                    dst->type = (le_script_property_type)
                        lua_tointeger(L, -1);
                    lua_pop(L, 1);
                    /* current value from the anchored values
                     * table (base+3). */
                    lua_getfield(L, base + 3, name);
                    if (!lua_isnil(L, -1)) {
                        le_script_property cur;

                        if (le_script_prop_from_lua(L, -1, name,
                                                    &cur)) {
                            dst->type = cur.type;
                            *dst = cur;
                            snprintf(dst->name,
                                     sizeof(dst->name), "%s",
                                     name);
                        }
                    }
                    lua_pop(L, 1);
                }
            }
            lua_pop(L, 1);
        }
        lua_settop(L, base);
    }
    if (out_count != NULL) {
        *out_count = n;
    }
    return 1;
}
