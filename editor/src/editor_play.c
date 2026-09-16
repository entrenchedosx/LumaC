/* Play mode: edit <-> runtime isolation. Enter captures the edit
 * world into a scene asset and instantiates a FRESH runtime world on
 * the same engine; ticks step ONLY the runtime world; exit destroys
 * it. Edit scripts NEVER run during play (matrices-only refresh).
 * Physics determinism across platforms is NOT promised (matches the
 * engine contract); same-build explicit-dt replay is deterministic. */

#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

led_result led_play_enter(led_session *session) {
    le_world_desc wd;
    le_scene_instance inst;

    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->playing) {
        return LED_ERROR_ALREADY_PLAYING;
    }
    memset(&wd, 0, sizeof(wd));
    memset(&inst, 0, sizeof(inst));
    /* Stash selection (handles pruned/restored on exit). */
    {
        uint32_t n = led_selection_get(session,
                                       session->pre_play_selection,
                                       LED_SELECTION_MAX);

        session->pre_play_selection_count = n;
    }
    if (le_scene_create(session->engine, 0, &session->play_scene) !=
        LE_SUCCESS) {
        return LED_ERROR_OUT_OF_MEMORY;
    }
    session->has_play_scene = 1;
    {
        uint32_t skipped = 0;
        le_result rc = le_scene_capture(session->edit_world,
                                        &session->play_scene,
                                        &skipped);

        (void)skipped;
        if (rc != LE_SUCCESS) {
            le_asset_unload(session->engine, &session->play_scene);
            session->has_play_scene = 0;
            session->last_engine_error = (int)rc;
            return LED_ERROR_PLAY_FAILED;
        }
    }
    if (le_world_create(session->engine, &wd,
                        &session->play_world) != LE_SUCCESS) {
        le_asset_unload(session->engine, &session->play_scene);
        session->has_play_scene = 0;
        return LED_ERROR_OUT_OF_MEMORY;
    }
    {
        le_result rc = le_scene_instantiate(session->play_world,
                                            &session->play_scene,
                                            &inst);

        le_scene_instance_free(&inst);
        if (rc != LE_SUCCESS) {
            /* Failed play: edit untouched, no runtime leaked. */
            le_world_destroy(session->play_world);
            session->play_world = NULL;
            le_asset_unload(session->engine, &session->play_scene);
            session->has_play_scene = 0;
            session->last_engine_error = (int)rc;
            return LED_ERROR_PLAY_FAILED;
        }
    }
    session->edit_paused_before_play =
        le_world_is_paused(session->edit_world);
    le_world_set_paused(session->edit_world, 1);
    le_world_set_paused(session->play_world, 0);
    session->playing = 1;
    session->play_ticks = 0;
    session->play_elapsed = 0.0;
    session->play_paused = 0;
    session->selection_count = 0;
    return LED_SUCCESS;
}

led_result led_play_tick(led_session *session, float dt) {
    le_result rc;

    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!session->playing || session->play_world == NULL) {
        return LED_ERROR_NOT_PLAYING;
    }
    if (!(dt == dt) || dt < 0.0f) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    /* Step ONLY the runtime world (explicit dt, deterministic). */
    rc = le_engine_step(session->engine, session->play_world, dt);
    if (rc != LE_SUCCESS) {
        session->last_engine_error = (int)rc;
        return LED_ERROR_ENGINE;
    }
    /* Edit world: matrices-only refresh (scripts NEVER run). */
    le_world_update(session->edit_world, 0.0f);
    session->play_ticks++;
    session->play_elapsed += (double)dt;
    return LED_SUCCESS;
}

led_result led_play_set_paused(led_session *session, int paused) {
    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!session->playing || session->play_world == NULL) {
        return LED_ERROR_NOT_PLAYING;
    }
    if (le_world_set_paused(session->play_world, paused) !=
        LE_SUCCESS) {
        return LED_ERROR_ENGINE;
    }
    session->play_paused = paused ? 1 : 0;
    return LED_SUCCESS;
}

int led_play_is_paused(const led_session *session) {
    if (session == NULL || !session->playing ||
        session->play_world == NULL) {
        return 0;
    }
    return le_world_is_paused(session->play_world);
}

led_result led_play_step(led_session *session) {
    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!session->playing || session->play_world == NULL) {
        return LED_ERROR_NOT_PLAYING;
    }
    if (le_time_request_single_step(session->engine) != LE_SUCCESS) {
        return LED_ERROR_ENGINE;
    }
    return led_play_tick(session, 0.0f);
}

led_result led_play_exit(led_session *session) {
    uint32_t i;

    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!session->playing) {
        return LED_ERROR_NOT_PLAYING;
    }
    if (session->play_world != NULL) {
        le_world_destroy(session->play_world);
        session->play_world = NULL;
    }
    if (session->has_play_scene) {
        le_asset_unload(session->engine, &session->play_scene);
        session->has_play_scene = 0;
    }
    session->playing = 0;
    session->play_paused = 0;
    if (!session->edit_paused_before_play) {
        le_world_set_paused(session->edit_world, 0);
    }
    /* Restore pre-play selection (pruned: runtime handles never
     * leak back; stale entries drop). */
    session->selection_count = 0;
    for (i = 0; i < session->pre_play_selection_count &&
                session->selection_count < LED_SELECTION_MAX;
         i++) {
        if (le_object_is_alive(session->edit_world,
                               &session->pre_play_selection[i])) {
            session->selection[session->selection_count++] =
                session->pre_play_selection[i];
        }
    }
    session->pre_play_selection_count = 0;
    session->hier_count = 0;
    session->inspector_count = 0;
    return LED_SUCCESS;
}

int led_is_playing(const led_session *session) {
    if (session == NULL) {
        return 0;
    }
    return session->playing;
}

le_world *led_play_get_world(led_session *session) {
    if (session == NULL || !session->playing) {
        return NULL;
    }
    return session->play_world;
}

void led_play_get_stats(const led_session *session,
                        led_play_stats *out_stats) {
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (session == NULL) {
        return;
    }
    out_stats->playing = session->playing;
    out_stats->runtime_paused = session->play_paused;
    out_stats->ticks = session->play_ticks;
    out_stats->runtime_elapsed = session->play_elapsed;
    if (session->playing && session->play_world != NULL) {
        out_stats->runtime_objects =
            le_world_get_object_count(session->play_world);
    }
}
