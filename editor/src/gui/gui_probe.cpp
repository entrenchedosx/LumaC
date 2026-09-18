/* Phase 34A probe snapshot (isolated C++ over public C ABIs).
 *
 * Observation ONLY: records last-frame widget rects (toolbar tool
 * buttons, viewport capture, asset rows, hierarchy rows) plus the
 * projected gizmo handle math (mirroring the overlay's own
 * projection). No led calls, no state mutation — the harness
 * reads geometry here, then drives REAL input through
 * lc_window_inject_event, then asserts led and le state.
 *
 * Confinement: this TU may include imgui.h (see gui_internal.h).
 */

#include <cmath>
#include <cstdio>
#include <cstring>

#include "gui_internal.h"

void leg_probe_record_tool(leg_ui *ui, const char *id) {
    uint32_t k = 0;

    if (ui == NULL || id == NULL || id[0] == '\0') {
        return;
    }
    /* The Button is the last item: capture its rect NOW. */
    {
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();

        for (k = 0; k < ui->tool_count; k++) {
            if (strcmp(ui->tools[k].id, id) == 0) {
                break;
            }
        }
        if (k >= LEG_PROBE_TOOLS) {
            return;
        }
        if (k == ui->tool_count) {
            strncpy(ui->tools[k].id, id,
                    sizeof(ui->tools[k].id) - 1);
            ui->tools[k].id[sizeof(ui->tools[k].id) - 1] =
                '\0';
            ui->tool_count++;
        }
        ui->tools[k].x = rmin.x;
        ui->tools[k].y = rmin.y;
        ui->tools[k].w = rmax.x - rmin.x;
        ui->tools[k].h = rmax.y - rmin.y;
        ui->tools[k].valid = 1;
    }
}

void leg_probe_record_viewport(leg_ui *ui) {
    ImVec2 rmin;
    ImVec2 rmax;

    if (ui == NULL) {
        return;
    }
    /* The ##vp-capture InvisibleButton is the last item. Its rect
     * is in SCREEN px (DisplayPos-relative == client px: the host
     * window sits at the origin). */
    rmin = ImGui::GetItemRectMin();
    rmax = ImGui::GetItemRectMax();
    ui->vp_x = rmin.x;
    ui->vp_y = rmin.y;
    ui->vp_w = rmax.x - rmin.x;
    ui->vp_h = rmax.y - rmin.y;
    ui->vp_valid = 1;
}

void leg_probe_record_asset(leg_ui *ui, const char *path) {
    uint32_t k = 0;
    const char *leaf = NULL;

    if (ui == NULL || path == NULL) {
        return;
    }
    /* Store by leaf name (the row label shows the leaf; paths
     * stay unique in the probe table by full path). */
    (void)leaf;
    for (k = 0; k < ui->asset_count; k++) {
        if (strcmp(ui->assets[k].path, path) == 0) {
            break;
        }
    }
    if (k >= LEG_PROBE_ASSETS) {
        return;
    }
    if (k == ui->asset_count) {
        strncpy(ui->assets[k].path, path,
                sizeof(ui->assets[k].path) - 1);
        ui->assets[k].path[sizeof(ui->assets[k].path) - 1] =
            '\0';
        ui->asset_count++;
    }
    {
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();

        ui->assets[k].x = rmin.x;
        ui->assets[k].y = rmin.y;
        ui->assets[k].w = rmax.x - rmin.x;
        ui->assets[k].h = rmax.y - rmin.y;
        ui->assets[k].valid = 1;
    }
}

void leg_probe_record_hier(leg_ui *ui, const le_object *obj) {
    uint32_t k = 0;

    if (ui == NULL || obj == NULL) {
        return;
    }
    for (k = 0; k < ui->hier_count; k++) {
        if (ui->hier[k].index == obj->index &&
            ui->hier[k].generation == obj->generation &&
            ui->hier[k].world_tag == obj->world_tag) {
            break;
        }
    }
    if (k >= LEG_PROBE_HIER) {
        return;
    }
    if (k == ui->hier_count) {
        ui->hier[k].index = obj->index;
        ui->hier[k].generation = obj->generation;
        ui->hier[k].world_tag = obj->world_tag;
        ui->hier_count++;
    }
    {
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();

        ui->hier[k].x = rmin.x;
        ui->hier[k].y = rmin.y;
        ui->hier[k].w = rmax.x - rmin.x;
        ui->hier[k].h = rmax.y - rmin.y;
        ui->hier[k].valid = 1;
    }
}

