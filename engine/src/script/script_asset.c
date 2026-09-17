/*
 * Script assets + script component (Phase 26). Assets live in the
 * Phase 25 registry (LE_ASSET_SCRIPT slots own source bytes +
 * compiled chunk refs); the component is one-per-object dense
 * storage with the standard swap-remove discipline.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "script/script_internal.h"

/* Read a whole file (bounded by LE_SCRIPT_MAX_SOURCE). Returns NULL
 * on any failure (absent/unreadable/oversize/OOM). */
static char *le_read_script_file(const char *path, size_t *out_size) {
    FILE *f = NULL;
    long len = 0;
    char *bytes = NULL;
    size_t got = 0;
#ifdef _MSC_VER
    errno_t fe = 0;
#endif

    if (out_size != NULL) {
        *out_size = 0;
    }
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
#ifdef _MSC_VER
    fe = fopen_s(&f, path, "rb");
    if (fe != 0 || f == NULL) {
        return NULL;
    }
#else
    f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
#endif
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    len = ftell(f);
    if (len <= 0 || (uint64_t)len > LE_SCRIPT_MAX_SOURCE) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    bytes = (char *)malloc((size_t)len + 1u);
    if (bytes == NULL) {
        fclose(f);
        return NULL;
    }
    got = fread(bytes, 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) {
        free(bytes);
        return NULL;
    }
    bytes[len] = '\0';
    if (out_size != NULL) {
        *out_size = (size_t)len;
    }
    return bytes;
}

/* Compile source through the backend (validates syntax). Returns a
 * chunk ref (>= 0) or a negative sentinel with error text. */
static int le_backend_compile(le_engine *engine, const char *source,
                              size_t size, const char *chunkname,
                              char *err, size_t err_cap) {
    le_script_runtime *rt = engine->script_runtime;

    if (rt == NULL || rt->backend == NULL ||
        rt->backend->compile == NULL) {
        if (err != NULL && err_cap > 0) {
            snprintf(err, err_cap, "no script runtime");
        }
        return -1;
    }
    return rt->backend->compile(rt, source, size, chunkname, err,
                                err_cap);
}

le_result le_asset_create_script(le_engine *engine,
                                 const le_script_asset_desc *desc,
                                 le_asset *out_asset) {
    le_result rc;
    le_result code = LE_SUCCESS;
    int32_t idx;
    le_asset handle = LE_ASSET_INVALID;
    le_asset_id id;
    char norm[1024];
    char err[512];
    char *copy = NULL;
    int chunk = -1;

    if (out_asset != NULL) {
        *out_asset = LE_ASSET_INVALID;
    }
    if (engine == NULL || desc == NULL || out_asset == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->source == NULL || desc->size == 0 ||
        desc->size > LE_SCRIPT_MAX_SOURCE) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    rc = le_script_runtime_ensure(engine);
    if (rc != LE_SUCCESS) {
        return rc;
    }
    if (desc->path_hint != NULL && desc->path_hint[0] != '\0') {
        if (!le_normalize_source(desc->path_hint, norm,
                                 sizeof(norm))) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    } else {
        snprintf(norm, sizeof(norm), "<snippet>");
    }
    /* Compile FIRST (transactional: nothing allocated on syntax
     * error). */
    err[0] = '\0';
    chunk = le_backend_compile(engine, desc->source, desc->size,
                               norm, err, sizeof(err));
    if (chunk < 0) {
        le_script_record_error(engine->script_runtime, "compile",
                               NULL, NULL, norm, err);
        return LE_ERROR_PARSE;
    }
    /* Persistent ID: content hash (identical bytes = identical ID).
     * NOTE: no dedup-by-ID — repeat creates return DISTINCT handles
     * sharing one ID (find_by_id returns the first READY match).
     * Dedup is by canonical PATH (load path only). Handles stay
     * generational; IDs stay content-derived.
     *
     * Portable identity (Phase 34A): when the caller supplies an
     * identity key (project UUID + relative locator), the key —
     * not the normalized access path — feeds the low half, so the
     * ID survives project relocation. NULL key keeps legacy
     * path-derived IDs. */
    id.hi = le_fnv1a64(desc->source, desc->size);
    if (desc->identity_key != NULL && desc->identity_len > 0) {
        id.lo = le_fnv1a64(desc->identity_key, desc->identity_len) ^
                (id.hi | 1u);
    } else {
        id.lo = le_fnv1a64(norm, strlen(norm)) ^ (id.hi | 1u);
    }
    if (id.hi == 0 && id.lo == 0) {
        id.lo = 1;
    }
    copy = (char *)malloc(desc->size + 1u);
    if (copy == NULL) {
        engine->script_runtime->backend->release_chunk(
            engine->script_runtime, chunk);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, desc->source, desc->size);
    copy[desc->size] = '\0';
    idx = le_asset_alloc(engine, LE_ASSET_SCRIPT, LE_ASSET_READY,
                         &id, norm, &code, &handle);
    if (idx < 0) {
        free(copy);
        engine->script_runtime->backend->release_chunk(
            engine->script_runtime, chunk);
        return code;
    }
    engine->assets[idx].script_source = copy;
    engine->assets[idx].script_size = desc->size;
    engine->assets[idx].script_chunk_ref = chunk;
    *out_asset = handle;
    return LE_SUCCESS;
}

