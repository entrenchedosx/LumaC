/*
 * Luma Engine world: le_world_create / le_world_destroy, storage
 * growth, shared slot helpers (resolve, dirty propagation, matrix
 * refresh, hierarchy links).
 *
 * Growth: geometric x2 from max(LE_INITIAL_CAPACITY,
 * initial_capacity), capped at LE_MAX_CAPACITY with LE_ERROR_OVERFLOW
 * past it. Every growth path allocates the new array FIRST and only
 * swaps it in on full success, so failure leaves the world
 * unchanged (strong exception safety in C: no half-grown state).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"

const le_object LE_OBJECT_INVALID = { LE_OBJECT_INDEX_INVALID, 0u, 0u };

/* Process-wide world tag counter. Tags are never 0 (0 = "no
 * world"). No atomic needed: world creation follows the
 * single-owning-thread contract (same rule as all world
 * mutation). Wraps 0xFFFFFFFF -> 1 (never 0); tag reuse across
 * world lifetimes is documented on le_object (joint validation
 * with live generations keeps the alias bar at generation-wrap
 * level). */
static uint32_t le_world_next_tag(void) {
    static uint32_t counter = 0;

    counter++;
    if (counter == 0) {
        counter = 1;
    }
    return counter;
}

static uint64_t le_world_make_salt(void) {
    /* Per-process counter folded with the address of a static: two
     * worlds never share a salt in one process, and salts differ
     * across runs in practice (ASLR). No crypto needed — the salt
     * only decorrelates renderer temporal keys between worlds. */
    static uint64_t counter = 0;
    static int anchor = 0;
    uint64_t a;
    uint64_t b;

    counter++;
    a = (uint64_t)(uintptr_t)&anchor;
    b = counter * 11400714819323198485ull;
    a ^= b + 0x9e3779b97f4a7c15ull + (a << 6) + (a >> 2);
    a ^= (uint64_t)(uintptr_t)&counter;
    if (a == 0) {
        a = 0x9e3779b97f4a7c15ull;
    }
    return a;
}

int le_object_is_valid(const le_object *object) {
    if (object == NULL) {
        return 0;
    }
    if (object->index == LE_OBJECT_INDEX_INVALID) {
        return 0;
    }
    if (object->generation == 0) {
        return 0;
    }
    if (object->world_tag == 0) {
        return 0;
    }
    return 1;
}

int le_object_is_alive(const le_world *world, const le_object *object) {
    uint32_t index;
    le_result code = LE_SUCCESS;

    /* Single validation funnel: identical verdicts to every mutating
     * API (tag mismatch => dead here, WRONG_WORLD there). */
    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_resolve_live(world, object, &index, &code)) {
        return 0;
    }
    (void)index;
    return 1;
}

static uint64_t le_mix64(uint64_t x) {
    /* splitmix64 finalizer: avalanche for the stable-ID hash. */
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebull;
    x ^= x >> 31;
    return x;
}

uint64_t le_object_stable_id(const le_world *world,
                             const le_object *object) {
    uint64_t h;

    if (world == NULL || object == NULL) {
        return 0;
    }
    if (!le_object_is_alive(world, object)) {
        return 0;
    }
    /* Mix {salt, tag, index, generation} so the key changes on
     * slot reuse (generation bump) and never collides across
     * worlds (salt + tag). Concatenate-then-hash (never XOR
     * folding of adjacent fields): index and generation occupy
     * disjoint lanes, so consecutive slots/generations cannot
     * cancel each other. 0 stays reserved for "no stable
     * identity". */
    h = ((uint64_t)object->world_tag << 32) | object->index;
    h ^= world->salt + 0x9e3779b97f4a7c15ull +
         ((uint64_t)object->generation << 32) + (h << 6) + (h >> 2);
    h = le_mix64(h);
    h = le_mix64(h ^ (world->salt >> 17));
    if (h == 0) {
        h = 1;
    }
    return h;
}

