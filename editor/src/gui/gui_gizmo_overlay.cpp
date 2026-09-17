/* Phase 33 GUI gizmo overlay (isolated C++ over led_* math).
 *
 * Screen-space Translate/Rotate/Scale handles drawn over the viewport
 * image; drags map through led_viewport_ray planes into
 * led_gizmo_begin/led_gizmo_apply (ONE coalesced undoable command per
 * drag tick — the core merges consecutive same-target same-kind TRS
 * commands, so a drag is ONE history entry; Escape cancels with no
 * history entry). Camera owns its input while orbiting; gizmo
 * overlays never submit scene geometry (ImGui draw list only).
 *
 * Handle model (screen-space, pure function of the capture rect +
 * selection AABB center projected through the viewport camera):
 * - Translate: 3 axis arrows (X red, Y green, Z blue) + center cube
 *   (free-plane XYZ). Hit = nearest handle within 10px.
 * - Rotate: 3 axis rings drawn as circles + free yaw. Hit = nearest
 *   ring within 8px of its radius.
 * - Scale: 3 axis ticks (shorter arrows, square tips) + center cube
 *   (uniform XYZ). Same picking as translate.
 * Drag plane: axis drags use the plane containing the axis most
 * facing the camera (axis x view-dir cross products); free-plane
 * drags use the camera-facing plane through the anchor. World delta
 * = current-plane-point minus start-plane-point; snap applies in
 * led_gizmo_apply (translate meters / rotate degrees / scale frac).
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gui_internal.h"

#ifndef LEG_PI
#define LEG_PI 3.14159265358979323846f
#endif

static void leg_v3_sub(const float a[3], const float b[3],
                       float out[3]) {
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static void leg_v3_add(const float a[3], const float b[3],
                       float out[3]) {
    out[0] = a[0] + b[0];
    out[1] = a[1] + b[1];
    out[2] = a[2] + b[2];
}

static void leg_v3_scale(const float a[3], float s, float out[3]) {
    out[0] = a[0] * s;
    out[1] = a[1] * s;
    out[2] = a[2] * s;
}

static float leg_v3_dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void leg_v3_cross(const float a[3], const float b[3],
                         float out[3]) {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
}

static float leg_v3_len(const float a[3]) {
    return sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
}

static void leg_v3_norm(const float a[3], float out[3]) {
    float n = leg_v3_len(a);

    if (n > 1e-12f && n == n) {
        out[0] = a[0] / n;
        out[1] = a[1] / n;
        out[2] = a[2] / n;
    } else {
        out[0] = 1.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
    }
}

/* Project a world point to viewport-panel px (origin top-left).
 * Returns 1 + px when in front of the camera, 0 when behind. */
static int leg_world_to_panel(const led_viewport *vp,
                              const float world[3],
                              const ImVec2 *origin,
                              const ImVec2 *size, float out_px[2]) {
    lr_camera cam;
    float rel[3];
    float view[3];
    float clip[4];
    float w = 0.0f;

    if (vp == NULL || world == NULL || origin == NULL ||
        size == NULL || out_px == NULL) {
        return 0;
    }
    if (size->x <= 0 || size->y <= 0) {
        return 0;
    }
    memset(&cam, 0, sizeof(cam));
    if (!led_viewport_camera(vp, &cam)) {
        return 0;
    }
    /* Column-major view: view = V * (world,1); clip = P * view. */
    rel[0] = world[0] - cam.position[0];
    rel[1] = world[1] - cam.position[1];
    rel[2] = world[2] - cam.position[2];
    view[0] = cam.view[0] * rel[0] + cam.view[4] * rel[1] +
              cam.view[8] * rel[2];
    view[1] = cam.view[1] * rel[0] + cam.view[5] * rel[1] +
              cam.view[9] * rel[2];
    view[2] = cam.view[2] * rel[0] + cam.view[6] * rel[1] +
              cam.view[10] * rel[2];
    w = -(cam.view[3] * rel[0] + cam.view[7] * rel[1] +
          cam.view[11] * rel[2]);
    /* (affine view: translation row unused; w = -view.z sign.) */
    w = -view[2];
    if (!(w > 1e-9f)) {
        return 0; /* behind / on the near plane */
    }
    clip[0] = cam.projection[0] * view[0] +
              cam.projection[4] * view[1] +
              cam.projection[8] * view[2] +
              cam.projection[12] * w;
    clip[1] = cam.projection[1] * view[0] +
              cam.projection[5] * view[1] +
              cam.projection[9] * view[2] +
              cam.projection[13] * w;
    out_px[0] = origin->x + (clip[0] / w * 0.5f + 0.5f) * size->x;
    out_px[1] = origin->y + (0.5f - clip[1] / w * 0.5f) * size->y;
    (void)clip[2];
    return 1;
}

