/*
 * Engine input core (Phase 27): raw device state, edges, motion
 * accumulation, focus policy, injection, gamepad foundation.
 *
 * Conventions (project-wide): every fallible function returns
 * le_result; NULL/out-of-range queries return 0/zeros, never
 * crash. Geometric growth for pending-adjacent registry arrays;
 * allocation failure preserves existing maps (no half-mutation).
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "input/input_internal.h"

const le_input_action LE_INPUT_ACTION_INVALID = { 0xFFFFFFFFu, 0u };
const le_input_axis LE_INPUT_AXIS_INVALID = { 0xFFFFFFFFu, 0u };
const le_input_context LE_INPUT_CONTEXT_INVALID = { 0xFFFFFFFFu,
                                                    0u };

uint64_t le_input_hash_name(const char *name) {
    /* FNV-1a 64 (same primitive as le_fnv1a64; self-contained so
     * input.c needs no asset TU). */
    uint64_t h = 1469598103934665603ULL;
    const unsigned char *p;

    if (name == NULL) {
        return 0;
    }
    for (p = (const unsigned char *)name; *p != '\0'; p++) {
        h ^= (uint64_t)*p;
        h *= 1099511628211ULL;
    }
    return (h != 0) ? h : 1u;
}

struct le_input_state *le_input_create(void) {
    struct le_input_state *in =
        (struct le_input_state *)calloc(1, sizeof(*in));

    if (in == NULL) {
        return NULL;
    }
    in->has_focus = 1; /* headless/tests start focused */
    return in;
}

void le_input_destroy(struct le_input_state *in) {
    if (in == NULL) {
        return;
    }
    free(in->actions);
    free(in->axes);
    free(in);
}

/* Push one pending event; drops the OLDEST when full (input stays
 * live under flood; stats still count the ingestion). */
static void le_push_pending(struct le_input_state *in,
                            const le_pending_event *ev) {
    uint32_t i;

    if (in == NULL || ev == NULL) {
        return;
    }
    in->events_ingested++;
    if (in->pending_count >= LE_INPUT_MAX_PENDING) {
        for (i = 1; i < in->pending_count; i++) {
            in->pending[i - 1] = in->pending[i];
        }
        in->pending_count--;
    }
    in->pending[in->pending_count++] = *ev;
}

/* Drain every live window's lc queue into pending (platform path).
 * Tracks the focus window (most recent FOCUS_GAINED; NULL when
 * none) and mirrors close/resize/focus onto engine lifecycle
 * flags. Called from le_engine_begin_frame. */
void le_input_poll_platform(le_engine *engine) {
    struct le_input_state *in;
    uint32_t wi;

    if (engine == NULL || engine->input == NULL) {
        return;
    }
    in = engine->input;
    lc_poll_events();
    for (wi = 0; wi < engine->window_count; wi++) {
        lc_window *w = engine->windows[wi];

        if (w == NULL) {
            continue;
        }
        {
            lc_window_event ev;

            for (;;) {
                memset(&ev, 0, sizeof(ev));
                if (lc_window_read_event(w, &ev) != LC_SUCCESS) {
                    break;
                }
                if (ev.type == LC_EVENT_NONE) {
                    break;
                }
                /* Re-target onto the engine queue. */
                {
                    le_pending_event p;

                    memset(&p, 0, sizeof(p));
                    switch (ev.type) {
                    case LC_EVENT_KEY_DOWN:
                        p.kind = 0;
                        p.key = (le_key)ev.key;
                        p.down = 1;
                        p.repeat = ev.repeat;
                        p.mods = ev.mods;
                        le_push_pending(in, &p);
                        break;
                    case LC_EVENT_KEY_UP:
                        p.kind = 0;
                        p.key = (le_key)ev.key;
                        p.down = 0;
                        p.mods = ev.mods;
                        le_push_pending(in, &p);
                        break;
                    case LC_EVENT_CHAR:
                        p.kind = 4;
                        memcpy(p.text, ev.utf8, sizeof(p.text));
                        p.text_len = ev.utf8_len;
                        p.mods = ev.mods;
                        le_push_pending(in, &p);
                        break;
                    case LC_EVENT_MOUSE_DOWN:
                        p.kind = 1;
                        p.button =
                            (le_mouse_button)ev.button;
                        p.down = 1;
                        p.x = ev.mouse_x;
                        p.y = ev.mouse_y;
                        p.mods = ev.mods;
                        le_push_pending(in, &p);
                        break;
                    case LC_EVENT_MOUSE_UP:
                        p.kind = 1;
                        p.button =
                            (le_mouse_button)ev.button;
                        p.down = 0;
                        p.x = ev.mouse_x;
                        p.y = ev.mouse_y;
                        p.mods = ev.mods;
                        le_push_pending(in, &p);
                        break;
                    case LC_EVENT_MOUSE_MOVE:
                        p.kind = 2;
                        p.x = ev.mouse_x;
                        p.y = ev.mouse_y;
                        p.dx = ev.delta_x;
                        p.dy = ev.delta_y;
                        p.mods = ev.mods;
                        le_push_pending(in, &p);
                        break;
                    case LC_EVENT_MOUSE_WHEEL:
                        p.kind = 3;
                        p.dx = ev.wheel_x;
                        p.dy = ev.wheel_y;
                        le_push_pending(in, &p);
                        break;
                    case LC_EVENT_FOCUS_GAINED:
                        p.kind = 5;
                        p.focused = 1;
                        le_push_pending(in, &p);
                        break;
                    case LC_EVENT_FOCUS_LOST:
                        p.kind = 5;
                        p.focused = 0;
                        le_push_pending(in, &p);
                        break;
                    case LC_EVENT_CLOSE:
                        engine->quit_requested = 1;
                        break;
                    case LC_EVENT_RESIZE:
                        if (ev.width == 0 || ev.height == 0) {
                            engine->minimized = 1;
                        } else {
                            engine->minimized = 0;
                        }
                        break;
                    default:
                        break;
                    }
                }
            }
        }
    }
}

