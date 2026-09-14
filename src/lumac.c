#include "lumac/lumac.h"
#include "internal/lumac_internal.h"

/* Private global state - file scope only */
static lc_state s_state = { 0 };

lc_state *lc_get_internal_state(void) {
    return &s_state;
}

lc_result lc_init(void) {
    if (s_state.initialized) {
        return LC_ERROR_ALREADY_INITIALIZED;
    }
    s_state.initialized = 1;
    if (s_state.next_resource_id == 0) {
        s_state.next_resource_id = 1;
    }
    return LC_SUCCESS;
}

void lc_shutdown(void) {
    if (!s_state.initialized) {
        return;
    }
    /* Policy: auto-destroy in dependency order - pipelines, binding
     * sets/layouts, shaders, samplers, render targets, image views,
     * images, buffers, swapchains, surfaces, devices, windows.
     * Pipelines hold only structural target signatures (never target
     * objects); targets borrow views, so targets die before views. A
     * VkSwapchainKHR needs its device alive; a VkSurfaceKHR needs its
     * device's instance and native window. */
    lc_pipeline_destroy_all();
    lc_compute_pipeline_destroy_all();
    lc_binding_set_destroy_all();
    lc_binding_layout_destroy_all();
    lc_shader_destroy_all();
    lc_sampler_destroy_all();
    lc_render_target_destroy_all();
    lc_image_view_destroy_all();
    lc_image_destroy_all();
    lc_buffer_destroy_all();
    lc_swapchain_destroy_all();
    lc_surface_destroy_all();
    lc_device_destroy_all();
    lc_window_destroy_all();
    s_state.initialized = 0;
}

const char *lc_get_version_string(void) {
    return LC_VERSION_STRING;
}
