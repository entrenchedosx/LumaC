#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/* Phase 22 lock-order enforcement (PARTs A-B).
 *
 * DOCUMENTED ORDER (only nesting direction that may ever exist):
 *
 *     submit  ->  transfer
 *
 * Taking transfer while holding submit is legal (frame submission
 * holds submit across emit->submit). Taking submit while holding
 * transfer is FORBIDDEN (it would invert against the frame path
 * and could deadlock). The allocator shard (mem_mutex, taken only
 * inside vulkan_memory.c, which takes no other shard) is a leaf
 * by inspection and is not tracked here. Every other shard
 * (state, cache, desc) is a leaf: taking one while holding any
 * *different* shard is a contract violation. Same-shard
 * recursion is always legal (recursive mutexes).
 *
 * Rationale: a completion semaphore collected by emit must stay
 * alive until the frame's vkQueueSubmit consumes it. The frame
 * holds `submit` across the whole emit->submit window, so reclaim
 * — which destroys completion objects under `submit` taken only
 * after releasing `transfer` — can never interleave a destroy
 * inside that window, and no inversion cycle can form.
 * Command pools live in the submit domain (alloc + free + reset);
 * worker pools live in the transfer domain.
 *
 * Enforcement is a per-thread depth table (TLS): lock functions
 * assert the rule on every transition. assert() compiles out in
 * NDEBUG/release builds; violations abort debug builds loudly
 * instead of deadlocking or racing silently. Best-effort guards
 * (device NULL or shard uninitialized) run unlocked and skip
 * tracking, matching the single-threaded contract. */
enum {
    LC_SHARD_TRANSFER = 0,
    LC_SHARD_SUBMIT = 1,
    LC_SHARD_STATE = 2,
    LC_SHARD_CACHE = 3,
    LC_SHARD_DESC = 4,
    LC_SHARD_COUNT = 5
};

#if defined(_WIN32) || defined(_WIN64)
#define LC_TLS_STORAGE __declspec(thread)
#else
#define LC_TLS_STORAGE __thread
#endif

static LC_TLS_STORAGE int lc_shard_depth[LC_SHARD_COUNT];

static int lc_shards_held_except(int except) {
    int i;

    for (i = 0; i < LC_SHARD_COUNT; i++) {
        if (i != except && lc_shard_depth[i] > 0) {
            return 1;
        }
    }
    return 0;
}

/* Allowed nesting: taking transfer while holding submit (frame
 * submission holds submit across emit->submit). Every other
 * cross-shard nesting is a violation — in particular, taking
 * submit while holding transfer would invert against the frame
 * path and could deadlock. */
static void lc_shard_check_take(int shard) {
    if (shard == LC_SHARD_TRANSFER) {
        int i;
        int ok = 1;

        for (i = 0; i < LC_SHARD_COUNT; i++) {
            if (lc_shard_depth[i] <= 0 || i == shard) {
                continue;
            }
            if (i == LC_SHARD_SUBMIT) {
                continue;
            }
            ok = 0;
            break;
        }
        assert(ok && "lock order violation: illegal shard nesting");
        (void)ok;
    } else {
        assert(!lc_shards_held_except(shard) &&
               "lock order violation: leaf shard nested under another");
    }
}

static void lc_shard_note_take(int shard) {
    lc_shard_depth[shard]++;
}

static void lc_shard_note_drop(int shard) {
    if (lc_shard_depth[shard] > 0) {
        lc_shard_depth[shard]--;
    }
}

static void lc_device_list_add(lc_device *device) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || device == NULL) {
        return;
    }
    device->next = state->devices;
    device->prev = NULL;
    if (state->devices != NULL) {
        state->devices->prev = device;
    }
    state->devices = device;
}

static void lc_device_list_remove(lc_device *device) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || device == NULL) {
        return;
    }
    if (device->prev != NULL) {
        device->prev->next = device->next;
    } else if (state->devices == device) {
        state->devices = device->next;
    }
    if (device->next != NULL) {
        device->next->prev = device->prev;
    }
    device->next = NULL;
    device->prev = NULL;
}

