#ifndef LUMAC_H
#define LUMAC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Symbol visibility
 * ------------------------------------------------------------------------- */
#if defined(_WIN32) || defined(_WIN64)
    #if defined(LUMAC_BUILD_SHARED)
        #if defined(LUMAC_EXPORTS)
            #define LC_API __declspec(dllexport)
        #else
            #define LC_API __declspec(dllimport)
        #endif
    #else
        #define LC_API
    #endif
#else
    #if defined(__GNUC__) && __GNUC__ >= 4
        #define LC_API __attribute__((visibility("default")))
    #else
        #define LC_API
    #endif
#endif

/* -------------------------------------------------------------------------
 * Version
 * ------------------------------------------------------------------------- */
#define LC_VERSION_MAJOR 0
#define LC_VERSION_MINOR 1
#define LC_VERSION_PATCH 0

/* Stringified version for compile-time use */
#define LC_VERSION_STRING "0.1.0"

/* -------------------------------------------------------------------------
 * Result codes
 * ------------------------------------------------------------------------- */
typedef enum lc_result {
    LC_SUCCESS = 0,
    LC_ERROR_UNKNOWN = 1,
    LC_ERROR_INVALID_ARGUMENT = 2,
    LC_ERROR_NOT_INITIALIZED = 3,
    LC_ERROR_ALREADY_INITIALIZED = 4,
    LC_ERROR_OUT_OF_MEMORY = 5,
    LC_ERROR_PLATFORM = 6,
    LC_ERROR_WINDOW_CREATION_FAILED = 7,
    LC_ERROR_BACKEND_UNAVAILABLE = 8,
    LC_ERROR_NO_SUPPORTED_DEVICE = 9,
    LC_ERROR_DEVICE_CREATION_FAILED = 10,
    LC_ERROR_SURFACE_UNSUPPORTED = 11,
    LC_ERROR_SWAPCHAIN_UNSUPPORTED = 12,
    LC_ERROR_SWAPCHAIN_CREATION_FAILED = 13,
    LC_ERROR_ZERO_EXTENT = 14,
    LC_ERROR_SWAPCHAIN_OUT_OF_DATE = 15,
    /* Non-error status: the frame completed, but the swapchain is
     * suboptimal and recreation is recommended. */
    LC_SUBOPTIMAL = 16,
    LC_ERROR_SHADER_CREATION_FAILED = 17,
    LC_ERROR_PIPELINE_CREATION_FAILED = 18,
    LC_ERROR_PIPELINE_INCOMPATIBLE = 19,
    LC_ERROR_IMAGE_CREATION_FAILED = 20,
    LC_ERROR_SAMPLER_CREATION_FAILED = 21,
    /* Recoverable capability gap (e.g. format lacks linear blit
     * support, anisotropy unavailable): valid request, valid device,
     * unsupported combination. Not fatal. */
    LC_ERROR_UNSUPPORTED = 22
} lc_result;

/* -------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------- */

/**
 * Initialize LumaC.
 *
 * @return LC_SUCCESS on first successful initialization.
 * @return LC_ERROR_ALREADY_INITIALIZED if already initialized without shutdown.
 */
LC_API lc_result lc_init(void);

/**
 * Shutdown LumaC and reset initialization state.
 * Safe to call when not initialized; does not crash.
 */
LC_API void lc_shutdown(void);

/**
 * Get LumaC version string.
 *
 * @return Null-terminated static string "0.1.0". Never returns NULL.
 */
LC_API const char *lc_get_version_string(void);

/* -------------------------------------------------------------------------
 * Window API (Phase 2: native platform window, no rendering)
 *
 * Threading: use only from the application's main thread.
 * Dimensions are the drawable/client area in pixels (not the decorated
 * outer window size), since future rendering will use the client area.
 * ------------------------------------------------------------------------- */

/* Opaque native window handle. Never dereference; use API below. */
typedef struct lc_window lc_window;

/* Window creation parameters. Title is UTF-8; NULL means "LumaC". */
typedef struct lc_window_desc {
    const char *title;
    uint32_t width;
    uint32_t height;
} lc_window_desc;

/**
 * Create a native OS window. Requires lc_init() first.
 *
 * @param desc Window parameters (title may be NULL for default).
 * @param out_window Receives the new window on success (caller-owned).
 * @return LC_SUCCESS on success.
 * @return LC_ERROR_NOT_INITIALIZED if LumaC is not initialized.
 * @return LC_ERROR_INVALID_ARGUMENT for NULL desc/out or zero width/height.
 * @return LC_ERROR_OUT_OF_MEMORY on allocation failure.
 * @return LC_ERROR_PLATFORM / LC_ERROR_WINDOW_CREATION_FAILED on OS failure.
 */
LC_API lc_result lc_window_create(
    const lc_window_desc *desc,
    lc_window **out_window
);

/**
 * Destroy a window and release its OS resources.
 * Safe to call with NULL. Removes the window from shutdown tracking.
 */
LC_API void lc_window_destroy(lc_window *window);

