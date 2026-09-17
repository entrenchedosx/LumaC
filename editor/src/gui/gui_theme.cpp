/* Phase 33V Luma editor theme ("Luma Slate" dark theme + font chain).
 *
 * Isolated C++ over public C ABIs (same confinement as the rest of
 * editor/src/gui/). Owns:
 *   - leg_theme_apply(): full ImGuiStyle setup from the design-system
 *     roles in docs/EDITOR_DESIGN_SYSTEM.md (no one-off style values
 *     in panel code; panels may PushStyleVar locally ONLY for chrome
 *     zero-rounding documented at the call site).
 *   - leg_font_load(): UI font chain — Segoe UI (Windows system font,
 *     NOT redistributed) -> DejaVu Sans (Linux distro font, NOT
 *     redistributed) -> ImGui default raster (always works). Called
 *     from leg_context_create BEFORE the first frame so the atlas
 *     builds with the chosen face; the existing gui_font.cpp walk
 *     uploads whatever the atlas holds (single atlas texture, so no
 *     bridge change is needed).
 */

#include <cstdio>
#include <cstring>

#include "gui_internal.h"

/* Design-system roles (docs/EDITOR_DESIGN_SYSTEM.md). Linear 0..1. */
static void leg_roles_luma_slate(leg_roles *p) {
    memset(p, 0, sizeof(*p));
    p->window_bg[0] = 0.10f; p->window_bg[1] = 0.11f; p->window_bg[2] = 0.13f;
    p->child_bg[0] = 0.08f; p->child_bg[1] = 0.09f; p->child_bg[2] = 0.11f;
    p->titlebar[0] = 0.13f; p->titlebar[1] = 0.14f; p->titlebar[2] = 0.17f;
    p->titlebar_active[0] = 0.16f; p->titlebar_active[1] = 0.17f;
    p->titlebar_active[2] = 0.21f;
    p->menu_bg[0] = 0.12f; p->menu_bg[1] = 0.13f; p->menu_bg[2] = 0.16f;
    p->border[0] = 0.23f; p->border[1] = 0.24f; p->border[2] = 0.28f;
    p->text[0] = 0.88f; p->text[1] = 0.89f; p->text[2] = 0.92f;
    p->text_2nd[0] = 0.55f; p->text_2nd[1] = 0.57f; p->text_2nd[2] = 0.62f;
    p->text_dis[0] = 0.38f; p->text_dis[1] = 0.39f; p->text_dis[2] = 0.43f;
    /* Luma amber (single accent). */
    p->accent[0] = 0.95f; p->accent[1] = 0.62f; p->accent[2] = 0.18f;
    p->selection[0] = 0.72f; p->selection[1] = 0.46f;
    p->selection[2] = 0.13f; p->selection[3] = 0.55f;
    p->hover[0] = 1.00f; p->hover[1] = 1.00f;
    p->hover[2] = 1.00f; p->hover[3] = 0.06f;
    p->active_tool[0] = 0.95f; p->active_tool[1] = 0.62f;
    p->active_tool[2] = 0.18f; p->active_tool[3] = 0.30f;
    p->play[0] = 0.35f; p->play[1] = 0.75f; p->play[2] = 0.40f;
    p->warning[0] = 1.00f; p->warning[1] = 0.80f; p->warning[2] = 0.35f;
    p->error[0] = 1.00f; p->error[1] = 0.42f; p->error[2] = 0.38f;
    p->success[0] = 0.55f; p->success[1] = 0.85f; p->success[2] = 0.60f;
}

/* Fill the design-system roles (read-only copy for panels that
 * need role colors, e.g. console severity + status pill). */
void leg_theme_roles(leg_roles *out);

/* Apply the full style (call once per context, before first frame).
 * Starts from the ImGui default style (never memset: fields like
 * CurveTessellationTol must stay positive) and overrides every
 * visual role from the design system. */
