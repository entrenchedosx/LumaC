/*
 * Lua character + shape-cast bindings (Phase 30): thin over
 * le_character_* and le_physics_*cast. No Lua-side gameplay
 * state; all queries return plain value data.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "script/script_internal.h"

static int le_ch_live(lua_State *L, le_world **w, le_object *o) {
    if (!le_lua_check_object(L, 1, w, o)) {
        return luaL_error(L, "stale object handle");
    }
    return 1;
}

static int le_ch_mutable(lua_State *L, le_world **w,
                         le_object *o) {
    le_script_runtime *rt = le_lua_current_runtime(L);

    le_ch_live(L, w, o);
    if (rt == NULL || rt->firing_world != *w) {
        return luaL_error(L, "cross-world object misuse");
    }
    if ((*w)->scripts_tearing_down) {
        return luaL_error(L, "world is shutting down");
    }
    return 1;
}

/* self:character_move(dx, dy, dz) -> grounded (bool).
 * Sweeps the controller capsule; deterministic per fixed step. */
static int le_c_character_move(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float d[3];
    le_character_move_result res;

    le_ch_mutable(L, &w, &o);
    d[0] = (float)luaL_checknumber(L, 2);
    d[1] = (float)luaL_checknumber(L, 3);
    d[2] = (float)luaL_checknumber(L, 4);
    memset(&res, 0, sizeof(res));
    if (le_character_move(w, &o, d, &res) != LE_SUCCESS) {
        return luaL_error(L, "character_move failed");
    }
    lua_pushboolean(L, res.grounded);
    return 1;
}

/* self:character_is_grounded() -> bool */
static int le_c_character_is_grounded(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_ch_live(L, &w, &o);
    lua_pushboolean(L, le_character_is_grounded(w, &o));
    return 1;
}

/* self:character_ground_normal() -> x, y, z */
static int le_c_character_ground_normal(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float n[3];

    le_ch_live(L, &w, &o);
    le_character_ground_normal(w, &o, n);
    lua_pushnumber(L, (lua_Number)n[0]);
    lua_pushnumber(L, (lua_Number)n[1]);
    lua_pushnumber(L, (lua_Number)n[2]);
    return 3;
}

/* self:character_ground_object() -> object or nil */
static int le_c_character_ground_object(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    le_object g;

    le_ch_live(L, &w, &o);
    g = le_character_ground_object(w, &o);
    if (!le_object_is_valid(&g)) {
        lua_pushnil(L);
        return 1;
    }
    le_lua_push_object(L, w, &g);
    return 1;
}

/* self:character_velocity() -> x, y, z */
static int le_c_character_velocity(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float v[3];

    le_ch_live(L, &w, &o);
    le_character_get_velocity(w, &o, v);
    lua_pushnumber(L, (lua_Number)v[0]);
    lua_pushnumber(L, (lua_Number)v[1]);
    lua_pushnumber(L, (lua_Number)v[2]);
    return 3;
}

/* self:character_speed() -> horizontal speed (scalar) */
static int le_c_character_speed(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_ch_live(L, &w, &o);
    lua_pushnumber(
        L, (lua_Number)le_character_horizontal_speed(w, &o));
    return 1;
}

/* self:character_set_vertical_velocity(v) */
static int le_c_character_set_vertical_velocity(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float v = (float)luaL_checknumber(L, 2);

    le_ch_mutable(L, &w, &o);
    if (le_character_set_vertical_velocity(w, &o, v) !=
        LE_SUCCESS) {
        return luaL_error(L,
                          "set_vertical_velocity failed");
    }
    return 0;
}

/* self:character_vertical_velocity() -> v */
static int le_c_character_vertical_velocity(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_ch_live(L, &w, &o);
    lua_pushnumber(
        L,
        (lua_Number)le_character_get_vertical_velocity(w,
                                                       &o));
    return 1;
}

/* self:character_teleport(x, y, z) */
static int le_c_character_teleport(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float p[3];

    le_ch_mutable(L, &w, &o);
    p[0] = (float)luaL_checknumber(L, 2);
    p[1] = (float)luaL_checknumber(L, 3);
    p[2] = (float)luaL_checknumber(L, 4);
    if (le_character_teleport(w, &o, p) != LE_SUCCESS) {
        return luaL_error(L, "character_teleport failed");
    }
    return 0;
}

