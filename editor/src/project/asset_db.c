/* Phase 32 asset database: project records over sidecar
 * metadata, indexed by UUID (hash) + normalized path (hash),
 * deterministic path-sorted enumeration. The DB describes assets;
 * the Phase 25 engine registry owns runtime resources.
 *
 * Sidecar `<source>.luma` (hand format, no new dependency):
 *   LUMA_ASSET 1
 *   id <32hex>                 (project UUID, minted at discovery)
 *   type <model|texture|script|scene|prefab>
 *   source <rel path>          (must match discovery path)
 *   fingerprint <size> <fnvhex>
 *   importer <id> <version>
 *   settings <digesthex>
 *   dep <32hex>                (repeatable, project UUIDs)
 *   sub <key> <32hex>          (repeatable, sub-asset keys)
 *   runtime <32hex>            (persistent engine ID, when imported)
 * Unknown fields tolerated; unknown versions fail the record
 * (diagnostic, scan continues); malformed sidecar marks the record
 * diagnostic (never crashes the scan).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

#define LED_SIDECAR_MAGIC "LUMA_ASSET 1"
#define LED_SIDECAR_SUFFIX ".luma"

/* ---- record lifetime ---- */

static void led_record_clear(led_db_record *r) {
    uint32_t i;

    if (r == NULL) {
        return;
    }
    free(r->deps);
    if (r->sub_keys != NULL) {
        for (i = 0; i < r->sub_count; i++) {
            free(r->sub_keys[i]);
        }
        free(r->sub_keys);
    }
    free(r->sub_ids);
    memset(r, 0, sizeof(*r));
    r->runtime_asset = LE_ASSET_INVALID;
}

void led_project_free_db(led_project *p) {
    uint32_t i;

    if (p == NULL) {
        return;
    }
    if (p->records != NULL) {
        for (i = 0; i < p->record_count; i++) {
            led_record_clear(&p->records[i]);
        }
        free(p->records);
        p->records = NULL;
    }
    free(p->by_id);
    p->by_id = NULL;
    free(p->by_path);
    p->by_path = NULL;
    p->record_count = 0;
    p->record_cap = 0;
    p->id_cap = 0;
    p->path_cap = 0;
    p->view_count = 0;
}

/* ---- open-addressing indexes (UUID + path hash) ---- */

static uint64_t led_id_hash(const led_project_asset_id *id) {
    uint64_t h = id->hi ^ (id->lo * 0x9e3779b97f4a7c15ull);

    h ^= h >> 29;
    h *= 0xbf58476d1ce4e5b9ull;
    h ^= h >> 32;
    return (h != 0) ? h : 1u;
}

static int led_db_reindex(led_project *p) {
    uint32_t want;
    uint32_t i;

    free(p->by_id);
    free(p->by_path);
    p->by_id = NULL;
    p->by_path = NULL;
    want = (p->record_count * 2u + 16u) | 1u;
    if (want < 17u) {
        want = 17u;
    }
    p->by_id = (uint32_t *)calloc(want, sizeof(uint32_t));
    p->by_path = (uint32_t *)calloc(want, sizeof(uint32_t));
    if (p->by_id == NULL || p->by_path == NULL) {
        free(p->by_id);
        free(p->by_path);
        p->by_id = NULL;
        p->by_path = NULL;
        p->id_cap = 0;
        p->path_cap = 0;
        return 0;
    }
    p->id_cap = want;
    p->path_cap = want;
    for (i = 0; i < p->record_count; i++) {
        uint32_t h =
            (uint32_t)(led_id_hash(&p->records[i].id) % want);
        uint32_t guard = 0;

        while (p->by_id[h] != 0 && guard++ < want) {
            h = (h + 1u) % want;
        }
        p->by_id[h] = i + 1u;
        {
            uint64_t ph = led_fnv1a64(p->records[i].source_path,
                                      strlen(p->records[i]
                                                 .source_path));
            uint32_t q = (uint32_t)((ph | 1u) % want);
            uint32_t g2 = 0;

            while (p->by_path[q] != 0 && g2++ < want) {
                q = (q + 1u) % want;
            }
            p->by_path[q] = i + 1u;
        }
    }
    return 1;
}

