/* Phase 33 GUI panels (isolated C++ over led_* commands).
 *
 * Dear ImGui panels driving EditorCore ONLY through led_* APIs:
 * menu/toolbar, hierarchy (select/reparent), inspector (reflected
 * widgets), assets (browse/drag), console, status bar. Every mutation
 * funnels GUI intent -> led_write_property / led_execute -> history.
 * No direct component-memory writes; no long-lived borrowed engine
 * pointers (IDs/handles/owned strings only).
 *
 * This TU owns the per-context UI state machine (panel visibility,
 * inspector edit buffers, asset search text, drag staging, gizmo
 * mode/space, play pause). Rendering itself stays in gui_draw.cpp;
 * the viewport scene render stays engine-side (le_world_render_*).
 */

#include <cctype>
#include <cstdio>
#include <cstring>
#include <new>

#include "gui_internal.h"

/* One UI per context (single-context apps in Phase 33; owned by the
 * leg_context, never global). */
static leg_ui *leg_ui_for(leg_context *ctx) {
    return (ctx != NULL) ? ctx->ui : NULL;
}

/* Ensure the UI exists (idempotent; called at frame begin). */
static leg_ui *leg_ui_ensure(leg_context *ctx) {
    if (ctx == NULL) {
        return NULL;
    }
    if (ctx->ui == NULL) {
        ctx->ui = new (std::nothrow) leg_ui;
        if (ctx->ui == NULL) {
            return NULL;
        }
        memset(ctx->ui, 0, sizeof(*ctx->ui));
        ctx->ui->show_hierarchy = 1;
        ctx->ui->show_inspector = 1;
        ctx->ui->show_assets = 1;
        ctx->ui->show_console = 1;
        ctx->ui->show_viewport = 1;
        ctx->ui->show_toolbar = 1;
        ctx->ui->show_status = 1;
        ctx->ui->asset_type_filter = -1;
        ctx->ui->gizmo_mode = LED_GIZMO_TRANSLATE;
        ctx->ui->camera_speed = 100;
        ctx->ui->console_autoscroll = 1;
        ctx->ui->staged_row = -1;
        ctx->ui->create_parent_to_selection = 1;
    }
    return ctx->ui;
}

/* Free the UI (called from leg_context_destroy). */
void leg_ui_teardown(leg_context *ctx) {
    if (ctx == NULL || ctx->ui == NULL) {
        return;
    }
    delete ctx->ui;
    ctx->ui = NULL;
}

/* ------------------------------------------------------------------ */
/* Status helper (owned strings only; console mirror for errors).     */
/* ------------------------------------------------------------------ */

void leg_status(leg_context *ctx, const char *text, int error) {
    leg_ui *ui = leg_ui_for(ctx);

    if (ui == NULL || text == NULL) {
        return;
    }
    strncpy(ui->status_text, text, sizeof(ui->status_text) - 1);
    ui->status_text[sizeof(ui->status_text) - 1] = '\0';
    ui->status_is_error = error ? 1 : 0;
    if (error && ctx->session != NULL) {
        led_console_push(ctx->session, LED_LOG_ERROR, "gui", text);
    }
}

/* ------------------------------------------------------------------ */
/* Command funnel (every structural mutation flows through here).     */
/* Value writes go through leg_write_property in the widgets TU.      */
/* ------------------------------------------------------------------ */

static int leg_run(leg_context *ctx, const led_command *cmd,
                   const char *ok_text) {
    led_result rc = LED_ERROR_INVALID_ARGUMENT;

    if (ctx == NULL || cmd == NULL) {
        return 0;
    }
    if (ctx->session == NULL) {
        leg_status(ctx, "No session attached", 1);
        return 0;
    }
    rc = led_execute(ctx->session, cmd);
    if (rc != LED_SUCCESS) {
        char buf[256];

        snprintf(buf, sizeof(buf), "Command '%s' failed (%d)",
                 cmd->label[0] != '\0' ? cmd->label : "?",
                 (int)rc);
        leg_status(ctx, buf, 1);
        return 0;
    }
    if (ok_text != NULL) {
        leg_status(ctx, ok_text, 0);
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Menu bar (File/Edit/View/Project).                                 */
/* ------------------------------------------------------------------ */

static void leg_menu_file(leg_context *ctx, leg_ui *ui) {
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New scene")) {
            if (led_scene_new(ctx->session) == LED_SUCCESS) {
                leg_status(ctx, "New scene", 0);
            } else {
                leg_status(ctx, "New scene failed (playing?)", 1);
            }
        }
        if (ImGui::MenuItem("Open scene...")) {
            ui->scene_path_stage[0] = '\0';
            ui->show_open_scene_popup = 1;
            ImGui::OpenPopup("Open scene");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save scene", "Ctrl+S",
                            false, !led_is_playing(ctx->session))) {
            led_result rc = led_scene_save(ctx->session);

            if (rc == LED_SUCCESS) {
                leg_status(ctx, "Scene saved", 0);
            } else {
                leg_status(ctx, "Save failed (no path? playing?)",
                           1);
            }
        }
        if (ImGui::MenuItem("Save scene as...")) {
            const char *cur = led_scene_get_path(ctx->session);

            if (cur != NULL) {
                strncpy(ui->scene_path_stage, cur,
                        sizeof(ui->scene_path_stage) - 1);
                ui->scene_path_stage[sizeof(ui->scene_path_stage) -
                                     1] = '\0';
            } else {
                ui->scene_path_stage[0] = '\0';
            }
            ui->show_save_as_popup = 1;
            ImGui::OpenPopup("Save scene as");
        }
        if (ImGui::MenuItem("Revert scene", NULL, false,
                            !led_is_playing(ctx->session))) {
            if (led_scene_revert(ctx->session) == LED_SUCCESS) {
                leg_status(ctx, "Scene reverted", 0);
            } else {
                leg_status(ctx, "Revert failed (no path?)", 1);
            }
        }
        ImGui::EndMenu();
    }
    /* Scene-path popups (modal text fields; the session remembers
     * the path as given — no stored absolute paths). */
    if (ImGui::BeginPopupModal("Open scene", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Scene path", ui->scene_path_stage,
                         sizeof(ui->scene_path_stage));
        ImGui::TextDisabled("Project-relative (.luma_scene)");
        if (ImGui::Button("Open")) {
            if (led_scene_open(ctx->session,
                               ui->scene_path_stage) ==
                LED_SUCCESS) {
                leg_status(ctx, "Scene opened", 0);
            } else {
                leg_status(ctx, "Scene open failed", 1);
            }
            ImGui::CloseCurrentPopup();
            ui->show_open_scene_popup = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
            ui->show_open_scene_popup = 0;
        }
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Save scene as", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Scene path", ui->scene_path_stage,
                         sizeof(ui->scene_path_stage));
        ImGui::TextDisabled("Project-relative (.luma_scene)");
        if (ImGui::Button("Save")) {
            if (led_scene_save_as(ctx->session,
                                  ui->scene_path_stage) ==
                LED_SUCCESS) {
                leg_status(ctx, "Scene saved", 0);
            } else {
                leg_status(ctx, "Save failed", 1);
            }
            ImGui::CloseCurrentPopup();
            ui->show_save_as_popup = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
            ui->show_save_as_popup = 0;
        }
        ImGui::EndPopup();
    }
}

static void leg_menu_edit(leg_context *ctx, leg_ui *ui) {
    (void)ui;
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
            led_undo(ctx->session);
        }
        if (ImGui::MenuItem("Redo", "Ctrl+Y")) {
            led_redo(ctx->session);
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) {
            led_dispatch_action(ctx->session, LED_ACTION_DUPLICATE,
                                NULL);
        }
        if (ImGui::MenuItem("Delete", "Del")) {
            led_dispatch_action(ctx->session, LED_ACTION_DELETE,
                                NULL);
        }
        ImGui::EndMenu();
    }
}

static void leg_menu_view(leg_context *ctx, leg_ui *ui) {
    bool show[7];

    (void)ctx;
    show[0] = ui->show_hierarchy != 0;
    show[1] = ui->show_inspector != 0;
    show[2] = ui->show_assets != 0;
    show[3] = ui->show_console != 0;
    show[4] = ui->show_viewport != 0;
    show[5] = ui->show_toolbar != 0;
    show[6] = ui->show_status != 0;
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Hierarchy", NULL, &show[0]);
        ImGui::MenuItem("Inspector", NULL, &show[1]);
        ImGui::MenuItem("Assets", NULL, &show[2]);
        ImGui::MenuItem("Console", NULL, &show[3]);
        ImGui::MenuItem("Viewport", NULL, &show[4]);
        ImGui::MenuItem("Toolbar", NULL, &show[5]);
        ImGui::MenuItem("Status bar", NULL, &show[6]);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset layout")) {
            leg_layout_request_reset(ctx);
            leg_status(ctx, "Layout reset", 0);
        }
        ImGui::EndMenu();
    }
    ui->show_hierarchy = show[0] ? 1 : 0;
    ui->show_inspector = show[1] ? 1 : 0;
    ui->show_assets = show[2] ? 1 : 0;
    ui->show_console = show[3] ? 1 : 0;
    ui->show_viewport = show[4] ? 1 : 0;
    ui->show_toolbar = show[5] ? 1 : 0;
    ui->show_status = show[6] ? 1 : 0;
}

