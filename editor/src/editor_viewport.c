/* Viewport: orbit camera math, unproject (view-proj inverse),
 * physics-raycast picking, AABB composition, frame-selection.
 * Math only, headless; NEVER submits to a renderer here. */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

#ifndef LED_PI
    #define LED_PI 3.14159265358979323846f
#endif

void led_viewport_default(led_viewport *viewport) {
    if (viewport == NULL) {
        return;
    }
    memset(viewport, 0, sizeof(*viewport));
    viewport->width = 640;
    viewport->height = 480;
    viewport->target[0] = 0.0f;
    viewport->target[1] = 0.0f;
    viewport->target[2] = 0.0f;
    viewport->yaw_rad = 0.0f;
    viewport->pitch_rad = -0.35f;
    viewport->distance = 8.0f;
    viewport->fov_y_rad = 60.0f * LED_PI / 180.0f;
    viewport->near_plane = 0.1f;
    viewport->far_plane = 1000.0f;
}

void led_viewport_orbit(led_viewport *viewport, float dyaw,
                        float dpitch) {
    if (viewport == NULL) {
        return;
    }
    if (dyaw != dyaw || dpitch != dpitch) {
        return;
    }
    viewport->yaw_rad += dyaw;
    viewport->pitch_rad += dpitch;
    /* Wrap yaw to [-PI, PI]. */
    while (viewport->yaw_rad > LED_PI) {
        viewport->yaw_rad -= 2.0f * LED_PI;
    }
    while (viewport->yaw_rad < -LED_PI) {
        viewport->yaw_rad += 2.0f * LED_PI;
    }
    if (viewport->pitch_rad > 1.55f) {
        viewport->pitch_rad = 1.55f;
    }
    if (viewport->pitch_rad < -1.55f) {
        viewport->pitch_rad = -1.55f;
    }
}

void led_viewport_dolly(led_viewport *viewport, float factor) {
    if (viewport == NULL) {
        return;
    }
    if (factor != factor || factor <= 0.0f) {
        return;
    }
    viewport->distance *= factor;
    if (viewport->distance < 0.05f) {
        viewport->distance = 0.05f;
    }
    if (viewport->distance > 1e5f) {
        viewport->distance = 1e5f;
    }
}

void led_viewport_pan(led_viewport *viewport, float dx, float dy) {
    /* Pan in the camera frame: right = +x, up = +y of the view. */
    float cp;
    float sp;
    float cy;
    float sy;
    float right[3];
    float up[3];
    float k;

    if (viewport == NULL) {
        return;
    }
    if (dx != dx || dy != dy) {
        return;
    }
    cp = cosf(viewport->pitch_rad);
    sp = sinf(viewport->pitch_rad);
    cy = cosf(viewport->yaw_rad);
    sy = sinf(viewport->yaw_rad);
    (void)sp;
    /* Forward (target - eye) normalized, yaw/pitch composed. */
    right[0] = cy;
    right[1] = 0.0f;
    right[2] = -sy;
    up[0] = -sy * sp;
    up[1] = cp;
    up[2] = -cy * sp;
    k = viewport->distance / 480.0f;
    viewport->target[0] -= (right[0] * dx - up[0] * dy) * k;
    viewport->target[1] -= (right[1] * dx - up[1] * dy) * k;
    viewport->target[2] -= (right[2] * dx - up[2] * dy) * k;
}

static int led_viewport_valid(const led_viewport *v) {
    if (v == NULL) {
        return 0;
    }
    if (v->width == 0 || v->height == 0) {
        return 0;
    }
    if (!(v->distance > 0.0f) || !(v->fov_y_rad > 0.0f) ||
        !(v->near_plane > 0.0f) || !(v->far_plane > v->near_plane)) {
        return 0;
    }
    return 1;
}

/* Eye position for the orbit state. */
static void led_viewport_eye(const led_viewport *v, float eye[3]) {
    float cp = cosf(v->pitch_rad);
    float sp = sinf(v->pitch_rad);
    float cy = cosf(v->yaw_rad);
    float sy = sinf(v->yaw_rad);

    /* Orbit convention (matches the gizmo overlay's drag-plane
     * basis and the Phase 33V verified headed framing): yaw 0
     * parks the eye on +Z looking at the target; positive pitch
     * raises the eye above the target plane. */
    eye[0] = v->target[0] + v->distance * cp * sy;
    eye[1] = v->target[1] + v->distance * sp;
    eye[2] = v->target[2] + v->distance * cp * cy;
}

