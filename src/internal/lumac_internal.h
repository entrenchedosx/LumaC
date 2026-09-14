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
 * `shaders` tracks live shader modules; `pipelines` tracks live
 * graphics pipelines (each anchored to its creation swapchain).
 * lc_shutdown() drains the lists in dependency order: pipelines, then
 * shaders, swapchains, surfaces, devices, windows - destroying a
 * VkSwapchainKHR needs its device alive, and destroying a VkSurfaceKHR
 * needs its device's Vulkan instance, so dependents always die first.
 */
typedef struct lc_state {
    int initialized;
    uint64_t next_resource_id; /* atomically issued, starts at 1 */
    lc_window *windows;
    lc_device *devices;
    lc_surface *surfaces;
    lc_swapchain *swapchains;
    lc_shader *shaders;
    lc_pipeline *pipelines;
    lc_compute_pipeline *compute_pipelines;
    lc_buffer *buffers;
    lc_image *images;
    lc_image_view *image_views;
    lc_sampler *samplers;
    lc_binding_layout *binding_layouts;
    lc_binding_set *binding_sets;
    lc_render_target *render_targets;
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

/* Destroys all live shaders. Called by lc_shutdown().
 * See src/graphics/shader.c. */
void lc_shader_destroy_all(void);

/* Destroys all shaders owned by a device being torn down.
 * Called by lc_device_destroy(). See src/graphics/shader.c. */
void lc_shader_destroy_for_device(const lc_device *device);

/* Destroys all live pipelines. Called by lc_shutdown().
 * See src/graphics/pipeline.c. */
void lc_pipeline_destroy_all(void);

/* Destroys all pipelines owned by a device being torn down.
 * Called by lc_device_destroy(). See src/graphics/pipeline.c. */
void lc_pipeline_destroy_for_device(const lc_device *device);

/* Nonzero when a pipeline handle is live. Shared by frame.c for
 * bind/draw validation. See src/graphics/pipeline.c. */
int lc_pipeline_is_live(const lc_pipeline *pipeline);

/* Destroys all live compute pipelines. Called by lc_shutdown().
 * See src/graphics/compute_pipeline.c. */
void lc_compute_pipeline_destroy_all(void);

/* Destroys all live buffers. Called by lc_shutdown().
 * See src/graphics/buffer.c. */
void lc_buffer_destroy_all(void);

/* Destroys all buffers owned by a device being torn down.
 * Called by lc_device_destroy(). See src/graphics/buffer.c. */
void lc_buffer_destroy_for_device(const lc_device *device);

/* Destroys all live images. Called by lc_shutdown().
 * See src/graphics/image.c. */
void lc_image_destroy_all(void);

/* Destroys all images owned by a device being torn down.
 * Called by lc_device_destroy(). See src/graphics/image.c. */
void lc_image_destroy_for_device(const lc_device *device);

/* Destroys all live samplers. Called by lc_shutdown().
 * See src/graphics/sampler.c. */
void lc_sampler_destroy_all(void);

/* Destroys all samplers owned by a device being torn down.
 * Called by lc_device_destroy(). See src/graphics/sampler.c. */
void lc_sampler_destroy_for_device(const lc_device *device);

/* Destroys all live image views. Called by lc_shutdown().
 * See src/graphics/image_view.c. */
void lc_image_view_destroy_all(void);

/* Destroys all views of one image being torn down.
 * Called by lc_image_destroy(). See src/graphics/image_view.c. */
void lc_image_view_destroy_for_image(const lc_image *image);

/* Destroys all views owned by a device being torn down.
 * Called by lc_device_destroy(). See src/graphics/image_view.c. */
void lc_image_view_destroy_for_device(const lc_device *device);

/* Destroys all live binding layouts. Called by lc_shutdown().
 * See src/graphics/binding.c. */
void lc_binding_layout_destroy_all(void);

/* Destroys all binding layouts owned by a device being torn down.
 * Called by lc_device_destroy(). See src/graphics/binding.c. */
void lc_binding_layout_destroy_for_device(const lc_device *device);

/* Destroys all live binding sets. Called by lc_shutdown().
 * See src/graphics/binding.c. */
void lc_binding_set_destroy_all(void);

/* Destroys all binding sets owned by a device being torn down.
 * Called by lc_device_destroy(). See src/graphics/binding.c. */
void lc_binding_set_destroy_for_device(const lc_device *device);

/* Nonzero when a render-target handle is live. Shared by frame and
 * encoder validation. See src/graphics/render_target.c. */
int lc_render_target_is_live(const lc_render_target *target);

/* Destroys all live render targets. Called by lc_shutdown().
 * See src/graphics/render_target.c. */
void lc_render_target_destroy_all(void);

/* Destroys all render targets owned by a device being torn down.
 * Called by lc_device_destroy(). See src/graphics/render_target.c. */
void lc_render_target_destroy_for_device(const lc_device *device);

/* Destroys all render targets referencing a view being torn down.
 * Called by lc_image_view_destroy(). See src/graphics/render_target.c. */
void lc_render_target_destroy_for_view(const lc_image_view *view);

/* Default title used when lc_window_desc.title is NULL */
#define LC_DEFAULT_TITLE "LumaC"

#endif /* LUMAC_INTERNAL_H */