/**
 * Process pending OS events (non-blocking, returns immediately).
 * Safe to call with no windows; no-op when not initialized.
 */
LC_API void lc_poll_events(void);

/**
 * Check whether the user requested window close (e.g. close button).
 * @return Non-zero if close requested (or window is NULL), 0 otherwise.
 */
LC_API int lc_window_should_close(const lc_window *window);

/**
 * Get current drawable/client-area width in pixels. Returns 0 for NULL.
 */
LC_API uint32_t lc_window_get_width(const lc_window *window);

/**
 * Get current drawable/client-area height in pixels. Returns 0 for NULL.
 */
LC_API uint32_t lc_window_get_height(const lc_window *window);

/* -------------------------------------------------------------------------
 * Graphics device API (Phase 3: Vulkan device foundation, no rendering)
 *
 * Threading: use only from the application's main thread.
 * A device is independent of any window; lc_device_create() needs only
 * lc_init(). No surfaces, swapchains, or drawing exist yet.
 * ------------------------------------------------------------------------- */

/* Opaque graphics device handle. Never dereference; use API below. */
typedef struct lc_device lc_device;

/* Only backends actually implemented appear here. */
typedef enum lc_backend {
    LC_BACKEND_VULKAN = 1
} lc_backend;

/* Device creation parameters. enable_validation != 0 requests Vulkan
 * validation layers; if they are unavailable, creation continues without
 * validation after a brief stderr notice (best-effort diagnostics). */
typedef struct lc_device_desc {
    lc_backend backend;
    int enable_validation;
} lc_device_desc;

/**
 * Create a graphics device. Requires lc_init() first.
 *
 * @param desc Creation parameters.
 * @param out_device Receives the new device on success (caller-owned).
 *        Set to NULL on failure.
 * @return LC_SUCCESS on success.
 * @return LC_ERROR_NOT_INITIALIZED if LumaC is not initialized.
 * @return LC_ERROR_INVALID_ARGUMENT for NULL desc/out or unknown backend.
 * @return LC_ERROR_OUT_OF_MEMORY on allocation failure.
 * @return LC_ERROR_BACKEND_UNAVAILABLE if the backend cannot initialize.
 * @return LC_ERROR_NO_SUPPORTED_DEVICE if no usable GPU was found (a
 *         usable GPU needs a graphics queue family and VK_KHR_swapchain
 *         support so future presentation is predictable).
 * @return LC_ERROR_DEVICE_CREATION_FAILED on logical-device failure.
 */
LC_API lc_result lc_device_create(
    const lc_device_desc *desc,
    lc_device **out_device
);

/**
 * Destroy a device and release its graphics resources.
 * Safe to call with NULL. Removes the device from shutdown tracking.
 */
LC_API void lc_device_destroy(lc_device *device);

/**
 * Get the GPU name (e.g. "NVIDIA GeForce RTX 5060").
 * Valid for the lifetime of the device. Returns NULL for NULL device.
 */
LC_API const char *lc_device_get_name(const lc_device *device);

/**
 * Get the backend this device was created with.
 * Returns 0 (no valid backend) for NULL device.
 */
LC_API lc_backend lc_device_get_backend(const lc_device *device);

/**
 * Get the PCI vendor ID of the GPU. Returns 0 for NULL device.
 */
LC_API uint32_t lc_device_get_vendor_id(const lc_device *device);

/**
 * Get the PCI device ID of the GPU. Returns 0 for NULL device.
 */
LC_API uint32_t lc_device_get_device_id(const lc_device *device);

/* -------------------------------------------------------------------------
 * Surface API (Phase 4: Vulkan presentation foundation, no swapchain)
 *
 * A surface connects one lc_device with one lc_window for presentation.
 * It is NOT a swapchain, framebuffer, or render target.
 * Threading: use only from the application's main thread.
 *
 * Lifetime: a surface needs both its device and its window alive.
 * Destroying a device or window first automatically destroys its
 * dependent surfaces; lc_shutdown() destroys surfaces, then devices,
 * then windows.
 * ------------------------------------------------------------------------- */

/* Opaque presentation surface handle. Never dereference; use API below. */
typedef struct lc_surface lc_surface;

/**
 * Create a presentation surface linking a device and a window.
 * Requires lc_init() first. The device and window must both be live;
 * both must outlive the surface (or be destroyed through LumaC, which
 * cleans up dependent surfaces automatically).
 *
 * @param device Live graphics device (must support surface creation).
 * @param window Live native window.
 * @param out_surface Receives the new surface on success (caller-owned).
 *        Set to NULL on failure.
 * @return LC_SUCCESS on success.
 * @return LC_ERROR_NOT_INITIALIZED if LumaC is not initialized.
 * @return LC_ERROR_INVALID_ARGUMENT for NULL device/window/out or for
 *         device/window handles that are no longer live.
 * @return LC_ERROR_OUT_OF_MEMORY on allocation failure.
 * @return LC_ERROR_SURFACE_UNSUPPORTED if no queue family can present
 *         to the window or the surface cannot be established.
 */
