/*
 * Vulkan worker recording (Phase 20, PARTs A–E, I–L, 26, 28).
 *
 * Worker encoders own one command pool each (graphics family) and
 * record secondary buffers concurrently on separate threads — no
 * global recording mutex. Lists execute deterministically inside a
 * frame primary's open pass on the SAME target object.
 *
 * State discipline (PARTs I–L): workers never mutate global
 * tracking. Transitions record barriers from tracked states read
 * under the state shard and log (from → to) intents; binds log
 * read assumptions. Execute reconciles in order under the state
 * shard: mismatches fail loudly before anything further records.
 * Destroyed referents poison lists via the registry (PART 26).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

/* ------------------------------------------------------------------ */
/* Liveness + registry.                                                */
/* ------------------------------------------------------------------ */

int lc_vk_worker_live(const lc_command_encoder *enc) {
    lc_state *state = lc_get_internal_state();
    const lc_device *device;

    if (state == NULL || enc == NULL) {
        return 0;
    }
    /* Candidate pointers are untrusted API input: locate by identity
     * before dereferencing. This preserves the established sentinel/dead
     * handle behavior used throughout the public API. */
    for (device = state->devices; device != NULL; device = device->next) {
        const lc_command_encoder *it;
        int found = 0;

        lc_device_lock_transfer((lc_device *)device);
        for (it = device->worker_encoders; it != NULL;
             it = it->worker_next) {
            if (it == enc) {
                found = 1;
                break;
            }
        }
        lc_device_unlock_transfer((lc_device *)device);
        if (found) {
            return 1;
        }
    }
    return 0;
}

static lc_cmdlist_record *lc_worker_find_record(
    lc_device *device, const lc_command_list *list) {
    lc_cmdlist_record *it;

    if (device == NULL || list == NULL) {
        return NULL;
    }
    for (it = device->cmdlists; it != NULL; it = it->next) {
        if (it->list == list) {
            return it;
        }
    }
    return NULL;
}

void lc_vk_cmdlist_register(lc_device *device, lc_cmdlist_record *rec) {
    if (device == NULL || rec == NULL) {
        return;
    }
    rec->next = device->cmdlists;
    rec->prev = NULL;
    if (device->cmdlists != NULL) {
        device->cmdlists->prev = rec;
    }
    device->cmdlists = rec;
}

void lc_vk_cmdlist_unregister(lc_device *device,
                              lc_cmdlist_record *rec) {
    if (device == NULL || rec == NULL) {
        return;
    }
    if (rec->prev != NULL) {
        rec->prev->next = rec->next;
    } else if (device->cmdlists == rec) {
        device->cmdlists = rec->next;
    }
    if (rec->next != NULL) {
        rec->next->prev = rec->prev;
    }
    rec->next = NULL;
    rec->prev = NULL;
}

void lc_vk_cmdlist_poison_for(lc_device *device, const void *handle) {
    lc_cmdlist_record *it;

    if (device == NULL || handle == NULL) {
        return;
    }
    lc_device_lock_transfer(device);
    for (it = device->cmdlists; it != NULL; it = it->next) {
        lc_command_list *list = it->list;
        uint32_t i;

        if (list == NULL || !it->valid) {
            continue;
        }
        for (i = 0; i < list->ref_count; i++) {
            if (list->refs[i] == handle) {
                it->valid = 0;
                break;
            }
        }
    }
    lc_device_unlock_transfer(device);
}

/* ------------------------------------------------------------------ */
/* Log + ref helpers.                                                  */
/* ------------------------------------------------------------------ */

static lc_result lc_worker_log_ref(lc_command_list *list,
                                   const void *handle) {
    uint32_t i;

    if (handle == NULL) {
        return LC_SUCCESS;
    }
    for (i = 0; i < list->ref_count; i++) {
        if (list->refs[i] == handle) {
            return LC_SUCCESS;
        }
    }
    if (list->ref_count == list->ref_capacity) {
        uint32_t grown = (list->ref_capacity == 0) ? 8u
                                                  : list->ref_capacity * 2u;
        const void **refs = (const void **)realloc(
            (void *)list->refs, sizeof(const void *) * grown);

        if (refs == NULL) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
        list->refs = refs;
        list->ref_capacity = grown;
    }
    list->refs[list->ref_count++] = handle;
    return LC_SUCCESS;
}

static lc_result lc_worker_log_intent(lc_command_list *list,
                                      int is_transition, lc_image *image,
                                      uint32_t base_mip,
                                      uint32_t level_count,
                                      uint32_t base_layer,
                                      uint32_t layer_count,
                                      lc_resource_state from,
                                      lc_resource_state to) {
    lc_list_log_entry *grown = NULL;

    if (list->log_count == list->log_capacity) {
        uint32_t cap =
            (list->log_capacity == 0) ? 8u : list->log_capacity * 2u;

        grown = (lc_list_log_entry *)realloc(
            list->log, sizeof(lc_list_log_entry) * cap);
        if (grown == NULL) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
        list->log = grown;
        list->log_capacity = cap;
    }
    list->log[list->log_count].is_transition = is_transition;
    list->log[list->log_count].is_buffer = 0;
    list->log[list->log_count].image = image;
    list->log[list->log_count].buffer = NULL;
    list->log[list->log_count].base_mip = base_mip;
    list->log[list->log_count].level_count = level_count;
    list->log[list->log_count].base_layer = base_layer;
    list->log[list->log_count].layer_count = layer_count;
    list->log[list->log_count].from = from;
    list->log[list->log_count].to = to;
    list->log_count++;
    return lc_worker_log_ref(list, image);
}

/* Buffer transition intent (transfer shard owns buffer_state; the
 * caller holds it for the snapshot, releases to record, and the
 * reconcile pass below re-takes it — single-locked phases only). */
static lc_result lc_worker_log_buffer_intent(lc_command_list *list,
                                             lc_buffer *buffer,
                                             lc_resource_state from,
                                             lc_resource_state to) {
    lc_list_log_entry *grown = NULL;

    if (list->log_count == list->log_capacity) {
        uint32_t cap =
            (list->log_capacity == 0) ? 8u : list->log_capacity * 2u;

        grown = (lc_list_log_entry *)realloc(
            list->log, sizeof(lc_list_log_entry) * cap);
        if (grown == NULL) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
        list->log = grown;
        list->log_capacity = cap;
    }
    list->log[list->log_count].is_transition = 1;
    list->log[list->log_count].is_buffer = 1;
    list->log[list->log_count].image = NULL;
    list->log[list->log_count].buffer = buffer;
    list->log[list->log_count].base_mip = 0;
    list->log[list->log_count].level_count = 0;
    list->log[list->log_count].base_layer = 0;
    list->log[list->log_count].layer_count = 0;
    list->log[list->log_count].from = from;
    list->log[list->log_count].to = to;
    list->log_count++;
    return lc_worker_log_ref(list, buffer);
}

/* ------------------------------------------------------------------ */
/* Encoder lifecycle.                                                  */
/* ------------------------------------------------------------------ */