/* self:character_gravity(dt) */
static int le_c_character_gravity(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float dt = (float)luaL_checknumber(L, 2);

    le_ch_mutable(L, &w, &o);
    if (le_character_gravity(w, &o, dt) != LE_SUCCESS) {
        return luaL_error(L, "character_gravity failed");
    }
    lua_pushboolean(L, le_character_is_grounded(w, &o));
    return 1;
}

static const luaL_Reg kCharacterMethods[] = {
    { "character_move", le_c_character_move },
    { "character_is_grounded", le_c_character_is_grounded },
    { "character_ground_normal",
      le_c_character_ground_normal },
    { "character_ground_object",
      le_c_character_ground_object },
    { "character_velocity", le_c_character_velocity },
    { "character_speed", le_c_character_speed },
    { "character_set_vertical_velocity",
      le_c_character_set_vertical_velocity },
    { "character_vertical_velocity",
      le_c_character_vertical_velocity },
    { "character_teleport", le_c_character_teleport },
    { "character_gravity", le_c_character_gravity },
    { NULL, NULL },
};

void le_lua_register_character_methods(lua_State *L) {
    luaL_setfuncs(L, kCharacterMethods, 0);
}

/* ---- Physics shape-cast queries (global Physics table) ---- */

/* Push a shape hit as a value table:
 * {object, fraction, distance, point={x,y,z}, normal={x,y,z},
 *  started_overlapping, penetration} or nil when no hit. */
static void le_push_shape_hit(lua_State *L, le_world *w,
                              int have,
                              const le_shape_hit *hit) {
    if (!have || hit == NULL) {
        lua_pushnil(L);
        return;
    }
    lua_newtable(L);
    le_lua_push_object(L, w, &hit->object);
    lua_setfield(L, -2, "object");
    lua_pushnumber(L, (lua_Number)hit->fraction);
    lua_setfield(L, -2, "fraction");
    lua_pushnumber(L, (lua_Number)hit->distance);
    lua_setfield(L, -2, "distance");
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)hit->point[0]);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, (lua_Number)hit->point[1]);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, (lua_Number)hit->point[2]);
    lua_rawseti(L, -2, 3);
    lua_setfield(L, -2, "point");
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)hit->normal[0]);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, (lua_Number)hit->normal[1]);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, (lua_Number)hit->normal[2]);
    lua_rawseti(L, -2, 3);
    lua_setfield(L, -2, "normal");
    lua_pushboolean(L, hit->started_overlapping);
    lua_setfield(L, -2, "started_overlapping");
    lua_pushnumber(L, (lua_Number)hit->penetration);
    lua_setfield(L, -2, "penetration");
}

static le_world *le_cast_world(lua_State *L) {
    le_script_runtime *rt = le_lua_current_runtime(L);

    if (rt == NULL || rt->firing_world == NULL) {
        luaL_error(L, "cast outside dispatch context");
        return NULL;
    }
    return rt->firing_world;
}

/* Physics.sphere_cast(cx,cy,cz, r, dx,dy,dz[, mask]) -> hit|nil */
static int le_p_sphere_cast(lua_State *L) {
    le_world *w = le_cast_world(L);
    float c[3];
    float d[3];
    float r;
    uint32_t mask = 0xFFFFFFFFu;
    le_shape_hit hit;
    int have;

    if (w == NULL) {
        return 0;
    }
    c[0] = (float)luaL_checknumber(L, 1);
    c[1] = (float)luaL_checknumber(L, 2);
    c[2] = (float)luaL_checknumber(L, 3);
    r = (float)luaL_checknumber(L, 4);
    d[0] = (float)luaL_checknumber(L, 5);
    d[1] = (float)luaL_checknumber(L, 6);
    d[2] = (float)luaL_checknumber(L, 7);
    if (!lua_isnoneornil(L, 8)) {
        mask = (uint32_t)luaL_checkinteger(L, 8);
    }
    memset(&hit, 0, sizeof(hit));
    have = le_physics_sphere_cast(w, c, r, d, mask, 0,
                                  0xFFFFFFFFu, &hit);
    le_push_shape_hit(L, w, have, &hit);
    return 1;
}

/* Physics.capsule_cast(cx,cy,cz, qx,qy,qz,qw, r, half,
 * dx,dy,dz[, mask]) -> hit|nil */