void le_input_advance_frame(le_engine *engine) {
    struct le_input_state *in;
    uint32_t i;

    if (engine == NULL || engine->input == NULL) {
        return;
    }
    in = engine->input;
    /* Lazy aggregate latch: held still shows the PREVIOUS frame
     * (pending not folded yet), so capture it as was_down for
     * the edges about to be computed. Exactly-once per frame,
     * read-safe: any number of reads may happen between
     * advances (scripts during update, host/tests after step). */
    if (in->actions != NULL) {
        uint32_t a;

        for (a = 0; a < in->action_cap; a++) {
            if (!in->actions[a].alive) {
                continue;
            }
            {
                extern int le_input_action_down_impl(
                    struct le_input_state *in,
                    const le_action_slot *a);
                in->actions[a].was_down =
                    le_input_action_down_impl(
                        in, &in->actions[a]);
            }
        }
    }
    /* Edges are per-frame: recomputed from pending every frame.
     * Motion/wheel deltas accumulate across pending within the
     * frame, then freeze as the snapshot until end_frame. */
    memset(in->key_pressed, 0, sizeof(in->key_pressed));
    memset(in->key_released, 0, sizeof(in->key_released));
    memset(in->mouse_pressed, 0, sizeof(in->mouse_pressed));
    memset(in->mouse_released, 0, sizeof(in->mouse_released));
    in->delta_x = 0.0f;
    in->delta_y = 0.0f;
    in->wheel_x = 0.0f;
    in->wheel_y = 0.0f;
    for (i = 0; i < in->pending_count; i++) {
        le_pending_event *p = &in->pending[i];

        switch (p->kind) {
        case 0: /* key */
            if ((int)p->key > LE_KEY_UNKNOWN &&
                (int)p->key < LE_KEY_COUNT) {
                uint32_t k = (uint32_t)p->key;

                if (p->down) {
                    /* Auto-repeat never re-arms the edge. */
                    if (!in->key_held[k] && !p->repeat) {
                        in->key_pressed[k] = 1;
                    }
                    in->key_held[k] = 1;
                } else {
                    /* Release always edges (even without a
                     * recorded press — e.g. press arrived
                     * before focus, release after). Held
                     * clears; a same-frame re-press below
                     * re-arms pressed with held set. */
                    in->key_released[k] = 1;
                    in->key_held[k] = 0;
                }
                in->mods = p->mods;
            }
            break;
        case 1: /* mouse button */
            if ((int)p->button >= 0 &&
                (int)p->button < LE_MOUSE_BUTTON_COUNT) {
                uint32_t b = (uint32_t)p->button;

                if (p->down) {
                    if (!in->mouse_held[b]) {
                        in->mouse_pressed[b] = 1;
                    }
                    in->mouse_held[b] = 1;
                } else {
                    in->mouse_released[b] = 1;
                    in->mouse_held[b] = 0;
                }
                in->mouse_x = p->x;
                in->mouse_y = p->y;
                in->mods = p->mods;
            }
            break;
        case 2: /* mouse move */
            in->mouse_x = p->x;
            in->mouse_y = p->y;
            in->delta_x += p->dx;
            in->delta_y += p->dy;
            in->mods = p->mods;
            break;
        case 3: /* wheel */
            in->wheel_x += p->dx;
            in->wheel_y += p->dy;
            break;
        case 4: /* text */
            if (p->text_len > 0 &&
                in->text_count < LE_INPUT_MAX_TEXT) {
                uint32_t t =
                    (in->text_head + in->text_count) %
                    LE_INPUT_MAX_TEXT;

                memcpy(in->text[t], p->text, sizeof(in->text[t]));
                in->text_len[t] = p->text_len;
                in->text_count++;
            }
            in->mods = p->mods;
            break;
        case 5: /* focus */
            in->has_focus = p->focused ? 1 : 0;
            engine->has_focus = in->has_focus;
            if (!p->focused) {
                /* Focus lost: clear held state NOW (no stuck
                 * keys). Edges are not synthesized — motion
                 * already stopped; a phantom release would
                 * re-trigger tap detection. */
                memset(in->key_held, 0, sizeof(in->key_held));
                memset(in->mouse_held, 0,
                       sizeof(in->mouse_held));
            }
            break;
        case 6: /* gamepad button */
            if (p->pad_slot < LE_GAMEPAD_MAX_SLOTS &&
                (int)p->pad_button >= 0 &&
                (int)p->pad_button < LE_GAMEPAD_BUTTON_COUNT) {
                le_gamepad_slot *g =
                    &in->pads[p->pad_slot];

                g->connected = 1;
                if (p->down) {
                    if (!g->buttons[p->pad_button]) {
                        g->pressed[p->pad_button] = 1;
                    }
                    g->buttons[p->pad_button] = 1;
                } else {
                    g->released[p->pad_button] = 1;
                    g->buttons[p->pad_button] = 0;
                }
            }
            break;
        case 7: /* gamepad axis */
            if (p->pad_slot < LE_GAMEPAD_MAX_SLOTS &&
                (int)p->pad_axis >= 0 &&
                (int)p->pad_axis < LE_GAMEPAD_AXIS_COUNT) {
                le_gamepad_slot *g =
                    &in->pads[p->pad_slot];
                float v = p->pad_value;

                if (v < -1.0f) {
                    v = -1.0f;
                }
                if (v > 1.0f) {
                    v = 1.0f;
                }
                g->connected = 1;
                g->axes[p->pad_axis] = v;
            }
            break;
        default:
            break;
        }
    }
    in->pending_count = 0;
}

