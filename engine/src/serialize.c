/*
 * Luma Engine scene serialization, Phase 25 (Stages 30-38, 67-86):
 * canonical versioned text format, memory-first with file helpers
 * layered above. Deterministic: ascending-ID order, fixed float
 * formatting, sorted keys. Unknown FIELDS tolerated (forward
 * compat); unknown COMPONENTS and VERSIONS rejected. Untrusted
 * input: every count/length/enum/float checked, bounded allocations,
 * no partial scene/world corruption on failure.
 *
 * Format sketch (UTF-8, LF):
 *   LUMA_SCENE 1
 *   asset <type> <hexid> <path...>
 *   object <hexid>
 *     name "..."            (only when nonempty)
 *     enabled 0|1
 *     parent <hexid>|nil
 *     position x y z
 *     rotation x y z w
 *     scale x y z
 *     renderable <meshhex> <mathex> cast recv visible
 *     camera persp|ortho fov|height aspect near far
 *     light dir|point|spot color... intensity range inner outer
 *       shadow enabled res dbias nbias near far dist
 *   end
 * One record per line; `end` terminates each object. Assets list
 * the persistent IDs the scene references (informational +
 * relocation hints; instantiation resolves IDs through the
 * registry, never by path).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"

#define LE_SER_MAX_OBJECTS 0x00FFFFFFu
#define LE_SER_MAX_LINE 4096u
#define LE_SER_MAX_TEXT (64u * 1024u * 1024u)
#define LE_SER_MAX_NAME 127u
#define LE_SER_MAX_PATH 1023u

/* ------------------------------------------------------------------
 * Persistent-ID helpers.
 * ------------------------------------------------------------------ */

void le_scene_object_id_make(le_engine *engine,
                             le_scene_object_id *out_id) {
    if (out_id == NULL) {
        return;
    }
    out_id->hi = 0;
    out_id->lo = 0;
    le_uuid_mint(engine, &out_id->hi, &out_id->lo);
}

int le_scene_object_id_is_nil(const le_scene_object_id *id) {
    if (id == NULL) {
        return 1;
    }
    return (id->hi == 0 && id->lo == 0) ? 1 : 0;
}

int le_scene_object_id_equal(const le_scene_object_id *a,
                             const le_scene_object_id *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    return (a->hi == b->hi && a->lo == b->lo) ? 1 : 0;
}

static void le_hex64(uint64_t v, char out[17]) {
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 16; i++) {
        out[15 - i] = digits[v & 0xFu];
        v >>= 4;
    }
    out[16] = '\0';
}

void le_asset_id_to_string(const le_asset_id *id, char out_hex[33]) {
    char hi[17];
    char lo[17];

    if (out_hex == NULL) {
        return;
    }
    memset(out_hex, '0', 32);
    out_hex[32] = '\0';
    if (id == NULL) {
        return;
    }
    le_hex64(id->hi, hi);
    le_hex64(id->lo, lo);
    memcpy(out_hex, hi, 16);
    memcpy(out_hex + 16, lo, 16);
}

