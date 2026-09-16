/*
 * Luma Engine objects: creation, destruction (whole-subtree
 * default, iterative), names, enabled state, hierarchy.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"

static void le_slot_init_live(le_object_slot *slot, uint32_t generation) {
    slot->alive = 1;
    slot->generation = generation;
    slot->free_next = LE_NO_LINK;
    slot->enabled = 1;
    slot->present = 0;
    slot->parent = LE_NO_LINK;
    slot->first_child = LE_NO_LINK;
    slot->next_sibling = LE_NO_LINK;
    slot->name = NULL;
    slot->position[0] = 0.0f;
    slot->position[1] = 0.0f;
    slot->position[2] = 0.0f;
    slot->rotation[0] = 0.0f;
    slot->rotation[1] = 0.0f;
    slot->rotation[2] = 0.0f;
    slot->rotation[3] = 1.0f;
    slot->scale[0] = 1.0f;
    slot->scale[1] = 1.0f;
    slot->scale[2] = 1.0f;
    le_mat4_identity(slot->world_matrix);
    slot->dirty = 0;
    slot->renderable_index = LE_NO_LINK;
    slot->camera_index = LE_NO_LINK;
    slot->light_index = LE_NO_LINK;
    slot->script_index = LE_NO_LINK;
    slot->body_index = LE_NO_LINK;
    slot->collider_index = LE_NO_LINK;
    slot->animator_index = LE_NO_LINK;
    slot->character_index = LE_NO_LINK;
}

le_result le_object_create(le_world *world, le_object *out_object) {
    le_result grow = LE_SUCCESS;
    int32_t slot_idx;
    uint32_t gen;

    if (world == NULL || out_object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    grow = le_ensure_object_capacity(world);
    if (grow != LE_SUCCESS) {
        return grow;
    }
    if (world->free_head == LE_NO_LINK) {
        return LE_ERROR_OVERFLOW;
    }
    slot_idx = world->free_head;
    if (slot_idx < 0 || (uint32_t)slot_idx >= world->capacity) {
        return LE_ERROR_OVERFLOW;
    }
    world->free_head = world->slots[slot_idx].free_next;
    /* Preserve the slot's generation: first use bumps 0 -> 1 (0
     * never validates, so {i,0,tag} is never a live handle). */
    gen = world->slots[slot_idx].generation;
    if (gen == 0) {
        gen = 1;
    }
    le_slot_init_live(&world->slots[slot_idx], gen);
    world->alive_count++;
    world->enabled_count++;
    out_object->index = (uint32_t)slot_idx;
    out_object->generation = gen;
    out_object->world_tag = world->tag;
    return LE_SUCCESS;
}

/* Remove one slot's optional components without touching hierarchy
 * links (the destroy walker handles links separately). Keeps all
 * counters and dense arrays consistent. */
