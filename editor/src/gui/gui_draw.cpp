/* Phase 33 GUI draw/font/viewport bridges (isolated C++ over lc_*).
 *
 * gui_font.cpp: font atlas upload (ImTextureData WantCreate/WantUpdates
 *   -> lc_image_write into a SAMPLED|TRANSFER_DST RGBA8 image; TexID =
 *   index+1 into the context texture table; BackendUserData = table
 *   slot anchor for liveness checks).
 * gui_draw.cpp: ImDrawData walk into public lc_* recording
 *   (CPU-visible vertex/index buffers, push-constant scale/translate,
 *   blended GUI pipeline, per-draw scissor clips). D3D12 replacement
 *   point: reimplement THIS file's record() against the same walk.
 * gui_viewport_tex.cpp: offscreen scene target sampled by the viewport
 *   panel (sized to the panel, zero-size-safe, swapchain-independent).
 *
 * Confinement: public lc_* ONLY. No Vulkan/Win32/X11 headers, no
 * imgui_impl_*, no engine/renderer internals (configure-time audit).
 */

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

#include "gui_internal.h"

/* Bundled SPIR-V for the GUI shaders (compiled offline from
 * editor/src/gui/shaders/gui.vert / gui.frag with glslc; the GLSL
 * sources are canonical, the byte arrays below are generated — see
 * shaders/README). The vertex shader takes ImDrawVert
 * (pos R32G32 + uv R32G32 + col R8G8B8A8 UNORM) and a 4-float
 * push-constant (scale.xy, translate.xy); the fragment shader
 * multiplies vertex color by the bound texture sample. */
#include "gui_shaders_spv.h"

/* Draw-buffer cap (mirrors the leg_draw_budget probe: 64 MiB per
 * buffer; over-budget frames skip loudly instead of half-recording). */
#define LEG_GUI_MAX_DRAW_BYTES ((uint64_t)(64ull * 1024ull * 1024ull))

/* Device objects owned per-context (laziily created on first GPU use
 * so headless contexts never touch the device). Defined here (the
 * only TU that uses them); gui_internal.h forward-declares. */
struct leg_gpu {
    int ready;
    lc_shader *vs;
    lc_shader *fs;
    lc_binding_layout *tex_layout;   /* slot 0: sampled image */
    lc_binding_layout *samp_layout;  /* slot 1: sampler */
    lc_pipeline *pipeline;           /* blended, depthless */
    lc_sampler *linear_sampler;
    lc_buffer *vtx;
    uint64_t vtx_cap;
    lc_buffer *idx;
    uint64_t idx_cap;
    struct {
        int used;
        lc_image *image;
        lc_image_view *view;
        lc_binding_set *set;
        uint32_t width;
        uint32_t height;
    } tex[LEG_TEX_MAX];
};

