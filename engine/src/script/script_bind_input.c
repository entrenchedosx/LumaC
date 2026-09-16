/*
 * Lua Input/Time/Key/Mouse bindings (Phase 27). Thin over the
 * engine C APIs — no Lua-side input or time state machine exists.
 * All scripts in one world/frame observe the same finalized
 * snapshot (engine input state is per-engine, advanced once per
 * frame before dispatch). dt passed to update() is exactly
 * Time.delta() (both read the engine time state).
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "script/script_internal.h"

static le_engine *le_bind_engine(lua_State *L) {
    le_script_runtime *rt = le_lua_current_runtime(L);
    le_world *w = (rt != NULL) ? rt->firing_world : NULL;

    return (w != NULL) ? w->engine : NULL;
}

/* Resolve an le_key from arg 1 (integer key id). */
static le_key le_check_key(lua_State *L, int idx) {
    lua_Integer v = luaL_checkinteger(L, idx);

    if (v <= LE_KEY_UNKNOWN || v >= LE_KEY_COUNT) {
        luaL_argerror(L, idx, "unknown key");
    }
    return (le_key)v;
}

static le_mouse_button le_check_mouse(lua_State *L, int idx) {
    lua_Integer v = luaL_checkinteger(L, idx);

    if (v < 0 || v >= LE_MOUSE_BUTTON_COUNT) {
        luaL_argerror(L, idx, "unknown mouse button");
    }
    return (le_mouse_button)v;
}

/* Input.key_down(key) -> bool */
static int le_i_key_down(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Input outside dispatch");
    }
    lua_pushboolean(L, le_input_key_down(e, le_check_key(L, 1)));
    return 1;
}

static int le_i_key_pressed(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Input outside dispatch");
    }
    lua_pushboolean(L,
                    le_input_key_pressed(e, le_check_key(L, 1)));
    return 1;
}

static int le_i_key_released(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Input outside dispatch");
    }
    lua_pushboolean(L,
                    le_input_key_released(e, le_check_key(L, 1)));
    return 1;
}

static int le_i_mouse_down(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Input outside dispatch");
    }
    lua_pushboolean(L,
                    le_input_mouse_down(e, le_check_mouse(L, 1)));
    return 1;
}

static int le_i_mouse_pressed(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Input outside dispatch");
    }
    lua_pushboolean(L,
                    le_input_mouse_pressed(e, le_check_mouse(L, 1)));
    return 1;
}

static int le_i_mouse_released(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Input outside dispatch");
    }
    lua_pushboolean(L, le_input_mouse_released(
                           e, le_check_mouse(L, 1)));
    return 1;
}

/* Input.mouse_position() -> x, y */
static int le_i_mouse_position(lua_State *L) {
    le_engine *e = le_bind_engine(L);
    float x = 0.0f;
    float y = 0.0f;

    if (e == NULL) {
        return luaL_error(L, "Input outside dispatch");
    }
    le_input_mouse_position(e, &x, &y);
    lua_pushnumber(L, (lua_Number)x);
    lua_pushnumber(L, (lua_Number)y);
    return 2;
}

/* Input.mouse_delta() -> dx, dy */
static int le_i_mouse_delta(lua_State *L) {
    le_engine *e = le_bind_engine(L);
    float x = 0.0f;
    float y = 0.0f;

    if (e == NULL) {
        return luaL_error(L, "Input outside dispatch");
    }
    le_input_mouse_delta(e, &x, &y);
    lua_pushnumber(L, (lua_Number)x);
    lua_pushnumber(L, (lua_Number)y);
    return 2;
}

/* Input.scroll_delta() -> dx, dy */
static int le_i_scroll_delta(lua_State *L) {
    le_engine *e = le_bind_engine(L);
    float x = 0.0f;
    float y = 0.0f;

    if (e == NULL) {
        return luaL_error(L, "Input outside dispatch");
    }
    le_input_scroll_delta(e, &x, &y);
    lua_pushnumber(L, (lua_Number)x);
    lua_pushnumber(L, (lua_Number)y);
    return 2;
}

/* Resolve an action by name (convenience; hosts cache handles in
 * C — Lua pays one hash lookup per call, no per-frame scans of
 * unrelated entries). */
static int le_resolve_action_arg(lua_State *L, int idx,
                                 le_engine *e,
                                 le_input_action *out) {
    const char *name = luaL_checkstring(L, idx);

    if (e == NULL || name == NULL) {
        return luaL_error(L, "Input outside dispatch");
    }
    if (!le_input_find_action(e, name, out)) {
        return luaL_error(L, "unknown action '%s'", name);
    }
    return 0;
}

