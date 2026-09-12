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
#endif

#include <vulkan/vulkan.h>

/* GPUs without any rating info still get a name buffer this large.
 * VK_MAX_PHYSICAL_DEVICE_NAME_SIZE is 256. */
#define LC_DEVICE_NAME_SIZE 256

/* One retrieved queue: families are enumerated once at device creation
 * (see Approach C discussion in vulkan_backend.c) so any family,
 * including a present-only family discovered later, has a queue ready
 * without recreating the logical device. */
typedef struct lc_vk_queue {
    uint32_t family_index;
    VkQueue queue;
} lc_vk_queue;

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
};

/* Opaque public buffer type, completed here. A buffer belongs to one
 * device and survives swapchain recreation; device teardown destroys
 * dependent buffers before VkDevice. CPU-visible memory is mapped
 * persistently at creation (coherent required); GPU-only memory is
 * never mapped and is written through staging. */
struct lc_buffer {
    lc_device *device;

    uint64_t size;
    uint32_t usage; /* lc_buffer_usage bits, as requested */
    lc_memory_usage memory_usage;

    VkBuffer vk_buffer;
    VkDeviceMemory vk_memory;
    void *mapped_ptr; /* persistent mapping, or NULL when not mappable */

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
 * enable_validation != 0 requests validation layers (best-effort).
 * On failure, tears down whatever stage was reached and returns an
 * lc_result; the caller still owns (and frees) the struct.
 */
lc_result lc_vulkan_device_create(lc_device *device, int enable_validation);

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
} lc_vk_flight;

/* Opaque public swapchain type, completed here. A swapchain borrows its
 * device and surface; both must outlive it. Destruction order is
 * enforced by hooks: surface/device/window teardown and lc_shutdown()
 * destroy dependent swapchains first (see swapchain.c). Images are owned
 * by VkSwapchainKHR (never destroyed directly); views are owned here.
 * Frame state (pool, slots, per-image tracking) is swapchain-local, so
 * two swapchains run fully independent frame loops. */
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
    /* Render scope (Phase 7): one minimal render pass (clear/store,
     * UNDEFINED-to-present) plus one framebuffer per image view,
     * rebuilt with the swapchain. The pass begins lazily on first
     * clear/bind/draw so the clear color is always known. */
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

    /* Frame lifecycle (Phase 6). Pool/buffers/sync persist across
     * recreates; per-image tracking is rebuilt with the images. */
    VkCommandPool cmd_pool;
    lc_vk_flight flights[LC_MAX_FRAMES_IN_FLIGHT];
    uint32_t current_frame; /* next flight slot, not an image index */
    VkFence *images_in_flight; /* per image: fence to wait before reuse */
    uint32_t current_image; /* acquired index, valid only mid-frame */
    int frame_active; /* exactly one open frame per swapchain max */
    int frame_suboptimal; /* acquire reported SUBOPTIMAL this frame */

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

/* Maximum entry-point name stored per shader (including NUL). */
#define LC_SHADER_ENTRY_MAX 64

/* Opaque public shader type, completed here. A shader belongs to one
 * device; pipeline creation consumes only the module, so shaders may
 * die while their pipelines live on. */
struct lc_shader {
    lc_device *device;
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

/* Opaque public pipeline type, completed here. A pipeline is created
 * against one swapchain's color format (same fixed render-pass recipe
 * everywhere, so format equality implies compatibility) but may be
 * bound on any same-device, same-format swapchain. It is destroyed
 * with its creation swapchain or its device, whichever goes first.
 * Binding-layout anchors (non-owning, malloc'd array) identify the
 * resource slots; bind-time checks compare anchors, never dereference
 * dead layouts. */
struct lc_pipeline {
    lc_device *device;
    lc_swapchain *swapchain; /* creation anchor for lifetime tracking */
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkFormat format; /* must equal the target swapchain's format */
    const lc_binding_layout **layouts; /* slot anchors, malloc'd (maybe NULL) */
    uint32_t layout_count;
    lc_pipeline *next;
    lc_pipeline *prev;
};

/*
 * Validate shaders (live, correctly staged, same device) and the vertex
 * layout, then create the pipeline layout plus graphics pipeline
 * against the swapchain's render pass. Consumes only module handles:
 * shaders may be destroyed afterwards. On failure tears down partial
 * state.
 */
lc_result lc_vulkan_pipeline_create(lc_pipeline *pipeline, lc_device *device,
                                    lc_swapchain *swapchain,
                                    const lc_graphics_pipeline_desc *desc);

/* Destroys pipeline then layout. Device must still be alive. */
void lc_vulkan_pipeline_destroy(lc_pipeline *pipeline);

/* Centralized backend-neutral <-> Vulkan format translation. Unknown
 * inputs map to VK_FORMAT_UNDEFINED / LC_FORMAT_UNDEFINED. */
VkFormat lc_vulkan_translate_format(lc_format format);
lc_format lc_vulkan_untranslate_format(VkFormat format);

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
    VkDeviceMemory vk_memory;
    VkImageView default_view;

    /* Per-subresource layout tracking: entry [layer * mip_levels + mip]
     * for every mip of every layer (malloc'd). Uploads, mip generation,
     * and transitions keep each entry truthful, so mixed states (e.g.
     * mid-mipmap-dance) are representable and the next transition
     * always names a correct old layout. */
    VkImageLayout *layouts;

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
 * Transition the whole image to a new layout on the upload context
 * (immediate submit, one barrier per subresource). Each entry moves
 * from its tracked layout with stage/access masks suited to the
 * endpoints; `visibility` selects stages for SHADER_READ endpoints
 * (0 defaults to fragment). Unknown endpoints fail rather than emit
 * invalid barriers. Tracked state follows only on success.
 */
lc_result lc_vulkan_image_transition(lc_image *image, VkImageLayout new_layout,
                                     uint32_t visibility);

/*
 * Transition an explicit mip/layer range from an explicit old layout
 * (one barrier, immediate submit). Used by mip generation, where
 * levels temporarily diverge. The range's tracked entries must all
 * equal old_layout; they are updated only on success.
 */
lc_result lc_vulkan_image_transition_range(
    lc_image *image, uint32_t base_mip, uint32_t level_count,
    uint32_t base_layer, uint32_t layer_count, VkImageLayout old_layout,
    VkImageLayout new_layout, uint32_t visibility);

/* Opaque public image-view type, completed here. A view borrows its
 * image (and thereby its device); image teardown destroys dependent
 * views first. Views carry no layout state themselves. */
struct lc_image_view {
    lc_device *device;
    lc_image *image;
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

/* Copy one mip/layer region into a buffer, leaving the image
 * sampled-readable. Exported (but not in the public header) so
 * white-box integration tests can verify round-trips exactly; not
 * part of the API. */
LC_API lc_result lc_vulkan_copy_image_to_buffer(
    lc_device *device, lc_image *image, uint32_t mip_level,
    uint32_t array_layer, uint32_t width, uint32_t height, uint32_t depth,
    VkBuffer dst, uint64_t dst_offset);

/* Opaque public sampler type, completed here. Device-owned, fully
 * independent of images (no bindings exist yet). */
struct lc_sampler {
    lc_device *device;
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

#endif /* LUMAC_GRAPHICS_INTERNAL_H */