static int le_hexval(char c, unsigned *out) {
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

static int le_hex64_parse(const char *s, uint64_t *out) {
    uint64_t v = 0;
    int i;

    for (i = 0; i < 16; i++) {
        unsigned d = 0;

        if (!le_hexval(s[i], &d)) {
            return 0;
        }
        v = (v << 4) | (uint64_t)d;
    }
    *out = v;
    return 1;
}

int le_asset_id_from_string(const char *hex, le_asset_id *out_id) {
    le_asset_id tmp;

    if (out_id != NULL) {
        memset(out_id, 0, sizeof(*out_id));
    }
    if (hex == NULL || out_id == NULL) {
        return 0;
    }
    if (strlen(hex) != 32) {
        return 0;
    }
    if (!le_hex64_parse(hex, &tmp.hi) ||
        !le_hex64_parse(hex + 16, &tmp.lo)) {
        return 0;
    }
    *out_id = tmp;
    return 1;
}

int le_asset_id_equal(const le_asset_id *a, const le_asset_id *b) {
    if (a == NULL || b == NULL) {
        return 0;
    }
    return (a->hi == b->hi && a->lo == b->lo) ? 1 : 0;
}

int le_asset_id_is_nil(const le_asset_id *id) {
    if (id == NULL) {
        return 1;
    }
    return (id->hi == 0 && id->lo == 0) ? 1 : 0;
}

/* ------------------------------------------------------------------
 * Deterministic float formatting (%.9g round-trips float32 exactly;
 * fixed choice keeps bytes canonical).
 * ------------------------------------------------------------------ */

static void le_fmt_float(char *out, size_t cap, float v) {
    /* Non-finite floats never serialize (callers reject NaN/Inf
     * records before writing). Defensive: emit 0. */
    if (!(v == v) || v > 3.402823466e+38f ||
        v < -3.402823466e+38f) {
        if (!(v == v)) {
            snprintf(out, cap, "0");
            return;
        }
    }
    snprintf(out, cap, "%.9g", (double)v);
}

static int le_parse_float_checked(const char *s, float *out) {
    /* strtof with full-consumption + range discipline. NaN/Inf
     * spellings rejected (return 0): records must carry finite
     * values (quaternion/lens/range rules enforce the rest). */
    char *end = NULL;
    float v;

    if (s == NULL || *s == '\0' || out == NULL) {
        return 0;
    }
    v = strtof(s, &end);
    if (end == s || *end != '\0') {
        return 0;
    }
    if (!(v == v)) {
        return 0;
    }
    if (v > 3.402823466e+38f || v < -3.402823466e+38f) {
        /* Clamp-out: strtof HUGE_VALF overflow — reject (records
         * never legitimately carry 3.4e38). */
        return 0;
    }
    {
        /* Reject inf spellings explicitly (strtof accepts them). */
        const char *p = s;

        if (*p == '+' || *p == '-') {
            p++;
        }
        if ((p[0] == 'i' || p[0] == 'I') &&
            (p[1] == 'n' || p[1] == 'N')) {
            return 0;
        }
    }
    *out = v;
    return 1;
}

/* ------------------------------------------------------------------
 * Escaping (names + paths): \" \\ \n \t \r + \uXXXX for other
 * C0/C1 controls; UTF-8 bytes pass through verbatim. Malformed
 * escapes fail the parse (never silent mojibake).
 * ------------------------------------------------------------------ */

static int le_escape_append(char **buf, size_t *len, size_t *cap,
                            const char *s, size_t max_in) {
    size_t i;

    for (i = 0; i < max_in && s[i] != '\0'; i++) {
        unsigned char c = (unsigned char)s[i];
        const char *esc = NULL;
        char ubuf[8];

        if (c == '"') {
            esc = "\\\"";
        } else if (c == '\\') {
            esc = "\\\\";
        } else if (c == '\n') {
            esc = "\\n";
        } else if (c == '\t') {
            esc = "\\t";
        } else if (c == '\r') {
            esc = "\\r";
        } else if (c < 0x20 || c == 0x7F) {
            snprintf(ubuf, sizeof(ubuf), "\\u%04x", c);
            esc = ubuf;
        }
        {
            const char *chunk = (esc != NULL) ? esc : s + i;
            size_t chunk_len =
                (esc != NULL) ? strlen(esc) : 1u;

            if (*len + chunk_len + 1u > *cap) {
                size_t grown = (*cap == 0) ? 1024u : *cap * 2u;
                char *fresh;

                while (grown < *len + chunk_len + 1u) {
                    grown *= 2u;
                }
                if (grown > LE_SER_MAX_TEXT) {
                    return 0;
                }
                fresh = (char *)realloc(*buf, grown);
                if (fresh == NULL) {
                    return 0;
                }
                *buf = fresh;
                *cap = grown;
            }
            memcpy(*buf + *len, chunk, chunk_len);
            *len += chunk_len;
        }
    }
    return 1;
}

static int le_buf_puts(char **buf, size_t *len, size_t *cap,
                       const char *s) {
    size_t n = strlen(s);

    if (*len + n + 1u > *cap) {
        size_t grown = (*cap == 0) ? 1024u : *cap * 2u;
        char *fresh;

        while (grown < *len + n + 1u) {
            grown *= 2u;
        }
        if (grown > LE_SER_MAX_TEXT) {
            return 0;
        }
        fresh = (char *)realloc(*buf, grown);
        if (fresh == NULL) {
            return 0;
        }
        *buf = fresh;
        *cap = grown;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
    return 1;
}

/* Unescape one quoted string (input WITHOUT quotes). Writes at most
 * out_cap-1 bytes + NUL. Returns 1 ok / 0 malformed/overflow. */
static int le_unescape(const char *s, char *out, size_t out_cap) {
    size_t pos = 0;
    size_t i = 0;

    if (s == NULL || out == NULL || out_cap == 0) {
        return 0;
    }
    while (s[i] != '\0') {
        if (s[i] != '\\') {
            if (pos + 1 >= out_cap) {
                return 0;
            }
            out[pos++] = s[i++];
            continue;
        }
        i++;
        switch (s[i]) {
        case '"':
        case '\\':
        case '/':
            if (pos + 1 >= out_cap) {
                return 0;
            }
            out[pos++] = s[i++];
            break;
        case 'n':
            if (pos + 1 >= out_cap) {
                return 0;
            }
            out[pos++] = '\n';
            i++;
            break;
        case 't':
            if (pos + 1 >= out_cap) {
                return 0;
            }
            out[pos++] = '\t';
            i++;
            break;
        case 'r':
            if (pos + 1 >= out_cap) {
                return 0;
            }
            out[pos++] = '\r';
            i++;
            break;
        case 'u': {
            unsigned h = 0;
            int k;

            for (k = 1; k <= 4; k++) {
                unsigned d = 0;

                if (s[i + k] == '\0' ||
                    !le_hexval(s[i + k], &d)) {
                    return 0;
                }
                h = (h << 4) | d;
            }
            i += 5;
            /* Encode h as UTF-8 (BMP only; surrogates rejected). */
            if (h >= 0xD800u && h <= 0xDFFFu) {
                return 0;
            }
            if (h < 0x80u) {
                if (pos + 1 >= out_cap) {
                    return 0;
                }
                out[pos++] = (char)h;
            } else if (h < 0x800u) {
                if (pos + 2 >= out_cap) {
                    return 0;
                }
                out[pos++] = (char)(0xC0u | (h >> 6));
                out[pos++] = (char)(0x80u | (h & 0x3Fu));
            } else {
                if (pos + 3 >= out_cap) {
                    return 0;
                }
                out[pos++] = (char)(0xE0u | (h >> 12));
                out[pos++] = (char)(0x80u | ((h >> 6) & 0x3Fu));
                out[pos++] = (char)(0x80u | (h & 0x3Fu));
            }
            break;
        }
        default:
            return 0; /* bad escape */
        }
    }
    if (pos >= out_cap) {
        return 0;
    }
    out[pos] = '\0';
    return 1;
}

/* ------------------------------------------------------------------
 * Save (deterministic: records sorted by persistent ID bytes).
 * ------------------------------------------------------------------ */

static int le_cmp_records(const void *a, const void *b) {
    const le_scene_object *ra = (const le_scene_object *)a;
    const le_scene_object *rb = (const le_scene_object *)b;

    if (ra->id.hi != rb->id.hi) {
        return (ra->id.hi < rb->id.hi) ? -1 : 1;
    }
    if (ra->id.lo != rb->id.lo) {
        return (ra->id.lo < rb->id.lo) ? -1 : 1;
    }
    return 0;
}

le_result le_scene_save_text(le_engine *engine, const le_asset *scene,
                             char **out_text, size_t *out_size) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_asset_slot *s;
    char *buf = NULL;
    size_t len = 0;
    size_t cap = 0;
    le_scene_object *sorted = NULL;
    uint32_t i;

    if (out_text != NULL) {
        *out_text = NULL;
    }
    if (out_size != NULL) {
        *out_size = 0;
    }
    if (engine == NULL || scene == NULL || out_text == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_asset_live(engine, scene, &slot, &code)) {
        return code;
    }
    s = &engine->assets[slot];
    if (s->type != LE_ASSET_SCENE) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    if (!le_buf_puts(&buf, &len, &cap, "LUMA_SCENE 1\n")) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    /* Canonical order: sort a COPY by persistent ID (registry
     * order is allocation order, not canonical). */
    if (s->scene_count > 0) {
        sorted = (le_scene_object *)malloc(
            (size_t)s->scene_count * sizeof(*sorted));
        if (sorted == NULL) {
            free(buf);
            return LE_ERROR_OUT_OF_MEMORY;
        }
        memcpy(sorted, s->scene_objects,
               (size_t)s->scene_count * sizeof(*sorted));
        qsort(sorted, s->scene_count, sizeof(*sorted),
              le_cmp_records);
    }
    /* Asset reference table (sorted by hex for determinism):
     * every mesh/material ID named by any record. */
    {
        le_asset_id *ids = NULL;
        size_t nids = 0;
        size_t cids = 0;

        for (i = 0; i < s->scene_count; i++) {
            const le_scene_object *rec = &sorted[i];
            int k;

            if (!rec->has_renderable) {
                continue;
            }
            for (k = 0; k < 2; k++) {
                const le_asset_id *id =
                    (k == 0) ? &rec->mesh_id : &rec->material_id;
                size_t j;
                int seen = 0;

                for (j = 0; j < nids; j++) {
                    if (le_asset_id_equal(&ids[j], id)) {
                        seen = 1;
                        break;
                    }
                }
                if (seen) {
                    continue;
                }
                if (nids >= cids) {
                    size_t grown = (cids == 0) ? 8u : cids * 2u;
                    le_asset_id *fresh = (le_asset_id *)realloc(
                        ids, grown * sizeof(*fresh));

                    if (fresh == NULL) {
                        free(ids);
                        free(sorted);
                        free(buf);
                        return LE_ERROR_OUT_OF_MEMORY;
                    }
                    ids = fresh;
                    cids = grown;
                }
                ids[nids++] = *id;
            }
        }
        /* Sort by hex bytes (hi,lo compare). */
        {
            size_t a;
            size_t b;

            for (a = 0; a < nids; a++) {
                for (b = a + 1u; b < nids; b++) {
                    if (ids[b].hi < ids[a].hi ||
                        (ids[b].hi == ids[a].hi &&
                         ids[b].lo < ids[a].lo)) {
                        le_asset_id t = ids[a];

                        ids[a] = ids[b];
                        ids[b] = t;
                    }
                }
            }
        }
        for (i = 0; i < nids; i++) {
            char hex[33];
            char line[128];
            const char *kind = "material";
            uint32_t aslot;
            const char *path = "";

            le_asset_id_to_string(&ids[i], hex);
            /* Kind + path hint from the live registry when the
             * asset is loaded (informational only; instantiate
             * resolves by ID). */
            {
                uint32_t q;

                for (q = 0; q < engine->asset_capacity; q++) {
                    const le_asset_slot *as = &engine->assets[q];

                    if (!as->alive ||
                        le_asset_id_is_nil(
                            (const le_asset_id *)&as->id)) {
                        continue;
                    }
                    if (as->id.hi == ids[i].hi &&
                        as->id.lo == ids[i].lo) {
                        aslot = q;
                        kind = (as->type == LE_ASSET_MESH)
                                   ? "mesh"
                                   : "material";
                        path = (as->source != NULL) ? as->source
                                                    : "";
                        break;
                    }
                }
                (void)aslot;
            }
            snprintf(line, sizeof(line), "asset %s %s ", kind, hex);
            if (!le_buf_puts(&buf, &len, &cap, line)) {
                free(ids);
                free(sorted);
                free(buf);
                return LE_ERROR_OUT_OF_MEMORY;
            }
            if (!le_escape_append(&buf, &len, &cap, path,
                                 LE_SER_MAX_PATH)) {
                free(ids);
                free(sorted);
                free(buf);
                return LE_ERROR_OUT_OF_MEMORY;
            }
            buf[len] = '\0';
            if (!le_buf_puts(&buf, &len, &cap, "\n")) {
                free(ids);
                free(sorted);
                free(buf);
                return LE_ERROR_OUT_OF_MEMORY;
            }
        }
        free(ids);
    }
    for (i = 0; i < s->scene_count; i++) {
        const le_scene_object *rec = &sorted[i];
        char hex[33];
        char line[256];
        char f[32];
        int k;

        snprintf(hex, sizeof(hex), "%016llx%016llx",
                 (unsigned long long)rec->id.hi,
                 (unsigned long long)rec->id.lo);
        snprintf(line, sizeof(line), "object %s\n", hex);
        if (!le_buf_puts(&buf, &len, &cap, line)) {
            goto oom;
        }
        if (rec->name[0] != '\0') {
            if (!le_buf_puts(&buf, &len, &cap, "name \"")) {
                goto oom;
            }
            if (!le_escape_append(&buf, &len, &cap, rec->name,
                                 sizeof(rec->name))) {
                goto oom;
            }
            buf[len] = '\0';
            if (!le_buf_puts(&buf, &len, &cap, "\"\n")) {
                goto oom;
            }
        }
        snprintf(line, sizeof(line), "enabled %d\n",
                 rec->enabled ? 1 : 0);
        if (!le_buf_puts(&buf, &len, &cap, line)) {
            goto oom;
        }
        if (rec->has_parent) {
            snprintf(line, sizeof(line),
                     "parent %016llx%016llx\n",
                     (unsigned long long)rec->parent.hi,
                     (unsigned long long)rec->parent.lo);
        } else {
            snprintf(line, sizeof(line), "parent nil\n");
        }
        if (!le_buf_puts(&buf, &len, &cap, line)) {
            goto oom;
        }
        if (!le_buf_puts(&buf, &len, &cap, "position")) {
            goto oom;
        }
        for (k = 0; k < 3; k++) {
            le_fmt_float(f, sizeof(f), rec->position[k]);
            snprintf(line, sizeof(line), " %s", f);
            if (!le_buf_puts(&buf, &len, &cap, line)) {
                goto oom;
            }
        }
        if (!le_buf_puts(&buf, &len, &cap, "\nrotation")) {
            goto oom;
        }
        for (k = 0; k < 4; k++) {
            le_fmt_float(f, sizeof(f), rec->rotation[k]);
            snprintf(line, sizeof(line), " %s", f);
            if (!le_buf_puts(&buf, &len, &cap, line)) {
                goto oom;
            }
        }
        if (!le_buf_puts(&buf, &len, &cap, "\nscale")) {
            goto oom;
        }
        for (k = 0; k < 3; k++) {
            le_fmt_float(f, sizeof(f), rec->scale[k]);
            snprintf(line, sizeof(line), " %s", f);
            if (!le_buf_puts(&buf, &len, &cap, line)) {
                goto oom;
            }
        }
        if (!le_buf_puts(&buf, &len, &cap, "\n")) {
            goto oom;
        }
        if (rec->has_renderable) {
            char mhex[33];
            char thex[33];

            le_asset_id_to_string(&rec->mesh_id, mhex);
            le_asset_id_to_string(&rec->material_id, thex);
            snprintf(line, sizeof(line),
                     "renderable %s %s %d %d %d\n", mhex, thex,
                     rec->casts_shadow ? 1 : 0,
                     rec->receives_shadow ? 1 : 0,
                     rec->visible ? 1 : 0);
            if (!le_buf_puts(&buf, &len, &cap, line)) {
                goto oom;
            }
        }
        if (rec->has_camera) {
            const le_camera_desc *c = &rec->camera;

            le_fmt_float(f, sizeof(f),
                         (c->projection == LE_PROJECTION_PERSPECTIVE)
                             ? c->fov_y_rad
                             : c->ortho_height);
            if (c->projection == LE_PROJECTION_PERSPECTIVE) {
                char a[32];
                char n[32];
                char fa[32];

                le_fmt_float(a, sizeof(a), c->aspect);
                le_fmt_float(n, sizeof(n), c->near_plane);
                le_fmt_float(fa, sizeof(fa), c->far_plane);
                snprintf(line, sizeof(line),
                         "camera persp %s %s %s %s\n", f, a, n,
                         fa);
            } else {
                char a[32];
                char n[32];
                char fa[32];

                le_fmt_float(a, sizeof(a), c->aspect);
                le_fmt_float(n, sizeof(n), c->near_plane);
                le_fmt_float(fa, sizeof(fa), c->far_plane);
                snprintf(line, sizeof(line),
                         "camera ortho %s %s %s %s\n", f, a, n,
                         fa);
            }
            if (!le_buf_puts(&buf, &len, &cap, line)) {
                goto oom;
            }
        }
        if (rec->has_light) {
            const le_light_desc *l = &rec->light;
            const char *kind = (l->type == LE_LIGHT_POINT)
                                   ? "point"
                                   : ((l->type == LE_LIGHT_SPOT)
                                          ? "spot"
                                          : "dir");
            char c0[32];
            char c1[32];
            char c2[32];
            char in[32];
            char rg[32];
            char si[32];
            char so[32];

            le_fmt_float(c0, sizeof(c0), l->color[0]);
            le_fmt_float(c1, sizeof(c1), l->color[1]);
            le_fmt_float(c2, sizeof(c2), l->color[2]);
            le_fmt_float(in, sizeof(in), l->intensity);
            le_fmt_float(rg, sizeof(rg), l->range);
            le_fmt_float(si, sizeof(si), l->spot_inner);
            le_fmt_float(so, sizeof(so), l->spot_outer);
            snprintf(line, sizeof(line),
                     "light %s %s %s %s %s %s %s %s\n", kind, c0,
                     c1, c2, in, rg, si, so);
            if (!le_buf_puts(&buf, &len, &cap, line)) {
                goto oom;
            }
            if (l->shadow.enabled) {
                char db[32];
                char nb[32];
                char sn[32];
                char sf[32];
                char sd[32];

                le_fmt_float(db, sizeof(db),
                             l->shadow.depth_bias);
                le_fmt_float(nb, sizeof(nb),
                             l->shadow.normal_bias);
                le_fmt_float(sn, sizeof(sn),
                             l->shadow.near_plane);
                le_fmt_float(sf, sizeof(sf),
                             l->shadow.far_plane);
                le_fmt_float(sd, sizeof(sd),
                             l->shadow.shadow_distance);
                snprintf(line, sizeof(line),
                         "shadow 1 %u %s %s %s %s %s\n",
                         l->shadow.resolution, db, nb, sn, sf,
                         sd);
            } else {
                snprintf(line, sizeof(line), "shadow 0\n");
            }
            if (!le_buf_puts(&buf, &len, &cap, line)) {
                goto oom;
            }
        }
        /* Script component (Phase 26): `script <hex>` + one
         * `sprop <kind> <name> <value...>` line per exported
         * property, in export order (never VM state). */
        if (rec->has_script) {
            char shex[33];
            uint32_t p;

            le_asset_id_to_string(&rec->script_id, shex);
            snprintf(line, sizeof(line), "script %s\n", shex);
            if (!le_buf_puts(&buf, &len, &cap, line)) {
                goto oom;
            }
            for (p = 0; p < rec->script_prop_count &&
                        p < LE_SCRIPT_MAX_PROPS;
                 p++) {
                const le_script_property *sp =
                    &rec->script_props[p];
                char pname[80];
                size_t pn = 0;

                /* Property names are identifier-checked at
                 * export(); quote defensively anyway. */
                while (sp->name[pn] != '\0' && pn < 63) {
                    pn++;
                }
                snprintf(pname, sizeof(pname), "%.*s",
                         (int)pn, sp->name);
                switch (sp->type) {
                case LE_SCRIPT_PROP_BOOL:
                    snprintf(line, sizeof(line), "sprop bool %s %d\n",
                             pname, sp->boolean ? 1 : 0);
                    break;
                case LE_SCRIPT_PROP_INT:
                    snprintf(line, sizeof(line),
                             "sprop int %s %lld\n", pname,
                             (long long)sp->integer);
                    break;
                case LE_SCRIPT_PROP_NUMBER: {
                    char v[32];

                    le_fmt_float(v, sizeof(v),
                                 (float)sp->number);
                    /* Full double round-trip needs more digits
                     * than float formatting: emit %.17g. */
                    snprintf(line, sizeof(line),
                             "sprop number %s %.17g\n", pname,
                             sp->number);
                    break;
                }
                case LE_SCRIPT_PROP_STRING:
                    if (!le_buf_puts(&buf, &len, &cap,
                                     "sprop string ")) {
                        goto oom;
                    }
                    if (!le_buf_puts(&buf, &len, &cap, pname)) {
                        goto oom;
                    }
                    if (!le_buf_puts(&buf, &len, &cap, " \"")) {
                        goto oom;
                    }
                    if (!le_escape_append(&buf, &len, &cap,
                                          sp->string_value,
                                          sizeof(sp
                                                     ->string_value))) {
                        goto oom;
                    }
                    buf[len] = '\0';
                    if (!le_buf_puts(&buf, &len, &cap, "\"\n")) {
                        goto oom;
                    }
                    continue;
                case LE_SCRIPT_PROP_VEC3: {
                    char v0[32];
                    char v1[32];
                    char v2[32];

                    le_fmt_float(v0, sizeof(v0), sp->vec3[0]);
                    le_fmt_float(v1, sizeof(v1), sp->vec3[1]);
                    le_fmt_float(v2, sizeof(v2), sp->vec3[2]);
                    snprintf(line, sizeof(line),
                             "sprop vec3 %s %s %s %s\n", pname,
                             v0, v1, v2);
                    break;
                }
                case LE_SCRIPT_PROP_ASSET: {
                    char ahex[33];
                    le_asset_id aid;

                    /* sp->asset is a LIVE handle (authoritative);
                     * persist its registry ID. Unresolvable
                     * (unloaded mid-capture — impossible via the
                     * refcount guard, but defensive) skips the
                     * prop rather than writing garbage. */
                    {
                        uint32_t aslot = 0;
                        le_result ac = LE_SUCCESS;

                        if (!le_resolve_asset_live(
                                engine, &sp->asset, &aslot,
                                &ac)) {
                            continue;
                        }
                        aid = engine->assets[aslot].id;
                    }
                    le_asset_id_to_string(&aid, ahex);
                    snprintf(line, sizeof(line),
                             "sprop asset %s %s\n", pname, ahex);
                    break;
                }
                default:
                    continue;
                }
                if (!le_buf_puts(&buf, &len, &cap, line)) {
                    goto oom;
                }
            }
        }
        if (!le_buf_puts(&buf, &len, &cap, "end\n")) {
            goto oom;
        }
    }
    free(sorted);
    if (out_size != NULL) {
        *out_size = len;
    }
    *out_text = buf;
    return LE_SUCCESS;

oom:
    free(sorted);
    free(buf);
    return LE_ERROR_OUT_OF_MEMORY;
}

void le_scene_free_text(char *text) {
    free(text);
}

/* ------------------------------------------------------------------
 * Parse (transactional: records stage in a temp array; the scene
 * payload swaps in ONLY on full success).
 * ------------------------------------------------------------------ */

typedef struct le_parse_rec {
    le_scene_object rec;
    int has_id;
    int ended;
} le_parse_rec;

/* sprop asset-ID side table: `sprop asset` lines persist asset
 * VALUES as persistent IDs (handles are runtime-only). One entry
 * per asset-valued sprop, keyed by (record index, prop name). The
 * commit path resolves each ID to a live handle; resolution
 * failure fails the whole load (transactional, like every other
 * asset ref). Bounded: at most LE_SCRIPT_MAX_PROPS per record. */
typedef struct le_sprop_asset_note {
    size_t rec;
    char name[64];
    le_asset_id id;
} le_sprop_asset_note;

/* Split a line into whitespace tokens (in place; up to max_tok).
 * Returns token count. A '\"' opens a quoted tail (for name "..."
 * and asset paths with spaces): the tail through the closing quote
 * is ONE token (quotes stripped). */
static int le_tokenize(char *line, char **tok, int max_tok) {
    int n = 0;
    char *p = line;

    while (*p != '\0' && n < max_tok) {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
        }
        if (*p == '\0' || *p == '\n') {
            break;
        }
        if (*p == '"') {
            /* Quoted token: through the closing quote. */
            char *start = p + 1;
            char *q = start;

            while (*q != '\0' && *q != '"' && *q != '\n') {
                if (*q == '\\' && q[1] != '\0' && q[1] != '\n') {
                    q += 2;
                } else {
                    q++;
                }
            }
            if (*q != '"') {
                return -1; /* unterminated quote */
            }
            *q = '\0';
            tok[n++] = start;
            p = q + 1;
        } else {
            char *start = p;

            while (*p != '\0' && *p != ' ' && *p != '\t' &&
                   *p != '\r' && *p != '\n') {
                p++;
            }
            if (*p != '\0') {
                *p = '\0';
                p++;
            }
            tok[n++] = start;
        }
    }
    return n;
}