int led_viewport_camera(const led_viewport *viewport,
                        lr_camera *out_camera) {
    float eye[3];
    float center[3];
    float up[3] = { 0.0f, 1.0f, 0.0f };
    float aspect;

    if (!led_viewport_valid(viewport)) {
        return 0;
    }
    if (out_camera == NULL) {
        return 1;
    }
    led_viewport_eye(viewport, eye);
    center[0] = viewport->target[0];
    center[1] = viewport->target[1];
    center[2] = viewport->target[2];
    /* Polar guard: look_at rejects up-parallel views; tilt up. */
    if (fabsf(viewport->pitch_rad) > 1.569f) {
        up[0] = 0.0f;
        up[1] = 0.0f;
        up[2] = (viewport->pitch_rad > 0.0f) ? -1.0f : 1.0f;
    }
    lr_camera_init(out_camera);
    if (lr_camera_look_at(out_camera, eye, center, up) != 0) {
        return 0;
    }
    aspect = (float)viewport->width / (float)viewport->height;
    if (lr_camera_set_perspective(out_camera, viewport->fov_y_rad,
                                  aspect, viewport->near_plane,
                                  viewport->far_plane) != 0) {
        return 0;
    }
    return 1;
}

/* 4x4 inverse (column-major, Cramer; 0 = singular). */
static int led_mat4_inverse(const float m[16], float out[16]) {
    float inv[16];
    float det;
    int i;

    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] -
             m[9] * m[6] * m[15] + m[9] * m[7] * m[14] +
             m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] +
             m[8] * m[6] * m[15] - m[8] * m[7] * m[14] -
             m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] -
             m[8] * m[5] * m[15] + m[8] * m[7] * m[13] +
             m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] +
              m[8] * m[5] * m[14] - m[8] * m[6] * m[13] -
              m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] +
             m[9] * m[2] * m[15] - m[9] * m[3] * m[14] -
             m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] -
             m[8] * m[2] * m[15] + m[8] * m[3] * m[14] +
             m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] +
             m[8] * m[1] * m[15] - m[8] * m[3] * m[13] -
             m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] -
              m[8] * m[1] * m[14] + m[8] * m[2] * m[13] +
              m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] -
             m[5] * m[2] * m[15] + m[5] * m[3] * m[14] +
             m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] +
             m[4] * m[2] * m[15] - m[4] * m[3] * m[14] -
             m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] -
              m[4] * m[1] * m[15] + m[4] * m[3] * m[13] +
              m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] +
              m[4] * m[1] * m[14] - m[4] * m[2] * m[13] -
              m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] +
             m[5] * m[2] * m[11] - m[5] * m[3] * m[10] -
             m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] -
             m[4] * m[2] * m[11] + m[4] * m[3] * m[10] +
             m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] +
              m[4] * m[1] * m[11] - m[4] * m[3] * m[9] -
              m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] -
              m[4] * m[1] * m[10] + m[4] * m[2] * m[9] +
              m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] +
          m[3] * inv[12];
    if (det == 0.0f || det != det) {
        return 0;
    }
    det = 1.0f / det;
    for (i = 0; i < 16; i++) {
        out[i] = inv[i] * det;
    }
    return 1;
}

