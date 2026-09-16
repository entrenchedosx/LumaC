/* Phase 32 transactional reimport: candidate-then-swap.
 *
 *   old usable asset
 *        |
 *        +----> import candidate (fresh handles)
 *        |           |
 *        |        validate
 *        |        /      \
 *        |     fail    success
 *        |      |         |
 *        |      v         v
 *        +-- keep old  atomic replace (project ID stable,
 *                          new runtime handle published)
 *
 * Scripts reuse le_script_reload semantics where the handle can be
 * preserved (recompile + rebind, values kept); mesh/material/
 * texture take the new-handle path (old handle detectably stale).
 * Failed reimport preserves last-known-good (FAILED diagnostics on
 * the record, old handle live). Reimport while PLAY is rejected.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

static int led_record_needs_reimport(led_project *p,
                                     const led_db_record *r) {
    char abs[2048];
    uint64_t fsize = 0;
    uint64_t fhash = 0;
    FILE *f = NULL;
    unsigned char buf[4096];
    size_t n = 0;
    uint64_t h = 14695981039346656037ull;
    uint64_t size = 0;

    if (strlen(p->root) + 1 + strlen(r->source_path) + 1 >
        sizeof(abs)) {
        return 1; /* treat as changed (safe direction) */
    }
    snprintf(abs, sizeof(abs), "%s/%s", p->root, r->source_path);
    f = fopen(abs, "rb");
    if (f == NULL) {
        return 0; /* missing is not stale (scan marks MISSING) */
    }
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        size_t i;

        for (i = 0; i < n; i++) {
            h ^= (uint64_t)buf[i];
            h *= 1099511628211ull;
        }
        size += (uint64_t)n;
    }
    fclose(f);
    fsize = size;
    fhash = h;
    if (fsize != r->fp_size || fhash != r->fp_hash) {
        return 1;
    }
    if (strcmp(r->importer, led_importer_id_for(r->type)) != 0 ||
        r->importer_version !=
            led_importer_version_for(r->type)) {
        return 1;
    }
    if (r->settings_digest !=
        led_import_settings_digest(r->type)) {
        return 1;
    }
    return 0;
}

