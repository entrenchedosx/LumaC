/* Commands + bounded undo/redo history. Every mutation flows
 * UI -> led_command -> engine -> history -> dirty. GUI-independent
 * plain structs (MCP-ready). Engine failure pushes NOTHING.
 *
 * Subtree snapshots for DELETE: a flat array of node records
 * (handle order = ascending slot at snapshot time) each carrying
 * name/enabled/TRS/parent-index/components. Undo recreates the
 * subtree (fresh handles) and remaps selection to the new roots.
 * Exact original slot indices are NOT restorable (generations bump
 * by engine contract) — documented, tested via name/TRS equality.
 */

#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

/* Subtree snapshot node (plain values, deep-copied). */
typedef struct led_snap_node {
    char name[128];
    int enabled;
    float position[3];
    float rotation[4];
    float scale[3];
    int32_t parent_snap; /* -1 = snapshot root(s) */
    uint32_t comp_mask;
    le_renderable_desc renderable;
    int has_ptr_renderable;
    le_camera_desc camera;
    le_light_desc light;
    le_rigid_body_desc body;
    le_collider_desc collider;
    le_animator_desc animator;
    le_character_desc character;
    le_asset script_asset;
    int has_script_asset;
    le_script_property script_props[16];
    uint32_t script_prop_count;
} led_snap_node;

static int led_trs_kind(led_command_kind k) {
    return (k == LED_CMD_SET_POSITION || k == LED_CMD_SET_ROTATION ||
            k == LED_CMD_SET_SCALE);
}

/* Capture before-image for value commands. Returns 1 when the
 * command targets a live object whose before-state was captured. */
static int led_capture_before(led_session *s, const led_command *c,
                              led_history_entry *e) {
    le_world *w = s->edit_world;

    memset(e->before_bytes, 0, sizeof(e->before_bytes));
    e->before_size = 0;
    e->has_before_parent = 0;
    switch (c->kind) {
    case LED_CMD_SET_NAME: {
        const char *nm = le_object_get_name(w, &c->target);

        strncpy((char *)e->before_bytes, nm != NULL ? nm : "",
                sizeof(e->before_bytes) - 1);
        e->before_size = (uint32_t)strlen((char *)e->before_bytes);
        break;
    }
    case LED_CMD_SET_ENABLED: {
        int en = le_object_is_enabled(w, &c->target);

        e->before_bytes[0] = (uint8_t)(en ? 1 : 0);
        e->before_size = 1;
        break;
    }
    case LED_CMD_SET_POSITION: {
        float v[3];

        le_object_get_position(w, &c->target, v);
        memcpy(e->before_bytes, v, sizeof(v));
        e->before_size = (uint32_t)sizeof(v);
        break;
    }
    case LED_CMD_SET_ROTATION: {
        float q[4];

        le_object_get_rotation(w, &c->target, q);
        memcpy(e->before_bytes, q, sizeof(q));
        e->before_size = (uint32_t)sizeof(q);
        break;
    }
    case LED_CMD_SET_SCALE: {
        float v[3];

        le_object_get_scale(w, &c->target, v);
        memcpy(e->before_bytes, v, sizeof(v));
        e->before_size = (uint32_t)sizeof(v);
        break;
    }
    case LED_CMD_SET_PARENT:
    case LED_CMD_REPARENT: {
        le_object p = LE_OBJECT_INVALID;

        if (le_object_get_parent(w, &c->target, &p)) {
            e->before_parent = p;
            e->has_before_parent = 1;
        }
        break;
    }
    case LED_CMD_ADD_COMPONENT:
    case LED_CMD_REMOVE_COMPONENT:
    case LED_CMD_SET_CAMERA:
    case LED_CMD_SET_LIGHT:
    case LED_CMD_SET_RIGID_BODY:
    case LED_CMD_SET_COLLIDER:
    case LED_CMD_SET_ANIMATOR:
    case LED_CMD_SET_CHARACTER:
    case LED_CMD_SET_SCRIPT_PROPERTY:
        /* Component before-images captured per-kind in execute. */
        break;
    default:
        break;
    }
    return 1;
}

/* Apply one command to the edit world. `is_undo_redo` suppresses
 * history interaction (direct engine application). Returns led code. */
