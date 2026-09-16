/* Reflection: editor-side static tables over validated engine
 * APIs. No engine internals; every read validates liveness first.
 *
 * Path vocabulary (stable, documented in ENGINE_REFLECTION.md):
 *   object.name / object.enabled
 *   transform.position / transform.rotation_quat / transform.rotation_euler_deg
 *   transform.scale / transform.parent (read-only handle summary)
 *   renderable.visible / renderable.casts_shadow / renderable.receives_shadow
 *     / renderable.mesh_asset (read-only hex or UNAVAILABLE)
 *   camera.projection / camera.fov_y_deg / camera.ortho_height /
 *     camera.aspect / camera.near / camera.far
 *   light.type / light.color / light.intensity / light.range /
 *     light.spot_inner_deg / light.spot_outer_deg
 *   script.<export_name> (enumerated live; typed via le_script_* API)
 *   rigidbody.type / rigidbody.mass / rigidbody.linear_damping /
 *     rigidbody.angular_damping / rigidbody.gravity_scale /
 *     rigidbody.linear_velocity / rigidbody.angular_velocity
 *   collider.shape / collider.radius / collider.half_extents /
 *     collider.capsule_radius / collider.capsule_half_height /
 *     collider.offset / collider.is_trigger / collider.layer /
 *     collider.mask / collider.friction / collider.restitution
 *   animator.autoplay / animator.loop / animator.speed /
 *     animator.start_time / animator.playing (read-only) /
 *     animator.time (read-only)
 *   character.radius / character.height / character.skin_width /
 *     character.max_slope_deg / character.step_height / character.gravity /
 *     character.terminal_velocity / character.snap_distance /
 *     character.push_strength / character.layer / character.mask
 *
 * Euler policy (normative): storage is always quaternion
 * (le_object_set_rotation, normalized on store). The inspector shows
 * ZYX-Euler degrees; writes convert deg->quat. Gimbal branch: |pitch|
 * <= 90 degrees canonical. NaN/non-finite rejected pre-conversion.
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

#ifndef LED_PI
    #define LED_PI 3.14159265358979323846f
#endif

/* Static property table (sorted by path for binary search). */
typedef struct led_static_prop {
    const char *path;
    const char *label;
    le_component_type component;
    led_data_type type;
    uint32_t index;
    float min_value;
    float max_value;
    int has_range;
    const char *enum_labels;
    int read_only;
} led_static_prop;

#define LED_PROP(path_, label_, comp_, type_, idx_, ...) \
    { path_, label_, comp_, type_, idx_, __VA_ARGS__ }

