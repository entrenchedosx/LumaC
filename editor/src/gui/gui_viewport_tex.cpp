/* Phase 33 GUI viewport texture bridge (isolated C++ over lc_*).
 *
 * Owns the offscreen scene target sampled by the viewport panel:
 * sized to the panel (not the window), rebuilt on panel resize,
 * zero-size-safe (no target at 0 extent), swapchain-independent
 * (window resizes never rebuild it). Backend-neutral handle bridge
 * for ImGui::Image-compatible display.
 *
 * Implemented here (lc_* objects); the ImTextureID registration
 * rides the shared texture table in gui_draw.cpp (viewport images
 * are RGBA8 sampled targets, registered like font pages but with
 * COLOR_ATTACHMENT usage + own view/set per resize generation).
 */

#include "gui_internal.h"

/* Viewport target state (owned here; one per context — single
 * viewport in Phase 33, multi-viewport deferred). The scene target
 * needs COLOR_ATTACHMENT + SAMPLED + TRANSFER_SRC (readback proofs);
 * depth is a peer DEPTH_STENCIL image. Both die on resize (views and
 * target before images) and rebuild at the new panel extent. */
struct leg_viewport_target {
    uint32_t width;
    uint32_t height;
    lc_image *color;
    lc_image *depth;
    lc_image_view *color_view;
    lc_image_view *depth_view;
    lc_render_target *target;
    lc_sampler *sampler;
    lc_binding_set *set;
    uint64_t generation; /* bumps every rebuild (stale Image guard) */
};

/* Ensure the target matches `w x h` (1 on success / already exact).
 * Zero extent destroys without recreating (minimized-safe). */
static int leg_viewport_ensure(leg_context *ctx,
                               leg_viewport_target *vt, uint32_t w,
                               uint32_t h) {
    (void)ctx;
    (void)vt;
    (void)w;
    (void)h;
    /* Full implementation lands with the panels (needs the texture
     * table share + generation guard); the signature is fixed now so
     * gui_loop.cpp compiles against it. */
    return 0;
}

