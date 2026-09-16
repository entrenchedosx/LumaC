/* Phase 32 project system: manifest, path canon, open/close/
 * switch, validation. Single project open per session (singleton
 * policy). No CWD dependence: all project-relative paths resolve
 * against the stored absolute root.
 *
 * Manifest `luma.project` (line discipline, hand parser, no new
 * dependency):
 *   LUMA_PROJECT 1
 *   name <...>
 *   startup_scene <rel>|-
 *   asset_root <rel>        (repeatable, default Assets/ + Scenes/)
 *   window <w> <h> <title...>
 * Unknown fields tolerated (forward compat); unknown versions fail
 * loudly; missing manifest fails IO.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
    #include <direct.h>
#else
    #include <sys/stat.h>
    #include <sys/types.h>
#endif

#include "internal/editor_internal.h"

#define LED_MANIFEST_NAME "luma.project"
#define LED_MANIFEST_MAGIC "LUMA_PROJECT 1"

/* ---- shared identity helpers (project + DB + prefab) ---- */

uint64_t led_fnv1a64(const void *bytes, size_t size) {
    const unsigned char *p = (const unsigned char *)bytes;
    uint64_t h = 14695981039346656037ull;
    size_t i;

    if (p == NULL) {
        return h;
    }
    for (i = 0; i < size; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

int led_project_id_equal(const led_project_asset_id *a,
                         const led_project_asset_id *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    return (a->hi == b->hi && a->lo == b->lo) ? 1 : 0;
}

void led_project_id_to_hex(const led_project_asset_id *id,
                           char out_hex[33]) {
    static const char digits[] = "0123456789abcdef";
    int i;

    if (out_hex == NULL) {
        return;
    }
    memset(out_hex, '0', 32);
    out_hex[32] = '\0';
    if (id == NULL) {
        return;
    }
    for (i = 0; i < 16; i++) {
        out_hex[15 - i] = digits[(id->hi >> (4 * i)) & 0xFu];
        out_hex[31 - i] = digits[(id->lo >> (4 * i)) & 0xFu];
    }
}

static int led_hexval(char c, unsigned *out) {
    if (c >= '0' && c <= '9') {
        *out = (unsigned)(c - '0');
        return 1;
    }
    if (c >= 'a' && c <= 'f') {
        *out = (unsigned)(c - 'a') + 10u;
        return 1;
    }
    if (c >= 'A' && c <= 'F') {
        *out = (unsigned)(c - 'A') + 10u;
        return 1;
    }
    return 0;
}

int led_project_id_from_hex(const char *hex,
                            led_project_asset_id *out_id) {
    uint64_t hi = 0;
    uint64_t lo = 0;
    int i;

    if (out_id != NULL) {
        memset(out_id, 0, sizeof(*out_id));
    }
    if (hex == NULL || out_id == NULL || strlen(hex) != 32) {
        return 0;
    }
    for (i = 0; i < 32; i++) {
        unsigned v = 0;

        if (!led_hexval(hex[i], &v)) {
            return 0;
        }
        if (i < 16) {
            hi = (hi << 4) | v;
        } else {
            lo = (lo << 4) | v;
        }
    }
    if (hi == 0 && lo == 0) {
        return 0;
    }
    out_id->hi = hi;
    out_id->lo = lo;
    return 1;
}

void led_project_id_mint(led_project_asset_id *out_id) {
    /* Counter-seeded splitmix64 (mirrors le_uuid_mint discipline:
     * unique per process, no RNG, never nil). Authoring identity —
     * never derived from content. */
    static uint64_t counter = 0;
    uint64_t z;

    if (out_id == NULL) {
        return;
    }
    counter += 0x9e3779b97f4a7c15ull;
    z = counter + (uint64_t)(uintptr_t)out_id;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z = z ^ (z >> 31);
    out_id->hi = z;
    z = counter * 0xbf58476d1ce4e5b9ull +
        (uint64_t)(uintptr_t)&counter;
    z = (z ^ (z >> 30)) * 0x94d049bb133111ebull;
    z = z ^ (z >> 31);
    out_id->lo = (z != 0) ? z : 1u;
    if (out_id->hi == 0 && out_id->lo == 0) {
        out_id->lo = 1u;
    }
}

/* ---- authoritative path normalization (lexical, no FS) ----
 * `/` separators, backslash folding, collapsed duplicates,
 * resolved `.`/`..` (leading `..` retained lexically so resolve()
 * can reject escapes), no trailing slash, 1023-char cap. */

int led_project_normalize(const char *path, char out[1024]) {
    char scratch[1024];
    size_t offsets[256];
    size_t lengths[256];
    size_t nseg = 0;
    size_t i = 0;
    size_t len;
    size_t pos = 0;

    if (path == NULL || out == NULL) {
        return 0;
    }
    len = strlen(path);
    if (len == 0 || len >= sizeof(scratch)) {
        return 0;
    }
    for (i = 0; i <= len; i++) {
        char c = path[i];

        if (c == '\\') {
            c = '/';
        }
        scratch[i] = c;
    }
    /* Absolute inputs are NOT project-relative: reject drive and
     * leading-slash roots here (resolve() enforces the same). */
    if (scratch[0] == '/') {
        return 0;
    }
    if (len >= 3 &&
        ((scratch[0] >= 'A' && scratch[0] <= 'Z') ||
         (scratch[0] >= 'a' && scratch[0] <= 'z')) &&
        scratch[1] == ':' && scratch[2] == '/') {
        return 0;
    }
    i = 0;
    while (i <= len) {
        size_t start = i;

        while (i < len && scratch[i] != '/') {
            i++;
        }
        {
            size_t seglen = i - start;

            if (seglen == 0) {
                /* skip */
            } else if (seglen == 1 && scratch[start] == '.') {
                /* skip */
            } else if (seglen == 2 && scratch[start] == '.' &&
                       scratch[start + 1] == '.') {
                if (nseg > 0 && !(lengths[nseg - 1] == 2 &&
                                  scratch[offsets[nseg - 1]] == '.' &&
                                  scratch[offsets[nseg - 1] + 1] ==
                                      '.')) {
                    nseg--;
                } else if (nseg < 256) {
                    offsets[nseg] = start;
                    lengths[nseg] = seglen;
                    nseg++;
                } else {
                    return 0;
                }
            } else {
                if (nseg >= 256) {
                    return 0;
                }
                offsets[nseg] = start;
                lengths[nseg] = seglen;
                nseg++;
            }
        }
        i++;
    }
    if (nseg == 0) {
        return 0;
    }
    {
        size_t k;
        int first = 1;

        for (k = 0; k < nseg; k++) {
            size_t j;

            if (!first) {
                if (pos + 1 >= 1024) {
                    return 0;
                }
                out[pos++] = '/';
            }
            first = 0;
            if (pos + lengths[k] + 1 > 1024) {
                return 0;
            }
            for (j = 0; j < lengths[k]; j++) {
                out[pos++] = scratch[offsets[k] + j];
            }
        }
    }
    if (pos + 1 > 1024) {
        return 0;
    }
    out[pos] = '\0';
    return (pos > 0) ? 1 : 0;
}

/* Resolve rel (project-relative) against the open root. Rejects:
 * leading `..` escapes, absolute paths, drive paths, embedded NUL
 * (via strlen discipline), overlong joins. */
int led_project_resolve(const led_session *session,
                        const char *rel_path,
                        char out_absolute[2048]) {
    char norm[1024];
    size_t rl;
    size_t i;

    if (out_absolute != NULL) {
        out_absolute[0] = '\0';
    }
    if (session == NULL || rel_path == NULL ||
        out_absolute == NULL) {
        return 0;
    }
    if (session->project == NULL) {
        return 0;
    }
    if (!led_project_normalize(rel_path, norm)) {
        return 0;
    }
    /* Leading `..` (or anything resolving above root) rejected. */
    if ((norm[0] == '.' && norm[1] == '.' &&
         (norm[2] == '/' || norm[2] == '\0'))) {
        return 0;
    }
    for (i = 0; norm[i] != '\0'; i++) {
        if (norm[i] == '\0') {
            return 0;
        }
    }
    rl = strlen(session->project->root);
    if (rl == 0 || rl + 1 + strlen(norm) + 1 > 2048) {
        return 0;
    }
    memcpy(out_absolute, session->project->root, rl);
    out_absolute[rl] = '/';
    memcpy(out_absolute + rl + 1, norm, strlen(norm) + 1);
    return 1;
}

/* ---- manifest read/write ---- */

static led_result led_manifest_write(const char *root_abs,
                                     const char *name,
                                     const char *startup,
                                     int has_startup) {
    char path[2048];
    FILE *f;

    if (strlen(root_abs) + 1 + strlen(LED_MANIFEST_NAME) + 1 >
        sizeof(path)) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    snprintf(path, sizeof(path), "%s/%s", root_abs,
             LED_MANIFEST_NAME);
    f = fopen(path, "w");
    if (f == NULL) {
        return LED_ERROR_IO;
    }
    fprintf(f, "%s\n", LED_MANIFEST_MAGIC);
    fprintf(f, "name %s\n", (name != NULL && name[0] != '\0')
                                ? name
                                : "Untitled");
    if (has_startup && startup != NULL && startup[0] != '\0') {
        fprintf(f, "startup_scene %s\n", startup);
    } else {
        fprintf(f, "startup_scene -\n");
    }
    fprintf(f, "asset_root Assets\n");
    fprintf(f, "asset_root Scenes\n");
    fprintf(f, "window 1280 720 Luma\n");
    if (fclose(f) != 0) {
        return LED_ERROR_IO;
    }
    return LED_SUCCESS;
}

typedef struct led_manifest_data {
    char name[128];
    char startup[1024];
    int has_startup;
    char roots[8][1024];
    uint32_t root_count;
    uint32_t win_w;
    uint32_t win_h;
    char win_title[128];
} led_manifest_data;

static led_result led_manifest_parse(const char *path,
                                     led_manifest_data *out) {
    FILE *f = NULL;
    char line[2048];
    int first = 1;

    memset(out, 0, sizeof(*out));
    out->win_w = 1280;
    out->win_h = 720;
    strncpy(out->win_title, "Luma", sizeof(out->win_title) - 1);
    f = fopen(path, "r");
    if (f == NULL) {
        return LED_ERROR_IO;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        size_t n = strlen(line);

        while (n > 0 &&
               (line[n - 1] == '\n' || line[n - 1] == '\r')) {
            line[--n] = '\0';
        }
        if (n == 0 || line[0] == '#') {
            continue;
        }
        if (first) {
            first = 0;
            if (strcmp(line, LED_MANIFEST_MAGIC) != 0) {
                /* Unknown future versions fail loudly. */
                fclose(f);
                return LED_ERROR_PARSE;
            }
            continue;
        }
        if (strncmp(line, "name ", 5) == 0 && line[5] != '\0') {
            strncpy(out->name, line + 5, sizeof(out->name) - 1);
        } else if (strncmp(line, "startup_scene ", 14) == 0) {
            if (strcmp(line + 14, "-") != 0 && line[14] != '\0') {
                strncpy(out->startup, line + 14,
                        sizeof(out->startup) - 1);
                out->has_startup = 1;
            }
        } else if (strncmp(line, "asset_root ", 11) == 0 &&
                   line[11] != '\0' && out->root_count < 8) {
            char norm[1024];

            if (led_project_normalize(line + 11, norm)) {
                strncpy(out->roots[out->root_count], norm,
                        sizeof(out->roots[0]) - 1);
                out->root_count++;
            }
        } else if (strncmp(line, "window ", 7) == 0) {
            unsigned w = 0;
            unsigned h = 0;
            char title[128];

            memset(title, 0, sizeof(title));
            if (sscanf(line + 7, "%u %u %127[^\n]", &w, &h,
                       title) >= 2 &&
                w > 0 && h > 0 && w <= 16384 && h <= 16384) {
                out->win_w = w;
                out->win_h = h;
                if (title[0] != '\0') {
                    strncpy(out->win_title, title,
                            sizeof(out->win_title) - 1);
                }
            }
        }
        /* Unknown fields tolerated (forward compat). */
    }
    fclose(f);
    if (first) {
        return LED_ERROR_PARSE; /* empty file */
    }
    return LED_SUCCESS;
}

/* ---- project lifetime ---- */

static void led_project_destroy(led_project *p) {
    if (p == NULL) {
        return;
    }
    led_project_free_db(p);
    free(p->folders);
    free(p->view_indices);
    free(p);
}

led_result led_project_create(const char *root_dir,
                              const char *project_name) {
    char manifest[2048];
    FILE *probe = NULL;

    if (root_dir == NULL || root_dir[0] == '\0') {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (strlen(root_dir) + 1 + strlen(LED_MANIFEST_NAME) + 1 >
        sizeof(manifest)) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    snprintf(manifest, sizeof(manifest), "%s/%s", root_dir,
             LED_MANIFEST_NAME);
    /* Refuse to overwrite an existing manifest (use open). */
    probe = fopen(manifest, "r");
    if (probe != NULL) {
        fclose(probe);
        return LED_ERROR_IO;
    }
    {
        led_result rc = led_manifest_write(root_dir, project_name,
                                           NULL, 0);

        if (rc != LED_SUCCESS) {
            return rc;
        }
    }
    /* Best-effort standard directories (missing mkdir is IO). */
    {
        char sub[2048];

        snprintf(sub, sizeof(sub), "%s/Assets", root_dir);
#ifdef _WIN32
        _mkdir(sub);
        snprintf(sub, sizeof(sub), "%s/Scenes", root_dir);
        _mkdir(sub);
#else
        mkdir(sub, 0755);
        snprintf(sub, sizeof(sub), "%s/Scenes", root_dir);
        mkdir(sub, 0755);
#endif
    }
    return LED_SUCCESS;
}

led_result led_project_open(led_session *session,
                            const char *root_dir) {
    char manifest[2048];
    led_manifest_data md;
    led_result rc;
    led_project *p = NULL;

    if (session == NULL || root_dir == NULL ||
        root_dir[0] == '\0') {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (!led_is_attached(session)) {
        return LED_ERROR_NOT_ATTACHED;
    }
    if (strlen(root_dir) + 1 + strlen(LED_MANIFEST_NAME) + 1 >
        sizeof(manifest)) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    snprintf(manifest, sizeof(manifest), "%s/%s", root_dir,
             LED_MANIFEST_NAME);
    rc = LED_SUCCESS;
    {
        led_result pr = led_manifest_parse(manifest, &md);

        if (pr != LED_SUCCESS) {
            return pr;
        }
    }
    /* Close any open project first (switch policy). */
    led_project_close(session);
    p = (led_project *)calloc(1, sizeof(*p));
    if (p == NULL) {
        return LED_ERROR_OUT_OF_MEMORY;
    }
    /* Store the root absolute-as-given (callers pass absolute
     * paths; no CWD resolution is performed here — portability
     * holds because all joins use this stored root). */
    strncpy(p->root, root_dir, sizeof(p->root) - 1);
    /* Strip trailing slashes (keep `X:/` / `/` floors). */
    {
        size_t n = strlen(p->root);

        while (n > 1 && p->root[n - 1] == '/') {
            p->root[--n] = '\0';
        }
        /* Fold backslashes in the root once. */
        {
            size_t i;

            for (i = 0; i < n; i++) {
                if (p->root[i] == '\\') {
                    p->root[i] = '/';
                }
            }
        }
    }
    strncpy(p->name, md.name[0] != '\0' ? md.name : "Untitled",
            sizeof(p->name) - 1);
    p->format_version = LED_PROJECT_FORMAT_VERSION;
    if (md.has_startup) {
        char norm[1024];

        if (led_project_normalize(md.startup, norm)) {
            strncpy(p->startup_scene, norm,
                    sizeof(p->startup_scene) - 1);
            p->has_startup_scene = 1;
        }
    }
    {
        uint32_t i;

        for (i = 0; i < md.root_count && i < 8; i++) {
            strncpy(p->asset_roots[i], md.roots[i],
                    sizeof(p->asset_roots[i]) - 1);
        }
        p->asset_root_count = md.root_count;
        if (p->asset_root_count == 0) {
            strncpy(p->asset_roots[0], "Assets",
                    sizeof(p->asset_roots[0]) - 1);
            strncpy(p->asset_roots[1], "Scenes",
                    sizeof(p->asset_roots[1]) - 1);
            p->asset_root_count = 2;
        }
    }
    p->window_w = md.win_w;
    p->window_h = md.win_h;
    strncpy(p->window_title, md.win_title,
            sizeof(p->window_title) - 1);
    p->session = session;
    p->view_filter = LED_PROJECT_ASSET_TYPE_COUNT;
    p->view_sort = 0;
    session->project = p;
    /* Initial scan (discover + sidecars + fingerprints). Import is
     * lazy: open never uploads GPU resources. */
    {
        led_scan_stats st;

        memset(&st, 0, sizeof(st));
        rc = led_project_scan(session, &st);
        if (rc != LED_SUCCESS) {
            led_project_close(session);
            return rc;
        }
    }
    (void)rc;
    return LED_SUCCESS;
}

void led_project_close(led_session *session) {
    if (session == NULL) {
        return;
    }
    if (session->project != NULL) {
        led_project_destroy(session->project);
        session->project = NULL;
    }
}

int led_project_is_open(const led_session *session) {
    if (session == NULL) {
        return 0;
    }
    return (session->project != NULL) ? 1 : 0;
}

led_project *led_project_get(led_session *session) {
    if (session == NULL) {
        return NULL;
    }
    return session->project;
}

void led_project_get_info(const led_session *session,
                          led_project_info *out_info) {
    uint32_t i;

    if (out_info == NULL) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (session == NULL || session->project == NULL) {
        return;
    }
    {
        led_project *p = session->project;

        strncpy(out_info->name, p->name, sizeof(out_info->name) - 1);
        strncpy(out_info->root, p->root, sizeof(out_info->root) - 1);
        out_info->format_version = p->format_version;
        if (p->has_startup_scene) {
            strncpy(out_info->startup_scene, p->startup_scene,
                    sizeof(out_info->startup_scene) - 1);
            out_info->has_startup_scene = 1;
        }
        out_info->asset_count = p->record_count;
        for (i = 0; i < p->record_count; i++) {
            switch (p->records[i].status) {
            case LED_IMPORT_READY:
                out_info->ready_count++;
                break;
            case LED_IMPORT_STALE:
                out_info->stale_count++;
                break;
            case LED_IMPORT_FAILED:
                out_info->failed_count++;
                break;
            case LED_IMPORT_MISSING:
                out_info->missing_count++;
                break;
            default:
                break;
            }
        }
    }
}
