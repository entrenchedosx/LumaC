/*
 * Vulkan transfer engine (Phase 20, PARTs O–Z, Q–R, AA–AE, 17).
 *
 * Dedicated transfer queue when offered (graphics fallback
 * otherwise), timeline completion with binary-fence fallback,
 * async buffer/image uploads, async readbacks, bounded staging
 * ring, cross-family ownership transfers, deferred retirement.
 *
 * Submission model: transfer work submits eagerly at schedule
 * time (never waits on the CPU); consumers order via timeline
 * values (frame submits wait the in-flight max) or fences.
 * Staging frees only after GPU completion; ownership acquires
 * for graphics consumption ride tiny chained immediate submits
 * scheduled atomically with the transfer (no frame hooks, no
 * lazily-orphaned obligations).
 *
 * Locks: transfer_mutex around scheduling/reclaim bookkeeping
 * only (never across GPU waits except the documented cap-pressure
 * wait, which drops... no: it holds nothing while waiting — see
 * staging pressure handling). State-shard rules from Phase 19
 * apply to all tracking touches.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <time.h>
#endif

#include "graphics/graphics_internal.h"

/* Forward declarations (helpers defined after the reclaim core so
 * the diff stays append-only below). */
static void lc_vk_imm_cmd_free(lc_device *device, VkCommandBuffer cmd);
static void lc_vk_transfer_cmd_free_entry(lc_device *device,
                                          lc_transfer_entry *tr);
static void lc_vk_transfer_copy_cmd_free(lc_device *device,
                                         lc_transfer_entry *tr);
static lc_result lc_vk_imm_cmd_alloc(lc_device *device,
                                     VkCommandBuffer *out);
static lc_result lc_vk_xfer_fence_create(lc_device *device,
                                         VkFence *out);
static lc_result lc_vk_xfer_submit(
    lc_device *device, VkQueue queue, VkCommandBuffer cmd,
    uint32_t wait_count, const VkSemaphore *waits,
    const VkPipelineStageFlags *wait_stages, const uint64_t *wait_vals,
    uint32_t sig_count, const VkSemaphore *sigs,
    const uint64_t *sig_vals, VkFence fence);
static lc_result lc_vk_xfer_record_acquire(lc_device *device,
                                           VkCommandBuffer cmd,
                                           lc_transfer_entry *tr);
static void lc_vk_xfer_restore_owners(lc_device *device,
                                      lc_transfer_entry *tr);

/* Default async staging cap (PART V): 256 MiB. */
#define LC_TRANSFER_STAGING_DEFAULT (256u * 1024u * 1024u)

/* ------------------------------------------------------------------ */
/* Signals.                                                            */
/* ------------------------------------------------------------------ */

uint64_t lc_vk_timeline_counter(lc_device *device) {
    uint64_t value = 0;

    if (device == NULL || !device->timeline_ok ||
        device->timeline == VK_NULL_HANDLE ||
        device->device == VK_NULL_HANDLE ||
        device->pfn_sem_counter == NULL) {
        return 0;
    }
    if (device->pfn_sem_counter(device->device, device->timeline,
                                &value) != VK_SUCCESS) {
        return 0;
    }
    return value;
}

uint64_t lc_vk_signal_reserve(lc_device *device) {
    uint64_t value = 0;

    if (device == NULL) {
        return 0;
    }
    lc_device_lock_transfer(device);
    value = device->timeline_next;
    device->timeline_next++;
    if (device->timeline_next == 0) {
        /* Wrap guard (practically unreachable): skip 0. */
        device->timeline_next = 1;
    }
    lc_device_unlock_transfer(device);
    return value;
}

/* Host-signal one timeline value (best effort; device loss fails
 * everything anyway). Thread-safe; holds no shard. */
static void lc_vk_signal_host_signal(lc_device *device, uint64_t value) {
    VkSemaphoreSignalInfo info;

    if (device == NULL || value == 0) {
        return;
    }
    if (!device->timeline_ok || device->pfn_sem_signal == NULL ||
        device->timeline == VK_NULL_HANDLE ||
        device->device == VK_NULL_HANDLE) {
        return;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
    info.semaphore = device->timeline;
    info.value = value;
    device->pfn_sem_signal(device->device, &info);
}

/* Reserve a value AND link a placeholder entry atomically (one
 * transfer-lock hold). Phase 22 publication atomicity: from the
 * moment the value exists, every observer (frame max-wait,
 * wait_for, retirement horizon) sees the entry, so tracked state
 * and completion dependency can never disagree. The entry starts
 * unsubmitted; the scheduler publishes (submitted=1) after its
 * submits succeed, or drops it (host-signaling the value in
 * timeline mode) on failure. Returns 0 + NULL entry on host OOM
 * (no value consumed, no gap). */
static uint64_t lc_vk_xfer_reserve_linked(
    lc_device *device, const lc_transfer_entry *tmpl,
    lc_transfer_entry **out_entry) {
    uint64_t value = 0;
    lc_transfer_entry *entry = NULL;

    if (out_entry != NULL) {
        *out_entry = NULL;
    }
    if (device == NULL || tmpl == NULL || out_entry == NULL) {
        return 0;
    }
    entry = (lc_transfer_entry *)calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return 0;
    }
    lc_device_lock_transfer(device);
    value = device->timeline_next;
    device->timeline_next++;
    if (device->timeline_next == 0) {
        device->timeline_next = 1;
    }
    memcpy(entry, tmpl, sizeof(*entry));
    entry->signal_value = value;
    entry->copy_value = value;
    entry->submitted = 0;
    entry->next = device->transfers;
    entry->prev = NULL;
    if (device->transfers != NULL) {
        device->transfers->prev = entry;
    }
    device->transfers = entry;
    lc_device_unlock_transfer(device);
    *out_entry = entry;
    return value;
}

/* Drop a placeholder after submit failure: host-signal the value
 * (timeline mode, so value waiters complete) and unlink + free.
 * Staging/commands are freed by the caller's fail_cleanup. */
static void lc_vk_xfer_drop_placeholder(lc_device *device,
                                        lc_transfer_entry *entry,
                                        uint64_t value) {
    if (device == NULL || entry == NULL) {
        return;
    }
    lc_vk_signal_host_signal(device, value);
    lc_device_lock_transfer(device);
    if (entry->prev != NULL) {
        entry->prev->next = entry->next;
    } else if (device->transfers == entry) {
        device->transfers = entry->next;
    }
    if (entry->next != NULL) {
        entry->next->prev = entry->prev;
    }
    lc_device_unlock_transfer(device);
    free(entry);
}

/* Completion test without locks (mirrors reclaim's done logic).
 * Used by the overlap serializer below. */
static int lc_vk_xfer_entry_done(lc_device *device,
                                 const lc_transfer_entry *tr) {
    if (tr == NULL || !tr->submitted) {
        return 0;
    }
    if (device->timeline_ok) {
        return (tr->signal_value == 0 ||
                lc_vk_timeline_counter(device) >= tr->signal_value)
                   ? 1
                   : 0;
    }
    if (tr->fallback_fence != VK_NULL_HANDLE &&
        device->device != VK_NULL_HANDLE) {
        return (vkGetFenceStatus(device->device, tr->fallback_fence) ==
                VK_SUCCESS)
                   ? 1
                   : 0;
    }
    return 1;
}

/* Range overlap for one entry (NULL refs count as overlapping:
 * the resource is mid-destroy and conservative ordering is the
 * only safe choice). Buffers and images never alias each other. */
static int lc_vk_xfer_ranges_overlap(const lc_transfer_entry *tr,
                                     int is_image, uint32_t base_mip,
                                     uint32_t level_count,
                                     uint32_t base_layer,
                                     uint32_t layer_count,
                                     uint64_t buf_offset,
                                     uint64_t buf_size) {
    if (tr->is_image != is_image) {
        return 0;
    }
    if (is_image) {
        uint32_t a0;
        uint32_t a1;
        uint32_t b0;
        uint32_t b1;

        if (tr->image == NULL) {
            return 1;
        }
        a0 = tr->base_mip;
        a1 = tr->base_mip + tr->level_count;
        b0 = base_mip;
        b1 = base_mip + level_count;
        if (a0 >= b1 || b0 >= a1) {
            return 0;
        }
        a0 = tr->base_layer;
        a1 = tr->base_layer + tr->layer_count;
        b0 = base_layer;
        b1 = base_layer + layer_count;
        if (a0 >= b1 || b0 >= a1) {
            return 0;
        }
        return 1;
    }
    if (tr->buffer == NULL) {
        return 1;
    }
    if (buf_size == 0) {
        return 1;
    }
    if (tr->buf_offset >= buf_offset + buf_size ||
        buf_offset >= tr->buf_offset + tr->buf_size) {
        return 0;
    }
    return 1;
}

/* Overlapping-transfer serializer (Phase 22, PARTs I-K): wait for
 * every INCOMPLETE overlapping entry before submitting, so link
 * order == submit order == completion order and the newest
 * submission deterministically wins the subresource. Different
 * mips/layers/ranges stay fully parallel. Readbacks participate
 * (an upload overlapping a pending readback waits for it: natural
 * read-after-write). Terminates: the GPU always progresses, and
 * placeholders publish (or drop) within microseconds of linking.
 * Called with no shard held; holds none across waits. */