/* Public probe queries (C ABI): read the snapshot, never mutate. */

/* Main-menu label + open-menu item recorders (the label/item is
 * the last submitted item when called; screen px like tools). */
void leg_probe_record_menu(leg_ui *ui, const char *label) {
    uint32_t k = 0;

    if (ui == NULL || label == NULL || label[0] == '\0') {
        return;
    }
    for (k = 0; k < ui->menu_count; k++) {
        if (strcmp(ui->menus[k].label, label) == 0) {
            break;
        }
    }
    if (k >= LEG_PROBE_MENUS) {
        return;
    }
    if (k == ui->menu_count) {
        strncpy(ui->menus[k].label, label,
                sizeof(ui->menus[k].label) - 1);
        ui->menus[k].label[sizeof(ui->menus[k].label) - 1] =
            '\0';
        ui->menu_count++;
    }
    {
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();

        ui->menus[k].x = rmin.x;
        ui->menus[k].y = rmin.y;
        ui->menus[k].w = rmax.x - rmin.x;
        ui->menus[k].h = rmax.y - rmin.y;
        ui->menus[k].valid = 1;
    }
}

void leg_probe_record_menu_item(leg_ui *ui, const char *menu,
                                const char *item) {
    uint32_t k = 0;

    if (ui == NULL || menu == NULL || item == NULL ||
        menu[0] == '\0' || item[0] == '\0') {
        return;
    }
    for (k = 0; k < ui->menu_item_count; k++) {
        if (strcmp(ui->menu_items[k].menu, menu) == 0 &&
            strcmp(ui->menu_items[k].item, item) == 0) {
            break;
        }
    }
    if (k >= LEG_PROBE_MENU_ITEMS) {
        return;
    }
    if (k == ui->menu_item_count) {
        strncpy(ui->menu_items[k].menu, menu,
                sizeof(ui->menu_items[k].menu) - 1);
        ui->menu_items[k].menu[sizeof(ui->menu_items[k].menu) -
                               1] = '\0';
        strncpy(ui->menu_items[k].item, item,
                sizeof(ui->menu_items[k].item) - 1);
        ui->menu_items[k].item[sizeof(ui->menu_items[k].item) -
                               1] = '\0';
        ui->menu_item_count++;
    }
    {
        ImVec2 rmin = ImGui::GetItemRectMin();
        ImVec2 rmax = ImGui::GetItemRectMax();

        ui->menu_items[k].x = rmin.x;
        ui->menu_items[k].y = rmin.y;
        ui->menu_items[k].w = rmax.x - rmin.x;
        ui->menu_items[k].h = rmax.y - rmin.y;
        ui->menu_items[k].valid = 1;
    }
}

/* Focus-loss reset (called by the frame drain's FOCUS_LOST branch
 * in gui_loop.cpp): force-release every latched key into the
 * engine NOW? No engine handle here — instead mark all `sent[]`
 * latched WITHOUT emitting (ImGui state was wiped by
 * ClearInputKeys, so the next consume call sees up and emits the
 * release then). Simpler + race-free: clear the latch AND inject
 * nothing; the engine's own focus-clear (input.c kind-5) already
 * dropped held state on... no — the engine never sees FOCUS_LOST
 * (GUI owns the queue). So: remember the wipe; the next
 * leg_consume_play_input emits key-UP for every latched key. */
static int g_focus_wiped = 0;

void leg_consume_play_input_reset(void) {
    g_focus_wiped = 1;
}

/* Phase 34A mutation arm: M-input stubs the play bridge (H8's
 * runtime object never moves while armed). */
#if defined(LUMA34A_MUT_INPUT)
#define LEG_MUT_INPUT 1
#else
#define LEG_MUT_INPUT 0
#endif

