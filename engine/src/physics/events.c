/*
 * Collision events (Phase 28): ENTER/STAY/EXIT + trigger
 * variants, deterministic order, generation-safe identity.
 *
 * Each step AFTER the solve: current contact pairs (canonical
 * slot_a <= slot_b + generations) diff against prev_overlaps:
 * new -> ENTER, continuing -> STAY, vanished -> EXIT. Both
 * partners get mirrored events (self/other swapped, normal
 * flipped for the B side). Destroyed slots were already pruned
 * by le_physics_retire_slot (with EXIT to survivors); the diff
 * additionally guards liveness so no event names a dead slot.
 * Per-object rings hold 64; overflow drops oldest and counts.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_engine/luma_engine.h"
#include "internal/engine_internal.h"
#include "physics/physics_internal.h"

void le_physics_queue_event(le_world *world, uint32_t slot,
                            const le_collision_event *ev) {
    struct le_physics_world *pw;

    if (world == NULL || world->physics == NULL || ev == NULL) {
        return;
    }
    pw = world->physics;
    if (slot >= pw->events_cap) {
        return;
    }
    {
        le_object_events *ring = &pw->events[slot];
        uint32_t tail;

        if (ring->count >= LE_PHYS_MAX_EVENTS_PER_OBJECT) {
            ring->head = (ring->head + 1u) %
                         LE_PHYS_MAX_EVENTS_PER_OBJECT;
            ring->count--;
            ring->dropped++;
            pw->events_dropped_total++;
        }
        tail = (ring->head + ring->count) %
               LE_PHYS_MAX_EVENTS_PER_OBJECT;
        ring->ring[tail].ev = *ev;
        ring->count++;
    }
}

static int le_overlap_find(struct le_physics_world *pw,
                           uint32_t sa, uint32_t sb, uint32_t ga,
                           uint32_t gb) {
    uint32_t i;

    for (i = 0; i < pw->prev_count; i++) {
        le_overlap_rec *r = &pw->prev_overlaps[i];

        if (r->slot_a == sa && r->slot_b == sb &&
            r->gen_a == ga && r->gen_b == gb) {
            return (int)i;
        }
    }
    return -1;
}

static int le_pair_live(le_world *world, uint32_t slot,
                        uint32_t gen) {
    if (slot >= world->capacity) {
        return 0;
    }
    if (!world->slots[slot].alive) {
        return 0;
    }
    return world->slots[slot].generation == gen;
}

void le_physics_emit_events(le_world *world) {
    struct le_physics_world *pw;
    uint32_t i;
    /* Next-frame overlap set (built fresh, swapped in). */
    le_overlap_rec *next = NULL;
    uint32_t next_cap = 0;
    uint32_t next_count = 0;

    if (world == NULL || world->physics == NULL) {
        return;
    }
    pw = world->physics;
    if (pw->contact_count > 0) {
        next_cap = pw->contact_count;
        next = (le_overlap_rec *)malloc((size_t)next_cap *
                                        sizeof(*next));
        if (next == NULL) {
            return; /* OOM: keep prev set (STAYs repeat once;
                     * safe, tested allocation-failure path) */
        }
    }
    for (i = 0; i < pw->contact_count; i++) {
        le_contact_point *cp = &pw->contacts[i];
        int staying;
        le_collision_event ea;
        le_collision_event eb;

        if (!le_pair_live(world, cp->slot_a, cp->gen_a) ||
            !le_pair_live(world, cp->slot_b, cp->gen_b)) {
            continue;
        }
        staying = le_overlap_find(pw, cp->slot_a, cp->slot_b,
                                  cp->gen_a, cp->gen_b) >= 0;
        memset(&ea, 0, sizeof(ea));
        ea.type = cp->is_trigger
                      ? (staying ? LE_TRIGGER_STAY
                                 : LE_TRIGGER_ENTER)
                      : (staying ? LE_COLLISION_STAY
                                 : LE_COLLISION_ENTER);
        ea.self.index = cp->slot_a;
        ea.self.generation = cp->gen_a;
        ea.self.world_tag = world->tag;
        ea.other.index = cp->slot_b;
        ea.other.generation = cp->gen_b;
        ea.other.world_tag = world->tag;
        memcpy(ea.normal, cp->normal, sizeof(ea.normal));
        memcpy(ea.point, cp->point, sizeof(ea.point));
        ea.penetration = cp->penetration;
        ea.is_trigger = cp->is_trigger;
        le_physics_queue_event(world, cp->slot_a, &ea);
        /* Mirror for B (normal flipped: B -> A). */
        eb = ea;
        eb.self = ea.other;
        eb.other = ea.self;
        eb.normal[0] = -ea.normal[0];
        eb.normal[1] = -ea.normal[1];
        eb.normal[2] = -ea.normal[2];
        le_physics_queue_event(world, cp->slot_b, &eb);
        if (next != NULL && next_count < next_cap) {
            next[next_count].slot_a = cp->slot_a;
            next[next_count].slot_b = cp->slot_b;
            next[next_count].gen_a = cp->gen_a;
            next[next_count].gen_b = cp->gen_b;
            next[next_count].was_trigger = cp->is_trigger;
            next_count++;
        }
    }
    /* EXITs: prev pairs absent from the new set (both partners
     * notified when still live; retire_slot already handled
     * destroyed slots). */
    for (i = 0; i < pw->prev_count; i++) {
        le_overlap_rec *r = &pw->prev_overlaps[i];
        uint32_t k;
        int found = 0;

        for (k = 0; k < next_count; k++) {
            if (next[k].slot_a == r->slot_a &&
                next[k].slot_b == r->slot_b &&
                next[k].gen_a == r->gen_a &&
                next[k].gen_b == r->gen_b) {
                found = 1;
                break;
            }
        }
        if (found) {
            continue;
        }
        {
            int la = le_pair_live(world, r->slot_a, r->gen_a);
            int lb = le_pair_live(world, r->slot_b, r->gen_b);
            le_collision_event e;

            if (!la && !lb) {
                continue;
            }
            memset(&e, 0, sizeof(e));
            e.type = r->was_trigger ? LE_TRIGGER_EXIT
                                    : LE_COLLISION_EXIT;
            e.is_trigger = r->was_trigger;
            if (la) {
                e.self.index = r->slot_a;
                e.self.generation = r->gen_a;
                e.self.world_tag = world->tag;
                e.other.index = r->slot_b;
                e.other.generation = r->gen_b;
                e.other.world_tag = world->tag;
                le_physics_queue_event(world, r->slot_a, &e);
            }
            if (lb) {
                e.self.index = r->slot_b;
                e.self.generation = r->gen_b;
                e.self.world_tag = world->tag;
                e.other.index = r->slot_a;
                e.other.generation = r->gen_a;
                e.other.world_tag = world->tag;
                le_physics_queue_event(world, r->slot_b, &e);
            }
        }
    }
    free(pw->prev_overlaps);
    pw->prev_overlaps = next;
    pw->prev_cap = next_cap;
    pw->prev_count = next_count;
}

