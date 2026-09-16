/*
 * Lua physics bindings (Phase 28): thin over le_physics_*.
 * No Lua-side physics state; collision/trigger callbacks arrive
 * through the VM-independent backend lifecycle (callback ids
 * 4..9); raycast/overlap return plain value tables.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "script/script_internal.h"

static int le_ph_live(lua_State *L, le_world **w, le_object *o) {
    if (!le_lua_check_object(L, 1, w, o)) {
        return luaL_error(L, "stale object handle");
    }
    return 1;
}

static int le_ph_mutable(lua_State *L, le_world **w, le_object *o) {
    le_script_runtime *rt = le_lua_current_runtime(L);

    le_ph_live(L, w, o);
    if (rt == NULL || rt->firing_world != *w) {
        return luaL_error(L, "cross-world object misuse");
    }
    if ((*w)->scripts_tearing_down) {
        return luaL_error(L, "world is shutting down");
    }
    return 1;
}

/* self:linear_velocity() -> x, y, z */
static int le_p_linear_velocity(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float v[3];

    le_ph_live(L, &w, &o);
    if (le_physics_get_linear_velocity(w, &o, v) != LE_SUCCESS) {
        return luaL_error(L, "no rigid body");
    }
    lua_pushnumber(L, (lua_Number)v[0]);
    lua_pushnumber(L, (lua_Number)v[1]);
    lua_pushnumber(L, (lua_Number)v[2]);
    return 3;
}

/* self:set_linear_velocity(x, y, z) */
static int le_p_set_linear_velocity(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float v[3];

    le_ph_mutable(L, &w, &o);
    v[0] = (float)luaL_checknumber(L, 2);
    v[1] = (float)luaL_checknumber(L, 3);
    v[2] = (float)luaL_checknumber(L, 4);
    if (le_physics_set_linear_velocity(w, &o, v[0], v[1],
                                       v[2]) != LE_SUCCESS) {
        return luaL_error(L, "set_linear_velocity failed");
    }
    return 0;
}

/* self:angular_velocity() -> x, y, z */
static int le_p_angular_velocity(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float wv[3];

    le_ph_live(L, &w, &o);
    if (le_physics_get_angular_velocity(w, &o, wv) !=
        LE_SUCCESS) {
        return luaL_error(L, "no rigid body");
    }
    lua_pushnumber(L, (lua_Number)wv[0]);
    lua_pushnumber(L, (lua_Number)wv[1]);
    lua_pushnumber(L, (lua_Number)wv[2]);
    return 3;
}

/* self:set_angular_velocity(x, y, z) */
static int le_p_set_angular_velocity(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float v[3];

    le_ph_mutable(L, &w, &o);
    v[0] = (float)luaL_checknumber(L, 2);
    v[1] = (float)luaL_checknumber(L, 3);
    v[2] = (float)luaL_checknumber(L, 4);
    if (le_physics_set_angular_velocity(w, &o, v[0], v[1],
                                        v[2]) != LE_SUCCESS) {
        return luaL_error(L, "set_angular_velocity failed");
    }
    return 0;
}

/* self:add_force(fx, fy, fz) */
static int le_p_add_force(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_ph_mutable(L, &w, &o);
    if (le_physics_add_force(w, &o, (float)luaL_checknumber(L, 2),
                             (float)luaL_checknumber(L, 3),
                             (float)luaL_checknumber(L, 4)) !=
        LE_SUCCESS) {
        return luaL_error(L, "add_force failed");
    }
    return 0;
}

/* self:add_torque(tx, ty, tz) */
static int le_p_add_torque(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_ph_mutable(L, &w, &o);
    if (le_physics_add_torque(w, &o,
                              (float)luaL_checknumber(L, 2),
                              (float)luaL_checknumber(L, 3),
                              (float)luaL_checknumber(L, 4)) !=
        LE_SUCCESS) {
        return luaL_error(L, "add_torque failed");
    }
    return 0;
}

/* self:apply_impulse(jx, jy, jz) */
static int le_p_apply_impulse(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_ph_mutable(L, &w, &o);
    if (le_physics_apply_impulse(w, &o,
                                 (float)luaL_checknumber(L, 2),
                                 (float)luaL_checknumber(L, 3),
                                 (float)luaL_checknumber(L, 4)) !=
        LE_SUCCESS) {
        return luaL_error(L, "apply_impulse failed");
    }
    return 0;
}

