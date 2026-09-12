#ifndef LUMAC_H
#define LUMAC_H

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
    LC_SUBOPTIMAL = 16
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

#ifdef __cplusplus
}
#endif

#endif /* LUMAC_H */