int led_viewport_ray(const led_viewport *viewport, float pixel_x,
                     float pixel_y, float out_origin[3],
                     float out_dir[3]) {
    lr_camera cam;
    float vp[16];
    float inv[16];
    float nx;
    float ny;
    float p0[4];
    float p1[4];
    float w0;
    float w1;
    float d[3];
    float n;

    if (!led_viewport_valid(viewport)) {
        return 0;
    }
    if (pixel_x != pixel_x || pixel_y != pixel_y) {
        return 0;
    }
    if (pixel_x < 0.0f || pixel_y < 0.0f ||
        pixel_x > (float)viewport->width ||
        pixel_y > (float)viewport->height) {
        return 0;
    }
    if (!led_viewport_camera(viewport, &cam)) {
        return 0;
    }
    /* view-proj = proj * view (column-major). */
    {
        const float *a = cam.projection;
        const float *b = cam.view;
        int c;
        int r;

        for (c = 0; c < 4; c++) {
            for (r = 0; r < 4; r++) {
                vp[c * 4 + r] = a[0 * 4 + r] * b[c * 4 + 0] +
                                a[1 * 4 + r] * b[c * 4 + 1] +
                                a[2 * 4 + r] * b[c * 4 + 2] +
                                a[3 * 4 + r] * b[c * 4 + 3];
            }
        }
    }
    if (!led_mat4_inverse(vp, inv)) {
        return 0;
    }
    /* Client px -> NDC (Vulkan depth 0..1; x/y flipped to NDC). */
    nx = (pixel_x / (float)viewport->width) * 2.0f - 1.0f;
    ny = 1.0f - (pixel_y / (float)viewport->height) * 2.0f;
    {
        float pts[2][4] = { { nx, ny, 0.0f, 1.0f },
                            { nx, ny, 1.0f, 1.0f } };
        float *outs[2] = { p0, p1 };
        int k;

        for (k = 0; k < 2; k++) {
            int r;

            for (r = 0; r < 4; r++) {
                outs[k][r] = inv[0 * 4 + r] * pts[k][0] +
                             inv[1 * 4 + r] * pts[k][1] +
                             inv[2 * 4 + r] * pts[k][2] +
                             inv[3 * 4 + r] * pts[k][3];
            }
        }
    }
    w0 = (p0[3] != 0.0f) ? 1.0f / p0[3] : 0.0f;
    w1 = (p1[3] != 0.0f) ? 1.0f / p1[3] : 0.0f;
    if (w0 == 0.0f || w1 == 0.0f) {
        return 0;
    }
    if (out_origin != NULL) {
        out_origin[0] = p0[0] * w0;
        out_origin[1] = p0[1] * w0;
        out_origin[2] = p0[2] * w0;
    }
    d[0] = p1[0] * w1 - p0[0] * w0;
    d[1] = p1[1] * w1 - p0[1] * w0;
    d[2] = p1[2] * w1 - p0[2] * w0;
    n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n < 1e-12f || n != n) {
        return 0;
    }
    if (out_dir != NULL) {
        out_dir[0] = d[0] / n;
        out_dir[1] = d[1] / n;
        out_dir[2] = d[2] / n;
    }
    return 1;
}

int led_viewport_pick(led_session *session,
                      const led_viewport *viewport, float pixel_x,
                      float pixel_y, float max_distance,
                      uint32_t layer_mask, le_ray_hit *out_hit) {
    float origin[3];
    float dir[3];
    le_world *w;

    if (out_hit != NULL) {
        memset(out_hit, 0, sizeof(*out_hit));
        out_hit->object = LE_OBJECT_INVALID;
    }
    if (session == NULL || viewport == NULL ||
        !led_is_attached(session)) {
        return 0;
    }
    if (session->playing && session->play_world != NULL) {
        w = session->play_world;
    } else {
        w = session->edit_world;
    }
    if (!led_viewport_ray(viewport, pixel_x, pixel_y, origin, dir)) {
        return 0;
    }
    if (!(max_distance > 0.0f)) {
        max_distance = viewport->far_plane;
    }
    return le_physics_raycast(w, origin[0], origin[1], origin[2],
                              dir[0], dir[1], dir[2], max_distance,
                              layer_mask, 0, out_hit);
}

int led_viewport_pick_select(led_session *session,
                             const led_viewport *viewport,
                             float pixel_x, float pixel_y) {
    le_ray_hit hit;

    memset(&hit, 0, sizeof(hit));
    if (session == NULL || viewport == NULL ||
        !led_is_attached(session)) {
        return 0;
    }
    if (session->playing) {
        return 0;
    }
    if (led_viewport_pick(session, viewport, pixel_x, pixel_y, 0.0f,
                          0xFFFFFFFFu, &hit)) {
        led_selection_set(session, &hit.object, 1);
        return 1;
    }
    led_selection_clear(session);
    return 0;
}

