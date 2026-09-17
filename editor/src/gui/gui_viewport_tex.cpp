/* Phase 33 GUI viewport texture bridge (isolated C++ over lc_*).
 *
 * Owns the offscreen scene target sampled by the viewport panel:
 * sized to the panel (not the window), rebuilt on panel resize,
 * zero-size-safe (no target at 0 extent), swapchain-independent
 * (window resizes never rebuild it). The scene renders through the
 * engine trio (le_world_render_scene into HDR + le_world_render_output
 * into this target, see engine_scene/main.c); the panel samples the
 * color image through ONE binding set (rebuilt per generation) whose
 * TexID is handed to ImGui::Image.
 *
 * Lifetime: target/view(s)/set die on resize BEFORE images (views
 * borrow images; sets borrow the layout). The GUI context owns one
 * bridge (single viewport in Phase 33). All lc_* calls are public
 * API; no Vulkan/Win32/X11 here (configure-time audit).
 */

#include <cstring>
#include <new>

#include "gui_internal.h"

/* Shared viewport binding layout (lazily created: one sampled image
 * + one sampler in a SINGLE set — unlike the GUI font walk which
 * uses two sets, the viewport image is sampled through one combined
 * layout so ImGui::Image needs one TexID).
 *
 * NOTE: struct leg_viewport_target is COMPLETE in gui_internal.h
 * (draw/panels TUs read ->set); only the functions live here. */
struct leg_viewport_layout {
    lc_binding_layout *layout;
    lc_sampler *sampler;
};

static leg_viewport_layout g_vp_layout;