static void leg_menu_help(leg_context *ctx, leg_ui *ui) {
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("About Luma")) {
            ui->show_about = 1;
            ImGui::OpenPopup("About Luma");
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginPopupModal("About Luma", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        leg_roles roles;

        leg_theme_roles(&roles);
        ImGui::TextColored(
            ImVec4(roles.accent[0], roles.accent[1], roles.accent[2],
                   1.0f),
            "◆ Luma Editor");
        ImGui::Separator();
        ImGui::TextDisabled("Phase 33V — independent verification "
                            "and visual rebuild.");
        ImGui::Text("Project → scene → select → edit → gizmo → "
                    "prefab → undo → save → play → stop.");
        ImGui::Separator();
        ImGui::TextDisabled("Shortcuts: W/E/R gizmo · F focus · "
                            "F5 play · Shift+F5 stop · F10 step · "
                            "Ctrl+Z/Y/D/S undo/redo/dup/save");
        if (ImGui::Button("Close")) {
            ImGui::CloseCurrentPopup();
            ui->show_about = 0;
        }
        ImGui::EndPopup();
    }
    (void)ctx;
}

static void leg_menu_project(leg_context *ctx, leg_ui *ui) {
    (void)ui;
    if (ImGui::BeginMenu("Project")) {
        if (ImGui::MenuItem("Scan")) {
            led_scan_stats st;

            memset(&st, 0, sizeof(st));
            if (led_project_scan(ctx->session, &st) ==
                LED_SUCCESS) {
                char buf[128];

                snprintf(buf, sizeof(buf),
                         "Scan: +%u -%u stale=%u missing=%u", st.added,
                         st.removed, st.stale_marked,
                         st.missing_marked);
                leg_status(ctx, buf, 0);
            } else {
                leg_status(ctx, "Scan failed (no project?)", 1);
            }
        }
        if (ImGui::MenuItem("Reimport all")) {
            uint32_t ok = 0;

            if (led_project_reimport_all(ctx->session, &ok) ==
                LED_SUCCESS) {
                char buf[64];

                snprintf(buf, sizeof(buf), "Reimported %u", ok);
                leg_status(ctx, buf, 0);
            } else {
                leg_status(ctx, "Reimport-all failed", 1);
            }
        }
        ImGui::EndMenu();
    }
}

/* ------------------------------------------------------------------ */
/* Toolbar (create/select/play + gizmo mode/space + camera speed).    */
/* ------------------------------------------------------------------ */

static void leg_toolbar(leg_context *ctx, leg_ui *ui) {
    int playing = led_is_playing(ctx->session);
    int paused = led_play_is_paused(ctx->session);
    const float tbs = 28.0f; /* tool button square (design system) */
    leg_roles roles;

    leg_theme_roles(&roles);
    /* Probe snapshot resets each frame (stale rects never
     * survive; valid==0 until the widget draws). */
    ui->tool_count = 0;
    ui->asset_count = 0;
    ui->hier_count = 0;
    ui->vp_valid = 0;
    /* Authoring group. */
    if (leg_tool_button("##tb-create", LEG_ICON_PLUS,
                        "Create object", 0, tbs)) {
        ui->create_name[0] = '\0';
        ui->show_create_popup = 1;
        ImGui::OpenPopup("Create object");
    }
    leg_probe_record_tool(ui, "##tb-create");
    ImGui::SameLine();
    if (leg_tool_button("##tb-prefab", LEG_ICON_PREFAB,
                        "Instantiate prefab", 0, tbs)) {
        ui->prefab_path[0] = '\0';
        ui->show_prefab_popup = 1;
        ImGui::OpenPopup("Instantiate prefab");
    }
    leg_probe_record_tool(ui, "##tb-prefab");
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    /* Play group (icon transport). */
    ImGui::BeginDisabled(playing);
    if (leg_tool_button("##tb-play", LEG_ICON_PLAY,
                        playing ? "Playing" : "Play (F5)", 0,
                        tbs)) {
        if (led_play_enter(ctx->session) == LED_SUCCESS) {
            leg_status(ctx, "Playing (edit locked)", 0);
        } else {
            leg_status(ctx, "Play failed", 1);
        }
    }
    leg_probe_record_tool(ui, "##tb-play");
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!playing);
    if (leg_tool_button("##tb-pause",
                        paused ? LEG_ICON_PLAY : LEG_ICON_PAUSE,
                        paused ? "Resume" : "Pause", paused, tbs)) {
        led_play_set_paused(ctx->session, !paused);
    }
    leg_probe_record_tool(ui, "##tb-pause");
    ImGui::SameLine();
    if (leg_tool_button("##tb-step", LEG_ICON_STEP,
                        "Step one tick (F10)", 0, tbs)) {
        led_play_step(ctx->session);
    }
    leg_probe_record_tool(ui, "##tb-step");
    ImGui::SameLine();
    if (leg_tool_button("##tb-stop", LEG_ICON_STOP,
                        playing ? "Stop (Shift+F5)"
                                : "Stop (not playing)",
                        0, tbs)) {
        if (led_play_exit(ctx->session) == LED_SUCCESS) {
            leg_status(ctx, "Stopped (edit unchanged)", 0);
        } else {
            leg_status(ctx, "Stop failed", 1);
        }
    }
    leg_probe_record_tool(ui, "##tb-stop");
    ImGui::EndDisabled();
    if (playing) {
        /* Play pill: unmistakable but quiet (design system). */
        ImGui::SameLine();
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            ImVec4(roles.play[0], roles.play[1], roles.play[2],
                   0.25f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImVec4(roles.play[0], roles.play[1], roles.play[2],
                   0.25f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonActive,
            ImVec4(roles.play[0], roles.play[1], roles.play[2],
                   0.25f));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(roles.play[0], roles.play[1],
                                     roles.play[2], 1.0f));
        ImGui::Button(paused ? "PAUSED" : "PLAYING");
        ImGui::PopStyleColor(4);
    }
    /* Gizmo mode group (icon radio + W/E/R hotkeys). */
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    if (leg_tool_button(
            "##tb-translate", LEG_ICON_TRANSLATE,
            "Translate (W)",
            ui->gizmo_mode == LED_GIZMO_TRANSLATE, tbs)) {
        ui->gizmo_mode = LED_GIZMO_TRANSLATE;
    }
    leg_probe_record_tool(ui, "##tb-translate");
    ImGui::SameLine();
    if (leg_tool_button("##tb-rotate", LEG_ICON_ROTATE,
                        "Rotate (E)",
                        ui->gizmo_mode == LED_GIZMO_ROTATE, tbs)) {
        ui->gizmo_mode = LED_GIZMO_ROTATE;
    }
    leg_probe_record_tool(ui, "##tb-rotate");
    ImGui::SameLine();
    if (leg_tool_button("##tb-scale", LEG_ICON_SCALE,
                        "Scale (R)",
                        ui->gizmo_mode == LED_GIZMO_SCALE, tbs)) {
        ui->gizmo_mode = LED_GIZMO_SCALE;
    }
    leg_probe_record_tool(ui, "##tb-scale");
    /* Snap + space + camera speed (compact; tooltips carry docs). */
    ImGui::SameLine();
    ImGui::Separator();
    ImGui::SameLine();
    {
        int s = ui->gizmo_space;

        ImGui::SetNextItemWidth(76);
        if (ImGui::Combo("##tb-space", &s, "World\0Local\0")) {
            ui->gizmo_space = (s != 0) ? 1 : 0;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Gizmo space (display only)");
        }
    }
    ImGui::SameLine();
    {
        bool snap = ui->gizmo_snap_on != 0;

        ImGui::Checkbox("Snap", &snap);
        ui->gizmo_snap_on = snap ? 1 : 0;
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Snap gizmo drags to Step");
        }
    }
    if (ui->gizmo_snap_on) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64);
        ImGui::DragFloat("##tb-step", &ui->gizmo_snap_step, 0.05f,
                         0.001f, 10.0f, "%.3f");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Snap step");
        }
        if (ui->gizmo_snap_step <= 0.0f) {
            ui->gizmo_snap_step = 0.1f;
        }
    }
    ImGui::SameLine();
    {
        int sp = ui->camera_speed;

        ImGui::SetNextItemWidth(90);
        if (ImGui::SliderInt("##tb-cam", &sp, 10, 1000, "%d%%")) {
            ui->camera_speed = sp;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Viewport fly speed");
        }
    }
    /* Dirty dot (authored-change indicator, right side). */
    if (led_is_dirty(ctx->session)) {
        ImGui::SameLine();
        ImGui::TextDisabled("●");
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Unsaved changes (Ctrl+S)");
        }
    }
}