static lc_result lc_vk_xfer_wait_overlap(
    lc_device *device, int is_image, const lc_image *image,
    const lc_buffer *buffer, uint32_t base_mip, uint32_t level_count,
    uint32_t base_layer, uint32_t layer_count, uint64_t buf_offset,
    uint64_t buf_size) {
    if (device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    for (;;) {
        uint64_t wait_value = 0;
        int blocking = 0;
        lc_transfer_entry *it;

        lc_device_lock_transfer(device);
        for (it = device->transfers; it != NULL; it = it->next) {
            /* Same resource only (NULL refs on either side block
             * conservatively: mid-destroy entries cannot prove
             * disjointness). */
            if (is_image != it->is_image) {
                continue;
            }
            if (is_image) {
                if (it->image != NULL && image != NULL &&
                    it->image != image) {
                    continue;
                }
            } else {
                if (it->buffer != NULL && buffer != NULL &&
                    it->buffer != buffer) {
                    continue;
                }
            }
            if (!lc_vk_xfer_ranges_overlap(
                    it, is_image, base_mip, level_count, base_layer,
                    layer_count, buf_offset, buf_size)) {
                continue;
            }
            if (lc_vk_xfer_entry_done(device, it)) {
                continue;
            }
            blocking = 1;
            if (it->signal_value > wait_value) {
                wait_value = it->signal_value;
            }
        }
        lc_device_unlock_transfer(device);
        if (!blocking) {
            return LC_SUCCESS;
        }
        if (lc_vk_signal_wait(device, wait_value,
                              LC_TIMEOUT_INFINITE) != LC_SUCCESS) {
            return LC_ERROR_UNKNOWN;
        }
        lc_vk_reclaim_completed(device);
    }
}

int lc_vk_signal_ready(lc_device *device, uint64_t value) {
    lc_transfer_entry *it;
    int ready;

    if (device == NULL || value == 0) {
        return 1;
    }
    if (device->timeline_ok) {
        return (lc_vk_timeline_counter(device) >= value) ? 1 : 0;
    }
    /* Fence fallback: every SUBMITTED entry at or below the value
     * must have a signaled fence. Placeholders (reserved but not
     * yet submitted) are skipped: their value will be signaled by
     * their submit, host-signaled on submit failure, or the entry
     * is dropped before anyone could observe the value. */
    ready = 1;
    lc_device_lock_transfer(device);
    for (it = device->transfers; it != NULL; it = it->next) {
        if (!it->submitted) {
            continue;
        }
        if (it->signal_value <= value && it->signal_value != 0) {
            if (it->fallback_fence == VK_NULL_HANDLE ||
                device->device == VK_NULL_HANDLE ||
                vkGetFenceStatus(device->device, it->fallback_fence) !=
                    VK_SUCCESS) {
                ready = 0;
                break;
            }
        }
    }
    lc_device_unlock_transfer(device);
    return ready;
}

lc_result lc_vk_signal_wait(lc_device *device, uint64_t value,
                            uint64_t timeout_ns) {
    uint64_t deadline;
    uint64_t now;

    if (device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (value == 0 || lc_vk_signal_ready(device, value)) {
        return LC_SUCCESS;
    }
    if (device->timeline_ok && device->pfn_sem_wait != NULL &&
        device->device != VK_NULL_HANDLE) {
        VkSemaphoreWaitInfo info;
        VkResult res;

        memset(&info, 0, sizeof(info));
        info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        info.semaphoreCount = 1;
        info.pSemaphores = &device->timeline;
        info.pValues = &value;
        res = device->pfn_sem_wait(device->device, &info, timeout_ns);
        if (res == VK_SUCCESS) {
            return LC_SUCCESS;
        }
        return (res == VK_TIMEOUT) ? LC_ERROR_TIMEOUT
                                   : LC_ERROR_UNKNOWN;
    }
    /* Fence fallback: poll in 1 ms slices against a nanosecond
     * deadline (Phase 22 clock contract: lc_clock_now() is
     * nanoseconds on every platform, so no conversion here). */
    now = lc_clock_now();
    if (timeout_ns == LC_TIMEOUT_INFINITE) {
        deadline = UINT64_MAX;
    } else if (timeout_ns > UINT64_MAX - now) {
        deadline = UINT64_MAX; /* saturate instead of wrapping */
    } else {
        deadline = now + timeout_ns;
    }
    for (;;) {
        lc_transfer_entry *it;
        int pending = 0;

        lc_device_lock_transfer(device);
        for (it = device->transfers; it != NULL; it = it->next) {
            if (!it->submitted) {
                continue;
            }
            if (it->signal_value <= value && it->signal_value != 0 &&
                it->fallback_fence != VK_NULL_HANDLE &&
                device->device != VK_NULL_HANDLE &&
                vkGetFenceStatus(device->device, it->fallback_fence) !=
                    VK_SUCCESS) {
                pending = 1;
                break;
            }
        }
        lc_device_unlock_transfer(device);
        if (!pending) {
            return LC_SUCCESS;
        }
        now = lc_clock_now();
        if (now >= deadline) {
            return LC_ERROR_TIMEOUT;
        }
#if defined(_WIN32) || defined(_WIN64)
        Sleep(1);
#else
        {
            struct timespec ts;

            ts.tv_sec = 0;
            ts.tv_nsec = 1000000;
            nanosleep(&ts, NULL);
        }
#endif
    }
}

/* ------------------------------------------------------------------ */
/* Transfer pool + transient commands.                                 */
/* ------------------------------------------------------------------ */

static lc_result lc_vk_transfer_pool_ensure(lc_device *device) {
    VkCommandPoolCreateInfo info;

    if (device->transfer_pool != VK_NULL_HANDLE) {
        return LC_SUCCESS;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = device->transfer_queue_family;
    if (vkCreateCommandPool(device->device, &info, NULL,
                            &device->transfer_pool) != VK_SUCCESS) {
        device->transfer_pool = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    return LC_SUCCESS;
}

static lc_result lc_vk_transfer_cmd_alloc(lc_device *device,
                                          VkCommandBuffer *out) {
    VkCommandBufferAllocateInfo info;
    lc_result res;

    res = lc_vk_transfer_pool_ensure(device);
    if (res != LC_SUCCESS) {
        return res;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = device->transfer_pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device->device, &info, out) !=
        VK_SUCCESS) {
        *out = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    device->transfer_cmds_outstanding++;
    return LC_SUCCESS;
}

static void lc_vk_transfer_cmd_free(lc_device *device,
                                    VkCommandBuffer cmd) {
    if (device == NULL || cmd == VK_NULL_HANDLE ||
        device->transfer_pool == VK_NULL_HANDLE ||
        device->device == VK_NULL_HANDLE) {
        return;
    }
    /* Phase 22: transfer-pool objects live in the submit domain. */
    assert(lc_device_submit_held());
    vkFreeCommandBuffers(device->device, device->transfer_pool, 1,
                         &cmd);
    if (device->transfer_cmds_outstanding > 0) {
        device->transfer_cmds_outstanding--;
    }
    /* Reset the pool when fully idle (PART D recycling). */
    if (device->transfer_cmds_outstanding == 0) {
        vkResetCommandPool(device->device, device->transfer_pool, 0);
    }
}

/* Begin a one-time transfer command buffer. */
static lc_result lc_vk_transfer_cmd_begin(lc_device *device,
                                           VkCommandBuffer cmd) {
    VkCommandBufferBeginInfo begin_info;

    (void)device;
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(cmd, &begin_info) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    return LC_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Staging pressure (PART V).                                          */
/* ------------------------------------------------------------------ */

static uint64_t lc_vk_staging_cap(lc_device *device) {
    if (device->staging_cap != 0) {
        return device->staging_cap;
    }
    return LC_TRANSFER_STAGING_DEFAULT;
}

/* Block until `need` staging bytes fit (waits oldest completions).
 * Called WITHOUT the transfer lock held (it locks internally per
 * step); GPU always progresses so this terminates. */
static lc_result lc_vk_staging_make_room(lc_device *device,
                                         uint64_t need) {
    uint64_t cap = lc_vk_staging_cap(device);
    uint64_t limit = (need > cap) ? need : cap;

    /* Oversize requests drain everything first (no shortcut OOM:
     * the GPU always progresses, so the loop below terminates).
     * Readback staging never releases via reclaim (it holds the
     * result until destroy), so pressure waits skip readbacks; if
     * only readback-pinned memory remains and the request still
     * does not fit, fail loudly instead of spinning forever. */
    for (;;) {
        lc_transfer_entry *oldest = NULL;
        uint64_t oldest_value = 0;
        int first = 1;
        lc_transfer_entry *it;
        lc_result res;

        lc_device_lock_transfer(device);
        if (device->staging_used <= limit - need) {
            device->staging_used += need;
            if (device->staging_used > device->staging_high_water) {
                device->staging_high_water = device->staging_used;
            }
            lc_device_unlock_transfer(device);
            return LC_SUCCESS;
        }
        /* Reclaim first (non-blocking), then wait oldest. */
        lc_device_unlock_transfer(device);
        lc_vk_reclaim_completed(device);
        lc_device_lock_transfer(device);
        if (device->staging_used <= limit - need) {
            device->staging_used += need;
            if (device->staging_used > device->staging_high_water) {
                device->staging_high_water = device->staging_used;
            }
            lc_device_unlock_transfer(device);
            return LC_SUCCESS;
        }
        for (it = device->transfers; it != NULL; it = it->next) {
            if (!it->staging_released && !it->is_readback &&
                it->submitted &&
                (first || it->signal_value < oldest_value)) {
                oldest = it;
                oldest_value = it->signal_value;
                first = 0;
            }
        }
        if (oldest != NULL) {
            device->stat_staging_waits++;
        }
        lc_device_unlock_transfer(device);
        if (oldest == NULL) {
            /* Only readback-pinned (or no) staging remains. If it
             * fits now something raced us free; else the pin set
             * genuinely exceeds the cap: fail loudly. */
            lc_vk_reclaim_completed(device);
            lc_device_lock_transfer(device);
            if (device->staging_used <= limit - need) {
                device->staging_used += need;
                if (device->staging_used >
                    device->staging_high_water) {
                    device->staging_high_water = device->staging_used;
                }
                lc_device_unlock_transfer(device);
                return LC_SUCCESS;
            }
            lc_device_unlock_transfer(device);
            return LC_ERROR_OUT_OF_MEMORY;
        }
        res = lc_vk_signal_wait(device, oldest_value,
                                LC_TIMEOUT_INFINITE);
        if (res != LC_SUCCESS) {
            return res;
        }
        lc_vk_reclaim_completed(device);
    }
}

static void lc_vk_staging_release_locked(lc_device *device,
                                         uint64_t bytes) {
    if (device->staging_used >= bytes) {
        device->staging_used -= bytes;
    } else {
        device->staging_used = 0;
    }
}

/* ------------------------------------------------------------------ */
/* Retirement (PARTs AB–AD).                                           */
/* ------------------------------------------------------------------ */

void lc_vk_retire(lc_device *device, const lc_retire_entry *entry) {
    lc_retire_entry *copy = NULL;
    int active_frame = 0;

    if (device == NULL || entry == NULL) {
        return;
    }
    copy = (lc_retire_entry *)malloc(sizeof(lc_retire_entry));
    if (copy == NULL) {
        /* Host OOM during retire: leak loudly rather than free
         * in-use GPU objects (Section 13). */
        fprintf(stderr,
                "[lumac] retirement allocation failed; GPU resource "
                "leaked (no use-after-free)\n");
        return;
    }
    memcpy(copy, entry, sizeof(*copy));
    copy->next = NULL;
    copy->prev = NULL;
    lc_device_lock_transfer(device);
    /* Horizon: the newest submitted transfer value and the newest
     * frame value (linked values only — reserved-but-failed
     * schedules never link, so gaps cannot stall reclamation).
     * Frame submits signal the shared timeline, so this covers
     * frame work too; fallback mode ignores the value and gates
     * on device quiet instead. */
    copy->signal_value = device->last_frame_value;
    {
        lc_state *state = lc_get_internal_state();
        const lc_swapchain *sw;

        if (state != NULL) {
            for (sw = state->swapchains; sw != NULL; sw = sw->next) {
                if (sw->device == device && sw->frame_active) {
                    active_frame = 1;
                    break;
                }
            }
        }
    }
    if (active_frame && device->timeline_ok) {
        copy->signal_value = UINT64_MAX;
    }
    {
        lc_transfer_entry *tr;

        for (tr = device->transfers; tr != NULL; tr = tr->next) {
            if (tr->submitted && tr->signal_value > copy->signal_value) {
                copy->signal_value = tr->signal_value;
            }
        }
    }
    if (device->retire_tail != NULL) {
        device->retire_tail->next = copy;
        copy->prev = device->retire_tail;
        device->retire_tail = copy;
    } else {
        device->retire_head = copy;
        device->retire_tail = copy;
    }
    device->retire_pending_count++;
    if (device->retire_pending_count > device->retire_high_water) {
        device->retire_high_water = device->retire_pending_count;
    }
    lc_device_unlock_transfer(device);

    /* Preserve immediate-destruction behavior when there is no GPU
     * dependency at all.  This is both cheaper in steady state and
     * important for allocator churn: a resource created and destroyed
     * before any submission has nothing to retire behind.  Entries with
     * a real completion horizon remain queued. */
    lc_vk_reclaim_completed(device);
}

void lc_vk_retire_bind_active_frame(lc_device *device, uint64_t value) {
    lc_retire_entry *it;

    if (device == NULL || value == 0) {
        return;
    }
    lc_device_lock_transfer(device);
    for (it = device->retire_head; it != NULL; it = it->next) {
        if (it->signal_value == UINT64_MAX) {
            it->signal_value = value;
        }
    }
    lc_device_unlock_transfer(device);
    lc_vk_cmdlist_reclaim_completed(device);
}

/* True when a retirement entry's GPU work finished (no waiting).
 * Timeline mode gates on the shared counter (frame submits signal
 * it too, so the horizon covers frame work). Fallback mode has no
 * device-wide fence, so retirement additionally requires full
 * quiet (no active frames, no in-flight transfers). */
static int lc_vk_retire_due(lc_device *device,
                            const lc_retire_entry *entry, int quiet) {
    if (device->timeline_ok) {
        return (lc_vk_timeline_counter(device) >= entry->signal_value ||
                entry->signal_value == 0)
                   ? 1
                   : 0;
    }
    if (!quiet) {
        return 0;
    }
    if (entry->fallback_fence == VK_NULL_HANDLE ||
        device->device == VK_NULL_HANDLE) {
        return 1;
    }
    return (vkGetFenceStatus(device->device, entry->fallback_fence) ==
            VK_SUCCESS)
               ? 1
               : 0;
}

static void lc_vk_retire_execute(lc_device *device,
                                 lc_retire_entry *entry) {
    VkDevice dev = device->device;

    if (dev == VK_NULL_HANDLE) {
        return;
    }
    switch (entry->kind) {
    case LC_RETIRE_BUFFER:
        if (entry->buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(dev, entry->buffer, NULL);
        }
        break;
    case LC_RETIRE_IMAGE:
        if (entry->image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, entry->image_view, NULL);
        }
        if (entry->default_view != VK_NULL_HANDLE &&
            entry->default_view != entry->image_view) {
            vkDestroyImageView(dev, entry->default_view, NULL);
        }
        if (entry->image != VK_NULL_HANDLE) {
            vkDestroyImage(dev, entry->image, NULL);
        }
        break;
    case LC_RETIRE_VIEW:
        if (entry->image_view != VK_NULL_HANDLE) {
            vkDestroyImageView(dev, entry->image_view, NULL);
        }
        break;
    case LC_RETIRE_SAMPLER:
        if (entry->sampler != VK_NULL_HANDLE) {
            vkDestroySampler(dev, entry->sampler, NULL);
        }
        break;
    case LC_RETIRE_PIPELINE:
        if (entry->pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(dev, entry->pipeline, NULL);
        }
        if (entry->pipeline_layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(dev, entry->pipeline_layout, NULL);
        }
        break;
    case LC_RETIRE_SET:
        if (entry->set != VK_NULL_HANDLE &&
            entry->set_pool != VK_NULL_HANDLE) {
            vkFreeDescriptorSets(dev, entry->set_pool, 1, &entry->set);
        }
        break;
    case LC_RETIRE_LAYOUT:
        if (entry->desc_layout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(dev, entry->desc_layout,
                                         NULL);
        }
        break;
    case LC_RETIRE_FRAMEBUFFER:
        if (entry->framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(dev, entry->framebuffer, NULL);
        }
        break;
    }
    {
        lc_vk_mem_binding binding;

        memset(&binding, 0, sizeof(binding));
        binding.memory = entry->binding.memory;
        binding.offset = entry->binding.offset;
        binding.size = entry->binding.size;
        binding.mapped = entry->binding.mapped;
        binding.coherent = entry->binding.coherent;
        binding.dedicated = entry->binding.dedicated;
        binding.block = entry->binding.block;
        if (binding.memory != VK_NULL_HANDLE) {
            lc_vk_mem_free(device, &binding);
        }
    }
    if (entry->fallback_fence != VK_NULL_HANDLE) {
        vkDestroyFence(dev, entry->fallback_fence, NULL);
    }
}

/* Device fully quiet: nothing submitted-but-incomplete. Timeline
 * mode queries the counter; fallback requires no active frames and
 * no in-flight transfers (conservative but correct). */
static int lc_vk_device_quiet(lc_device *device) {
    if (device == NULL) {
        return 1;
    }
    if (device->timeline_ok) {
        return (device->timeline_next <= 1 ||
                lc_vk_timeline_counter(device) >=
                    device->timeline_next - 1)
                   ? 1
                   : 0;
    }
    {
        lc_state *state = lc_get_internal_state();
        const lc_swapchain *sw;

        if (state != NULL) {
            for (sw = state->swapchains; sw != NULL; sw = sw->next) {
                uint32_t s;

                if (sw->device != device) {
                    continue;
                }
                if (sw->frame_active) {
                    return 0;
                }
                for (s = 0; s < sw->max_flights; s++) {
                    VkFence fence = sw->flights[s].fence;

                    if (fence != VK_NULL_HANDLE &&
                        vkGetFenceStatus(device->device, fence) ==
                            VK_NOT_READY) {
                        return 0;
                    }
                }
            }
        }
    }
    lc_device_lock_transfer(device);
    {
        int busy = (device->transfers != NULL) ? 1 : 0;

        lc_device_unlock_transfer(device);
        return !busy;
    }
}

void lc_vk_reclaim_completed(lc_device *device) {
    lc_retire_entry *it;
    lc_retire_entry *next;
    lc_transfer_entry *tr;
    lc_transfer_entry *tr_next;
    lc_transfer_entry *doomed = NULL;
    int quiet = 0;

    if (device == NULL) {
        return;
    }
    /* Fallback retirement gates on full-device quiet (computed
     * before locking; it locks internally where needed). */
    quiet = lc_vk_device_quiet(device);
    lc_device_lock_transfer(device);
    /* Phase 1 (transfer shard): completion tests, staging release,
     * unlink splicing. No pool objects are freed here — command
     * buffers live in the submit domain (Phase 22) and are freed
     * in phase 2 below. vkDestroyBuffer/mem frees need no shard. */
    for (tr = device->transfers; tr != NULL; tr = tr_next) {
        int done = 0;

        tr_next = tr->next;
        if (!tr->submitted) {
            done = 0;
        } else if (device->timeline_ok) {
            done = (tr->signal_value == 0 ||
                    lc_vk_timeline_counter(device) >= tr->signal_value)
                       ? 1
                       : 0;
        } else if (tr->fallback_fence != VK_NULL_HANDLE &&
                   device->device != VK_NULL_HANDLE) {
            done = (vkGetFenceStatus(device->device,
                                     tr->fallback_fence) == VK_SUCCESS)
                       ? 1
                       : 0;
        } else {
            done = 1;
        }
        /* Readback staging survives (it holds the result until
         * destroy); everything else releases its staging at
         * completion so pool memory recycles promptly. */
        if (done && !tr->is_readback && !tr->staging_released) {
            if (tr->stage_buffer != VK_NULL_HANDLE &&
                device->device != VK_NULL_HANDLE) {
                vkDestroyBuffer(device->device, tr->stage_buffer,
                                NULL);
                tr->stage_buffer = VK_NULL_HANDLE;
            }
            if (tr->stage_mem.memory != VK_NULL_HANDLE) {
                lc_vk_mem_free(device, &tr->stage_mem);
                memset(&tr->stage_mem, 0, sizeof(tr->stage_mem));
            }
            lc_vk_staging_release_locked(device, tr->stage_bytes);
            tr->stage_bytes = 0;
            tr->staging_released = 1;
            if (device->stat_uploads_in_flight > 0) {
                device->stat_uploads_in_flight--;
            }
        }
        /* Entries unlink once staging is gone AND graphics has
         * acquired (or no acquire was needed: buffers link
         * acquired since Phase 22, images ride the graphics
         * queue). destroy_claimed entries are mid-acquire in
         * wait_for: reclaim skips them until the claim clears.
         * Readbacks stay linked until their request is destroyed
         * (staging holds the result); acquires for the frame path
         * arrive via emit, for destroy via wait_for (reclaim
         * never submits). Unlinked entries join the doomed list;
         * their Vulkan objects die in phase 2 under submit. */
        if (!tr->keep_until_destroy && !tr->destroy_claimed && done &&
            tr->staging_released && tr->acquired) {
            if (tr->prev != NULL) {
                tr->prev->next = tr->next;
            } else {
                device->transfers = tr->next;
            }
            if (tr->next != NULL) {
                tr->next->prev = tr->prev;
            }
            tr->next = doomed;
            tr->prev = NULL;
            doomed = tr;
            device->stat_reclaimed_transfers++;
        }
    }
    /* Retired resources whose GPU work finished. */
    for (it = device->retire_head; it != NULL; it = next) {
        next = it->next;
        if (!lc_vk_retire_due(device, it, quiet)) {
            continue;
        }
        if (it->prev != NULL) {
            it->prev->next = it->next;
        } else {
            device->retire_head = it->next;
        }
        if (it->next != NULL) {
            it->next->prev = it->prev;
        } else {
            device->retire_tail = it->prev;
        }
        if (device->retire_pending_count > 0) {
            device->retire_pending_count--;
        }
        lc_vk_retire_execute(device, it);
        device->stat_retired_completed++;
        free(it);
    }
    lc_device_unlock_transfer(device);
    /* Phase 2 (submit shard, never nested: transfer is released
     * above): destroy unlinked entries' completion objects and
     * free their pool commands. Serialized against frame submits,
     * so no emit-collected semaphore can die mid-submit. */
    if (doomed != NULL) {
        lc_device_lock_submit(device);
        while (doomed != NULL) {
            tr = doomed;
            doomed = doomed->next;
            if (tr->fallback_fence != VK_NULL_HANDLE &&
                device->device != VK_NULL_HANDLE) {
                vkDestroyFence(device->device, tr->fallback_fence,
                               NULL);
            }
            if (device->device != VK_NULL_HANDLE) {
                if (tr->fallback_rel != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device->device, tr->fallback_rel,
                                       NULL);
                }
                if (tr->fallback_copy != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device->device, tr->fallback_copy,
                                       NULL);
                }
            }
            lc_vk_transfer_cmd_free_entry(device, tr);
            free(tr);
        }
        lc_device_unlock_submit(device);
    }
    lc_vk_cmdlist_reclaim_completed(device);
}

void lc_vk_retire_flush_all(lc_device *device) {
    lc_retire_entry *it;

    if (device == NULL) {
        return;
    }
    /* Shutdown path only: wait everything out, then free. */
    if (device->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->device);
    }
    /* Live readback requests die with the device (their entries
     * unlink here, so the transfer sweep below never sees them). */
    lc_vk_requests_flush_all(device);
    lc_device_lock_transfer(device);
    for (it = device->retire_head; it != NULL;) {
        lc_retire_entry *next = it->next;

        if (it->prev != NULL) {
            it->prev->next = it->next;
        } else {
            device->retire_head = it->next;
        }
        if (it->next != NULL) {
            it->next->prev = it->prev;
        } else {
            device->retire_tail = it->prev;
        }
        lc_device_unlock_transfer(device);
        lc_vk_retire_execute(device, it);
        free(it);
        lc_device_lock_transfer(device);
        device->stat_retired_completed++;
        it = next;
    }
    lc_device_unlock_transfer(device);
    device->retire_pending_count = 0;
    /* Transfer entries: the GPU is idle; entries may still borrow
     * dead wrappers — touch only Vk handles + bindings here.
     * Staging dies now; completion objects + pool commands die in
     * the submit section below (Phase 22 pool domain). */
    {
        lc_transfer_entry *tr = device->transfers;
        lc_transfer_entry *tr_next;
        lc_transfer_entry *doomed = NULL;

        for (; tr != NULL; tr = tr_next) {
            tr_next = tr->next;
            if (tr->stage_buffer != VK_NULL_HANDLE &&
                device->device != VK_NULL_HANDLE) {
                vkDestroyBuffer(device->device, tr->stage_buffer,
                                NULL);
                tr->stage_buffer = VK_NULL_HANDLE;
            }
            if (tr->stage_mem.memory != VK_NULL_HANDLE) {
                lc_vk_mem_free(device, &tr->stage_mem);
                memset(&tr->stage_mem, 0, sizeof(tr->stage_mem));
            }
            tr->next = doomed;
            tr->prev = NULL;
            doomed = tr;
        }
        device->transfers = NULL;
        device->staging_used = 0;
        device->stat_uploads_in_flight = 0;
        lc_device_unlock_transfer(device);
        lc_device_lock_submit(device);
        while (doomed != NULL) {
            tr = doomed;
            doomed = doomed->next;
            if (tr->fallback_fence != VK_NULL_HANDLE &&
                device->device != VK_NULL_HANDLE) {
                vkDestroyFence(device->device, tr->fallback_fence,
                               NULL);
            }
            if (device->device != VK_NULL_HANDLE) {
                if (tr->fallback_rel != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device->device, tr->fallback_rel,
                                       NULL);
                }
                if (tr->fallback_copy != VK_NULL_HANDLE) {
                    vkDestroySemaphore(device->device, tr->fallback_copy,
                                       NULL);
                }
            }
            lc_vk_transfer_cmd_free_entry(device, tr);
            free(tr);
        }
        lc_device_unlock_submit(device);
        return;
    }
}