/* Ensure the shared layout exists (idempotent; 1 on success). */
static int leg_viewport_layout_ensure(lc_device *device) {
    lc_binding_desc b[2];
    lc_binding_layout_desc ld;
    lc_sampler_desc sd;

    if (device == NULL) {
        return 0;
    }
    if (g_vp_layout.layout != NULL && g_vp_layout.sampler != NULL) {
        return 1;
    }
    memset(b, 0, sizeof(b));
    b[0].binding = 0;
    b[0].type = LC_BINDING_SAMPLED_IMAGE;
    b[0].count = 1;
    b[0].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    b[1].binding = 1;
    b[1].type = LC_BINDING_SAMPLER;
    b[1].count = 1;
    b[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    memset(&ld, 0, sizeof(ld));
    ld.bindings = b;
    ld.binding_count = 2;
    if (lc_binding_layout_create(device, &ld,
                                 &g_vp_layout.layout) !=
        LC_SUCCESS) {
        g_vp_layout.layout = NULL;
        return 0;
    }
    memset(&sd, 0, sizeof(sd));
    sd.min_filter = LC_FILTER_LINEAR;
    sd.mag_filter = LC_FILTER_LINEAR;
    sd.mipmap_mode = LC_MIPMAP_MODE_NEAREST;
    sd.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
    sd.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
    sd.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
    sd.mip_lod_bias = 0.0f;
    sd.min_lod = 0.0f;
    sd.max_lod = 0.0f;
    sd.max_anisotropy = 1.0f;
    if (lc_sampler_create(device, &sd, &g_vp_layout.sampler) !=
        LC_SUCCESS) {
        lc_binding_layout_destroy(g_vp_layout.layout);
        g_vp_layout.layout = NULL;
        g_vp_layout.sampler = NULL;
        return 0;
    }
    return 1;
}

/* Destroy one target's objects (views/target/set before images). */
static void leg_viewport_destroy_objects(leg_viewport_target *vt) {
    if (vt == NULL) {
        return;
    }
    if (vt->set != NULL) {
        lc_binding_set_destroy(vt->set);
        vt->set = NULL;
    }
    if (vt->target != NULL) {
        lc_render_target_destroy(vt->target);
        vt->target = NULL;
    }
    if (vt->color_view != NULL) {
        lc_image_view_destroy(vt->color_view);
        vt->color_view = NULL;
    }
    if (vt->depth_view != NULL) {
        lc_image_view_destroy(vt->depth_view);
        vt->depth_view = NULL;
    }
    if (vt->color != NULL) {
        lc_image_destroy(vt->color);
        vt->color = NULL;
    }
    if (vt->depth != NULL) {
        lc_image_destroy(vt->depth);
        vt->depth = NULL;
    }
    vt->width = 0;
    vt->height = 0;
}

/* Create the target objects at w x h (1 on success; vt cleared and
 * 0 on failure — caller keeps the old target alive... actually the
 * old was already destroyed: rebuild failure leaves vt empty and
 * the panel shows the state readout until the next frame retries). */
static int leg_viewport_create_objects(leg_context *ctx,
                                       leg_viewport_target *vt,
                                       uint32_t w, uint32_t h) {
    lc_device *device = NULL;
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_render_target_create_desc rtdesc;
    lc_render_target_attachment ratt;
    lc_binding_write writes[2];

    if (ctx == NULL || vt == NULL || w == 0 || h == 0) {
        return 0;
    }
    device = ctx->device;
    if (device == NULL) {
        return 0;
    }
    if (!leg_viewport_layout_ensure(device)) {
        return 0;
    }
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RGBA8_UNORM;
    idesc.width = w;
    idesc.height = h;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                  LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                  LC_IMAGE_USAGE_TRANSFER_SRC |
                  LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.flags = LC_IMAGE_FLAG_NONE;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &idesc, &vt->color) != LC_SUCCESS) {
        vt->color = NULL;
        return 0;
    }
    idesc.format = LC_FORMAT_D32_FLOAT;
    idesc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL;
    if (lc_image_create(device, &idesc, &vt->depth) != LC_SUCCESS) {
        vt->depth = NULL;
        lc_image_destroy(vt->color);
        vt->color = NULL;
        return 0;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.format = LC_FORMAT_UNDEFINED;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.base_mip_level = 0;
    vdesc.mip_level_count = 1;
    vdesc.base_array_layer = 0;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(vt->color, &vdesc, &vt->color_view) !=
        LC_SUCCESS) {
        vt->color_view = NULL;
        leg_viewport_destroy_objects(vt);
        return 0;
    }
    vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
    if (lc_image_view_create(vt->depth, &vdesc, &vt->depth_view) !=
        LC_SUCCESS) {
        vt->depth_view = NULL;
        leg_viewport_destroy_objects(vt);
        return 0;
    }
    /* Prime the color image sampled-readable before the sampling
     * set points at it (same discipline as render_to_texture). */
    {
        lc_image_upload_desc primer;
        unsigned char *zeros = new (std::nothrow) unsigned char
            [(size_t)w * h * 4u];

        if (zeros == NULL) {
            leg_viewport_destroy_objects(vt);
            return 0;
        }
        memset(zeros, 0, (size_t)w * h * 4u);
        memset(&primer, 0, sizeof(primer));
        primer.mip_level = 0;
        primer.array_layer = 0;
        primer.width = w;
        primer.height = h;
        primer.depth = 1;
        primer.data = zeros;
        primer.data_size = (uint64_t)w * h * 4u;
        if (lc_image_write(vt->color, &primer) != LC_SUCCESS) {
            delete[] zeros;
            leg_viewport_destroy_objects(vt);
            return 0;
        }
        delete[] zeros;
    }
    memset(&rtdesc, 0, sizeof(rtdesc));
    ratt.view = vt->color_view;
    rtdesc.color_attachments = &ratt;
    rtdesc.color_attachment_count = 1;
    rtdesc.depth_stencil_attachment = vt->depth_view;
    rtdesc.width = w;
    rtdesc.height = h;
    if (lc_render_target_create(device, &rtdesc, &vt->target) !=
        LC_SUCCESS) {
        vt->target = NULL;
        leg_viewport_destroy_objects(vt);
        return 0;
    }
    if (lc_binding_set_create(g_vp_layout.layout, &vt->set) !=
        LC_SUCCESS) {
        vt->set = NULL;
        leg_viewport_destroy_objects(vt);
        return 0;
    }
    memset(writes, 0, sizeof(writes));
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_SAMPLED_IMAGE;
    writes[0].u.image.view = vt->color_view;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLER;
    writes[1].u.sampler.sampler = g_vp_layout.sampler;
    if (lc_binding_set_update(vt->set, writes, 2) != LC_SUCCESS) {
        leg_viewport_destroy_objects(vt);
        return 0;
    }
    vt->width = w;
    vt->height = h;
    vt->generation++;
    return 1;
}

/* Ensure the target matches `w x h` (1 on success / already exact).
 * Zero extent destroys without recreating (minimized-safe). */
