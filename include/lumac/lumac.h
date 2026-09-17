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
    LC_ERROR_UNSUPPORTED = 22,
    /* Bounded wait expired (signal/retirement waits with an explicit
     * timeout only; never from infinite waits). */
    LC_ERROR_TIMEOUT = 23
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
 * Window input events (Phase 27: backend-neutral event queue)
 *
 * Threading: pump on the application's main thread (same thread that
 * calls lc_poll_events). Read queued events with lc_window_read_event
 * any time after the pump; the queue is drained, never implicitly
 * cleared — unread events persist until read or window destruction.
 *
 * Key identity is PHYSICAL (position), not text: LC_KEY_W is the key
 * in the W position of a US layout, regardless of keyboard layout or
 * modifiers. Text entry arrives separately as LC_EVENT_CHAR (UTF-8).
 * Mouse position is in client-area pixels: origin top-left, +x right,
 * +y down. Deltas and wheel accumulate per engine input frame.
 * ------------------------------------------------------------------------- */

/* Backend-neutral physical key identity (USB-HID-usage-inspired
 * ordering, Luma-owned values — never Win32 VK_* or X11 KeySym). */
typedef enum lc_keycode {
    LC_KEY_UNKNOWN = 0,
    /* A-Z (physical positions). */
    LC_KEY_A = 1, LC_KEY_B, LC_KEY_C, LC_KEY_D, LC_KEY_E, LC_KEY_F,
    LC_KEY_G, LC_KEY_H, LC_KEY_I, LC_KEY_J, LC_KEY_K, LC_KEY_L,
    LC_KEY_M, LC_KEY_N, LC_KEY_O, LC_KEY_P, LC_KEY_Q, LC_KEY_R,
    LC_KEY_S, LC_KEY_T, LC_KEY_U, LC_KEY_V, LC_KEY_W, LC_KEY_X,
    LC_KEY_Y, LC_KEY_Z,
    /* 0-9 (top row). */
    LC_KEY_0 = 30, LC_KEY_1, LC_KEY_2, LC_KEY_3, LC_KEY_4,
    LC_KEY_5, LC_KEY_6, LC_KEY_7, LC_KEY_8, LC_KEY_9,
    /* Control / whitespace. */
    LC_KEY_ESCAPE = 50, LC_KEY_ENTER, LC_KEY_TAB, LC_KEY_SPACE,
    LC_KEY_BACKSPACE,
    /* Modifiers (left/right distinguished where the OS reports it). */
    LC_KEY_LEFT_SHIFT = 60, LC_KEY_RIGHT_SHIFT,
    LC_KEY_LEFT_CONTROL, LC_KEY_RIGHT_CONTROL,
    LC_KEY_LEFT_ALT, LC_KEY_RIGHT_ALT,
    LC_KEY_LEFT_SUPER, LC_KEY_RIGHT_SUPER,
    /* Arrows. */
    LC_KEY_LEFT = 70, LC_KEY_RIGHT, LC_KEY_UP, LC_KEY_DOWN,
    /* Navigation. */
    LC_KEY_INSERT = 80, LC_KEY_DELETE, LC_KEY_HOME, LC_KEY_END,
    LC_KEY_PAGE_UP, LC_KEY_PAGE_DOWN,
    /* F1-F12. */
    LC_KEY_F1 = 90, LC_KEY_F2, LC_KEY_F3, LC_KEY_F4, LC_KEY_F5,
    LC_KEY_F6, LC_KEY_F7, LC_KEY_F8, LC_KEY_F9, LC_KEY_F10,
    LC_KEY_F11, LC_KEY_F12,
    /* Numpad. */
    LC_KEY_NUMPAD_0 = 110, LC_KEY_NUMPAD_1, LC_KEY_NUMPAD_2,
    LC_KEY_NUMPAD_3, LC_KEY_NUMPAD_4, LC_KEY_NUMPAD_5,
    LC_KEY_NUMPAD_6, LC_KEY_NUMPAD_7, LC_KEY_NUMPAD_8,
    LC_KEY_NUMPAD_9, LC_KEY_NUMPAD_DECIMAL, LC_KEY_NUMPAD_DIVIDE,
    LC_KEY_NUMPAD_MULTIPLY, LC_KEY_NUMPAD_SUBTRACT,
    LC_KEY_NUMPAD_ADD, LC_KEY_NUMPAD_ENTER, LC_KEY_NUMPAD_EQUAL,
    /* Punctuation (US positions). */
    LC_KEY_MINUS = 130, LC_KEY_EQUAL, LC_KEY_LEFT_BRACKET,
    LC_KEY_RIGHT_BRACKET, LC_KEY_BACKSLASH, LC_KEY_SEMICOLON,
    LC_KEY_APOSTROPHE, LC_KEY_GRAVE, LC_KEY_COMMA, LC_KEY_PERIOD,
    LC_KEY_SLASH, LC_KEY_CAPS_LOCK,
    LC_KEY_COUNT = 143
} lc_keycode;

/* Mouse buttons (backend-neutral; 4/5 = side buttons where present). */
typedef enum lc_mouse_button {
    LC_MOUSE_LEFT = 0,
    LC_MOUSE_RIGHT = 1,
    LC_MOUSE_MIDDLE = 2,
    LC_MOUSE_4 = 3,
    LC_MOUSE_5 = 4,
    LC_MOUSE_BUTTON_COUNT = 5
} lc_mouse_button;

/* Event kinds in the per-window queue. */
typedef enum lc_event_type {
    LC_EVENT_NONE = 0,
    LC_EVENT_KEY_DOWN = 1,   /* key field; repeat != 0 iff OS auto-repeat */
    LC_EVENT_KEY_UP = 2,     /* key field */
    LC_EVENT_CHAR = 3,       /* utf8[0..utf8_len) holds one UTF-8 scalar */
    LC_EVENT_MOUSE_DOWN = 4, /* button + position fields */
    LC_EVENT_MOUSE_UP = 5,   /* button + position fields */
    LC_EVENT_MOUSE_MOVE = 6, /* position; delta holds relative motion */
    LC_EVENT_MOUSE_WHEEL = 7,/* wheel_x/wheel_y in detents (lines) */
    LC_EVENT_FOCUS_GAINED = 8,
    LC_EVENT_FOCUS_LOST = 9,
    LC_EVENT_RESIZE = 10,    /* width/height fields */
    LC_EVENT_CLOSE = 11      /* mirrors should_close for queue readers */
} lc_event_type;

/* Key modifier snapshot carried on key/char/mouse events. */
typedef enum lc_key_mod {
    LC_MOD_NONE = 0,
    LC_MOD_SHIFT = 1 << 0,
    LC_MOD_CONTROL = 1 << 1,
    LC_MOD_ALT = 1 << 2,
    LC_MOD_SUPER = 1 << 3
} lc_key_mod;

/* One queued window event (plain data, no pointers). */
typedef struct lc_window_event {
    lc_event_type type;
    lc_window *window;      /* source window (borrowed, never NULL) */
    lc_keycode key;         /* KEY_DOWN/UP */
    int repeat;             /* KEY_DOWN: nonzero iff OS auto-repeat */
    uint32_t mods;          /* lc_key_mod bitmask */
    lc_mouse_button button; /* MOUSE_DOWN/UP */
    float mouse_x;          /* client px: origin top-left, +x right */
    float mouse_y;          /* client px: +y down */
    float delta_x;          /* MOUSE_MOVE relative motion (px) */
    float delta_y;
    float wheel_x;          /* WHEEL: horizontal detents */
    float wheel_y;          /* WHEEL: vertical detents (up positive) */
    uint32_t width;         /* RESIZE client px */
    uint32_t height;
    char utf8[8];           /* CHAR: NUL-terminated UTF-8 scalar */
    uint32_t utf8_len;      /* CHAR: bytes excluding NUL (1..4) */
} lc_window_event;

/**
 * Read (and remove) the oldest pending event for a window.
 *
 * @param window Window to read from (NULL fails).
 * @param out Receives the event; LC_EVENT_NONE when the queue is empty.
 * @return LC_SUCCESS (type NONE included) or LC_ERROR_INVALID_ARGUMENT.
 */
LC_API lc_result lc_window_read_event(lc_window *window,
                                      lc_window_event *out);

/**
 * Drop all pending events for a window. Safe with NULL (no-op... returns
 * INVALID_ARGUMENT to match read_event; NULL window has no queue).
 */
LC_API lc_result lc_window_drain_events(lc_window *window);

/**
 * Query the number of pending events for a window (0 for NULL).
 */
LC_API uint32_t lc_window_pending_events(const lc_window *window);

/**
 * Inject one synthetic event into a window's queue (Phase 34A
 * automation path). The event lands in the SAME ring buffer the
 * OS backends push through (lc_window_push_event) and is consumed
 * by the SAME drain (lc_window_read_event inside leg_frame_begin
 * / le_input_poll_platform) — automation exercises the exact
 * production event path, never a widget callback directly.
 * LC_EVENT_NONE is refused (no-op, INVALID_ARGUMENT); the
 * window field is overwritten with `window`.
 */
LC_API lc_result lc_window_inject_event(lc_window *window,
                                        const lc_window_event *event);

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

/* Device creation parameters. Zero-initialize the whole struct
 * (e.g. `= { 0 }`) and set the fields you use: an uninitialized
 * pipeline_cache_path pointer is undefined behavior (Vulkan-style
 * create-info discipline). enable_validation != 0 requests Vulkan
 * validation layers; if they are unavailable, creation continues without
 * validation after a brief stderr notice (best-effort diagnostics).
 * Phase 18: pipeline_cache_path optionally names a file backing the
 * persistent Vulkan pipeline cache (backend-neutral name; a future
 * D3D12 backend may use its own representation). NULL or empty means
 * in-memory cache only (no file I/O). disable_pipeline_cache != 0
 * disables even the in-memory VkPipelineCache (no file I/O either;
 * for tests and sandboxed environments). The path is copied at
 * creation; the caller keeps ownership. Parent directories must
 * already exist; unwritable paths fail safely (cache disabled). */