int led_compute_world_aabb(led_session *session,
                           const le_object *object, float out_min[3],
                           float out_max[3]) {
    le_world *w;
    le_renderable_desc d;
    lr_bounds b;
    float m[16];
    float corners[8][3];
    int i;

    if (out_min != NULL) {
        out_min[0] = out_min[1] = out_min[2] = 0.0f;
    }
    if (out_max != NULL) {
        out_max[0] = out_max[1] = out_max[2] = 0.0f;
    }
    if (session == NULL || object == NULL ||
        !led_is_attached(session)) {
        return 0;
    }
    w = session->edit_world;
    if (!le_object_is_alive(w, object)) {
        return 0;
    }
    /* Asset-backed renderables resolve through the engine registry
     * (le_asset_get_mesh — the same resolution the submit path
     * uses). R-013: every Game/Shadow scene object is asset-backed,
     * so the old "never guessed" early-out made F fall back to a
     * zero-size point box for ALL real content (Game framed at
     * dist 0.87, Shadow at 0.52 — inside the geometry). Unready
     * assets still report 0 (never guessed). */
    {
        le_asset_renderable_desc ad;
        lr_mesh *amesh = NULL;

        memset(&ad, 0, sizeof(ad));
        if (le_object_get_asset_renderable(w, object, &ad)) {
            if (session->engine != NULL) {
                amesh = le_asset_get_mesh(session->engine,
                                          &ad.mesh);
            }
            if (amesh == NULL) {
                return 0;
            }
            lr_mesh_get_bounds(amesh, &b);
            le_object_get_world_matrix(w, object, m);
            goto corners;
        }
    }
    if (!le_object_get_renderable(w, object, &d)) {
        return 0;
    }
    if (d.mesh == NULL) {
        return 0;
    }
    lr_mesh_get_bounds(d.mesh, &b);
    le_object_get_world_matrix(w, object, m);
corners:
    /* 8 corners of the local AABB through the world matrix. */
    for (i = 0; i < 8; i++) {
        float x = (i & 1) ? b.max[0] : b.min[0];
        float y = (i & 2) ? b.max[1] : b.min[1];
        float z = (i & 4) ? b.max[2] : b.min[2];

        corners[i][0] = m[0] * x + m[4] * y + m[8] * z + m[12];
        corners[i][1] = m[1] * x + m[5] * y + m[9] * z + m[13];
        corners[i][2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    }
    if (out_min != NULL) {
        out_min[0] = out_min[1] = out_min[2] = 1e30f;
    }
    if (out_max != NULL) {
        out_max[0] = out_max[1] = out_max[2] = -1e30f;
    }
    for (i = 0; i < 8; i++) {
        int a;

        for (a = 0; a < 3; a++) {
            if (out_min != NULL && corners[i][a] < out_min[a]) {
                out_min[a] = corners[i][a];
            }
            if (out_max != NULL && corners[i][a] > out_max[a]) {
                out_max[a] = corners[i][a];
            }
        }
    }
    return 1;
}

int led_selection_aabb(led_session *session, float out_min[3],
                       float out_max[3]) {
    uint32_t i;
    int any = 0;
    float mn[3] = { 1e30f, 1e30f, 1e30f };
    float mx[3] = { -1e30f, -1e30f, -1e30f };

    if (out_min != NULL) {
        out_min[0] = out_min[1] = out_min[2] = 0.0f;
    }
    if (out_max != NULL) {
        out_max[0] = out_max[1] = out_max[2] = 0.0f;
    }
    if (session == NULL || !led_is_attached(session)) {
        return 0;
    }
    for (i = 0; i < session->selection_count; i++) {
        float lmn[3];
        float lmx[3];

        if (!le_object_is_alive(session->edit_world,
                                &session->selection[i])) {
            continue;
        }
        if (led_compute_world_aabb(session, &session->selection[i],
                                   lmn, lmx)) {
            int a;

            for (a = 0; a < 3; a++) {
                if (lmn[a] < mn[a]) {
                    mn[a] = lmn[a];
                }
                if (lmx[a] > mx[a]) {
                    mx[a] = lmx[a];
                }
            }
            any = 1;
        } else {
            /* R-013: hierarchy roots have no mesh of their own —
             * union the descendant subtree bounds (bounded DFS:
             * 4096 descendants, 256-child chunks, cycle-safe via
             * the engine's own child lists). A rig framing only
             * its own point would park the camera at an empty
             * location; the subtree bounds frame the actual
             * content (CameraRig → Camera). Members that still
             * have no bounds fall back to their position. */
            le_object stack[256];
            uint32_t top = 0;
            uint32_t walked = 0;
            int sub_any = 0;

            stack[top++] = session->selection[i];
            while (top > 0 && walked < 4096) {
                le_object cur = stack[--top];
                uint32_t cc = le_object_get_child_count(
                    session->edit_world, &cur);
                le_object kids[256];
                uint32_t total = 0;
                uint32_t k = 0;
                uint32_t got = 0;

                walked++;
                if (cc == 0 || cc > 4096) {
                    continue;
                }
                if (le_object_get_children(session->edit_world,
                                           &cur, kids, 256,
                                           &total) != LE_SUCCESS) {
                    continue;
                }
                got = (total < 256) ? total : 256;
                for (k = 0; k < got; k++) {
                    float kmn[3];
                    float kmx[3];
                    int a;

                    if (!le_object_is_alive(
                            session->edit_world, &kids[k])) {
                        continue;
                    }
                    if (led_compute_world_aabb(session,
                                               &kids[k], kmn,
                                               kmx)) {
                        for (a = 0; a < 3; a++) {
                            if (kmn[a] < mn[a]) {
                                mn[a] = kmn[a];
                            }
                            if (kmx[a] > mx[a]) {
                                mx[a] = kmx[a];
                            }
                        }
                        any = 1;
                        sub_any = 1;
                    }
                    if (top < 256) {
                        stack[top++] = kids[k];
                    }
                }
            }
            if (!sub_any) {
                /* Fall back to the object position for non-mesh
                 * selection members. */
                float p[3];

                le_object_get_position(session->edit_world,
                                       &session->selection[i], p);
                {
                    int a;

                    for (a = 0; a < 3; a++) {
                        if (p[a] < mn[a]) {
                            mn[a] = p[a];
                        }
                        if (p[a] > mx[a]) {
                            mx[a] = p[a];
                        }
                    }
                }
                any = 1;
            }
        }
    }
    if (!any) {
        return 0;
    }
    if (out_min != NULL) {
        memcpy(out_min, mn, sizeof(mn));
    }
    if (out_max != NULL) {
        memcpy(out_max, mx, sizeof(mx));
    }
    return 1;
}

int led_frame_selection(led_session *session,
                        led_viewport *viewport) {
    float mn[3];
    float mx[3];
    float center[3];
    float radius;
    float d[3];

    if (session == NULL || viewport == NULL ||
        !led_is_attached(session)) {
        return 0;
    }
    if (!led_selection_aabb(session, mn, mx)) {
        /* Whole scene: union over all objects' positions. */
        uint32_t live = le_world_get_object_count(
            session->edit_world);
        le_object *all = NULL;
        uint32_t i;
        int any = 0;

        if (live == 0) {
            return 0;
        }
        all = (le_object *)malloc(live * sizeof(*all));
        if (all == NULL) {
            return 0;
        }
        {
            uint32_t got = le_world_get_all_objects(
                session->edit_world, all, live);

            mn[0] = mn[1] = mn[2] = 1e30f;
            mx[0] = mx[1] = mx[2] = -1e30f;
            for (i = 0; i < got; i++) {
                float p[3];
                int a;

                le_object_get_position(session->edit_world,
                                       &all[i], p);
                for (a = 0; a < 3; a++) {
                    if (p[a] < mn[a]) {
                        mn[a] = p[a];
                    }
                    if (p[a] > mx[a]) {
                        mx[a] = p[a];
                    }
                }
                any = 1;
            }
        }
        free(all);
        if (!any) {
            return 0;
        }
    }
    center[0] = (mn[0] + mx[0]) * 0.5f;
    center[1] = (mn[1] + mx[1]) * 0.5f;
    center[2] = (mn[2] + mx[2]) * 0.5f;
    d[0] = mx[0] - mn[0];
    d[1] = mx[1] - mn[1];
    d[2] = mx[2] - mn[2];
    radius = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * 0.5f;
    /* R-013: degenerate (point) selections — position-only members
     * (cameras, lights, empty transforms) union to a zero-size box.
     * A zero radius would park the camera ON the point (inside the
     * object / near-plane clip). Floor the radius at 1 m so F on a
     * camera/light/empty frames its neighborhood at a useful
     * working distance instead of ending inside it. */
    if (!(radius >= 1.0f)) {
        radius = 1.0f;
    }
    memcpy(viewport->target, center, sizeof(center));
    {
        float half_fov = viewport->fov_y_rad * 0.5f;
        float dist;

        if (!(half_fov > 0.01f)) {
            half_fov = 0.5f;
        }
        dist = (radius + 0.5f) / tanf(half_fov);
        if (!(dist > 0.05f)) {
            dist = 8.0f;
        }
        if (dist > 1e5f) {
            dist = 1e5f;
        }
        viewport->distance = dist;
    }
    return 1;
}
