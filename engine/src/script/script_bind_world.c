/*
 * Lua World/Assets/require bindings (Phase 26). Thin over le_*;
 * every handle crossing the boundary is validated (stale/cross-
 * world/type failures become Lua errors caught by the dispatcher).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "script/script_internal.h"

/* firing-world helper: bindings that need "the current world"
 * (require-time code has none — those bindings take explicit
 * handles instead). */
static le_world *le_current_world(lua_State *L) {
    le_script_runtime *rt = le_lua_current_runtime(L);

    if (rt == NULL) {
        return NULL;
    }
    return rt->firing_world;
}

/* World.create(name) -> object */
static int le_w_create(lua_State *L) {
    le_world *w = le_current_world(L);
    const char *name = NULL;
    le_object obj;
    le_result rc;

    if (w == NULL) {
        return luaL_error(L, "World.create outside dispatch");
    }
    if (w->scripts_tearing_down) {
        return luaL_error(L, "world is shutting down");
    }
    if (lua_gettop(L) >= 1 && !lua_isnil(L, 1)) {
        name = luaL_checkstring(L, 1);
    }
    rc = le_object_create(w, &obj);
    if (rc != LE_SUCCESS) {
        return luaL_error(L, "create failed (%d)", (int)rc);
    }
    if (name != NULL) {
        le_object_set_name(w, &obj, name);
    }
    le_lua_push_object(L, w, &obj);
    return 1;
}

/* World.destroy(obj) */
static int le_w_destroy(lua_State *L) {
    le_world *w = NULL;
    le_object obj = LE_OBJECT_INVALID;
    le_lua_check_object(L, 1, &w, &obj);

    if (w == NULL) {
        return luaL_error(L, "stale object handle");
    }
    if (w->scripts_tearing_down) {
        return luaL_error(L, "world is shutting down");
    }
    /* Cross-world safety: the userdata's world must be the firing
     * world (world tags are authoritative; mixing worlds through
     * one dispatch is rejected loudly). */
    if (w != le_current_world(L)) {
        return luaL_error(L, "cross-world object misuse");
    }
    {
        le_result rc = le_object_destroy(w, &obj);

        if (rc != LE_SUCCESS) {
            return luaL_error(L, "destroy failed (%d)", (int)rc);
        }
    }
    return 0;
}