typedef struct lc_device_desc {
    lc_backend backend;
    int enable_validation;
    const char *pipeline_cache_path;
    int disable_pipeline_cache;
    /* Async staging cap in bytes (PART V; 0 selects the 256 MiB
     * default). Schedulers wait for the oldest completion rather
     * than exceed it, so staging stays bounded under load. */
    uint64_t upload_staging_cap;
    /* Test-only capability forcing (Phase 22, PARTs W-Y):
     * nonzero forces the named fallback even on capable hardware
     * so the binary-fallback, graphics-transfer, and
     * graphics-compute paths get exercised everywhere. Production
     * code leaves all three zero. */
    int force_binary_fallback;  /* disable timeline semaphores */
    int force_graphics_transfer;/* alias transfer to graphics */
    int force_graphics_compute; /* alias compute to graphics */
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
 * Block until all previously submitted device work completes.
 * Safe with NULL (no-op) and with dead devices. Needed before
 * destroying resources the GPU may still reference (e.g. sampled
 * attachments being recreated) and before teardown without
 * external synchronization. Best effort: wait failures are
 * swallowed (nothing useful to report to a sync primitive).
 */
LC_API void lc_device_wait_idle(lc_device *device);

/**
 * Get the GPU name (e.g. "NVIDIA GeForce RTX 5060").
 * Valid for the lifetime of the device. Returns NULL for NULL device.
 */
LC_API const char *lc_device_get_name(const lc_device *device);

/* -------------------------------------------------------------------------
 * Monotonic clock (Phase 18: profiling foundation; Phase 22
 * nanosecond contract).
 *
 * Backend-neutral monotonic time for CPU-side timing (renderer
 * frame profiles, pipeline-creation timing) and nanosecond
 * timeout APIs. Never wall time; never Vulkan timestamps (GPU
 * timing is deferred debt). Threading: callable from any thread.
 * ------------------------------------------------------------------------- */

/** Nanoseconds per second (1,000,000,000 on every platform). */
LC_API uint64_t lc_clock_frequency(void);

/** Current monotonic time in nanoseconds (arbitrary epoch;
 *  differences only). */
LC_API uint64_t lc_clock_now(void);

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
 * guaranteed FIFO present mode; vsync == 0 prefers low-latency modes.
 * max_frames_in_flight == 0 selects the default (2); otherwise 1..8
 * flight slots are allocated (clamped). It is independent of the
 * swapchain image count (more slots than images simply wait longer
 * on image ownership). */
typedef struct lc_swapchain_desc {
    uint32_t width;
    uint32_t height;
    uint32_t image_count;
    int vsync;
    uint32_t max_frames_in_flight;
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
    LC_SHADER_STAGE_FRAGMENT = 1,
    LC_SHADER_STAGE_COMPUTE = 2
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

/* Opaque binding-layout handle (defined below); pipeline slots reference it. */
typedef struct lc_binding_layout lc_binding_layout;

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

/* Backend-neutral index element types for indexed drawing. Maps to
 * Vulkan index types and D3D12 index-buffer formats alike. */
typedef enum lc_index_type {
    LC_INDEX_UINT16 = 0,
    LC_INDEX_UINT32 = 1
} lc_index_type;

/* Backend-neutral rasterizer state. Zero-initialized descriptors
 * preserve the legacy behavior (no culling, clockwise front). */
typedef enum lc_cull_mode {
    LC_CULL_NONE = 0,
    LC_CULL_FRONT = 1,
    LC_CULL_BACK = 2
} lc_cull_mode;

typedef enum lc_front_face {
    LC_FRONT_FACE_CLOCKWISE = 0,
    LC_FRONT_FACE_COUNTER_CLOCKWISE = 1
} lc_front_face;

/* Backend-neutral color-blend factors (Phase 33: editor GUI overlays;
 * Vulkan blend-factor mapping, D3D12 BLEND-mappable). */
typedef enum lc_blend_factor {
    LC_BLEND_ZERO = 0,
    LC_BLEND_ONE = 1,
    LC_BLEND_SRC_ALPHA = 2,
    LC_BLEND_ONE_MINUS_SRC_ALPHA = 3
} lc_blend_factor;

/* Backend-neutral color-blend operations (Phase 33). */
typedef enum lc_blend_op {
    LC_BLEND_OP_ADD = 0
} lc_blend_op;

/* One color-attachment blend recipe (Phase 33: GUI/overlay pipelines
 * need source-alpha blending; legacy opaque pipelines leave
 * blend_enable == 0). Unknown factors/ops are rejected at creation. */
typedef struct lc_blend_attachment {
    int blend_enable;
    lc_blend_factor src_color_factor;
    lc_blend_factor dst_color_factor;
    lc_blend_op color_op;
    lc_blend_factor src_alpha_factor;
    lc_blend_factor dst_alpha_factor;
    lc_blend_op alpha_op;
} lc_blend_attachment;

/* One push-constant range: a byte window owned by the pipeline
 * layout, addressed with lc_push_constants(). `visibility` uses
 * lc_shader_visibility bits (graphics pipelines accept VERTEX and/or
 * FRAGMENT). `offset` and `size` are in bytes, both multiples of 4,
 * with size > 0 and offset + size inside the device limit
 * (at least 128 bytes on Vulkan/D3D12-class hardware). Ranges must
 * not overlap. Zero-initialize descriptors you do not use. */
typedef struct lc_push_constant_range {
    uint32_t visibility;
    uint32_t offset;
    uint32_t size;
} lc_push_constant_range;

/* Maximum color attachments per render target / pass. Targets with
 * more are rejected; devices may support fewer (see
 * lc_device_limits.max_color_attachments). */
#define LC_MAX_COLOR_ATTACHMENTS 8

/* Number of samples per texel (multisampling). Only 1 is exercised
 * yet; the enum exists so descriptors never need reshaping when MSAA
 * work begins. Render-target compatibility includes the sample count
 * even before MSAA resolve exists. */
typedef enum lc_sample_count {
    LC_SAMPLE_COUNT_1 = 1,
    LC_SAMPLE_COUNT_2 = 2,
    LC_SAMPLE_COUNT_4 = 4,
    LC_SAMPLE_COUNT_8 = 8
} lc_sample_count;

/* Backend-neutral render-target compatibility description (Phase 12).
 * Pipelines are compatible with targets whose structural signature
 * matches: color count + color formats + depth format + sample count.
 * Width/height are NOT part of pipeline compatibility (dynamic
 * viewport/scissor); they describe a concrete target object. Depth
 * LC_FORMAT_UNDEFINED means "no depth". Zero attachments of both
 * kinds is invalid for real targets; for pipeline descs an all-zero
 * struct means "legacy: infer from the creation swapchain". */
typedef struct lc_render_target_desc {
    uint32_t width;
    uint32_t height;
    uint32_t color_attachment_count;
    lc_format color_formats[LC_MAX_COLOR_ATTACHMENTS];
    lc_format depth_stencil_format;
    lc_sample_count samples;
} lc_render_target_desc;

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

/* Minimal graphics-pipeline description. The vertex shader is
 * required; the fragment shader is optional (NULL selects
 * rasterization without fragment processing — no color attachment
 * is ever written, so this is only useful with depth-only targets,
 * e.g. shadow-map depth passes).
 * Optional vertex input: zero counts mean no vertex buffers (shader
 * vertex-index generation, as in the first triangle). Optional
 * resource slots: an ordered array of binding layouts, indexed by
 * slot at bind time (backend-neutral numbering shared with D3D12
 * root-signature slots). Counts are validated at creation.
 * Raster state defaults to no culling with a clockwise front face
 * (legacy behavior). Depth is off by default; enable test/write for
 * 3D rendering (LESS comparison). Optional push-constant ranges
 * default to none. Blend state (Phase 33): `blend` is per-color-
 * attachment blending; NULL (or all entries blend_enable == 0)
 * preserves the legacy opaque behavior exactly. Enabled entries need
 * known factors/ops (validated at creation). Blend participates in
 * NO compatibility signature: width/height-style, pipelines stay
 * extent-independent AND blend-independent (a UI overlay pipeline
 * and an opaque pipeline share targets/passes).
 * Render target (Phase 13): a valid structural compatibility
 * description is MANDATORY (no legacy inference): 1..8 color formats
 * and/or a depth format, color formats with color, depth format with
 * depth, known sample count. Depth test/write require a depth format.
 * Width/height are ignored for pipeline compatibility (pipelines are
 * extent-independent). Use lc_swapchain_get_render_target_desc() to
 * build a presentation-compatible description deliberately.
 * Scissor (Phase 33): pipelines carry NO scissor rect — scissor is
 * dynamic encoder state (lc_encoder_set_scissor) defaulting to the
 * full target each pass. A future D3D12 backend maps blend recipes
 * and scissor rects onto OMSetBlendState / RSSetScissorRects.
 * Zero-initialize the whole struct (e.g. `= { 0 }`) and set the
 * fields you use. */
typedef struct lc_graphics_pipeline_desc {
    lc_shader *vertex_shader;
    lc_shader *fragment_shader;
    const lc_vertex_binding_desc *vertex_bindings;
    uint32_t vertex_binding_count;
    const lc_vertex_attribute_desc *vertex_attributes;
    uint32_t vertex_attribute_count;
    const lc_binding_layout *const *binding_layouts;
    uint32_t binding_layout_count;
    lc_cull_mode cull_mode;
    lc_front_face front_face;
    int depth_test_enable;
    int depth_write_enable;
    const lc_blend_attachment *blend;
    uint32_t blend_attachment_count;
    const lc_push_constant_range *push_constant_ranges;
    uint32_t push_constant_range_count;
    lc_render_target_desc render_target;
} lc_graphics_pipeline_desc;

/**
 * Create a graphics pipeline for a structural render-target
 * signature. Requires lc_init() first and a live device. The shaders
 * must be live, correctly staged, and owned by `device`; they may be
 * destroyed afterwards without affecting the pipeline. The pipeline
 * holds only the structural signature (never any target object) and
 * works with every target or pass sharing it.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL device/desc/out, dead
 *         device, NULL/mis-staged/dead shaders, cross-device use,
 *         invalid vertex layout, invalid render-target description,
 *         depth enabled without a depth format),
 *         LC_ERROR_OUT_OF_MEMORY, LC_ERROR_PIPELINE_CREATION_FAILED.
 */
LC_API lc_result lc_graphics_pipeline_create(
    lc_device *device,
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
 * Legacy convenience: operates on the frame's implicit swapchain
 * pass. New code should prefer the encoder path.
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
 * Phase 11: indexed drawing, instancing, push constants, depth clears
 *
 * Index buffers reuse lc_buffer with LC_BUFFER_USAGE_INDEX. Push
 * constants reuse the pipeline's declared ranges. Depth comes from
 * the swapchain's owned depth buffer (cleared to 1.0 by default).
 * Threading: main thread only, inside an open frame unless noted.
 * ------------------------------------------------------------------------- */

/* Forward declaration: full lc_buffer definition follows in the
 * Buffer API section; only the pointer is used here. */
typedef struct lc_buffer lc_buffer;

/**
 * Bind an index buffer for subsequent lc_draw_indexed() calls. Only
 * valid inside an open frame; like vertex bindings this is
 * command-buffer state and must be re-established every frame.
 *
 * @param swapchain Live swapchain with an open frame.
 * @param buffer Live buffer with LC_BUFFER_USAGE_INDEX, same device.
 * @param offset Byte offset into the buffer (multiple of the index
 *        size: 2 for UINT16, 4 for UINT32; must be inside the buffer).
 * @param index_type Element width for index interpretation.
 * @return LC_SUCCESS or LC_ERROR_INVALID_ARGUMENT (NULL/dead handles,
 *         no open frame, cross-device use, missing INDEX usage, bad
 *         offset/type).
 */
LC_API lc_result lc_bind_index_buffer(
    lc_swapchain *swapchain,
    lc_buffer *buffer,
    uint64_t offset,
    lc_index_type index_type
);

/**
 * Draw indexed geometry with instancing. Requires a bound pipeline
 * and a bound index buffer in the open frame.
 *
 * @param index_count Indices to process (> 0; offset + range must
 *        fit the bound index buffer).
 * @param instance_count Instances to draw (> 0; 1 for non-instanced).
 * @param first_index First index inside the index buffer view.
 * @param vertex_offset Signed offset added to each index before
 *        vertex fetching (negative values address relative vertices).
 * @param first_instance First instance ID (offsets gl_InstanceIndex
 *        and per-instance attributes).
 * @return LC_SUCCESS or LC_ERROR_INVALID_ARGUMENT (NULL/dead
 *         swapchain, no open frame, nothing bound, no index buffer,
 *         zero counts, out-of-range indices).
 */
LC_API lc_result lc_draw_indexed(
    lc_swapchain *swapchain,
    uint32_t index_count,
    uint32_t instance_count,
    uint32_t first_index,
    int32_t vertex_offset,
    uint32_t first_instance
);

/**
 * Draw non-indexed geometry with instancing. Requires a bound
 * pipeline in the open frame.
 *
 * @return LC_SUCCESS or LC_ERROR_INVALID_ARGUMENT (NULL/dead
 *         swapchain, no open frame, nothing bound, zero counts).
 */
LC_API lc_result lc_draw_instanced(
    lc_swapchain *swapchain,
    uint32_t vertex_count,
    uint32_t instance_count,
    uint32_t first_vertex,
    uint32_t first_instance
);

/**
 * Push small constants (e.g. a 64-byte MVP matrix) for subsequent
 * draws. Only valid inside an open frame with the matching pipeline
 * bound. The byte window [offset, offset + size) must sit inside one
 * declared pipeline range with a compatible stage mask.
 *
 * @param swapchain Live swapchain with an open frame.
 * @param pipeline Bound pipeline owning the push-constant layout.
 * @param visibility Stage mask for this push (subset of the range).
 * @param offset Byte offset (multiple of 4).
 * @param size Byte count (multiple of 4, > 0, <= 128 typically).
 * @param data Source bytes (must be non-NULL when size > 0).
 * @return LC_SUCCESS or LC_ERROR_INVALID_ARGUMENT (NULL/dead handles,
 *         no open frame, pipeline not bound, bad range/visibility).
 */
LC_API lc_result lc_push_constants(
    lc_swapchain *swapchain,
    lc_pipeline *pipeline,
    uint32_t visibility,
    uint32_t offset,
    uint32_t size,
    const void *data
);

/**
 * Clear the current frame's depth buffer. The value is clamped to
 * [0, 1] (1.0 is the far plane). Only valid between lc_begin_frame
 * and lc_end_frame; may be called multiple times (latest wins before
 * the render pass opens, exact ordering via clear attachments after).
 * When never called, depth clears to 1.0.
 *
 * @return LC_SUCCESS or LC_ERROR_INVALID_ARGUMENT (NULL/dead
 *         swapchain, or no open frame).
 */
LC_API lc_result lc_clear_depth(
    lc_swapchain *swapchain,
    float depth
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
    LC_BUFFER_USAGE_TRANSFER_DST = 1 << 5,
    LC_BUFFER_USAGE_INDIRECT     = 1 << 6
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

/**
 * Read bytes back from a buffer with bounds and overflow checking
 * (Phase 21, test/debug path — never in steady production frames:
 * GPU-only buffers drain prior device work first). CPU-visible
 * buffers invalidate + memcpy; GPU-only buffers copy through
 * staging.
 *
 * @param buffer Live buffer.
 * @param offset Byte offset; must be within the buffer.
 * @param dst Destination bytes (may be NULL only when size == 0).
 * @param size Byte count; offset + size must fit the buffer.
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL/dead buffer,
 *         out-of-range read, NULL dst with nonzero size),
 *         LC_ERROR_OUT_OF_MEMORY (staging allocation),
 *         LC_ERROR_UNKNOWN (transfer failure).
 */
LC_API lc_result lc_buffer_read(
    lc_buffer *buffer,
    uint64_t offset,
    void *dst,
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
 * max_texture_2d_dimension also bounds 2D images and render targets.
 * Anisotropy: max_sampler_anisotropy is 1.0 when unsupported,
 * otherwise the clamp upper bound for lc_sampler_desc.max_anisotropy.
 * max_color_attachments bounds render-target/pass color counts
 * (at most LC_MAX_COLOR_ATTACHMENTS). */
typedef struct lc_device_limits {
    uint32_t max_texture_2d_dimension;
    uint32_t max_vertex_attributes;
    uint32_t max_vertex_bindings;
    uint64_t max_uniform_buffer_size;
    uint32_t max_image_array_layers;
    float max_sampler_anisotropy;
    uint32_t min_uniform_buffer_offset_alignment;
    uint32_t min_storage_buffer_offset_alignment;
    uint32_t max_bound_resource_slots;
    uint32_t max_color_attachments;
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
 * Debug accounting for the internal render-pass cache (Phase 12/13):
 * how many VkRenderPass objects are currently cached on the device.
 * The cache never evicts by design (one entry per structure+policy
 * key, shared); everything is destroyed with the device. Returns 0
 * for NULL device.
 */
LC_API uint32_t lc_device_get_pass_cache_count(const lc_device *device);

/**
 * Get a swapchain's color format in backend-neutral form.
 * Returns LC_FORMAT_UNDEFINED for NULL.
 */
LC_API lc_format lc_swapchain_get_format(const lc_swapchain *swapchain);

/**
 * Get a swapchain's depth format in backend-neutral form (Phase 11).
 * Every swapchain owns a depth buffer; the format is selected from
 * device-supported depth formats at creation. Returns
 * LC_FORMAT_UNDEFINED for NULL.
 */
LC_API lc_format lc_swapchain_get_depth_format(const lc_swapchain *swapchain);

/**
 * Fill a structural render-target description matching a swapchain
 * (Phase 13 helper for presentation-compatible pipelines).
 * Width/height mirror the current extent; color count is 1 with the
 * swapchain color format; depth mirrors the swapchain depth format;
 * samples are 1.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL swapchain/desc/out, dead
 *         swapchain).
 */
LC_API lc_result lc_swapchain_get_render_target_desc(
    const lc_swapchain *swapchain,
    lc_render_target_desc *out_desc);

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

/* (lc_sample_count is defined with the render-target types above.) */

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
 * Image readback (Phase 18: public CPU-visible capture).
 *
 * Synchronous convenience API: image -> transfer to staging -> wait
 * for completion -> copy to CPU. No Vulkan staging buffers, command
 * buffers, fences, or query pools are exposed.
 *
 * PERFORMANCE WARNING: synchronous readback stalls both GPU and CPU
 * (device idle + immediate transfer + staging copy). Use for
 * screenshots, tests, editor thumbnails, and debugging — never every
 * frame in normal gameplay. Normal rendering performs no readback
 * unless explicitly requested.
 *
 * Layout semantics: CPU output is ALWAYS tightly packed deterministic
 * rows (row_pitch == width * element_bytes), even when backend
 * staging uses a larger pitch internally. Element bytes follow
 * lc_format_byte_size; no reinterpretation ever happens (sRGB stays
 * encoded, float stays float, BGRA stays BGRA, depth stays raw).
 *
 * Eligibility: the image MUST carry LC_IMAGE_USAGE_TRANSFER_SRC;
 * otherwise INVALID_ARGUMENT is returned loudly (never silent
 * zeros — the Phase 14 bug class). Renderer-owned capture-capable
 * targets (HDR, post intermediates, environment maps, shadow maps)
 * include TRANSFER_SRC automatically.
 *
 * Synchronization: the call drains prior device work before copying,
 * so a readback immediately after lc_end_frame observes the finished
 * frame. Images written by the STILL-OPEN recording must NOT be read
 * until after lc_end_frame: layout tracking describes recorded (not
 * yet executed) passes, and an immediate-submit copy issued
 * mid-recording names layouts the GPU has not reached (validation
 * error + garbage bytes). This is the same reason the test harness
 * reads back only after end_frame.
 *
 * Future async path (reserved, not implemented): lc_readback_request
 * / lc_readback_poll / lc_readback_map will build on these structs
 * without breaking this synchronous API.
 * ------------------------------------------------------------------------- */

/* Which subresource to read: one mip level of one array layer
 * (cube faces are array layers 0..5 of a cube-compatible image). */
typedef struct lc_image_readback_desc {
    uint32_t mip_level;
    uint32_t array_layer;
} lc_image_readback_desc;

/* Deterministic CPU layout of a readback result. */
typedef struct lc_image_readback_info {
    uint32_t width;     /* texels of the selected mip */
    uint32_t height;    /* texels of the selected mip */
    lc_format format;   /* image format (no conversion) */
    size_t row_pitch;   /* == width * element bytes (tight) */
    size_t size;        /* == row_pitch * height (* depth for 3D) */
} lc_image_readback_info;

/**
 * Query the CPU layout (and required byte size) for reading one
 * mip/layer of an image without copying anything.
 *
 * Supported formats: all color formats plus D16_UNORM, D32_FLOAT,
 * and D24_UNORM_S8_UINT (raw 4 bytes/texel). Multisampled images
 * (samples != 1) are rejected. Requires TRANSFER_SRC usage.
 *
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL image/desc/
 *         out, dead image, bad mip/layer, unsupported format,
 *         multisampled, missing TRANSFER_SRC).
 */
LC_API lc_result lc_image_query_readback(
    const lc_image *image,
    const lc_image_readback_desc *desc,
    lc_image_readback_info *out_info);

/**
 * Read one mip/layer of an image into tightly packed CPU memory.
 * When dst == NULL or dst_size == 0, no bytes are written but
 * *out_required_size still receives the required size (sizing
 * query). When dst is provided but too small, nothing is written
 * and INVALID_ARGUMENT is returned with the required size written
 * when out_required_size != NULL.
 *
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL image/desc,
 *         dead image, bad mip/layer, unsupported format,
 *         multisampled, missing TRANSFER_SRC, short destination),
 *         LC_ERROR_OUT_OF_MEMORY (staging), LC_ERROR_UNKNOWN
 *         (transfer failure).
 */
LC_API lc_result lc_image_readback(
    lc_image *image,
    const lc_image_readback_desc *desc,
    void *dst,
    size_t dst_size,
    size_t *out_required_size);

/* -------------------------------------------------------------------------
 * Pipeline-cache introspection (Phase 18: backend-neutral).
 *
 * The persistent Vulkan pipeline cache stays entirely behind LumaC;
 * these fields expose only load/save facts (never Vulkan UUIDs).
 * ------------------------------------------------------------------------- */
typedef struct lc_pipeline_cache_info {
    int enabled;           /* a VkPipelineCache exists for this device */
    int loaded_from_file;  /* initial bytes came from pipeline_cache_path */
    int saved_to_file;     /* shutdown wrote bytes back to that path */
    uint64_t bytes_loaded; /* blob bytes accepted at creation */
    uint64_t bytes_saved;  /* blob bytes written at shutdown */
} lc_pipeline_cache_info;
/* NOTE: saved_to_file/bytes_saved are set during lc_device_destroy
 * (which frees the device struct), so post-shutdown verification
 * should stat the file itself; the flags are best-effort live
 * state, mainly useful to confirm "no save attempted". */

/**
 * Copy out pipeline-cache status (zeros for NULL device or NULL out
 * handling: NULL device zeroes *out when provided; NULL out is a
 * no-op).
 */
LC_API void lc_device_get_pipeline_cache_info(
    const lc_device *device,
    lc_pipeline_cache_info *out_info);

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

/* -------------------------------------------------------------------------
 * Image view API (Phase 10: subresource views for binding)
 *
 * A view selects a type, aspect, mip range, and layer range of one
 * image. Binding sets reference views, never images directly, so mip
 * chains, array slices, cubemaps, and depth aspects are all
 * addressable. Threading: main thread only.
 * ------------------------------------------------------------------------- */

/* Opaque image-view handle. Never dereference; use API below. */
typedef struct lc_image_view lc_image_view;

/* Backend-neutral view dimensionality. */
typedef enum lc_image_view_type {
    LC_IMAGE_VIEW_1D = 0,
    LC_IMAGE_VIEW_1D_ARRAY = 1,
    LC_IMAGE_VIEW_2D = 2,
    LC_IMAGE_VIEW_2D_ARRAY = 3,
    LC_IMAGE_VIEW_3D = 4,
    LC_IMAGE_VIEW_CUBE = 5,
    LC_IMAGE_VIEW_CUBE_ARRAY = 6
} lc_image_view_type;

/* Backend-neutral aspect selection (bitmask, combine with |). */
typedef enum lc_image_aspect {
    LC_IMAGE_ASPECT_COLOR   = 1 << 0,
    LC_IMAGE_ASPECT_DEPTH   = 1 << 1,
    LC_IMAGE_ASPECT_STENCIL = 1 << 2
} lc_image_aspect;

/* View creation parameters. `format` may be LC_FORMAT_UNDEFINED to use
 * the image format (no reinterpretation beyond exact match yet).
 * Ranges must fit the image: base + count within mips/layers. */
typedef struct lc_image_view_desc {
    lc_image_view_type type;
    lc_format format;
    uint32_t aspect;
    uint32_t base_mip_level;
    uint32_t mip_level_count;
    uint32_t base_array_layer;
    uint32_t array_layer_count;
} lc_image_view_desc;

/**
 * Create a view of a live image. The view borrows its image; the
 * image must outlive it (destroying an image destroys its views).
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL image/desc/out, dead image,
 *         unknown type, aspect incompatible with the format, empty or
 *         overflowing ranges, cube rules violated, format mismatch),
 *         LC_ERROR_OUT_OF_MEMORY, LC_ERROR_IMAGE_CREATION_FAILED.
 */
LC_API lc_result lc_image_view_create(
    lc_image *image,
    const lc_image_view_desc *desc,
    lc_image_view **out_view
);

/**
 * Destroy an image view. Safe to call with NULL.
 */
LC_API void lc_image_view_destroy(lc_image_view *view);

/* -------------------------------------------------------------------------
 * Resource binding API (Phase 10: backend-neutral descriptors)
 *
 * A binding layout describes what resources shaders expect (numbered
 * slots with types, counts, visibility). A binding set is a layout
 * instance populated with buffers, image views, and samplers. Neither
 * concept names Vulkan descriptor sets or D3D12 heaps; backends map
 * them (Vulkan: set layouts/pools/sets; D3D12: root signature/tables).
 * Threading: main thread only.
 * ------------------------------------------------------------------------- */

/* Backend-neutral shader visibility (bitmask, combine with |). */
typedef enum lc_shader_visibility {
    LC_SHADER_VISIBILITY_VERTEX   = 1 << 0,
    LC_SHADER_VISIBILITY_FRAGMENT = 1 << 1,
    LC_SHADER_VISIBILITY_COMPUTE  = 1 << 2,
    LC_SHADER_VISIBILITY_ALL_GRAPHICS =
        (1 << 0) | (1 << 1),
    LC_SHADER_VISIBILITY_ALL = (1 << 0) | (1 << 1) | (1 << 2)
} lc_shader_visibility;

/* Backend-neutral resource slot types. */
typedef enum lc_binding_type {
    LC_BINDING_UNIFORM_BUFFER = 0,
    LC_BINDING_STORAGE_BUFFER = 1,
    LC_BINDING_SAMPLED_IMAGE = 2,
    LC_BINDING_STORAGE_IMAGE = 3,
    LC_BINDING_SAMPLER = 4
} lc_binding_type;


/* One numbered resource slot: `count` prepares for arrays (count >= 1
 * always; descriptor arrays map structurally). */
typedef struct lc_binding_desc {
    uint32_t binding;
    lc_binding_type type;
    uint32_t count;
    uint32_t visibility;
} lc_binding_desc;

/* Binding-layout description: an unordered set of slots. */
typedef struct lc_binding_layout_desc {
    const lc_binding_desc *bindings;
    uint32_t binding_count;
} lc_binding_layout_desc;

/**
 * Create a binding layout on a device. Requires lc_init() first and a
 * live device.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL device/desc/out, NULL
 *         bindings with nonzero count, duplicate binding numbers,
 *         count == 0, unknown type or visibility bits, over device
 *         limits, dead device), LC_ERROR_OUT_OF_MEMORY.
 */
LC_API lc_result lc_binding_layout_create(
    lc_device *device,
    const lc_binding_layout_desc *desc,
    lc_binding_layout **out_layout
);

/**
 * Destroy a binding layout. Safe to call with NULL. Pipelines and
 * sets built from it hold no reference: destroying a layout first
 * invalidates them for binding (rejected where detectable).
 */
LC_API void lc_binding_layout_destroy(lc_binding_layout *layout);

/* Opaque binding-set handle. Never dereference; use API below. */
typedef struct lc_binding_set lc_binding_set;

/**
 * Create an empty binding set from a live layout. No pipeline needed.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL layout/out, dead layout),
 *         LC_ERROR_OUT_OF_MEMORY.
 */
LC_API lc_result lc_binding_set_create(
    lc_binding_layout *layout,
    lc_binding_set **out_set
);

/**
 * Destroy a binding set, freeing its descriptor allocation.
 * Safe to call with NULL.
 */
LC_API void lc_binding_set_destroy(lc_binding_set *set);

/* One buffer range for a binding write. `size` == 0 binds the whole
 * buffer from `offset` (explicit whole-size naming deferred). */
typedef struct lc_buffer_binding {
    lc_buffer *buffer;
    uint64_t offset;
    uint64_t size;
} lc_buffer_binding;

/* One image view for a binding write. */
typedef struct lc_image_binding {
    lc_image_view *view;
} lc_image_binding;

/* One sampler for a binding write. */
typedef struct lc_sampler_binding {
    lc_sampler *sampler;
} lc_sampler_binding;

/* One slot update. Only the member matching `type` is read. */
typedef struct lc_binding_write {
    uint32_t binding;
    uint32_t array_element;
    lc_binding_type type;
    union {
        lc_buffer_binding buffer;
        lc_image_binding image;
        lc_sampler_binding sampler;
    } u;
} lc_binding_write;

/**
 * Write resources into a live binding set. Validates every write
 * (slot exists, type matches, array bounds, buffer ranges, device
 * match, resource liveness) before recording anything.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL set, NULL writes with
 *         nonzero count, dead set, unknown binding/type, element
 *         overflow, dead/mismatched resources, bad ranges),
 *         LC_ERROR_OUT_OF_MEMORY.
 */
LC_API lc_result lc_binding_set_update(
    lc_binding_set *set,
    const lc_binding_write *writes,
    uint32_t write_count
);

/**
 * Bind a resource set at a pipeline slot during an open frame.
 * The set's layout must be the pipeline's layout at `slot`.
 * Legacy convenience: operates on the frame's implicit swapchain
 * pass. New code should prefer the encoder path.
 *
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL/dead handles,
 *         no open frame, cross-device use, slot out of range,
 *         layout mismatch).
 */
LC_API lc_result lc_bind_binding_set(
    lc_swapchain *swapchain,
    lc_pipeline *pipeline,
    uint32_t slot,
    lc_binding_set *set
);

/* -------------------------------------------------------------------------
 * Render targets + command encoders (Phase 12: offscreen rendering)
 *
 * A swapchain is a presentation mechanism, not the universal render
 * target. Offscreen targets render 3D scenes into textures (editor
 * viewports, thumbnails, G-buffers, shadow maps); a second pass
 * samples the result to the swapchain. Pipelines are compatible
 * structurally (formats + samples), never by target pointer.
 * Threading: main thread only.
 * ------------------------------------------------------------------------- */

/* Opaque render-target handle. Never dereference; use API below.
 * Offscreen targets are caller-owned (create/destroy). Swapchain
 * targets are borrowed (never destroy). Targets hold non-owning
 * references to their views; views must outlive their targets. */
typedef struct lc_render_target lc_render_target;

/* Opaque command-encoder handle. Borrowed from an open swapchain
 * frame (see lc_swapchain_get_encoder); never destroy, never store
 * past lc_end_frame. All generic recording happens through it. */
typedef struct lc_command_encoder lc_command_encoder;

/* Backend-neutral attachment load/store policies. */
typedef enum lc_load_op {
    LC_LOAD_OP_LOAD = 0,
    LC_LOAD_OP_CLEAR = 1,
    LC_LOAD_OP_DONT_CARE = 2
} lc_load_op;

typedef enum lc_store_op {
    LC_STORE_OP_STORE = 0,
    LC_STORE_OP_DONT_CARE = 1
} lc_store_op;

/* One offscreen color attachment reference. The view's image must
 * carry LC_IMAGE_USAGE_COLOR_ATTACHMENT and (Phase 12 finals always
 * land sampled-readable) LC_IMAGE_USAGE_SAMPLED. Dimensions must
 * match the target extent (base mip level covering width x height). */
typedef struct lc_render_target_attachment {
    lc_image_view *view;
} lc_render_target_attachment;

/* Offscreen target creation parameters. At least one color attachment
 * or a depth attachment is required. All color views must share one
 * device, one extent (width x height at their subresource), one
 * sample count, and color formats; the depth view (when present) must
 * share the device/extent/samples with a depth format. Formats are
 * inferred from the views (never duplicated). */
typedef struct lc_render_target_create_desc {
    uint32_t width;
    uint32_t height;
    const lc_render_target_attachment *color_attachments;
    uint32_t color_attachment_count;
    lc_image_view *depth_stencil_attachment;
} lc_render_target_create_desc;

/**
 * Create an offscreen render target on a device. Requires lc_init()
 * first and a live device. Views are borrowed (non-owning); the
 * target becomes invalid for recording once any attachment view is
 * destroyed (use is then rejected, never dereferenced).
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL device/desc/out, dead device,
 *         zero extent, zero attachments of both kinds, too many colors,
 *         NULL color array with nonzero count, NULL/dead views,
 *         cross-device use, dimension/sample mismatch, missing
 *         COLOR_ATTACHMENT or DEPTH_STENCIL usage, color format used
 *         as depth or vice versa, over device limits),
 *         LC_ERROR_OUT_OF_MEMORY.
 */
LC_API lc_result lc_render_target_create(
    lc_device *device,
    const lc_render_target_create_desc *desc,
    lc_render_target **out_target
);

/**
 * Destroy an offscreen target and its backend framebuffers (views and
 * images are untouched). Safe to call with NULL. Borrowed swapchain
 * targets must never be passed here.
 */
LC_API void lc_render_target_destroy(lc_render_target *target);

/**
 * Get a target's extent width. Returns 0 for NULL.
 */
LC_API uint32_t lc_render_target_get_width(const lc_render_target *target);

/**
 * Get a target's extent height. Returns 0 for NULL.
 */
LC_API uint32_t lc_render_target_get_height(const lc_render_target *target);

/**
 * Get a target's color attachment count. Returns 0 for NULL.
 */
LC_API uint32_t
lc_render_target_get_color_count(const lc_render_target *target);

/**
 * Get one color attachment format. Returns LC_FORMAT_UNDEFINED for
 * NULL target or out-of-range index.
 */
LC_API lc_format
lc_render_target_get_color_format(const lc_render_target *target,
                                  uint32_t index);

/**
 * Get a target's depth format (UNDEFINED when depthless).
 * Returns LC_FORMAT_UNDEFINED for NULL.
 */
LC_API lc_format
lc_render_target_get_depth_format(const lc_render_target *target);

/**
 * Get a target's sample count (0 for NULL).
 */
LC_API lc_sample_count
lc_render_target_get_samples(const lc_render_target *target);

/**
 * Get one color attachment view (borrowed, do not destroy). Needed by
 * render layers that build explicit passes from targets without
 * tracking views themselves. Returns NULL for NULL target or
 * out-of-range index.
 */
LC_API lc_image_view *lc_render_target_get_color_view(
    const lc_render_target *target,
    uint32_t index);

/**
 * Get the depth attachment view (borrowed, do not destroy), or NULL
 * when depthless or for NULL target.
 */
LC_API lc_image_view *
lc_render_target_get_depth_view(const lc_render_target *target);

/**
 * Structural compatibility query: nonzero when `target` and
 * `pipeline` share color count/formats, depth format, and samples.
 * Returns 0 for NULL/dead handles (never crashes).
 */
LC_API int lc_render_target_is_compatible_with_pipeline(
    const lc_render_target *target,
    const lc_pipeline *pipeline);

/**
 * Borrow a swapchain's render-target representation (color + depth as
 * created with the swapchain). The pointer is valid until the next
 * swapchain recreate/destroy; re-query after recreation. Never
 * destroy it. Returns NULL for NULL swapchain.
 */
LC_API lc_render_target *
lc_swapchain_get_render_target(lc_swapchain *swapchain);

/**
 * Borrow the open frame's command encoder. Requires an open frame on
 * a live swapchain (between lc_begin_frame and lc_end_frame).
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL/out NULL, dead swapchain,
 *         no open frame).
 */
LC_API lc_result lc_swapchain_get_encoder(
    lc_swapchain *swapchain,
    lc_command_encoder **out_encoder);

/* One color attachment for an explicit render pass: the view to
 * render into plus load/store policy and clear color. A NULL view is
 * never valid here (views come from the target or swapchain). */
typedef struct lc_render_color_attachment {
    lc_image_view *view;
    lc_load_op load_op;
    lc_store_op store_op;
    float clear_color[4];
} lc_render_color_attachment;

/* Depth attachment for an explicit render pass (nullable as a whole
 * via a NULL desc pointer). Stencil must stay DONT_CARE with
 * clear_stencil 0 (stencil rendering is deferred past Phase 12). */
typedef struct lc_render_depth_attachment {
    lc_image_view *view;
    lc_load_op depth_load_op;
    lc_store_op depth_store_op;
    float clear_depth;
    lc_load_op stencil_load_op;
    lc_store_op stencil_store_op;
    uint32_t clear_stencil;
} lc_render_depth_attachment;

/* Explicit offscreen render-pass description. Views must belong to
 * one live lc_render_target created for the same extent; the
 * attachment count/formats/samples must equal that target's
 * signature (checked structurally). Width/height must equal the
 * target extent. */
typedef struct lc_render_pass_desc {
    const lc_render_color_attachment *color_attachments;
    uint32_t color_attachment_count;
    const lc_render_depth_attachment *depth_attachment;
    uint32_t width;
    uint32_t height;
} lc_render_pass_desc;

/* Explicit swapchain render-pass description. Renders into the
 * swapchain's current image (+ owned depth). Color must CLEAR (swap
 * images start UNDEFINED each frame, so LOAD is rejected); depth may
 * CLEAR or DONT_CARE. */
typedef struct lc_render_swapchain_pass_desc {
    lc_load_op color_load_op;
    lc_store_op color_store_op;
    float clear_color[4];
    lc_load_op depth_load_op;
    lc_store_op depth_store_op;
    float clear_depth;
} lc_render_swapchain_pass_desc;

/**
 * Begin an explicit offscreen render pass on an encoder. Exactly one
 * pass may be open per frame (legacy implicit passes count too).
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL encoder/desc, dead objects,
 *         no open frame, pass already open, legacy pass state pending,
 *         bad counts/enums/dimensions, views not from one live target,
 *         signature mismatch, missing attachment usage, LOAD from
 *         UNDEFINED state, stencil misuse).
 */
LC_API lc_result lc_encoder_begin_render_pass(
    lc_command_encoder *encoder,
    const lc_render_pass_desc *desc);

/**
 * Begin an explicit swapchain render pass on an encoder (same
 * single-pass rules as above).
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL encoder/swapchain/desc, dead
 *         objects, encoder belongs to another swapchain, no open frame,
 *         pass already open, legacy pass state pending, bad enums,
 *         LOAD color on UNDEFINED swap image, stencil misuse).
 */
LC_API lc_result lc_encoder_begin_swapchain_pass(
    lc_command_encoder *encoder,
    lc_swapchain *swapchain,
    const lc_render_swapchain_pass_desc *desc);

/**
 * End the open explicit pass. Requires a pass opened through this
 * encoder.
 *
 * @return LC_SUCCESS or LC_ERROR_INVALID_ARGUMENT (NULL/dead encoder,
 *         no open frame, no open pass).
 */
LC_API lc_result lc_encoder_end_render_pass(lc_command_encoder *encoder);

/**
 * Bind a pipeline for subsequent encoder draws. Requires an open
 * explicit pass; the pipeline must be structurally compatible with
 * the pass target (count/formats/samples) on the same device.
 *
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL/dead handles,
 *         no open frame, no open pass, cross-device use),
 *         LC_ERROR_PIPELINE_INCOMPATIBLE (signature mismatch).
 */
LC_API lc_result lc_encoder_bind_pipeline(
    lc_command_encoder *encoder,
    lc_pipeline *pipeline);

/**
 * Bind a resource set at a pipeline slot in the open explicit pass.
 * Signature matching is canonical (contents, never pointers).
 */
LC_API lc_result lc_encoder_bind_binding_set(
    lc_command_encoder *encoder,
    lc_pipeline *pipeline,
    uint32_t slot,
    lc_binding_set *set);

/**
 * Bind a vertex buffer in the open explicit pass.
 */
LC_API lc_result lc_encoder_bind_vertex_buffer(
    lc_command_encoder *encoder,
    uint32_t binding,
    lc_buffer *buffer,
    uint64_t offset);

/**
 * Bind an index buffer in the open explicit pass.
 */
LC_API lc_result lc_encoder_bind_index_buffer(
    lc_command_encoder *encoder,
    lc_buffer *buffer,
    uint64_t offset,
    lc_index_type index_type);

/**
 * Push constants in the open explicit pass. The bound pipeline must
 * equal `pipeline`; the window must fit a declared range.
 */
LC_API lc_result lc_encoder_push_constants(
    lc_command_encoder *encoder,
    lc_pipeline *pipeline,
    uint32_t visibility,
    uint32_t offset,
    uint32_t size,
    const void *data);

/* Backend-neutral 2D scissor rectangle (Phase 33: GUI clipping).
 * Integer target pixels: offset >= 0, extent > 0, and the rect must
 * fit inside the open pass extent (offset + extent <= pass extent).
 * Partially-offscreen GUI rects must be clamped by the CALLER (the
 * editor GUI bridge clamps ImGui clip rects); out-of-range rects are
 * rejected, never silently wrapped. */
typedef struct lc_scissor_rect {
    int32_t offset_x;
    int32_t offset_y;
    uint32_t width;
    uint32_t height;
} lc_scissor_rect;

/**
 * Set the scissor rectangle for subsequent draws in the open
 * explicit pass (Phase 33: editor GUI clip rects). Requires an open
 * pass on a live frame; the rect must fit the pass extent (see
 * lc_scissor_rect). The pass begin resets scissor to the full target
 * (pass width x height at offset 0,0), so every frame starts
 * unclipped. Worker-list recording rejects this call (GUI draws run
 * on the primary encoder only — worker lists inherit the primary's
 * full-target pass state at execute time).
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL encoder, dead/no-frame
 *         encoder, no open pass, rect outside the pass extent,
 *         worker-mode encoder).
 */
LC_API lc_result lc_encoder_set_scissor(lc_command_encoder *encoder,
                                        const lc_scissor_rect *rect);

/**
 * Query the current scissor rectangle of the open explicit pass
 * (1 + fill, 0 when unavailable; out may be NULL for a probe).
 * Reports the full-target rect right after pass begin.
 */
LC_API int lc_encoder_get_scissor(const lc_command_encoder *encoder,
                                  lc_scissor_rect *out_rect);

/**
 * Draw in the open explicit pass. Requires a bound compatible
 * pipeline (and index buffer for indexed draws).
 */
LC_API lc_result lc_encoder_draw(
    lc_command_encoder *encoder,
    uint32_t vertex_count,
    uint32_t first_vertex);

LC_API lc_result lc_encoder_draw_indexed(
    lc_command_encoder *encoder,
    uint32_t index_count,
    uint32_t instance_count,
    uint32_t first_index,
    int32_t vertex_offset,
    uint32_t first_instance);

LC_API lc_result lc_encoder_draw_instanced(
    lc_command_encoder *encoder,
    uint32_t vertex_count,
    uint32_t instance_count,
    uint32_t first_vertex,
    uint32_t first_instance);

/* -------------------------------------------------------------------------
 * Resource states + explicit transitions (Phase 19: AAA
 * synchronization foundation).
 *
 * Backend-neutral semantic states. LumaC converts them to
 * backend synchronization (Vulkan: layouts, stages, access masks)
 * internally — those never cross this header, so the same states
 * map onto future D3D12 barriers. No state names any queue;
 * graphics/compute/transfer scheduling arrives later without
 * replacing this model.
 * ------------------------------------------------------------------------- */
typedef enum lc_resource_state {
    LC_RESOURCE_STATE_UNDEFINED = 0,
    LC_RESOURCE_STATE_COLOR_ATTACHMENT_WRITE = 1,
    LC_RESOURCE_STATE_DEPTH_ATTACHMENT_WRITE = 2,
    LC_RESOURCE_STATE_SHADER_READ = 3,
    /* General read/write (mapped, reserved for future storage/
     * compute use; valid to name, tracked, transitioned). */
    LC_RESOURCE_STATE_SHADER_READ_WRITE = 4,
    LC_RESOURCE_STATE_TRANSFER_SRC = 5,
    LC_RESOURCE_STATE_TRANSFER_DST = 6,
    /* Swapchain presentation state (swapchain images are untracked;
     * present for completeness and future use). */
    LC_RESOURCE_STATE_PRESENT = 7,
    /* Buffer-oriented states (tracked when buffer-state tracking
     * lands with compute/indirect work; valid enum values today so
     * the abstraction never needs replacing). */
    LC_RESOURCE_STATE_VERTEX_READ = 8,
    LC_RESOURCE_STATE_INDEX_READ = 9,
    LC_RESOURCE_STATE_UNIFORM_READ = 10,
    LC_RESOURCE_STATE_STORAGE_READ = 11,
    LC_RESOURCE_STATE_STORAGE_WRITE = 12,
    LC_RESOURCE_STATE_INDIRECT_READ = 13,
    /* Phase 23: read-modify-write storage (compute-ordered read +
     * write access; the D3D12 UAV-barrier equivalent). Buffers that
     * one dispatch both reads and writes (e.g. GPU LOD history)
     * live in this state across frames. */
    LC_RESOURCE_STATE_STORAGE_READ_WRITE = 14
} lc_resource_state;

/* One subresource range: mip/layer subset (cubemap faces are array
 * layers). Counts are exact (>= 1); whole-image callers pass the
 * full range explicitly. */
typedef struct lc_image_subresource_range {
    uint32_t base_mip_level;
    uint32_t level_count;
    uint32_t base_array_layer;
    uint32_t layer_count;
} lc_image_subresource_range;

/**
 * Record an explicit image transition into the open frame's command
 * buffer (no immediate submit). The old state comes from LumaC
 * tracking — callers name only the destination (PART F). Updates
 * tracking immediately (recorded-not-executed discipline, same as
 * render-pass finals). Allowed between passes only: never inside
 * an open pass (explicit barriers are illegal in render-pass
 * instances) and never outside an open frame.
 *
 * Suitable states for images: UNDEFINED, COLOR_ATTACHMENT_WRITE,
 * DEPTH_ATTACHMENT_WRITE, SHADER_READ, SHADER_READ_WRITE,
 * TRANSFER_SRC, TRANSFER_DST. Buffer-only states and PRESENT are
 * rejected for images.
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL handles, dead objects, no
 *         open frame, open pass, bad/overflowing range, bad state
 *         for images).
 */
LC_API lc_result lc_encoder_transition_image(
    lc_command_encoder *encoder,
    lc_image *image,
    const lc_image_subresource_range *range,
    lc_resource_state new_state);

/* -------------------------------------------------------------------------
 * GPU memory statistics + introspection (Phase 19: AAA allocator).
 *
 * Backend-neutral committed/used accounting over allocator memory
 * classes. device_local covers GPU-resident blocks; host_visible
 * covers CPU-visible upload/readback blocks. free/largest-free
 * expose fragmentation; counts expose sharing (resource_count >>
 * block_count proves suballocation). D3D12 maps these onto heap
 * categories (DEFAULT / UPLOAD / READBACK) later.
 * ------------------------------------------------------------------------- */
typedef struct lc_memory_stats {
    uint64_t device_local_allocated; /* committed block bytes */
    uint64_t device_local_used;      /* live suballocation bytes */
    uint64_t device_local_free;      /* allocated - used */
    uint64_t host_visible_allocated;
    uint64_t host_visible_used;
    uint64_t host_visible_free;
    uint64_t allocation_count;       /* live suballocations */
    uint64_t block_count;            /* live VkDeviceMemory blocks */
    uint64_t dedicated_allocation_count;
    uint64_t largest_free_range;     /* biggest single free range */
} lc_memory_stats;

/**
 * Copy out allocator statistics (zeros for NULL device or NULL out
 * handling: NULL device zeroes *out when provided; NULL out is a
 * no-op).
 */
LC_API void lc_device_get_memory_stats(
    const lc_device *device,
    lc_memory_stats *out_stats);

/* Backend-neutral memory class for one resource (D3D12-mappable). */
typedef enum lc_memory_class {
    LC_MEMORY_CLASS_UNKNOWN = 0,
    LC_MEMORY_CLASS_DEVICE_LOCAL = 1, /* GPU-resident */
    LC_MEMORY_CLASS_UPLOAD = 2,       /* CPU->GPU visible */
    LC_MEMORY_CLASS_READBACK = 3      /* GPU->CPU visible */
} lc_memory_class;

/* Per-resource memory introspection (no backend handles). */
typedef struct lc_resource_memory_info {
    uint64_t requested_size;  /* resource size as created */
    uint64_t allocation_size; /* aligned suballocation (or whole) */
    int dedicated;            /* nonzero: own VkDeviceMemory */
    lc_memory_class memory_class;
} lc_resource_memory_info;

/**
 * Copy out one buffer's memory info (zeros for NULL/dead buffer;
 * out may be NULL for a no-op).
 */
LC_API void lc_buffer_get_memory_info(
    const lc_buffer *buffer,
    lc_resource_memory_info *out_info);

/**
 * Copy out one image's memory info (zeros for NULL/dead image;
 * out may be NULL for a no-op).
 */
LC_API void lc_image_get_memory_info(
    const lc_image *image,
    lc_resource_memory_info *out_info);

/* One heap's budget (backend-neutral; unknown when unsupported). */
typedef struct lc_memory_heap_budget {
    uint64_t budget; /* usable budget bytes, 0 when unknown */
    uint64_t usage;  /* current usage bytes, 0 when unknown */
} lc_memory_heap_budget;

#define LC_MAX_MEMORY_HEAPS 16

/* Memory budget query (Vulkan: VK_EXT_memory_budget when present;
 * never faked — unknown reports available == 0 with zero heaps). */
typedef struct lc_memory_budget {
    int available; /* nonzero when backend reported real numbers */
    uint32_t heap_count;
    lc_memory_heap_budget heaps[LC_MAX_MEMORY_HEAPS];
} lc_memory_budget;

/**
 * Copy out memory budget (zeros for NULL device or NULL out
 * handling as above; available == 0 when unsupported).
 */
LC_API void lc_device_get_memory_budget(
    const lc_device *device,
    lc_memory_budget *out_budget);

/* -------------------------------------------------------------------------
 * Multithreaded recording, queues, async transfer (Phase 20).
 *
 * Thread-safety contract (binding, PART H):
 * - independent buffer create/map/destroy calls are thread-safe (the shared
 *   allocator, stable-ID issuer, retirement queue, and buffer registry are
 *   synchronized); other resource lifecycle APIs retain their section's
 *   main-thread contract;
 * - immutable queries/stats are thread-safe while the queried handle's
 *   lifetime is externally stable;
 * - command recording on DIFFERENT worker encoders: thread-safe;
 * - the SAME encoder from two threads: NOT thread-safe;
 * - destroying an object referenced by an unexecuted command list:
 *   safe (list fails loudly at execute; physical retirement is
 *   deferred until GPU completion);
 * - immediate (synchronous) transfer/upload/readback calls serialize
 *   internally and are thread-safe; overlapping same-image access
 *   across threads without ordering is an application bug, as in
 *   raw Vulkan.
 * ------------------------------------------------------------------------- */

/* Backend-neutral queue types (no family indices publicly; a future
 * D3D12 backend maps these onto direct/copy/compute queues).
 * COMPUTE is exposed for forward compatibility; async compute
 * scheduling itself is deferred. */
typedef enum lc_queue_type {
    LC_QUEUE_GRAPHICS = 0,
    LC_QUEUE_TRANSFER = 1,
    LC_QUEUE_COMPUTE = 2
} lc_queue_type;

/* Backend-neutral queue diagnostics (editor/MCP-friendly). */
typedef struct lc_queue_info {
    int available;            /* queue type usable on this device */
    int dedicated;            /* nonzero: own family (transfer only) */
    uint32_t count;           /* retrieved queues of this type */
    int async_supported;      /* nonzero: overlaps graphics work */
} lc_queue_info;

/**
 * Copy out queue diagnostics (zeros for NULL device or unknown
 * type; out may be NULL for a no-op).
 */
LC_API void lc_device_get_queue_info(
    const lc_device *device,
    lc_queue_type type,
    lc_queue_info *out_info);

/* -------------------------------------------------------------------------
 * GPU completion tokens (Phase 20, PART Q).
 *
 * Value-based (timeline-style) completion: submissions produce
 * monotonically increasing values; polling/waiting observes them.
 * Maps to Vulkan timeline semaphores where supported (preferred)
 * and to D3D12 fences later; binary-fence fallback where timeline
 * semaphores are unavailable. A zero value means "nothing" (always
 * complete).
 * ------------------------------------------------------------------------- */
typedef struct lc_gpu_signal {
    uint64_t value;
} lc_gpu_signal;

/* Infinite wait (lc_gpu_signal_wait only). */
#define LC_TIMEOUT_INFINITE (~(uint64_t)0)

/** Nonzero when the signal has completed (1 for zero values and
 *  NULL device handling: NULL device reports not-ready... actually
 *  zero signals are complete; NULL device returns 0). */
LC_API int lc_gpu_signal_is_ready(lc_device *device,
                                  lc_gpu_signal signal);

/**
 * Block until the signal completes or timeout_ns elapses.
 *
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL device),
 *         LC_ERROR_TIMEOUT (bounded wait expired).
 */
LC_API lc_result lc_gpu_signal_wait(lc_device *device,
                                    lc_gpu_signal signal,
                                    uint64_t timeout_ns);

/* -------------------------------------------------------------------------
 * Worker recording contexts + command lists (Phase 20, PARTs A–E).
 *
 * A worker encoder records on exactly one thread at a time;
 * different encoders record concurrently with no global recording
 * mutex (one command pool per encoder, PART C; buffers recycled
 * across begins, PART D). Lists are finished secondary-style work
 * executed deterministically inside a frame encoder's open pass
 * (batch submission, PART 28). Only graphics-queue recording
 * exists in Phase 20 (other queue types: UNSUPPORTED, reserved).
 *
 * Recordable into worker lists: pipeline/set/vertex/index binds,
 * push constants, draws, image transitions. Render passes are
 * opened by the executing primary, never inside a list.
 * ------------------------------------------------------------------------- */

/* Opaque finished command list. Never dereference. */
typedef struct lc_command_list lc_command_list;

/* Worker encoder creation parameters. LC_QUEUE_GRAPHICS records
 * graphics work; LC_QUEUE_COMPUTE records compute work on the
 * baseline graphics-queue path (Phase 21). */
typedef struct lc_command_encoder_desc {
    lc_queue_type queue;
} lc_command_encoder_desc;

/**
 * Create a worker recording context on a device. Requires lc_init()
 * first and a live device. Independent command pool; safe to record
 * on its own thread while other encoders record elsewhere.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL device/desc/out, dead
 *         device, non-graphics queue), LC_ERROR_OUT_OF_MEMORY.
 */
LC_API lc_result lc_command_encoder_create(
    lc_device *device,
    const lc_command_encoder_desc *desc,
    lc_command_encoder **out_encoder);

/**
 * Destroy a worker encoder (not frame-borrowed ones: those die with
 * their swapchain). Safe with NULL. Unexecuted lists created from
 * it must be destroyed first (INVALID_ARGUMENT otherwise... no:
 * lists hold their own pool references; destroying the encoder
 * first invalidates unexecuted lists, and executing them afterwards
 * fails safely — documented, tested).
 */
LC_API void lc_command_encoder_destroy(lc_command_encoder *encoder);

/**
 * Begin recording a list against an offscreen target + pass shape
 * (provides render-pass/framebuffer inheritance for the secondary
 * buffer; no pass opens on the worker). The target must stay alive
 * through execute. Exactly one open list per encoder at a time.
 * Width/height must equal the target extent; views must be the
 * target's attachments in order (same rules as
 * lc_encoder_begin_render_pass). Swapchain targets are rejected
 * (presentation stays primary-only in Phase 20).
 */
LC_API lc_result lc_command_list_begin(
    lc_command_encoder *encoder,
    lc_render_target *target,
    const lc_render_pass_desc *desc);

/**
 * Finish recording; the encoder becomes reusable for the next list
 * (command buffer recycled). The list borrows nothing: referenced
 * handles are logged by identity for execute-time validation.
 */
LC_API lc_result lc_command_encoder_finish(
    lc_command_encoder *encoder,
    lc_command_list **out_list);

/**
 * Destroy a finished list, recycling its command buffer. Safe with
 * NULL. Executing afterwards fails safely (never crashes).
 */
LC_API void lc_command_list_destroy(lc_command_list *list);

/**
 * Execute finished lists, in order, inside the primary's currently
 * open explicit pass. Signatures must be mutually compatible (same
 * target family: validated structurally; mismatch is
 * PIPELINE_INCOMPATIBLE... INVALID_ARGUMENT). State intents
 * reconcile in execution order; conflicts fail loudly with
 * INVALID_ARGUMENT and record nothing further. Submitting the
 * frame afterwards yields ONE completion covering all lists.
 */
LC_API lc_result lc_encoder_execute_lists(
    lc_command_encoder *primary,
    lc_command_list *const *lists,
    uint32_t list_count);

/**
 * Query the frame-flight slot a frame-borrowed encoder records
 * into (Phase 21, per-flight GPU-driven resources). out_index
 * receives the current slot, out_count the slot count (either may
 * be NULL). Worker encoders are rejected (INVALID_ARGUMENT); the
 * slot is stable between lc_begin_frame and lc_end_frame on the
 * owning swapchain.
 */
LC_API lc_result lc_encoder_get_flight_slot(
    lc_command_encoder *encoder,
    uint32_t *out_index,
    uint32_t *out_count);

/* -------------------------------------------------------------------------
 * Async transfer: uploads, readbacks, staging, retirement
 * (Phase 20, PARTs S–Z, AA–AE).
 *
 * Scheduling never blocks on the GPU (bounded only by the staging
 * cap, which waits for the oldest completion). Source data is
 * copied into Luma-owned staging BEFORE return, so caller buffers
 * may be reused immediately (PART S lifetime rule). Staging is
 * reclaimed only after GPU completion (PART U); pressure waits
 * for the oldest in-flight request (PART V).
 * ------------------------------------------------------------------------- */

/**
 * Schedule an async buffer upload (device + offset validated like
 * lc_buffer_write; destination may be any placement). Returns a
 * signal for polling/waiting. Never waits on the GPU.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL device/buffer, NULL data
 *         with nonzero size, dead objects, out-of-range write),
 *         LC_ERROR_OUT_OF_MEMORY (staging).
 */
LC_API lc_result lc_upload_buffer_async(
    lc_device *device,
    lc_buffer *dst,
    uint64_t dst_offset,
    const void *data,
    uint64_t size,
    lc_gpu_signal *out_signal);

/* Async image upload region (mirrors lc_image_upload_desc). */
typedef struct lc_image_upload_async_desc {
    uint32_t mip_level;
    uint32_t array_layer;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    const void *data;
    uint64_t data_size;
} lc_image_upload_async_desc;

/**
 * Schedule an async image upload (validated like lc_image_write).
 * Returns a signal for polling/waiting. Never waits on the GPU.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (bad handles/range/size/usage),
 *         LC_ERROR_OUT_OF_MEMORY (staging).
 */
LC_API lc_result lc_upload_image_async(
    lc_device *device,
    lc_image *dst,
    const lc_image_upload_async_desc *upload,
    lc_gpu_signal *out_signal);

/* Opaque async readback request. Never dereference. */
typedef struct lc_readback_request lc_readback_request;

/**
 * Schedule an async image readback (validated like the sync
 * query). Layout semantics match the synchronous call (tightly
 * packed deterministic rows). Never waits on the GPU.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT, LC_ERROR_OUT_OF_MEMORY.
 */
LC_API lc_result lc_image_readback_async(
    lc_device *device,
    lc_image *image,
    const lc_image_readback_desc *desc,
    lc_readback_request **out_request);

/** Nonzero when the request's GPU copy has completed. */
LC_API int lc_readback_request_is_ready(
    lc_readback_request *request);

/**
 * Block until completion or timeout_ns elapses.
 *
 * @return LC_SUCCESS, LC_ERROR_INVALID_ARGUMENT (NULL request),
 *         LC_ERROR_TIMEOUT (bounded wait expired).
 */
LC_API lc_result lc_readback_request_wait(
    lc_readback_request *request,
    uint64_t timeout_ns);

/**
 * Copy the result into tightly packed caller memory (dst rules
 * mirror lc_image_readback, including sizing queries). Requires * completion (INVALID_ARGUMENT otherwise... no: waits are the
 * caller's job — mapping an incomplete request returns
 * LC_ERROR_INVALID_ARGUMENT; document, test).
 */
LC_API lc_result lc_readback_request_map(
    lc_readback_request *request,
    void *dst,
    size_t dst_size,
    size_t *out_required_size);

/**
 * Destroy a request, releasing staging (waits for its own
 * completion first — never frees in-flight GPU memory). Safe with
 * NULL.
 */
LC_API void lc_readback_request_destroy(lc_readback_request *request);

/* Transfer + retirement diagnostics (editor/streaming profilers,
 * MCP futures). Counters are lifetime-monotonic except *_used,
 * *_in_flight, and *_pending (point-in-time). */
typedef struct lc_transfer_stats {
    uint64_t graphics_submissions;
    uint64_t transfer_submissions;
    uint64_t bytes_uploaded;
    uint64_t bytes_read_back;
    uint64_t staging_committed;   /* pool bytes backing staging */
    uint64_t staging_used;        /* live staged bytes */
    uint64_t staging_high_water;  /* max live staged bytes */
    uint64_t uploads_in_flight;
    uint64_t command_lists_in_flight;
    uint64_t retired_pending;     /* logically dead, GPU-held */
    uint64_t retired_high_water;  /* max pending observed */
    /* Phase 22 concurrency/pressure counters. */
    uint64_t pending_transfers;   /* linked in-flight entries now */
    uint64_t staging_waits;       /* lifetime pressure waits taken */
    uint64_t reclaimed_transfers; /* lifetime entries unlinked */
    uint64_t retired_completed;   /* lifetime retirements executed */
    uint64_t emit_retries;        /* lifetime emit-capacity retries */
    int timeline_active;          /* nonzero: timeline completion */
    int binary_fallback_active;   /* nonzero: fence fallback */
} lc_transfer_stats;

/**
 * Copy out transfer/retirement diagnostics (zeros for NULL device;
 * out may be NULL for a no-op).
 */
LC_API void lc_device_get_transfer_stats(
    const lc_device *device,
    lc_transfer_stats *out_stats);

/**
 * Reclaim completed staging/retirement work without submitting
 * anything (optional progress pump; submission and waits already
 * reclaim lazily). Never blocks.
 */
LC_API void lc_device_poll_completed(lc_device *device);

/* -------------------------------------------------------------------------
 * Compute pipelines, dispatch, and indirect drawing (Phase 21).
 *
 * Compute shaders are created through the same lc_shader_create path
 * with LC_SHADER_STAGE_COMPUTE. Compute pipelines bind binding
 * layouts + push ranges exactly like graphics pipelines (no
 * render-target signature) and participate in the same pipeline
 * cache. Dispatch is recorded on frame encoders and on compute
 * worker lists; on hardware without a separate compute queue it
 * executes on the graphics queue (baseline compatibility path).
 *
 * Scheduling model (binding): async compute overlapping graphics is
 * a later phase. The queue model already exposes LC_QUEUE_COMPUTE
 * and GPU-side completion values, so no API change is needed when
 * overlapping lands.
 * ------------------------------------------------------------------------- */

/* Opaque compute-pipeline handle. Never dereference. */
typedef struct lc_compute_pipeline lc_compute_pipeline;

/* Compute pipeline creation parameters. The shader must be a live
 * LC_SHADER_STAGE_COMPUTE module on the same device. Binding
 * layouts are ordered by slot, exactly like graphics pipelines.
 * Push ranges use lc_shader_visibility bits (VERTEX/FRAGMENT/COMPUTE
 * as appropriate; compute pipelines normally use COMPUTE). */
typedef struct lc_compute_pipeline_desc {
    lc_shader *compute_shader;
    const lc_binding_layout *const *binding_layouts;
    uint32_t binding_layout_count;
    const lc_push_constant_range *push_constant_ranges;
    uint32_t push_constant_range_count;
} lc_compute_pipeline_desc;

/**
 * Create a compute pipeline on a device. Participates in the same
 * binary pipeline cache as graphics pipelines.
 *
 * @return LC_SUCCESS, LC_ERROR_NOT_INITIALIZED,
 *         LC_ERROR_INVALID_ARGUMENT (NULL device/desc/shader/out,
 *         dead objects, non-compute shader, bad ranges/layouts),
 *         LC_ERROR_UNSUPPORTED (compute not supported),
 *         LC_ERROR_OUT_OF_MEMORY, LC_ERROR_PIPELINE_CREATION_FAILED.
 */
LC_API lc_result lc_compute_pipeline_create(
    lc_device *device,
    const lc_compute_pipeline_desc *desc,
    lc_compute_pipeline **out_pipeline);

/** Destroy a compute pipeline. Safe with NULL. In-flight GPU use
 *  retires through the existing deferred-retirement queue. */
LC_API void lc_compute_pipeline_destroy(lc_compute_pipeline *pipeline);

/**
 * Bind a compute pipeline for dispatch. Frame encoders: allowed
 * outside an open pass only (dispatch inside a render pass is
 * rejected). Worker compute lists: always allowed while open.
 */
LC_API lc_result lc_encoder_bind_compute_pipeline(
    lc_command_encoder *encoder,
    lc_compute_pipeline *pipeline);

/**
 * Dispatch compute workgroups (x, y, z). Each count must be >= 1
 * and within the device compute limits (see
 * lc_device_get_compute_capabilities). Requires a bound compute
 * pipeline. Storage/invocation out-of-range behavior is the
 * shader's contract (guard `id >= count`).
 */
LC_API lc_result lc_encoder_dispatch(lc_command_encoder *encoder,
                                     uint32_t x, uint32_t y, uint32_t z);

/**
 * Bind a descriptor set to a compute pipeline slot (same
 * content-matching rules as lc_encoder_bind_binding_set, against
 * the compute pipeline's canonical signatures). Frame encoders:
 * allowed outside an open pass. Worker compute lists: always
 * allowed while open.
 */
LC_API lc_result lc_encoder_bind_compute_set(
    lc_command_encoder *enc,
    lc_compute_pipeline *pipeline,
    uint32_t slot,
    lc_binding_set *set);

/**
 * Push constants to a compute pipeline (visibility must fit the
 * pipeline's ranges; COMPUTE visibility typical). Same alignment
 * and range rules as lc_encoder_push_constants.
 */
LC_API lc_result lc_encoder_push_compute_constants(
    lc_command_encoder *enc,
    lc_compute_pipeline *pipeline,
    uint32_t visibility,
    uint32_t offset,
    uint32_t size,
    const void *data);

/**
 * Begin a compute worker list (dispatch/buffer-transition/push
 * recording without render-pass inheritance). Exactly one open
 * list per encoder. Execute only when the primary has no open
 * pass, and never mixed with graphics lists in one batch.
 */
LC_API lc_result lc_command_list_begin_compute(
    lc_command_encoder *encoder);

/**
 * Transition a buffer to a semantic state (compute/indirect
 * ordering). Frame encoders record a barrier immediately
 * (recorded-not-executed discipline, like image transitions);
 * worker lists log the intent for execute-time reconciliation.
 * Allowed between passes only: never inside an open pass
 * (explicit barriers are illegal in render-pass instances).
 */
LC_API lc_result lc_encoder_transition_buffer(
    lc_command_encoder *enc,
    lc_buffer *buffer,
    lc_resource_state new_state);

/* -------------------------------------------------------------------------
 * Indirect drawing (Phase 21).
 *
 * Layouts are explicit-width and backend-neutral; the Vulkan backend
 * asserts structural compatibility with VkDrawIndirectCommand /
 * VkDrawIndexedIndirectCommand internally. D3D12 maps these onto
 * indirect-argument structures without API change.
 * ------------------------------------------------------------------------- */

/* Non-indexed indirect draw command (one element per draw). */
typedef struct lc_indirect_draw_command {
    uint32_t vertex_count;
    uint32_t instance_count;
    uint32_t first_vertex;
    uint32_t first_instance;
} lc_indirect_draw_command;

/* Indexed indirect draw command (one element per draw). */
typedef struct lc_indirect_draw_indexed_command {
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    int32_t vertex_offset;
    uint32_t first_instance;
} lc_indirect_draw_indexed_command;

/**
 * Record a non-indexed indirect draw batch. `buffer` must carry
 * INDIRECT_READ-capable usage and hold at least
 * offset + draw_count * stride bytes (stride >= sizeof command;
 * stride must be a multiple of 4). Requires a bound graphics
 * pipeline and an open pass (same rules as lc_encoder_draw).
 * Multi-draw executes natively when supported, else as a
 * compatibility loop of single indirect draws.
 */
LC_API lc_result lc_encoder_draw_indirect(lc_command_encoder *encoder,
                                          lc_buffer *buffer,
                                          uint64_t offset,
                                          uint32_t draw_count,
                                          uint32_t stride);

/**
 * Record an indexed indirect draw batch (same rules as the
 * non-indexed form, plus a bound index buffer).
 */
LC_API lc_result lc_encoder_draw_indexed_indirect(
    lc_command_encoder *encoder,
    lc_buffer *buffer,
    uint64_t offset,
    uint32_t draw_count,
    uint32_t stride);

/**
 * Record a non-indexed indirect draw with a GPU-written draw count
 * (Phase 23). `buffer` follows the fixed-count rules above;
 * `count_buffer` must carry INDIRECT usage, hold at least
 * count_offset + 4 bytes (count_offset must be a multiple of 4),
 * and both buffers must already be INDIRECT_READ (barriers stay
 * outside passes). `max_draw_count` bounds execution:
 * at most max_draw_count commands starting at `offset` run, and
 * the GPU count is clamped to it.
 *
 * Where lc_compute_capabilities.indirect_count is nonzero the
 * backend consumes the GPU count natively; otherwise it draws all
 * max_draw_count commands, so producers must leave unused slots
 * with instance_count 0 (valid no-op draws). Either way no CPU
 * readback of the count is required.
 */
LC_API lc_result lc_encoder_draw_indirect_count(
    lc_command_encoder *encoder,
    lc_buffer *buffer,
    uint64_t offset,
    lc_buffer *count_buffer,
    uint64_t count_offset,
    uint32_t max_draw_count,
    uint32_t stride);

/**
 * Record an indexed indirect draw with a GPU-written draw count
 * (same rules as the non-indexed count form, plus a bound index
 * buffer).
 */
LC_API lc_result lc_encoder_draw_indexed_indirect_count(
    lc_command_encoder *encoder,
    lc_buffer *buffer,
    uint64_t offset,
    lc_buffer *count_buffer,
    uint64_t count_offset,
    uint32_t max_draw_count,
    uint32_t stride);

/* -------------------------------------------------------------------------
 * Compute/indirect capabilities (Phase 21, backend-neutral).
 * ------------------------------------------------------------------------- */

/* Backend-neutral compute/indirect capability report. */
typedef struct lc_compute_capabilities {
    int compute_supported;      /* dispatch path usable */
    int indirect_draw_supported;
    int multi_draw_indirect;    /* native N-command indirect */
    int indirect_count;         /* GPU-count indirect variant */
    int dedicated_compute;      /* nonzero: own compute queue family */
    uint32_t max_workgroup_size[3];
    uint32_t max_workgroup_count[3];
    uint32_t max_workgroup_invocations;
    uint32_t max_shared_memory; /* bytes of workgroup-local memory */
    uint32_t max_push_size;     /* push-constant bytes */
    uint32_t subgroup_size;     /* 0 when unknown */
} lc_compute_capabilities;

/**
 * Copy out compute/indirect capabilities (zeros for NULL device or
 * NULL out handling: NULL device zeroes *out when provided; NULL
 * out is a no-op).
 */
LC_API void lc_device_get_compute_capabilities(
    const lc_device *device,
    lc_compute_capabilities *out_caps);

/* -------------------------------------------------------------------------
 * Resource identity (Phase 18: ABA hardening).
 *
 * Every LumaC resource carries a stable unique identity assigned at
 * creation that is NEVER reused, even when the allocator recycles the
 * same wrapper pointer address after a destroy/create cycle. Pointer
 * equality is NOT resource identity: caches (notably renderer
 * binding caches) must compare these IDs, never raw pointers.
 * Zero means "no resource" (NULL handle).
 * ------------------------------------------------------------------------- */

/* Generic stable resource identity (monotonic per process). */
typedef uint64_t lc_resource_id;

/** Stable ID of an image (0 for NULL/dead). */
LC_API lc_resource_id lc_image_get_resource_id(const lc_image *image);

/** Stable ID of an image view (0 for NULL/dead). */
LC_API lc_resource_id lc_image_view_get_resource_id(
    const lc_image_view *view);

/** Stable ID of a buffer (0 for NULL/dead). */
LC_API lc_resource_id lc_buffer_get_resource_id(const lc_buffer *buffer);

/** Stable ID of a sampler (0 for NULL/dead). */
LC_API lc_resource_id lc_sampler_get_resource_id(
    const lc_sampler *sampler);

/** Stable ID of a render target (0 for NULL/dead; borrowed swapchain
 *  targets report a stable per-swapchain ID that survives recreation). */
LC_API lc_resource_id lc_render_target_get_resource_id(
    const lc_render_target *target);

/** Stable ID of a pipeline (0 for NULL/dead). */
LC_API lc_resource_id lc_pipeline_get_resource_id(
    const lc_pipeline *pipeline);

/** Stable ID of a binding set (0 for NULL/dead). */
LC_API lc_resource_id lc_binding_set_get_resource_id(
    const lc_binding_set *set);

/** Stable ID of a binding layout (0 for NULL/dead). */
LC_API lc_resource_id lc_binding_layout_get_resource_id(
    const lc_binding_layout *layout);

/**
 * Borrow the image a view was created from (do not destroy).
 * Needed so public readback (which takes lc_image*) can inspect
 * renderer-borrowed views (HDR, shadow, environment, BRDF) without
 * backend-private access. Returns NULL for NULL/dead views.
 */
LC_API lc_image *lc_image_view_get_image(lc_image_view *view);

#ifdef __cplusplus
}
#endif

#endif /* LUMAC_H */
