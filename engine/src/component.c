/*
 * Luma Engine components: presence queries, renderable / camera /
 * light add/remove/get, active-camera designation.
 *
 * Storage: dense per-type arrays with slot<->entry index maps and
 * swap-remove (O(1) add/remove, deterministic ascending-slot
 * iteration by scanning slots — never by dense order, which churns
 * on removal). Component arrays grow geometrically; growth
 * allocates first and swaps in on success (world unchanged on OOM).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"

int le_object_has_component(const le_world *world, const le_object *object,
                            le_component_type type) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    switch (type) {
    case LE_COMPONENT_TRANSFORM:
        return 1;
    case LE_COMPONENT_RENDERABLE:
        return ((world->slots[slot].present & LE_PRESENT_RENDERABLE) !=
                    0u) ||
               ((world->slots[slot].present &
                 LE_PRESENT_ASSET_RENDERABLE) != 0u);
    case LE_COMPONENT_CAMERA:
        return (world->slots[slot].present & LE_PRESENT_CAMERA) != 0u;
    case LE_COMPONENT_LIGHT:
        return (world->slots[slot].present & LE_PRESENT_LIGHT) != 0u;
    case LE_COMPONENT_SCRIPT:
        return (world->slots[slot].present & LE_PRESENT_SCRIPT) !=
               0u;
    case LE_COMPONENT_RIGID_BODY:
        return (world->slots[slot].present & LE_PRESENT_RIGID_BODY) !=
               0u;
    case LE_COMPONENT_COLLIDER:
        return (world->slots[slot].present & LE_PRESENT_COLLIDER) !=
               0u;
    case LE_COMPONENT_ANIMATOR:
        return (world->slots[slot].present & LE_PRESENT_ANIMATOR) !=
               0u;
    default:
        return 0;
    }
}

static le_result le_grow_renderables(le_world *world) {
    uint32_t grown;
    le_renderable_entry *fresh;

    if (world->renderable_count < world->renderable_capacity) {
        return LE_SUCCESS;
    }
    grown = (world->renderable_capacity == 0) ? 64u
                                             : world->renderable_capacity * 2u;
    if (grown < world->renderable_capacity + 1u) {
        return LE_ERROR_OVERFLOW;
    }
    if (grown > LE_MAX_CAPACITY) {
        return LE_ERROR_OVERFLOW;
    }
    fresh = (le_renderable_entry *)realloc(world->renderables,
                                           (size_t)grown *
                                               sizeof(*fresh));
    if (fresh == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    world->renderables = fresh;
    world->renderable_capacity = grown;
    return LE_SUCCESS;
}

static le_result le_grow_asset_renderables(le_world *world) {
    uint32_t grown;
    le_asset_renderable_entry *fresh;

    if (world->asset_renderable_count <
        world->asset_renderable_capacity) {
        return LE_SUCCESS;
    }
    grown = (world->asset_renderable_capacity == 0)
                ? 64u
                : world->asset_renderable_capacity * 2u;
    if (grown < world->asset_renderable_capacity + 1u) {
        return LE_ERROR_OVERFLOW;
    }
    if (grown > LE_MAX_CAPACITY) {
        return LE_ERROR_OVERFLOW;
    }
    fresh = (le_asset_renderable_entry *)realloc(
        world->asset_renderables, (size_t)grown * sizeof(*fresh));
    if (fresh == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    world->asset_renderables = fresh;
    world->asset_renderable_capacity = grown;
    return LE_SUCCESS;
}

static le_result le_grow_cameras(le_world *world) {
    uint32_t grown;
    le_camera_entry *fresh;

    if (world->camera_count < world->camera_capacity) {
        return LE_SUCCESS;
    }
    grown = (world->camera_capacity == 0) ? 16u
                                          : world->camera_capacity * 2u;
    if (grown < world->camera_capacity + 1u) {
        return LE_ERROR_OVERFLOW;
    }
    if (grown > 65536u) {
        return LE_ERROR_OVERFLOW;
    }
    fresh = (le_camera_entry *)realloc(world->cameras,
                                       (size_t)grown * sizeof(*fresh));
    if (fresh == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    world->cameras = fresh;
    world->camera_capacity = grown;
    return LE_SUCCESS;
}

static le_result le_grow_lights(le_world *world) {
    uint32_t grown;
    le_light_entry *fresh;

    if (world->light_count < world->light_capacity) {
        return LE_SUCCESS;
    }
    grown = (world->light_capacity == 0) ? 16u
                                         : world->light_capacity * 2u;
    if (grown < world->light_capacity + 1u) {
        return LE_ERROR_OVERFLOW;
    }
    if (grown > LR_MAX_LIGHTS * 64u) {
        return LE_ERROR_OVERFLOW;
    }
    fresh = (le_light_entry *)realloc(world->lights,
                                      (size_t)grown * sizeof(*fresh));
    if (fresh == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    world->lights = fresh;
    world->light_capacity = grown;
    return LE_SUCCESS;
}

le_result le_object_add_renderable(le_world *world, const le_object *object,
                                   const le_renderable_desc *desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_object_slot *s;
    le_result grow;

    if (world == NULL || object == NULL || desc == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (desc->mesh == NULL || desc->material == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    s = &world->slots[slot];
    /* One renderable spelling per object: adding pointer-backed
     * removes an asset-backed renderable first. */
    if ((s->present & LE_PRESENT_ASSET_RENDERABLE) != 0u) {
        le_object_remove_asset_renderable(world, object);
    }
    if ((s->present & LE_PRESENT_RENDERABLE) != 0u) {
        /* Replace in place (no growth, no counter change). */
        uint32_t idx = (uint32_t)s->renderable_index;

        if (idx >= world->renderable_count) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        world->renderables[idx].desc = *desc;
        return LE_SUCCESS;
    }
    grow = le_grow_renderables(world);
    if (grow != LE_SUCCESS) {
        return grow;
    }
    world->renderables[world->renderable_count].slot = slot;
    world->renderables[world->renderable_count].desc = *desc;
    s->renderable_index = (int32_t)world->renderable_count;
    s->present |= LE_PRESENT_RENDERABLE;
    world->renderable_count++;
    return LE_SUCCESS;
}

