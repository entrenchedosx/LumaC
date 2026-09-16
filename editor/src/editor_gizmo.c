/* Gizmos: math-only drag intents applied through coalesced
 * undoable commands. Preview line soup (selection AABB edges + axis
 * rays) for a future Phase-32 overlay pass. No rendering here. */

#include <math.h>
#include <string.h>

#include "internal/editor_internal.h"

int led_gizmo_begin(led_session *session, led_gizmo_mode mode,
                    int axis) {
    le_object sel = LE_OBJECT_INVALID;
    uint32_t n;

    if (session == NULL) {
        return 0;
    }
    if (!led_is_attached(session)) {
        return 0;
    }
    if (mode < LED_GIZMO_TRANSLATE || mode > LED_GIZMO_SCALE) {
        return 0;
    }
    if (axis < 0 || axis > 3) {
        return 0;
    }
    n = led_selection_get(session, &sel, 1);
    if (n != 1) {
        return 0;
    }
    /* Exactly one live object required. */
    if (led_selection_get(session, NULL, 0) != 1) {
        return 0;
    }
    session->gizmo_active = 1;
    session->gizmo_mode = mode;
    session->gizmo_axis = axis;
    session->gizmo_target = sel;
    return 1;
}

led_result led_gizmo_apply(led_session *session,
                           const led_gizmo_drag *drag) {
    led_command cmd;
    float delta[3];

    if (session == NULL || drag == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!session->gizmo_active) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!le_object_is_alive(session->edit_world,
                            &session->gizmo_target)) {
        session->gizmo_active = 0;
        return LED_ERROR_STALE_HANDLE;
    }
    if (drag->mode != session->gizmo_mode) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    delta[0] = drag->current_world[0] - drag->start_world[0];
    delta[1] = drag->current_world[1] - drag->start_world[1];
    delta[2] = drag->current_world[2] - drag->start_world[2];
    /* Snap (translate meters / rotate degrees / scale fraction). */
    if (drag->snap > 0.0f) {
        int a;

        for (a = 0; a < 3; a++) {
            delta[a] =
                roundf(delta[a] / drag->snap) * drag->snap;
        }
    }
    /* Axis constraint (3 = free). */
    if (drag->axis >= 0 && drag->axis <= 2) {
        int a;

        for (a = 0; a < 3; a++) {
            if (a != drag->axis) {
                delta[a] = 0.0f;
            }
        }
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.target = session->gizmo_target;
    if (drag->mode == LED_GIZMO_TRANSLATE) {
        float p[3];

        le_object_get_position(session->edit_world,
                               &session->gizmo_target, p);
        cmd.kind = LED_CMD_SET_POSITION;
        strncpy(cmd.label, "Gizmo translate", sizeof(cmd.label) - 1);
        cmd.vec_value[0] = p[0] + delta[0];
        cmd.vec_value[1] = p[1] + delta[1];
        cmd.vec_value[2] = p[2] + delta[2];
        cmd.vec_value[3] = 0.0f;
    } else if (drag->mode == LED_GIZMO_SCALE) {
        float s[3];

        le_object_get_scale(session->edit_world,
                            &session->gizmo_target, s);
        cmd.kind = LED_CMD_SET_SCALE;
        strncpy(cmd.label, "Gizmo scale", sizeof(cmd.label) - 1);
        cmd.vec_value[0] = s[0] + delta[0] * 0.1f;
        cmd.vec_value[1] = s[1] + delta[1] * 0.1f;
        cmd.vec_value[2] = s[2] + delta[2] * 0.1f;
        cmd.vec_value[3] = 0.0f;
    } else {
        /* Rotate: delta maps to degrees about the axis (free =
         * yaw). Applied as axis-angle premultiply. */
        float q[4];
        float axis[3] = { 0.0f, 1.0f, 0.0f };
        float angle;
        float half;
        float dq[4];
        float out[4];

        if (drag->axis >= 0 && drag->axis <= 2) {
            axis[0] = axis[1] = axis[2] = 0.0f;
            axis[drag->axis] = 1.0f;
            angle = (drag->axis == 0)
                        ? delta[0]
                        : ((drag->axis == 1) ? delta[1] : delta[2]);
        } else {
            angle = delta[0] + delta[1];
        }
        angle *= 3.14159265358979323846f / 180.0f;
        half = angle * 0.5f;
        dq[0] = axis[0] * sinf(half);
        dq[1] = axis[1] * sinf(half);
        dq[2] = axis[2] * sinf(half);
        dq[3] = cosf(half);
        le_object_get_rotation(session->edit_world,
                               &session->gizmo_target, q);
        /* out = dq * q (Hamilton). */
        out[0] = dq[3] * q[0] + dq[0] * q[3] + dq[1] * q[2] -
                 dq[2] * q[1];
        out[1] = dq[3] * q[1] - dq[0] * q[2] + dq[1] * q[3] +
                 dq[2] * q[0];
        out[2] = dq[3] * q[2] + dq[0] * q[1] - dq[1] * q[0] +
                 dq[2] * q[3];
        out[3] = dq[3] * q[3] - dq[0] * q[0] - dq[1] * q[1] -
                 dq[2] * q[2];
        cmd.kind = LED_CMD_SET_ROTATION;
        strncpy(cmd.label, "Gizmo rotate", sizeof(cmd.label) - 1);
        memcpy(cmd.vec_value, out, sizeof(out));
    }
    return led_execute(session, &cmd);
}

