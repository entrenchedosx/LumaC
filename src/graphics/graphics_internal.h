#ifndef LUMAC_GRAPHICS_INTERNAL_H
#define LUMAC_GRAPHICS_INTERNAL_H

/*
 * Internal graphics interface (Phase 5: device + surface + swapchain).
 *
 * Platform-neutral logic (argument validation, init checks, allocation,
 * tracking lists, getters) lives in src/graphics/graphics.c,
 * surface.c, swapchain.c, frame.c, shader.c, and pipeline.c. Only
 * Vulkan work lives in src/graphics/vulkan/ (backend, surface,
 * swapchain, frame, shader, pipeline).
 *
 * Vulkan and native-platform types appear here and in the backend only -
 * never in the public header. Vulkan include directories and platform
 * surface macros (VK_USE_PLATFORM_*) stay PRIVATE in CMake, so consumers
 * of lumac.h never need the Vulkan SDK.
 *
 * Include order matters: the platform Vulkan surface structs reference
 * HWND/HINSTANCE (Win32) or Display/Window (Xlib), so the matching
 * native headers must precede <vulkan/vulkan.h> in every translation
 * unit. They are pulled in here so no backend file can get it wrong.
 */

#include "internal/lumac_internal.h"

#if defined(_WIN32) || defined(_WIN64)
#include "platform/platform.h" /* windows.h before vulkan.h */
#else
#include <X11/Xlib.h> /* Display/Window before vulkan.h */
#include <pthread.h>  /* allocator mutex (PART AL) */
#endif

#include <vulkan/vulkan.h>

/* GPUs without any rating info still get a name buffer this large.
 * VK_MAX_PHYSICAL_DEVICE_NAME_SIZE is 256. */
#define LC_DEVICE_NAME_SIZE 256

/* Allocator memory classes (PARTs L/T: buffers and images never
 * share blocks). */
typedef enum lc_vk_mem_class {
    LC_VK_MEM_DEVICE_IMAGES = 0,
    LC_VK_MEM_DEVICE_BUFFERS = 1,
    LC_VK_MEM_UPLOAD = 2,
    LC_VK_MEM_READBACK = 3,
    LC_VK_MEM_CLASS_COUNT = 4
} lc_vk_mem_class;

struct lc_vk_mem_block;

/* Allocator pool header (blocks are private to vulkan_memory.c). */
typedef struct lc_vk_mem_pool {
    struct lc_vk_mem_block *blocks;
} lc_vk_mem_pool;
typedef struct lc_vk_mem_binding {
    VkDeviceMemory memory;
    uint64_t offset;
    uint64_t size;
    void *mapped;      /* block_base + offset (host classes) */
    int coherent;
    int dedicated;
    struct lc_vk_mem_block *block; /* NULL when dedicated */
    uint32_t memory_type;
    lc_vk_mem_class memory_class;
} lc_vk_mem_binding;

/* One retrieved queue: families are enumerated once at device creation
 * (see Approach C discussion in vulkan_backend.c) so any family,
 * including a present-only family discovered later, has a queue ready
 * without recreating the logical device. */
typedef struct lc_vk_queue {
    uint32_t family_index;
    VkQueue queue;
} lc_vk_queue;

/* Render-pass cache key (Phase 12): structural signature plus
 * load/store policy and presentation final. Initial layouts derive
 * deterministically: UNDEFINED unless a LOAD needs the previous
 * contents (offscreen color SHADER_READ, depth ATTACHMENT). Phase 16:
 * `depth_sampled` records whether the depth image carries SAMPLED
 * usage — such passes finalize depth to SHADER_READ_ONLY_OPTIMAL
 * (mirroring color STORE) so depth can be sampled after the pass;
 * plain depth keeps ATTACHMENT_OPTIMAL. */
typedef struct lc_vk_pass_key {
    uint32_t color_count;
    VkFormat color_formats[LC_MAX_COLOR_ATTACHMENTS];
    VkFormat depth_format; /* VK_FORMAT_UNDEFINED when depthless */
    VkSampleCountFlagBits samples;
    VkAttachmentLoadOp color_loads[LC_MAX_COLOR_ATTACHMENTS];
    VkAttachmentStoreOp color_stores[LC_MAX_COLOR_ATTACHMENTS];
    VkAttachmentLoadOp depth_load;
    VkAttachmentStoreOp depth_store;
    int present; /* 1: color[0] ends PRESENT_SRC (swapchain) */
    int depth_sampled; /* 1: depth ends SHADER_READ (sampled usage) */
} lc_vk_pass_key;

typedef struct lc_vk_cached_pass {
    lc_vk_pass_key key;
    VkRenderPass pass;
} lc_vk_cached_pass;

/* Opaque public type, completed here. Only the Vulkan backend exists, so
 * its handles live directly in this struct; a future D3D12 backend would
 * refactor these into a backend union. */
struct lc_device {
    lc_backend backend;
    char name[LC_DEVICE_NAME_SIZE]; /* NUL-terminated GPU name */
    uint32_t vendor_id;
    uint32_t device_id;
    lc_device *next;
    lc_device *prev;
    lc_resource_id resource_id; /* stable, never reused */

    /* Vulkan backend state (VK_NULL_HANDLE / 0 / NULL when not created) */
    VkInstance instance;
    VkDebugUtilsMessengerEXT debug_messenger;
    PFN_vkDestroyDebugUtilsMessengerEXT pfn_destroy_messenger;
    VkPhysicalDevice physical_device;
    uint32_t graphics_queue_family;
    VkDevice device;
    VkQueue graphics_queue;
    lc_vk_queue *queues; /* one entry per queue family, malloc'd */
    uint32_t queue_count;

    /* Device-level immediate-submit upload context (lazy). One command
     * pool/buffer on the graphics family plus a fence; reused by every
     * staging copy. Transfers run on the graphics queue: universally
     * supported and simple; a dedicated transfer queue is future work. */
    VkCommandPool upload_pool;
    VkCommandBuffer upload_cmd;
    VkFence upload_fence;

    /* Capability state negotiated at creation: the beginning of a
     * supported-vs-enabled model. Anisotropy is enabled when the
     * physical device offers it; samplers clamp to the device maximum
     * (see lc_device_get_limits). Nothing else is enabled: features
     * stay minimal and deliberate. */
    int anisotropy_supported;

    /* Device-level descriptor allocator (lazy): growable Vulkan pools
     * backing binding-set allocation. Completely private; pools die
     * with the device. Sets are individually freeable, so pools carry
     * FREE_DESCRIPTOR_SET_BIT. */
    VkDescriptorPool *desc_pools;
    uint32_t desc_pool_count;
    uint32_t desc_pool_capacity;

    /* Render-pass cache (Phase 12, lazy): one VkRenderPass per
     * structural+policy key shared by pipelines (creation) and passes
     * (offscreen + swapchain generic). Never evicted; destroyed with
     * the device after all targets/framebuffers are gone. */
    lc_vk_cached_pass *pass_cache;
    uint32_t pass_cache_count;
    uint32_t pass_cache_capacity;

    /* Persistent pipeline cache (Phase 18, lazy): one VkPipelineCache
     * shared by every graphics pipeline on this device. Created empty
     * (or seeded from pipeline_cache_path when compatible); saved
     * atomically at destroy when a path was configured. Never fails
     * device creation: corrupt blobs fall back to an empty cache. */
    VkPipelineCache pipeline_cache;
    int pipeline_cache_enabled;
    int pipeline_cache_loaded;
    int pipeline_cache_saved;
    uint64_t pipeline_cache_bytes_loaded;
    uint64_t pipeline_cache_bytes_saved;
    char pipeline_cache_path[512]; /* copied desc path, maybe empty */

    /* GPU memory allocator (Phase 19): pools per class plus an
     * internal lock (PART AL). All other LumaC use stays
     * single-threaded by contract. */
    lc_vk_mem_pool mem_pools[LC_VK_MEM_CLASS_COUNT];
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION mem_mutex; /* via platform.h (windows.h) */
#else
    pthread_mutex_t mem_mutex;
#endif
    int mem_mutex_init;
    int mem_limits_ready;
    uint64_t mem_atom_size;    /* nonCoherentAtomSize (>= 1) */
    uint64_t mem_granularity;  /* bufferImageGranularity (>= 1) */
    uint64_t mem_max_image_dim; /* maxImageDimension2D */
    uint64_t mem_max_alloc;     /* largest heap (early-OOM guard) */
    uint64_t mem_dedicated_count;
    /* Memory-budget extension (VK_EXT_memory_budget): resolved flag
     * only; the entry point is fetched per query (old loaders). */
    int mem_budget_supported;

    /* Queues (Phase 20): graphics plus an optional dedicated
     * transfer queue (PARTs M–P). */
    uint32_t transfer_queue_family;
    VkQueue transfer_queue;
    int has_dedicated_transfer;
    /* Compute queue (Phase 21): a dedicated compute family when the
     * device offers a useful one, else the graphics family (alias).
     * Production dispatch records into frame/worker command buffers;
     * the separate handle exists for isolated cross-queue tests and
     * future async compute (no scheduler yet). */
    uint32_t compute_queue_family;
    VkQueue compute_queue;
    int has_dedicated_compute;
    /* Isolated compute-submit pool (Phase 21, PART Z): transient
     * compute-family pool + a bounded ring of parked once-submits
     * (buffer+fence each, reclaimed non-blockingly, drained at
     * shutdown). Submit-shard guarded. */
    VkCommandPool compute_pool;
    VkCommandBuffer compute_once_cmds[8];
    VkFence compute_once_fences[8];
    uint32_t compute_once_count;
    /* Compute/indirect device properties (Phase 21): queried once at
     * creation; zeros when compute is unsupported. */
    int compute_supported;
    int indirect_draw_supported;
    int multi_draw_indirect;
    int indirect_count_supported;
    uint32_t max_workgroup_size[3];
    uint32_t max_workgroup_count[3];
    uint32_t max_workgroup_invocations;
    uint32_t max_compute_shared_memory;
    uint32_t max_compute_push_size;
    uint32_t subgroup_size;
    /* Enabled 1.2 feature subset (Phase 21): drawIndirectCount when
     * offered (never blind). Outlives creation for capability
     * reporting. */
    VkPhysicalDeviceVulkan12Features vulkan12_enabled;

