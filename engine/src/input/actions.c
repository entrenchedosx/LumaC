/*
 * Input actions, axes, and contexts (Phase 27).
 *
 * Identity: names hash (FNV-1a) with full string compare on
 * collision — handles are {index, generation}; stale handles
 * fail safely. Actions/axes start GLOBAL (visible everywhere);
 * the first context-bind narrows them to the owning contexts.
 * Queries skip globally-invisible entries; the top active context
 * (highest priority) may consume keyboard/mouse domains.
 *
 * Aggregate edges: action down = any binding down; pressed =
 * aggregate was up last frame and is down now (latch updated in
 * le_input_end_frame). Holding Space then pressing Gamepad A does
 * NOT re-press the aggregate (tested contract).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "input/input_internal.h"

int le_input_resolve_action(const struct le_input_state *in,
                            const le_input_action *h,
                            uint32_t *out_idx) {
    if (out_idx != NULL) {
        *out_idx = 0;
    }
    if (in == NULL || h == NULL || in->actions == NULL) {
        return 0;
    }
    if (h->index >= in->action_cap) {
        return 0;
    }
    if (!in->actions[h->index].alive) {
        return 0;
    }
    if (in->actions[h->index].generation != h->generation ||
        h->generation == 0) {
        return 0;
    }
    if (out_idx != NULL) {
        *out_idx = h->index;
    }
    return 1;
}

int le_input_resolve_axis(const struct le_input_state *in,
                          const le_input_axis *h, uint32_t *out_idx) {
    if (out_idx != NULL) {
        *out_idx = 0;
    }
    if (in == NULL || h == NULL || in->axes == NULL) {
        return 0;
    }
    if (h->index >= in->axis_cap) {
        return 0;
    }
    if (!in->axes[h->index].alive) {
        return 0;
    }
    if (in->axes[h->index].generation != h->generation ||
        h->generation == 0) {
        return 0;
    }
    if (out_idx != NULL) {
        *out_idx = h->index;
    }
    return 1;
}

int le_input_resolve_context(const struct le_input_state *in,
                             const le_input_context *h,
                             uint32_t *out_idx) {
    if (out_idx != NULL) {
        *out_idx = 0;
    }
    if (in == NULL || h == NULL) {
        return 0;
    }
    if (h->index >= LE_INPUT_MAX_CONTEXTS) {
        return 0;
    }
    if (!in->contexts[h->index].alive) {
        return 0;
    }
    if (in->contexts[h->index].generation != h->generation ||
        h->generation == 0) {
        return 0;
    }
    if (out_idx != NULL) {
        *out_idx = h->index;
    }
    return 1;
}

/* Highest-priority active context (NULL when none active). */
static const le_context_slot *le_top_context(
    struct le_input_state *in) {
    const le_context_slot *best = NULL;
    uint32_t i;

    if (in == NULL) {
        return NULL;
    }
    for (i = 0; i < LE_INPUT_MAX_CONTEXTS; i++) {
        const le_context_slot *c = &in->contexts[i];

        if (!c->alive || !c->active) {
            continue;
        }
        if (best == NULL || c->priority > best->priority) {
            best = c;
        }
    }
    return best;
}

int le_input_keyboard_consumed(struct le_input_state *in) {
    const le_context_slot *top = le_top_context(in);

    if (top == NULL) {
        return 0;
    }
    return (top->consume_mask & (uint32_t)LE_CONSUME_KEYBOARD) != 0;
}

int le_input_mouse_consumed(struct le_input_state *in) {
    const le_context_slot *top = le_top_context(in);

    if (top == NULL) {
        return 0;
    }
    return (top->consume_mask & (uint32_t)LE_CONSUME_MOUSE) != 0;
}

int le_input_action_visible(struct le_input_state *in,
                            const le_action_slot *a) {
    uint32_t i;

    if (in == NULL || a == NULL || !a->alive) {
        return 0;
    }
    if (!a->narrowed) {
        return 1;
    }
    for (i = 0; i < a->context_count; i++) {
        uint32_t ci = a->contexts[i];

        if (ci < LE_INPUT_MAX_CONTEXTS &&
            in->contexts[ci].alive && in->contexts[ci].active) {
            return 1;
        }
    }
    return 0;
}