static const led_static_prop kLedProps[] = {
    LED_PROP("animator.autoplay", "Autoplay", LE_COMPONENT_ANIMATOR,
             LED_DATA_BOOL, 0, 0, 0, 0, NULL, 0),
    LED_PROP("animator.loop", "Loop mode", LE_COMPONENT_ANIMATOR,
             LED_DATA_ENUM, 1, 0, 0, 0, "once;loop;pingpong", 0),
    LED_PROP("animator.playing", "Playing", LE_COMPONENT_ANIMATOR,
             LED_DATA_BOOL, 2, 0, 0, 0, NULL, 1),
    LED_PROP("animator.speed", "Speed", LE_COMPONENT_ANIMATOR,
             LED_DATA_FLOAT, 3, -64.0f, 64.0f, 1, NULL, 0),
    LED_PROP("animator.start_time", "Start time (s)",
             LE_COMPONENT_ANIMATOR, LED_DATA_FLOAT, 4, 0, 0, 0, NULL, 0),
    LED_PROP("animator.time", "Time (s)", LE_COMPONENT_ANIMATOR,
             LED_DATA_FLOAT, 5, 0, 0, 0, NULL, 1),
    LED_PROP("camera.aspect", "Aspect", LE_COMPONENT_CAMERA,
             LED_DATA_FLOAT, 3, 0.01f, 100.0f, 1, NULL, 0),
    LED_PROP("camera.far", "Far", LE_COMPONENT_CAMERA, LED_DATA_FLOAT,
             5, 0.001f, 1.0e6f, 1, NULL, 0),
    LED_PROP("camera.fov_y_deg", "FOV Y (deg)", LE_COMPONENT_CAMERA,
             LED_DATA_FLOAT, 1, 1.0f, 179.0f, 1, NULL, 0),
    LED_PROP("camera.near", "Near", LE_COMPONENT_CAMERA, LED_DATA_FLOAT,
             4, 0.001f, 1.0e5f, 1, NULL, 0),
    LED_PROP("camera.ortho_height", "Ortho height", LE_COMPONENT_CAMERA,
             LED_DATA_FLOAT, 2, 0.001f, 1.0e6f, 1, NULL, 0),
    LED_PROP("camera.projection", "Projection", LE_COMPONENT_CAMERA,
             LED_DATA_ENUM, 0, 0, 0, 0, "perspective;orthographic", 0),
    LED_PROP("character.gravity", "Gravity", LE_COMPONENT_CHARACTER_CONTROLLER,
             LED_DATA_FLOAT, 6, 0, 100.0f, 1, NULL, 0),
    LED_PROP("character.height", "Height", LE_COMPONENT_CHARACTER_CONTROLLER,
             LED_DATA_FLOAT, 1, 0.01f, 100.0f, 1, NULL, 0),
    LED_PROP("character.layer", "Layer", LE_COMPONENT_CHARACTER_CONTROLLER,
             LED_DATA_UINT, 10, 0, 31, 1, NULL, 0),
    LED_PROP("character.mask", "Mask", LE_COMPONENT_CHARACTER_CONTROLLER,
             LED_DATA_UINT, 11, 0, 0, 0, NULL, 0),
    LED_PROP("character.max_slope_deg", "Max slope (deg)",
             LE_COMPONENT_CHARACTER_CONTROLLER, LED_DATA_FLOAT, 4,
             0, 89.9f, 1, NULL, 0),
    LED_PROP("character.push_strength", "Push strength",
             LE_COMPONENT_CHARACTER_CONTROLLER, LED_DATA_FLOAT, 9,
             0, 1000.0f, 1, NULL, 0),
    LED_PROP("character.radius", "Radius", LE_COMPONENT_CHARACTER_CONTROLLER,
             LED_DATA_FLOAT, 0, 0.001f, 100.0f, 1, NULL, 0),
    LED_PROP("character.skin_width", "Skin width",
             LE_COMPONENT_CHARACTER_CONTROLLER, LED_DATA_FLOAT, 3,
             0, 1.0f, 1, NULL, 0),
    LED_PROP("character.snap_distance", "Snap distance",
             LE_COMPONENT_CHARACTER_CONTROLLER, LED_DATA_FLOAT, 8,
             0, 10.0f, 1, NULL, 0),
    LED_PROP("character.step_height", "Step height",
             LE_COMPONENT_CHARACTER_CONTROLLER, LED_DATA_FLOAT, 5,
             0, 10.0f, 1, NULL, 0),
    LED_PROP("character.terminal_velocity", "Terminal velocity",
             LE_COMPONENT_CHARACTER_CONTROLLER, LED_DATA_FLOAT, 7,
             0, 1000.0f, 1, NULL, 0),
    LED_PROP("collider.capsule_half_height", "Capsule half height",
             LE_COMPONENT_COLLIDER, LED_DATA_FLOAT, 4, 0, 1.0e4f, 1,
             NULL, 0),
    LED_PROP("collider.capsule_radius", "Capsule radius",
             LE_COMPONENT_COLLIDER, LED_DATA_FLOAT, 3, 0.0001f, 1.0e4f,
             1, NULL, 0),
    LED_PROP("collider.friction", "Friction", LE_COMPONENT_COLLIDER,
             LED_DATA_FLOAT, 11, 0, 10.0f, 1, NULL, 0),
    LED_PROP("collider.half_extents", "Half extents",
             LE_COMPONENT_COLLIDER, LED_DATA_VEC3, 2, 0, 0, 0, NULL, 0),
    LED_PROP("collider.is_trigger", "Is trigger", LE_COMPONENT_COLLIDER,
             LED_DATA_BOOL, 7, 0, 0, 0, NULL, 0),
    LED_PROP("collider.layer", "Layer", LE_COMPONENT_COLLIDER,
             LED_DATA_UINT, 8, 0, 31, 1, NULL, 0),
    LED_PROP("collider.mask", "Mask", LE_COMPONENT_COLLIDER,
             LED_DATA_UINT, 9, 0, 0, 0, NULL, 0),
    LED_PROP("collider.offset", "Offset", LE_COMPONENT_COLLIDER,
             LED_DATA_VEC3, 5, 0, 0, 0, NULL, 0),
    LED_PROP("collider.radius", "Radius", LE_COMPONENT_COLLIDER,
             LED_DATA_FLOAT, 1, 0.0001f, 1.0e4f, 1, NULL, 0),
    LED_PROP("collider.restitution", "Restitution", LE_COMPONENT_COLLIDER,
             LED_DATA_FLOAT, 12, 0, 1.0f, 1, NULL, 0),
    LED_PROP("collider.shape", "Shape", LE_COMPONENT_COLLIDER,
             LED_DATA_ENUM, 0, 0, 0, 0, "sphere;box;capsule", 0),
    LED_PROP("light.color", "Color", LE_COMPONENT_LIGHT, LED_DATA_COLOR3,
             1, 0, 0, 0, NULL, 0),
    LED_PROP("light.intensity", "Intensity", LE_COMPONENT_LIGHT,
             LED_DATA_FLOAT, 2, 0, 1.0e9f, 1, NULL, 0),
    LED_PROP("light.range", "Range", LE_COMPONENT_LIGHT, LED_DATA_FLOAT,
             3, 0, 1.0e6f, 1, NULL, 0),
    LED_PROP("light.spot_inner_deg", "Spot inner (deg)",
             LE_COMPONENT_LIGHT, LED_DATA_FLOAT, 4, 0, 89.9f, 1, NULL, 0),
    LED_PROP("light.spot_outer_deg", "Spot outer (deg)",
             LE_COMPONENT_LIGHT, LED_DATA_FLOAT, 5, 0, 89.9f, 1, NULL, 0),
    LED_PROP("light.type", "Type", LE_COMPONENT_LIGHT, LED_DATA_ENUM,
             0, 0, 0, 0, "directional;point;spot", 0),
    LED_PROP("object.enabled", "Enabled", LE_COMPONENT_TRANSFORM,
             LED_DATA_BOOL, 1, 0, 0, 0, NULL, 0),
    LED_PROP("object.name", "Name", LE_COMPONENT_TRANSFORM,
             LED_DATA_STRING, 0, 0, 0, 0, NULL, 0),
    LED_PROP("renderable.casts_shadow", "Casts shadow",
             LE_COMPONENT_RENDERABLE, LED_DATA_BOOL, 1, 0, 0, 0, NULL, 0),
    LED_PROP("renderable.mesh_asset", "Mesh asset",
             LE_COMPONENT_RENDERABLE, LED_DATA_ASSET_ID, 3, 0, 0, 0,
             NULL, 1),
    LED_PROP("renderable.receives_shadow", "Receives shadow",
             LE_COMPONENT_RENDERABLE, LED_DATA_BOOL, 2, 0, 0, 0, NULL, 0),
    LED_PROP("renderable.visible", "Visible", LE_COMPONENT_RENDERABLE,
             LED_DATA_BOOL, 0, 0, 0, 0, NULL, 0),
    LED_PROP("rigidbody.angular_damping", "Angular damping",
             LE_COMPONENT_RIGID_BODY, LED_DATA_FLOAT, 3, 0, 100.0f, 1,
             NULL, 0),
    LED_PROP("rigidbody.angular_velocity", "Angular velocity",
             LE_COMPONENT_RIGID_BODY, LED_DATA_VEC3, 6, 0, 0, 0, NULL, 0),
    LED_PROP("rigidbody.gravity_scale", "Gravity scale",
             LE_COMPONENT_RIGID_BODY, LED_DATA_FLOAT, 4, -100.0f,
             100.0f, 1, NULL, 0),
    LED_PROP("rigidbody.linear_damping", "Linear damping",
             LE_COMPONENT_RIGID_BODY, LED_DATA_FLOAT, 2, 0, 100.0f, 1,
             NULL, 0),
    LED_PROP("rigidbody.linear_velocity", "Linear velocity",
             LE_COMPONENT_RIGID_BODY, LED_DATA_VEC3, 5, 0, 0, 0, NULL, 0),
    LED_PROP("rigidbody.mass", "Mass", LE_COMPONENT_RIGID_BODY,
             LED_DATA_FLOAT, 1, 0.0001f, 1.0e9f, 1, NULL, 0),
    LED_PROP("rigidbody.type", "Body type", LE_COMPONENT_RIGID_BODY,
             LED_DATA_ENUM, 0, 0, 0, 0, "static;dynamic;kinematic", 0),
    LED_PROP("transform.parent", "Parent", LE_COMPONENT_TRANSFORM,
             LED_DATA_STRING, 3, 0, 0, 0, NULL, 1),
    LED_PROP("transform.position", "Position", LE_COMPONENT_TRANSFORM,
             LED_DATA_VEC3, 0, 0, 0, 0, NULL, 0),
    LED_PROP("transform.rotation_euler_deg", "Rotation (deg)",
             LE_COMPONENT_TRANSFORM, LED_DATA_EULER_DEG, 2, 0, 0, 0,
             NULL, 0),
    LED_PROP("transform.rotation_quat", "Rotation (quat)",
             LE_COMPONENT_TRANSFORM, LED_DATA_QUAT, 1, 0, 0, 0, NULL, 0),
    LED_PROP("transform.scale", "Scale", LE_COMPONENT_TRANSFORM,
             LED_DATA_VEC3, 3 - 1, 0, 0, 0, NULL, 0),
};