    /* Timeline completion (Phase 20, PARTs Q–R): one device timeline
     * shared by graphics and transfer submissions (one monotonic value
     * domain). Zero when unsupported (binary-fence fallback). */
    VkSemaphore timeline;
    uint64_t timeline_next; /* next value to signal (starts at 1) */
    int timeline_ok;
    /* Newest value any frame submit signaled (retirement horizon;
     * updated under the transfer lock at submit). */
    uint64_t last_frame_value;
    /* Resolved 1.2 entry points (NULL on 1.0 loaders without KHR).
     * All three are required for timeline mode; the signal pointer
     * lets the scheduler host-signal reserved-but-unsubmitted
     * values on submit failure so no waiter can stall on a gap. */
    PFN_vkGetSemaphoreCounterValue pfn_sem_counter;
    PFN_vkWaitSemaphores pfn_sem_wait;
    PFN_vkSignalSemaphore pfn_sem_signal;

    /* Transfer scheduling (Phase 20, PARTs S–V): pool + transient
     * command buffers + in-flight entries + staging pressure.
     * Guarded by transfer_mutex (tiny critical sections only). */
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION transfer_mutex;
#else
    pthread_mutex_t transfer_mutex;
#endif
    int transfer_mutex_init;
    VkCommandPool transfer_pool;
    uint32_t transfer_cmds_outstanding;
    /* Immediate graphics pool for release/acquire submits (Phase
     * 20): transient per-submit buffers, never CPU-waited. */
    VkCommandPool imm_pool;
    uint32_t imm_cmds_outstanding;
    /* Fallback (no timeline): one shared binary semaphore for
     * transfer->graphics handoff + armed flag. */
    VkSemaphore fallback_sem;
    int fallback_armed;
    uint64_t staging_cap; /* 0 = default (256 MiB) */
    /* Test-only capability forcing (Phase 22, PARTs W-Y): copied
     * from the device desc, honored at creation. */
    int force_binary_fallback;
    int force_graphics_transfer;
    int force_graphics_compute;
    uint64_t staging_used;
    uint64_t staging_high_water;
    uint64_t stat_graphics_submissions;
    uint64_t stat_transfer_submissions;
    uint64_t stat_bytes_uploaded;
    uint64_t stat_bytes_read;
    uint64_t stat_uploads_in_flight;
    uint64_t stat_lists_in_flight;
    /* Phase 22 concurrency diagnostics (transfer lock). */
    uint64_t stat_staging_waits;      /* lifetime pressure waits */
    uint64_t stat_reclaimed_transfers;/* lifetime entries unlinked */
    uint64_t stat_retired_completed;  /* lifetime retirements run */
    uint64_t stat_emit_retries;       /* lifetime emit-cap retries */
    struct lc_transfer_entry *transfers; /* in-flight async work */
    struct lc_readback_request *requests; /* live async readbacks */

    /* Deferred retirement (Phase 20, PARTs AB–AD): logically dead,
     * GPU-held resources freed after completion. */
    struct lc_retire_entry *retire_head;
    struct lc_retire_entry *retire_tail;
    uint64_t retire_pending_count;
    uint64_t retire_high_water;

    /* Submission sequencing + state lock (Phase 20, PARTs I–L):
     * tiny dedicated mutex for tracking reads/writes only — never
     * held across recording, draws, or submits. */
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION state_mutex;
#else
    pthread_mutex_t state_mutex;
#endif
    int state_mutex_init;
    uint64_t submission_seq;

    /* Cache + descriptor shards (Phase 20, PARTs 22/24): dedicated
     * small locks (never the allocator or transfer locks). */
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION cache_mutex;
    CRITICAL_SECTION desc_mutex;
#else
    pthread_mutex_t cache_mutex;
    pthread_mutex_t desc_mutex;
#endif
    int cache_mutex_init;
    int desc_mutex_init;
    /* Graphics submission shard (Phase 20): serializes vkQueueSubmit
     * (+present) on the graphics queue across frame, immediate, and
     * acquire submits from any thread. Held only around the call. */
#if defined(_WIN32) || defined(_WIN64)
    CRITICAL_SECTION submit_mutex;
#else
    pthread_mutex_t submit_mutex;
#endif
    int submit_mutex_init;

    /* Worker command-list registry (Phase 20, PART 26): all lists
     * with validity + referenced handles (borrowed, compare-only). */
    struct lc_cmdlist_record *cmdlists;
    /* Live worker encoders (liveness validation). */
    lc_command_encoder *worker_encoders;
};

/* Opaque public buffer type, completed here. A buffer belongs to one
 * device and survives swapchain recreation; device teardown destroys
 * dependent buffers before VkDevice. CPU-visible memory is mapped
 * persistently at creation (coherent required); GPU-only memory is
 * never mapped and is written through staging. */
struct lc_buffer {
    lc_device *device;
    lc_resource_id resource_id; /* stable, never reused */

    uint64_t size;
    uint32_t usage; /* lc_buffer_usage bits, as requested */
    lc_memory_usage memory_usage;

    VkBuffer vk_buffer;
    /* Allocator binding (Phase 19): suballocated block region or a
     * dedicated allocation. vk_memory/vk_offset name the binding;
     * mapped_ptr is block_map_base + offset for host classes. */
    VkDeviceMemory vk_memory;
    VkDeviceSize vk_memory_offset;
    void *mapped_ptr; /* persistent mapping, or NULL when not mappable */
    int memory_coherent;   /* valid when mapped_ptr != NULL */
    int memory_dedicated;  /* 1: own VkDeviceMemory (no suballoc) */
    struct lc_vk_mem_block *memory_block; /* NULL when dedicated */
    lc_memory_class memory_class; /* allocator class used */
    uint64_t allocation_size;     /* aligned suballocation bytes */
    /* Owning queue family (Phase 20, PART Y): transfer ops update. */
    uint32_t owner_family;
    /* Last async transfer value touching this buffer. */
    uint64_t xfer_value;
    /* Semantic buffer state (Phase 21, PART J): whole-resource
     * tracking for compute/indirect dependencies. UNDEFINED until
     * first use; transitions publish through the sync barrier
     * helper (state shard). */
    lc_resource_state buffer_state;

    lc_buffer *next;
    lc_buffer *prev;
};

/* Opaque public surface type, completed here. A surface borrows its
 * device (VkInstance owner) and its window (native handle owner); both
 * must outlive it. Destruction order is enforced by surface.c hooks:
 * device/window teardown and lc_shutdown() destroy dependent surfaces
 * first. This is NOT a swapchain - only presentation infrastructure. */
struct lc_surface {
    lc_device *device;
    lc_window *window;
    VkSurfaceKHR vk_surface;
    uint32_t present_queue_family;
    VkQueue present_queue;
    int present_supported; /* always true post-creation; kept explicit */
    lc_surface *next;
    lc_surface *prev;
};

/*
 * Fill an allocated (zeroed) lc_device with a live Vulkan device.
 * The full creation desc controls validation + pipeline-cache policy
 * (path copied by the caller into device->pipeline_cache_path).
 * On failure, tears down whatever stage was reached and returns an
 * lc_result; the caller still owns (and frees) the struct.
 */
lc_result lc_vulkan_device_create(lc_device *device,
                                  const lc_device_desc *desc);

/* Tears down Vulkan resources in reverse creation order. Idempotent over
 * partially-created devices (VK_NULL_HANDLE stages are skipped). */
void lc_vulkan_device_destroy(lc_device *device);

/* Find the retrieved queue for a family, or VK_NULL_HANDLE if the
 * device holds none (cannot happen for enumerated families). */
VkQueue lc_vulkan_get_queue(const lc_device *device, uint32_t family_index);

/*
 * Create the VkSurfaceKHR for an allocated (zeroed) lc_surface and
 * resolve presentation support. On failure, tears down whatever stage
 * was reached and returns an lc_result; the caller still owns the struct.
 */
lc_result lc_vulkan_surface_create(lc_surface *surface, lc_device *device,
                                   lc_window *window);

/* Destroys the VkSurfaceKHR. Requires device + window still alive;
 * callers (surface.c) guarantee this via dependency-ordered teardown. */
void lc_vulkan_surface_destroy(lc_surface *surface);

/* Fixed frames in flight per swapchain. Small and constant: each frame
 * owns its semaphores, fence, and command buffer, so CPU recording and
 * GPU execution overlap without per-frame allocation. */
#define LC_MAX_FRAMES_IN_FLIGHT 2

/* One in-flight frame slot: acquire/submit synchronization plus its
 * reusable primary command buffer. The acquire semaphore is strictly
 * ordered (signaled by acquire, waited by the same slot's submit after
 * the slot fence was waited on), so per-slot is safe. The
 * submit-to-present semaphore is NOT slot-safe - presentation consumes
 * it asynchronously - so it lives per swapchain image below. */
typedef struct lc_vk_flight {
    VkSemaphore image_available; /* signaled by acquire, waited by submit */
    VkFence fence; /* signaled when this slot's submission completes */
    VkCommandBuffer cmd; /* reusable; reset and rerecorded each use */
    uint64_t signal_value; /* timeline value signaled (0 in fallback) */
} lc_vk_flight;

/* Opaque public compute-pipeline type, completed here. Like
 * graphics pipelines but with a single compute shader and no
 * render-target signature. Shares the device pipeline cache and
 * the deferred-retirement path (LC_RETIRE_PIPELINE). */
struct lc_compute_pipeline {
    lc_device *device;
    lc_resource_id resource_id; /* stable, never reused */
    lc_shader *compute_shader;  /* borrowed, must outlive creation */
    VkPipelineLayout layout;
    VkPipeline pipeline;
    const lc_binding_layout **layouts; /* slot anchors, malloc'd */
    uint32_t layout_count;
    lc_binding_desc **slot_signatures; /* canonical per-slot copies */
    uint32_t *slot_signature_counts;
    lc_push_constant_range *push_ranges; /* malloc'd copy */
    uint32_t push_range_count;
    lc_compute_pipeline *next;
    lc_compute_pipeline *prev;
};

