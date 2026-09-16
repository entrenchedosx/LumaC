/* Phase 32 project filesystem operations: rename/move/delete.
 * These are PROJECT operations, NOT scene undo (documented split:
 * no fake filesystem undo). Rename/move preserve the project UUID
 * (source + sidecar travel together); delete checks references
 * (scene/prefab/asset refs by persistent ID) and reports
 * dependents instead of silently breaking the project. No
 * force-delete in Phase 32.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

static int led_move_file(const char *from, const char *to) {
    /* Portable move: rename(); cross-volume fallback copies. */
    if (rename(from, to) == 0) {
        return 1;
    }
    {
        FILE *fi = fopen(from, "rb");
        FILE *fo = NULL;

        if (fi == NULL) {
            return 0;
        }
        fo = fopen(to, "wb");
        if (fo == NULL) {
            fclose(fi);
            return 0;
        }
        {
            unsigned char buf[4096];
            size_t n = 0;

            while ((n = fread(buf, 1, sizeof(buf), fi)) > 0) {
                if (fwrite(buf, 1, n, fo) != n) {
                    fclose(fi);
                    fclose(fo);
                    remove(to);
                    return 0;
                }
            }
        }
        fclose(fi);
        if (fclose(fo) != 0) {
            remove(to);
            return 0;
        }
        remove(from);
    }
    return 1;
}

led_result led_project_rename(led_session *session,
                              const char *old_rel,
                              const char *new_rel) {
    char old_norm[1024];
    char new_norm[1024];
    char old_abs[2048];
    char new_abs[2048];
    char old_side[2048 + 8];
    char new_side[2048 + 8];
    led_project *p = NULL;
    uint32_t i;
    int idx = -1;

    if (session == NULL || old_rel == NULL || new_rel == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->project == NULL) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!led_project_normalize(old_rel, old_norm) ||
        !led_project_normalize(new_rel, new_norm)) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (strcmp(old_norm, new_norm) == 0) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_project_resolve(session, old_norm, old_abs) ||
        !led_project_resolve(session, new_norm, new_abs)) {
        /* Escape or unrepresentable. */
        return LED_ERROR_INVALID_ARGUMENT;
    }
    p = session->project;
    for (i = 0; i < p->record_count; i++) {
        if (strcmp(p->records[i].source_path, old_norm) == 0) {
            idx = (int)i;
        }
        if (strcmp(p->records[i].source_path, new_norm) == 0) {
            return LED_ERROR_VALIDATION; /* destination exists */
        }
    }
    if (idx < 0) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    /* Move source + sidecar together (identity travels). */
    snprintf(old_side, sizeof(old_side), "%s.luma", old_abs);
    snprintf(new_side, sizeof(new_side), "%s.luma", new_abs);
    if (!led_move_file(old_abs, new_abs)) {
        return LED_ERROR_IO;
    }
    {
        FILE *probe = fopen(old_side, "r");

        if (probe != NULL) {
            fclose(probe);
            if (!led_move_file(old_side, new_side)) {
                /* Roll back the source move. */
                led_move_file(new_abs, old_abs);
                return LED_ERROR_IO;
            }
        }
    }
    /* Update the record (UUID preserved -> references intact)
     * and rewrite the sidecar so its `source` line names the NEW
     * path (otherwise the next scan reads a stale copy). */
    strncpy(p->records[idx].source_path, new_norm,
            sizeof(p->records[idx].source_path) - 1);
    p->records[idx].source_path[sizeof(
                                    p->records[idx].source_path) -
                                1] = '\0';
    led_sidecar_write_pub(p, &p->records[idx]);
    /* Re-sort (deterministic order) + reindex + view refresh. */
    {
        if (p->record_count > 1) {
            qsort(p->records, p->record_count,
                  sizeof(*p->records), led_record_cmp_pub);
        }
        led_db_rebuild(p);
        led_browser_refresh(session);
    }
    return LED_SUCCESS;
}

/* Reference check: scenes/prefabs referencing `id` (by runtime ID
 * text scan of their source files — portable, no registry needed).
 * Reports dependent record UUIDs via the console + count. */
