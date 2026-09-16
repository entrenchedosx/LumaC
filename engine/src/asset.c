/*
 * Luma Engine assets, Phase 25 (Stages 1-20, 50-55, 59-60, 82-84):
 * engine-owned registry of generational runtime handles wrapping
 * renderer resources, with deterministic persistent IDs, explicit
 * ownership, type safety, and unload policy.
 *
 * Domain: assets live on the ENGINE (never in a world). Every world
 * on the engine references every READY asset. Destroying a world
 * never destroys assets (renderables hold HANDLES; handles die with
 * their slots, backing resources stay). Engine shutdown drains
 * worlds first, then the registry (see le_engine_destroy).
 *
 * Identity (three concepts):
 * - runtime handle {index, generation}: temporary, like le_object.
 * - persistent le_asset_id {hi,lo}: FNV-1a content hash for
 *   imported/primitive bytes (deterministic across runs), UUID for
 *   procedural assets (minted, process-unique).
 * - source path: normalized locator, NOT identity. Dedup key only.
 *
 * Lifetime: registry-owned until explicit unload (no GC). Unload
 * scans every world for referencing renderables + material texture
 * pins (O(objects)); referenced assets fail ASSET_IN_USE. No
 * back-pointer table exists to rot.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"

/* Phase 29: animation backing free without pulling the animation
 * layouts into asset.c (defined in src/animation/anim_asset.c).
 * Takes the opaque slot pointers (struct-blind). */
void le_anim_free_slot_backing(struct le_skeleton_data *skeleton,
                               struct le_clip_data *clip);

/* Phase 29: animator refcount without animator layout
 * (defined in src/animation/anim_serialize.c). */
uint32_t le_anim_refcount_slot(const le_world *world,
                               uint32_t asset_slot,
                               uint32_t generation);

const le_asset LE_ASSET_INVALID = { LE_ASSET_INDEX_INVALID, 0u };

#define LE_ASSET_INITIAL_CAPACITY 64u
#define LE_ASSET_MAX_CAPACITY ((uint32_t)0x00FFFFFFu)

/* ------------------------------------------------------------------
 * Small shared helpers.
 * ------------------------------------------------------------------ */

int le_asset_is_valid(const le_asset *asset) {
    if (asset == NULL) {
        return 0;
    }
    if (asset->index == LE_ASSET_INDEX_INVALID) {
        return 0;
    }
    if (asset->generation == 0) {
        return 0;
    }
    return 1;
}

int le_asset_is_alive(const le_engine *engine, const le_asset *asset) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (engine == NULL || asset == NULL) {
        return 0;
    }
    return le_resolve_asset_live(engine, asset, &slot, &code);
}

int le_resolve_asset_live(const le_engine *engine, const le_asset *asset,
                          uint32_t *out_slot, le_result *out_result) {
    const le_asset_slot *slot;

    if (engine == NULL || asset == NULL) {
        if (out_result != NULL) {
            *out_result = LE_ERROR_INVALID_ARGUMENT;
        }
        return 0;
    }
    if (asset->index == LE_ASSET_INDEX_INVALID ||
        asset->generation == 0 ||
        asset->index >= engine->asset_capacity) {
        if (out_result != NULL) {
            *out_result = LE_ERROR_STALE_ASSET;
        }
        return 0;
    }
    slot = &engine->assets[asset->index];
    if (!slot->alive || slot->generation != asset->generation) {
        if (out_result != NULL) {
            *out_result = LE_ERROR_STALE_ASSET;
        }
        return 0;
    }
    if (out_slot != NULL) {
        *out_slot = asset->index;
    }
    return 1;
}