/* Ray-plane intersect (1 + point, 0 when parallel/miss). */
static int leg_ray_plane(const float ro[3], const float rd[3],
                         const float n[3], float d, float out[3]) {
    float denom = leg_v3_dot(rd, n);
    float t = 0.0f;

    if (fabsf(denom) < 1e-9f) {
        return 0;
    }
    t = (d - leg_v3_dot(ro, n)) / denom;
    if (!(t > 0.0f)) {
        return 0;
    }
    out[0] = ro[0] + rd[0] * t;
    out[1] = ro[1] + rd[1] * t;
    out[2] = ro[2] + rd[2] * t;
    return 1;
}

/* Gizmo anchor: selection AABB center (falls back to the viewport
 * target when nothing frames). Returns 1 + center + extent. */
static int leg_gizmo_anchor(leg_context *ctx, float center[3],
                            float *extent) {
    float mn[3];
    float mx[3];

    if (ctx == NULL || center == NULL) {
        return 0;
    }
    if (led_selection_aabb(ctx->session, mn, mx)) {
        center[0] = (mn[0] + mx[0]) * 0.5f;
        center[1] = (mn[1] + mx[1]) * 0.5f;
        center[2] = (mn[2] + mx[2]) * 0.5f;
        if (extent != NULL) {
            float e = mx[0] - mn[0];

            if (mx[1] - mn[1] > e) {
                e = mx[1] - mn[1];
            }
            if (mx[2] - mn[2] > e) {
                e = mx[2] - mn[2];
            }
            *extent = e * 0.5f + 0.5f;
        }
        return 1;
    }
    return 0;
}

static ImU32 leg_axis_color(int axis, int active) {
    /* X red, Y green, Z blue (Godot/Unity convention); active =
     * full-bright, idle = dimmer. */
    if (axis == 0) {
        return active ? IM_COL32(255, 90, 90, 255)
                      : IM_COL32(170, 60, 60, 255);
    }
    if (axis == 1) {
        return active ? IM_COL32(120, 255, 120, 255)
                      : IM_COL32(70, 170, 70, 255);
    }
    return active ? IM_COL32(120, 160, 255, 255)
                  : IM_COL32(70, 90, 170, 255);
}

/* Pick the best handle under the mouse (screen px). Returns axis
 * 0..2, 3 = center/free, -1 = none. */
