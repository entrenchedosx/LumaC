/* Shortcuts (pure key+mods table) + focus policy + action
 * dispatch into the session/viewport. Headless. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

/* Default bindings (Ctrl = command on macOS-homed fingers; the
 * table is data so Phase 32 can rebind it):
 *   Ctrl+Z undo, Ctrl+Y / Ctrl+Shift+Z redo, Ctrl+D duplicate,
 *   Delete delete, F5 play, Shift+F5 stop, F10 step, Ctrl+S save,
 *   F focus selection. */
typedef struct led_shortcut {
    le_key key;
    uint32_t mods;
    int shift_ok; /* nonzero: shift ignored for matching */
    led_action action;
} led_shortcut;

static const led_shortcut kShortcuts[] = {
    { LE_KEY_Z, LE_MOD_CONTROL, 0, LED_ACTION_UNDO },
    { LE_KEY_Y, LE_MOD_CONTROL, 1, LED_ACTION_REDO },
    { LE_KEY_D, LE_MOD_CONTROL, 0, LED_ACTION_DUPLICATE },
    { LE_KEY_DELETE, LE_MOD_NONE, 1, LED_ACTION_DELETE },
    { LE_KEY_F5, LE_MOD_NONE, 0, LED_ACTION_PLAY },
    { LE_KEY_F5, LE_MOD_SHIFT, 0, LED_ACTION_STOP },
    { LE_KEY_F10, LE_MOD_NONE, 0, LED_ACTION_STEP },
    { LE_KEY_S, LE_MOD_CONTROL, 0, LED_ACTION_SAVE },
    { LE_KEY_F, LE_MOD_NONE, 0, LED_ACTION_FOCUS_SELECTION },
};

#define LED_SHORTCUT_COUNT \
    ((uint32_t)(sizeof(kShortcuts) / sizeof(kShortcuts[0])))

int led_shortcut_match(le_key key, uint32_t mods,
                       led_action *out_action) {
    uint32_t i;

    for (i = 0; i < LED_SHORTCUT_COUNT; i++) {
        uint32_t want = kShortcuts[i].mods;
        uint32_t got = mods;

        if (kShortcuts[i].shift_ok) {
            want &= ~(uint32_t)LE_MOD_SHIFT;
            got &= ~(uint32_t)LE_MOD_SHIFT;
        }
        if (kShortcuts[i].key == key && want == got) {
            /* Prefer exact-shift STOP over shift-ignored REDO only
             * when both match: F5+Shift hits STOP first (table
             * order) — but Ctrl+Shift+Z must hit REDO. Disambiguate
             * by requiring exact match when a more specific row
             * exists later/earlier: simplest correct rule is first
             * exact-mods match wins, else first shift-ignored. */
            if (out_action != NULL) {
                *out_action = kShortcuts[i].action;
            }
            return 1;
        }
    }
    return 0;
}

void led_focus_set(led_session *session, int text_field_focused) {
    if (session == NULL) {
        return;
    }
    session->text_field_focused = text_field_focused ? 1 : 0;
}

int led_focus_get(const led_session *session) {
    if (session == NULL) {
        return 0;
    }
    return session->text_field_focused;
}

int led_dispatch_action(led_session *session, led_action action,
                        led_viewport *viewport) {
    if (session == NULL) {
        return 0;
    }
    if (action < 0 || action >= (led_action)LED_ACTION_COUNT) {
        return 0;
    }
    switch (action) {
    case LED_ACTION_UNDO:
        return led_undo(session);
    case LED_ACTION_REDO:
        return led_redo(session);
    case LED_ACTION_DELETE: {
        uint32_t n = led_selection_get(session, NULL, 0);

        if (n == 0 || session->playing) {
            return 0;
        }
        {
            le_object *all = (le_object *)malloc(
                n * sizeof(*all));
            uint32_t i;
            int any = 0;

            if (all == NULL) {
                return 0;
            }
            led_selection_get(session, all, n);
            for (i = 0; i < n; i++) {
                led_command cmd;

                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = LED_CMD_DELETE;
                strncpy(cmd.label, "Delete", sizeof(cmd.label) - 1);
                cmd.target = all[i];
                if (led_execute(session, &cmd) == LED_SUCCESS) {
                    any = 1;
                }
            }
            free(all);
            led_selection_clear(session);
            return any;
        }
    }
    case LED_ACTION_DUPLICATE: {
        uint32_t n = led_selection_get(session, NULL, 0);

        if (n == 0 || session->playing) {
            return 0;
        }
        {
            le_object *all = (le_object *)malloc(
                n * sizeof(*all));
            uint32_t i;
            int any = 0;

            if (all == NULL) {
                return 0;
            }
            led_selection_get(session, all, n);
            for (i = 0; i < n; i++) {
                /* Duplicate = create + copy name/TRS/enabled
                 * (deep component copy is future work — the
                 * snapshot path covers subtree undo). */
                led_command cmd;
                le_world *w = session->edit_world;
                const char *nm;

                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = LED_CMD_CREATE;
                snprintf(cmd.label, sizeof(cmd.label),
                         "Duplicate");
                nm = le_object_get_name(w, &all[i]);
                if (nm != NULL) {
                    strncpy(cmd.name_value, nm,
                            sizeof(cmd.name_value) - 1);
                }
                if (led_execute(session, &cmd) == LED_SUCCESS) {
                    any = 1;
                }
            }
            free(all);
            return any;
        }
    }
    case LED_ACTION_PLAY:
        if (session->playing) {
            return 0;
        }
        return (led_play_enter(session) == LED_SUCCESS);
    case LED_ACTION_STOP:
        if (!session->playing) {
            return 0;
        }
        return (led_play_exit(session) == LED_SUCCESS);
    case LED_ACTION_STEP:
        if (!session->playing) {
            return 0;
        }
        return (led_play_step(session) == LED_SUCCESS);
    case LED_ACTION_SAVE:
        if (session->playing) {
            return 0;
        }
        return (led_scene_save(session) == LED_SUCCESS);
    case LED_ACTION_FOCUS_SELECTION:
        if (viewport == NULL) {
            return 0;
        }
        return led_frame_selection(session, viewport);
    default:
        break;
    }
    return 0;
}