void le_input_end_frame(le_engine *engine) {
    struct le_input_state *in;
    uint32_t s;
    int b;

    if (engine == NULL || engine->input == NULL) {
        return;
    }
    in = engine->input;
    /* Edges + per-frame deltas die at the frame boundary. Held
     * state, positions, text queue persist. (The aggregate action
     * latch updates in advance_frame, not here.) */
    memset(in->key_pressed, 0, sizeof(in->key_pressed));
    memset(in->key_released, 0, sizeof(in->key_released));
    memset(in->mouse_pressed, 0, sizeof(in->mouse_pressed));
    memset(in->mouse_released, 0, sizeof(in->mouse_released));
    in->delta_x = 0.0f;
    in->delta_y = 0.0f;
    in->wheel_x = 0.0f;
    in->wheel_y = 0.0f;
    for (s = 0; s < LE_GAMEPAD_MAX_SLOTS; s++) {
        for (b = 0; b < LE_GAMEPAD_BUTTON_COUNT; b++) {
            in->pads[s].pressed[b] = 0;
            in->pads[s].released[b] = 0;
        }
    }
}

/* ---- raw queries ---- */

int le_input_key_down(le_engine *engine, le_key key) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    if ((int)key <= LE_KEY_UNKNOWN || (int)key >= LE_KEY_COUNT) {
        return 0;
    }
    if (le_input_keyboard_consumed(engine->input)) {
        return 0;
    }
    return engine->input->key_held[(uint32_t)key] ? 1 : 0;
}

int le_input_key_pressed(le_engine *engine, le_key key) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    if ((int)key <= LE_KEY_UNKNOWN || (int)key >= LE_KEY_COUNT) {
        return 0;
    }
    if (le_input_keyboard_consumed(engine->input)) {
        return 0;
    }
    return engine->input->key_pressed[(uint32_t)key] ? 1 : 0;
}