static int leg_gizmo_pick(leg_context *ctx, led_viewport *vp,
                          const ImVec2 *origin, const ImVec2 *size,
                          int mode, const float center[3],
                          float handle_len, const ImVec2 *mouse) {
    float cpx[2];
    int axis = 0;
    int best = -1;
    float best_d = 10.0f; /* 10px grab radius */

    (void)ctx;
    if (vp == NULL || origin == NULL || size == NULL ||
        mouse == NULL) {
        return -1;
    }
    if (!leg_world_to_panel(vp, center, origin, size, cpx)) {
        return -1;
    }
    if (mode == LED_GIZMO_ROTATE) {
        /* Rings: distance-to-radius test (radius = handle_len in
         * world units projected ~ conservatively to px). */
        float ring_px = handle_len / ((vp->distance > 0)
                                          ? vp->distance
                                          : 1.0f) *
                        size->y;
        float dx = mouse->x - cpx[0];
        float dy = mouse->y - cpx[1];
        float r = sqrtf(dx * dx + dy * dy);

        if (fabsf(r - ring_px) < 8.0f) {
            /* Nearest ring by screen angle thirds (stable,
             * documented approximation — exact ellipse picking
             * is future work). */
            float ang = atan2f(dy, dx) * 180.0f / LEG_PI;

            if (ang < 0) {
                ang += 360.0f;
            }
            return (ang < 120.0f) ? 0 : ((ang < 240.0f) ? 1 : 2);
        }
        if (r < 12.0f) {
            return 3;
        }
        return -1;
    }
    /* Translate/scale arrows: distance point-to-segment in px. */
    for (axis = 0; axis < 3; axis++) {
        float tip[3];
        float tpx[2];
        float ax[3] = { 0, 0, 0 };
        float abx = 0.0f;
        float aby = 0.0f;
        float amx = 0.0f;
        float amy = 0.0f;
        float t = 0.0f;
        float dx = 0.0f;
        float dy = 0.0f;
        float d = 0.0f;

        ax[axis] = 1.0f;
        tip[0] = center[0] + ax[0] * handle_len;
        tip[1] = center[1] + ax[1] * handle_len;
        tip[2] = center[2] + ax[2] * handle_len;
        if (!leg_world_to_panel(vp, tip, origin, size, tpx)) {
            continue;
        }
        abx = tpx[0] - cpx[0];
        aby = tpx[1] - cpx[1];
        amx = mouse->x - cpx[0];
        amy = mouse->y - cpx[1];
        t = (abx * abx + aby * aby > 1e-9f)
                ? (amx * abx + amy * aby) / (abx * abx + aby * aby)
                : 0.0f;
        if (t < 0) {
            t = 0;
        }
        if (t > 1) {
            t = 1;
        }
        dx = amx - abx * t;
        dy = amy - aby * t;
        d = sqrtf(dx * dx + dy * dy);
        if (d < best_d) {
            best_d = d;
            best = axis;
        }
    }
    /* Center cube: within 8px of the anchor. */
    {
        float dx = mouse->x - cpx[0];
        float dy = mouse->y - cpx[1];

        if (sqrtf(dx * dx + dy * dy) < 8.0f) {
            return 3;
        }
    }
    return best;
}

/* Draw the handles into the window draw list (ImGui primitives —
 * never scene geometry). */
static void leg_gizmo_draw(leg_context *ctx, led_viewport *vp,
                           const ImVec2 *origin, const ImVec2 *size,
                           int mode, int hot_axis,
                           const float center[3],
                           float handle_len) {
    float cpx[2];
    int axis = 0;

    (void)ctx;
    if (vp == NULL || origin == NULL || size == NULL) {
        return;
    }
    if (!leg_world_to_panel(vp, center, origin, size, cpx)) {
        return;
    }
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 c = ImVec2(cpx[0], cpx[1]);

        if (mode == LED_GIZMO_ROTATE) {
            float ring_px = handle_len /
                            ((vp->distance > 0) ? vp->distance
                                                : 1.0f) *
                            size->y;

            for (axis = 0; axis < 3; axis++) {
                dl->AddCircle(c, ring_px,
                              leg_axis_color(axis,
                                             axis == hot_axis),
                              48, 2.0f);
            }
            dl->AddCircleFilled(c, 4.0f,
                                hot_axis == 3
                                    ? IM_COL32(255, 255, 120, 255)
                                    : IM_COL32(200, 200, 200, 255),
                                16);
            return;
        }
        for (axis = 0; axis < 3; axis++) {
            float tip[3];
            float tpx[2];
            float ax[3] = { 0, 0, 0 };

            ax[axis] = 1.0f;
            tip[0] = center[0] + ax[0] * handle_len;
            tip[1] = center[1] + ax[1] * handle_len;
            tip[2] = center[2] + ax[2] * handle_len;
            if (!leg_world_to_panel(vp, tip, origin, size, tpx)) {
                continue;
            }
            {
                ImVec2 t = ImVec2(tpx[0], tpx[1]);
                ImU32 col =
                    leg_axis_color(axis, axis == hot_axis);

                dl->AddLine(c, t, col, 2.0f);
                if (mode == LED_GIZMO_SCALE) {
                    /* Square tip for scale. */
                    ImVec2 d = ImVec2(t.x - c.x, t.y - c.y);
                    float n = sqrtf(d.x * d.x + d.y * d.y);

                    if (n > 1e-6f) {
                        ImVec2 u = ImVec2(d.x / n * 5.0f,
                                          d.y / n * 5.0f);
                        ImVec2 p = ImVec2(-u.y, u.x);

                        dl->AddQuadFilled(
                            ImVec2(t.x + u.x + p.x,
                                   t.y + u.y + p.y),
                            ImVec2(t.x + u.x - p.x,
                                   t.y + u.y - p.y),
                            ImVec2(t.x - u.x - p.x,
                                   t.y - u.y - p.y),
                            ImVec2(t.x - u.x + p.x,
                                   t.y - u.y + p.y),
                            col);
                    }
                } else {
                    dl->AddCircleFilled(t, 5.0f, col, 12);
                }
            }
        }
        dl->AddRectFilled(ImVec2(c.x - 4, c.y - 4),
                          ImVec2(c.x + 4, c.y + 4),
                          hot_axis == 3
                              ? IM_COL32(255, 255, 120, 255)
                              : IM_COL32(200, 200, 200, 255));
    }
}