static int led_db_find_index_by_id(const led_project *p,
                                   const led_project_asset_id *id) {
    uint32_t h;
    uint32_t guard = 0;

    if (p == NULL || id == NULL || p->by_id == NULL ||
        p->id_cap == 0) {
        return -1;
    }
    h = (uint32_t)(led_id_hash(id) % p->id_cap);
    while (guard++ < p->id_cap) {
        uint32_t slot = p->by_id[h];

        if (slot == 0) {
            return -1;
        }
        if (slot - 1u < p->record_count &&
            led_project_id_equal(&p->records[slot - 1u].id, id)) {
            return (int)(slot - 1u);
        }
        h = (h + 1u) % p->id_cap;
    }
    return -1;
}

static int led_db_find_index_by_path(const led_project *p,
                                     const char *norm_path) {
    uint64_t ph;
    uint32_t q;
    uint32_t guard = 0;

    if (p == NULL || norm_path == NULL || p->by_path == NULL ||
        p->path_cap == 0) {
        return -1;
    }
    ph = led_fnv1a64(norm_path, strlen(norm_path));
    q = (uint32_t)((ph | 1u) % p->path_cap);
    while (guard++ < p->path_cap) {
        uint32_t slot = p->by_path[q];

        if (slot == 0) {
            return -1;
        }
        if (slot - 1u < p->record_count &&
            strcmp(p->records[slot - 1u].source_path, norm_path) ==
                0) {
            return (int)(slot - 1u);
        }
        q = (q + 1u) % p->path_cap;
    }
    return -1;
}

/* ---- type vocabulary ---- */

static led_project_asset_type led_type_from_extension(
    const char *rel_path) {
    const char *dot = NULL;
    const char *c;

    if (rel_path == NULL) {
        return LED_PROJECT_ASSET_UNKNOWN;
    }
    for (c = rel_path; *c != '\0'; c++) {
        if (*c == '.') {
            dot = c;
        }
        if (*c == '/') {
            dot = NULL;
        }
    }
    if (dot == NULL) {
        return LED_PROJECT_ASSET_UNKNOWN;
    }
    dot++;
    {
        char ext[16];
        size_t i = 0;

        while (dot[i] != '\0' && i + 1 < sizeof(ext)) {
            char ch = dot[i];

            if (ch >= 'A' && ch <= 'Z') {
                ch = (char)(ch - 'A' + 'a');
            }
            ext[i] = ch;
            i++;
        }
        ext[i] = '\0';
        if (strcmp(ext, "glb") == 0 || strcmp(ext, "gltf") == 0) {
            return LED_PROJECT_ASSET_MODEL;
        }
        if (strcmp(ext, "png") == 0 || strcmp(ext, "jpg") == 0 ||
            strcmp(ext, "jpeg") == 0) {
            return LED_PROJECT_ASSET_TEXTURE;
        }
        if (strcmp(ext, "lua") == 0) {
            return LED_PROJECT_ASSET_SCRIPT;
        }
        if (strcmp(ext, "luma_scene") == 0) {
            return LED_PROJECT_ASSET_SCENE;
        }
        if (strcmp(ext, "luprefab") == 0) {
            return LED_PROJECT_ASSET_PREFAB;
        }
    }
    return LED_PROJECT_ASSET_UNKNOWN;
}

static const char *led_type_name(led_project_asset_type t) {
    switch (t) {
    case LED_PROJECT_ASSET_MODEL:
        return "model";
    case LED_PROJECT_ASSET_TEXTURE:
        return "texture";
    case LED_PROJECT_ASSET_SCRIPT:
        return "script";
    case LED_PROJECT_ASSET_SCENE:
        return "scene";
    case LED_PROJECT_ASSET_PREFAB:
        return "prefab";
    case LED_PROJECT_ASSET_MESH:
        return "mesh";
    case LED_PROJECT_ASSET_MATERIAL:
        return "material";
    case LED_PROJECT_ASSET_SKELETON:
        return "skeleton";
    case LED_PROJECT_ASSET_CLIP:
        return "clip";
    default:
        return "unknown";
    }
}