/* World.find(name) -> object | nil */
static int le_w_find(lua_State *L) {
    le_world *w = le_current_world(L);
    const char *name = luaL_checkstring(L, 1);
    le_object obj = LE_OBJECT_INVALID;

    if (w == NULL) {
        return luaL_error(L, "World.find outside dispatch");
    }
    if (name == NULL) {
        lua_pushnil(L);
        return 1;
    }
    if (le_world_find_by_name(w, name, &obj)) {
        le_lua_push_object(L, w, &obj);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

/* World.is_alive(obj) -> bool */
static int le_w_is_alive(lua_State *L) {
    le_world *w = NULL;
    le_object obj = LE_OBJECT_INVALID;

    if (!le_lua_check_object(L, 1, &w, &obj)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L,
                    le_object_is_alive(w, &obj) ? 1 : 0);
    return 1;
}

/* World.instantiate(scene) -> instance */
static int le_w_instantiate(lua_State *L) {
    le_world *w = le_current_world(L);
    le_engine *e = NULL;
    le_asset scene = LE_ASSET_INVALID;
    le_scene_instance inst;

    if (w == NULL) {
        return luaL_error(L, "instantiate outside dispatch");
    }
    if (w->scripts_tearing_down) {
        return luaL_error(L, "world is shutting down");
    }
    /* Scene userdata (asset metatable). Accepts the le_scene tag
     * (load_scene) as well as the plain le_asset tag
     * (find_by_id) — layout-identical, both engine-validated. */
    {
        const le_lua_asset *u =
            (const le_lua_asset *)luaL_testudata(
                L, 1, LE_LUA_SCENE_MT);

        if (u == NULL) {
            u = (const le_lua_asset *)luaL_testudata(
                L, 1, LE_LUA_ASSET_MT);
        }
        if (u == NULL) {
            return luaL_error(L, "scene expected");
        }
        scene.index = u->index;
        scene.generation = u->generation;
        e = u->engine;
    }
    if (e != w->engine) {
        return luaL_error(L, "cross-engine scene misuse");
    }
    memset(&inst, 0, sizeof(inst));
    {
        le_result rc = le_scene_instantiate(w, &scene, &inst);

        if (rc != LE_SUCCESS) {
            return luaL_error(L, "instantiate failed (%d)",
                              (int)rc);
        }
    }
    /* Track the instance record for the Lua handle (kept in the
     * world's Lua instance registry; freed with the world). */
    {
        uint32_t serial = le_script_track_instance(w, &inst);

        if (serial == 0) {
            le_scene_instance_free(&inst);
            return luaL_error(L, "out of memory");
        }
        lua_newtable(L);
        lua_pushinteger(L, (lua_Integer)serial);
        lua_setfield(L, -2, "serial");
        /* Attach methods via metatable. */
        luaL_getmetatable(L, LE_LUA_INSTANCE_MT);
        lua_setmetatable(L, -2);
        /* Stash world tag for resolution. */
        lua_pushinteger(L, (lua_Integer)w->tag);
        lua_setfield(L, -2, "world_tag");
    }
    return 1;
}

/* instance:find_by_id(hex) -> object | nil */
static int le_i_find_by_id(lua_State *L) {
    const char *hex = luaL_checkstring(L, 2);
    le_script_runtime *rt = le_lua_current_runtime(L);
    uint32_t serial = 0;
    le_world *w = NULL;

    if (rt == NULL || hex == NULL) {
        return luaL_error(L, "bad instance lookup");
    }
    lua_getfield(L, 1, "serial");
    serial = (uint32_t)lua_tointeger(L, -1);
    lua_pop(L, 1);
    lua_getfield(L, 1, "world_tag");
    {
        uint32_t tag = (uint32_t)lua_tointeger(L, -1);
        le_world *it;

        lua_pop(L, 1);
        for (it = NULL, w = NULL; ; ) {
            /* find world by tag across engines? Instances are
             * world-local; tag suffices within the firing
             * engine. */
            le_engine *e = rt->firing_world
                               ? rt->firing_world->engine
                               : NULL;

            if (e == NULL) {
                break;
            }
            for (it = e->worlds; it != NULL; it = it->next) {
                if (it->tag == tag) {
                    w = it;
                    break;
                }
            }
            break;
        }
    }
    if (w == NULL) {
        lua_pushnil(L);
        return 1;
    }
    {
        le_object obj = LE_OBJECT_INVALID;

        if (le_script_instance_lookup_serial(w, serial, hex, &obj)) {
            le_lua_push_object(L, w, &obj);
            return 1;
        }
    }
    lua_pushnil(L);
    return 1;
}

/* instance:object_count() -> int */
static int le_i_count(lua_State *L) {
    le_script_runtime *rt = le_lua_current_runtime(L);

    (void)rt;
    lua_getfield(L, 1, "serial");
    {
        /* Count via the tracked record. */
        le_world *w = le_current_world(L);
        uint32_t serial = (uint32_t)lua_tointeger(L, -1);

        lua_pop(L, 1);
        lua_pushinteger(
            L, (lua_Integer)((w != NULL)
                                 ? le_script_instance_count(w,
                                                            serial)
                                 : 0));
        return 1;
    }
}

/* Assets.load(path) -> {meshes, materials, nodes} for glTF. */
static int le_a_load(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    le_script_runtime *rt = le_lua_current_runtime(L);
    le_world *w = (rt != NULL) ? rt->firing_world : NULL;
    le_engine *e = (w != NULL) ? w->engine : NULL;
    le_gltf_result imp;
    le_result rc;

    if (e == NULL || path == NULL) {
        return luaL_error(L, "Assets.load outside dispatch");
    }
    memset(&imp, 0, sizeof(imp));
    rc = le_gltf_import(e, path, &imp);
    if (rc != LE_SUCCESS) {
        return luaL_error(L, "import failed (%d)", (int)rc);
    }
    lua_newtable(L);
    lua_newtable(L);
    {
        uint32_t i;

        for (i = 0; i < imp.mesh_count; i++) {
            le_lua_push_asset(L, e, &imp.mesh_assets[i]);
            lua_seti(L, -2, (lua_Integer)(i + 1));
        }
    }
    lua_setfield(L, -2, "meshes");
    lua_newtable(L);
    {
        uint32_t i;

        for (i = 0; i < imp.material_count; i++) {
            le_lua_push_asset(L, e, &imp.material_assets[i]);
            lua_seti(L, -2, (lua_Integer)(i + 1));
        }
    }
    lua_setfield(L, -2, "materials");
    lua_pushinteger(L, (lua_Integer)imp.node_count);
    lua_setfield(L, -2, "node_count");
    le_gltf_import_free(&imp);
    return 1;
}

/* Assets.load_scene(path) -> scene asset userdata. */
static int le_a_load_scene(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    le_script_runtime *rt = le_lua_current_runtime(L);
    le_world *w = (rt != NULL) ? rt->firing_world : NULL;
    le_engine *e = (w != NULL) ? w->engine : NULL;
    le_asset scene = LE_ASSET_INVALID;
    le_result rc;

    if (e == NULL || path == NULL) {
        return luaL_error(L, "load_scene outside dispatch");
    }
    rc = le_scene_create(e, 1, &scene);
    if (rc != LE_SUCCESS) {
        return luaL_error(L, "scene create failed (%d)", (int)rc);
    }
    rc = le_scene_load_file(e, &scene, path);
    if (rc != LE_SUCCESS) {
        le_asset_unload(e, &scene);
        return luaL_error(L, "scene load failed (%d)", (int)rc);
    }
    le_lua_push_asset(L, e, &scene);
    /* Tag the metatable as scene for instantiate checks. */
    luaL_getmetatable(L, LE_LUA_SCENE_MT);
    lua_setmetatable(L, -2);
    return 1;
}

/* Assets.find_by_id(hex) -> asset | nil */
static int le_a_find_by_id(lua_State *L) {
    const char *hex = luaL_checkstring(L, 1);
    le_script_runtime *rt = le_lua_current_runtime(L);
    le_world *w = (rt != NULL) ? rt->firing_world : NULL;
    le_engine *e = (w != NULL) ? w->engine : NULL;
    le_asset_id id;
    le_asset h = LE_ASSET_INVALID;

    if (e == NULL || hex == NULL) {
        return luaL_error(L, "find_by_id outside dispatch");
    }
    if (!le_asset_id_from_string(hex, &id)) {
        lua_pushnil(L);
        return 1;
    }
    if (le_asset_find_by_id(e, &id, &h)) {
        le_lua_push_asset(L, e, &h);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

void le_lua_register_world(lua_State *L) {
    static const luaL_Reg world_fn[] = {
        { "create", le_w_create },
        { "destroy", le_w_destroy },
        { "find", le_w_find },
        { "is_alive", le_w_is_alive },
        { "instantiate", le_w_instantiate },
        { NULL, NULL },
    };
    static const luaL_Reg assets_fn[] = {
        { "load", le_a_load },
        { "load_scene", le_a_load_scene },
        { "find_by_id", le_a_find_by_id },
        { NULL, NULL },
    };
    static const luaL_Reg instance_m[] = {
        { "find_by_id", le_i_find_by_id },
        { "object_count", le_i_count },
        { NULL, NULL },
    };

    lua_newtable(L);
    luaL_setfuncs(L, world_fn, 0);
    lua_setglobal(L, "World");
    lua_newtable(L);
    luaL_setfuncs(L, assets_fn, 0);
    lua_setglobal(L, "Assets");
    /* Instance + scene metatables (method lookup via __index). */
    luaL_newmetatable(L, LE_LUA_INSTANCE_MT);
    lua_newtable(L);
    luaL_setfuncs(L, instance_m, 0);
    lua_setfield(L, -2, "__index");
    lua_pop(L, 1);
    luaL_newmetatable(L, LE_LUA_SCENE_MT);
    lua_pop(L, 1);
}
