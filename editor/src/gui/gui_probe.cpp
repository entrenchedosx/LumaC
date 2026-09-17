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
    /* The ##vp-capture InvisibleButton is the last item. */
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

int leg_consume_play_input(leg_context *context,
                           le_engine *engine) {
    int injected = 0;

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

        for (k = 0;
             k < sizeof(keys) / sizeof(keys[0]); k++) {
            if (ImGui::IsKeyDown(keys[k].imgui)) {
                if (le_input_inject_key(engine, keys[k].engine,
                                        1) == LE_SUCCESS) {
                    injected++;
                }
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
    if (axis < 0 || axis > 2) {
        return 0;
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