int le_input_key_released(le_engine *engine, le_key key) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    if ((int)key <= LE_KEY_UNKNOWN || (int)key >= LE_KEY_COUNT) {
        return 0;
    }
    if (le_input_keyboard_consumed(engine->input)) {
        return 0;
    }
    return engine->input->key_released[(uint32_t)key] ? 1 : 0;
}

int le_input_mouse_down(le_engine *engine, le_mouse_button button) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    if ((int)button < 0 || (int)button >= LE_MOUSE_BUTTON_COUNT) {
        return 0;
    }
    if (le_input_mouse_consumed(engine->input)) {
        return 0;
    }
    return engine->input->mouse_held[(uint32_t)button] ? 1 : 0;
}

int le_input_mouse_pressed(le_engine *engine,
                           le_mouse_button button) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    if ((int)button < 0 || (int)button >= LE_MOUSE_BUTTON_COUNT) {
        return 0;
    }
    if (le_input_mouse_consumed(engine->input)) {
        return 0;
    }
    return engine->input->mouse_pressed[(uint32_t)button] ? 1 : 0;
}

int le_input_mouse_released(le_engine *engine,
                            le_mouse_button button) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    if ((int)button < 0 || (int)button >= LE_MOUSE_BUTTON_COUNT) {
        return 0;
    }
    if (le_input_mouse_consumed(engine->input)) {
        return 0;
    }
    return engine->input->mouse_released[(uint32_t)button] ? 1 : 0;
}

void le_input_mouse_position(le_engine *engine, float *out_x,
                             float *out_y) {
    if (out_x != NULL) {
        *out_x = 0.0f;
    }
    if (out_y != NULL) {
        *out_y = 0.0f;
    }
    if (engine == NULL || engine->input == NULL) {
        return;
    }
    if (out_x != NULL) {
        *out_x = engine->input->mouse_x;
    }
    if (out_y != NULL) {
        *out_y = engine->input->mouse_y;
    }
}

void le_input_mouse_delta(le_engine *engine, float *out_dx,
                          float *out_dy) {
    if (out_dx != NULL) {
        *out_dx = 0.0f;
    }
    if (out_dy != NULL) {
        *out_dy = 0.0f;
    }
    if (engine == NULL || engine->input == NULL) {
        return;
    }
    if (le_input_mouse_consumed(engine->input)) {
        return;
    }
    if (out_dx != NULL) {
        *out_dx = engine->input->delta_x;
    }
    if (out_dy != NULL) {
        *out_dy = engine->input->delta_y;
    }
}

void le_input_scroll_delta(le_engine *engine, float *out_x,
                           float *out_y) {
    if (out_x != NULL) {
        *out_x = 0.0f;
    }
    if (out_y != NULL) {
        *out_y = 0.0f;
    }
    if (engine == NULL || engine->input == NULL) {
        return;
    }
    if (le_input_mouse_consumed(engine->input)) {
        return;
    }
    if (out_x != NULL) {
        *out_x = engine->input->wheel_x;
    }
    if (out_y != NULL) {
        *out_y = engine->input->wheel_y;
    }
}

uint32_t le_input_mods(le_engine *engine) {
    if (engine == NULL || engine->input == NULL) {
        return (uint32_t)LE_MOD_NONE;
    }
    return engine->input->mods;
}

int le_input_has_focus(le_engine *engine) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    return engine->input->has_focus;
}

le_cursor_mode le_input_set_cursor_mode(le_engine *engine,
                                        le_cursor_mode mode) {
    le_cursor_mode prev = LE_CURSOR_NORMAL;

    if (engine == NULL) {
        return LE_CURSOR_NORMAL;
    }
    prev = engine->cursor_mode;
    /* Platform cursor capture is deferred (no backend support in
     * the current platform abstraction — no hacks here). The mode
     * is recorded so bindings + future backends observe it. */
    if (mode == LE_CURSOR_NORMAL || mode == LE_CURSOR_HIDDEN ||
        mode == LE_CURSOR_CAPTURED) {
        engine->cursor_mode = mode;
    }
    return prev;
}

le_cursor_mode le_input_get_cursor_mode(le_engine *engine) {
    if (engine == NULL) {
        return LE_CURSOR_NORMAL;
    }
    return engine->cursor_mode;
}

