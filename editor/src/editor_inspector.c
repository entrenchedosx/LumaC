/* Inspector model: plain-data rows (descriptor + value snapshot)
 * for a future property UI. Session-owned rows. */

#include <stdio.h>
#include <string.h>

#include "internal/editor_internal.h"

uint32_t led_inspect(led_session *session, const le_object *object) {
    uint32_t n = 0;
    uint32_t i;

    if (session == NULL) {
        return 0;
    }
    session->inspector_count = 0;
    if (object == NULL || !led_is_attached(session)) {
        return 0;
    }
    if (!le_object_is_alive(session->edit_world, object)) {
        return 0;
    }
    for (i = 0; i < 64 && n < LED_INSPECTOR_MAX_ROWS; i++) {
        /* Enumerate the static vocabulary by probing known paths.
         * The table order in editor_reflect.c is alphabetical; rows
         * follow that deterministic order. */
        break;
    }
    {
        /* Walk the static table via led_list_properties. */
        const led_property_desc *props[128];
        uint32_t total = 0;
        uint32_t got;

        got = led_list_properties(session, object, props, 128,
                                  &total);
        if (got > 128) {
            got = 128;
        }
        for (i = 0; i < got && n < LED_INSPECTOR_MAX_ROWS; i++) {
            led_inspector_row *row = &session->inspector_rows[n];
            led_property_value v;

            memset(&v, 0, sizeof(v));
            memset(row, 0, sizeof(*row));
            memcpy(&row->desc, props[i], sizeof(row->desc));
            row->has_value = led_read_property(session, object,
                                               props[i]->path, &v);
            if (row->has_value) {
                row->value = v;
            }
            n++;
        }
        /* Dynamic script props appended after statics. */
        {
            le_script_property sprops[16];
            uint32_t scount = 0;

            if (le_script_list_properties(session->edit_world,
                                          object, sprops, 16,
                                          &scount)) {
                uint32_t k;

                if (scount > 16) {
                    scount = 16;
                }
                for (k = 0; k < scount && n < LED_INSPECTOR_MAX_ROWS;
                     k++) {
                    led_inspector_row *row =
                        &session->inspector_rows[n];
                    char path[80];
                    led_property_value v;

                    memset(row, 0, sizeof(*row));
                    memset(&v, 0, sizeof(v));
                    snprintf(path, sizeof(path), "script.%s",
                             sprops[k].name);
                    row->desc.path = "script.*";
                    row->desc.label = sprops[k].name;
                    row->desc.component = LE_COMPONENT_SCRIPT;
                    row->desc.index = k;
                    row->desc.read_only = 0;
                    switch (sprops[k].type) {
                    case LE_SCRIPT_PROP_BOOL:
                        row->desc.type = LED_DATA_BOOL;
                        break;
                    case LE_SCRIPT_PROP_INT:
                        row->desc.type = LED_DATA_INT;
                        break;
                    case LE_SCRIPT_PROP_NUMBER:
                        row->desc.type = LED_DATA_FLOAT;
                        break;
                    case LE_SCRIPT_PROP_STRING:
                        row->desc.type = LED_DATA_STRING;
                        break;
                    case LE_SCRIPT_PROP_VEC3:
                        row->desc.type = LED_DATA_VEC3;
                        break;
                    case LE_SCRIPT_PROP_ASSET:
                        row->desc.type = LED_DATA_ASSET_ID;
                        break;
                    default:
                        row->desc.type = LED_DATA_UNAVAILABLE;
                        break;
                    }
                    row->has_value = led_read_property(
                        session, object, path, &v);
                    if (row->has_value) {
                        row->value = v;
                    }
                    n++;
                }
            }
        }
    }
    session->inspector_count = n;
    return n;
}

const led_inspector_row *led_inspector_rows(
    const led_session *session) {
    if (session == NULL || session->inspector_count == 0) {
        return NULL;
    }
    return session->inspector_rows;
}

uint32_t led_inspector_count(const led_session *session) {
    if (session == NULL) {
        return 0;
    }
    return session->inspector_count;
}