static int le_parse_hex32(const char *s, uint64_t *hi,
                          uint64_t *lo) {
    if (s == NULL || strlen(s) != 32) {
        return 0;
    }
    return le_hex64_parse(s, hi) && le_hex64_parse(s + 16, lo);
}

le_result le_scene_load_text(le_engine *engine, const le_asset *scene,
                             const char *text, size_t size) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_asset_slot *s;
    le_parse_rec *recs = NULL;
    size_t nrecs = 0;
    size_t crecs = 0;
    le_sprop_asset_note *anotes = NULL;
    size_t nnotes = 0;
    size_t cnotes = 0;
    int cur = -1;
    size_t pos = 0;
    size_t text_len;
    int header_seen = 0;

    if (engine == NULL || scene == NULL || text == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (size > LE_SER_MAX_TEXT) {
        return LE_ERROR_PARSE;
    }
    if (!le_resolve_asset_live(engine, scene, &slot, &code)) {
        return code;
    }
    s = &engine->assets[slot];
    if (s->type != LE_ASSET_SCENE) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    text_len = 0;
    while (text_len < size && text[text_len] != '\0') {
        text_len++;
    }
    if (text_len != size) {
        /* Embedded NUL inside declared size: malformed. (Size
         * counts bytes; text must be NUL-free UTF-8.) */
        return LE_ERROR_PARSE;
    }
    /* Line loop over a scratch copy (bounded). */
    {
        char *copy;
        char *line;
        char *save = NULL;

        if (size == 0) {
            return LE_ERROR_PARSE;
        }
        copy = (char *)malloc(size + 1u);
        if (copy == NULL) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
        memcpy(copy, text, size);
        copy[size] = '\0';
        line = copy;
        /* Manual line split (strtok-free: nesting/depth bounded by
         * construction — one record per line). */
        {
            char *p = copy;

            while (1) {
                char *eol = strchr(p, '\n');
                char *ln;
                size_t llen;

                (void)save;
                (void)line;
                if (eol != NULL) {
                    *eol = '\0';
                    ln = p;
                    p = eol + 1;
                } else {
                    ln = p;
                    p = p + strlen(p);
                }
                llen = strlen(ln);
                if (llen > 0 && ln[llen - 1] == '\r') {
                    ln[llen - 1] = '\0';
                }
                if (ln[0] != '\0' && ln[0] != '#') {
                    /* Parse one line. */
                    char *tok[16];
                    int ntok = le_tokenize(ln, tok, 16);
                    le_result lr =
                        LE_SUCCESS; /* per-line status */

                    if (ntok < 0) {
                        lr = LE_ERROR_PARSE;
                    } else if (ntok == 0) {
                        lr = LE_SUCCESS;
                    } else if (!header_seen) {
                        if (ntok == 2 &&
                            strcmp(tok[0], "LUMA_SCENE") == 0) {
                            if (strcmp(tok[1], "1") == 0) {
                                header_seen = 1;
                            } else {
                                free(copy);
                                free(recs);
                                free(anotes);
                                return LE_ERROR_UNSUPPORTED_VERSION;
                            }
                        } else {
                            free(copy);
                            free(recs);
                            free(anotes);
                            return LE_ERROR_PARSE;
                        }
                    } else if (strcmp(tok[0], "asset") == 0) {
                        /* Informational table: validate shape,
                         * tolerate fully (IDs resolve at
                         * instantiate). Unknown KINDS rejected
                         * (typo-proofing beats silent skip). */
                        if (ntok < 3 ||
                            (strcmp(tok[1], "mesh") != 0 &&
                             strcmp(tok[1], "material") != 0)) {
                            lr = LE_ERROR_PARSE;
                        } else {
                            uint64_t hi = 0;
                            uint64_t lo = 0;

                            if (!le_parse_hex32(tok[2], &hi,
                                                &lo)) {
                                lr = LE_ERROR_PARSE;
                            }
                        }
                    } else if (strcmp(tok[0], "object") == 0) {
                        if (ntok != 2) {
                            lr = LE_ERROR_PARSE;
                        } else {
                            uint64_t hi = 0;
                            uint64_t lo = 0;

                            if (!le_parse_hex32(tok[1], &hi,
                                                &lo) ||
                                (hi == 0 && lo == 0)) {
                                lr = LE_ERROR_PARSE;
                            } else {
                                if (nrecs >= crecs) {
                                    size_t grown =
                                        (crecs == 0) ? 16u
                                                     : crecs * 2u;
                                    le_parse_rec *fresh;

                                    if (grown >
                                        LE_SER_MAX_OBJECTS) {
                                        free(copy);
                                        free(recs);
                                        free(anotes);
                                        return LE_ERROR_PARSE;
                                    }
                                    fresh =
                                        (le_parse_rec *)realloc(
                                            recs,
                                            grown *
                                                sizeof(*fresh));
                                    if (fresh == NULL) {
                                        free(copy);
                                        free(recs);
                                        free(anotes);
                                        return LE_ERROR_OUT_OF_MEMORY;
                                    }
                                    recs = fresh;
                                    crecs = grown;
                                }
                                memset(&recs[nrecs], 0,
                                       sizeof(recs[nrecs]));
                                recs[nrecs].rec.id.hi = hi;
                                recs[nrecs].rec.id.lo = lo;
                                recs[nrecs].has_id = 1;
                                recs[nrecs].rec.enabled = 1;
                                recs[nrecs]
                                    .rec.rotation[3] = 1.0f;
                                recs[nrecs].rec.scale[0] = 1.0f;
                                recs[nrecs].rec.scale[1] = 1.0f;
                                recs[nrecs].rec.scale[2] = 1.0f;
                                recs[nrecs].rec.visible = 1;
                                cur = (int)nrecs;
                                nrecs++;
                            }
                        }
                    } else if (cur < 0) {
                        lr = LE_ERROR_PARSE; /* field w/o object */
                    } else if (strcmp(tok[0], "name") == 0) {
                        if (ntok != 2) {
                            lr = LE_ERROR_PARSE;
                        } else if (!le_unescape(
                                       tok[1],
                                       recs[cur].rec.name,
                                       sizeof(
                                           recs[cur].rec.name))) {
                            lr = LE_ERROR_PARSE;
                        }
                    } else if (strcmp(tok[0], "enabled") == 0) {
                        if (ntok != 2 ||
                            (strcmp(tok[1], "0") != 0 &&
                             strcmp(tok[1], "1") != 0)) {
                            lr = LE_ERROR_PARSE;
                        } else {
                            recs[cur].rec.enabled =
                                (tok[1][0] == '1') ? 1 : 0;
                        }
                    } else if (strcmp(tok[0], "parent") == 0) {
                        if (ntok != 2) {
                            lr = LE_ERROR_PARSE;
                        } else if (strcmp(tok[1], "nil") == 0) {
                            recs[cur].rec.has_parent = 0;
                        } else {
                            uint64_t hi = 0;
                            uint64_t lo = 0;

                            if (!le_parse_hex32(tok[1], &hi,
                                                &lo) ||
                                (hi == 0 && lo == 0)) {
                                lr = LE_ERROR_PARSE;
                            } else {
                                recs[cur].rec.parent.hi = hi;
                                recs[cur].rec.parent.lo = lo;
                                recs[cur].rec.has_parent = 1;
                            }
                        }
                    } else if (strcmp(tok[0], "position") == 0 ||
                               strcmp(tok[0], "scale") == 0) {
                        if (ntok != 4) {
                            lr = LE_ERROR_PARSE;
                        } else {
                            float v[3];
                            int k;
                            int ok = 1;

                            for (k = 0; k < 3; k++) {
                                if (!le_parse_float_checked(
                                        tok[1 + k], &v[k])) {
                                    ok = 0;
                                }
                            }
                            if (!ok) {
                                lr = LE_ERROR_PARSE;
                            } else if (
                                strcmp(tok[0], "position") ==
                                0) {
                                memcpy(recs[cur].rec.position,
                                       v, sizeof(v));
                            } else {
                                memcpy(recs[cur].rec.scale, v,
                                       sizeof(v));
                            }
                        }
                    } else if (strcmp(tok[0], "rotation") == 0) {
                        if (ntok != 5) {
                            lr = LE_ERROR_PARSE;
                        } else {
                            float v[4];
                            int k;
                            int ok = 1;

                            for (k = 0; k < 4; k++) {
                                if (!le_parse_float_checked(
                                        tok[1 + k], &v[k])) {
                                    ok = 0;
                                }
                            }
                            if (!ok) {
                                lr = LE_ERROR_PARSE;
                            } else {
                                memcpy(recs[cur].rec.rotation,
                                       v, sizeof(v));
                            }
                        }
                    } else if (strcmp(tok[0], "renderable") ==
                               0) {
                        if (ntok != 6) {
                            lr = LE_ERROR_PARSE;
                        } else {
                            le_asset_id mid;
                            le_asset_id tid;
                            int b0;
                            int b1;
                            int b2;

                            if (!le_asset_id_from_string(
                                    tok[1], &mid) ||
                                !le_asset_id_from_string(
                                    tok[2], &tid) ||
                                le_asset_id_is_nil(&mid) ||
                                le_asset_id_is_nil(&tid)) {
                                lr = LE_ERROR_PARSE;
                            } else if (
                                (strcmp(tok[3], "0") != 0 &&
                                 strcmp(tok[3], "1") != 0) ||
                                (strcmp(tok[4], "0") != 0 &&
                                 strcmp(tok[4], "1") != 0) ||
                                (strcmp(tok[5], "0") != 0 &&
                                 strcmp(tok[5], "1") != 0)) {
                                lr = LE_ERROR_PARSE;
                            } else {
                                b0 = (tok[3][0] == '1') ? 1 : 0;
                                b1 = (tok[4][0] == '1') ? 1 : 0;
                                b2 = (tok[5][0] == '1') ? 1 : 0;
                                recs[cur].rec.has_renderable = 1;
                                recs[cur].rec.mesh_id = mid;
                                recs[cur].rec.material_id = tid;
                                recs[cur].rec.casts_shadow = b0;
                                recs[cur].rec.receives_shadow =
                                    b1;
                                recs[cur].rec.visible = b2;
                            }
                        }
                    } else if (strcmp(tok[0], "camera") == 0) {
                        if (ntok != 6) {
                            lr = LE_ERROR_PARSE;
                        } else {
                            int persp;

                            if (strcmp(tok[1], "persp") == 0) {
                                persp = 1;
                            } else if (strcmp(tok[1],
                                              "ortho") == 0) {
                                persp = 0;
                            } else {
                                persp = -1;
                            }
                            if (persp < 0) {
                                lr = LE_ERROR_PARSE;
                            } else {
                                float lens;
                                float asp;
                                float np;
                                float fp;

                                if (!le_parse_float_checked(
                                        tok[2], &lens) ||
                                    !le_parse_float_checked(
                                        tok[3], &asp) ||
                                    !le_parse_float_checked(
                                        tok[4], &np) ||
                                    !le_parse_float_checked(
                                        tok[5], &fp)) {
                                    lr = LE_ERROR_PARSE;
                                } else {
                                    le_camera_desc cd;

                                    memset(&cd, 0, sizeof(cd));
                                    cd.projection =
                                        persp
                                            ? LE_PROJECTION_PERSPECTIVE
                                            : LE_PROJECTION_ORTHOGRAPHIC;
                                    if (persp) {
                                        cd.fov_y_rad = lens;
                                        cd.ortho_height = 10.0f;
                                    } else {
                                        cd.fov_y_rad =
                                            1.0471976f;
                                        cd.ortho_height = lens;
                                    }
                                    cd.aspect = asp;
                                    cd.near_plane = np;
                                    cd.far_plane = fp;
                                    recs[cur].rec.has_camera = 1;
                                    recs[cur].rec.camera = cd;
                                }
                            }
                        }
                    } else if (strcmp(tok[0], "light") == 0) {
                        if (ntok != 9) {
                            lr = LE_ERROR_PARSE;
                        } else {
                            le_light_type lt;

                            if (strcmp(tok[1], "dir") == 0) {
                                lt = LE_LIGHT_DIRECTIONAL;
                            } else if (strcmp(tok[1],
                                              "point") == 0) {
                                lt = LE_LIGHT_POINT;
                            } else if (strcmp(tok[1],
                                              "spot") == 0) {
                                lt = LE_LIGHT_SPOT;
                            } else {
                                free(copy);
                                free(recs);
                                free(anotes);
                                return LE_ERROR_PARSE;
                            }
                            {
                                float v[7];
                                int k;
                                int ok = 1;

                                for (k = 0; k < 7; k++) {
                                    if (!le_parse_float_checked(
                                            tok[2 + k],
                                            &v[k])) {
                                        ok = 0;
                                    }
                                }
                                if (!ok) {
                                    lr = LE_ERROR_PARSE;
                                } else {
                                    le_light_desc ld;

                                    memset(&ld, 0,
                                           sizeof(ld));
                                    ld.type = lt;
                                    ld.color[0] = v[0];
                                    ld.color[1] = v[1];
                                    ld.color[2] = v[2];
                                    ld.intensity = v[3];
                                    ld.range = v[4];
                                    ld.spot_inner = v[5];
                                    ld.spot_outer = v[6];
                                    recs[cur].rec.has_light = 1;
                                    recs[cur].rec.light = ld;
                                }
                            }
                        }
                    } else if (strcmp(tok[0], "shadow") == 0) {
                        if (!recs[cur].rec.has_light) {
                            lr = LE_ERROR_PARSE;
                        } else if (ntok == 2 &&
                                   strcmp(tok[1], "0") == 0) {
                            memset(&recs[cur]
                                        .rec.light.shadow,
                                   0,
                                   sizeof(recs[cur]
                                              .rec.light.shadow));
                        } else if (ntok == 8 &&
                                   strcmp(tok[1], "1") == 0) {
                            /* Canonical 8-token form (writer
                             * always emits this): enabled,
                             * resolution, depth/normal bias,
                             * near, far, distance. */
                            char *ep = NULL;
                            unsigned long res =
                                strtoul(tok[2], &ep, 10);
                            float db;
                            float nb;
                            float sn;
                            float sf;
                            float sd;

                            if (ep == tok[2] || *ep != '\0' ||
                                res < 128u || res > 4096u ||
                                (res & (res - 1u)) != 0u ||
                                !le_parse_float_checked(
                                    tok[3], &db) ||
                                !le_parse_float_checked(
                                    tok[4], &nb) ||
                                !le_parse_float_checked(
                                    tok[5], &sn) ||
                                !le_parse_float_checked(
                                    tok[6], &sf) ||
                                !le_parse_float_checked(
                                    tok[7], &sd)) {
                                lr = LE_ERROR_PARSE;
                            } else {
                                recs[cur]
                                    .rec.light.shadow.enabled = 1;
                                recs[cur]
                                    .rec.light.shadow.resolution =
                                    (uint32_t)res;
                                recs[cur]
                                    .rec.light.shadow.depth_bias =
                                    db;
                                recs[cur]
                                    .rec.light.shadow.normal_bias =
                                    nb;
                                recs[cur]
                                    .rec.light.shadow.near_plane =
                                    sn;
                                recs[cur]
                                    .rec.light.shadow.far_plane =
                                    sf;
                                recs[cur]
                                    .rec.light.shadow
                                    .shadow_distance = sd;
                            }
                        } else {
                            lr = LE_ERROR_PARSE;
                        }
                    } else if (strcmp(tok[0], "end") == 0) {
                        if (ntok != 1) {
                            lr = LE_ERROR_PARSE;
                        } else {
                            recs[cur].ended = 1;
                            cur = -1;
                        }
                    } else if (strcmp(tok[0], "script") == 0) {
                        /* script <hex>: marks the record scripted;
                         * property lines (sprop) follow. Duplicate
                         * script lines in one record are
                         * malformed. */
                        if (ntok != 2) {
                            lr = LE_ERROR_PARSE;
                        } else if (recs[cur].rec.has_script) {
                            lr = LE_ERROR_PARSE;
                        } else {
                            le_asset_id sid;

                            if (!le_asset_id_from_string(
                                    tok[1], &sid) ||
                                le_asset_id_is_nil(&sid)) {
                                lr = LE_ERROR_PARSE;
                            } else {
                                recs[cur].rec.has_script = 1;
                                recs[cur].rec.script_id = sid;
                                recs[cur]
                                    .rec.script_prop_count = 0;
                            }
                        }
                    } else if (strcmp(tok[0], "sprop") == 0) {
                        /* sprop <kind> <name> <value...>: appended
                         * to the current record's script props.
                         * sprop outside a scripted record is
                         * malformed. */
                        if (!recs[cur].rec.has_script) {
                            lr = LE_ERROR_PARSE;
                        } else if (ntok < 4) {
                            lr = LE_ERROR_PARSE;
                        } else if (recs[cur]
                                       .rec.script_prop_count >=
                                   LE_SCRIPT_MAX_PROPS) {
                            lr = LE_ERROR_PARSE;
                        } else {
                            le_script_property *dst =
                                &recs[cur].rec.script_props
                                     [recs[cur]
                                          .rec.script_prop_count];
                            size_t kn = strlen(tok[2]);

                            memset(dst, 0, sizeof(*dst));
                            if (kn == 0 || kn >= sizeof(dst->name)) {
                                lr = LE_ERROR_PARSE;
                            } else if (strcmp(tok[1], "bool") ==
                                       0) {
                                if (ntok != 4 ||
                                    (strcmp(tok[3], "0") != 0 &&
                                     strcmp(tok[3], "1") != 0)) {
                                    lr = LE_ERROR_PARSE;
                                } else {
                                    memcpy(dst->name, tok[2],
                                           kn + 1);
                                    dst->type =
                                        LE_SCRIPT_PROP_BOOL;
                                    dst->boolean =
                                        (tok[3][0] == '1') ? 1
                                                            : 0;
                                    recs[cur]
                                        .rec.script_prop_count++;
                                }
                            } else if (strcmp(tok[1], "int") ==
                                       0) {
                                if (ntok != 4) {
                                    lr = LE_ERROR_PARSE;
                                } else {
                                    char *ep = NULL;
                                    long long v =
                                        strtoll(tok[3], &ep, 10);

                                    if (ep == tok[3] ||
                                        *ep != '\0') {
                                        lr = LE_ERROR_PARSE;
                                    } else {
                                        memcpy(dst->name, tok[2],
                                               kn + 1);
                                        dst->type =
                                            LE_SCRIPT_PROP_INT;
                                        dst->integer =
                                            (int64_t)v;
                                        recs[cur]
                                            .rec.script_prop_count++;
                                    }
                                }
                            } else if (strcmp(tok[1], "number") ==
                                       0) {
                                float fv;

                                if (ntok != 4 ||
                                    !le_parse_float_checked(
                                        tok[3], &fv)) {
                                    lr = LE_ERROR_PARSE;
                                } else {
                                    memcpy(dst->name, tok[2],
                                           kn + 1);
                                    dst->type =
                                        LE_SCRIPT_PROP_NUMBER;
                                    dst->number =
                                        (double)fv;
                                    recs[cur]
                                        .rec.script_prop_count++;
                                }
                            } else if (strcmp(tok[1], "string") ==
                                       0) {
                                if (ntok != 4) {
                                    lr = LE_ERROR_PARSE;
                                } else if (!le_unescape(
                                               tok[3],
                                               dst->string_value,
                                               sizeof(dst
                                                          ->string_value))) {
                                    lr = LE_ERROR_PARSE;
                                } else {
                                    memcpy(dst->name, tok[2],
                                           kn + 1);
                                    dst->type =
                                        LE_SCRIPT_PROP_STRING;
                                    recs[cur]
                                        .rec.script_prop_count++;
                                }
                            } else if (strcmp(tok[1], "vec3") ==
                                       0) {
                                if (ntok != 6) {
                                    lr = LE_ERROR_PARSE;
                                } else {
                                    float v[3];
                                    int k;
                                    int ok = 1;

                                    for (k = 0; k < 3; k++) {
                                        if (!le_parse_float_checked(
                                                tok[3 + k],
                                                &v[k])) {
                                            ok = 0;
                                        }
                                    }
                                    if (!ok) {
                                        lr = LE_ERROR_PARSE;
                                    } else {
                                        memcpy(dst->name, tok[2],
                                               kn + 1);
                                        dst->type =
                                            LE_SCRIPT_PROP_VEC3;
                                        memcpy(dst->vec3, v,
                                               sizeof(v));
                                        recs[cur]
                                            .rec.script_prop_count++;
                                    }
                                }
                            } else if (strcmp(tok[1], "asset") ==
                                       0) {
                                /* Asset props persist as IDs; the
                                 * sprop asset-ID side table (kept
                                 * alongside recs) resolves at
                                 * instantiate. */
                                if (ntok != 4) {
                                    lr = LE_ERROR_PARSE;
                                } else {
                                    le_asset_id aid;

                                    if (!le_asset_id_from_string(
                                            tok[3], &aid) ||
                                        le_asset_id_is_nil(
                                            &aid)) {
                                        lr = LE_ERROR_PARSE;
                                    } else {
                                        memcpy(dst->name, tok[2],
                                               kn + 1);
                                        dst->type =
                                            LE_SCRIPT_PROP_ASSET;
                                        dst->asset.index =
                                            LE_ASSET_INDEX_INVALID;
                                        dst->asset.generation = 0;
                                        /* Stash the ID in the side
                                         * table (resolved at
                                         * commit). */
                                        {
                                            le_sprop_asset_note
                                                *fresh = NULL;

                                            if (nnotes >=
                                                cnotes) {
                                                size_t grown =
                                                    (cnotes == 0)
                                                        ? 16u
                                                        : cnotes *
                                                          2u;
                                                fresh = (le_sprop_asset_note *)
                                                    realloc(
                                                        anotes,
                                                        grown *
                                                        sizeof(
                                                            *fresh));
                                                if (fresh ==
                                                    NULL) {
                                                    free(copy);
                                                    free(recs);
                                                    free(anotes);
                                                    return LE_ERROR_OUT_OF_MEMORY;
                                                }
                                                anotes = fresh;
                                                cnotes = grown;
                                            }
                                            anotes[nnotes]
                                                .rec = (size_t)cur;
                                            memcpy(anotes[nnotes]
                                                       .name,
                                                   tok[2],
                                                   kn + 1);
                                            anotes[nnotes].id =
                                                aid;
                                            nnotes++;
                                        }
                                        recs[cur]
                                            .rec.script_prop_count++;
                                    }
                                }
                            } else {
                                lr = LE_ERROR_PARSE;
                            }
                        }
                    } else {
                        /* Unknown LINE KIND inside an object:
                         * tolerate only `shadowdist`-style
                         * future fields? Policy: unknown FIELDS
                         * (key vocabulary outside the known set)
                         * are IGNORED for forward compat ONLY
                         * when they carry no structural meaning.
                         * Unknown component kinds (renderable2,
                         * physics, sprop-typos, ...) are REJECTED
                         * — silently dropping a component would
                         * corrupt the scene's meaning. ("script"
                         * and "sprop" take the dedicated branches
                         * above, so they never reach this list.) */
                        if (strcmp(tok[0], "shadowdist") == 0 &&
                            ntok == 2 &&
                            recs[cur].rec.has_light) {
                            float sd;

                            if (!le_parse_float_checked(
                                    tok[1], &sd)) {
                                lr = LE_ERROR_PARSE;
                            } else {
                                recs[cur]
                                    .rec.light.shadow
                                    .shadow_distance = sd;
                            }
                        } else if (
                            strcmp(tok[0], "renderable") == 0 ||
                            strcmp(tok[0], "camera") == 0 ||
                            strcmp(tok[0], "light") == 0 ||
                            strcmp(tok[0], "physics") == 0 ||
                            strcmp(tok[0], "audio") == 0 ||
                            strcmp(tok[0], "component") == 0) {
                            /* NOTE: "script"/"sprop" are handled as
                             * known lines above — they must NOT
                             * appear here (script is not an
                             * unknown component). */
                            free(copy);
                            free(recs);
                            free(anotes);
                            return LE_ERROR_PARSE;
                        } else {
                            /* Unknown field: tolerant (forward
                             * compat). Shape-check lightly:
                             * must have at least a value. */
                            if (ntok < 2) {
                                lr = LE_ERROR_PARSE;
                            }
                        }
                    }
                    if (lr != LE_SUCCESS) {
                        free(copy);
                        free(recs);
                        free(anotes);
                        return lr;
                    }
                }
                if (*p == '\0') {
                    break;
                }
            }
        }
        free(copy);
    }
    if (!header_seen) {
        free(recs);
        free(anotes);
        return LE_ERROR_PARSE;
    }
    if (nrecs > LE_SER_MAX_OBJECTS) {
        free(recs);
        free(anotes);
        return LE_ERROR_PARSE;
    }
    /* Unterminated record (missing end) is malformed. */
    if (cur >= 0) {
        free(recs);
        free(anotes);
        return LE_ERROR_PARSE;
    }
    {
        size_t a;
        size_t b;

        for (a = 0; a < nrecs; a++) {
            if (!recs[a].has_id || !recs[a].ended) {
                free(recs);
                free(anotes);
                return LE_ERROR_PARSE;
            }
            for (b = a + 1u; b < nrecs; b++) {
                if (recs[a].rec.id.hi == recs[b].rec.id.hi &&
                    recs[a].rec.id.lo == recs[b].rec.id.lo) {
                    free(recs);
                    free(anotes);
                    return LE_ERROR_DUPLICATE_ID;
                }
            }
        }
    }
    /* Commit: swap in (old payload freed; scene was validated
     * fully before touching it — transactional). sprop asset-ID
     * side notes resolve to live handles here (missing assets fail
     * the whole load — same rule as every other asset ref). */
    {
        le_scene_object *payload = NULL;

        if (nrecs > 0) {
            payload = (le_scene_object *)malloc(
                nrecs * sizeof(*payload));
            if (payload == NULL) {
                free(recs);
                free(anotes);
                return LE_ERROR_OUT_OF_MEMORY;
            }
            for (pos = 0; pos < nrecs; pos++) {
                payload[pos] = recs[pos].rec;
            }
        }
        /* Resolve asset-valued script props against the live
         * registry (by persistent ID). */
        {
            size_t k;

            for (k = 0; k < nnotes; k++) {
                size_t r = anotes[k].rec;
                uint32_t p;
                int done = 0;

                if (r >= nrecs) {
                    free(payload);
                    free(recs);
                    free(anotes);
                    return LE_ERROR_PARSE;
                }
                for (p = 0;
                     p < payload[r].script_prop_count; p++) {
                    le_script_property *dst =
                        &payload[r].script_props[p];

                    if (dst->type == LE_SCRIPT_PROP_ASSET &&
                        strcmp(dst->name, anotes[k].name) ==
                            0) {
                        le_asset h = LE_ASSET_INVALID;

                        if (!le_asset_find_by_id(
                                engine, &anotes[k].id, &h)) {
                            free(payload);
                            free(recs);
                            free(anotes);
                            return LE_ERROR_MISSING_ASSET;
                        }
                        dst->asset = h;
                        done = 1;
                        break;
                    }
                }
                if (!done) {
                    free(payload);
                    free(recs);
                    free(anotes);
                    return LE_ERROR_PARSE;
                }
            }
        }
        free(s->scene_objects);
        s->scene_objects = payload;
        s->scene_count = (uint32_t)nrecs;
        s->scene_capacity = (uint32_t)nrecs;
    }
    free(anotes);
    free(recs);
    return LE_SUCCESS;
}