#define LED_STATIC_PROP_COUNT \
    ((uint32_t)(sizeof(kLedProps) / sizeof(kLedProps[0])))

/* Euler helpers (ZYX: yaw Y, pitch X, roll Z; degrees on the wire). */
static void led_quat_to_euler_deg(const float q[4], float out_deg[3]) {
    float x = q[0];
    float y = q[1];
    float z = q[2];
    float w = q[3];
    float sp;
    float yaw;
    float pitch;
    float roll;

    /* Normalize defensively (storage is unit, math stays safe). */
    {
        float n = sqrtf(x * x + y * y + z * z + w * w);

        if (n > 1e-12f && n == n) {
            x /= n;
            y /= n;
            z /= n;
            w /= n;
        } else {
            out_deg[0] = 0.0f;
            out_deg[1] = 0.0f;
            out_deg[2] = 0.0f;
            return;
        }
    }
    /* ZYX extraction (yaw Y, pitch X, roll Z) matching the
     * composition above: pitch = asin(clamp(2*(w*x + y*z))). */
    sp = 2.0f * (w * x + y * z);
    if (sp > 1.0f) {
        sp = 1.0f;
    }
    if (sp < -1.0f) {
        sp = -1.0f;
    }
    pitch = asinf(sp);
    if (fabsf(fabsf(sp) - 1.0f) < 1e-6f) {
        /* Gimbal lock: yaw from y/w, roll 0 (canonical branch). */
        yaw = 2.0f * atan2f(y, w);
        roll = 0.0f;
    } else {
        yaw = atan2f(2.0f * (w * y - z * x),
                     1.0f - 2.0f * (x * x + y * y));
        roll = atan2f(2.0f * (w * z - x * y),
                      1.0f - 2.0f * (x * x + z * z));
    }
    /* Wire order: pitch(X), yaw(Y), roll(Z) degrees. */
    out_deg[0] = pitch * (180.0f / LED_PI);
    out_deg[1] = yaw * (180.0f / LED_PI);
    out_deg[2] = roll * (180.0f / LED_PI);
}

static int led_euler_deg_to_quat(const float deg[3], float out_q[4]) {
    float p;
    float y;
    float r;
    float cp;
    float sp_;
    float cy;
    float sy;
    float cr;
    float sr;

    if (deg[0] != deg[0] || deg[1] != deg[1] || deg[2] != deg[2]) {
        return 0;
    }
    p = deg[0] * (LED_PI / 180.0f) * 0.5f;
    y = deg[1] * (LED_PI / 180.0f) * 0.5f;
    r = deg[2] * (LED_PI / 180.0f) * 0.5f;
    cp = cosf(p);
    sp_ = sinf(p);
    cy = cosf(y);
    sy = sinf(y);
    cr = cosf(r);
    sr = sinf(r);
    /* ZYX (yaw Y, pitch X, roll Z): q = qy * qx * qz. With
     * qx=(sp,0,0,cp), qy=(0,sy,0,cy), qz=(0,0,sr,cr):
     * 90-deg pitch -> (0.7071,0,0,0.7071) as required. */
    out_q[0] = cy * sp_ * cr + sy * cp * sr;
    out_q[1] = sy * cp * cr - cy * sp_ * sr;
    out_q[2] = sy * sp_ * cr - cy * cp * sr;
    out_q[3] = cy * cp * cr + sy * sp_ * sr;
    {
        float n = sqrtf(out_q[0] * out_q[0] + out_q[1] * out_q[1] +
                        out_q[2] * out_q[2] + out_q[3] * out_q[3]);

        if (n < 1e-12f || n != n) {
            out_q[0] = 0.0f;
            out_q[1] = 0.0f;
            out_q[2] = 0.0f;
            out_q[3] = 1.0f;
            return 1;
        }
        out_q[0] /= n;
        out_q[1] /= n;
        out_q[2] /= n;
        out_q[3] /= n;
    }
    return 1;
}