LC_API lc_result lc_surface_create(
    lc_device *device,
    lc_window *window,
    lc_surface **out_surface
);

/**
 * Destroy a surface and release its presentation resources.
 * Safe to call with NULL. Removes the surface from shutdown tracking.
 */
LC_API void lc_surface_destroy(lc_surface *surface);

/**
 * Check whether the surface can present (a present-capable queue family
 * was found at creation). Returns 0 for NULL surface.
 */
LC_API int lc_surface_is_present_supported(const lc_surface *surface);

/* -------------------------------------------------------------------------
 * Swapchain API (Phase 5: Vulkan swapchain infrastructure, no rendering)
 *
 * A swapchain owns the presentable images (and one 2D color image view
 * per image) for one lc_surface. It is NOT a renderer: no image is ever
 * acquired or presented by this API; that belongs to a later phase.
 * Threading: use only from the application's main thread.
 *
 * Lifetime: a swapchain needs its device and surface alive. At most one
 * live swapchain may exist per surface; destroying a surface or device
 * first automatically destroys dependent swapchains. lc_shutdown()
 * destroys swapchains, then surfaces, devices, windows.
 * ------------------------------------------------------------------------- */

/* Opaque swapchain handle. Never dereference; use API below. */
typedef struct lc_swapchain lc_swapchain;

/* Swapchain creation parameters. image_count == 0 selects an automatic
 * count derived from the surface capabilities. vsync != 0 prefers the
 * guaranteed FIFO present mode; vsync == 0 prefers low-latency modes. */
typedef struct lc_swapchain_desc {
    uint32_t width;
    uint32_t height;
    uint32_t image_count;
    int vsync;
} lc_swapchain_desc;

/**
 * Create a swapchain (with image views) for a device/surface pair.
 * Requires lc_init() first. The device and surface must both be live,
 * and the surface must have been created from the same device.
 *
 * @param device Live graphics device owning the surface.
 * @param surface Live presentation surface.
 * @param desc Requested dimensions and preferences (width/height > 0).
 * @param out_swapchain Receives the new swapchain on success
 *        (caller-owned). Set to NULL on failure.
 * @return LC_SUCCESS on success.
 * @return LC_ERROR_NOT_INITIALIZED if LumaC is not initialized.
 * @return LC_ERROR_INVALID_ARGUMENT for NULL device/surface/desc/out,
 *         zero width/height, dead handles, device/surface mismatch, or a
 *         second live swapchain for the same surface.
 * @return LC_ERROR_OUT_OF_MEMORY on allocation failure.
 * @return LC_ERROR_ZERO_EXTENT if the surface currently has a zero
 *         extent (e.g. minimized window); retry after resize.
 * @return LC_ERROR_SWAPCHAIN_UNSUPPORTED if the surface cannot support
 *         a swapchain (capabilities, usage, formats, or present modes).
 * @return LC_ERROR_SWAPCHAIN_CREATION_FAILED if VkSwapchainKHR or image
 *         view creation fails.
 */
LC_API lc_result lc_swapchain_create(
    lc_device *device,
    lc_surface *surface,
    const lc_swapchain_desc *desc,
    lc_swapchain **out_swapchain
);

/**
 * Destroy a swapchain, its image views, and its images' views.
 * (Swapchain images themselves are owned by VkSwapchainKHR.)
 * Safe to call with NULL. Removes the swapchain from tracking.
 */
LC_API void lc_swapchain_destroy(lc_swapchain *swapchain);

/**
 * Recreate a swapchain for new dimensions, transactionally: on success
 * the new swapchain replaces the old one (old resources destroyed); on
 * failure the existing swapchain remains intact.
 *
 * @param swapchain Live swapchain to recreate.
 * @param width New width (> 0).
 * @param height New height (> 0).
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL swapchain),
 *         LC_ERROR_ZERO_EXTENT (zero dimensions), or the same
 *         swapchain-creation failures as lc_swapchain_create.
 */
LC_API lc_result lc_swapchain_recreate(
    lc_swapchain *swapchain,
    uint32_t width,
    uint32_t height
);

/**
 * Get the actual swapchain extent width in pixels (capability-clamped,
 * may differ from the requested width). Returns 0 for NULL.
 */
LC_API uint32_t lc_swapchain_get_width(const lc_swapchain *swapchain);

/**
 * Get the actual swapchain extent height in pixels. Returns 0 for NULL.
 */
LC_API uint32_t lc_swapchain_get_height(const lc_swapchain *swapchain);

/**
 * Get the actual swapchain image (and image view) count.
 * Returns 0 for NULL.
 */
LC_API uint32_t lc_swapchain_get_image_count(const lc_swapchain *swapchain);