/* Validate desc (live compute shader, layouts, push ranges) and
 * create the VkPipeline + layout. Caller owns the struct. */
lc_result lc_vulkan_compute_pipeline_create(lc_compute_pipeline *pipeline,
                                            lc_device *device,
                                            const lc_compute_pipeline_desc *desc);

/* Destroys VkPipeline + layout (handles already stolen read as
 * NULL). Frees canonical CPU copies. Device must still be alive
 * for the Vk destroys; CPU frees happen on every path. */
void lc_vulkan_compute_pipeline_destroy(lc_compute_pipeline *pipeline);

/* Opaque public command-encoder type, completed here. Borrowed from
 * an open swapchain frame (inline storage, never allocated); valid
 * only between lc_begin_frame and lc_end_frame on that swapchain.
 * Exactly one explicit pass may be open at a time; legacy implicit
 * swapchain passes are mutually exclusive with explicit passes. */
struct lc_command_encoder {
    lc_swapchain *swapchain;
    lc_device *device;
    int in_pass; /* explicit pass open */
    int pass_is_swapchain; /* 1: swapchain pass, 0: offscreen target */
    lc_render_target_desc pass_target; /* structural signature of open pass */
    const lc_render_target *pass_target_obj; /* offscreen target (borrowed) */
    const lc_pipeline *bound_pipeline; /* last encoder-bound, NULL at pass start */
    const lc_buffer *bound_index_buffer;
    uint64_t bound_index_offset;
    lc_index_type bound_index_type;
    int index_bound;
    /* Phase 21 compute: last encoder-bound compute pipeline (frame
     * encoders clear at frame begin; worker encoders at list
     * begin). Graphics and compute binds coexist. */
    const lc_compute_pipeline *bound_compute_pipeline;
    /* Pass-end bookkeeping for attachment tracking (no per-frame
     * allocation: bounded copies filled at begin, consumed at end). */
    lc_image_view *end_color_views[LC_MAX_COLOR_ATTACHMENTS];
    lc_store_op end_color_stores[LC_MAX_COLOR_ATTACHMENTS];
    uint32_t end_color_count;
    lc_image_view *end_depth_view;
    lc_store_op end_depth_store;
    int end_has_depth;
    /* Phase 20 worker mode: 1 when this encoder is worker-owned
     * (lc_command_encoder_create) rather than frame-borrowed.
     * Worker encoders record secondary buffers for one list at a
     * time; passes never open on them (inheritance comes from the
     * list target instead). */
    int worker_mode;
    lc_command_list *worker_list; /* open list, or NULL */
    lc_device *worker_device;     /* owner when worker_mode */
    VkCommandPool worker_pool;    /* owned pool (recycled lists) */
    uint32_t worker_live_lists;   /* lists outstanding (pool kept) */
    int worker_zombie;            /* destroyed with live lists */
    struct lc_command_encoder *worker_next;
    struct lc_command_encoder *worker_prev;
};

/* Opaque public readback request, completed here. Staging stays
 * alive until map/destroy; completion is timeline/fence based. */
struct lc_readback_request {
    lc_device *device;
    lc_image *image; /* borrowed (destroy waits in-flight refs) */
    lc_image_readback_desc desc;
    lc_image_readback_info info;
    uint64_t ready_value; /* timeline completion value */
    int mapped;
    struct lc_transfer_entry *entry; /* linked transfer (owned here) */
    struct lc_readback_request *next;
    struct lc_readback_request *prev;
};

/* Opaque public command-list type, completed here. A finished
 * secondary-style recording plus its execution-time validation
 * log (state intents + referenced handles, compare-only). */
struct lc_command_list {
    lc_device *device;
    lc_resource_id resource_id; /* stable, never reused */
    VkCommandPool pool;         /* borrowed worker pool (frees cmd) */
    VkCommandBuffer cmd;        /* finished secondary buffer */
    uint32_t queue_family;      /* recording family (graphics) */
    /* Execution context (target + pass shape, borrowed). */
    lc_render_target *target;
    lc_render_pass_desc pass_desc;
    lc_render_color_attachment pass_color[LC_MAX_COLOR_ATTACHMENTS];
    lc_render_depth_attachment pass_depth;
    int has_depth;
    /* Validation log (PARTs I–K, 26): transitions + bind-reads. */
    struct lc_list_log_entry *log;
    uint32_t log_count;
    uint32_t log_capacity;
    /* Referenced wrappers (borrowed pointers, compare-only; the
     * registry poisons lists whose referents die first). */
    const void **refs;
    uint32_t ref_count;
    uint32_t ref_capacity;
    int executed; /* set after first execution (single-shot lists) */
    /* Phase 21 compute lists: recorded without render-pass
     * inheritance (dispatch is illegal inside a pass); execute only
     * when the primary has no open pass. */
    int is_compute;
    uint64_t completion_value; /* UINT64_MAX until frame submit binds it */
    int destroy_requested;     /* deferred while the GPU owns cmd */
    /* Phase 22 fallback completion: borrowed flight fence stamped
     * at frame submit (binary-fence mode has no values).
     * fallback_bound latches once (single-shot lists bind once);
     * the fence NULLs if its flight dies first (teardown
     * guarantees idle, so a bound-but-fenceless list is
     * complete). */
    VkFence fallback_fence;
    int fallback_bound;
    lc_command_list *next;
    lc_command_list *prev;
};

/* One validation-log entry: a transition intent or a bind-read
 * assumption, both verified in execution order at execute time. */
typedef struct lc_list_log_entry {
    int is_transition; /* 1: transition, 0: bind-read assumption */
    int is_buffer;     /* 1: buffer transition (image NULL) */
    lc_image *image;
    lc_buffer *buffer;
    uint32_t base_mip;
    uint32_t level_count;
    uint32_t base_layer;
    uint32_t layer_count;
    lc_resource_state from; /* tracked state at record time */
    lc_resource_state to;   /* transition destination / SHADER_READ */
} lc_list_log_entry;

/* Device command-list registry record (PART 26): validity +
 * borrowed referents for destroy-safety. */
typedef struct lc_cmdlist_record {
    lc_command_list *list;
    lc_command_encoder *encoder; /* creator, may be destroyed */
    int valid;
    struct lc_cmdlist_record *next;
    struct lc_cmdlist_record *prev;
} lc_cmdlist_record;

/* In-flight async transfer entry (PARTs S–V, W–X): staging plus
 * ownership obligations. Staging frees on GPU completion;
 * graphics-acquire records once at a frame boundary; the entry
 * unlinks when both are done. */
typedef struct lc_transfer_entry {
    int is_readback;   /* 1: image->staging copy (else staging->dst) */
    int is_image;      /* target kind (readbacks are always images) */
    lc_image *image;   /* borrowed; destroy waits in-flight refs */
    lc_buffer *buffer; /* borrowed; same rule */
    uint32_t base_mip;
    uint32_t level_count;
    uint32_t base_layer;
    uint32_t layer_count;
    uint64_t buf_offset;
    uint64_t buf_size;
    lc_resource_state final_state; /* images: post-copy state */
    /* Staging (transient VkBuffer + pool suballoc). */
    VkBuffer stage_buffer;
    lc_vk_mem_binding stage_mem;
    uint64_t stage_bytes;
    /* Transfer command (transient, transfer pool). */
    VkCommandBuffer cmd;
    int cmd_on_transfer_pool; /* otherwise allocated from imm_pool */
    /* Release command (transient, graphics imm pool; dedicated path
     * only, NULL otherwise). Freed at unlink like cmd. */
    VkCommandBuffer cmd_rel;
    /* Completion: timeline value, or fallback fence+semaphore. */
    uint64_t signal_value;      /* consumable (ready) value */
    uint64_t copy_value;        /* staging releasable at this value */
    VkFence fallback_fence;     /* on the copy submit */
    VkSemaphore fallback_rel;   /* release submit signal (dedicated) */
    VkSemaphore fallback_copy;  /* copy submit signal (frame waits) */
    int staging_released;
    int acquired; /* graphics acquire recorded */
    int keep_until_destroy; /* readbacks: unlink only at destroy */
    int submitted; /* submits finished (reclaim skips until set) */
    int destroy_claimed; /* wait_for drives acquire; unlink skips */
    struct lc_transfer_entry *next;
    struct lc_transfer_entry *prev;
} lc_transfer_entry;

/* Deferred retirement entry (PARTs AB–AD): logically dead wrapper
 * already freed; these Vk objects + bindings die after GPU
 * completion. Reclaimed by timeline query (or fence status in
 * fallback mode). */
typedef enum lc_retire_kind {
    LC_RETIRE_BUFFER = 0,
    LC_RETIRE_IMAGE = 1,
    LC_RETIRE_VIEW = 2,
    LC_RETIRE_SAMPLER = 3,
    LC_RETIRE_PIPELINE = 4,
    LC_RETIRE_SET = 5,
    LC_RETIRE_LAYOUT = 6,
    LC_RETIRE_FRAMEBUFFER = 7
} lc_retire_kind;

typedef struct lc_retire_entry {
    lc_retire_kind kind;
    VkBuffer buffer;
    VkImage image;
    VkImageView image_view;
    VkImageView default_view;
    VkSampler sampler;
    VkPipeline pipeline;
    VkPipelineLayout pipeline_layout;
    VkDescriptorSet set;
    VkDescriptorPool set_pool;
    VkDescriptorSetLayout desc_layout;
    VkFramebuffer framebuffer;
    lc_vk_mem_binding binding; /* owned memory (buffer/image) */
    uint64_t signal_value;     /* timeline reclaim threshold */
    VkFence fallback_fence;    /* fence mode reclaim gate */
    uint64_t bytes;            /* binding size (diagnostics) */
    struct lc_retire_entry *next;
    struct lc_retire_entry *prev;
} lc_retire_entry;

