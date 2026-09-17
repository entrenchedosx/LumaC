/* Scene I/O + dirty tracking over le_scene_* (capture /
 * instantiate / save_file / load_file). Transactional open/revert:
 * failed loads preserve the edit world. */

#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

int led_is_dirty(const led_session *session) {
    if (session == NULL) {
        return 0;
    }
    return session->dirty;
}

/* Destroy every live edit-world object (roots first via repeated
 * destroy; subtree default handles descendants). */
static led_result led_destroy_all(le_world *w) {
    uint32_t guard = 0;

    while (le_world_get_object_count(w) > 0 && guard++ < 1000000u) {
        uint32_t live = le_world_get_object_count(w);
        le_object *all = NULL;
        le_result rc;

        if (live == 0) {
            break;
        }
        all = (le_object *)malloc(live * sizeof(*all));
        if (all == NULL) {
            return LED_ERROR_OUT_OF_MEMORY;
        }
        {
            uint32_t got = le_world_get_all_objects(w, all, live);

            if (got == 0) {
                free(all);
                break;
            }
            rc = le_object_destroy(w, &all[0]);
            free(all);
            if (rc != LE_SUCCESS) {
                return LED_ERROR_ENGINE;
            }
        }
    }
    return LED_SUCCESS;
}

led_result led_scene_new(led_session *session) {
    led_result rc;

    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->playing) {
        return LED_ERROR_ALREADY_PLAYING;
    }
    rc = led_destroy_all(session->edit_world);
    if (rc != LED_SUCCESS) {
        return rc;
    }
    session->selection_count = 0;
    led_history_clear(session);
    session->dirty = 0;
    session->has_scene_path = 0;
    session->scene_path[0] = '\0';
    session->hier_count = 0;
    session->inspector_count = 0;
    return LED_SUCCESS;
}

led_result led_scene_open(led_session *session, const char *path) {
    le_asset scene;
    le_scene_instance inst;
    le_result rc = LE_SUCCESS;

    if (session == NULL || path == NULL || path[0] == '\0') {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->playing) {
        return LED_ERROR_ALREADY_PLAYING;
    }
    memset(&inst, 0, sizeof(inst));
    /* Load into a temp scene asset first (edit world untouched on
     * parse failure). */
    if (le_scene_create(session->engine, 0, &scene) != LE_SUCCESS) {
        return LED_ERROR_OUT_OF_MEMORY;
    }
    {
        le_result load_rc =
            le_scene_load_file(session->engine, &scene, path);

        if (load_rc != LE_SUCCESS) {
            le_asset_unload(session->engine, &scene);
            session->last_engine_error = (int)load_rc;
            return (load_rc == LE_ERROR_OUT_OF_MEMORY)
                       ? LED_ERROR_OUT_OF_MEMORY
                       : LED_ERROR_PARSE;
        }
    }
    /* Validate by instantiating into a SCRATCH world first: a
     * broken scene never clears the edit world. */
    {
        le_world *scratch = NULL;
        le_world_desc wd;

        memset(&wd, 0, sizeof(wd));
        if (le_world_create(session->engine, &wd, &scratch) !=
            LE_SUCCESS) {
            le_asset_unload(session->engine, &scene);
            return LED_ERROR_OUT_OF_MEMORY;
        }
        rc = le_scene_instantiate(scratch, &scene, &inst);
        le_scene_instance_free(&inst);
        memset(&inst, 0, sizeof(inst));
        le_world_destroy(scratch);
        if (rc != LE_SUCCESS) {
            le_asset_unload(session->engine, &scene);
            session->last_engine_error = (int)rc;
            return LED_ERROR_PARSE;
        }
    }
    /* Swap: clear edit, instantiate for real. */
    rc = led_destroy_all(session->edit_world);
    if (rc != LED_SUCCESS) {
        le_asset_unload(session->engine, &scene);
        return rc;
    }
    rc = le_scene_instantiate(session->edit_world, &scene, &inst);
    le_scene_instance_free(&inst);
    le_asset_unload(session->engine, &scene);
    if (rc != LE_SUCCESS) {
        session->last_engine_error = (int)rc;
        return LED_ERROR_ENGINE;
    }
    /* Phase 33V fix (verified headed: viewport rendered black after
     * every scene open): the scene format carries camera COMPONENTS
     * but no active-camera selection, so the opened world had cameras
     * yet no active one — render fell back to the default origin
     * camera and mesh content missed the frame. Adopt the first live
     * camera object as active (view state like selection: not undo-
     * tracked, not dirty). Scenes without cameras keep the default. */
    {
        le_object probe = LE_OBJECT_INVALID;

        if (!le_world_get_active_camera(session->edit_world,
                                        &probe)) {
            uint32_t live =
                le_world_get_object_count(session->edit_world);

            if (live > 0) {
                le_object *all =
                    (le_object *)malloc(live * sizeof(*all));

                if (all != NULL) {
                    uint32_t got = le_world_get_all_objects(
                        session->edit_world, all, live);
                    uint32_t i = 0;

                    for (i = 0; i < got; i++) {
                        if (le_object_has_component(
                                session->edit_world, &all[i],
                                LE_COMPONENT_CAMERA)) {
                            le_world_set_active_camera(
                                session->edit_world, &all[i]);
                            break;
                        }
                    }
                    free(all);
                }
            }
        }
    }
    session->selection_count = 0;
    led_history_clear(session);
    session->dirty = 0;
    /* memmove: save() passes session->scene_path itself (overlap is
     * legal here; strncpy would be UB — caught by ASan). */
    memmove(session->scene_path, path, strlen(path) + 1);
    session->scene_path[sizeof(session->scene_path) - 1] = '\0';
    session->has_scene_path = 1;
    session->hier_count = 0;
    session->inspector_count = 0;
    return LED_SUCCESS;
}

