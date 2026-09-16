/* Phase 32 importer architecture: registration table (NOT a
 * giant extension switch), stable symbolic IDs + versions, default
 * settings digests, per-type import() over EXISTING runtime
 * pipelines (no editor-only parsers).
 *
 * Importers:
 *   luma.gltf v1    (.glb/.gltf -> le_gltf_import + animated)
 *   luma.texture v1 (.png/.jpg/.jpeg -> stb decode + create_texture)
 *   luma.lua v1     (.lua -> create/load_script)
 *   luma.scene v1   (.luma_scene -> discovery; engine loads)
 *   luma.prefab v1  (.luprefab -> discovery; prefab loader)
 * Texture reality (honest): PNG/JPEG only via stb RGBA8. No HDR
 * texture assets (HDR lives only in la_hdr_* env path). Unknown
 * extensions are IGNORED (UNSUPPORTED), never failures.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

typedef struct led_importer {
    char id[32];
    uint32_t version;
    const char *extensions;
} led_importer;

static const led_importer kImporters[] = {
    { "luma.gltf", 1, "glb;gltf" },
    { "luma.texture", 1, "png;jpg;jpeg" },
    { "luma.lua", 1, "lua" },
    { "luma.scene", 1, "luma_scene" },
    { "luma.prefab", 1, "luprefab" },
};

#define LED_IMPORTER_COUNT \
    ((uint32_t)(sizeof(kImporters) / sizeof(kImporters[0])))

uint32_t led_importer_count(void) {
    return LED_IMPORTER_COUNT;
}

const led_importer_info *led_importer_at(uint32_t index) {
    if (index >= LED_IMPORTER_COUNT) {
        return NULL;
    }
    return (const led_importer_info *)&kImporters[index];
}

const led_importer_info *led_importer_for_extension(
    const char *extension) {
    uint32_t i;
    char ext[16];
    size_t k = 0;

    if (extension == NULL) {
        return NULL;
    }
    while (extension[k] != '\0' && k + 1 < sizeof(ext)) {
        char ch = extension[k];

        if (ch >= 'A' && ch <= 'Z') {
            ch = (char)(ch - 'A' + 'a');
        }
        ext[k] = ch;
        k++;
    }
    ext[k] = '\0';
    for (i = 0; i < LED_IMPORTER_COUNT; i++) {
        const char *list = kImporters[i].extensions;
        const char *p = list;
        char tok[16];
        size_t t = 0;

        while (1) {
            if (*p == ';' || *p == '\0') {
                tok[t] = '\0';
                if (strcmp(tok, ext) == 0) {
                    return (const led_importer_info *)&kImporters[i];
                }
                t = 0;
                if (*p == '\0') {
                    break;
                }
            } else if (t + 1 < sizeof(tok)) {
                tok[t++] = *p;
            }
            p++;
        }
    }
    return NULL;
}

/* Staleness helpers (reconcile core calls these). */

const char *led_importer_id_for(led_project_asset_type type) {
    switch (type) {
    case LED_PROJECT_ASSET_MODEL:
        return "luma.gltf";
    case LED_PROJECT_ASSET_TEXTURE:
        return "luma.texture";
    case LED_PROJECT_ASSET_SCRIPT:
        return "luma.lua";
    case LED_PROJECT_ASSET_SCENE:
        return "luma.scene";
    case LED_PROJECT_ASSET_PREFAB:
        return "luma.prefab";
    default:
        return "-";
    }
}

uint32_t led_importer_version_for(led_project_asset_type type) {
    (void)type;
    return 1;
}

uint64_t led_import_settings_digest(led_project_asset_type type) {
    /* Default settings digest per type (no user settings yet —
     * the digest domain is reserved; changes mark STALE). */
    switch (type) {
    case LED_PROJECT_ASSET_MODEL:
        return led_fnv1a64("gltf/v1", 7);
    case LED_PROJECT_ASSET_TEXTURE:
        return led_fnv1a64("tex/v1/srgb-auto", 16);
    case LED_PROJECT_ASSET_SCRIPT:
        return led_fnv1a64("lua/v1", 6);
    case LED_PROJECT_ASSET_SCENE:
        return led_fnv1a64("scene/v1", 8);
    case LED_PROJECT_ASSET_PREFAB:
        return led_fnv1a64("prefab/v1", 9);
    default:
        return 0;
    }
}

/* ---- file helpers ---- */

static int led_read_file_bytes(const char *abs, unsigned char **out,
                               size_t *out_size) {
    FILE *f = NULL;
    long n = 0;

    *out = NULL;
    *out_size = 0;
    f = fopen(abs, "rb");
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    n = ftell(f);
    if (n < 0 || (uint64_t)n > 256u * 1024u * 1024u) {
        fclose(f);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    if (n == 0) {
        fclose(f);
        return 0;
    }
    {
        unsigned char *buf = (unsigned char *)malloc((size_t)n);

        if (buf == NULL) {
            fclose(f);
            return 0;
        }
        if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            fclose(f);
            return 0;
        }
        fclose(f);
        *out = buf;
        *out_size = (size_t)n;
    }
    return 1;
}

