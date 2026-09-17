/* Phase 33 GUI input bridge (isolated C++ over public C ABIs).
 *
 * Maps lc_window_event values (lc_poll_events + lc_window_read_event)
 * onto Dear ImGui io inputs. No OS headers here: key identity stays
 * in the lc_keycode numbering (mirrors le_key ordering); mouse is
 * client px (origin top-left); wheel is detents; text is UTF-8.
 *
 * Ownership split (documented, no leaks either way): the HOST owns
 * the event queue (lc_window_read_event drains; focus/close/resize
 * stay host-owned). leg_feed_event maps ONE event value (never
 * drains); leg_frame_begin drains the queue (each event mapped once)
 * so a host that ALSO drains would double-consume — the contract is:
 * hosts call leg_frame_begin (which drains) and NEVER drain
 * separately while a GUI context is active; leg_feed_event exists
 * for headless tests + synthetic injection only.
 */

#include <cstring>

#include "gui_internal.h"

/* lc_keycode -> ImGuiKey. Both numberings mirror the same physical
 * layout (LC_KEY_A == 1 ... == LE_KEY_A); ImGui numbers from 512.
 * Returns ImGuiKey_None for unknown/unmapped (never crashes). */
static ImGuiKey leg_map_key(lc_keycode key) {
    switch (key) {
    case LC_KEY_A: return ImGuiKey_A;
    case LC_KEY_B: return ImGuiKey_B;
    case LC_KEY_C: return ImGuiKey_C;
    case LC_KEY_D: return ImGuiKey_D;
    case LC_KEY_E: return ImGuiKey_E;
    case LC_KEY_F: return ImGuiKey_F;
    case LC_KEY_G: return ImGuiKey_G;
    case LC_KEY_H: return ImGuiKey_H;
    case LC_KEY_I: return ImGuiKey_I;
    case LC_KEY_J: return ImGuiKey_J;
    case LC_KEY_K: return ImGuiKey_K;
    case LC_KEY_L: return ImGuiKey_L;
    case LC_KEY_M: return ImGuiKey_M;
    case LC_KEY_N: return ImGuiKey_N;
    case LC_KEY_O: return ImGuiKey_O;
    case LC_KEY_P: return ImGuiKey_P;
    case LC_KEY_Q: return ImGuiKey_Q;
    case LC_KEY_R: return ImGuiKey_R;
    case LC_KEY_S: return ImGuiKey_S;
    case LC_KEY_T: return ImGuiKey_T;
    case LC_KEY_U: return ImGuiKey_U;
    case LC_KEY_V: return ImGuiKey_V;
    case LC_KEY_W: return ImGuiKey_W;
    case LC_KEY_X: return ImGuiKey_X;
    case LC_KEY_Y: return ImGuiKey_Y;
    case LC_KEY_Z: return ImGuiKey_Z;
    case LC_KEY_0: return ImGuiKey_0;
    case LC_KEY_1: return ImGuiKey_1;
    case LC_KEY_2: return ImGuiKey_2;
    case LC_KEY_3: return ImGuiKey_3;
    case LC_KEY_4: return ImGuiKey_4;
    case LC_KEY_5: return ImGuiKey_5;
    case LC_KEY_6: return ImGuiKey_6;
    case LC_KEY_7: return ImGuiKey_7;
    case LC_KEY_8: return ImGuiKey_8;
    case LC_KEY_9: return ImGuiKey_9;
    case LC_KEY_ESCAPE: return ImGuiKey_Escape;
    case LC_KEY_ENTER: return ImGuiKey_Enter;
    case LC_KEY_TAB: return ImGuiKey_Tab;
    case LC_KEY_SPACE: return ImGuiKey_Space;
    case LC_KEY_BACKSPACE: return ImGuiKey_Backspace;
    case LC_KEY_LEFT_SHIFT: return ImGuiKey_LeftShift;
    case LC_KEY_RIGHT_SHIFT: return ImGuiKey_RightShift;
    case LC_KEY_LEFT_CONTROL: return ImGuiKey_LeftCtrl;
    case LC_KEY_RIGHT_CONTROL: return ImGuiKey_RightCtrl;
    case LC_KEY_LEFT_ALT: return ImGuiKey_LeftAlt;
    case LC_KEY_RIGHT_ALT: return ImGuiKey_RightAlt;
    case LC_KEY_LEFT_SUPER: return ImGuiKey_LeftSuper;
    case LC_KEY_RIGHT_SUPER: return ImGuiKey_RightSuper;
    case LC_KEY_LEFT: return ImGuiKey_LeftArrow;
    case LC_KEY_RIGHT: return ImGuiKey_RightArrow;
    case LC_KEY_UP: return ImGuiKey_UpArrow;
    case LC_KEY_DOWN: return ImGuiKey_DownArrow;
    case LC_KEY_INSERT: return ImGuiKey_Insert;
    case LC_KEY_DELETE: return ImGuiKey_Delete;
    case LC_KEY_HOME: return ImGuiKey_Home;
    case LC_KEY_END: return ImGuiKey_End;
    case LC_KEY_PAGE_UP: return ImGuiKey_PageUp;
    case LC_KEY_PAGE_DOWN: return ImGuiKey_PageDown;
    case LC_KEY_F1: return ImGuiKey_F1;
    case LC_KEY_F2: return ImGuiKey_F2;
    case LC_KEY_F3: return ImGuiKey_F3;
    case LC_KEY_F4: return ImGuiKey_F4;
    case LC_KEY_F5: return ImGuiKey_F5;
    case LC_KEY_F6: return ImGuiKey_F6;
    case LC_KEY_F7: return ImGuiKey_F7;
    case LC_KEY_F8: return ImGuiKey_F8;
    case LC_KEY_F9: return ImGuiKey_F9;
    case LC_KEY_F10: return ImGuiKey_F10;
    case LC_KEY_F11: return ImGuiKey_F11;
    case LC_KEY_F12: return ImGuiKey_F12;
    case LC_KEY_MINUS: return ImGuiKey_Minus;
    case LC_KEY_EQUAL: return ImGuiKey_Equal;
    case LC_KEY_LEFT_BRACKET: return ImGuiKey_LeftBracket;
    case LC_KEY_RIGHT_BRACKET: return ImGuiKey_RightBracket;
    case LC_KEY_BACKSLASH: return ImGuiKey_Backslash;
    case LC_KEY_SEMICOLON: return ImGuiKey_Semicolon;
    case LC_KEY_APOSTROPHE: return ImGuiKey_Apostrophe;
    case LC_KEY_GRAVE: return ImGuiKey_GraveAccent;
    case LC_KEY_COMMA: return ImGuiKey_Comma;
    case LC_KEY_PERIOD: return ImGuiKey_Period;
    case LC_KEY_SLASH: return ImGuiKey_Slash;
    case LC_KEY_CAPS_LOCK: return ImGuiKey_CapsLock;
    default: break;
    }
    /* Numpad block (LC_KEY_NUMPAD_* = 110..126). */
    if (key >= LC_KEY_NUMPAD_0 && key <= LC_KEY_NUMPAD_9) {
        return (ImGuiKey)(ImGuiKey_Keypad0 + (key - LC_KEY_NUMPAD_0));
    }
    switch (key) {
    case LC_KEY_NUMPAD_DECIMAL: return ImGuiKey_KeypadDecimal;
    case LC_KEY_NUMPAD_DIVIDE: return ImGuiKey_KeypadDivide;
    case LC_KEY_NUMPAD_MULTIPLY: return ImGuiKey_KeypadMultiply;
    case LC_KEY_NUMPAD_SUBTRACT: return ImGuiKey_KeypadSubtract;
    case LC_KEY_NUMPAD_ADD: return ImGuiKey_KeypadAdd;
    case LC_KEY_NUMPAD_ENTER: return ImGuiKey_KeypadEnter;
    case LC_KEY_NUMPAD_EQUAL: return ImGuiKey_KeypadEqual;
    default: break;
    }
    return ImGuiKey_None;
}