void leg_theme_apply(void) {
    leg_roles p;
    ImGuiStyle &st = ImGui::GetStyle();

    leg_roles_luma_slate(&p);
    st.WindowPadding = ImVec2(8, 6);
    st.FramePadding = ImVec2(6, 3);
    st.CellPadding = ImVec2(6, 3);
    st.ItemSpacing = ImVec2(8, 4);
    st.ItemInnerSpacing = ImVec2(4, 4);
    st.TouchExtraPadding = ImVec2(0, 0);
    st.IndentSpacing = 16.0f;
    st.ScrollbarSize = 12.0f;
    st.GrabMinSize = 10.0f;
    st.WindowBorderSize = 1.0f;
    st.ChildBorderSize = 1.0f;
    st.PopupBorderSize = 1.0f;
    st.FrameBorderSize = 0.0f;
    st.TabBorderSize = 0.0f;
    st.WindowRounding = 4.0f;
    st.ChildRounding = 3.0f;
    st.FrameRounding = 3.0f;
    st.PopupRounding = 4.0f;
    st.ScrollbarRounding = 4.0f;
    st.GrabRounding = 3.0f;
    st.TabRounding = 3.0f;
    st.WindowTitleAlign = ImVec2(0.0f, 0.5f);
    st.ButtonTextAlign = ImVec2(0.5f, 0.5f);
    st.SelectableTextAlign = ImVec2(0.0f, 0.0f);
    st.SeparatorTextAlign = ImVec2(0.0f, 0.5f);
    st.SeparatorTextPadding = ImVec2(8, 3);
    st.DisplaySafeAreaPadding = ImVec2(4, 4);

#define LUMA_C3(a) ImVec4((a)[0], (a)[1], (a)[2], 1.0f)
#define LUMA_C4(a) ImVec4((a)[0], (a)[1], (a)[2], (a)[3])
    st.Colors[ImGuiCol_Text] = LUMA_C3(p.text);
    st.Colors[ImGuiCol_TextDisabled] = LUMA_C3(p.text_dis);
    st.Colors[ImGuiCol_WindowBg] = LUMA_C3(p.window_bg);
    st.Colors[ImGuiCol_ChildBg] = LUMA_C3(p.child_bg);
    st.Colors[ImGuiCol_PopupBg] = ImVec4(0.11f, 0.12f, 0.15f, 0.98f);
    st.Colors[ImGuiCol_Border] = LUMA_C3(p.border);
    st.Colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_FrameBg] =
        ImVec4(0.16f, 0.17f, 0.21f, 1.0f);
    st.Colors[ImGuiCol_FrameBgHovered] =
        ImVec4(0.20f, 0.21f, 0.26f, 1.0f);
    st.Colors[ImGuiCol_FrameBgActive] =
        ImVec4(0.24f, 0.25f, 0.31f, 1.0f);
    st.Colors[ImGuiCol_TitleBg] = LUMA_C3(p.titlebar);
    st.Colors[ImGuiCol_TitleBgActive] = LUMA_C3(p.titlebar_active);
    st.Colors[ImGuiCol_TitleBgCollapsed] = LUMA_C3(p.titlebar);
    st.Colors[ImGuiCol_MenuBarBg] = LUMA_C3(p.menu_bg);
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.09f, 0.10f, 0.12f, 1.0f);
    st.Colors[ImGuiCol_ScrollbarGrab] =
        ImVec4(0.28f, 0.29f, 0.34f, 1.0f);
    st.Colors[ImGuiCol_ScrollbarGrabHovered] =
        ImVec4(0.35f, 0.36f, 0.42f, 1.0f);
    st.Colors[ImGuiCol_ScrollbarGrabActive] =
        ImVec4(0.42f, 0.43f, 0.50f, 1.0f);
    st.Colors[ImGuiCol_CheckMark] = LUMA_C3(p.accent);
    st.Colors[ImGuiCol_SliderGrab] =
        ImVec4(0.55f, 0.56f, 0.62f, 1.0f);
    st.Colors[ImGuiCol_SliderGrabActive] = LUMA_C3(p.accent);
    st.Colors[ImGuiCol_Button] = ImVec4(0.18f, 0.19f, 0.23f, 1.0f);
    st.Colors[ImGuiCol_ButtonHovered] =
        ImVec4(0.24f, 0.25f, 0.30f, 1.0f);
    st.Colors[ImGuiCol_ButtonActive] =
        ImVec4(0.28f, 0.29f, 0.35f, 1.0f);
    st.Colors[ImGuiCol_Header] = ImVec4(0.18f, 0.19f, 0.23f, 1.0f);
    st.Colors[ImGuiCol_HeaderHovered] = LUMA_C4(p.hover);
    st.Colors[ImGuiCol_HeaderActive] =
        ImVec4(0.24f, 0.25f, 0.31f, 1.0f);
    st.Colors[ImGuiCol_Separator] = LUMA_C3(p.border);
    st.Colors[ImGuiCol_SeparatorHovered] = LUMA_C3(p.accent);
    st.Colors[ImGuiCol_SeparatorActive] = LUMA_C3(p.accent);
    st.Colors[ImGuiCol_ResizeGrip] =
        ImVec4(0.28f, 0.29f, 0.34f, 1.0f);
    st.Colors[ImGuiCol_ResizeGripHovered] = LUMA_C3(p.accent);
    st.Colors[ImGuiCol_ResizeGripActive] = LUMA_C3(p.accent);
    st.Colors[ImGuiCol_Tab] = ImVec4(0.13f, 0.14f, 0.17f, 1.0f);
    st.Colors[ImGuiCol_TabHovered] =
        ImVec4(0.20f, 0.21f, 0.26f, 1.0f);
    st.Colors[ImGuiCol_TabActive] =
        ImVec4(0.17f, 0.18f, 0.22f, 1.0f);
    st.Colors[ImGuiCol_TabUnfocused] =
        ImVec4(0.12f, 0.13f, 0.16f, 1.0f);
    st.Colors[ImGuiCol_TabUnfocusedActive] =
        ImVec4(0.16f, 0.17f, 0.21f, 1.0f);
    st.Colors[ImGuiCol_DockingPreview] =
        ImVec4(0.95f, 0.62f, 0.18f, 0.35f);
    st.Colors[ImGuiCol_DockingEmptyBg] = LUMA_C3(p.child_bg);
    st.Colors[ImGuiCol_PlotLines] = LUMA_C3(p.text_2nd);
    st.Colors[ImGuiCol_PlotLinesHovered] = LUMA_C3(p.accent);
    st.Colors[ImGuiCol_PlotHistogram] = LUMA_C3(p.accent);
    st.Colors[ImGuiCol_PlotHistogramHovered] =
        ImVec4(1.0f, 0.70f, 0.25f, 1.0f);
    st.Colors[ImGuiCol_TextSelectedBg] = LUMA_C4(p.selection);
    st.Colors[ImGuiCol_DragDropTarget] =
        ImVec4(0.95f, 0.62f, 0.18f, 0.60f);
    st.Colors[ImGuiCol_NavHighlight] =
        ImVec4(0.95f, 0.62f, 0.18f, 0.60f);
    st.Colors[ImGuiCol_NavWindowingHighlight] =
        ImVec4(0.95f, 0.62f, 0.18f, 0.60f);
    st.Colors[ImGuiCol_NavWindowingDimBg] =
        ImVec4(0.05f, 0.05f, 0.07f, 0.60f);
    st.Colors[ImGuiCol_ModalWindowDimBg] =
        ImVec4(0.05f, 0.05f, 0.07f, 0.60f);