int leg_consume_play_input(leg_context *context,
                           le_engine *engine) {
    int injected = 0;

    if (LEG_MUT_INPUT) {
        return 0;
    }
    if (context == NULL || engine == NULL) {
        return 0;
    }
    if (context->imgui == NULL || context->session == NULL) {
        return 0;
    }
    /* Only while playing (edit mode keeps keys for the camera). */
    if (!led_is_playing(context->session)) {
        return 0;
    }
    /* Text fields own the keyboard: keystrokes never leak into
     * gameplay (the exact Phase 33 anti-leak policy). */
    if (leg_wants_keyboard(context)) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    {
        struct {
            ImGuiKey imgui;
            le_key engine;
        } keys[] = {
            { ImGuiKey_W, LE_KEY_W },
            { ImGuiKey_A, LE_KEY_A },
            { ImGuiKey_S, LE_KEY_S },
            { ImGuiKey_D, LE_KEY_D },
            { ImGuiKey_UpArrow, LE_KEY_UP },
            { ImGuiKey_DownArrow, LE_KEY_DOWN },
            { ImGuiKey_LeftArrow, LE_KEY_LEFT },
            { ImGuiKey_RightArrow, LE_KEY_RIGHT },
            { ImGuiKey_Space, LE_KEY_SPACE },
        };
        size_t k = 0;

        /* Edge-true injection: press => down+up pair ordering is
         * owned by ImGui state (IsKeyPressed edge + IsKeyDown
         * held). The engine folds pending per led_play_tick, so:
         * fresh press this frame => inject down (held latches);
         * release observed (was down, now up) => inject up.
         * Track last-sent state in GUI-owned static storage (per
         * context would be cleaner; one context per process in
         * practice — documented). */
        static int sent[9] = { 0, 0, 0, 0, 0, 0, 0, 0, 0 };

        /* Focus wipe (see reset above): emit UP for every latched
         * key even though ImGui shows up (state was cleared). */
        if (g_focus_wiped) {
            for (k = 0;
                 k < sizeof(keys) / sizeof(keys[0]); k++) {
                if (sent[k]) {
                    le_input_inject_key(engine, keys[k].engine,
                                        0);
                    sent[k] = 0;
                }
            }
            g_focus_wiped = 0;
            /* Also clear the ENGINE's held state directly? The
             * injected UPs fold on the next led_play_tick — the
             * caller ticks right after consume, so held drops
             * this frame. Nothing more needed here. */
        }
        for (k = 0;
             k < sizeof(keys) / sizeof(keys[0]); k++) {
            int down =
                ImGui::IsKeyDown(keys[k].imgui) ? 1 : 0;

            if (down && !sent[k]) {
                if (le_input_inject_key(engine, keys[k].engine,
                                        1) == LE_SUCCESS) {
                    injected++;
                }
                sent[k] = 1;
            } else if (!down && sent[k]) {
                if (le_input_inject_key(engine, keys[k].engine,
                                        0) == LE_SUCCESS) {
                    injected++;
                }
                sent[k] = 0;
            }
        }
        /* Mouse deltas + wheel ride the same frame (scripts read
         * deltas; held-button state comes from the GUI-owned
         * queue — the engine never sees OS buttons here). */
        {
            const ImGuiIO &io = ImGui::GetIO();

            if (io.MouseDelta.x != 0.0f ||
                io.MouseDelta.y != 0.0f) {
                if (le_input_inject_mouse_move(
                        engine, io.MousePos.x, io.MousePos.y,
                        io.MouseDelta.x,
                        io.MouseDelta.y) == LE_SUCCESS) {
                    injected++;
                }
            }
            if (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f) {
                if (le_input_inject_scroll(engine, io.MouseWheelH,
                                           io.MouseWheel) ==
                    LE_SUCCESS) {
                    injected++;
                }
            }
        }
    }
    return injected;
}

int leg_probe_tool_rect(const leg_context *context,
                        const char *tool_id,
                        leg_rect *out_rect) {
    uint32_t k = 0;

    if (out_rect != NULL) {
        memset(out_rect, 0, sizeof(*out_rect));
    }
    if (context == NULL || tool_id == NULL ||
        out_rect == NULL) {
        return 0;
    }
    if (context->ui == NULL) {
        return 0;
    }
    for (k = 0; k < context->ui->tool_count; k++) {
        if (strcmp(context->ui->tools[k].id, tool_id) == 0) {
            out_rect->x = context->ui->tools[k].x;
            out_rect->y = context->ui->tools[k].y;
            out_rect->w = context->ui->tools[k].w;
            out_rect->h = context->ui->tools[k].h;
            out_rect->valid = context->ui->tools[k].valid;
            return 1;
        }
    }
    return 0;
}

