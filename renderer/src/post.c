/*
 * Luma Renderer post-process chain (Phase 18 foundation) + monotonic
 * timing helpers. Public LumaC only — no backend, platform, or
 * internal headers beyond lumac.h (backend-independence audit).
 *
 * Chain model: render_scene draws sky + items into the HDR target,
 * then runs each enabled post stage reading the chain head and
 * writing a ping-pong intermediate (RGBA16F, extent-keyed,
 * lazily allocated, reused across frames). render_output tonemaps
 * from the chain head (post output, or the HDR view when empty).
 * Exactly one verification stage exists today (tint multiply).
 */

#include <stdlib.h>
#include <string.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

/* Embedded SPIR-V (generated at build time from staged .spv). */
extern const unsigned char lr_fulltri_vert_spv[];
extern const unsigned long lr_fulltri_vert_spv_size;
extern const unsigned char lr_post_tint_frag_spv[];
extern const unsigned long lr_post_tint_frag_spv_size;

/* Tint push block: rgb multiplier (12B, fragment visibility). */
typedef struct lr_post_tint_push {
    float tint[3];
} lr_post_tint_push;

uint64_t lr_perf_frequency(void) {
    return lc_clock_frequency();
}

uint64_t lr_perf_now(void) {
    return lc_clock_now();
}

double lr_perf_to_ms(uint64_t ticks, uint64_t frequency) {
    double t;
    double f;

    if (frequency == 0) {
        return 0.0;
    }
    /* Ratio in double (exact for realistic tick magnitudes); a zero
     * frequency is guarded above, NaN/Inf inputs cannot occur from
     * the integer clock sources. */
    t = (double)ticks;
    f = (double)frequency;
    if (!(t >= 0.0) || !(f > 0.0)) {
        return 0.0;
    }
    return (t * 1000.0) / f;
}

lr_result lr_post_set_stage(lr_renderer *renderer, lr_post_stage stage) {
    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (stage != LR_POST_NONE && stage != LR_POST_TINT_VERIFY) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    renderer->post_stage = stage;
    renderer->post_head_view = NULL;
    return LR_SUCCESS;
}