static void le_remove_slot_components(le_world *world, uint32_t slot) {
    le_object_slot *s = &world->slots[slot];

    if ((s->present & LE_PRESENT_SCRIPT) != 0u) {
        uint32_t idx = (uint32_t)s->script_index;
        uint32_t last;

        if (idx < world->script_count &&
            world->scripts[idx].slot == slot) {
            le_script_release_entry(world, &world->scripts[idx]);
            last = world->script_count - 1u;
            if (idx != last) {
                world->scripts[idx] = world->scripts[last];
                world->slots[world->scripts[idx].slot]
                    .script_index = (int32_t)idx;
            }
            world->script_count--;
        }
        s->script_index = LE_NO_LINK;
        s->present &= ~LE_PRESENT_SCRIPT;
    }
    if ((s->present & LE_PRESENT_RENDERABLE) != 0u &&
        s->renderable_index != LE_NO_LINK) {
        uint32_t idx = (uint32_t)s->renderable_index;
        uint32_t last;

        if (idx < world->renderable_count) {
            last = world->renderable_count - 1u;
            if (idx != last) {
                world->renderables[idx] = world->renderables[last];
                world->slots[world->renderables[idx].slot]
                    .renderable_index = (int32_t)idx;
            }
            world->renderable_count--;
        }
        s->renderable_index = LE_NO_LINK;
        s->present &= ~LE_PRESENT_RENDERABLE;
    }
    if ((s->present & LE_PRESENT_ASSET_RENDERABLE) != 0u &&
        s->renderable_index != LE_NO_LINK) {
        uint32_t aidx = (uint32_t)s->renderable_index;
        uint32_t alast;

        if (aidx < world->asset_renderable_count &&
            world->asset_renderables[aidx].slot == slot) {
            alast = world->asset_renderable_count - 1u;
            if (aidx != alast) {
                world->asset_renderables[aidx] =
                    world->asset_renderables[alast];
                world->slots[world->asset_renderables[aidx].slot]
                    .renderable_index = (int32_t)aidx;
            }
            world->asset_renderable_count--;
        }
        s->renderable_index = LE_NO_LINK;
        s->present &= ~LE_PRESENT_ASSET_RENDERABLE;
    }
    if ((s->present & LE_PRESENT_CAMERA) != 0u &&
        s->camera_index != LE_NO_LINK) {
        uint32_t idx = (uint32_t)s->camera_index;
        uint32_t last;

        if (idx < world->camera_count) {
            last = world->camera_count - 1u;
            if (idx != last) {
                world->cameras[idx] = world->cameras[last];
                world->slots[world->cameras[idx].slot].camera_index =
                    (int32_t)idx;
            }
            world->camera_count--;
        }
        s->camera_index = LE_NO_LINK;
        s->present &= ~LE_PRESENT_CAMERA;
    }
    if ((s->present & LE_PRESENT_LIGHT) != 0u &&
        s->light_index != LE_NO_LINK) {
        uint32_t idx = (uint32_t)s->light_index;
        uint32_t last;

        if (idx < world->light_count) {
            last = world->light_count - 1u;
            if (idx != last) {
                world->lights[idx] = world->lights[last];
                world->slots[world->lights[idx].slot].light_index =
                    (int32_t)idx;
            }
            world->light_count--;
        }
        s->light_index = LE_NO_LINK;
        s->present &= ~LE_PRESENT_LIGHT;
    }
    /* Phase 28: physics components retire with the slot
     * (swap-remove + EXIT emission to survivors + pair purge).
     * The hook lives in physics (object.c stays struct-blind
     * to le_physics_world, like the script hooks). */
    if (((s->present & LE_PRESENT_RIGID_BODY) != 0u ||
         (s->present & LE_PRESENT_COLLIDER) != 0u)) {
        le_physics_remove_slot_components(world, slot);
        s->body_index = LE_NO_LINK;
        s->collider_index = LE_NO_LINK;
        s->present &=
            ~(LE_PRESENT_RIGID_BODY | LE_PRESENT_COLLIDER);
        le_physics_retire_slot(world, slot);
    }
    /* Phase 29: animator retires with the slot (swap-remove via
     * the animation hook; object.c stays struct-blind). */
    if ((s->present & LE_PRESENT_ANIMATOR) != 0u) {
        le_anim_remove_slot_animator(world, slot);
        s->animator_index = LE_NO_LINK;
        s->present &= ~LE_PRESENT_ANIMATOR;
    }
    /* Phase 30: character controller retires with the slot. */
    if ((s->present & LE_PRESENT_CHARACTER) != 0u) {
        le_character_remove_slot(world, slot);
        s->character_index = LE_NO_LINK;
        s->present &= ~LE_PRESENT_CHARACTER;
    }
}

/* Retire one slot: fire script destroy() FIRST (iff start ran —
 * still-valid world, siblings alive in post-order), then detach,
 * remove components, free name, clear the active-camera
 * designation when it names this slot, bump the generation
 * (wrapping 0 -> 1, never 0), and push to the free-list.
 * The generation bump is the temporal-key retirement: any
 * renderer history keyed by the old stable ID can never reattach
 * to the slot's next occupant. */