lc_result lc_device_create(const lc_device_desc *desc, lc_device **out_device) {
    lc_state *state = lc_get_internal_state();
    lc_device *device;
    lc_result res;

    if (desc == NULL || out_device == NULL) {
        if (out_device != NULL) {
            *out_device = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_device = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (desc->backend != LC_BACKEND_VULKAN) {
        *out_device = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }

    device = (lc_device *)calloc(1, sizeof(lc_device));
    if (device == NULL) {
        *out_device = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    device->backend = LC_BACKEND_VULKAN;
    device->resource_id = lc_issue_resource_id();
    if (desc->pipeline_cache_path != NULL) {
        size_t n = strlen(desc->pipeline_cache_path);

        if (n >= sizeof(device->pipeline_cache_path)) {
            n = sizeof(device->pipeline_cache_path) - 1;
        }
        memcpy(device->pipeline_cache_path, desc->pipeline_cache_path, n);
        device->pipeline_cache_path[n] = '\0';
    }

    res = lc_vulkan_device_create(device, desc);
    if (res != LC_SUCCESS) {
        *out_device = NULL;
        free(device);
        return res;
    }

    lc_device_list_add(device);
    *out_device = device;
    return LC_SUCCESS;
}

void lc_device_destroy(lc_device *device) {    if (device == NULL) {
        return;
    }
    /* Dependents first: pipelines (graphics + compute), binding
     * sets and layouts, shaders, samplers, render targets (borrow
     * views), images (with views), buffers, swapchains, then
     * surfaces. Descriptor pools and the render-pass cache die
     * inside lc_vulkan_device_destroy, after every set/target/
     * framebuffer is freed. vkDestroySwapchainKHR and
     * vkDestroySurfaceKHR both need the logical device / instance,
     * which die with the device below. */
    lc_pipeline_destroy_for_device(device);
    lc_compute_pipeline_destroy_for_device(device);
    lc_binding_set_destroy_for_device(device);
    lc_binding_layout_destroy_for_device(device);
    lc_shader_destroy_for_device(device);
    lc_sampler_destroy_for_device(device);
    lc_render_target_destroy_for_device(device);
    lc_image_view_destroy_for_device(device);
    lc_image_destroy_for_device(device);
    lc_buffer_destroy_for_device(device);
    lc_swapchain_destroy_for_device(device);
    lc_surface_destroy_for_device(device);
    lc_device_list_remove(device);
    lc_vulkan_device_destroy(device);
    free(device);
}

void lc_device_wait_idle(lc_device *device) {
    lc_state *state = lc_get_internal_state();
    const lc_device *it;
    int live = 0;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->devices; it != NULL; it = it->next) {
        if (it == device) {
            live = 1;
            break;
        }
    }
    if (!live) {
        return;
    }
    if (device->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->device);
        lc_vk_reclaim_completed(device);
    }
}

void lc_device_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->devices != NULL) {
        lc_device_destroy(state->devices);
    }
}

uint32_t lc_device_get_pass_cache_count(const lc_device *device) {
    if (device == NULL) {
        return 0;
    }
    return device->pass_cache_count;
}

const char *lc_device_get_name(const lc_device *device) {
    if (device == NULL) {
        return NULL;
    }
    return device->name;
}

lc_backend lc_device_get_backend(const lc_device *device) {
    if (device == NULL) {
        return (lc_backend)0;
    }
    return device->backend;
}

uint32_t lc_device_get_vendor_id(const lc_device *device) {
    if (device == NULL) {
        return 0;
    }
    return device->vendor_id;
}

uint32_t lc_device_get_device_id(const lc_device *device) {
    if (device == NULL) {
        return 0;
    }
    return device->device_id;
}

void lc_device_get_pipeline_cache_info(
    const lc_device *device,
    lc_pipeline_cache_info *out_info) {    if (out_info == NULL) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (device == NULL) {
        return;
    }
    out_info->enabled = device->pipeline_cache_enabled;
    out_info->loaded_from_file = device->pipeline_cache_loaded;
    out_info->saved_to_file = device->pipeline_cache_saved;
    out_info->bytes_loaded = device->pipeline_cache_bytes_loaded;
    out_info->bytes_saved = device->pipeline_cache_bytes_saved;
}