int leg_probe_viewport_rect(const leg_context *context,
                            leg_rect *out_rect) {
    if (out_rect != NULL) {
        memset(out_rect, 0, sizeof(*out_rect));
    }
    if (context == NULL || out_rect == NULL) {
        return 0;
    }
    if (context->ui == NULL) {
        return 0;
    }
    out_rect->x = context->ui->vp_x;
    out_rect->y = context->ui->vp_y;
    out_rect->w = context->ui->vp_w;
    out_rect->h = context->ui->vp_h;
    out_rect->valid = context->ui->vp_valid;
    return 1;
}

/* R-012 headed-test hook: expose the recorded viewport panel rect
 * as plain floats (the sky-pixel legs map window-px probe space
 * into target-px sample space through this). */
int leg_viewport_panel_rect(const leg_context *context,
                            float out_xywh[4]) {
    if (out_xywh != NULL) {
        out_xywh[0] = out_xywh[1] = out_xywh[2] =
            out_xywh[3] = 0.0f;
    }
    if (context == NULL || out_xywh == NULL) {
        return 0;
    }
    if (context->ui == NULL || !context->ui->vp_valid) {
        return 0;
    }
    out_xywh[0] = context->ui->vp_x;
    out_xywh[1] = context->ui->vp_y;
    out_xywh[2] = context->ui->vp_w;
    out_xywh[3] = context->ui->vp_h;
    return 1;
}

int leg_probe_asset_row(const leg_context *context,
                        const char *source_path,
                        leg_rect *out_rect) {
    uint32_t k = 0;

    if (out_rect != NULL) {
        memset(out_rect, 0, sizeof(*out_rect));
    }
    if (context == NULL || source_path == NULL ||
        out_rect == NULL) {
        return 0;
    }
    if (context->ui == NULL) {
        return 0;
    }
    for (k = 0; k < context->ui->asset_count; k++) {
        if (strcmp(context->ui->assets[k].path, source_path) ==
            0) {
            out_rect->x = context->ui->assets[k].x;
            out_rect->y = context->ui->assets[k].y;
            out_rect->w = context->ui->assets[k].w;
            out_rect->h = context->ui->assets[k].h;
            out_rect->valid = context->ui->assets[k].valid;
            return 1;
        }
    }
    return 0;
}

int leg_probe_hierarchy_row(const leg_context *context,
                            const le_object *object,
                            leg_rect *out_rect) {
    uint32_t k = 0;

    if (out_rect != NULL) {
        memset(out_rect, 0, sizeof(*out_rect));
    }
    if (context == NULL || object == NULL ||
        out_rect == NULL) {
        return 0;
    }
    if (context->ui == NULL) {
        return 0;
    }
    for (k = 0; k < context->ui->hier_count; k++) {
        if (context->ui->hier[k].index == object->index &&
            context->ui->hier[k].generation ==
                object->generation &&
            context->ui->hier[k].world_tag ==
                object->world_tag) {
            out_rect->x = context->ui->hier[k].x;
            out_rect->y = context->ui->hier[k].y;
            out_rect->w = context->ui->hier[k].w;
            out_rect->h = context->ui->hier[k].h;
            out_rect->valid = context->ui->hier[k].valid;
            return 1;
        }
    }
    return 0;
}

int leg_probe_menu_rect(const leg_context *context,
                        const char *menu_label,
                        leg_rect *out_rect) {
    uint32_t k = 0;

    if (out_rect != NULL) {
        memset(out_rect, 0, sizeof(*out_rect));
    }
    if (context == NULL || menu_label == NULL ||
        out_rect == NULL) {
        return 0;
    }
    if (context->ui == NULL) {
        return 0;
    }
    for (k = 0; k < context->ui->menu_count; k++) {
        if (strcmp(context->ui->menus[k].label, menu_label) ==
            0) {
            out_rect->x = context->ui->menus[k].x;
            out_rect->y = context->ui->menus[k].y;
            out_rect->w = context->ui->menus[k].w;
            out_rect->h = context->ui->menus[k].h;
            out_rect->valid = context->ui->menus[k].valid;
            return 1;
        }
    }
    return 0;
}