/* -------------------------------------------------------------------------
 * Frame API (Phase 6: acquire, clear, submit, present)
 *
 * Minimal per-swapchain frame lifecycle. A frame borrows its swapchain;
 * exactly one frame may be open per swapchain at a time. Intended loop:
 *
 *   while (!lc_window_should_close(window)) {
 *       lc_poll_events();
 *       width = lc_window_get_width(window);
 *       height = lc_window_get_height(window);
 *       if (width == 0 || height == 0) continue; // minimized
 *       result = lc_begin_frame(swapchain);
 *       if (result == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
 *           lc_swapchain_recreate(swapchain, width, height);
 *           continue;
 *       }
 *       if (result != LC_SUCCESS) break; // fatal
 *       lc_clear_color(swapchain, 0.08f, 0.12f, 0.20f, 1.0f);
 *       result = lc_end_frame(swapchain);
 *       if (result == LC_SUBOPTIMAL ||
 *           result == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
 *           lc_swapchain_recreate(swapchain, width, height);
 *       } else if (result != LC_SUCCESS) break; // fatal
 *   }
 *
 * Threading: use only from the application's main thread.
 * ------------------------------------------------------------------------- */

/**
 * Begin a frame: wait for a free in-flight slot, acquire a swapchain
 * image, and start command recording with the image transitioned for
 * clearing. Requires no currently open frame on this swapchain.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED (LumaC down),
 *         LC_ERROR_INVALID_ARGUMENT (NULL/dead swapchain, or a frame
 *         is already open), LC_ERROR_SWAPCHAIN_OUT_OF_DATE (recreate
 *         and retry; nothing was started), or a fatal swapchain error.
 */
LC_API lc_result lc_begin_frame(lc_swapchain *swapchain);

/**
 * Clear the current frame's image to a color. Components are clamped
 * to [0, 1]. Only valid between lc_begin_frame and lc_end_frame; may
 * be called multiple times (clears apply in order).
 *
 * @return LC_SUCCESS or LC_ERROR_INVALID_ARGUMENT (NULL/dead
 *         swapchain, or no open frame).
 */
LC_API lc_result lc_clear_color(
    lc_swapchain *swapchain,
    float r,
    float g,
    float b,
    float a
);

/**
 * End a frame: transition the image for presentation, submit recorded
 * commands, and present. Requires an open frame on this swapchain.
 *
 * @return LC_SUCCESS (presented), LC_SUBOPTIMAL (presented; recreation
 *         recommended), LC_ERROR_SWAPCHAIN_OUT_OF_DATE (recreate before
 *         the next frame), LC_ERROR_INVALID_ARGUMENT (NULL/dead
 *         swapchain, or no open frame), or a fatal error.
 */
LC_API lc_result lc_end_frame(lc_swapchain *swapchain);

/* -------------------------------------------------------------------------
 * Shader API (Phase 7: SPIR-V modules, no compilation)
 *
 * LumaC consumes SPIR-V bytecode (backend-neutral `void *` + size);
 * compiling GLSL/HLSL is the application's build-time job. A shader
 * belongs to one device and is independent after pipeline creation:
 * shaders may be destroyed once their pipeline exists.
 * Threading: use only from the application's main thread.
 * ------------------------------------------------------------------------- */

/* Opaque shader-module handle. Never dereference; use API below. */
typedef struct lc_shader lc_shader;

/* Backend-neutral shader stages. Only stages actually implemented. */
typedef enum lc_shader_stage {
    LC_SHADER_STAGE_VERTEX = 0,
    LC_SHADER_STAGE_FRAGMENT = 1
} lc_shader_stage;

/* Shader creation parameters. `code` points to SPIR-V words
 * (4-byte aligned, `code_size` a nonzero multiple of 4).
 * A NULL `entry_point` selects "main". */
typedef struct lc_shader_desc {
    lc_shader_stage stage;
    const void *code;
    size_t code_size;
    const char *entry_point;
} lc_shader_desc;

/**
 * Create a shader module on a device. Requires lc_init() first and a
 * live device.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL device/desc/code/out,
 *         zero or misaligned size, bad SPIR-V magic, unknown stage,
 *         overlong entry point, dead device),
 *         LC_ERROR_OUT_OF_MEMORY, LC_ERROR_SHADER_CREATION_FAILED.
 */
LC_API lc_result lc_shader_create(
    lc_device *device,
    const lc_shader_desc *desc,
    lc_shader **out_shader
);

/**
 * Destroy a shader module. Safe to call with NULL. Pipelines created
 * from it remain valid.
 */
LC_API void lc_shader_destroy(lc_shader *shader);

/* -------------------------------------------------------------------------
 * Graphics pipeline API (Phase 7: triangle drawing, no vertex buffers)
 *
 * A pipeline binds a vertex + fragment shader pair to a compatible
 * render target. Vertices come from the shader's vertex index (no
 * vertex buffers yet). Recording happens through the frame API:
 * lc_bind_pipeline() then lc_draw() inside an open frame.
 * ------------------------------------------------------------------------- */

/* Opaque graphics-pipeline handle. Never dereference; use API below. */
typedef struct lc_pipeline lc_pipeline;

/* -------------------------------------------------------------------------
 * Format system (Phase 8: backend-neutral GPU formats)
 *
 * Threading: use only from the application's main thread.
 * ------------------------------------------------------------------------- */