lc_resource_id lc_issue_resource_id(void) {
    lc_state *state = lc_get_internal_state();
    lc_resource_id id;

    if (state == NULL) {
        return 0;
    }
#if defined(_WIN32) || defined(_WIN64)
    id = (lc_resource_id)(InterlockedIncrement64(
                              (volatile LONG64 *)&state->next_resource_id) -
                          1);
#else
    id = (lc_resource_id)__atomic_fetch_add(&state->next_resource_id, 1,
                                            __ATOMIC_RELAXED);
#endif
    /* Zero is reserved for NULL. A 64-bit wrap is theoretical, but keep the
     * invariant without introducing a racy load/store pair. */
    while (id == 0) {
#if defined(_WIN32) || defined(_WIN64)
        id = (lc_resource_id)(InterlockedIncrement64(
                                  (volatile LONG64 *)&state->next_resource_id) -
                              1);
#else
        id = (lc_resource_id)__atomic_fetch_add(&state->next_resource_id, 1,
                                                __ATOMIC_RELAXED);
#endif
    }
    return id;
}

void lc_device_get_limits(const lc_device *device,
                          lc_device_limits *out_limits) {
    VkPhysicalDeviceProperties props;

    if (out_limits == NULL) {
        return;
    }
    memset(out_limits, 0, sizeof(*out_limits));
    if (device == NULL || device->physical_device == VK_NULL_HANDLE) {
        return;
    }
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(device->physical_device, &props);
    out_limits->max_texture_2d_dimension = props.limits.maxImageDimension2D;
    out_limits->max_vertex_attributes =
        props.limits.maxVertexInputAttributes;
    out_limits->max_vertex_bindings = props.limits.maxVertexInputBindings;
    out_limits->max_uniform_buffer_size =
        (uint64_t)props.limits.maxUniformBufferRange;
    out_limits->max_image_array_layers = props.limits.maxImageArrayLayers;
    out_limits->max_sampler_anisotropy =
        (device->anisotropy_supported != 0) ? props.limits.maxSamplerAnisotropy
                                            : 1.0f;
    out_limits->min_uniform_buffer_offset_alignment =
        (uint32_t)props.limits.minUniformBufferOffsetAlignment;
    out_limits->min_storage_buffer_offset_alignment =
        (uint32_t)props.limits.minStorageBufferOffsetAlignment;
    out_limits->max_bound_resource_slots = props.limits.maxBoundDescriptorSets;
    /* Never promise more than the public structural maximum. */
    out_limits->max_color_attachments = props.limits.maxColorAttachments;
    if (out_limits->max_color_attachments > LC_MAX_COLOR_ATTACHMENTS) {
        out_limits->max_color_attachments = LC_MAX_COLOR_ATTACHMENTS;
    }
}

void lc_device_get_memory_stats(const lc_device *device,
                                lc_memory_stats *out_stats) {
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (device == NULL) {
        return;
    }
    lc_vk_mem_stats(device, out_stats);
}

void lc_device_get_memory_budget(const lc_device *device,
                                 lc_memory_budget *out_budget) {
    if (out_budget == NULL) {
        return;
    }
    memset(out_budget, 0, sizeof(*out_budget));
    if (device == NULL ||
        device->physical_device == VK_NULL_HANDLE ||
        device->instance == VK_NULL_HANDLE ||
        !device->mem_budget_supported) {
        return;
    }
    {
        /* Resolved per call: static-linking 1.1+ entry points would
         * break load on 1.0 loaders. */
        PFN_vkGetPhysicalDeviceMemoryProperties2 pfn =
            (PFN_vkGetPhysicalDeviceMemoryProperties2)
                vkGetInstanceProcAddr(
                    device->instance,
                    "vkGetPhysicalDeviceMemoryProperties2");
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget;
        VkPhysicalDeviceMemoryProperties2 props2;
        uint32_t i;

        if (pfn == NULL) {
            return;
        }
        memset(&budget, 0, sizeof(budget));
        budget.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
        memset(&props2, 0, sizeof(props2));
        props2.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
        props2.pNext = &budget;
        pfn(device->physical_device, &props2);
        for (i = 0; i < props2.memoryProperties.memoryHeapCount && i < LC_MAX_MEMORY_HEAPS; i++) {
            out_budget->heaps[i].budget = budget.heapBudget[i];
            out_budget->heaps[i].usage = budget.heapUsage[i];
            out_budget->heap_count++;
        }
        out_budget->available = (out_budget->heap_count > 0) ? 1 : 0;
    }
}