static int le_p_capsule_cast(lua_State *L) {
    le_world *w = le_cast_world(L);
    float c[3];
    float q[4];
    float d[3];
    float r;
    float h;
    uint32_t mask = 0xFFFFFFFFu;
    le_shape_hit hit;
    int have;

    if (w == NULL) {
        return 0;
    }
    c[0] = (float)luaL_checknumber(L, 1);
    c[1] = (float)luaL_checknumber(L, 2);
    c[2] = (float)luaL_checknumber(L, 3);
    q[0] = (float)luaL_checknumber(L, 4);
    q[1] = (float)luaL_checknumber(L, 5);
    q[2] = (float)luaL_checknumber(L, 6);
    q[3] = (float)luaL_checknumber(L, 7);
    r = (float)luaL_checknumber(L, 8);
    h = (float)luaL_checknumber(L, 9);
    d[0] = (float)luaL_checknumber(L, 10);
    d[1] = (float)luaL_checknumber(L, 11);
    d[2] = (float)luaL_checknumber(L, 12);
    if (!lua_isnoneornil(L, 13)) {
        mask = (uint32_t)luaL_checkinteger(L, 13);
    }
    memset(&hit, 0, sizeof(hit));
    have = le_physics_capsule_cast(w, c, q, r, h, d, mask, 0,
                                   0xFFFFFFFFu, &hit);
    le_push_shape_hit(L, w, have, &hit);
    return 1;
}

/* Physics.box_cast(cx,cy,cz, qx,qy,qz,qw, hx,hy,hz,
 * dx,dy,dz[, mask]) -> hit|nil */
static int le_p_box_cast(lua_State *L) {
    le_world *w = le_cast_world(L);
    float c[3];
    float q[4];
    float he[3];
    float d[3];
    uint32_t mask = 0xFFFFFFFFu;
    le_shape_hit hit;
    int have;

    if (w == NULL) {
        return 0;
    }
    c[0] = (float)luaL_checknumber(L, 1);
    c[1] = (float)luaL_checknumber(L, 2);
    c[2] = (float)luaL_checknumber(L, 3);
    q[0] = (float)luaL_checknumber(L, 4);
    q[1] = (float)luaL_checknumber(L, 5);
    q[2] = (float)luaL_checknumber(L, 6);
    q[3] = (float)luaL_checknumber(L, 7);
    he[0] = (float)luaL_checknumber(L, 8);
    he[1] = (float)luaL_checknumber(L, 9);
    he[2] = (float)luaL_checknumber(L, 10);
    d[0] = (float)luaL_checknumber(L, 11);
    d[1] = (float)luaL_checknumber(L, 12);
    d[2] = (float)luaL_checknumber(L, 13);
    if (!lua_isnoneornil(L, 14)) {
        mask = (uint32_t)luaL_checkinteger(L, 14);
    }
    memset(&hit, 0, sizeof(hit));
    have = le_physics_box_cast(w, c, q, he, d, mask, 0,
                               0xFFFFFFFFu, &hit);
    le_push_shape_hit(L, w, have, &hit);
    return 1;
}

/* Physics.set_collision_mode(self, "discrete"|"continuous") */
static int le_p_set_collision_mode(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    const char *m = luaL_checkstring(L, 2);
    le_collision_mode mode = LE_COLLISION_DISCRETE;

    le_ch_mutable(L, &w, &o);
    if (strcmp(m, "continuous") == 0) {
        mode = LE_COLLISION_CONTINUOUS;
    } else if (strcmp(m, "discrete") == 0) {
        mode = LE_COLLISION_DISCRETE;
    } else {
        return luaL_error(L, "bad collision mode");
    }
    if (le_physics_set_collision_mode(w, &o, mode) !=
        LE_SUCCESS) {
        return luaL_error(L, "set_collision_mode failed");
    }
    return 0;
}

void le_lua_register_cast_queries(lua_State *L) {
    lua_getglobal(L, "Physics");
    if (!lua_istable(L, -1)) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_setglobal(L, "Physics");
        lua_getglobal(L, "Physics");
    }
    lua_pushcfunction(L, le_p_sphere_cast);
    lua_setfield(L, -2, "sphere_cast");
    lua_pushcfunction(L, le_p_capsule_cast);
    lua_setfield(L, -2, "capsule_cast");
    lua_pushcfunction(L, le_p_box_cast);
    lua_setfield(L, -2, "box_cast");
    lua_pushcfunction(L, le_p_set_collision_mode);
    lua_setfield(L, -2, "set_collision_mode");
    lua_pop(L, 1);
}
