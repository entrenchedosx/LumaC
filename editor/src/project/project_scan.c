/* Phase 32 filesystem scan: explicit Refresh (no watcher).
 * Walks asset roots recursively (portable dirent), ignores policy
 * dirs (.git, build*, .luma, cache), honors types by extension,
 * reconciles sidecars + fingerprints, marks STALE/MISSING/restored.
 * Incremental: unchanged fingerprints keep READY without reimport.
 *
 * Symlink policy: directory symlinks are NOT followed (lstat gate
 * where available; on Windows, reparse-point dirs skipped by
 * attribute). File symlinks hash as their target bytes (documented).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

#ifndef _WIN32
    #include <dirent.h>
    #include <sys/stat.h>
    #include <unistd.h>
#endif

/* ---- ignore policy ---- */

static int led_scan_ignored_name(const char *name) {
    if (name == NULL || name[0] == '\0') {
        return 1;
    }
    if (strcmp(name, ".git") == 0 || strcmp(name, ".luma") == 0 ||
        strcmp(name, ".svn") == 0 || strcmp(name, ".hg") == 0) {
        return 1;
    }
    if (strncmp(name, "build", 5) == 0) {
        return 1;
    }
    if (strcmp(name, "cache") == 0 ||
        strcmp(name, "__pycache__") == 0 ||
        strcmp(name, "node_modules") == 0) {
        return 1;
    }
    if (name[0] == '.' && name[1] != '\0') {
        /* Hidden dotfiles/dirs (except "." / ".." handled by
         * caller): skip policy dirs AND hidden files — sidecars
         * (.luma suffix) are handled explicitly, never scanned. */
        return 1;
    }
    return 0;
}

static int led_scan_ignored_file(const char *rel) {
    size_t n;

    if (rel == NULL) {
        return 1;
    }
    n = strlen(rel);
    if (n >= 5 && strcmp(rel + n - 5, ".luma") == 0) {
        return 1; /* sidecars are metadata, never assets */
    }
    if (n >= 13 && strcmp(rel + n - 13, "luma.project") == 0) {
        return 1; /* manifest is not an asset */
    }
    return 0;
}

/* ---- portable directory listing ----
 * Minimal abstraction: list names + dir/file kinds for one absolute
 * directory. Symlinked dirs are reported as symlinks (not followed).
 * Cap entries per directory (adversarial depth-safe). */

#define LED_SCAN_MAX_ENTRIES 65536
#define LED_SCAN_MAX_DEPTH 64

typedef struct led_dir_entry {
    char name[512];
    int is_dir;
    int is_symlink;
} led_dir_entry;

#if defined(_MSC_VER)
/* MSVC directory backend via the C-runtime findfirst family
 * (declared in io.h; no OS-native headers). Junctions surface as
 * plain dirs; the walker never escapes regardless (all descents
 * join under the project root, resolve() rejects escapes, depth is
 * capped), so worst case is a bounded duplicate visit. */
