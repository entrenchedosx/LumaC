/* Phase 33 GUI inspector widgets (isolated C++ over led_*).
 *
 * Typed widgets per LED_DATA_* reflection type: bool/int/uint/float
 * (range-clamped), vec3, euler-degrees (quaternion-stable), string,
 * enum combo, read-only asset-ID/color/unavailable rows. Writes route
 * through led_write_property ONLY (the engine-validated oracle; the
 * command funnel confirms undoability separately). History rule of
 * the core: value writes (everything EXCEPT CREATE/DELETE/parent/
 * component add/remove/prefab/assign) flow through led_write_property
 * and are undoable (TRS coalescing merges consecutive drags);
 * structural commands (add/remove component, parent set/reparent)
 * route through led_execute with full-desc snapshots or
 * SET_PARENT/REPARENT kinds.
 *
 * Inspector rows are session-owned snapshots (led_inspect) pairing a
 * led_property_desc (path/label/type/range/read_only) with the
 * current led_property_value. Script rows carry path "script.*"
 * with the export name in label (see editor_inspector.c); the real
 * write path is "script.<name>" — reconstructed here. renderable
 * bool rows (visible/casts/receives) and the scalar fast paths
 * (rigidbody.mass, collider.is_trigger, animator.speed) write through
 * the same led_write_property funnel.
 */

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "gui_internal.h"

/* Commit one validated value through the engine oracle.
 * led_write_property type+range validates BEFORE any engine write
 * (see editor_reflect.c: static-table range gate, NaN rejection,
 * read_only rejection, enum bounds, euler->quat conversion,
 * component read-modify-write with add_* re-validation); failures
 * surface as status + console entries, never partial writes. */