/* Backend-neutral GPU data formats for vertex attributes, color and
 * depth data, textures, and render targets. Names describe layout;
 * exact bit patterns follow the Vulkan convention (e.g. RGBA8 is R in
 * the lowest byte). LC_FORMAT_UNDEFINED means "no format". Compressed
 * (BC/ASTC/ETC) formats are future work; no API here assumes one
 * uncompressed texel. */
typedef enum lc_format {
    LC_FORMAT_UNDEFINED = 0,

    LC_FORMAT_R8_UNORM,
    LC_FORMAT_RG8_UNORM,

    LC_FORMAT_RGBA8_UNORM,
    LC_FORMAT_RGBA8_SRGB,
    LC_FORMAT_BGRA8_UNORM,
    LC_FORMAT_BGRA8_SRGB,

    LC_FORMAT_R16_FLOAT,
    LC_FORMAT_RG16_FLOAT,
    LC_FORMAT_RGBA16_FLOAT,

    LC_FORMAT_R32_FLOAT,
    LC_FORMAT_RG32_FLOAT,
    LC_FORMAT_RGB32_FLOAT,
    LC_FORMAT_RGBA32_FLOAT,

    LC_FORMAT_R32_UINT,
    LC_FORMAT_RG32_UINT,
    LC_FORMAT_RGB32_UINT,
    LC_FORMAT_RGBA32_UINT,

    /* Depth/stencil formats. Creatable as images today (allocation,
     * views, uploads); depth *rendering* arrives in a later phase. */
    LC_FORMAT_D16_UNORM,
    LC_FORMAT_D24_UNORM_S8_UINT,
    LC_FORMAT_D32_FLOAT
} lc_format;

/* Per-vertex or per-instance stepping. */
typedef enum lc_vertex_input_rate {
    LC_VERTEX_INPUT_PER_VERTEX = 0,
    LC_VERTEX_INPUT_PER_INSTANCE = 1
} lc_vertex_input_rate;

/* One vertex buffer binding: `stride` bytes per element. */
typedef struct lc_vertex_binding_desc {
    uint32_t binding;
    uint32_t stride;
    lc_vertex_input_rate input_rate;
} lc_vertex_binding_desc;

/* One shader input attribute fed from a binding. `offset` is the byte
 * offset of the attribute within each stride-sized element. */
typedef struct lc_vertex_attribute_desc {
    uint32_t location;
    uint32_t binding;
    lc_format format;
    uint32_t offset;
} lc_vertex_attribute_desc;

/* Minimal graphics-pipeline description. Both shaders are required.
 * Optional vertex input: zero counts mean no vertex buffers (shader
 * vertex-index generation, as in the first triangle). Counts are
 * validated against device limits at creation. Zero-initialize the
 * whole struct (e.g. `= { 0 }`) and set the fields you use. */
typedef struct lc_graphics_pipeline_desc {
    lc_shader *vertex_shader;
    lc_shader *fragment_shader;
    const lc_vertex_binding_desc *vertex_bindings;
    uint32_t vertex_binding_count;
    const lc_vertex_attribute_desc *vertex_attributes;
    uint32_t vertex_attribute_count;
} lc_graphics_pipeline_desc;

/**
 * Create a graphics pipeline for a swapchain's color format.
 * The shaders must be live, correctly staged, and owned by `device`;
 * they may be destroyed afterwards without affecting the pipeline.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL device/swapchain/desc/out,
 *         NULL/mis-staged/dead shaders, cross-device use, dead
 *         handles, invalid vertex layout), LC_ERROR_OUT_OF_MEMORY,
 *         LC_ERROR_PIPELINE_CREATION_FAILED.
 */
LC_API lc_result lc_graphics_pipeline_create(
    lc_device *device,
    lc_swapchain *swapchain,
    const lc_graphics_pipeline_desc *desc,
    lc_pipeline **out_pipeline
);

/**
 * Destroy a graphics pipeline. Safe to call with NULL.
 */
LC_API void lc_pipeline_destroy(lc_pipeline *pipeline);

/**
 * Bind a pipeline for subsequent lc_draw() calls. Only valid inside
 * an open frame. The pipeline must belong to the frame's device and
 * match the swapchain's current color format.
 *
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL/dead handles,
 *         no open frame, cross-device use),
 *         LC_ERROR_PIPELINE_INCOMPATIBLE (format drifted after a
 *         swapchain recreation; recreate the pipeline).
 */
LC_API lc_result lc_bind_pipeline(
    lc_swapchain *swapchain,
    lc_pipeline *pipeline
);

/**
 * Draw vertices generated by the bound shader (vertex index source,
 * no vertex buffers). Only valid inside an open frame with a bound
 * pipeline.
 *
 * @return LC_SUCCESS or LC_ERROR_INVALID_ARGUMENT (NULL/dead
 *         swapchain, no open frame, nothing bound).
 */
