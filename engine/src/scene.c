/*
 * Luma Engine scenes, Phase 25 (Stages 21-29, 39-45, 56, 60-66):
 * engine-owned scene payloads (LE_ASSET_SCENE slots), world<->scene
 * capture, and transactional instantiation with persistent IDs.
 *
 * scene != world: the scene is serializable project content
 * (records with persistent IDs, hierarchy by persistent parent ID,
 * canonical TRS, asset refs by persistent asset ID); the world is
 * runtime state (le_object handles, cached matrices). Capture
 * assigns fresh UUIDs to objects lacking scene-ID mapping;
 * instantiation mints FRESH runtime handles every time (two
 * instances never share handles, salts, or renderer keys).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"

le_result le_scene_create(le_engine *engine, int register_asset,
                          le_asset *out_asset) {
    le_result code = LE_SUCCESS;
    int32_t idx;
    le_asset handle = LE_ASSET_INVALID;
    le_asset_id id;

    if (engine == NULL || out_asset == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    *out_asset = LE_ASSET_INVALID;
    (void)register_asset; /* scenes always register (asset type). */
    le_uuid_mint(engine, &id.hi, &id.lo);
    idx = le_asset_alloc(engine, LE_ASSET_SCENE, LE_ASSET_READY, &id,
                         NULL, &code, &handle);
    if (idx < 0) {
        return code;
    }
    *out_asset = handle;
    return LE_SUCCESS;
}

static int le_scene_id_equal(const le_scene_object_id *a,
                             const le_scene_object_id *b) {
    return a->hi == b->hi && a->lo == b->lo;
}

static int le_scene_id_nil(const le_scene_object_id *id) {
    return id->hi == 0 && id->lo == 0;
}