lc_result lc_vulkan_worker_create(lc_device *device, lc_queue_type queue,
                                  lc_command_encoder **out) {
    lc_command_encoder *enc = NULL;
    VkCommandPoolCreateInfo pool_info;

    if (device == NULL || out == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Phase 21: COMPUTE workers record on the graphics family pool
     * (dispatch executes on the graphics queue — baseline
     * compatibility path). A compute-family pool for cross-queue
     * recording is future async-compute work. */
    if (queue != LC_QUEUE_GRAPHICS && queue != LC_QUEUE_COMPUTE) {
        return LC_ERROR_UNSUPPORTED;
    }
    if (queue == LC_QUEUE_COMPUTE && !device->compute_supported) {
        return LC_ERROR_UNSUPPORTED;
    }
    if (device->device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    enc = (lc_command_encoder *)calloc(1, sizeof(lc_command_encoder));
    if (enc == NULL) {
        *out = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = device->graphics_queue_family;
    if (vkCreateCommandPool(device->device, &pool_info, NULL,
                            &enc->worker_pool) != VK_SUCCESS) {
        enc->worker_pool = VK_NULL_HANDLE;
        free(enc);
        *out = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    enc->worker_mode = 1;
    enc->worker_device = device;
    enc->device = device;
    /* Link into the device worker list (liveness validation). */
    lc_device_lock_transfer(device);
    enc->worker_next = device->worker_encoders;
    enc->worker_prev = NULL;
    if (device->worker_encoders != NULL) {
        device->worker_encoders->worker_prev = enc;
    }
    device->worker_encoders = enc;
    lc_device_unlock_transfer(device);
    *out = enc;
    return LC_SUCCESS;
}

void lc_vulkan_worker_destroy(lc_command_encoder *enc) {
    lc_device *device;

    if (enc == NULL || !enc->worker_mode) {
        return;
    }
    device = enc->worker_device;
    /* Poison live lists first (execute-afterwards fails safely);
     * the pool survives until the last list dies (refcount). */
    if (device != NULL) {
        lc_cmdlist_record *it;

        lc_device_lock_transfer(device);
        for (it = device->cmdlists; it != NULL; it = it->next) {
            if (it->encoder == enc) {
                it->valid = 0;
            }
        }
        if (enc->worker_prev != NULL) {
            enc->worker_prev->worker_next = enc->worker_next;
        } else if (device->worker_encoders == enc) {
            device->worker_encoders = enc->worker_next;
        }
        if (enc->worker_next != NULL) {
            enc->worker_next->worker_prev = enc->worker_prev;
        }
        lc_device_unlock_transfer(device);
    }
    enc->worker_next = NULL;
    enc->worker_prev = NULL;
    if (enc->worker_live_lists == 0) {
        if (enc->worker_pool != VK_NULL_HANDLE && device != NULL &&
            device->device != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device->device, enc->worker_pool,
                                 NULL);
        }
        free(enc);
    } else {
        /* Zombie: struct + pool outlive outstanding lists. */
        enc->worker_zombie = 1;
    }
}

/* Drop one list reference; free the zombie encoder + pool when the
 * last list dies. */
static void lc_worker_list_released(lc_command_encoder *enc) {
    lc_device *device;

    if (enc == NULL || enc->worker_live_lists == 0) {
        return;
    }
    enc->worker_live_lists--;
    if (!enc->worker_zombie || enc->worker_live_lists != 0) {
        return;
    }
    device = enc->worker_device;
    if (enc->worker_pool != VK_NULL_HANDLE && device != NULL &&
        device->device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device->device, enc->worker_pool, NULL);
    }
    enc->worker_pool = VK_NULL_HANDLE;
    free(enc);
}

/* ------------------------------------------------------------------ */
/* List lifecycle.                                                     */
/* ------------------------------------------------------------------ */

lc_result lc_vulkan_worker_begin(lc_command_encoder *enc,
                                 lc_render_target *target,
                                 const lc_render_pass_desc *desc) {
    VkCommandBufferAllocateInfo alloc_info;
    VkCommandBufferInheritanceInfo inherit;
    VkCommandBufferBeginInfo begin_info;
    VkRenderPass pass = VK_NULL_HANDLE;
    lc_device *device;
    lc_result res;

    if (enc == NULL || target == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = enc->worker_device;
    if (device == NULL || device->device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    if (enc->worker_list != NULL) {
        return LC_ERROR_INVALID_ARGUMENT; /* one list at a time */
    }
    if (target->is_swapchain_borrow) {
        return LC_ERROR_UNSUPPORTED; /* presentation stays primary */
    }
    if (target->device != device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Same view/target/extent rules as explicit passes. */
    if (desc->color_attachment_count != target->color_count ||
        desc->width != target->width ||
        desc->height != target->height) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    {
        uint32_t i;

        for (i = 0; i < desc->color_attachment_count; i++) {
            if (desc->color_attachments[i].view !=
                target->color_views[i]) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    /* Render pass + framebuffer for inheritance (shared cache). */
    res = lc_vulkan_offscreen_pass(device, target, desc, &pass);
    if (res != LC_SUCCESS || pass == VK_NULL_HANDLE) {
        return res;
    }
    {
        lc_command_list *list = NULL;

        list = (lc_command_list *)calloc(1, sizeof(lc_command_list));
        if (list == NULL) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
        memset(&alloc_info, 0, sizeof(alloc_info));
        alloc_info.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        alloc_info.commandPool = enc->worker_pool;
        alloc_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        alloc_info.commandBufferCount = 1;
        lc_device_lock_transfer(device);
        if (vkAllocateCommandBuffers(device->device, &alloc_info,
                                     &list->cmd) != VK_SUCCESS) {
            lc_device_unlock_transfer(device);
            free(list);
            return LC_ERROR_OUT_OF_MEMORY;
        }
        lc_device_unlock_transfer(device);
        list->device = device;
        list->resource_id = lc_issue_resource_id();
        list->pool = enc->worker_pool;
        list->queue_family = device->graphics_queue_family;
        list->target = target;
        list->pass_desc = *desc;
        /* Copy attachment storage (desc borrows caller memory). */
        if (desc->color_attachment_count > 0) {
            uint32_t i;

            for (i = 0;
                 i < desc->color_attachment_count && i < LC_MAX_COLOR_ATTACHMENTS;
                 i++) {
                list->pass_color[i] = desc->color_attachments[i];
            }
            list->pass_desc.color_attachments = list->pass_color;
        }
        if (desc->depth_attachment != NULL) {
            list->pass_depth = *desc->depth_attachment;
            list->pass_desc.depth_attachment = &list->pass_depth;
            list->has_depth = 1;
        }
        memset(&inherit, 0, sizeof(inherit));
        inherit.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
        inherit.renderPass = pass;
        inherit.subpass = 0;
        inherit.framebuffer = target->framebuffer;
        memset(&begin_info, 0, sizeof(begin_info));
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags =
            VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT |
            VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
        begin_info.pInheritanceInfo = &inherit;
        if (vkBeginCommandBuffer(list->cmd, &begin_info) !=
            VK_SUCCESS) {
            lc_device_lock_transfer(device);
            vkFreeCommandBuffers(device->device, enc->worker_pool, 1,
                                 &list->cmd);
            lc_device_unlock_transfer(device);
            free(list);
            return LC_ERROR_UNKNOWN;
        }
        /* Dynamic state is command-buffer local and is not inherited
         * from the primary command buffer. */
        {
            VkViewport viewport;
            VkRect2D scissor;

            memset(&viewport, 0, sizeof(viewport));
            viewport.width = (float)target->width;
            viewport.height = (float)target->height;
            viewport.maxDepth = 1.0f;
            memset(&scissor, 0, sizeof(scissor));
            scissor.extent.width = target->width;
            scissor.extent.height = target->height;
            vkCmdSetViewport(list->cmd, 0, 1, &viewport);
            vkCmdSetScissor(list->cmd, 0, 1, &scissor);
        }
        enc->worker_list = list;
        enc->bound_pipeline = NULL;
        enc->bound_index_buffer = NULL;
        enc->index_bound = 0;
        enc->worker_live_lists++;
        if (lc_worker_log_ref(list, target) != LC_SUCCESS) {
            lc_device_lock_transfer(device);
            vkFreeCommandBuffers(device->device, enc->worker_pool, 1,
                                 &list->cmd);
            lc_device_unlock_transfer(device);
            enc->worker_list = NULL;
            enc->worker_live_lists--;
            free(list);
            return LC_ERROR_OUT_OF_MEMORY;
        }
        lc_device_lock_transfer(device);
        device->stat_lists_in_flight++;
        lc_device_unlock_transfer(device);
        return LC_SUCCESS;
    }
}

lc_result lc_vulkan_worker_finish(lc_command_encoder *enc,
                                  lc_command_list **out_list) {
    lc_command_list *list;
    lc_cmdlist_record *rec;
    lc_device *device;

    if (enc == NULL || out_list == NULL) {
        if (out_list != NULL) {
            *out_list = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    list = enc->worker_list;
    if (list == NULL) {
        *out_list = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = enc->worker_device;
    if (vkEndCommandBuffer(list->cmd) != VK_SUCCESS) {
        *out_list = NULL;
        return LC_ERROR_UNKNOWN;
    }
    rec = (lc_cmdlist_record *)calloc(1, sizeof(lc_cmdlist_record));
    if (rec == NULL) {
        *out_list = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    rec->list = list;
    rec->encoder = enc;
    rec->valid = 1;
    lc_device_lock_transfer(device);
    lc_vk_cmdlist_register(device, rec);
    lc_device_unlock_transfer(device);
    enc->worker_list = NULL;
    *out_list = list;
    return LC_SUCCESS;
}

void lc_vulkan_worker_list_destroy(lc_command_list *list) {
    lc_device *device;
    lc_command_encoder *owner = NULL;
    lc_cmdlist_record *rec;

    if (list == NULL) {
        return;
    }
    device = list->device;
    if (device != NULL) {
        int defer = 0;

        lc_device_lock_transfer(device);
        rec = lc_worker_find_record(device, list);
        if (rec != NULL && list->executed) {
            if (device->timeline_ok) {
                defer = (list->completion_value == UINT64_MAX ||
                         (list->completion_value != 0 &&
                          !lc_vk_signal_ready(
                              device, list->completion_value)))
                            ? 1
                            : 0;
            } else if (device->device == VK_NULL_HANDLE) {
                defer = 0; /* dead device: pools are gone, just free */
            } else {
                /* Binary fallback (Phase 22, PART F): defer until
                 * the bound flight fence signals. Unbound means
                 * never submitted; bound-but-fenceless means the
                 * flight died idle (teardown contract) — both
                 * complete. */
                defer = (!list->fallback_bound ||
                         (list->fallback_fence != VK_NULL_HANDLE &&
                          vkGetFenceStatus(device->device,
                                           list->fallback_fence) !=
                              VK_SUCCESS))
                            ? 1
                            : 0;
            }
        }
        if (defer) {
            /* VkCommandBuffer lifetime extends through execution.  Keep the
             * worker pool/list alive and make the public handle unusable;
             * the non-blocking completion poll performs the real free. */
            list->destroy_requested = 1;
            rec->valid = 0;
            lc_device_unlock_transfer(device);
            return;
        }
        if (rec != NULL) {
            owner = rec->encoder;
            lc_vk_cmdlist_unregister(device, rec);
            free(rec);
        }
        if (list->cmd != VK_NULL_HANDLE &&
            device->device != VK_NULL_HANDLE) {
            /* Pool may belong to a zombie encoder; freeing the
             * buffer back is still safe (pool outlives lists). */
            VkCommandPool pool = list->pool;

            if (pool != VK_NULL_HANDLE) {
                vkFreeCommandBuffers(device->device, pool, 1,
                                     &list->cmd);
            }
        }
        if (device->stat_lists_in_flight > 0) {
            device->stat_lists_in_flight--;
        }
        lc_device_unlock_transfer(device);
    }
    free(list->log);
    free((void *)list->refs);
    free(list);
    if (owner != NULL) {
        lc_worker_list_released(owner);
    }
}

void lc_vk_cmdlist_bind_active_frame(lc_device *device, uint64_t value) {
    lc_cmdlist_record *it;

    if (device == NULL) {
        return;
    }
    lc_device_lock_transfer(device);
    for (it = device->cmdlists; it != NULL; it = it->next) {
        if (it->list != NULL && it->list->executed &&
            it->list->completion_value == UINT64_MAX) {
            it->list->completion_value = value;
        }
    }
    lc_device_unlock_transfer(device);
}

/* Binary-fallback completion binding (Phase 22, PART F): stamp the
 * submitting flight's fence on executed-but-unbound lists. The
 * fence is borrowed (owned by the flight); swapchain teardown
 * resolves bound lists before destroying fences (see
 * lc_vk_cmdlist_release_flight), so it never dangles past use. */
void lc_vk_cmdlist_bind_fallback_fence(lc_device *device,
                                       VkFence fence) {
    lc_cmdlist_record *it;

    if (device == NULL || fence == VK_NULL_HANDLE) {
        return;
    }
    lc_device_lock_transfer(device);
    for (it = device->cmdlists; it != NULL; it = it->next) {
        if (it->list != NULL && it->list->executed &&
            !it->list->fallback_bound) {
            it->list->fallback_fence = fence;
            it->list->fallback_bound = 1;
        }
    }
    lc_device_unlock_transfer(device);
}

/* Swapchain teardown hook (Phase 22, PART F): the flights array is
 * about to die with its fences. The teardown contract guarantees
 * no in-flight submissions, so every bound list is complete:
 * destroy-requested ones free now; live ones lose their fence but
 * keep bound=1 (treated as complete at their destroy). */
void lc_vk_cmdlist_release_flight(lc_device *device,
                                  const VkFence *fences,
                                  uint32_t fence_count) {
    lc_cmdlist_record *it;
    uint32_t i;

    if (device == NULL || fences == NULL || fence_count == 0) {
        return;
    }
    /* Destroy under the same hold (PART C: no select/destroy race
     * with concurrent reclaim or destroy calls). */
    lc_device_lock_transfer(device);
    for (it = device->cmdlists; it != NULL;) {
        lc_command_list *list = it->list;
        lc_cmdlist_record *next = it->next;
        int match = 0;

        if (list != NULL && list->fallback_bound &&
            list->fallback_fence != VK_NULL_HANDLE) {
            for (i = 0; i < fence_count; i++) {
                if (list->fallback_fence == fences[i]) {
                    match = 1;
                    break;
                }
            }
        }
        if (match) {
            list->fallback_fence = VK_NULL_HANDLE;
            if (list->destroy_requested) {
                lc_vulkan_worker_list_destroy(list);
            }
        }
        it = next;
    }
    lc_device_unlock_transfer(device);
    /* Live (non-requested) lists keep bound=1 with a NULL fence:
     * teardown was idle, so a later destroy treats them as
     * complete. No leak, no dangling fence. */
}

/* Reclaim destroy-requested lists whose completion is proven
 * (Phase 22, PART F: timeline values where available, bound
 * flight fences in binary-fallback mode). Non-blocking; shutdown
 * forces the rest. The transfer shard is held across select AND
 * destroy (Phase 22, PART C): concurrent reclaim/poll/destroy
 * calls serialize here, so a list can never be freed twice.
 * Worker-pool vkFree calls are transfer-domain by design. */
void lc_vk_cmdlist_reclaim_completed(lc_device *device) {
    if (device == NULL) {
        return;
    }
    lc_device_lock_transfer(device);
    for (;;) {
        lc_cmdlist_record *it;
        lc_command_list *due = NULL;

        for (it = device->cmdlists; it != NULL; it = it->next) {
            lc_command_list *list = it->list;
            int ready = 0;

            if (list == NULL || !list->destroy_requested) {
                continue;
            }
            if (device->timeline_ok) {
                ready = (list->completion_value != UINT64_MAX &&
                         lc_vk_signal_ready(device,
                                            list->completion_value))
                            ? 1
                            : 0;
            } else if (device->device == VK_NULL_HANDLE) {
                ready = 1;
            } else {
                ready = (list->fallback_bound &&
                         (list->fallback_fence == VK_NULL_HANDLE ||
                          vkGetFenceStatus(device->device,
                                           list->fallback_fence) ==
                              VK_SUCCESS))
                            ? 1
                            : 0;
            }
            if (ready) {
                due = list;
                break;
            }
        }
        if (due == NULL) {
            break;
        }
        lc_vulkan_worker_list_destroy(due);
    }
    lc_device_unlock_transfer(device);
}

void lc_vk_worker_shutdown(lc_device *device) {
    if (device == NULL) {
        return;
    }
    /* The transfer shutdown has already waited the device idle. */
    while (device->cmdlists != NULL) {
        lc_command_list *list = device->cmdlists->list;

        if (list == NULL) {
            break;
        }
        list->executed = 0;
        lc_vulkan_worker_list_destroy(list);
    }
    while (device->worker_encoders != NULL) {
        lc_vulkan_worker_destroy(device->worker_encoders);
    }
}

/* Structural pipeline-vs-list-pass compatibility (extent ignored,
 * mirroring lc_enc_compat): the list side rebuilds from the
 * borrowed target's structural signature. */
static int lc_worker_compat(const lc_command_list *list,
                            const lc_pipeline *pipeline) {
    lc_render_target_desc pipe_desc;
    lc_render_target_desc list_desc;
    uint32_t i;

    if (list == NULL || pipeline == NULL || list->target == NULL) {
        return 0;
    }
    if (pipeline->target_hash != list->target->hash) {
        return 0;
    }
    memset(&pipe_desc, 0, sizeof(pipe_desc));
    pipe_desc.color_attachment_count = pipeline->target_color_count;
    for (i = 0; i < pipeline->target_color_count &&
                i < LC_MAX_COLOR_ATTACHMENTS;
         i++) {
        pipe_desc.color_formats[i] = pipeline->target_color_formats[i];
    }
    pipe_desc.depth_stencil_format = pipeline->target_depth_format;
    pipe_desc.samples = pipeline->target_samples;
    memset(&list_desc, 0, sizeof(list_desc));
    list_desc.color_attachment_count = list->target->color_count;
    for (i = 0; i < list->target->color_count &&
                i < LC_MAX_COLOR_ATTACHMENTS;
         i++) {
        list_desc.color_formats[i] = list->target->color_formats[i];
    }
    list_desc.depth_stencil_format = list->target->depth_format;
    list_desc.samples = list->target->samples;
    return lc_render_target_desc_equal(&list_desc, &pipe_desc);
}

static lc_command_list *lc_worker_open(lc_command_encoder *enc) {
    if (enc == NULL || !enc->worker_mode) {
        return NULL;
    }
    return enc->worker_list;
}

lc_result lc_worker_record_bind_pipeline(lc_command_encoder *enc,
                                         const lc_pipeline *pipeline) {
    lc_command_list *list = lc_worker_open(enc);
    lc_device *device;

    if (list == NULL || pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = list->device;
    if (pipeline->device != device ||
        pipeline->pipeline == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_worker_compat(list, pipeline)) {
        return LC_ERROR_PIPELINE_INCOMPATIBLE;
    }
    vkCmdBindPipeline(list->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                       pipeline->pipeline);
    enc->bound_pipeline = pipeline;
    return lc_worker_log_ref(list, pipeline);
}

lc_result lc_worker_record_bind_set(lc_command_encoder *enc,
                                    const lc_pipeline *pipeline,
                                    uint32_t slot,
                                    lc_binding_set *set) {
    lc_command_list *list = lc_worker_open(enc);
    lc_result res = LC_SUCCESS;
    uint32_t i;

    if (list == NULL || pipeline == NULL || set == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (slot >= pipeline->layout_count ||
        set->vk_set == VK_NULL_HANDLE || pipeline->layout == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (set->device != list->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Canonical layout match (contents, like the frame path). */
    {
        uint32_t n = 0;

        if (slot < pipeline->layout_count) {
            n = pipeline->slot_signature_counts[slot];
        }
        if (n != set->slot_count ||
            !lc_binding_signature_equal(
                pipeline->slot_signatures[slot], n, set->slots,
                set->slot_count)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    vkCmdBindDescriptorSets(list->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline->layout, slot, 1, &set->vk_set, 0,
                            NULL);
    res = lc_worker_log_ref(list, set);
    if (res != LC_SUCCESS) {
        return res;
    }
    /* Log sampled-image reads for execute-time verification. */
    for (i = 0; i < set->slot_count && res == LC_SUCCESS; i++) {
        /* Slots carry only their descriptors; views are not stored
         * per set — the BIND call site knows them. Re-derive: binds
         * were validated at set-update time against live views, but
         * the set does not retain view pointers. Log the set only;
         * image-state assumptions are covered by transition intents
         * and pass-boundary reconciliation (documented boundary:
         * cross-list read/write hazards without ordering fail at
         * the transition check, and sampling an image whose state
         * moved after record is an application ordering bug, as in
         * raw Vulkan). */
        (void)i;
    }
    return res;
}

lc_result lc_worker_record_transition(
    lc_command_encoder *enc, lc_image *image, uint32_t base_mip,
    uint32_t level_count, uint32_t base_layer, uint32_t layer_count,
    lc_resource_state new_state) {
    lc_command_list *list = lc_worker_open(enc);
    lc_device *device;
    lc_resource_state from = LC_RESOURCE_STATE_UNDEFINED;
    uint32_t mip;
    uint32_t layer;
    lc_result res;

    if (list == NULL || image == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = list->device;
    if (image->device != device ||
        image->vk_image == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_state_valid_for_image(new_state)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_validate_range(image, base_mip, level_count,
                                   base_layer, layer_count)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Read current tracked states under the shard (races with other
     * recorders resolve to SOME consistent snapshot; execution
     * re-verifies). All covered entries must agree. */
    lc_device_lock_state(device);
    from = image->states[(size_t)base_layer * image->mip_levels +
                         base_mip];
    for (layer = base_layer; layer < base_layer + layer_count;
         layer++) {
        for (mip = base_mip; mip < base_mip + level_count; mip++) {
            if (image->states[(size_t)layer * image->mip_levels +
                              mip] != from) {
                lc_device_unlock_state(device);
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    lc_device_unlock_state(device);
    /* Barrier with the observed old state; NO global mark (local
     * intent only — execute reconciles). */
    res = lc_vk_sync_record_span(list->cmd, image, base_mip,
                                 level_count, base_layer, layer_count,
                                 from, new_state);
    if (res != LC_SUCCESS) {
        return res;
    }
    return lc_worker_log_intent(list, 1, image, base_mip, level_count,
                                base_layer, layer_count, from,
                                new_state);
}

/* Reconcile one list's intents in execution order (state shard
 * held by the caller): transitions require global == from (else
 * CONFLICT), then apply; bind-reads require SHADER_READ. Marks
 * stamp the submission sequence (PART L epochs). */
static lc_result lc_worker_reconcile(lc_device *device,
                                     lc_command_list *list) {
    uint32_t i;

    for (i = 0; i < list->log_count; i++) {
        lc_list_log_entry *e = &list->log[i];
        uint32_t mip;
        uint32_t layer;

        if (e->is_buffer) {
            /* Buffer intents reconcile in the transfer-shard pass
             * below (lc_worker_reconcile_buffers); image states
             * must not be touched here. */
            continue;
        }
        if (!lc_vk_sync_validate_range(e->image, e->base_mip,
                                       e->level_count, e->base_layer,
                                       e->layer_count)) {
            fprintf(stderr,
                    "[lumac] execute conflict: list range went stale\n");
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (e->is_transition) {
            for (layer = e->base_layer;
                 layer < e->base_layer + e->layer_count; layer++) {
                for (mip = e->base_mip;
                     mip < e->base_mip + e->level_count; mip++) {
                    size_t idx = (size_t)layer *
                                     e->image->mip_levels +
                                 mip;

                    if (e->image->states[idx] != e->from) {
                        fprintf(stderr,
                                "[lumac] execute conflict: image %p "
                                "mip %u layer %u is state %d, list "
                                "assumed %d\n",
                                (void *)e->image, mip, layer,
                                (int)e->image->states[idx],
                                (int)e->from);
                        return LC_ERROR_INVALID_ARGUMENT;
                    }
                }
            }
            lc_vk_sync_mark(e->image, e->base_mip, e->level_count,
                            e->base_layer, e->layer_count, e->to);
            device->submission_seq++;
            for (layer = e->base_layer;
                 layer < e->base_layer + e->layer_count; layer++) {
                for (mip = e->base_mip;
                     mip < e->base_mip + e->level_count; mip++) {
                    e->image->epochs[(size_t)layer *
                                         e->image->mip_levels +
                                     mip] = device->submission_seq;
                }
            }
        } else {
            /* Bind-read assumption: SHADER_READ required now. */
            for (layer = e->base_layer;
                 layer < e->base_layer + e->layer_count; layer++) {
                for (mip = e->base_mip;
                     mip < e->base_mip + e->level_count; mip++) {
                    size_t idx = (size_t)layer *
                                     e->image->mip_levels +
                                 mip;

                    if (e->image->states[idx] !=
                        LC_RESOURCE_STATE_SHADER_READ) {
                        fprintf(stderr,
                                "[lumac] execute conflict: sampled "
                                "image %p mip %u layer %u is state "
                                "%d, need SHADER_READ\n",
                                (void *)e->image, mip, layer,
                                (int)e->image->states[idx]);
                        return LC_ERROR_INVALID_ARGUMENT;
                    }
                }
            }
        }
    }
    return LC_SUCCESS;
}

/* Reconcile one list's BUFFER intents in execution order (transfer
 * shard held by the caller): transitions require global == from
 * (else CONFLICT), then apply. Image intents are handled by
 * lc_worker_reconcile under the state shard; the two passes run
 * back-to-back before any vkCmdExecuteCommands, so a conflict in
 * either still executes nothing. */
static lc_result lc_worker_reconcile_buffers(lc_device *device,
                                             lc_command_list *list) {
    uint32_t i;

    (void)device;
    for (i = 0; i < list->log_count; i++) {
        lc_list_log_entry *e = &list->log[i];

        if (!e->is_buffer) {
            continue;
        }
        if (e->buffer == NULL) {
            fprintf(stderr,
                    "[lumac] execute conflict: buffer intent went "
                    "stale\n");
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (!e->is_transition) {
            continue;
        }
        if (e->buffer->buffer_state != e->from) {
            fprintf(stderr,
                    "[lumac] execute conflict: buffer %p is state %d, "
                    "list assumed %d\n",
                    (void *)e->buffer, (int)e->buffer->buffer_state,
                    (int)e->from);
            return LC_ERROR_INVALID_ARGUMENT;
        }
        e->buffer->buffer_state = e->to;
    }
    return LC_SUCCESS;
}

lc_result lc_vulkan_worker_execute(lc_command_encoder *primary,
                                   lc_command_list *const *lists,
                                   uint32_t list_count,
                                   VkCommandBuffer primary_cmd) {
    lc_device *device;
    VkCommandBuffer *cmds = NULL;
    uint32_t i;
    lc_result res = LC_SUCCESS;

    if (primary == NULL || (lists == NULL && list_count > 0)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (list_count == 0) {
        return LC_SUCCESS;
    }
    device = primary->device;
    if (device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    cmds = (VkCommandBuffer *)malloc(sizeof(VkCommandBuffer) *
                                     list_count);
    if (cmds == NULL) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    /* Reconcile everything first (no partial execution on
     * conflict): single ordered pass under the state shard. */
    lc_device_lock_state(device);
    for (i = 0; i < list_count; i++) {
        lc_cmdlist_record *rec =
            lc_worker_find_record(device, lists[i]);

        if (lists[i] == NULL || lists[i]->device != device || rec == NULL ||
            !rec->valid) {
            fprintf(stderr,
                    "[lumac] execute conflict: list %u dead or "
                    "poisoned\n",
                    i);
            res = LC_ERROR_INVALID_ARGUMENT;
            break;
        }
        if (lists[i]->executed) {
            fprintf(stderr,
                    "[lumac] execute conflict: list %u already "
                    "executed (single-shot)\n",
                    i);
            res = LC_ERROR_INVALID_ARGUMENT;
            break;
        }
        if (lists[i]->is_compute) {
            /* Compute lists execute outside any pass (dispatch is
             * illegal inside a render-pass instance). */
            if (primary->in_pass) {
                fprintf(stderr,
                        "[lumac] execute conflict: list %u is a "
                        "compute list but the primary has an open "
                        "pass\n",
                        i);
                res = LC_ERROR_INVALID_ARGUMENT;
                break;
            }
        } else if (lists[i]->target != primary->pass_target_obj) {
            fprintf(stderr,
                    "[lumac] execute conflict: list %u targets a "
                    "different render target\n",
                    i);
            res = LC_ERROR_INVALID_ARGUMENT;
            break;
        }
        res = lc_worker_reconcile(device, lists[i]);
        if (res != LC_SUCCESS) {
            break;
        }
        cmds[i] = lists[i]->cmd;
    }
    device->submission_seq++;
    lc_device_unlock_state(device);
    if (res != LC_SUCCESS) {
        free(cmds);
        return res;
    }
    /* Buffer intents reconcile under the transfer shard (second
     * ordered pass; still before any execution). */
    lc_device_lock_transfer(device);
    for (i = 0; i < list_count; i++) {
        res = lc_worker_reconcile_buffers(device, lists[i]);
        if (res != LC_SUCCESS) {
            break;
        }
    }
    lc_device_unlock_transfer(device);
    if (res != LC_SUCCESS) {
        free(cmds);
        return res;
    }
    /* Single-shot burns only on a fully accepted batch. */
    /* Compute-only batches execute directly (no render-pass reopen
     * dance); mixing compute and graphics lists in one batch is
     * rejected (deterministic ordering across domains is a later
     * scheduler's job). The mix check runs BEFORE burning single-shot
     * state so a rejected mixed batch stays re-executable. */
    {
        int any_compute = 0;
        int any_graphics = 0;

        for (i = 0; i < list_count; i++) {
            if (lists[i]->is_compute) {
                any_compute = 1;
            } else {
                any_graphics = 1;
            }
        }
        if (any_compute && any_graphics) {
            fprintf(stderr,
                    "[lumac] execute conflict: mixed compute and "
                    "graphics lists in one batch\n");
            free(cmds);
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    for (i = 0; i < list_count; i++) {
        lists[i]->executed = 1;
        lists[i]->completion_value = UINT64_MAX;
    }
    /* Compute-only batches execute directly (no render-pass
     * reopen dance). */
    {
        int any_compute = 0;
        int any_graphics = 0;

        for (i = 0; i < list_count; i++) {
            if (lists[i]->is_compute) {
                any_compute = 1;
            } else {
                any_graphics = 1;
            }
        }
        if (any_compute) {
            vkCmdExecuteCommands(primary_cmd, list_count, cmds);
            free(cmds);
            return LC_SUCCESS;
        }
        (void)any_graphics;
    }
    /* Worker graphics lists target offscreen render targets only, so a
     * swapchain primary can never match (rejected above). Guard anyway:
     * ending the open pass below is unrecoverable if the reopen fails. */
    if (primary->pass_is_swapchain || primary->pass_target_obj == NULL) {
        fprintf(stderr,
                "[lumac] execute conflict: worker graphics lists cannot "
                "execute inside a swapchain pass\n");
        free(cmds);
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Offscreen begin permits ordinary inline commands, so its first
     * render-pass instance uses INLINE contents. Vulkan requires a
     * secondary-capable instance for vkCmdExecuteCommands. End the
     * inline instance and reopen a render-pass-compatible LOAD instance;
     * render-pass compatibility ignores load/store ops, preserving the
     * worker inheritance contract and any inline output already written. */
    {
        lc_render_color_attachment colors[LC_MAX_COLOR_ATTACHMENTS];
        lc_render_depth_attachment depth;
        lc_render_pass_desc load_desc;
        lc_render_target *target =
            (lc_render_target *)primary->pass_target_obj;
        VkRenderPass load_pass = VK_NULL_HANDLE;
        VkRenderPassBeginInfo begin;
        uint32_t c;

        memset(&load_desc, 0, sizeof(load_desc));
        for (c = 0; c < primary->end_color_count; c++) {
            memset(&colors[c], 0, sizeof(colors[c]));
            colors[c].view = primary->end_color_views[c];
            colors[c].load_op = LC_LOAD_OP_LOAD;
            colors[c].store_op = primary->end_color_stores[c];
        }
        load_desc.color_attachments = colors;
        load_desc.color_attachment_count = primary->end_color_count;
        load_desc.width = primary->pass_target.width;
        load_desc.height = primary->pass_target.height;
        if (primary->end_has_depth) {
            memset(&depth, 0, sizeof(depth));
            depth.view = primary->end_depth_view;
            depth.depth_load_op = LC_LOAD_OP_LOAD;
            depth.depth_store_op = primary->end_depth_store;
            depth.stencil_load_op = LC_LOAD_OP_DONT_CARE;
            depth.stencil_store_op = LC_STORE_OP_DONT_CARE;
            load_desc.depth_attachment = &depth;
        }
        vkCmdEndRenderPass(primary_cmd);
        res = lc_vulkan_offscreen_pass(device, target, &load_desc,
                                       &load_pass);
        if (res != LC_SUCCESS) {
            free(cmds);
            return res;
        }
        memset(&begin, 0, sizeof(begin));
        begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        begin.renderPass = load_pass;
        begin.framebuffer = target->framebuffer;
        begin.renderArea.extent.width = target->width;
        begin.renderArea.extent.height = target->height;
        vkCmdBeginRenderPass(primary_cmd, &begin,
                             VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);
    }
    vkCmdExecuteCommands(primary_cmd, list_count, cmds);
    free(cmds);
    return LC_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Draw-state recording (secondary command buffer + ref log).          */
/* ------------------------------------------------------------------ */

static VkShaderStageFlags lc_worker_push_stages(uint32_t visibility) {
    VkShaderStageFlags stages = 0;

    if ((visibility & LC_SHADER_VISIBILITY_VERTEX) != 0) {
        stages |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if ((visibility & LC_SHADER_VISIBILITY_FRAGMENT) != 0) {
        stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    return stages;
}

lc_result lc_worker_record_bind_vertex(lc_command_encoder *enc,
                                       uint32_t binding,
                                       const lc_buffer *buffer,
                                       uint64_t offset) {
    lc_command_list *list = lc_worker_open(enc);
    VkPhysicalDeviceProperties props;
    VkDeviceSize vk_offset;

    if (list == NULL || buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (buffer->device != list->device ||
        buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (offset > buffer->size) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(list->device->physical_device, &props);
    if (binding >= props.limits.maxVertexInputBindings) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    vk_offset = (VkDeviceSize)offset;
    vkCmdBindVertexBuffers(list->cmd, binding, 1, &buffer->vk_buffer,
                           &vk_offset);
    return lc_worker_log_ref(list, buffer);
}

lc_result lc_worker_record_bind_index(lc_command_encoder *enc,
                                      const lc_buffer *buffer,
                                      uint64_t offset,
                                      lc_index_type index_type) {
    lc_command_list *list = lc_worker_open(enc);
    VkIndexType vk_type;

    if (list == NULL || buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (buffer->device != list->device ||
        buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (offset > buffer->size) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (index_type != LC_INDEX_UINT16 && index_type != LC_INDEX_UINT32) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    vk_type = (index_type == LC_INDEX_UINT16) ? VK_INDEX_TYPE_UINT16
                                              : VK_INDEX_TYPE_UINT32;
    vkCmdBindIndexBuffer(list->cmd, buffer->vk_buffer,
                         (VkDeviceSize)offset, vk_type);
    enc->bound_index_buffer = buffer;
    enc->bound_index_offset = offset;
    enc->bound_index_type = index_type;
    enc->index_bound = 1;
    return lc_worker_log_ref(list, buffer);
}

lc_result lc_worker_record_push(lc_command_encoder *enc,
                                const lc_pipeline *pipeline,
                                uint32_t visibility, uint32_t offset,
                                uint32_t size, const void *data) {
    lc_command_list *list = lc_worker_open(enc);
    VkShaderStageFlags stages;

    if (list == NULL || pipeline == NULL || data == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (pipeline->device != list->device ||
        pipeline->layout == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_pipeline != pipeline) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    stages = lc_worker_push_stages(visibility);
    if (stages == 0 || size == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    vkCmdPushConstants(list->cmd, pipeline->layout, stages, offset,
                       size, data);
    return lc_worker_log_ref(list, pipeline);
}

lc_result lc_worker_record_draw(lc_command_encoder *enc,
                                uint32_t vertex_count,
                                uint32_t first_vertex) {
    lc_command_list *list = lc_worker_open(enc);

    if (list == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    vkCmdDraw(list->cmd, vertex_count, 1, first_vertex, 0);
    return LC_SUCCESS;
}

lc_result lc_worker_record_draw_indexed(
    lc_command_encoder *enc, uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
    lc_command_list *list = lc_worker_open(enc);

    if (list == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_pipeline == NULL || !enc->index_bound ||
        enc->bound_index_buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    vkCmdDrawIndexed(list->cmd, index_count, instance_count,
                     first_index, vertex_offset, first_instance);
    return LC_SUCCESS;
}

lc_result lc_worker_record_draw_instanced(
    lc_command_encoder *enc, uint32_t vertex_count, uint32_t instance_count,
    uint32_t first_vertex, uint32_t first_instance) {
    lc_command_list *list = lc_worker_open(enc);

    if (list == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    vkCmdDraw(list->cmd, vertex_count, instance_count, first_vertex,
              first_instance);
    return LC_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Compute lists + compute/indirect recording (Phase 21).              */
/*                                                                     */
/* Compute lists record without render-pass inheritance (dispatch is  */
/* illegal inside a pass instance) and execute outside any pass.      */
/* Indirect draws are ordinary in-pass draws, legal in graphics       */
/* lists; buffer transitions log intents reconciled under the         */
/* transfer shard at execute.                                         */
/* ------------------------------------------------------------------ */

lc_result lc_vulkan_worker_begin_compute(lc_command_encoder *enc) {
    VkCommandBufferAllocateInfo alloc_info;
    VkCommandBufferBeginInfo begin_info;
    lc_command_list *list = NULL;
    lc_device *device;

    if (enc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = enc->worker_device;
    if (device == NULL || device->device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    if (!device->compute_supported) {
        return LC_ERROR_UNSUPPORTED;
    }
    if (enc->worker_list != NULL) {
        return LC_ERROR_INVALID_ARGUMENT; /* one list at a time */
    }
    list = (lc_command_list *)calloc(1, sizeof(lc_command_list));
    if (list == NULL) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType =
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = enc->worker_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
    alloc_info.commandBufferCount = 1;
    lc_device_lock_transfer(device);
    if (vkAllocateCommandBuffers(device->device, &alloc_info,
                                 &list->cmd) != VK_SUCCESS) {
        lc_device_unlock_transfer(device);
        free(list);
        return LC_ERROR_OUT_OF_MEMORY;
    }
    lc_device_unlock_transfer(device);
    list->device = device;
    list->resource_id = lc_issue_resource_id();
    list->pool = enc->worker_pool;
    list->queue_family = device->graphics_queue_family;
    list->is_compute = 1;
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    /* Secondary buffers always need valid inheritance info, even
     * without RENDER_PASS_CONTINUE (VUID-00051). */
    {
        VkCommandBufferInheritanceInfo inherit;

        memset(&inherit, 0, sizeof(inherit));
        inherit.sType =
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
        begin_info.pInheritanceInfo = &inherit;
        if (vkBeginCommandBuffer(list->cmd, &begin_info) !=
            VK_SUCCESS) {
            lc_device_lock_transfer(device);
            vkFreeCommandBuffers(device->device, enc->worker_pool, 1,
                                 &list->cmd);
            lc_device_unlock_transfer(device);
            free(list);
            return LC_ERROR_UNKNOWN;
        }
    }
    enc->worker_list = list;
    enc->bound_compute_pipeline = NULL;
    enc->worker_live_lists++;
    lc_device_lock_transfer(device);
    device->stat_lists_in_flight++;
    lc_device_unlock_transfer(device);
    return LC_SUCCESS;
}

lc_result lc_worker_record_bind_compute_pipeline(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline) {
    lc_command_list *list = lc_worker_open(enc);
    lc_device *device;

    if (list == NULL || pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!list->is_compute) {
        /* Graphics-continue secondaries cannot dispatch; compute
         * binds belong on compute lists (frame encoders bind
         * directly). */
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = list->device;
    if (pipeline->device != device ||
        pipeline->pipeline == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    vkCmdBindPipeline(list->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                      pipeline->pipeline);
    enc->bound_compute_pipeline = pipeline;
    return lc_worker_log_ref(list, pipeline);
}

lc_result lc_worker_record_dispatch(lc_command_encoder *enc, uint32_t x,
                                    uint32_t y, uint32_t z) {
    lc_command_list *list = lc_worker_open(enc);
    lc_device *device;

    if (list == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!list->is_compute) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = list->device;
    if (enc->bound_compute_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (x == 0 || y == 0 || z == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (x > device->max_workgroup_count[0] ||
        y > device->max_workgroup_count[1] ||
        z > device->max_workgroup_count[2]) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    vkCmdDispatch(list->cmd, x, y, z);
    return LC_SUCCESS;
}

lc_result lc_worker_record_push_compute(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline,
    uint32_t visibility, uint32_t offset, uint32_t size,
    const void *data) {
    lc_command_list *list = lc_worker_open(enc);
    VkShaderStageFlags stages = 0;

    if (list == NULL || pipeline == NULL || data == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!list->is_compute) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (pipeline->device != list->device ||
        pipeline->layout == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_compute_pipeline != pipeline) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((visibility & LC_SHADER_VISIBILITY_COMPUTE) != 0) {
        stages |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    if (stages == 0 || size == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    vkCmdPushConstants(list->cmd, pipeline->layout, stages, offset,
                       size, data);
    return lc_worker_log_ref(list, pipeline);
}

lc_result lc_worker_record_draw_indirect(lc_command_encoder *enc,
                                         const lc_buffer *buffer,
                                         uint64_t offset,
                                         uint32_t draw_count,
                                         uint32_t stride) {
    lc_command_list *list = lc_worker_open(enc);
    lc_result res;

    if (list == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (list->is_compute) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vk_indirect_batch_valid(buffer, offset, draw_count, stride,
                                     (uint32_t)sizeof(
                                         lc_indirect_draw_command));
    if (res != LC_SUCCESS) {
        return res;
    }
    /* The buffer must already be INDIRECT_READ: barriers are
     * illegal inside the pass this secondary executes in, so the
     * producer transitions before the pass opens (frame-level
     * transition or renderer prepare). Loud reject otherwise. */
    lc_device_lock_transfer(list->device);
    if (buffer->buffer_state != LC_RESOURCE_STATE_INDIRECT_READ) {
        lc_device_unlock_transfer(list->device);
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_worker_log_ref(list, buffer);
    lc_device_unlock_transfer(list->device);
    if (res != LC_SUCCESS) {
        return res;
    }
    if (draw_count == 1 || list->device->multi_draw_indirect != 0) {
        vkCmdDrawIndirect(list->cmd, buffer->vk_buffer,
                          (VkDeviceSize)offset, draw_count, stride);
        return LC_SUCCESS;
    }
    /* Compatibility loop (same honest fallback as the frame path). */
    {
        uint32_t i;

        for (i = 0; i < draw_count; i++) {
            vkCmdDrawIndirect(list->cmd, buffer->vk_buffer,
                              (VkDeviceSize)offset +
                                  (VkDeviceSize)i * (VkDeviceSize)stride,
                              1, stride);
        }
    }
    return LC_SUCCESS;
}

lc_result lc_worker_record_draw_indexed_indirect(
    lc_command_encoder *enc, const lc_buffer *buffer, uint64_t offset,
    uint32_t draw_count, uint32_t stride) {
    lc_command_list *list = lc_worker_open(enc);
    lc_result res;

    if (list == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (list->is_compute) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_pipeline == NULL || !enc->index_bound ||
        enc->bound_index_buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vk_indirect_batch_valid(
        buffer, offset, draw_count, stride,
        (uint32_t)sizeof(lc_indirect_draw_indexed_command));
    if (res != LC_SUCCESS) {
        return res;
    }
    /* Same outside-pass rule as above (no barrier in the pass). */
    lc_device_lock_transfer(list->device);
    if (buffer->buffer_state != LC_RESOURCE_STATE_INDIRECT_READ) {
        lc_device_unlock_transfer(list->device);
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_worker_log_ref(list, buffer);
    lc_device_unlock_transfer(list->device);
    if (res != LC_SUCCESS) {
        return res;
    }
    if (draw_count == 1 || list->device->multi_draw_indirect != 0) {
        vkCmdDrawIndexedIndirect(list->cmd, buffer->vk_buffer,
                                 (VkDeviceSize)offset, draw_count,
                                 stride);
        return LC_SUCCESS;
    }
    {
        uint32_t i;

        for (i = 0; i < draw_count; i++) {
            vkCmdDrawIndexedIndirect(
                list->cmd, buffer->vk_buffer,
                (VkDeviceSize)offset +
                    (VkDeviceSize)i * (VkDeviceSize)stride,
                1, stride);
        }
    }
    return LC_SUCCESS;
}

/* Worker-list count draws record directly into the secondary
 * (same native-or-fallback rule as the frame path). */
static lc_result lc_worker_record_indirect_count_common(
    lc_command_list *list, const lc_buffer *buffer, uint64_t offset,
    const lc_buffer *count_buffer, uint64_t count_offset,
    uint32_t max_draw_count, uint32_t stride, uint32_t elem_size) {
    lc_result res;

    res = lc_vk_indirect_batch_valid(buffer, offset, max_draw_count,
                                     stride, elem_size);
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vk_indirect_count_valid(count_buffer, count_offset,
                                     max_draw_count);
    if (res != LC_SUCCESS) {
        return res;
    }
    lc_device_lock_transfer(list->device);
    if (buffer->buffer_state != LC_RESOURCE_STATE_INDIRECT_READ ||
        count_buffer->buffer_state !=
            LC_RESOURCE_STATE_INDIRECT_READ) {
        lc_device_unlock_transfer(list->device);
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_worker_log_ref(list, buffer);
    if (res == LC_SUCCESS) {
        res = lc_worker_log_ref(list, count_buffer);
    }
    lc_device_unlock_transfer(list->device);
    return res;
}

lc_result lc_worker_record_draw_indirect_count(
    lc_command_encoder *enc, const lc_buffer *buffer, uint64_t offset,
    const lc_buffer *count_buffer, uint64_t count_offset,
    uint32_t max_draw_count, uint32_t stride) {
    lc_command_list *list = lc_worker_open(enc);
    lc_result res;

    if (list == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (list->is_compute) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_worker_record_indirect_count_common(
        list, buffer, offset, count_buffer, count_offset,
        max_draw_count, stride,
        (uint32_t)sizeof(lc_indirect_draw_command));
    if (res != LC_SUCCESS) {
        return res;
    }
    if (list->device->indirect_count_supported != 0) {
        vkCmdDrawIndirectCount(list->cmd, buffer->vk_buffer,
                               (VkDeviceSize)offset,
                               count_buffer->vk_buffer,
                               (VkDeviceSize)count_offset,
                               max_draw_count, stride);
        return LC_SUCCESS;
    }
    {
        uint32_t i;

        for (i = 0; i < max_draw_count; i++) {
            vkCmdDrawIndirect(list->cmd, buffer->vk_buffer,
                              (VkDeviceSize)offset +
                                  (VkDeviceSize)i * (VkDeviceSize)stride,
                              1, stride);
        }
    }
    return LC_SUCCESS;
}

lc_result lc_worker_record_draw_indexed_indirect_count(
    lc_command_encoder *enc, const lc_buffer *buffer, uint64_t offset,
    const lc_buffer *count_buffer, uint64_t count_offset,
    uint32_t max_draw_count, uint32_t stride) {
    lc_command_list *list = lc_worker_open(enc);
    lc_result res;

    if (list == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (list->is_compute) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (enc->bound_pipeline == NULL || !enc->index_bound ||
        enc->bound_index_buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_worker_record_indirect_count_common(
        list, buffer, offset, count_buffer, count_offset,
        max_draw_count, stride,
        (uint32_t)sizeof(lc_indirect_draw_indexed_command));
    if (res != LC_SUCCESS) {
        return res;
    }
    if (list->device->indirect_count_supported != 0) {
        vkCmdDrawIndexedIndirectCount(
            list->cmd, buffer->vk_buffer, (VkDeviceSize)offset,
            count_buffer->vk_buffer, (VkDeviceSize)count_offset,
            max_draw_count, stride);
        return LC_SUCCESS;
    }
    {
        uint32_t i;

        for (i = 0; i < max_draw_count; i++) {
            vkCmdDrawIndexedIndirect(
                list->cmd, buffer->vk_buffer,
                (VkDeviceSize)offset +
                    (VkDeviceSize)i * (VkDeviceSize)stride,
                1, stride);
        }
    }
    return LC_SUCCESS;
}

lc_result lc_worker_record_transition_buffer(
    lc_command_encoder *enc, lc_buffer *buffer,
    lc_resource_state new_state) {    lc_command_list *list = lc_worker_open(enc);
    lc_device *device;
    lc_resource_state from = LC_RESOURCE_STATE_UNDEFINED;
    lc_result res;

    if (list == NULL || buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = list->device;
    if (buffer->device != device ||
        buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_state_valid_for_buffer(new_state)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!list->is_compute) {
        /* The barrier would execute inside the graphics pass this
         * secondary runs in (passes carry no self-dependency), so
         * graphics lists cannot transition buffers. Compute lists
         * execute outside passes. */
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Snapshot under the transfer shard; the barrier records
     * immediately while the global mark waits for execute
     * reconciliation (same discipline as image intents). */
    lc_device_lock_transfer(device);
    from = buffer->buffer_state;
    if (from != LC_RESOURCE_STATE_UNDEFINED &&
        !lc_vk_sync_state_valid_for_buffer(from)) {
        from = LC_RESOURCE_STATE_UNDEFINED;
    }
    if (from != new_state) {
        VkPipelineStageFlags src_stage = 0;
        VkPipelineStageFlags dst_stage = 0;
        VkAccessFlags src_access = 0;
        VkAccessFlags dst_access = 0;

        if (!lc_vk_sync_buffer_barrier_params(from, &src_stage,
                                              &src_access) ||
            !lc_vk_sync_buffer_barrier_params(new_state, &dst_stage,
                                              &dst_access)) {
            lc_device_unlock_transfer(device);
            return LC_ERROR_INVALID_ARGUMENT;
        }
        lc_vk_sync_record_buffer(list->cmd, buffer, 0, buffer->size,
                                 src_stage, src_access, dst_stage,
                                 dst_access, VK_QUEUE_FAMILY_IGNORED,
                                 VK_QUEUE_FAMILY_IGNORED);
        res = lc_worker_log_buffer_intent(list, buffer, from,
                                          new_state);
    } else {
        res = lc_worker_log_ref(list, buffer);
    }
    lc_device_unlock_transfer(device);
    return res;
}

lc_result lc_worker_record_bind_compute_set(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline,
    uint32_t slot, lc_binding_set *set) {
    lc_command_list *list = lc_worker_open(enc);
    lc_result res = LC_SUCCESS;

    if (list == NULL || pipeline == NULL || set == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!list->is_compute) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (slot >= pipeline->layout_count ||
        set->vk_set == VK_NULL_HANDLE ||
        pipeline->layout == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (set->device != list->device) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Canonical layout match (contents, like the graphics path). */
    {
        uint32_t n = 0;

        if (slot < pipeline->layout_count) {
            n = pipeline->slot_signature_counts[slot];
        }
        if (n != set->slot_count ||
            !lc_binding_signature_equal(
                pipeline->slot_signatures[slot], n, set->slots,
                set->slot_count)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    vkCmdBindDescriptorSets(list->cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            pipeline->layout, slot, 1, &set->vk_set, 0,
                            NULL);
    res = lc_worker_log_ref(list, set);
    if (res != LC_SUCCESS) {
        return res;
    }
    return lc_worker_log_ref(list, pipeline);
}
