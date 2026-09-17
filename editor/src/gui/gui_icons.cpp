/* Phase 33V editor icons (isolated C++ over public C ABIs).
 *
 * Consistent ImDrawList-geometric icon set: no font files, no image
 * assets, no licensing exposure, no texture dependency, DPI-trivial
 * (vector primitives at the current cursor scale). Every icon draws
 * inside a square of side `size` at the current cursor position and
 * advances the cursor past it.
 *
 * Colors come from the caller (default: current style text color;
 * active tools pass the accent). leg_tool_button renders a themed
 * toolbar button (frame + icon + tooltip + active fill).
 */

#include <cmath>

#include "gui_internal.h"

leg_icon_kind leg_icon_for_asset(int asset_type, int status_failed);

void leg_icon_draw_at(leg_icon_kind kind, ImVec2 origin, float size,
                      ImU32 color) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    float s = (size > 0.0f) ? size : 14.0f;
    float cx = origin.x + s * 0.5f;
    float cy = origin.y + s * 0.5f;
    float u = s / 16.0f; /* icon grid unit (16x16 design space) */

    if (dl == NULL) {
        return;
    }
    switch (kind) {
    case LEG_ICON_PLAY: {
        ImVec2 a(cx - 4 * u, cy - 5 * u);
        ImVec2 b(cx - 4 * u, cy + 5 * u);
        ImVec2 c(cx + 5 * u, cy);
        dl->AddTriangleFilled(a, b, c, color);
        break;
    }
    case LEG_ICON_PAUSE: {
        float w = 2.2f * u;
        dl->AddRectFilled(ImVec2(cx - 3.5f * u, cy - 5 * u),
                          ImVec2(cx - 3.5f * u + w, cy + 5 * u),
                          color, 1.0f);
        dl->AddRectFilled(ImVec2(cx + 3.5f * u - w, cy - 5 * u),
                          ImVec2(cx + 3.5f * u, cy + 5 * u), color,
                          1.0f);
        break;
    }
    case LEG_ICON_STOP: {
        dl->AddRectFilled(ImVec2(cx - 4.5f * u, cy - 4.5f * u),
                          ImVec2(cx + 4.5f * u, cy + 4.5f * u),
                          color, 1.5f);
        break;
    }
    case LEG_ICON_STEP: {
        ImVec2 a(cx - 5 * u, cy - 5 * u);
        ImVec2 b(cx - 5 * u, cy + 5 * u);
        ImVec2 c(cx + 1 * u, cy);
        dl->AddTriangleFilled(a, b, c, color);
        dl->AddRectFilled(ImVec2(cx + 2.5f * u, cy - 5 * u),
                          ImVec2(cx + 5 * u, cy + 5 * u), color, 1.0f);
        break;
    }
    case LEG_ICON_PLUS: {
        float t = 1.8f * u;
        dl->AddRectFilled(ImVec2(cx - t * 0.5f, cy - 5 * u),
                          ImVec2(cx + t * 0.5f, cy + 5 * u), color,
                          1.0f);
        dl->AddRectFilled(ImVec2(cx - 5 * u, cy - t * 0.5f),
                          ImVec2(cx + 5 * u, cy + t * 0.5f), color,
                          1.0f);
        break;
    }
    case LEG_ICON_BOX: {
        /* Isometric-ish cube: front square + top/right hints. */
        ImVec2 f0(cx - 4.5f * u, cy - 2.5f * u);
        ImVec2 f1(cx + 1.5f * u, cy + 4.5f * u);
        dl->AddRect(f0, f1, color, 1.0f, 0, 1.5f * u);
        dl->AddLine(ImVec2(f0.x + 2 * u, f0.y - 2 * u),
                    ImVec2(f1.x + 2 * u, f1.y - 7 * u), color,
                    1.5f * u);
        dl->AddLine(ImVec2(f1.x, f1.y - 7 * u),
                    ImVec2(f1.x + 2 * u, f1.y - 7 * u + 2 * u),
                    color, 1.5f * u);
        break;
    }
    case LEG_ICON_FOLDER: {
        dl->AddRectFilled(ImVec2(cx - 6 * u, cy - 2 * u),
                          ImVec2(cx + 6 * u, cy + 5 * u), color, 1.5f);
        dl->AddRectFilled(ImVec2(cx - 6 * u, cy - 5 * u),
                          ImVec2(cx - 1 * u, cy - 1 * u), color, 1.5f);
        break;
    }
    case LEG_ICON_SCENE: {
        /* Landscape frame: mountains + sun. */
        dl->AddRect(ImVec2(cx - 5.5f * u, cy - 4 * u),
                    ImVec2(cx + 5.5f * u, cy + 4.5f * u), color, 1.0f,
                    0, 1.4f * u);
        dl->AddCircleFilled(ImVec2(cx + 2.5f * u, cy - 1.5f * u),
                            1.4f * u, color);
        dl->AddLine(ImVec2(cx - 5.5f * u, cy + 4.5f * u),
                    ImVec2(cx - 1 * u, cy), color, 1.4f * u);
        dl->AddLine(ImVec2(cx - 1 * u, cy),
                    ImVec2(cx + 5.5f * u, cy + 4.5f * u), color,
                    1.4f * u);
        break;
    }
    case LEG_ICON_MESH: {
        /* Wireframe triangle. */
        dl->AddTriangle(ImVec2(cx, cy - 5 * u),
                        ImVec2(cx - 5 * u, cy + 4 * u),
                        ImVec2(cx + 5 * u, cy + 4 * u), color,
                        1.5f * u);
        break;
    }
    case LEG_ICON_MATERIAL: {
        /* Half-filled sphere. */
        dl->AddCircle(ImVec2(cx, cy), 5 * u, color, 20, 1.5f * u);
        dl->AddCircleFilled(ImVec2(cx, cy), 5 * u, color & 0x55FFFFFFu);
        dl->AddCircleFilled(ImVec2(cx - 1.2f * u, cy - 1.2f * u),
                            1.4f * u,
                            ImGui::GetColorU32(ImGuiCol_WindowBg));
        break;
    }
    case LEG_ICON_TEXTURE: {
        /* Checker 2x2. */
        float q = 2.6f * u;
        dl->AddRectFilled(ImVec2(cx - q, cy - q), ImVec2(cx, cy),
                          color, 1.0f);
        dl->AddRectFilled(ImVec2(cx, cy), ImVec2(cx + q, cy + q),
                          color, 1.0f);
        dl->AddRect(ImVec2(cx - q, cy - q), ImVec2(cx + q, cy + q),
                    color, 1.0f, 0, 1.2f * u);
        break;
    }
    case LEG_ICON_SCRIPT: {
        /* Document + code lines. */
        dl->AddRect(ImVec2(cx - 4 * u, cy - 5.5f * u),
                    ImVec2(cx + 4 * u, cy + 5.5f * u), color, 1.0f,
                    0, 1.4f * u);
        dl->AddLine(ImVec2(cx - 2.2f * u, cy - 2 * u),
                    ImVec2(cx + 2.2f * u, cy - 2 * u), color,
                    1.3f * u);
        dl->AddLine(ImVec2(cx - 2.2f * u, cy + 0.5f * u),
                    ImVec2(cx + 0.5f * u, cy + 0.5f * u), color,
                    1.3f * u);
        dl->AddLine(ImVec2(cx - 2.2f * u, cy + 3 * u),
                    ImVec2(cx + 2.2f * u, cy + 3 * u), color,
                    1.3f * u);
        break;
    }
    case LEG_ICON_PREFAB: {
        /* Three stacked squares (instances). */
        float t = 1.4f * u;
        dl->AddRect(ImVec2(cx - 5 * u, cy - 4 * u),
                    ImVec2(cx - 1 * u, cy), color, 1.0f, 0, t);
        dl->AddRect(ImVec2(cx - 2.5f * u, cy - 1.5f * u),
                    ImVec2(cx + 1.5f * u, cy + 2.5f * u), color, 1.0f,
                    0, t);
        dl->AddRect(ImVec2(cx, cy + 1 * u), ImVec2(cx + 5 * u, cy + 5 * u),
                    color, 1.0f, 0, t);
        break;
    }
    case LEG_ICON_CAMERA: {
        dl->AddRect(ImVec2(cx - 5.5f * u, cy - 3.5f * u),
                    ImVec2(cx + 5.5f * u, cy + 3.5f * u), color, 1.5f,
                    0, 1.4f * u);
        dl->AddCircle(ImVec2(cx, cy), 2 * u, color, 16, 1.4f * u);
        dl->AddTriangle(ImVec2(cx - 5.5f * u, cy - 3.5f * u),
                        ImVec2(cx - 8 * u, cy - 5 * u),
                        ImVec2(cx - 8 * u, cy - 1 * u), color,
                        1.4f * u);
        break;
    }
    case LEG_ICON_LIGHT: {
        /* Bulb: circle + rays. */
        dl->AddCircle(ImVec2(cx, cy - 1 * u), 3 * u, color, 16,
                      1.5f * u);
        dl->AddLine(ImVec2(cx - 2 * u, cy + 3.5f * u),
                    ImVec2(cx + 2 * u, cy + 3.5f * u), color,
                    1.5f * u);
        dl->AddLine(ImVec2(cx, cy - 6 * u), ImVec2(cx, cy - 4.6f * u),
                    color, 1.4f * u);
        dl->AddLine(ImVec2(cx - 5 * u, cy - 4 * u),
                    ImVec2(cx - 4 * u, cy - 2.8f * u), color,
                    1.4f * u);
        dl->AddLine(ImVec2(cx + 5 * u, cy - 4 * u),
                    ImVec2(cx + 4 * u, cy - 2.8f * u), color,
                    1.4f * u);
        break;
    }
    case LEG_ICON_TRANSLATE: {
        /* Move arrows (4-way). */
        dl->AddLine(ImVec2(cx - 5 * u, cy), ImVec2(cx + 5 * u, cy),
                    color, 1.6f * u);
        dl->AddLine(ImVec2(cx, cy - 5 * u), ImVec2(cx, cy + 5 * u),
                    color, 1.6f * u);
        dl->AddTriangleFilled(
            ImVec2(cx + 5 * u, cy - 2 * u),
            ImVec2(cx + 5 * u, cy + 2 * u), ImVec2(cx + 7 * u, cy),
            color);
        dl->AddTriangleFilled(
            ImVec2(cx - 5 * u, cy - 2 * u),
            ImVec2(cx - 5 * u, cy + 2 * u), ImVec2(cx - 7 * u, cy),
            color);
        break;
    }
    case LEG_ICON_ROTATE: {
        /* Circular arrow. */
        dl->PathArcTo(ImVec2(cx, cy), 4.5f * u, 0.6f, 5.4f, 20);
        dl->PathStroke(color, 0, 1.6f * u);
        {
            ImVec2 tip(cx + 4.5f * u * cosf(0.6f),
                       cy + 4.5f * u * sinf(0.6f));
            dl->AddTriangleFilled(
                ImVec2(tip.x - 2.4f * u, tip.y - 0.6f * u),
                ImVec2(tip.x + 0.4f * u, tip.y - 2.2f * u),
                ImVec2(tip.x + 1.2f * u, tip.y + 1.8f * u), color);
        }
        break;
    }
    case LEG_ICON_SCALE: {
        /* Expand arrows (corners out). */
        dl->AddRect(ImVec2(cx - 2 * u, cy - 2 * u),
                    ImVec2(cx + 2 * u, cy + 2 * u), color, 1.0f, 0,
                    1.3f * u);
        dl->AddLine(ImVec2(cx - 2 * u, cy - 2 * u),
                    ImVec2(cx - 5 * u, cy - 5 * u), color, 1.5f * u);
        dl->AddLine(ImVec2(cx + 2 * u, cy + 2 * u),
                    ImVec2(cx + 5 * u, cy + 5 * u), color, 1.5f * u);
        dl->AddLine(ImVec2(cx + 2 * u, cy - 2 * u),
                    ImVec2(cx + 5 * u, cy - 5 * u), color, 1.5f * u);
        dl->AddLine(ImVec2(cx - 2 * u, cy + 2 * u),
                    ImVec2(cx - 5 * u, cy + 5 * u), color, 1.5f * u);
        break;
    }
    case LEG_ICON_SEARCH: {
        dl->AddCircle(ImVec2(cx - 1 * u, cy - 1 * u), 3.6f * u, color,
                      18, 1.6f * u);
        dl->AddLine(ImVec2(cx + 1.6f * u, cy + 1.6f * u),
                    ImVec2(cx + 5 * u, cy + 5 * u), color, 1.8f * u);
        break;
    }
    case LEG_ICON_REFRESH: {
        dl->PathArcTo(ImVec2(cx, cy), 4.5f * u, -0.4f, 4.4f, 20);
        dl->PathStroke(color, 0, 1.6f * u);
        {
            ImVec2 tip(cx + 4.5f * u * cosf(4.4f),
                       cy + 4.5f * u * sinf(4.4f));
            dl->AddTriangleFilled(
                ImVec2(tip.x - 2.4f * u, tip.y + 0.4f * u),
                ImVec2(tip.x + 0.6f * u, tip.y + 2.2f * u),
                ImVec2(tip.x + 1.0f * u, tip.y - 1.8f * u), color);
        }
        break;
    }
    case LEG_ICON_SAVE: {
        /* Floppy: body + label + notch. */
        dl->AddRect(ImVec2(cx - 5 * u, cy - 5.5f * u),
                    ImVec2(cx + 5 * u, cy + 5.5f * u), color, 1.0f,
                    0, 1.4f * u);
        dl->AddRectFilled(ImVec2(cx - 2.5f * u, cy - 5.5f * u),
                          ImVec2(cx + 2.5f * u, cy - 1 * u), color);
        dl->AddRectFilled(ImVec2(cx - 2.5f * u, cy + 1.5f * u),
                          ImVec2(cx + 2.5f * u, cy + 5.5f * u),
                          color & 0x55FFFFFFu);
        break;
    }
    case LEG_ICON_SETTINGS: {
        /* Gear-ish: circle + spokes. */
        int k = 0;
        dl->AddCircle(ImVec2(cx, cy), 2.4f * u, color, 18, 1.6f * u);
        for (k = 0; k < 6; k++) {
            float a = (float)k * 3.14159265f / 3.0f;
            float dx = cosf(a);
            float dy = sinf(a);
            dl->AddLine(
                ImVec2(cx + dx * 3.4f * u, cy + dy * 3.4f * u),
                ImVec2(cx + dx * 5.4f * u, cy + dy * 5.4f * u),
                color, 1.6f * u);
        }
        break;
    }
    case LEG_ICON_OBJECT:
    default: {
        /* Generic object: rounded square node. */
        dl->AddRect(ImVec2(cx - 4 * u, cy - 4 * u),
                    ImVec2(cx + 4 * u, cy + 4 * u), color, 2.0f, 0,
                    1.5f * u);
        break;
    }
    }
}