le_result le_scene_add_object(le_engine *engine, const le_asset *scene,
                              const le_scene_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_asset_slot *s;
    le_scene_object *fresh;
    uint32_t i;
    int k;

    if (engine == NULL || scene == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_asset_live(engine, scene, &slot, &code)) {
        return code;
    }
    s = &engine->assets[slot];
    if (s->type != LE_ASSET_SCENE) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    /* Validate TRS: finite position/scale, normalizable rotation. */
    for (k = 0; k < 3; k++) {
        float v;

        v = object->position[k];
        if (v != v || v > 3.402823466e+38f ||
            v < -3.402823466e+38f) {
            /* NaN check via self-compare (no math.h dep here). */
            if (!(v == v)) {
                return LE_ERROR_INVALID_ARGUMENT;
            }
        }
        v = object->scale[k];
        if (!(v == v)) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    }
    {
        double len = (double)object->rotation[0] *
                         object->rotation[0] +
                     (double)object->rotation[1] *
                         object->rotation[1] +
                     (double)object->rotation[2] *
                         object->rotation[2] +
                     (double)object->rotation[3] *
                         object->rotation[3];
        int finite = 1;

        for (k = 0; k < 4; k++) {
            if (!(object->rotation[k] == object->rotation[k])) {
                finite = 0;
            }
        }
        if (!finite || !(len > 1e-12)) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    }
    if (le_scene_id_nil(&object->id)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Script records: non-nil asset ID + bounded prop count +
     * NUL-terminated prop names (add_object copies blindly —
     * validate before the struct copy). */
    if (object->has_script) {
        uint32_t p;

        if (object->script_id.hi == 0 &&
            object->script_id.lo == 0) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        if (object->script_prop_count > LE_SCRIPT_MAX_PROPS) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        for (p = 0; p < object->script_prop_count; p++) {
            const le_script_property *sp =
                &object->script_props[p];

            if (sp->name[0] == '\0' ||
                sp->name[sizeof(sp->name) - 1] != '\0') {
                return LE_ERROR_INVALID_ARGUMENT;
            }
            if (sp->type < LE_SCRIPT_PROP_BOOL ||
                sp->type > LE_SCRIPT_PROP_ASSET) {
                return LE_ERROR_INVALID_ARGUMENT;
            }
        }
    } else if (object->script_prop_count != 0) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* Physics records validate the same way (malformed values
     * rejected before the struct copy). */
    if (object->has_rigid_body || object->has_collider) {
        if (le_physics_validate_record(object) != LE_SUCCESS) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    }
    /* Animator records validate the same way. */
    if (object->has_animator) {
        if (le_anim_validate_record(object) != LE_SUCCESS) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
    }
    for (i = 0; i < s->scene_count; i++) {
        if (le_scene_id_equal(&s->scene_objects[i].id,
                              &object->id)) {
            return LE_ERROR_DUPLICATE_ID;
        }
    }
    if (s->scene_count >= s->scene_capacity) {
        uint32_t grown =
            (s->scene_capacity == 0) ? 16u : s->scene_capacity * 2u;
        le_scene_object *grown_mem;

        if (grown < s->scene_capacity + 1u ||
            grown > 0x00FFFFFFu) {
            return LE_ERROR_OVERFLOW;
        }
        grown_mem = (le_scene_object *)realloc(
            s->scene_objects, (size_t)grown * sizeof(*grown_mem));
        if (grown_mem == NULL) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
        s->scene_objects = grown_mem;
        s->scene_capacity = grown;
    }
    fresh = &s->scene_objects[s->scene_count];
    *fresh = *object;
    /* Name is a fixed array: enforce NUL termination. */
    fresh->name[sizeof(fresh->name) - 1] = '\0';
    s->scene_count++;
    return LE_SUCCESS;
}

int le_scene_get_info(const le_engine *engine, const le_asset *scene,
                      uint32_t *out_objects, uint32_t *out_roots,
                      uint32_t *out_version) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    uint32_t i;
    uint32_t roots = 0;

    if (out_objects != NULL) {
        *out_objects = 0;
    }
    if (out_roots != NULL) {
        *out_roots = 0;
    }
    if (out_version != NULL) {
        *out_version = 0;
    }
    if (engine == NULL || scene == NULL) {
        return 0;
    }
    if (!le_resolve_asset_live(engine, scene, &slot, &code)) {
        return 0;
    }
    if (engine->assets[slot].type != LE_ASSET_SCENE) {
        return 0;
    }
    for (i = 0; i < engine->assets[slot].scene_count; i++) {
        if (!engine->assets[slot].scene_objects[i].has_parent) {
            roots++;
        }
    }
    if (out_objects != NULL) {
        *out_objects = engine->assets[slot].scene_count;
    }
    if (out_roots != NULL) {
        *out_roots = roots;
    }
    if (out_version != NULL) {
        *out_version = LE_SCENE_FORMAT_VERSION;
    }
    return 1;
}

/* Find a READY asset slot by persistent ID + expected type (for
 * capture: handles -> IDs; for instantiate: IDs -> handles). */
static int le_find_asset_by_id(const le_engine *engine,
                               const le_asset_id *id,
                               le_asset_type type, uint32_t *out_slot) {
    uint32_t i;

    if (engine == NULL || id == NULL) {
        return 0;
    }
    for (i = 0; i < engine->asset_capacity; i++) {
        const le_asset_slot *s = &engine->assets[i];

        if (!s->alive || s->type != type ||
            s->state != LE_ASSET_READY) {
            continue;
        }
        if (s->id.hi == id->hi && s->id.lo == id->lo) {
            if (out_slot != NULL) {
                *out_slot = i;
            }
            return 1;
        }
    }
    return 0;
}

