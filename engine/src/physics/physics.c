/*
 * Physics world lifecycle (Phase 28): per-le_world state creation,
 * destruction, event-storage growth, slot retirement.
 *
 * Ownership: the le_world owns its le_physics_world; bodies,
 * colliders, broad-phase scratch, contacts, overlaps, and event
 * rings all die with it. No process-global physics state exists
 * anywhere (two engines/worlds never share). The renderer never
 * owns physics memory.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

struct le_physics_world *le_physics_create(void) {
    struct le_physics_world *pw =
        (struct le_physics_world *)calloc(1, sizeof(*pw));

    if (pw == NULL) {
        return NULL;
    }
    pw->gravity[0] = 0.0f;
    pw->gravity[1] = -9.81f;
    pw->gravity[2] = 0.0f;
    pw->velocity_iters = LE_PHYS_DEFAULT_VELOCITY_ITERS;
    pw->position_iters = LE_PHYS_DEFAULT_POSITION_ITERS;
    return pw;
}

void le_physics_destroy(struct le_physics_world *pw) {
    if (pw == NULL) {
        return;
    }
    free(pw->bodies);
    free(pw->colliders);
    free(pw->contacts);
    free(pw->prev_overlaps);
    free(pw->sap_order);
    free(pw->pair_a);
    free(pw->pair_b);
    free(pw->events);
    free(pw);
}

le_result le_physics_ensure_events(struct le_physics_world *pw,
                                   uint32_t slots) {
    le_object_events *fresh;
    uint32_t i;
    uint32_t old;

    if (pw == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (slots <= pw->events_cap) {
        return LE_SUCCESS;
    }
    {
        uint32_t grown =
            (pw->events_cap == 0) ? 64u : pw->events_cap * 2u;

        while (grown < slots) {
            if (grown > 0x00FFFFFFu / 2u) {
                grown = slots;
                break;
            }
            grown *= 2u;
        }
        if (grown < slots) {
            return LE_ERROR_OVERFLOW;
        }
        fresh = (le_object_events *)realloc(
            pw->events, (size_t)grown * sizeof(*fresh));
        if (fresh == NULL) {
            return LE_ERROR_OUT_OF_MEMORY;
        }
        old = pw->events_cap;
        pw->events = fresh;
        pw->events_cap = grown;
        for (i = old; i < grown; i++) {
            memset(&pw->events[i], 0, sizeof(pw->events[i]));
            pw->events[i].slot = i;
        }
        return LE_SUCCESS;
    }
}

/* Retire every trace of a slot: drop live contacts touching it
 * (this step's manifold is rebuilt next step anyway), erase
 * persistent overlaps (with EXIT emission for live partners —
 * destroying an overlapping object has defined exit behavior),
 * and clear its event ring. Safe for slots without physics. */
void le_physics_retire_slot(le_world *world, uint32_t slot) {
    struct le_physics_world *pw;
    uint32_t i;

    if (world == NULL || world->physics == NULL) {
        return;
    }
    pw = world->physics;
    if (pw->contacts != NULL) {
        uint32_t w = 0;

        for (i = 0; i < pw->contact_count; i++) {
            if (pw->contacts[i].slot_a == slot ||
                pw->contacts[i].slot_b == slot) {
                continue;
            }
            if (w != i) {
                pw->contacts[w] = pw->contacts[i];
            }
            w++;
        }
        pw->contact_count = w;
    }
    if (pw->prev_overlaps != NULL) {
        uint32_t w = 0;

        for (i = 0; i < pw->prev_count; i++) {
            le_overlap_rec *r = &pw->prev_overlaps[i];

            if (r->slot_a == slot || r->slot_b == slot) {
                /* Emit EXIT to the surviving partner when it
                 * is still alive (defined exit behavior). */
                uint32_t other =
                    (r->slot_a == slot) ? r->slot_b : r->slot_a;
                uint32_t other_gen =
                    (r->slot_a == slot) ? r->gen_b : r->gen_a;

                if (other < world->capacity &&
                    world->slots[other].alive &&
                    world->slots[other].generation ==
                        other_gen) {
                    le_collision_event ev;

                    memset(&ev, 0, sizeof(ev));
                    ev.type = r->was_trigger ? LE_TRIGGER_EXIT
                                             : LE_COLLISION_EXIT;
                    ev.self.index = other;
                    ev.self.generation = other_gen;
                    ev.self.world_tag = world->tag;
                    ev.other.index = slot;
                    ev.other.generation =
                        (r->slot_a == slot) ? r->gen_a
                                            : r->gen_b;
                    ev.other.world_tag = world->tag;
                    ev.is_trigger = r->was_trigger;
                    /* Queue directly (retire runs outside the
                     * step; queue helper lives in events.c). */
                    {
                        extern void le_physics_queue_event(
                            le_world *world, uint32_t slot,
                            const le_collision_event *ev);
                        le_physics_queue_event(world, other,
                                               &ev);
                    }
                }
                continue;
            }
            if (w != i) {
                pw->prev_overlaps[w] = pw->prev_overlaps[i];
            }
            w++;
        }
        pw->prev_count = w;
    }
    if (slot < pw->events_cap) {
        memset(&pw->events[slot], 0, sizeof(pw->events[slot]));
        pw->events[slot].slot = slot;
    }
}

/* Swap-remove a slot's body/collider entries (struct-blind hook
 * for object.c's component strip). */
void le_physics_remove_slot_components(le_world *world,
                                       uint32_t slot) {
    struct le_physics_world *pw;

    if (world == NULL || world->physics == NULL ||
        slot >= world->capacity) {
        return;
    }
    pw = world->physics;
    if ((world->slots[slot].present & LE_PRESENT_RIGID_BODY) !=
            0u &&
        world->slots[slot].body_index != LE_NO_LINK) {
        uint32_t bidx = (uint32_t)world->slots[slot].body_index;

        if (bidx < pw->body_count &&
            pw->bodies[bidx].slot == slot) {
            uint32_t last = pw->body_count - 1u;

            if (bidx != last) {
                pw->bodies[bidx] = pw->bodies[last];
                world->slots[pw->bodies[bidx].slot].body_index =
                    (int32_t)bidx;
            }
            pw->body_count--;
        }
    }
    if ((world->slots[slot].present & LE_PRESENT_COLLIDER) != 0u &&
        world->slots[slot].collider_index != LE_NO_LINK) {
        uint32_t cidx =
            (uint32_t)world->slots[slot].collider_index;

        if (cidx < pw->collider_count &&
            pw->colliders[cidx].slot == slot) {
            uint32_t last = pw->collider_count - 1u;

            if (cidx != last) {
                pw->colliders[cidx] = pw->colliders[last];
                world->slots[pw->colliders[cidx].slot]
                    .collider_index = (int32_t)cidx;
            }
            pw->collider_count--;
        }
    }
}