led_result led_scene_save_as(led_session *session, const char *path) {
    le_asset scene;
    uint32_t skipped = 0;
    le_result rc;

    if (session == NULL || path == NULL || path[0] == '\0') {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (le_scene_create(session->engine, 0, &scene) != LE_SUCCESS) {
        return LED_ERROR_OUT_OF_MEMORY;
    }
    rc = le_scene_capture(session->edit_world, &scene, &skipped);
    if (rc != LE_SUCCESS) {
        le_asset_unload(session->engine, &scene);
        session->last_engine_error = (int)rc;
        return LED_ERROR_ENGINE;
    }
    (void)skipped;
    rc = le_scene_save_file(session->engine, &scene, path);
    le_asset_unload(session->engine, &scene);
    if (rc != LE_SUCCESS) {
        /* Failed save KEEPS dirty (contract). */
        session->last_engine_error = (int)rc;
        return LED_ERROR_IO;
    }
    session->dirty = 0;
    /* memmove: save() passes session->scene_path itself (overlap is
     * legal here; strncpy would be UB — caught by ASan). */
    memmove(session->scene_path, path, strlen(path) + 1);
    session->scene_path[sizeof(session->scene_path) - 1] = '\0';
    session->has_scene_path = 1;
    return LED_SUCCESS;
}

led_result led_scene_save(led_session *session) {
    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!session->has_scene_path) {
        return LED_ERROR_IO;
    }
    return led_scene_save_as(session, session->scene_path);
}

led_result led_scene_revert(led_session *session) {
    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->playing) {
        return LED_ERROR_ALREADY_PLAYING;
    }
    if (!session->has_scene_path) {
        return LED_ERROR_IO;
    }
    return led_scene_open(session, session->scene_path);
}

const char *led_scene_get_path(const led_session *session) {
    if (session == NULL || !session->has_scene_path) {
        return "";
    }
    return session->scene_path;
}