/* Create-object modal (name + parent-to-selection option). */
static void leg_create_popup(leg_context *ctx, leg_ui *ui) {
    if (ImGui::BeginPopupModal("Create object", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", ui->create_name,
                         sizeof(ui->create_name));
        {
            bool parent_to = ui->create_parent_to_selection != 0;

            ImGui::Checkbox("Parent to selection", &parent_to);
            ui->create_parent_to_selection = parent_to ? 1 : 0;
        }
        if (ImGui::Button("Create")) {
            led_command cmd;

            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = LED_CMD_CREATE;
            snprintf(cmd.label, sizeof(cmd.label), "Create");
            if (ui->create_name[0] != '\0') {
                strncpy(cmd.name_value, ui->create_name,
                        sizeof(cmd.name_value) - 1);
            }
            if (ui->create_parent_to_selection) {
                le_object sel = LE_OBJECT_INVALID;

                if (led_selection_get(ctx->session, &sel, 1) == 1) {
                    cmd.parent = sel;
                    cmd.has_parent = 1;
                }
            }
            if (leg_run(ctx, &cmd, "Object created")) {
                /* Select the newborn (tail census, like drops). */
                le_world *w =
                    led_session_get_edit_world(ctx->session);

                if (w != NULL) {
                    uint32_t live = le_world_get_object_count(w);

                    if (live > 0) {
                        le_object *all = new (std::nothrow)
                            le_object[live];

                        if (all != NULL) {
                            uint32_t got =
                                le_world_get_all_objects(w, all,
                                                         live);

                            if (got > 0) {
                                led_selection_set(ctx->session,
                                                  &all[got - 1], 1);
                            }
                            delete[] all;
                        }
                    }
                }
            }
            ImGui::CloseCurrentPopup();
            ui->show_create_popup = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
            ui->show_create_popup = 0;
        }
        ImGui::EndPopup();
    }
}

/* Prefab-instantiate helper (path -> load -> command). */
static void leg_prefab_popup(leg_context *ctx, leg_ui *ui) {
    if (ImGui::BeginPopupModal("Instantiate prefab", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Prefab path", ui->prefab_path,
                         sizeof(ui->prefab_path));
        ImGui::TextDisabled("Project-relative (.luprefab)");
        if (ImGui::Button("Instantiate")) {
            le_asset prefab = LE_ASSET_INVALID;

            if (led_prefab_load(ctx->session, ui->prefab_path,
                                &prefab) == LED_SUCCESS) {
                led_command cmd;

                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = LED_CMD_INSTANTIATE_PREFAB;
                snprintf(cmd.label, sizeof(cmd.label),
                         "Instantiate prefab");
                cmd.prefab.prefab_asset = prefab;
                leg_run(ctx, &cmd, "Prefab instantiated");
            } else {
                leg_status(ctx, "Prefab load failed", 1);
            }
            ImGui::CloseCurrentPopup();
            ui->show_prefab_popup = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
            ui->show_prefab_popup = 0;
        }
        ImGui::EndPopup();
    }
}

/* ------------------------------------------------------------------ */
/* Hierarchy (select/expand/context/reparent).                        */
/* ------------------------------------------------------------------ */

static leg_icon_kind leg_hierarchy_icon(leg_context *ctx,
                                         const le_object *handle) {
    le_world *w = NULL;

    if (ctx == NULL || handle == NULL) {
        return LEG_ICON_OBJECT;
    }
    w = led_session_get_edit_world(ctx->session);
    if (w == NULL) {
        return LEG_ICON_OBJECT;
    }
    if (le_object_has_component(w, handle, LE_COMPONENT_CAMERA)) {
        return LEG_ICON_CAMERA;
    }
    if (le_object_has_component(w, handle, LE_COMPONENT_LIGHT)) {
        return LEG_ICON_LIGHT;
    }
    if (le_object_has_component(w, handle, LE_COMPONENT_SCRIPT)) {
        return LEG_ICON_SCRIPT;
    }
    if (le_object_has_component(w, handle,
                                LE_COMPONENT_RENDERABLE)) {
        return LEG_ICON_BOX;
    }
    return LEG_ICON_OBJECT;
}

static void leg_hierarchy_row(leg_context *ctx, leg_ui *ui,
                              const led_hierarchy_node *node) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_SpanAvailWidth;
    int selected = 0;
    char label[160];

    (void)ui;
    if (node == NULL) {
        return;
    }
    if (!node->has_children) {
        flags |= ImGuiTreeNodeFlags_Leaf |
                 ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    selected = led_selection_contains(ctx->session, &node->handle);
    if (selected) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }
    {
        const char *nm =
            (node->name != NULL && node->name[0] != '\0')
                ? node->name
                : "(unnamed)";

        snprintf(label, sizeof(label), "%s", nm);
    }
    {
        char id[64];
        /* TreeNodeEx(label, flags): no ID stack entry. The label
         * doubles as the ID — duplicate sibling names collide in
         * the ID stack (ImGui asserts in debug). Push the handle
         * first so every row has a unique ID. */
        snprintf(id, sizeof(id), "##h%u:%u:%u", node->handle.index,
                 node->handle.generation, node->handle.world_tag);
        ImGui::PushID(id);
        {
            /* Type icon ahead of the tree row (same line; the tree
             * node keeps its own arrow + selection highlight). */
            leg_icon_draw(leg_hierarchy_icon(ctx, &node->handle),
                          14.0f,
                          ImGui::GetColorU32(ImGuiCol_Text));
            ImGui::SameLine(0, 4);
            bool open = ImGui::TreeNodeEx(label, flags);
            /* Probe: hierarchy-row rect for automation (the tree
             * node is the last item). */
            leg_probe_record_hier(ui, &node->handle);
            /* Click-to-select (replaces; Ctrl toggles; Shift adds). */
            if (ImGui::IsItemClicked(0)) {
                const ImGuiIO &io = ImGui::GetIO();

                if (io.KeyCtrl) {
                    led_selection_toggle(ctx->session,
                                         &node->handle);
                } else if (io.KeyShift) {
                    led_selection_add(ctx->session, &node->handle);
                } else {
                    led_selection_set(ctx->session, &node->handle,
                                      1);
                }
            }
            /* Drops onto a row: reparent the dragged object under
             * this row's object (keep-world; structural command). */
            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload *pl =
                    ImGui::AcceptDragDropPayload("LUMA_OBJECT");

                if (pl != NULL &&
                    pl->DataSize == sizeof(le_object)) {
                    const le_object *dp =
                        (const le_object *)pl->Data;
                    led_command cmd;

                    memset(&cmd, 0, sizeof(cmd));
                    cmd.kind = LED_CMD_REPARENT;
                    snprintf(cmd.label, sizeof(cmd.label),
                             "Reparent");
                    cmd.target = *dp;
                    cmd.parent = node->handle;
                    cmd.has_parent = 1;
                    cmd.reparent_mode = LED_REPARENT_KEEP_WORLD;
                    leg_run(ctx, &cmd, "Reparented");
                }
                ImGui::EndDragDropTarget();
            }
            /* Drag source: hierarchy rows drag as LUMA_OBJECT. */
            if (ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("LUMA_OBJECT",
                                          &node->handle,
                                          sizeof(node->handle));
                ImGui::Text("%s", label);
                ImGui::EndDragDropSource();
            }
            /* Context menu: rename/subtree/duplicate/delete/
             * unparent/reparent-to-selection. */
            if (ImGui::BeginPopupContextItem("##hctx")) {
                if (ImGui::MenuItem("Rename")) {
                    const char *nm = (node->name != NULL)
                                         ? node->name
                                         : "";

                    strncpy(ui->rename_stage, nm,
                            sizeof(ui->rename_stage) - 1);
                    ui->rename_stage[sizeof(ui->rename_stage) - 1] =
                        '\0';
                    ui->rename_target = node->handle;
                    ui->has_rename_target = 1;
                    ui->show_rename_popup = 1;
                    ImGui::OpenPopup("Rename object");
                }
                if (ImGui::MenuItem("Select subtree")) {
                    led_selection_select_subtree(ctx->session,
                                                 &node->handle);
                }
                if (ImGui::MenuItem("Duplicate")) {
                    led_dispatch_action(
                        ctx->session, LED_ACTION_DUPLICATE, NULL);
                }
                if (ImGui::MenuItem("Delete")) {
                    led_command cmd;

                    memset(&cmd, 0, sizeof(cmd));
                    cmd.kind = LED_CMD_DELETE;
                    snprintf(cmd.label, sizeof(cmd.label),
                             "Delete");
                    cmd.target = node->handle;
                    leg_run(ctx, &cmd, "Deleted");
                }
                if (ImGui::MenuItem("Unparent")) {
                    led_command cmd;

                    memset(&cmd, 0, sizeof(cmd));
                    cmd.kind = LED_CMD_SET_PARENT;
                    snprintf(cmd.label, sizeof(cmd.label),
                             "Unparent");
                    cmd.target = node->handle;
                    cmd.has_parent = 0;
                    leg_run(ctx, &cmd, "Unparented");
                }
                if (ImGui::MenuItem("Reparent to selection")) {
                    le_object sel = LE_OBJECT_INVALID;

                    if (led_selection_get(ctx->session, &sel, 1) ==
                            1 &&
                        (sel.index != node->handle.index ||
                         sel.generation !=
                             node->handle.generation ||
                         sel.world_tag != node->handle.world_tag)) {
                        led_command cmd;

                        memset(&cmd, 0, sizeof(cmd));
                        cmd.kind = LED_CMD_REPARENT;
                        snprintf(cmd.label, sizeof(cmd.label),
                                 "Reparent");
                        cmd.target = node->handle;
                        cmd.parent = sel;
                        cmd.has_parent = 1;
                        cmd.reparent_mode = LED_REPARENT_KEEP_WORLD;
                        leg_run(ctx, &cmd, "Reparented");
                    } else {
                        leg_status(ctx, "Reparent needs a selection",
                                   1);
                    }
                }
                ImGui::EndPopup();
            }
            if (open &&
                (flags & ImGuiTreeNodeFlags_NoTreePushOnOpen) ==
                    0) {
                ImGui::TreePop();
            }
        }
        ImGui::PopID();
    }
}