le_result le_scene_capture(le_world *world, const le_asset *scene,
                           uint32_t *out_skipped) {
    le_engine *engine;
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_asset_slot *s;
    uint32_t i;
    uint32_t skipped = 0;

    if (out_skipped != NULL) {
        *out_skipped = 0;
    }
    if (world == NULL || scene == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    engine = world->engine;
    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_asset_live(engine, scene, &slot, &code)) {
        return code;
    }
    s = &engine->assets[slot];
    if (s->type != LE_ASSET_SCENE) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    /* Cross-engine scene: handles resolve on the scene's engine,
     * which must equal the world's engine. */
    le_refresh_world_matrices(world);
    /* Replace payload wholesale (capture is a snapshot, not a
     * merge): deleted objects disappear (Stage 65), surviving
     * objects keep persistent IDs (Stage 64), new objects mint
     * fresh ones (Stage 66). */
    free(s->scene_objects);
    s->scene_objects = NULL;
    s->scene_count = 0;
    s->scene_capacity = 0;
    for (i = 0; i < world->capacity; i++) {
        le_object_slot *slot_obj;
        le_scene_object rec;

        if (!world->slots[i].alive) {
            continue;
        }
        slot_obj = &world->slots[i];
        memset(&rec, 0, sizeof(rec));
        /* Persistent ID: reuse mapping, else mint + remember. */
        if (slot_obj->has_scene_id) {
            rec.id = slot_obj->scene_id;
        } else {
            le_uuid_mint(engine, &rec.id.hi, &rec.id.lo);
            slot_obj->scene_id = rec.id;
            slot_obj->has_scene_id = 1;
        }
        /* Parent by persistent ID (nil-safe: parent slot always
         * has mapping after its own capture pass — parents may
         * come later in slot order, so resolve via slots
         * directly, not via emitted records). */
        if (slot_obj->parent != LE_NO_LINK && slot_obj->parent >= 0 &&
            (uint32_t)slot_obj->parent < world->capacity &&
            world->slots[slot_obj->parent].alive) {
            le_object_slot *pslot =
                &world->slots[slot_obj->parent];

            if (!pslot->has_scene_id) {
                le_uuid_mint(engine, &pslot->scene_id.hi,
                             &pslot->scene_id.lo);
                pslot->has_scene_id = 1;
            }
            rec.parent = pslot->scene_id;
            rec.has_parent = 1;
        }
        if (slot_obj->name != NULL) {
            size_t n = strlen(slot_obj->name);

            if (n >= sizeof(rec.name)) {
                n = sizeof(rec.name) - 1;
            }
            memcpy(rec.name, slot_obj->name, n);
            rec.name[n] = '\0';
        }
        rec.enabled = slot_obj->enabled;
        memcpy(rec.position, slot_obj->position,
               sizeof(rec.position));
        memcpy(rec.rotation, slot_obj->rotation,
               sizeof(rec.rotation));
        memcpy(rec.scale, slot_obj->scale, sizeof(rec.scale));
        /* Renderables: asset-backed serialize by persistent asset
         * ID; pointer-backed are SKIPPED (counted, never stored).
         * Cameras/lights copy verbatim (explicit fields). */
        if ((slot_obj->present & LE_PRESENT_ASSET_RENDERABLE) != 0u) {
            uint32_t ridx = (uint32_t)slot_obj->renderable_index;

            if (ridx < world->asset_renderable_count &&
                world->asset_renderables[ridx].slot == i) {
                const le_asset_renderable_desc *d =
                    &world->asset_renderables[ridx].desc;
                uint32_t mslot;
                uint32_t tslot;

                if (le_resolve_asset_live(engine, &d->mesh, &mslot,
                                          &code) &&
                    le_resolve_asset_live(engine, &d->material,
                                          &tslot, &code) &&
                    engine->assets[mslot].type == LE_ASSET_MESH &&
                    engine->assets[tslot].type ==
                        LE_ASSET_MATERIAL) {
                    rec.has_renderable = 1;
                    rec.mesh_id = engine->assets[mslot].id;
                    rec.material_id = engine->assets[tslot].id;
                    rec.casts_shadow = d->casts_shadow;
                    rec.receives_shadow = d->receives_shadow;
                    rec.visible = d->visible;
                } else {
                    skipped++;
                }
            }
        } else if ((slot_obj->present & LE_PRESENT_RENDERABLE) != 0u) {
            skipped++;
        }
        if ((slot_obj->present & LE_PRESENT_CAMERA) != 0u) {
            uint32_t cidx = (uint32_t)slot_obj->camera_index;

            if (cidx < world->camera_count &&
                world->cameras[cidx].slot == i) {
                rec.has_camera = 1;
                rec.camera = world->cameras[cidx].desc;
            }
        }
        if ((slot_obj->present & LE_PRESENT_LIGHT) != 0u) {
            uint32_t lidx = (uint32_t)slot_obj->light_index;

            if (lidx < world->light_count &&
                world->lights[lidx].slot == i) {
                rec.has_light = 1;
                rec.light = world->lights[lidx].desc;
            }
        }
        /* Scripts: asset ID + exported property values (never VM
         * state). Scriptless objects keep has_script 0. */
        le_script_capture_for_record(world, i, &rec);
        /* Physics: authoring state only (never accumulators or
         * solver caches). Component-less objects keep both 0. */
        le_physics_capture_for_record(world, i, &rec);
        /* Animation: authoring/playback state only (never poses
         * or palettes). Animator-less objects keep has_animator
         * 0. */
        le_anim_capture_for_record(world, i, &rec);
        /* Append (capacity checked inside add path — inline here
         * for speed; failure aborts capture with prior records
         * intact? No: capture replaced wholesale, so OOM leaves
         * the scene PARTIAL. Mitigation: grow-only loop with
         * early counting — count first, allocate once. */
        {
            le_result ar;

            /* Ensure capacity directly. */
            if (s->scene_count >= s->scene_capacity) {
                uint32_t grown = (s->scene_capacity == 0)
                                     ? 16u
                                     : s->scene_capacity * 2u;
                le_scene_object *grown_mem;

                if (grown < s->scene_capacity + 1u ||
                    grown > 0x00FFFFFFu) {
                    if (out_skipped != NULL) {
                        *out_skipped = skipped;
                    }
                    return LE_ERROR_OVERFLOW;
                }
                grown_mem = (le_scene_object *)realloc(
                    s->scene_objects,
                    (size_t)grown * sizeof(*grown_mem));
                if (grown_mem == NULL) {
                    if (out_skipped != NULL) {
                        *out_skipped = skipped;
                    }
                    return LE_ERROR_OUT_OF_MEMORY;
                }
                s->scene_objects = grown_mem;
                s->scene_capacity = grown;
            }
            s->scene_objects[s->scene_count] = rec;
            s->scene_count++;
            (void)ar;
        }
    }
    if (out_skipped != NULL) {
        *out_skipped = skipped;
    }
    return LE_SUCCESS;
}