le_result le_object_remove_renderable(le_world *world,
                                      const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_object_slot *s;
    uint32_t idx;
    uint32_t last;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    s = &world->slots[slot];
    if ((s->present & LE_PRESENT_RENDERABLE) == 0u) {
        return LE_SUCCESS;
    }
    idx = (uint32_t)s->renderable_index;
    if (idx >= world->renderable_count) {
        s->present &= ~LE_PRESENT_RENDERABLE;
        s->renderable_index = LE_NO_LINK;
        return LE_SUCCESS;
    }
    last = world->renderable_count - 1u;
    if (idx != last) {
        world->renderables[idx] = world->renderables[last];
        world->slots[world->renderables[idx].slot].renderable_index =
            (int32_t)idx;
    }
    world->renderable_count--;
    s->present &= ~LE_PRESENT_RENDERABLE;
    s->renderable_index = LE_NO_LINK;
    return LE_SUCCESS;
}

int le_object_get_renderable(const le_world *world, const le_object *object,
                             le_renderable_desc *out_desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_desc != NULL) {
        memset(out_desc, 0, sizeof(*out_desc));
    }
    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    if ((world->slots[slot].present & LE_PRESENT_RENDERABLE) == 0u) {
        return 0;
    }
    {
        uint32_t idx = (uint32_t)world->slots[slot].renderable_index;

        if (idx >= world->renderable_count) {
            return 0;
        }
        if (world->renderables[idx].slot != slot) {
            return 0;
        }
        if (out_desc != NULL) {
            *out_desc = world->renderables[idx].desc;
        }
        return 1;
    }
}

void le_camera_desc_default(le_camera_desc *desc) {
    if (desc == NULL) {
        return;
    }
    memset(desc, 0, sizeof(*desc));
    desc->projection = LE_PROJECTION_PERSPECTIVE;
    desc->fov_y_rad = 1.0471976f; /* 60 degrees */
    desc->ortho_height = 10.0f;
    desc->aspect = 16.0f / 9.0f;
    desc->near_plane = 0.1f;
    desc->far_plane = 1000.0f;
}

static int le_camera_desc_valid(const le_camera_desc *desc) {
    if (desc == NULL) {
        return 0;
    }
    if (desc->projection != LE_PROJECTION_PERSPECTIVE &&
        desc->projection != LE_PROJECTION_ORTHOGRAPHIC) {
        return 0;
    }
    if (!(desc->aspect > 0.0f) || !isfinite(desc->aspect)) {
        return 0;
    }
    if (!(desc->near_plane > 0.0f) || !isfinite(desc->near_plane)) {
        return 0;
    }
    if (!(desc->far_plane > desc->near_plane) ||
        !isfinite(desc->far_plane)) {
        return 0;
    }
    if (desc->projection == LE_PROJECTION_PERSPECTIVE) {
        if (!(desc->fov_y_rad > 0.0f) ||
            !(desc->fov_y_rad < 3.14159265358979323846f) ||
            !isfinite(desc->fov_y_rad)) {
            return 0;
        }
    } else {
        if (!(desc->ortho_height > 0.0f) ||
            !isfinite(desc->ortho_height)) {
            return 0;
        }
    }
    return 1;
}

