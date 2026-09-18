/* Phase 32 asset browser model (headless): folder tree + sorted
 * asset list + filter/search/sort + selection BY PROJECT UUID
 * (never row index) + drag payloads (UUID + type, never raw
 * paths). GUI-independent; a future GUI renders this model.
 *
 * Drops route through Phase 31 led_execute commands:
 * model->scene = CREATE + asset-renderable assign; material->object
 * = typed assign; script->object = add-script; scene = open (never
 * accidental instantiate).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

/* ---- model refresh ---- */

static int led_browser_record_visible(const led_project *p,
                                      uint32_t rec_idx) {
    const led_db_record *r = &p->records[rec_idx];

    if (p->view_filter != LED_PROJECT_ASSET_TYPE_COUNT &&
        r->type != p->view_filter) {
        return 0;
    }
    if (p->view_search[0] != '\0') {
        const char *base = strrchr(r->source_path, '/');
        extern int led_browser_match(const char *hay,
                                     const char *query);

        base = (base != NULL) ? base + 1 : r->source_path;
        if (!led_browser_match(base, p->view_search) &&
            !led_browser_match(r->source_path, p->view_search)) {
            return 0;
        }
    }
    return 1;
}

int led_browser_match(const char *haystack, const char *query) {
    size_t hl;
    size_t ql;
    size_t i;

    if (query == NULL || query[0] == '\0') {
        return 1;
    }
    if (haystack == NULL) {
        return 0;
    }
    hl = strlen(haystack);
    ql = strlen(query);
    if (ql > hl) {
        return 0;
    }
    for (i = 0; i + ql <= hl; i++) {
        size_t k = 0;

        while (k < ql) {
            char a = haystack[i + k];
            char b = query[k];

            if (a >= 'A' && a <= 'Z') {
                a = (char)(a - 'A' + 'a');
            }
            if (b >= 'A' && b <= 'Z') {
                b = (char)(b - 'A' + 'a');
            }
            if (a != b) {
                break;
            }
            k++;
        }
        if (k == ql) {
            return 1;
        }
    }
    return 0;
}

static const char *led_browser_basename(const char *path) {
    const char *b = strrchr(path, '/');

    return (b != NULL) ? b + 1 : path;
}

static int led_browser_view_cmp(const led_project *p, uint32_t a,
                                uint32_t b) {
    const led_db_record *ra = &p->records[a];
    const led_db_record *rb = &p->records[b];

    if (p->view_sort == 1) {
        int c = strcmp(led_browser_basename(ra->source_path),
                       led_browser_basename(rb->source_path));

        if (c != 0) {
            return c;
        }
        return strcmp(ra->source_path, rb->source_path);
    }
    if (p->view_sort == 2) {
        if (ra->type != rb->type) {
            return ((int)ra->type < (int)rb->type) ? -1 : 1;
        }
        return strcmp(ra->source_path, rb->source_path);
    }
    return strcmp(ra->source_path, rb->source_path);
}