/* Ensure the GPU objects exist (idempotent; 1 on success). */
static int leg_gpu_ensure(leg_context *ctx, leg_gpu *gpu,
                          lc_format target_format) {
    lc_device *device = NULL;

    if (ctx == NULL || gpu == NULL) {
        return 0;
    }
    if (gpu->ready) {
        return 1;
    }
    device = ctx->device;
    if (device == NULL) {
        return 0;
    }
    memset(gpu, 0, sizeof(*gpu));
    /* Shaders. */
    {
        lc_shader_desc sd;

        memset(&sd, 0, sizeof(sd));
        sd.stage = LC_SHADER_STAGE_VERTEX;
        sd.code = kLegGuiVertSpv;
        sd.code_size = sizeof(kLegGuiVertSpv);
        sd.entry_point = NULL;
        if (lc_shader_create(device, &sd, &gpu->vs) != LC_SUCCESS) {
            return 0;
        }
        memset(&sd, 0, sizeof(sd));
        sd.stage = LC_SHADER_STAGE_FRAGMENT;
        sd.code = kLegGuiFragSpv;
        sd.code_size = sizeof(kLegGuiFragSpv);
        sd.entry_point = NULL;
        if (lc_shader_create(device, &sd, &gpu->fs) != LC_SUCCESS) {
            lc_shader_destroy(gpu->vs);
            gpu->vs = NULL;
            return 0;
        }
    }
    /* Binding layouts: slot 0 = sampled image (fragment), slot 1 =
     * sampler (fragment). Two layouts mirror the upstream Vulkan
     * backend (set 0 texture, set 1 sampler). */
    {
        lc_binding_desc b;
        lc_binding_layout_desc ld;

        memset(&b, 0, sizeof(b));
        b.binding = 0;
        b.type = LC_BINDING_SAMPLED_IMAGE;
        b.count = 1;
        b.visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        memset(&ld, 0, sizeof(ld));
        ld.bindings = &b;
        ld.binding_count = 1;
        if (lc_binding_layout_create(device, &ld, &gpu->tex_layout) !=
            LC_SUCCESS) {
            lc_shader_destroy(gpu->fs);
            lc_shader_destroy(gpu->vs);
            gpu->fs = NULL;
            gpu->vs = NULL;
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
                                     &gpu->samp_layout) != LC_SUCCESS) {
            lc_binding_layout_destroy(gpu->tex_layout);
            lc_shader_destroy(gpu->fs);
            lc_shader_destroy(gpu->vs);
            gpu->tex_layout = NULL;
            gpu->fs = NULL;
            gpu->vs = NULL;
            return 0;
        }
    }
    /* Blended depthless pipeline (classic src-alpha / 1-src-alpha,
     * alpha preserved like the upstream backend). */
    {
        lc_graphics_pipeline_desc pd;
        lc_vertex_binding_desc vb;
        lc_vertex_attribute_desc va[3];
        const lc_binding_layout *slots[2];
        lc_push_constant_range push;
        lc_blend_attachment blend;

        memset(&pd, 0, sizeof(pd));
        pd.vertex_shader = gpu->vs;
        pd.fragment_shader = gpu->fs;
        memset(&vb, 0, sizeof(vb));
        vb.binding = 0;
        vb.stride = 20;
        vb.input_rate = LC_VERTEX_INPUT_PER_VERTEX;
        pd.vertex_bindings = &vb;
        pd.vertex_binding_count = 1;
        memset(va, 0, sizeof(va));
        va[0].location = 0;
        va[0].binding = 0;
        va[0].format = LC_FORMAT_RG32_FLOAT;
        va[0].offset = 0;
        va[1].location = 1;
        va[1].binding = 0;
        va[1].format = LC_FORMAT_RG32_FLOAT;
        va[1].offset = 8;
        va[2].location = 2;
        va[2].binding = 0;
        va[2].format = LC_FORMAT_RGBA8_UNORM;
        va[2].offset = 16;
        pd.vertex_attributes = va;
        pd.vertex_attribute_count = 3;
        slots[0] = gpu->tex_layout;
        slots[1] = gpu->samp_layout;
        pd.binding_layouts = slots;
        pd.binding_layout_count = 2;
        pd.cull_mode = LC_CULL_NONE;
        pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
        pd.depth_test_enable = 0;
        pd.depth_write_enable = 0;
        memset(&blend, 0, sizeof(blend));
        blend.blend_enable = 1;
        blend.src_color_factor = LC_BLEND_SRC_ALPHA;
        blend.dst_color_factor = LC_BLEND_ONE_MINUS_SRC_ALPHA;
        blend.color_op = LC_BLEND_OP_ADD;
        blend.src_alpha_factor = LC_BLEND_ONE;
        blend.dst_alpha_factor = LC_BLEND_ONE_MINUS_SRC_ALPHA;
        blend.alpha_op = LC_BLEND_OP_ADD;
        pd.blend = &blend;
        pd.blend_attachment_count = 1;
        memset(&push, 0, sizeof(push));
        push.visibility = LC_SHADER_VISIBILITY_VERTEX;
        push.offset = 0;
        push.size = 16;
        pd.push_constant_ranges = &push;
        pd.push_constant_range_count = 1;
        pd.render_target.color_attachment_count = 1;
        pd.render_target.color_formats[0] = target_format;
        pd.render_target.depth_stencil_format = LC_FORMAT_UNDEFINED;
        pd.render_target.samples = LC_SAMPLE_COUNT_1;
        if (lc_graphics_pipeline_create(device, &pd, &gpu->pipeline) !=
            LC_SUCCESS) {
            lc_binding_layout_destroy(gpu->samp_layout);
            lc_binding_layout_destroy(gpu->tex_layout);
            lc_shader_destroy(gpu->fs);
            lc_shader_destroy(gpu->vs);
            gpu->samp_layout = NULL;
            gpu->tex_layout = NULL;
            gpu->fs = NULL;
            gpu->vs = NULL;
            return 0;
        }
    }
    /* Shared linear sampler (font + viewport images sample linear,
     * clamp-to-edge; matches the upstream backend default). */
    {
        lc_sampler_desc sd;

        memset(&sd, 0, sizeof(sd));
        sd.min_filter = LC_FILTER_LINEAR;
        sd.mag_filter = LC_FILTER_LINEAR;
        sd.mipmap_mode = LC_MIPMAP_MODE_LINEAR;
        sd.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
        sd.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
        sd.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
        sd.mip_lod_bias = 0.0f;
        sd.min_lod = 0.0f;
        sd.max_lod = 0.0f;
        sd.max_anisotropy = 1.0f;
        if (lc_sampler_create(device, &sd, &gpu->linear_sampler) !=
            LC_SUCCESS) {
            lc_pipeline_destroy(gpu->pipeline);
            lc_binding_layout_destroy(gpu->samp_layout);
            lc_binding_layout_destroy(gpu->tex_layout);
            lc_shader_destroy(gpu->fs);
            lc_shader_destroy(gpu->vs);
            gpu->pipeline = NULL;
            gpu->samp_layout = NULL;
            gpu->tex_layout = NULL;
            gpu->fs = NULL;
            gpu->vs = NULL;
            return 0;
        }
    }
    gpu->ready = 1;
    return 1;
}