/* Cursor-advancing wrapper for row layouts (hierarchy/asset rows):
 * reserves the square, then paints at it. Do NOT use inside custom
 * buttons (use leg_icon_draw_at — a Dummy there disturbs SameLine
 * layout and slides following widgets down). */
void leg_icon_draw(leg_icon_kind kind, float size, ImU32 color) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = (size > 0.0f) ? size : 14.0f;

    ImGui::Dummy(ImVec2(s, s));
    leg_icon_draw_at(kind, p, s, color);
}

/* Map an asset record type (+failed flag) to an icon. */
leg_icon_kind leg_icon_for_asset(int asset_type, int failed) {
    (void)failed;
    switch (asset_type) {
    case LED_PROJECT_ASSET_MODEL: return LEG_ICON_BOX;
    case LED_PROJECT_ASSET_TEXTURE: return LEG_ICON_TEXTURE;
    case LED_PROJECT_ASSET_SCRIPT: return LEG_ICON_SCRIPT;
    case LED_PROJECT_ASSET_SCENE: return LEG_ICON_SCENE;
    case LED_PROJECT_ASSET_PREFAB: return LEG_ICON_PREFAB;
    case LED_PROJECT_ASSET_MESH: return LEG_ICON_MESH;
    case LED_PROJECT_ASSET_MATERIAL: return LEG_ICON_MATERIAL;
    case LED_PROJECT_ASSET_SKELETON:
    case LED_PROJECT_ASSET_CLIP:
    default: return LEG_ICON_OBJECT;
    }
}