uint32_t led_gizmo_lines(led_session *session,
                         const led_viewport *viewport,
                         float *out_xyz, uint32_t float_cap) {
    /* Preview: selection AABB 12 edges (72 floats) + 3 axis rays
     * from the AABB center (18 floats) = 90 floats total. */
    float mn[3];
    float mx[3];
    float c[8][3];
    uint32_t need = 90;
    int i;

    (void)viewport;
    if (session == NULL || !led_is_attached(session)) {
        return 0;
    }
    if (!session->gizmo_active) {
        return 0;
    }
    if (!led_selection_aabb(session, mn, mx)) {
        return 0;
    }
    if (out_xyz == NULL || float_cap == 0) {
        return need;
    }
    for (i = 0; i < 8; i++) {
        c[i][0] = (i & 1) ? mx[0] : mn[0];
        c[i][1] = (i & 2) ? mx[1] : mn[1];
        c[i][2] = (i & 4) ? mx[2] : mn[2];
    }
    {
        /* 12 box edges as corner index pairs. */
        static const int kEdges[12][2] = {
            { 0, 1 }, { 2, 3 }, { 4, 5 }, { 6, 7 }, { 0, 2 },
            { 1, 3 }, { 4, 6 }, { 5, 7 }, { 0, 4 }, { 1, 5 },
            { 2, 6 }, { 3, 7 }
        };
        uint32_t w = 0;
        int e;
        int a;

        for (e = 0; e < 12 && w + 6 <= float_cap; e++) {
            for (a = 0; a < 3; a++) {
                out_xyz[w++] = c[kEdges[e][0]][a];
            }
            for (a = 0; a < 3; a++) {
                out_xyz[w++] = c[kEdges[e][1]][a];
            }
        }
        /* 3 axis rays from the center (length = max extent). */
        {
            float ctr[3] = { (mn[0] + mx[0]) * 0.5f,
                             (mn[1] + mx[1]) * 0.5f,
                             (mn[2] + mx[2]) * 0.5f };
            float len = mx[0] - mn[0];

            if (mx[1] - mn[1] > len) {
                len = mx[1] - mn[1];
            }
            if (mx[2] - mn[2] > len) {
                len = mx[2] - mn[2];
            }
            len = len * 0.5f + 1.0f;
            for (a = 0; a < 3 && w + 6 <= float_cap; a++) {
                float tip[3];

                memcpy(tip, ctr, sizeof(tip));
                tip[a] += len;
                out_xyz[w++] = ctr[0];
                out_xyz[w++] = ctr[1];
                out_xyz[w++] = ctr[2];
                out_xyz[w++] = tip[0];
                out_xyz[w++] = tip[1];
                out_xyz[w++] = tip[2];
            }
        }
    }
    return need;
}