void lc_vk_transfer_shutdown(lc_device *device) {
    if (device == NULL) {
        return;
    }
    lc_vk_retire_flush_all(device);
    /* Isolated compute once-submits drain here (test path only). */
    lc_vk_compute_pool_shutdown(device);
    lc_vk_worker_shutdown(device);
    if (device->transfer_pool != VK_NULL_HANDLE &&
        device->device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device->device, device->transfer_pool,
                             NULL);
    }
    device->transfer_pool = VK_NULL_HANDLE;
    device->transfer_cmds_outstanding = 0;
    if (device->imm_pool != VK_NULL_HANDLE &&
        device->device != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device->device, device->imm_pool, NULL);
    }
    device->imm_pool = VK_NULL_HANDLE;
    device->imm_cmds_outstanding = 0;
#if defined(_WIN32) || defined(_WIN64)
    if (device->transfer_mutex_init) {
        DeleteCriticalSection(&device->transfer_mutex);
        device->transfer_mutex_init = 0;
    }
#else
    if (device->transfer_mutex_init) {
        pthread_mutex_destroy(&device->transfer_mutex);
        device->transfer_mutex_init = 0;
    }
#endif
}

/* Wait for in-flight transfers touching one wrapper, then drop
 * their borrowed refs (destroy path; bounded — the GPU always
 * progresses). Dedicated-path entries also get their graphics
 * acquire driven to completion here (immediate submit + fence),
 * so the retired Vk objects are safe the moment this returns.
 * Frame-riding acquires (claimed by emit) are covered by waiting
 * every frame slot fence afterwards. */