static void le_retire_slot(le_world *world, uint32_t slot) {
    le_object_slot *s = &world->slots[slot];

    if ((s->present & LE_PRESENT_SCRIPT) != 0u) {
        le_script_fire_slot_destroy(world, slot);
    }
    uint32_t gen;

    le_detach_from_parent(world, slot);
    /* Children were already retired by the subtree walker before
     * this runs (post-order), so first_child must be empty here;
     * defensively clear either way (never follow it). */
    s->first_child = LE_NO_LINK;
    s->next_sibling = LE_NO_LINK;
    le_remove_slot_components(world, slot);
    if (s->name != NULL) {
        world->name_bytes -= strlen(s->name) + 1u;
        if (world->named_count > 0) {
            world->named_count--;
        }
        free(s->name);
        s->name = NULL;
    }
    if (s->enabled) {
        if (world->enabled_count > 0) {
            world->enabled_count--;
        }
    }
    if (world->has_active_camera &&
        world->active_camera.index == slot &&
        world->active_camera.generation == s->generation) {
        world->has_active_camera = 0;
        world->active_camera = LE_OBJECT_INVALID;
    }
    s->alive = 0;
    s->present = 0;
    s->dirty = 0;
    gen = s->generation + 1u;
    if (gen == 0) {
        gen = 1; /* skip 0 forever: generation 0 never validates */
    }
    s->generation = gen;
    s->free_next = world->free_head;
    world->free_head = (int32_t)slot;
    if (world->alive_count > 0) {
        world->alive_count--;
    }
}

le_result le_object_destroy(le_world *world, const le_object *object) {
    uint32_t root;
    le_result code = LE_SUCCESS;
    /* Iterative post-order over the subtree: explicit stack with a
     * visited flag per entry (no recursion: 100k-deep chains are
     * safe). Validation happens BEFORE any mutation: a stale
     * handle destroys nothing. */
    int32_t *stack = NULL;
    unsigned char *state = NULL;
    size_t cap = 0;
    size_t top = 0;

    if (world == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (object == NULL) {
        return LE_SUCCESS;
    }
    if (!le_object_is_valid(object)) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &root, &code)) {
        return code;
    }
    cap = 64;
    stack = (int32_t *)malloc(cap * sizeof(int32_t));
    state = (unsigned char *)malloc(cap * sizeof(unsigned char));
    if (stack == NULL || state == NULL) {
        free(stack);
        free(state);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    stack[top] = (int32_t)root;
    state[top] = 0;
    top++;
    while (top > 0) {
        int32_t cur = stack[top - 1];
        unsigned char st = state[top - 1];

        if (cur < 0 || (uint32_t)cur >= world->capacity ||
            !world->slots[cur].alive) {
            top--;
            continue;
        }
        if (st == 0) {
            int32_t child;

            state[top - 1] = 1;
            child = world->slots[cur].first_child;
            while (child != LE_NO_LINK) {
                int32_t next = LE_NO_LINK;

                if (child < 0 ||
                    (uint32_t)child >= world->capacity) {
                    break;
                }
                if (!world->slots[child].alive) {
                    next = world->slots[child].next_sibling;
                    child = next;
                    continue;
                }
                next = world->slots[child].next_sibling;
                if (top + 1 >= cap) {
                    size_t grown = cap * 2u;
                    int32_t *fs;
                    unsigned char *bs;

                    if (grown < cap + 64u) {
                        grown = cap + 64u;
                    }
                    fs = (int32_t *)realloc(stack,
                                            grown * sizeof(int32_t));
                    if (fs == NULL) {
                        free(stack);
                        free(state);
                        return LE_ERROR_OUT_OF_MEMORY;
                    }
                    stack = fs;
                    bs = (unsigned char *)realloc(
                        state, grown * sizeof(unsigned char));
                    if (bs == NULL) {
                        free(stack);
                        free(state);
                        return LE_ERROR_OUT_OF_MEMORY;
                    }
                    state = bs;
                    cap = grown;
                }
                stack[top] = child;
                state[top] = 0;
                top++;
                child = next;
            }
        } else {
            top--;
            le_retire_slot(world, (uint32_t)cur);
        }
    }
    free(stack);
    free(state);
    return LE_SUCCESS;
}