lr_result lr_post_set_tint(lr_renderer *renderer, const float rgb[3]) {
    if (renderer == NULL || rgb == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    renderer->post_tint[0] = rgb[0];
    renderer->post_tint[1] = rgb[1];
    renderer->post_tint[2] = rgb[2];
    return LR_SUCCESS;
}

/* Destroy one ping-pong side (device drained by the caller). */
static void lr_post_destroy_side(lr_renderer *renderer, uint32_t slot,
                                 uint32_t side) {
    lc_render_target_destroy(renderer->post_targets[slot][side]);
    renderer->post_targets[slot][side] = NULL;
    lc_image_view_destroy(renderer->post_views[slot][side]);
    renderer->post_views[slot][side] = NULL;
    lc_image_destroy(renderer->post_images[slot][side]);
    renderer->post_images[slot][side] = NULL;
}

void lr_post_destroy(lr_renderer *renderer) {
    uint32_t slot;
    uint32_t side;

    if (renderer == NULL) {
        return;
    }
    for (slot = 0; slot < 2; slot++) {
        for (side = 0; side < 2; side++) {
            lr_post_destroy_side(renderer, slot, side);
        }
        renderer->post_width[slot] = 0;
        renderer->post_height[slot] = 0;
        if (renderer->post_tint_pipelines[slot] != NULL) {
            lc_pipeline_destroy(renderer->post_tint_pipelines[slot]);
            renderer->post_tint_pipelines[slot] = NULL;
        }
        if (renderer->post_stage_sets[slot] != NULL) {
            lc_binding_set_destroy(renderer->post_stage_sets[slot]);
            renderer->post_stage_sets[slot] = NULL;
        }
        renderer->post_pipeline_epochs[slot] = 0;
    }
    if (renderer->post_tint_fragment_shader != NULL) {
        lc_shader_destroy(renderer->post_tint_fragment_shader);
        renderer->post_tint_fragment_shader = NULL;
    }
    renderer->post_head_view = NULL;
    renderer->post_head = 0;
}

/* Drop intermediates without destroying pipelines/sets (extent change
 * keeps compiled state; images rebuild lazily on next ensure). */
void lr_post_invalidate(lr_renderer *renderer) {
    uint32_t slot;
    uint32_t side;

    if (renderer == NULL) {
        return;
    }
    if (renderer->device != NULL) {
        lc_device_wait_idle(renderer->device);
    }
    for (slot = 0; slot < 2; slot++) {
        for (side = 0; side < 2; side++) {
            lr_post_destroy_side(renderer, slot, side);
        }
        renderer->post_width[slot] = 0;
        renderer->post_height[slot] = 0;
    }
    renderer->post_head_view = NULL;
    renderer->post_head = 0;
}

/* Ensure one slot's ping-pong pair at an extent (lazy; reused across
 * frames; rebuilt only on extent change). */
lr_result lr_post_ensure(lr_renderer *renderer, uint32_t slot,
                         uint32_t width, uint32_t height) {
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_render_target_create_desc tdesc;
    lc_render_target_attachment att;
    uint32_t side;

    if (renderer == NULL || slot >= 2 || width == 0 || height == 0) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->post_width[slot] == width &&
        renderer->post_height[slot] == height &&
        renderer->post_images[slot][0] != NULL &&
        renderer->post_images[slot][1] != NULL) {
        return LR_SUCCESS;
    }
    /* Drain first: an in-flight post read may still sample the old
     * pair (same Phase-16 class of fault as HDR/env rebuilds). */
    lc_device_wait_idle(renderer->device);
    lr_post_destroy_side(renderer, slot, 0);
    lr_post_destroy_side(renderer, slot, 1);
    for (side = 0; side < 2; side++) {
        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_RGBA16_FLOAT;
        idesc.width = width;
        idesc.height = height;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        /* Capture-capable (TRANSFER_SRC) like the HDR target: chain
         * intermediates stay publicly readable for debugging. */
        idesc.usage = (uint32_t)LC_IMAGE_USAGE_SAMPLED |
                      (uint32_t)LC_IMAGE_USAGE_TRANSFER_SRC |
                      (uint32_t)LC_IMAGE_USAGE_TRANSFER_DST |
                      (uint32_t)LC_IMAGE_USAGE_COLOR_ATTACHMENT;
        idesc.samples = LC_SAMPLE_COUNT_1;
        if (lc_image_create(renderer->device, &idesc,
                            &renderer->post_images[slot][side]) !=
            LC_SUCCESS) {
            lr_post_destroy_side(renderer, slot, 0);
            lr_post_destroy_side(renderer, slot, 1);
            return LR_ERROR_RENDER;
        }
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.mip_level_count = 1;
        vdesc.array_layer_count = 1;
        if (lc_image_view_create(renderer->post_images[slot][side],
                                 &vdesc,
                                 &renderer->post_views[slot][side]) !=
            LC_SUCCESS) {
            lr_post_destroy_side(renderer, slot, 0);
            lr_post_destroy_side(renderer, slot, 1);
            return LR_ERROR_RENDER;
        }
        memset(&tdesc, 0, sizeof(tdesc));
        tdesc.width = width;
        tdesc.height = height;
        att.view = renderer->post_views[slot][side];
        tdesc.color_attachments = &att;
        tdesc.color_attachment_count = 1;
        tdesc.depth_stencil_attachment = NULL;
        if (lc_render_target_create(
                renderer->device, &tdesc,
                &renderer->post_targets[slot][side]) != LC_SUCCESS) {
            lr_post_destroy_side(renderer, slot, 0);
            lr_post_destroy_side(renderer, slot, 1);
            return LR_ERROR_RENDER;
        }
    }
    renderer->post_width[slot] = width;
    renderer->post_height[slot] = height;
    return LR_SUCCESS;
}