led_result led_reimport_asset(led_session *session,
                              const led_project_asset_id *id,
                              int force) {
    led_project *p = NULL;
    uint32_t i;
    int idx = -1;

    if (session == NULL || id == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->project == NULL) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->playing) {
        return LED_ERROR_ALREADY_PLAYING;
    }
    p = session->project;
    for (i = 0; i < p->record_count; i++) {
        if (led_project_id_equal(&p->records[i].id, id)) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    {
        led_db_record *r = &p->records[idx];

        if (r->status == LED_IMPORT_MISSING) {
            snprintf(r->diagnostic, sizeof(r->diagnostic),
                     "cannot reimport missing source");
            return LED_ERROR_VALIDATION;
        }
        if (!force && r->status == LED_IMPORT_READY &&
            !led_record_needs_reimport(p, r)) {
            return LED_SUCCESS; /* unchanged: no-op */
        }
        /* Snapshot the good state (handles + IDs) for rollback. */
        {
            le_asset old_handle = r->runtime_asset;
            int had_old = r->has_runtime_asset;
            le_asset_id old_id = r->runtime_id;
            int had_old_id = r->has_runtime_id;

            /* Scripts: handle-preserving path first. */
            if (r->type == LED_PROJECT_ASSET_SCRIPT &&
                had_old && session->engine != NULL &&
                le_asset_is_alive(session->engine,
                                  &old_handle)) {
                char abs[2048];

                snprintf(abs, sizeof(abs), "%s/%s", p->root,
                         r->source_path);
                /* Re-read bytes + set_source (transactional in
                 * the engine: PARSE keeps old code). */
                {
                    FILE *f = fopen(abs, "rb");
                    unsigned char *buf = NULL;
                    long n = 0;
                    le_result erc;

                    if (f == NULL) {
                        r->status = LED_IMPORT_MISSING;
                        return LED_ERROR_VALIDATION;
                    }
                    if (fseek(f, 0, SEEK_END) == 0) {
                        n = ftell(f);
                    }
                    if (n <= 0 || n > 4 * 1024 * 1024 ||
                        fseek(f, 0, SEEK_SET) != 0) {
                        fclose(f);
                        r->status = LED_IMPORT_FAILED;
                        snprintf(r->diagnostic,
                                 sizeof(r->diagnostic),
                                 "script unreadable");
                        return LED_ERROR_VALIDATION;
                    }
                    buf = (unsigned char *)malloc((size_t)n);
                    if (buf == NULL) {
                        fclose(f);
                        return LED_ERROR_OUT_OF_MEMORY;
                    }
                    if (fread(buf, 1, (size_t)n, f) !=
                        (size_t)n) {
                        free(buf);
                        fclose(f);
                        r->status = LED_IMPORT_FAILED;
                        return LED_ERROR_VALIDATION;
                    }
                    fclose(f);
                    erc = le_script_asset_set_source(
                        session->engine, &old_handle,
                        (const char *)buf, (size_t)n);
                    free(buf);
                    if (erc == LE_SUCCESS) {
                        erc = le_script_reload(session->engine,
                                               &old_handle);
                    }
                    if (erc != LE_SUCCESS) {
                        /* PARSE: old code live (engine
                         * guarantee). Record FAILED but keep
                         * the good handle. */
                        r->status = LED_IMPORT_FAILED;
                        snprintf(r->diagnostic,
                                 sizeof(r->diagnostic),
                                 "script reimport failed (%d); "
                                 "last-known-good live",
                                 (int)erc);
                        led_console_push(
                            session, LED_LOG_ERROR, "reimport",
                            r->diagnostic);
                        return LED_ERROR_VALIDATION;
                    }
                    le_asset_get_id(session->engine, &old_handle,
                                    &r->runtime_id);
                    r->has_runtime_id = 1;
                    r->status = LED_IMPORT_READY;
                    r->diagnostic[0] = '\0';
                    /* Refresh fingerprint (sidecar rewrite keeps
                     * identity; fingerprint adopts new bytes). */
                    {
                        char abs2[2048];
                        FILE *ff = NULL;
                        unsigned char fbuf[4096];
                        size_t nn = 0;
                        uint64_t hh = 14695981039346656037ull;
                        uint64_t sz = 0;

                        snprintf(abs2, sizeof(abs2), "%s/%s",
                                 p->root, r->source_path);
                        ff = fopen(abs2, "rb");
                        if (ff != NULL) {
                            while ((nn = fread(fbuf, 1,
                                               sizeof(fbuf),
                                               ff)) > 0) {
                                size_t zi;

                                for (zi = 0; zi < nn; zi++) {
                                    hh ^= (uint64_t)fbuf[zi];
                                    hh *= 1099511628211ull;
                                }
                                sz += (uint64_t)nn;
                            }
                            fclose(ff);
                            r->fp_size = sz;
                            r->fp_hash = hh;
                        }
                    }
                    led_browser_refresh(session);
                    return LED_SUCCESS;
                }
            }
            /* Generic path: import candidate, swap on success. */
            {
                led_result rc = led_import_one_record(
                    session, (uint32_t)idx);

                if (rc != LED_SUCCESS) {
                    /* Roll back the record to the good state. */
                    r->runtime_asset = old_handle;
                    r->has_runtime_asset = had_old;
                    r->runtime_id = old_id;
                    r->has_runtime_id = had_old_id;
                    if (had_old && session->engine != NULL &&
                        le_asset_is_alive(session->engine,
                                          &old_handle)) {
                        r->status = LED_IMPORT_FAILED;
                        snprintf(r->diagnostic,
                                 sizeof(r->diagnostic),
                                 "reimport failed; "
                                 "last-known-good live");
                    }
                    led_console_push(session, LED_LOG_ERROR,
                                     "reimport", r->diagnostic);
                    return rc;
                }
                /* Success: retire the old handle when it differs
                 * (unload; IN_USE keeps it alive safely — the
                 * engine refuses, and we keep the new one). */
                if (had_old && session->engine != NULL &&
                    (old_handle.index !=
                         r->runtime_asset.index ||
                     old_handle.generation !=
                         r->runtime_asset.generation)) {
                    if (le_asset_is_alive(session->engine,
                                          &old_handle)) {
                        le_asset_unload(session->engine,
                                        &old_handle);
                    }
                }
                return LED_SUCCESS;
            }
        }
    }
}

led_result led_project_reimport_all(led_session *session,
                                    uint32_t *out_ok) {
    uint32_t ok = 0;
    uint32_t i;

    if (out_ok != NULL) {
        *out_ok = 0;
    }
    if (session == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->project == NULL) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (session->playing) {
        return LED_ERROR_ALREADY_PLAYING;
    }
    for (i = 0; i < session->project->record_count; i++) {
        led_db_record *r = &session->project->records[i];

        if (r->status == LED_IMPORT_STALE) {
            if (led_reimport_asset(session, &r->id, 0) ==
                LED_SUCCESS) {
                ok++;
            }
        }
    }
    if (out_ok != NULL) {
        *out_ok = ok;
    }
    return LED_SUCCESS;
}