le_result le_object_add_camera(le_world *world, const le_object *object,
                               const le_camera_desc *desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_object_slot *s;
    le_result grow;

    if (world == NULL || object == NULL || desc == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_camera_desc_valid(desc)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    s = &world->slots[slot];
    if ((s->present & LE_PRESENT_CAMERA) != 0u) {
        uint32_t idx = (uint32_t)s->camera_index;

        if (idx >= world->camera_count) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        world->cameras[idx].desc = *desc;
        return LE_SUCCESS;
    }
    grow = le_grow_cameras(world);
    if (grow != LE_SUCCESS) {
        return grow;
    }
    world->cameras[world->camera_count].slot = slot;
    world->cameras[world->camera_count].desc = *desc;
    s->camera_index = (int32_t)world->camera_count;
    s->present |= LE_PRESENT_CAMERA;
    world->camera_count++;
    return LE_SUCCESS;
}

le_result le_object_remove_camera(le_world *world, const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_object_slot *s;
    uint32_t idx;
    uint32_t last;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    s = &world->slots[slot];
    if ((s->present & LE_PRESENT_CAMERA) == 0u) {
        return LE_SUCCESS;
    }
    idx = (uint32_t)s->camera_index;
    if (idx < world->camera_count) {
        last = world->camera_count - 1u;
        if (idx != last) {
            world->cameras[idx] = world->cameras[last];
            world->slots[world->cameras[idx].slot].camera_index =
                (int32_t)idx;
        }
        world->camera_count--;
    }
    s->present &= ~LE_PRESENT_CAMERA;
    s->camera_index = LE_NO_LINK;
    if (world->has_active_camera &&
        world->active_camera.index == slot &&
        world->active_camera.generation == s->generation) {
        world->has_active_camera = 0;
        world->active_camera = LE_OBJECT_INVALID;
    }
    return LE_SUCCESS;
}

int le_object_get_camera(const le_world *world, const le_object *object,
                         le_camera_desc *out_desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_desc != NULL) {
        memset(out_desc, 0, sizeof(*out_desc));
    }
    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    if ((world->slots[slot].present & LE_PRESENT_CAMERA) == 0u) {
        return 0;
    }
    {
        uint32_t idx = (uint32_t)world->slots[slot].camera_index;

        if (idx >= world->camera_count) {
            return 0;
        }
        if (world->cameras[idx].slot != slot) {
            return 0;
        }
        if (out_desc != NULL) {
            *out_desc = world->cameras[idx].desc;
        }
        return 1;
    }
}