/* Phase 20 lock shards (PARTs AL/22/24): one tiny mutex per
 * subsystem, never nested, never held across recording/draws/
 * submits. Best effort: without init (allocation failure at
 * creation) the guarded section runs unlocked under the
 * single-threaded contract. */
#if defined(_WIN32) || defined(_WIN64)
#define LC_LOCK_OP_LOCK(m) EnterCriticalSection(m)
#define LC_LOCK_OP_UNLOCK(m) LeaveCriticalSection(m)
#else
#define LC_LOCK_OP_LOCK(m) pthread_mutex_lock(m)
#define LC_LOCK_OP_UNLOCK(m) pthread_mutex_unlock(m)
#endif

#define LC_LOCK_BODY(mutex_field, init_field, op)                  \
    do {                                                           \
        if ((device) != NULL && (device)->init_field) {            \
            op(&(device)->mutex_field);                            \
        }                                                          \
    } while (0)

void lc_device_lock_transfer(lc_device *device) {
    if (device != NULL && device->transfer_mutex_init) {
        lc_shard_check_take(LC_SHARD_TRANSFER);
    }
    LC_LOCK_BODY(transfer_mutex, transfer_mutex_init, LC_LOCK_OP_LOCK);
    if (device != NULL && device->transfer_mutex_init) {
        lc_shard_note_take(LC_SHARD_TRANSFER);
    }
}

void lc_device_unlock_transfer(lc_device *device) {
    int tracked =
        (device != NULL && device->transfer_mutex_init) ? 1 : 0;

    if (tracked) {
        lc_shard_note_drop(LC_SHARD_TRANSFER);
    }
    LC_LOCK_BODY(transfer_mutex, transfer_mutex_init, LC_LOCK_OP_UNLOCK);
}

void lc_device_lock_state(lc_device *device) {
    if (device != NULL && device->state_mutex_init) {
        lc_shard_check_take(LC_SHARD_STATE);
    }
    LC_LOCK_BODY(state_mutex, state_mutex_init, LC_LOCK_OP_LOCK);
    if (device != NULL && device->state_mutex_init) {
        lc_shard_note_take(LC_SHARD_STATE);
    }
}

void lc_device_unlock_state(lc_device *device) {
    int tracked =
        (device != NULL && device->state_mutex_init) ? 1 : 0;

    if (tracked) {
        lc_shard_note_drop(LC_SHARD_STATE);
    }
    LC_LOCK_BODY(state_mutex, state_mutex_init, LC_LOCK_OP_UNLOCK);
}

void lc_device_lock_cache(lc_device *device) {
    if (device != NULL && device->cache_mutex_init) {
        lc_shard_check_take(LC_SHARD_CACHE);
    }
    LC_LOCK_BODY(cache_mutex, cache_mutex_init, LC_LOCK_OP_LOCK);
    if (device != NULL && device->cache_mutex_init) {
        lc_shard_note_take(LC_SHARD_CACHE);
    }
}

void lc_device_unlock_cache(lc_device *device) {
    int tracked =
        (device != NULL && device->cache_mutex_init) ? 1 : 0;

    if (tracked) {
        lc_shard_note_drop(LC_SHARD_CACHE);
    }
    LC_LOCK_BODY(cache_mutex, cache_mutex_init, LC_LOCK_OP_UNLOCK);
}

void lc_device_lock_desc(lc_device *device) {
    if (device != NULL && device->desc_mutex_init) {
        lc_shard_check_take(LC_SHARD_DESC);
    }
    LC_LOCK_BODY(desc_mutex, desc_mutex_init, LC_LOCK_OP_LOCK);
    if (device != NULL && device->desc_mutex_init) {
        lc_shard_note_take(LC_SHARD_DESC);
    }
}