int leg_probe_menu_item_rect(const leg_context *context,
                             const char *menu_label,
                             const char *item_label,
                             leg_rect *out_rect) {
    uint32_t k = 0;
    uint32_t n = 0;

    if (out_rect != NULL) {
        memset(out_rect, 0, sizeof(*out_rect));
    }
    if (context == NULL || menu_label == NULL ||
        item_label == NULL || out_rect == NULL) {
        return 0;
    }
    if (context->ui == NULL) {
        return 0;
    }
    /* Newest-first: menu_item_count resets each frame and the
     * table is append-only, so the TAIL holds this frame's
     * geometry. The old head-first scan returned a STALE rect
     * slot (valid flag from an earlier frame) when the menu
     * reopened — the harness clicked a dead rect while the live
     * one sat at the tail (found headed: items_now=1 yet probe
     * read the stale head). */
    n = context->ui->menu_item_count;
    for (k = n; k > 0; k--) {
        uint32_t i = k - 1;

        if (strcmp(context->ui->menu_items[i].menu,
                   menu_label) == 0 &&
            strcmp(context->ui->menu_items[i].item,
                   item_label) == 0) {
            out_rect->x = context->ui->menu_items[i].x;
            out_rect->y = context->ui->menu_items[i].y;
            out_rect->w = context->ui->menu_items[i].w;
            out_rect->h = context->ui->menu_items[i].h;
            out_rect->valid = context->ui->menu_items[i].valid;
            return 1;
        }
    }
    return 0;
}

/* Edit-assist tap (observe-only): last assist evaluation. */
static struct {
    int tapped;
    int popup_was_open;
    int down;
    int dur0;
    int in_rect;
    int seen;
    /* Post-BeginMenu readback: did the popup survive to item
     * submit this frame (menu_item_count for Edit > 0)? */
    int items_now;
} g_edit_assist_tap;

/* Menu-open diagnostic (observe-only): 1 when the named menu's
 * BeginMenu saw an open popup on the last panels frame (the click
 * reached ImGui and it opened), 0 otherwise. Backed by the
 * post-submit survival mark (leg_dbg_edit_assist_items): the mark
 * records the ACTUAL opened return per frame, so a transient open
 * (popup opened then closed before items submitted) still reads 1
 * on the press frame. Falls back to the item snapshot. */
int leg_dbg_menu_open(const leg_context *context,
                      const char *menu_label) {
    uint32_t k = 0;

    if (context == NULL || menu_label == NULL) {
        return 0;
    }
    if (g_edit_assist_tap.seen &&
        strcmp(menu_label, "Edit") == 0 &&
        g_edit_assist_tap.items_now) {
        return 1;
    }
    if (context->ui == NULL) {
        return 0;
    }
    for (k = 0; k < context->ui->menu_item_count; k++) {
        if (strcmp(context->ui->menu_items[k].menu,
                   menu_label) == 0) {
            return 1;
        }
    }
    return 0;
}

void leg_dbg_edit_assist_tap(int tapped, int popup_was_open,
                             int down, int dur0, int in_rect) {
    g_edit_assist_tap.tapped = tapped;
    g_edit_assist_tap.popup_was_open = popup_was_open;
    g_edit_assist_tap.down = down;
    g_edit_assist_tap.dur0 = dur0;
    g_edit_assist_tap.in_rect = in_rect;
    g_edit_assist_tap.seen = 1;
}

/* Post-submit survival mark (called after EndMenu / skipped menu
 * each frame with the attempted menu name). ALSO snapshots the
 * item count so the harness can tell "no items submitted" from
 * "probe missed". */
void leg_dbg_edit_assist_items(const char *menu_label, int opened) {
    (void)menu_label;
    g_edit_assist_tap.items_now = opened;
}

/* Inside-open mark (called first thing inside `if (opened)`):
 * proves the body RAN this frame. The harness compares items_now
 * (BeginMenu returned true) vs the item snapshot count (MenuItems
 * submitted) to isolate "body skipped" from "items rejected". */
void leg_dbg_edit_assist_items_open(const char *menu_label) {
    (void)menu_label;
    g_edit_assist_tap.popup_was_open = 2;
}

int leg_dbg_edit_item_count(const leg_context *context) {
    if (context == NULL || context->ui == NULL) {
        return -1;
    }
    return (int)context->ui->menu_item_count;
}

/* Drag-drop census (observe-only): 1 while an ImGui drag-drop
 * operation is active (source armed, payload in flight), 0
 * otherwise. Lets the headed H-drop leg tell "source never
 * armed" from "target refused". (No public IsDragDropActive in
 * this branch — GetDragDropPayload() non-NULL IS the active
 * signal.) */