static int le_i_action_down(lua_State *L) {
    le_engine *e = le_bind_engine(L);
    le_input_action a = LE_INPUT_ACTION_INVALID;

    le_resolve_action_arg(L, 1, e, &a);
    lua_pushboolean(L, le_input_action_down(e, &a));
    return 1;
}

static int le_i_action_pressed(lua_State *L) {
    le_engine *e = le_bind_engine(L);
    le_input_action a = LE_INPUT_ACTION_INVALID;

    le_resolve_action_arg(L, 1, e, &a);
    lua_pushboolean(L, le_input_action_pressed(e, &a));
    return 1;
}

static int le_i_action_released(lua_State *L) {
    le_engine *e = le_bind_engine(L);
    le_input_action a = LE_INPUT_ACTION_INVALID;

    le_resolve_action_arg(L, 1, e, &a);
    lua_pushboolean(L, le_input_action_released(e, &a));
    return 1;
}

/* Input.axis(name) -> number */
static int le_i_axis(lua_State *L) {
    le_engine *e = le_bind_engine(L);
    const char *name = luaL_checkstring(L, 1);
    le_input_axis a = LE_INPUT_AXIS_INVALID;

    if (e == NULL) {
        return luaL_error(L, "Input outside dispatch");
    }
    if (!le_input_find_axis(e, name, &a)) {
        return luaL_error(L, "unknown axis '%s'", name);
    }
    lua_pushnumber(L, (lua_Number)le_input_axis_value(e, &a));
    return 1;
}

/* Time.delta() / unscaled_delta / elapsed / unscaled_elapsed /
 * frame / scale / set_scale / fixed_delta */
static int le_t_delta(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Time outside dispatch");
    }
    lua_pushnumber(L, (lua_Number)le_time_delta(e));
    return 1;
}

static int le_t_unscaled_delta(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Time outside dispatch");
    }
    lua_pushnumber(L, (lua_Number)le_time_unscaled_delta(e));
    return 1;
}

static int le_t_elapsed(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Time outside dispatch");
    }
    lua_pushnumber(L, (lua_Number)le_time_elapsed(e));
    return 1;
}

static int le_t_unscaled_elapsed(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Time outside dispatch");
    }
    lua_pushnumber(L, (lua_Number)le_time_unscaled_elapsed(e));
    return 1;
}

static int le_t_frame(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Time outside dispatch");
    }
    lua_pushinteger(L, (lua_Integer)le_time_frame_index(e));
    return 1;
}

static int le_t_scale(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Time outside dispatch");
    }
    lua_pushnumber(L, (lua_Number)le_time_get_scale(e));
    return 1;
}

static int le_t_set_scale(lua_State *L) {
    le_engine *e = le_bind_engine(L);
    double v = luaL_checknumber(L, 1);

    if (e == NULL) {
        return luaL_error(L, "Time outside dispatch");
    }
    if (le_time_set_scale(e, (float)v) != LE_SUCCESS) {
        return luaL_error(L, "invalid time scale");
    }
    return 0;
}

static int le_t_fixed_delta(lua_State *L) {
    le_engine *e = le_bind_engine(L);

    if (e == NULL) {
        return luaL_error(L, "Time outside dispatch");
    }
    lua_pushnumber(L, (lua_Number)le_time_get_fixed_delta(e));
    return 1;
}

/* Key/Mouse constant tables (readable names, no magic ints). */
typedef struct le_const_entry {
    const char *name;
    int value;
} le_const_entry;

