/* Console ring + script-error mirror. Session-owned storage. */

#include <string.h>

#include "internal/editor_internal.h"

led_result led_console_push(led_session *session, led_log_level level,
                            const char *tag, const char *message) {
    uint32_t slot;

    if (session == NULL || tag == NULL || message == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (level < LED_LOG_INFO || level > LED_LOG_ERROR) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (session->console_count < LED_CONSOLE_CAPACITY) {
        slot = (session->console_start + session->console_count) %
               LED_CONSOLE_CAPACITY;
        session->console_count++;
    } else {
        slot = session->console_start;
        session->console_start =
            (session->console_start + 1) % LED_CONSOLE_CAPACITY;
        session->console_dropped++;
    }
    session->console_entries[slot].level = level;
    strncpy(session->console_entries[slot].tag, tag,
            sizeof(session->console_entries[slot].tag) - 1);
    session->console_entries[slot].tag[sizeof(
        session->console_entries[slot].tag) - 1] = '\0';
    strncpy(session->console_entries[slot].message, message,
            sizeof(session->console_entries[slot].message) - 1);
    session->console_entries[slot].message[sizeof(
        session->console_entries[slot].message) - 1] = '\0';
    return LED_SUCCESS;
}

uint32_t led_console_count(const led_session *session) {
    if (session == NULL) {
        return 0;
    }
    return session->console_count;
}

const char *led_console_message(const led_session *session,
                                uint32_t index,
                                led_log_level *out_level,
                                const char **out_tag) {
    uint32_t slot;

    if (out_level != NULL) {
        *out_level = LED_LOG_INFO;
    }
    if (out_tag != NULL) {
        *out_tag = "";
    }
    if (session == NULL || index >= session->console_count) {
        return NULL;
    }
    slot = (session->console_start + index) % LED_CONSOLE_CAPACITY;
    if (out_level != NULL) {
        *out_level = session->console_entries[slot].level;
    }
    if (out_tag != NULL) {
        *out_tag = session->console_entries[slot].tag;
    }
    return session->console_entries[slot].message;
}

void led_console_clear(led_session *session) {
    if (session == NULL) {
        return;
    }
    session->console_count = 0;
    session->console_start = 0;
}

int led_console_mirror_script_error(led_session *session) {
    le_script_error err;

    if (session == NULL || !led_is_attached(session)) {
        return 0;
    }
    memset(&err, 0, sizeof(err));
    le_script_get_last_error(session->engine, &err);
    if (err.message == NULL || err.message[0] == '\0') {
        return 0;
    }
    {
        char buf[256];

        memset(buf, 0, sizeof(buf));
        strncpy(buf, err.message, sizeof(buf) - 1);
        led_console_push(session, LED_LOG_ERROR,
                         err.callback != NULL ? err.callback
                                              : "script",
                         buf);
    }
    return 1;
}