int leg_gizmo_overlay(leg_context *ctx, leg_ui *ui,
                      const ImVec2 *origin, const ImVec2 *size) {
    led_viewport *vp = NULL;
    float center[3];
    float extent = 1.0f;
    float handle_len = 1.0f;
    int mode = LED_GIZMO_TRANSLATE;
    ImVec2 mouse;
    int hot = -1;

    if (ctx == NULL || ui == NULL || origin == NULL ||
        size == NULL) {
        return 0;
    }
    if (ctx->session == NULL) {
        return 0;
    }
    /* Viewport comes from the panels host (set per frame). */
    {
        /* Reach the host through the panels TU accessor. */
        extern led_viewport *leg_viewport_host_viewport(void);
        vp = leg_viewport_host_viewport();
    }
    if (vp == NULL) {
        ui->gizmo_active = 0;
        return 0;
    }
    mode = ui->gizmo_mode;
    if (mode < LED_GIZMO_TRANSLATE || mode > LED_GIZMO_SCALE) {
        mode = LED_GIZMO_TRANSLATE;
    }
    if (!leg_gizmo_anchor(ctx, center, &extent)) {
        /* Nothing framed: no handles, no capture. */
        ui->gizmo_active = 0;
        return 0;
    }
    handle_len = extent * 1.5f;
    if (handle_len < 0.5f) {
        handle_len = 0.5f;
    }
    mouse = ImGui::GetMousePos();
    /* Active drag: map mouse -> drag plane -> world delta. */
    if (ui->gizmo_active) {
        float px = mouse.x - origin->x;
        float py = mouse.y - origin->y;
        float ro[3];
        float rd[3];
        float hit[3];
        led_gizmo_drag drag;

        /* Escape cancels (no history entry: end WITHOUT apply —
         * led_gizmo_begin armed nothing until the first apply, so
         * a zero-drag cancel is history-clean). */
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            ui->gizmo_active = 0;
            return 1;
        }
        if (!ImGui::IsMouseDown(0)) {
            /* Release: drag ends (coalesced entry already holds
             * the final value). */
            ui->gizmo_active = 0;
            return 0;
        }
        if (!led_viewport_ray(vp, px, py, ro, rd)) {
            return 1;
        }
        if (!leg_ray_plane(ro, rd, ui->gizmo_plane_n,
                           ui->gizmo_plane_d, hit)) {
            return 1;
        }
        memset(&drag, 0, sizeof(drag));
        drag.mode = (led_gizmo_mode)mode;
        drag.axis = ui->gizmo_axis;
        memcpy(drag.start_world, ui->gizmo_start_world,
               sizeof(drag.start_world));
        memcpy(drag.current_world, hit, sizeof(hit));
        drag.snap = ui->gizmo_snap_on ? ui->gizmo_snap_step : 0.0f;
        if (led_gizmo_apply(ctx->session, &drag) != LED_SUCCESS) {
            leg_status(ctx, "Gizmo apply failed", 1);
            ui->gizmo_active = 0;
            return 0;
        }
        leg_gizmo_draw(ctx, vp, origin, size, mode, ui->gizmo_axis,
                       center, handle_len);
        return 1;
    }
    /* Hover pick (hot highlight, no capture yet). */
    hot = leg_gizmo_pick(ctx, vp, origin, size, mode, center,
                         handle_len, &mouse);
    leg_gizmo_draw(ctx, vp, origin, size, mode, hot, center,
                   handle_len);
    /* Press on a handle: arm the drag (plane + anchor snapshot).
     * led_gizmo_begin validates single-selection; failure means
     * multi/empty selection — handles should not have drawn, but
     * the anchor may come from a stale AABB: re-check liveness. */
    if (hot >= 0 && ImGui::IsMouseClicked(0) &&
        ImGui::IsItemHovered()) {
        float px = mouse.x - origin->x;
        float py = mouse.y - origin->y;
        float ro[3];
        float rd[3];

        if (!led_viewport_ray(vp, px, py, ro, rd)) {
            return 0;
        }
        {
            /* Drag plane: axis drags use the plane spanned by the
             * axis and the most-facing perpendicular; free drags
             * use the camera-facing plane through the anchor. */
            float n[3];
            float ax[3] = { 0, 0, 0 };

            if (hot >= 0 && hot <= 2) {
                float view[3];
                float cand[3];

                ax[hot] = 1.0f;
                leg_v3_sub(ro, center, view);
                leg_v3_norm(view, view);
                /* n = axis x (axis x view): perpendicular to the
                 * axis, most facing the camera. */
                leg_v3_cross(ax, view, cand);
                leg_v3_cross(cand, ax, n);
                if (leg_v3_len(n) < 1e-6f) {
                    /* Axis-on view: fall back to camera plane. */
                    leg_v3_sub(center, ro, n);
                }
                leg_v3_norm(n, n);
            } else {
                leg_v3_sub(center, ro, n);
                leg_v3_norm(n, n);
            }
            ui->gizmo_plane_n[0] = n[0];
            ui->gizmo_plane_n[1] = n[1];
            ui->gizmo_plane_n[2] = n[2];
            ui->gizmo_plane_d = leg_v3_dot(center, n);
            /* Anchor: intersect the ray NOW (drag deltas are
             * relative to this point, not the AABB center). */
            {
                float anchor[3];

                if (!leg_ray_plane(ro, rd, n, ui->gizmo_plane_d,
                                   anchor)) {
                    return 0;
                }
                if (!led_gizmo_begin(ctx->session,
                                     (led_gizmo_mode)mode, hot)) {
                    return 0;
                }
                ui->gizmo_active = 1;
                ui->gizmo_axis = hot;
                ui->gizmo_start_px[0] = px;
                ui->gizmo_start_px[1] = py;
                memcpy(ui->gizmo_start_world, anchor,
                       sizeof(anchor));
                return 1;
            }
        }
    }
    return 0;
}