LC_API lc_result lc_draw(
    lc_swapchain *swapchain,
    uint32_t vertex_count,
    uint32_t first_vertex
);

/* -------------------------------------------------------------------------
 * Buffer API (Phase 8: generic GPU buffers)
 *
 * Buffers belong to one device and survive swapchain recreation.
 * Destroying a device first destroys its dependent buffers.
 * Threading: use only from the application's main thread.
 * ------------------------------------------------------------------------- */

/* Opaque GPU buffer handle. Never dereference; use API below. */
typedef struct lc_buffer lc_buffer;

/* Backend-neutral buffer usage flags (bitmask, combine with |). */
typedef enum lc_buffer_usage {
    LC_BUFFER_USAGE_VERTEX       = 1 << 0,
    LC_BUFFER_USAGE_INDEX        = 1 << 1,
    LC_BUFFER_USAGE_UNIFORM      = 1 << 2,
    LC_BUFFER_USAGE_STORAGE      = 1 << 3,
    LC_BUFFER_USAGE_TRANSFER_SRC = 1 << 4,
    LC_BUFFER_USAGE_TRANSFER_DST = 1 << 5
} lc_buffer_usage;

/* Memory placement model. No backend memory flags are exposed. */
typedef enum lc_memory_usage {
    /* Fastest device memory. Not CPU-mappable; written via
     * lc_buffer_write() staging. */
    LC_MEMORY_GPU_ONLY = 0,
    /* CPU-writable memory, persistently mapped. For uploads and
     * frequently updated resources. */
    LC_MEMORY_CPU_TO_GPU = 1,
    /* CPU-readable memory, persistently mapped. For readback. */
    LC_MEMORY_GPU_TO_CPU = 2
} lc_memory_usage;

/* Buffer creation parameters. `size` > 0. `usage` is a combination of
 * lc_buffer_usage bits (at least one bit required). GPU-only buffers
 * implicitly gain transfer-destination support so lc_buffer_write()
 * staging always works. */
typedef struct lc_buffer_desc {
    uint64_t size;
    uint32_t usage;
    lc_memory_usage memory;
} lc_buffer_desc;

/**
 * Create a GPU buffer on a device. Requires lc_init() first and a
 * live device.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL device/desc/out, zero size,
 *         empty/unknown usage, unknown memory usage, dead device),
 *         LC_ERROR_OUT_OF_MEMORY (host allocation or no suitable
 *         device memory).
 */
LC_API lc_result lc_buffer_create(
    lc_device *device,
    const lc_buffer_desc *desc,
    lc_buffer **out_buffer
);

/**
 * Destroy a buffer and release its memory. Safe to call with NULL.
 */
LC_API void lc_buffer_destroy(lc_buffer *buffer);

/**
 * Get the buffer size in bytes. Returns 0 for NULL.
 */
LC_API uint64_t lc_buffer_get_size(const lc_buffer *buffer);

/**
 * Map a CPU-visible buffer for direct access. Returns the same
 * persistently mapped pointer on every call; memory stays mapped
 * until the buffer is destroyed.
 *
 * @return LC_SUCCESS, or LC_ERROR_INVALID_ARGUMENT (NULL/dead buffer,
 *         NULL out pointer, or buffer is not CPU-mappable).
 */
LC_API lc_result lc_buffer_map(
    lc_buffer *buffer,
    void **out_data
);

/**
 * Unmap a buffer. Buffers use persistent mapping, so this is a
 * documented no-op kept for API symmetry and future backends.
 * Safe to call with NULL or on any buffer.
 */
LC_API void lc_buffer_unmap(lc_buffer *buffer);

/**
 * Write bytes into a buffer with bounds and overflow checking.
 * CPU-visible buffers memcpy directly; GPU-only buffers upload
 * through an internal staging buffer.
 *
 * @param buffer Live buffer.
 * @param offset Byte offset; must be within the buffer.
 * @param data Source bytes (may be NULL only when size == 0).
 * @param size Byte count; offset + size must fit the buffer.
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL/dead buffer,
 *         out-of-range write, NULL data with nonzero size),
 *         LC_ERROR_OUT_OF_MEMORY (staging allocation),
 *         LC_ERROR_UNKNOWN (transfer submission failure).
 */
LC_API lc_result lc_buffer_write(
    lc_buffer *buffer,
    uint64_t offset,
    const void *data,
    uint64_t size
);

/* -------------------------------------------------------------------------
 * Vertex input API (Phase 8: backend-neutral vertex layouts)
 * ------------------------------------------------------------------------- */

/**
 * Bind a vertex buffer for subsequent lc_draw() calls. Only valid
 * inside an open frame; bindings are command-buffer state and must be
 * re-established every frame. Call once per binding used.
 *
 * @return LC_SUCCESS or LC_ERROR_INVALID_ARGUMENT (NULL/dead
 *         swapchain or buffer, no open frame, cross-device use,
 *         buffer lacks VERTEX usage, offset past buffer end).
 */
LC_API lc_result lc_bind_vertex_buffer(
    lc_swapchain *swapchain,
    uint32_t binding,
    lc_buffer *buffer,
    uint64_t offset
);