/* Forward: texture-slot destroy (definition follows the draw record;
 * destroy-all needs it first). */
static void leg_tex_destroy_slot(leg_context *ctx, leg_gpu *gpu,
                                 uint32_t slot);

/* Destroy the GPU bridge (idempotent; views/sets before images,
 * buffers/pipeline/layouts/shaders/sampler after). Borrowed device
 * untouched. Note: tex[LEG_TEX_MAX-1].set is the shared sampler set
 * (no image/view there) — destroyed as a set only. */
static void leg_gpu_destroy(leg_context *ctx, leg_gpu *gpu) {
    uint32_t i = 0;

    if (gpu == NULL) {
        return;
    }
    for (i = 0; i < LEG_TEX_MAX - 1; i++) {
        if (gpu->tex[i].used) {
            leg_tex_destroy_slot(ctx, gpu, i);
        }
    }
    if (gpu->tex[LEG_TEX_MAX - 1].set != NULL) {
        lc_binding_set_destroy(gpu->tex[LEG_TEX_MAX - 1].set);
        gpu->tex[LEG_TEX_MAX - 1].set = NULL;
    }
    if (gpu->idx != NULL) {
        lc_buffer_destroy(gpu->idx);
        gpu->idx = NULL;
    }
    if (gpu->vtx != NULL) {
        lc_buffer_destroy(gpu->vtx);
        gpu->vtx = NULL;
    }
    gpu->vtx_cap = 0;
    gpu->idx_cap = 0;
    if (gpu->pipeline != NULL) {
        lc_pipeline_destroy(gpu->pipeline);
        gpu->pipeline = NULL;
    }
    if (gpu->linear_sampler != NULL) {
        lc_sampler_destroy(gpu->linear_sampler);
        gpu->linear_sampler = NULL;
    }
    if (gpu->samp_layout != NULL) {
        lc_binding_layout_destroy(gpu->samp_layout);
        gpu->samp_layout = NULL;
    }
    if (gpu->tex_layout != NULL) {
        lc_binding_layout_destroy(gpu->tex_layout);
        gpu->tex_layout = NULL;
    }
    if (gpu->fs != NULL) {
        lc_shader_destroy(gpu->fs);
        gpu->fs = NULL;
    }
    if (gpu->vs != NULL) {
        lc_shader_destroy(gpu->vs);
        gpu->vs = NULL;
    }
    gpu->ready = 0;
}

/* Public teardown for gui_context.cpp (frees the bridge + struct;
 * borrowed device untouched; NULL-safe). */
void leg_gpu_teardown(leg_context *ctx, struct leg_gpu *gpu) {
    if (gpu == NULL) {
        return;
    }
    leg_gpu_destroy(ctx, gpu);
    delete gpu;
}

/* Public record entry (luma_editor.h contract): fetch this cycle's
 * ImDrawData and walk it into the caller's open pass. */
static int leg_draw_record(leg_context *ctx, leg_gpu *gpu,
                           lc_command_encoder *enc,
                           const ImDrawData *draw,
                           lc_format target_format);