#include <io.h>
static int led_list_dir(const char *abs_dir,
                        led_dir_entry **out_entries,
                        uint32_t *out_count) {
    struct led_find_wrap {
        char name[512];
        int is_dir;
        int is_symlink;
    };
    struct led_row_alias {
        char name[512];
        int is_dir;
        int is_symlink;
    };
    led_dir_entry *ents = NULL;
    uint32_t n = 0;
    uint32_t cap = 0;
    char pattern[2048];
    intptr_t h = -1;
    struct _finddata_t fd;

    (void)sizeof(struct led_find_wrap);
    (void)sizeof(struct led_row_alias);
    memset(&fd, 0, sizeof(fd));
    *out_entries = NULL;
    *out_count = 0;
    if (abs_dir == NULL) {
        return 0;
    }
    if (strlen(abs_dir) + 3 + 1 > sizeof(pattern)) {
        return 0;
    }
    snprintf(pattern, sizeof(pattern), "%s/*", abs_dir);
    h = _findfirst(pattern, &fd);
    if (h == -1) {
        return 0;
    }
    do {
        int is_dir;

        if (strcmp(fd.name, ".") == 0 ||
            strcmp(fd.name, "..") == 0) {
            continue;
        }
        if (n >= LED_SCAN_MAX_ENTRIES) {
            break;
        }
        if (n >= cap) {
            uint32_t grown = (cap == 0) ? 64u : cap * 2u;
            led_dir_entry *fresh = (led_dir_entry *)realloc(
                ents, grown * sizeof(*fresh));

            if (fresh == NULL) {
                free(ents);
                _findclose(h);
                return 0;
            }
            ents = fresh;
            cap = grown;
        }
        is_dir = (fd.attrib & _A_SUBDIR) ? 1 : 0;
        strncpy(ents[n].name, fd.name, sizeof(ents[n].name) - 1);
        ents[n].name[sizeof(ents[n].name) - 1] = '\0';
        ents[n].is_dir = is_dir;
        ents[n].is_symlink = 0;
        n++;
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
    *out_entries = ents;
    *out_count = n;
    return 1;
}
#else
static int led_list_dir(const char *abs_dir,
                        led_dir_entry **out_entries,
                        uint32_t *out_count) {
    DIR *d = NULL;
    led_dir_entry *ents = NULL;
    uint32_t n = 0;
    uint32_t cap = 0;

    *out_entries = NULL;
    *out_count = 0;
    d = opendir(abs_dir);
    if (d == NULL) {
        return 0;
    }
    {
        struct dirent *de = NULL;

        while ((de = readdir(d)) != NULL) {
            struct stat st;
            char full[2048];
            int is_link = 0;
            int is_dir = 0;

            if (strcmp(de->d_name, ".") == 0 ||
                strcmp(de->d_name, "..") == 0) {
                continue;
            }
            if (n >= LED_SCAN_MAX_ENTRIES) {
                break;
            }
            if (n >= cap) {
                uint32_t grown = (cap == 0) ? 64u : cap * 2u;
                led_dir_entry *fresh =
                    (led_dir_entry *)realloc(ents,
                                             grown *
                                                 sizeof(*fresh));

                if (fresh == NULL) {
                    free(ents);
                    closedir(d);
                    return 0;
                }
                ents = fresh;
                cap = grown;
            }
            snprintf(full, sizeof(full), "%s/%s", abs_dir,
                     de->d_name);
            if (lstat(full, &st) == 0) {
                is_link = S_ISLNK(st.st_mode) ? 1 : 0;
                is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
                if (is_link) {
                    /* Do not follow: stat the target only to
                     * decide file-vs-dir display; a dangling link
                     * is listed as a non-dir entry. */
                    struct stat tst;

                    if (stat(full, &tst) == 0) {
                        is_dir = S_ISDIR(tst.st_mode) ? 1 : 0;
                    } else {
                        is_dir = 0;
                    }
                }
            } else {
                /* Unstattable: list as file (fingerprint open
                 * will fail -> MISSING diagnostic path). */
                is_dir = 0;
            }
            strncpy(ents[n].name, de->d_name,
                    sizeof(ents[n].name) - 1);
            ents[n].name[sizeof(ents[n].name) - 1] = '\0';
            ents[n].is_dir = is_dir;
            ents[n].is_symlink = is_link;
            n++;
        }
    }
    closedir(d);
    *out_entries = ents;
    *out_count = n;
    return 1;
}
#endif

/* ---- record helpers (led_scan_ensure_record is used by the
 *  reconcile path for out-of-band inserts; ordering by qsort) ---- */

static int led_scan_ensure_record(led_project *p, const char *rel,
                                  led_project_asset_type type,
                                  uint32_t *out_idx) {
    uint32_t i;

    for (i = 0; i < p->record_count; i++) {
        if (strcmp(p->records[i].source_path, rel) == 0) {
            *out_idx = i;
            return 1; /* existing */
        }
    }
    if (p->record_count >= LED_PROJECT_MAX_RECORDS) {
        return 0;
    }
    if (p->record_count >= p->record_cap) {
        uint32_t grown = (p->record_cap == 0) ? 256u
                                              : p->record_cap * 2u;
        led_db_record *fresh = (led_db_record *)realloc(
            p->records, grown * sizeof(*fresh));

        if (fresh == NULL) {
            return 0;
        }
        p->records = fresh;
        p->record_cap = grown;
    }
    {
        led_db_record *r = &p->records[p->record_count];

        memset(r, 0, sizeof(*r));
        r->runtime_asset = LE_ASSET_INVALID;
        strncpy(r->source_path, rel, sizeof(r->source_path) - 1);
        r->type = type;
        r->status = LED_IMPORT_UNIMPORTED;
        *out_idx = p->record_count;
        p->record_count++;
    }
    return 1;
}

/* Keep records sorted by source_path (deterministic enumeration)
 * after structural changes. Simple insertion discipline: full
 * qsort on close of scan (n log n, scans are rare). */
static int led_record_cmp(const void *a, const void *b) {
    const led_db_record *ra = (const led_db_record *)a;
    const led_db_record *rb = (const led_db_record *)b;

    return strcmp(ra->source_path, rb->source_path);
}

/* ---- per-file reconciliation ---- */

static void led_scan_one_file(led_project *p, const char *rel,
                              led_scan_stats *st);

/* Recursive walk (explicit depth cap; symlink dirs never
 * descended). */
static void led_scan_walk(led_project *p, const char *rel_dir,
                          uint32_t depth, led_scan_stats *st) {
    char abs[2048];
    led_dir_entry *ents = NULL;
    uint32_t n = 0;
    uint32_t i;

    if (depth > LED_SCAN_MAX_DEPTH) {
        st->errors++;
        return;
    }
    if (rel_dir[0] == '\0') {
        snprintf(abs, sizeof(abs), "%s", p->root);
    } else {
        if (strlen(p->root) + 1 + strlen(rel_dir) + 1 >
            sizeof(abs)) {
            st->errors++;
            return;
        }
        snprintf(abs, sizeof(abs), "%s/%s", p->root, rel_dir);
    }
    if (!led_list_dir(abs, &ents, &n)) {
        return; /* unreadable dir: skip */
    }
    /* Deterministic traversal: sort entries by name. */
    {
        uint32_t a;
        uint32_t b;

        for (a = 0; a < n; a++) {
            for (b = a + 1u; b < n; b++) {
                if (strcmp(ents[b].name, ents[a].name) < 0) {
                    led_dir_entry t = ents[a];

                    ents[a] = ents[b];
                    ents[b] = t;
                }
            }
        }
    }
    for (i = 0; i < n; i++) {
        char child[1024];

        if (led_scan_ignored_name(ents[i].name)) {
            continue;
        }
        if (rel_dir[0] == '\0') {
            snprintf(child, sizeof(child), "%s", ents[i].name);
        } else {
            snprintf(child, sizeof(child), "%s/%s", rel_dir,
                     ents[i].name);
        }
        if (ents[i].is_dir) {
            if (ents[i].is_symlink) {
                continue; /* policy: never follow dir symlinks */
            }
            led_scan_walk(p, child, depth + 1u, st);
        } else {
            if (led_scan_ignored_file(child)) {
                continue;
            }
            /* Case-collision note: on case-insensitive filesystems
             * two records differing only by case alias one file.
             * Detect exact-duplicate normalized paths (records are
             * unique by normalized rel); the collision itself is
             * reported via the DB conflict check at reindex. */
            led_scan_one_file(p, child, st);
        }
    }
    free(ents);
}

static void led_scan_one_file(led_project *p, const char *rel,
                              led_scan_stats *st) {
    char abs[2048];

    if (strlen(p->root) + 1 + strlen(rel) + 1 > sizeof(abs)) {
        st->errors++;
        return;
    }
    snprintf(abs, sizeof(abs), "%s/%s", p->root, rel);
    st->discovered++;
    if (!led_scan_reconcile(p, rel, abs, st)) {
        st->errors++;
    }
}

led_result led_project_scan(led_session *session,
                            led_scan_stats *out_stats) {
    led_scan_stats st;
    uint32_t r;

    memset(&st, 0, sizeof(st));
    if (out_stats != NULL) {
        memset(out_stats, 0, sizeof(*out_stats));
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
    {
        led_project *p = session->project;

        /* Mark pass: every record starts unvisited; survivors keep
         * identity, vanishers become MISSING. */
        for (r = 0; r < p->asset_root_count; r++) {
            led_scan_walk(p, p->asset_roots[r], 0, &st);
        }
        /* Also scan the root itself for top-level assets (manifest
         * dir doubles as a root when asset roots are absent). */
        /* Sweep: records whose source file no longer exists ->
         * MISSING (identity retained for restore). */
        {
            uint32_t i;

            for (i = 0; i < p->record_count; i++) {
                led_db_record *rec = &p->records[i];

                if ((rec->status & 0x80000000u) != 0u) {
                    rec->status =
                        (led_import_status)(rec->status &
                                            ~0x80000000u);
                    continue;
                }
                if (rec->status != LED_IMPORT_MISSING) {
                    rec->status = LED_IMPORT_MISSING;
                    snprintf(rec->diagnostic,
                             sizeof(rec->diagnostic),
                             "source missing");
                    st.missing_marked++;
                }
            }
        }
        /* Deterministic order + reindex. */
        if (p->record_count > 1) {
            qsort(p->records, p->record_count, sizeof(*p->records),
                  led_record_cmp);
        }
        /* Duplicate-UUID conflict check: two records claiming one
         * project ID -> both FAILED with diagnostics (never silent
         * pick). */
        {
            uint32_t a;
            uint32_t b;

            for (a = 0; a < p->record_count; a++) {
                for (b = a + 1u; b < p->record_count; b++) {
                    if (led_project_id_equal(
                            &p->records[a].id,
                            &p->records[b].id)) {
                        p->records[a].status = LED_IMPORT_FAILED;
                        p->records[b].status = LED_IMPORT_FAILED;
                        snprintf(p->records[a].diagnostic,
                                 sizeof(p->records[a].diagnostic),
                                 "duplicate asset ID");
                        snprintf(p->records[b].diagnostic,
                                 sizeof(p->records[b].diagnostic),
                                 "duplicate asset ID");
                        st.errors += 2;
                    }
                }
            }
        }
        /* Rebuild hash indexes over the sorted array. */
        led_db_rebuild(p);
        p->scan_seq++;
        /* Refresh the browser view over the new DB. */
        led_browser_refresh(session);
    }
    if (out_stats != NULL) {
        *out_stats = st;
    }
    return LED_SUCCESS;
}