static uint32_t led_find_referencers(led_session *session,
                                     const led_project_asset_id *id,
                                     char details[512]) {
    led_project *p = session->project;
    uint32_t n = 0;
    uint32_t i;
    char idhex[33];
    char runhex[33];

    (void)idhex;
    details[0] = '\0';
    for (i = 0; i < p->record_count; i++) {
        led_db_record *r = &p->records[i];
        uint32_t k;

        if (led_project_id_equal(&r->id, id)) {
            continue;
        }
        /* Direct DB dependency edges. */
        for (k = 0; k < r->dep_count; k++) {
            if (led_project_id_equal(&r->deps[k], id)) {
                n++;
                snprintf(details + strlen(details),
                         512 - strlen(details), "%s ",
                         r->source_path);
                break;
            }
        }
        /* Runtime-ID text refs inside scene/prefab sources. */
        if ((r->type == LED_PROJECT_ASSET_SCENE ||
             r->type == LED_PROJECT_ASSET_PREFAB) &&
            r->has_runtime_id) {
            (void)runhex;
        }
    }
    /* Scene-file text scan: look for the doomed record's runtime
     * hex inside scene/prefab sources (bridge §162: scenes store
     * le_asset_id hex). */
    {
        int doomed = -1;

        for (i = 0; i < p->record_count; i++) {
            if (led_project_id_equal(&p->records[i].id, id)) {
                doomed = (int)i;
                break;
            }
        }
        if (doomed >= 0 && p->records[doomed].has_runtime_id) {
            char hex[33];

            le_asset_id_to_string(&p->records[doomed].runtime_id,
                                  hex);
            for (i = 0; i < p->record_count; i++) {
                led_db_record *r = &p->records[i];

                if ((int)i == doomed ||
                    (r->type != LED_PROJECT_ASSET_SCENE &&
                     r->type != LED_PROJECT_ASSET_PREFAB)) {
                    continue;
                }
                {
                    char abs[2048];
                    FILE *f = NULL;

                    snprintf(abs, sizeof(abs), "%s/%s", p->root,
                             r->source_path);
                    f = fopen(abs, "rb");
                    if (f != NULL) {
                        /* Chunked substring search (64KB window
                         * + 32B overlap for boundary hex). */
                        char *win = NULL;

                        win = (char *)malloc(65536 + 32);
                        if (win != NULL) {
                            size_t got = 0;
                            size_t carry = 0;
                            int hit = 0;

                            while ((got = fread(win + carry, 1,
                                                65536,
                                                f)) > 0) {
                                win[carry + got] = '\0';
                                if (strstr(win, hex) != NULL) {
                                    hit = 1;
                                    break;
                                }
                                if (got < 65536) {
                                    break;
                                }
                                memmove(win, win + 65536, 32);
                                carry = 32;
                            }
                            free(win);
                            if (hit) {
                                n++;
                                snprintf(
                                    details + strlen(details),
                                    512 - strlen(details), "%s ",
                                    r->source_path);
                            }
                        }
                        fclose(f);
                    }
                }
            }
        }
    }
    return n;
}

led_result led_project_delete(led_session *session,
                              const char *rel_path) {
    char norm[1024];
    char abs[2048];
    char side[2048 + 8];
    led_project *p = NULL;
    uint32_t i;
    int idx = -1;

    if (session == NULL || rel_path == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->project == NULL) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (!led_project_normalize(rel_path, norm)) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_project_resolve(session, norm, abs)) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    p = session->project;
    for (i = 0; i < p->record_count; i++) {
        if (strcmp(p->records[i].source_path, norm) == 0) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    /* Reference check (no silent breakage, no force-delete). */
    {
        char details[512];

        memset(details, 0, sizeof(details));
        if (led_find_referencers(session, &p->records[idx].id,
                                 details) > 0) {
            char msg[640];

            snprintf(msg, sizeof(msg), "referenced by %s",
                     details);
            led_console_push(session, LED_LOG_WARNING, "delete",
                             msg);
            return LED_ERROR_VALIDATION;
        }
    }
    /* Remove source + sidecar; drop the record (order kept). */
    remove(abs);
    snprintf(side, sizeof(side), "%s.luma", abs);
    remove(side);
    {
        uint32_t k;

        /* Free record heap state. */
        free(p->records[idx].deps);
        if (p->records[idx].sub_keys != NULL) {
            for (k = 0; k < p->records[idx].sub_count; k++) {
                free(p->records[idx].sub_keys[k]);
            }
            free(p->records[idx].sub_keys);
        }
        free(p->records[idx].sub_ids);
        for (k = (uint32_t)idx + 1u; k < p->record_count; k++) {
            p->records[k - 1u] = p->records[k];
        }
        p->record_count--;
        {
            led_db_rebuild(p);
            led_browser_refresh(session);
        }
    }
    return LED_SUCCESS;
}