uint32_t led_browser_refresh(led_session *session) {
    led_project *p = NULL;
    uint32_t i;

    if (session == NULL || session->project == NULL) {
        return 0;
    }
    p = session->project;
    /* Folders: derive from record paths (root "" + every distinct
     * ancestor dir). Deterministic: sorted unique. */
    {
        uint32_t need = p->record_count * 4u + 1u;

        if (need > p->folder_cap) {
            led_browser_folder *fresh =
                (led_browser_folder *)realloc(p->folders,
                                              need *
                                                  sizeof(*fresh));

            if (fresh == NULL) {
                return p->view_count;
            }
            p->folders = fresh;
            p->folder_cap = need;
        }
        p->folder_count = 0;
        /* Root folder always first. */
        memset(&p->folders[0], 0, sizeof(p->folders[0]));
        p->folder_count = 1;
        for (i = 0; i < p->record_count; i++) {
            const char *path = p->records[i].source_path;
            char prefix[1024];
            size_t k = 0;

            while (path[k] != '\0') {
                if (path[k] == '/') {
                    uint32_t f;

                    if (k >= sizeof(prefix)) {
                        break;
                    }
                    memcpy(prefix, path, k);
                    prefix[k] = '\0';
                    {
                        int known = 0;

                        for (f = 0; f < p->folder_count; f++) {
                            if (strcmp(p->folders[f].path,
                                       prefix) == 0) {
                                known = 1;
                                break;
                            }
                        }
                        if (!known &&
                            p->folder_count < p->folder_cap) {
                            strncpy(p->folders[p->folder_count]
                                        .path,
                                    prefix,
                                    sizeof(p->folders[0].path) -
                                        1);
                            p->folder_count++;
                        }
                    }
                }
                k++;
            }
        }
        /* Sort folders (root "" stays first). */
        {
            uint32_t a;
            uint32_t b;

            for (a = 1; a < p->folder_count; a++) {
                for (b = a + 1u; b < p->folder_count; b++) {
                    if (strcmp(p->folders[b].path,
                               p->folders[a].path) < 0) {
                        led_browser_folder t = p->folders[a];

                        p->folders[a] = p->folders[b];
                        p->folders[b] = t;
                    }
                }
            }
        }
        /* Counts per folder. */
        for (i = 0; i < p->folder_count; i++) {
            uint32_t r;

            p->folders[i].asset_count = 0;
            p->folders[i].folder_count = 0;
            for (r = 0; r < p->record_count; r++) {
                const char *sp = p->records[r].source_path;
                const char *fp = p->folders[i].path;

                if (fp[0] == '\0') {
                    if (strchr(sp, '/') == NULL) {
                        p->folders[i].asset_count++;
                    }
                } else {
                    size_t fl = strlen(fp);

                    if (strncmp(sp, fp, fl) == 0 &&
                        sp[fl] == '/') {
                        const char *rest = sp + fl + 1;

                        if (strchr(rest, '/') == NULL) {
                            p->folders[i].asset_count++;
                        }
                    }
                }
            }
            /* Subfolders: folders whose parent is this one. */
            {
                uint32_t f;

                for (f = 0; f < p->folder_count; f++) {
                    const char *fp = p->folders[f].path;
                    const char *mp = p->folders[i].path;

                    if (f == i || fp[0] == '\0') {
                        continue;
                    }
                    if (mp[0] == '\0') {
                        if (strchr(fp, '/') == NULL) {
                            p->folders[i].folder_count++;
                        }
                    } else {
                        size_t ml = strlen(mp);

                        if (strncmp(fp, mp, ml) == 0 &&
                            fp[ml] == '/') {
                            const char *rest = fp + ml + 1;

                            if (strchr(rest, '/') == NULL) {
                                p->folders[i].folder_count++;
                            }
                        }
                    }
                }
            }
        }
    }
    /* View: visible record indices in sort order. */
    {
        uint32_t need = (p->record_count > 0) ? p->record_count
                                              : 1u;

        if (need > p->view_cap) {
            uint32_t *fresh = (uint32_t *)realloc(
                p->view_indices, need * sizeof(*fresh));

            if (fresh == NULL) {
                return p->view_count;
            }
            p->view_indices = fresh;
            p->view_cap = need;
        }
        p->view_count = 0;
        for (i = 0; i < p->record_count; i++) {
            if (led_browser_record_visible(p, i)) {
                p->view_indices[p->view_count++] = i;
            }
        }
        {
            uint32_t a;
            uint32_t b;

            for (a = 0; a < p->view_count; a++) {
                for (b = a + 1u; b < p->view_count; b++) {
                    if (led_browser_view_cmp(
                            p, p->view_indices[b],
                            p->view_indices[a]) < 0) {
                        uint32_t t = p->view_indices[a];

                        p->view_indices[a] = p->view_indices[b];
                        p->view_indices[b] = t;
                    }
                }
            }
        }
    }
    /* Prune browser selection to live records. */
    {
        uint32_t w = 0;
        uint32_t s;

        for (s = 0; s < p->browser_sel.count; s++) {
            uint32_t r;

            for (r = 0; r < p->record_count; r++) {
                if (led_project_id_equal(
                        &p->records[r].id,
                        &p->browser_sel.ids[s])) {
                    p->browser_sel.ids[w++] =
                        p->browser_sel.ids[s];
                    break;
                }
            }
        }
        p->browser_sel.count = w;
    }
    return p->view_count;
}

