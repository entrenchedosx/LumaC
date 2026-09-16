/*
 * Lua object bindings (Phase 26): methods on le_object userdata.
 * Thin over le_*; cross-world misuse is rejected (world tags
 * authoritative); stale handles raise catchable script errors.
 * Property sugar (self.speed) resolves DECLARED exports only.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "script/script_internal.h"

/* le_o_set_parent forwards nil as a detach. le_object_reparent
 * rejects LE_OBJECT_INVALID-shaped parent args as stale (it only
 * accepts NULL for detach), and the binding layer speaks
 * reparent() — so nil maps to le_object_set_parent(world, child,
 * NULL) directly. */

/* Fetch (world, obj), raising on stale. Cross-world-vs-firing
 * check is the caller's policy (mutations require firing world). */
static int le_obj_live(lua_State *L, le_world **w, le_object *o) {
    if (!le_lua_check_object(L, 1, w, o)) {
        return luaL_error(L, "stale object handle");
    }
    return 1;
}

static int le_obj_mutable(lua_State *L, le_world **w, le_object *o) {
    le_script_runtime *rt = le_lua_current_runtime(L);

    le_obj_live(L, w, o);
    if (rt == NULL || rt->firing_world != *w) {
        return luaL_error(L, "cross-world object misuse");
    }
    if ((*w)->scripts_tearing_down) {
        return luaL_error(L, "world is shutting down");
    }
    return 1;
}

static int le_o_name(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_obj_live(L, &w, &o);
    lua_pushstring(L, le_object_get_name(w, &o));
    return 1;
}

static int le_o_set_name(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    const char *name = luaL_checkstring(L, 2);

    le_obj_mutable(L, &w, &o);
    if (le_object_set_name(w, &o, name) != LE_SUCCESS) {
        return luaL_error(L, "set_name failed");
    }
    return 0;
}

static int le_o_is_alive(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    if (!le_lua_check_object(L, 1, &w, &o)) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, le_object_is_alive(w, &o) ? 1 : 0);
    return 1;
}

static int le_o_is_enabled(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_obj_live(L, &w, &o);
    lua_pushboolean(L, le_object_is_enabled(w, &o) ? 1 : 0);
    return 1;
}

static int le_o_set_enabled(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    luaL_checktype(L, 2, LUA_TBOOLEAN);
    le_obj_mutable(L, &w, &o);
    if (le_object_set_enabled(w, &o, lua_toboolean(L, 2)) !=
        LE_SUCCESS) {
        return luaL_error(L, "set_enabled failed");
    }
    return 0;
}

