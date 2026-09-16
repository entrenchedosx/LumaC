/*
 * Lua animation bindings (Phase 29, script backend): thin over
 * le_anim_*. No Lua-side animation state; every call resolves
 * live handles and delegates to the engine. Object methods only
 * (no global table — animation is per-object playback, like
 * physics forces). Registered into the object method table by
 * le_lua_register_object.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "script/script_internal.h"

static int le_an_live(lua_State *L, le_world **w, le_object *o) {
    if (!le_lua_check_object(L, 1, w, o)) {
        return luaL_error(L, "stale object handle");
    }
    return 1;
}

static int le_an_mutable(lua_State *L, le_world **w, le_object *o) {
    le_script_runtime *rt = le_lua_current_runtime(L);

    le_an_live(L, w, o);
    if (rt == NULL || rt->firing_world != *w) {
        return luaL_error(L, "cross-world object misuse");
    }
    if ((*w)->scripts_tearing_down) {
        return luaL_error(L, "world is shutting down");
    }
    return 1;
}

static int le_lua_check_anim_asset(lua_State *L, int idx,
                                   le_world *w, le_asset *out) {
    le_engine *e = NULL;
    le_asset a = LE_ASSET_INVALID;

    if (!le_lua_check_asset(L, idx, &e, &a) || e != w->engine) {
        return 0;
    }
    *out = a;
    return 1;
}

/* self:animation_play(clip [, restart]) */
static int le_a_play(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    le_asset clip = LE_ASSET_INVALID;
    int restart = 0;

    le_an_mutable(L, &w, &o);
    /* Absent args (LUA_TNONE) behave as nil: bare
     * animation_play() continues the current clip. */
    if (!lua_isnoneornil(L, 2)) {
        if (!le_lua_check_anim_asset(L, 2, w, &clip)) {
            return luaL_error(L, "stale clip asset");
        }
    }
    if (!lua_isnoneornil(L, 3)) {
        restart = lua_toboolean(L, 3);
    }
    if (le_anim_play(w, &o, lua_isnoneornil(L, 2) ? NULL : &clip,
                     restart) != LE_SUCCESS) {
        return luaL_error(L, "animation_play failed");
    }
    return 0;
}

/* self:animation_pause() / resume() / stop([reset]) */
static int le_a_pause(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_an_mutable(L, &w, &o);
    if (le_anim_pause(w, &o) != LE_SUCCESS) {
        return luaL_error(L, "animation_pause failed");
    }
    return 0;
}

static int le_a_resume(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_an_mutable(L, &w, &o);
    if (le_anim_resume(w, &o) != LE_SUCCESS) {
        return luaL_error(L, "animation_resume failed");
    }
    return 0;
}

static int le_a_stop(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    int reset = 0;

    le_an_mutable(L, &w, &o);
    if (!lua_isnoneornil(L, 2)) {
        reset = lua_toboolean(L, 2);
    }
    if (le_anim_stop(w, &o, reset) != LE_SUCCESS) {
        return luaL_error(L, "animation_stop failed");
    }
    return 0;
}

/* self:animation_seek(t) */
static int le_a_seek(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_an_mutable(L, &w, &o);
    if (le_anim_seek(w, &o,
                     (float)luaL_checknumber(L, 2)) !=
        LE_SUCCESS) {
        return luaL_error(L, "animation_seek failed");
    }
    return 0;
}

/* self:animation_speed([s]) -> current when no arg */
static int le_a_speed(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    le_animator_desc d;

    le_an_live(L, &w, &o);
    if (lua_isnoneornil(L, 2)) {
        memset(&d, 0, sizeof(d));
        if (!le_object_get_animator(w, &o, &d)) {
            return luaL_error(L, "no animator");
        }
        lua_pushnumber(L, (lua_Number)d.speed);
        return 1;
    }
    le_an_mutable(L, &w, &o);
    if (le_anim_set_speed(w, &o,
                          (float)luaL_checknumber(L, 2)) !=
        LE_SUCCESS) {
        return luaL_error(L, "animation_speed failed");
    }
    return 0;
}

/* self:animation_crossfade(clip, duration) */
static int le_a_crossfade(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;
    le_asset clip = LE_ASSET_INVALID;

    le_an_mutable(L, &w, &o);
    if (!le_lua_check_anim_asset(L, 2, w, &clip)) {
        return luaL_error(L, "stale clip asset");
    }
    if (le_anim_crossfade(w, &o, &clip,
                          (float)luaL_checknumber(L, 3)) !=
        LE_SUCCESS) {
        return luaL_error(L, "animation_crossfade failed");
    }
    return 0;
}

/* self:animation_is_playing() -> bool */
static int le_a_is_playing(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_an_live(L, &w, &o);
    lua_pushboolean(L, le_anim_is_playing(w, &o) ? 1 : 0);
    return 1;
}

/* self:animation_time() -> number */
static int le_a_time(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_an_live(L, &w, &o);
    lua_pushnumber(L, (lua_Number)le_anim_get_time(w, &o));
    return 1;
}

/* self:animation_duration() -> number */
static int le_a_duration(lua_State *L) {
    le_world *w = NULL;
    le_object o = LE_OBJECT_INVALID;

    le_an_live(L, &w, &o);
    lua_pushnumber(L,
                   (lua_Number)le_anim_get_duration(w, &o));
    return 1;
}

static const luaL_Reg kAnimMethods[] = {
    { "animation_play", le_a_play },
    { "animation_pause", le_a_pause },
    { "animation_resume", le_a_resume },
    { "animation_stop", le_a_stop },
    { "animation_seek", le_a_seek },
    { "animation_speed", le_a_speed },
    { "animation_crossfade", le_a_crossfade },
    { "animation_is_playing", le_a_is_playing },
    { "animation_time", le_a_time },
    { "animation_duration", le_a_duration },
    { NULL, NULL },
};

void le_lua_register_anim_methods(lua_State *L) {
    /* Stack: [methods table]. Appends animation methods. */
    luaL_setfuncs(L, kAnimMethods, 0);
}