le_result le_ensure_asset_capacity(le_engine *engine) {
    uint32_t grown;
    le_asset_slot *fresh;
    uint32_t i;

    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (engine->asset_capacity > 0 &&
        engine->asset_free_head != LE_NO_LINK) {
        return LE_SUCCESS;
    }
    if (engine->asset_capacity == 0) {
        grown = LE_ASSET_INITIAL_CAPACITY;
    } else {
        if (engine->asset_capacity > LE_ASSET_MAX_CAPACITY / 2u) {
            grown = LE_ASSET_MAX_CAPACITY;
        } else {
            grown = engine->asset_capacity * 2u;
        }
    }
    if (grown > LE_ASSET_MAX_CAPACITY) {
        return LE_ERROR_OVERFLOW;
    }
    if (engine->asset_capacity > 0 && grown <= engine->asset_capacity) {
        return LE_ERROR_OVERFLOW;
    }
    fresh = (le_asset_slot *)calloc(grown, sizeof(le_asset_slot));
    if (fresh == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    if (engine->assets != NULL) {
        memcpy(fresh, engine->assets,
               (size_t)engine->asset_capacity * sizeof(le_asset_slot));
        for (i = engine->asset_capacity; i < grown; i++) {
            fresh[i].alive = 0;
            fresh[i].generation = 0;
            fresh[i].free_next =
                (i + 1u < grown) ? (int32_t)(i + 1u)
                                 : engine->asset_free_head;
        }
        if (engine->asset_capacity < grown) {
            engine->asset_free_head = (int32_t)engine->asset_capacity;
        }
        free(engine->assets);
    } else {
        for (i = 0; i < grown; i++) {
            fresh[i].alive = 0;
            fresh[i].generation = 0;
            fresh[i].free_next =
                (i + 1u < grown) ? (int32_t)(i + 1u) : LE_NO_LINK;
        }
        engine->asset_free_head = 0;
    }
    engine->assets = fresh;
    engine->asset_capacity = grown;
    return LE_SUCCESS;
}

/* Normalize `path` into out (lexical only, no filesystem): '/'
 * separators, collapsed redundant separators, resolved '.' and
 * '..' (leading '..' retained lexically — loaders reject escapes
 * above the project root separately), no trailing slash. A leading
 * '/' (Unix absolute) or drive prefix (`C:/`, after backslash
 * folding) is PRESERVED as the root — absolute paths stay absolute
 * (tests pass absolute fixture paths on both platforms). Returns 1
 * on success, 0 on NULL/empty/overlong/unrepresentable input. */
int le_normalize_source(const char *path, char *out, size_t out_size) {
    /* Segments as (offset,length) into a scratch copy. */
    char scratch[1024];
    size_t offsets[256];
    size_t lengths[256];
    size_t nseg = 0;
    size_t i = 0;
    size_t len;
    size_t pos = 0;
    int absolute = 0;
    char drive = 0;

    if (path == NULL || out == NULL || out_size == 0) {
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
    /* Detect root: leading '/' (Unix absolute) or `X:/` drive
     * prefix (Windows absolute, after folding). The root is
     * emitted verbatim; only the remainder normalizes. */
    if (len >= 1 && scratch[0] == '/') {
        absolute = 1;
    } else if (len >= 3 &&
               ((scratch[0] >= 'A' && scratch[0] <= 'Z') ||
                (scratch[0] >= 'a' && scratch[0] <= 'z')) &&
               scratch[1] == ':' && scratch[2] == '/') {
        drive = scratch[0];
        absolute = 1;
    }
    /* Split on '/', skipping empties; resolve '.' / '..'. For a
     * drive prefix, the `X:` segment is consumed as the root (not a
     * normal segment): start after it. */
    i = (drive != 0) ? 3 : 0;
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
                                  scratch[offsets[nseg - 1] + 1] == '.')) {
                    nseg--;
                } else if (absolute) {
                    /* Above the root clamps (no escape from `/`
                     * or `X:/` — the root is the floor). */
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
    /* Re-emit: root first, then segments with '/' separators.
     * `first` tracks whether a separator is needed (absolute roots
     * already end with one). */
    {
        int first = 1;

        if (absolute) {
            if (drive != 0) {
                if (pos + 3 >= out_size) {
                    return 0;
                }
                out[pos++] = drive;
                out[pos++] = ':';
                out[pos++] = '/';
            } else {
                if (pos + 1 >= out_size) {
                    return 0;
                }
                out[pos++] = '/';
            }
            first = 0;
        }
        for (i = 0; i < nseg; i++) {
            size_t k;

            if (!first) {
                if (pos + 1 >= out_size) {
                    return 0;
                }
                out[pos++] = '/';
            }
            first = 0;
            if (pos + lengths[i] + 1 > out_size) {
                return 0;
            }
            for (k = 0; k < lengths[i]; k++) {
                out[pos++] = scratch[offsets[i] + k];
            }
        }
    }
    if (pos + 1 > out_size) {
        return 0;
    }
    out[pos] = '\0';
    return (pos > 0) ? 1 : 0;
}

uint64_t le_fnv1a64(const void *bytes, size_t size) {
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

void le_uuid_mint(le_engine *engine, uint64_t *hi, uint64_t *lo) {
    /* splitmix64 over a process counter folded with the engine
     * address: unique per process, deterministic-free (no RNG
     * needed, no persistence claims — UUIDs identify, content
     * hashes deduplicate). Never returns nil. */
    static uint64_t counter = 0;
    uint64_t z;

    counter += 0x9e3779b97f4a7c15ull;
    z = counter + (uint64_t)(uintptr_t)engine;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z = z ^ (z >> 31);
    if (hi != NULL) {
        *hi = z;
    }
    z = counter * 0xbf58476d1ce4e5b9ull +
        (uint64_t)(uintptr_t)&counter;
    z = (z ^ (z >> 30)) * 0x94d049bb133111ebull;
    z = z ^ (z >> 31);
    if (lo != NULL) {
        *lo = (z != 0) ? z : 1u;
    }
    if (hi != NULL && lo != NULL && *hi == 0 && *lo == 0) {
        *lo = 1u;
    }
}

/* Allocate + link one asset slot. Caller fills backing afterwards;
 * on backing failure the caller must le_asset_abandon the slot
 * (destroy partial backing, free source, return to free-list). */
int32_t le_asset_alloc(le_engine *engine, le_asset_type type,
                       le_asset_state state, const le_asset_id *id,
                       const char *source, le_result *out_result,
                       le_asset *out_handle) {
    le_result grow;
    int32_t idx;
    uint32_t gen;
    le_asset_slot *s;
    char *copy = NULL;

    if (engine == NULL) {
        if (out_result != NULL) {
            *out_result = LE_ERROR_INVALID_ARGUMENT;
        }
        return LE_NO_LINK;
    }
    if (type < 0 || type >= LE_ASSET_COUNT) {
        if (out_result != NULL) {
            *out_result = LE_ERROR_INVALID_ARGUMENT;
        }
        return LE_NO_LINK;
    }
    if (source != NULL && source[0] != '\0') {
        copy = (char *)malloc(strlen(source) + 1u);
        if (copy == NULL) {
            if (out_result != NULL) {
                *out_result = LE_ERROR_OUT_OF_MEMORY;
            }
            return LE_NO_LINK;
        }
        memcpy(copy, source, strlen(source) + 1u);
    }
    grow = le_ensure_asset_capacity(engine);
    if (grow != LE_SUCCESS) {
        free(copy);
        if (out_result != NULL) {
            *out_result = grow;
        }
        return LE_NO_LINK;
    }
    if (engine->asset_free_head == LE_NO_LINK) {
        free(copy);
        if (out_result != NULL) {
            *out_result = LE_ERROR_OVERFLOW;
        }
        return LE_NO_LINK;
    }
    idx = engine->asset_free_head;
    if (idx < 0 || (uint32_t)idx >= engine->asset_capacity) {
        free(copy);
        if (out_result != NULL) {
            *out_result = LE_ERROR_OVERFLOW;
        }
        return LE_NO_LINK;
    }
    engine->asset_free_head = engine->assets[idx].free_next;
    gen = engine->assets[idx].generation;
    if (gen == 0) {
        gen = 1;
    }
    s = &engine->assets[idx];
    memset(s, 0, sizeof(*s));
    s->alive = 1;
    s->generation = gen;
    s->free_next = LE_NO_LINK;
    s->type = type;
    s->state = state;
    if (id != NULL) {
        s->id = *id;
    }
    s->source = copy;
    if (copy != NULL) {
        engine->name_bytes_assets += strlen(copy) + 1u;
    }
    engine->asset_alive++;
    switch (type) {
    case LE_ASSET_MESH:
        engine->asset_meshes++;
        break;
    case LE_ASSET_MATERIAL:
        engine->asset_materials++;
        break;
    case LE_ASSET_TEXTURE:
        engine->asset_textures++;
        break;
    case LE_ASSET_SCENE:
        engine->asset_scenes++;
        break;
    case LE_ASSET_SCRIPT:
        engine->asset_scripts++;
        break;
    case LE_ASSET_SKELETON:
        engine->asset_skeletons++;
        break;
    case LE_ASSET_ANIMATION_CLIP:
        engine->asset_clips++;
        break;
    case LE_ASSET_PREFAB:
        engine->asset_prefabs++;
        break;
    default:
        break;
    }
    if (state == LE_ASSET_READY) {
        engine->asset_ready++;
    } else if (state == LE_ASSET_FAILED) {
        engine->asset_failed++;
    }
    if (out_handle != NULL) {
        out_handle->index = (uint32_t)idx;
        out_handle->generation = gen;
    }
    if (out_result != NULL) {
        *out_result = LE_SUCCESS;
    }
    return idx;
}

/* Abandon a just-allocated slot (backing creation failed):
 * reverses every counter le_asset_alloc bumped. */
static void le_asset_abandon(le_engine *engine, int32_t idx) {
    le_asset_slot *s;

    if (engine == NULL || idx < 0 ||
        (uint32_t)idx >= engine->asset_capacity) {
        return;
    }
    s = &engine->assets[idx];
    free(s->source);
    s->source = NULL;
    free(s->script_source);
    s->script_source = NULL;
    free(s->prefab_text);
    s->prefab_text = NULL;
    s->prefab_size = 0;
    /* Phase 29 animation backing frees via
     * le_anim_free_slot_backing (declared at the top; defined
     * in src/animation/ — asset.c must not learn the struct
     * layouts). */
    le_anim_free_slot_backing(s->skeleton, s->clip);
    s->skeleton = NULL;
    s->clip = NULL;
    if (s->state == LE_ASSET_READY && engine->asset_ready > 0) {
        engine->asset_ready--;
    } else if (s->state == LE_ASSET_FAILED &&
               engine->asset_failed > 0) {
        engine->asset_failed--;
    }
    switch (s->type) {
    case LE_ASSET_MESH:
        if (engine->asset_meshes > 0) {
            engine->asset_meshes--;
        }
        break;
    case LE_ASSET_MATERIAL:
        if (engine->asset_materials > 0) {
            engine->asset_materials--;
        }
        break;
    case LE_ASSET_TEXTURE:
        if (engine->asset_textures > 0) {
            engine->asset_textures--;
        }
        break;
    case LE_ASSET_SCENE:
        if (engine->asset_scenes > 0) {
            engine->asset_scenes--;
        }
        break;
    case LE_ASSET_SCRIPT:
        if (engine->asset_scripts > 0) {
            engine->asset_scripts--;
        }
        break;
    case LE_ASSET_SKELETON:
        if (engine->asset_skeletons > 0) {
            engine->asset_skeletons--;
        }
        break;
    case LE_ASSET_ANIMATION_CLIP:
        if (engine->asset_clips > 0) {
            engine->asset_clips--;
        }
        break;
    case LE_ASSET_PREFAB:
        if (engine->asset_prefabs > 0) {
            engine->asset_prefabs--;
        }
        break;
    default:
        break;
    }
    {
        uint32_t gen = s->generation + 1u;

        if (gen == 0) {
            gen = 1;
        }
        memset(s, 0, sizeof(*s));
        s->generation = gen;
    }
    s->free_next = engine->asset_free_head;
    engine->asset_free_head = idx;
    if (engine->asset_alive > 0) {
        engine->asset_alive--;
    }
}

/* Count live references to an asset slot: renderables in every
 * world naming it + material texture pins + script components
 * naming a script asset. O(worlds x objects). */
uint32_t le_asset_refcount(const le_engine *engine, uint32_t slot) {
    uint32_t n = 0;
    const le_world *w;

    if (engine == NULL || slot >= engine->asset_capacity) {
        return 0;
    }
    for (w = engine->worlds; w != NULL; w = w->next) {
        uint32_t i;

        for (i = 0; i < w->asset_renderable_count; i++) {
            const le_asset_renderable_desc *d =
                &w->asset_renderables[i].desc;

            if ((d->mesh.index == slot &&
                 engine->assets[slot].type == LE_ASSET_MESH) ||
                (d->material.index == slot &&
                 engine->assets[slot].type == LE_ASSET_MATERIAL)) {
                /* Generation check: only live handles count. */
                const le_asset *h = (d->mesh.index == slot) ? &d->mesh
                                                            : &d->material;

                if (h->generation ==
                    engine->assets[slot].generation) {
                    n++;
                }
            }
        }
        /* Script components naming this slot (script assets only;
         * generation-checked like every other handle user). */
        if (engine->assets[slot].type == LE_ASSET_SCRIPT) {
            for (i = 0; i < w->script_count; i++) {
                const le_script_entry *e = &w->scripts[i];

                if (e->asset.index == slot &&
                    e->asset.generation ==
                        engine->assets[slot].generation) {
                    n++;
                }
            }
        }
    }
    /* Material texture pins. */
    {
        uint32_t i;

        for (i = 0; i < engine->asset_capacity; i++) {
            const le_asset_slot *s = &engine->assets[i];

            if (!s->alive || s->type != LE_ASSET_MATERIAL) {
                continue;
            }
            if (s->has_base_color_texture &&
                s->base_color_texture.index == slot &&
                s->base_color_texture.generation ==
                    engine->assets[slot].generation) {
                n++;
            }
            if (s->has_metallic_roughness_texture &&
                s->metallic_roughness_texture.index == slot &&
                s->metallic_roughness_texture.generation ==
                    engine->assets[slot].generation) {
                n++;
            }
        }
    }
    /* Phase 29: animator skeleton/clip users (struct-blind hook;
     * asset.c never learns animator layout). Unload refuses
     * while referenced (same discipline as script assets). */
    if (engine->assets[slot].type == LE_ASSET_SKELETON ||
        engine->assets[slot].type == LE_ASSET_ANIMATION_CLIP) {
        const le_world *w;

        for (w = engine->worlds; w != NULL; w = w->next) {
            n += le_anim_refcount_slot(
                w, slot, engine->assets[slot].generation);
        }
    }
    return n;
}

/* ------------------------------------------------------------------
 * Procedural asset creation.
 * ------------------------------------------------------------------ */

#define LE_PREFAB_MAX_TEXT (64u * 1024u * 1024u)

/* Create a READY prefab asset from canonical prefab text (Phase
 * 32). Bytes are copied in; the format is validated by the editor
 * layer (the engine stores the payload opaquely). Empty/overlong
 * input creates nothing. */
le_result le_asset_create_prefab(le_engine *engine, const char *text,
                                 size_t size, le_asset *out_asset) {
    le_result code = LE_SUCCESS;
    int32_t idx;
    le_asset handle = LE_ASSET_INVALID;
    le_asset_id id;
    char *copy = NULL;

    if (engine == NULL || text == NULL || out_asset == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    *out_asset = LE_ASSET_INVALID;
    if (size == 0 || size >= LE_PREFAB_MAX_TEXT) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    copy = (char *)malloc(size + 1u);
    if (copy == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    memcpy(copy, text, size);
    copy[size] = '\0';
    /* Persistent ID: UUID per creation (authoring identity — two
     * copies of identical bytes are intentionally distinct; the
     * project layer assigns stable project IDs on top). */
    le_uuid_mint(engine, &id.hi, &id.lo);
    idx = le_asset_alloc(engine, LE_ASSET_PREFAB, LE_ASSET_READY,
                         &id, NULL, &code, &handle);
    if (idx < 0) {
        free(copy);
        return code;
    }
    engine->assets[idx].prefab_text = copy;
    engine->assets[idx].prefab_size = size;
    *out_asset = handle;
    return LE_SUCCESS;
}

const char *le_asset_get_prefab_text(const le_engine *engine,
                                     const le_asset *asset,
                                     size_t *out_size) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_size != NULL) {
        *out_size = 0;
    }
    if (engine == NULL || asset == NULL) {
        return "";
    }
    if (!le_resolve_asset_live(engine, asset, &slot, &code)) {
        return "";
    }
    if (engine->assets[slot].type != LE_ASSET_PREFAB ||
        engine->assets[slot].state != LE_ASSET_READY ||
        engine->assets[slot].prefab_text == NULL) {
        return "";
    }
    if (out_size != NULL) {
        *out_size = engine->assets[slot].prefab_size;
    }
    return engine->assets[slot].prefab_text;
}

le_result le_asset_create_mesh(le_engine *engine,
                              const le_mesh_asset_desc *desc,
                              le_asset *out_asset) {
    le_result code = LE_SUCCESS;
    int32_t idx;
    le_asset handle = LE_ASSET_INVALID;
    lr_mesh *mesh = NULL;
    lr_mesh_desc mdesc;
    le_asset_id id;

    if (engine == NULL || desc == NULL || out_asset == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    *out_asset = LE_ASSET_INVALID;
    if (engine->renderer == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->vertices == NULL || desc->vertex_count == 0 ||
        desc->indices == NULL || desc->index_count == 0 ||
        (desc->index_count % 3u) != 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Validate indices before allocating anything. */
    {
        uint32_t i;

        for (i = 0; i < desc->index_count; i++) {
            if (desc->indices[i] >= desc->vertex_count) {
                return LE_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    /* Persistent ID: content hash (deterministic across runs —
     * identical bytes dedup conceptually; registry dedups exact
     * procedural duplicates only when both bytes AND source match,
     * which for procedural means: never implicit — each create is
     * a distinct asset unless re-imported by path). Procedural
     * meshes mint UUIDs (each create is intentionally distinct). */
    le_uuid_mint(engine, &id.hi, &id.lo);
    (void)code;
    idx = le_asset_alloc(engine, LE_ASSET_MESH, LE_ASSET_READY, &id,
                         NULL, &code, &handle);
    if (idx < 0) {
        return code;
    }
    memset(&mdesc, 0, sizeof(mdesc));
    mdesc.vertices = desc->vertices;
    mdesc.vertex_count = desc->vertex_count;
    mdesc.indices = desc->indices;
    mdesc.index_count = desc->index_count;
    if (lr_mesh_create(engine->renderer, &mdesc, &mesh) != LR_SUCCESS) {
        le_asset_abandon(engine, idx);
        return LE_ERROR_RENDERER;
    }
    engine->assets[idx].mesh = mesh;
    *out_asset = handle;
    return LE_SUCCESS;
}

le_result le_asset_create_material(le_engine *engine,
                                   const le_material_asset_desc *desc,
                                   le_asset *out_asset) {
    le_result code = LE_SUCCESS;
    int32_t idx;
    le_asset handle = LE_ASSET_INVALID;
    lr_material *material = NULL;
    lr_pbr_material_desc mdesc;
    le_asset_id id;
    lc_image_view *base_view = NULL;
    lc_image_view *mr_view = NULL;

    if (engine == NULL || desc == NULL || out_asset == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    *out_asset = LE_ASSET_INVALID;
    if (engine->renderer == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!((desc->base_color_factor[3] == desc->base_color_factor[3]))) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Validate + resolve texture dependencies FIRST (no partial
     * allocation on bad deps). */
    if (desc->has_base_color_texture) {
        uint32_t tslot;

        if (!le_resolve_asset_live(engine, &desc->base_color_texture,
                                   &tslot, &code)) {
            return (code == LE_ERROR_INVALID_ARGUMENT)
                       ? LE_ERROR_INVALID_ARGUMENT
                       : LE_ERROR_STALE_ASSET;
        }
        if (engine->assets[tslot].type != LE_ASSET_TEXTURE) {
            return LE_ERROR_WRONG_ASSET_TYPE;
        }
        if (engine->assets[tslot].state != LE_ASSET_READY) {
            return LE_ERROR_MISSING_ASSET;
        }
        base_view = engine->assets[tslot].texture_view;
    }
    if (desc->has_metallic_roughness_texture) {
        uint32_t tslot;

        if (!le_resolve_asset_live(
                engine, &desc->metallic_roughness_texture, &tslot,
                &code)) {
            return (code == LE_ERROR_INVALID_ARGUMENT)
                       ? LE_ERROR_INVALID_ARGUMENT
                       : LE_ERROR_STALE_ASSET;
        }
        if (engine->assets[tslot].type != LE_ASSET_TEXTURE) {
            return LE_ERROR_WRONG_ASSET_TYPE;
        }
        if (engine->assets[tslot].state != LE_ASSET_READY) {
            return LE_ERROR_MISSING_ASSET;
        }
        mr_view = engine->assets[tslot].texture_view;
    }
    le_uuid_mint(engine, &id.hi, &id.lo);
    idx = le_asset_alloc(engine, LE_ASSET_MATERIAL, LE_ASSET_READY,
                         &id, NULL, &code, &handle);
    if (idx < 0) {
        return code;
    }
    memset(&mdesc, 0, sizeof(mdesc));
    mdesc.base_color_factor[0] = desc->base_color_factor[0];
    mdesc.base_color_factor[1] = desc->base_color_factor[1];
    mdesc.base_color_factor[2] = desc->base_color_factor[2];
    mdesc.base_color_factor[3] = desc->base_color_factor[3];
    mdesc.metallic_factor = desc->metallic_factor;
    mdesc.roughness_factor = desc->roughness_factor;
    mdesc.normal_scale = 1.0f;
    mdesc.occlusion_strength = 1.0f;
    mdesc.alpha_mode = LR_ALPHA_OPAQUE;
    mdesc.base_color_texture = base_view;
    mdesc.metallic_roughness_texture = mr_view;
    if (lr_material_create_pbr(engine->renderer, &mdesc, &material) !=
        LR_SUCCESS) {
        le_asset_abandon(engine, idx);
        return LE_ERROR_RENDERER;
    }
    engine->assets[idx].material = material;
    if (desc->has_base_color_texture) {
        engine->assets[idx].base_color_texture =
            desc->base_color_texture;
        engine->assets[idx].has_base_color_texture = 1;
    }
    if (desc->has_metallic_roughness_texture) {
        engine->assets[idx].metallic_roughness_texture =
            desc->metallic_roughness_texture;
        engine->assets[idx].has_metallic_roughness_texture = 1;
    }
    *out_asset = handle;
    return LE_SUCCESS;
}

le_result le_asset_create_texture(le_engine *engine,
                                  const le_texture_asset_desc *desc,
                                  le_asset *out_asset) {
    le_result code = LE_SUCCESS;
    int32_t idx;
    le_asset handle = LE_ASSET_INVALID;
    lc_image *image = NULL;
    lc_image_view *view = NULL;
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_image_upload_desc upload;
    le_asset_id id;
    lc_device *device;

    if (engine == NULL || desc == NULL || out_asset == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    *out_asset = LE_ASSET_INVALID;
    if (engine->renderer == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->rgba == NULL || desc->width == 0 || desc->height == 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if ((uint64_t)desc->width * (uint64_t)desc->height >
        (uint64_t)16384 * 16384u) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    device = lr_renderer_get_device(engine->renderer);
    if (device == NULL) {
        return LE_ERROR_RENDERER;
    }
    le_uuid_mint(engine, &id.hi, &id.lo);
    idx = le_asset_alloc(engine, LE_ASSET_TEXTURE, LE_ASSET_READY,
                         &id, NULL, &code, &handle);
    if (idx < 0) {
        return code;
    }
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = desc->srgb ? LC_FORMAT_RGBA8_SRGB
                              : LC_FORMAT_RGBA8_UNORM;
    idesc.width = desc->width;
    idesc.height = desc->height;
    idesc.depth = 1;
    idesc.mip_levels = 0;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_SRC |
                  LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &idesc, &image) != LC_SUCCESS) {
        le_asset_abandon(engine, idx);
        return LE_ERROR_RENDERER;
    }
    memset(&upload, 0, sizeof(upload));
    upload.mip_level = 0;
    upload.array_layer = 0;
    upload.width = desc->width;
    upload.height = desc->height;
    upload.depth = 1;
    upload.data = desc->rgba;
    upload.data_size =
        (uint64_t)desc->width * desc->height * 4u;
    if (lc_image_write(image, &upload) != LC_SUCCESS ||
        lc_image_generate_mipmaps(image) != LC_SUCCESS) {
        lc_image_destroy(image);
        le_asset_abandon(engine, idx);
        return LE_ERROR_RENDERER;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.base_mip_level = 0;
    vdesc.mip_level_count = lc_image_get_mip_levels(image);
    vdesc.base_array_layer = 0;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(image, &vdesc, &view) != LC_SUCCESS) {
        lc_image_destroy(image);
        le_asset_abandon(engine, idx);
        return LE_ERROR_RENDERER;
    }
    engine->assets[idx].texture_image = image;
    engine->assets[idx].texture_view = view;
    engine->assets[idx].texture_width = desc->width;
    engine->assets[idx].texture_height = desc->height;
    engine->assets[idx].texture_srgb = (desc->srgb != 0) ? 1 : 0;
    *out_asset = handle;
    return LE_SUCCESS;
}

/* ------------------------------------------------------------------
 * Unload + inspection.
 * ------------------------------------------------------------------ */

le_result le_asset_unload(le_engine *engine, const le_asset *asset) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_asset_slot *s;

    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (asset == NULL) {
        return LE_SUCCESS;
    }
    if (!le_asset_is_valid(asset)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_asset_live(engine, asset, &slot, &code)) {
        return code;
    }
    /* Referenced assets refuse unload (worlds unchanged). */
    if (le_asset_refcount(engine, slot) > 0) {
        return LE_ERROR_ASSET_IN_USE;
    }
    s = &engine->assets[slot];
    /* Destroy backing through public APIs (renderer retirement
     * handles GPU-side safety; the engine owns no sync). */
    if (s->type == LE_ASSET_MESH && s->mesh != NULL) {
        lr_mesh_destroy(s->mesh);
        s->mesh = NULL;
    } else if (s->type == LE_ASSET_MATERIAL && s->material != NULL) {
        lr_material_destroy(s->material);
        s->material = NULL;
    } else if (s->type == LE_ASSET_TEXTURE) {
        lc_image_view_destroy(s->texture_view);
        lc_image_destroy(s->texture_image);
        s->texture_view = NULL;
        s->texture_image = NULL;
    } else if (s->type == LE_ASSET_SCENE) {
        free(s->scene_objects);
        s->scene_objects = NULL;
        s->scene_count = 0;
        s->scene_capacity = 0;
    } else if (s->type == LE_ASSET_SCRIPT) {
        /* Chunk ref released BEFORE backend-state teardown
         * ordering matters only at engine shutdown; here the
         * runtime stays live, so unref normally. Script-source
         * bytes freed with the slot. */
        if (s->script_chunk_ref >= 0) {
            le_script_release_chunk_token(engine,
                                          s->script_chunk_ref);
        }
        s->script_chunk_ref = -2; /* LE_SCRIPT_NOREF */
        free(s->script_source);
        s->script_source = NULL;
        s->script_size = 0;
    } else if (s->type == LE_ASSET_SKELETON ||
               s->type == LE_ASSET_ANIMATION_CLIP) {
        /* Phase 29: animation assets are pure CPU data (no
         * renderer backing); free the immutable payload. */
        le_anim_free_slot_backing(s->skeleton, s->clip);
        s->skeleton = NULL;
        s->clip = NULL;
    } else if (s->type == LE_ASSET_PREFAB) {
        /* Phase 32: prefab payload is owned text (no backing). */
        free(s->prefab_text);
        s->prefab_text = NULL;
        s->prefab_size = 0;
    }
    if (s->source != NULL) {
        engine->name_bytes_assets -= strlen(s->source) + 1u;
        free(s->source);
        s->source = NULL;
    }
    if (s->state == LE_ASSET_READY && engine->asset_ready > 0) {
        engine->asset_ready--;
    } else if (s->state == LE_ASSET_FAILED &&
               engine->asset_failed > 0) {
        engine->asset_failed--;
    }
    switch (s->type) {
    case LE_ASSET_MESH:
        if (engine->asset_meshes > 0) {
            engine->asset_meshes--;
        }
        break;
    case LE_ASSET_MATERIAL:
        if (engine->asset_materials > 0) {
            engine->asset_materials--;
        }
        break;
    case LE_ASSET_TEXTURE:
        if (engine->asset_textures > 0) {
            engine->asset_textures--;
        }
        break;
    case LE_ASSET_SCENE:
        if (engine->asset_scenes > 0) {
            engine->asset_scenes--;
        }
        break;
    case LE_ASSET_SCRIPT:
        if (engine->asset_scripts > 0) {
            engine->asset_scripts--;
        }
        break;
    case LE_ASSET_SKELETON:
        if (engine->asset_skeletons > 0) {
            engine->asset_skeletons--;
        }
        break;
    case LE_ASSET_ANIMATION_CLIP:
        if (engine->asset_clips > 0) {
            engine->asset_clips--;
        }
        break;
    case LE_ASSET_PREFAB:
        if (engine->asset_prefabs > 0) {
            engine->asset_prefabs--;
        }
        break;
    default:
        break;
    }
    {
        uint32_t gen = s->generation + 1u;

        if (gen == 0) {
            gen = 1;
        }
        memset(s, 0, sizeof(*s));
        s->generation = gen;
    }
    s->free_next = engine->asset_free_head;
    engine->asset_free_head = (int32_t)slot;
    if (engine->asset_alive > 0) {
        engine->asset_alive--;
    }
    return LE_SUCCESS;
}

void le_asset_get_id(const le_engine *engine, const le_asset *asset,
                     le_asset_id *out_id) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_id != NULL) {
        memset(out_id, 0, sizeof(*out_id));
    }
    if (engine == NULL || asset == NULL || out_id == NULL) {
        return;
    }
    if (!le_resolve_asset_live(engine, asset, &slot, &code)) {
        return;
    }
    *out_id = engine->assets[slot].id;
}

le_asset_type le_asset_get_type(const le_engine *engine,
                                const le_asset *asset) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (engine == NULL || asset == NULL) {
        return LE_ASSET_COUNT;
    }
    if (!le_resolve_asset_live(engine, asset, &slot, &code)) {
        return LE_ASSET_COUNT;
    }
    return engine->assets[slot].type;
}

le_asset_state le_asset_get_state(const le_engine *engine,
                                  const le_asset *asset) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (engine == NULL || asset == NULL) {
        return LE_ASSET_UNLOADED;
    }
    if (!le_resolve_asset_live(engine, asset, &slot, &code)) {
        return LE_ASSET_UNLOADED;
    }
    return engine->assets[slot].state;
}

const char *le_asset_get_source(const le_engine *engine,
                                const le_asset *asset) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (engine == NULL || asset == NULL) {
        return "";
    }
    if (!le_resolve_asset_live(engine, asset, &slot, &code)) {
        return "";
    }
    if (engine->assets[slot].source == NULL) {
        return "";
    }
    return engine->assets[slot].source;
}

void le_engine_get_asset_stats(const le_engine *engine,
                               le_asset_stats *out_stats) {
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (engine == NULL) {
        return;
    }
    out_stats->assets_alive = engine->asset_alive;
    out_stats->asset_capacity = engine->asset_capacity;
    out_stats->mesh_count = engine->asset_meshes;
    out_stats->material_count = engine->asset_materials;
    out_stats->texture_count = engine->asset_textures;
    out_stats->scene_count = engine->asset_scenes;
    out_stats->skeleton_count = engine->asset_skeletons;
    out_stats->clip_count = engine->asset_clips;
    out_stats->prefab_count = engine->asset_prefabs;
    out_stats->ready_count = engine->asset_ready;
    out_stats->failed_count = engine->asset_failed;
    out_stats->name_bytes = engine->name_bytes_assets;
    /* le_asset_stats has no script counter field (Phase 25 shape);
     * script assets are observable via le_script_get_stats. */
}

void le_asset_get_info(const le_engine *engine, const le_asset *asset,
                       le_asset_info *out_info) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_info == NULL) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    out_info->type = LE_ASSET_COUNT;
    out_info->state = LE_ASSET_UNLOADED;
    out_info->source = "";
    if (engine == NULL || asset == NULL) {
        return;
    }
    if (!le_resolve_asset_live(engine, asset, &slot, &code)) {
        return;
    }
    {
        const le_asset_slot *s = &engine->assets[slot];

        out_info->alive = 1;
        out_info->type = s->type;
        out_info->state = s->state;
        out_info->id = s->id;
        out_info->source = (s->source != NULL) ? s->source : "";
        out_info->references = le_asset_refcount(engine, slot);
    }
}