/* self:apply_impulse_at_point(jx,jy,jz, px,py,pz) */
static int le_p_apply_impulse_at_point(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_ph_mutable(L, &w, &o);
    if (le_physics_apply_impulse_at_point(
            w, &o, (float)luaL_checknumber(L, 2),
            (float)luaL_checknumber(L, 3),
            (float)luaL_checknumber(L, 4),
            (float)luaL_checknumber(L, 5),
            (float)luaL_checknumber(L, 6),
            (float)luaL_checknumber(L, 7)) != LE_SUCCESS) {
        return luaL_error(L, "apply_impulse_at_point failed");
    }
    return 0;
}

/* self:add_rigid_body{type=, mass=, ...} (table arg; missing =
 * static defaults). Type strings: "static"|"dynamic"|"kinematic". */
static int le_p_add_rigid_body(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    le_rigid_body_desc d;

    le_ph_mutable(L, &w, &o);
    if (!lua_istable(L, 2)) {
        return luaL_argerror(L, 2, "expected table");
    }
    memset(&d, 0, sizeof(d));
    d.type = LE_BODY_DYNAMIC;
    d.mass = 1.0f;
    d.gravity_scale = 1.0f;
    lua_getfield(L, 2, "type");
    if (lua_isstring(L, -1)) {
        const char *t = lua_tostring(L, -1);

        if (strcmp(t, "static") == 0) {
            d.type = LE_BODY_STATIC;
        } else if (strcmp(t, "dynamic") == 0) {
            d.type = LE_BODY_DYNAMIC;
        } else if (strcmp(t, "kinematic") == 0) {
            d.type = LE_BODY_KINEMATIC;
        } else {
            return luaL_error(L, "bad body type");
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "mass");
    if (lua_isnumber(L, -1)) {
        d.mass = (float)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "linear_damping");
    if (lua_isnumber(L, -1)) {
        d.linear_damping = (float)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "angular_damping");
    if (lua_isnumber(L, -1)) {
        d.angular_damping = (float)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "gravity_scale");
    if (lua_isnumber(L, -1)) {
        d.gravity_scale = (float)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    if (le_object_add_rigid_body(w, &o, &d) != LE_SUCCESS) {
        return luaL_error(L, "add_rigid_body failed");
    }
    return 0;
}

/* self:add_collider{shape=, radius=, half_extents={...}, ...} */
static int le_p_add_collider(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    le_collider_desc d;

    le_ph_mutable(L, &w, &o);
    if (!lua_istable(L, 2)) {
        return luaL_argerror(L, 2, "expected table");
    }
    memset(&d, 0, sizeof(d));
    d.shape = LE_COLLIDER_BOX;
    d.half_extents[0] = d.half_extents[1] = d.half_extents[2] =
        0.5f;
    d.orientation[3] = 1.0f;
    d.mask = 0xFFFFFFFFu;
    d.friction = 0.5f;
    lua_getfield(L, 2, "shape");
    if (lua_isstring(L, -1)) {
        const char *t = lua_tostring(L, -1);

        if (strcmp(t, "sphere") == 0) {
            d.shape = LE_COLLIDER_SPHERE;
        } else if (strcmp(t, "box") == 0) {
            d.shape = LE_COLLIDER_BOX;
        } else {
            return luaL_error(L, "bad collider shape");
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "radius");
    if (lua_isnumber(L, -1)) {
        d.radius = (float)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "half_extents");
    if (lua_istable(L, -1)) {
        int k;

        for (k = 0; k < 3; k++) {
            lua_rawgeti(L, -1, k + 1);
            if (lua_isnumber(L, -1)) {
                d.half_extents[k] = (float)lua_tonumber(L, -1);
            }
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "is_trigger");
    if (!lua_isnil(L, -1)) {
        d.is_trigger = lua_toboolean(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "friction");
    if (lua_isnumber(L, -1)) {
        d.friction = (float)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    lua_getfield(L, 2, "restitution");
    if (lua_isnumber(L, -1)) {
        d.restitution = (float)lua_tonumber(L, -1);
    }
    lua_pop(L, 1);
    if (le_object_add_collider(w, &o, &d) != LE_SUCCESS) {
        return luaL_error(L, "add_collider failed");
    }
    return 0;
}

/* Physics.gravity() -> x, y, z */
static int le_ph_gravity(lua_State *L) {
    le_script_runtime *rt = le_lua_current_runtime(L);
    le_world *w = (rt != NULL) ? rt->firing_world : NULL;
    float g[3];

    if (w == NULL) {
        return luaL_error(L, "Physics outside dispatch");
    }
    le_physics_get_gravity(w, g);
    lua_pushnumber(L, (lua_Number)g[0]);
    lua_pushnumber(L, (lua_Number)g[1]);
    lua_pushnumber(L, (lua_Number)g[2]);
    return 3;
}

/* Physics.set_gravity(x, y, z) */
static int le_ph_set_gravity(lua_State *L) {
    le_script_runtime *rt = le_lua_current_runtime(L);
    le_world *w = (rt != NULL) ? rt->firing_world : NULL;

    if (w == NULL) {
        return luaL_error(L, "Physics outside dispatch");
    }
    if (le_physics_set_gravity(w, (float)luaL_checknumber(L, 1),
                               (float)luaL_checknumber(L, 2),
                               (float)luaL_checknumber(L, 3)) !=
        LE_SUCCESS) {
        return luaL_error(L, "set_gravity failed");
    }
    return 0;
}

/* Push one le_object handle (reuse the object userdata path). */
static void le_push_other(lua_State *L, le_world *w,
                          const le_object *o) {
    le_lua_push_object(L, w, o);
}

/* Physics.raycast(ox,oy,oz, dx,dy,dz, max_dist[, mask, triggers])
 * -> hit table {object, point={x,y,z}, normal={...}, distance}
 * | nil. */
static int le_ph_raycast(lua_State *L) {
    le_script_runtime *rt = le_lua_current_runtime(L);
    le_world *w = (rt != NULL) ? rt->firing_world : NULL;
    uint32_t mask = 0xFFFFFFFFu;
    int triggers = 0;
    le_ray_hit hit;

    if (w == NULL) {
        return luaL_error(L, "Physics outside dispatch");
    }
    if (lua_gettop(L) >= 8 && !lua_isnil(L, 8)) {
        mask = (uint32_t)luaL_checkinteger(L, 8);
    }
    if (lua_gettop(L) >= 9 && !lua_isnil(L, 9)) {
        triggers = lua_toboolean(L, 9);
    }
    if (!le_physics_raycast(w, (float)luaL_checknumber(L, 1),
                            (float)luaL_checknumber(L, 2),
                            (float)luaL_checknumber(L, 3),
                            (float)luaL_checknumber(L, 4),
                            (float)luaL_checknumber(L, 5),
                            (float)luaL_checknumber(L, 6),
                            (float)luaL_checknumber(L, 7), mask,
                            triggers, &hit)) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    le_push_other(L, w, &hit.object);
    lua_setfield(L, -2, "object");
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)hit.point[0]);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, (lua_Number)hit.point[1]);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, (lua_Number)hit.point[2]);
    lua_rawseti(L, -2, 3);
    lua_setfield(L, -2, "point");
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)hit.normal[0]);
    lua_rawseti(L, -2, 1);
    lua_pushnumber(L, (lua_Number)hit.normal[1]);
    lua_rawseti(L, -2, 2);
    lua_pushnumber(L, (lua_Number)hit.normal[2]);
    lua_rawseti(L, -2, 3);
    lua_setfield(L, -2, "normal");
    lua_pushnumber(L, (lua_Number)hit.distance);
    lua_setfield(L, -2, "distance");
    return 1;
}