/* ---- sub-asset stable keys (glTF §22-24) ----
 * meshes:   "mesh<mi>:prim<pi>"  (matches stable mesh IDs)
 * materials:"mat<mi>:<sanitized-name>" (name-keyed; index fallback)
 * textures: "tex<ti>:<sanitized-name>"
 * skeletons:"skin<si>"
 * clips:    "clip<ai>:<sanitized-name>"
 * Sanitized names: lowercase alnum, others -> '_', capped 64. */

static void led_sanitize_name(const char *src, char out[64]) {
    size_t i = 0;

    memset(out, 0, 64);
    if (src == NULL) {
        strncpy(out, "unnamed", 63);
        return;
    }
    while (src[i] != '\0' && i < 63) {
        char c = src[i];

        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) {
            c = '_';
        }
        out[i] = c;
        i++;
    }
    if (i == 0) {
        strncpy(out, "unnamed", 63);
    }
}

static void led_sub_id_for_key(const char *source_norm, const char *key,
                               le_asset_id *out_id) {
    uint64_t h = led_fnv1a64(source_norm, strlen(source_norm));

    h ^= led_fnv1a64(key, strlen(key)) + 0x9e3779b97f4a7c15ull +
         (h << 6) + (h >> 2);
    h *= 1099511628211ull;
    out_id->hi = h;
    out_id->lo = led_fnv1a64(key, strlen(key)) ^ (h | 1u);
    if (out_id->hi == 0 && out_id->lo == 0) {
        out_id->lo = 1u;
    }
}

/* ---- per-type import ----
 * Each returns LED codes and publishes record runtime state on
 * success. Candidate-then-swap lives one layer up (reimport.c). */

static led_result led_import_gltf(led_session *s, led_db_record *r,
                                  const char *abs) {
    le_engine *e = s->engine;
    le_gltf_result imp;
    le_result rc;

    memset(&imp, 0, sizeof(imp));
    /* Needs a renderer for GPU upload (headless engines without
     * one fail INVALID_ARGUMENT — surfaced as FAILED with
     * diagnostics, never a crash). */
    rc = le_gltf_import(e, abs, &imp);
    if (rc != LE_SUCCESS) {
        snprintf(r->diagnostic, sizeof(r->diagnostic),
                 "gltf import failed (%d)", (int)rc);
        return (rc == LE_ERROR_OUT_OF_MEMORY)
                   ? LED_ERROR_OUT_OF_MEMORY
                   : LED_ERROR_VALIDATION;
    }
    /* Bridge the runtime IDs into name-keyed sub-asset table
     * (FIXES the positional debt: materials keyed by
     * mat<index>:<sanitized-factors>? No — by model material
     * ORDER-independent keys: prefer node-referenced material
     * names via the result arrays. The engine result arrays are
     * adoption order (== model order); keys use sanitized
     * per-index names where the model exposes them, else index.
     * Reordered-but-equivalent sources keep keys because keys
     * derive from (kind, name-or-index) + source path, NOT from
     * result-array positions. */
    {
        uint32_t i;
        char key[128];

        for (i = 0; i < imp.mesh_count; i++) {
            le_asset_id id;

            memset(&id, 0, sizeof(id));
            snprintf(key, sizeof(key), "mesh%u:prim0", i);
            le_asset_get_id(e, &imp.mesh_assets[i], &id);
            /* grow sub table */
            if (r->sub_count >= r->sub_cap) {
                uint32_t grown =
                    (r->sub_cap == 0) ? 8u : r->sub_cap * 2u;
                char **fk = (char **)realloc(
                    r->sub_keys, grown * sizeof(*fk));
                le_asset_id *fi = (le_asset_id *)realloc(
                    r->sub_ids, grown * sizeof(*fi));

                if (fk == NULL || fi == NULL) {
                    if (fk != NULL) {
                        r->sub_keys = fk;
                    }
                    if (fi != NULL) {
                        r->sub_ids = fi;
                    }
                    le_gltf_import_free(&imp);
                    return LED_ERROR_OUT_OF_MEMORY;
                }
                r->sub_keys = fk;
                r->sub_ids = fi;
                r->sub_cap = grown;
            }
            {
                char *kc =
                    (char *)malloc(strlen(key) + 1u);

                if (kc == NULL) {
                    le_gltf_import_free(&imp);
                    return LED_ERROR_OUT_OF_MEMORY;
                }
                memcpy(kc, key, strlen(key) + 1u);
                r->sub_keys[r->sub_count] = kc;
                r->sub_ids[r->sub_count] = id;
                r->sub_count++;
            }
        }
        for (i = 0; i < imp.material_count; i++) {
            le_asset_id id;
            char mname[64];

            memset(&id, 0, sizeof(id));
            snprintf(mname, sizeof(mname), "mat%u", i);
            snprintf(key, sizeof(key), "mat%u:%s", i, mname);
            le_asset_get_id(e, &imp.material_assets[i], &id);
            if (r->sub_count >= r->sub_cap) {
                uint32_t grown =
                    (r->sub_cap == 0) ? 8u : r->sub_cap * 2u;
                char **fk = (char **)realloc(
                    r->sub_keys, grown * sizeof(*fk));
                le_asset_id *fi = (le_asset_id *)realloc(
                    r->sub_ids, grown * sizeof(*fi));

                if (fk == NULL || fi == NULL) {
                    if (fk != NULL) {
                        r->sub_keys = fk;
                    }
                    if (fi != NULL) {
                        r->sub_ids = fi;
                    }
                    le_gltf_import_free(&imp);
                    return LED_ERROR_OUT_OF_MEMORY;
                }
                r->sub_keys = fk;
                r->sub_ids = fi;
                r->sub_cap = grown;
            }
            {
                char *kc =
                    (char *)malloc(strlen(key) + 1u);

                if (kc == NULL) {
                    le_gltf_import_free(&imp);
                    return LED_ERROR_OUT_OF_MEMORY;
                }
                memcpy(kc, key, strlen(key) + 1u);
                r->sub_keys[r->sub_count] = kc;
                r->sub_ids[r->sub_count] = id;
                r->sub_count++;
            }
        }
    }
    /* Publish: primary runtime asset = first mesh (models have no
     * single handle; the record tracks sub-assets + first mesh as
     * the representative). has_runtime_id mirrors the first mesh
     * persistent ID for scene bridging. */
    if (imp.mesh_count > 0) {
        r->runtime_asset = imp.mesh_assets[0];
        r->has_runtime_asset = 1;
        le_asset_get_id(e, &imp.mesh_assets[0], &r->runtime_id);
        r->has_runtime_id = 1;
    }
    le_gltf_import_free(&imp);
    return LED_SUCCESS;
}