static led_project_asset_type led_type_from_name(const char *name) {
    if (name == NULL) {
        return LED_PROJECT_ASSET_UNKNOWN;
    }
    if (strcmp(name, "model") == 0) {
        return LED_PROJECT_ASSET_MODEL;
    }
    if (strcmp(name, "texture") == 0) {
        return LED_PROJECT_ASSET_TEXTURE;
    }
    if (strcmp(name, "script") == 0) {
        return LED_PROJECT_ASSET_SCRIPT;
    }
    if (strcmp(name, "scene") == 0) {
        return LED_PROJECT_ASSET_SCENE;
    }
    if (strcmp(name, "prefab") == 0) {
        return LED_PROJECT_ASSET_PREFAB;
    }
    return LED_PROJECT_ASSET_UNKNOWN;
}

/* ---- sidecar read/write ---- */

static void led_sidecar_path(const led_project *p, const char *rel,
                             char out[2048]) {
    out[0] = '\0';
    if (p == NULL || rel == NULL) {
        return;
    }
    if (strlen(p->root) + 1 + strlen(rel) +
            strlen(LED_SIDECAR_SUFFIX) + 1 >
        2048) {
        return;
    }
    snprintf(out, 2048, "%s/%s%s", p->root, rel,
             LED_SIDECAR_SUFFIX);
}