static int led_has_comp(le_world *w, const le_object *o,
                        le_component_type t) {
    return le_object_has_component(w, o, t);
}

void led_describe_object(led_session *session, const le_object *object,
                         led_object_schema *out_schema) {
    le_world *w;
    uint32_t mask = 0;
    uint32_t n = 0;
    uint32_t i;

    if (out_schema != NULL) {
        memset(out_schema, 0, sizeof(*out_schema));
    }
    if (session == NULL || object == NULL || out_schema == NULL) {
        return;
    }
    if (!led_is_attached(session)) {
        return;
    }
    w = session->edit_world;
    if (!le_object_is_alive(w, object)) {
        return;
    }
    out_schema->handle = *object;
    out_schema->alive = 1;
    mask |= (1u << LE_COMPONENT_TRANSFORM);
    for (i = 0; i < LED_STATIC_PROP_COUNT; i++) {
        if (led_has_comp(w, object, kLedProps[i].component)) {
            mask |= (1u << kLedProps[i].component);
        }
    }
    /* Script props are dynamic: presence via has_component. */
    if (led_has_comp(w, object, LE_COMPONENT_SCRIPT)) {
        mask |= (1u << LE_COMPONENT_SCRIPT);
    }
    out_schema->component_mask = mask;
    /* Count static props with live components + dynamic script props. */
    for (i = 0; i < LED_STATIC_PROP_COUNT; i++) {
        if ((mask & (1u << kLedProps[i].component)) != 0u) {
            /* transform.parent counts once; script has no statics. */
            n++;
        }
    }
    if ((mask & (1u << LE_COMPONENT_SCRIPT)) != 0u) {
        uint32_t sc = 0;

        if (le_script_list_properties(w, object, NULL, 0, &sc)) {
            n += sc;
        }
    }
    out_schema->property_count = n;
}

