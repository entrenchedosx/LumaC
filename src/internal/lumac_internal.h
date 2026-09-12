#ifndef LUMAC_INTERNAL_H
#define LUMAC_INTERNAL_H

#include "lumac/lumac.h"

/*
 * Internal LumaC state - not exposed in the public header.
 * `windows` is the head of a small doubly-linked list of live windows
 * (links live inside struct lc_window, see src/platform/platform.h).
 * `devices` is the head of a small doubly-linked list of live graphics
 * devices (links live inside struct lc_device, see
 * src/graphics/graphics_internal.h).
 * `surfaces` is the head of a small doubly-linked list of live
 * presentation surfaces (links live inside struct lc_surface, see
 * src/graphics/graphics_internal.h). A surface depends on both its
 * device and its window.
 * `swapchains` is the head of a small doubly-linked list of live
 * presentation swapchains (links live inside struct lc_swapchain, see
 * src/graphics/graphics_internal.h). A swapchain depends on its device
 * and surface.
 * lc_shutdown() drains the lists in dependency order: swapchains, then
 * surfaces, devices, windows - destroying a VkSwapchainKHR needs its
 * device alive, and destroying a VkSurfaceKHR needs its device's
 * Vulkan instance, so dependents always die first.
 */
typedef struct lc_state {
    int initialized;
    lc_window *windows;
    lc_device *devices;
    lc_surface *surfaces;
    lc_swapchain *swapchains;
} lc_state;

/* Accessor for the single process-wide state (see src/lumac.c) */
lc_state *lc_get_internal_state(void);

/* Destroys all live windows. Called by lc_shutdown(). See src/window.c. */
void lc_window_destroy_all(void);

/* Destroys all live graphics devices. Called by lc_shutdown(). See src/graphics/graphics.c. */
void lc_device_destroy_all(void);

/* Destroys all live surfaces. Called by lc_shutdown(). See src/graphics/surface.c. */
void lc_surface_destroy_all(void);

/* Destroys all surfaces referencing a device being torn down.
 * Called by lc_device_destroy(). See src/graphics/surface.c. */
void lc_surface_destroy_for_device(const lc_device *device);

/* Destroys all surfaces referencing a window being torn down.
 * Called by lc_window_destroy(). See src/graphics/surface.c. */
void lc_surface_destroy_for_window(const lc_window *window);

/* Destroys all live swapchains. Called by lc_shutdown().
 * See src/graphics/swapchain.c. */
void lc_swapchain_destroy_all(void);

/* Destroys all swapchains referencing a surface being torn down.
 * Called by lc_surface_destroy(). See src/graphics/swapchain.c. */
void lc_swapchain_destroy_for_surface(const lc_surface *surface);

/* Destroys all swapchains using a device being torn down.
 * Called by lc_device_destroy(). See src/graphics/swapchain.c. */
void lc_swapchain_destroy_for_device(const lc_device *device);

/* Destroys all swapchains whose surface belongs to a window being torn
 * down. Called by lc_window_destroy(). See src/graphics/swapchain.c. */
void lc_swapchain_destroy_for_window(const lc_window *window);

/* Default title used when lc_window_desc.title is NULL */
#define LC_DEFAULT_TITLE "LumaC"

#endif /* LUMAC_INTERNAL_H */