/* ------------------------------------------------------------------
 * File helpers (explicit paths; thin over the memory core).
 * ------------------------------------------------------------------ */

le_result le_scene_save_file(le_engine *engine, const le_asset *scene,
                             const char *path) {
    char *text = NULL;
    size_t size = 0;
    le_result res;
    FILE *f;

    if (engine == NULL || scene == NULL || path == NULL ||
        path[0] == '\0') {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    res = le_scene_save_text(engine, scene, &text, &size);
    if (res != LE_SUCCESS) {
        return res;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        le_scene_free_text(text);
        return LE_ERROR_PARSE;
    }
    if (size > 0 && fwrite(text, 1, size, f) != size) {
        fclose(f);
        le_scene_free_text(text);
        return LE_ERROR_PARSE;
    }
    fclose(f);
    le_scene_free_text(text);
    return LE_SUCCESS;
}

le_result le_scene_load_file(le_engine *engine, const le_asset *scene,
                             const char *path) {
    FILE *f;
    long len;
    char *buf = NULL;
    size_t got;
    le_result res;

    if (engine == NULL || scene == NULL || path == NULL ||
        path[0] == '\0') {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return LE_ERROR_MISSING_ASSET;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return LE_ERROR_PARSE;
    }
    len = ftell(f);
    if (len < 0 || (uint64_t)len > LE_SER_MAX_TEXT) {
        fclose(f);
        return LE_ERROR_PARSE;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return LE_ERROR_PARSE;
    }
    buf = (char *)malloc((size_t)len + 1u);
    if (buf == NULL) {
        fclose(f);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    got = fread(buf, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(buf);
        return LE_ERROR_PARSE;
    }
    res = le_scene_load_text(engine, scene, buf, got);
    free(buf);
    return res;
}