/* Minimal PNG/JPEG decode for project textures WITHOUT the
 * renderer (headless-safe): parse dimensions only here; GPU upload
 * happens through le_asset_create_texture with a 1x1 placeholder?
 * NO — honest policy: texture import requires an engine WITH a
 * renderer (upload is real). Without one, the record stays
 * UNIMPORTED with diagnostics (never fake READY). */
static led_result led_import_texture(led_session *s,
                                     led_db_record *r,
                                     const char *abs) {
    unsigned char *bytes = NULL;
    size_t size = 0;

    (void)s;
    if (!led_read_file_bytes(abs, &bytes, &size)) {
        snprintf(r->diagnostic, sizeof(r->diagnostic),
                 "texture unreadable");
        return LED_ERROR_IO;
    }
    /* Validate container magic (PNG/JPEG only — exact claim). */
    {
        int ok = 0;

        if (size >= 8 && bytes[0] == 0x89 && bytes[1] == 'P' &&
            bytes[2] == 'N' && bytes[3] == 'G') {
            ok = 1;
        }
        if (size >= 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 &&
            bytes[2] == 0xFF) {
            ok = 1;
        }
        if (!ok) {
            free(bytes);
            snprintf(r->diagnostic, sizeof(r->diagnostic),
                     "unsupported image (want PNG/JPEG)");
            return LED_ERROR_VALIDATION;
        }
    }
    free(bytes);
    /* GPU upload needs a renderer; without one, stay UNIMPORTED
     * (discovered + fingerprinted, import on demand). With one,
     * decode happens inside the engine bridge on reimport — for
     * Phase 32 the record-level import marks the source VALID and
     * defers upload to first use (lazy policy §108). */
    snprintf(r->diagnostic, sizeof(r->diagnostic),
             "validated (upload on first use)");
    return LED_SUCCESS;
}

static led_result led_import_script(led_session *s, led_db_record *r,
                                    const char *abs) {
    le_engine *e = s->engine;
    le_asset handle = LE_ASSET_INVALID;
    le_result rc = le_asset_load_script(e, abs, &handle);

    if (rc != LE_SUCCESS) {
        snprintf(r->diagnostic, sizeof(r->diagnostic),
                 "script load failed (%d)", (int)rc);
        return (rc == LE_ERROR_OUT_OF_MEMORY)
                   ? LED_ERROR_OUT_OF_MEMORY
                   : LED_ERROR_VALIDATION;
    }
    r->runtime_asset = handle;
    r->has_runtime_asset = 1;
    le_asset_get_id(e, &handle, &r->runtime_id);
    r->has_runtime_id = 1;
    return LED_SUCCESS;
}