void lc_vk_transfer_wait_for(lc_device *device, const lc_image *image,
                             const lc_buffer *buffer) {
    uint64_t wait_value = 0;
    lc_transfer_entry *claim[64];
    int again = 0;

    if (device == NULL || (image == NULL && buffer == NULL)) {
        return;
    }
    if (device->device == VK_NULL_HANDLE) {
        return;
    }
    /* Phase 22 publication atomicity, fallback side: timeline waits
     * terminate on reserved values via submit/host-signal, but the
     * fallback wait skips unsubmitted placeholders. Settle them
     * first (bounded: schedules publish within microseconds — link
     * to publish performs no blocking call — so 1000x1ms is an
     * enormous margin, never a hang). No lock is held while
     * sleeping. */
    if (!device->timeline_ok) {
        int spins = 0;

        for (;;) {
            int pending = 0;
            lc_transfer_entry *it = NULL;

            lc_device_lock_transfer(device);
            for (it = device->transfers; it != NULL; it = it->next) {
                int hit = 0;

                if (image != NULL && it->is_image &&
                    it->image == image) {
                    hit = 1;
                }
                if (buffer != NULL && !it->is_image &&
                    it->buffer == buffer) {
                    hit = 1;
                }
                if (hit && !it->submitted) {
                    pending = 1;
                    break;
                }
            }
            lc_device_unlock_transfer(device);
            if (!pending || spins++ >= 1000) {
                break;
            }
#if defined(_WIN32) || defined(_WIN64)
            Sleep(1);
#else
            {
                struct timespec ts;

                ts.tv_sec = 0;
                ts.tv_nsec = 1000000;
                nanosleep(&ts, NULL);
            }
#endif
        }
    }
    do {
        int nclaim = 0;
        int i = 0;

        again = 0;
        lc_device_lock_transfer(device);
        for (lc_transfer_entry *it = device->transfers; it != NULL;
             it = it->next) {
            int hit = 0;

            if (image != NULL && it->is_image && it->image == image) {
                hit = 1;
            }
            if (buffer != NULL && !it->is_image &&
                it->buffer == buffer) {
                hit = 1;
            }
            if (!hit) {
                continue;
            }
            if (it->signal_value > wait_value) {
                wait_value = it->signal_value;
            }
            /* Claim un-acquired dedicated entries for a synchronous
             * acquire below (readbacks included: their staging
             * stays until request destroy, but ownership must
             * return before the image retires). */
            if (!it->acquired && it->submitted &&
                it->cmd_rel != VK_NULL_HANDLE) {
                if (nclaim < 64) {
                    it->acquired = 1;
                    it->destroy_claimed = 1;
                    claim[nclaim++] = it;
                } else {
                    again = 1;
                }
                continue;
            }
            if (it->is_image) {
                it->image = NULL;
            } else {
                it->buffer = NULL;
            }
        }
        lc_device_unlock_transfer(device);
        if (wait_value != 0) {
            lc_vk_signal_wait(device, wait_value, LC_TIMEOUT_INFINITE);
            lc_vk_reclaim_completed(device);
        }
        /* Drive claimed acquires to completion (copies are done:
         * no semaphore waits needed, just submit + fence-wait). */
        for (i = 0; i < nclaim; i++) {
            lc_transfer_entry *tr = claim[i];
            VkCommandBuffer acq = VK_NULL_HANDLE;
            VkFence fence = VK_NULL_HANDLE;
            lc_result res = LC_SUCCESS;

            lc_device_lock_submit(device);
            if (res == LC_SUCCESS) {
                res = lc_vk_imm_cmd_alloc(device, &acq);
            }
            if (res == LC_SUCCESS) {
                res = lc_vk_transfer_cmd_begin(device, acq);
            }
            if (res == LC_SUCCESS) {
                res = lc_vk_xfer_record_acquire(device, acq, tr);
            }
            if (res == LC_SUCCESS) {
                res = (vkEndCommandBuffer(acq) == VK_SUCCESS)
                          ? LC_SUCCESS
                          : LC_ERROR_UNKNOWN;
            }
            if (res == LC_SUCCESS) {
                res = lc_vk_xfer_fence_create(device, &fence);
            }
            if (res == LC_SUCCESS) {
                res = lc_vk_xfer_submit(device, device->graphics_queue,
                                        acq, 0, NULL, NULL, NULL, 0,
                                        NULL, NULL, fence);
            }
            lc_device_unlock_submit(device);
            if (res == LC_SUCCESS) {
                vkWaitForFences(device->device, 1, &fence, VK_TRUE,
                                UINT64_MAX);
            }
            /* Pool free under submit (no shard held here). */
            lc_device_lock_submit(device);
            lc_vk_imm_cmd_free(device, acq);
            lc_device_unlock_submit(device);
            if (fence != VK_NULL_HANDLE) {
                vkDestroyFence(device->device, fence, NULL);
            }
            lc_device_lock_transfer(device);
            if (res == LC_SUCCESS) {
                lc_vk_xfer_restore_owners(device, tr);
                if (tr->is_image) {
                    tr->image = NULL;
                } else {
                    tr->buffer = NULL;
                }
            } else {
                /* Submit failed: release the claim for a later
                 * pass (fatal paths report through the caller). */
                tr->acquired = 0;
            }
            tr->destroy_claimed = 0;
            lc_device_unlock_transfer(device);
        }
    } while (again);
    /* Do not wait every frame-slot fence here: an open frame has reset
     * its fence but cannot signal it until the caller submits, making
     * destruction inside recording deadlock. Already-recorded acquire
     * barriers contain native handles, and deferred retirement retains
     * those handles through the latest submitted frame value. */
    lc_vk_reclaim_completed(device);
}

uint64_t lc_vk_transfer_max_inflight(lc_device *device) {
    lc_transfer_entry *it;
    uint64_t max = 0;

    if (device == NULL) {
        return 0;
    }
    lc_device_lock_transfer(device);
    for (it = device->transfers; it != NULL; it = it->next) {
        if (it->signal_value > max) {
            max = it->signal_value;
        }
    }
    lc_device_unlock_transfer(device);
    return max;
}

/* ------------------------------------------------------------------ */
/* Immediate graphics pool (release submits).                          */
/* ------------------------------------------------------------------ */

/* Graphics-family transient pool for release/acquire commands.
 * Separate from the transfer pool (different family when
 * dedicated); same reset-when-idle discipline. Caller holds the
 * submit lock. */