uint32_t led_browser_folder_count(const led_session *session) {
    if (session == NULL || session->project == NULL) {
        return 0;
    }
    return session->project->folder_count;
}

const led_browser_folder *led_browser_folders(
    const led_session *session) {
    if (session == NULL || session->project == NULL ||
        session->project->folder_count == 0) {
        return NULL;
    }
    return session->project->folders;
}

uint32_t led_browser_asset_count(const led_session *session) {
    if (session == NULL || session->project == NULL) {
        return 0;
    }
    return session->project->view_count;
}

static void led_db_record_to_snapshot_pub(
    const led_db_record *r, led_asset_record *out) {
    memset(out, 0, sizeof(*out));
    out->id = r->id;
    led_project_id_to_hex(&r->id, out->id_hex);
    out->type = r->type;
    strncpy(out->source_path, r->source_path,
            sizeof(out->source_path) - 1);
    out->status = r->status;
    out->fingerprint_size = r->fp_size;
    out->fingerprint_hash = r->fp_hash;
    strncpy(out->importer, r->importer, sizeof(out->importer) - 1);
    out->importer_version = r->importer_version;
    out->settings_digest = r->settings_digest;
    out->dependency_count = r->dep_count;
    out->sub_asset_count = r->sub_count;
    out->runtime_asset = r->runtime_asset;
    out->has_runtime_asset = r->has_runtime_asset;
    out->runtime_id = r->runtime_id;
    out->has_runtime_id = r->has_runtime_id;
    strncpy(out->diagnostic, r->diagnostic,
            sizeof(out->diagnostic) - 1);
}

int led_browser_asset_at(const led_session *session, uint32_t index,
                         led_asset_record *out_record) {
    if (out_record != NULL) {
        memset(out_record, 0, sizeof(*out_record));
    }
    if (session == NULL || session->project == NULL ||
        out_record == NULL) {
        return 0;
    }
    if (index >= session->project->view_count) {
        return 0;
    }
    {
        uint32_t rec =
            session->project->view_indices[index];

        if (rec >= session->project->record_count) {
            return 0;
        }
        led_db_record_to_snapshot_pub(
            &session->project->records[rec], out_record);
    }
    return 1;
}

led_result led_browser_set_filter(led_session *session,
                                  led_project_asset_type type,
                                  const char *search,
                                  int sort_mode) {
    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (session->project == NULL) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if ((int)type < 0 ||
        type > LED_PROJECT_ASSET_TYPE_COUNT) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (sort_mode < 0 || sort_mode > 2) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    session->project->view_filter = type;
    if (search == NULL) {
        session->project->view_search[0] = '\0';
    } else {
        strncpy(session->project->view_search, search,
                sizeof(session->project->view_search) - 1);
        session->project
            ->view_search[sizeof(session->project->view_search) -
                          1] = '\0';
    }
    session->project->view_sort = sort_mode;
    led_browser_refresh(session);
    return LED_SUCCESS;
}

/* ---- selection by UUID ---- */