/* -------------------------------------------------------------------------
 * Device capabilities (Phase 8: engine-oriented queries)
 * ------------------------------------------------------------------------- */

/* Small backend-neutral capability set for resource planning.
 * max_texture_2d_dimension also bounds 2D images. Anisotropy:
 * max_sampler_anisotropy is 1.0 when unsupported, otherwise the clamp
 * upper bound for lc_sampler_desc.max_anisotropy. */
typedef struct lc_device_limits {
    uint32_t max_texture_2d_dimension;
    uint32_t max_vertex_attributes;
    uint32_t max_vertex_bindings;
    uint64_t max_uniform_buffer_size;
    uint32_t max_image_array_layers;
    float max_sampler_anisotropy;
} lc_device_limits;

/**
 * Query device limits. A NULL device (or NULL out pointer handling)
 * yields zeros rather than crashing: with a NULL device, *out_limits
 * is zeroed if provided; a NULL out_limits is a no-op.
 */
LC_API void lc_device_get_limits(
    const lc_device *device,
    lc_device_limits *out_limits
);

/**
 * Get a swapchain's color format in backend-neutral form.
 * Returns LC_FORMAT_UNDEFINED for NULL.
 */
LC_API lc_format lc_swapchain_get_format(const lc_swapchain *swapchain);

/* -------------------------------------------------------------------------
 * Image API (Phase 9: texture/image resource foundation)
 *
 * An image owns GPU storage (dimensions, mips, layers, format); a
 * sampler (below) describes how it is sampled. The two are independent
 * objects by design. Images belong to one device, never to a
 * swapchain, so they survive swapchain recreation.
 * Threading: use only from the application's main thread.
 * ------------------------------------------------------------------------- */

/* Opaque GPU image handle. Never dereference; use API below. */
typedef struct lc_image lc_image;

/* Image dimensionality. Only 2D is heavily exercised yet, but the
 * architecture carries 1D/3D, mips, and layers from the start. */
typedef enum lc_image_type {
    LC_IMAGE_TYPE_1D = 0,
    LC_IMAGE_TYPE_2D = 1,
    LC_IMAGE_TYPE_3D = 2
} lc_image_type;

/* Backend-neutral image usage flags (bitmask, combine with |). */
typedef enum lc_image_usage {
    LC_IMAGE_USAGE_SAMPLED          = 1 << 0,
    LC_IMAGE_USAGE_STORAGE          = 1 << 1,
    LC_IMAGE_USAGE_COLOR_ATTACHMENT = 1 << 2,
    LC_IMAGE_USAGE_DEPTH_STENCIL    = 1 << 3,
    LC_IMAGE_USAGE_TRANSFER_SRC     = 1 << 4,
    LC_IMAGE_USAGE_TRANSFER_DST     = 1 << 5
} lc_image_usage;

/* Optional image flags (bitmask). */
typedef enum lc_image_flags {
    LC_IMAGE_FLAG_NONE = 0,
    /* 2D image with a layer count that is a multiple of 6, created so
     * each consecutive 6-layer group can become a cubemap view later.
     * No cubemap rendering exists yet; this reserves the structure. */
    LC_IMAGE_FLAG_CUBE_COMPATIBLE = 1 << 0
} lc_image_flags;

/* Number of samples per texel (multisampling). Only 1 is exercised
 * yet; the enum exists so the descriptor never needs reshaping when
 * MSAA work begins. */
typedef enum lc_sample_count {
    LC_SAMPLE_COUNT_1 = 1,
    LC_SAMPLE_COUNT_2 = 2,
    LC_SAMPLE_COUNT_4 = 4,
    LC_SAMPLE_COUNT_8 = 8
} lc_sample_count;

/* Image creation parameters. Dimension semantics:
 *   1D: width > 0, height/depth conceptually 1;
 *   2D: width > 0, height > 0, depth conceptually 1;
 *   3D: width/height/depth > 0 and array_layers == 1.
 * mip_levels >= 1, or 0 for the full chain
 * (floor(log2(max dimension))) + 1. array_layers >= 1; cube-compatible
 * images need a multiple of 6. `usage` needs at least one known bit.
 * `flags` accepts lc_image_flags bits (0 for none). */
typedef struct lc_image_desc {
    lc_image_type type;
    lc_format format;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mip_levels;
    uint32_t array_layers;
    uint32_t usage;
    uint32_t flags;
    lc_sample_count samples;
} lc_image_desc;

/**
 * Create a GPU image on a device. Requires lc_init() first and a
 * live device. The image carries a default full-resource view.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL device/desc/out, bad
 *         dimensions for the type, UNDEFINED format, empty/unknown
 *         usage, bad mip/layer counts, bad cube combination, bad
 *         sample count, dead device), LC_ERROR_OUT_OF_MEMORY,
 *         LC_ERROR_IMAGE_CREATION_FAILED.
 */
LC_API lc_result lc_image_create(
    lc_device *device,
    const lc_image_desc *desc,
    lc_image **out_image
);