le_result le_physics_drain_events(le_world *world,
                                  const le_object *object,
                                  le_collision_event *out,
                                  uint32_t cap,
                                  uint32_t *out_count) {
    uint32_t slot;
    le_result code = LE_SUCCESS;
    struct le_physics_world *pw;
    le_object_events *ring;

    if (out_count != NULL) {
        *out_count = 0;
    }
    if (world == NULL || object == NULL) {
        return LE_ERROR_INVALID_ARGUMENT;
    }
    if (world->physics == NULL) {
        return LE_ERROR_NOT_INITIALIZED;
    }
    if (!le_resolve_live(world, object, &slot, &code)) {
        return code;
    }
    pw = world->physics;
    if (slot >= pw->events_cap) {
        return LE_SUCCESS;
    }
    ring = &pw->events[slot];
    if (out_count != NULL) {
        *out_count = ring->count;
    }
    if (out != NULL) {
        uint32_t n =
            (cap < ring->count) ? cap : ring->count;
        uint32_t k;

        for (k = 0; k < n; k++) {
            out[k] =
                ring->ring[(ring->head + k) %
                           LE_PHYS_MAX_EVENTS_PER_OBJECT]
                    .ev;
        }
        /* Draining consumes (prefix-compaction for partial). */
        if (n == ring->count) {
            ring->head = 0;
            ring->count = 0;
        } else {
            ring->head = (ring->head + n) %
                         LE_PHYS_MAX_EVENTS_PER_OBJECT;
            ring->count -= n;
        }
    }
    return LE_SUCCESS;
}