static int le_o_parent(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    le_object p = LE_OBJECT_INVALID;

    le_obj_live(L, &w, &o);
    if (le_object_get_parent(w, &o, &p)) {
        le_lua_push_object(L, w, &p);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int le_o_set_parent(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    int mode = LE_REPARENT_KEEP_LOCAL;

    le_obj_mutable(L, &w, &o);
    if (lua_gettop(L) >= 3 && !lua_isnil(L, 3)) {
        const char *m = luaL_checkstring(L, 3);

        if (strcmp(m, "keep_world") == 0) {
            mode = LE_REPARENT_KEEP_WORLD;
        } else if (strcmp(m, "keep_local") == 0) {
            mode = LE_REPARENT_KEEP_LOCAL;
        } else {
            return luaL_error(L, "bad reparent mode");
        }
    }
    if (lua_isnil(L, 2)) {
        le_result rc =
            le_object_set_parent(w, &o, NULL);

        if (rc != LE_SUCCESS) {
            return luaL_error(L, "set_parent failed (%d)",
                              (int)rc);
        }
        return 0;
    }
    {
        le_world *pw = NULL;
        le_object p = LE_OBJECT_INVALID;

        if (!le_lua_check_object(L, 2, &pw, &p)) {
            return luaL_error(L, "stale parent handle");
        }
        if (pw != w) {
            return luaL_error(L, "cross-world parent");
        }
        {
            le_result rc = le_object_reparent(w, &o, &p,
                                              (le_reparent_mode)mode);

            if (rc == LE_ERROR_UNREPRESENTABLE_TRANSFORM) {
                return luaL_error(
                    L, "keep_world shear/singular (unrepresentable)");
            }
            if (rc != LE_SUCCESS) {
                return luaL_error(L, "set_parent failed (%d)",
                                  (int)rc);
            }
        }
    }
    return 0;
}

/* --- transforms (local explicit; world read-only) --- */

static int le_o_position(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float p[3];

    le_obj_live(L, &w, &o);
    le_object_get_position(w, &o, p);
    lua_pushnumber(L, p[0]);
    lua_pushnumber(L, p[1]);
    lua_pushnumber(L, p[2]);
    return 3;
}

static int le_o_set_position(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float p[3];

    p[0] = (float)luaL_checknumber(L, 2);
    p[1] = (float)luaL_checknumber(L, 3);
    p[2] = (float)luaL_checknumber(L, 4);
    le_obj_mutable(L, &w, &o);
    if (le_object_set_position(w, &o, p) != LE_SUCCESS) {
        return luaL_error(L, "set_position failed");
    }
    return 0;
}

static int le_o_world_position(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float m[16];

    le_obj_live(L, &w, &o);
    le_object_get_world_matrix(w, &o, m);
    lua_pushnumber(L, m[12]);
    lua_pushnumber(L, m[13]);
    lua_pushnumber(L, m[14]);
    return 3;
}

static int le_o_translate(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float p[3];
    float d[3];

    d[0] = (float)luaL_checknumber(L, 2);
    d[1] = (float)luaL_checknumber(L, 3);
    d[2] = (float)luaL_checknumber(L, 4);
    le_obj_mutable(L, &w, &o);
    le_object_get_position(w, &o, p);
    p[0] += d[0];
    p[1] += d[1];
    p[2] += d[2];
    if (le_object_set_position(w, &o, p) != LE_SUCCESS) {
        return luaL_error(L, "translate failed");
    }
    return 0;
}

static int le_o_set_scale(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float s[3];

    s[0] = (float)luaL_checknumber(L, 2);
    s[1] = (float)luaL_checknumber(L, 3);
    s[2] = (float)luaL_checknumber(L, 4);
    le_obj_mutable(L, &w, &o);
    if (le_object_set_scale(w, &o, s) != LE_SUCCESS) {
        return luaL_error(L, "set_scale failed");
    }
    return 0;
}

static int le_o_rotate_axis_angle(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float axis[3];
    float angle;
    float q[4];
    float cur[4];
    float out[4];

    axis[0] = (float)luaL_checknumber(L, 2);
    axis[1] = (float)luaL_checknumber(L, 3);
    axis[2] = (float)luaL_checknumber(L, 4);
    angle = (float)luaL_checknumber(L, 5);
    le_obj_mutable(L, &w, &o);
    le_quat_from_axis_angle(axis, angle, q);
    le_object_get_rotation(w, &o, cur);
    le_quat_multiply(q, cur, out);
    if (le_object_set_rotation(w, &o, out) != LE_SUCCESS) {
        return luaL_error(L, "rotate failed");
    }
    return 0;
}

static int le_o_rotate_y(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    float angle = (float)luaL_checknumber(L, 2);
    float axis[3] = { 0.0f, 1.0f, 0.0f };
    float q[4];
    float cur[4];
    float out[4];

    le_obj_mutable(L, &w, &o);
    le_quat_from_axis_angle(axis, angle, q);
    le_object_get_rotation(w, &o, cur);
    le_quat_multiply(q, cur, out);
    if (le_object_set_rotation(w, &o, out) != LE_SUCCESS) {
        return luaL_error(L, "rotate_y failed");
    }
    return 0;
}

/* --- components --- */

static int le_o_has_component(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    const char *name = luaL_checkstring(L, 2);
    le_component_type t = LE_COMPONENT_COUNT;

    le_obj_live(L, &w, &o);
    if (name != NULL) {
        if (strcmp(name, "transform") == 0) {
            t = LE_COMPONENT_TRANSFORM;
        } else if (strcmp(name, "renderable") == 0) {
            t = LE_COMPONENT_RENDERABLE;
        } else if (strcmp(name, "camera") == 0) {
            t = LE_COMPONENT_CAMERA;
        } else if (strcmp(name, "light") == 0) {
            t = LE_COMPONENT_LIGHT;
        } else if (strcmp(name, "script") == 0) {
            t = LE_COMPONENT_SCRIPT;
        }
    }
    if (t == LE_COMPONENT_COUNT) {
        return luaL_error(L, "unknown component");
    }
    lua_pushboolean(L, le_object_has_component(w, &o, t) ? 1 : 0);
    return 1;
}

static int le_o_set_renderable(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    le_engine *e = NULL;
    le_asset mesh = LE_ASSET_INVALID;
    le_asset mat = LE_ASSET_INVALID;
    le_asset_renderable_desc rd;

    le_obj_mutable(L, &w, &o);
    if (!le_lua_check_asset(L, 2, &e, &mesh) || e != w->engine) {
        return luaL_error(L, "stale mesh asset");
    }
    if (!le_lua_check_asset(L, 3, &e, &mat) || e != w->engine) {
        return luaL_error(L, "stale material asset");
    }
    memset(&rd, 0, sizeof(rd));
    rd.mesh = mesh;
    rd.material = mat;
    rd.visible = 1;
    rd.receives_shadow = 1;
    {
        le_result rc =
            le_object_add_asset_renderable(w, &o, &rd);

        if (rc != LE_SUCCESS) {
            return luaL_error(L, "set_renderable failed (%d)",
                              (int)rc);
        }
    }
    return 0;
}

static int le_o_remove_renderable(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_obj_mutable(L, &w, &o);
    le_object_remove_asset_renderable(w, &o);
    le_object_remove_renderable(w, &o);
    return 0;
}

/* self:get(name) / self:set(name, value): explicit property API. */
static int le_o_get_prop(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    const char *name = luaL_checkstring(L, 2);
    le_script_property prop;

    le_obj_live(L, &w, &o);
    memset(&prop, 0, sizeof(prop));
    snprintf(prop.name, sizeof(prop.name), "%s",
             (name != NULL) ? name : "");
    /* Type-agnostic read: fetch declared type first. */
    {
        le_script_property probe;

        memset(&probe, 0, sizeof(probe));
        /* list + match to discover the type */
        le_script_property all[16];
        uint32_t count = 0;
        uint32_t i;

        if (!le_script_list_properties(w, &o, all, 16, &count)) {
            return luaL_error(L, "no script");
        }
        for (i = 0; i < count; i++) {
            if (strcmp(all[i].name, prop.name) == 0) {
                prop.type = all[i].type;
                if (!le_script_get_property(w, &o, prop.name,
                                            &prop)) {
                    return luaL_error(L, "get failed");
                }
                if (prop.type == LE_SCRIPT_PROP_ASSET) {
                    le_lua_push_asset(L, w->engine,
                                      &prop.asset);
                    return 1;
                }
                le_script_prop_to_lua(L, &prop);
                return 1;
            }
        }
        return luaL_error(L, "unknown property");
    }
}

static int le_o_set_prop(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    const char *name = luaL_checkstring(L, 2);
    le_script_property prop;

    le_obj_mutable(L, &w, &o);
    memset(&prop, 0, sizeof(prop));
    snprintf(prop.name, sizeof(prop.name), "%s",
             (name != NULL) ? name : "");
    /* Discover declared type, then convert the value. */
    {
        le_script_property all[16];
        uint32_t count = 0;
        uint32_t i;
        int found = 0;

        if (!le_script_list_properties(w, &o, all, 16, &count)) {
            return luaL_error(L, "no script");
        }
        for (i = 0; i < count; i++) {
            if (strcmp(all[i].name, prop.name) == 0) {
                prop.type = all[i].type;
                found = 1;
                break;
            }
        }
        if (!found) {
            return luaL_error(L, "unknown property");
        }
        if (prop.type == LE_SCRIPT_PROP_ASSET) {
            le_engine *e = NULL;

            if (!le_lua_check_asset(L, 3, &e, &prop.asset) ||
                e != w->engine) {
                return luaL_error(L, "stale asset value");
            }
        } else {
            le_script_property conv;

            if (!le_script_prop_from_lua(L, 3, prop.name,
                                         &conv)) {
                return luaL_error(L, "bad property value");
            }
            /* Coerce numbers across int/number boundary. */
            if (conv.type != prop.type) {
                if (prop.type == LE_SCRIPT_PROP_NUMBER &&
                    conv.type == LE_SCRIPT_PROP_INT) {
                    prop.number = (double)conv.integer;
                } else if (prop.type == LE_SCRIPT_PROP_INT &&
                           conv.type == LE_SCRIPT_PROP_NUMBER) {
                    prop.integer = (int64_t)conv.number;
                } else {
                    return luaL_error(L, "type mismatch");
                }
            } else {
                prop = conv;
                snprintf(prop.name, sizeof(prop.name), "%s",
                         name);
            }
        }
        {
            le_result rc = le_script_set_property(w, &o, &prop);

            if (rc != LE_SUCCESS) {
                return luaL_error(L, "set failed (%d)", (int)rc);
            }
        }
    }
    return 0;
}

/* __index: methods first, then declared export sugar (self.speed).
 * Stack discipline: every path returns exactly 1 value with a
 * balanced stack. The method-table probe is popped on BOTH
 * outcomes (hit: the two tables; miss: tables + nil). */
static int le_o_index(lua_State *L) {
    const char *key = luaL_checkstring(L, 2);

    if (key == NULL) {
        lua_pushnil(L);
        return 1;
    }
    /* Method table on the metatable: [mt][methods][fn|nil]. */
    lua_getmetatable(L, 1);
    lua_getfield(L, -1, "__methods");
    lua_getfield(L, -1, key);
    if (!lua_isnil(L, -1)) {
        /* Hit: drop mt + methods, keep fn. */
        lua_remove(L, -3);
        lua_remove(L, -2);
        return 1;
    }
    lua_pop(L, 3); /* nil + methods + mt */
    /* Declared property sugar. */
    {
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;

        if (!le_lua_check_object(L, 1, &w, &o)) {
            return luaL_error(L, "stale object handle");
        }
        {
            le_script_property all[16];
            uint32_t count = 0;
            uint32_t i;

            if (le_script_list_properties(w, &o, all, 16,
                                          &count)) {
                for (i = 0; i < count; i++) {
                    if (strcmp(all[i].name, key) == 0) {
                        le_script_property prop;

                        memset(&prop, 0, sizeof(prop));
                        snprintf(prop.name, sizeof(prop.name),
                                 "%s", key);
                        prop.type = all[i].type;
                        if (!le_script_get_property(w, &o, key,
                                                    &prop)) {
                            return luaL_error(L, "get failed");
                        }
                        if (prop.type ==
                            LE_SCRIPT_PROP_ASSET) {
                            le_lua_push_asset(L, w->engine,
                                              &prop.asset);
                            return 1;
                        }
                        le_script_prop_to_lua(L, &prop);
                        return 1;
                    }
                }
            }
        }
    }
    lua_pushnil(L);
    return 1;
}

static int le_o_newindex(lua_State *L) {
    const char *key = luaL_checkstring(L, 2);

    if (key == NULL) {
        return luaL_error(L, "bad property name");
    }
    {
        le_world *w = NULL;
        le_object o = LE_OBJECT_INVALID;

        if (!le_lua_check_object(L, 1, &w, &o)) {
            return luaL_error(L, "stale object handle");
        }
        {
            le_script_property all[16];
            uint32_t count = 0;
            uint32_t i;

            if (le_script_list_properties(w, &o, all, 16,
                                          &count)) {
                for (i = 0; i < count; i++) {
                    if (strcmp(all[i].name, key) == 0) {
                        /* Inline set (no lua_call reentry):
                         * convert value -> property -> store. */
                        le_script_property prop;

                        memset(&prop, 0, sizeof(prop));
                        snprintf(prop.name, sizeof(prop.name),
                                 "%s", key);
                        prop.type = all[i].type;
                        if (prop.type ==
                            LE_SCRIPT_PROP_ASSET) {
                            le_engine *e = NULL;

                            if (!le_lua_check_asset(L, 3, &e,
                                                    &prop.asset) ||
                                e != w->engine) {
                                return luaL_error(
                                    L, "stale asset value");
                            }
                        } else {
                            le_script_property conv;

                            if (!le_script_prop_from_lua(
                                    L, 3, key, &conv)) {
                                return luaL_error(
                                    L, "bad property value");
                            }
                            if (conv.type != prop.type) {
                                if (prop.type ==
                                        LE_SCRIPT_PROP_NUMBER &&
                                    conv.type ==
                                        LE_SCRIPT_PROP_INT) {
                                    prop.number =
                                        (double)conv.integer;
                                } else if (
                                    prop.type ==
                                        LE_SCRIPT_PROP_INT &&
                                    conv.type ==
                                        LE_SCRIPT_PROP_NUMBER) {
                                    prop.integer =
                                        (int64_t)conv.number;
                                } else {
                                    return luaL_error(
                                        L, "type mismatch");
                                }
                            } else {
                                prop = conv;
                                snprintf(
                                    prop.name,
                                    sizeof(prop.name), "%s",
                                    key);
                            }
                        }
                        if (w->scripts_tearing_down) {
                            return luaL_error(
                                L, "world is shutting down");
                        }
                        if (le_script_set_property(w, &o,
                                                   &prop) !=
                            LE_SUCCESS) {
                            return luaL_error(L, "set failed");
                        }
                        return 0;
                    }
                }
            }
        }
    }
    return luaL_error(L, "undeclared property (use export())");
}

static int le_o_eq(lua_State *L) {
    const le_lua_object *a =
        (const le_lua_object *)luaL_testudata(L, 1,
                                              LE_LUA_OBJECT_MT);
    const le_lua_object *b =
        (const le_lua_object *)luaL_testudata(L, 2,
                                              LE_LUA_OBJECT_MT);

    if (a == NULL || b == NULL) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(
        L, (a->engine == b->engine &&
            a->world_tag == b->world_tag && a->index == b->index &&
            a->generation == b->generation)
               ? 1
               : 0);
    return 1;
}

static int le_o_tostring(lua_State *L) {
    const le_lua_object *a =
        (const le_lua_object *)luaL_testudata(L, 1,
                                              LE_LUA_OBJECT_MT);
    char buf[96];

    if (a == NULL) {
        lua_pushstring(L, "Object(?)");
        return 1;
    }
    snprintf(buf, sizeof(buf), "Object(%u:%u)",
             (unsigned)a->index, (unsigned)a->generation);
    lua_pushstring(L, buf);
    return 1;
}

static int le_a_eq(lua_State *L) {
    /* Asset equality: same engine + same index + same generation
     * (le_lua_asset layout, shared with scenes — never the object
     * layout). */
    const le_lua_asset *a =
        (const le_lua_asset *)luaL_testudata(L, 1, LE_LUA_ASSET_MT);
    const le_lua_asset *b =
        (const le_lua_asset *)luaL_testudata(L, 2, LE_LUA_ASSET_MT);

    if (a == NULL) {
        a = (const le_lua_asset *)luaL_testudata(L, 1,
                                                 LE_LUA_SCENE_MT);
    }
    if (b == NULL) {
        b = (const le_lua_asset *)luaL_testudata(L, 2,
                                                 LE_LUA_SCENE_MT);
    }
    if (a == NULL || b == NULL) {
        lua_pushboolean(L, 0);
        return 1;
    }
    lua_pushboolean(L, (a->engine == b->engine &&
                        a->index == b->index &&
                        a->generation == b->generation)
                           ? 1
                           : 0);
    return 1;
}

void le_lua_register_object(lua_State *L) {
    static const luaL_Reg methods[] = {
        { "name", le_o_name },
        { "set_name", le_o_set_name },
        { "is_alive", le_o_is_alive },
        { "is_enabled", le_o_is_enabled },
        { "set_enabled", le_o_set_enabled },
        { "parent", le_o_parent },
        { "set_parent", le_o_set_parent },
        { "position", le_o_position },
        { "set_position", le_o_set_position },
        { "world_position", le_o_world_position },
        { "translate", le_o_translate },
        { "set_scale", le_o_set_scale },
        { "rotate_axis_angle", le_o_rotate_axis_angle },
        { "rotate_y", le_o_rotate_y },
        { "has_component", le_o_has_component },
        { "set_renderable", le_o_set_renderable },
        { "remove_renderable", le_o_remove_renderable },
        { "get", le_o_get_prop },
        { "set", le_o_set_prop },
        { NULL, NULL },
    };

    luaL_newmetatable(L, LE_LUA_OBJECT_MT);
    lua_newtable(L);
    luaL_setfuncs(L, methods, 0);
    /* Phase 28: physics methods share the object method table
     * (same userdata, same staleness discipline). */
    le_lua_register_physics_methods(L);
    /* Phase 29: animation playback methods ride the same table
     * (thin over le_anim_*; no Lua-side state). */
    le_lua_register_anim_methods(L);
    /* Phase 30: character controller methods ride the same table
     * (thin over le_character_*; no Lua-side state). */
    le_lua_register_character_methods(L);
    lua_setfield(L, -2, "__methods");
    lua_pushcfunction(L, le_o_index);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, le_o_newindex);
    lua_setfield(L, -2, "__newindex");
    lua_pushcfunction(L, le_o_eq);
    lua_setfield(L, -2, "__eq");
    lua_pushcfunction(L, le_o_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);
    /* Asset + scene metatables (equality/tostring only; behavior
     * lives on World/Assets tables). */
    luaL_newmetatable(L, LE_LUA_ASSET_MT);
    lua_pushcfunction(L, le_a_eq);
    lua_setfield(L, -2, "__eq");
    lua_pop(L, 1);
    luaL_newmetatable(L, LE_LUA_SCENE_MT);
    lua_pushcfunction(L, le_a_eq);
    lua_setfield(L, -2, "__eq");
    lua_pop(L, 1);
}