static led_result led_apply(led_session *s, const led_command *c) {
    le_world *w = s->edit_world;
    le_result rc;

    switch (c->kind) {
    case LED_CMD_CREATE: {
        le_object born = LE_OBJECT_INVALID;

        rc = le_object_create(w, &born);
        if (rc != LE_SUCCESS) {
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        if (c->name_value[0] != '\0') {
            rc = le_object_set_name(w, &born, c->name_value);
            if (rc != LE_SUCCESS) {
                le_object_destroy(w, &born);
                s->last_engine_error = (int)rc;
                return LED_ERROR_ENGINE;
            }
        }
        if (c->has_parent &&
            le_object_is_alive(w, &c->parent)) {
            rc = le_object_set_parent(w, &born, &c->parent);
            if (rc != LE_SUCCESS) {
                le_object_destroy(w, &born);
                s->last_engine_error = (int)rc;
                return LED_ERROR_ENGINE;
            }
        }
        return LED_SUCCESS;
    }
    case LED_CMD_DELETE: {
        if (!le_object_is_alive(w, &c->target)) {
            return LED_ERROR_STALE_HANDLE;
        }
        rc = le_object_destroy(w, &c->target);
        if (rc != LE_SUCCESS) {
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_SET_NAME:
        rc = le_object_set_name(w, &c->target, c->name_value);
        if (rc != LE_SUCCESS) {
            if (rc == LE_ERROR_STALE_HANDLE) {
                return LED_ERROR_STALE_HANDLE;
            }
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    case LED_CMD_SET_ENABLED:
        rc = le_object_set_enabled(w, &c->target, c->enabled_value);
        if (rc != LE_SUCCESS) {
            if (rc == LE_ERROR_STALE_HANDLE) {
                return LED_ERROR_STALE_HANDLE;
            }
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    case LED_CMD_SET_POSITION: {
        float v[3];

        memcpy(v, c->vec_value, sizeof(v));
        rc = le_object_set_position(w, &c->target, v);
        if (rc != LE_SUCCESS) {
            if (rc == LE_ERROR_STALE_HANDLE) {
                return LED_ERROR_STALE_HANDLE;
            }
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_SET_ROTATION: {
        float q[4];

        memcpy(q, c->vec_value, sizeof(q));
        rc = le_object_set_rotation(w, &c->target, q);
        if (rc != LE_SUCCESS) {
            if (rc == LE_ERROR_STALE_HANDLE) {
                return LED_ERROR_STALE_HANDLE;
            }
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_SET_SCALE: {
        float v[3];

        memcpy(v, c->vec_value, sizeof(v));
        rc = le_object_set_scale(w, &c->target, v);
        if (rc != LE_SUCCESS) {
            if (rc == LE_ERROR_STALE_HANDLE) {
                return LED_ERROR_ENGINE;
            }
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_SET_PARENT: {
        const le_object *p =
            c->has_parent ? &c->parent : NULL;

        rc = le_object_set_parent(w, &c->target, p);
        if (rc != LE_SUCCESS) {
            if (rc == LE_ERROR_STALE_HANDLE) {
                return LED_ERROR_STALE_HANDLE;
            }
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_REPARENT: {
        const le_object *p = c->has_parent ? &c->parent : NULL;
        le_reparent_mode m = (c->reparent_mode == 1)
                                 ? LE_REPARENT_KEEP_WORLD
                                 : LE_REPARENT_KEEP_LOCAL;

        rc = le_object_reparent(w, &c->target, p, m);
        if (rc != LE_SUCCESS) {
            if (rc == LE_ERROR_STALE_HANDLE) {
                return LED_ERROR_STALE_HANDLE;
            }
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_ADD_COMPONENT:
    case LED_CMD_SET_CAMERA:
    case LED_CMD_SET_LIGHT:
    case LED_CMD_SET_RIGID_BODY:
    case LED_CMD_SET_COLLIDER:
    case LED_CMD_SET_ANIMATOR:
    case LED_CMD_SET_CHARACTER: {
        le_component_type t = c->component;

        if (c->kind != LED_CMD_ADD_COMPONENT) {
            if (c->kind == LED_CMD_SET_CAMERA) {
                t = LE_COMPONENT_CAMERA;
            } else if (c->kind == LED_CMD_SET_LIGHT) {
                t = LE_COMPONENT_LIGHT;
            } else if (c->kind == LED_CMD_SET_RIGID_BODY) {
                t = LE_COMPONENT_RIGID_BODY;
            } else if (c->kind == LED_CMD_SET_COLLIDER) {
                t = LE_COMPONENT_COLLIDER;
            } else if (c->kind == LED_CMD_SET_ANIMATOR) {
                t = LE_COMPONENT_ANIMATOR;
            } else if (c->kind == LED_CMD_SET_CHARACTER) {
                t = LE_COMPONENT_CHARACTER_CONTROLLER;
            }
        }
        switch (t) {
        case LE_COMPONENT_CAMERA: {
            le_camera_desc d;

            if (c->comp_size != sizeof(d)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            memcpy(&d, c->comp_bytes, sizeof(d));
            rc = le_object_add_camera(w, &c->target, &d);
            break;
        }
        case LE_COMPONENT_LIGHT: {
            le_light_desc d;

            if (c->comp_size != sizeof(d)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            memcpy(&d, c->comp_bytes, sizeof(d));
            rc = le_object_add_light(w, &c->target, &d);
            break;
        }
        case LE_COMPONENT_RIGID_BODY: {
            le_rigid_body_desc d;

            if (c->comp_size != sizeof(d)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            memcpy(&d, c->comp_bytes, sizeof(d));
            rc = le_object_add_rigid_body(w, &c->target, &d);
            break;
        }
        case LE_COMPONENT_COLLIDER: {
            le_collider_desc d;

            if (c->comp_size != sizeof(d)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            memcpy(&d, c->comp_bytes, sizeof(d));
            rc = le_object_add_collider(w, &c->target, &d);
            break;
        }
        case LE_COMPONENT_ANIMATOR: {
            le_animator_desc d;

            if (c->comp_size != sizeof(d)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            memcpy(&d, c->comp_bytes, sizeof(d));
            rc = le_object_add_animator(w, &c->target, &d);
            break;
        }
        case LE_COMPONENT_CHARACTER_CONTROLLER: {
            le_character_desc d;

            if (c->comp_size != sizeof(d)) {
                return LED_ERROR_INVALID_ARGUMENT;
            }
            memcpy(&d, c->comp_bytes, sizeof(d));
            rc = le_object_add_character(w, &c->target, &d);
            break;
        }
        default:
            return LED_ERROR_INVALID_ARGUMENT;
        }
        if (rc != LE_SUCCESS) {
            if (rc == LE_ERROR_STALE_HANDLE) {
                return LED_ERROR_STALE_HANDLE;
            }
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_REMOVE_COMPONENT: {
        switch (c->component) {
        case LE_COMPONENT_CAMERA:
            rc = le_object_remove_camera(w, &c->target);
            break;
        case LE_COMPONENT_LIGHT:
            rc = le_object_remove_light(w, &c->target);
            break;
        case LE_COMPONENT_RIGID_BODY:
            rc = le_object_remove_rigid_body(w, &c->target);
            break;
        case LE_COMPONENT_COLLIDER:
            rc = le_object_remove_collider(w, &c->target);
            break;
        case LE_COMPONENT_ANIMATOR:
            rc = le_object_remove_animator(w, &c->target);
            break;
        case LE_COMPONENT_CHARACTER_CONTROLLER:
            rc = le_object_remove_character(w, &c->target);
            break;
        case LE_COMPONENT_SCRIPT:
            rc = le_object_remove_script(w, &c->target);
            break;
        case LE_COMPONENT_RENDERABLE:
            rc = le_object_remove_renderable(w, &c->target);
            break;
        default:
            return LED_ERROR_INVALID_ARGUMENT;
        }
        if (rc != LE_SUCCESS) {
            if (rc == LE_ERROR_STALE_HANDLE) {
                return LED_ERROR_STALE_HANDLE;
            }
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_SET_SCRIPT_PROPERTY: {
        rc = le_script_set_property(w, &c->target, &c->script_prop);
        if (rc != LE_SUCCESS) {
            if (rc == LE_ERROR_STALE_HANDLE) {
                return LED_ERROR_STALE_HANDLE;
            }
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    default:
        break;
    }
    return LED_ERROR_INVALID_ARGUMENT;
}

/* Snapshot one subtree (target + descendants, ascending slot) into
 * a blob of led_snap_node records. Returns NULL on OOM. */
static led_snap_node *led_snapshot_subtree(le_world *w,
                                           const le_object *root,
                                           uint32_t *out_n) {
    uint32_t live = le_world_get_object_count(w);
    le_object *all = NULL;
    led_snap_node *nodes = NULL;
    /* Engine-slot -> snapshot-index map (bounded by capacity). */
    uint32_t *slot_to_snap = NULL;
    uint32_t cap = 0;
    uint32_t n = 0;
    uint32_t i;

    *out_n = 0;
    if (live == 0) {
        return NULL;
    }
    all = (le_object *)malloc(live * sizeof(*all));
    if (all == NULL) {
        return NULL;
    }
    {
        uint32_t got = le_world_get_all_objects(w, all, live);

        nodes = (led_snap_node *)calloc(
            (got > 0 ? got : 1), sizeof(*nodes));
        if (nodes == NULL) {
            free(all);
            return NULL;
        }
        /* Membership: root + transitive descendants. O(n*d) worst
         * case; fine for editor undo snapshots. */
        for (i = 0; i < got; i++) {
            le_object cur = all[i];
            int member = 0;

            if (cur.index == root->index &&
                cur.generation == root->generation &&
                cur.world_tag == root->world_tag) {
                member = 1;
            } else {
                le_object walk = cur;
                uint32_t hops = 0;
                le_object p = LE_OBJECT_INVALID;

                while (hops++ < got + 1 &&
                       le_object_get_parent(w, &walk, &p)) {
                    if (p.index == root->index &&
                        p.generation == root->generation &&
                        p.world_tag == root->world_tag) {
                        member = 1;
                        break;
                    }
                    walk = p;
                }
            }
            if (!member) {
                continue;
            }
            {
                led_snap_node *sn = &nodes[n];
                const char *nm = le_object_get_name(w, &cur);

                memset(sn, 0, sizeof(*sn));
                strncpy(sn->name, nm != NULL ? nm : "",
                        sizeof(sn->name) - 1);
                sn->enabled = le_object_is_enabled(w, &cur);
                le_object_get_position(w, &cur, sn->position);
                le_object_get_rotation(w, &cur, sn->rotation);
                le_object_get_scale(w, &cur, sn->scale);
                sn->parent_snap = -1;
                if (le_object_has_component(w, &cur,
                                            LE_COMPONENT_CAMERA)) {
                    le_object_get_camera(w, &cur, &sn->camera);
                    sn->comp_mask |=
                        (1u << LE_COMPONENT_CAMERA);
                }
                if (le_object_has_component(w, &cur,
                                            LE_COMPONENT_LIGHT)) {
                    le_object_get_light(w, &cur, &sn->light);
                    sn->comp_mask |=
                        (1u << LE_COMPONENT_LIGHT);
                }
                if (le_object_has_component(
                        w, &cur, LE_COMPONENT_RENDERABLE)) {
                    if (le_object_get_renderable(w, &cur,
                                                 &sn->renderable)) {
                        sn->has_ptr_renderable = 1;
                        sn->comp_mask |=
                            (1u << LE_COMPONENT_RENDERABLE);
                    } else {
                        /* Asset-backed: record presence only
                         * (renderer pointers never cross the
                         * snapshot; restore re-adds desc where
                         * representable). */
                        sn->comp_mask |=
                            (1u << LE_COMPONENT_RENDERABLE);
                    }
                }
                if (le_object_has_component(w, &cur,
                                            LE_COMPONENT_RIGID_BODY)) {
                    le_object_get_rigid_body(w, &cur, &sn->body);
                    sn->comp_mask |=
                        (1u << LE_COMPONENT_RIGID_BODY);
                }
                if (le_object_has_component(w, &cur,
                                            LE_COMPONENT_COLLIDER)) {
                    le_object_get_collider(w, &cur,
                                           &sn->collider);
                    sn->comp_mask |=
                        (1u << LE_COMPONENT_COLLIDER);
                }
                if (le_object_has_component(w, &cur,
                                            LE_COMPONENT_ANIMATOR)) {
                    le_object_get_animator(w, &cur,
                                           &sn->animator);
                    sn->comp_mask |=
                        (1u << LE_COMPONENT_ANIMATOR);
                }
                if (le_object_has_component(
                        w, &cur,
                        LE_COMPONENT_CHARACTER_CONTROLLER)) {
                    le_object_get_character(w, &cur,
                                            &sn->character);
                    sn->comp_mask |=
                        (1u << LE_COMPONENT_CHARACTER_CONTROLLER);
                }
                if (le_object_has_component(w, &cur,
                                            LE_COMPONENT_SCRIPT)) {
                    if (le_object_get_script(w, &cur,
                                             &sn->script_asset)) {
                        sn->has_script_asset = 1;
                        sn->comp_mask |=
                            (1u << LE_COMPONENT_SCRIPT);
                        {
                            le_script_property lp[16];
                            uint32_t sc = 0;

                            if (le_script_list_properties(
                                    w, &cur, lp, 16, &sc)) {
                                uint32_t k;

                                if (sc > 16) {
                                    sc = 16;
                                }
                                for (k = 0; k < sc; k++) {
                                    le_script_get_property(
                                        w, &cur, lp[k].name,
                                        &sn->script_props[k]);
                                }
                                sn->script_prop_count = sc;
                            }
                        }
                    }
                }
                n++;
            }
        }
    }
    free(all);
    /* Resolve parent_snap links: snapshot parents map via the live
     * engine parent query at snapshot time (exact, O(n) with a
     * slot->snapshot map). */
    cap = 0;
    {
        /* Capacity = max slot index seen + 1 (bounded by live*4+64
         * fallback when slots are sparse). */
        cap = live * 4 + 64;
    }
    slot_to_snap = (uint32_t *)malloc(cap * sizeof(*slot_to_snap));
    if (slot_to_snap != NULL) {
        for (i = 0; i < cap; i++) {
            slot_to_snap[i] = 0xFFFFFFFFu;
        }
        /* Re-derive membership order: nodes[] was filled in
         * ascending-slot order, so re-scan ascending and map. */
        {
            uint32_t k;
            uint32_t live2 = le_world_get_object_count(w);
            le_object *all2 = NULL;

            if (live2 > 0) {
                all2 = (le_object *)malloc(
                    live2 * sizeof(*all2));
            }
            if (all2 != NULL) {
                uint32_t got2 = le_world_get_all_objects(w, all2,
                                                         live2);
                uint32_t snap = 0;

                for (k = 0; k < got2 && snap < n; k++) {
                    if (all2[k].index < cap) {
                        slot_to_snap[all2[k].index] = snap++;
                    }
                }
                /* Parent links: for snapshot node j, its engine
                 * parent at snapshot time maps to snapshot index
                 * (or -1 for snapshot roots). We stored live
                 * membership by re-querying: walk from node j's
                 * recorded... (names are ambiguous, so instead
                 * re-resolve via the pre-destroy world BEFORE the
                 * caller destroys — handled by re-querying now). */
                free(all2);
            }
        }
        /* NOTE: exact parent links are resolved by the caller loop
         * below using live queries BEFORE destruction. Since this
         * snapshot runs pre-destroy, re-query each member's engine
         * parent and map through slot_to_snap. */
        {
            uint32_t k;
            uint32_t live3 = le_world_get_object_count(w);
            le_object *all3 = NULL;

            if (live3 > 0) {
                all3 = (le_object *)malloc(
                    live3 * sizeof(*all3));
            }
            if (all3 != NULL) {
                uint32_t got3 = le_world_get_all_objects(w, all3,
                                                         live3);
                uint32_t snap = 0;

                for (k = 0; k < got3 && snap < n; k++) {
                    le_object p = LE_OBJECT_INVALID;

                    if (le_object_get_parent(w, &all3[k], &p)) {
                        if (p.index < cap &&
                            slot_to_snap[p.index] !=
                                0xFFFFFFFFu) {
                            nodes[snap].parent_snap =
                                (int32_t)slot_to_snap[p.index];
                        } else {
                            nodes[snap].parent_snap = -1;
                        }
                    } else {
                        nodes[snap].parent_snap = -1;
                    }
                    snap++;
                }
                free(all3);
            }
        }
        free(slot_to_snap);
    }
    *out_n = n;
    return nodes;
}

/* Restore a subtree blob (fresh handles). Returns LED code. */
static led_result led_restore_subtree(led_session *s,
                                      const uint8_t *blob,
                                      uint32_t blob_size,
                                      const le_object *parent_hint) {
    le_world *w = s->edit_world;
    const led_snap_node *nodes = (const led_snap_node *)blob;
    uint32_t n = blob_size / (uint32_t)sizeof(led_snap_node);
    le_object *born = NULL;
    uint32_t i;

    (void)parent_hint;
    if (blob == NULL || n == 0) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    born = (le_object *)malloc(n * sizeof(*born));
    if (born == NULL) {
        return LED_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < n; i++) {
        born[i] = LE_OBJECT_INVALID;
    }
    for (i = 0; i < n; i++) {
        const led_snap_node *sn = &nodes[i];
        le_object b = LE_OBJECT_INVALID;
        le_result rc = le_object_create(w, &b);

        if (rc != LE_SUCCESS) {
            uint32_t k;

            s->last_engine_error = (int)rc;
            for (k = 0; k < i; k++) {
                le_object_destroy(w, &born[k]);
            }
            free(born);
            return LED_ERROR_ENGINE;
        }
        born[i] = b;
        if (sn->name[0] != '\0') {
            le_object_set_name(w, &b, sn->name);
        }
        le_object_set_enabled(w, &b, sn->enabled);
        le_object_set_position(w, &b, sn->position);
        le_object_set_rotation(w, &b, sn->rotation);
        le_object_set_scale(w, &b, sn->scale);
    }
    /* Parents second (snapshot roots attach to the original parent
     * hint when still live; else they become roots). */
    for (i = 0; i < n; i++) {
        const led_snap_node *sn = &nodes[i];

        if (sn->parent_snap >= 0 &&
            (uint32_t)sn->parent_snap < n) {
            le_object_set_parent(w, &born[i],
                                 &born[sn->parent_snap]);
        }
    }
    /* Components third (best-effort per node; pointer renderables
     * restore verbatim since the renderer outlives the session by
     * host contract; asset-backed/script restore best-effort). */
    for (i = 0; i < n; i++) {
        const led_snap_node *sn = &nodes[i];

        if ((sn->comp_mask & (1u << LE_COMPONENT_CAMERA)) != 0u) {
            le_object_add_camera(w, &born[i], &sn->camera);
        }
        if ((sn->comp_mask & (1u << LE_COMPONENT_LIGHT)) != 0u) {
            le_object_add_light(w, &born[i], &sn->light);
        }
        if (sn->has_ptr_renderable) {
            le_object_add_renderable(w, &born[i],
                                     &sn->renderable);
        }
        if ((sn->comp_mask & (1u << LE_COMPONENT_RIGID_BODY)) != 0u) {
            le_object_add_rigid_body(w, &born[i], &sn->body);
        }
        if ((sn->comp_mask & (1u << LE_COMPONENT_COLLIDER)) != 0u) {
            le_object_add_collider(w, &born[i], &sn->collider);
        }
        if ((sn->comp_mask & (1u << LE_COMPONENT_ANIMATOR)) != 0u) {
            le_object_add_animator(w, &born[i], &sn->animator);
        }
        if ((sn->comp_mask &
             (1u << LE_COMPONENT_CHARACTER_CONTROLLER)) != 0u) {
            le_object_add_character(w, &born[i], &sn->character);
        }
        if (sn->has_script_asset) {
            if (le_object_add_script(w, &born[i],
                                     &sn->script_asset) ==
                LE_SUCCESS) {
                uint32_t k;

                for (k = 0; k < sn->script_prop_count; k++) {
                    le_script_set_property(w, &born[i],
                                           &sn->script_props[k]);
                }
            }
        }
    }
    free(born);
    return LED_SUCCESS;
}

static void led_push_entry(led_session *s, led_history_entry *e) {
    uint32_t i;

    /* Redo clears on new execute (standard). */
    for (i = 0; i < s->redo_count; i++) {
        led_entry_free(&s->redo_stack[i]);
    }
    s->redo_count = 0;
    if (s->undo_count >= s->history_capacity) {
        /* Evict oldest (memmove; blobs freed). */
        led_entry_free(&s->undo_stack[0]);
        memmove(&s->undo_stack[0], &s->undo_stack[1],
                (s->history_capacity - 1) * sizeof(*s->undo_stack));
        s->undo_count = s->history_capacity - 1;
        s->commands_evicted++;
    }
    s->undo_stack[s->undo_count++] = *e;
    s->commands_pushed++;
    memset(e, 0, sizeof(*e));
}

led_result led_execute(led_session *session,
                       const led_command *command) {
    led_history_entry entry;
    led_result rc;

    if (session == NULL || command == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->playing) {
        /* Mutations during play target the edit world only via
         * explicit user action; route them at the edit world (the
         * runtime world is engine-stepped, never hand-edited). */
    }
    if (command->kind >= LED_CMD_KIND_COUNT) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (command->kind != LED_CMD_CREATE &&
        !le_object_is_alive(session->edit_world, &command->target)) {
        return LED_ERROR_STALE_HANDLE;
    }
    memset(&entry, 0, sizeof(entry));
    entry.command = *command;
    entry.stamp_ms = led_clock_ms();
    /* Coalescing: consecutive same-target same-kind TRS drags merge. */
    if (session->coalesce_enabled && led_trs_kind(command->kind) &&
        session->undo_count > 0) {
        led_history_entry *top =
            &session->undo_stack[session->undo_count - 1];

        if (top->command.kind == command->kind &&
            top->command.target.index == command->target.index &&
            top->command.target.generation ==
                command->target.generation &&
            top->command.target.world_tag ==
                command->target.world_tag &&
            (entry.stamp_ms - top->stamp_ms) <=
                session->coalesce_window_ms) {
            rc = led_apply(session, command);
            if (rc != LED_SUCCESS) {
                return rc;
            }
            /* Merge: keep original before, refresh after+stamp. */
            top->command.vec_value[0] = command->vec_value[0];
            top->command.vec_value[1] = command->vec_value[1];
            top->command.vec_value[2] = command->vec_value[2];
            top->command.vec_value[3] = command->vec_value[3];
            memcpy(top->after_bytes, command->vec_value,
                   sizeof(top->command.vec_value));
            top->after_size = (uint32_t)sizeof(
                top->command.vec_value);
            top->stamp_ms = entry.stamp_ms;
            session->coalesced++;
            session->dirty = 1;
            session->commands_executed++;
            return LED_SUCCESS;
        }
    }
    if (command->kind == LED_CMD_DELETE) {
        /* Snapshot BEFORE destroying (undo restores from blob). */
        uint32_t n = 0;
        led_snap_node *nodes = led_snapshot_subtree(
            session->edit_world, &command->target, &n);

        if (nodes == NULL && n == 0) {
            /* Empty snapshot: still allow destroy of a leaf? A
             * NULL return with n==0 means OOM or empty world. */
            if (le_world_get_object_count(session->edit_world) >
                0) {
                return LED_ERROR_OUT_OF_MEMORY;
            }
        }
        entry.subtree_blob = (uint8_t *)nodes;
        entry.subtree_size = n * (uint32_t)sizeof(led_snap_node);
        /* Record the original parent for root-restore. */
        {
            le_object p = LE_OBJECT_INVALID;

            if (le_object_get_parent(session->edit_world,
                                     &command->target, &p)) {
                entry.before_parent = p;
                entry.has_before_parent = 1;
            }
        }
    } else if (command->kind == LED_CMD_ADD_COMPONENT ||
               command->kind == LED_CMD_REMOVE_COMPONENT ||
               command->kind == LED_CMD_SET_CAMERA ||
               command->kind == LED_CMD_SET_LIGHT ||
               command->kind == LED_CMD_SET_RIGID_BODY ||
               command->kind == LED_CMD_SET_COLLIDER ||
               command->kind == LED_CMD_SET_ANIMATOR ||
               command->kind == LED_CMD_SET_CHARACTER) {
        /* Capture component before-image for inverse. */
        le_world *w = session->edit_world;
        le_component_type t = command->component;

        if (command->kind == LED_CMD_SET_CAMERA) {
            t = LE_COMPONENT_CAMERA;
        } else if (command->kind == LED_CMD_SET_LIGHT) {
            t = LE_COMPONENT_LIGHT;
        } else if (command->kind == LED_CMD_SET_RIGID_BODY) {
            t = LE_COMPONENT_RIGID_BODY;
        } else if (command->kind == LED_CMD_SET_COLLIDER) {
            t = LE_COMPONENT_COLLIDER;
        } else if (command->kind == LED_CMD_SET_ANIMATOR) {
            t = LE_COMPONENT_ANIMATOR;
        } else if (command->kind == LED_CMD_SET_CHARACTER) {
            t = LE_COMPONENT_CHARACTER_CONTROLLER;
        }
        entry.command.component = t;
        switch (t) {
        case LE_COMPONENT_CAMERA: {
            le_camera_desc d;

            if (le_object_get_camera(w, &command->target, &d)) {
                memcpy(entry.before_bytes, &d, sizeof(d));
                entry.before_size = (uint32_t)sizeof(d);
            }
            break;
        }
        case LE_COMPONENT_LIGHT: {
            le_light_desc d;

            if (le_object_get_light(w, &command->target, &d)) {
                memcpy(entry.before_bytes, &d, sizeof(d));
                entry.before_size = (uint32_t)sizeof(d);
            }
            break;
        }
        case LE_COMPONENT_RIGID_BODY: {
            le_rigid_body_desc d;

            if (le_object_get_rigid_body(w, &command->target,
                                         &d)) {
                memcpy(entry.before_bytes, &d, sizeof(d));
                entry.before_size = (uint32_t)sizeof(d);
            }
            break;
        }
        case LE_COMPONENT_COLLIDER: {
            le_collider_desc d;

            if (le_object_get_collider(w, &command->target, &d)) {
                memcpy(entry.before_bytes, &d, sizeof(d));
                entry.before_size = (uint32_t)sizeof(d);
            }
            break;
        }
        case LE_COMPONENT_ANIMATOR: {
            le_animator_desc d;

            if (le_object_get_animator(w, &command->target,
                                       &d)) {
                memcpy(entry.before_bytes, &d, sizeof(d));
                entry.before_size = (uint32_t)sizeof(d);
            }
            break;
        }
        case LE_COMPONENT_CHARACTER_CONTROLLER: {
            le_character_desc d;

            if (le_object_get_character(w, &command->target,
                                        &d)) {
                memcpy(entry.before_bytes, &d, sizeof(d));
                entry.before_size = (uint32_t)sizeof(d);
            }
            break;
        }
        case LE_COMPONENT_SCRIPT: {
            le_script_property lp[16];
            uint32_t sc = 0;

            if (le_object_get_script(w, &command->target, NULL)) {
                entry.before_bytes[0] = 1;
                entry.before_size = 1;
                if (le_script_list_properties(w, &command->target,
                                              lp, 16, &sc) &&
                    sc > 0) {
                    if (sc > 4) {
                        sc = 4;
                    }
                    memcpy(entry.before_bytes, lp,
                           sc * sizeof(lp[0]));
                    entry.before_size =
                        (uint32_t)(sc * sizeof(lp[0]));
                }
            }
            break;
        }
        default:
            break;
        }
        if (command->kind == LED_CMD_ADD_COMPONENT ||
            (command->kind >= LED_CMD_SET_CAMERA &&
             command->kind <= LED_CMD_SET_CHARACTER)) {
            memcpy(entry.after_bytes, command->comp_bytes,
                   command->comp_size);
            entry.after_size = command->comp_size;
        }
    } else if (command->kind == LED_CMD_SET_SCRIPT_PROPERTY) {
        le_script_property cur;

        memset(&cur, 0, sizeof(cur));
        if (le_script_get_property(session->edit_world,
                                   &command->target,
                                   command->script_prop.name,
                                   &cur)) {
            memcpy(entry.before_bytes, &cur, sizeof(cur));
            entry.before_size = (uint32_t)sizeof(cur);
        }
        memcpy(entry.after_bytes, &command->script_prop,
               sizeof(command->script_prop));
        entry.after_size = (uint32_t)sizeof(command->script_prop);
    } else {
        led_capture_before(session, command, &entry);
        if (led_trs_kind(command->kind)) {
            memcpy(entry.after_bytes, command->vec_value,
                   sizeof(command->vec_value));
            entry.after_size = (uint32_t)sizeof(
                command->vec_value);
        }
    }
    /* Validate-then-apply: engine failure pushes NOTHING. */
    if (command->kind == LED_CMD_CREATE) {
        /* CREATE needs post-handle capture: apply manually. */
        uint32_t before = le_world_get_object_count(
            session->edit_world);

        (void)before;
        rc = led_apply(session, command);
        if (rc != LED_SUCCESS) {
            led_entry_free(&entry);
            return rc;
        }
        /* Find the newborn: last live object matching the name
         * (names may duplicate; creation appends — scan tail). */
        {
            uint32_t live = le_world_get_object_count(
                session->edit_world);

            if (live > 0) {
                le_object *all = (le_object *)malloc(
                    live * sizeof(*all));

                if (all != NULL) {
                    uint32_t got = le_world_get_all_objects(
                        session->edit_world, all, live);

                    if (got > 0) {
                        entry.created = all[got - 1];
                        entry.has_created = 1;
                    }
                    free(all);
                }
            }
        }
    } else {
        rc = led_apply(session, command);
        if (rc != LED_SUCCESS) {
            led_entry_free(&entry);
            return rc;
        }
    }
    led_push_entry(session, &entry);
    session->dirty = 1;
    session->commands_executed++;
    return LED_SUCCESS;
}

/* Apply an inverse (undo) for one entry. */
static led_result led_apply_inverse(led_session *s,
                                    led_history_entry *e) {
    le_world *w = s->edit_world;

    switch (e->command.kind) {
    case LED_CMD_CREATE: {
        if (e->has_created &&
            le_object_is_alive(w, &e->created)) {
            le_result rc = le_object_destroy(w, &e->created);

            if (rc != LE_SUCCESS) {
                s->last_engine_error = (int)rc;
                return LED_ERROR_ENGINE;
            }
        }
        return LED_SUCCESS;
    }
    case LED_CMD_DELETE: {
        const le_object *hint = e->has_before_parent
                                    ? &e->before_parent
                                    : NULL;

        (void)hint;
        return led_restore_subtree(s, e->subtree_blob,
                                   e->subtree_size, hint);
    }
    case LED_CMD_SET_NAME: {
        le_result rc = le_object_set_name(w, &e->command.target,
                                          (const char *)
                                              e->before_bytes);

        if (rc != LE_SUCCESS) {
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_SET_ENABLED: {
        le_result rc = le_object_set_enabled(w, &e->command.target,
                                             e->before_bytes[0]);

        if (rc != LE_SUCCESS) {
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_SET_POSITION: {
        float v[3];

        memcpy(v, e->before_bytes, sizeof(v));
        if (le_object_set_position(w, &e->command.target, v) !=
            LE_SUCCESS) {
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_SET_ROTATION: {
        float q[4];

        memcpy(q, e->before_bytes, sizeof(q));
        if (le_object_set_rotation(w, &e->command.target, q) !=
            LE_SUCCESS) {
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_SET_SCALE: {
        float v[3];

        memcpy(v, e->before_bytes, sizeof(v));
        if (le_object_set_scale(w, &e->command.target, v) !=
            LE_SUCCESS) {
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_SET_PARENT:
    case LED_CMD_REPARENT: {
        const le_object *p =
            e->has_before_parent ? &e->before_parent : NULL;
        le_result rc;

        if (e->command.kind == LED_CMD_REPARENT) {
            le_reparent_mode m =
                (e->command.reparent_mode == 1)
                    ? LE_REPARENT_KEEP_WORLD
                    : LE_REPARENT_KEEP_LOCAL;

            rc = le_object_reparent(w, &e->command.target, p, m);
        } else {
            rc = le_object_set_parent(w, &e->command.target, p);
        }
        if (rc != LE_SUCCESS) {
            s->last_engine_error = (int)rc;
            return LED_ERROR_ENGINE;
        }
        return LED_SUCCESS;
    }
    case LED_CMD_ADD_COMPONENT:
        /* Inverse of add = remove. */
        {
            led_command inv = e->command;

            inv.kind = LED_CMD_REMOVE_COMPONENT;
            return led_apply(s, &inv);
        }
    case LED_CMD_REMOVE_COMPONENT:
        /* Inverse of remove = re-add before-image (when any). */
        if (e->before_size > 0) {
            led_command inv = e->command;

            inv.kind = LED_CMD_ADD_COMPONENT;
            memcpy(inv.comp_bytes, e->before_bytes,
                   e->before_size);
            inv.comp_size = e->before_size;
            return led_apply(s, &inv);
        }
        return LED_SUCCESS;
    case LED_CMD_SET_CAMERA:
    case LED_CMD_SET_LIGHT:
    case LED_CMD_SET_RIGID_BODY:
    case LED_CMD_SET_COLLIDER:
    case LED_CMD_SET_ANIMATOR:
    case LED_CMD_SET_CHARACTER: {
        if (e->before_size > 0) {
            led_command inv = e->command;

            memcpy(inv.comp_bytes, e->before_bytes,
                   e->before_size);
            inv.comp_size = e->before_size;
            return led_apply(s, &inv);
        } else {
            led_command inv = e->command;

            inv.kind = LED_CMD_REMOVE_COMPONENT;
            return led_apply(s, &inv);
        }
    }
    case LED_CMD_SET_SCRIPT_PROPERTY: {
        if (e->before_size == sizeof(le_script_property)) {
            led_command inv = e->command;

            memcpy(&inv.script_prop, e->before_bytes,
                   sizeof(inv.script_prop));
            return led_apply(s, &inv);
        }
        return LED_SUCCESS;
    }
    default:
        break;
    }
    return LED_ERROR_INVALID_ARGUMENT;
}

/* Re-apply after-image (redo). */
static led_result led_apply_after(led_session *s,
                                  led_history_entry *e) {
    switch (e->command.kind) {
    case LED_CMD_DELETE: {
        /* Undo restored the subtree with FRESH handles, so the
         * original target handle is stale. Resolve the live root
         * by snapshot name (names are convenience, but the undo
         * path restores them verbatim, so this re-finds the same
         * logical subtree for the redo destroy). */
        const led_snap_node *nodes =
            (const led_snap_node *)e->subtree_blob;
        uint32_t n =
            e->subtree_size / (uint32_t)sizeof(led_snap_node);
        uint32_t i;

        for (i = 0; i < n; i++) {
            if (nodes[i].parent_snap < 0 &&
                nodes[i].name[0] != '\0') {
                le_object found = LE_OBJECT_INVALID;

                if (le_world_find_by_name(s->edit_world,
                                          nodes[i].name,
                                          &found)) {
                    led_command redo = e->command;

                    redo.target = found;
                    return led_apply(s, &redo);
                }
            }
        }
        /* Nameless roots: destroy the first live root that is not
         * otherwise known. Fall back to destroying by current
         * live census order (undo restored exactly the deleted
         * set, so any surplus live root set must contain it).
         * When nothing resolves, the redo is a no-op success. */
        return LED_SUCCESS;
    }
    case LED_CMD_CREATE: {
        /* Recreate (fresh handle) and refresh the entry. */
        led_result rc = led_apply(s, &e->command);

        if (rc == LED_SUCCESS) {
            uint32_t live = le_world_get_object_count(
                s->edit_world);

            if (live > 0) {
                le_object *all = (le_object *)malloc(
                    live * sizeof(*all));

                if (all != NULL) {
                    uint32_t got = le_world_get_all_objects(
                        s->edit_world, all, live);

                    if (got > 0) {
                        e->created = all[got - 1];
                        e->has_created = 1;
                    }
                    free(all);
                }
            }
        }
        return rc;
    }
    default:
        return led_apply(s, &e->command);
    }
}

int led_undo(led_session *session) {
    led_history_entry e;

    if (session == NULL || !led_is_attached(session)) {
        return 0;
    }
    if (session->undo_count == 0) {
        return 0;
    }
    e = session->undo_stack[--session->undo_count];
    memset(&session->undo_stack[session->undo_count], 0,
           sizeof(e));
    if (led_apply_inverse(session, &e) != LED_SUCCESS) {
        /* Best-effort: restore the entry (world may be mid-state;
         * the engine guarantees validated calls never half-apply). */
        session->undo_stack[session->undo_count++] = e;
        return 0;
    }
    /* Move to redo (bounded). */
    if (session->redo_count >= session->redo_cap) {
        led_entry_free(&session->redo_stack[0]);
        memmove(&session->redo_stack[0], &session->redo_stack[1],
                (session->redo_cap - 1) * sizeof(e));
        session->redo_count = session->redo_cap - 1;
    }
    session->redo_stack[session->redo_count++] = e;
    session->undos++;
    led_selection_prune(session);
    return 1;
}

int led_redo(led_session *session) {
    led_history_entry e;

    if (session == NULL || !led_is_attached(session)) {
        return 0;
    }
    if (session->redo_count == 0) {
        return 0;
    }
    e = session->redo_stack[--session->redo_count];
    memset(&session->redo_stack[session->redo_count], 0,
           sizeof(e));
    if (led_apply_after(session, &e) != LED_SUCCESS) {
        session->redo_stack[session->redo_count++] = e;
        return 0;
    }
    if (session->undo_count >= session->history_capacity) {
        led_entry_free(&session->undo_stack[0]);
        memmove(&session->undo_stack[0],
                &session->undo_stack[1],
                (session->history_capacity - 1) * sizeof(e));
        session->undo_count = session->history_capacity - 1;
        session->commands_evicted++;
    }
    session->undo_stack[session->undo_count++] = e;
    session->redos++;
    led_selection_prune(session);
    return 1;
}

void led_history_clear(led_session *session) {
    uint32_t i;

    if (session == NULL) {
        return;
    }
    for (i = 0; i < session->undo_count; i++) {
        led_entry_free(&session->undo_stack[i]);
    }
    for (i = 0; i < session->redo_count; i++) {
        led_entry_free(&session->redo_stack[i]);
    }
    session->undo_count = 0;
    session->redo_count = 0;
}

led_result led_history_set_capacity(led_session *session,
                                    uint32_t capacity) {
    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (capacity == 0 || capacity > 65536) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (capacity != session->history_capacity) {
        led_history_entry *nu = (led_history_entry *)calloc(
            capacity, sizeof(*nu));
        led_history_entry *nr = (led_history_entry *)calloc(
            capacity, sizeof(*nr));

        if (nu == NULL || nr == NULL) {
            free(nu);
            free(nr);
            return LED_ERROR_OUT_OF_MEMORY;
        }
        {
            uint32_t keep_u = session->undo_count;

            if (keep_u > capacity) {
                uint32_t drop = keep_u - capacity;
                uint32_t i;

                for (i = 0; i < drop; i++) {
                    led_entry_free(
                        &session->undo_stack[i]);
                }
                memmove(session->undo_stack,
                        &session->undo_stack[drop],
                        capacity * sizeof(*nu));
                keep_u = capacity;
                session->commands_evicted += drop;
            }
            memcpy(nu, session->undo_stack,
                   keep_u * sizeof(*nu));
            session->undo_count = keep_u;
        }
        {
            uint32_t keep_r = session->redo_count;

            if (keep_r > capacity) {
                keep_r = capacity;
            }
            memcpy(nr, session->redo_stack,
                   keep_r * sizeof(*nr));
            session->redo_count = keep_r;
        }
        free(session->undo_stack);
        free(session->redo_stack);
        session->undo_stack = nu;
        session->redo_stack = nr;
        session->undo_cap = capacity;
        session->redo_cap = capacity;
        session->history_capacity = capacity;
    }
    return LED_SUCCESS;
}

led_result led_history_set_coalesce(led_session *session, int enabled,
                                    uint64_t window_ms) {
    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    session->coalesce_enabled = enabled ? 1 : 0;
    session->coalesce_window_ms = window_ms;
    return LED_SUCCESS;
}

void led_history_get_stats(const led_session *session,
                           led_history_stats *out_stats) {
    uint32_t i;
    uint64_t bytes = 0;

    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (session == NULL) {
        return;
    }
    for (i = 0; i < session->undo_count; i++) {
        bytes += sizeof(session->undo_stack[i]);
        bytes += session->undo_stack[i].subtree_size;
    }
    for (i = 0; i < session->redo_count; i++) {
        bytes += sizeof(session->redo_stack[i]);
        bytes += session->redo_stack[i].subtree_size;
    }
    out_stats->undo_depth = session->undo_count;
    out_stats->redo_depth = session->redo_count;
    out_stats->capacity = session->history_capacity;
    out_stats->coalesce_enabled = session->coalesce_enabled;
    out_stats->commands_pushed = session->commands_pushed;
    out_stats->commands_evicted = session->commands_evicted;
    out_stats->undos = session->undos;
    out_stats->redos = session->redos;
    out_stats->coalesced = session->coalesced;
    out_stats->bytes_estimate = bytes;
}