/* Viewport camera controls (orbit/pan/dolly + WASD fly).
 *
 * - RMB-drag orbits (yaw/pitch), MMB-drag or Shift+RMB pans,
 *   wheel dollies (x0.9/x1.1 per detent).
 * - WASD/QE flies the target in the camera frame when the viewport
 *   is hovered and the GUI does not want the keyboard (speed from
 *   the toolbar Cam% scaled by distance, dt-clamped).
 * - F focuses the selection (led_frame_selection).
 * While orbiting/flying the camera captures input (returns void;
 * the caller skips click-selection via the gizmo return + hover). */
void leg_viewport_camera_update(leg_context *ctx, leg_ui *ui,
                                led_viewport *vp, int hovered,
                                float dt) {
    ImVec2 delta;
    float wheel = 0.0f;

    if (ctx == NULL || ui == NULL || vp == NULL) {
        return;
    }
    if (!hovered) {
        return;
    }
    if (dt != dt || dt <= 0.0f) {
        dt = 1.0f / 60.0f;
    }
    if (dt > 0.25f) {
        dt = 0.25f;
    }
    /* Wheel dolly (ImGui aggregates wheel per frame). */
    {
        const ImGuiIO &io = ImGui::GetIO();

        wheel = io.MouseWheel;
    }
    if (wheel != 0.0f) {
        float f = 1.0f;

        /* One detent = x1.1 out / /1.1 in (fractional wheels
         * compose multiplicatively). */
        if (wheel > 0) {
            int k = 0;

            for (k = 0; k < (int)wheel; k++) {
                f /= 1.1f;
            }
            f /= 1.0f + 0.1f * (wheel - (float)(int)wheel);
        } else {
            int k = 0;

            for (k = 0; k < (int)(-wheel); k++) {
                f *= 1.1f;
            }
            f *= 1.0f + 0.1f * (-wheel - (float)(int)(-wheel));
        }
        led_viewport_dolly(vp, f);
    }
    /* Drag orbit/pan while the capture button is held. */
    delta = ImGui::GetMouseDragDelta(1); /* RMB */
    if (ImGui::IsMouseDown(1) &&
        (delta.x != 0.0f || delta.y != 0.0f)) {
        const ImGuiIO &io = ImGui::GetIO();

        if (io.KeyShift || ImGui::IsMouseDown(2)) {
            led_viewport_pan(vp, -delta.x, delta.y);
        } else {
            led_viewport_orbit(vp, -delta.x * 0.005f,
                               -delta.y * 0.005f);
        }
        ImGui::ResetMouseDragDelta(1);
        return; /* orbiting captures the frame */
    }
    delta = ImGui::GetMouseDragDelta(2); /* MMB pans */
    if (ImGui::IsMouseDown(2) &&
        (delta.x != 0.0f || delta.y != 0.0f)) {
        led_viewport_pan(vp, -delta.x, delta.y);
        ImGui::ResetMouseDragDelta(2);
        return;
    }
    /* WASD/QE fly (viewport hovered + GUI yields keyboard).
     * A W/E/R mode-switch press this frame suppresses flying that
     * same key (the press already did its job as a hotkey). */
    if (!leg_wants_keyboard(ctx)) {
        float speed = vp->distance *
                      (float)ui->camera_speed / 100.0f * dt;
        float fwd[2] = { 0, 0 };
        float strafe = 0.0f;
        float rise = 0.0f;
        int skip = (ui != NULL) ? ui->gizmo_switched_key : 0;

        if (skip != (int)ImGuiKey_W && ImGui::IsKeyDown(ImGuiKey_W)) {
            fwd[0] += 1.0f;
        }
        if (ImGui::IsKeyDown(ImGuiKey_S)) {
            fwd[0] -= 1.0f;
        }
        if (ImGui::IsKeyDown(ImGuiKey_D)) {
            strafe += 1.0f;
        }
        if (ImGui::IsKeyDown(ImGuiKey_A)) {
            strafe -= 1.0f;
        }
        if (skip != (int)ImGuiKey_E && ImGui::IsKeyDown(ImGuiKey_E)) {
            rise += 1.0f;
        }
        if (ImGui::IsKeyDown(ImGuiKey_Q)) {
            rise -= 1.0f;
        }
        /* Forward in the orbit frame (yaw only — fly stays level;
         * pitch-following flight is future work). */
        if (fwd[0] != 0.0f) {
            float cy = cosf(vp->yaw_rad);
            float sy = sinf(vp->yaw_rad);

            vp->target[0] += -sy * fwd[0] * speed * 4.0f;
            vp->target[2] += -cy * fwd[0] * speed * 4.0f;
            (void)cy;
        }
        if (strafe != 0.0f || rise != 0.0f) {
            led_viewport_pan(vp, -strafe * speed * 4.0f,
                             rise * speed * 4.0f);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F, false) &&
            ctx->session != NULL) {
            led_frame_selection(ctx->session, vp);
        }
    }
}