/* Find a live READY asset by persistent ID (any type; 1 found).
 * Used by script bindings, tools, and the scene loaders. */
int le_asset_find_by_id(le_engine *engine, const le_asset_id *id,
                         le_asset *out_asset) {
    uint32_t i;

    if (out_asset != NULL) {
        *out_asset = LE_ASSET_INVALID;
    }
    if (engine == NULL || id == NULL) {
        return 0;
    }
    if (id->hi == 0 && id->lo == 0) {
        return 0;
    }
    for (i = 0; i < engine->asset_capacity; i++) {
        const le_asset_slot *s = &engine->assets[i];

        if (!s->alive || s->state != LE_ASSET_READY) {
            continue;
        }
        if (s->id.hi == id->hi && s->id.lo == id->lo) {
            if (out_asset != NULL) {
                out_asset->index = i;
                out_asset->generation = s->generation;
            }
            return 1;
        }
    }
    return 0;
}

lr_mesh *le_asset_get_mesh(const le_engine *engine,
                           const le_asset *asset) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (engine == NULL || asset == NULL) {
        return NULL;
    }
    if (!le_resolve_asset_live(engine, asset, &slot, &code)) {
        return NULL;
    }
    if (engine->assets[slot].type != LE_ASSET_MESH ||
        engine->assets[slot].state != LE_ASSET_READY) {
        return NULL;
    }
    return engine->assets[slot].mesh;
}

lr_material *le_asset_get_material(const le_engine *engine,
                                   const le_asset *asset) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (engine == NULL || asset == NULL) {
        return NULL;
    }
    if (!le_resolve_asset_live(engine, asset, &slot, &code)) {
        return NULL;
    }
    if (engine->assets[slot].type != LE_ASSET_MATERIAL ||
        engine->assets[slot].state != LE_ASSET_READY) {
        return NULL;
    }
    return engine->assets[slot].material;
}