le_result le_object_set_name(le_world *world, const le_object *object,
                             const char *name) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    le_object_slot *s;
    char *copy = NULL;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    s = &world->slots[slot];
    if (name == NULL || name[0] == '\0') {
        if (s->name != NULL) {
            world->name_bytes -= strlen(s->name) + 1u;
            if (world->named_count > 0) {
                world->named_count--;
            }
            free(s->name);
            s->name = NULL;
        }
        return LE_SUCCESS;
    }
    {
        size_t len = strlen(name);

        if (len > 1024 * 1024) {
            return LE_ERROR_INVALID_ARGUMENT;
        }
        copy = (char *)malloc(len + 1u);
        if (copy == NULL) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
        memcpy(copy, name, len + 1u);
    }
    if (s->name != NULL) {
        world->name_bytes -= strlen(s->name) + 1u;
        free(s->name);
    } else {
        world->named_count++;
    }
    s->name = copy;
    world->name_bytes += strlen(copy) + 1u;
    return LE_SUCCESS;
}

const char *le_object_get_name(const le_world *world,
                               const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (world == NULL || object == NULL) {
        return NULL;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return NULL;
    }
    if (world->slots[slot].name == NULL) {
        return "";
    }
    return world->slots[slot].name;
}

le_result le_object_set_enabled(le_world *world, const le_object *object,
                                int enabled) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int want;

    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    want = (enabled != 0) ? 1 : 0;
    if (world->slots[slot].enabled != want) {
        if (want) {
            world->enabled_count++;
        } else if (world->enabled_count > 0) {
            world->enabled_count--;
        }
        world->slots[slot].enabled = want;
    }
    return LE_SUCCESS;
}

int le_object_is_enabled(const le_world *world, const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;

    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    return world->slots[slot].enabled;
}

int le_object_is_effectively_enabled(const le_world *world,
                                     const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int32_t cur;

    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    /* Walk root-ward iteratively: any disabled ancestor (or self)
     * disables the subtree. Depth-safe (no recursion, O(depth)). */
    cur = (int32_t)slot;
    while (cur != LE_NO_LINK) {
        if (cur < 0 || (uint32_t)cur >= world->capacity) {
            return 0;
        }
        if (!world->slots[cur].alive) {
            return 0;
        }
        if (!world->slots[cur].enabled) {
            return 0;
        }
        cur = world->slots[cur].parent;
    }
    return 1;
}

le_result le_object_set_parent(le_world *world, const le_object *child,
                               const le_object *parent) {
    uint32_t child_slot;
    le_result code = LE_SUCCESS;
    int has_parent = 0;
    uint32_t parent_slot = 0;

    if (world == NULL || child == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, child, &child_slot, &code)) {
        return code;
    }
    if (parent != NULL && le_object_is_valid(parent)) {
        le_result pcode = LE_SUCCESS;

        if (!le_resolve_live(world, parent, &parent_slot, &pcode)) {
            return pcode;
        }
        has_parent = 1;
    } else if (parent != NULL && (parent->index != LE_OBJECT_INVALID.index ||
                                  parent->generation !=
                                      LE_OBJECT_INVALID.generation)) {
        /* A well-formed-but-stale parent handle is STALE, not a
         * detach: detaching requires NULL or LE_OBJECT_INVALID. */
        return LE_ERROR_STALE_HANDLE;
    }
    if (has_parent) {
        if (parent_slot == child_slot) {
            return LE_ERROR_CYCLE;
        }
        /* Reject when the child is an ancestor-or-self of the new
         * parent (attaching under a descendant would loop). */
        if (le_is_ancestor(world, child_slot, parent_slot)) {
            return LE_ERROR_CYCLE;
        }
    }
    if (has_parent) {
        int32_t cur_parent = world->slots[child_slot].parent;

        if (cur_parent >= 0 && (uint32_t)cur_parent == parent_slot) {
            return LE_SUCCESS; /* already attached there */
        }
    } else if (world->slots[child_slot].parent == LE_NO_LINK) {
        return LE_SUCCESS; /* already a root */
    }
    /* Children chain through slots (no link blocks exist), so
     * reparenting cannot fail here; detach is infallible. */
    le_detach_from_parent(world, child_slot);
    if (has_parent) {
        world->slots[child_slot].parent = (int32_t)parent_slot;
        world->slots[child_slot].next_sibling =
            world->slots[parent_slot].first_child;
        world->slots[parent_slot].first_child = (int32_t)child_slot;
    }
    /* Local transform is PRESERVED (documented Phase 24 policy):
     * bytes untouched, world matrix follows the new parent. */
    le_mark_subtree_dirty(world, child_slot);
    return LE_SUCCESS;
}