int leg_dbg_drag_active(const leg_context *context) {
    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    return (ImGui::GetDragDropPayload() != NULL) ? 1 : 0;
}

/* Drag-drop payload census (observe-only): 1 when the in-flight
 * payload is a LUMA_ASSET payload of the expected byte size, 0
 * otherwise (none/wrong type). Proves the source published the
 * right payload kind while held. */
int leg_dbg_drop_payload(const leg_context *context) {
    const ImGuiPayload *pl = NULL;

    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    pl = ImGui::GetDragDropPayload();
    if (pl == NULL) {
        return 0;
    }
    if (!pl->IsDataType("LUMA_ASSET")) {
        return 0;
    }
    return (pl->DataSize == sizeof(led_drag_payload)) ? 1 : 0;
}

/* Viewport drop-target census (observe-only): 1 when the mouse is
 * over the viewport capture rect AND a drag is in flight (the
 * BeginDragDropTarget condition minus the payload type). Tells
 * "pointer missed the target" from "target refused". */
int leg_dbg_drop_target(const leg_context *context,
                        float *out_mxy) {
    float mx = 0;
    float my = 0;

    if (out_mxy != NULL) {
        out_mxy[0] = out_mxy[1] = 0.0f;
    }
    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    if (context->ui == NULL || !context->ui->vp_valid) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    if (ImGui::GetDragDropPayload() == NULL) {
        return 0;
    }
    {
        /* Sample the LATEST mouse pos from io (the context may be
         * read post-frame; GetMousePos is frame-safe anytime). */
        const ImGuiIO &dio = ImGui::GetIO();

        mx = dio.MousePos.x;
        my = dio.MousePos.y;
        if (out_mxy != NULL) {
            out_mxy[0] = mx;
            out_mxy[1] = my;
        }
        if (mx >= context->ui->vp_x &&
            mx < context->ui->vp_x + context->ui->vp_w &&
            my >= context->ui->vp_y &&
            my < context->ui->vp_y + context->ui->vp_h) {
            return 1;
        }
    }
    return 0;
}

/* Drop-target delivery mark (observe-only): set while the
 * viewport's BeginDragDropTarget branch runs (pointer over the
 * capture with a drag in flight, same frame). The headed H-drop
 * leg reads it to tell "target never opened" from "payload
 * refused". */
static int g_drop_mark = 0;

void leg_dbg_drop_mark(int open) {
    g_drop_mark = open ? 1 : 0;
}

int leg_dbg_drop_mark_read(const leg_context *context) {
    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    return g_drop_mark;
}

/* Drop-delivery tap (observe-only): last script-drop delivery.
 * Fields: got_payload, picked, attached. Tells "payload refused"
 * from "pick missed" from "core refused". */
static struct {
    int got_payload;
    int picked;
    int attached;
    int seen;
} g_drop_tap;

void leg_dbg_drop_tap(int got_payload, int picked,
                      int attached) {
    g_drop_tap.got_payload = got_payload;
    g_drop_tap.picked = picked;
    g_drop_tap.attached = attached;
    g_drop_tap.seen = 1;
}

int leg_dbg_drop_tap_read(const leg_context *context,
                          int *out_flags) {
    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    if (!g_drop_tap.seen) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    if (out_flags != NULL) {
        out_flags[0] = g_drop_tap.got_payload;
        out_flags[1] = g_drop_tap.picked;
        out_flags[2] = g_drop_tap.attached;
    }
    return 1;
}

int leg_dbg_edit_assist(const leg_context *context,
                        int *out_flags) {
    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    if (!g_edit_assist_tap.seen) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    if (out_flags != NULL) {
        out_flags[0] = g_edit_assist_tap.tapped;
        out_flags[1] = g_edit_assist_tap.popup_was_open;
        out_flags[2] = g_edit_assist_tap.down;
        out_flags[3] = g_edit_assist_tap.dur0;
        out_flags[4] = g_edit_assist_tap.in_rect;
        out_flags[5] = g_edit_assist_tap.items_now;
    }
    return 1;
}

