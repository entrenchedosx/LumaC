/*
 * Engine input internals (Phase 27). Never public, never includes
 * backend headers — the engine consumes only the backend-neutral
 * lc_window_event queue. All gameplay input state lives here, per
 * le_engine; worlds share one finalized snapshot.
 *
 * Frame model:
 * - Events (platform drain + injection) land in `pending[]`.
 * - le_input_advance_frame() folds pending into `held[]`, computes
 *   pressed/released edges, accumulates motion/wheel deltas, then
 *   clears pending. Scripts observe the snapshot until the next
 *   advance. le_input_end_frame() clears edges + per-frame deltas.
 * - Press+release within one frame: both edges set, held cleared
 *   (explicitly tested contract).
 * - Focus loss clears held keys/buttons immediately (no stuck keys);
 *   the release edge is NOT synthesized (motion already stopped —
 *   synthesizing would re-trigger taps; documented + tested).
 */

#ifndef LE_INPUT_INTERNAL_H
#define LE_INPUT_INTERNAL_H

#include <stdint.h>

#include "luma_engine/luma_engine.h"

#define LE_INPUT_MAX_PENDING 1024u
#define LE_INPUT_MAX_ACTIONS 4096u
#define LE_INPUT_MAX_AXES 1024u
#define LE_INPUT_MAX_CONTEXTS 64u
#define LE_INPUT_MAX_BINDINGS_PER_ENTRY 16u
#define LE_INPUT_MAX_TEXT 256u

/* Pending event: same shape for platform + injected input. */
typedef struct le_pending_event {
    int kind; /* 0=key 1=mouse_button 2=mouse_move 3=wheel 4=text
               * 5=focus 6=gamepad_button 7=gamepad_axis */
    le_key key;
    int down;
    int repeat; /* platform auto-repeat flag (never makes edges) */
    le_mouse_button button;
    float x;
    float y;
    float dx;
    float dy;
    char text[8];
    uint32_t text_len;
    int focused;
    uint32_t pad_slot;
    le_gamepad_button pad_button;
    le_gamepad_axis pad_axis;
    float pad_value;
    uint32_t mods;
} le_pending_event;

typedef struct le_action_slot {
    int alive;
    uint32_t generation;
    char name[128];
    uint64_t name_hash;
    le_input_binding bindings[LE_INPUT_MAX_BINDINGS_PER_ENTRY];
    uint32_t binding_count;
    int narrowed; /* nonzero once bound into any context */
    /* Aggregate edge latch: previous frame's aggregate down. */
    int was_down;
    /* Owning context indices (narrowed set); empty = global. */
    uint32_t contexts[LE_INPUT_MAX_CONTEXTS];
    uint32_t context_count;
} le_action_slot;

typedef struct le_axis_slot {
    int alive;
    uint32_t generation;
    char name[128];
    uint64_t name_hash;
    float deadzone;
    float scale;
    int invert;
    le_input_binding bindings[LE_INPUT_MAX_BINDINGS_PER_ENTRY];
    uint32_t binding_count;
    int narrowed;
    uint32_t contexts[LE_INPUT_MAX_CONTEXTS];
    uint32_t context_count;
} le_axis_slot;

typedef struct le_context_slot {
    int alive;
    uint32_t generation;
    char name[64];
    int priority;
    int active;
    uint32_t consume_mask;
} le_context_slot;

typedef struct le_gamepad_slot {
    int connected;
    uint32_t generation;
    uint8_t buttons[LE_GAMEPAD_BUTTON_COUNT];
    uint8_t pressed[LE_GAMEPAD_BUTTON_COUNT];
    uint8_t released[LE_GAMEPAD_BUTTON_COUNT];
    float axes[LE_GAMEPAD_AXIS_COUNT];
} le_gamepad_slot;

struct le_input_state {
    /* Raw held state + per-frame edges (indexed by le_key value). */
    uint8_t key_held[LE_KEY_COUNT];
    uint8_t key_pressed[LE_KEY_COUNT];
    uint8_t key_released[LE_KEY_COUNT];
    uint8_t mouse_held[LE_MOUSE_BUTTON_COUNT];
    uint8_t mouse_pressed[LE_MOUSE_BUTTON_COUNT];
    uint8_t mouse_released[LE_MOUSE_BUTTON_COUNT];
    float mouse_x;
    float mouse_y;
    float delta_x;
    float delta_y;
    float wheel_x;
    float wheel_y;
    uint32_t mods;
    int has_focus;
    /* Text queue (ring of pending scalars). */
    char text[LE_INPUT_MAX_TEXT][8];
    uint32_t text_len[LE_INPUT_MAX_TEXT];
    uint32_t text_head;
    uint32_t text_count;
    /* Pending pre-frame events (platform + injection share it). */
    le_pending_event pending[LE_INPUT_MAX_PENDING];
    uint32_t pending_count;
    uint64_t events_ingested;
    /* Registry. */
    le_action_slot *actions;
    uint32_t action_cap;
    uint32_t action_alive;
    le_axis_slot *axes;
    uint32_t axis_cap;
    uint32_t axis_alive;
    le_context_slot contexts[LE_INPUT_MAX_CONTEXTS];
    uint32_t context_alive;
    le_gamepad_slot pads[LE_GAMEPAD_MAX_SLOTS];
};

/* Lifecycle (input.c). */
struct le_input_state *le_input_create(void);
void le_input_destroy(struct le_input_state *in);
/* Drain platform queues (all windows) + focus-window tracking into
 * pending. Called by le_engine_begin_frame. */
void le_input_poll_platform(le_engine *engine);
/* Fold pending into the snapshot (edges, deltas, focus-clear). */
void le_input_advance_frame(le_engine *engine);
/* Clear edges + per-frame deltas (end of frame). */
void le_input_end_frame(le_engine *engine);

/* Registry helpers shared with lifecycle.c (none public). */
uint64_t le_input_hash_name(const char *name);
int le_input_resolve_action(const struct le_input_state *in,
                            const le_input_action *h,
                            uint32_t *out_idx);
int le_input_resolve_axis(const struct le_input_state *in,
                          const le_input_axis *h, uint32_t *out_idx);
int le_input_resolve_context(const struct le_input_state *in,
                             const le_input_context *h,
                             uint32_t *out_idx);
/* Nonzero when the action/axis is visible in the current active
 * set (global entries always visible; narrowed entries need an
 * active owning context). */
int le_input_action_visible(struct le_input_state *in,
                            const le_action_slot *a);
int le_input_axis_visible(struct le_input_state *in,
                          const le_axis_slot *a);
/* Consumption: top active context (highest priority) may consume
 * keyboard/mouse domains for gameplay queries below it. */
int le_input_keyboard_consumed(struct le_input_state *in);
int le_input_mouse_consumed(struct le_input_state *in);

#endif /* LE_INPUT_INTERNAL_H */
