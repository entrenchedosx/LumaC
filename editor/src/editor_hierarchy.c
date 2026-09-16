/* Hierarchy model: plain-data snapshot (roots ascending + DFS)
 * for a future outliner UI. Session-owned storage. */

#include <stdlib.h>
#include <string.h>

#include "internal/editor_internal.h"

uint32_t led_hierarchy_refresh(led_session *session) {
    uint32_t root_count = 0;
    uint32_t needed = 0;
    uint32_t written = 0;

    if (session == NULL || !led_is_attached(session)) {
        return 0;
    }
    {
        uint32_t live = le_world_get_object_count(session->edit_world);

        needed = (live > 0) ? live : 0;
    }
    if (needed == 0) {
        session->hier_count = 0;
        return 0;
    }
    if (needed > session->hier_cap) {
        led_hierarchy_node *nn = (led_hierarchy_node *)realloc(
            session->hier_nodes, needed * sizeof(*nn));

        if (nn == NULL) {
            return session->hier_count;
        }
        session->hier_nodes = nn;
        session->hier_cap = needed;
    }
    {
        /* Roots via the Phase 31 engine API. */
        uint32_t cap = needed;
        le_object *roots = NULL;
        uint32_t total = 0;

        roots = (le_object *)malloc(cap * sizeof(*roots));
        if (roots == NULL) {
            return session->hier_count;
        }
        if (le_world_get_roots(session->edit_world, roots, cap,
                               &total) != LE_SUCCESS) {
            free(roots);
            return session->hier_count;
        }
        root_count = total;
        {
            /* Iterative DFS: explicit stack of (handle, depth,
             * child_cursor). Bounded by live count. */
            typedef struct led_walk {
                le_object handle;
                uint32_t depth;
                uint32_t kid_total;
                uint32_t kid_next;
                le_object *kids;
            } led_walk;
            led_walk *stack = NULL;
            uint32_t top = 0;
            uint32_t i;

            stack = (led_walk *)calloc(
                (root_count > 0 ? root_count : 1) + needed,
                sizeof(*stack));
            if (stack == NULL) {
                free(roots);
                return session->hier_count;
            }
            for (i = 0; i < root_count && i < cap; i++) {
                stack[top].handle = roots[i];
                stack[top].depth = 0;
                stack[top].kid_total = 0;
                stack[top].kid_next = 0;
                stack[top].kids = NULL;
                top++;
            }
            /* Reverse so the first root pops first. */
            for (i = 0; i < top / 2; i++) {
                led_walk t = stack[i];

                stack[i] = stack[top - 1 - i];
                stack[top - 1 - i] = t;
            }
            while (top > 0 && written < needed) {
                led_walk *fr = &stack[top - 1];

                if (fr->kid_next == 0 && fr->kids == NULL) {
                    /* First visit: emit node, fetch children. */
                    uint32_t cc = le_object_get_child_count(
                        session->edit_world, &fr->handle);
                    led_hierarchy_node *node =
                        &session->hier_nodes[written++];

                    node->handle = fr->handle;
                    node->depth = fr->depth;
                    node->has_children = (cc > 0);
                    node->name = le_object_get_name(session->edit_world,
                                                    &fr->handle);
                    if (cc > 0) {
                        uint32_t total_kids = 0;

                        fr->kids = (le_object *)malloc(
                            cc * sizeof(le_object));
                        if (fr->kids == NULL) {
                            break;
                        }
                        if (le_object_get_children(
                                session->edit_world, &fr->handle,
                                fr->kids, cc,
                                &total_kids) != LE_SUCCESS) {
                            free(fr->kids);
                            fr->kids = NULL;
                            fr->kid_total = 0;
                        } else {
                            fr->kid_total = total_kids;
                        }
                    }
                }
                if (fr->kids != NULL &&
                    fr->kid_next < fr->kid_total) {
                    le_object kid =
                        fr->kids[fr->kid_next++];

                    if (top >= (root_count + needed)) {
                        break;
                    }
                    stack[top].handle = kid;
                    stack[top].depth = fr->depth + 1;
                    stack[top].kid_total = 0;
                    stack[top].kid_next = 0;
                    stack[top].kids = NULL;
                    top++;
                } else {
                    free(fr->kids);
                    top--;
                }
            }
            for (i = 0; i < top; i++) {
                free(stack[i].kids);
            }
            free(stack);
        }
        free(roots);
    }
    session->hier_count = written;
    return written;
}

const led_hierarchy_node *led_hierarchy_nodes(
    const led_session *session) {
    if (session == NULL || session->hier_count == 0) {
        return NULL;
    }
    return session->hier_nodes;
}

uint32_t led_hierarchy_count(const led_session *session) {
    if (session == NULL) {
        return 0;
    }
    return session->hier_count;
}