int le_input_axis_visible(struct le_input_state *in,
                          const le_axis_slot *a) {
    uint32_t i;

    if (in == NULL || a == NULL || !a->alive) {
        return 0;
    }
    if (!a->narrowed) {
        return 1;
    }
    for (i = 0; i < a->context_count; i++) {
        uint32_t ci = a->contexts[i];

        if (ci < LE_INPUT_MAX_CONTEXTS &&
            in->contexts[ci].alive && in->contexts[ci].active) {
            return 1;
        }
    }
    return 0;
}

/* Validate a binding payload (kind-appropriate ranges). */
static int le_binding_valid(const le_input_binding *b) {
    if (b == NULL) {
        return 0;
    }
    switch (b->kind) {
    case LE_BINDING_KEY:
        return (int)b->key > LE_KEY_UNKNOWN &&
            (int)b->key < LE_KEY_COUNT;
    case LE_BINDING_MOUSE_BUTTON:
        return (int)b->mouse_button >= 0 &&
            (int)b->mouse_button < LE_MOUSE_BUTTON_COUNT;
    case LE_BINDING_GAMEPAD_BUTTON:
        return (int)b->gamepad_button >= 0 &&
            (int)b->gamepad_button < LE_GAMEPAD_BUTTON_COUNT &&
            b->gamepad_slot < LE_GAMEPAD_MAX_SLOTS;
    case LE_BINDING_GAMEPAD_AXIS:
        return (int)b->gamepad_axis >= 0 &&
            (int)b->gamepad_axis < LE_GAMEPAD_AXIS_COUNT &&
            b->gamepad_slot < LE_GAMEPAD_MAX_SLOTS;
    case LE_BINDING_MOUSE_DELTA_X:
    case LE_BINDING_MOUSE_DELTA_Y:
    case LE_BINDING_MOUSE_WHEEL_X:
    case LE_BINDING_MOUSE_WHEEL_Y:
        return 1;
    default:
        return 0;
    }
}

static int le_binding_equal(const le_input_binding *a,
                            const le_input_binding *b) {
    if (a == NULL || b == NULL || a->kind != b->kind) {
        return 0;
    }
    switch (a->kind) {
    case LE_BINDING_KEY:
        return a->key == b->key;
    case LE_BINDING_MOUSE_BUTTON:
        return a->mouse_button == b->mouse_button;
    case LE_BINDING_GAMEPAD_BUTTON:
        return a->gamepad_button == b->gamepad_button &&
            a->gamepad_slot == b->gamepad_slot;
    case LE_BINDING_GAMEPAD_AXIS:
        return a->gamepad_axis == b->gamepad_axis &&
            a->gamepad_slot == b->gamepad_slot;
    case LE_BINDING_MOUSE_DELTA_X:
    case LE_BINDING_MOUSE_DELTA_Y:
    case LE_BINDING_MOUSE_WHEEL_X:
    case LE_BINDING_MOUSE_WHEEL_Y:
        return 1;
    default:
        return 0;
    }
}

/* Raw digital source state (ignores contexts — actions apply
 * visibility at the aggregate level, not per binding). */
static int le_binding_down_raw(struct le_input_state *in,
                               const le_input_binding *b) {
    switch (b->kind) {
    case LE_BINDING_KEY:
        return in->key_held[(uint32_t)b->key] ? 1 : 0;
    case LE_BINDING_MOUSE_BUTTON:
        return in->mouse_held[(uint32_t)b->mouse_button] ? 1 : 0;
    case LE_BINDING_GAMEPAD_BUTTON:
        if (!in->pads[b->gamepad_slot].connected) {
            return 0;
        }
        return in->pads[b->gamepad_slot]
                   .buttons[b->gamepad_button]
                   ? 1
                   : 0;
    case LE_BINDING_GAMEPAD_AXIS:
    case LE_BINDING_MOUSE_DELTA_X:
    case LE_BINDING_MOUSE_DELTA_Y:
    case LE_BINDING_MOUSE_WHEEL_X:
    case LE_BINDING_MOUSE_WHEEL_Y:
        /* Motion sources are not digital (axes sample them). */
        return 0;
    default:
        return 0;
    }
}

/* Aggregate action evaluation (exported for the end_frame latch;
 * declared extern in input.c — keep the signature in sync). */