int le_input_read_text(le_engine *engine, char *buf,
                       uint32_t buf_cap, uint32_t *out_len) {
    if (out_len != NULL) {
        *out_len = 0;
    }
    if (engine == NULL || engine->input == NULL || buf == NULL ||
        buf_cap == 0) {
        return 0;
    }
    if (engine->input->text_count == 0) {
        buf[0] = '\0';
        return 0;
    }
    {
        struct le_input_state *in = engine->input;
        uint32_t need = in->text_len[in->text_head];

        if (need + 1u > buf_cap) {
            /* Caller buffer too small: report length, keep the
             * scalar queued (no truncation, no loss). */
            if (out_len != NULL) {
                *out_len = need;
            }
            return 0;
        }
        memcpy(buf, in->text[in->text_head], need);
        buf[need] = '\0';
        if (out_len != NULL) {
            *out_len = need;
        }
        in->text_head =
            (in->text_head + 1u) % LE_INPUT_MAX_TEXT;
        in->text_count--;
        return 1;
    }
}

uint32_t le_input_pending_text(le_engine *engine) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    return engine->input->text_count;
}

/* ---- gamepad foundation ---- */

int le_gamepad_is_connected(le_engine *engine, uint32_t slot) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    if (slot >= LE_GAMEPAD_MAX_SLOTS) {
        return 0;
    }
    return engine->input->pads[slot].connected ? 1 : 0;
}

int le_gamepad_button_down(le_engine *engine, uint32_t slot,
                           le_gamepad_button button) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    if (slot >= LE_GAMEPAD_MAX_SLOTS || (int)button < 0 ||
        (int)button >= LE_GAMEPAD_BUTTON_COUNT) {
        return 0;
    }
    if (!engine->input->pads[slot].connected) {
        return 0;
    }
    return engine->input->pads[slot].buttons[button] ? 1 : 0;
}

int le_gamepad_button_pressed(le_engine *engine, uint32_t slot,
                              le_gamepad_button button) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    if (slot >= LE_GAMEPAD_MAX_SLOTS || (int)button < 0 ||
        (int)button >= LE_GAMEPAD_BUTTON_COUNT) {
        return 0;
    }
    if (!engine->input->pads[slot].connected) {
        return 0;
    }
    return engine->input->pads[slot].pressed[button] ? 1 : 0;
}

int le_gamepad_button_released(le_engine *engine, uint32_t slot,
                               le_gamepad_button button) {
    if (engine == NULL || engine->input == NULL) {
        return 0;
    }
    if (slot >= LE_GAMEPAD_MAX_SLOTS || (int)button < 0 ||
        (int)button >= LE_GAMEPAD_BUTTON_COUNT) {
        return 0;
    }
    if (!engine->input->pads[slot].connected) {
        return 0;
    }
    return engine->input->pads[slot].released[button] ? 1 : 0;
}

float le_gamepad_axis_value(le_engine *engine, uint32_t slot,
                            le_gamepad_axis axis) {
    if (engine == NULL || engine->input == NULL) {
        return 0.0f;
    }
    if (slot >= LE_GAMEPAD_MAX_SLOTS || (int)axis < 0 ||
        (int)axis >= LE_GAMEPAD_AXIS_COUNT) {
        return 0.0f;
    }
    if (!engine->input->pads[slot].connected) {
        return 0.0f;
    }
    return engine->input->pads[slot].axes[axis];
}

/* ---- injection (same pending list as platform events) ---- */