led_result led_browser_select(led_session *session,
                              const led_project_asset_id *id) {
    uint32_t i;

    if (session == NULL || id == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (session->project == NULL) {
        return LED_ERROR_NOT_ATTACHED;
    }
    {
        led_project *p = session->project;
        int known = 0;

        for (i = 0; i < p->record_count; i++) {
            if (led_project_id_equal(&p->records[i].id, id)) {
                known = 1;
                break;
            }
        }
        if (!known) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        for (i = 0; i < p->browser_sel.count; i++) {
            if (led_project_id_equal(&p->browser_sel.ids[i],
                                     id)) {
                return LED_SUCCESS;
            }
        }
        if (p->browser_sel.count >= 256) {
            return LED_ERROR_OVERFLOW;
        }
        p->browser_sel.ids[p->browser_sel.count++] = *id;
    }
    return LED_SUCCESS;
}

led_result led_browser_deselect(led_session *session,
                                const led_project_asset_id *id) {
    uint32_t i;
    uint32_t j;

    if (session == NULL || id == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (session->project == NULL) {
        return LED_ERROR_NOT_ATTACHED;
    }
    for (i = 0; i < session->project->browser_sel.count; i++) {
        if (led_project_id_equal(
                &session->project->browser_sel.ids[i], id)) {
            for (j = i + 1u;
                 j < session->project->browser_sel.count; j++) {
                session->project->browser_sel.ids[j - 1u] =
                    session->project->browser_sel.ids[j];
            }
            session->project->browser_sel.count--;
            return LED_SUCCESS;
        }
    }
    return LED_SUCCESS;
}

led_result led_browser_clear_selection(led_session *session) {
    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (session->project == NULL) {
        return LED_ERROR_NOT_ATTACHED;
    }
    session->project->browser_sel.count = 0;
    return LED_SUCCESS;
}

uint32_t led_browser_get_selection(
    const led_session *session, led_project_asset_id *out_ids,
    uint32_t capacity) {
    uint32_t n = 0;

    if (session == NULL || session->project == NULL) {
        return 0;
    }
    n = session->project->browser_sel.count;
    if (out_ids != NULL && capacity > 0) {
        uint32_t want = (n < capacity) ? n : capacity;
        uint32_t i;

        for (i = 0; i < want; i++) {
            out_ids[i] = session->project->browser_sel.ids[i];
        }
    }
    return n;
}

/* ---- headless asset inspector ---- */

uint32_t led_browser_inspect(led_session *session,
                             const led_project_asset_id *id) {
    led_project *p = NULL;
    uint32_t i;
    int idx = -1;
    uint32_t n = 0;

    if (session == NULL) {
        return 0;
    }
    session->project->browser_inspector_count = 0;
    if (id == NULL || session->project == NULL) {
        return 0;
    }
    p = session->project;
    for (i = 0; i < p->record_count; i++) {
        if (led_project_id_equal(&p->records[i].id, id)) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) {
        return 0;
    }
    {
        const led_db_record *r = &p->records[idx];
        led_inspector_row *rows = p->browser_inspector;
        char idhex[33];
        char runhex[33];

        led_project_id_to_hex(&r->id, idhex);
#define LED_BROWSER_ROW(path_, label_, type_, str_)          \
    do {                                                     \
        led_inspector_row *row = &rows[n++];                 \
        memset(row, 0, sizeof(*row));                        \
        row->desc.path = path_;                              \
        row->desc.label = label_;                            \
        row->desc.component = LE_COMPONENT_TRANSFORM;        \
        row->desc.type = type_;                              \
        row->desc.read_only = 1;                             \
        row->has_value = 1;                                  \
        row->value.type = type_;                             \
        strncpy(row->value.string_value, str_,               \
                sizeof(row->value.string_value) - 1);        \
    } while (0)
        LED_BROWSER_ROW("asset.id", "Asset ID", LED_DATA_STRING,
                        idhex);
        {
            const char *tn = "unknown";

            switch (r->type) {
            case LED_PROJECT_ASSET_MODEL:
                tn = "model";
                break;
            case LED_PROJECT_ASSET_TEXTURE:
                tn = "texture";
                break;
            case LED_PROJECT_ASSET_SCRIPT:
                tn = "script";
                break;
            case LED_PROJECT_ASSET_SCENE:
                tn = "scene";
                break;
            case LED_PROJECT_ASSET_PREFAB:
                tn = "prefab";
                break;
            default:
                break;
            }
            LED_BROWSER_ROW("asset.type", "Type", LED_DATA_STRING,
                            tn);
        }
        LED_BROWSER_ROW("asset.source", "Source",
                        LED_DATA_STRING, r->source_path);
        {
            const char *st = "unimported";

            switch (r->status) {
            case LED_IMPORT_READY:
                st = "ready";
                break;
            case LED_IMPORT_STALE:
                st = "stale";
                break;
            case LED_IMPORT_FAILED:
                st = "failed";
                break;
            case LED_IMPORT_MISSING:
                st = "missing";
                break;
            case LED_IMPORT_UNSUPPORTED:
                st = "unsupported";
                break;
            default:
                break;
            }
            LED_BROWSER_ROW("asset.status", "Status",
                            LED_DATA_STRING, st);
        }
        LED_BROWSER_ROW("asset.importer", "Importer",
                        LED_DATA_STRING,
                        r->importer[0] != '\0' ? r->importer
                                               : "-");
        if (r->has_runtime_id) {
            le_asset_id_to_string(&r->runtime_id, runhex);
            LED_BROWSER_ROW("asset.runtime_id", "Runtime ID",
                            LED_DATA_STRING, runhex);
        }
        LED_BROWSER_ROW("asset.diagnostic", "Diagnostic",
                        LED_DATA_STRING,
                        r->diagnostic[0] != '\0' ? r->diagnostic
                                                 : "-");
#undef LED_BROWSER_ROW
    }
    p->browser_inspector_count = n;
    return n;
}

/* ---- drag / drop ---- */

int led_drag_begin(led_session *session,
                   led_drag_payload *out_payload) {
    if (out_payload != NULL) {
        memset(out_payload, 0, sizeof(*out_payload));
    }
    if (session == NULL) {
        return 0;
    }
    if (session->project == NULL) {
        return 0;
    }
    if (session->project->browser_sel.count != 1) {
        return 0;
    }
    {
        led_project *p = session->project;
        uint32_t i;

        for (i = 0; i < p->record_count; i++) {
            if (led_project_id_equal(
                    &p->records[i].id,
                    &p->browser_sel.ids[0])) {
                if (out_payload != NULL) {
                    out_payload->asset = p->records[i].id;
                    out_payload->type = p->records[i].type;
                }
                p->drag_active = 1;
                if (out_payload != NULL) {
                    p->drag_payload = *out_payload;
                }
                return 1;
            }
        }
    }
    return 0;
}

static led_db_record *led_drag_record(
    led_session *session, const led_drag_payload *payload) {
    uint32_t i;

    if (session == NULL || session->project == NULL ||
        payload == NULL) {
        return NULL;
    }
    for (i = 0; i < session->project->record_count; i++) {
        led_db_record *r = &session->project->records[i];

        if (led_project_id_equal(&r->id, &payload->asset)) {
            return r;
        }
    }
    return NULL;
}

int led_drop_model_into_scene(led_session *session,
                              const led_drag_payload *payload,
                              const float position[3]) {
    led_db_record *r = NULL;
    led_command cmd;

    if (session == NULL || payload == NULL) {
        return 0;
    }
    if (!led_is_attached(session) || session->project == NULL) {
        return 0;
    }
    if (session->playing) {
        return 0;
    }
    r = led_drag_record(session, payload);
    if (r == NULL || !r->has_runtime_asset ||
        session->engine == NULL ||
        !le_asset_is_alive(session->engine, &r->runtime_asset)) {
        return 0;
    }
    /* Model records publish the first mesh as representative; mesh
     * sub-asset drops carry the sub-asset handle instead. For the
     * model drop we need a MATERIAL too — report 0 when the model
     * has no material sub-asset resolved (honest, no fallback
     * invention). The material comes from the first material
     * sub-asset re-resolved through the registry by persistent
     * ID. */
    {
        le_asset material = LE_ASSET_INVALID;
        uint32_t i;

        for (i = 0; i < r->sub_count; i++) {
            if (r->sub_keys[i] != NULL &&
                strncmp(r->sub_keys[i], "mat", 3) == 0) {
                if (le_asset_find_by_id(session->engine,
                                        &r->sub_ids[i],
                                        &material)) {
                    break;
                }
                material = LE_ASSET_INVALID;
            }
        }
        if (!le_asset_is_alive(session->engine, &material)) {
            return 0;
        }
        /* CREATE via normal command, then attach the asset
         * renderable to the SELECTION the drop leaves behind is
         * wrong — led_execute does not select. Instead snapshot
         * the live set BEFORE the CREATE and take the census
         * diff after (exact even with slot recycling and
         * parented children, which sort with all live objects —
         * a tail rule attaches the drop to a camera child once
         * one exists: real defect R-008, found live in the
         * Shadow scene). */
        {
            uint32_t nbefore = le_world_get_object_count(
                session->edit_world);
            le_object *before = NULL;

            if (nbefore > 0) {
                before = (le_object *)malloc(
                    nbefore * sizeof(*before));
                if (before != NULL) {
                    uint32_t gotb = le_world_get_all_objects(
                        session->edit_world, before,
                        nbefore);

                    if (gotb != nbefore) {
                        free(before);
                        before = NULL;
                        nbefore = 0;
                    }
                }
            }
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = LED_CMD_CREATE;
            snprintf(cmd.label, sizeof(cmd.label),
                     "Drop model");
            if (led_execute(session, &cmd) != LED_SUCCESS) {
                free(before);
                return 0;
            }
            {
                uint32_t live = le_world_get_object_count(
                    session->edit_world);
                le_object *all = NULL;
                le_object born = LE_OBJECT_INVALID;
                int found = 0;

                if (live > 0) {
                    all = (le_object *)malloc(
                        live * sizeof(*all));
                    if (all != NULL) {
                        uint32_t got =
                            le_world_get_all_objects(
                                session->edit_world, all,
                                live);
                        uint32_t i;

                        for (i = 0; i < got && !found;
                             i++) {
                            uint32_t j;
                            int was_live = 0;

                            for (j = 0;
                                 j < nbefore &&
                                 before != NULL;
                                 j++) {
                                if (all[i].index ==
                                        before[j].index &&
                                    all[i].generation ==
                                        before[j]
                                            .generation &&
                                    all[i].world_tag ==
                                        before[j]
                                            .world_tag) {
                                    was_live = 1;
                                    break;
                                }
                            }
                            if (!was_live) {
                                born = all[i];
                                found = 1;
                            }
                        }
                        free(all);
                    }
                }
                free(before);
                if (!found) {
                    return 0;
                }
                {
                    le_asset_renderable_desc d;

                    memset(&d, 0, sizeof(d));
                    d.mesh = r->runtime_asset;
                    d.material = material;
                    /* Dropped models participate in shadows (opt-out
                     * is per-object via the inspector, not the
                     * drop default — a zeroed desc leaves every
                     * GUI-authored scene shadowless even under a
                     * shadow light: real defect R-009, found live
                     * in the Shadow ON/OFF pair: identical pixels
                     * with shadows enabled vs disabled). */
                    d.casts_shadow = 1;
                    d.receives_shadow = 1;
                    d.visible = 1;
                    if (le_object_add_asset_renderable(
                            session->edit_world, &born,
                            &d) != LE_SUCCESS) {
                        return 0;
                    }
                    if (position != NULL) {
                        le_object_set_position(
                            session->edit_world, &born,
                            position);
                    }
                    led_selection_set(session, &born, 1);
                    return 1;
                }
            }
        }
    }
    return 0;
}

int led_drop_material_onto_object(led_session *session,
                                  const led_drag_payload *payload,
                                  const le_object *target) {
    led_db_record *r = NULL;
    led_command cmd;

    if (session == NULL || payload == NULL || target == NULL) {
        return 0;
    }
    if (!led_is_attached(session) || session->project == NULL) {
        return 0;
    }
    if (session->playing) {
        return 0;
    }
    /* TYPE SAFETY: only material records onto objects with (or
     * receiving) asset renderables. */
    if (payload->type != LED_PROJECT_ASSET_MATERIAL) {
        return 0;
    }
    r = led_drag_record(session, payload);
    if (r == NULL || !r->has_runtime_asset ||
        session->engine == NULL ||
        !le_asset_is_alive(session->engine, &r->runtime_asset)) {
        return 0;
    }
    if (!le_object_is_alive(session->edit_world, target)) {
        return 0;
    }
    if (le_asset_get_type(session->engine, &r->runtime_asset) !=
        LE_ASSET_MATERIAL) {
        return 0;
    }
    {
        le_asset_renderable_desc cur;

        memset(&cur, 0, sizeof(cur));
        if (!le_object_get_asset_renderable(session->edit_world,
                                            target, &cur)) {
            return 0; /* needs an asset renderable to retarget */
        }
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = LED_CMD_ASSIGN_ASSET;
        snprintf(cmd.label, sizeof(cmd.label), "Assign material");
        cmd.target = *target;
        cmd.prefab.project_id = r->id;
        cmd.prefab.assign_target = *target;
        cmd.prefab.assign_role = LED_PROJECT_ASSET_MATERIAL;
        cmd.prefab.prefab_asset = r->runtime_asset;
        if (led_execute(session, &cmd) != LED_SUCCESS) {
            return 0;
        }
    }
    return 1;
}

int led_drop_script_onto_object(led_session *session,
                                const led_drag_payload *payload,
                                const le_object *target) {
    led_db_record *r = NULL;
    le_asset script;
#if defined(LUMA34A_MUT_DROP)
    /* M-drop: refuse every script drop while armed (the headed
     * H-drop proof "attached" MUST fail under this arm). */
    (void)session;
    (void)payload;
    (void)target;
    return 0;
#endif

    if (session == NULL || payload == NULL || target == NULL) {
        return 0;
    }
    if (!led_is_attached(session) || session->project == NULL) {
        return 0;
    }
    if (session->playing) {
        return 0;
    }
    if (payload->type != LED_PROJECT_ASSET_SCRIPT) {
        return 0;
    }
    r = led_drag_record(session, payload);
    if (r == NULL || !r->has_runtime_asset ||
        session->engine == NULL) {
        return 0;
    }
    script = r->runtime_asset;
    if (!le_asset_is_alive(session->engine, &script) ||
        le_asset_get_type(session->engine, &script) !=
            LE_ASSET_SCRIPT) {
        return 0;
    }
    if (!le_object_is_alive(session->edit_world, target)) {
        return 0;
    }
    /* Through the normal script-attach path (command history via
     * a synthetic component command is future work; direct attach
     * keeps Phase 32 honest — the drop returns success and the
     * scene dirty flag is set explicitly). */
    if (le_object_add_script(session->edit_world, target,
                             &script) != LE_SUCCESS) {
        return 0;
    }
    return 1;
}

led_result led_open_scene_payload(led_session *session,
                                  const led_drag_payload *payload) {
    led_db_record *r = NULL;

    if (session == NULL || payload == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session) || session->project == NULL) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (payload->type != LED_PROJECT_ASSET_SCENE) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    r = led_drag_record(session, payload);
    if (r == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    {
        char abs[2048];

        if (strlen(session->project->root) + 1 +
                strlen(r->source_path) + 1 >
            sizeof(abs)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        snprintf(abs, sizeof(abs), "%s/%s", session->project->root,
                 r->source_path);
        /* led_scene_open takes an absolute OS path (engine file
         * helpers are path-agnostic). */
        return led_scene_open(session, abs);
    }
}