void lc_device_unlock_desc(lc_device *device) {
    int tracked =
        (device != NULL && device->desc_mutex_init) ? 1 : 0;

    if (tracked) {
        lc_shard_note_drop(LC_SHARD_DESC);
    }
    LC_LOCK_BODY(desc_mutex, desc_mutex_init, LC_LOCK_OP_UNLOCK);
}

void lc_device_lock_submit(lc_device *device) {
    if (device != NULL && device->submit_mutex_init) {
        lc_shard_check_take(LC_SHARD_SUBMIT);
    }
    LC_LOCK_BODY(submit_mutex, submit_mutex_init, LC_LOCK_OP_LOCK);
    if (device != NULL && device->submit_mutex_init) {
        lc_shard_note_take(LC_SHARD_SUBMIT);
    }
}

void lc_device_unlock_submit(lc_device *device) {
    int tracked =
        (device != NULL && device->submit_mutex_init) ? 1 : 0;

    if (tracked) {
        lc_shard_note_drop(LC_SHARD_SUBMIT);
    }
    LC_LOCK_BODY(submit_mutex, submit_mutex_init, LC_LOCK_OP_UNLOCK);
}

/* Nonzero when the calling thread holds the submit shard (pool
 * domain assertions in backend files). Best-effort guards that
 * run unlocked report 0. */
int lc_device_submit_held(void) {
    return (lc_shard_depth[LC_SHARD_SUBMIT] > 0) ? 1 : 0;
}

/* ------------------------------------------------------------------ */
/* Phase 20 public queue/transfer API.                                 */
/* ------------------------------------------------------------------ */

static int lc_gfx_live_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    const lc_device *it;

    if (state == NULL || device == NULL) {
        return 0;
    }
    for (it = state->devices; it != NULL; it = it->next) {
        if (it == device) {
            return 1;
        }
    }
    return 0;
}

static int lc_gfx_ready(void) {
    lc_state *state = lc_get_internal_state();

    return (state != NULL && state->initialized) ? 1 : 0;
}

void lc_device_get_queue_info(const lc_device *device, lc_queue_type type,
                              lc_queue_info *out_info) {
    if (out_info == NULL) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (!lc_gfx_live_device(device)) {
        return;
    }
    switch (type) {
    case LC_QUEUE_GRAPHICS:
        out_info->available = 1;
        out_info->dedicated = 1;
        out_info->count = 1;
        out_info->async_supported = 0;
        break;
    case LC_QUEUE_TRANSFER:
        out_info->available = 1;
        out_info->dedicated = device->has_dedicated_transfer;
        out_info->count = 1;
        out_info->async_supported = device->has_dedicated_transfer;
        break;
    case LC_QUEUE_COMPUTE:
        out_info->available = device->compute_supported;
        out_info->dedicated = device->has_dedicated_compute;
        out_info->count = device->compute_supported ? 1 : 0;
        /* Overlap policy is a later phase (PART AA27): production
         * dispatch records into frame/worker buffers today, so no
         * overlap is promised even when dedicated. */
        out_info->async_supported = 0;
        break;
    default:
        break;
    }
}

void lc_device_get_compute_capabilities(const lc_device *device,
                                        lc_compute_capabilities *out_caps) {
    uint32_t i;

    if (out_caps == NULL) {
        return;
    }
    memset(out_caps, 0, sizeof(*out_caps));
    if (!lc_gfx_live_device(device)) {
        return;
    }
    out_caps->compute_supported = device->compute_supported;
    out_caps->indirect_draw_supported = device->indirect_draw_supported;
    out_caps->multi_draw_indirect = device->multi_draw_indirect;
    out_caps->indirect_count = device->indirect_count_supported;
    out_caps->dedicated_compute = device->has_dedicated_compute;
    for (i = 0; i < 3; i++) {
        out_caps->max_workgroup_size[i] = device->max_workgroup_size[i];
        out_caps->max_workgroup_count[i] = device->max_workgroup_count[i];
    }
    out_caps->max_workgroup_invocations =
        device->max_workgroup_invocations;
    out_caps->max_shared_memory = device->max_compute_shared_memory;
    out_caps->max_push_size = device->max_compute_push_size;
    out_caps->subgroup_size = device->subgroup_size;
}