int le_input_action_down_impl(struct le_input_state *in,
                              const le_action_slot *a) {
    uint32_t i;
    int kbd_consumed;
    int mouse_consumed;

    if (!le_input_action_visible(in, a)) {
        return 0;
    }
    kbd_consumed = le_input_keyboard_consumed(in);
    mouse_consumed = le_input_mouse_consumed(in);
    for (i = 0; i < a->binding_count; i++) {
        const le_input_binding *b = &a->bindings[i];

        if (b->kind == LE_BINDING_KEY && kbd_consumed) {
            continue;
        }
        if ((b->kind == LE_BINDING_MOUSE_BUTTON ||
             b->kind == LE_BINDING_MOUSE_DELTA_X ||
             b->kind == LE_BINDING_MOUSE_DELTA_Y ||
             b->kind == LE_BINDING_MOUSE_WHEEL_X ||
             b->kind == LE_BINDING_MOUSE_WHEEL_Y) &&
            mouse_consumed) {
            continue;
        }
        if (le_binding_down_raw(in, b)) {
            return 1;
        }
    }
    return 0;
}

/* ---- action API ---- */

static int le_name_valid(const char *name, size_t max_len) {
    size_t n;

    if (name == NULL || name[0] == '\0') {
        return 0;
    }
    n = strlen(name);
    if (n == 0 || n >= max_len) {
        return 0;
    }
    /* Reject embedded NUL-adjacent garbage implicitly (strlen);
     * require printable ASCII for registry sanity (Lua/editor
     * names are identifiers, never binary). */
    {
        size_t i;

        for (i = 0; i < n; i++) {
            unsigned char c = (unsigned char)name[i];

            if (c < 32 || c > 126) {
                return 0;
            }
        }
    }
    return 1;
}

static le_result le_ensure_action_cap(struct le_input_state *in) {
    if (in->actions != NULL) {
        uint32_t i;

        for (i = 0; i < in->action_cap; i++) {
            if (!in->actions[i].alive) {
                return LE_SUCCESS;
            }
        }
    }
    {
        uint32_t grown =
            (in->action_cap == 0) ? 16u : in->action_cap * 2u;
        le_action_slot *fresh;

        if (grown > LE_INPUT_MAX_ACTIONS) {
            grown = LE_INPUT_MAX_ACTIONS;
        }
        if (grown <= in->action_cap) {
            return LE_ERROR_OVERFLOW;
        }
        fresh = (le_action_slot *)realloc(
            in->actions, (size_t)grown * sizeof(*fresh));
        if (fresh == NULL) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
        memset(fresh + in->action_cap, 0,
               (size_t)(grown - in->action_cap) * sizeof(*fresh));
        in->actions = fresh;
        in->action_cap = grown;
        return LE_SUCCESS;
    }
}

le_result le_input_create_action(le_engine *engine, const char *name,
                                 le_input_action *out_action) {
    struct le_input_state *in;
    uint64_t h;
    uint32_t i;
    le_result rc;

    if (out_action != NULL) {
        *out_action = LE_INPUT_ACTION_INVALID;
    }
    if (engine == NULL || engine->input == NULL || name == NULL ||
        out_action == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_name_valid(name, sizeof(in->actions[0].name))) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    h = le_input_hash_name(name);
    /* Idempotent: live same-name returns the existing handle. */
    for (i = 0; i < in->action_cap; i++) {
        if (in->actions[i].alive &&
            in->actions[i].name_hash == h &&
            strcmp(in->actions[i].name, name) == 0) {
            out_action->index = i;
            out_action->generation = in->actions[i].generation;
            return LE_SUCCESS;
        }
    }
    rc = le_ensure_action_cap(in);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    for (i = 0; i < in->action_cap; i++) {
        if (!in->actions[i].alive) {
            le_action_slot *a = &in->actions[i];

            memset(a, 0, sizeof(*a));
            a->alive = 1;
            a->generation++;
            if (a->generation == 0) {
                a->generation = 1;
            }
            snprintf(a->name, sizeof(a->name), "%s", name);
            a->name_hash = h;
            in->action_alive++;
            out_action->index = i;
            out_action->generation = a->generation;
            return LE_SUCCESS;
        }
    }
    return LE_ERROR_OVERFLOW;
}

int le_input_find_action(le_engine *engine, const char *name,
                         le_input_action *out_action) {
    struct le_input_state *in;
    uint64_t h;
    uint32_t i;

    if (out_action != NULL) {
        *out_action = LE_INPUT_ACTION_INVALID;
    }
    if (engine == NULL || engine->input == NULL || name == NULL) {
        return 0;
    }
    in = engine->input;
    h = le_input_hash_name(name);
    for (i = 0; i < in->action_cap; i++) {
        if (in->actions[i].alive &&
            in->actions[i].name_hash == h &&
            strcmp(in->actions[i].name, name) == 0) {
            if (out_action != NULL) {
                out_action->index = i;
                out_action->generation =
                    in->actions[i].generation;
            }
            return 1;
        }
    }
    return 0;
}

