/* Phase 33V default editor layout (isolated C++ over public C ABIs).
 *
 * SINGLE-EXCEPTION TU: this file alone may include
 * imgui_internal.h, and ONLY for the DockBuilder* layout functions
 * (DockBuilderAddNode/SplitNode/DockWindow/Finish/RemoveNode/
 * SetNodePos/SetNodeSize). See docs/EDITOR_FRONTEND_DECISION.md for
 * the rationale (the public header itself documents
 * SetNextWindowDockID as "very limited"; the vendored ImGui is
 * version-pinned so the WIP-API risk is contained to upgrade time).
 * No other internal API is used anywhere in editor/src/gui/ — no
 * BeginViewportSideBar, no dock-node poking, no input internals.
 *
 * leg_layout_ensure(): builds the default dock layout once per
 * context (first panels frame, or on user "Reset layout"). It
 * addresses the SAME root dockspace id the frame loop submits
 * (LEG_DOCK_ROOT, shared via gui_internal.h) and never submits its
 * own DockSpace. Target (1280x800 default window):
 *   left 300px ......... Hierarchy (full height)
 *   right 300px ........ Inspector (full height)
 *   center ............. Viewport (top)
 *   center-bottom 240px  Assets | Console (tabbed)
 * The toolbar/status fixed windows (NoDocking) overlay top/bottom;
 * the builder insets the root node around that chrome.
 */

#include <cstring>

#include "gui_internal.h"
#include "imgui_internal.h"

void leg_layout_request_reset(leg_context *ctx) {
    leg_ui *ui = NULL;

    if (ctx == NULL) {
        return;
    }
    ui = ctx->ui;
    if (ui != NULL) {
        ui->layout_reset = 1;
    }
}

/* Build (or rebuild) the default layout on the loop's root node.
 * Runs AFTER the panels frame submits DockSpace(LEG_DOCK_ROOT) in
 * its dock-host window (the host owns position/size; the builder
 * only splits + docks). */
void leg_layout_ensure(leg_context *ctx) {
    leg_ui *ui = NULL;
    ImGuiViewport *vp = NULL;
    ImGuiID left = 0;
    ImGuiID right = 0;
    ImGuiID center = 0;
    ImGuiID bottom = 0;

    if (ctx == NULL || ctx->imgui == NULL) {
        return;
    }
    ui = ctx->ui;
    if (ui == NULL) {
        return;
    }
    if (ui->layout_built && !ui->layout_reset) {
        return;
    }
    ui->layout_built = 1;
    ui->layout_reset = 0;
    ImGui::SetCurrentContext(ctx->imgui);
    vp = ImGui::GetMainViewport();
    if (vp == NULL || vp->Size.x < 200 || vp->Size.y < 200) {
        /* Too small to lay out: retry next frame (keeps the flag
         * clear so a minimized first frame doesn't lock in a junk
         * layout). */
        ui->layout_built = 0;
        return;
    }
    {
        /* Ratios only (the host window fixes geometry): 300px side
         * bars and a 240px bottom stack at 1280x800, proportional
         * below that. The node needs an explicit size BEFORE the
         * splits (ratios convert against it); position comes from
         * the dock-host window's DockSpace submit. */
        float avail_w = vp->Size.x;
        float avail_h = vp->Size.y;
        float host_h = (ui->dock_h > 0) ? ui->dock_h : avail_h;
        float left_w = (avail_w > 900.0f) ? 300.0f : avail_w * 0.24f;
        float right_w = (avail_w > 900.0f) ? 300.0f : avail_w * 0.24f;
        float bottom_h =
            (avail_h > 500.0f) ? 240.0f : avail_h * 0.30f;

        /* Clear any user arrangement first (reset is total). */
        ImGui::DockBuilderRemoveNode(LEG_DOCK_ROOT);
        ImGui::DockBuilderAddNode(LEG_DOCK_ROOT,
                                  ImGuiDockNodeFlags_DockSpace);
        ImGui::DockBuilderSetNodeSize(LEG_DOCK_ROOT,
                                      ImVec2(avail_w, host_h));
        /* Left / right / center splits. */
        ImGui::DockBuilderSplitNode(LEG_DOCK_ROOT, ImGuiDir_Left,
                                    left_w / avail_w, &left,
                                    &center);
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Right,
                                    right_w /
                                        (avail_w - left_w),
                                    &right, &center);
        /* Center-bottom tab stack for Assets + Console (ratio is
         * relative to the center node, still full height here). */
        ImGui::DockBuilderSplitNode(center, ImGuiDir_Down,
                                    bottom_h / avail_h, &bottom,
                                    &center);
        ImGui::DockBuilderDockWindow("Hierarchy", left);
        ImGui::DockBuilderDockWindow("Inspector", right);
        ImGui::DockBuilderDockWindow("Viewport", center);
        ImGui::DockBuilderDockWindow("Assets", bottom);
        ImGui::DockBuilderDockWindow("Console", bottom);
        ImGui::DockBuilderFinish(LEG_DOCK_ROOT);
    }
}