static void leg_commit_value(leg_context *ctx, const le_object *obj,
                             const led_inspector_row *row,
                             const led_property_value *val) {
    char path[96];
    led_result rc;

    if (ctx == NULL || obj == NULL || row == NULL || val == NULL) {
        return;
    }
    if (ctx->session == NULL) {
        return;
    }
    if (row->desc.path == NULL) {
        return;
    }
    if (row->desc.read_only) {
        return; /* read-only rows never commit */
    }
    /* Script rows: led_inspect stores path "script.*" with the export
     * name in label; rebuild the real "script.<name>" write path. */
    if (strcmp(row->desc.path, "script.*") == 0) {
        if (row->desc.label == NULL || row->desc.label[0] == '\0') {
            leg_status(ctx, "Unknown script property", 1);
            return;
        }
        snprintf(path, sizeof(path), "script.%s", row->desc.label);
    } else {
        strncpy(path, row->desc.path, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    }
    rc = led_write_property(ctx->session, obj, path, val);
    if (rc != LED_SUCCESS) {
        char buf[192];

        snprintf(buf, sizeof(buf), "'%s' rejected (%d)",
                 row->desc.label != NULL ? row->desc.label : "?",
                 (int)rc);
        leg_status(ctx, buf, 1);
    }
}

/* One inspector row -> typed widget. Returns 1 when the row drew
 * (0 for NULL args). String staging: edit buffer commits on Enter
 * (InputText flags) or focus loss; failed validation restores the
 * snapshot text (no partial write). Drag widgets (DragInt/DragFloat)
 * stream led_write_property per tick; TRS coalescing in the core
 * merges consecutive same-target same-kind drags into ONE history
 * entry, so no GUI-side batching is needed. */
int leg_widget_row(leg_context *ctx, leg_ui *ui, int row_id,
                   const led_inspector_row *row, const le_object *obj) {
    const char *label = NULL;

    if (ctx == NULL || ui == NULL || row == NULL || obj == NULL) {
        return 0;
    }
    label = (row->desc.label != NULL) ? row->desc.label : "(?)";
    ImGui::PushID(row_id);
    switch (row->desc.type) {
    case LED_DATA_BOOL: {
        /* Checkbox wants bool*; the snapshot stores int. */
        bool v = row->has_value ? (row->value.boolean != 0) : false;

        if (!row->has_value) {
            ImGui::TextDisabled("%s: (—)", label);
            break;
        }
        if (ImGui::Checkbox(label, &v)) {
            led_property_value val;

            memset(&val, 0, sizeof(val));
            val.type = LED_DATA_BOOL;
            val.boolean = v ? 1 : 0;
            leg_commit_value(ctx, obj, row, &val);
        }
        break;
    }
    case LED_DATA_INT: {
        int v = row->has_value ? (int)row->value.integer : 0;

        if (!row->has_value) {
            ImGui::TextDisabled("%s: (—)", label);
            break;
        }
        ImGui::SetNextItemWidth(160);
        /* Unranged DragInt: v_min >= v_max means no bound (so pass
         * 0/0, never INT_MIN/INT_MAX which WOULD bound). */
        if (ImGui::DragInt(label, &v, 1.0f,
                           row->desc.has_range
                               ? (int)row->desc.min_value
                               : 0,
                           row->desc.has_range
                               ? (int)row->desc.max_value
                               : 0)) {
            led_property_value val;

            memset(&val, 0, sizeof(val));
            val.type = LED_DATA_INT;
            val.integer = v;
            leg_commit_value(ctx, obj, row, &val);
        }
        break;
    }
    case LED_DATA_UINT: {
        /* DragInt is int-based: stage as int (nonnegative), write
         * back as uint64. Ranges cap at INT32_MAX on the widget;
         * the oracle enforces the desc range on commit. */
        int v = 0;

        if (!row->has_value) {
            ImGui::TextDisabled("%s: (—)", label);
            break;
        }
        v = (row->value.uinteger > 0x7FFFFFFFu)
                ? 0x7FFFFFFF
                : (int)row->value.uinteger;
        ImGui::SetNextItemWidth(160);
        if (ImGui::DragInt(label, &v, 1.0f, 0,
                           row->desc.has_range &&
                                   row->desc.max_value < 2147483647.0f
                               ? (int)row->desc.max_value
                               : 0)) {
            led_property_value val;

            if (v < 0) {
                v = 0;
            }
            memset(&val, 0, sizeof(val));
            val.type = LED_DATA_UINT;
            val.uinteger = (uint64_t)v;
            leg_commit_value(ctx, obj, row, &val);
        }
        break;
    }
    case LED_DATA_FLOAT: {
        float v =
            row->has_value ? (float)row->value.number : 0.0f;

        if (!row->has_value) {
            ImGui::TextDisabled("%s: (—)", label);
            break;
        }
        ImGui::SetNextItemWidth(160);
        /* Unranged DragFloat: min==max==0 means no clamp (so pass
         * 0/0, never -FLT_MAX/FLT_MAX which WOULD clamp). */
        if (ImGui::DragFloat(label, &v, 0.05f,
                             row->desc.has_range
                                 ? row->desc.min_value
                                 : 0.0f,
                             row->desc.has_range
                                 ? row->desc.max_value
                                 : 0.0f,
                             "%.4g")) {
            led_property_value val;

            memset(&val, 0, sizeof(val));
            val.type = LED_DATA_FLOAT;
            val.number = v;
            leg_commit_value(ctx, obj, row, &val);
        }
        break;
    }
    case LED_DATA_VEC3: {
        float v[3];

        if (!row->has_value) {
            ImGui::TextDisabled("%s: (—)", label);
            break;
        }
        memcpy(v, row->value.vec3, sizeof(v));
        ImGui::SetNextItemWidth(220);
        if (ImGui::DragFloat3(label, v, 0.05f)) {
            led_property_value val;

            memset(&val, 0, sizeof(val));
            val.type = LED_DATA_VEC3;
            memcpy(val.vec3, v, sizeof(v));
            leg_commit_value(ctx, obj, row, &val);
        }
        break;
    }
    case LED_DATA_COLOR3: {
        float v[3];

        if (!row->has_value) {
            ImGui::TextDisabled("%s: (—)", label);
            break;
        }
        /* led_write_property accepts COLOR3 or VEC3 for light.color;
         * keep the desc type so the oracle sees COLOR3. */
        memcpy(v, row->value.vec3, sizeof(v));
        ImGui::SetNextItemWidth(220);
        if (ImGui::ColorEdit3(label, v)) {
            led_property_value val;

            memset(&val, 0, sizeof(val));
            val.type = LED_DATA_COLOR3;
            memcpy(val.vec3, v, sizeof(v));
            leg_commit_value(ctx, obj, row, &val);
        }
        break;
    }
    case LED_DATA_QUAT: {
        /* Quaternion-stable: direct DragFloat4 on the unit quat
         * (engine normalizes on store). The sibling euler row is
         * the friendly editor; this row stays numeric-exact. */
        float v[4];

        if (!row->has_value) {
            ImGui::TextDisabled("%s: (—)", label);
            break;
        }
        memcpy(v, row->value.quat, sizeof(v));
        ImGui::SetNextItemWidth(220);
        if (ImGui::DragFloat4(label, v, 0.01f, -1.0f, 1.0f,
                              "%.4g")) {
            led_property_value val;

            memset(&val, 0, sizeof(val));
            val.type = LED_DATA_QUAT;
            memcpy(val.quat, v, sizeof(v));
            leg_commit_value(ctx, obj, row, &val);
        }
        break;
    }
    case LED_DATA_EULER_DEG: {
        /* led_read_property reports euler degrees in value.vec3
         * (pitch X, yaw Y, roll Z); led_write_property accepts
         * EULER_DEG or VEC3 and converts via the core ZYX policy. */
        float v[3];

        if (!row->has_value) {
            ImGui::TextDisabled("%s: (—)", label);
            break;
        }
        memcpy(v, row->value.vec3, sizeof(v));
        ImGui::SetNextItemWidth(220);
        if (ImGui::DragFloat3(label, v, 0.5f)) {
            led_property_value val;

            memset(&val, 0, sizeof(val));
            val.type = LED_DATA_EULER_DEG;
            memcpy(val.vec3, v, sizeof(v));
            leg_commit_value(ctx, obj, row, &val);
        }
        break;
    }
    case LED_DATA_STRING: {
        /* Staged text: edit buffer commits on Enter
         * (EnterReturnsTrue) or focus loss (DeactivatedAfterEdit
         * clears staging so the next frame re-seeds from the fresh
         * snapshot — failed validation restores snapshot text). */
        if (ui->staged_row != row_id || ui->string_stage[0] == '\0') {
            const char *cur = row->has_value
                                  ? row->value.string_value
                                  : "";

            strncpy(ui->string_stage, cur,
                    sizeof(ui->string_stage) - 1);
            ui->string_stage[sizeof(ui->string_stage) - 1] = '\0';
            ui->staged_row = row_id;
        }
        ImGui::SetNextItemWidth(220);
        if (ImGui::InputText(label, ui->string_stage,
                             sizeof(ui->string_stage),
                             ImGuiInputTextFlags_EnterReturnsTrue)) {
            led_property_value val;

            memset(&val, 0, sizeof(val));
            val.type = LED_DATA_STRING;
            strncpy(val.string_value, ui->string_stage,
                    sizeof(val.string_value) - 1);
            leg_commit_value(ctx, obj, row, &val);
            ui->staged_row = -1;
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            ui->staged_row = -1;
        }
        break;
    }
    case LED_DATA_ENUM: {
        /* Semicolon-separated labels; current = integer index.
         * ImGui Combo wants NUL-separated items: convert a copy. */
        int cur = row->has_value ? (int)row->value.integer : 0;
        const char *labels =
            (row->desc.enum_labels != NULL)
                ? row->desc.enum_labels
                : "(?)";
        char items[256];
        int count = 1;
        size_t k = 0;

        strncpy(items, labels, sizeof(items) - 1);
        items[sizeof(items) - 1] = '\0';
        for (k = 0; items[k] != '\0'; k++) {
            if (items[k] == ';') {
                items[k] = '\0';
                count++;
            }
        }
        if (cur < 0) {
            cur = 0;
        }
        if (cur >= count) {
            cur = count - 1;
        }
        ImGui::SetNextItemWidth(180);
        if (ImGui::Combo(label, &cur, items)) {
            led_property_value val;

            memset(&val, 0, sizeof(val));
            val.type = LED_DATA_ENUM;
            val.integer = cur;
            leg_commit_value(ctx, obj, row, &val);
        }
        break;
    }
    case LED_DATA_ASSET_ID:
    case LED_DATA_UNAVAILABLE:
    default: {
        /* Read-only rows (asset hex, unavailable, unknown): text,
         * never an editor. */
        if (row->desc.type == LED_DATA_ASSET_ID && row->has_value) {
            ImGui::TextDisabled("%s: %s", label,
                                row->value.asset_hex);
        } else if (row->desc.type == LED_DATA_UNAVAILABLE) {
            ImGui::TextDisabled("%s: (unavailable)", label);
        } else if (!row->has_value) {
            ImGui::TextDisabled("%s: (—)", label);
        } else {
            ImGui::TextDisabled("%s: (read-only)", label);
        }
        break;
    }
    }
    ImGui::PopID();
    return 1;
}