/* Map one event value into ImGui io. Returns 1 when consumed as GUI
 * input, 0 when ignored. Never touches the window queue (no drain):
 * the caller owns queue discipline. Modifiers ride every key/mouse
 * event (lc_key_mod bitmask mirrors ImGuiMod_*); focus/close/resize
 * are host-owned (ignored here, never consumed). */
static int leg_map_one(ImGuiIO &io, const lc_window_event *event) {
    if (event == NULL) {
        return 0;
    }
    switch (event->type) {
    case LC_EVENT_KEY_DOWN:
    case LC_EVENT_KEY_UP: {
        ImGuiKey mapped = leg_map_key(event->key);
        int down = (event->type == LC_EVENT_KEY_DOWN) ? 1 : 0;

        /* Modifier keys ALSO update the mod snapshot ImGui reads for
         * shortcuts (Ctrl+Z etc.): mirror lc_key_mod -> io mods. */
        if (event->key == LC_KEY_LEFT_SHIFT ||
            event->key == LC_KEY_RIGHT_SHIFT) {
            io.AddKeyEvent(ImGuiKey_LeftShift,
                           (event->mods & LC_MOD_SHIFT) != 0);
            io.AddKeyEvent(ImGuiKey_RightShift, false);
        } else if (event->key == LC_KEY_LEFT_CONTROL ||
                   event->key == LC_KEY_RIGHT_CONTROL) {
            io.AddKeyEvent(ImGuiKey_LeftCtrl,
                           (event->mods & LC_MOD_CONTROL) != 0);
            io.AddKeyEvent(ImGuiKey_RightCtrl, false);
        } else if (event->key == LC_KEY_LEFT_ALT ||
                   event->key == LC_KEY_RIGHT_ALT) {
            io.AddKeyEvent(ImGuiKey_LeftAlt,
                           (event->mods & LC_MOD_ALT) != 0);
            io.AddKeyEvent(ImGuiKey_RightAlt, false);
        } else if (event->key == LC_KEY_LEFT_SUPER ||
                   event->key == LC_KEY_RIGHT_SUPER) {
            io.AddKeyEvent(ImGuiKey_LeftSuper,
                           (event->mods & LC_MOD_SUPER) != 0);
            io.AddKeyEvent(ImGuiKey_RightSuper, false);
        }
        if (mapped == ImGuiKey_None) {
            return 0;
        }
        /* Auto-repeat still feeds held state (down == down); edge
         * consumers (IsKeyPressed) dedupe internally. */
        io.AddKeyEvent(mapped, down != 0);
        return 1;
    }
    case LC_EVENT_CHAR: {
        unsigned int codepoint = 0;
        const unsigned char *bytes =
            (const unsigned char *)event->utf8;
        uint32_t len = event->utf8_len;

        /* Decode one UTF-8 scalar (1..4 bytes, validated closed). */
        if (len == 0 || len > 4) {
            return 0;
        }
        if (len == 1) {
            if (bytes[0] < 0x20 || bytes[0] == 0x7F) {
                return 0; /* control chars never reach fields */
            }
            codepoint = bytes[0];
        } else {
            uint32_t i = 0;
            unsigned int min = 0;

            if ((bytes[0] & 0xE0) == 0xC0 && len == 2) {
                codepoint = bytes[0] & 0x1F;
                min = 0x80;
            } else if ((bytes[0] & 0xF0) == 0xE0 && len == 3) {
                codepoint = bytes[0] & 0x0F;
                min = 0x800;
            } else if ((bytes[0] & 0xF8) == 0xF0 && len == 4) {
                codepoint = bytes[0] & 0x07;
                min = 0x10000;
            } else {
                return 0;
            }
            for (i = 1; i < len; i++) {
                if ((bytes[i] & 0xC0) != 0x80) {
                    return 0;
                }
                codepoint =
                    (codepoint << 6) | (bytes[i] & 0x3F);
            }
            if (codepoint < min || codepoint > 0x10FFFF ||
                (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
                return 0;
            }
        }
        io.AddInputCharacter(codepoint);
        return 1;
    }
    case LC_EVENT_MOUSE_DOWN:
    case LC_EVENT_MOUSE_UP: {
        int down = (event->type == LC_EVENT_MOUSE_DOWN) ? 1 : 0;
        int button = -1;

        switch (event->button) {
        case LC_MOUSE_LEFT: button = 0; break;
        case LC_MOUSE_RIGHT: button = 1; break;
        case LC_MOUSE_MIDDLE: button = 2; break;
        case LC_MOUSE_4: button = 3; break;
        case LC_MOUSE_5: button = 4; break;
        default: return 0;
        }
        io.AddMousePosEvent(event->mouse_x, event->mouse_y);
        io.AddMouseButtonEvent(button, down != 0);
        return 1;
    }
    case LC_EVENT_MOUSE_MOVE:
        io.AddMousePosEvent(event->mouse_x, event->mouse_y);
        return 1;
    case LC_EVENT_MOUSE_WHEEL:
        io.AddMouseWheelEvent(event->wheel_x, event->wheel_y);
        return 1;
    default:
        /* FOCUS_GAINED/LOST, RESIZE, CLOSE, NONE: host-owned. */
        break;
    }
    return 0;
}

int leg_feed_event(leg_context *context,
                   const lc_window_event *event) {
    if (context == NULL || event == NULL) {
        return 0;
    }
    if (context->imgui == NULL) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    return leg_map_one(ImGui::GetIO(), event);
}

/* Frame-drain alias (same mapping; separate symbol so the audit can
 * tell headless probes from the loop drain). */
int leg_map_one_for_frame(ImGuiIO &io,
                          const lc_window_event *event) {
    return leg_map_one(io, event);
}

int leg_wants_keyboard(const leg_context *context) {
    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    {
        const ImGuiIO &io = ImGui::GetIO();

        return (io.WantCaptureKeyboard != 0) ? 1 : 0;
    }
}

int leg_wants_mouse(const leg_context *context) {
    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    {
        const ImGuiIO &io = ImGui::GetIO();

        return (io.WantCaptureMouse != 0) ? 1 : 0;
    }
}