/* Opaque public render-target type, completed here. Offscreen targets
 * are device-owned (tracked list) and borrow their views; the
 * borrowed swapchain target lives inline in lc_swapchain (never
 * tracked, never destroyed directly). One VkFramebuffer is created
 * lazily on first begin and reused for all load/store variants (same
 * formats/count/extent stay compatible). */
struct lc_render_target {
    lc_device *device;
    lc_resource_id resource_id; /* stable, never reused */
    uint32_t width;
    uint32_t height;
    lc_image_view **color_views; /* borrowed, malloc'd array (maybe NULL) */
    uint32_t color_count;
    lc_image_view *depth_view; /* borrowed, maybe NULL */
    lc_format color_formats[LC_MAX_COLOR_ATTACHMENTS];
    lc_format depth_format; /* UNDEFINED when depthless */
    lc_sample_count samples;
    uint32_t hash; /* structural signature hash */
    int is_swapchain_borrow; /* 1: inline in swapchain, reject destroy */
    VkFramebuffer framebuffer; /* offscreen only, lazy (swapchain uses
                                * its per-image legacy framebuffers) */
    int framebuffer_valid;
    lc_render_target *next;
    lc_render_target *prev;
};
/* Opaque public swapchain type, completed here. A swapchain borrows its
 * device and surface; both must outlive it. Destruction order is
 * enforced by hooks: surface/device/window teardown and lc_shutdown()
 * destroy dependent swapchains first (see swapchain.c). Images are owned
 * by VkSwapchainKHR (never destroyed directly); views are owned here.
 * Frame state (pool, slots, per-image tracking) is swapchain-local, so
 * two swapchains run fully independent frame loops. Phase 11 adds an
 * owned depth buffer (image + memory + view) sized to the extent and
 * rebuilt with the swapchain; the render pass clears it alongside
 * color. Phase 12 adds a borrowed render-target view plus an inline
 * command encoder sharing the frame command buffer with the legacy
 * implicit pass (mutually exclusive per frame). Pipelines are NOT
 * anchored here anymore (structural compatibility only). */
struct lc_swapchain {
    lc_device *device;
    lc_surface *surface;
    int vsync; /* preserved across lc_swapchain_recreate() */
    uint32_t preferred_image_count; /* 0 = automatic; preserved too */

    VkSwapchainKHR vk_swapchain;

    VkFormat format;
    VkColorSpaceKHR color_space;
    VkPresentModeKHR present_mode;

    VkExtent2D extent; /* actual, capability-clamped extent */

    VkImage *images; /* owned by VkSwapchainKHR; array owned here */
    VkImageView *image_views; /* one 2D color view per image, owned here */
    uint32_t image_count;
    /* NOTE: no per-image layout tracking is needed. The render pass
     * always starts from UNDEFINED (contents come from its clear) and
     * ends at PRESENT_SRC, so every frame is self-contained. */
    /* One present semaphore per image (not per flight slot): submit
     * signals it, presentation consumes it asynchronously, and with
     * more images than flight slots a slot's semaphore could otherwise
     * be re-signaled while a previous present still holds it
     * (VUID-vkQueueSubmit-pSignalSemaphores). Rebuilt with the images;
     * indexed by acquired image. */
    VkSemaphore *present_semaphores;
    /* Depth buffer owned by the swapchain (Phase 11): one image sized
     * to the extent, rebuilt on every recreate. Format is selected
     * once per device from supported depth formats. */
    VkFormat depth_format;
    VkImage depth_image;
    VkDeviceMemory depth_memory;
    VkImageView depth_view;
    /* Render scope (Phase 7, extended Phase 11): one render pass with
     * color + depth attachments (clear/store, UNDEFINED-to-present)
     * plus one framebuffer per image view, rebuilt with the swapchain.
     * The pass begins lazily on first clear/bind/draw so the clear
     * colors are always known. */
    VkRenderPass render_pass;
    VkFramebuffer *framebuffers; /* one per image, owned here */
    /* Open-frame recording state. */
    const lc_pipeline *bound_pipeline; /* last bound, NULL at frame start */
    int rp_open; /* render pass instance active in the command buffer */
    int clear_pending; /* lc_clear_color stored but not yet consumed */
    float clear_r;
    float clear_g;
    float clear_b;
    float clear_a;
    int depth_clear_pending; /* lc_clear_depth stored but not consumed */
    float depth_clear; /* clamped [0,1], defaults to 1.0 per frame */
    /* Index-buffer binding (Phase 11): command-buffer state, reset at
     * frame start, re-established by lc_bind_index_buffer. */
    const lc_buffer *bound_index_buffer;
    uint64_t bound_index_offset;
    lc_index_type bound_index_type;
    int index_bound; /* nonzero once bound this frame */

    /* Frame lifecycle (Phase 6, configurable Phase 20). Pool/buffers/
     * sync persist across recreates; per-image tracking is rebuilt
     * with the images. flights is malloc'd to max_flights slots. */
    VkCommandPool cmd_pool;
    lc_vk_flight *flights;
    uint32_t max_flights; /* from desc (default LC_MAX_FRAMES_IN_FLIGHT) */
    uint32_t current_frame; /* next flight slot, not an image index */
    VkFence *images_in_flight; /* per image: fence to wait before reuse */
    uint32_t current_image; /* acquired index, valid only mid-frame */
    int frame_active; /* exactly one open frame per swapchain max */
    int frame_suboptimal; /* acquire reported SUBOPTIMAL this frame */

    /* Phase 12 generic recording: inline borrowed encoder sharing the
     * frame command buffer with the legacy implicit pass (exactly one
     * of legacy rp_open / encoder.in_pass may be active), plus a
     * borrowed render-target snapshot refreshed on every rebuild. */
    lc_command_encoder encoder;
    lc_render_target swapchain_target;

    lc_swapchain *next;
    lc_swapchain *prev;
};

/*
 * Transactionally (re)build the Vulkan resources of an allocated
 * lc_swapchain whose device/surface/vsync fields are already set.
 * old_swapchain is passed as VkSwapchainKHR oldSwapchain (may be
 * VK_NULL_HANDLE for fresh creation). On success the struct holds the
 * new resources; on failure it is left exactly as it was (old state
 * intact) and an lc_result is returned. Zero width/height yields
 * LC_ERROR_ZERO_EXTENT without touching Vulkan.
 */
lc_result lc_vulkan_swapchain_rebuild(lc_swapchain *swapchain,
                                      uint32_t width, uint32_t height,
                                      VkSwapchainKHR old_swapchain);

/* Destroys views, present semaphores, then the VkSwapchainKHR, then
 * frees the image and per-image tracking arrays, and zeroes the
 * resource fields (device/surface/vsync/links/frame slots untouched).
 * Requires device + surface still alive; callers guarantee ordering.
 * Safe on empty/partial state. Waits idle first: frames may be in
 * flight, and destroying a swapchain with pending work is invalid;
 * this coarse wait is intentionally simple and may be refined later. */
void lc_vulkan_swapchain_teardown(lc_swapchain *swapchain);

/*
 * Create the swapchain-local frame objects (command pool, per-flight
 * semaphores/fences/command buffers). Called once after the first
 * successful build; persists across recreates. On failure tears down
 * what it created and returns an lc_result. Fences start signaled so
 * frame zero never deadlocks.
 */
lc_result lc_vulkan_frame_init(lc_swapchain *swapchain);

/* Destroys frame objects (pool frees command buffers implicitly, then
 * semaphores and fences). Safe on empty/partial state. */
void lc_vulkan_frame_teardown(lc_swapchain *swapchain);

/* Record a pipeline bind, opening the render pass first if needed.
 * The pipeline's compatibility was verified by the caller. */
lc_result lc_vulkan_frame_bind(lc_swapchain *swapchain,
                               const lc_pipeline *pipeline);

/* Record a draw with the bound pipeline, opening the render pass
 * first if needed. Requires a bound pipeline. */
lc_result lc_vulkan_frame_draw(lc_swapchain *swapchain, uint32_t vertex_count,
                               uint32_t first_vertex);

/* Phase 11 frame recording: indexed/instanced draws, index binding,
 * push constants, depth clears. */
lc_result lc_vulkan_frame_draw_indexed(lc_swapchain *swapchain,
                                       uint32_t index_count,
                                       uint32_t instance_count,
                                       uint32_t first_index,
                                       int32_t vertex_offset,
                                       uint32_t first_instance);
lc_result lc_vulkan_frame_draw_instanced(lc_swapchain *swapchain,
                                         uint32_t vertex_count,
                                         uint32_t instance_count,
                                         uint32_t first_vertex,
                                         uint32_t first_instance);
lc_result lc_vulkan_frame_bind_index(lc_swapchain *swapchain,
                                     const lc_buffer *buffer,
                                     uint64_t offset,
                                     lc_index_type index_type);
lc_result lc_vulkan_frame_push(lc_swapchain *swapchain,
                               const lc_pipeline *pipeline,
                               uint32_t visibility, uint32_t offset,
                               uint32_t size, const void *data);
lc_result lc_vulkan_frame_clear_depth(lc_swapchain *swapchain, float depth);

/* Maximum entry-point name stored per shader (including NUL). */
#define LC_SHADER_ENTRY_MAX 64

/* Opaque public shader type, completed here. A shader belongs to one
 * device; pipeline creation consumes only the module, so shaders may
 * die while their pipelines live on. */
struct lc_shader {
    lc_device *device;
    lc_resource_id resource_id; /* stable, never reused */
    lc_shader_stage stage;
    char entry_point[LC_SHADER_ENTRY_MAX]; /* normalized, NUL-terminated */
    VkShaderModule module;
    lc_shader *next;
    lc_shader *prev;
};

/*
 * Validate `desc` and create the VkShaderModule on a live device.
 * On failure tears down whatever stage was reached; the caller still
 * owns (and frees) the struct.
 */
lc_result lc_vulkan_shader_create(lc_shader *shader, lc_device *device,
                                  const lc_shader_desc *desc);