/* Click-to-select branch tap (observe-only ring for headed
 * diagnostics): the viewport panel records its branch inputs every
 * frame it evaluates a click (and the pick result when it picks).
 * The harness reads the LAST tap to tell "edge never arrived" from
 * "branch declined" from "picked empty". m = screen mouse,
 * p = panel-local pick point ACTUALLY passed to pick_select,
 * ox/oy = capture origin used, ww/hh = capture size used. */
static struct {
    int hovered;
    int clicked;
    int captured;
    int wants_kb;
    float mx;
    float my;
    float px;
    float py;
    float ox;
    float oy;
    float ww;
    float hh;
    int picked;
    int sel_count;
    int seen;
    char hovwin[64];
} g_pick_tap;

void leg_dbg_pick_tap(int hovered, int clicked, int captured,
                      int wants_kb, float mx, float my, float px,
                      float py, int picked, int sel_count) {
    g_pick_tap.hovered = hovered;
    g_pick_tap.clicked = clicked;
    g_pick_tap.captured = captured;
    g_pick_tap.wants_kb = wants_kb;
    g_pick_tap.mx = mx;
    g_pick_tap.my = my;
    g_pick_tap.px = px;
    g_pick_tap.py = py;
    g_pick_tap.picked = picked;
    g_pick_tap.sel_count = sel_count;
    g_pick_tap.seen = 1;
}

void leg_dbg_pick_origin(float ox, float oy, float ww, float hh) {
    g_pick_tap.ox = ox;
    g_pick_tap.oy = oy;
    g_pick_tap.ww = ww;
    g_pick_tap.hh = hh;
}

int leg_dbg_pick_state(const leg_context *context, float *out_mxy,
                       float *out_pxy, int *out_flags) {
    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    if (!g_pick_tap.seen) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    if (out_mxy != NULL) {
        out_mxy[0] = g_pick_tap.mx;
        out_mxy[1] = g_pick_tap.my;
    }
    if (out_pxy != NULL) {
        out_pxy[0] = g_pick_tap.px;
        out_pxy[1] = g_pick_tap.py;
    }
    if (out_flags != NULL) {
        out_flags[0] = g_pick_tap.hovered;
        out_flags[1] = g_pick_tap.clicked;
        out_flags[2] = g_pick_tap.captured;
        out_flags[3] = g_pick_tap.wants_kb;
        out_flags[4] = g_pick_tap.picked;
        out_flags[5] = g_pick_tap.sel_count;
    }
    return 1;
}

/** Capture origin+size used by the last pick tap (observe-only). */
void leg_dbg_hover_mark(int view, int any) {
    snprintf(g_pick_tap.hovwin, sizeof(g_pick_tap.hovwin),
             "view=%d any=%d", view, any);
}

int leg_dbg_pick_origin(const leg_context *context, float *out_owh) {
    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    if (!g_pick_tap.seen) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    if (out_owh != NULL) {
        out_owh[0] = g_pick_tap.ox;
        out_owh[1] = g_pick_tap.oy;
        out_owh[2] = g_pick_tap.ww;
        out_owh[3] = g_pick_tap.hh;
    }
    return 1;
}

/* Frame-edge diagnostics: raw ImGui mouse state (observe only). */
int leg_dbg_mouse_down(const leg_context *context, int button) {
    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    if (button < 0 || button > 4) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    return ImGui::IsMouseDown(button) ? 1 : 0;
}

int leg_dbg_mouse_clicked(const leg_context *context, int button) {
    if (context == NULL || context->imgui == NULL) {
        return 0;
    }
    if (button < 0 || button > 4) {
        return 0;
    }
    ImGui::SetCurrentContext(context->imgui);
    return ImGui::IsMouseClicked(button) ? 1 : 0;
}

int leg_dbg_hover_window(const leg_context *context, char *out_name,
                         unsigned out_cap) {
    /* Hovered-window census runs INSIDE the panels frame (see
     * leg_dbg_pick_tap: called while a frame is open, so Begin/End
     * pairs are legal there). Post-frame queries cannot open
     * windows; report the last in-frame census instead. */
    if (out_name != NULL && out_cap > 0) {
        strncpy(out_name, g_pick_tap.hovwin,
                out_cap - 1);
        out_name[out_cap - 1] = '\0';
        return (g_pick_tap.hovwin[0] != '\0') ? 1 : 0;
    }
    return 0;
}