static led_result led_sidecar_write(led_project *p,
                                    const led_db_record *r) {
    char path[2048];
    char idhex[33];
    char runhex[33];
    FILE *f = NULL;
    uint32_t i;

    led_sidecar_path(p, r->source_path, path);
    if (path[0] == '\0') {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    led_project_id_to_hex(&r->id, idhex);
    f = fopen(path, "w");
    if (f == NULL) {
        return LED_ERROR_IO;
    }
    fprintf(f, "%s\n", LED_SIDECAR_MAGIC);
    fprintf(f, "id %s\n", idhex);
    fprintf(f, "type %s\n", led_type_name(r->type));
    fprintf(f, "source %s\n", r->source_path);
    fprintf(f, "fingerprint %llu %016llx\n",
            (unsigned long long)r->fp_size,
            (unsigned long long)r->fp_hash);
    fprintf(f, "importer %s %u\n",
            r->importer[0] != '\0' ? r->importer : "-",
            r->importer_version);
    fprintf(f, "settings %016llx\n",
            (unsigned long long)r->settings_digest);
    for (i = 0; i < r->dep_count; i++) {
        char dhex[33];

        led_project_id_to_hex(&r->deps[i], dhex);
        fprintf(f, "dep %s\n", dhex);
    }
    for (i = 0; i < r->sub_count; i++) {
        char shex[33];

        le_asset_id_to_string(&r->sub_ids[i], shex);
        fprintf(f, "sub %s %s\n",
                r->sub_keys[i] != NULL ? r->sub_keys[i] : "-", shex);
    }
    if (r->has_runtime_id) {
        le_asset_id_to_string(&r->runtime_id, runhex);
        fprintf(f, "runtime %s\n", runhex);
    }
    if (fclose(f) != 0) {
        return LED_ERROR_IO;
    }
    return LED_SUCCESS;
}

/* Parse a sidecar into a record (record pre-zeroed by caller).
 * Returns 1 on structural success (record usable, possibly with a
 * diagnostic), 0 when the file is absent/unreadable (caller treats
 * as no-sidecar), -1 on version/ID structural failure (caller
 * keeps discovery but marks diagnostic). */
static int led_sidecar_read(led_project *p, const char *rel,
                            led_db_record *r) {
    char path[2048];
    FILE *f = NULL;
    char line[2048];
    int first = 1;
    int saw_id = 0;

    (void)p;
    led_sidecar_path(p, rel, path);
    if (path[0] == '\0') {
        return 0;
    }
    f = fopen(path, "r");
    if (f == NULL) {
        return 0;
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
            if (strcmp(line, LED_SIDECAR_MAGIC) != 0) {
                fclose(f);
                return -1;
            }
            continue;
        }
        if (strncmp(line, "id ", 3) == 0) {
            if (!led_project_id_from_hex(line + 3, &r->id)) {
                fclose(f);
                return -1;
            }
            saw_id = 1;
        } else if (strncmp(line, "type ", 5) == 0) {
            r->type = led_type_from_name(line + 5);
        } else if (strncmp(line, "source ", 7) == 0) {
            /* Must match discovery path (else stale copy). */
            if (strcmp(line + 7, rel) != 0) {
                snprintf(r->diagnostic, sizeof(r->diagnostic),
                         "sidecar source mismatch");
            }
        } else if (strncmp(line, "fingerprint ", 12) == 0) {
            unsigned long long sz = 0;
            unsigned long long hh = 0;

            if (sscanf(line + 12, "%llu %llx", &sz, &hh) == 2) {
                r->fp_size = (uint64_t)sz;
                r->fp_hash = (uint64_t)hh;
            }
        } else if (strncmp(line, "importer ", 9) == 0) {
            char iid[32];
            unsigned ver = 0;

            memset(iid, 0, sizeof(iid));
            if (sscanf(line + 9, "%31s %u", iid, &ver) >= 1) {
                if (strcmp(iid, "-") != 0) {
                    strncpy(r->importer, iid,
                            sizeof(r->importer) - 1);
                    r->importer_version = ver;
                }
            }
        } else if (strncmp(line, "settings ", 9) == 0) {
            unsigned long long dg = 0;

            if (sscanf(line + 9, "%llx", &dg) == 1) {
                r->settings_digest = (uint64_t)dg;
            }
        } else if (strncmp(line, "dep ", 4) == 0) {
            led_project_asset_id dep;

            if (led_project_id_from_hex(line + 4, &dep)) {
                if (r->dep_count >= r->dep_cap) {
                    uint32_t grown =
                        (r->dep_cap == 0) ? 4u : r->dep_cap * 2u;
                    led_project_asset_id *fresh =
                        (led_project_asset_id *)realloc(
                            r->deps,
                            grown * sizeof(*fresh));

                    if (fresh == NULL) {
                        fclose(f);
                        return -1;
                    }
                    r->deps = fresh;
                    r->dep_cap = grown;
                }
                r->deps[r->dep_count++] = dep;
            }
        } else if (strncmp(line, "sub ", 4) == 0) {
            char key[256];
            char hex[64];

            memset(key, 0, sizeof(key));
            memset(hex, 0, sizeof(hex));
            if (sscanf(line + 4, "%255s %63s", key, hex) == 2) {
                le_asset_id sid;

                memset(&sid, 0, sizeof(sid));
                if (le_asset_id_from_string(hex, &sid)) {
                    if (r->sub_count >= r->sub_cap) {
                        uint32_t grown =
                            (r->sub_cap == 0) ? 4u
                                              : r->sub_cap * 2u;
                        char **fk;
                        le_asset_id *fi;

                        fk = (char **)realloc(
                            r->sub_keys, grown * sizeof(*fk));
                        fi = (le_asset_id *)realloc(
                            r->sub_ids, grown * sizeof(*fi));
                        if (fk == NULL || fi == NULL) {
                            free(fk == NULL ? NULL : fk);
                            fclose(f);
                            return -1;
                        }
                        r->sub_keys = fk;
                        r->sub_ids = fi;
                        r->sub_cap = grown;
                    }
                    {
                        size_t kl = strlen(key);
                        char *kc = (char *)malloc(kl + 1u);

                        if (kc == NULL) {
                            fclose(f);
                            return -1;
                        }
                        memcpy(kc, key, kl + 1u);
                        r->sub_keys[r->sub_count] = kc;
                        r->sub_ids[r->sub_count] = sid;
                        r->sub_count++;
                    }
                }
            }
        } else if (strncmp(line, "runtime ", 8) == 0) {
            le_asset_id rid;

            memset(&rid, 0, sizeof(rid));
            if (le_asset_id_from_string(line + 8, &rid) &&
                !le_asset_id_is_nil(&rid)) {
                r->runtime_id = rid;
                r->has_runtime_id = 1;
            }
        }
        /* Unknown fields tolerated (forward compat). */
    }
    fclose(f);
    if (first) {
        return -1; /* empty file */
    }
    if (!saw_id) {
        return -1;
    }
    r->has_sidecar = 1;
    return 1;
}

/* ---- source fingerprint (size + FNV-1a-64 over bytes) ---- */