/* Destroys the VkShaderModule. Device must still be alive; callers
 * guarantee ordering via device-teardown hooks. */
void lc_vulkan_shader_destroy(lc_shader *shader);

/* Opaque public pipeline type, completed here. Phase 12: pipelines
 * are device children with a structural render-target signature
 * (color count/formats, depth format, samples) copied at creation —
 * never a swapchain or target pointer. Any target or pass with an
 * equal signature works; incompatible use fails predictably. Phase 11
 * canonical binding-layout signatures plus push ranges are kept for
 * content-based bind/push validation. */
struct lc_pipeline {
    lc_device *device;
    lc_resource_id resource_id; /* stable, never reused */
    VkPipelineLayout layout;
    VkPipeline pipeline;
    /* Structural compatibility (backend-neutral, D3D12-mappable). */
    uint32_t target_color_count;
    lc_format target_color_formats[LC_MAX_COLOR_ATTACHMENTS];
    lc_format target_depth_format; /* UNDEFINED when depthless */
    lc_sample_count target_samples;
    uint32_t target_hash; /* FNV-1a over the signature; collisions
                           * resolve by structural compare */
    const lc_binding_layout **layouts; /* slot anchors, malloc'd (maybe NULL) */
    uint32_t layout_count;
    /* Canonical signatures: one sorted slot copy per layout slot.
     * slot_signatures[i] has slot_signature_counts[i] entries. */
    lc_binding_desc **slot_signatures;
    uint32_t *slot_signature_counts;
    lc_cull_mode cull_mode;
    lc_front_face front_face;
    int depth_test_enable;
    int depth_write_enable;
    lc_push_constant_range *push_ranges; /* malloc'd copy (maybe NULL) */
    uint32_t push_range_count;
    lc_pipeline *next;
    lc_pipeline *prev;
};

/*
 * Validate shaders (live, correctly staged, same device), the vertex
 * layout, and the mandatory structural render-target signature, then
 * create the pipeline layout plus graphics pipeline against a cached
 * compatible render pass. Consumes only module handles: shaders may be
 * destroyed afterwards. No swapchain is involved (Phase 13). On
 * failure tears down partial state.
 */
lc_result lc_vulkan_pipeline_create(lc_pipeline *pipeline, lc_device *device,
                                    const lc_graphics_pipeline_desc *desc);

/* Destroys pipeline then layout. Device must still be alive. */
void lc_vulkan_pipeline_destroy(lc_pipeline *pipeline);

/* Canonical signature comparison (Phase 11 fix for anchor
 * discipline): sorted slot copies compare by contents (binding,
 * type, count, visibility), never by layout pointer. Returns
 * nonzero when equal. */
int lc_binding_signature_equal(const lc_binding_desc *a, uint32_t a_count,
                               const lc_binding_desc *b, uint32_t b_count);

/* Issue the next stable resource ID (never 0, never reused within
 * the process). Threading: main thread only, like all LumaC use. */
lc_resource_id lc_issue_resource_id(void);

/* Centralized backend-neutral <-> Vulkan format translation. Unknown
 * inputs map to VK_FORMAT_UNDEFINED / LC_FORMAT_UNDEFINED. */
VkFormat lc_vulkan_translate_format(lc_format format);
lc_format lc_vulkan_untranslate_format(VkFormat format);

/* Aspect mask for a format (color/depth/stencil bits). */
VkImageAspectFlags lc_vk_aspect_for(lc_format format);

/* Byte size of one lc_format element (0 for UNDEFINED). */
uint32_t lc_format_byte_size(lc_format format);

/* Format classification (0 for UNDEFINED/unknown). Backs aspect-mask
 * choice, upload validation, and future depth paths. */
int lc_format_is_color(lc_format format);
int lc_format_is_depth(lc_format format);
int lc_format_is_stencil(lc_format format);
uint32_t lc_format_component_count(lc_format format);

/*
 * Fill an allocated (zeroed) lc_buffer whose size/usage/memory fields
 * are already set: create VkBuffer, allocate and bind memory
 * (persistently mapping CPU-visible coherent memory). On failure tears
 * down whatever stage was reached; the caller still owns the struct.
 */
lc_result lc_vulkan_buffer_create(lc_buffer *buffer);

/* Unmaps (if mapped) and destroys buffer + memory. Device must still
 * be alive; callers guarantee ordering via device-teardown hooks. */
void lc_vulkan_buffer_destroy(lc_buffer *buffer);

/*
 * Bounds-checked write; assumes the caller validated liveness. Mappable
 * buffers memcpy directly; GPU-only buffers stage through a temporary
 * CPU-visible buffer plus an immediate-submit copy. Returns an
 * lc_result; data/size were validated by the caller.
 */
lc_result lc_vulkan_buffer_write(lc_buffer *buffer, uint64_t offset,
                                 const void *data, uint64_t size);
/* Synchronous download (Phase 21, test/debug path): drains prior
 * work, copies through staging, invalidates, memcpys out. */
lc_result lc_vulkan_buffer_read(lc_buffer *buffer, uint64_t offset,
                                void *dst, uint64_t size);
/* Full-range invalidate for map (non-coherent correctness). */
lc_result lc_vulkan_buffer_invalidate(lc_buffer *buffer);

/*
 * Device upload context (lazy immediate-submit on the graphics queue).
 * Ensure creates pool/buffer/fence on first use; begin drains prior
 * use and starts recording; cmd exposes the recording buffer; submit
 * ends, submits, and waits; teardown destroys all three. One session
 * records arbitrary upload work (copies, blits, barriers) without a
 * public command API. Safe on empty state; device must be alive.
 */
lc_result lc_vulkan_upload_ensure(lc_device *device);
void lc_vulkan_upload_teardown(lc_device *device);
lc_result lc_vulkan_upload_begin(lc_device *device);
VkCommandBuffer lc_vulkan_upload_cmd(const lc_device *device);
lc_result lc_vulkan_upload_submit(lc_device *device);

/* Raw buffer storage block shared by tracked buffers and staging
 * scratch (tracked and untracked alike): create, size, bind, optional
 * persistent map. Fully unwinding. `out_mapped` may be NULL to skip
 * mapping. */
lc_result lc_vulkan_storage_create(
    lc_device *device, uint64_t size, VkBufferUsageFlags usage,
    VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred,
    VkBuffer *out_buffer, VkDeviceMemory *out_memory, void **out_mapped);

/* Shared memory-type search (required bits + ordered preferences). */
int lc_vk_find_memory_type(VkPhysicalDevice physical, uint32_t type_bits,
                           VkMemoryPropertyFlags required,
                           VkMemoryPropertyFlags preferred,
                           uint32_t *out_index);

/* AAA allocator (vulkan_memory.c): suballocate or dedicate. */
lc_result lc_vk_mem_alloc(lc_device *device, lc_vk_mem_class cls,
                          uint32_t type_bits,
                          VkMemoryPropertyFlags required,
                          uint64_t size, uint64_t align,
                          lc_vk_mem_binding *out);
void lc_vk_mem_free(lc_device *device, lc_vk_mem_binding *binding);
lc_result lc_vk_mem_flush(lc_device *device,
                          const lc_vk_mem_binding *binding, uint64_t offset,
                          uint64_t size);
lc_result lc_vk_mem_invalidate(lc_device *device,
                               const lc_vk_mem_binding *binding,
                               uint64_t offset, uint64_t size);
/* Transient pool staging (PART Z): VkBuffer + bound suballoc. */
lc_result lc_vk_stage_acquire(lc_device *device, uint64_t size,
                              int upload, VkBufferUsageFlags usage,
                              VkBuffer *out_buffer,
                              lc_vk_mem_binding *out_binding,
                              void **out_mapped);
void lc_vk_stage_release(lc_device *device, VkBuffer buffer,
                         lc_vk_mem_binding *binding);
void lc_vk_mem_teardown(lc_device *device);
/* Locked statistics walk (PARTs AA/AC). */
void lc_vk_mem_stats(const lc_device *device, lc_memory_stats *out);
/* Exported (but not in the public header) so white-box integration
 * tests can drive the copy path directly; not part of the API. */
LC_API lc_result lc_vulkan_copy_buffer(lc_device *device, VkBuffer dst,
                                       uint64_t dst_offset, VkBuffer src,
                                       uint64_t src_offset, uint64_t size);

/* Record a vertex-buffer bind into the open frame's command buffer.
 * Validation (liveness, device match, VERTEX usage, offset) is done by
 * the caller; binding number range is checked here against the device. */
lc_result lc_vulkan_frame_bind_vertex(lc_swapchain *swapchain,
                                      uint32_t binding,
                                      const lc_buffer *buffer,
                                      uint64_t offset);

/* Opaque public image type, completed here. An image belongs to one
 * device and never to a swapchain, so it survives swapchain
 * recreation; device teardown destroys dependent images before
 * VkDevice. `layout` tracks the whole image uniformly (see below);
 * per-subresource state is documented future work. The default view
 * covers all mips and layers; specialized views arrive later. */
struct lc_image {
    lc_device *device;
    lc_resource_id resource_id; /* stable, never reused */

    lc_image_type type;
    lc_format format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mip_levels;
    uint32_t array_layers;
    uint32_t usage; /* lc_image_usage bits, as requested */
    uint32_t flags; /* lc_image_flags bits, as requested */
    uint32_t samples;

    VkImage vk_image;
    /* Allocator binding (Phase 19): same discipline as buffers. */
    VkDeviceMemory vk_memory;
    VkDeviceSize vk_memory_offset;
    int memory_dedicated;
    struct lc_vk_mem_block *memory_block; /* NULL when dedicated */
    lc_memory_class memory_class;
    uint64_t allocation_size;
    VkImageView default_view;