static lc_result lc_vk_imm_pool_ensure(lc_device *device) {
    VkCommandPoolCreateInfo info;

    if (device->imm_pool != VK_NULL_HANDLE) {
        return LC_SUCCESS;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT |
                 VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = device->graphics_queue_family;
    if (vkCreateCommandPool(device->device, &info, NULL,
                            &device->imm_pool) != VK_SUCCESS) {
        device->imm_pool = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    return LC_SUCCESS;
}

static lc_result lc_vk_imm_cmd_alloc(lc_device *device,
                                     VkCommandBuffer *out) {
    VkCommandBufferAllocateInfo info;
    lc_result res;

    res = lc_vk_imm_pool_ensure(device);
    if (res != LC_SUCCESS) {
        return res;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    info.commandPool = device->imm_pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device->device, &info, out) !=
        VK_SUCCESS) {
        *out = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    device->imm_cmds_outstanding++;
    return LC_SUCCESS;
}

static void lc_vk_imm_cmd_free(lc_device *device,
                               VkCommandBuffer cmd) {
    if (device == NULL || cmd == VK_NULL_HANDLE ||
        device->imm_pool == VK_NULL_HANDLE ||
        device->device == VK_NULL_HANDLE) {
        return;
    }
    /* Phase 22: imm-pool objects live in the submit domain. */
    assert(lc_device_submit_held());
    vkFreeCommandBuffers(device->device, device->imm_pool, 1, &cmd);
    if (device->imm_cmds_outstanding > 0) {
        device->imm_cmds_outstanding--;
    }
    if (device->imm_cmds_outstanding == 0) {
        vkResetCommandPool(device->device, device->imm_pool, 0);
    }
}

/* Free an entry's recorded commands (NULL-tolerant; pools may be
 * gone on the shutdown path). */
static void lc_vk_transfer_cmd_free_entry(lc_device *device,
                                          lc_transfer_entry *tr) {
    if (device == NULL || tr == NULL) {
        return;
    }
    if (tr->cmd != VK_NULL_HANDLE) {
        lc_vk_transfer_copy_cmd_free(device, tr);
    }
    if (tr->cmd_rel != VK_NULL_HANDLE) {
        lc_vk_imm_cmd_free(device, tr->cmd_rel);
        tr->cmd_rel = VK_NULL_HANDLE;
    }
}

static void lc_vk_transfer_copy_cmd_free(lc_device *device,
                                         lc_transfer_entry *tr) {
    VkCommandBuffer cmd;

    if (device == NULL || tr == NULL || tr->cmd == VK_NULL_HANDLE) {
        return;
    }
    cmd = tr->cmd;
    tr->cmd = VK_NULL_HANDLE;
    if (tr->cmd_on_transfer_pool) {
        lc_vk_transfer_cmd_free(device, cmd);
    } else {
        lc_vk_imm_cmd_free(device, cmd);
    }
}

/* ------------------------------------------------------------------ */
/* Async staging (CONCURRENT sharing when dedicated).                  */
/* ------------------------------------------------------------------ */

/* Staging buffers outlive the schedule call (freed at GPU
 * completion), so they must be legally accessed from both queues:
 * CONCURRENT sharing across graphics+transfer when dedicated,
 * EXCLUSIVE otherwise (mirrors lc_vk_stage_acquire's binding
 * discipline). */
static lc_result lc_vk_xfer_stage_acquire(lc_device *device,
                                          uint64_t size, int upload,
                                          VkBufferUsageFlags usage,
                                          VkBuffer *out_buffer,
                                          lc_vk_mem_binding *out_binding,
                                          void **out_mapped) {
    VkBufferCreateInfo info;
    VkMemoryRequirements reqs;
    VkBuffer buffer = VK_NULL_HANDLE;
    uint32_t families[2];
    lc_result res;

    if (device == NULL || size == 0 || out_buffer == NULL ||
        out_binding == NULL || out_mapped == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = (VkDeviceSize)size;
    info.usage = usage;
    if (device->has_dedicated_transfer) {
        families[0] = device->graphics_queue_family;
        families[1] = device->transfer_queue_family;
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    } else {
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    if (vkCreateBuffer(device->device, &info, NULL, &buffer) !=
        VK_SUCCESS) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    vkGetBufferMemoryRequirements(device->device, buffer, &reqs);
    res = lc_vk_mem_alloc(
        device, upload ? LC_VK_MEM_UPLOAD : LC_VK_MEM_READBACK,
        reqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
        reqs.size, reqs.alignment, out_binding);
    if (res != LC_SUCCESS) {
        vkDestroyBuffer(device->device, buffer, NULL);
        return res;
    }
    if (vkBindBufferMemory(device->device, buffer, out_binding->memory,
                           (VkDeviceSize)out_binding->offset) !=
        VK_SUCCESS) {
        lc_vk_mem_free(device, out_binding);
        vkDestroyBuffer(device->device, buffer, NULL);
        return LC_ERROR_UNKNOWN;
    }
    *out_buffer = buffer;
    *out_mapped = out_binding->mapped;
    return LC_SUCCESS;
}

static void lc_vk_xfer_stage_release(lc_device *device, VkBuffer buffer,
                                     lc_vk_mem_binding *binding) {
    if (device == NULL) {
        return;
    }
    if (buffer != VK_NULL_HANDLE && device->device != VK_NULL_HANDLE) {
        vkDestroyBuffer(device->device, buffer, NULL);
    }
    lc_vk_mem_free(device, binding);
}

/* Mip extent for one level (minimum 1 per axis; mirrors the sync
 * upload path's arithmetic). */
static void lc_vk_xfer_mip_extent(const lc_image *image, uint32_t mip,
                                  uint32_t *w, uint32_t *h,
                                  uint32_t *d) {
    *w = image->width >> mip;
    *h = image->height >> mip;
    *d = image->depth >> mip;
    if (*w == 0) {
        *w = 1;
    }
    if (*h == 0) {
        *h = 1;
    }
    if (*d == 0) {
        *d = 1;
    }
    if (image->type == LC_IMAGE_TYPE_1D) {
        *h = 1;
        *d = 1;
    } else if (image->type == LC_IMAGE_TYPE_2D) {
        *d = 1;
    }
}

/* ------------------------------------------------------------------ */
/* Ownership barriers (raw families; sync layer stays IGNORED).        */
/* ------------------------------------------------------------------ */

/* One image ownership/layout barrier with explicit families. */
static lc_result lc_vk_xfer_image_barrier(
    VkCommandBuffer cmd, lc_image *image, uint32_t base_mip,
    uint32_t level_count, uint32_t base_layer, uint32_t layer_count,
    lc_resource_state old_state, lc_resource_state new_state,
    uint32_t src_family, uint32_t dst_family) {
    VkImageLayout old_layout;
    VkImageLayout new_layout;
    VkPipelineStageFlags src_stage = 0;
    VkPipelineStageFlags dst_stage = 0;
    VkAccessFlags src_access = 0;
    VkAccessFlags dst_access = 0;
    VkImageMemoryBarrier barrier;

    if (!lc_vk_sync_barrier_params(old_state, &old_layout, &src_stage,
                                   &src_access) ||
        !lc_vk_sync_barrier_params(new_state, &new_layout, &dst_stage,
                                   &dst_access)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = src_family;
    barrier.dstQueueFamilyIndex = dst_family;
    barrier.image = image->vk_image;
    barrier.subresourceRange.aspectMask =
        lc_vk_aspect_for(image->format);
    barrier.subresourceRange.baseMipLevel = base_mip;
    barrier.subresourceRange.levelCount = level_count;
    barrier.subresourceRange.baseArrayLayer = base_layer;
    barrier.subresourceRange.layerCount = layer_count;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0,
                         NULL, 1, &barrier);
    return LC_SUCCESS;
}

/* One buffer ownership/availability barrier with explicit
 * families. */
static void lc_vk_xfer_buffer_barrier(VkCommandBuffer cmd,
                                      const lc_buffer *buffer,
                                      uint64_t offset, uint64_t size,
                                      VkPipelineStageFlags src_stage,
                                      VkAccessFlags src_access,
                                      VkPipelineStageFlags dst_stage,
                                      VkAccessFlags dst_access,
                                      uint32_t src_family,
                                      uint32_t dst_family) {
    VkBufferMemoryBarrier barrier;

    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    barrier.srcQueueFamilyIndex = src_family;
    barrier.dstQueueFamilyIndex = dst_family;
    barrier.buffer = buffer->vk_buffer;
    barrier.offset = (VkDeviceSize)offset;
    barrier.size = (VkDeviceSize)size;
    vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, NULL, 1,
                         &barrier, 0, NULL);
}

/* ------------------------------------------------------------------ */
/* Semaphores, fences, submits (caller holds the submit lock).         */
/* ------------------------------------------------------------------ */

static lc_result lc_vk_xfer_sem_create(lc_device *device,
                                       VkSemaphore *out) {
    VkSemaphoreCreateInfo info;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(device->device, &info, NULL, out) !=
        VK_SUCCESS) {
        *out = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    return LC_SUCCESS;
}

static lc_result lc_vk_xfer_fence_create(lc_device *device,
                                         VkFence *out) {
    VkFenceCreateInfo info;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (vkCreateFence(device->device, &info, NULL, out) !=
        VK_SUCCESS) {
        *out = VK_NULL_HANDLE;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    return LC_SUCCESS;
}

/* One queue submit with mixed binary/timeline waits+signals.
 * NULL value arrays mean all-binary; timeline entries are skipped
 * when the device runs the fence fallback. */
static lc_result lc_vk_xfer_submit(
    lc_device *device, VkQueue queue, VkCommandBuffer cmd,
    uint32_t wait_count, const VkSemaphore *waits,
    const VkPipelineStageFlags *wait_stages, const uint64_t *wait_vals,
    uint32_t sig_count, const VkSemaphore *sigs,
    const uint64_t *sig_vals, VkFence fence) {
    VkSubmitInfo submit_info;
    VkTimelineSemaphoreSubmitInfo tinfo;
    VkSemaphore wait_sems[4];
    VkPipelineStageFlags wait_masks[4];
    uint64_t wait_values[4];
    VkSemaphore sig_sems[4];
    uint64_t sig_values[4];
    uint32_t nw = 0;
    uint32_t ns = 0;
    uint32_t i;

    if (wait_count > 3 || sig_count > 3) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < wait_count; i++) {
        uint64_t v = (wait_vals != NULL) ? wait_vals[i] : 0;

        if (v != 0 && !device->timeline_ok) {
            continue;
        }
        wait_sems[nw] = waits[i];
        wait_masks[nw] = wait_stages[i];
        wait_values[nw] = v;
        nw++;
    }
    for (i = 0; i < sig_count; i++) {
        uint64_t v = (sig_vals != NULL) ? sig_vals[i] : 0;

        if (v != 0 && !device->timeline_ok) {
            continue;
        }
        sig_sems[ns] = sigs[i];
        sig_values[ns] = v;
        ns++;
    }
    if (device->timeline_ok) {
        memset(&tinfo, 0, sizeof(tinfo));
        tinfo.sType =
            VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        tinfo.waitSemaphoreValueCount = nw;
        tinfo.pWaitSemaphoreValues = wait_values;
        tinfo.signalSemaphoreValueCount = ns;
        tinfo.pSignalSemaphoreValues = sig_values;
    }
    memset(&submit_info, 0, sizeof(submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (device->timeline_ok) {
        submit_info.pNext = &tinfo;
    }
    submit_info.waitSemaphoreCount = nw;
    submit_info.pWaitSemaphores = wait_sems;
    submit_info.pWaitDstStageMask = wait_masks;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &cmd;
    submit_info.signalSemaphoreCount = ns;
    submit_info.pSignalSemaphores = sig_sems;
    if (vkQueueSubmit(queue, 1, &submit_info, fence) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    return LC_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Schedule core (phased single-lock disciplines; never nested).        */
/* ------------------------------------------------------------------ */

static int lc_vk_xfer_live_buffer(lc_device *device,
                                  const lc_buffer *buffer) {
    lc_state *state = lc_get_internal_state();
    const lc_buffer *it;

    if (state == NULL || device == NULL || buffer == NULL) {
        return 0;
    }
    if (buffer->device != device) {
        return 0;
    }
    for (it = state->buffers; it != NULL; it = it->next) {
        if (it == buffer) {
            return 1;
        }
    }
    return 0;
}

static int lc_vk_xfer_live_image(lc_device *device,
                                 const lc_image *image) {
    lc_state *state = lc_get_internal_state();
    const lc_image *it;

    if (state == NULL || device == NULL || image == NULL) {
        return 0;
    }
    if (image->device != device) {
        return 0;
    }
    for (it = state->images; it != NULL; it = it->next) {
        if (it == image) {
            return 1;
        }
    }
    return 0;
}

/* Release cap accounting after a failed schedule (transfer lock). */
static void lc_vk_staging_unreserve(lc_device *device, uint64_t bytes) {
    lc_device_lock_transfer(device);
    lc_vk_staging_release_locked(device, bytes);
    lc_device_unlock_transfer(device);
}

/* Mark image states after a scheduled copy (state lock). Owners
 * move separately: the transfer lock owns the ownership domain
 * (schedule snapshots, emit, destroy all serialize there), so no
 * lock ever nests. */
static void lc_vk_xfer_mark_image_states(lc_device *device,
                                         lc_transfer_entry *tr) {
    uint32_t mip;
    uint32_t layer;
    lc_image *image = tr->image;

    lc_device_lock_state(device);
    for (layer = tr->base_layer;
         layer < tr->base_layer + tr->layer_count; layer++) {
        for (mip = tr->base_mip; mip < tr->base_mip + tr->level_count;
             mip++) {
            uint64_t idx =
                (uint64_t)layer * (uint64_t)image->mip_levels +
                (uint64_t)mip;

            image->states[idx] = tr->final_state;
        }
    }
    lc_device_unlock_state(device);
}

/* Mark image ownership after a scheduled copy (transfer lock
 * held by the caller). */
static void lc_vk_xfer_mark_image_owners(lc_device *device,
                                          lc_transfer_entry *tr,
                                          uint32_t new_owner) {
    uint32_t mip;
    uint32_t layer;
    lc_image *image = tr->image;

    (void)device;
    for (layer = tr->base_layer;
         layer < tr->base_layer + tr->layer_count; layer++) {
        for (mip = tr->base_mip; mip < tr->base_mip + tr->level_count;
             mip++) {
            uint64_t idx =
                (uint64_t)layer * (uint64_t)image->mip_levels +
                (uint64_t)mip;

            image->owners[idx] = new_owner;
            image->epochs[idx] = tr->signal_value;
        }
    }
    image->xfer_value = tr->signal_value;
}

/* Mark buffer ownership after a scheduled copy (transfer lock
 * held by the caller). */
static void lc_vk_xfer_mark_buffer(lc_device *device, lc_buffer *buffer,
                                   uint64_t value) {
    /* Phase 22: ownership never moves for CONCURRENT buffers; only
     * the transfer horizon and tracked upload state publish. */
    buffer->xfer_value = value;
    lc_vk_sync_mark_buffer(buffer, LC_RESOURCE_STATE_TRANSFER_DST);
}

/* Snapshot image range states (state lock held by the caller):
 * uniform old state required (mixed ranges fail loudly). */
static int lc_vk_xfer_snap_image(lc_device *device, lc_image *image,
                                 uint32_t base_mip, uint32_t level_count,
                                 uint32_t base_layer, uint32_t layer_count,
                                 lc_resource_state *out_old) {
    uint64_t first;
    lc_resource_state old;

    (void)device;
    if (image->states == NULL || image->owners == NULL) {
        return 0;
    }
    first = (uint64_t)base_layer * (uint64_t)image->mip_levels +
            (uint64_t)base_mip;
    old = image->states[first];
    if (!lc_vk_sync_all_equal(image, base_mip, level_count, base_layer,
                              layer_count, old)) {
        return 0;
    }
    *out_old = old;
    return 1;
}

/* Read one subresource owner (transfer lock held by the caller). */
static uint32_t lc_vk_xfer_owner_at(const lc_image *image,
                                    uint32_t base_mip,
                                    uint32_t base_layer) {
    uint64_t first =
        (uint64_t)base_layer * (uint64_t)image->mip_levels +
        (uint64_t)base_mip;

    return image->owners[first];
}

/* Destroy submit-time temporaries after a failed schedule.
 * Called with no shard held; pool frees take the submit shard. */
static void lc_vk_xfer_fail_cleanup(lc_device *device,
                                    VkCommandBuffer copy_cmd,
                                    int copy_on_transfer_pool,
                                    VkCommandBuffer rel_cmd,
                                    VkSemaphore sem_rel,
                                    VkSemaphore sem_copy, VkFence fence,
                                    VkBuffer stage,
                                    lc_vk_mem_binding *stage_mem,
                                    uint64_t stage_bytes) {
    lc_device_lock_submit(device);
    if (copy_cmd != VK_NULL_HANDLE) {
        if (copy_on_transfer_pool) {
            lc_vk_transfer_cmd_free(device, copy_cmd);
        } else {
            lc_vk_imm_cmd_free(device, copy_cmd);
        }
    }
    lc_vk_imm_cmd_free(device, rel_cmd);
    lc_device_unlock_submit(device);
    if (sem_rel != VK_NULL_HANDLE &&
        device->device != VK_NULL_HANDLE) {
        vkDestroySemaphore(device->device, sem_rel, NULL);
    }
    if (sem_copy != VK_NULL_HANDLE &&
        device->device != VK_NULL_HANDLE) {
        vkDestroySemaphore(device->device, sem_copy, NULL);
    }
    if (fence != VK_NULL_HANDLE && device->device != VK_NULL_HANDLE) {
        vkDestroyFence(device->device, fence, NULL);
    }
    lc_vk_xfer_stage_release(device, stage, stage_mem);
    lc_vk_staging_unreserve(device, stage_bytes);
}

/* Async buffer upload schedule. Staging is pre-filled by the
 * caller-side wrapper; *out_value receives the completion value
 * (0 + SUCCESS for the no-op/mapped paths). */
lc_result lc_vk_transfer_upload_buffer(lc_device *device, lc_buffer *dst,
                                       uint64_t dst_offset,
                                       const void *data, uint64_t size,
                                       uint64_t *out_value) {
    VkBuffer stage = VK_NULL_HANDLE;
    lc_vk_mem_binding stage_mem;
    void *stage_ptr = NULL;
    int dedicated = 0;
    int need_release = 0;
    uint64_t value = 0;
    VkCommandBuffer copy_cmd = VK_NULL_HANDLE;
    VkCommandBuffer rel_cmd = VK_NULL_HANDLE;
    VkSemaphore sem_rel = VK_NULL_HANDLE;
    VkSemaphore sem_copy = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBufferCopy region;
    lc_transfer_entry *entry = NULL;
    lc_result res;

    if (out_value != NULL) {
        *out_value = 0;
    }
    if (device == NULL || dst == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_xfer_live_buffer(device, dst)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (size == 0) {
        return LC_SUCCESS;
    }
    if (data == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (dst_offset > dst->size || size > dst->size - dst_offset) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE ||
        dst->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    memset(&stage_mem, 0, sizeof(stage_mem));

    /* Persistently mapped fast path: memcpy + flush completes
     * synchronously (zero signal, like the sync write). */
    if (dst->mapped_ptr != NULL) {
        lc_vk_mem_binding self;

        memcpy((char *)dst->mapped_ptr + dst_offset, data,
               (size_t)size);
        memset(&self, 0, sizeof(self));
        self.memory = dst->vk_memory;
        self.offset = dst->vk_memory_offset;
        self.size = dst->allocation_size;
        self.mapped = dst->mapped_ptr;
        self.coherent = dst->memory_coherent;
        return lc_vk_mem_flush(device, &self, dst_offset, size);
    }

    res = lc_vk_xfer_stage_acquire(device, size, 1,
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   &stage, &stage_mem, &stage_ptr);
    if (res != LC_SUCCESS) {
        return res;
    }
    if (stage_ptr == NULL) {
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return LC_ERROR_UNKNOWN;
    }
    memcpy(stage_ptr, data, (size_t)size);
    res = lc_vk_mem_flush(device, &stage_mem, 0, size);
    if (res != LC_SUCCESS) {
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return res;
    }
    res = lc_vk_staging_make_room(device, size);
    if (res != LC_SUCCESS) {
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return res;
    }

    /* Phase 22: user buffers are CONCURRENT-shared when a dedicated
     * transfer family exists (vulkan_buffer.c), so queue-family
     * ownership never moves for buffers. The release submit stays
     * (it drains prior graphics writes into the copy: cross-queue
     * availability), but no acquire is ever needed — visibility
     * back to graphics rides the frame's transfer-value wait — so
     * buffer entries link already-acquired and unlink promptly at
     * completion. Images stay graphics-queue-only (no release). */
    dedicated = device->has_dedicated_transfer;
    need_release = dedicated ? 1 : 0;

    /* Overlapping same-range uploads serialize here (newest wins:
     * link order == submit order == completion order); disjoint
     * ranges proceed in parallel. No shard held. */
    res = lc_vk_xfer_wait_overlap(device, 0, NULL, dst, 0, 0, 0, 0,
                                  dst_offset, size);
    if (res != LC_SUCCESS) {
        lc_vk_staging_unreserve(device, size);
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return res;
    }

    /* Reserve + link the placeholder BEFORE submitting (Phase 22
     * publication atomicity): observers see the entry from the
     * moment its value exists. Identity is filled pre-link so
     * concurrent wait_for matches work. */
    {
        lc_transfer_entry tmpl;

        memset(&tmpl, 0, sizeof(tmpl));
        tmpl.is_image = 0;
        tmpl.buffer = dst;
        tmpl.buf_offset = dst_offset;
        tmpl.buf_size = size;
        tmpl.acquired = 1;
        value = lc_vk_xfer_reserve_linked(device, &tmpl, &entry);
        if (value == 0 || entry == NULL) {
            lc_vk_xfer_stage_release(device, stage, &stage_mem);
            lc_vk_staging_unreserve(device, size);
            return LC_ERROR_OUT_OF_MEMORY;
        }
    }

    lc_device_lock_submit(device);
    if (need_release) {
        res = lc_vk_xfer_sem_create(device, &sem_rel);
        if (res == LC_SUCCESS) {
            res = lc_vk_imm_cmd_alloc(device, &rel_cmd);
        }
        if (res == LC_SUCCESS) {
            res = lc_vk_transfer_cmd_begin(device, rel_cmd);
        }
        if (res == LC_SUCCESS) {
            /* Release graphics ownership; prior graphics use
             * drains by submit order, availability by src. */
            lc_vk_xfer_buffer_barrier(
                rel_cmd, dst, dst_offset, size,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_ACCESS_MEMORY_READ_BIT |
                    VK_ACCESS_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                device->graphics_queue_family,
                device->transfer_queue_family);
            res = (vkEndCommandBuffer(rel_cmd) == VK_SUCCESS)
                      ? LC_SUCCESS
                      : LC_ERROR_UNKNOWN;
        }
        if (res == LC_SUCCESS) {
            res = lc_vk_xfer_submit(device, device->graphics_queue,
                                    rel_cmd, 0, NULL, NULL, NULL, 1,
                                    &sem_rel, NULL, VK_NULL_HANDLE);
        }
        if (res != LC_SUCCESS) {
            lc_device_unlock_submit(device);
            lc_vk_xfer_drop_placeholder(device, entry, value);
            lc_vk_xfer_fail_cleanup(device, VK_NULL_HANDLE, 0, rel_cmd,
                                    sem_rel, VK_NULL_HANDLE,
                                    VK_NULL_HANDLE, stage, &stage_mem,
                                    size);
            return res;
        }
    }
    if (dedicated) {
        res = lc_vk_transfer_cmd_alloc(device, &copy_cmd);
    } else {
        res = lc_vk_imm_cmd_alloc(device, &copy_cmd);
    }
    if (res == LC_SUCCESS) {
        res = lc_vk_transfer_cmd_begin(device, copy_cmd);
    }
    if (res == LC_SUCCESS) {
        memset(&region, 0, sizeof(region));
        region.srcOffset = 0;
        region.dstOffset = (VkDeviceSize)dst_offset;
        region.size = (VkDeviceSize)size;
        vkCmdCopyBuffer(copy_cmd, stage, dst->vk_buffer, 1, &region);
        res = (vkEndCommandBuffer(copy_cmd) == VK_SUCCESS)
                  ? LC_SUCCESS
                  : LC_ERROR_UNKNOWN;
    }
    if (res == LC_SUCCESS) {
        res = lc_vk_xfer_sem_create(device, &sem_copy);
    }
    if (res == LC_SUCCESS && !device->timeline_ok) {
        res = lc_vk_xfer_fence_create(device, &fence);
    }
    if (res == LC_SUCCESS) {
        VkQueue copy_queue = dedicated ? device->transfer_queue
                                       : device->graphics_queue;
        if (need_release) {
            VkPipelineStageFlags stage_mask =
                VK_PIPELINE_STAGE_TRANSFER_BIT;

            res = lc_vk_xfer_submit(
                device, copy_queue, copy_cmd, 1, &sem_rel, &stage_mask,
                NULL, 2, (const VkSemaphore[]){ sem_copy,
                                                device->timeline },
                (const uint64_t[]){ 0, value }, fence);
        } else {
            res = lc_vk_xfer_submit(
                device, copy_queue, copy_cmd, 0, NULL, NULL, NULL, 2,
                (const VkSemaphore[]){ sem_copy, device->timeline },
                (const uint64_t[]){ 0, value }, fence);
        }
    }
    lc_device_unlock_submit(device);
    if (res != LC_SUCCESS) {
        lc_vk_xfer_drop_placeholder(device, entry, value);
        lc_vk_xfer_fail_cleanup(device, copy_cmd, dedicated, rel_cmd,
                                sem_rel, sem_copy, fence, stage,
                                &stage_mem, size);
        return res;
    }

    /* Publish under the transfer shard (handles + submitted flag +
     * stats atomically: concurrent observers never see a torn
     * entry). */
    lc_device_lock_transfer(device);
    entry->stage_buffer = stage;
    entry->stage_mem = stage_mem;
    entry->stage_bytes = size;
    entry->cmd = copy_cmd;
    entry->cmd_on_transfer_pool = dedicated;
    entry->cmd_rel = rel_cmd;
    entry->fallback_fence = fence;
    entry->fallback_rel = sem_rel;
    entry->fallback_copy = sem_copy;
    entry->submitted = 1;
    device->stat_transfer_submissions++;
    device->stat_bytes_uploaded += size;
    device->stat_uploads_in_flight++;
    lc_device_unlock_transfer(device);
    lc_device_lock_transfer(device);
    lc_vk_xfer_mark_buffer(device, dst, value);
    lc_device_unlock_transfer(device);
    if (out_value != NULL) {
        *out_value = value;
    }
    return LC_SUCCESS;
}

/* Async image upload schedule (one mip/layer region). Mirrors the
 * sync path's validation; the level ends sampled-readable when
 * SAMPLED exists, else TRANSFER_DST. */
lc_result lc_vk_transfer_upload_image(
    lc_device *device, lc_image *dst, uint32_t mip_level,
    uint32_t array_layer, uint32_t width, uint32_t height,
    uint32_t depth, const void *data, uint64_t data_size,
    uint64_t *out_value) {
    VkBuffer stage = VK_NULL_HANDLE;
    lc_vk_mem_binding stage_mem;
    void *stage_ptr = NULL;
    uint32_t mw = 0;
    uint32_t mh = 0;
    uint32_t md = 0;
    uint32_t elem = 0;
    uint64_t expected = 0;
    lc_resource_state old_state;
    lc_resource_state final_state;
    int dedicated = 0;
    int need_release = 0;
    int snap_ok = 0;
    uint64_t value = 0;
    VkCommandBuffer copy_cmd = VK_NULL_HANDLE;
    VkCommandBuffer rel_cmd = VK_NULL_HANDLE;
    VkSemaphore sem_rel = VK_NULL_HANDLE;
    VkSemaphore sem_copy = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBufferImageCopy region;
    lc_transfer_entry *entry = NULL;
    lc_result res;

    if (out_value != NULL) {
        *out_value = 0;
    }
    if (device == NULL || dst == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_xfer_live_image(device, dst)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (dst->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE ||
        dst->vk_image == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    if (data == NULL || data_size == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (mip_level >= dst->mip_levels ||
        array_layer >= dst->array_layers) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (dst->samples != 1) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((dst->usage & LC_IMAGE_USAGE_TRANSFER_DST) == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    lc_vk_xfer_mip_extent(dst, mip_level, &mw, &mh, &md);
    if (width == 0 || width > mw || height == 0 || height > mh ||
        depth == 0 || depth > md) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    elem = lc_format_byte_size(dst->format);
    if (elem == 0) {
        return LC_ERROR_IMAGE_CREATION_FAILED;
    }
    expected = (uint64_t)width * (uint64_t)height * (uint64_t)depth *
               (uint64_t)elem;
    if (data_size < expected) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&stage_mem, 0, sizeof(stage_mem));

    final_state = ((dst->usage & LC_IMAGE_USAGE_SAMPLED) != 0)
                      ? LC_RESOURCE_STATE_SHADER_READ
                      : LC_RESOURCE_STATE_TRANSFER_DST;

    res = lc_vk_xfer_stage_acquire(device, data_size, 1,
                                   VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                   &stage, &stage_mem, &stage_ptr);
    if (res != LC_SUCCESS) {
        return res;
    }
    if (stage_ptr == NULL) {
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return LC_ERROR_UNKNOWN;
    }
    memcpy(stage_ptr, data, (size_t)data_size);
    res = lc_vk_mem_flush(device, &stage_mem, 0, data_size);
    if (res != LC_SUCCESS) {
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return res;
    }
    res = lc_vk_staging_make_room(device, data_size);
    if (res != LC_SUCCESS) {
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return res;
    }
    /* Overlapping same-range uploads serialize here (newest wins);
     * disjoint mips/layers proceed in parallel. No shard held. */
    res = lc_vk_xfer_wait_overlap(device, 1, dst, NULL, mip_level, 1,
                                  array_layer, 1, 0, 0);
    if (res != LC_SUCCESS) {
        lc_vk_staging_unreserve(device, data_size);
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return res;
    }

    /* Image layout transitions may target shader stages, which a
     * transfer-only family cannot record. Keep image copies on the
     * graphics queue for Phase 20; buffer streaming still uses the
     * dedicated transfer queue. A later queue scheduler can split the
     * copy and graphics-finalize submissions without changing the
     * public completion-token API. */
    dedicated = 0;
    lc_device_lock_state(device);
    if (lc_vk_sync_validate_range(dst, mip_level, 1, array_layer, 1)) {
        snap_ok = lc_vk_xfer_snap_image(device, dst, mip_level, 1,
                                        array_layer, 1, &old_state);
    }
    lc_device_unlock_state(device);
    if (snap_ok && dedicated) {
        lc_device_lock_transfer(device);
        need_release =
            (lc_vk_xfer_owner_at(dst, mip_level, array_layer) ==
             device->graphics_queue_family)
                ? 1
                : 0;
        lc_device_unlock_transfer(device);
    }
    if (!snap_ok) {
        lc_vk_staging_unreserve(device, data_size);
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return LC_ERROR_INVALID_ARGUMENT;
    }
    {
        VkImageLayout layout;
        VkPipelineStageFlags stage_flags = 0;
        VkAccessFlags access = 0;

        /* UNDEFINED is a valid source state (discard) even though it
         * is intentionally not a valid destination state. */
        if (!lc_vk_sync_barrier_params(
                LC_RESOURCE_STATE_TRANSFER_DST, &layout, &stage_flags,
                &access) ||
            !lc_vk_sync_barrier_params(final_state, &layout,
                                       &stage_flags, &access)) {
            lc_vk_staging_unreserve(device, data_size);
            lc_vk_xfer_stage_release(device, stage, &stage_mem);
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }

    /* Placeholder links here (identity pre-filled); published after
     * the submits succeed, dropped (host-signaled) on failure. */
    {
        lc_transfer_entry tmpl;

        memset(&tmpl, 0, sizeof(tmpl));
        tmpl.is_image = 1;
        tmpl.image = dst;
        tmpl.base_mip = mip_level;
        tmpl.level_count = 1;
        tmpl.base_layer = array_layer;
        tmpl.layer_count = 1;
        tmpl.final_state = final_state;
        tmpl.acquired = !need_release;
        value = lc_vk_xfer_reserve_linked(device, &tmpl, &entry);
        if (value == 0 || entry == NULL) {
            lc_vk_staging_unreserve(device, data_size);
            lc_vk_xfer_stage_release(device, stage, &stage_mem);
            return LC_ERROR_OUT_OF_MEMORY;
        }
    }

    lc_device_lock_submit(device);
    if (need_release) {
        res = lc_vk_xfer_sem_create(device, &sem_rel);
        if (res == LC_SUCCESS) {
            res = lc_vk_imm_cmd_alloc(device, &rel_cmd);
        }
        if (res == LC_SUCCESS) {
            res = lc_vk_transfer_cmd_begin(device, rel_cmd);
        }
        if (res == LC_SUCCESS) {
            res = lc_vk_xfer_image_barrier(
                rel_cmd, dst, mip_level, 1, array_layer, 1, old_state,
                LC_RESOURCE_STATE_TRANSFER_DST,
                device->graphics_queue_family,
                device->transfer_queue_family);
        }
        if (res == LC_SUCCESS) {
            res = (vkEndCommandBuffer(rel_cmd) == VK_SUCCESS)
                      ? LC_SUCCESS
                      : LC_ERROR_UNKNOWN;
        }
        if (res == LC_SUCCESS) {
            res = lc_vk_xfer_submit(device, device->graphics_queue,
                                    rel_cmd, 0, NULL, NULL, NULL, 1,
                                    &sem_rel, NULL, VK_NULL_HANDLE);
        }
        if (res != LC_SUCCESS) {
            lc_device_unlock_submit(device);
            lc_vk_xfer_drop_placeholder(device, entry, value);
            lc_vk_xfer_fail_cleanup(device, VK_NULL_HANDLE, 0, rel_cmd,
                                    sem_rel, VK_NULL_HANDLE,
                                    VK_NULL_HANDLE, stage, &stage_mem,
                                    data_size);
            return res;
        }
    }
    if (dedicated) {
        res = lc_vk_transfer_cmd_alloc(device, &copy_cmd);
    } else {
        res = lc_vk_imm_cmd_alloc(device, &copy_cmd);
    }
    if (res == LC_SUCCESS) {
        res = lc_vk_transfer_cmd_begin(device, copy_cmd);
    }
    if (res == LC_SUCCESS && !need_release) {
        /* Same-family or concurrently shared: transition inline (no
         * ownership transfer is required). */
        res = lc_vk_sync_record_span(copy_cmd, dst, mip_level, 1,
                                     array_layer, 1, old_state,
                                     LC_RESOURCE_STATE_TRANSFER_DST);
    }
    if (res == LC_SUCCESS) {
        memset(&region, 0, sizeof(region));
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask =
            lc_vk_aspect_for(dst->format);
        region.imageSubresource.mipLevel = mip_level;
        region.imageSubresource.baseArrayLayer = array_layer;
        region.imageSubresource.layerCount = 1;
        region.imageExtent.width = width;
        region.imageExtent.height = height;
        region.imageExtent.depth = depth;
        vkCmdCopyBufferToImage(copy_cmd, stage, dst->vk_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        if (!need_release) {
            /* Submissions without a later graphics acquire must
             * perform this transition. End the copy in the state
             * published to the global tracker so the next submitted
             * list observes the real native layout. */
            res = lc_vk_sync_record_span(
                copy_cmd, dst, mip_level, 1, array_layer, 1,
                LC_RESOURCE_STATE_TRANSFER_DST, final_state);
        }
    }
    if (res == LC_SUCCESS) {
        res = (vkEndCommandBuffer(copy_cmd) == VK_SUCCESS)
                  ? LC_SUCCESS
                  : LC_ERROR_UNKNOWN;
    }
    if (res == LC_SUCCESS) {
        res = lc_vk_xfer_sem_create(device, &sem_copy);
    }
    if (res == LC_SUCCESS && !device->timeline_ok) {
        res = lc_vk_xfer_fence_create(device, &fence);
    }
    if (res == LC_SUCCESS) {
        VkQueue copy_queue = dedicated ? device->transfer_queue
                                       : device->graphics_queue;
        if (need_release) {
            VkPipelineStageFlags stage_mask =
                VK_PIPELINE_STAGE_TRANSFER_BIT;

            res = lc_vk_xfer_submit(
                device, copy_queue, copy_cmd, 1, &sem_rel, &stage_mask,
                NULL, 2, (const VkSemaphore[]){ sem_copy,
                                                device->timeline },
                (const uint64_t[]){ 0, value }, fence);
        } else {
            res = lc_vk_xfer_submit(
                device, copy_queue, copy_cmd, 0, NULL, NULL, NULL, 2,
                (const VkSemaphore[]){ sem_copy, device->timeline },
                (const uint64_t[]){ 0, value }, fence);
        }
    }
    lc_device_unlock_submit(device);
    if (res != LC_SUCCESS) {
        lc_vk_xfer_drop_placeholder(device, entry, value);
        lc_vk_xfer_fail_cleanup(device, copy_cmd, dedicated, rel_cmd,
                                sem_rel, sem_copy, fence, stage,
                                &stage_mem, data_size);
        return res;
    }

    /* Publish under the transfer shard (handles + submitted flag +
     * stats atomically: concurrent observers never see a torn
     * entry). */
    lc_device_lock_transfer(device);
    entry->stage_buffer = stage;
    entry->stage_mem = stage_mem;
    entry->stage_bytes = data_size;
    entry->cmd = copy_cmd;
    entry->cmd_on_transfer_pool = dedicated;
    entry->cmd_rel = rel_cmd;
    entry->fallback_fence = fence;
    entry->fallback_rel = sem_rel;
    entry->fallback_copy = sem_copy;
    entry->submitted = 1;
    device->stat_transfer_submissions++;
    device->stat_bytes_uploaded += data_size;
    device->stat_uploads_in_flight++;
    lc_device_unlock_transfer(device);
    lc_vk_xfer_mark_image_states(device, entry);
    lc_device_lock_transfer(device);
    lc_vk_xfer_mark_image_owners(
        device, entry,
        need_release ? device->transfer_queue_family
                     : device->graphics_queue_family);
    lc_device_unlock_transfer(device);
    if (out_value != NULL) {
        *out_value = value;
    }
    return LC_SUCCESS;
}

/* Async image readback schedule. Validates like the sync query,
 * copies one mip/layer into CONCURRENT staging, and returns a
 * request holding the result until destroy. The entry stays linked
 * (keep_until_destroy) so waits and pressure accounting observe
 * it; reclaim frees only its command buffer. */
lc_result lc_vk_transfer_readback_image(lc_device *device, lc_image *image,
                                        uint32_t mip_level,
                                        uint32_t array_layer,
                                        lc_readback_request **out_request) {
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t d = 0;
    uint32_t elem = 0;
    uint64_t bytes = 0;
    VkBuffer stage = VK_NULL_HANDLE;
    lc_vk_mem_binding stage_mem;
    void *stage_ptr = NULL;
    lc_resource_state old_state;
    int dedicated = 0;
    int need_release = 0;
    int snap_ok = 0;
    uint64_t value = 0;
    VkCommandBuffer copy_cmd = VK_NULL_HANDLE;
    VkCommandBuffer rel_cmd = VK_NULL_HANDLE;
    VkSemaphore sem_rel = VK_NULL_HANDLE;
    VkSemaphore sem_copy = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkBufferImageCopy region;
    lc_transfer_entry *entry = NULL;
    lc_readback_request *request = NULL;
    lc_result res;

    if (out_request != NULL) {
        *out_request = NULL;
    }
    if (device == NULL || image == NULL || out_request == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_xfer_live_image(device, image)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (mip_level >= image->mip_levels ||
        array_layer >= image->array_layers) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (image->samples != 1) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((image->usage & LC_IMAGE_USAGE_TRANSFER_SRC) == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE ||
        image->vk_image == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    elem = lc_format_byte_size(image->format);
    if (elem == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    lc_vk_xfer_mip_extent(image, mip_level, &w, &h, &d);
    bytes = (uint64_t)w * (uint64_t)elem * (uint64_t)h * (uint64_t)d;
    if (bytes == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&stage_mem, 0, sizeof(stage_mem));

    res = lc_vk_xfer_stage_acquire(device, bytes, 0,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   &stage, &stage_mem, &stage_ptr);
    if (res != LC_SUCCESS) {
        return res;
    }
    res = lc_vk_staging_make_room(device, bytes);
    if (res != LC_SUCCESS) {
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return res;
    }
    /* Overlapping same-range work serializes here (newest wins);
     * disjoint ranges proceed in parallel. No shard held. */
    res = lc_vk_xfer_wait_overlap(device, 1, image, NULL, mip_level, 1,
                                  array_layer, 1, 0, 0);
    if (res != LC_SUCCESS) {
        lc_vk_staging_unreserve(device, bytes);
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return res;
    }

    /* See the upload path: image layout restore currently records on
     * the graphics queue; the public operation remains asynchronous. */
    dedicated = 0;
    lc_device_lock_state(device);
    if (image->states != NULL &&
        lc_vk_sync_validate_range(image, mip_level, 1, array_layer,
                                  1)) {
        snap_ok = lc_vk_xfer_snap_image(device, image, mip_level, 1,
                                        array_layer, 1, &old_state);
    }
    lc_device_unlock_state(device);
    if (snap_ok && dedicated) {
        lc_device_lock_transfer(device);
        need_release =
            (lc_vk_xfer_owner_at(image, mip_level, array_layer) ==
             device->graphics_queue_family)
                ? 1
                : 0;
        lc_device_unlock_transfer(device);
    }
    if (!snap_ok) {
        lc_vk_staging_unreserve(device, bytes);
        lc_vk_xfer_stage_release(device, stage, &stage_mem);
        return LC_ERROR_INVALID_ARGUMENT;
    }
    {
        VkImageLayout layout;
        VkPipelineStageFlags stage_flags = 0;
        VkAccessFlags access = 0;

        if (!lc_vk_sync_barrier_params(
                LC_RESOURCE_STATE_TRANSFER_SRC, &layout, &stage_flags,
                &access)) {
            lc_vk_staging_unreserve(device, bytes);
            lc_vk_xfer_stage_release(device, stage, &stage_mem);
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }

    /* Placeholder links here (readbacks keep it until request
     * destroy); published after the submits, dropped on failure. */
    {
        lc_transfer_entry tmpl;

        memset(&tmpl, 0, sizeof(tmpl));
        tmpl.is_readback = 1;
        tmpl.is_image = 1;
        tmpl.image = image;
        tmpl.base_mip = mip_level;
        tmpl.level_count = 1;
        tmpl.base_layer = array_layer;
        tmpl.layer_count = 1;
        tmpl.final_state = old_state;
        tmpl.acquired = !need_release;
        tmpl.keep_until_destroy = 1;
        value = lc_vk_xfer_reserve_linked(device, &tmpl, &entry);
        if (value == 0 || entry == NULL) {
            lc_vk_staging_unreserve(device, bytes);
            lc_vk_xfer_stage_release(device, stage, &stage_mem);
            return LC_ERROR_OUT_OF_MEMORY;
        }
    }

    lc_device_lock_submit(device);
    if (need_release) {
        res = lc_vk_xfer_sem_create(device, &sem_rel);
        if (res == LC_SUCCESS) {
            res = lc_vk_imm_cmd_alloc(device, &rel_cmd);
        }
        if (res == LC_SUCCESS) {
            res = lc_vk_transfer_cmd_begin(device, rel_cmd);
        }
        if (res == LC_SUCCESS) {
            res = lc_vk_xfer_image_barrier(
                rel_cmd, image, mip_level, 1, array_layer, 1,
                old_state, LC_RESOURCE_STATE_TRANSFER_SRC,
                device->graphics_queue_family,
                device->transfer_queue_family);
        }
        if (res == LC_SUCCESS) {
            res = (vkEndCommandBuffer(rel_cmd) == VK_SUCCESS)
                      ? LC_SUCCESS
                      : LC_ERROR_UNKNOWN;
        }
        if (res == LC_SUCCESS) {
            res = lc_vk_xfer_submit(device, device->graphics_queue,
                                    rel_cmd, 0, NULL, NULL, NULL, 1,
                                    &sem_rel, NULL, VK_NULL_HANDLE);
        }
        if (res != LC_SUCCESS) {
            lc_device_unlock_submit(device);
            lc_vk_xfer_drop_placeholder(device, entry, value);
            lc_vk_xfer_fail_cleanup(device, VK_NULL_HANDLE, 0, rel_cmd,
                                    sem_rel, VK_NULL_HANDLE,
                                    VK_NULL_HANDLE, stage, &stage_mem,
                                    bytes);
            return res;
        }
    }
    if (dedicated) {
        res = lc_vk_transfer_cmd_alloc(device, &copy_cmd);
    } else {
        res = lc_vk_imm_cmd_alloc(device, &copy_cmd);
    }
    if (res == LC_SUCCESS) {
        res = lc_vk_transfer_cmd_begin(device, copy_cmd);
    }
    if (res == LC_SUCCESS && !need_release) {
        res = lc_vk_sync_record_span(copy_cmd, image, mip_level, 1,
                                     array_layer, 1, old_state,
                                     LC_RESOURCE_STATE_TRANSFER_SRC);
    }
    if (res == LC_SUCCESS) {
        memset(&region, 0, sizeof(region));
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask =
            lc_vk_aspect_for(image->format);
        region.imageSubresource.mipLevel = mip_level;
        region.imageSubresource.baseArrayLayer = array_layer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset.x = 0;
        region.imageOffset.y = 0;
        region.imageOffset.z = 0;
        region.imageExtent.width = w;
        region.imageExtent.height = h;
        region.imageExtent.depth = d;
        vkCmdCopyImageToBuffer(copy_cmd, image->vk_image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               stage, 1, &region);
        if (!need_release) {
            /* Net state unchanged when no ownership acquire follows. */
            res = lc_vk_sync_record_span(
                copy_cmd, image, mip_level, 1, array_layer, 1,
                LC_RESOURCE_STATE_TRANSFER_SRC, old_state);
        }
    }
    if (res == LC_SUCCESS) {
        res = (vkEndCommandBuffer(copy_cmd) == VK_SUCCESS)
                  ? LC_SUCCESS
                  : LC_ERROR_UNKNOWN;
    }
    if (res == LC_SUCCESS) {
        res = lc_vk_xfer_sem_create(device, &sem_copy);
    }
    if (res == LC_SUCCESS && !device->timeline_ok) {
        res = lc_vk_xfer_fence_create(device, &fence);
    }
    if (res == LC_SUCCESS) {
        VkQueue copy_queue = dedicated ? device->transfer_queue
                                       : device->graphics_queue;
        if (need_release) {
            VkPipelineStageFlags stage_mask =
                VK_PIPELINE_STAGE_TRANSFER_BIT;

            res = lc_vk_xfer_submit(
                device, copy_queue, copy_cmd, 1, &sem_rel, &stage_mask,
                NULL, 2, (const VkSemaphore[]){ sem_copy,
                                                device->timeline },
                (const uint64_t[]){ 0, value }, fence);
        } else {
            res = lc_vk_xfer_submit(
                device, copy_queue, copy_cmd, 0, NULL, NULL, NULL, 2,
                (const VkSemaphore[]){ sem_copy, device->timeline },
                (const uint64_t[]){ 0, value }, fence);
        }
    }
    lc_device_unlock_submit(device);
    if (res != LC_SUCCESS) {
        lc_vk_xfer_drop_placeholder(device, entry, value);
        lc_vk_xfer_fail_cleanup(device, copy_cmd, dedicated, rel_cmd,
                                sem_rel, sem_copy, fence, stage,
                                &stage_mem, bytes);
        return res;
    }

    /* Publish under the transfer shard (handles + submitted flag +
     * stats atomically). The request below owns the result until
     * destroy; on request-alloc OOM the linked entry still
     * completes and reclaims its command, holding staging until
     * shutdown flush (bounded, host-OOM only). */
    lc_device_lock_transfer(device);
    entry->stage_buffer = stage;
    entry->stage_mem = stage_mem;
    entry->stage_bytes = bytes;
    entry->cmd = copy_cmd;
    entry->cmd_on_transfer_pool = dedicated;
    entry->cmd_rel = rel_cmd;
    entry->fallback_fence = fence;
    entry->fallback_rel = sem_rel;
    entry->fallback_copy = sem_copy;
    entry->submitted = 1;
    device->stat_transfer_submissions++;
    device->stat_bytes_read += bytes;
    lc_device_unlock_transfer(device);
    request = (lc_readback_request *)calloc(1, sizeof(*request));
    if (request == NULL) {
        fprintf(stderr,
                "[lumac] async readback scheduled but request "
                "allocation failed; staging held until shutdown\n");
        return LC_ERROR_OUT_OF_MEMORY;
    }
    lc_vk_xfer_mark_image_states(device, entry);
    lc_device_lock_transfer(device);
    lc_vk_xfer_mark_image_owners(
        device, entry,
        need_release ? device->transfer_queue_family
                     : device->graphics_queue_family);
    lc_device_unlock_transfer(device);

    request->device = device;
    request->image = image;
    request->desc.mip_level = mip_level;
    request->desc.array_layer = array_layer;
    request->info.width = w;
    request->info.height = h;
    request->info.format = image->format;
    request->info.row_pitch = (size_t)w * (size_t)elem;
    request->info.size = (size_t)bytes;
    request->ready_value = value;
    request->entry = entry;
    lc_device_lock_transfer(device);
    request->next = device->requests;
    request->prev = NULL;
    if (device->requests != NULL) {
        device->requests->prev = request;
    }
    device->requests = request;
    lc_device_unlock_transfer(device);
    *out_request = request;
    return LC_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Acquires: frame-riding (emit) and destroy-driven (wait_for).         */
/* ------------------------------------------------------------------ */

/* Record a graphics acquire for one entry into cmd: ownership
 * restore plus the transition into the entry's final state. The
 * entry's borrowed wrapper must be live (callers claim under the
 * transfer lock, which serializes against destroy's ref-drop). */
static lc_result lc_vk_xfer_record_acquire(lc_device *device,
                                           VkCommandBuffer cmd,
                                           lc_transfer_entry *tr) {
    uint32_t gfx = device->graphics_queue_family;
    uint32_t xfer = device->transfer_queue_family;

    if (tr->is_image) {
        lc_resource_state mid = tr->is_readback
                                    ? LC_RESOURCE_STATE_TRANSFER_SRC
                                    : LC_RESOURCE_STATE_TRANSFER_DST;

        if (tr->image == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        return lc_vk_xfer_image_barrier(cmd, tr->image, tr->base_mip,
                                        tr->level_count, tr->base_layer,
                                        tr->layer_count, mid,
                                        tr->final_state, xfer, gfx);
    }
    if (tr->buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    lc_vk_xfer_buffer_barrier(cmd, tr->buffer, tr->buf_offset,
                              tr->buf_size,
                              VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_ACCESS_TRANSFER_WRITE_BIT,
                              VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                              VK_ACCESS_MEMORY_READ_BIT |
                                  VK_ACCESS_MEMORY_WRITE_BIT,
                              xfer, gfx);
    return LC_SUCCESS;
}

/* Restore graphics ownership in tracking (transfer lock held by
 * the caller — the transfer lock owns the ownership domain). */
static void lc_vk_xfer_restore_owners(lc_device *device,
                                      lc_transfer_entry *tr) {
    uint32_t mip;
    uint32_t layer;

    if (tr->is_image && tr->image != NULL) {
        for (layer = tr->base_layer;
             layer < tr->base_layer + tr->layer_count; layer++) {
            for (mip = tr->base_mip;
                 mip < tr->base_mip + tr->level_count; mip++) {
                uint64_t idx =
                    (uint64_t)layer *
                        (uint64_t)tr->image->mip_levels +
                    (uint64_t)mip;

                tr->image->owners[idx] =
                    device->graphics_queue_family;
            }
        }
    } else if (!tr->is_image && tr->buffer != NULL) {
        tr->buffer->owner_family = device->graphics_queue_family;
    }
}

/* Emit graphics acquires for un-acquired in-flight transfers into
 * an open frame command buffer (frame end, before end-cmd). Claims
 * entries under the transfer lock (serializing against destroy),
 * appends each copy semaphore (frame orders the barrier on it),
 * records the acquire barrier, then restores ownership tracking
 * in bounded chunks (no lock nesting anywhere). A full wait array
 * reports OUT_OF_MEMORY; the caller drains the oldest entry and
 * retries. Timeline mode needs no semaphore waits for ordering
 * (the frame's max-value wait covers the copies) but still takes
 * them: one uniform precise path in both modes. */
lc_result lc_vk_emit_pending_acquires(
    lc_device *device, VkCommandBuffer cmd, VkSemaphore *wait_sems,
    VkPipelineStageFlags *wait_stages, uint32_t *inout_count,
    uint32_t cap) {
    uint32_t need = 0;

    if (device == NULL || cmd == VK_NULL_HANDLE ||
        inout_count == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    lc_device_lock_transfer(device);
    /* Phase 22 preflight (PART G): count claimable entries first.
     * If the wait array cannot hold them all, fail BEFORE
     * mutating anything — no claimed flags, no appended sems, no
     * recorded barriers — so a later retry observes exactly the
     * state a clean first attempt would have seen. */
    for (lc_transfer_entry *tr = device->transfers; tr != NULL;
         tr = tr->next) {
        if (tr->acquired || !tr->submitted ||
            tr->cmd_rel == VK_NULL_HANDLE) {
            continue;
        }
        need++;
    }
    if (*inout_count + need > cap) {
        lc_device_unlock_transfer(device);
        return LC_ERROR_OUT_OF_MEMORY;
    }
    if (need > 0 && (wait_sems == NULL || wait_stages == NULL)) {
        lc_device_unlock_transfer(device);
        return LC_ERROR_INVALID_ARGUMENT;
    }
    for (lc_transfer_entry *tr = device->transfers; tr != NULL;
         tr = tr->next) {
        if (tr->acquired || !tr->submitted ||
            tr->cmd_rel == VK_NULL_HANDLE) {
            continue;
        }
        tr->acquired = 1;
        wait_sems[*inout_count] = tr->fallback_copy;
        wait_stages[*inout_count] = VK_PIPELINE_STAGE_TRANSFER_BIT;
        (*inout_count)++;
        /* The barrier references the borrowed wrapper, which
         * destroy cannot free first: destroy claims (and frees)
         * only entries it observes un-acquired under this same
         * lock, and retires the Vk objects only after GPU
         * completion (retirement horizon covers submitted
         * frames). Ownership restores here, under the same lock:
         * reclaim may unlink the entry the moment this returns,
         * so nothing may be deferred. A record failure reverts
         * just this entry (it stays unacquired for a later
         * frame); the batch still succeeds. */
        if (lc_vk_xfer_record_acquire(device, cmd, tr) != LC_SUCCESS) {
            tr->acquired = 0;
            (*inout_count)--;
            continue;
        }
        lc_vk_xfer_restore_owners(device, tr);
    }
    lc_device_unlock_transfer(device);
    return LC_SUCCESS;
}

/* Free one request and its linked entry (transfer lock held).
 * GPU-idle or completion-waited contexts only. Pool command
 * buffers are NOT freed here (submit domain); they are returned
 * for the caller to free under submit. */
static void lc_vk_request_free_locked(lc_device *device,
                                      lc_readback_request *req,
                                      VkCommandBuffer *out_cmd,
                                      VkCommandBuffer *out_rel,
                                      int *out_on_transfer_pool) {
    lc_transfer_entry *tr = req->entry;

    if (out_cmd != NULL) {
        *out_cmd = VK_NULL_HANDLE;
    }
    if (out_rel != NULL) {
        *out_rel = VK_NULL_HANDLE;
    }
    if (out_on_transfer_pool != NULL) {
        *out_on_transfer_pool = 0;
    }
    if (tr != NULL) {
        if (tr->prev != NULL) {
            tr->prev->next = tr->next;
        } else if (device->transfers == tr) {
            device->transfers = tr->next;
        }
        if (tr->next != NULL) {
            tr->next->prev = tr->prev;
        }
        if (tr->stage_buffer != VK_NULL_HANDLE &&
            device->device != VK_NULL_HANDLE) {
            vkDestroyBuffer(device->device, tr->stage_buffer, NULL);
        }
        if (tr->stage_mem.memory != VK_NULL_HANDLE) {
            lc_vk_mem_free(device, &tr->stage_mem);
        }
        if (tr->stage_bytes > 0) {
            lc_vk_staging_release_locked(device, tr->stage_bytes);
        }
        if (tr->fallback_fence != VK_NULL_HANDLE &&
            device->device != VK_NULL_HANDLE) {
            vkDestroyFence(device->device, tr->fallback_fence, NULL);
        }
        if (device->device != VK_NULL_HANDLE) {
            if (tr->fallback_rel != VK_NULL_HANDLE) {
                vkDestroySemaphore(device->device, tr->fallback_rel,
                                   NULL);
            }
            if (tr->fallback_copy != VK_NULL_HANDLE) {
                vkDestroySemaphore(device->device, tr->fallback_copy,
                                   NULL);
            }
        }
        if (out_cmd != NULL) {
            *out_cmd = tr->cmd;
        }
        if (out_rel != NULL) {
            *out_rel = tr->cmd_rel;
        }
        if (out_on_transfer_pool != NULL) {
            *out_on_transfer_pool = tr->cmd_on_transfer_pool;
        }
        tr->cmd = VK_NULL_HANDLE;
        tr->cmd_rel = VK_NULL_HANDLE;
        free(tr);
    }
    if (req->prev != NULL) {
        req->prev->next = req->next;
    } else if (device->requests == req) {
        device->requests = req->next;
    }
    if (req->next != NULL) {
        req->next->prev = req->prev;
    }
    free(req);
}

/* Free one pool command from a request teardown (submit held). */
static void lc_vk_request_cmd_free(lc_device *device, VkCommandBuffer cmd,
                                   VkCommandBuffer rel,
                                   int on_transfer_pool) {
    if (cmd != VK_NULL_HANDLE) {
        if (on_transfer_pool) {
            lc_vk_transfer_cmd_free(device, cmd);
        } else {
            lc_vk_imm_cmd_free(device, cmd);
        }
    }
    lc_vk_imm_cmd_free(device, rel);
}

/* Shutdown path: the device is idle; free every live request and
 * its entry unconditionally. Phased per request (transfer, then
 * submit for pool commands). */
void lc_vk_requests_flush_all(lc_device *device) {
    if (device == NULL) {
        return;
    }
    lc_device_lock_transfer(device);
    while (device->requests != NULL) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBuffer rel = VK_NULL_HANDLE;
        int on_pool = 0;

        lc_vk_request_free_locked(device, device->requests, &cmd,
                                  &rel, &on_pool);
        lc_device_unlock_transfer(device);
        lc_device_lock_submit(device);
        lc_vk_request_cmd_free(device, cmd, rel, on_pool);
        lc_device_unlock_submit(device);
        lc_device_lock_transfer(device);
    }
    lc_device_unlock_transfer(device);
}

/* Destroy one request: wait its own completion first (never frees
 * in-flight GPU memory), then free the entry, staging, and the
 * request itself. Safe with NULL. */
void lc_vk_request_discard(lc_device *device, lc_readback_request *req) {
    lc_readback_request *it;
    int member = 0;

    if (device == NULL || req == NULL) {
        return;
    }
    lc_vk_signal_wait(device, req->ready_value, LC_TIMEOUT_INFINITE);
    lc_device_lock_transfer(device);
    for (it = device->requests; it != NULL; it = it->next) {
        if (it == req) {
            member = 1;
            break;
        }
    }
    if (member) {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkCommandBuffer rel = VK_NULL_HANDLE;
        int on_pool = 0;

        lc_vk_request_free_locked(device, req, &cmd, &rel, &on_pool);
        lc_device_unlock_transfer(device);
        lc_device_lock_submit(device);
        lc_vk_request_cmd_free(device, cmd, rel, on_pool);
        lc_device_unlock_submit(device);
    } else {
        lc_device_unlock_transfer(device);
    }
    lc_vk_reclaim_completed(device);
}