int le_resolve_live(const le_world *world, const le_object *object,
                    uint32_t *out_slot, le_result *out_result) {
    const le_object_slot *slot;

    if (world == NULL || object == NULL) {
        if (out_result != NULL) {
            *out_result = LE_ERROR_INVALID_ARGUMENT;
        }
        return 0;
    }
    if (object->index == LE_OBJECT_INDEX_INVALID ||
        object->generation == 0 || object->world_tag == 0 ||
        object->index >= world->capacity) {
        if (out_result != NULL) {
            *out_result = LE_ERROR_STALE_HANDLE;
        }
        return 0;
    }
    if (object->world_tag != world->tag) {
        if (out_result != NULL) {
            *out_result = LE_ERROR_WRONG_WORLD;
        }
        return 0;
    }
    slot = &world->slots[object->index];
    if (!slot->alive || slot->generation != object->generation) {
        if (out_result != NULL) {
            *out_result = LE_ERROR_STALE_HANDLE;
        }
        return 0;
    }
    if (out_slot != NULL) {
        *out_slot = object->index;
    }
    return 1;
}

le_result le_ensure_object_capacity(le_world *world) {
    uint32_t needed;
    uint32_t grown;
    le_object_slot *fresh;

    if (world == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    /* A non-empty free-list always satisfies one more creation:
     * no growth needed. Only an exhausted list grows. */
    if (world->capacity > 0 && world->free_head != LE_NO_LINK) {
        return LE_SUCCESS;
    }
    needed = (world->capacity == 0) ? LE_INITIAL_CAPACITY
                                    : world->alive_count + 1u;
    if (world->capacity == 0) {
        grown = (needed < LE_INITIAL_CAPACITY) ? LE_INITIAL_CAPACITY
                                               : needed;
    } else {
        if (world->capacity > LE_MAX_CAPACITY / 2u) {
            grown = LE_MAX_CAPACITY;
        } else {
            grown = world->capacity * 2u;
        }
        if (grown < needed) {
            grown = needed;
        }
    }
    if (grown > LE_MAX_CAPACITY) {
        return LE_ERROR_OVERFLOW;
    }
    if (world->capacity > 0 && grown <= world->capacity) {
        return LE_ERROR_OVERFLOW;
    }
    fresh = (le_object_slot *)calloc(grown, sizeof(le_object_slot));
    if (fresh == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    if (world->slots != NULL) {
        memcpy(fresh, world->slots,
               (size_t)world->capacity * sizeof(le_object_slot));
        /* New tail slots start dead with generation 0 (never valid)
         * and join the free-list in ascending order so reuse order
         * is deterministic. */
        {
            uint32_t i;

            for (i = world->capacity; i < grown; i++) {
                fresh[i].alive = 0;
                fresh[i].generation = 0;
                fresh[i].free_next = (i + 1u < grown)
                                         ? (int32_t)(i + 1u)
                                         : world->free_head;
                fresh[i].parent = LE_NO_LINK;
                fresh[i].first_child = LE_NO_LINK;
                fresh[i].next_sibling = LE_NO_LINK;
                fresh[i].renderable_index = LE_NO_LINK;
                fresh[i].camera_index = LE_NO_LINK;
                fresh[i].light_index = LE_NO_LINK;
            }
            if (world->capacity < grown) {
                world->free_head = (int32_t)world->capacity;
                /* The loop above already chained the last new slot
                 * to the previous free_head. */
            }
        }
        free(world->slots);
    } else {
        uint32_t i;

        for (i = 0; i < grown; i++) {
            fresh[i].alive = 0;
            fresh[i].generation = 0;
            fresh[i].free_next =
                (i + 1u < grown) ? (int32_t)(i + 1u) : LE_NO_LINK;
            fresh[i].parent = LE_NO_LINK;
            fresh[i].first_child = LE_NO_LINK;
            fresh[i].next_sibling = LE_NO_LINK;
            fresh[i].renderable_index = LE_NO_LINK;
            fresh[i].camera_index = LE_NO_LINK;
            fresh[i].light_index = LE_NO_LINK;
        }
        world->free_head = 0;
    }
    world->slots = fresh;
    world->capacity = grown;
    return LE_SUCCESS;
}

int le_mark_subtree_dirty(le_world *world, uint32_t slot) {
    /* Iterative DFS with an explicit heap stack (no recursion:
     * 100k-deep chains are safe). Marks slot + all descendants. */
    int32_t *stack = NULL;
    size_t stack_cap = 0;
    size_t stack_top = 0;

    if (world == NULL || slot >= world->capacity) {
        return 0;
    }
    stack_cap = 64;
    stack = (int32_t *)malloc(stack_cap * sizeof(int32_t));
    if (stack == NULL) {
        return 0;
    }
    stack[stack_top++] = (int32_t)slot;
    while (stack_top > 0) {
        int32_t cur;
        int32_t child;

        stack_top--;
        cur = stack[stack_top];
        if (cur < 0 || (uint32_t)cur >= world->capacity) {
            continue;
        }
        if (!world->slots[cur].alive) {
            continue;
        }
        world->slots[cur].dirty = 1;
        child = world->slots[cur].first_child;
        while (child != LE_NO_LINK) {
            int32_t next = LE_NO_LINK;

            if (child < 0 || (uint32_t)child >= world->capacity) {
                break;
            }
            next = world->slots[child].next_sibling;
            if (stack_top >= stack_cap) {
                size_t grown = stack_cap * 2u;
                int32_t *fresh;

                if (grown < stack_cap + 64u) {
                    grown = stack_cap + 64u;
                }
                fresh =
                    (int32_t *)realloc(stack, grown * sizeof(int32_t));
                if (fresh == NULL) {
                    free(stack);
                    return 0;
                }
                stack = fresh;
                stack_cap = grown;
            }
            stack[stack_top++] = child;
            child = next;
        }
    }
    free(stack);
    return 1;
}

void le_compose_slot_world(le_world *world, uint32_t slot) {
    float local[16];
    le_object_slot *s;

    if (world == NULL || slot >= world->capacity) {
        return;
    }
    s = &world->slots[slot];
    le_transform_compose(s->position, s->rotation, s->scale, local);
    if (s->parent == LE_NO_LINK) {
        memcpy(s->world_matrix, local, sizeof(local));
    } else {
        uint32_t p = (uint32_t)s->parent;

        if (p >= world->capacity || !world->slots[p].alive) {
            memcpy(s->world_matrix, local, sizeof(local));
        } else {
            le_mat4_multiply(s->world_matrix,
                             world->slots[p].world_matrix, local);
        }
    }
    s->dirty = 0;
}

void le_refresh_world_matrices(le_world *world) {
    uint32_t i;

    if (world == NULL || world->slots == NULL) {
        return;
    }
    /* Ascending slot order is NOT topological in general
     * (a parent may sit at a higher index than its child), so
     * refresh via an explicit iterative root-first walk: push
     * roots, then pop-and-compose with children (children always
     * follow their parent in pop order because each pop composes
     * immediately and pushes its children). Two-pass fallback:
     * any slot still dirty afterwards (should not happen — every
     * live slot is reachable from exactly one root) composes
     * against its ancestors directly. */
    {
        int32_t *stack = NULL;
        size_t cap = 0;
        size_t top = 0;
        uint32_t alive_seen = 0;

        /* Only walk when something is dirty (O(1) when clean). */
        {
            uint32_t dirty_hint = 0;

            for (i = 0; i < world->capacity; i++) {
                if (world->slots[i].alive && world->slots[i].dirty) {
                    dirty_hint = 1;
                    break;
                }
            }
            if (!dirty_hint) {
                return;
            }
        }
        cap = (world->alive_count > 64u) ? (size_t)world->alive_count
                                         : 64u;
        stack = (int32_t *)malloc(cap * sizeof(int32_t));
        if (stack == NULL) {
            /* Allocation failure: fall back to per-slot ancestor
             * refresh below (slower, still correct). */
        } else {
            for (i = 0; i < world->capacity; i++) {
                if (world->slots[i].alive &&
                    world->slots[i].parent == LE_NO_LINK) {
                    if (top >= cap) {
                        break;
                    }
                    stack[top++] = (int32_t)i;
                }
            }
            while (top > 0) {
                int32_t cur;
                int32_t child;

                top--;
                cur = stack[top];
                if (cur < 0 ||
                    (uint32_t)cur >= world->capacity) {
                    continue;
                }
                if (!world->slots[cur].alive) {
                    continue;
                }
                alive_seen++;
                le_compose_slot_world(world, (uint32_t)cur);
                child = world->slots[cur].first_child;
                while (child != LE_NO_LINK) {
                    int32_t next = LE_NO_LINK;

                    if (child < 0 ||
                        (uint32_t)child >= world->capacity) {
                        break;
                    }
                    next = world->slots[child].next_sibling;
                    if (top < cap) {
                        stack[top++] = child;
                    }
                    child = next;
                }
            }
            free(stack);
            if (alive_seen == world->alive_count) {
                return;
            }
        }
    }
    /* Fallback (or stack overflow path): per-slot ancestor-chain
     * refresh, iterative with a small heap buffer. */
    for (i = 0; i < world->capacity; i++) {
        if (!world->slots[i].alive || !world->slots[i].dirty) {
            continue;
        }
        {
            /* Collect the dirty ancestor chain root-first. */
            uint32_t chain[256];
            size_t chain_len = 0;
            int32_t cur = (int32_t)i;
            int use_heap = 0;
            uint32_t *heap = NULL;
            size_t heap_cap = 0;
            size_t heap_len = 0;

            while (cur != LE_NO_LINK && cur >= 0 &&
                   (uint32_t)cur < world->capacity &&
                   world->slots[cur].alive) {
                if (!use_heap) {
                    if (chain_len < 256u) {
                        chain[chain_len++] = (uint32_t)cur;
                    } else {
                        heap_cap = 512;
                        heap = (uint32_t *)malloc(
                            heap_cap * sizeof(uint32_t));
                        if (heap == NULL) {
                            break;
                        }
                        memcpy(heap, chain, sizeof(chain));
                        heap_len = chain_len;
                        use_heap = 1;
                    }
                } else {
                    if (heap_len >= heap_cap) {
                        size_t grown = heap_cap * 2u;
                        uint32_t *fresh = (uint32_t *)realloc(
                            heap, grown * sizeof(uint32_t));

                        if (fresh == NULL) {
                            break;
                        }
                        heap = fresh;
                        heap_cap = grown;
                    }
                    heap[heap_len++] = (uint32_t)cur;
                }
                if (!world->slots[cur].dirty) {
                    break;
                }
                cur = world->slots[cur].parent;
            }
            if (use_heap) {
                while (heap_len > 0) {
                    heap_len--;
                    le_compose_slot_world(world, heap[heap_len]);
                }
                free(heap);
            } else {
                while (chain_len > 0) {
                    chain_len--;
                    le_compose_slot_world(world, chain[chain_len]);
                }
            }
        }
    }
}

void le_detach_from_parent(le_world *world, uint32_t slot) {
    int32_t parent;
    int32_t *link;

    if (world == NULL || slot >= world->capacity) {
        return;
    }
    parent = world->slots[slot].parent;
    if (parent == LE_NO_LINK) {
        return;
    }
    if (parent < 0 || (uint32_t)parent >= world->capacity) {
        world->slots[slot].parent = LE_NO_LINK;
        world->slots[slot].next_sibling = LE_NO_LINK;
        return;
    }
    link = &world->slots[parent].first_child;
    while (*link != LE_NO_LINK) {
        int32_t cur = *link;

        if (cur < 0 || (uint32_t)cur >= world->capacity) {
            break;
        }
        if ((uint32_t)cur == slot) {
            *link = world->slots[slot].next_sibling;
            break;
        }
        link = &world->slots[cur].next_sibling;
    }
    world->slots[slot].parent = LE_NO_LINK;
    world->slots[slot].next_sibling = LE_NO_LINK;
}

int le_is_ancestor(const le_world *world, uint32_t ancestor,
                   uint32_t slot) {
    int32_t cur;

    if (world == NULL || ancestor >= world->capacity ||
        slot >= world->capacity) {
        return 0;
    }
    cur = (int32_t)slot;
    while (cur != LE_NO_LINK) {
        if (cur < 0 || (uint32_t)cur >= world->capacity) {
            return 0;
        }
        if ((uint32_t)cur == ancestor) {
            return 1;
        }
        cur = world->slots[cur].parent;
    }
    return 0;
}

le_result le_world_create(le_engine *engine, const le_world_desc *desc,
                          le_world **out_world) {
    le_world *world;
    uint32_t initial = LE_INITIAL_CAPACITY;
    uint32_t i;

    if (engine == NULL || out_world == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    *out_world = NULL;
    if (desc != NULL && desc->initial_capacity != 0) {
        if (desc->initial_capacity > LE_MAX_CAPACITY) {
            return LE_ERROR_OVERFLOW;
        }
        initial = desc->initial_capacity;
    }
    world = (le_world *)calloc(1, sizeof(le_world));
    if (world == NULL) {
        return LE_ERROR_OUT_OF_MEMORY;
    }
    world->slots =
        (le_object_slot *)calloc(initial, sizeof(le_object_slot));
    if (world->slots == NULL) {
        free(world);
        return LE_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < initial; i++) {
        world->slots[i].alive = 0;
        world->slots[i].generation = 0;
        world->slots[i].free_next =
            (i + 1u < initial) ? (int32_t)(i + 1u) : LE_NO_LINK;
        world->slots[i].parent = LE_NO_LINK;
        world->slots[i].first_child = LE_NO_LINK;
        world->slots[i].next_sibling = LE_NO_LINK;
        world->slots[i].renderable_index = LE_NO_LINK;
        world->slots[i].camera_index = LE_NO_LINK;
        world->slots[i].light_index = LE_NO_LINK;
    }
    world->engine = engine;
    world->salt = le_world_make_salt();
    world->tag = le_world_next_tag();
    world->capacity = initial;
    world->free_head = 0;
    world->active_camera = LE_OBJECT_INVALID;
    world->has_active_camera = 0;
    world->time = 0.0;
    /* Fixed-step script schedule defaults: fixed_update disabled
     * (le_script_set_fixed_step opts in); spiral guard 4 steps. */
    world->script_fixed_dt = 0.0f;
    world->script_max_steps = 4u;
    world->script_accum = 0.0;
    world->scripts_firing = 0;
    world->scripts_tearing_down = 0;
    /* Engine-owned: link into the engine's world list. */
    world->next = engine->worlds;
    world->prev = NULL;
    if (engine->worlds != NULL) {
        engine->worlds->prev = world;
    }
    engine->worlds = world;
    *out_world = world;
    return LE_SUCCESS;
}

void le_world_destroy(le_world *world) {
    uint32_t i;

    if (world == NULL) {
        return;
    }
    /* Retire everything while slots are still addressable: fire
     * script destroy() callbacks FIRST (still-valid world; sets
     * the teardown guard so structural script ops fail safely),
     * release script entry state with the backend, then free
     * names, then release component arrays and slots. */
    le_script_fire_world_destroy(world);
    {
        uint32_t k;

        for (k = 0; k < world->script_count; k++) {
            le_script_release_entry(world, &world->scripts[k]);
        }
        free(world->scripts);
        world->scripts = NULL;
        world->script_count = 0;
        world->script_capacity = 0;
    }
    le_script_free_world(world);
    /* Borrowed renderer mesh/material handles are untouched
     * (application keeps them alive per the renderer contract);
     * asset HANDLES die with their slots while registry backing
     * stays (shared assets outlive worlds by design); temporal
     * keys die implicitly with the generations. */
    if (world->slots != NULL) {
        for (i = 0; i < world->capacity; i++) {
            free(world->slots[i].name);
            world->slots[i].name = NULL;
        }
        free(world->slots);
        world->slots = NULL;
    }
    free(world->renderables);
    free(world->asset_renderables);
    free(world->cameras);
    free(world->lights);
    /* Unlink from the owning engine. */
    if (world->engine != NULL) {
        if (world->prev != NULL) {
            world->prev->next = world->next;
        } else {
            world->engine->worlds = world->next;
        }
        if (world->next != NULL) {
            world->next->prev = world->prev;
        }
    }
    free(world);
}

le_engine *le_world_get_engine(const le_world *world) {
    if (world == NULL) {
        return NULL;
    }
    return world->engine;
}

uint32_t le_world_get_object_count(const le_world *world) {
    if (world == NULL) {
        return 0;
    }
    return world->alive_count;
}

uint32_t le_world_get_object_capacity(const le_world *world) {
    if (world == NULL) {
        return 0;
    }
    return world->capacity;
}