    /* Per-subresource SEMANTIC state tracking (Phase 19): entry
     * [layer * mip_levels + mip] for every mip of every layer
     * (malloc'd). Barriers derive Vulkan layouts/stages/access from
     * these states; mixed states (e.g. mip 0 SHADER_READ while mip
     * 1 renders) stay representable and truthful. */
    lc_resource_state *states;
    /* Owning queue family per subresource (Phase 20, PART Y):
     * graphics family normally, transfer family while an async
     * transfer holds the range. Parallel array to states. */
    uint32_t *owners;
    /* Last submission sequence touching each subresource (Phase 20,
     * PART L epochs). Parallel array to states. */
    uint64_t *epochs;
    /* Last async transfer value touching this image (same-resource
     * ordering for streaming updates). */
    uint64_t xfer_value;
    lc_image *next;
    lc_image *prev;
};

/*
 * Validate `desc` (dimensions per type, format, usage, mips, layers,
 * cube combination, samples) and create the VkImage, memory, and
 * default full-resource view. Starts in UNDEFINED layout. On failure
 * tears down whatever stage was reached; the caller owns the struct.
 */
lc_result lc_vulkan_image_create(lc_image *image, lc_device *device,
                                 const lc_image_desc *desc);

/* Destroys view, image, and memory. Device must still be alive;
 * callers guarantee ordering via device-teardown hooks. */
void lc_vulkan_image_destroy(lc_image *image);

/*
 * Transition the whole image to a semantic state on the upload
 * context (immediate submit, one barrier per subresource). Old
 * states come from tracking (PART F). Tracked state follows only
 * on success.
 */
lc_result lc_vulkan_image_transition(lc_image *image,
                                     lc_resource_state new_state);

/*
 * Transition an explicit mip/layer range to a semantic state (one
 * barrier per subresource, immediate submit). Used by mip
 * generation, where levels temporarily diverge.
 */
lc_result lc_vulkan_image_transition_range(
    lc_image *image, uint32_t base_mip, uint32_t level_count,
    uint32_t base_layer, uint32_t layer_count,
    lc_resource_state new_state);

/* Opaque public image-view type, completed here. A view borrows its
 * image (and thereby its device); image teardown destroys dependent
 * views first. Views carry no layout state themselves. */
struct lc_image_view {
    lc_device *device;
    lc_image *image;
    lc_resource_id resource_id; /* stable, never reused */
    lc_image_view_type type;
    lc_format format;
    uint32_t aspect; /* lc_image_aspect bits, as requested */
    uint32_t base_mip_level;
    uint32_t mip_level_count;
    uint32_t base_array_layer;
    uint32_t array_layer_count;
    VkImageView vk_view;
    lc_image_view *next;
    lc_image_view *prev;
};

/*
 * Validate `desc` against the live image (type/dimension match, aspect
 * vs format, ranges, cube rules, format equality) and create the
 * VkImageView. On failure the struct stays empty; the caller owns it.
 */
lc_result lc_vulkan_image_view_create(lc_image_view *view, lc_image *image,
                                      const lc_image_view_desc *desc);

/* Destroys the VkImageView. Device must still be alive. */
void lc_vulkan_image_view_destroy(lc_image_view *view);

/* Upload one tightly packed mip/layer region via staging. Assumes the
 * caller validated liveness, range, size, and TRANSFER_DST usage. */
lc_result lc_vulkan_image_write(lc_image *image,
                                const lc_image_upload_desc *upload);

/* Generate the full mip chain with linear blits, ending all levels
 * sampled-readable. Requires TRANSFER_SRC + TRANSFER_DST usage and
 * linear-blit support for the format. */
lc_result lc_vulkan_image_generate_mipmaps(lc_image *image);

/* Public readback backend: copy one mip/layer into CPU memory
 * (tightly packed). Assumes the caller validated liveness, range,
 * format, samples, and TRANSFER_SRC usage and computed `byte_size`.
 * Drains prior device work first (synchronous convenience). */
lc_result lc_vulkan_image_readback(lc_image *image, uint32_t mip_level,
                                   uint32_t array_layer, uint32_t width,
                                   uint32_t height, uint32_t depth,
                                   void *dst, size_t byte_size);

/* Copy one mip/layer region into a buffer, leaving the image
 * sampled-readable. Exported (but not in the public header) so
 * white-box integration tests can verify round-trips exactly; not
 * part of the API. */
LC_API lc_result lc_vulkan_copy_image_to_buffer(
    lc_device *device, lc_image *image, uint32_t mip_level,
    uint32_t array_layer, uint32_t width, uint32_t height, uint32_t depth,
    VkBuffer dst, uint64_t dst_offset);

/* Mark one mip/layer range at a new semantic state after an
 * explicit render pass whose finalLayout already performed the
 * transition (no barrier emitted). Used by the encoder for
 * attachment stores. Returns INVALID_ARGUMENT for bad ranges. */
lc_result lc_vulkan_image_notify_range(lc_image *image, uint32_t base_mip,
                                       uint32_t level_count,
                                       uint32_t base_layer,
                                       uint32_t layer_count,
                                       lc_resource_state new_state);

/* Sync helpers (vulkan_sync.c): range validation, tracking
 * queries, state marking. */
int lc_vk_sync_state_valid_for_image(lc_resource_state state);
int lc_vk_sync_validate_range(const lc_image *image, uint32_t base_mip,
                              uint32_t level_count, uint32_t base_layer,
                              uint32_t layer_count);
void lc_vk_sync_mark(lc_image *image, uint32_t base_mip,
                     uint32_t level_count, uint32_t base_layer,
                     uint32_t layer_count, lc_resource_state state);
int lc_vk_sync_all_equal(const lc_image *image, uint32_t base_mip,
                         uint32_t level_count, uint32_t base_layer,
                         uint32_t layer_count, lc_resource_state state);
/* Recorded transition into a caller-provided command buffer. */
lc_result lc_vulkan_encoder_transition_image(
    VkCommandBuffer cmd, lc_image *image, uint32_t base_mip,
    uint32_t level_count, uint32_t base_layer, uint32_t layer_count,
    lc_resource_state new_state);
/* Pure span barrier emission with caller-known old state (no
 * tracking touch; the caller marks). Mip-generation dance. */
lc_result lc_vk_sync_record_span(VkCommandBuffer cmd, lc_image *image,
                                uint32_t base_mip, uint32_t level_count,
                                uint32_t base_layer, uint32_t layer_count,
                                lc_resource_state old_state,
                                lc_resource_state new_state);
/* Barrier parameters for one semantic state (layout + stage +
 * access). Returns 0 for states with no image meaning. */
int lc_vk_sync_barrier_params(lc_resource_state state,
                              VkImageLayout *out_layout,
                              VkPipelineStageFlags *out_stage,
                              VkAccessFlags *out_access);

/* Opaque public sampler type, completed here. Device-owned, fully
 * independent of images (no bindings exist yet). */
struct lc_sampler {
    lc_device *device;
    lc_resource_id resource_id; /* stable, never reused */
    VkSampler vk_sampler;
    lc_sampler *next;
    lc_sampler *prev;
};

/*
 * Validate `desc` (enums, LOD range, anisotropy against device
 * support) and create the VkSampler. On failure the struct is left
 * empty; the caller still owns it.
 */
lc_result lc_vulkan_sampler_create(lc_sampler *sampler, lc_device *device,
                                   const lc_sampler_desc *desc);

/* Destroys the VkSampler. Device must still be alive. */
void lc_vulkan_sampler_destroy(lc_sampler *sampler);

/*
 * Begin a frame on a live, idle swapchain struct: wait for a flight
 * slot, acquire an image (handling out-of-date/suboptimal), wait any
 * image-owned fence, reset fence + command buffer, begin recording,
 * and transition the image for clearing.
 */
lc_result lc_vulkan_frame_begin(lc_swapchain *swapchain);

/* Record a clear of the active frame's image. Requires an open frame. */
lc_result lc_vulkan_frame_clear(lc_swapchain *swapchain, float r, float g,
                                float b, float a);

/*
 * Finish a frame: transition the image for presentation, end
 * recording, submit (waiting on acquire, signaling present, fencing
 * the slot), present on the present queue, and advance the flight
 * index. Maps out-of-date/suboptimal to recoverable results.
 */
lc_result lc_vulkan_frame_end(lc_swapchain *swapchain);

/* Opaque public binding-layout type, completed here. Device-owned;
 * pipelines and sets built from it hold non-owning anchors (see
 * binding.c), so destroying a layout first invalidates them for
 * binding, rejected where detectable. */
struct lc_binding_layout {
    lc_device *device;
    lc_resource_id resource_id; /* stable, never reused */
    lc_binding_desc *bindings; /* sorted copy, malloc'd */
    uint32_t binding_count;
    VkDescriptorSetLayout vk_layout;
    lc_binding_layout *next;
    lc_binding_layout *prev;
};

/*
 * Validate `desc` (counts, duplicates, types, visibility, device
 * limits) and create the VkDescriptorSetLayout. Sorted internal copy
 * of the slots. On failure the struct stays empty.
 */
lc_result lc_vulkan_binding_layout_create(lc_binding_layout *layout,
                                          lc_device *device,
                                          const lc_binding_layout_desc *desc);

/* Destroys the VkDescriptorSetLayout. Device must still be alive. */
void lc_vulkan_binding_layout_destroy(lc_binding_layout *layout);

/* Destroys all descriptor pools (all sets must already be freed via
 * hooks). Called by lc_vulkan_device_destroy(). Safe on empty state. */
void lc_vulkan_desc_teardown(lc_device *device);

/* Opaque public binding-set type, completed here. References (never
 * owns) its layout and resources; the set's VkDescriptorSet is freed
 * back to the device allocator on destroy. The slot snapshot (copied
 * at creation) keeps updates valid even if the layout object dies
 * first; slot matching at bind time still uses the anchor. */
struct lc_binding_set {
    lc_device *device;
    lc_resource_id resource_id; /* stable, never reused */
    const lc_binding_layout *layout; /* creation anchor, non-owning */
    lc_binding_desc *slots; /* sorted snapshot copy, malloc'd (maybe NULL) */
    uint32_t slot_count;
    VkDescriptorSet vk_set;
    VkDescriptorPool vk_pool; /* owning pool, for individual free */
    lc_binding_set *next;
    lc_binding_set *prev;
};