le_result le_asset_load_script(le_engine *engine, const char *path,
                               le_asset *out_asset) {
    char norm[1024];
    uint32_t i;
    char *bytes = NULL;
    size_t size = 0;
    le_script_asset_desc desc;
    le_result rc;

    if (out_asset != NULL) {
        *out_asset = LE_ASSET_INVALID;
    }
    if (engine == NULL || path == NULL || out_asset == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_normalize_source(path, norm, sizeof(norm))) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (norm[0] == '.' && norm[1] == '.' &&
        (norm[2] == '/' || norm[2] == '\0')) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Dedup by canonical path (same policy as glTF). */
    for (i = 0; i < engine->asset_capacity; i++) {
        const le_asset_slot *s = &engine->assets[i];

        if (!s->alive || s->type != LE_ASSET_SCRIPT ||
            s->state != LE_ASSET_READY || s->source == NULL) {
            continue;
        }
        if (strcmp(s->source, norm) == 0) {
            out_asset->index = i;
            out_asset->generation = s->generation;
            return LE_SUCCESS;
        }
    }
    bytes = le_read_script_file(norm, &size);
    if (bytes == NULL) {
        return LE_ERROR_MISSING_ASSET;
    }
    memset(&desc, 0, sizeof(desc));
    desc.source = bytes;
    desc.size = size;
    desc.path_hint = norm;
    rc = le_asset_create_script(engine, &desc, out_asset);
    free(bytes);
    return rc;
}

le_result le_script_asset_set_source(le_engine *engine,
                                     const le_asset *asset,
                                     const char *source, size_t size) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_asset_slot *s;
    char err[512];
    char *copy = NULL;
    int chunk = -1;

    if (engine == NULL || asset == NULL || source == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0 || size > LE_SCRIPT_MAX_SOURCE) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (engine->script_runtime == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_asset_live(engine, asset, &slot, &code)) {
        return code;
    }
    s = &engine->assets[slot];
    if (s->type != LE_ASSET_SCRIPT) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    /* Compile the NEW source first; swap only on success. */
    err[0] = '\0';
    chunk = le_backend_compile(
        engine, source, size,
        (s->source != NULL) ? s->source : "<script>", err,
        sizeof(err));
    if (chunk < 0) {
        le_script_record_error(engine->script_runtime, "compile",
                               NULL, NULL,
                               (s->source != NULL) ? s->source
                                                   : "",
                               err);
        return LE_ERROR_PARSE;
    }
    copy = (char *)malloc(size + 1u);
    if (copy == NULL) {
        engine->script_runtime->backend->release_chunk(
            engine->script_runtime, chunk);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, source, size);
    copy[size] = '\0';
    engine->script_runtime->backend->release_chunk(
        engine->script_runtime, s->script_chunk_ref);
    free(s->script_source);
    s->script_source = copy;
    s->script_size = size;
    s->script_chunk_ref = chunk;
    /* Content hash follows the bytes (dedup identity = content).
     * Portable identity (Phase 34A): the KEY half is caller-
     * supplied at create time (project UUID) and set_source must
     * NOT re-derive it from the abs access path — the path is a
     * locator, not identity. Content half follows the bytes; key
     * half is preserved verbatim (it was fixed at create). */
    s->id.hi = le_fnv1a64(source, size);
    if (s->id.hi == 0 && s->id.lo == 0) {
        s->id.lo = 1;
    }
    return LE_SUCCESS;
}