static const le_const_entry kKeyConsts[] = {
    { "A", LE_KEY_A }, { "B", LE_KEY_B }, { "C", LE_KEY_C },
    { "D", LE_KEY_D }, { "E", LE_KEY_E }, { "F", LE_KEY_F },
    { "G", LE_KEY_G }, { "H", LE_KEY_H }, { "I", LE_KEY_I },
    { "J", LE_KEY_J }, { "K", LE_KEY_K }, { "L", LE_KEY_L },
    { "M", LE_KEY_M }, { "N", LE_KEY_N }, { "O", LE_KEY_O },
    { "P", LE_KEY_P }, { "Q", LE_KEY_Q }, { "R", LE_KEY_R },
    { "S", LE_KEY_S }, { "T", LE_KEY_T }, { "U", LE_KEY_U },
    { "V", LE_KEY_V }, { "W", LE_KEY_W }, { "X", LE_KEY_X },
    { "Y", LE_KEY_Y }, { "Z", LE_KEY_Z },
    { "N0", LE_KEY_0 }, { "N1", LE_KEY_1 }, { "N2", LE_KEY_2 },
    { "N3", LE_KEY_3 }, { "N4", LE_KEY_4 }, { "N5", LE_KEY_5 },
    { "N6", LE_KEY_6 }, { "N7", LE_KEY_7 }, { "N8", LE_KEY_8 },
    { "N9", LE_KEY_9 },
    { "Escape", LE_KEY_ESCAPE }, { "Enter", LE_KEY_ENTER },
    { "Tab", LE_KEY_TAB }, { "Space", LE_KEY_SPACE },
    { "Backspace", LE_KEY_BACKSPACE },
    { "LeftShift", LE_KEY_LEFT_SHIFT },
    { "RightShift", LE_KEY_RIGHT_SHIFT },
    { "LeftControl", LE_KEY_LEFT_CONTROL },
    { "RightControl", LE_KEY_RIGHT_CONTROL },
    { "LeftAlt", LE_KEY_LEFT_ALT },
    { "RightAlt", LE_KEY_RIGHT_ALT },
    { "LeftSuper", LE_KEY_LEFT_SUPER },
    { "RightSuper", LE_KEY_RIGHT_SUPER },
    { "Left", LE_KEY_LEFT }, { "Right", LE_KEY_RIGHT },
    { "Up", LE_KEY_UP }, { "Down", LE_KEY_DOWN },
    { "Insert", LE_KEY_INSERT }, { "Delete", LE_KEY_DELETE },
    { "Home", LE_KEY_HOME }, { "End", LE_KEY_END },
    { "PageUp", LE_KEY_PAGE_UP }, { "PageDown", LE_KEY_PAGE_DOWN },
    { "F1", LE_KEY_F1 }, { "F2", LE_KEY_F2 }, { "F3", LE_KEY_F3 },
    { "F4", LE_KEY_F4 }, { "F5", LE_KEY_F5 }, { "F6", LE_KEY_F6 },
    { "F7", LE_KEY_F7 }, { "F8", LE_KEY_F8 }, { "F9", LE_KEY_F9 },
    { "F10", LE_KEY_F10 }, { "F11", LE_KEY_F11 },
    { "F12", LE_KEY_F12 },
    { NULL, 0 },
};

static const le_const_entry kMouseConsts[] = {
    { "Left", LE_MOUSE_LEFT },
    { "Right", LE_MOUSE_RIGHT },
    { "Middle", LE_MOUSE_MIDDLE },
    { "Button4", LE_MOUSE_4 },
    { "Button5", LE_MOUSE_5 },
    { NULL, 0 },
};

void le_lua_register_input(lua_State *L) {
    static const luaL_Reg input_fn[] = {
        { "key_down", le_i_key_down },
        { "key_pressed", le_i_key_pressed },
        { "key_released", le_i_key_released },
        { "mouse_down", le_i_mouse_down },
        { "mouse_pressed", le_i_mouse_pressed },
        { "mouse_released", le_i_mouse_released },
        { "mouse_position", le_i_mouse_position },
        { "mouse_delta", le_i_mouse_delta },
        { "scroll_delta", le_i_scroll_delta },
        { "action_down", le_i_action_down },
        { "action_pressed", le_i_action_pressed },
        { "action_released", le_i_action_released },
        { "axis", le_i_axis },
        { NULL, NULL },
    };
    static const luaL_Reg time_fn[] = {
        { "delta", le_t_delta },
        { "unscaled_delta", le_t_unscaled_delta },
        { "elapsed", le_t_elapsed },
        { "unscaled_elapsed", le_t_unscaled_elapsed },
        { "frame", le_t_frame },
        { "scale", le_t_scale },
        { "set_scale", le_t_set_scale },
        { "fixed_delta", le_t_fixed_delta },
        { NULL, NULL },
    };
    const le_const_entry *c;

    lua_newtable(L);
    luaL_setfuncs(L, input_fn, 0);
    lua_setglobal(L, "Input");
    lua_newtable(L);
    luaL_setfuncs(L, time_fn, 0);
    lua_setglobal(L, "Time");
    lua_newtable(L);
    for (c = kKeyConsts; c->name != NULL; c++) {
        lua_pushinteger(L, (lua_Integer)c->value);
        lua_setfield(L, -2, c->name);
    }
    lua_setglobal(L, "Key");
    lua_newtable(L);
    for (c = kMouseConsts; c->name != NULL; c++) {
        lua_pushinteger(L, (lua_Integer)c->value);
        lua_setfield(L, -2, c->name);
    }
    lua_setglobal(L, "Mouse");
}