/* Compile (once per slot signature) the tint pipeline. */
static lr_result lr_post_tint_pipeline_for(lr_renderer *renderer,
                                           uint32_t slot,
                                           lc_pipeline **out) {
    lc_render_target_desc sig;
    lc_push_constant_range push;
    lc_graphics_pipeline_desc pd;
    const lc_binding_layout *layouts[1];

    if (renderer->post_tint_pipelines[slot] != NULL) {
        *out = renderer->post_tint_pipelines[slot];
        return LR_SUCCESS;
    }
    if (renderer->post_tint_fragment_shader == NULL) {
        lc_shader_desc sdesc;

        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.stage = LC_SHADER_STAGE_FRAGMENT;
        sdesc.code = lr_post_tint_frag_spv;
        sdesc.code_size = (size_t)lr_post_tint_frag_spv_size;
        sdesc.entry_point = NULL;
        if (lc_shader_create(renderer->device, &sdesc,
                             &renderer->post_tint_fragment_shader) !=
            LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    if (renderer->fulltri_vertex_shader == NULL) {
        return LR_ERROR_RENDER;
    }
    memset(&sig, 0, sizeof(sig));
    sig.color_attachment_count = 1;
    sig.color_formats[0] = LC_FORMAT_RGBA16_FLOAT;
    sig.depth_stencil_format = LC_FORMAT_UNDEFINED;
    sig.samples = LC_SAMPLE_COUNT_1;
    memset(&pd, 0, sizeof(pd));
    pd.vertex_shader = renderer->fulltri_vertex_shader;
    pd.fragment_shader = renderer->post_tint_fragment_shader;
    layouts[0] = renderer->post_layout;
    pd.binding_layouts = layouts;
    pd.binding_layout_count = 1;
    pd.cull_mode = LC_CULL_NONE;
    pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
    push.visibility = (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT;
    push.offset = 0;
    push.size = sizeof(lr_post_tint_push);
    pd.push_constant_ranges = &push;
    pd.push_constant_range_count = 1;
    pd.render_target = sig;
    if (lc_graphics_pipeline_create(
            renderer->device, &pd,
            &renderer->post_tint_pipelines[slot]) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    *out = renderer->post_tint_pipelines[slot];
    return LR_SUCCESS;
}

/* Record one tint stage: head -> ping (or pong), head follows. */
static lr_result lr_post_record_tint(lr_renderer *renderer,
                                     lc_command_encoder *enc, uint32_t slot,
                                     lc_image_view *input) {
    lc_pipeline *pipeline = NULL;
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_binding_write writes[2];
    lr_post_tint_push push;
    lc_result cr;
    uint32_t dst_side = (uint32_t)(1 - renderer->post_head);

    if (renderer->post_stage_sets[slot] == NULL) {
        if (lc_binding_set_create(renderer->post_layout,
                                  &renderer->post_stage_sets[slot]) !=
            LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    if (lr_post_tint_pipeline_for(renderer, slot, &pipeline) !=
        LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_SAMPLED_IMAGE;
    writes[0].u.image.view = input;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLER;
    writes[1].u.sampler.sampler = renderer->default_sampler;
    /* Per-frame rewrite is legal here: the prior frame drained
     * (frame serialization), so no in-flight bind observes the
     * update — the same rule sky/tonemap sets follow. */
    if (lc_binding_set_update(renderer->post_stage_sets[slot], writes,
                              2) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    memset(&catt, 0, sizeof(catt));
    catt.view = renderer->post_views[slot][dst_side];
    catt.load_op = LC_LOAD_OP_DONT_CARE;
    catt.store_op = LC_STORE_OP_STORE;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.depth_attachment = NULL;
    pdesc.width = renderer->post_width[slot];
    pdesc.height = renderer->post_height[slot];
    cr = lc_encoder_begin_render_pass(enc, &pdesc);
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    cr = lc_encoder_bind_pipeline(enc, pipeline);
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_bind_binding_set(enc, pipeline, 0,
                                         renderer->post_stage_sets[slot]);
    }
    if (cr == LC_SUCCESS) {
        push.tint[0] = renderer->post_tint[0];
        push.tint[1] = renderer->post_tint[1];
        push.tint[2] = renderer->post_tint[2];
        cr = lc_encoder_push_constants(
            enc, pipeline, (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT, 0,
            sizeof(push), &push);
    }
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_draw(enc, 3, 0);
    }
    if (cr != LC_SUCCESS) {
        /* Best effort: leave the pass closed so the frame stays
         * balanced for the caller's end_frame. */
        lc_encoder_end_render_pass(enc);
        return lr_map_result(cr);
    }
    cr = lc_encoder_end_render_pass(enc);
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    renderer->post_head = (int)dst_side;
    renderer->post_head_view = renderer->post_views[slot][dst_side];
    renderer->profile.post_passes++;
    return LR_SUCCESS;
}

/* Record the enabled stages for one output extent slot. Input is the
 * HDR view; on success post_head_view names the chain output (NULL
 * when the chain is empty). */
lr_result lr_post_record(lr_renderer *renderer, lc_command_encoder *enc,
                         uint32_t slot) {
    if (renderer == NULL || enc == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    renderer->post_head_view = NULL;
    renderer->post_head = 0;
    if (renderer->post_stage == LR_POST_NONE) {
        return LR_SUCCESS;
    }
    if (renderer->hdr_view == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (lr_post_ensure(renderer, slot, renderer->hdr_width,
                       renderer->hdr_height) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (renderer->post_stage == LR_POST_TINT_VERIFY) {
        return lr_post_record_tint(renderer, enc, slot,
                                   renderer->hdr_view);
    }
    return LR_ERROR_UNSUPPORTED;
}