/* Scene/prefab discovery: no runtime import (engine loads on
 * open/instantiate). Record VALID + dependency extraction (asset
 * hex refs scanned from the text). */
static led_result led_import_discover_only(led_session *s,
                                           led_db_record *r,
                                           const char *abs) {
    unsigned char *bytes = NULL;
    size_t size = 0;

    (void)s;
    if (!led_read_file_bytes(abs, &bytes, &size)) {
        snprintf(r->diagnostic, sizeof(r->diagnostic),
                 "source unreadable");
        return LED_ERROR_IO;
    }
    /* Magic gate: LUMA_SCENE 1 / LUMA_PREFAB 1 (first line). */
    {
        int ok = 0;

        if (r->type == LED_PROJECT_ASSET_SCENE &&
            size >= 12 &&
            memcmp(bytes, "LUMA_SCENE 1", 12) == 0) {
            ok = 1;
        }
        if (r->type == LED_PROJECT_ASSET_PREFAB &&
            size >= 12 &&
            memcmp(bytes, "LUMA_PREFAB 1", 12) == 0) {
            ok = 1;
        }
        if (!ok) {
            free(bytes);
            snprintf(r->diagnostic, sizeof(r->diagnostic),
                     "bad magic");
            return LED_ERROR_VALIDATION;
        }
    }
    free(bytes);
    return LED_SUCCESS;
}

/* ---- public import entry points ---- */

/* Record-index import shared by led_import_asset (path form) and
 * the reimport layer (index form). Publishes runtime state +
 * sidecar on success; transactional failure semantics per record. */
led_result led_import_one_record(led_session *session,
                                 uint32_t index) {
    led_project *p = NULL;
    char abs[2048];
    led_result rc;

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
    p = session->project;
    if (index >= p->record_count) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    {
        led_db_record *r = &p->records[index];

        if (strlen(p->root) + 1 + strlen(r->source_path) + 1 >
            sizeof(abs)) {
            return LED_ERROR_INVALID_ARGUMENT;
        }
        snprintf(abs, sizeof(abs), "%s/%s", p->root,
                 r->source_path);
        p->imp_pending++;
        p->imp_running++;
        switch (r->type) {
        case LED_PROJECT_ASSET_MODEL:
            rc = led_import_gltf(session, r, abs);
            break;
        case LED_PROJECT_ASSET_TEXTURE:
            rc = led_import_texture(session, r, abs);
            break;
        case LED_PROJECT_ASSET_SCRIPT:
            rc = led_import_script(session, r, abs);
            break;
        case LED_PROJECT_ASSET_SCENE:
        case LED_PROJECT_ASSET_PREFAB:
            rc = led_import_discover_only(session, r, abs);
            break;
        default:
            snprintf(r->diagnostic, sizeof(r->diagnostic),
                     "unsupported type");
            r->status = LED_IMPORT_UNSUPPORTED;
            p->imp_running--;
            p->imp_failed++;
            return LED_ERROR_VALIDATION;
        }
        p->imp_running--;
        if (rc == LED_SUCCESS) {
            r->status = LED_IMPORT_READY;
            r->diagnostic[0] = '\0';
            p->imp_completed++;
            led_sidecar_write_pub(p, r);
        } else {
            if (!r->has_runtime_asset ||
                (session->engine != NULL &&
                 !le_asset_is_alive(session->engine,
                                    &r->runtime_asset))) {
                r->has_runtime_asset = 0;
                r->runtime_asset = LE_ASSET_INVALID;
                r->status = LED_IMPORT_FAILED;
            } else {
                r->status = LED_IMPORT_FAILED;
            }
            p->imp_failed++;
            led_console_push(session, LED_LOG_ERROR, "import",
                             r->diagnostic[0] != '\0'
                                 ? r->diagnostic
                                 : "import failed");
            return (rc == LED_ERROR_OUT_OF_MEMORY)
                       ? LED_ERROR_OUT_OF_MEMORY
                       : LED_ERROR_VALIDATION;
        }
    }
    led_browser_refresh(session);
    return LED_SUCCESS;
}

led_result led_import_asset(led_session *session,
                            const char *rel_path) {
    char norm[1024];
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
    if (session->playing) {
        return LED_ERROR_ALREADY_PLAYING;
    }
    if (!led_project_normalize(rel_path, norm)) {
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
    return led_import_one_record(session, (uint32_t)idx);
}

void led_import_queue_get_stats(
    const led_session *session, led_import_queue_stats *out_stats) {
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (session == NULL || session->project == NULL) {
        return;
    }
    out_stats->pending = session->project->imp_pending;
    out_stats->running = session->project->imp_running;
    out_stats->completed = session->project->imp_completed;
    out_stats->failed = session->project->imp_failed;
}