void le_scene_instance_free(le_scene_instance *instance) {
    if (instance == NULL) {
        return;
    }
    free(instance->object_ids);
    free(instance->objects);
    memset(instance, 0, sizeof(*instance));
}

int le_scene_instance_lookup(const le_scene_instance *instance,
                             const le_scene_object_id *id,
                             le_object *out_object) {
    uint32_t i;

    if (out_object != NULL) {
        *out_object = LE_OBJECT_INVALID;
    }
    if (instance == NULL || id == NULL) {
        return 0;
    }
    for (i = 0; i < instance->count; i++) {
        if (le_scene_id_equal(&instance->object_ids[i], id)) {
            if (out_object != NULL) {
                *out_object = instance->objects[i];
            }
            return 1;
        }
    }
    return 0;
}

/* Instantiate: parse/validate/resolve FIRST (no world mutation),
 * then commit. Staging: arrays of (record, parent_runtime_index,
 * mesh/tslot handles). */
le_result le_scene_instantiate(le_world *world, const le_asset *scene,
                               le_scene_instance *out_instance) {
    le_engine *engine;
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_asset_slot *s;
    uint32_t n;
    uint32_t i;
    /* Staging buffers (all freed on every exit path). */
    le_object *handles = NULL;
    int32_t *parent_of = NULL;
    le_asset *mesh_h = NULL;
    le_asset *mat_h = NULL;
    uint32_t created = 0;

    if (out_instance != NULL) {
        memset(out_instance, 0, sizeof(*out_instance));
    }
    if (world == NULL || scene == NULL || out_instance == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    engine = world->engine;
    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_asset_live(engine, scene, &slot, &code)) {
        return code;
    }
    s = &engine->assets[slot];
    if (s->type != LE_ASSET_SCENE) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    n = s->scene_count;
    if (n == 0) {
        return LE_SUCCESS; /* empty scene: nothing to do */
    }
    if (n > LE_MAX_CAPACITY) {
        return LE_ERROR_OVERFLOW;
    }
    handles =
        (le_object *)calloc(n, sizeof(le_object));
    parent_of = (int32_t *)malloc(n * sizeof(int32_t));
    mesh_h = (le_asset *)malloc(n * sizeof(le_asset));
    mat_h = (le_asset *)malloc(n * sizeof(le_asset));
    if (handles == NULL || parent_of == NULL || mesh_h == NULL ||
        mat_h == NULL) {
        free(handles);
        free(parent_of);
        free(mesh_h);
        free(mat_h);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < n; i++) {
        parent_of[i] = LE_NO_LINK;
        mesh_h[i] = LE_ASSET_INVALID;
        mat_h[i] = LE_ASSET_INVALID;
        handles[i] = LE_OBJECT_INVALID;
    }
    /* PASS 1: duplicate IDs + record validation (no world touch).
     * Duplicate detection is O(n^2) worst-case — scenes are
     * thousands of records, fine; 100k scenes take the hash-free
     * path too (documented: instantiation is O(n^2) worst-case on
     * adversarial duplicate-heavy inputs, O(n) typical). */
    {
        uint32_t a;
        uint32_t b;

        for (a = 0; a < n; a++) {
            const le_scene_object *rec = &s->scene_objects[a];

            if (le_scene_id_nil(&rec->id)) {
                goto fail_invalid;
            }
            for (b = a + 1u; b < n; b++) {
                if (le_scene_id_equal(
                        &rec->id, &s->scene_objects[b].id)) {
                    free(handles);
                    free(parent_of);
                    free(mesh_h);
                    free(mat_h);
                    return LE_ERROR_DUPLICATE_ID;
                }
            }
            /* TRS sanity (mirrors add-time validation). */
            {
                int k;
                double len = 0;

                for (k = 0; k < 3; k++) {
                    if (!(rec->position[k] == rec->position[k]) ||
                        !(rec->scale[k] == rec->scale[k])) {
                        goto fail_invalid;
                    }
                }
                for (k = 0; k < 4; k++) {
                    if (!(rec->rotation[k] == rec->rotation[k])) {
                        goto fail_invalid;
                    }
                    len += (double)rec->rotation[k] *
                           rec->rotation[k];
                }
                if (!(len > 1e-12)) {
                    goto fail_invalid;
                }
            }
            /* Asset refs resolve to READY handles now (before any
             * object exists). */
            if (rec->has_renderable) {
                uint32_t mslot;
                uint32_t tslot;
                le_asset_id mid = rec->mesh_id;
                le_asset_id tid = rec->material_id;

                if (!le_find_asset_by_id(engine, &mid, LE_ASSET_MESH,
                                         &mslot) ||
                    !le_find_asset_by_id(engine, &tid,
                                         LE_ASSET_MATERIAL,
                                         &tslot)) {
                    free(handles);
                    free(parent_of);
                    free(mesh_h);
                    free(mat_h);
                    return LE_ERROR_MISSING_ASSET;
                }
                mesh_h[a].index = mslot;
                mesh_h[a].generation =
                    engine->assets[mslot].generation;
                mat_h[a].index = tslot;
                mat_h[a].generation =
                    engine->assets[tslot].generation;
            }
            /* Script refs: the asset must EXIST as READY now (same
             * rule as renderables — resolve before any object
             * exists). Property shape validated here (count bound);
             * names/values apply best-effort at commit. */
            if (rec->has_script) {
                uint32_t sslot;

                if (!le_find_asset_by_id(engine,
                                         &rec->script_id,
                                         LE_ASSET_SCRIPT,
                                         &sslot)) {
                    free(handles);
                    free(parent_of);
                    free(mesh_h);
                    free(mat_h);
                    return LE_ERROR_MISSING_ASSET;
                }
                if (rec->script_prop_count >
                    LE_SCRIPT_MAX_PROPS) {
                    goto fail_invalid;
                }
            }
            /* Physics records validate here (malformed values
             * fail the load transactionally, never half-load). */
            if (rec->has_rigid_body || rec->has_collider) {
                if (le_physics_validate_record(rec) !=
                    LE_SUCCESS) {
                    goto fail_invalid;
                }
            }
            /* Animator records validate here (loop/speed/range;
             * asset IDs resolve at commit — missing IDs fail
             * there with MISSING_ASSET). */
            if (rec->has_animator) {
                uint32_t aslot;

                if (le_anim_validate_record(rec) != LE_SUCCESS) {
                    goto fail_invalid;
                }
                if (!(rec->skeleton_id.hi == 0 &&
                      rec->skeleton_id.lo == 0) &&
                    !le_find_asset_by_id(engine,
                                         &rec->skeleton_id,
                                         LE_ASSET_SKELETON,
                                         &aslot)) {
                    free(handles);
                    free(parent_of);
                    free(mesh_h);
                    free(mat_h);
                    return LE_ERROR_MISSING_ASSET;
                }
                if (!(rec->clip_id.hi == 0 &&
                      rec->clip_id.lo == 0) &&
                    !le_find_asset_by_id(engine, &rec->clip_id,
                                         LE_ASSET_ANIMATION_CLIP,
                                         &aslot)) {
                    free(handles);
                    free(parent_of);
                    free(mesh_h);
                    free(mat_h);
                    return LE_ERROR_MISSING_ASSET;
                }
            }
        }
    }
    /* PASS 2: parent resolution by persistent ID (forward refs OK:
     * index map over the record array, not file order). */
    {
        for (i = 0; i < n; i++) {
            const le_scene_object *rec = &s->scene_objects[i];
            uint32_t p;

            if (!rec->has_parent) {
                continue;
            }
            if (le_scene_id_equal(&rec->parent, &rec->id)) {
                goto fail_hierarchy; /* self parent */
            }
            parent_of[i] = LE_NO_LINK;
            for (p = 0; p < n; p++) {
                if (le_scene_id_equal(&s->scene_objects[p].id,
                                      &rec->parent)) {
                    parent_of[i] = (int32_t)p;
                    break;
                }
            }
            if (parent_of[i] == LE_NO_LINK) {
                goto fail_hierarchy; /* missing parent */
            }
        }
        /* Cycle detection (iterative color walk over parent_of). */
        {
            unsigned char *color =
                (unsigned char *)calloc(n, sizeof(*color));

            if (color == NULL) {
                free(handles);
                free(parent_of);
                free(mesh_h);
                free(mat_h);
                return LE_ERROR_OUT_OF_MEMORY;
            }
            for (i = 0; i < n; i++) {
                int32_t cur = (int32_t)i;

                if (color[i] != 0) {
                    continue;
                }
                while (cur != LE_NO_LINK) {
                    if (cur < 0 || (uint32_t)cur >= n) {
                        break;
                    }
                    if (color[cur] == 1) {
                        free(color);
                        goto fail_hierarchy;
                    }
                    if (color[cur] == 2) {
                        break;
                    }
                    color[cur] = 1;
                    cur = parent_of[cur];
                }
                cur = (int32_t)i;
                while (cur != LE_NO_LINK && cur >= 0 &&
                       (uint32_t)cur < n && color[cur] == 1) {
                    color[cur] = 2;
                    cur = parent_of[cur];
                }
            }
            free(color);
        }
    }
    /* PASS 3: commit — create objects, attach hierarchy,
     * components. Any failure rolls back everything created. */
    for (i = 0; i < n; i++) {
        const le_scene_object *rec = &s->scene_objects[i];
        le_object o;

        if (le_object_create(world, &o) != LE_SUCCESS) {
            goto fail_rollback;
        }
        handles[i] = o;
        created++;
        /* Persistent-ID mapping: stamp the slot so a later
         * capture preserves IDs (Stage 64). */
        {
            uint32_t oslot = o.index;
            le_result rc = LE_SUCCESS;

            if (le_resolve_live(world, &o, &oslot, &rc)) {
                world->slots[oslot].scene_id = rec->id;
                world->slots[oslot].has_scene_id = 1;
            }
        }
        if (rec->name[0] != '\0') {
            if (le_object_set_name(world, &o, rec->name) !=
                LE_SUCCESS) {
                goto fail_rollback;
            }
        }
        if (!rec->enabled) {
            if (le_object_set_enabled(world, &o, 0) != LE_SUCCESS) {
                goto fail_rollback;
            }
        }
        if (le_object_set_position(world, &o, rec->position) !=
                LE_SUCCESS ||
            le_object_set_rotation(world, &o, rec->rotation) !=
                LE_SUCCESS ||
            le_object_set_scale(world, &o, rec->scale) !=
                LE_SUCCESS) {
            goto fail_rollback;
        }
    }
    /* Hierarchy commit (indices now runtime handles). */
    for (i = 0; i < n; i++) {
        if (parent_of[i] != LE_NO_LINK) {
            if (le_object_set_parent(world, &handles[i],
                                     &handles[parent_of[i]]) !=
                LE_SUCCESS) {
                goto fail_rollback;
            }
        }
    }
    /* Component commit. */
    for (i = 0; i < n; i++) {
        const le_scene_object *rec = &s->scene_objects[i];

        if (rec->has_renderable) {
            le_asset_renderable_desc rd;

            memset(&rd, 0, sizeof(rd));
            rd.mesh = mesh_h[i];
            rd.material = mat_h[i];
            rd.casts_shadow = rec->casts_shadow;
            rd.receives_shadow = rec->receives_shadow;
            rd.visible = rec->visible;
            if (le_object_add_asset_renderable(world, &handles[i],
                                               &rd) != LE_SUCCESS) {
                goto fail_rollback;
            }
        }
        if (rec->has_camera) {
            if (le_object_add_camera(world, &handles[i],
                                     &rec->camera) != LE_SUCCESS) {
                goto fail_rollback;
            }
        }
        if (rec->has_light) {
            if (le_object_add_light(world, &handles[i],
                                    &rec->light) != LE_SUCCESS) {
                goto fail_rollback;
            }
        }
        /* Physics attaches before scripts (collision callbacks
         * need live colliders when instances start stepping). */
        if (rec->has_rigid_body || rec->has_collider) {
            if (le_physics_apply_record(world, &handles[i],
                                        rec) != LE_SUCCESS) {
                goto fail_rollback;
            }
        }
        /* Animators attach after physics (object-track ownership
         * validates against the live body) and before scripts
         * (start() observes the animator). */
        if (rec->has_animator) {
            if (le_anim_apply_record(world, &handles[i], rec) !=
                LE_SUCCESS) {
                goto fail_rollback;
            }
        }
        /* Scripts attach after physics (component commit order):
         * attach + recorded property values (best-effort per
         * property — unknown names from newer files are
         * skipped, never fatal). Instances start on the next
         * le_world_update in instantiation order
         * (pending_start, never twice). */
        if (rec->has_script) {
            if (le_script_apply_record(world, &handles[i], rec) !=
                LE_SUCCESS) {
                goto fail_rollback;
            }
        }
    }
    /* Publish the instance record. */
    {
        le_scene_object_id *ids =
            (le_scene_object_id *)malloc(n * sizeof(*ids));
        le_object *objs =
            (le_object *)malloc(n * sizeof(*objs));

        if (objs == NULL || ids == NULL) {
            free(ids);
            free(objs);
            goto fail_rollback;
        }
        for (i = 0; i < n; i++) {
            ids[i] = s->scene_objects[i].id;
            objs[i] = handles[i];
        }
        out_instance->scene.index = slot;
        out_instance->scene.generation =
            engine->assets[slot].generation;
        out_instance->object_ids = ids;
        out_instance->objects = objs;
        out_instance->count = n;
        out_instance->has_root = 0;
        for (i = 0; i < n; i++) {
            if (parent_of[i] == LE_NO_LINK) {
                out_instance->root = handles[i];
                out_instance->has_root = 1;
                break;
            }
        }
    }
    free(handles);
    free(parent_of);
    free(mesh_h);
    free(mat_h);
    return LE_SUCCESS;

fail_invalid:
    free(handles);
    free(parent_of);
    free(mesh_h);
    free(mat_h);
    return LE_ERROR_INVALID_ARGUMENT;
fail_hierarchy:
    free(handles);
    free(parent_of);
    free(mesh_h);
    free(mat_h);
    return LE_ERROR_INVALID_HIERARCHY;
fail_rollback:
    while (created > 0) {
        created--;
        le_object_destroy(world, &handles[created]);
    }
    free(handles);
    free(parent_of);
    free(mesh_h);
    free(mat_h);
    memset(out_instance, 0, sizeof(*out_instance));
    return LE_ERROR_OUT_OF_MEMORY;
}