/*
 * Allocate a descriptor set for a live layout from the device
 * allocator. On failure the struct stays empty.
 */
lc_result lc_vulkan_binding_set_create(lc_binding_set *set,
                                       const lc_binding_layout *layout);

/* Frees the descriptor allocation. Device must still be alive. */
void lc_vulkan_binding_set_destroy(lc_binding_set *set);

/*
 * Validate every write against the set's layout (slot, type, array
 * bounds, buffer ranges, device match, liveness) into temporary
 * Vulkan structs, then record them with a single
 * vkUpdateDescriptorSets call. All-or-nothing: a bad write leaves
 * prior updates of the same call unrecorded. Image bindings require
 * sampled-readable tracked state over the view's range.
 */
lc_result lc_vulkan_binding_set_update(lc_binding_set *set,
                                       const lc_binding_write *writes,
                                       uint32_t write_count);

/* Record a descriptor-set bind at a pipeline slot. Compatibility was
 * verified by the caller (exact layout anchor match). */
lc_result lc_vulkan_frame_bind_set(lc_swapchain *swapchain,
                                   const lc_pipeline *pipeline,
                                   uint32_t slot, const lc_binding_set *set);

/* ------------------------------------------------------------------
 * Phase 12 render-target / pass-cache / encoder backend.
 * ------------------------------------------------------------------ */

/* Structural target-signature hash (FNV-1a over count, formats,
 * depth, samples). Collisions resolve by structural compare. */
uint32_t lc_render_target_hash(uint32_t color_count,
                               const lc_format *color_formats,
                               lc_format depth_format,
                               lc_sample_count samples);

/* Structural equality of two target descs (ignores width/height).
 * Returns nonzero when compatible for pipeline sharing. */
int lc_render_target_desc_equal(const lc_render_target_desc *a,
                                const lc_render_target_desc *b);

/* Validate a structural target desc for pipeline creation (no views).
 * Requires 1..8 colors and/or a depth format, color formats only,
 * depth format with depth, known sample count, within device color
 * limits. Width/height ignored. */
lc_result lc_vulkan_target_desc_validate(
    lc_device *device, const lc_render_target_desc *desc);

/* Get-or-create a cached VkRenderPass for a key (lazy). Never evicted;
 * destroyed with the device. */
lc_result lc_vulkan_pass_cache_get(lc_device *device,
                                   const lc_vk_pass_key *key,
                                   VkRenderPass *out_pass);

/* Destroy the whole pass cache. Requires no live targets/framebuffers
 * (callers destroy those first via hooks). Safe on empty state. */
void lc_vulkan_pass_cache_teardown(lc_device *device);

/* Map helpers (backend-neutral -> Vulkan). Unknown inputs map to
 * zero/UNDEFINED equivalents; callers validate first. */
VkSampleCountFlagBits lc_vulkan_translate_samples(lc_sample_count samples);
VkAttachmentLoadOp lc_vulkan_translate_load(lc_load_op op);
VkAttachmentStoreOp lc_vulkan_translate_store(lc_store_op op);

/* Validate + fill an allocated (zeroed) offscreen lc_render_target
 * (views borrowed, formats inferred, lazy framebuffer). The caller
 * owns the struct and its tracking-list membership. */
lc_result lc_vulkan_render_target_create(lc_render_target *target,
                                         lc_device *device,
                                         const lc_render_target_create_desc *desc);

/* Destroy an offscreen target's framebuffer (views/images untouched).
 * Device must still be alive. Safe on partial state. */
void lc_vulkan_render_target_destroy(lc_render_target *target);

/* Refresh a swapchain's inline borrowed target snapshot from its
 * current extent/formats (called on every rebuild + teardown reset).
 * No Vulkan work; never fails. */
void lc_vulkan_swapchain_target_sync(lc_swapchain *swapchain);

/* Ensure an offscreen target's VkFramebuffer for `pass` exists
 * (lazy, reused across ops variants). Creates on first use. */
lc_result lc_vulkan_target_ensure_framebuffer(lc_render_target *target,
                                              VkRenderPass pass);

/* Shared offscreen pass lookup + framebuffer ensure (explicit
 * passes and worker-list inheritance share one recipe). */
lc_result lc_vulkan_offscreen_pass(lc_device *device,
                                   lc_render_target *target,
                                   const lc_render_pass_desc *desc,
                                   VkRenderPass *out_pass);
/* Encoder recording (all require an open frame; pass state validated
 * by the caller, re-checked here defensively). */
lc_result lc_vulkan_encoder_begin_offscreen(    lc_command_encoder *enc, lc_render_target *target,
    const lc_render_pass_desc *desc);
lc_result lc_vulkan_encoder_begin_swapchain(
    lc_command_encoder *enc, lc_swapchain *swapchain,
    const lc_render_swapchain_pass_desc *desc);
lc_result lc_vulkan_encoder_end(lc_command_encoder *enc);
lc_result lc_vulkan_encoder_bind(lc_command_encoder *enc,
                                 const lc_pipeline *pipeline);
lc_result lc_vulkan_encoder_bind_set(lc_command_encoder *enc,
                                     const lc_pipeline *pipeline,
                                     uint32_t slot,
                                     lc_binding_set *set);
lc_result lc_vulkan_encoder_bind_vertex(lc_command_encoder *enc,
                                        uint32_t binding,
                                        const lc_buffer *buffer,
                                        uint64_t offset);
lc_result lc_vulkan_encoder_bind_index(lc_command_encoder *enc,
                                       const lc_buffer *buffer,
                                       uint64_t offset,
                                       lc_index_type index_type);
lc_result lc_vulkan_encoder_push(lc_command_encoder *enc,
                                 const lc_pipeline *pipeline,
                                 uint32_t visibility, uint32_t offset,
                                 uint32_t size, const void *data);
lc_result lc_vulkan_encoder_draw(lc_command_encoder *enc,
                                 uint32_t vertex_count,
                                 uint32_t first_vertex);
lc_result lc_vulkan_encoder_draw_indexed(
    lc_command_encoder *enc, uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, int32_t vertex_offset, uint32_t first_instance);
lc_result lc_vulkan_encoder_draw_instanced(
    lc_command_encoder *enc, uint32_t vertex_count, uint32_t instance_count,
    uint32_t first_vertex, uint32_t first_instance);
/* Explicit image transition (open frame required; passes optional).
 * Validates frame/device, then records via the transition_image
 * barrier helper. */
lc_result lc_vulkan_encoder_transition(
    lc_command_encoder *enc, lc_image *image, uint32_t base_mip,
    uint32_t level_count, uint32_t base_layer, uint32_t layer_count,
    lc_resource_state new_state);

/* Phase 20 lock shards (tiny critical sections) plus the Phase 22
 * nesting contract. The ONLY legal cross-shard nesting is
 * submit -> transfer (frame submission holds submit across
 * emit->submit); taking submit while holding transfer is forbidden,
 * as is any other cross-shard nesting. Same-shard recursion is
 * always legal. Debug builds assert every transition; see
 * graphics.c for the enforced order. Submit is never held across
 * recording/draws (only frame-end finalization + submits). */
void lc_device_lock_transfer(lc_device *device);
void lc_device_unlock_transfer(lc_device *device);
void lc_device_lock_state(lc_device *device);
void lc_device_unlock_state(lc_device *device);
void lc_device_lock_cache(lc_device *device);
void lc_device_unlock_cache(lc_device *device);
void lc_device_lock_desc(lc_device *device);
void lc_device_unlock_desc(lc_device *device);
void lc_device_lock_submit(lc_device *device);
void lc_device_unlock_submit(lc_device *device);
/* Nonzero when the calling thread holds the submit shard (pool
 * domain assertions). */
int lc_device_submit_held(void);

/* Timeline query without waiting (0 when unsupported/uncreated). */
uint64_t lc_vk_timeline_counter(lc_device *device);
/* Next submission value (reserves and returns timeline_next++ in
 * timeline mode; device counter otherwise). */
uint64_t lc_vk_signal_reserve(lc_device *device);
/* Wait for a value (timeline wait or fence fallback emulation). */
lc_result lc_vk_signal_wait(lc_device *device, uint64_t value,
                            uint64_t timeout_ns);
/* Non-blocking completion test for a value. */
int lc_vk_signal_ready(lc_device *device, uint64_t value);
/* Retire a Vk-level resource set for later reclamation. */
void lc_vk_retire(lc_device *device, const lc_retire_entry *entry);
/* Bind resources destroyed during an open frame to its newly reserved
 * completion value before submission. */
void lc_vk_retire_bind_active_frame(lc_device *device, uint64_t value);
/* Reclaim due staging + retirements (no blocking). */
void lc_vk_reclaim_completed(lc_device *device);
/* Flush ALL pending retirements (waits; shutdown path only). */
void lc_vk_retire_flush_all(lc_device *device);
/* Emit graphics acquires for un-acquired in-flight transfers into
 * an open frame command buffer (frame end, before end-cmd). Appends
 * each entry's copy semaphore to the caller's wait arrays (bounded
 * by cap; *inout_count updated). Timeline mode needs no semaphore
 * waits (the frame's max-value wait orders the barriers), so the
 * arrays stay untouched there but barriers are still recorded. */
lc_result lc_vk_emit_pending_acquires(lc_device *device,
                                      VkCommandBuffer cmd,
                                      VkSemaphore *wait_sems,
                                      VkPipelineStageFlags *wait_stages,
                                      uint32_t *inout_count, uint32_t cap);
/* Max in-flight transfer timeline value (0 when none) for frame
 * submit waits. */
uint64_t lc_vk_transfer_max_inflight(lc_device *device);
/* Wait (CPU) for in-flight transfers touching one resource, then
 * drop those entries' refs (destroy path). */
void lc_vk_transfer_wait_for(lc_device *device, const lc_image *image,
                             const lc_buffer *buffer);
/* Async schedule entry points (vulkan_transfer.c): never block on
 * the GPU (staging pressure waits for the oldest completion only).
 * Uploads return the completion value (0 = already complete);
 * readbacks return an owned request. */