/**
 * Destroy an image, its view, and its memory. Safe with NULL.
 */
LC_API void lc_image_destroy(lc_image *image);

/**
 * Get the image format in backend-neutral form.
 * Returns LC_FORMAT_UNDEFINED for NULL.
 */
LC_API lc_format lc_image_get_format(const lc_image *image);

/**
 * Get the image width in texels (base mip level). Returns 0 for NULL.
 */
LC_API uint32_t lc_image_get_width(const lc_image *image);

/**
 * Get the image height in texels (base mip level, 1 for 1D).
 * Returns 0 for NULL.
 */
LC_API uint32_t lc_image_get_height(const lc_image *image);

/**
 * Get the image mip level count. Returns 0 for NULL.
 */
LC_API uint32_t lc_image_get_mip_levels(const lc_image *image);

/**
 * Get the image array layer count. Returns 0 for NULL.
 */
LC_API uint32_t lc_image_get_array_layers(const lc_image *image);

/* Upload region: one mip level of one array layer. Dimensions must fit
 * inside that mip (`extent >> mip_level`, minimum 1 per axis);
 * `data_size` must hold the tightly packed region
 * (width*height*depth*element bytes). Uploads beyond mip 0 require the
 * image to have been created with enough mip levels; generation (see
 * below) is the normal way to fill the rest. */
typedef struct lc_image_upload_desc {
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    const void *data;
    uint64_t data_size;
} lc_image_upload_desc;

/**
 * Upload tightly packed texels into one mip level of one array layer
 * through a staging buffer. The level ends in a sampled-readable
 * layout (upper levels are generated separately, if at all).
 *
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL image/upload,
 *         NULL data with nonzero size, dead image, level/layer out of
 *         range, region exceeds the mip, short data, missing
 *         TRANSFER_DST usage), LC_ERROR_OUT_OF_MEMORY (staging),
 *         LC_ERROR_UNKNOWN (transfer failure).
 */
LC_API lc_result lc_image_write(
    lc_image *image,
    const lc_image_upload_desc *upload
);

/**
 * Generate a full mip chain on the GPU with linear filtering, ending
 * with every level sampled-readable. Requires TRANSFER_SRC and
 * TRANSFER_DST usage; the format must support linear blits.
 *
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL/dead image),
 *         LC_ERROR_UNSUPPORTED (usage or format cannot blit),
 *         LC_ERROR_UNKNOWN (transfer failure).
 */
LC_API lc_result lc_image_generate_mipmaps(lc_image *image);

/* -------------------------------------------------------------------------
 * Sampler API (Phase 9: sampling configuration, no bindings yet)
 * ------------------------------------------------------------------------- */

/* Opaque sampler handle. Never dereference; use API below. */
typedef struct lc_sampler lc_sampler;

/* Backend-neutral magnification/minification filters. */
typedef enum lc_filter {
    LC_FILTER_NEAREST = 0,
    LC_FILTER_LINEAR = 1
} lc_filter;

/* Backend-neutral mip filter. */
typedef enum lc_mipmap_mode {
    LC_MIPMAP_MODE_NEAREST = 0,
    LC_MIPMAP_MODE_LINEAR = 1
} lc_mipmap_mode;

/* Backend-neutral sampler address modes. */
typedef enum lc_address_mode {
    LC_ADDRESS_REPEAT = 0,
    LC_ADDRESS_MIRRORED_REPEAT = 1,
    LC_ADDRESS_CLAMP_TO_EDGE = 2,
    LC_ADDRESS_CLAMP_TO_BORDER = 3
} lc_address_mode;

/* Sampler creation parameters. LODs satisfy min_lod <= max_lod >= 0.
 * max_anisotropy <= 1 disables anisotropy; larger values require
 * device support (see lc_device_limits) and are clamped to it. */
typedef struct lc_sampler_desc {
    lc_filter min_filter;
    lc_filter mag_filter;
    lc_mipmap_mode mipmap_mode;
    lc_address_mode address_u;
    lc_address_mode address_v;
    lc_address_mode address_w;
    float mip_lod_bias;
    float min_lod;
    float max_lod;
    float max_anisotropy;
} lc_sampler_desc;

/**
 * Create a sampler on a device. Requires lc_init() first and a live
 * device. Anisotropy above 1 needs device support; values are clamped
 * to the device maximum.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL device/desc/out, unknown
 *         filter/address enums, inverted/negative LOD range, anisotropy
 *         below 1, dead device), LC_ERROR_OUT_OF_MEMORY,
 *         LC_ERROR_SAMPLER_CREATION_FAILED.
 */
LC_API lc_result lc_sampler_create(
    lc_device *device,
    const lc_sampler_desc *desc,
    lc_sampler **out_sampler
);

/**
 * Destroy a sampler. Safe to call with NULL.
 */
LC_API void lc_sampler_destroy(lc_sampler *sampler);

#ifdef __cplusplus
}
#endif

#endif /* LUMAC_H */
