/* Selection: ordered unique le_object set, liveness-filtered on
 * every read path. View state — never marks the scene dirty. */

#include <string.h>

#include "internal/editor_internal.h"

static int led_handle_equal(const le_object *a, const le_object *b) {
    return (a->index == b->index && a->generation == b->generation &&
            a->world_tag == b->world_tag);
}

static int led_handle_live(le_world *world, const le_object *o) {
    if (world == NULL || o == NULL) {
        return 0;
    }
    return le_object_is_alive(world, o);
}

led_result led_selection_set(led_session *session,
                             const le_object *objects, uint32_t count) {
    uint32_t i;
    uint32_t n = 0;

    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    session->selection_count = 0;
    if (objects == NULL || count == 0) {
        return LED_SUCCESS;
    }
    for (i = 0; i < count && n < LED_SELECTION_MAX; i++) {
        uint32_t j;
        int dup = 0;

        if (!led_handle_live(session->edit_world, &objects[i])) {
            continue;
        }
        for (j = 0; j < n; j++) {
            if (led_handle_equal(&session->selection[j], &objects[i])) {
                dup = 1;
                break;
            }
        }
        if (!dup) {
            session->selection[n++] = objects[i];
        }
    }
    /* Deterministic ascending-slot order for tests + outliner. */
    {
        uint32_t a;
        uint32_t b;

        for (a = 0; a < n; a++) {
            for (b = a + 1; b < n; b++) {
                if (session->selection[b].index <
                    session->selection[a].index) {
                    le_object t = session->selection[a];

                    session->selection[a] = session->selection[b];
                    session->selection[b] = t;
                }
            }
        }
    }
    session->selection_count = n;
    return LED_SUCCESS;
}

led_result led_selection_add(led_session *session,
                             const le_object *object) {
    uint32_t i;

    if (session == NULL || object == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!led_handle_live(session->edit_world, object)) {
        return LED_ERROR_STALE_HANDLE;
    }
    for (i = 0; i < session->selection_count; i++) {
        if (led_handle_equal(&session->selection[i], object)) {
            return LED_SUCCESS;
        }
    }
    if (session->selection_count >= LED_SELECTION_MAX) {
        return LED_ERROR_OVERFLOW;
    }
    session->selection[session->selection_count++] = *object;
    return LED_SUCCESS;
}

led_result led_selection_remove(led_session *session,
                                const le_object *object) {
    uint32_t i;
    uint32_t j;

    if (session == NULL || object == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    for (i = 0; i < session->selection_count; i++) {
        if (led_handle_equal(&session->selection[i], object)) {
            for (j = i + 1; j < session->selection_count; j++) {
                session->selection[j - 1] = session->selection[j];
            }
            session->selection_count--;
            return LED_SUCCESS;
        }
    }
    return LED_SUCCESS;
}

led_result led_selection_toggle(led_session *session,
                                const le_object *object) {
    uint32_t i;

    if (session == NULL || object == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    for (i = 0; i < session->selection_count; i++) {
        if (led_handle_equal(&session->selection[i], object)) {
            return led_selection_remove(session, object);
        }
    }
    return led_selection_add(session, object);
}

led_result led_selection_clear(led_session *session) {
    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    session->selection_count = 0;
    return LED_SUCCESS;
}

int led_selection_contains(led_session *session,
                           const le_object *object) {
    uint32_t i;

    if (session == NULL || object == NULL ||
        !led_is_attached(session)) {
        return 0;
    }
    if (!led_handle_live(session->edit_world, object)) {
        return 0;
    }
    for (i = 0; i < session->selection_count; i++) {
        if (!led_handle_live(session->edit_world,
                             &session->selection[i])) {
            continue;
        }
        if (led_handle_equal(&session->selection[i], object)) {
            return 1;
        }
    }
    return 0;
}

uint32_t led_selection_get(led_session *session, le_object *out_objects,
                           uint32_t capacity) {
    uint32_t n = 0;
    uint32_t i;

    if (session == NULL || !led_is_attached(session)) {
        return 0;
    }
    for (i = 0; i < session->selection_count; i++) {
        if (!led_handle_live(session->edit_world,
                             &session->selection[i])) {
            continue;
        }
        if (out_objects != NULL && n < capacity) {
            out_objects[n] = session->selection[i];
        }
        n++;
    }
    return n;
}

uint32_t led_selection_prune(led_session *session) {
    uint32_t w = 0;
    uint32_t i;

    if (session == NULL || !led_is_attached(session)) {
        return 0;
    }
    for (i = 0; i < session->selection_count; i++) {
        if (led_handle_live(session->edit_world,
                            &session->selection[i])) {
            session->selection[w++] = session->selection[i];
        }
    }
    session->selection_count = w;
    return w;
}

led_result led_selection_select_subtree(led_session *session,
                                        const le_object *object) {
    /* Iterative DFS with an explicit stack (100k-deep safe). Cap
     * the walk at LED_SELECTION_MAX collected handles. */
    le_object stack[1024];
    uint32_t top = 0;
    le_object collected[LED_SELECTION_MAX];
    uint32_t n = 0;

    if (session == NULL || object == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!led_handle_live(session->edit_world, object)) {
        return LED_ERROR_STALE_HANDLE;
    }
    stack[top++] = *object;
    while (top > 0 && n < LED_SELECTION_MAX) {
        le_object cur = stack[--top];
        uint32_t cc = le_object_get_child_count(session->edit_world,
                                                &cur);
        uint32_t i;

        collected[n++] = cur;
        if (cc > 0 && cc <= 4096) {
            /* Fetch children (bounded temp buffer, chunked). */
            le_object kids[256];
            uint32_t got = 0;
            uint32_t total = 0;

            if (le_object_get_children(session->edit_world, &cur,
                                       kids, 256, &total) ==
                LE_SUCCESS) {
                got = (total < 256) ? total : 256;
                for (i = 0; i < got; i++) {
                    if (top < 1024 && n + top < LED_SELECTION_MAX + 1024) {
                        stack[top++] = kids[i];
                    }
                }
            }
        }
    }
    return led_selection_set(session, collected, n);
}

led_result led_selection_select_by_name(led_session *session,
                                        const char *name) {
    le_object found;

    if (session == NULL || name == NULL || name[0] == '\0') {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!le_world_find_by_name(session->edit_world, name, &found)) {
        return LED_ERROR_STALE_HANDLE;
    }
    return led_selection_set(session, &found, 1);
}