le_result le_input_inject_key(le_engine *engine, le_key key,
                              int down) {
    le_pending_event p;

    if (engine == NULL || engine->input == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if ((int)key <= LE_KEY_UNKNOWN || (int)key >= LE_KEY_COUNT) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memset(&p, 0, sizeof(p));
    p.kind = 0;
    p.key = key;
    p.down = down ? 1 : 0;
    p.repeat = 0;
    p.mods = engine->input->mods;
    le_push_pending(engine->input, &p);
    return LE_SUCCESS;
}

le_result le_input_inject_mouse_button(le_engine *engine,
                                       le_mouse_button button,
                                       int down) {
    le_pending_event p;

    if (engine == NULL || engine->input == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if ((int)button < 0 || (int)button >= LE_MOUSE_BUTTON_COUNT) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memset(&p, 0, sizeof(p));
    p.kind = 1;
    p.button = button;
    p.down = down ? 1 : 0;
    p.x = engine->input->mouse_x;
    p.y = engine->input->mouse_y;
    p.mods = engine->input->mods;
    le_push_pending(engine->input, &p);
    return LE_SUCCESS;
}

le_result le_input_inject_mouse_move(le_engine *engine, float x,
                                     float y, float dx, float dy) {
    le_pending_event p;

    if (engine == NULL || engine->input == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memset(&p, 0, sizeof(p));
    p.kind = 2;
    p.x = x;
    p.y = y;
    p.dx = dx;
    p.dy = dy;
    p.mods = engine->input->mods;
    le_push_pending(engine->input, &p);
    return LE_SUCCESS;
}

le_result le_input_inject_scroll(le_engine *engine, float dx,
                                 float dy) {
    le_pending_event p;

    if (engine == NULL || engine->input == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memset(&p, 0, sizeof(p));
    p.kind = 3;
    p.dx = dx;
    p.dy = dy;
    le_push_pending(engine->input, &p);
    return LE_SUCCESS;
}

le_result le_input_inject_text(le_engine *engine, const char *utf8) {
    le_pending_event p;
    size_t n;

    if (engine == NULL || engine->input == NULL || utf8 == NULL ||
        utf8[0] == '\0') {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* One scalar per call (1..4 bytes validated loosely: leading
     * byte sets the width; malformed input rejected). */
    {
        unsigned char c = (unsigned char)utf8[0];

        if (c < 0x80) {
            n = 1;
        } else if ((c & 0xE0) == 0xC0) {
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            n = 4;
        } else {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    }
    memset(&p, 0, sizeof(p));
    p.kind = 4;
    memcpy(p.text, utf8, n);
    p.text[n] = '\0';
    p.text_len = (uint32_t)n;
    p.mods = engine->input->mods;
    le_push_pending(engine->input, &p);
    return LE_SUCCESS;
}

le_result le_input_inject_focus(le_engine *engine, int focused) {
    le_pending_event p;

    if (engine == NULL || engine->input == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memset(&p, 0, sizeof(p));
    p.kind = 5;
    p.focused = focused ? 1 : 0;
    le_push_pending(engine->input, &p);
    return LE_SUCCESS;
}

le_result le_input_inject_gamepad_button(le_engine *engine,
                                         uint32_t slot,
                                         le_gamepad_button b,
                                         int down) {
    le_pending_event p;

    if (engine == NULL || engine->input == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (slot >= LE_GAMEPAD_MAX_SLOTS || (int)b < 0 ||
        (int)b >= LE_GAMEPAD_BUTTON_COUNT) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memset(&p, 0, sizeof(p));
    p.kind = 6;
    p.pad_slot = slot;
    p.pad_button = b;
    p.down = down ? 1 : 0;
    le_push_pending(engine->input, &p);
    return LE_SUCCESS;
}

le_result le_input_inject_gamepad_axis(le_engine *engine,
                                       uint32_t slot,
                                       le_gamepad_axis axis,
                                       float value) {
    le_pending_event p;

    if (engine == NULL || engine->input == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (slot >= LE_GAMEPAD_MAX_SLOTS || (int)axis < 0 ||
        (int)axis >= LE_GAMEPAD_AXIS_COUNT) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (value != value || value > 1e30f || value < -1e30f) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    memset(&p, 0, sizeof(p));
    p.kind = 7;
    p.pad_slot = slot;
    p.pad_axis = axis;
    p.pad_value = value;
    le_push_pending(engine->input, &p);
    return LE_SUCCESS;
}

void le_input_get_stats(le_engine *engine, le_input_stats *out) {
    uint32_t k;
    uint32_t b;
    uint32_t s;

    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (engine == NULL || engine->input == NULL) {
        return;
    }
    {
        struct le_input_state *in = engine->input;

        for (k = 0; k < (uint32_t)LE_KEY_COUNT; k++) {
            if (in->key_held[k]) {
                out->keys_down++;
            }
        }
        for (b = 0; b < (uint32_t)LE_MOUSE_BUTTON_COUNT; b++) {
            if (in->mouse_held[b]) {
                out->mouse_buttons_down++;
            }
        }
        out->events_ingested = in->events_ingested;
        out->action_count = in->action_alive;
        out->axis_count = in->axis_alive;
        for (s = 0; s < LE_INPUT_MAX_CONTEXTS; s++) {
            if (in->contexts[s].alive && in->contexts[s].active) {
                out->active_contexts++;
            }
        }
        for (s = 0; s < LE_GAMEPAD_MAX_SLOTS; s++) {
            if (in->pads[s].connected) {
                out->connected_gamepads++;
            }
        }
        out->pending_events = in->pending_count;
    }
}