int le_object_get_parent(const le_world *world, const le_object *child,
                         le_object *out_parent) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int32_t p;

    if (out_parent != NULL) {
        *out_parent = LE_OBJECT_INVALID;
    }
    if (world == NULL || child == NULL || out_parent == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, child, &slot, &code)) {
        return 0;
    }
    p = world->slots[slot].parent;
    if (p == LE_NO_LINK || p < 0 || (uint32_t)p >= world->capacity ||
        !world->slots[p].alive) {
        return 0;
    }
    out_parent->index = (uint32_t)p;
    out_parent->generation = world->slots[p].generation;
    out_parent->world_tag = world->tag;
    return 1;
}

uint32_t le_object_get_child_count(const le_world *world,
                                   const le_object *object) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int32_t child;
    uint32_t n = 0;

    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return 0;
    }
    child = world->slots[slot].first_child;
    while (child != LE_NO_LINK) {
        if (child < 0 || (uint32_t)child >= world->capacity) {
            break;
        }
        if (world->slots[child].alive) {
            n++;
        }
        child = world->slots[child].next_sibling;
    }
    return n;
}

le_result le_object_get_children(const le_world *world,
                                 const le_object *object,
                                 le_object *out_children, uint32_t capacity,
                                 uint32_t *out_count) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    int32_t child;
    uint32_t n = 0;

    if (out_count != NULL) {
        *out_count = 0;
    }
    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    /* Ascending slot order: children chain in reverse attach
     * order, so count first, then fill deterministically by
     * scanning slots ascending (deterministic iteration rule). */
    child = world->slots[slot].first_child;
    while (child != LE_NO_LINK) {
        if (child < 0 || (uint32_t)child >= world->capacity) {
            break;
        }
        if (world->slots[child].alive) {
            n++;
        }
        child = world->slots[child].next_sibling;
    }
    if (out_count != NULL) {
        *out_count = n;
    }
    if (out_children != NULL && capacity > 0 && n > 0) {
        uint32_t want = (n < capacity) ? n : capacity;
        uint32_t i;
        uint32_t filled = 0;

        for (i = 0; i < world->capacity && filled < want; i++) {
            if (world->slots[i].alive &&
                world->slots[i].parent == (int32_t)slot) {
                out_children[filled].index = i;
                out_children[filled].generation =
                    world->slots[i].generation;
                out_children[filled].world_tag = world->tag;
                filled++;
            }
        }
    }
    return LE_SUCCESS;
}

uint32_t le_world_get_root_count(const le_world *world) {
    uint32_t i;
    uint32_t n = 0;

    if (world == NULL) {
        return 0;
    }
    for (i = 0; i < world->capacity; i++) {
        if (world->slots[i].alive &&
            world->slots[i].parent == LE_NO_LINK) {
            n++;
        }
    }
    return n;
}

/* First live object with an exact name match (ascending slot
 * order — deterministic; names are convenience, NOT identity). */
int le_world_find_by_name(le_world *world, const char *name,
                           le_object *out_object) {
    uint32_t i;

    if (out_object != NULL) {
        *out_object = LE_OBJECT_INVALID;
    }
    if (world == NULL || name == NULL || name[0] == '\0') {
        return 0;
    }
    for (i = 0; i < world->capacity; i++) {
        const le_object_slot *s = &world->slots[i];

        if (!s->alive || s->name == NULL) {
            continue;
        }
        if (strcmp(s->name, name) == 0) {
            if (out_object != NULL) {
                out_object->index = i;
                out_object->generation = s->generation;
                out_object->world_tag = world->tag;
            }
            return 1;
        }
    }
    return 0;
}