static const led_static_prop *led_find_static(const char *path) {
    int lo = 0;
    int hi = (int)LED_STATIC_PROP_COUNT - 1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(path, kLedProps[mid].path);

        if (c == 0) {
            return &kLedProps[mid];
        }
        if (c < 0) {
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}

uint32_t led_list_properties(led_session *session,
                             const le_object *object,
                             const led_property_desc **out_props,
                             uint32_t capacity, uint32_t *out_count) {
    /* Returns the STATIC count; descriptors are static storage so
     * out_props[i] borrows kLedProps entries. Dynamic script props
     * are enumerated via le_script_list_properties by the caller
     * (count included in led_describe_object). */
    static const led_property_desc *kEmpty = NULL;
    uint32_t n = 0;
    uint32_t i;

    (void)kEmpty;
    if (out_count != NULL) {
        *out_count = 0;
    }
    if (session == NULL || object == NULL ||
        !led_is_attached(session)) {
        return 0;
    }
    if (!le_object_is_alive(session->edit_world, object)) {
        return 0;
    }
    for (i = 0; i < LED_STATIC_PROP_COUNT; i++) {
        if (led_has_comp(session->edit_world, object,
                         kLedProps[i].component)) {
            if (out_props != NULL && n < capacity) {
                /* Reinterpret: layout-compatible view. */
                out_props[n] =
                    (const led_property_desc *)&kLedProps[i];
            }
            n++;
        }
    }
    if (out_count != NULL) {
        *out_count = n;
    }
    return n;
}

const led_property_desc *led_find_property(led_session *session,
                                           const le_object *object,
                                           const char *path) {
    const led_static_prop *sp;

    if (session == NULL || object == NULL || path == NULL ||
        !led_is_attached(session)) {
        return NULL;
    }
    if (!le_object_is_alive(session->edit_world, object)) {
        return NULL;
    }
    if (strncmp(path, "script.", 7) == 0) {
        /* Dynamic script props: presence-check only; descriptor is
         * synthesized into a rotating slot (descriptor + label
         * storage both static so nothing dangles). */
        static led_property_desc s_script_desc[4];
        static char s_script_label[4][64];
        static uint32_t s_slot = 0;
        le_script_property prop;
        led_property_desc *d;
        uint32_t slot;

        memset(&prop, 0, sizeof(prop));
        if (!le_script_get_property(session->edit_world, object,
                                    path + 7, &prop)) {
            return NULL;
        }
        slot = (s_slot++) & 3u;
        d = &s_script_desc[slot];
        memset(d, 0, sizeof(*d));
        memset(s_script_label[slot], 0,
               sizeof(s_script_label[slot]));
        strncpy(s_script_label[slot], prop.name,
                sizeof(s_script_label[slot]) - 1);
        d->path = path;
        d->label = s_script_label[slot];
        d->component = LE_COMPONENT_SCRIPT;
        switch (prop.type) {
        case LE_SCRIPT_PROP_BOOL:
            d->type = LED_DATA_BOOL;
            break;
        case LE_SCRIPT_PROP_INT:
            d->type = LED_DATA_INT;
            break;
        case LE_SCRIPT_PROP_NUMBER:
            d->type = LED_DATA_FLOAT;
            break;
        case LE_SCRIPT_PROP_STRING:
            d->type = LED_DATA_STRING;
            break;
        case LE_SCRIPT_PROP_VEC3:
            d->type = LED_DATA_VEC3;
            break;
        case LE_SCRIPT_PROP_ASSET:
            d->type = LED_DATA_ASSET_ID;
            break;
        default:
            d->type = LED_DATA_UNAVAILABLE;
            break;
        }
        return d;
    }
    sp = led_find_static(path);
    if (sp == NULL) {
        return NULL;
    }
    if (!led_has_comp(session->edit_world, object, sp->component) &&
        sp->component != LE_COMPONENT_TRANSFORM) {
        return NULL;
    }
    return (const led_property_desc *)sp;
}

/* Range check helper: 1 when finite and in [min,max] (when ranged). */
static int led_range_ok(const led_static_prop *sp, double v) {
    if (v != v || v > 1e300 || v < -1e300) {
        return 0;
    }
    if (sp->has_range && (v < sp->min_value || v > sp->max_value)) {
        return 0;
    }
    return 1;
}

int led_read_property(led_session *session, const le_object *object,
                      const char *path, led_property_value *out_value) {
    const led_static_prop *sp;
    le_world *w;

    if (session == NULL || object == NULL || path == NULL ||
        !led_is_attached(session)) {
        return 0;
    }
    w = session->edit_world;
    if (!le_object_is_alive(w, object)) {
        return 0;
    }
    if (strncmp(path, "script.", 7) == 0) {
        le_script_property prop;

        memset(&prop, 0, sizeof(prop));
        if (!le_script_get_property(w, object, path + 7, &prop)) {
            return 0;
        }
        if (out_value == NULL) {
            return 1;
        }
        memset(out_value, 0, sizeof(*out_value));
        switch (prop.type) {
        case LE_SCRIPT_PROP_BOOL:
            out_value->type = LED_DATA_BOOL;
            out_value->boolean = prop.boolean;
            break;
        case LE_SCRIPT_PROP_INT:
            out_value->type = LED_DATA_INT;
            out_value->integer = prop.integer;
            break;
        case LE_SCRIPT_PROP_NUMBER:
            out_value->type = LED_DATA_FLOAT;
            out_value->number = prop.number;
            break;
        case LE_SCRIPT_PROP_STRING:
            out_value->type = LED_DATA_STRING;
            strncpy(out_value->string_value, prop.string_value,
                    sizeof(out_value->string_value) - 1);
            break;
        case LE_SCRIPT_PROP_VEC3:
            out_value->type = LED_DATA_VEC3;
            memcpy(out_value->vec3, prop.vec3, sizeof(prop.vec3));
            break;
        case LE_SCRIPT_PROP_ASSET: {
            le_asset_id id;

            out_value->type = LED_DATA_ASSET_ID;
            le_asset_get_id(w == NULL ? NULL : le_world_get_engine(w),
                            &prop.asset, &id);
            {
                char hex[33];

                memset(hex, 0, sizeof(hex));
                le_asset_id_to_string(&id, hex);
                strncpy(out_value->asset_hex, hex,
                        sizeof(out_value->asset_hex) - 1);
            }
            break;
        }
        default:
            return 0;
        }
        return 1;
    }
    sp = led_find_static(path);
    if (sp == NULL) {
        return 0;
    }
    if (out_value == NULL) {
        return 1;
    }
    memset(out_value, 0, sizeof(*out_value));
    /* Dispatch per path (validated engine getters only). */
    if (strcmp(path, "object.name") == 0) {
        const char *nm = le_object_get_name(w, object);

        out_value->type = LED_DATA_STRING;
        strncpy(out_value->string_value, nm != NULL ? nm : "",
                sizeof(out_value->string_value) - 1);
        return 1;
    }
    if (strcmp(path, "object.enabled") == 0) {
        out_value->type = LED_DATA_BOOL;
        out_value->boolean = le_object_is_enabled(w, object);
        return 1;
    }
    if (strcmp(path, "transform.position") == 0) {
        out_value->type = LED_DATA_VEC3;
        le_object_get_position(w, object, out_value->vec3);
        return 1;
    }
    if (strcmp(path, "transform.rotation_quat") == 0) {
        out_value->type = LED_DATA_QUAT;
        le_object_get_rotation(w, object, out_value->quat);
        return 1;
    }
    if (strcmp(path, "transform.rotation_euler_deg") == 0) {
        float q[4];

        out_value->type = LED_DATA_EULER_DEG;
        le_object_get_rotation(w, object, q);
        led_quat_to_euler_deg(q, out_value->vec3);
        return 1;
    }
    if (strcmp(path, "transform.scale") == 0) {
        out_value->type = LED_DATA_VEC3;
        le_object_get_scale(w, object, out_value->vec3);
        return 1;
    }
    if (strcmp(path, "transform.parent") == 0) {
        le_object p = LE_OBJECT_INVALID;

        out_value->type = LED_DATA_STRING;
        if (le_object_get_parent(w, object, &p)) {
            const char *nm = le_object_get_name(w, &p);

            strncpy(out_value->string_value, nm != NULL ? nm : "",
                    sizeof(out_value->string_value) - 1);
        }
        return 1;
    }
    if (strncmp(path, "renderable.", 11) == 0) {
        le_renderable_desc d;
        int has_ptr = le_object_get_renderable(w, object, &d);

        if (strcmp(path, "renderable.visible") == 0 && has_ptr) {
            out_value->type = LED_DATA_BOOL;
            out_value->boolean = d.visible;
            return 1;
        }
        if (strcmp(path, "renderable.casts_shadow") == 0 && has_ptr) {
            out_value->type = LED_DATA_BOOL;
            out_value->boolean = d.casts_shadow;
            return 1;
        }
        if (strcmp(path, "renderable.receives_shadow") == 0 &&
            has_ptr) {
            out_value->type = LED_DATA_BOOL;
            out_value->boolean = d.receives_shadow;
            return 1;
        }
        if (strcmp(path, "renderable.mesh_asset") == 0) {
            /* Asset-backed spelling: report UNAVAILABLE (persistent
             * IDs are not renderer pointers; never guessed). */
            le_object_info2 info;

            memset(&info, 0, sizeof(info));
            le_object_get_info2(w, object, &info);
            if (info.has_asset_renderable) {
                out_value->type = LED_DATA_UNAVAILABLE;
                return 1;
            }
            if (has_ptr) {
                out_value->type = LED_DATA_UNAVAILABLE;
                return 1;
            }
            return 0;
        }
        return 0;
    }
    if (strncmp(path, "camera.", 7) == 0) {
        le_camera_desc d;

        if (!le_object_get_camera(w, object, &d)) {
            return 0;
        }
        if (strcmp(path, "camera.projection") == 0) {
            out_value->type = LED_DATA_ENUM;
            out_value->integer = (int64_t)d.projection;
            return 1;
        }
        if (strcmp(path, "camera.fov_y_deg") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.fov_y_rad * 180.0 / LED_PI;
            return 1;
        }
        if (strcmp(path, "camera.ortho_height") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.ortho_height;
            return 1;
        }
        if (strcmp(path, "camera.aspect") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.aspect;
            return 1;
        }
        if (strcmp(path, "camera.near") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.near_plane;
            return 1;
        }
        if (strcmp(path, "camera.far") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.far_plane;
            return 1;
        }
        return 0;
    }
    if (strncmp(path, "light.", 6) == 0) {
        le_light_desc d;

        if (!le_object_get_light(w, object, &d)) {
            return 0;
        }
        if (strcmp(path, "light.type") == 0) {
            out_value->type = LED_DATA_ENUM;
            out_value->integer = (int64_t)d.type;
            return 1;
        }
        if (strcmp(path, "light.color") == 0) {
            out_value->type = LED_DATA_COLOR3;
            memcpy(out_value->vec3, d.color, sizeof(d.color));
            return 1;
        }
        if (strcmp(path, "light.intensity") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.intensity;
            return 1;
        }
        if (strcmp(path, "light.range") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.range;
            return 1;
        }
        if (strcmp(path, "light.spot_inner_deg") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number =
                (double)d.spot_inner * 180.0 / LED_PI;
            return 1;
        }
        if (strcmp(path, "light.spot_outer_deg") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number =
                (double)d.spot_outer * 180.0 / LED_PI;
            return 1;
        }
        return 0;
    }
    if (strncmp(path, "rigidbody.", 10) == 0) {
        le_rigid_body_desc d;

        if (!le_object_get_rigid_body(w, object, &d)) {
            return 0;
        }
        if (strcmp(path, "rigidbody.type") == 0) {
            out_value->type = LED_DATA_ENUM;
            out_value->integer = (int64_t)d.type;
            return 1;
        }
        if (strcmp(path, "rigidbody.mass") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.mass;
            return 1;
        }
        if (strcmp(path, "rigidbody.linear_damping") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.linear_damping;
            return 1;
        }
        if (strcmp(path, "rigidbody.angular_damping") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.angular_damping;
            return 1;
        }
        if (strcmp(path, "rigidbody.gravity_scale") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.gravity_scale;
            return 1;
        }
        if (strcmp(path, "rigidbody.linear_velocity") == 0) {
            out_value->type = LED_DATA_VEC3;
            memcpy(out_value->vec3, d.linear_velocity,
                   sizeof(d.linear_velocity));
            return 1;
        }
        if (strcmp(path, "rigidbody.angular_velocity") == 0) {
            out_value->type = LED_DATA_VEC3;
            memcpy(out_value->vec3, d.angular_velocity,
                   sizeof(d.angular_velocity));
            return 1;
        }
        return 0;
    }
    if (strncmp(path, "collider.", 9) == 0) {
        le_collider_desc d;

        if (!le_object_get_collider(w, object, &d)) {
            return 0;
        }
        if (strcmp(path, "collider.shape") == 0) {
            out_value->type = LED_DATA_ENUM;
            out_value->integer = (int64_t)d.shape;
            return 1;
        }
        if (strcmp(path, "collider.radius") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.radius;
            return 1;
        }
        if (strcmp(path, "collider.half_extents") == 0) {
            out_value->type = LED_DATA_VEC3;
            memcpy(out_value->vec3, d.half_extents,
                   sizeof(d.half_extents));
            return 1;
        }
        if (strcmp(path, "collider.capsule_radius") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.capsule_radius;
            return 1;
        }
        if (strcmp(path, "collider.capsule_half_height") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.capsule_half_height;
            return 1;
        }
        if (strcmp(path, "collider.offset") == 0) {
            out_value->type = LED_DATA_VEC3;
            memcpy(out_value->vec3, d.offset, sizeof(d.offset));
            return 1;
        }
        if (strcmp(path, "collider.is_trigger") == 0) {
            out_value->type = LED_DATA_BOOL;
            out_value->boolean = d.is_trigger;
            return 1;
        }
        if (strcmp(path, "collider.layer") == 0) {
            out_value->type = LED_DATA_UINT;
            out_value->uinteger = d.layer;
            return 1;
        }
        if (strcmp(path, "collider.mask") == 0) {
            out_value->type = LED_DATA_UINT;
            out_value->uinteger = d.mask;
            return 1;
        }
        if (strcmp(path, "collider.friction") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.friction;
            return 1;
        }
        if (strcmp(path, "collider.restitution") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.restitution;
            return 1;
        }
        return 0;
    }
    if (strncmp(path, "animator.", 9) == 0) {
        /* Playback readouts come from the animator desc where the
         * engine exposes them; loop/speed/autorun via desc. */
        if (strcmp(path, "animator.playing") == 0) {
            out_value->type = LED_DATA_BOOL;
            out_value->boolean =
                le_anim_is_playing(w, object);
            return 1;
        }
        if (strcmp(path, "animator.time") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number =
                (double)le_anim_get_time(w, object);
            return 1;
        }
        {
            le_animator_desc d;

            if (!le_object_get_animator(w, object, &d)) {
                return 0;
            }
            if (strcmp(path, "animator.autoplay") == 0) {
                out_value->type = LED_DATA_BOOL;
                out_value->boolean = d.autoplay;
                return 1;
            }
            if (strcmp(path, "animator.loop") == 0) {
                out_value->type = LED_DATA_ENUM;
                out_value->integer = (int64_t)d.loop_mode;
                return 1;
            }
            if (strcmp(path, "animator.speed") == 0) {
                out_value->type = LED_DATA_FLOAT;
                out_value->number = (double)d.speed;
                return 1;
            }
            if (strcmp(path, "animator.start_time") == 0) {
                out_value->type = LED_DATA_FLOAT;
                out_value->number = (double)d.start_time;
                return 1;
            }
        }
        return 0;
    }
    if (strncmp(path, "character.", 10) == 0) {
        le_character_desc d;

        if (!le_object_get_character(w, object, &d)) {
            return 0;
        }
        if (strcmp(path, "character.radius") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.radius;
            return 1;
        }
        if (strcmp(path, "character.height") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.height;
            return 1;
        }
        if (strcmp(path, "character.skin_width") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.skin_width;
            return 1;
        }
        if (strcmp(path, "character.max_slope_deg") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number =
                (double)d.max_slope_angle * 180.0 / LED_PI;
            return 1;
        }
        if (strcmp(path, "character.step_height") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.step_height;
            return 1;
        }
        if (strcmp(path, "character.gravity") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.gravity;
            return 1;
        }
        if (strcmp(path, "character.terminal_velocity") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.terminal_velocity;
            return 1;
        }
        if (strcmp(path, "character.snap_distance") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.snap_distance;
            return 1;
        }
        if (strcmp(path, "character.push_strength") == 0) {
            out_value->type = LED_DATA_FLOAT;
            out_value->number = (double)d.push_strength;
            return 1;
        }
        if (strcmp(path, "character.layer") == 0) {
            out_value->type = LED_DATA_UINT;
            out_value->uinteger = d.layer;
            return 1;
        }
        if (strcmp(path, "character.mask") == 0) {
            out_value->type = LED_DATA_UINT;
            out_value->uinteger = d.mask;
            return 1;
        }
        return 0;
    }
    return 0;
}

led_result led_write_property(led_session *session,
                              const le_object *object,
                              const char *path,
                              const led_property_value *value) {
    const led_static_prop *sp;
    le_world *w;
    le_result rc;

    if (session == NULL || object == NULL || path == NULL ||
        value == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    w = session->edit_world;
    if (!le_object_is_alive(w, object)) {
        return LED_ERROR_STALE_HANDLE;
    }
    if (strncmp(path, "script.", 7) == 0) {
        le_script_property cur;
        le_script_property set;

        memset(&cur, 0, sizeof(cur));
        if (!le_script_get_property(w, object, path + 7, &cur)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        memset(&set, 0, sizeof(set));
        strncpy(set.name, cur.name, sizeof(set.name) - 1);
        set.type = cur.type;
        switch (cur.type) {
        case LE_SCRIPT_PROP_BOOL:
            if (value->type != LED_DATA_BOOL) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            set.boolean = value->boolean;
            break;
        case LE_SCRIPT_PROP_INT:
            if (value->type != LED_DATA_INT) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            set.integer = value->integer;
            break;
        case LE_SCRIPT_PROP_NUMBER:
            if (value->type != LED_DATA_FLOAT) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            if (value->number != value->number) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            set.number = value->number;
            break;
        case LE_SCRIPT_PROP_STRING:
            if (value->type != LED_DATA_STRING) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            strncpy(set.string_value, value->string_value,
                    sizeof(set.string_value) - 1);
            break;
        case LE_SCRIPT_PROP_VEC3:
            if (value->type != LED_DATA_VEC3) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            if (value->vec3[0] != value->vec3[0] ||
                value->vec3[1] != value->vec3[1] ||
                value->vec3[2] != value->vec3[2]) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            memcpy(set.vec3, value->vec3, sizeof(set.vec3));
            break;
        case LE_SCRIPT_PROP_ASSET:
            return LED_ERROR_INVALID_ARGUMENT;
        default:
            return LED_ERROR_INVALID_ARGUMENT;
        }
        rc = le_script_set_property(w, object, &set);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    sp = led_find_static(path);
    if (sp == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (sp->read_only) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (strcmp(path, "object.name") == 0) {
        if (value->type != LED_DATA_STRING) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        rc = le_object_set_name(w, object, value->string_value);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    if (strcmp(path, "object.enabled") == 0) {
        if (value->type != LED_DATA_BOOL) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        rc = le_object_set_enabled(w, object, value->boolean);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    if (strcmp(path, "transform.position") == 0) {
        float v[3];

        if (value->type != LED_DATA_VEC3) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        memcpy(v, value->vec3, sizeof(v));
        if (v[0] != v[0] || v[1] != v[1] || v[2] != v[2]) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        rc = le_object_set_position(w, object, v);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    if (strcmp(path, "transform.rotation_quat") == 0) {
        float q[4];

        if (value->type != LED_DATA_QUAT) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        memcpy(q, value->quat, sizeof(q));
        rc = le_object_set_rotation(w, object, q);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    if (strcmp(path, "transform.rotation_euler_deg") == 0) {
        float deg[3];
        float q[4];

        if (value->type != LED_DATA_EULER_DEG &&
            value->type != LED_DATA_VEC3) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        memcpy(deg, value->vec3, sizeof(deg));
        if (!led_euler_deg_to_quat(deg, q)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        rc = le_object_set_rotation(w, object, q);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    if (strcmp(path, "transform.scale") == 0) {
        float v[3];

        if (value->type != LED_DATA_VEC3) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        memcpy(v, value->vec3, sizeof(v));
        if (v[0] != v[0] || v[1] != v[1] || v[2] != v[2]) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        rc = le_object_set_scale(w, object, v);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    if (strncmp(path, "camera.", 7) == 0) {
        le_camera_desc d;

        if (!le_object_get_camera(w, object, &d)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        if (strcmp(path, "camera.projection") == 0) {
            if (value->type != LED_DATA_ENUM &&
                value->type != LED_DATA_INT) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            if (value->integer != 0 && value->integer != 1) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.projection = (value->integer == 0)
                               ? LE_PROJECTION_PERSPECTIVE
                               : LE_PROJECTION_ORTHOGRAPHIC;
        } else if (strcmp(path, "camera.fov_y_deg") == 0) {
            double deg;

            if (value->type != LED_DATA_FLOAT) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            if (!led_range_ok(sp, value->number)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            deg = value->number;
            d.fov_y_rad = (float)(deg * LED_PI / 180.0);
        } else if (strcmp(path, "camera.ortho_height") == 0) {
            if (value->type != LED_DATA_FLOAT ||
                !led_range_ok(sp, value->number)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.ortho_height = (float)value->number;
        } else if (strcmp(path, "camera.aspect") == 0) {
            if (value->type != LED_DATA_FLOAT ||
                !led_range_ok(sp, value->number)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.aspect = (float)value->number;
        } else if (strcmp(path, "camera.near") == 0) {
            if (value->type != LED_DATA_FLOAT ||
                !led_range_ok(sp, value->number)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.near_plane = (float)value->number;
        } else if (strcmp(path, "camera.far") == 0) {
            if (value->type != LED_DATA_FLOAT ||
                !led_range_ok(sp, value->number)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.far_plane = (float)value->number;
        } else {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        rc = le_object_add_camera(w, object, &d);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    if (strncmp(path, "light.", 6) == 0) {
        le_light_desc d;

        if (!le_object_get_light(w, object, &d)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        if (strcmp(path, "light.type") == 0) {
            if (value->type != LED_DATA_ENUM &&
                value->type != LED_DATA_INT) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            if (value->integer < 0 || value->integer > 2) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.type = (le_light_type)value->integer;
        } else if (strcmp(path, "light.color") == 0) {
            if (value->type != LED_DATA_COLOR3 &&
                value->type != LED_DATA_VEC3) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            memcpy(d.color, value->vec3, sizeof(d.color));
        } else if (strcmp(path, "light.intensity") == 0) {
            if (value->type != LED_DATA_FLOAT ||
                !led_range_ok(sp, value->number)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.intensity = (float)value->number;
        } else if (strcmp(path, "light.range") == 0) {
            if (value->type != LED_DATA_FLOAT ||
                !led_range_ok(sp, value->number)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.range = (float)value->number;
        } else if (strcmp(path, "light.spot_inner_deg") == 0) {
            if (value->type != LED_DATA_FLOAT ||
                !led_range_ok(sp, value->number)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.spot_inner = (float)(value->number * LED_PI / 180.0);
        } else if (strcmp(path, "light.spot_outer_deg") == 0) {
            if (value->type != LED_DATA_FLOAT ||
                !led_range_ok(sp, value->number)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.spot_outer = (float)(value->number * LED_PI / 180.0);
        } else {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        rc = le_object_add_light(w, object, &d);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    if (strncmp(path, "renderable.", 11) == 0) {
        le_renderable_desc d;

        if (!le_object_get_renderable(w, object, &d)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        if (strcmp(path, "renderable.visible") == 0) {
            if (value->type != LED_DATA_BOOL) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.visible = value->boolean;
        } else if (strcmp(path, "renderable.casts_shadow") == 0) {
            if (value->type != LED_DATA_BOOL) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.casts_shadow = value->boolean;
        } else if (strcmp(path, "renderable.receives_shadow") == 0) {
            if (value->type != LED_DATA_BOOL) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            d.receives_shadow = value->boolean;
        } else {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        rc = le_object_add_renderable(w, object, &d);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    /* rigidbody/collider/animator/character full-desc writes route
     * through commands (SET_* kinds) so they stay undoable; direct
     * single-field writes below cover the scalar fast paths. */
    if (strcmp(path, "rigidbody.mass") == 0) {
        le_rigid_body_desc d;

        if (!le_object_get_rigid_body(w, object, &d)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        if (value->type != LED_DATA_FLOAT ||
            !led_range_ok(sp, value->number)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        d.mass = (float)value->number;
        rc = le_object_add_rigid_body(w, object, &d);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    if (strcmp(path, "collider.is_trigger") == 0) {
        le_collider_desc d;

        if (!le_object_get_collider(w, object, &d)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        if (value->type != LED_DATA_BOOL) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        d.is_trigger = value->boolean;
        rc = le_object_add_collider(w, object, &d);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    if (strcmp(path, "animator.speed") == 0) {
        le_animator_desc d;

        if (!le_object_get_animator(w, object, &d)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        if (value->type != LED_DATA_FLOAT ||
            !led_range_ok(sp, value->number)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        d.speed = (float)value->number;
        rc = le_object_add_animator(w, object, &d);
        if (rc != LE_SUCCESS) {
            session->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    return LED_ERROR_INVALID_ARGUMENT;
}