le_result le_world_set_active_camera(le_world *world,
                                     const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (world == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (object == NULL || !le_object_is_valid(object) ||
        (object->index == LE_OBJECT_INVALID.index &&
         object->generation == LE_OBJECT_INVALID.generation)) {
        world->has_active_camera = 0;
        world->active_camera = LE_OBJECT_INVALID;
        return LE_SUCCESS;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    if ((world->slots[slot].present & LE_PRESENT_CAMERA) == 0u) {
        return LE_ERROR_MISSING_COMPONENT;
    }
    world->active_camera.index = slot;
    world->active_camera.generation = world->slots[slot].generation;
    world->active_camera.world_tag = world->tag;
    world->has_active_camera = 1;
    return LE_SUCCESS;
}

int le_world_get_active_camera(const le_world *world,
                               le_object *out_object) {
    if (out_object != NULL) {
        *out_object = LE_OBJECT_INVALID;
    }
    if (world == NULL || out_object == NULL) {
        return 0;
    }
    if (!world->has_active_camera) {
        return 0;
    }
    /* Auto-clear when the object died (destroy path also clears,
     * but this covers every ordering defensively). */
    if (!le_object_is_alive(world, &world->active_camera)) {
        ((le_world *)world)->has_active_camera = 0;
        ((le_world *)world)->active_camera = LE_OBJECT_INVALID;
        return 0;
    }
    *out_object = world->active_camera;
    return 1;
}

static int le_light_desc_valid(const le_light_desc *desc) {
    int i;

    if (desc == NULL) {
        return 0;
    }
    if (desc->type != LE_LIGHT_DIRECTIONAL &&
        desc->type != LE_LIGHT_POINT && desc->type != LE_LIGHT_SPOT) {
        return 0;
    }
    if (!isfinite(desc->intensity)) {
        return 0;
    }
    for (i = 0; i < 3; i++) {
        if (!isfinite(desc->color[i])) {
            return 0;
        }
    }
    if (desc->type != LE_LIGHT_DIRECTIONAL) {
        if (!isfinite(desc->range) || !(desc->range > 0.0f)) {
            return 0;
        }
    }
    if (desc->type == LE_LIGHT_SPOT) {
        if (!isfinite(desc->spot_inner) || !isfinite(desc->spot_outer) ||
            !(desc->spot_inner >= 0.0f) ||
            !(desc->spot_outer > 0.0f) ||
            !(desc->spot_outer < 1.5707963267948966f) ||
            !(desc->spot_inner <= desc->spot_outer)) {
            return 0;
        }
    }
    /* Shadow config: mirror the renderer's structural rules that
     * are checkable without a device (full validation happens at
     * sync, where lr_renderer_submit_light is the authority).
     * Zero-initialized shadow (enabled == 0) always passes. */
    if (desc->shadow.enabled) {
        if (desc->type == LE_LIGHT_POINT) {
            return 0;
        }
        if (desc->shadow.resolution != 0 &&
            (desc->shadow.resolution < 128u ||
             desc->shadow.resolution > 4096u ||
             (desc->shadow.resolution &
              (desc->shadow.resolution - 1u)) != 0u)) {
            return 0;
        }
        if (!isfinite(desc->shadow.depth_bias) ||
            !isfinite(desc->shadow.normal_bias) ||
            !isfinite(desc->shadow.near_plane) ||
            !isfinite(desc->shadow.far_plane) ||
            !isfinite(desc->shadow.shadow_distance)) {
            return 0;
        }
        if (desc->shadow.near_plane < 0.0f ||
            desc->shadow.far_plane < 0.0f ||
            desc->shadow.shadow_distance < 0.0f) {
            return 0;
        }
    }
    return 1;
}

le_result le_object_add_light(le_world *world, const le_object *object,
                              const le_light_desc *desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_object_slot *s;
    le_result grow;

    if (world == NULL || object == NULL || desc == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_light_desc_valid(desc)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    s = &world->slots[slot];
    if ((s->present & LE_PRESENT_LIGHT) != 0u) {
        uint32_t idx = (uint32_t)s->light_index;

        if (idx >= world->light_count) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        world->lights[idx].desc = *desc;
        return LE_SUCCESS;
    }
    grow = le_grow_lights(world);
    if (grow != LE_SUCCESS) {
        return grow;
    }
    world->lights[world->light_count].slot = slot;
    world->lights[world->light_count].desc = *desc;
    s->light_index = (int32_t)world->light_count;
    s->present |= LE_PRESENT_LIGHT;
    world->light_count++;
    return LE_SUCCESS;
}

le_result le_object_remove_light(le_world *world, const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_object_slot *s;
    uint32_t idx;
    uint32_t last;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    s = &world->slots[slot];
    if ((s->present & LE_PRESENT_LIGHT) == 0u) {
        return LE_SUCCESS;
    }
    idx = (uint32_t)s->light_index;
    if (idx < world->light_count) {
        last = world->light_count - 1u;
        if (idx != last) {
            world->lights[idx] = world->lights[last];
            world->slots[world->lights[idx].slot].light_index =
                (int32_t)idx;
        }
        world->light_count--;
    }
    s->present &= ~LE_PRESENT_LIGHT;
    s->light_index = LE_NO_LINK;
    return LE_SUCCESS;
}

int le_object_get_light(const le_world *world, const le_object *object,
                        le_light_desc *out_desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_desc != NULL) {
        memset(out_desc, 0, sizeof(*out_desc));
    }
    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    if ((world->slots[slot].present & LE_PRESENT_LIGHT) == 0u) {
        return 0;
    }
    {
        uint32_t idx = (uint32_t)world->slots[slot].light_index;

        if (idx >= world->light_count) {
            return 0;
        }
        if (world->lights[idx].slot != slot) {
            return 0;
        }
        if (out_desc != NULL) {
            *out_desc = world->lights[idx].desc;
        }
        return 1;
    }
}

/* ------------------------------------------------------------------
 * Asset-backed renderables (Phase 25): same dense + swap-remove
 * discipline as the pointer spelling, but entries hold HANDLES.
 * At most one renderable spelling per object (adding one removes
 * the other, so extraction never double-submits).
 * ------------------------------------------------------------------ */

static void le_remove_asset_renderable_entry(le_world *world,
                                             uint32_t slot) {
    le_object_slot *s = &world->slots[slot];
    uint32_t idx;
    uint32_t last;

    if ((s->present & LE_PRESENT_ASSET_RENDERABLE) == 0u) {
        return;
    }
    idx = (uint32_t)s->renderable_index;
    if (idx < world->asset_renderable_count &&
        world->asset_renderables[idx].slot == slot) {
        last = world->asset_renderable_count - 1u;
        if (idx != last) {
            world->asset_renderables[idx] =
                world->asset_renderables[last];
            world->slots[world->asset_renderables[idx].slot]
                .renderable_index = (int32_t)idx;
        }
        world->asset_renderable_count--;
    }
    s->present &= ~LE_PRESENT_ASSET_RENDERABLE;
    s->renderable_index = LE_NO_LINK;
}

le_result le_object_add_asset_renderable(
    le_world *world, const le_object *object,
    const le_asset_renderable_desc *desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_object_slot *s;
    le_result grow;
    le_engine *engine;
    uint32_t mesh_slot;
    uint32_t mat_slot;

    if (world == NULL || object == NULL || desc == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    engine = world->engine;
    if (engine == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_asset_live(engine, &desc->mesh, &mesh_slot,
                               &code)) {
        return (code == LE_ERROR_INVALID_ARGUMENT)
                   ? LE_ERROR_INVALID_ARGUMENT
                   : LE_ERROR_STALE_ASSET;
    }
    if (engine->assets[mesh_slot].type != LE_ASSET_MESH) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    if (engine->assets[mesh_slot].state != LE_ASSET_READY) {
        return LE_ERROR_MISSING_ASSET;
    }
    if (!le_resolve_asset_live(engine, &desc->material, &mat_slot,
                               &code)) {
        return (code == LE_ERROR_INVALID_ARGUMENT)
                   ? LE_ERROR_INVALID_ARGUMENT
                   : LE_ERROR_STALE_ASSET;
    }
    if (engine->assets[mat_slot].type != LE_ASSET_MATERIAL) {
        return LE_ERROR_WRONG_ASSET_TYPE;
    }
    if (engine->assets[mat_slot].state != LE_ASSET_READY) {
        return LE_ERROR_MISSING_ASSET;
    }
    s = &world->slots[slot];
    if ((s->present & LE_PRESENT_RENDERABLE) != 0u) {
        le_object_remove_renderable(world, object);
    }
    if ((s->present & LE_PRESENT_ASSET_RENDERABLE) != 0u) {
        uint32_t idx = (uint32_t)s->renderable_index;

        if (idx >= world->asset_renderable_count) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        world->asset_renderables[idx].desc = *desc;
        return LE_SUCCESS;
    }
    grow = le_grow_asset_renderables(world);
    if (grow != LE_SUCCESS) {
        return grow;
    }
    world->asset_renderables[world->asset_renderable_count].slot =
        slot;
    world->asset_renderables[world->asset_renderable_count].desc =
        *desc;
    s->renderable_index =
        (int32_t)world->asset_renderable_count;
    s->present |= LE_PRESENT_ASSET_RENDERABLE;
    world->asset_renderable_count++;
    return LE_SUCCESS;
}

int le_object_get_asset_renderable(const le_world *world,
                                   const le_object *object,
                                   le_asset_renderable_desc *out_desc) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (out_desc != NULL) {
        memset(out_desc, 0, sizeof(*out_desc));
    }
    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    if ((world->slots[slot].present & LE_PRESENT_ASSET_RENDERABLE) ==
        0u) {
        return 0;
    }
    {
        uint32_t idx = (uint32_t)world->slots[slot].renderable_index;

        if (idx >= world->asset_renderable_count) {
            return 0;
        }
        if (world->asset_renderables[idx].slot != slot) {
            return 0;
        }
        if (out_desc != NULL) {
            *out_desc = world->asset_renderables[idx].desc;
        }
        return 1;
    }
}

le_result le_object_remove_asset_renderable(le_world *world,
                                            const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    le_remove_asset_renderable_entry(world, slot);
    return LE_SUCCESS;
}