le_result le_script_reload(le_engine *engine, const le_asset *asset) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_asset_slot *s;
    char err[512];
    int chunk = -1;

    if (engine == NULL || asset == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (engine->script_runtime == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_asset_live(engine, asset, &slot, &code)) {
        return code;
    }
    s = &engine->assets[slot];
    if (s->type != LE_ASSET_SCRIPT) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    if (s->script_source == NULL || s->script_size == 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Recompile CURRENT bytes; swap on success (instances keep
     * state + property values, adopt new code, no start() rerun —
     * applied in script_step.c via chunk re-resolve). */
    err[0] = '\0';
    chunk = le_backend_compile(
        engine, s->script_source, s->script_size,
        (s->source != NULL) ? s->source : "<script>", err,
        sizeof(err));
    if (chunk < 0) {
        le_script_record_error(engine->script_runtime, "reload",
                               NULL, NULL,
                               (s->source != NULL) ? s->source
                                                   : "",
                               err);
        return LE_ERROR_PARSE;
    }
    engine->script_runtime->backend->release_chunk(
        engine->script_runtime, s->script_chunk_ref);
    s->script_chunk_ref = chunk;
    /* Bump the instance code epoch so live instances re-resolve
     * funcs from the new chunk (state + values preserved). */
    {
        le_world *w;

        for (w = engine->worlds; w != NULL; w = w->next) {
            uint32_t i;

            for (i = 0; i < w->script_count; i++) {
                if (w->scripts[i].asset.index == slot &&
                    w->scripts[i].asset.generation ==
                        s->generation) {
                    le_script_rebind_instance(w, &w->scripts[i]);
                }
            }
        }
    }
    return LE_SUCCESS;
}

/* ------------------------------------------------------------------
 * Script component (dense, swap-remove, one per object).
 * ------------------------------------------------------------------ */

static le_result le_grow_scripts(le_world *world) {
    uint32_t grown;
    le_script_entry *fresh;

    if (world->script_count < world->script_capacity) {
        return LE_SUCCESS;
    }
    grown = (world->script_capacity == 0) ? 16u
                                          : world->script_capacity * 2u;
    if (grown < world->script_capacity + 1u) {
        return LE_ERROR_OVERFLOW;
    }
    if (grown > LE_MAX_CAPACITY) {
        return LE_ERROR_OVERFLOW;
    }
    fresh = (le_script_entry *)realloc(world->scripts,
                                       (size_t)grown *
                                           sizeof(*fresh));
    if (fresh == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    world->scripts = fresh;
    world->script_capacity = grown;
    return LE_SUCCESS;
}

static void le_remove_script_entry(le_world *world, uint32_t slot) {
    le_object_slot *s = &world->slots[slot];
    uint32_t idx;
    uint32_t last;

    if ((s->present & LE_PRESENT_SCRIPT) == 0u) {
        return;
    }
    idx = (uint32_t)s->script_index;
    if (idx < world->script_count &&
        world->scripts[idx].slot == slot) {
        if (world->engine != NULL &&
            world->engine->script_runtime != NULL) {
            world->engine->script_runtime->backend
                ->release_instance(world->engine->script_runtime,
                                   world, &world->scripts[idx]);
        }
        last = world->script_count - 1u;
        if (idx != last) {
            world->scripts[idx] = world->scripts[last];
            world->slots[world->scripts[idx].slot].script_index =
                (int32_t)idx;
        }
        world->script_count--;
    }
    s->present &= ~LE_PRESENT_SCRIPT;
    s->script_index = LE_NO_LINK;
}

le_result le_object_add_script(le_world *world, const le_object *object,
                               const le_asset *script) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_object_slot *s;
    le_engine *engine;
    uint32_t aslot;
    le_result grow;

    if (world == NULL || object == NULL || script == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    engine = world->engine;
    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->scripts_tearing_down) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_asset_live(engine, script, &aslot, &code)) {
        return (code == LE_ERROR_INVALID_ARGUMENT)
                   ? LE_ERROR_INVALID_ARGUMENT
                   : LE_ERROR_STALE_ASSET;
    }
    if (engine->assets[aslot].type != LE_ASSET_SCRIPT) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    if (engine->assets[aslot].state != LE_ASSET_READY) {
        return LE_ERROR_MISSING_ASSET;
    }
    {
        le_result rc = le_script_runtime_ensure(engine);

        if (rc != LE_SUCCESS) {
            return rc;
        }
    }
    s = &world->slots[slot];
    /* Replace: fire destroy for the old instance iff started,
     * release its state, then attach anew (pending start). */
    if ((s->present & LE_PRESENT_SCRIPT) != 0u) {
        le_script_fire_slot_destroy(world, slot);
        le_remove_script_entry(world, slot);
    }
    grow = le_grow_scripts(world);
    if (grow != LE_SUCCESS) {
        return grow;
    }
    world->scripts[world->script_count].slot = slot;
    world->scripts[world->script_count].asset = *script;
    world->scripts[world->script_count].started = 0;
    world->scripts[world->script_count].failed = 0;
    world->scripts[world->script_count].pending_start = 1;
    world->scripts[world->script_count].state_ref =
        LE_SCRIPT_NOREF;
    s->script_index = (int32_t)world->script_count;
    s->present |= LE_PRESENT_SCRIPT;
    world->script_count++;
    return LE_SUCCESS;
}