static void leg_panel_hierarchy(leg_context *ctx, leg_ui *ui) {
    uint32_t n = 0;
    const led_hierarchy_node *nodes = NULL;
    uint32_t i = 0;
    bool open = false;

    if (!ui->show_hierarchy) {
        return;
    }
    open = ui->show_hierarchy != 0;
    if (!ImGui::Begin("Hierarchy", &open)) {
        ImGui::End();
        ui->show_hierarchy = open ? 1 : 0;
        return;
    }
    ui->show_hierarchy = open ? 1 : 0;
    n = led_hierarchy_refresh(ctx->session);
    nodes = led_hierarchy_nodes(ctx->session);
    if (n == 0 || nodes == NULL) {
        ImGui::TextDisabled("Empty scene — nothing to show.");
        if (ImGui::Button("Create object")) {
            ui->create_name[0] = '\0';
            ui->show_create_popup = 1;
            ImGui::OpenPopup("Create object");
        }
        ImGui::End();
        return;
    }
    /* Indentation follows snapshot depth (DFS order, absolute
     * depths; pop/push handled per-row delta). */
    {
        uint32_t depth = 0;

        for (i = 0; i < n; i++) {
            while (depth < nodes[i].depth) {
                ImGui::Indent();
                depth++;
            }
            while (depth > nodes[i].depth) {
                ImGui::Unindent();
                depth--;
            }
            leg_hierarchy_row(ctx, ui, &nodes[i]);
        }
        while (depth > 0) {
            ImGui::Unindent();
            depth--;
        }
    }
    /* Rename popup (undoable SET_NAME on the stored target). */
    if (ImGui::BeginPopupModal("Rename object", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::InputText("Name", ui->rename_stage,
                         sizeof(ui->rename_stage));
        if (ImGui::Button("Rename")) {
            if (ui->has_rename_target) {
                led_command cmd;

                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = LED_CMD_SET_NAME;
                snprintf(cmd.label, sizeof(cmd.label), "Rename");
                cmd.target = ui->rename_target;
                strncpy(cmd.name_value, ui->rename_stage,
                        sizeof(cmd.name_value) - 1);
                leg_run(ctx, &cmd, "Renamed");
                ui->has_rename_target = 0;
            }
            ImGui::CloseCurrentPopup();
            ui->show_rename_popup = 0;
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
            ui->show_rename_popup = 0;
            ui->has_rename_target = 0;
        }
        ImGui::EndPopup();
    }
    ImGui::End();
}

/* ------------------------------------------------------------------ */
/* Inspector (typed rows + add/remove component).                     */
/* ------------------------------------------------------------------ */

static void leg_component_add_menu(leg_context *ctx, leg_ui *ui,
                                   const le_object *obj) {
    (void)ui;
    /* The inspector has no menu bar, so BeginMenu can never open
     * here (Phase 33 shipped exactly that dead menu — the add path
     * was unreachable). A button + popup carries the same items and
     * actually opens. */
    if (ImGui::Button("Add component...")) {
        ImGui::OpenPopup("Add component");
    }
    if (!ImGui::BeginPopup("Add component")) {
        return;
    }
    {
        struct {
            const char *label;
            le_component_type type;
        } items[] = {
            { "Camera", LE_COMPONENT_CAMERA },
            { "Light", LE_COMPONENT_LIGHT },
            { "Rigid body", LE_COMPONENT_RIGID_BODY },
            { "Collider", LE_COMPONENT_COLLIDER },
            { "Animator", LE_COMPONENT_ANIMATOR },
            { "Character", LE_COMPONENT_CHARACTER_CONTROLLER },
        };
        size_t k = 0;

        for (k = 0; k < sizeof(items) / sizeof(items[0]); k++) {
            char name[64];

            snprintf(name, sizeof(name), "Add %s",
                     items[k].label);
            if (ImGui::MenuItem(name)) {
                led_command cmd;

                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = LED_CMD_ADD_COMPONENT;
                snprintf(cmd.label, sizeof(cmd.label), "Add %s",
                         items[k].label);
                cmd.target = *obj;
                cmd.component = items[k].type;
                /* Default desc bytes per component (the command
                 * validates comp_size == sizeof(desc); zeroed descs
                 * are NOT valid lenses/frames so fill minimums). */
                switch (items[k].type) {
                case LE_COMPONENT_CAMERA: {
                    le_camera_desc d;

                    le_camera_desc_default(&d);
                    memcpy(cmd.comp_bytes, &d, sizeof(d));
                    cmd.comp_size = (uint32_t)sizeof(d);
                    break;
                }
                case LE_COMPONENT_LIGHT: {
                    le_light_desc d;

                    memset(&d, 0, sizeof(d));
                    d.type = LE_LIGHT_DIRECTIONAL;
                    d.color[0] = d.color[1] = d.color[2] = 1.0f;
                    d.intensity = 3.0f;
                    memcpy(cmd.comp_bytes, &d, sizeof(d));
                    cmd.comp_size = (uint32_t)sizeof(d);
                    break;
                }
                case LE_COMPONENT_COLLIDER: {
                    le_collider_desc d;

                    memset(&d, 0, sizeof(d));
                    d.shape = LE_COLLIDER_BOX;
                    d.half_extents[0] = d.half_extents[1] =
                        d.half_extents[2] = 0.5f;
                    memcpy(cmd.comp_bytes, &d, sizeof(d));
                    cmd.comp_size = (uint32_t)sizeof(d);
                    break;
                }
                case LE_COMPONENT_RIGID_BODY: {
                    le_rigid_body_desc d;

                    memset(&d, 0, sizeof(d));
                    memcpy(cmd.comp_bytes, &d, sizeof(d));
                    cmd.comp_size = (uint32_t)sizeof(d);
                    break;
                }
                case LE_COMPONENT_ANIMATOR: {
                    le_animator_desc d;

                    memset(&d, 0, sizeof(d));
                    /* loop_mode zero == ONCE; speed 0 pauses: pick
                     * a live default (loop, full speed). */
                    d.loop_mode = LE_ANIM_LOOP;
                    d.speed = 1.0f;
                    memcpy(cmd.comp_bytes, &d, sizeof(d));
                    cmd.comp_size = (uint32_t)sizeof(d);
                    break;
                }
                case LE_COMPONENT_CHARACTER_CONTROLLER: {
                    le_character_desc d;

                    memset(&d, 0, sizeof(d));
                    d.radius = 0.4f;
                    d.height = 1.8f;
                    d.up[1] = 1.0f;
                    d.max_slope_angle = 50.0f * 3.14159265f /
                                        180.0f;
                    memcpy(cmd.comp_bytes, &d, sizeof(d));
                    cmd.comp_size = (uint32_t)sizeof(d);
                    break;
                }
                default:
                    break;
                }
                leg_run(ctx, &cmd, "Component added");
            }
        }
    }
    ImGui::EndPopup();
}

/* Component remove buttons (one per present component; undoable
 * REMOVE_COMPONENT commands). Renderable remove included
 * (script remove too — direct component, not an asset op). */
static void leg_component_remove_row(leg_context *ctx,
                                     const le_object *obj,
                                     le_world *w) {
    struct {
        const char *label;
        le_component_type type;
    } items[] = {
        { "Camera", LE_COMPONENT_CAMERA },
        { "Light", LE_COMPONENT_LIGHT },
        { "Renderable", LE_COMPONENT_RENDERABLE },
        { "Script", LE_COMPONENT_SCRIPT },
        { "Rigid body", LE_COMPONENT_RIGID_BODY },
        { "Collider", LE_COMPONENT_COLLIDER },
        { "Animator", LE_COMPONENT_ANIMATOR },
        { "Character", LE_COMPONENT_CHARACTER_CONTROLLER },
    };
    size_t k = 0;

    for (k = 0; k < sizeof(items) / sizeof(items[0]); k++) {
        if (!le_object_has_component(w, obj, items[k].type)) {
            continue;
        }
        {
            char name[64];

            snprintf(name, sizeof(name), "Remove %s",
                     items[k].label);
            if (ImGui::Button(name)) {
                led_command cmd;

                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = LED_CMD_REMOVE_COMPONENT;
                snprintf(cmd.label, sizeof(cmd.label), "Remove %s",
                         items[k].label);
                cmd.target = *obj;
                cmd.component = items[k].type;
                leg_run(ctx, &cmd, "Component removed");
            }
        }
    }
}

/* Section title for an inspector row path ("transform.*" ->
 * "Transform", "script.*" -> "Script", ...). Never NULL. */
static const char *leg_inspector_section(const char *path) {
    if (path == NULL) {
        return "Properties";
    }
    if (strncmp(path, "transform.", 10) == 0) {
        return "Transform";
    }
    if (strncmp(path, "object.", 7) == 0) {
        return "Object";
    }
    if (strncmp(path, "camera.", 7) == 0) {
        return "Camera";
    }
    if (strncmp(path, "light.", 6) == 0) {
        return "Light";
    }
    if (strncmp(path, "renderable.", 11) == 0) {
        return "Renderable";
    }
    if (strncmp(path, "script.", 7) == 0) {
        return "Script";
    }
    if (strncmp(path, "rigidbody.", 10) == 0) {
        return "Rigid body";
    }
    if (strncmp(path, "collider.", 9) == 0) {
        return "Collider";
    }
    if (strncmp(path, "animator.", 9) == 0) {
        return "Animator";
    }
    if (strncmp(path, "character.", 10) == 0) {
        return "Character";
    }
    return "Properties";
}

static void leg_panel_inspector(leg_context *ctx, leg_ui *ui) {
    le_object sel = LE_OBJECT_INVALID;
    uint32_t count = 0;
    bool open = false;

    if (!ui->show_inspector) {
        return;
    }
    open = ui->show_inspector != 0;
    if (!ImGui::Begin("Inspector", &open)) {
        ImGui::End();
        ui->show_inspector = open ? 1 : 0;
        return;
    }
    ui->show_inspector = open ? 1 : 0;
    count = led_selection_get(ctx->session, &sel, 1);
    if (count != 1) {
        ImGui::TextDisabled("Nothing selected.");
        ImGui::TextDisabled(count == 0
                                ? "Select an object in the Hierarchy "
                                  "or the Viewport."
                                : "Multi-select: showing the first.");
        if (count == 0) {
            ImGui::End();
            return;
        }
        led_selection_get(ctx->session, &sel, 1);
    }
    {
        le_world *w = led_session_get_edit_world(ctx->session);
        const char *nm =
            (w != NULL) ? le_object_get_name(w, &sel) : NULL;

        /* Object header: type icon + name (rename via Hierarchy). */
        leg_icon_draw(leg_hierarchy_icon(ctx, &sel), 16.0f,
                      ImGui::GetColorU32(ImGuiCol_Text));
        ImGui::SameLine(0, 6);
        ImGui::Text("%s",
                    (nm != NULL && nm[0] != '\0') ? nm : "(unnamed)");
    }
    ImGui::Separator();
    {
        uint32_t rows = led_inspect(ctx->session, &sel);
        const led_inspector_row *prows =
            led_inspector_rows(ctx->session);
        uint32_t i = 0;
        const char *cur_section = NULL;

        if (rows == 0 || prows == NULL) {
            ImGui::TextDisabled("No editable properties.");
        }
        /* Rows arrive grouped by component (static table order +
         * appended script props): emit a section header on prefix
         * change instead of a flat endless list. */
        for (i = 0; i < rows; i++) {
            const char *sec =
                leg_inspector_section(prows[i].desc.path);

            if (cur_section == NULL ||
                strcmp(sec, cur_section) != 0) {
                cur_section = sec;
                if (i > 0) {
                    ImGui::Spacing();
                }
                ImGui::SeparatorText(sec);
            }
            leg_widget_row(ctx, ui, (int)i, &prows[i], &sel);
        }
    }
    /* Add/remove component (undoable commands; the add path is a
     * button + popup — the inspector has no menu bar). */
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::CollapsingHeader("Components")) {
        le_world *w = led_session_get_edit_world(ctx->session);

        leg_component_add_menu(ctx, ui, &sel);
        if (w != NULL) {
            leg_component_remove_row(ctx, &sel, w);
        }
    }
    ImGui::End();
}