/* Projected gizmo handle (mirrors leg_world_to_panel in
 * gui_gizmo_overlay.cpp: selection AABB center + axis *
 * handle_len through the host viewport camera). */
int leg_probe_gizmo_handle(const leg_context *context, int mode,
                           int axis, float out_px[2]) {
    extern led_viewport *leg_viewport_host_viewport(void);

    led_viewport *vp = NULL;
    float center[3];
    float mn[3];
    float mx[3];
    float extent = 1.0f;
    float handle_len = 1.0f;
    float tip[3];
    lr_camera cam;
    float rel[3];
    float view[3];
    float w = 0.0f;
    float clip[2];

    if (out_px != NULL) {
        out_px[0] = 0.0f;
        out_px[1] = 0.0f;
    }
    if (context == NULL || out_px == NULL) {
        return 0;
    }
    if (context->session == NULL) {
        return 0;
    }
    if (mode < LED_GIZMO_TRANSLATE || mode > LED_GIZMO_SCALE) {
        return 0;
    }
    if (axis < 0 || axis > 3) {
        return 0;
    }
    if (mode == LED_GIZMO_ROTATE && axis != 3) {
        return 0; /* rings project differently; center only */
    }
    if (mode != LED_GIZMO_TRANSLATE && mode != LED_GIZMO_SCALE &&
        axis != 3) {
        return 0; /* arrows only, except the shared center */
    }
    vp = leg_viewport_host_viewport();
    if (vp == NULL) {
        return 0;
    }
    if (!led_selection_aabb(context->session, mn, mx)) {
        return 0;
    }
    center[0] = (mn[0] + mx[0]) * 0.5f;
    center[1] = (mn[1] + mx[1]) * 0.5f;
    center[2] = (mn[2] + mx[2]) * 0.5f;
    {
        float e = mx[0] - mn[0];

        if (mx[1] - mn[1] > e) {
            e = mx[1] - mn[1];
        }
        if (mx[2] - mn[2] > e) {
            e = mx[2] - mn[2];
        }
        extent = e * 0.5f + 0.5f;
    }
    handle_len = extent * 1.5f;
    if (handle_len < 0.5f) {
        handle_len = 0.5f;
    }
    /* Axis 3 = center/free cube (all modes share the anchor). */
    tip[0] = center[0] + ((axis == 0) ? handle_len : 0.0f);
    tip[1] = center[1] + ((axis == 1) ? handle_len : 0.0f);
    tip[2] = center[2] + ((axis == 2) ? handle_len : 0.0f);
    /* Viewport origin on screen: the probe snapshot holds the
     * capture rect (client px). */
    {
        float ox;
        float oy;
        float ww;
        float wh;

        if (context->ui == NULL || !context->ui->vp_valid) {
            return 0;
        }
        ox = context->ui->vp_x;
        oy = context->ui->vp_y;
        ww = context->ui->vp_w;
        wh = context->ui->vp_h;
        if (ww <= 0 || wh <= 0) {
            return 0;
        }
        memset(&cam, 0, sizeof(cam));
        if (!led_viewport_camera(vp, &cam)) {
            return 0;
        }
        rel[0] = tip[0] - cam.position[0];
        rel[1] = tip[1] - cam.position[1];
        rel[2] = tip[2] - cam.position[2];
        view[0] = cam.view[0] * rel[0] + cam.view[4] * rel[1] +
                  cam.view[8] * rel[2];
        view[1] = cam.view[1] * rel[0] + cam.view[5] * rel[1] +
                  cam.view[9] * rel[2];
        view[2] = cam.view[2] * rel[0] + cam.view[6] * rel[1] +
                  cam.view[10] * rel[2];
        w = -view[2];
        if (!(w > 1e-9f)) {
            return 0;
        }
        clip[0] = cam.projection[0] * view[0] +
                  cam.projection[4] * view[1] +
                  cam.projection[8] * view[2] +
                  cam.projection[12] * w;
        clip[1] = cam.projection[1] * view[0] +
                  cam.projection[5] * view[1] +
                  cam.projection[9] * view[2] +
                  cam.projection[13] * w;
        out_px[0] = ox + (clip[0] / w * 0.5f + 0.5f) * ww;
        out_px[1] = oy + (0.5f - clip[1] / w * 0.5f) * wh;
        (void)extent;
        (void)handle_len;
        (void)center;
        return 1;
    }
}
