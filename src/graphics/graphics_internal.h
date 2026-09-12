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
 * with its creation swapchain or its device, whichever goes first. */
struct lc_pipeline {
    lc_device *device;
    lc_swapchain *swapchain; /* creation anchor for lifetime tracking */
    VkPipelineLayout layout;
    VkPipeline pipeline;
    VkFormat format; /* must equal the target swapchain's format */
    lc_pipeline *next;
    lc_pipeline *prev;
};

/*
 * Validate shaders (live, correctly staged, same device) and create
 * the pipeline layout plus graphics pipeline against the swapchain's
 * render pass. Consumes only module handles: shaders may be destroyed
 * afterwards. On failure tears down partial state.
 */
lc_result lc_vulkan_pipeline_create(lc_pipeline *pipeline, lc_device *device,
                                    lc_swapchain *swapchain,
                                    const lc_shader *vertex_shader,
                                    const lc_shader *fragment_shader);

/* Destroys pipeline then layout. Device must still be alive. */
void lc_vulkan_pipeline_destroy(lc_pipeline *pipeline);

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

#endif /* LUMAC_GRAPHICS_INTERNAL_H */