le_result le_object_remove_script(le_world *world,
                                  const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    if ((world->slots[slot].present & LE_PRESENT_SCRIPT) == 0u) {
        return LE_SUCCESS;
    }
    /* destroy() iff start() ran (consistent policy everywhere). */
    le_script_fire_slot_destroy(world, slot);
    le_remove_script_entry(world, slot);
    return LE_SUCCESS;
}

int le_object_get_script(const le_world *world, const le_object *object,
                         le_asset *out_script) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_script != NULL) {
        *out_script = LE_ASSET_INVALID;
    }
    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    if ((world->slots[slot].present & LE_PRESENT_SCRIPT) == 0u) {
        return 0;
    }
    {
        uint32_t idx = (uint32_t)world->slots[slot].script_index;

        if (idx >= world->script_count) {
            return 0;
        }
        if (world->scripts[idx].slot != slot) {
            return 0;
        }
        if (out_script != NULL) {
            *out_script = world->scripts[idx].asset;
        }
        return 1;
    }
}

int le_object_script_failed(const le_world *world,
                            const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    if ((world->slots[slot].present & LE_PRESENT_SCRIPT) == 0u) {
        return 0;
    }
    {
        uint32_t idx = (uint32_t)world->slots[slot].script_index;

        if (idx >= world->script_count) {
            return 0;
        }
        if (world->scripts[idx].slot != slot) {
            return 0;
        }
        return world->scripts[idx].failed ? 1 : 0;
    }
}