static int leg_viewport_ensure(leg_context *ctx,
                               leg_viewport_target *vt, uint32_t w,
                               uint32_t h) {
    if (ctx == NULL || vt == NULL) {
        return 0;
    }
    if (w == 0 || h == 0) {
        leg_viewport_destroy_objects(vt);
        return 1; /* zero-size: no target is the correct state */
    }
    if (vt->target != NULL && vt->width == w && vt->height == h) {
        return 1;
    }
    leg_viewport_destroy_objects(vt);
    return leg_viewport_create_objects(ctx, vt, w, h);
}

/* Render the edit world into the target (engine trio) + return the
 * ImGui TexID for the panel Image. Returns ImTextureID_Invalid when
 * there is nothing to show (zero extent, no world, GPU failure —
 * the panel falls back to the state readout). The caller must have
 * NO open pass on enc (render_scene opens/closes internally; the
 * output pass opens here). */
ImTextureID leg_viewport_render(leg_context *ctx,
                                leg_viewport_target *vt,
                                lc_command_encoder *enc,
                                uint32_t w, uint32_t h) {
    le_world *world = NULL;
    lc_render_color_attachment catt;
    lc_render_depth_attachment datt;
    lc_render_pass_desc pass;

    if (ctx == NULL || vt == NULL || enc == NULL) {
        return ImTextureID_Invalid;
    }
    if (ctx->session == NULL) {
        return ImTextureID_Invalid;
    }
    if (!leg_viewport_ensure(ctx, vt, w, h)) {
        return ImTextureID_Invalid;
    }
    if (w == 0 || h == 0 || vt->target == NULL) {
        return ImTextureID_Invalid;
    }
    /* Play shows the RUNTIME world; edit shows the edit world. */
    world = led_is_playing(ctx->session)
                ? led_play_get_world(ctx->session)
                : led_session_get_edit_world(ctx->session);
    if (world == NULL) {
        return ImTextureID_Invalid;
    }
    if (le_world_render_scene(world, enc, w, h) != LE_SUCCESS) {
        le_world_render_end(world);
        return ImTextureID_Invalid;
    }
    memset(&catt, 0, sizeof(catt));
    catt.view = vt->color_view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    catt.clear_color[0] = 0.04f;
    catt.clear_color[1] = 0.05f;
    catt.clear_color[2] = 0.09f;
    catt.clear_color[3] = 1.0f;
    memset(&datt, 0, sizeof(datt));
    datt.view = vt->depth_view;
    datt.depth_load_op = LC_LOAD_OP_CLEAR;
    datt.depth_store_op = LC_STORE_OP_STORE;
    datt.clear_depth = 1.0f;
    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_stencil = 0;
    memset(&pass, 0, sizeof(pass));
    pass.color_attachments = &catt;
    pass.color_attachment_count = 1;
    pass.depth_attachment = &datt;
    pass.width = w;
    pass.height = h;
    if (lc_encoder_begin_render_pass(enc, &pass) != LC_SUCCESS) {
        le_world_render_end(world);
        return ImTextureID_Invalid;
    }
    if (le_world_render_output(world, enc, vt->target) !=
        LE_SUCCESS) {
        lc_encoder_end_render_pass(enc);
        le_world_render_end(world);
        return ImTextureID_Invalid;
    }
    if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        le_world_render_end(world);
        return ImTextureID_Invalid;
    }
    le_world_render_end(world);
    /* TexID = binding-set identity for the draw walk. The GUI font
     * table owns TexIDs 1..63 (slot+1); viewport targets ride ABOVE
     * as pointer-sized IDs. The walk must accept them — see
     * gui_draw.cpp (viewport IDs resolve through the context's
     * viewport target, not the font table). Documented split:
     * - 1..63: font texture table slots
     * - pointer range: viewport binding sets (this bridge) */
    return (ImTextureID)(uint64_t)(uintptr_t)vt->set;
}

/* Destroy a viewport target + null it (NULL-safe; views/target/set
 * before images). The shared layout/sampler outlive targets (one
 * per process; device-lifetime objects, released at shutdown). */
void leg_viewport_target_destroy(leg_viewport_target **vt) {
    if (vt == NULL || *vt == NULL) {
        return;
    }
    leg_viewport_destroy_objects(*vt);
    delete *vt;
    *vt = NULL;
}