#undef LUMA_C3
#undef LUMA_C4
}

void leg_theme_roles(leg_roles *out) {
    if (out == NULL) {
        return;
    }
    leg_roles_luma_slate(out);
}

/* Font chain: first existing file wins; 0 = default raster kept.
 * System fonts are READ (never redistributed, never written). */
int leg_font_load(void) {
    ImGuiIO &io = ImGui::GetIO();
    static const char *kCandidates[] = {
#ifdef _WIN32
        "C:/Windows/Fonts/segoeui.ttf",
#else
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
#endif
        NULL,
    };
    int k = 0;

    for (k = 0; kCandidates[k] != NULL; k++) {
        FILE *probe = NULL;
#ifdef _WIN32
        if (fopen_s(&probe, kCandidates[k], "rb") != 0) {
            probe = NULL;
        }
#else
        probe = fopen(kCandidates[k], "rb");
#endif
        if (probe == NULL) {
            continue;
        }
        fclose(probe);
        {
            ImFont *face = io.Fonts->AddFontFromFileTTF(
                kCandidates[k], 15.0f, NULL, NULL);

            if (face != NULL) {
                /* Use the system face for all UI text (the ImGui
                 * raster stays in the atlas as fallback). */
                io.FontDefault = face;
                return 1;
            }
        }
    }
    return 0;
}