/* ------------------------------------------------------------------ */
/* Assets (browser model render + drag staging + drops).              */
/* ------------------------------------------------------------------ */

static const char *leg_asset_type_name(led_project_asset_type t) {
    switch (t) {
    case LED_PROJECT_ASSET_MODEL: return "model";
    case LED_PROJECT_ASSET_TEXTURE: return "texture";
    case LED_PROJECT_ASSET_SCRIPT: return "script";
    case LED_PROJECT_ASSET_SCENE: return "scene";
    case LED_PROJECT_ASSET_PREFAB: return "prefab";
    case LED_PROJECT_ASSET_MESH: return "mesh";
    case LED_PROJECT_ASSET_MATERIAL: return "material";
    case LED_PROJECT_ASSET_SKELETON: return "skeleton";
    case LED_PROJECT_ASSET_CLIP: return "clip";
    default: return "unknown";
    }
}

static void leg_panel_assets(leg_context *ctx, leg_ui *ui) {
    bool open = false;

    if (!ui->show_assets) {
        return;
    }
    open = ui->show_assets != 0;
    if (!ImGui::Begin("Assets", &open)) {
        ImGui::End();
        ui->show_assets = open ? 1 : 0;
        return;
    }
    ui->show_assets = open ? 1 : 0;
    if (!led_project_is_open(ctx->session)) {
        ImGui::TextDisabled("(no project open)");
        ImGui::End();
        return;
    }
    /* Filter + search + sort (sessions-owned view; refresh first). */
    {
        char *search = ui->asset_search;
        int type = ui->asset_type_filter;
        int sort = ui->asset_sort;

        ImGui::SetNextItemWidth(-140);
        ImGui::InputText("##asset-search", search,
                         sizeof(ui->asset_search));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Search assets (name or path)");
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64);
        {
            const char *names = "All\0model\0texture\0script\0scene\0"
                                "prefab\0";
            int cur = type + 1;

            /* Filter combo covers persisted source types only
             * (sub-assets are DB-internal; mesh/material/skeleton/
             * clip resolve through their model record). */
            if (ImGui::Combo("##asset-type", &cur, names)) {
                type = cur - 1;
                if (type < -1) {
                    type = -1;
                }
                if (type > LED_PROJECT_ASSET_PREFAB) {
                    type = LED_PROJECT_ASSET_PREFAB;
                }
                ui->asset_type_filter = type;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Filter by type");
            }
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(64);
        if (ImGui::Combo("##asset-sort", &sort,
                         "Path\0Name\0Type\0")) {
            sort = (sort < 0) ? 0 : ((sort > 2) ? 2 : sort);
            ui->asset_sort = sort;
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Sort order");
        }
        {
            led_project_asset_type filter =
                (type < 0) ? LED_PROJECT_ASSET_TYPE_COUNT
                           : (led_project_asset_type)type;

            led_browser_set_filter(ctx->session, filter, search,
                                   sort);
        }
    }
    led_browser_refresh(ctx->session);
    /* Folder tree (borrowed paths; selection by UUID only). */
    {
        uint32_t nf = led_browser_folder_count(ctx->session);
        const led_browser_folder *folders =
            led_browser_folders(ctx->session);
        uint32_t i = 0;

        if (ImGui::TreeNode("Folders")) {
            for (i = 0; i < nf; i++) {
                const char *p = (folders[i].path[0] != '\0')
                                    ? folders[i].path
                                    : "(root)";
                char buf[640];

                leg_icon_draw(LEG_ICON_FOLDER, 14.0f,
                              ImGui::GetColorU32(ImGuiCol_Text));
                ImGui::SameLine(0, 4);
                snprintf(buf, sizeof(buf), "%s — %u", p,
                         folders[i].asset_count);
                ImGui::TextDisabled("%s", buf);
            }
            ImGui::TreePop();
        }
    }
    ImGui::Separator();
    /* Asset list (deterministic view order; click selects by UUID,
     * double-click opens scenes / instantiates prefabs /
     * creates prefabs from the selection). */
    {
        uint32_t na = led_browser_asset_count(ctx->session);
        uint32_t i = 0;

        if (na == 0) {
            ImGui::TextDisabled("No assets match.");
        }
        for (i = 0; i < na; i++) {
            led_asset_record rec;

            memset(&rec, 0, sizeof(rec));
            if (!led_browser_asset_at(ctx->session, i, &rec)) {
                continue;
            }
            {
                /* File name first (scannable), type + status after.
                 * Full source path rides the tooltip. */
                const char *base = rec.source_path;
                const char *slash = NULL;
                const char *s = NULL;
                const char *st = "";
                char label[1152];

                for (s = base; *s != '\0'; s++) {
                    if (*s == '/' || *s == '\\') {
                        slash = s;
                    }
                }
                if (slash != NULL) {
                    base = slash + 1;
                }
                if (rec.status == LED_IMPORT_STALE) {
                    st = " · stale";
                } else if (rec.status == LED_IMPORT_FAILED) {
                    st = " · failed";
                } else if (rec.status != LED_IMPORT_READY) {
                    st = " · unimported";
                }
                snprintf(label, sizeof(label), "%s  [%s%s]", base,
                         leg_asset_type_name(rec.type), st);
                leg_icon_draw(
                    leg_icon_for_asset(
                        (int)rec.type,
                        rec.status == LED_IMPORT_FAILED),
                    14.0f,
                    ImGui::GetColorU32(
                        rec.status == LED_IMPORT_FAILED
                            ? ImGuiCol_TextDisabled
                            : ImGuiCol_Text));
                ImGui::SameLine(0, 4);
                if (ImGui::Selectable(label, false)) {
                    led_browser_clear_selection(ctx->session);
                    led_browser_select(ctx->session, &rec.id);
                }
                /* Probe: asset-row rect for automation (the
                 * Selectable is the last item). */
                leg_probe_record_asset(ui, rec.source_path);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s", rec.source_path);
                }
                if (ImGui::IsItemHovered() &&
                    ImGui::IsMouseDoubleClicked(0)) {
                    if (rec.type == LED_PROJECT_ASSET_SCENE) {
                        led_drag_payload pay;

                        memset(&pay, 0, sizeof(pay));
                        pay.asset = rec.id;
                        pay.type = rec.type;
                        if (led_open_scene_payload(ctx->session,
                                                   &pay) ==
                            LED_SUCCESS) {
                            leg_status(ctx, "Scene opened", 0);
                        } else {
                            leg_status(ctx, "Scene open failed", 1);
                        }
                    } else if (rec.type ==
                               LED_PROJECT_ASSET_PREFAB) {
                        le_asset prefab = LE_ASSET_INVALID;
                        char rel[1024];

                        strncpy(rel, rec.source_path,
                                sizeof(rel) - 1);
                        if (led_prefab_load(ctx->session, rel,
                                            &prefab) == LED_SUCCESS) {
                            led_command cmd;

                            memset(&cmd, 0, sizeof(cmd));
                            cmd.kind =
                                LED_CMD_INSTANTIATE_PREFAB;
                            snprintf(cmd.label, sizeof(cmd.label),
                                     "Instantiate prefab");
                            cmd.prefab.prefab_asset = prefab;
                            leg_run(ctx, &cmd,
                                    "Prefab instantiated");
                        } else {
                            leg_status(ctx, "Prefab load failed", 1);
                        }
                    }
                }
                /* Drag source (payload = UUID + type, never paths).
                 * led_drag_begin snapshots the selection into the
                 * session drag state; the ImGui payload carries the
                 * same bytes for the viewport/hierarchy targets. */
                if (ImGui::BeginDragDropSource()) {
                    led_browser_clear_selection(ctx->session);
                    led_browser_select(ctx->session, &rec.id);
                    {
                        led_drag_payload pay;

                        memset(&pay, 0, sizeof(pay));
                        if (led_drag_begin(ctx->session, &pay)) {
                            ImGui::SetDragDropPayload(
                                "LUMA_ASSET", &pay, sizeof(pay));
                            ImGui::Text("%s", rec.source_path);
                        }
                    }
                    ImGui::EndDragDropSource();
                }
            }
        }
    }
    /* Selected-record inspector (browser rows are LED_DATA_STRING
     * read-only metadata; the backing store is the PROJECT's
     * browser_inspector, not the session inspector rows) +
     * Import/Reimport + create-prefab-from-selection. */
    {
        led_project_asset_id selids[1];
        uint32_t nsel = led_browser_get_selection(
            ctx->session, selids, 1);

        if (nsel == 1) {
            uint32_t rows =
                led_browser_inspect(ctx->session, &selids[0]);

            /* Browser rows live in the SESSION inspector store
             * (browser_inspect borrows it); copy them out NOW —
             * the object inspector below must not run before we
             * render (led_inspect would clobber the same store). */
            {
                char paths[16][64];
                char labels[16][96];
                char values[16][256];
                uint32_t ncopy = (rows > 16) ? 16 : rows;
                const led_inspector_row *brows =
                    led_inspector_rows(ctx->session);
                uint32_t i = 0;

                if (brows != NULL) {
                    for (i = 0; i < ncopy; i++) {
                        strncpy(paths[i],
                                brows[i].desc.path != NULL
                                    ? brows[i].desc.path
                                    : "",
                                sizeof(paths[i]) - 1);
                        strncpy(labels[i],
                                brows[i].desc.label != NULL
                                    ? brows[i].desc.label
                                    : "",
                                sizeof(labels[i]) - 1);
                        strncpy(values[i],
                                brows[i].has_value
                                    ? brows[i].value.string_value
                                    : "(—)",
                                sizeof(values[i]) - 1);
                    }
                    ImGui::Separator();
                    ImGui::Text("Selected asset:");
                    for (i = 0; i < ncopy; i++) {
                        ImGui::BulletText("%s: %s", labels[i],
                                          values[i]);
                    }
                }
                (void)paths;
            }
            if (ImGui::Button("Import")) {
                led_asset_record rec;

                memset(&rec, 0, sizeof(rec));
                if (led_assetdb_find_by_id(ctx->session, &selids[0],
                                           &rec)) {
                    if (led_import_asset(ctx->session,
                                         rec.source_path) ==
                        LED_SUCCESS) {
                        leg_status(ctx, "Imported", 0);
                    } else {
                        leg_status(ctx, "Import failed", 1);
                    }
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Reimport")) {
                if (led_reimport_asset(ctx->session, &selids[0],
                                       1) == LED_SUCCESS) {
                    leg_status(ctx, "Reimported", 0);
                } else {
                    leg_status(ctx, "Reimport failed", 1);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Prefab from selection")) {
                le_object sel = LE_OBJECT_INVALID;

                if (led_selection_get(ctx->session, &sel, 1) ==
                    1) {
                    led_asset_record rec;

                    memset(&rec, 0, sizeof(rec));
                    if (led_assetdb_find_by_id(
                            ctx->session, &selids[0], &rec)) {
                        leg_status(ctx,
                                   "Prefab path: type it in "
                                   "Prefab... (create writes the "
                                   ".luprefab first)",
                                   1);
                    }
                    /* Direct create path (selection -> path):
                     * reuse the selected record's path when it IS
                     * a prefab path, else report the contract. */
                    if (rec.type == LED_PROJECT_ASSET_PREFAB) {
                        if (led_prefab_create(ctx->session, &sel,
                                              rec.source_path) ==
                            LED_SUCCESS) {
                            leg_status(ctx, "Prefab created", 0);
                        } else {
                            leg_status(ctx, "Prefab create failed "
                                            "(exists? pointers?)",
                                       1);
                        }
                    }
                } else {
                    leg_status(ctx,
                               "Prefab create needs an object "
                               "selection",
                               1);
                }
            }
        }
    }
    ImGui::End();
}

/* ------------------------------------------------------------------ */
/* Console + status bar.                                              */
/* ------------------------------------------------------------------ */

static void leg_panel_console(leg_context *ctx, leg_ui *ui) {
    uint32_t n = 0;
    uint32_t i = 0;
    bool open = false;

    if (!ui->show_console) {
        return;
    }
    open = ui->show_console != 0;
    if (!ImGui::Begin("Console", &open)) {
        ImGui::End();
        ui->show_console = open ? 1 : 0;
        return;
    }
    ui->show_console = open ? 1 : 0;
    {
        bool scroll = ui->console_autoscroll != 0;

        ImGui::Checkbox("Autoscroll", &scroll);
        ui->console_autoscroll = scroll ? 1 : 0;
    }
    ImGui::SameLine();
    {
        int lvl = ui->console_level;

        ImGui::SetNextItemWidth(120);
        if (ImGui::Combo("Level", &lvl, "All\0Warnings+\0Errors\0")) {
            ui->console_level = lvl;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        led_console_clear(ctx->session);
    }
    /* Mirror pending script errors (engine -> console) once per
     * frame so Play failures surface without a manual step. */
    led_console_mirror_script_error(ctx->session);
    ImGui::Separator();
    ImGui::BeginChild("console-scroll", ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    n = led_console_count(ctx->session);
    if (n == 0) {
        ImGui::TextDisabled("No messages.");
    }
    for (i = 0; i < n; i++) {
        led_log_level lvl = LED_LOG_INFO;
        const char *tag = "";
        const char *msg =
            led_console_message(ctx->session, i, &lvl, &tag);

        if (msg == NULL) {
            continue;
        }
        if (ui->console_level == 1 && lvl < LED_LOG_WARNING) {
            continue;
        }
        if (ui->console_level == 2 && lvl < LED_LOG_ERROR) {
            continue;
        }
        {
            /* Severity styling from the design roles (quiet info,
             * amber warning, red error — no other colors). */
            leg_roles roles;
            ImVec4 col;

            leg_theme_roles(&roles);
            col = (lvl == LED_LOG_ERROR)
                      ? ImVec4(roles.error[0], roles.error[1],
                               roles.error[2], 1.0f)
                  : (lvl == LED_LOG_WARNING)
                      ? ImVec4(roles.warning[0], roles.warning[1],
                               roles.warning[2], 1.0f)
                      : ImVec4(roles.text_2nd[0], roles.text_2nd[1],
                               roles.text_2nd[2], 1.0f);
            ImGui::TextColored(col, "[%s] %s",
                               (tag != NULL && tag[0] != '\0')
                                   ? tag
                                   : "log",
                               msg);
        }
    }
    if (ui->console_autoscroll) {
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
    ImGui::End();
}

static void leg_panel_status(leg_context *ctx, leg_ui *ui) {
    led_session_stats stats;
    led_history_stats hist;
    leg_roles roles;

    if (!ui->show_status) {
        return;
    }
    memset(&stats, 0, sizeof(stats));
    memset(&hist, 0, sizeof(hist));
    led_session_get_stats(ctx->session, &stats);
    led_history_get_stats(ctx->session, &hist);
    leg_theme_roles(&roles);
    /* Status bar: bottom-docked panel (public docking API only —
     * BeginViewportSideBar lives in imgui_internal.h and is NOT
     * part of the confinement contract). No close button (status
     * visibility toggles from View menu only): pass NULL. */
    {
        ImGuiViewport *vp = ImGui::GetMainViewport();
        float h = 26.0f;

        ImGui::SetNextWindowPos(
            ImVec2(vp->Pos.x, vp->Pos.y + vp->Size.y - h));
        ImGui::SetNextWindowSize(ImVec2(vp->Size.x, h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(8, 3));
        if (ImGui::Begin("##status", NULL,
                         ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings)) {
            /* Luma identity dot (amber) ahead of the project name. */
            ImGui::TextColored(
                ImVec4(roles.accent[0], roles.accent[1],
                       roles.accent[2], 1.0f),
                "◆");
            ImGui::SameLine(0, 4);
            if (ui->status_text[0] != '\0') {
                ImVec4 col = ui->status_is_error
                                 ? ImVec4(roles.error[0],
                                          roles.error[1],
                                          roles.error[2], 1.0f)
                                 : ImVec4(roles.success[0],
                                          roles.success[1],
                                          roles.success[2], 1.0f);

                ImGui::TextColored(col, "%s", ui->status_text);
                ImGui::SameLine();
                ImGui::TextDisabled("·");
                ImGui::SameLine();
            }
            {
                led_project_info info;
                int playing = led_is_playing(ctx->session);

                memset(&info, 0, sizeof(info));
                led_project_get_info(ctx->session, &info);
                ImGui::TextDisabled(
                    "%s   %u selected   undo %u · redo %u   %s%s",
                    led_project_is_open(ctx->session) ? info.name
                                                      : "No project",
                    stats.selection_count, hist.undo_depth,
                    hist.redo_depth,
                    playing ? "PLAYING" : "edit",
                    led_is_dirty(ctx->session) ? " ●" : "");
                if (playing) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(
                        ImVec4(roles.play[0], roles.play[1],
                               roles.play[2], 1.0f),
                        "●");
                }
                {
                    const char *sp =
                        led_scene_get_path(ctx->session);

                    if (sp != NULL && sp[0] != '\0') {
                        ImGui::SameLine();
                        ImGui::TextDisabled("·");
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", sp);
                    }
                }
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }
}

/* ------------------------------------------------------------------ */
/* Viewport panel (image + camera + picking + drops).                 */
/* ------------------------------------------------------------------ */

/* The app host owns the led_viewport + scene target; panels borrow
 * them through these hooks (set once per frame from gui_loop). */
struct leg_viewport_host {
    led_viewport *viewport;
    float dt;
};

static leg_viewport_host g_vp_host;

void leg_viewport_host_set(led_viewport *vp, float dt) {
    g_vp_host.viewport = vp;
    g_vp_host.dt = dt;
}

/* Panels-TU accessor for the gizmo overlay (same lib, one owner). */
led_viewport *leg_viewport_host_viewport(void) {
    return g_vp_host.viewport;
}

static void leg_panel_viewport(leg_context *ctx, leg_ui *ui) {
    bool open = false;

    if (!ui->show_viewport) {
        return;
    }
    open = ui->show_viewport != 0;
    if (!ImGui::Begin("Viewport", &open)) {
        ImGui::End();
        ui->show_viewport = open ? 1 : 0;
        return;
    }
    ui->show_viewport = open ? 1 : 0;
    {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 origin = ImGui::GetCursorScreenPos();

        /* Minimum useful viewport (below this the composite still
         * runs but the panel is a sliver — the default layout keeps
         * it far above; the clamp only guards pathological manual
         * docking). */
        if (avail.x < 200) {
            avail.x = 200;
        }
        if (avail.y < 150) {
            avail.y = 150;
        }
        /* Resize policy: viewport struct follows the panel (the
         * scene target rebuilds in gui_viewport_tex.cpp; zero-size
         * never creates a target). */
        if (g_vp_host.viewport != NULL) {
            g_vp_host.viewport->width = (uint32_t)avail.x;
            g_vp_host.viewport->height = (uint32_t)avail.y;
        }
        /* Scene image: the host composites the scene target on the
         * frame encoder BEFORE panels record (scene pass first,
         * then the swapchain pass holds panels+GUI). The panel
         * samples the live target set here (one frame of latency,
         * zero pass conflicts). Until the first composite lands
         * (generation 0 = never composited, or a NULL set), the
         * camera/state readout keeps the panel honest — submitting
         * Image() with a never-composited set would fail the draw
         * walk (stale TexID) on the very first frame. */
        if (g_vp_host.viewport != NULL &&
            ctx->viewport_target != NULL &&
            ctx->viewport_target->set != NULL &&
            ctx->viewport_target->generation > 0) {
            ImTextureID scene_tex =
                (ImTextureID)(uint64_t)(uintptr_t)
                    ctx->viewport_target->set;

            ImGui::Image(scene_tex, avail);
        } else if (g_vp_host.viewport != NULL) {
            led_viewport *vp = g_vp_host.viewport;

            ImGui::TextDisabled(
                "camera yaw %.2f pitch %.2f dist %.2f (%ux%u)",
                vp->yaw_rad, vp->pitch_rad, vp->distance,
                vp->width, vp->height);
        } else {
            ImGui::TextDisabled("(no viewport bound)");
        }
        ImGui::InvisibleButton("##vp-capture", avail);
        leg_probe_record_viewport(ui);
        {
            int hovered = ImGui::IsItemHovered() ? 1 : 0;

            /* Gizmo mode hotkeys (W/E/R): fresh press while the
             * viewport is hovered, no Ctrl/Cmd held, not playing.
             * Sets gizmo_switched_key so the overlay fly block skips
             * flying that key this same frame. */
            if (hovered && !leg_wants_keyboard(ctx) &&
                !led_is_playing(ctx->session)) {
                const ImGuiIO &hkio = ImGui::GetIO();

                if (!hkio.KeyCtrl && !hkio.KeySuper) {
                    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
                        ui->gizmo_mode = LED_GIZMO_TRANSLATE;
                        ui->gizmo_switched_key = ImGuiKey_W;
                    } else if (ImGui::IsKeyPressed(ImGuiKey_E,
                                                   false)) {
                        ui->gizmo_mode = LED_GIZMO_ROTATE;
                        ui->gizmo_switched_key = ImGuiKey_E;
                    } else if (ImGui::IsKeyPressed(ImGuiKey_R,
                                                   false)) {
                        ui->gizmo_mode = LED_GIZMO_SCALE;
                        ui->gizmo_switched_key = ImGuiKey_R;
                    }
                }
            }
            /* Camera owns input while orbiting (RMB drag / wheel /
             * WASD when the viewport is hovered and the GUI does not
             * want the keyboard). */
            if (g_vp_host.viewport != NULL) {
                leg_viewport_camera_update(ctx, ui,
                                           g_vp_host.viewport,
                                           hovered, g_vp_host.dt);
            }
            /* Gizmo overlay first (drags capture the click; the
             * return tells selection to stand down). */
            {
                int captured =
                    leg_gizmo_overlay(ctx, ui, &origin, &avail);

                /* Click-to-select (left click without drag, not on
                 * a gizmo handle): physics-raycast pick via core. */
                if (!captured && hovered &&
                    ImGui::IsMouseClicked(0) &&
                    !leg_wants_keyboard(ctx)) {
                    ImVec2 m = ImGui::GetMousePos();
                    float px = m.x - origin.x;
                    float py = m.y - origin.y;

                    if (g_vp_host.viewport != NULL) {
                        led_viewport_pick_select(
                            ctx->session, g_vp_host.viewport, px,
                            py);
                    }
                }
            }
            /* Drops: asset payloads + hierarchy reparent payloads. */
            if (ImGui::BeginDragDropTarget()) {
                const ImGuiPayload *pl =
                    ImGui::AcceptDragDropPayload("LUMA_ASSET");

                if (pl != NULL &&
                    pl->DataSize == sizeof(led_drag_payload)) {
                    const led_drag_payload *dp =
                        (const led_drag_payload *)pl->Data;

                    if (dp->type == LED_PROJECT_ASSET_MODEL ||
                        dp->type == LED_PROJECT_ASSET_MESH) {
                        float pos[3] = { 0, 0, 0 };

                        if (led_drop_model_into_scene(
                                ctx->session, dp, pos)) {
                            leg_status(ctx, "Model dropped", 0);
                        } else {
                            leg_status(ctx, "Model drop failed", 1);
                        }
                    } else if (dp->type ==
                               LED_PROJECT_ASSET_PREFAB) {
                        leg_status(
                            ctx,
                            "Prefab drop: use Prefab... to place", 1);
                    } else if (dp->type ==
                               LED_PROJECT_ASSET_SCENE) {
                        if (led_open_scene_payload(ctx->session,
                                                   dp) ==
                            LED_SUCCESS) {
                            leg_status(ctx, "Scene opened", 0);
                        } else {
                            leg_status(ctx, "Scene open failed", 1);
                        }
                    } else if (dp->type ==
                                   LED_PROJECT_ASSET_MATERIAL ||
                               dp->type ==
                                   LED_PROJECT_ASSET_SCRIPT) {
                        /* Material/script drops land ON objects:
                         * pick the drop point and assign. */
                        ImVec2 m = ImGui::GetMousePos();
                        float px = m.x - origin.x;
                        float py = m.y - origin.y;
                        le_ray_hit hit;

                        memset(&hit, 0, sizeof(hit));
                        if (g_vp_host.viewport != NULL &&
                            led_viewport_pick(
                                ctx->session,
                                g_vp_host.viewport, px, py, 0,
                                0xFFFFFFFFu, &hit)) {
                            int ok = 0;

                            if (dp->type ==
                                LED_PROJECT_ASSET_MATERIAL) {
                                ok = led_drop_material_onto_object(
                                    ctx->session, dp, &hit.object);
                            } else {
                                ok = led_drop_script_onto_object(
                                    ctx->session, dp, &hit.object);
                            }
                            leg_status(
                                ctx,
                                ok ? "Assigned" : "Assign failed",
                                ok ? 0 : 1);
                        } else {
                            leg_status(ctx, "Drop hit nothing", 1);
                        }
                    } else {
                        leg_status(ctx,
                                   "Drop type not placeable here",
                                   1);
                    }
                }
                {
                    const ImGuiPayload *op =
                        ImGui::AcceptDragDropPayload("LUMA_OBJECT");

                    if (op != NULL &&
                        op->DataSize == sizeof(le_object)) {
                        /* Viewport background is the scene root:
                         * unparent the dragged object. */
                        const le_object *dp =
                            (const le_object *)op->Data;
                        led_command cmd;

                        memset(&cmd, 0, sizeof(cmd));
                        cmd.kind = LED_CMD_SET_PARENT;
                        snprintf(cmd.label, sizeof(cmd.label),
                                 "Unparent");
                        cmd.target = *dp;
                        cmd.has_parent = 0;
                        leg_run(ctx, &cmd, "Unparented");
                    }
                }
                ImGui::EndDragDropTarget();
            }
        }
    }
    ImGui::End();
}

/* ------------------------------------------------------------------ */
/* Frame entry (called between leg_frame_begin/end by the host).      */
/* ------------------------------------------------------------------ */

void leg_panels_frame(leg_context *ctx, led_viewport *vp, float dt) {
    leg_ui *ui = NULL;

    if (ctx == NULL || ctx->imgui == NULL) {
        return;
    }
    ImGui::SetCurrentContext(ctx->imgui);
    ui = leg_ui_ensure(ctx);
    if (ui == NULL) {
        return;
    }
    leg_viewport_host_set(vp, dt);
    /* Focus policy: viewport shortcuts die while a text field owns
     * focus (console/field) — mirror ImGui's text-input desire into
     * the core flag every frame. */
    {
        const ImGuiIO &io = ImGui::GetIO();

        led_focus_set(ctx->session, io.WantTextInput ? 1 : 0);
    }
    /* Keyboard shortcuts (Ctrl+Z/Y/D/S, Del, F5/Shift+F5, F10, F,
     * W/E/R gizmo modes): fire ONLY when the GUI does not want the
     * keyboard (no text field focused) and this is a fresh press (no
     * auto-repeat storm: IsKeyPressed with repeat=false). W/E/R switch
     * the gizmo mode when the viewport is hovered (the routing doc
     * promises them); the fly block in the gizmo overlay skips the
     * press frame for a switched key (see ui->gizmo_switched_key). */
    ui->gizmo_switched_key = 0;
    if (!leg_wants_keyboard(ctx)) {
        struct {
            ImGuiKey key;
            led_action action;
            int needs_ctrl;
            int needs_shift;
        } binds[] = {
            { ImGuiKey_Z, LED_ACTION_UNDO, 1, 0 },
            { ImGuiKey_Y, LED_ACTION_REDO, 1, 0 },
            { ImGuiKey_D, LED_ACTION_DUPLICATE, 1, 0 },
            { ImGuiKey_S, LED_ACTION_SAVE, 1, 0 },
            { ImGuiKey_Delete, LED_ACTION_DELETE, 0, 0 },
            { ImGuiKey_F, LED_ACTION_FOCUS_SELECTION, 0, 0 },
        };
        size_t b = 0;

        for (b = 0; b < sizeof(binds) / sizeof(binds[0]); b++) {
            const ImGuiIO &io = ImGui::GetIO();
            int ctrl_ok =
                binds[b].needs_ctrl ? (io.KeyCtrl != 0) : 1;
            int shift_ok =
                binds[b].needs_shift ? (io.KeyShift != 0) : 1;

            if (ctrl_ok && shift_ok &&
                ImGui::IsKeyPressed(binds[b].key, false)) {
                led_dispatch_action(ctx->session, binds[b].action,
                                    g_vp_host.viewport);
            }
        }
        /* Play/Stop/Step are mode-edged (F5 toggles by state). */
        {
            const ImGuiIO &io = ImGui::GetIO();
            int playing = led_is_playing(ctx->session);

            if (ImGui::IsKeyPressed(ImGuiKey_F5, false)) {
                if (io.KeyShift || playing) {
                    led_dispatch_action(ctx->session,
                                        LED_ACTION_STOP,
                                        g_vp_host.viewport);
                } else {
                    led_dispatch_action(ctx->session,
                                        LED_ACTION_PLAY,
                                        g_vp_host.viewport);
                }
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F10, false)) {
                led_dispatch_action(ctx->session, LED_ACTION_STEP,
                                    g_vp_host.viewport);
            }
        }
        /* NOTE: W/E/R gizmo-mode hotkeys live in the viewport panel
         * (it alone knows this frame's hover state); the overlay's
         * fly block skips the switch frame via gizmo_switched_key. */
    }
    if (ImGui::BeginMainMenuBar()) {
        leg_menu_file(ctx, ui);
        leg_menu_edit(ctx, ui);
        leg_menu_view(ctx, ui);
        leg_menu_project(ctx, ui);
        leg_menu_help(ctx, ui);
        ImGui::EndMainMenuBar();
    }
    /* Chrome geometry: the menu bar reserves the viewport WorkPos
     * top strip; the toolbar stacks directly below it and the dock
     * host fills the remainder above the status bar. No overlaps by
     * construction (verified headed via window-pos diagnostics). */
    {
        ImGuiViewport *vp0 = ImGui::GetMainViewport();
        float menu_h = vp0->WorkPos.y - vp0->Pos.y;
        float tool_h = ui->show_toolbar ? 40.0f : 0.0f;
        float status_h = ui->show_status ? 26.0f : 0.0f;

        if (menu_h < 0) {
            menu_h = 0;
        }
        ui->dock_y = menu_h + tool_h;
        ui->dock_h = vp0->Size.y - menu_h - tool_h - status_h;
    }
    if (ui->show_toolbar) {
        /* Toolbar: fixed chrome below the menu bar (same public-API
         * discipline as the status bar). No close button: pass NULL.
         * Height 40 = 28px tool buttons + padding (design system). */
        ImGuiViewport *vp0 = ImGui::GetMainViewport();
        float h = 40.0f;

        ImGui::SetNextWindowPos(
            ImVec2(vp0->Pos.x, vp0->Pos.y + (ui->dock_y - h)));
        ImGui::SetNextWindowSize(ImVec2(vp0->Size.x, h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(8, 4));
        if (ImGui::Begin("##toolbar", NULL,
                         ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings)) {
            leg_toolbar(ctx, ui);
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }
    /* Dock host: fixed chrome window filling the panel area; the
     * LEG_DOCK_ROOT dockspace lives inside it (public DockSpace API;
     * the layout TU builds the default node tree on the same id). */
    {
        ImGuiViewport *vp0 = ImGui::GetMainViewport();

        ImGui::SetNextWindowPos(
            ImVec2(vp0->Pos.x, vp0->Pos.y + ui->dock_y));
        ImGui::SetNextWindowSize(
            ImVec2(vp0->Size.x, ui->dock_h));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(0, 0));
        if (ImGui::Begin("##dockhost", NULL,
                         ImGuiWindowFlags_NoDecoration |
                             ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus)) {
            ImGui::DockSpace(LEG_DOCK_ROOT, ImVec2(0, 0),
                             ImGuiDockNodeFlags_PassthruCentralNode);
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
    }
    leg_layout_ensure(ctx);
    leg_create_popup(ctx, ui);
    leg_prefab_popup(ctx, ui);
    leg_panel_viewport(ctx, ui);
    leg_panel_hierarchy(ctx, ui);
    leg_panel_inspector(ctx, ui);
    leg_panel_assets(ctx, ui);
    leg_panel_console(ctx, ui);
    leg_panel_status(ctx, ui);
}