int lc_gpu_signal_is_ready(lc_device *device, lc_gpu_signal signal) {
    if (device == NULL || !lc_gfx_live_device(device)) {
        return 0;
    }
    return lc_vk_signal_ready(device, signal.value);
}

lc_result lc_gpu_signal_wait(lc_device *device, lc_gpu_signal signal,
                             uint64_t timeout_ns) {
    lc_result res;

    if (device == NULL || !lc_gfx_live_device(device)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vk_signal_wait(device, signal.value, timeout_ns);
    if (res == LC_SUCCESS) {
        lc_vk_reclaim_completed(device);
    }
    return res;
}

lc_result lc_upload_buffer_async(lc_device *device, lc_buffer *dst,
                                 uint64_t dst_offset, const void *data,
                                 uint64_t size,
                                 lc_gpu_signal *out_signal) {
    uint64_t value = 0;
    lc_result res;

    if (out_signal != NULL) {
        out_signal->value = 0;
    }
    if (device == NULL || dst == NULL || out_signal == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_gfx_ready()) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_gfx_live_device(device)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vk_transfer_upload_buffer(device, dst, dst_offset, data,
                                       size, &value);
    if (res == LC_SUCCESS) {
        out_signal->value = value;
        lc_vk_reclaim_completed(device);
    }
    return res;
}

lc_result lc_upload_image_async(
    lc_device *device, lc_image *dst,
    const lc_image_upload_async_desc *upload, lc_gpu_signal *out_signal) {
    uint64_t value = 0;
    lc_result res;

    if (out_signal != NULL) {
        out_signal->value = 0;
    }
    if (device == NULL || dst == NULL || upload == NULL ||
        out_signal == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_gfx_ready()) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    if (!lc_gfx_live_device(device)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vk_transfer_upload_image(
        device, dst, upload->mip_level, upload->array_layer,
        upload->width, upload->height, upload->depth, upload->data,
        upload->data_size, &value);
    if (res == LC_SUCCESS) {
        out_signal->value = value;
        lc_vk_reclaim_completed(device);
    }
    return res;
}

void lc_device_get_transfer_stats(const lc_device *device,
                                  lc_transfer_stats *out_stats) {
    const lc_command_encoder *enc = NULL;
    lc_memory_stats mem;

    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    memset(&mem, 0, sizeof(mem));
    if (!lc_gfx_live_device(device)) {
        return;
    }
    lc_device_lock_transfer((lc_device *)device);
    out_stats->graphics_submissions = device->stat_graphics_submissions;
    out_stats->transfer_submissions = device->stat_transfer_submissions;
    out_stats->bytes_uploaded = device->stat_bytes_uploaded;
    out_stats->bytes_read_back = device->stat_bytes_read;
    out_stats->staging_used = device->staging_used;
    out_stats->staging_high_water = device->staging_high_water;
    out_stats->uploads_in_flight = device->stat_uploads_in_flight;
    for (enc = device->worker_encoders; enc != NULL;
         enc = enc->worker_next) {
        out_stats->command_lists_in_flight += enc->worker_live_lists;
    }
    out_stats->retired_pending = device->retire_pending_count;
    out_stats->retired_high_water = device->retire_high_water;
    {
        const lc_transfer_entry *tr;

        for (tr = device->transfers; tr != NULL; tr = tr->next) {
            out_stats->pending_transfers++;
        }
    }
    out_stats->staging_waits = device->stat_staging_waits;
    out_stats->reclaimed_transfers = device->stat_reclaimed_transfers;
    out_stats->retired_completed = device->stat_retired_completed;
    out_stats->emit_retries = device->stat_emit_retries;
    out_stats->timeline_active = device->timeline_ok ? 1 : 0;
    out_stats->binary_fallback_active = device->timeline_ok ? 0 : 1;
    lc_device_unlock_transfer((lc_device *)device);
    lc_vk_mem_stats(device, &mem);
    out_stats->staging_committed = mem.host_visible_allocated;
}

void lc_device_poll_completed(lc_device *device) {
    if (device == NULL || !lc_gfx_live_device(device)) {
        return;
    }
    lc_vk_reclaim_completed(device);
}