static int led_fingerprint_file(const char *abs_path, uint64_t *out_size,
                                uint64_t *out_hash) {
    FILE *f = NULL;
    uint64_t h = 14695981039346656037ull;
    uint64_t size = 0;
    unsigned char buf[4096];
    size_t n = 0;

    if (out_size != NULL) {
        *out_size = 0;
    }
    if (out_hash != NULL) {
        *out_hash = h;
    }
    if (abs_path == NULL) {
        return 0;
    }
    f = fopen(abs_path, "rb");
    if (f == NULL) {
        return 0;
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
    if (out_size != NULL) {
        *out_size = size;
    }
    if (out_hash != NULL) {
        *out_hash = h;
    }
    return 1;
}

/* ---- per-file reconciliation (scan core) ----
 * Discovery: classify by extension; unknown -> record with
 * UNSUPPORTED status (ignored, never a failure). Known: read
 * sidecar (mint UUID when absent), fingerprint the source, compare
 * against sidecar (size+hash, settings, importer version) to mark
 * READY-kept / STALE / UNIMPORTED / MISSING / restored. */

led_project_asset_type led_scan_classify(const char *rel) {
    const char *dot = NULL;
    const char *c;

    if (rel == NULL) {
        return LED_PROJECT_ASSET_UNKNOWN;
    }
    for (c = rel; *c != '\0'; c++) {
        if (*c == '.') {
            dot = c;
        }
        if (*c == '/') {
            dot = NULL;
        }
    }
    if (dot == NULL) {
        return LED_PROJECT_ASSET_UNKNOWN;
    }
    dot++;
    {
        char ext[16];
        size_t i = 0;

        while (dot[i] != '\0' && i + 1 < sizeof(ext)) {
            char ch = dot[i];

            if (ch >= 'A' && ch <= 'Z') {
                ch = (char)(ch - 'A' + 'a');
            }
            ext[i] = ch;
            i++;
        }
        ext[i] = '\0';
        if (strcmp(ext, "glb") == 0 || strcmp(ext, "gltf") == 0) {
            return LED_PROJECT_ASSET_MODEL;
        }
        if (strcmp(ext, "png") == 0 || strcmp(ext, "jpg") == 0 ||
            strcmp(ext, "jpeg") == 0) {
            return LED_PROJECT_ASSET_TEXTURE;
        }
        if (strcmp(ext, "lua") == 0) {
            return LED_PROJECT_ASSET_SCRIPT;
        }
        if (strcmp(ext, "luma_scene") == 0) {
            return LED_PROJECT_ASSET_SCENE;
        }
        if (strcmp(ext, "luprefab") == 0) {
            return LED_PROJECT_ASSET_PREFAB;
        }
    }
    return LED_PROJECT_ASSET_UNKNOWN;
}

/* Forward: importer identity for staleness (import_impl.c).
 * (Canonical declaration lives in internal/editor_internal.h.) */

int led_scan_reconcile(led_project *p, const char *rel,
                       const char *abs, led_scan_stats *st) {
    led_project_asset_type type = led_scan_classify(rel);
    uint32_t idx = 0;
    uint32_t i;
    int is_new = 1;

    for (i = 0; i < p->record_count; i++) {
        if (strcmp(p->records[i].source_path, rel) == 0) {
            idx = i;
            is_new = 0;
            break;
        }
    }
    if (type == LED_PROJECT_ASSET_UNKNOWN) {
        /* Unknown files: ignored (no record, never a failure). */
        return 1;
    }
    if (is_new) {
        if (p->record_count >= LED_PROJECT_MAX_RECORDS) {
            return 0;
        }
        if (p->record_count >= p->record_cap) {
            uint32_t grown = (p->record_cap == 0)
                                 ? 256u
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
            idx = p->record_count;
            p->record_count++;
        }
        st->added++;
    }
    {
        led_db_record *r = &p->records[idx];
        uint64_t fsize = 0;
        uint64_t fhash = 0;
        int have_file = led_fingerprint_file(abs, &fsize, &fhash);
        int had_sidecar = r->has_sidecar;

        /* Tag visited (high bit; cleared by the sweep). */
        r->status = (led_import_status)((uint32_t)r->status |
                                       0x80000000u);
        if (!have_file) {
            /* Vanished between listing and hashing -> MISSING. */
            r->status =
                (led_import_status)(((uint32_t)r->status &
                                    ~0x80000000u));
            if (r->status != LED_IMPORT_MISSING) {
                r->status = LED_IMPORT_MISSING;
                snprintf(r->diagnostic, sizeof(r->diagnostic),
                         "source missing");
                st->missing_marked++;
            }
            return 1;
        }
        /* Read (or re-read) the sidecar for stored identity
         * (static local above — same TU, no cross-TU shim). */
        {
            led_db_record side;

            memset(&side, 0, sizeof(side));
            {
                int src = led_sidecar_read(p, rel, &side);
                if (src == 1) {
                    /* Adopt stored identity + metadata. */
                    free(r->deps);
                    {
                        uint32_t k;

                        if (r->sub_keys != NULL) {
                            for (k = 0; k < r->sub_count; k++) {
                                free(r->sub_keys[k]);
                            }
                            free(r->sub_keys);
                        }
                        free(r->sub_ids);
                    }
                    r->id = side.id;
                    strncpy(r->importer, side.importer,
                            sizeof(r->importer) - 1);
                    r->importer_version = side.importer_version;
                    r->settings_digest = side.settings_digest;
                    r->deps = side.deps;
                    r->dep_count = side.dep_count;
                    r->dep_cap = side.dep_cap;
                    r->sub_keys = side.sub_keys;
                    r->sub_ids = side.sub_ids;
                    r->sub_count = side.sub_count;
                    r->sub_cap = side.sub_cap;
                    r->runtime_id = side.runtime_id;
                    r->has_runtime_id = side.has_runtime_id;
                    r->fp_size = side.fp_size;
                    r->fp_hash = side.fp_hash;
                    r->has_sidecar = 1;
                    memset(&side, 0, sizeof(side));
                } else if (src == -1) {
                    snprintf(r->diagnostic,
                             sizeof(r->diagnostic),
                             "sidecar corrupt");
                    st->errors++;
                }
            }
        }
        if (!r->has_sidecar && !had_sidecar) {
            /* Fresh discovery: mint identity + write sidecar. */
            led_project_id_mint(&r->id);
            strncpy(r->importer,
                    led_importer_id_for(r->type),
                    sizeof(r->importer) - 1);
            r->importer_version =
                led_importer_version_for(r->type);
            r->settings_digest =
                led_import_settings_digest(r->type);
            r->fp_size = fsize;
            r->fp_hash = fhash;
            r->has_sidecar = 1;
            led_sidecar_write(p, r);
            {
                led_import_status cur =
                    (led_import_status)(((uint32_t)r->status &
                                        ~0x80000000u));

                if (cur != LED_IMPORT_READY &&
                    cur != LED_IMPORT_FAILED) {
                    cur = LED_IMPORT_UNIMPORTED;
                }
                /* Keep the visited tag (see below). */
                r->status = (led_import_status)((uint32_t)cur |
                                               0x80000000u);
            }
            return 1;
        }
        /* Compare: fingerprint, importer version, settings. */
        {
            led_import_status cur =
                (led_import_status)((uint32_t)r->status &
                                   ~0x80000000u);
            int fp_same = (r->fp_size == fsize &&
                           r->fp_hash == fhash);
            int imp_same =
                (strcmp(r->importer,
                        led_importer_id_for(r->type)) == 0 &&
                 r->importer_version ==
                     led_importer_version_for(r->type));
            int set_same = (r->settings_digest ==
                            led_import_settings_digest(r->type));

            if (cur == LED_IMPORT_MISSING) {
                /* Restored file: identity recovers via sidecar. */
                st->restored++;
                cur = (!fp_same || !imp_same || !set_same)
                          ? LED_IMPORT_STALE
                          : LED_IMPORT_UNIMPORTED;
                /* A restored UNIMPORTED record with no runtime
                 * asset imports on demand; a restored record with
                 * a live runtime handle revalidates below. */
            }
            if (cur == LED_IMPORT_READY ||
                cur == LED_IMPORT_UNIMPORTED) {
                if (!fp_same || !imp_same || !set_same) {
                    cur = LED_IMPORT_STALE;
                    st->stale_marked++;
                    snprintf(r->diagnostic,
                             sizeof(r->diagnostic),
                             !fp_same ? "source changed"
                                      : (!imp_same ? "importer changed"
                                                  : "settings changed"));
                }
            }
            /* Live runtime handle revalidation: runtime assets
             * survive rescans (lazy policy); a READY record whose
             * handle died externally drops to UNIMPORTED. */
            if (cur == LED_IMPORT_READY && r->has_runtime_asset &&
                p->session != NULL &&
                p->session->engine != NULL &&
                !le_asset_is_alive(p->session->engine,
                                   &r->runtime_asset)) {
                r->has_runtime_asset = 0;
                r->runtime_asset = LE_ASSET_INVALID;
                cur = LED_IMPORT_UNIMPORTED;
            }
            r->fp_size = fsize;
            r->fp_hash = fhash;
            /* Re-tag visited (the compare above masks the sweep
             * bit out of `cur`; the sweep below must see it). */
            r->status = (led_import_status)((uint32_t)cur |
                                           0x80000000u);
        }
        return 1;
    }
}

/* Rebuild wrapper (project_scan.c calls this after sorting). */
int led_db_rebuild(led_project *p) {
    return led_db_reindex(p);
}

/* ---- DB record helpers ---- */

static void led_record_to_snapshot(const led_db_record *r,
                                   led_asset_record *out) {
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
    /* Copy stable sub-asset key strings (bounded: 64; the count
     * above stays exact even when truncated). */
    {
        uint32_t k;
        uint32_t ncopy =
            (r->sub_count < 64u) ? r->sub_count : 64u;

        out->sub_key_count = ncopy;
        for (k = 0; k < ncopy; k++) {
            if (r->sub_keys != NULL &&
                r->sub_keys[k] != NULL) {
                strncpy(out->sub_keys[k], r->sub_keys[k],
                        sizeof(out->sub_keys[k]) - 1);
                out->sub_keys[k][sizeof(out->sub_keys[k]) - 1] =
                    '\0';
            } else {
                out->sub_keys[k][0] = '\0';
            }
        }
    }
    out->runtime_asset = r->runtime_asset;
    out->has_runtime_asset = r->has_runtime_asset;
    out->runtime_id = r->runtime_id;
    out->has_runtime_id = r->has_runtime_id;
    strncpy(out->diagnostic, r->diagnostic,
            sizeof(out->diagnostic) - 1);
}

/* ---- public DB enumeration surface ---- */

uint32_t led_assetdb_count(const led_session *session) {
    if (session == NULL || session->project == NULL) {
        return 0;
    }
    return session->project->record_count;
}

int led_assetdb_get(const led_session *session, uint32_t index,
                    led_asset_record *out_record) {
    /* Deterministic path-sorted order: records are kept sorted by
     * source_path at scan/insert time (see led_db_sort). */
    if (out_record != NULL) {
        memset(out_record, 0, sizeof(*out_record));
    }
    if (session == NULL || session->project == NULL ||
        out_record == NULL) {
        return 0;
    }
    if (index >= session->project->record_count) {
        return 0;
    }
    led_record_to_snapshot(&session->project->records[index],
                           out_record);
    return 1;
}

int led_assetdb_find_by_id(const led_session *session,
                           const led_project_asset_id *id,
                           led_asset_record *out_record) {
    int idx;

    if (out_record != NULL) {
        memset(out_record, 0, sizeof(*out_record));
    }
    if (session == NULL || session->project == NULL || id == NULL ||
        out_record == NULL) {
        return 0;
    }
    idx = led_db_find_index_by_id(session->project, id);
    if (idx < 0) {
        return 0;
    }
    led_record_to_snapshot(&session->project->records[idx],
                           out_record);
    return 1;
}

int led_assetdb_find_by_path(const led_session *session,
                             const char *rel_path,
                             led_asset_record *out_record) {
    char norm[1024];
    int idx;

    if (out_record != NULL) {
        memset(out_record, 0, sizeof(*out_record));
    }
    if (session == NULL || session->project == NULL ||
        rel_path == NULL || out_record == NULL) {
        return 0;
    }
    if (!led_project_normalize(rel_path, norm)) {
        return 0;
    }
    idx = led_db_find_index_by_path(session->project, norm);
    if (idx < 0) {
        return 0;
    }
    led_record_to_snapshot(&session->project->records[idx],
                           out_record);
    return 1;
}

uint32_t led_assetdb_filter(const led_session *session,
                            led_project_asset_type type) {
    uint32_t n = 0;
    uint32_t i;

    if (session == NULL || session->project == NULL) {
        return 0;
    }
    if (type == LED_PROJECT_ASSET_TYPE_COUNT) {
        return session->project->record_count;
    }
    for (i = 0; i < session->project->record_count; i++) {
        if (session->project->records[i].type == type) {
            n++;
        }
    }
    return n;
}

static int led_match_query(const char *haystack, const char *query) {
    /* Case-insensitive substring (both sides folded inline). */
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

uint32_t led_assetdb_search(const led_session *session,
                            const char *query,
                            led_project_asset_type type,
                            led_project_asset_id *out_ids,
                            uint32_t capacity, uint32_t *out_count) {
    uint32_t n = 0;
    uint32_t i;

    if (out_count != NULL) {
        *out_count = 0;
    }
    if (session == NULL || session->project == NULL) {
        return 0;
    }
    for (i = 0; i < session->project->record_count; i++) {
        const led_db_record *r = &session->project->records[i];

        if (type != LED_PROJECT_ASSET_TYPE_COUNT &&
            r->type != type) {
            continue;
        }
        {
            const char *base = strrchr(r->source_path, '/');

            base = (base != NULL) ? base + 1 : r->source_path;
            if (!led_match_query(base, query) &&
                !led_match_query(r->source_path, query) &&
                !led_match_query(led_type_name(r->type), query)) {
                continue;
            }
        }
        if (out_ids != NULL && n < capacity) {
            out_ids[n] = r->id;
        }
        n++;
    }
    if (out_count != NULL) {
        *out_count = n;
    }
    return n;
}

uint32_t led_assetdb_dependencies(
    const led_session *session, const led_project_asset_id *id,
    led_project_asset_id *out_ids, uint32_t capacity) {
    int idx;

    if (session == NULL || session->project == NULL || id == NULL) {
        return 0;
    }
    idx = led_db_find_index_by_id(session->project, id);
    if (idx < 0) {
        return 0;
    }
    {
        const led_db_record *r = &session->project->records[idx];
        uint32_t i;

        for (i = 0; i < r->dep_count && out_ids != NULL &&
                    i < capacity;
             i++) {
            out_ids[i] = r->deps[i];
        }
        return r->dep_count;
    }
}

/* ---- cross-TU entry points (project_files.c / prefab_core.c /
 *  project_scan.c, same lib) ----
 *
 * Public-visibility aliases for the static sidecar helpers (the
 * static names stay file-local; these wrappers carry the same
 * semantics for sibling TUs). */
void led_sidecar_write_pub(led_project *p, const led_db_record *r) {
    (void)led_sidecar_write(p, r);
}

int led_sidecar_read_pub(led_project *p, const char *rel,
                         led_db_record *r) {
    memset(r, 0, sizeof(*r));
    return led_sidecar_read(p, rel, r);
}

int led_record_cmp_pub(const void *a, const void *b) {
    const led_db_record *ra = (const led_db_record *)a;
    const led_db_record *rb = (const led_db_record *)b;

    return strcmp(ra->source_path, rb->source_path);
}

uint32_t led_assetdb_dependents(
    const led_session *session, const led_project_asset_id *id,
    led_project_asset_id *out_ids, uint32_t capacity) {
    uint32_t n = 0;
    uint32_t i;
    uint32_t k;

    if (session == NULL || session->project == NULL || id == NULL) {
        return 0;
    }
    for (i = 0; i < session->project->record_count; i++) {
        const led_db_record *r = &session->project->records[i];

        for (k = 0; k < r->dep_count; k++) {
            if (led_project_id_equal(&r->deps[k], id)) {
                if (out_ids != NULL && n < capacity) {
                    out_ids[n] = r->id;
                }
                n++;
                break;
            }
        }
    }
    return n;
}

void led_assetdb_get_stats(const led_session *session,
                           led_assetdb_stats *out_stats) {
    uint32_t i;

    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (session == NULL || session->project == NULL) {
        return;
    }
    {
        led_project *p = session->project;

        out_stats->records = p->record_count;
        for (i = 0; i < p->record_count; i++) {
            const led_db_record *r = &p->records[i];

            if ((uint32_t)r->type <
                LED_PROJECT_ASSET_TYPE_COUNT) {
                out_stats->by_type[r->type]++;
            }
            if ((uint32_t)r->status < 6u) {
                out_stats->by_status[r->status]++;
            }
            out_stats->bytes_estimate +=
                sizeof(*r) + (uint64_t)r->dep_count *
                                 sizeof(r->deps[0]) +
                (uint64_t)r->sub_count *
                    (sizeof(r->sub_ids[0]) + 64u);
        }
    }
}