le_result le_input_add_action_binding(le_engine *engine,
                                      const le_input_action *action,
                                      const le_input_binding *binding) {
    struct le_input_state *in;
    uint32_t idx;
    uint32_t i;

    if (engine == NULL || engine->input == NULL || action == NULL ||
        binding == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_binding_valid(binding)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Motion sources are axis-only (actions are digital). */
    if (binding->kind == LE_BINDING_MOUSE_DELTA_X ||
        binding->kind == LE_BINDING_MOUSE_DELTA_Y ||
        binding->kind == LE_BINDING_MOUSE_WHEEL_X ||
        binding->kind == LE_BINDING_MOUSE_WHEEL_Y ||
        binding->kind == LE_BINDING_GAMEPAD_AXIS) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    if (!le_input_resolve_action(in, action, &idx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    {
        le_action_slot *a = &in->actions[idx];

        for (i = 0; i < a->binding_count; i++) {
            if (le_binding_equal(&a->bindings[i], binding)) {
                return LE_SUCCESS;
            }
        }
        if (a->binding_count >= LE_INPUT_MAX_BINDINGS_PER_ENTRY) {
            return LE_ERROR_OVERFLOW;
        }
        a->bindings[a->binding_count++] = *binding;
        return LE_SUCCESS;
    }
}

le_result le_input_remove_action_binding(
    le_engine *engine, const le_input_action *action,
    const le_input_binding *binding) {
    struct le_input_state *in;
    uint32_t idx;
    uint32_t i;

    if (engine == NULL || engine->input == NULL || action == NULL ||
        binding == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    if (!le_input_resolve_action(in, action, &idx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    {
        le_action_slot *a = &in->actions[idx];

        for (i = 0; i < a->binding_count; i++) {
            if (le_binding_equal(&a->bindings[i], binding)) {
                a->bindings[i] =
                    a->bindings[a->binding_count - 1u];
                a->binding_count--;
                return LE_SUCCESS;
            }
        }
        return LE_ERROR_INVALID_ARGUMENT;
    }
}

le_result le_input_clear_action_bindings(
    le_engine *engine, const le_input_action *action) {
    struct le_input_state *in;
    uint32_t idx;

    if (engine == NULL || engine->input == NULL || action == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    if (!le_input_resolve_action(in, action, &idx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in->actions[idx].binding_count = 0;
    return LE_SUCCESS;
}

le_result le_input_get_action_bindings(le_engine *engine,
                                       const le_input_action *action,
                                       le_input_binding *out,
                                       uint32_t cap,
                                       uint32_t *out_count) {
    struct le_input_state *in;
    uint32_t idx;
    uint32_t i;

    if (out_count != NULL) {
        *out_count = 0;
    }
    if (engine == NULL || engine->input == NULL || action == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    if (!le_input_resolve_action(in, action, &idx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    {
        le_action_slot *a = &in->actions[idx];

        if (out_count != NULL) {
            *out_count = a->binding_count;
        }
        if (out != NULL) {
            uint32_t n = (cap < a->binding_count)
                             ? cap
                             : a->binding_count;

            for (i = 0; i < n; i++) {
                out[i] = a->bindings[i];
            }
        }
        return LE_SUCCESS;
    }
}

int le_input_action_down(le_engine *engine,
                         const le_input_action *action) {
    struct le_input_state *in;
    uint32_t idx;

    if (engine == NULL || engine->input == NULL || action == NULL) {
        return 0;
    }
    in = engine->input;
    if (!le_input_resolve_action(in, action, &idx)) {
        return 0;
    }
    return le_input_action_down_impl(in, &in->actions[idx]);
}

int le_input_action_pressed(le_engine *engine,
                            const le_input_action *action) {
    struct le_input_state *in;
    uint32_t idx;
    int now;

    if (engine == NULL || engine->input == NULL || action == NULL) {
        return 0;
    }
    in = engine->input;
    if (!le_input_resolve_action(in, action, &idx)) {
        return 0;
    }
    if (!le_input_action_visible(in, &in->actions[idx])) {
        return 0;
    }
    now = le_input_action_down_impl(in, &in->actions[idx]);
    return (now && !in->actions[idx].was_down) ? 1 : 0;
}

int le_input_action_released(le_engine *engine,
                             const le_input_action *action) {
    struct le_input_state *in;
    uint32_t idx;
    int now;

    if (engine == NULL || engine->input == NULL || action == NULL) {
        return 0;
    }
    in = engine->input;
    if (!le_input_resolve_action(in, action, &idx)) {
        return 0;
    }
    if (!le_input_action_visible(in, &in->actions[idx])) {
        return 0;
    }
    now = le_input_action_down_impl(in, &in->actions[idx]);
    return (!now && in->actions[idx].was_down) ? 1 : 0;
}

/* ---- axes ---- */

static le_result le_ensure_axis_cap(struct le_input_state *in) {
    if (in->axes != NULL) {
        uint32_t i;

        for (i = 0; i < in->axis_cap; i++) {
            if (!in->axes[i].alive) {
                return LE_SUCCESS;
            }
        }
    }
    {
        uint32_t grown =
            (in->axis_cap == 0) ? 8u : in->axis_cap * 2u;
        le_axis_slot *fresh;

        if (grown > LE_INPUT_MAX_AXES) {
            grown = LE_INPUT_MAX_AXES;
        }
        if (grown <= in->axis_cap) {
            return LE_ERROR_OVERFLOW;
        }
        fresh = (le_axis_slot *)realloc(
            in->axes, (size_t)grown * sizeof(*fresh));
        if (fresh == NULL) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
        memset(fresh + in->axis_cap, 0,
               (size_t)(grown - in->axis_cap) * sizeof(*fresh));
        in->axes = fresh;
        in->axis_cap = grown;
        return LE_SUCCESS;
    }
}

le_result le_input_create_axis(le_engine *engine,
                               const le_axis_desc *desc,
                               le_input_axis *out_axis) {
    struct le_input_state *in;
    uint64_t h;
    uint32_t i;
    le_result rc;

    if (out_axis != NULL) {
        *out_axis = LE_INPUT_AXIS_INVALID;
    }
    if (engine == NULL || engine->input == NULL || desc == NULL ||
        out_axis == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_name_valid(desc->name, sizeof(in->axes[0].name))) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->deadzone != desc->deadzone || desc->deadzone < 0.0f ||
        desc->deadzone >= 1.0f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->scale != desc->scale) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    h = le_input_hash_name(desc->name);
    for (i = 0; i < in->axis_cap; i++) {
        if (in->axes[i].alive && in->axes[i].name_hash == h &&
            strcmp(in->axes[i].name, desc->name) == 0) {
            out_axis->index = i;
            out_axis->generation = in->axes[i].generation;
            return LE_SUCCESS;
        }
    }
    rc = le_ensure_axis_cap(in);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    for (i = 0; i < in->axis_cap; i++) {
        if (!in->axes[i].alive) {
            le_axis_slot *a = &in->axes[i];

            memset(a, 0, sizeof(*a));
            a->alive = 1;
            a->generation++;
            if (a->generation == 0) {
                a->generation = 1;
            }
            snprintf(a->name, sizeof(a->name), "%s", desc->name);
            a->name_hash = h;
            a->deadzone = desc->deadzone;
            a->scale = (desc->scale == 0.0f) ? 1.0f : desc->scale;
            a->invert = desc->invert ? 1 : 0;
            in->axis_alive++;
            out_axis->index = i;
            out_axis->generation = a->generation;
            return LE_SUCCESS;
        }
    }
    return LE_ERROR_OVERFLOW;
}

int le_input_find_axis(le_engine *engine, const char *name,
                       le_input_axis *out_axis) {
    struct le_input_state *in;
    uint64_t h;
    uint32_t i;

    if (out_axis != NULL) {
        *out_axis = LE_INPUT_AXIS_INVALID;
    }
    if (engine == NULL || engine->input == NULL || name == NULL) {
        return 0;
    }
    in = engine->input;
    h = le_input_hash_name(name);
    for (i = 0; i < in->axis_cap; i++) {
        if (in->axes[i].alive && in->axes[i].name_hash == h &&
            strcmp(in->axes[i].name, name) == 0) {
            if (out_axis != NULL) {
                out_axis->index = i;
                out_axis->generation = in->axes[i].generation;
            }
            return 1;
        }
    }
    return 0;
}

le_result le_input_add_axis_binding(le_engine *engine,
                                    const le_input_axis *axis,
                                    const le_input_binding *binding) {
    struct le_input_state *in;
    uint32_t idx;
    uint32_t i;

    if (engine == NULL || engine->input == NULL || axis == NULL ||
        binding == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_binding_valid(binding)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Axis bindings need a pole for digital sources (default +1
     * when the caller leaves scale 0). */
    in = engine->input;
    if (!le_input_resolve_axis(in, axis, &idx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    {
        le_axis_slot *a = &in->axes[idx];

        for (i = 0; i < a->binding_count; i++) {
            if (le_binding_equal(&a->bindings[i], binding)) {
                return LE_SUCCESS;
            }
        }
        if (a->binding_count >= LE_INPUT_MAX_BINDINGS_PER_ENTRY) {
            return LE_ERROR_OVERFLOW;
        }
        a->bindings[a->binding_count] = *binding;
        if (a->bindings[a->binding_count].scale == 0.0f &&
            (binding->kind == LE_BINDING_KEY ||
             binding->kind == LE_BINDING_MOUSE_BUTTON ||
             binding->kind == LE_BINDING_GAMEPAD_BUTTON)) {
            a->bindings[a->binding_count].scale = 1.0f;
        }
        a->binding_count++;
        return LE_SUCCESS;
    }
}

le_result le_input_remove_axis_binding(le_engine *engine,
                                       const le_input_axis *axis,
                                       const le_input_binding *binding) {
    struct le_input_state *in;
    uint32_t idx;
    uint32_t i;

    if (engine == NULL || engine->input == NULL || axis == NULL ||
        binding == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    if (!le_input_resolve_axis(in, axis, &idx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    {
        le_axis_slot *a = &in->axes[idx];

        for (i = 0; i < a->binding_count; i++) {
            if (le_binding_equal(&a->bindings[i], binding)) {
                a->bindings[i] =
                    a->bindings[a->binding_count - 1u];
                a->binding_count--;
                return LE_SUCCESS;
            }
        }
        return LE_ERROR_INVALID_ARGUMENT;
    }
}

le_result le_input_clear_axis_bindings(le_engine *engine,
                                       const le_input_axis *axis) {
    struct le_input_state *in;
    uint32_t idx;

    if (engine == NULL || engine->input == NULL || axis == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    if (!le_input_resolve_axis(in, axis, &idx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in->axes[idx].binding_count = 0;
    return LE_SUCCESS;
}

/* Sample one binding's signed contribution. */
static float le_axis_binding_sample(struct le_input_state *in,
                                    const le_axis_slot *a,
                                    const le_input_binding *b) {
    float dead = (a->deadzone > 0.0f) ? a->deadzone : 0.0f;

    switch (b->kind) {
    case LE_BINDING_KEY:
        return in->key_held[(uint32_t)b->key] ? b->scale : 0.0f;
    case LE_BINDING_MOUSE_BUTTON:
        return in->mouse_held[(uint32_t)b->mouse_button] ? b->scale
                                                         : 0.0f;
    case LE_BINDING_GAMEPAD_BUTTON:
        if (!in->pads[b->gamepad_slot].connected) {
            return 0.0f;
        }
        return in->pads[b->gamepad_slot].buttons[b->gamepad_button]
                   ? b->scale
                   : 0.0f;
    case LE_BINDING_GAMEPAD_AXIS:
        if (!in->pads[b->gamepad_slot].connected) {
            return 0.0f;
        }
        {
            float v =
                in->pads[b->gamepad_slot].axes[b->gamepad_axis];
            /* Triggers report [0,1]: center them to [-1,1]
             * before deadzone so rest (0) reads -1... instead
             * triggers bypass the stick deadzone (rest = 0). */
            if (b->gamepad_axis == LE_GAMEPAD_AXIS_LEFT_TRIGGER ||
                b->gamepad_axis == LE_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                if (v < 0.02f) {
                    return 0.0f;
                }
                return v * b->scale;
            }
            if (v < 0.0f ? -v < dead : v < dead) {
                return 0.0f;
            }
            return v * b->scale;
        }
    case LE_BINDING_MOUSE_DELTA_X:
        return in->delta_x * b->scale;
    case LE_BINDING_MOUSE_DELTA_Y:
        return in->delta_y * b->scale;
    case LE_BINDING_MOUSE_WHEEL_X:
        return in->wheel_x * b->scale;
    case LE_BINDING_MOUSE_WHEEL_Y:
        return in->wheel_y * b->scale;
    default:
        return 0.0f;
    }
}

float le_input_axis_value(le_engine *engine,
                          const le_input_axis *axis) {
    struct le_input_state *in;
    uint32_t idx;
    uint32_t i;
    float pos = 0.0f;
    float neg = 0.0f;
    int has_pos = 0;
    int has_neg = 0;

    if (engine == NULL || engine->input == NULL || axis == NULL) {
        return 0.0f;
    }
    in = engine->input;
    if (!le_input_resolve_axis(in, axis, &idx)) {
        return 0.0f;
    }
    {
        le_axis_slot *a = &in->axes[idx];
        int kbd_consumed;
        int mouse_consumed;

        if (!le_input_axis_visible(in, a)) {
            return 0.0f;
        }
        kbd_consumed = le_input_keyboard_consumed(in);
        mouse_consumed = le_input_mouse_consumed(in);
        for (i = 0; i < a->binding_count; i++) {
            const le_input_binding *b = &a->bindings[i];
            float v;

            if (b->kind == LE_BINDING_KEY && kbd_consumed) {
                continue;
            }
            if ((b->kind == LE_BINDING_MOUSE_BUTTON ||
                 b->kind == LE_BINDING_MOUSE_DELTA_X ||
                 b->kind == LE_BINDING_MOUSE_DELTA_Y ||
                 b->kind == LE_BINDING_MOUSE_WHEEL_X ||
                 b->kind == LE_BINDING_MOUSE_WHEEL_Y) &&
                mouse_consumed) {
                continue;
            }
            v = le_axis_binding_sample(in, a, b);
            if (v > 0.0f) {
                /* Strongest positive pole wins. */
                if (!has_pos || v > pos) {
                    pos = v;
                    has_pos = 1;
                }
            } else if (v < 0.0f) {
                if (!has_neg || v < neg) {
                    neg = v;
                    has_neg = 1;
                }
            }
        }
        /* Digital contract: +1 and -1 together cancel to 0
         * (documented; no last-wins surprise). Pure analog sums
         * clamp to [-1,1] only when no digital pole fired... the
         * pole-max rule above already handles mixing: opposite
         * digital poles cancel, analog extremes survive. */
        {
            float out;

            if (has_pos && has_neg &&
                (pos + neg) == 0.0f) {
                out = 0.0f;
            } else if (has_pos && has_neg) {
                /* Same-pole-strength check first; otherwise the
                 * stronger pole wins (explicit, tested). */
                out = (pos >= -neg) ? pos : neg;
                if (pos == -neg) {
                    out = 0.0f;
                }
            } else if (has_pos) {
                out = pos;
            } else if (has_neg) {
                out = neg;
            } else {
                out = 0.0f;
            }
            if (out > 1.0f) {
                out = 1.0f;
            }
            if (out < -1.0f) {
                out = -1.0f;
            }
            out *= a->scale;
            if (a->invert) {
                out = -out;
            }
            return out;
        }
    }
}

/* ---- contexts ---- */

le_result le_input_create_context(le_engine *engine, const char *name,
                                  int priority,
                                  le_input_context *out_ctx) {
    struct le_input_state *in;
    uint32_t i;

    if (out_ctx != NULL) {
        *out_ctx = LE_INPUT_CONTEXT_INVALID;
    }
    if (engine == NULL || engine->input == NULL || name == NULL ||
        out_ctx == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_name_valid(name, sizeof(in->contexts[0].name))) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    for (i = 0; i < LE_INPUT_MAX_CONTEXTS; i++) {
        if (in->contexts[i].alive &&
            strcmp(in->contexts[i].name, name) == 0) {
            in->contexts[i].priority = priority;
            out_ctx->index = i;
            out_ctx->generation = in->contexts[i].generation;
            return LE_SUCCESS;
        }
    }
    for (i = 0; i < LE_INPUT_MAX_CONTEXTS; i++) {
        if (!in->contexts[i].alive) {
            le_context_slot *c = &in->contexts[i];

            memset(c, 0, sizeof(*c));
            c->alive = 1;
            c->generation++;
            if (c->generation == 0) {
                c->generation = 1;
            }
            snprintf(c->name, sizeof(c->name), "%s", name);
            c->priority = priority;
            c->active = 0;
            c->consume_mask = (uint32_t)LE_CONSUME_NONE;
            in->context_alive++;
            out_ctx->index = i;
            out_ctx->generation = c->generation;
            return LE_SUCCESS;
        }
    }
    return LE_ERROR_OVERFLOW;
}

int le_input_find_context(le_engine *engine, const char *name,
                          le_input_context *out_ctx) {
    struct le_input_state *in;
    uint32_t i;

    if (out_ctx != NULL) {
        *out_ctx = LE_INPUT_CONTEXT_INVALID;
    }
    if (engine == NULL || engine->input == NULL || name == NULL) {
        return 0;
    }
    in = engine->input;
    for (i = 0; i < LE_INPUT_MAX_CONTEXTS; i++) {
        if (in->contexts[i].alive &&
            strcmp(in->contexts[i].name, name) == 0) {
            if (out_ctx != NULL) {
                out_ctx->index = i;
                out_ctx->generation = in->contexts[i].generation;
            }
            return 1;
        }
    }
    return 0;
}

le_result le_input_activate_context(le_engine *engine,
                                    const le_input_context *ctx) {
    struct le_input_state *in;
    uint32_t idx;

    if (engine == NULL || engine->input == NULL || ctx == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    if (!le_input_resolve_context(in, ctx, &idx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in->contexts[idx].active = 1;
    return LE_SUCCESS;
}

le_result le_input_deactivate_context(le_engine *engine,
                                      const le_input_context *ctx) {
    struct le_input_state *in;
    uint32_t idx;

    if (engine == NULL || engine->input == NULL || ctx == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    if (!le_input_resolve_context(in, ctx, &idx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in->contexts[idx].active = 0;
    return LE_SUCCESS;
}

int le_input_context_active(le_engine *engine,
                            const le_input_context *ctx) {
    struct le_input_state *in;
    uint32_t idx;

    if (engine == NULL || engine->input == NULL || ctx == NULL) {
        return 0;
    }
    in = engine->input;
    if (!le_input_resolve_context(in, ctx, &idx)) {
        return 0;
    }
    return in->contexts[idx].active ? 1 : 0;
}

static le_result le_narrow_entry(le_engine *engine, int is_action,
                                 uint32_t entry_idx,
                                 uint32_t ctx_idx) {
    struct le_input_state *in = engine->input;
    uint32_t i;

    if (is_action) {
        le_action_slot *a = &in->actions[entry_idx];

        for (i = 0; i < a->context_count; i++) {
            if (a->contexts[i] == ctx_idx) {
                return LE_SUCCESS;
            }
        }
        if (a->context_count >= LE_INPUT_MAX_CONTEXTS) {
            return LE_ERROR_OVERFLOW;
        }
        a->contexts[a->context_count++] = ctx_idx;
        a->narrowed = 1;
        return LE_SUCCESS;
    } else {
        le_axis_slot *a = &in->axes[entry_idx];

        for (i = 0; i < a->context_count; i++) {
            if (a->contexts[i] == ctx_idx) {
                return LE_SUCCESS;
            }
        }
        if (a->context_count >= LE_INPUT_MAX_CONTEXTS) {
            return LE_ERROR_OVERFLOW;
        }
        a->contexts[a->context_count++] = ctx_idx;
        a->narrowed = 1;
        return LE_SUCCESS;
    }
}

le_result le_input_context_bind_action(
    le_engine *engine, const le_input_context *ctx,
    const le_input_action *action) {
    struct le_input_state *in;
    uint32_t cidx;
    uint32_t aidx;

    if (engine == NULL || engine->input == NULL || ctx == NULL ||
        action == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    if (!le_input_resolve_context(in, ctx, &cidx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_input_resolve_action(in, action, &aidx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    return le_narrow_entry(engine, 1, aidx, cidx);
}

le_result le_input_context_bind_axis(le_engine *engine,
                                     const le_input_context *ctx,
                                     const le_input_axis *axis) {
    struct le_input_state *in;
    uint32_t cidx;
    uint32_t aidx;

    if (engine == NULL || engine->input == NULL || ctx == NULL ||
        axis == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    if (!le_input_resolve_context(in, ctx, &cidx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_input_resolve_axis(in, axis, &aidx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    return le_narrow_entry(engine, 0, aidx, cidx);
}

le_result le_input_context_set_consume(le_engine *engine,
                                       const le_input_context *ctx,
                                       uint32_t consume_mask) {
    struct le_input_state *in;
    uint32_t cidx;

    if (engine == NULL || engine->input == NULL || ctx == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if ((consume_mask & ~(uint32_t)LE_CONSUME_ALL) != 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in = engine->input;
    if (!le_input_resolve_context(in, ctx, &cidx)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    in->contexts[cidx].consume_mask = consume_mask;
    return LE_SUCCESS;
}