led_result leg_record_gui(leg_context *context,
                          lc_command_encoder *encoder,
                          lc_format target_format) {
    const ImDrawData *draw = NULL;

    if (context == NULL || encoder == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (context->imgui == NULL || context->device == NULL) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (context->frame_open) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (context->frame_minimized) {
        return LED_SUCCESS; /* minimized: nothing recorded */
    }
    if (target_format == LC_FORMAT_UNDEFINED) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    /* Color-only targets take GUI draws (depth-tested scene targets
     * never do — the GUI records into its own depthless pass). */
    switch (target_format) {
    case LC_FORMAT_R8_UNORM:
    case LC_FORMAT_RG8_UNORM:
    case LC_FORMAT_RGBA8_UNORM:
    case LC_FORMAT_RGBA8_SRGB:
    case LC_FORMAT_BGRA8_UNORM:
    case LC_FORMAT_BGRA8_SRGB:
    case LC_FORMAT_R16_FLOAT:
    case LC_FORMAT_RG16_FLOAT:
    case LC_FORMAT_RGBA16_FLOAT:
    case LC_FORMAT_R32_FLOAT:
    case LC_FORMAT_RG32_FLOAT:
    case LC_FORMAT_RGB32_FLOAT:
    case LC_FORMAT_RGBA32_FLOAT:
    case LC_FORMAT_R32_UINT:
    case LC_FORMAT_RG32_UINT:
    case LC_FORMAT_RGB32_UINT:
    case LC_FORMAT_RGBA32_UINT:
        break;
    default:
        return LED_ERROR_INVALID_ARGUMENT;
    }
    ImGui::SetCurrentContext(context->imgui);
    draw = ImGui::GetDrawData();
    if (draw == NULL || !draw->Valid) {
        return LED_ERROR_INVALID_ARGUMENT;
    }
    if (draw->TotalVtxCount <= 0 || draw->TotalIdxCount <= 0) {
        return LED_SUCCESS; /* valid empty frame */
    }
    if (context->gpu == NULL) {
        context->gpu = new (std::nothrow) leg_gpu;
        if (context->gpu == NULL) {
            return LED_ERROR_OUT_OF_MEMORY;
        }
        memset(context->gpu, 0, sizeof(*context->gpu));
    }
    if (!leg_draw_record(context, context->gpu, encoder, draw,
                         target_format)) {
        /* Distinguish budget overflow (probe) from GPU failure. */
        uint64_t vb = 0;
        uint64_t ib = 0;
        int ov = 0;

        leg_draw_budget((uint32_t)draw->TotalVtxCount,
                        (uint32_t)draw->TotalIdxCount, &vb, &ib, &ov);
        (void)vb;
        (void)ib;
        if (ov) {
            return LED_ERROR_OVERFLOW;
        }
        return LED_ERROR_UNAVAILABLE;
    }
    return LED_SUCCESS;
}

/* Ensure a CPU-visible buffer holds `need` bytes (grow-only,
 * power-of-two; 1 on success). */
static int leg_buf_ensure(lc_device *device, lc_buffer **slot,
                          uint64_t *cap, uint64_t need,
                          uint32_t usage) {
    lc_buffer_desc bd;
    uint64_t grown = 0;

    if (slot == NULL || cap == NULL || device == NULL) {
        return 0;
    }
    if (need == 0) {
        return 1;
    }
    if (*slot != NULL && *cap >= need) {
        return 1;
    }
    grown = (*cap != 0) ? *cap : 4096u;
    while (grown < need) {
        grown *= 2;
        if (grown > LEG_GUI_MAX_DRAW_BYTES) {
            grown = LEG_GUI_MAX_DRAW_BYTES;
            break;
        }
    }
    if (grown < need) {
        return 0;
    }
    if (*slot != NULL) {
        lc_buffer_destroy(*slot);
        *slot = NULL;
        *cap = 0;
    }
    memset(&bd, 0, sizeof(bd));
    bd.size = grown;
    bd.usage = usage;
    bd.memory = LC_MEMORY_CPU_TO_GPU;
    if (lc_buffer_create(device, &bd, slot) != LC_SUCCESS) {
        *slot = NULL;
        *cap = 0;
        return 0;
    }
    *cap = grown;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Draw walk: ImDrawData -> lc_* recording in the caller's open pass. */
/*                                                                            */
/* Contract (D3D12 replacement point: reimplement THIS function):     */
/* - Caller has an open pass on a swapchain frame whose target uses   */
/*   `target_format` (the GUI pipeline was created for it).           */
/* - Font/texture requests sync first (leg_font_sync); a sync failure */
/*   skips the frame (prior atlas stays, no partial draws).           */
/* - Vertex/index data uploads into grow-only CPU-visible buffers;    */
/*   over-budget frames (>64 MiB/buffer) skip loudly (returns 0).      */
/* - Projection push = upstream math (2/DisplaySize, -1-DisplayPos*). */
/* - Per-draw: bind pipeline + texture set + sampler set, push,       */
/*   clip->scissor (leg_clip_to_scissor; empty clips skip the draw),  */
/*   draw-indexed with VtxOffset/IdxOffset. User callbacks NEVER      */
/*   execute (counted in stats by leg_frame_end; skipped here).       */
/* - Returns 1 when every non-skipped draw recorded, 0 on GPU failure */
/*   (caller should end the pass and keep the frame alive).           */
/* ------------------------------------------------------------------ */
static int leg_draw_record(leg_context *ctx, leg_gpu *gpu,
                           lc_command_encoder *enc,
                           const ImDrawData *draw,
                           lc_format target_format) {
    uint64_t vb_need = 0;
    uint64_t ib_need = 0;
    int ov = 0;
    float push[4];
    uint64_t vtx_base = 0;
    uint64_t idx_base = 0;
    int li = 0;

    if (ctx == NULL || gpu == NULL || enc == NULL || draw == NULL) {
        return 0;
    }
    if (!draw->Valid) {
        return 0;
    }
    if (draw->TotalVtxCount <= 0 || draw->TotalIdxCount <= 0) {
        return 1; /* nothing to draw (valid empty frame) */
    }
    if (!leg_gpu_ensure(ctx, gpu, target_format)) {
        return 0;
    }
    if (!gpu->ready || gpu->pipeline == NULL) {
        return 0;
    }
    if (!leg_font_sync(ctx, gpu)) {
        return 0;
    }
    leg_draw_budget((uint32_t)draw->TotalVtxCount,
                    (uint32_t)draw->TotalIdxCount, &vb_need, &ib_need,
                    &ov);
    if (ov) {
        return 0;
    }
    if (!leg_buf_ensure(ctx->device, &gpu->vtx, &gpu->vtx_cap, vb_need,
                        LC_BUFFER_USAGE_VERTEX)) {
        return 0;
    }
    if (!leg_buf_ensure(ctx->device, &gpu->idx, &gpu->idx_cap, ib_need,
                        LC_BUFFER_USAGE_INDEX)) {
        return 0;
    }
    /* Upload: pack every list contiguously (global offsets mirror the
     * upstream backend's merged-buffer discipline). */
    for (li = 0; li < draw->CmdLists.Size; li++) {
        const ImDrawList *list = draw->CmdLists[li];

        if (list == NULL || list->VtxBuffer.Size <= 0) {
            continue;
        }
        {
            uint64_t bytes = (uint64_t)list->VtxBuffer.Size * 20u;

            if (lc_buffer_write(gpu->vtx, vtx_base,
                                list->VtxBuffer.Data,
                                bytes) != LC_SUCCESS) {
                return 0;
            }
            vtx_base += bytes;
        }
        {
            uint64_t bytes = (uint64_t)list->IdxBuffer.Size * 2u;

            if (lc_buffer_write(gpu->idx, idx_base,
                                list->IdxBuffer.Data,
                                bytes) != LC_SUCCESS) {
                return 0;
            }
            idx_base += bytes;
        }
    }
    /* Projection (upstream math; DisplayPos almost always 0,0). */
    if (draw->DisplaySize.x <= 0.0f || draw->DisplaySize.y <= 0.0f) {
        return 0;
    }
    push[0] = 2.0f / draw->DisplaySize.x;
    push[1] = 2.0f / draw->DisplaySize.y;
    push[2] = -1.0f - draw->DisplayPos.x * push[0];
    push[3] = -1.0f - draw->DisplayPos.y * push[1];
    if (lc_encoder_bind_pipeline(enc, gpu->pipeline) != LC_SUCCESS) {
        return 0;
    }
    if (lc_encoder_bind_vertex_buffer(enc, 0, gpu->vtx, 0) !=
        LC_SUCCESS) {
        return 0;
    }
    if (lc_encoder_bind_index_buffer(enc, gpu->idx, 0,
                                     LC_INDEX_UINT16) != LC_SUCCESS) {
        return 0;
    }
    if (lc_encoder_push_constants(enc, gpu->pipeline,
                                  LC_SHADER_VISIBILITY_VERTEX, 0,
                                  sizeof(push), push) != LC_SUCCESS) {
        return 0;
    }
    /* Sampler set is constant all frame (shared linear sampler,
     * cached in the reserved last table entry — never a texture). */
    {
        lc_binding_set *samp_set = NULL;

        samp_set = gpu->tex[LEG_TEX_MAX - 1].set;
        if (samp_set == NULL) {
            lc_binding_write w;

            memset(&w, 0, sizeof(w));
            if (lc_binding_set_create(gpu->samp_layout, &samp_set) !=
                LC_SUCCESS) {
                return 0;
            }
            w.binding = 0;
            w.array_element = 0;
            w.type = LC_BINDING_SAMPLER;
            w.u.sampler.sampler = gpu->linear_sampler;
            if (lc_binding_set_update(samp_set, &w, 1) != LC_SUCCESS) {
                lc_binding_set_destroy(samp_set);
                return 0;
            }
            gpu->tex[LEG_TEX_MAX - 1].set = samp_set;
        }
        if (lc_encoder_bind_binding_set(enc, gpu->pipeline, 1,
                                        samp_set) != LC_SUCCESS) {
            return 0;
        }
    }
    /* Per-draw: texture set + scissor + indexed draw. */
    {
        uint32_t global_vtx = 0;
        uint32_t global_idx = 0;

        for (li = 0; li < draw->CmdLists.Size; li++) {
            const ImDrawList *list = draw->CmdLists[li];
            int ci = 0;

            if (list == NULL) {
                continue;
            }
            for (ci = 0; ci < list->CmdBuffer.Size; ci++) {
                const ImDrawCmd *cmd = &list->CmdBuffer[ci];
                uint64_t tex_id = 0;
                uint32_t slot = 0;
                lc_binding_set *bound_set = NULL;
                lc_scissor_rect sc;

                if (cmd->UserCallback != NULL) {
                    continue; /* counted by leg_frame_end; skip */
                }
                tex_id = (uint64_t)cmd->GetTexID();
                if (tex_id == (uint64_t)ImTextureID_Invalid ||
                    tex_id == 0) {
                    return 0; /* missing texture (sync failed?) */
                }
                if (tex_id <= LEG_TEX_MAX - 1) {
                    /* Font table ID (slot+1): bind the slot set. */
                    slot = (uint32_t)(tex_id - 1);
                    if (!gpu->tex[slot].used ||
                        gpu->tex[slot].set == NULL) {
                        return 0;
                    }
                    bound_set = gpu->tex[slot].set;
                } else {
                    /* Viewport ID: binding-set pointer round-tripped
                     * through leg_viewport_render (gui_viewport_tex).
                     * Validate it names the context's live viewport
                     * target set before binding (never bind a stale
                     * pointer after a resize destroyed it). */
                    lc_binding_set *vs =
                        (lc_binding_set *)(uintptr_t)tex_id;

                    if (vs == NULL ||
                        ctx->viewport_target == NULL ||
                        vs != ctx->viewport_target->set) {
                        return 0;
                    }
                    bound_set = vs;
                }
                /* ClipRect is DisplayPos-relative; the pass extent is
                 * framebuffer px (scale 1 in this bridge). */
                memset(&sc, 0, sizeof(sc));
                if (!leg_clip_to_scissor(
                        cmd->ClipRect.x - draw->DisplayPos.x,
                        cmd->ClipRect.y - draw->DisplayPos.y,
                        cmd->ClipRect.z - draw->DisplayPos.x,
                        cmd->ClipRect.w - draw->DisplayPos.y,
                        ctx->frame_w, ctx->frame_h, &sc)) {
                    continue; /* fully clipped: skip the draw */
                }
                if (lc_encoder_set_scissor(enc, &sc) != LC_SUCCESS) {
                    return 0;
                }
                if (lc_encoder_bind_binding_set(enc, gpu->pipeline, 0,
                                                bound_set) !=
                    LC_SUCCESS) {
                    return 0;
                }
                if (lc_encoder_draw_indexed(
                        enc, cmd->ElemCount, 1,
                        cmd->IdxOffset + global_idx,
                        (int32_t)(cmd->VtxOffset + global_vtx),
                        0) != LC_SUCCESS) {
                    return 0;
                }
            }
            global_vtx += (uint32_t)list->VtxBuffer.Size;
            global_idx += (uint32_t)list->IdxBuffer.Size;
        }
    }
    /* Restore full-target scissor (upstream discipline: our clips
     * must not leak into application rendering). */
    {
        lc_scissor_rect full;

        memset(&full, 0, sizeof(full));
        full.offset_x = 0;
        full.offset_y = 0;
        full.width = ctx->frame_w;
        full.height = ctx->frame_h;
        if (full.width > 0 && full.height > 0) {
            lc_encoder_set_scissor(enc, &full);
        }
    }
    return 1;
}

/* Destroy one texture slot (idempotent; views/sets before images). */
static void leg_tex_destroy_slot(leg_context *ctx, leg_gpu *gpu,
                                 uint32_t slot) {
    (void)ctx;
    if (gpu == NULL || slot >= LEG_TEX_MAX) {
        return;
    }
    if (gpu->tex[slot].set != NULL) {
        lc_binding_set_destroy(gpu->tex[slot].set);
        gpu->tex[slot].set = NULL;
    }
    if (gpu->tex[slot].view != NULL) {
        lc_image_view_destroy(gpu->tex[slot].view);
        gpu->tex[slot].view = NULL;
    }
    if (gpu->tex[slot].image != NULL) {
        lc_image_destroy(gpu->tex[slot].image);
        gpu->tex[slot].image = NULL;
    }
    gpu->tex[slot].used = 0;
    gpu->tex[slot].width = 0;
    gpu->tex[slot].height = 0;
}

/* Allocate a texture slot (returns slot index, or UINT32_MAX).
 * The last table entry is reserved for the shared sampler set —
 * texture allocation never hands it out (draws reject it too). */
static uint32_t leg_tex_alloc(leg_gpu *gpu) {
    uint32_t i = 0;

    if (gpu == NULL) {
        return (uint32_t)0xFFFFFFFFu;
    }
    for (i = 0; i < LEG_TEX_MAX - 1; i++) {
        if (!gpu->tex[i].used) {
            gpu->tex[i].used = 1;
            return i;
        }
    }
    return (uint32_t)0xFFFFFFFFu;
}

/* Create-or-recreate an RGBA8 sampled image for ImTextureData pixels
 * (SAMPLED | TRANSFER_DST so lc_image_write lands sampled-readable).
 * Returns 1 on success (slot filled: image + view + binding set).
 * (Definition follows leg_font_sync; the forward declaration above
 * keeps the walker compiling.) */
static int leg_tex_upload_rgba_fwd(leg_context *ctx, leg_gpu *gpu,
                                   uint32_t slot, uint32_t width,
                                   uint32_t height, const void *pixels,
                                   uint64_t pixel_bytes);

/* ------------------------------------------------------------------ */
/* Font atlas sync (ImTextureData walk -> texture table). Called      */
/* before every draw walk AND exposed to gui_font.cpp's owner TU.     */
/* ------------------------------------------------------------------ */

int leg_font_sync(leg_context *ctx, leg_gpu *gpu) {
    ImGuiPlatformIO *pio = NULL;
    int i = 0;

    if (ctx == NULL || gpu == NULL || ctx->imgui == NULL) {
        return 0;
    }
    ImGui::SetCurrentContext(ctx->imgui);
    pio = &ImGui::GetPlatformIO();
    for (i = 0; i < pio->Textures.Size; i++) {
        ImTextureData *tex = pio->Textures[i];

        if (tex == NULL) {
            continue;
        }
        if (tex->Status == ImTextureStatus_OK) {
            continue;
        }
        if (tex->Status == ImTextureStatus_WantDestroy) {
            /* Free the table slot (TexID anchor -> slot). */
            uint64_t id = (uint64_t)tex->GetTexID();

            if (id != (uint64_t)ImTextureID_Invalid && id > 0 &&
                id <= LEG_TEX_MAX - 1) {
                leg_tex_destroy_slot(ctx, gpu, (uint32_t)(id - 1));
            }
            tex->BackendUserData = NULL;
            tex->SetTexID(ImTextureID_Invalid);
            tex->SetStatus(ImTextureStatus_Destroyed);
            continue;
        }
        if (tex->Status == ImTextureStatus_WantCreate ||
            tex->Status == ImTextureStatus_WantUpdates) {
            uint64_t id = (uint64_t)tex->GetTexID();
            uint32_t slot = (uint32_t)0xFFFFFFFFu;
            int fresh = 0;

            if (id != (uint64_t)ImTextureID_Invalid && id > 0 &&
                id <= LEG_TEX_MAX - 1 &&
                gpu->tex[id - 1].used) {
                slot = (uint32_t)(id - 1);
            } else {
                slot = leg_tex_alloc(gpu);
                fresh = 1;
            }
            if (slot == (uint32_t)0xFFFFFFFFu) {
                return 0; /* table full: honest failure */
            }
            if (tex->Format != ImTextureFormat_RGBA32 &&
                tex->Format != ImTextureFormat_Alpha8) {
                return 0;
            }
            if (tex->Width <= 0 || tex->Height <= 0 ||
                tex->Pixels == NULL) {
                return 0;
            }
            if (tex->Format == ImTextureFormat_RGBA32) {
                uint64_t bytes = (uint64_t)tex->Width *
                                 (uint64_t)tex->Height * 4u;

                if (!leg_tex_upload_rgba_fwd(
                        ctx, gpu, slot, (uint32_t)tex->Width,
                        (uint32_t)tex->Height, tex->Pixels, bytes)) {
                    if (fresh) {
                        leg_tex_destroy_slot(ctx, gpu, slot);
                    }
                    return 0;
                }
            } else {
                /* Alpha8 -> RGBA8 expand (white RGB + alpha). */
                uint64_t px = (uint64_t)tex->Width *
                              (uint64_t)tex->Height;
                unsigned char *rgba = NULL;
                uint64_t k = 0;

                if (px == 0 || px > (64ull * 1024ull * 1024ull)) {
                    return 0;
                }
                rgba = new (std::nothrow) unsigned char[(size_t)(px *
                                                                 4u)];
                if (rgba == NULL) {
                    if (fresh) {
                        leg_tex_destroy_slot(ctx, gpu, slot);
                    }
                    return 0;
                }
                for (k = 0; k < px; k++) {
                    rgba[k * 4 + 0] = 255;
                    rgba[k * 4 + 1] = 255;
                    rgba[k * 4 + 2] = 255;
                    rgba[k * 4 + 3] =
                        tex->Pixels[k];
                }
                {
                    int ok = leg_tex_upload_rgba_fwd(
                        ctx, gpu, slot, (uint32_t)tex->Width,
                        (uint32_t)tex->Height, rgba, px * 4u);

                    delete[] rgba;
                    if (!ok) {
                        if (fresh) {
                            leg_tex_destroy_slot(ctx, gpu, slot);
                        }
                        return 0;
                    }
                }
            }
            if (fresh) {
                tex->SetTexID((ImTextureID)(uint64_t)(slot + 1));
                tex->BackendUserData =
                    (void *)(uintptr_t)(slot + 1);
            }
            tex->SetStatus(ImTextureStatus_OK);
        }
    }
    return 1;
}

/* Create-or-recreate an RGBA8 sampled image for ImTextureData pixels
 * (SAMPLED | TRANSFER_DST so lc_image_write lands sampled-readable).
 * Returns 1 on success (slot filled: image + view + binding set). */
static int leg_tex_upload_rgba_fwd(leg_context *ctx, leg_gpu *gpu,
                                   uint32_t slot, uint32_t width,
                                   uint32_t height, const void *pixels,
                                   uint64_t pixel_bytes) {
    lc_device *device = NULL;
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_image_upload_desc up;
    lc_binding_write write;

    if (ctx == NULL || gpu == NULL || slot >= LEG_TEX_MAX) {
        return 0;
    }
    if (width == 0 || height == 0 || pixels == NULL ||
        pixel_bytes == 0) {
        return 0;
    }
    device = ctx->device;
    if (device == NULL) {
        return 0;
    }
    /* Recreate when the extent changed (font rebuilds / atlas
     * growth); reuse otherwise (WantUpdates overwrites in place). */
    if (gpu->tex[slot].image != NULL &&
        (gpu->tex[slot].width != width ||
         gpu->tex[slot].height != height)) {
        leg_tex_destroy_slot(ctx, gpu, slot);
        gpu->tex[slot].used = 1;
    }
    if (gpu->tex[slot].image == NULL) {
        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_RGBA8_UNORM;
        idesc.width = width;
        idesc.height = height;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                      LC_IMAGE_USAGE_TRANSFER_DST;
        idesc.flags = LC_IMAGE_FLAG_NONE;
        idesc.samples = LC_SAMPLE_COUNT_1;
        if (lc_image_create(device, &idesc,
                            &gpu->tex[slot].image) != LC_SUCCESS) {
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
        if (lc_image_view_create(gpu->tex[slot].image, &vdesc,
                                 &gpu->tex[slot].view) != LC_SUCCESS) {
            lc_image_destroy(gpu->tex[slot].image);
            gpu->tex[slot].image = NULL;
            return 0;
        }
        memset(&write, 0, sizeof(write));
        write.binding = 0;
        write.array_element = 0;
        write.type = LC_BINDING_SAMPLED_IMAGE;
        write.u.image.view = gpu->tex[slot].view;
        if (lc_binding_set_create(gpu->tex_layout,
                                  &gpu->tex[slot].set) != LC_SUCCESS) {
            lc_image_view_destroy(gpu->tex[slot].view);
            lc_image_destroy(gpu->tex[slot].image);
            gpu->tex[slot].view = NULL;
            gpu->tex[slot].image = NULL;
            return 0;
        }
        if (lc_binding_set_update(gpu->tex[slot].set, &write, 1) !=
            LC_SUCCESS) {
            lc_binding_set_destroy(gpu->tex[slot].set);
            lc_image_view_destroy(gpu->tex[slot].view);
            lc_image_destroy(gpu->tex[slot].image);
            gpu->tex[slot].set = NULL;
            gpu->tex[slot].view = NULL;
            gpu->tex[slot].image = NULL;
            return 0;
        }
        gpu->tex[slot].width = width;
        gpu->tex[slot].height = height;
    }
    /* Upload (whole image on create; partial-rect upload is a future
     * refinement — WantUpdates re-uploads the full UsedRect today,
     * which is correct and bounded by the 64-slot atlas budget). */
    memset(&up, 0, sizeof(up));
    up.mip_level = 0;
    up.array_layer = 0;
    up.width = width;
    up.height = height;
    up.depth = 1;
    up.data = pixels;
    up.data_size = pixel_bytes;
    if (lc_image_write(gpu->tex[slot].image, &up) != LC_SUCCESS) {
        return 0;
    }
    /* Refresh the binding (re-validate sampled-readable state after
     * the upload transitioned the image). */
    memset(&write, 0, sizeof(write));
    write.binding = 0;
    write.array_element = 0;
    write.type = LC_BINDING_SAMPLED_IMAGE;
    write.u.image.view = gpu->tex[slot].view;
    if (lc_binding_set_update(gpu->tex[slot].set, &write, 1) !=
        LC_SUCCESS) {
        return 0;
    }
    return 1;
}