int leg_tool_button(const char *id, leg_icon_kind kind,
                    const char *tooltip, int active, float size) {
    ImVec2 sz = ImVec2(size, size);
    int pressed = 0;
    ImVec2 rmin;
    ImVec2 rmax;

    if (active) {
        ImGui::PushStyleColor(
            ImGuiCol_Button,
            ImVec4(0.95f, 0.62f, 0.18f, 0.30f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonHovered,
            ImVec4(0.95f, 0.62f, 0.18f, 0.45f));
        ImGui::PushStyleColor(
            ImGuiCol_ButtonActive,
            ImVec4(0.95f, 0.62f, 0.18f, 0.60f));
    }
    pressed = ImGui::Button(id, sz) ? 1 : 0;
    if (active) {
        ImGui::PopStyleColor(3);
    }
    rmin = ImGui::GetItemRectMin();
    rmax = ImGui::GetItemRectMax();
    {
        /* Center the glyph in the button rect with PURE draw-list
         * calls (no cursor movement, no Dummy, no clip-stack play:
         * any layout participation here slides following SameLine
         * widgets — verified headed). The Button stays the last
         * item, so hover/tooltip naturally attach to it. */
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float s = (size > 0.0f ? size : 28.0f) - 12.0f;
        ImU32 col =
            active
                ? ImGui::GetColorU32(
                      ImVec4(1.0f, 0.78f, 0.42f, 1.0f))
                : ImGui::GetColorU32(ImGuiCol_Text);
        ImVec2 origin =
            ImVec2((rmin.x + rmax.x) * 0.5f - s * 0.5f,
                   (rmin.y + rmax.y) * 0.5f - s * 0.5f);

        (void)dl;
        ImGui::PushClipRect(rmin, rmax, true);
        leg_icon_draw_at(kind, origin, s, col);
        ImGui::PopClipRect();
    }
    if (tooltip != NULL && tooltip[0] != '\0' &&
        ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", tooltip);
    }
    return pressed;
}