lc_result lc_vk_transfer_upload_buffer(lc_device *device, lc_buffer *dst,
                                       uint64_t dst_offset,
                                       const void *data, uint64_t size,
                                       uint64_t *out_value);
lc_result lc_vk_transfer_upload_image(
    lc_device *device, lc_image *dst, uint32_t mip_level,
    uint32_t array_layer, uint32_t width, uint32_t height,
    uint32_t depth, const void *data, uint64_t data_size,
    uint64_t *out_value);
lc_result lc_vk_transfer_readback_image(lc_device *device, lc_image *image,
                                        uint32_t mip_level,
                                        uint32_t array_layer,
                                        lc_readback_request **out_request);
/* Free every live readback request (shutdown path only; the device
 * is idle). */
void lc_vk_requests_flush_all(lc_device *device);
/* Destroy one request after its own completion (public destroy). */
void lc_vk_request_discard(lc_device *device, lc_readback_request *req);
/* Transfer engine shutdown (pools, locks, flushed retirements). */
void lc_vk_transfer_shutdown(lc_device *device);
/* Command-list registry + validation. */
void lc_vk_cmdlist_register(lc_device *device, lc_cmdlist_record *rec);
void lc_vk_cmdlist_unregister(lc_device *device,
                              lc_cmdlist_record *rec);
void lc_vk_cmdlist_poison_for(lc_device *device, const void *handle);
void lc_vk_cmdlist_bind_active_frame(lc_device *device, uint64_t value);
void lc_vk_cmdlist_reclaim_completed(lc_device *device);
/* Binary-fallback fence binding + flight release (PART F). */
void lc_vk_cmdlist_bind_fallback_fence(lc_device *device,
                                       VkFence fence);
void lc_vk_cmdlist_release_flight(lc_device *device,
                                  const VkFence *fences,
                                  uint32_t fence_count);
void lc_vk_worker_shutdown(lc_device *device);
/* Worker liveness. */
int lc_vk_worker_live(const lc_command_encoder *enc);
/* Worker backend entry points (vulkan_worker.c). */
lc_result lc_vulkan_worker_create(lc_device *device, lc_queue_type queue,
                                  lc_command_encoder **out);
void lc_vulkan_worker_destroy(lc_command_encoder *enc);
lc_result lc_vulkan_worker_begin(lc_command_encoder *enc,
                                 lc_render_target *target,
                                 const lc_render_pass_desc *desc);
lc_result lc_vulkan_worker_finish(lc_command_encoder *enc,
                                  lc_command_list **out_list);
void lc_vulkan_worker_list_destroy(lc_command_list *list);
lc_result lc_vulkan_worker_execute(lc_command_encoder *primary,
                                   lc_command_list *const *lists,
                                   uint32_t list_count,
                                   VkCommandBuffer primary_cmd);
lc_result lc_worker_record_bind_pipeline(lc_command_encoder *enc,
                                         const lc_pipeline *pipeline);
lc_result lc_worker_record_bind_set(lc_command_encoder *enc,
                                    const lc_pipeline *pipeline,
                                    uint32_t slot, lc_binding_set *set);
lc_result lc_worker_record_bind_vertex(lc_command_encoder *enc,
                                       uint32_t binding, const lc_buffer *buffer,
                                       uint64_t offset);
lc_result lc_worker_record_bind_index(lc_command_encoder *enc,
                                      const lc_buffer *buffer, uint64_t offset,
                                      lc_index_type index_type);
lc_result lc_worker_record_push(lc_command_encoder *enc,
                                const lc_pipeline *pipeline,
                                uint32_t visibility, uint32_t offset,
                                uint32_t size, const void *data);
lc_result lc_worker_record_draw(lc_command_encoder *enc,
                                uint32_t vertex_count,
                                uint32_t first_vertex);
lc_result lc_worker_record_draw_indexed(
    lc_command_encoder *enc, uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, int32_t vertex_offset, uint32_t first_instance);
lc_result lc_worker_record_draw_instanced(
    lc_command_encoder *enc, uint32_t vertex_count, uint32_t instance_count,
    uint32_t first_vertex, uint32_t first_instance);
lc_result lc_worker_record_transition(
    lc_command_encoder *enc, lc_image *image, uint32_t base_mip,
    uint32_t level_count, uint32_t base_layer, uint32_t layer_count,
    lc_resource_state new_state);
/* Compute recording into worker lists (vulkan_worker.c). */
lc_result lc_worker_record_bind_compute_pipeline(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline);
lc_result lc_worker_record_dispatch(lc_command_encoder *enc, uint32_t x,
                                    uint32_t y, uint32_t z);
lc_result lc_worker_record_draw_indirect(lc_command_encoder *enc,
                                         const lc_buffer *buffer,
                                         uint64_t offset,
                                         uint32_t draw_count,
                                         uint32_t stride);
lc_result lc_worker_record_draw_indexed_indirect(
    lc_command_encoder *enc, const lc_buffer *buffer, uint64_t offset,
    uint32_t draw_count, uint32_t stride);
lc_result lc_worker_record_push_compute(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline,
    uint32_t visibility, uint32_t offset, uint32_t size,
    const void *data);
lc_result lc_worker_record_transition_buffer(
    lc_command_encoder *enc, lc_buffer *buffer,
    lc_resource_state new_state);
/* Compute worker-list lifecycle (vulkan_worker.c). */
lc_result lc_vulkan_worker_begin_compute(lc_command_encoder *enc);
lc_result lc_vulkan_worker_finish_compute(lc_command_encoder *enc,
                                          lc_command_list **out_list);
/* Frame compute/indirect entry points (vulkan_encoder.c). */
lc_result lc_vulkan_encoder_bind_compute_pipeline(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline);
lc_result lc_vulkan_encoder_dispatch(lc_command_encoder *enc, uint32_t x,
                                     uint32_t y, uint32_t z);
lc_result lc_vulkan_encoder_push_compute(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline,
    uint32_t visibility, uint32_t offset, uint32_t size,
    const void *data);
lc_result lc_vk_indirect_batch_valid(const lc_buffer *buffer,
                                     uint64_t offset, uint32_t draw_count,
                                     uint32_t stride, uint32_t elem_size);
lc_result lc_vulkan_encoder_draw_indirect(lc_command_encoder *enc,
                                          lc_buffer *buffer, uint64_t offset,
                                          uint32_t draw_count,
                                          uint32_t stride);
lc_result lc_vulkan_encoder_draw_indexed_indirect(
    lc_command_encoder *enc, lc_buffer *buffer, uint64_t offset,
    uint32_t draw_count, uint32_t stride);
lc_result lc_vulkan_encoder_transition_buffer(lc_command_encoder *enc,
                                              lc_buffer *buffer,
                                              lc_resource_state new_state);
lc_result lc_vulkan_encoder_bind_compute_set(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline,
    uint32_t slot, lc_binding_set *set);
lc_result lc_worker_record_bind_compute_set(
    lc_command_encoder *enc, const lc_compute_pipeline *pipeline,
    uint32_t slot, lc_binding_set *set);
/* Compute worker-list lifecycle (vulkan_worker.c). */
lc_result lc_vulkan_worker_begin_compute(lc_command_encoder *enc);
lc_result lc_vulkan_worker_finish_compute(lc_command_encoder *enc,
                                          lc_command_list **out_list);

/* Buffer state tracking (vulkan_sync.c, Phase 21 PART J):
 * whole-resource semantic states for compute/indirect ordering.
 * UNDEFINED is source-only (first use, like images). */
int lc_vk_sync_state_valid_for_buffer(lc_resource_state state);
int lc_vk_sync_buffer_barrier_params(lc_resource_state state,
                                     VkPipelineStageFlags *out_stage,
                                     VkAccessFlags *out_access);
/* Record a buffer memory barrier with explicit families (transfer
 * shard may pass a dedicated compute family later; graphics paths
 * use IGNORED). */
void lc_vk_sync_record_buffer(VkCommandBuffer cmd, const lc_buffer *buffer,
                              uint64_t offset, uint64_t size,
                              VkPipelineStageFlags src_stage,
                              VkAccessFlags src_access,
                              VkPipelineStageFlags dst_stage,
                              VkAccessFlags dst_access,
                              uint32_t src_family, uint32_t dst_family);
/* Mark a buffer's tracked state (transfer shard, held by caller —
 * buffer_state lives in the transfer-owned tracking domain with
 * owners/xfer_value, never nested). */
void lc_vk_sync_mark_buffer(lc_buffer *buffer, lc_resource_state state);
/* Transition helper: barrier from tracked state into cmd + mark.
 * Rejects INVALID states and dead handles loudly. Transfer shard
 * held by the caller. */
lc_result lc_vulkan_buffer_transition(VkCommandBuffer cmd, lc_buffer *buffer,
                                      lc_resource_state new_state);
/* Registry helpers for compute pipelines (compute_pipeline.c). */
void lc_compute_pipeline_list_add(lc_compute_pipeline *pipeline);
void lc_compute_pipeline_list_remove(lc_compute_pipeline *pipeline);
void lc_compute_pipeline_destroy_all(void);
void lc_compute_pipeline_destroy_for_device(const lc_device *device);
int lc_compute_pipeline_is_live(const lc_compute_pipeline *pipeline);
/* Isolated cross-queue compute submit, test path only
 * (vulkan_compute.c). Fire-and-forget in timeline mode (returns
 * the signaled value); synchronous in fallback (returns 0). */
lc_result lc_vk_compute_dispatch_once(
    lc_device *device, lc_compute_pipeline *pipeline,
    lc_binding_set *const *sets, const uint32_t *slots,
    uint32_t set_count, const void *push_data, uint32_t push_size,
    uint32_t x, uint32_t y, uint32_t z, uint64_t *out_value);
/* Drain once-submits + destroy the compute pool (shutdown path). */
void lc_vk_compute_pool_shutdown(lc_device *device);
/* Frame compute/indirect entry points (vulkan_encoder.c). */
#endif /* LUMAC_GRAPHICS_INTERNAL_H */