/* Object methods registered onto the object metatable method
 * table by name (called from le_lua_register_object's table
 * builder — see below). */
static const luaL_Reg kPhysicsMethods[] = {
    { "linear_velocity", le_p_linear_velocity },
    { "set_linear_velocity", le_p_set_linear_velocity },
    { "angular_velocity", le_p_angular_velocity },
    { "set_angular_velocity", le_p_set_angular_velocity },
    { "add_force", le_p_add_force },
    { "add_torque", le_p_add_torque },
    { "apply_impulse", le_p_apply_impulse },
    { "apply_impulse_at_point", le_p_apply_impulse_at_point },
    { "add_rigid_body", le_p_add_rigid_body },
    { "add_collider", le_p_add_collider },
    { NULL, NULL },
};

void le_lua_register_physics_methods(lua_State *L) {
    /* Stack: [methods table]. Appends physics methods (object
     * metatable builder calls this before sealing __index). */
    luaL_setfuncs(L, kPhysicsMethods, 0);
}

void le_lua_register_physics(lua_State *L) {
    static const luaL_Reg physics_fn[] = {
        { "gravity", le_ph_gravity },
        { "set_gravity", le_ph_set_gravity },
        { "raycast", le_ph_raycast },
        { NULL, NULL },
    };

    lua_newtable(L);
    luaL_setfuncs(L, physics_fn, 0);
    lua_setglobal(L, "Physics");
    /* Phase 30: shape-cast queries share the Physics table. */
    le_lua_register_cast_queries(L);
}
