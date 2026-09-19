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

/* Shared viewport binding layout (lazily created): slot 0 =
 * sampled image (fragment), slot 1 = sampler (fragment) — TWO
 * layouts, mirroring the GUI font walk (gui_draw.cpp: set 0 =
 * texture via tex_layout, set 1 = sampler via samp_layout). The
 * viewport panel Image() TexID round-trips the TEXTURE set (slot 0
 * layout); the draw walk binds it at pipeline slot 0. A combined
 * single-layout set would NEVER match the pipeline's slot-0
 * signature (count 2 != 1) — that mismatch was a REAL bug caught by
 * the headed app run (bind step 18), not harness noise. */
struct leg_viewport_layout {
    lc_binding_layout *tex_layout;
    lc_binding_layout *samp_layout;
    lc_sampler *sampler;
    lc_binding_set *samp_set; /* shared sampler set (slot 1) */
};

static leg_viewport_layout g_vp_layout;

/* Ensure the shared layouts exist (idempotent; 1 on success). */
static int leg_viewport_layout_ensure(lc_device *device) {
    lc_binding_desc b;
    lc_binding_layout_desc ld;
    lc_sampler_desc sd;

    if (device == NULL) {
        return 0;
    }
    if (g_vp_layout.tex_layout != NULL &&
        g_vp_layout.samp_layout != NULL &&
        g_vp_layout.sampler != NULL) {
        return 1;
    }
    memset(&b, 0, sizeof(b));
    b.binding = 0;
    b.type = LC_BINDING_SAMPLED_IMAGE;
    b.count = 1;
    b.visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    memset(&ld, 0, sizeof(ld));
    ld.bindings = &b;
    ld.binding_count = 1;
    if (lc_binding_layout_create(device, &ld,
                                 &g_vp_layout.tex_layout) !=
        LC_SUCCESS) {
        g_vp_layout.tex_layout = NULL;
        return 0;
    }
    memset(&b, 0, sizeof(b));
    b.binding = 0;
    b.type = LC_BINDING_SAMPLER;
    b.count = 1;
    b.visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    memset(&ld, 0, sizeof(ld));
    ld.bindings = &b;
    ld.binding_count = 1;
    if (lc_binding_layout_create(device, &ld,
                                 &g_vp_layout.samp_layout) !=
        LC_SUCCESS) {
        lc_binding_layout_destroy(g_vp_layout.tex_layout);
        g_vp_layout.tex_layout = NULL;
        g_vp_layout.samp_layout = NULL;
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
        lc_binding_layout_destroy(g_vp_layout.samp_layout);
        lc_binding_layout_destroy(g_vp_layout.tex_layout);
        g_vp_layout.samp_layout = NULL;
        g_vp_layout.tex_layout = NULL;
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
    lc_binding_write write;

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
    if (lc_binding_set_create(g_vp_layout.tex_layout, &vt->set) !=
        LC_SUCCESS) {
        vt->set = NULL;
        leg_viewport_destroy_objects(vt);
        return 0;
    }
    /* Texture set FIRST (SHADER_READ guaranteed by the primer
     * upload above — same ordering discipline as the font walk:
     * never update a sampled set over a non-readable image). */
    memset(&write, 0, sizeof(write));
    write.binding = 0;
    write.array_element = 0;
    write.type = LC_BINDING_SAMPLED_IMAGE;
    write.u.image.view = vt->color_view;
    if (lc_binding_set_update(vt->set, &write, 1) != LC_SUCCESS) {
        leg_viewport_destroy_objects(vt);
        return 0;
    }
    /* Shared sampler set (slot 1) for the walk: created once (the
     * GUI font walk keeps its OWN sampler set — layouts differ per
     * bridge... actually the layouts are content-identical (one
     * sampler slot) but distinct OBJECTS; sets borrow their layout
     * object, so each bridge keeps its own set). */
    if (g_vp_layout.samp_set == NULL) {
        lc_binding_write sw;

        memset(&sw, 0, sizeof(sw));
        if (lc_binding_set_create(g_vp_layout.samp_layout,
                                  &g_vp_layout.samp_set) !=
            LC_SUCCESS) {
            g_vp_layout.samp_set = NULL;
            leg_viewport_destroy_objects(vt);
            return 0;
        }
        sw.binding = 0;
        sw.array_element = 0;
        sw.type = LC_BINDING_SAMPLER;
        sw.u.sampler.sampler = g_vp_layout.sampler;
        if (lc_binding_set_update(g_vp_layout.samp_set, &sw, 1) !=
            LC_SUCCESS) {
            lc_binding_set_destroy(g_vp_layout.samp_set);
            g_vp_layout.samp_set = NULL;
            leg_viewport_destroy_objects(vt);
            return 0;
        }
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
    /* R-013: edit-viewport orbit camera override (value type, no
     * lifetime). Play keeps the active-camera path (gameplay
     * rendering untouched); edit renders through the orbit camera
     * the overlays/picking/gizmos already assume. */
    int use_orbit_camera = 0;
    lr_camera orbit_camera;

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
    /* R-013 scope: is_play + use_orbit_camera must survive the
     * blocks below (orbit override needs both). */
    int is_play = 0;
#if defined(LUMA34A_MUT_COMPOSITE)
    /* M-composite (clear-only): skip the engine trio (the census
     * must read ~0 non-clear pixels while armed). */
    (void)world;
    (void)is_play;
    (void)use_orbit_camera;
#else
    /* Play shows the RUNTIME world; edit shows the edit world.
     * R-012 editor environment: enabled ONLY around the EDIT-world
     * composite — never for the play world (game rendering keeps
     * its own clear/sky), never serialized, never scene content.
     * Toggles live on the GUI prefs (View menu); the renderer
     * defaults are off, so headless probes are unaffected. */
    {
        lr_renderer *ren = NULL;

        is_play = led_is_playing(ctx->session);

        world = is_play ? led_play_get_world(ctx->session)
                        : led_session_get_edit_world(ctx->session);
        if (ctx->session != NULL) {
            le_engine *eng = led_session_get_engine(ctx->session);

            if (eng != NULL) {
                ren = le_engine_get_renderer(eng);
            }
            if (ren != NULL) {
                if (is_play) {
                    /* Belt-and-braces: the play world must never
                     * inherit a stale editor environment (the
                     * renderer is shared). */
                    lr_renderer_set_editor_environment(ren, 0, 0);
                } else {
                    extern int leg_viewport_env_wanted(leg_context *c);
                    extern int leg_viewport_grid_wanted(leg_context *c);
                    lr_renderer_set_editor_environment(
                        ren, leg_viewport_env_wanted(ctx),
                        leg_viewport_grid_wanted(ctx));
                }
            }
        }
    }
    if (world == NULL) {
        return ImTextureID_Invalid;
    }
    /* R-013: derive the orbit camera from the host's led_viewport
     * (same struct the panel sizes + the overlays project through).
     * Width/height override to the live target extent (the struct
     * follows the panel a frame later; aspect must match THIS
     * composite). Failure falls back to the active-camera path
     * (never a blank viewport over a math hiccup). */
    if (!is_play) {
        extern led_viewport *leg_viewport_host_viewport(void);
        led_viewport *host_vp = leg_viewport_host_viewport();
        led_viewport scratch;

        if (host_vp != NULL) {
            scratch = *host_vp;
            scratch.width = w;
            scratch.height = h;
            memset(&orbit_camera, 0, sizeof(orbit_camera));
            if (led_viewport_camera(&scratch, &orbit_camera)) {
                use_orbit_camera = 1;
            }
        }
    }
    if (use_orbit_camera) {
        if (le_world_render_scene_with_camera(world, enc, w, h,
                                              &orbit_camera) !=
            LE_SUCCESS) {
            le_world_render_end(world);
            return ImTextureID_Invalid;
        }
    } else if (le_world_render_scene(world, enc, w, h) !=
               LE_SUCCESS) {
        le_world_render_end(world);
        return ImTextureID_Invalid;
    }
#endif
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
#if defined(LUMA34A_MUT_COMPOSITE)
        return ImTextureID_Invalid;
#else
        le_world_render_end(world);
        return ImTextureID_Invalid;
#endif
    }
#if defined(LUMA34A_MUT_COMPOSITE)
    /* Clear-only: no scene output (target keeps clear color). */
    if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        return ImTextureID_Invalid;
    }
#else
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
#endif
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

/* App-facing accessors (public C ABI in luma_editor.h; the app never
 * includes gui_internal.h or imgui.h — confinement holds). */

/* Borrow-or-create the context's viewport target (NULL on bad
 * context; the target is context-owned, destroyed with it). */
struct leg_viewport_target *leg_viewport_target_for(
    leg_context *ctx) {
    if (ctx == NULL) {
        return NULL;
    }
    if (ctx->viewport_target == NULL) {
        ctx->viewport_target = new (std::nothrow)
            leg_viewport_target;

        if (ctx->viewport_target == NULL) {
            return NULL;
        }
        memset(ctx->viewport_target, 0,
               sizeof(*ctx->viewport_target));
    }
    return ctx->viewport_target;
}

/* Composite the session world into the panel-sized target (engine
 * trio, NO open pass on enc) + return the panel TexID (0 when
 * nothing to show — panel falls back to the state readout). */
unsigned long long leg_viewport_composite(
    leg_context *ctx, struct leg_viewport_target *vt,
    lc_command_encoder *enc, unsigned w, unsigned h) {
    ImTextureID id;

    if (ctx == NULL || vt == NULL || enc == NULL) {
        return 0ull;
    }
    id = leg_viewport_render(ctx, vt, enc, (uint32_t)w,
                             (uint32_t)h);
    return (unsigned long long)(uint64_t)id;
}

/* Composite census (observe-only readback for the headed proof):
 * counts pixels in the panel-sized target differing from the
 * composite clear color (0.04/0.05/0.09 -> 10,13,23 @8bit) by
 * more than 3 LSBs on any channel. Requires TRANSFER_SRC (set at
 * target creation). The caller owns frame discipline: call AFTER
 * leg_viewport_composite recorded into `enc` and the frame
 * submitted (readback observes finished GPU work — same rule as
 * the h_shot path: end the frame first). */
int leg_viewport_composite_census(leg_context *ctx,
                                  struct leg_viewport_target *vt,
                                  uint64_t out[4]) {
    lc_image_readback_desc rbdesc;
    lc_image_readback_info rbinfo;
    unsigned char *rgba = NULL;
    uint64_t non_clear = 0;
    uint32_t x = 0;
    uint32_t y = 0;

    if (out != NULL) {
        out[0] = out[1] = out[2] = out[3] = 0ull;
    }
    if (ctx == NULL || vt == NULL || out == NULL) {
        return 0;
    }
    if (vt->color == NULL || vt->width == 0 ||
        vt->height == 0) {
        return 0;
    }
    memset(&rbdesc, 0, sizeof(rbdesc));
    memset(&rbinfo, 0, sizeof(rbinfo));
    if (lc_image_query_readback(vt->color, &rbdesc, &rbinfo) !=
        LC_SUCCESS) {
        return 0;
    }
    rgba = new (std::nothrow) unsigned char[rbinfo.size];
    if (rgba == NULL) {
        return 0;
    }
    if (lc_image_readback(vt->color, &rbdesc, rgba,
                          rbinfo.size, NULL) != LC_SUCCESS) {
        delete[] rgba;
        return 0;
    }
    for (y = 0; y < vt->height && y < rbinfo.height; y++) {
        for (x = 0; x < vt->width && x < rbinfo.width; x++) {
            size_t i =
                ((size_t)y * rbinfo.width + x) * 4u;
            int dr =
                (int)rgba[i + 0] - 10;
            int dg =
                (int)rgba[i + 1] - 13;
            int db =
                (int)rgba[i + 2] - 23;

            if (dr < 0) {
                dr = -dr;
            }
            if (dg < 0) {
                dg = -dg;
            }
            if (db < 0) {
                db = -db;
            }
            if (dr > 3 || dg > 3 || db > 3) {
                non_clear++;
            }
        }
    }
    delete[] rgba;
    out[0] = non_clear;
    out[1] = vt->width;
    out[2] = vt->height;
    out[3] = 1ull;
    (void)ctx;
    return 1;
}

/* R-012 sky-pixel probe (observe-only single-pixel readback for the
 * headed environment legs; same readback discipline as the census
 * above — call AFTER the composite + frame submit). Row stride is
 * the readback's (tight) width, NOT the target width: readbacks of
 * an image whose extent exceeds its render-target viewport still
 * return the full-image layout. */
int leg_viewport_sky_pixel(leg_context *ctx,
                           struct leg_viewport_target *vt,
                           unsigned x, unsigned y,
                           unsigned char out_rgb[3]) {
    lc_image_readback_desc rbdesc;
    lc_image_readback_info rbinfo;
    unsigned char *rgba = NULL;

    if (out_rgb != NULL) {
        out_rgb[0] = out_rgb[1] = out_rgb[2] = 0;
    }
    if (ctx == NULL || vt == NULL || out_rgb == NULL) {
        return 0;
    }
    if (vt->color == NULL || vt->width == 0 ||
        vt->height == 0) {
        return 0;
    }
    if (x >= vt->width) {
        x = vt->width - 1;
    }
    if (y >= vt->height) {
        y = vt->height - 1;
    }
    memset(&rbdesc, 0, sizeof(rbdesc));
    memset(&rbinfo, 0, sizeof(rbinfo));
    if (lc_image_query_readback(vt->color, &rbdesc, &rbinfo) !=
        LC_SUCCESS) {
        return 0;
    }
    rgba = new (std::nothrow) unsigned char[rbinfo.size];
    if (rgba == NULL) {
        return 0;
    }
    if (lc_image_readback(vt->color, &rbdesc, rgba,
                          rbinfo.size, NULL) != LC_SUCCESS) {
        delete[] rgba;
        return 0;
    }
    {
        size_t i = ((size_t)y * rbinfo.width + x) * 4u;

        if (i + 2 < rbinfo.size) {
            out_rgb[0] = rgba[i + 0];
            out_rgb[1] = rgba[i + 1];
            out_rgb[2] = rgba[i + 2];
        }
    }
    delete[] rgba;
    (void)ctx;
    return 1;
}
