/* EditorCore session lifetime, attach, tick, stats. Headless:
 * no window/renderer calls; the host owns engine/world lifetime. */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "internal/editor_internal.h"

uint64_t led_clock_ms(void) {
    /* C11 timespec_get (no OS-native headers — the backend
     * audit forbids them here). Millisecond resolution is plenty
     * for undo coalescing windows. */
    struct timespec ts;

    memset(&ts, 0, sizeof(ts));
    if (timespec_get(&ts, TIME_UTC) == TIME_UTC) {
        return (uint64_t)ts.tv_sec * 1000ull +
               (uint64_t)ts.tv_nsec / 1000000ull;
    }
    return 0;
}

void led_entry_free(led_history_entry *entry) {
    if (entry == NULL) {
        return;
    }
    if (entry->subtree_blob != NULL) {
        free(entry->subtree_blob);
        entry->subtree_blob = NULL;
    }
    entry->subtree_size = 0;
}

int led_is_attached(const led_session *s) {
    return (s != NULL && s->attached && s->engine != NULL &&
            s->edit_world != NULL);
}

led_result led_session_create(led_session **out_session) {
    led_session *s;

    if (out_session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    *out_session = NULL;
    s = (led_session *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return LED_ERROR_OUT_OF_MEMORY;
    }
    s->history_capacity = 256;
    s->undo_cap = 256;
    s->redo_cap = 256;
    s->undo_stack = (led_history_entry *)calloc(
        256, sizeof(led_history_entry));
    s->redo_stack = (led_history_entry *)calloc(
        256, sizeof(led_history_entry));
    if (s->undo_stack == NULL || s->redo_stack == NULL) {
        free(s->undo_stack);
        free(s->redo_stack);
        free(s);
        return LED_ERROR_OUT_OF_MEMORY;
    }
    s->coalesce_enabled = 1;
    s->coalesce_window_ms = 500;
    s->play_scene = LE_ASSET_INVALID;
    s->gizmo_target = LE_OBJECT_INVALID;
    *out_session = s;
    return LED_SUCCESS;
}

static void led_free_stacks(led_session *s) {
    uint32_t i;

    for (i = 0; i < s->undo_count; i++) {
        led_entry_free(&s->undo_stack[i]);
    }
    for (i = 0; i < s->redo_count; i++) {
        led_entry_free(&s->redo_stack[i]);
    }
    free(s->undo_stack);
    free(s->redo_stack);
    s->undo_stack = NULL;
    s->redo_stack = NULL;
}

void led_session_destroy(led_session *session) {
    if (session == NULL) {
        return;
    }
    if (session->playing && session->play_world != NULL) {
        le_world_destroy(session->play_world);
        session->play_world = NULL;
        session->playing = 0;
    }
    /* Phase 32: close any open project first (DB + state freed;
     * engine assets obey Phase 25 ownership). */
    led_project_close(session);
    /* Borrowed engine/world handles are untouched by contract. */
    led_free_stacks(session);
    free(session->hier_nodes);
    free(session);
}

led_result led_session_attach(led_session *session, le_engine *engine,
                              le_world *edit_world) {
    if (session == NULL || engine == NULL || edit_world == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (session->playing && session->play_world != NULL) {
        le_world_destroy(session->play_world);
        session->play_world = NULL;
        session->playing = 0;
    }
    {
        uint32_t i;

        for (i = 0; i < session->undo_count; i++) {
            led_entry_free(&session->undo_stack[i]);
        }
        for (i = 0; i < session->redo_count; i++) {
            led_entry_free(&session->redo_stack[i]);
        }
    }
    session->undo_count = 0;
    session->redo_count = 0;
    session->engine = engine;
    session->edit_world = edit_world;
    session->attached = 1;
    session->selection_count = 0;
    session->dirty = 0;
    session->has_scene_path = 0;
    session->scene_path[0] = '\0';
    session->commands_executed = 0;
    session->last_engine_error = 0;
    session->hier_count = 0;
    session->inspector_count = 0;
    session->pre_play_selection_count = 0;
    session->play_ticks = 0;
    session->play_elapsed = 0.0;
    session->play_paused = 0;
    session->gizmo_active = 0;
    session->gizmo_target = LE_OBJECT_INVALID;
    session->console_count = 0;
    session->console_start = 0;
    session->console_dropped = 0;
    session->text_field_focused = 0;
    /* Phase 32: re-attach closes any open project (no cross-bind
     * leakage between engine/world generations). */
    led_project_close(session);
    return LED_SUCCESS;
}

void led_session_detach(led_session *session) {
    uint32_t i;

    if (session == NULL) {
        return;
    }
    if (session->playing && session->play_world != NULL) {
        le_world_destroy(session->play_world);
        session->play_world = NULL;
        session->playing = 0;
    }
    led_project_close(session);
    for (i = 0; i < session->undo_count; i++) {
        led_entry_free(&session->undo_stack[i]);
    }
    for (i = 0; i < session->redo_count; i++) {
        led_entry_free(&session->redo_stack[i]);
    }
    session->undo_count = 0;
    session->redo_count = 0;
    session->engine = NULL;
    session->edit_world = NULL;
    session->attached = 0;
    session->selection_count = 0;
    session->dirty = 0;
    session->has_scene_path = 0;
    session->hier_count = 0;
    session->inspector_count = 0;
    session->text_field_focused = 0;
}

le_engine *led_session_get_engine(const led_session *session) {
    if (session == NULL || !session->attached) {
        return NULL;
    }
    return session->engine;
}

le_world *led_session_get_edit_world(const led_session *session) {
    if (session == NULL || !session->attached) {
        return NULL;
    }
    return session->edit_world;
}

led_result led_session_tick(led_session *session, float dt) {
    (void)dt;
    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    /* Edit-world scripts NEVER run here: matrices-only refresh keeps
     * the outliner valid without dispatching gameplay. */
    if (!session->playing) {
        le_world_update(session->edit_world, 0.0f);
    }
    led_selection_prune(session);
    session->editor_ms = led_clock_ms();
    return LED_SUCCESS;
}

void led_session_get_stats(const led_session *session,
                           led_session_stats *out_stats) {
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (session == NULL) {
        return;
    }
    out_stats->attached = session->attached;
    out_stats->playing = session->playing;
    out_stats->edit_paused_before_play = session->edit_paused_before_play;
    out_stats->selection_count = session->selection_count;
    out_stats->undo_depth = session->undo_count;
    out_stats->redo_depth = session->redo_count;
    out_stats->dirty = session->dirty;
    out_stats->commands_executed = session->commands_executed;
    out_stats->play_ticks = session->play_ticks;
    out_stats->last_engine_error = session->last_engine_error;
}
