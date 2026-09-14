/*
 * Luma Renderer core (Phases 13-15): renderer object, camera/light
 * uploads, pipeline cache, and recording. Public LumaC only — no
 * backend, platform, or internal headers beyond lumac.h (enforced by
 * the backend-independence audit).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

/* Embedded SPIR-V (generated at build time from staged .spv). */
extern const unsigned char lr_unlit_vert_spv[];
extern const unsigned long lr_unlit_vert_spv_size;
extern const unsigned char lr_unlit_frag_spv[];
extern const unsigned long lr_unlit_frag_spv_size;
extern const unsigned char lr_pbr_vert_spv[];
extern const unsigned long lr_pbr_vert_spv_size;
extern const unsigned char lr_pbr_frag_spv[];
extern const unsigned long lr_pbr_frag_spv_size;
extern const unsigned char lr_shadow_vert_spv[];
extern const unsigned long lr_shadow_vert_spv_size;

/* Camera GPU block: view + proj + viewProj + position (208 bytes). */
typedef struct lr_camera_gpu {
    float view[16];
    float proj[16];
    float view_proj[16];
    float cam_pos[4];
} lr_camera_gpu;

/* Unlit push block: model matrix + material color (80 bytes). */
typedef struct lr_unlit_push {
    float model[16];
    float color[4];
} lr_unlit_push;

/* Shadow defaults (also documented on lr_shadow_desc): negative
 * biases select these; zero disables. */
#define LR_SHADOW_DEFAULT_DEPTH_BIAS 0.0015f
#define LR_SHADOW_DEFAULT_NORMAL_BIAS 0.02f
#define LR_SHADOW_DEFAULT_DISTANCE 25.0f

static int lr_target_shape_valid(const lc_render_target_desc *rt) {
    uint32_t i;

    if (rt == NULL) {
        return 0;
    }
    if (rt->color_attachment_count == 0 ||
        rt->color_attachment_count > LC_MAX_COLOR_ATTACHMENTS) {
        return 0;
    }
    for (i = 0; i < rt->color_attachment_count; i++) {
        if (rt->color_formats[i] == LC_FORMAT_UNDEFINED) {
            return 0;
        }
    }
    if (rt->samples != LC_SAMPLE_COUNT_1 &&
        rt->samples != LC_SAMPLE_COUNT_2 &&
        rt->samples != LC_SAMPLE_COUNT_4 &&
        rt->samples != LC_SAMPLE_COUNT_8) {
        return 0;
    }
    return 1;
}

lr_result lr_renderer_pipeline_for(lr_renderer *renderer,
                                   const lc_render_target_desc *signature,
                                   lr_material_type material_type,
                                   lc_cull_mode cull_mode,
                                   lc_pipeline **out_pipeline) {
    uint32_t i;
    lc_graphics_pipeline_desc pd;
    lc_vertex_binding_desc vbinding;
    lc_vertex_attribute_desc vattrs[4];
    lc_push_constant_range push;
    lc_pipeline *pipeline = NULL;
    lc_result res;
    int want_pbr;

    if (renderer == NULL || signature == NULL || out_pipeline == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (material_type != LR_MATERIAL_UNLIT &&
        material_type != LR_MATERIAL_PBR_METALLIC_ROUGHNESS) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    want_pbr = (material_type == LR_MATERIAL_PBR_METALLIC_ROUGHNESS);
    for (i = 0; i < renderer->pipeline_count; i++) {
        /* Extent-independent structural compare (mirrors the LumaC
         * rule: counts, formats, depth, samples — never extent or
         * object identity) plus material type and cull mode. */
        const lc_render_target_desc *cached =
            &renderer->pipelines[i].signature;
        uint32_t k;
        int equal = 0;

        if (renderer->pipelines[i].material_type == material_type &&
            renderer->pipelines[i].cull_mode == cull_mode &&
            cached->color_attachment_count ==
                signature->color_attachment_count &&
            cached->depth_stencil_format ==
                signature->depth_stencil_format &&
            cached->samples == signature->samples) {
            equal = 1;
            for (k = 0; k < cached->color_attachment_count; k++) {
                if (cached->color_formats[k] !=
                    signature->color_formats[k]) {
                    equal = 0;
                    break;
                }
            }
        }
        if (equal) {
            *out_pipeline = renderer->pipelines[i].pipeline;
            return LR_SUCCESS;
        }
    }
    if (renderer->pipeline_count >= LR_PIPELINE_CACHE_MAX) {
        return LR_ERROR_UNSUPPORTED;
    }

    /* Shared vertex layout over lr_vertex (56-byte stride). */
    vbinding.binding = 0;
    vbinding.stride = sizeof(lr_vertex);
    vbinding.input_rate = LC_VERTEX_INPUT_PER_VERTEX;
    vattrs[0].location = 0;
    vattrs[0].binding = 0;
    vattrs[0].format = LC_FORMAT_RGB32_FLOAT;
    vattrs[0].offset = 0;
    vattrs[1].location = 1;
    vattrs[1].binding = 0;
    vattrs[1].format = LC_FORMAT_RGB32_FLOAT;
    vattrs[1].offset = sizeof(float) * 3u;
    vattrs[2].location = 2;
    vattrs[2].binding = 0;
    vattrs[2].format = LC_FORMAT_RGBA32_FLOAT;
    vattrs[2].offset = sizeof(float) * 6u;
    vattrs[3].location = 3;
    vattrs[3].binding = 0;
    vattrs[3].format = LC_FORMAT_RG32_FLOAT;
    vattrs[3].offset = sizeof(float) * 10u;
    if (want_pbr) {
        /* Model + normal matrices ride push (both stages: the
         * fragment side carries the receive-shadow flag). 116B. */
        push.visibility = (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
                          (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT;
        push.offset = 0;
        push.size = sizeof(lr_pbr_push);
    } else {
        push.visibility = (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
                          (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT;
        push.offset = 0;
        push.size = sizeof(lr_unlit_push);
    }

    memset(&pd, 0, sizeof(pd));
    pd.vertex_shader =
        want_pbr ? renderer->pbr_vertex_shader : renderer->vertex_shader;
    pd.fragment_shader = want_pbr ? renderer->pbr_fragment_shader
                                  : renderer->fragment_shader;
    pd.vertex_bindings = &vbinding;
    pd.vertex_binding_count = 1;
    pd.vertex_attributes = vattrs;
    pd.vertex_attribute_count = 4;
    {
        /* PBR draws bind slot 0 (material set) + slot 1 (frame
         * shadow set) + slot 2 (frame environment set); unlit
         * keeps its single slot. */
        const lc_binding_layout *slots[3];

        slots[0] = want_pbr ? renderer->pbr_layout : renderer->layout;
        if (want_pbr) {
            slots[1] = renderer->shadow_layout;
            slots[2] = renderer->env_layout;
            pd.binding_layouts = slots;
            pd.binding_layout_count = 3;
        } else {
            pd.binding_layouts = slots;
            pd.binding_layout_count = 1;
        }
        pd.cull_mode = cull_mode;
        pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
        pd.depth_test_enable =
            (signature->depth_stencil_format == LC_FORMAT_UNDEFINED) ? 0 : 1;
        pd.depth_write_enable = pd.depth_test_enable;
        pd.push_constant_ranges = &push;
        pd.push_constant_range_count = 1;
        pd.render_target = *signature;
        res = lc_graphics_pipeline_create(renderer->device, &pd, &pipeline);
        if (res != LC_SUCCESS) {
            return lr_map_result(res);
        }
    }
    renderer->pipelines[renderer->pipeline_count].signature = *signature;
    renderer->pipelines[renderer->pipeline_count].material_type =
        material_type;
    renderer->pipelines[renderer->pipeline_count].cull_mode = cull_mode;
    renderer->pipelines[renderer->pipeline_count].pipeline = pipeline;
    renderer->pipeline_count++;
    *out_pipeline = pipeline;
    return LR_SUCCESS;
}

/* Depth-only pipeline for shadow passes: fragment-less, model-only
 * push, shared depth layout, cull NONE (one documented rule for all
 * casters — never the main-pass culling state). Mini-cache keyed by
 * signature only. */
lr_result lr_renderer_depth_pipeline_for(
    lr_renderer *renderer, const lc_render_target_desc *signature,
    lc_pipeline **out_pipeline) {
    uint32_t i;
    lc_graphics_pipeline_desc pd;
    lc_vertex_binding_desc vbinding;
    lc_vertex_attribute_desc vattrs[4];
    lc_push_constant_range push;
    lc_pipeline *pipeline = NULL;
    lc_result res;

    if (renderer == NULL || signature == NULL || out_pipeline == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < renderer->depth_pipeline_count; i++) {
        const lc_render_target_desc *cached =
            &renderer->depth_pipelines[i].signature;
        uint32_t k;
        int equal = 0;

        if (cached->color_attachment_count ==
                signature->color_attachment_count &&
            cached->depth_stencil_format ==
                signature->depth_stencil_format &&
            cached->samples == signature->samples) {
            equal = 1;
            for (k = 0; k < cached->color_attachment_count; k++) {
                if (cached->color_formats[k] !=
                    signature->color_formats[k]) {
                    equal = 0;
                    break;
                }
            }
        }
        if (equal) {
            *out_pipeline = renderer->depth_pipelines[i].pipeline;
            return LR_SUCCESS;
        }
    }
    if (renderer->depth_pipeline_count >= 4) {
        return LR_ERROR_UNSUPPORTED;
    }
    vbinding.binding = 0;
    vbinding.stride = sizeof(lr_vertex);
    vbinding.input_rate = LC_VERTEX_INPUT_PER_VERTEX;
    /* Position only (locations 1-3 ride the stride but stay
     * undeclared, so no unused-attribute warnings). */
    vattrs[0].location = 0;
    vattrs[0].binding = 0;
    vattrs[0].format = LC_FORMAT_RGB32_FLOAT;
    vattrs[0].offset = 0;
    push.visibility = (uint32_t)LC_SHADER_VISIBILITY_VERTEX;
    push.offset = 0;
    push.size = sizeof(lr_shadow_push);

    memset(&pd, 0, sizeof(pd));
    pd.vertex_shader = renderer->depth_vertex_shader;
    pd.fragment_shader = NULL; /* rasterization without fragments */
    pd.vertex_bindings = &vbinding;
    pd.vertex_binding_count = 1;
    pd.vertex_attributes = vattrs;
    pd.vertex_attribute_count = 1;
    {
        const lc_binding_layout *slots[1];

        slots[0] = renderer->depth_layout;
        pd.binding_layouts = slots;
        pd.binding_layout_count = 1;
        pd.cull_mode = LC_CULL_NONE;
        pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
        pd.depth_test_enable =
            (signature->depth_stencil_format == LC_FORMAT_UNDEFINED) ? 0 : 1;
        pd.depth_write_enable = pd.depth_test_enable;
        pd.push_constant_ranges = &push;
        pd.push_constant_range_count = 1;
        pd.render_target = *signature;
        res = lc_graphics_pipeline_create(renderer->device, &pd, &pipeline);
        if (res != LC_SUCCESS) {
            return lr_map_result(res);
        }
    }
    renderer->depth_pipelines[renderer->depth_pipeline_count].signature =
        *signature;
    renderer->depth_pipelines[renderer->depth_pipeline_count].material_type =
        LR_MATERIAL_UNKNOWN;
    renderer->depth_pipelines[renderer->depth_pipeline_count].cull_mode =
        LC_CULL_NONE;
    renderer->depth_pipelines[renderer->depth_pipeline_count].pipeline =
        pipeline;
    renderer->depth_pipeline_count++;
    *out_pipeline = pipeline;
    return LR_SUCCESS;
}

/* One 1x1 fallback texture + view (UNORM; endpoints are exact in
 * any transfer, so one format serves sRGB and linear roles). */
static lr_result lr_create_fallback(lc_device *device,
                                    const unsigned char pixels[4],
                                    lc_image **out_image,
                                    lc_image_view **out_view) {
    lc_image_desc idesc;
    lc_image_upload_desc upload;
    lc_image_view_desc vdesc;

    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RGBA8_UNORM;
    idesc.width = 1;
    idesc.height = 1;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &idesc, out_image) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    memset(&upload, 0, sizeof(upload));
    upload.width = 1;
    upload.height = 1;
    upload.depth = 1;
    upload.data = pixels;
    upload.data_size = 4;
    if (lc_image_write(*out_image, &upload) != LC_SUCCESS) {
        lc_image_destroy(*out_image);
        *out_image = NULL;
        return LR_ERROR_RENDER;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(*out_image, &vdesc, out_view) != LC_SUCCESS) {
        lc_image_destroy(*out_image);
        *out_image = NULL;
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

/* Shadow-map depth format preference (creation success = support):
 * D32 first, D16 fallback. Stencil formats are skipped (sampling
 * .x gives depth either way, but stencil aspects complicate the
 * sampled-read transition for zero benefit here). */
static lc_format lr_shadow_pick_format(lc_device *device) {
    static const lc_format candidates[2] = { LC_FORMAT_D32_FLOAT,
                                             LC_FORMAT_D16_UNORM };
    uint32_t i;

    for (i = 0; i < 2; i++) {
        lc_image_desc idesc;
        lc_image *probe = NULL;

        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = candidates[i];
        idesc.width = 64;
        idesc.height = 64;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        idesc.usage =
            LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_DEPTH_STENCIL;
        idesc.samples = LC_SAMPLE_COUNT_1;
        if (lc_image_create(device, &idesc, &probe) == LC_SUCCESS) {
            lc_image_destroy(probe);
            return candidates[i];
        }
    }
    return LC_FORMAT_UNDEFINED;
}

static void lr_shadow_slot_destroy(lr_shadow_slot *slot) {
    if (slot == NULL) {
        return;
    }
    lc_render_target_destroy(slot->target);
    lc_image_view_destroy(slot->view);
    lc_image_destroy(slot->image);
    slot->target = NULL;
    slot->view = NULL;
    slot->image = NULL;
    slot->resolution = 0;
    slot->format = LC_FORMAT_UNDEFINED;
}

/* Create (or recreate, on resolution change) one slot's depth
 * image + view + depth-only target. Depth images carry SAMPLED
 * usage so passes finalize them sampled-readable (LumaC Phase 16
 * behavior); formats come from the renderer-wide pick.
 *
 * Ownership note: recreation destroys the sampled view. Callers
 * must have drained in-flight shadow-sampling work first (a prior
 * frame still executing would fault on the freed image). The test
 * harness serializes frames with a device idle for this reason. */
static lr_result lr_shadow_slot_ensure(lr_renderer *renderer,
                                       lr_shadow_slot *slot,
                                       uint32_t resolution,
                                       lc_format format) {
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_render_target_create_desc tdesc;

    if (slot->image != NULL && slot->resolution == resolution &&
        slot->format == format) {
        return LR_SUCCESS;
    }
    /* Drop the frame-set binding BEFORE destroying the view: wrapper
     * addresses recycle through the allocator, so identity (not
     * pointer) comparison governs the rebind below. */
    if (slot >= renderer->slots &&
        slot < renderer->slots + LR_MAX_SHADOWS) {
        renderer->shadow_bound[slot - renderer->slots] = 0;
    }
    lr_shadow_slot_destroy(slot);
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = format;
    idesc.width = resolution;
    idesc.height = resolution;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    /* Capture-capable: shadow-map readback (debugging/AI
     * diagnostics) needs TRANSFER_SRC; harmless for depth output. */
    idesc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_DEPTH_STENCIL |
                  LC_IMAGE_USAGE_TRANSFER_SRC;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(renderer->device, &idesc, &slot->image) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(slot->image, &vdesc, &slot->view) !=
        LC_SUCCESS) {
        lr_shadow_slot_destroy(slot);
        return LR_ERROR_RENDER;
    }
    memset(&tdesc, 0, sizeof(tdesc));
    tdesc.width = resolution;
    tdesc.height = resolution;
    tdesc.color_attachment_count = 0;
    tdesc.depth_stencil_attachment = slot->view;
    if (lc_render_target_create(renderer->device, &tdesc, &slot->target) !=
        LC_SUCCESS) {
        lr_shadow_slot_destroy(slot);
        return LR_ERROR_RENDER;
    }
    slot->resolution = resolution;
    slot->format = format;
    return LR_SUCCESS;
}

/* Power-of-two test for shadow resolutions. */
static int lr_is_pow2(uint32_t v) {
    return (v != 0u) && ((v & (v - 1u)) == 0u);
}

lr_result lr_renderer_create(const lr_renderer_desc *desc,
                              lr_renderer **out_renderer) {
    lr_renderer *renderer;
    lc_buffer_desc bdesc;
    lc_binding_desc slots[3];
    lc_binding_desc pbr_slots[9];
    lc_binding_layout_desc ldesc;
    lc_sampler_desc smdesc;
    lc_shader_desc sdesc;
    lc_pipeline *primed = NULL;
    lr_result res;
    /* Neutral 1x1 texels: white base, identity metal/rough (G=B=1
     * so factors pass through), flat tangent-space normal, white
     * occlusion (identity). Emissive is white too: emissive =
     * factor x texel, so a factor-only material (legal glTF) must
     * see identity — the zero default factor keeps it dark. */
    static const unsigned char white[4] = { 255, 255, 255, 255 };
    static const unsigned char neutral_mr[4] = { 0, 255, 255, 255 };
    static const unsigned char flat_normal[4] = { 128, 128, 255, 255 };

    if (desc == NULL || out_renderer == NULL) {
        if (out_renderer != NULL) {
            *out_renderer = NULL;
        }
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (desc->device == NULL || desc->max_objects == 0 ||
        !lr_target_shape_valid(&desc->render_target)) {
        *out_renderer = NULL;
        return LR_ERROR_INVALID_ARGUMENT;
    }

    renderer = (lr_renderer *)calloc(1, sizeof(lr_renderer));
    if (renderer == NULL) {
        *out_renderer = NULL;
        return LR_ERROR_OUT_OF_MEMORY;
    }
    renderer->device = desc->device;
    renderer->primary = desc->render_target;
    renderer->max_objects = desc->max_objects;
    /* Post chain defaults: empty (OFF), identity tint. */
    renderer->post_stage = LR_POST_NONE;
    renderer->post_tint[0] = 1.0f;
    renderer->post_tint[1] = 1.0f;
    renderer->post_tint[2] = 1.0f;
    /* Temporary ambient fallback: explicit value wins; all-zero
     * selects the 0.03 gray default (documented, Phase 16+ IBL). */
    if (desc->ambient_light[0] == 0.0f && desc->ambient_light[1] == 0.0f &&
        desc->ambient_light[2] == 0.0f) {
        renderer->ambient_light[0] = 0.03f;
        renderer->ambient_light[1] = 0.03f;
        renderer->ambient_light[2] = 0.03f;
    } else {
        memcpy(renderer->ambient_light, desc->ambient_light,
               sizeof(renderer->ambient_light));
    }

    renderer->queue = (lr_queued_item *)calloc(
        desc->max_objects, sizeof(lr_queued_item));
    if (renderer->queue == NULL) {
        free(renderer);
        *out_renderer = NULL;
        return LR_ERROR_OUT_OF_MEMORY;
    }

    /* Shared camera buffer (208 bytes, persistently mapped). */
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = sizeof(lr_camera_gpu);
    bdesc.usage = LC_BUFFER_USAGE_UNIFORM;
    bdesc.memory = LC_MEMORY_CPU_TO_GPU;
    if (lc_buffer_create(renderer->device, &bdesc, &renderer->camera_buffer) !=
            LC_SUCCESS ||
        lc_buffer_map(renderer->camera_buffer, &renderer->camera_mapped) !=
            LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }

    /* Standard unlit layout: camera UBO + base texture + sampler. */
    slots[0].binding = 0;
    slots[0].type = LC_BINDING_UNIFORM_BUFFER;
    slots[0].count = 1;
    slots[0].visibility = LC_SHADER_VISIBILITY_VERTEX;
    slots[1].binding = 1;
    slots[1].type = LC_BINDING_SAMPLED_IMAGE;
    slots[1].count = 1;
    slots[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    slots[2].binding = 2;
    slots[2].type = LC_BINDING_SAMPLER;
    slots[2].count = 1;
    slots[2].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    ldesc.bindings = slots;
    ldesc.binding_count = 3;
    if (lc_binding_layout_create(renderer->device, &ldesc,
                                 &renderer->layout) != LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }

    /* PBR layout: camera (both stages: fragment needs camPos) +
     * lights + material UBO + five maps + one sampler. */
    pbr_slots[0].binding = 0;
    pbr_slots[0].type = LC_BINDING_UNIFORM_BUFFER;
    pbr_slots[0].count = 1;
    pbr_slots[0].visibility = LC_SHADER_VISIBILITY_ALL_GRAPHICS;
    pbr_slots[1].binding = 1;
    pbr_slots[1].type = LC_BINDING_UNIFORM_BUFFER;
    pbr_slots[1].count = 1;
    pbr_slots[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    pbr_slots[2].binding = 2;
    pbr_slots[2].type = LC_BINDING_UNIFORM_BUFFER;
    pbr_slots[2].count = 1;
    pbr_slots[2].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    {
        uint32_t b;

        for (b = 3; b <= 7; b++) {
            pbr_slots[b].binding = b;
            pbr_slots[b].type = LC_BINDING_SAMPLED_IMAGE;
            pbr_slots[b].count = 1;
            pbr_slots[b].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        }
    }
    pbr_slots[8].binding = 8;
    pbr_slots[8].type = LC_BINDING_SAMPLER;
    pbr_slots[8].count = 1;
    pbr_slots[8].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    ldesc.bindings = pbr_slots;
    ldesc.binding_count = 9;
    if (lc_binding_layout_create(renderer->device, &ldesc,
                                 &renderer->pbr_layout) != LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }

    /* Fallback textures (bound for every missing map, so shaders
     * never branch on resource presence). */
    res = lr_create_fallback(renderer->device, white,
                             &renderer->fallback_image,
                             &renderer->fallback_view);
    if (res != LR_SUCCESS) {
        goto fail;
    }
    res = lr_create_fallback(renderer->device, neutral_mr,
                             &renderer->fallback_mr_image,
                             &renderer->fallback_mr_view);
    if (res != LR_SUCCESS) {
        goto fail;
    }
    res = lr_create_fallback(renderer->device, flat_normal,
                             &renderer->fallback_normal_image,
                             &renderer->fallback_normal_view);
    if (res != LR_SUCCESS) {
        goto fail;
    }
    res = lr_create_fallback(renderer->device, white,
                             &renderer->fallback_occlusion_image,
                             &renderer->fallback_occlusion_view);
    if (res != LR_SUCCESS) {
        goto fail;
    }
    res = lr_create_fallback(renderer->device, white,
                             &renderer->fallback_emissive_image,
                             &renderer->fallback_emissive_view);
    if (res != LR_SUCCESS) {
        goto fail;
    }
    memset(&smdesc, 0, sizeof(smdesc));
    smdesc.min_filter = LC_FILTER_LINEAR;
    smdesc.mag_filter = LC_FILTER_LINEAR;
    smdesc.mipmap_mode = LC_MIPMAP_MODE_NEAREST;
    smdesc.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
    smdesc.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
    smdesc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
    smdesc.max_anisotropy = 1.0f;
    if (lc_sampler_create(renderer->device, &smdesc,
                           &renderer->default_sampler) != LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }

    /* Environment resources: layouts, mapped params, empty set
     * (cheap; shaders/pipelines stay lazy until first use). */
    res = lr_renderer_create_env_resources(renderer);
    if (res != LR_SUCCESS) {
        goto fail;
    }

    /* Unlit shaders (embedded bytes; modules outlive the renderer for
     * the lazy pipeline cache). */
    sdesc.stage = LC_SHADER_STAGE_VERTEX;
    sdesc.code = lr_unlit_vert_spv;
    sdesc.code_size = (size_t)lr_unlit_vert_spv_size;
    sdesc.entry_point = NULL;
    if (lc_shader_create(renderer->device, &sdesc,
                         &renderer->vertex_shader) != LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }
    sdesc.stage = LC_SHADER_STAGE_FRAGMENT;
    sdesc.code = lr_unlit_frag_spv;
    sdesc.code_size = (size_t)lr_unlit_frag_spv_size;
    sdesc.entry_point = NULL;
    if (lc_shader_create(renderer->device, &sdesc,
                         &renderer->fragment_shader) != LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }

    /* PBR shaders (same lifetime discipline). */
    sdesc.stage = LC_SHADER_STAGE_VERTEX;
    sdesc.code = lr_pbr_vert_spv;
    sdesc.code_size = (size_t)lr_pbr_vert_spv_size;
    sdesc.entry_point = NULL;
    if (lc_shader_create(renderer->device, &sdesc,
                         &renderer->pbr_vertex_shader) != LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }
    sdesc.stage = LC_SHADER_STAGE_FRAGMENT;
    sdesc.code = lr_pbr_frag_spv;
    sdesc.code_size = (size_t)lr_pbr_frag_spv_size;
    sdesc.entry_point = NULL;
    if (lc_shader_create(renderer->device, &sdesc,
                         &renderer->pbr_fragment_shader) != LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }

    /* Depth vertex shader (fragment-less depth passes). */
    sdesc.stage = LC_SHADER_STAGE_VERTEX;
    sdesc.code = lr_shadow_vert_spv;
    sdesc.code_size = (size_t)lr_shadow_vert_spv_size;
    sdesc.entry_point = NULL;
    if (lc_shader_create(renderer->device, &sdesc,
                         &renderer->depth_vertex_shader) != LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }

    /* Shared light uniform buffer (persistently mapped; per-render
     * content updates, descriptor sets never rebuilt for lights). */
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = sizeof(lr_lights_gpu);
    bdesc.usage = LC_BUFFER_USAGE_UNIFORM;
    bdesc.memory = LC_MEMORY_CPU_TO_GPU;
    if (lc_buffer_create(renderer->device, &bdesc,
                         &renderer->light_buffer) != LC_SUCCESS ||
        lc_buffer_map(renderer->light_buffer, &renderer->light_mapped) !=
            LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }

    /* Shadow metadata buffer (TEMP DIAG: GPU_ONLY + staging writes
     * to test mapped-write visibility). */
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = sizeof(lr_shadows_gpu);
    bdesc.usage = LC_BUFFER_USAGE_UNIFORM;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(renderer->device, &bdesc,
                         &renderer->shadow_meta_buffer) != LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }
    renderer->shadow_meta_mapped = NULL;

    /* Depth-pass VP buffer (TEMP DIAG: same). */
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = sizeof(lr_depth_vp_gpu);
    bdesc.usage = LC_BUFFER_USAGE_UNIFORM;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(renderer->device, &bdesc,
                         &renderer->depth_vp_buffer) != LC_SUCCESS) {
        res = LR_ERROR_RENDER;
        goto fail;
    }
    renderer->depth_vp_mapped = NULL;

    /* Frame shadow layout: metadata UBO + 4 map slots + sampler.
     * One renderer-owned set; image bindings update only on slot
     * (re)creation, metadata flows through mapped memory. */
    {
        lc_binding_desc sh_slots[6];
        lc_binding_write sh_writes[6];
        uint32_t b;

        sh_slots[0].binding = 0;
        sh_slots[0].type = LC_BINDING_UNIFORM_BUFFER;
        sh_slots[0].count = 1;
        sh_slots[0].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        for (b = 1; b <= 4; b++) {
            sh_slots[b].binding = b;
            sh_slots[b].type = LC_BINDING_SAMPLED_IMAGE;
            sh_slots[b].count = 1;
            sh_slots[b].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        }
        sh_slots[5].binding = 5;
        sh_slots[5].type = LC_BINDING_SAMPLER;
        sh_slots[5].count = 1;
        sh_slots[5].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
        ldesc.bindings = sh_slots;
        ldesc.binding_count = 6;
        if (lc_binding_layout_create(renderer->device, &ldesc,
                                     &renderer->shadow_layout) !=
            LC_SUCCESS) {
            res = LR_ERROR_RENDER;
            goto fail;
        }
        if (lc_binding_set_create(renderer->shadow_layout,
                                  &renderer->shadow_set) != LC_SUCCESS) {
            res = LR_ERROR_RENDER;
            goto fail;
        }
        sh_writes[0].binding = 0;
        sh_writes[0].array_element = 0;
        sh_writes[0].type = LC_BINDING_UNIFORM_BUFFER;
        sh_writes[0].u.buffer.buffer = renderer->shadow_meta_buffer;
        sh_writes[0].u.buffer.offset = 0;
        sh_writes[0].u.buffer.size = 0;
        /* Inactive slots sample the white fallback (value 1 = lit). */
        for (b = 1; b <= 4; b++) {
            sh_writes[b].binding = b;
            sh_writes[b].array_element = 0;
            sh_writes[b].type = LC_BINDING_SAMPLED_IMAGE;
            sh_writes[b].u.image.view = renderer->fallback_view;
        }
        sh_writes[5].binding = 5;
        sh_writes[5].array_element = 0;
        sh_writes[5].type = LC_BINDING_SAMPLER;
        sh_writes[5].u.sampler.sampler = renderer->default_sampler;
        if (lc_binding_set_update(renderer->shadow_set, sh_writes, 6) !=
            LC_SUCCESS) {
            res = LR_ERROR_RENDER;
            goto fail;
        }
    }

    /* Depth-pass layout + set (VP buffer bound once, rewritten per
     * shadow light without descriptor traffic). */
    {
        lc_binding_desc dslot;
        lc_binding_write dwrite;

        dslot.binding = 0;
        dslot.type = LC_BINDING_UNIFORM_BUFFER;
        dslot.count = 1;
        dslot.visibility = LC_SHADER_VISIBILITY_VERTEX;
        ldesc.bindings = &dslot;
        ldesc.binding_count = 1;
        if (lc_binding_layout_create(renderer->device, &ldesc,
                                     &renderer->depth_layout) !=
            LC_SUCCESS) {
            res = LR_ERROR_RENDER;
            goto fail;
        }
        if (lc_binding_set_create(renderer->depth_layout,
                                  &renderer->depth_set) != LC_SUCCESS) {
            res = LR_ERROR_RENDER;
            goto fail;
        }
        dwrite.binding = 0;
        dwrite.array_element = 0;
        dwrite.type = LC_BINDING_UNIFORM_BUFFER;
        dwrite.u.buffer.buffer = renderer->depth_vp_buffer;
        dwrite.u.buffer.offset = 0;
        dwrite.u.buffer.size = 0;
        if (lc_binding_set_update(renderer->depth_set, &dwrite, 1) !=
            LC_SUCCESS) {
            res = LR_ERROR_RENDER;
            goto fail;
        }
    }

    /* Eager shadow slots (fail fast on the whole shadow path). */
    {
        lc_format shadow_format =
            lr_shadow_pick_format(renderer->device);
        uint32_t s;

        if (shadow_format == LC_FORMAT_UNDEFINED) {
            res = LR_ERROR_RENDER;
            goto fail;
        }
        for (s = 0; s < LR_MAX_SHADOWS; s++) {
            res = lr_shadow_slot_ensure(
                renderer, &renderer->slots[s],
                LR_SHADOW_RESOLUTION_DEFAULT, shadow_format);
            if (res != LR_SUCCESS) {
                goto fail;
            }
        }
    }

    /* Fail fast: pre-build both primary pipelines now. */
    res = lr_renderer_pipeline_for(renderer, &renderer->primary,
                                   LR_MATERIAL_UNLIT, LC_CULL_BACK, &primed);
    if (res != LR_SUCCESS) {
        goto fail;
    }
    res = lr_renderer_pipeline_for(renderer, &renderer->primary,
                                   LR_MATERIAL_PBR_METALLIC_ROUGHNESS,
                                   LC_CULL_BACK, &primed);
    if (res != LR_SUCCESS) {
        goto fail;
    }
    /* Fail fast on the shadow path too (depth pipeline over the
     * slot signature proves fragment-less depth rendering works). */
    {
        lc_render_target_desc depth_sig;

        memset(&depth_sig, 0, sizeof(depth_sig));
        depth_sig.width = LR_SHADOW_RESOLUTION_DEFAULT;
        depth_sig.height = LR_SHADOW_RESOLUTION_DEFAULT;
        depth_sig.color_attachment_count = 0;
        depth_sig.depth_stencil_format = renderer->slots[0].format;
        depth_sig.samples = LC_SAMPLE_COUNT_1;
        res = lr_renderer_depth_pipeline_for(renderer, &depth_sig,
                                             &primed);
        if (res != LR_SUCCESS) {
            goto fail;
        }
    }
    (void)primed;

    *out_renderer = renderer;
    return LR_SUCCESS;

fail:
    lr_renderer_destroy(renderer);
    *out_renderer = NULL;
    return res;
}

void lr_renderer_destroy(lr_renderer *renderer) {
    uint32_t i;

    if (renderer == NULL) {
        return;
    }
    while (renderer->materials != NULL) {
        lr_material_destroy(renderer->materials);
    }
    while (renderer->meshes != NULL) {
        lr_mesh_destroy(renderer->meshes);
    }
    for (i = 0; i < renderer->pipeline_count; i++) {
        lc_pipeline_destroy(renderer->pipelines[i].pipeline);
        renderer->pipelines[i].pipeline = NULL;
    }
    renderer->pipeline_count = 0;
    for (i = 0; i < renderer->depth_pipeline_count; i++) {
        lc_pipeline_destroy(renderer->depth_pipelines[i].pipeline);
        renderer->depth_pipelines[i].pipeline = NULL;
    }
    renderer->depth_pipeline_count = 0;
    for (i = 0; i < renderer->instanced_pipeline_count; i++) {
        lc_pipeline_destroy(renderer->instanced_pipelines[i].pipeline);
        renderer->instanced_pipelines[i].pipeline = NULL;
    }
    renderer->instanced_pipeline_count = 0;
    lr_gpu_destroy_shared(renderer);
    lr_post_destroy(renderer);
    lr_renderer_destroy_env_resources(renderer);
    for (i = 0; i < LR_MAX_SHADOWS; i++) {
        lr_shadow_slot_destroy(&renderer->slots[i]);
    }
    lc_binding_set_destroy(renderer->depth_set);
    lc_binding_layout_destroy(renderer->depth_layout);
    lc_binding_set_destroy(renderer->shadow_set);
    lc_binding_layout_destroy(renderer->shadow_layout);
    lc_shader_destroy(renderer->depth_vertex_shader);
    lc_shader_destroy(renderer->pbr_fragment_shader);
    lc_shader_destroy(renderer->pbr_vertex_shader);
    lc_shader_destroy(renderer->fragment_shader);
    lc_shader_destroy(renderer->vertex_shader);
    lc_binding_layout_destroy(renderer->pbr_layout);
    lc_binding_layout_destroy(renderer->layout);
    lc_buffer_destroy(renderer->shadow_meta_buffer);
    renderer->shadow_meta_mapped = NULL;
    lc_buffer_destroy(renderer->depth_vp_buffer);
    renderer->depth_vp_mapped = NULL;
    lc_buffer_destroy(renderer->light_buffer);
    renderer->light_mapped = NULL;
    lc_buffer_destroy(renderer->camera_buffer);
    renderer->camera_mapped = NULL;
    lc_sampler_destroy(renderer->default_sampler);
    lc_image_view_destroy(renderer->fallback_emissive_view);
    lc_image_destroy(renderer->fallback_emissive_image);
    lc_image_view_destroy(renderer->fallback_occlusion_view);
    lc_image_destroy(renderer->fallback_occlusion_image);
    lc_image_view_destroy(renderer->fallback_normal_view);
    lc_image_destroy(renderer->fallback_normal_image);
    lc_image_view_destroy(renderer->fallback_mr_view);
    lc_image_destroy(renderer->fallback_mr_image);
    lc_image_view_destroy(renderer->fallback_view);
    lc_image_destroy(renderer->fallback_image);
    free(renderer->queue);
    free(renderer);
}

lr_result lr_renderer_begin(lr_renderer *renderer,
                            const lr_camera *camera) {
    lr_camera_gpu gpu;

    if (renderer == NULL || camera == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Camera sanity (mirrors set_perspective rules; garbage matrices
     * from hand-filled structs are rejected, not uploaded). */
    if (!(camera->vertical_fov > 0.0f) ||
        !(camera->vertical_fov < 3.14159265358979323846f) ||
        !(camera->aspect_ratio > 0.0f) || !(camera->near_plane > 0.0f) ||
        !(camera->far_plane > camera->near_plane)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->camera_mapped == NULL) {
        return LR_ERROR_RENDER;
    }
    renderer->camera = *camera;    lr_mat4_multiply(gpu.view_proj, camera->projection, camera->view);
    memcpy(gpu.view, camera->view, sizeof(gpu.view));
    memcpy(gpu.proj, camera->projection, sizeof(gpu.proj));
    gpu.cam_pos[0] = camera->position[0];
    gpu.cam_pos[1] = camera->position[1];
    gpu.cam_pos[2] = camera->position[2];
    gpu.cam_pos[3] = 1.0f;
    memcpy(renderer->camera_mapped, &gpu, sizeof(gpu));
    lr_frustum_from_viewproj(gpu.view_proj, renderer->frustum_planes);
    renderer->queued = 0;
    renderer->light_count = 0;
    renderer->shadow_assigned = 0;
    renderer->shadows_prepared = 0;
    memset(renderer->shadow_slot_active, 0,
           sizeof(renderer->shadow_slot_active));
    /* Zeroed metadata = no shadowed lights until prepare fills
     * slots; skipping prepare can never shadow stale maps. Staged
     * through the buffer API (ordered like all uploads). */
    memset(&renderer->cpu_meta, 0, sizeof(renderer->cpu_meta));
    if (lc_buffer_write(renderer->shadow_meta_buffer, 0,
                        &renderer->cpu_meta,
                        sizeof(renderer->cpu_meta)) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    memset(&renderer->stats, 0, sizeof(renderer->stats));
    memset(&renderer->profile, 0, sizeof(renderer->profile));
    renderer->hdr_has_scene = 0;
    renderer->post_head_view = NULL;
    renderer->post_head = 0;
    renderer->frame_number++;
    renderer->profile.frame_number = renderer->frame_number;
    if (renderer->perf_freq == 0) {
        renderer->perf_freq = lr_perf_frequency();
    }
    renderer->t_begin = lr_perf_now();
    renderer->t_shadow_start = 0;
    renderer->t_shadow_end = 0;
    renderer->t_main_start = 0;
    renderer->t_main_end = 0;
    renderer->t_sky = 0;
    renderer->t_post = 0;
    renderer->t_tonemap = 0;
    renderer->frame_open = 1;
    return LR_SUCCESS;
}

/* Shadow-config validation (pure; no GPU). Normalizes nothing;
 * defaults resolve at prepare time (negative bias = default). */
static lr_result lr_shadow_desc_validate(const lr_shadow_desc *shadow,
                                         lr_light_type type, float range) {
    if (shadow == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!shadow->enabled) {
        return LR_SUCCESS;
    }
    if (type == LR_LIGHT_POINT) {
        /* Points stay non-shadowing in Phase 16 (no cubemaps yet). */
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (type != LR_LIGHT_DIRECTIONAL && type != LR_LIGHT_SPOT) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (shadow->resolution != 0 &&
        (!lr_is_pow2(shadow->resolution) || shadow->resolution < 128u ||
         shadow->resolution > 4096u)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!isfinite(shadow->depth_bias) || !isfinite(shadow->normal_bias) ||
        !isfinite(shadow->near_plane) || !isfinite(shadow->far_plane) ||
        !isfinite(shadow->shadow_distance)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (shadow->near_plane < 0.0f || shadow->far_plane < 0.0f ||
        shadow->shadow_distance < 0.0f) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (type == LR_LIGHT_DIRECTIONAL) {
        /* Directional depth range is fit-derived; explicit planes
         * would silently do nothing, so they are rejected. */
        if (shadow->near_plane > 0.0f || shadow->far_plane > 0.0f) {
            return LR_ERROR_INVALID_ARGUMENT;
        }
        return LR_SUCCESS;
    }
    /* Spot: effective range must admit a positive depth span. */
    {
        float eff_near =
            (shadow->near_plane > 0.0f) ? shadow->near_plane : 0.5f;
        float eff_far = (shadow->far_plane > 0.0f &&
                         shadow->far_plane < range)
                            ? shadow->far_plane
                            : range;

        if (!(eff_far > eff_near)) {
            return LR_ERROR_INVALID_ARGUMENT;
        }
    }
    return LR_SUCCESS;
}

lr_result lr_renderer_submit_light(lr_renderer *renderer,
                                   const lr_light *light) {
    lr_submitted_light *slot;
    uint32_t i;

    if (renderer == NULL || light == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!renderer->frame_open || renderer->shadows_prepared) {
        /* Submissions belong before prepare (shadow maps would
         * otherwise miss them); order violations are loud. */
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->light_count >= LR_MAX_LIGHTS) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (light->type != LR_LIGHT_DIRECTIONAL &&
        light->type != LR_LIGHT_POINT && light->type != LR_LIGHT_SPOT) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Finite components only (NaN/Inf would poison the frame). */
    if (!isfinite(light->intensity)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < 3; i++) {
        if (!isfinite(light->color[i])) {
            return LR_ERROR_INVALID_ARGUMENT;
        }
    }
    slot = &renderer->lights[renderer->light_count];
    memset(slot, 0, sizeof(*slot));
    slot->pub.type = light->type;
    memcpy(slot->pub.color, light->color, sizeof(slot->pub.color));
    slot->pub.intensity = light->intensity;
    slot->shadow_slot = -1;
    if (light->type == LR_LIGHT_DIRECTIONAL) {
        for (i = 0; i < 3; i++) {
            if (!isfinite(light->direction[i])) {
                return LR_ERROR_INVALID_ARGUMENT;
            }
        }
        /* Stored normalized (travel direction); shaders use L=-dir. */
        if (!lr_vec3_normalize(light->direction, slot->pub.direction)) {
            return LR_ERROR_INVALID_ARGUMENT;
        }
    } else {
        /* Point + spot share position/range validation. */
        for (i = 0; i < 3; i++) {
            if (!isfinite(light->position[i])) {
                return LR_ERROR_INVALID_ARGUMENT;
            }
        }
        if (!isfinite(light->range) || !(light->range > 0.0f)) {
            return LR_ERROR_INVALID_ARGUMENT;
        }
        memcpy(slot->pub.position, light->position,
               sizeof(slot->pub.position));
        slot->pub.range = light->range;
        if (light->type == LR_LIGHT_SPOT) {
            for (i = 0; i < 3; i++) {
                if (!isfinite(light->direction[i])) {
                    return LR_ERROR_INVALID_ARGUMENT;
                }
            }
            if (!lr_vec3_normalize(light->direction,
                                   slot->pub.direction)) {
                return LR_ERROR_INVALID_ARGUMENT;
            }
            /* Radians, 0 <= inner <= outer < pi/2. */
            if (!isfinite(light->spot_inner) ||
                !isfinite(light->spot_outer) ||
                !(light->spot_inner >= 0.0f) ||
                !(light->spot_outer > 0.0f) ||
                !(light->spot_outer < 1.5707963267948966f) ||
                !(light->spot_inner <= light->spot_outer)) {
                return LR_ERROR_INVALID_ARGUMENT;
            }
            slot->pub.spot_inner = light->spot_inner;
            slot->pub.spot_outer = light->spot_outer;
        }
    }
    /* Shadow config validates now; slot assigns deterministically in
     * submit order (first LR_MAX_SHADOWS win; extras unshadowed). */
    if (lr_shadow_desc_validate(&light->shadow, light->type,
                                slot->pub.range) != LR_SUCCESS) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    slot->shadow = light->shadow;
    if (light->shadow.enabled &&
        renderer->shadow_assigned < LR_MAX_SHADOWS) {
        slot->shadow_slot = (int)renderer->shadow_assigned;
        renderer->shadow_assigned++;
        renderer->stats.shadow_casting_lights++;
    }
    renderer->light_count++;
    renderer->stats.submitted_lights++;
    return LR_SUCCESS;
}

void lr_renderer_set_ambient(lr_renderer *renderer,
                             const float ambient[3]) {
    if (renderer == NULL) {
        return;
    }
    if (ambient == NULL) {
        renderer->ambient_light[0] = 0.0f;
        renderer->ambient_light[1] = 0.0f;
        renderer->ambient_light[2] = 0.0f;
        return;
    }
    memcpy(renderer->ambient_light, ambient, sizeof(renderer->ambient_light));
}

void lr_renderer_get_ambient(const lr_renderer *renderer,
                             float out_ambient[3]) {
    if (out_ambient == NULL) {
        return;
    }
    if (renderer == NULL) {
        out_ambient[0] = 0.0f;
        out_ambient[1] = 0.0f;
        out_ambient[2] = 0.0f;
        return;
    }
    memcpy(out_ambient, renderer->ambient_light,
           sizeof(renderer->ambient_light));
}

uint32_t lr_renderer_get_pipeline_count(const lr_renderer *renderer) {
    if (renderer == NULL) {
        return 0;
    }
    return renderer->pipeline_count;
}

uint32_t lr_renderer_get_shadow_count(const lr_renderer *renderer) {
    uint32_t n = 0;
    uint32_t i;

    if (renderer == NULL) {
        return 0;
    }
    for (i = 0; i < LR_MAX_SHADOWS; i++) {
        if (renderer->shadow_slot_active[i] != 0) {
            n++;
        }
    }
    return n;
}

void lr_renderer_get_shadow_slot_info(const lr_renderer *renderer,
                                      uint32_t index,
                                      lr_shadow_slot_info *out_info) {
    lr_shadow_slot_info empty;

    memset(&empty, 0, sizeof(empty));
    empty.format = LC_FORMAT_UNDEFINED;
    if (out_info == NULL) {
        return;
    }
    if (renderer == NULL || index >= LR_MAX_SHADOWS) {
        *out_info = empty;
        return;
    }
    empty.active = (renderer->shadow_slot_active[index] != 0) ? 1 : 0;
    empty.resolution = renderer->slots[index].resolution;
    empty.format = renderer->slots[index].format;
    empty.light_type = LR_LIGHT_DIRECTIONAL;
    {
        /* Report the served light's type when assigned. */
        uint32_t li;

        for (li = 0; li < renderer->light_count; li++) {
            if (renderer->lights[li].shadow_slot == (int)index) {
                empty.light_type = renderer->lights[li].pub.type;
                break;
            }
        }
    }
    *out_info = empty;
}

lc_image_view *lr_renderer_get_shadow_view(lr_renderer *renderer,
                                           uint32_t index) {
    if (renderer == NULL || index >= LR_MAX_SHADOWS) {
        return NULL;
    }
    return renderer->slots[index].view;
}

uint32_t lr_renderer_get_light_count(const lr_renderer *renderer) {
    if (renderer == NULL) {
        return 0;
    }
    return renderer->light_count;
}

void lr_renderer_get_light(const lr_renderer *renderer, uint32_t index,
                           lr_light *out_light,
                           lr_shadow_desc *out_shadow) {
    if (out_light != NULL) {
        memset(out_light, 0, sizeof(*out_light));
    }
    if (out_shadow != NULL) {
        memset(out_shadow, 0, sizeof(*out_shadow));
    }
    if (renderer == NULL || index >= renderer->light_count) {
        return;
    }
    if (out_light != NULL) {
        *out_light = renderer->lights[index].pub;
    }
    if (out_shadow != NULL) {
        *out_shadow = renderer->lights[index].shadow;
    }
}

/* Pack submitted lights + ambient into the mapped uniform block.
 * Shadow enablement additionally requires a prepared frame (skipped
 * prepare neutralizes every shadow request). Contents update only;
 * descriptor sets are never rebuilt. */
static void lr_upload_lights(lr_renderer *renderer) {
    lr_lights_gpu *gpu;
    uint32_t i;

    if (renderer->light_mapped == NULL) {
        return;
    }
    gpu = (lr_lights_gpu *)renderer->light_mapped;
    gpu->ambient[0] = renderer->ambient_light[0];
    gpu->ambient[1] = renderer->ambient_light[1];
    gpu->ambient[2] = renderer->ambient_light[2];
    gpu->ambient[3] = 0.0f;
    gpu->light_count = renderer->light_count;
    gpu->pad[0] = 0;
    gpu->pad[1] = 0;
    gpu->pad[2] = 0;
    for (i = 0; i < renderer->light_count; i++) {
        const lr_submitted_light *src = &renderer->lights[i];
        lr_light_gpu *dst = &gpu->lights[i];

        memset(dst, 0, sizeof(*dst));
        dst->color_intensity[0] = src->pub.color[0];
        dst->color_intensity[1] = src->pub.color[1];
        dst->color_intensity[2] = src->pub.color[2];
        dst->color_intensity[3] = src->pub.intensity;
        if (src->pub.type == LR_LIGHT_DIRECTIONAL) {
            dst->pos_range[0] = 0.0f;
            dst->pos_range[1] = 0.0f;
            dst->pos_range[2] = 0.0f;
            dst->pos_range[3] = 0.0f;
            dst->dir_kind[0] = src->pub.direction[0];
            dst->dir_kind[1] = src->pub.direction[1];
            dst->dir_kind[2] = src->pub.direction[2];
            dst->dir_kind[3] = 0.0f;
        } else {
            dst->pos_range[0] = src->pub.position[0];
            dst->pos_range[1] = src->pub.position[1];
            dst->pos_range[2] = src->pub.position[2];
            dst->pos_range[3] = src->pub.range;
            if (src->pub.type == LR_LIGHT_SPOT) {
                dst->dir_kind[0] = src->pub.direction[0];
                dst->dir_kind[1] = src->pub.direction[1];
                dst->dir_kind[2] = src->pub.direction[2];
                dst->dir_kind[3] = 2.0f;
                dst->spot_angles[0] = cosf(src->pub.spot_inner);
                dst->spot_angles[1] = cosf(src->pub.spot_outer);
            } else {
                dst->dir_kind[0] = 0.0f;
                dst->dir_kind[1] = 0.0f;
                dst->dir_kind[2] = 0.0f;
                dst->dir_kind[3] = 1.0f;
            }
        }
        /* Shadowed only with an assigned slot AND a prepared frame. */
        if (src->shadow_slot >= 0 && renderer->shadows_prepared) {
            dst->shadow_info[0] = 1.0f;
            dst->shadow_info[1] = (float)src->shadow_slot;
        } else {
            dst->shadow_info[0] = 0.0f;
            dst->shadow_info[1] = -1.0f;
        }
    }
}

/* Resolve per-light shadow parameters (resolution, biases). */
static void lr_shadow_resolve(const lr_shadow_desc *cfg, uint32_t *out_res,
                              float *out_const_bias, float *out_normal_bias) {
    *out_res = (cfg->resolution != 0) ? cfg->resolution
                                      : LR_SHADOW_RESOLUTION_DEFAULT;
    *out_const_bias = (cfg->depth_bias < 0.0f)
                          ? LR_SHADOW_DEFAULT_DEPTH_BIAS
                          : cfg->depth_bias;
    *out_normal_bias = (cfg->normal_bias < 0.0f)
                           ? LR_SHADOW_DEFAULT_NORMAL_BIAS
                           : cfg->normal_bias;
}

/* NDC depth of a view-space distance for the latched camera (for
 * directional slice selection). */
static float lr_view_distance_to_ndc(const lr_camera *camera, float dist) {
    float n = camera->near_plane;
    float f = camera->far_plane;
    float d;

    if (!(dist > n)) {
        return 0.0f;
    }
    if (!(dist < f)) {
        return 1.0f;
    }
    d = f * (dist - n) / (dist * (f - n));
    if (d < 0.0f) {
        return 0.0f;
    }
    if (d > 1.0f) {
        return 1.0f;
    }
    return d;
}

lr_result lr_renderer_render_shadows(lr_renderer *renderer,
                                     lc_command_encoder *encoder) {
    uint32_t li;

    if (renderer == NULL || encoder == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!renderer->frame_open) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->shadows_prepared) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    renderer->shadows_prepared = 1;
    renderer->t_shadow_start = lr_perf_now();
    if (renderer->shadow_assigned == 0) {
        renderer->t_shadow_end = renderer->t_shadow_start;
        renderer->profile.cpu_prepare_ms = 0.0;
        renderer->profile.cpu_shadow_ms = 0.0;
        return LR_SUCCESS; /* metadata already zeroed at begin */
    }
    /* Recreation destroys sampled attachments: idle once up front
     * when any assigned slot needs it (steady-state rewrites are
     * idempotent and need no drain). */
    {
        uint32_t ci;
        int need_idle = 0;

        for (ci = 0; ci < renderer->light_count && !need_idle; ci++) {
            int s = renderer->lights[ci].shadow_slot;
            uint32_t want;

            if (s < 0) {
                continue;
            }
            want = (renderer->lights[ci].shadow.resolution != 0)
                       ? renderer->lights[ci].shadow.resolution
                       : LR_SHADOW_RESOLUTION_DEFAULT;
            if (renderer->slots[s].image == NULL ||
                renderer->slots[s].resolution != want) {
                need_idle = 1;
            }
        }
        if (need_idle) {
            lc_device_wait_idle(renderer->device);
        }
    }
    for (li = 0; li < renderer->light_count; li++) {
        lr_submitted_light *light = &renderer->lights[li];
        int slot;
        uint32_t res;
        float const_bias;
        float normal_bias;
                float vp[16];
        float planes[6][4];
        float view[16]; /* spot branch scratch (dir uses fit view) */        lr_depth_vp_gpu pass_vp;
        lc_render_pass_desc pdesc;
        lc_render_depth_attachment datt;
        lc_render_target_desc signature;
        lc_pipeline *depth_pipeline = NULL;
        lr_result resr;
        lc_result cr;
        uint32_t i;
        lr_mesh *bound_mesh = NULL;

        if (light->shadow_slot < 0) {
            continue;
        }
        slot = light->shadow_slot;
        lr_shadow_resolve(&light->shadow, &res, &const_bias, &normal_bias);
        /* Slot resources follow the light's resolution (recreated
         * only on change). The frame-set rebind happens AFTER the
         * depth pass below: binding validation requires sampled-
         * readable state, which the pass itself establishes. */
        if (lr_shadow_slot_ensure(renderer, &renderer->slots[slot], res,
                                   renderer->slots[0].format) != LR_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        if (light->pub.type == LR_LIGHT_DIRECTIONAL) {
            float dist;
            float z_slice;
            float corners[8][3];
            float bounds[6];
            float eye[3];
            float light_view[16];
            float ortho[16];
            float cam_vp[16];

            dist = (light->shadow.shadow_distance > 0.0f)
                       ? light->shadow.shadow_distance
                       : LR_SHADOW_DEFAULT_DISTANCE;
            z_slice = lr_view_distance_to_ndc(&renderer->camera, dist);
            lr_mat4_multiply(cam_vp, renderer->camera.projection,
                             renderer->camera.view);
            lr_shadow_frustum_corners(cam_vp, 0.0f, z_slice, corners);
            lr_shadow_fit_directional(corners, light->pub.direction,
                                      bounds, eye, light_view);
            lr_mat4_ortho(ortho, bounds[0], bounds[1], bounds[2],
                          bounds[3], bounds[4], bounds[5]);
            lr_mat4_multiply(vp, ortho, light_view);
        } else {
            /* Spot: perspective off the cone (aspect 1). */
            float target[3];
            float near_z =
                (light->shadow.near_plane > 0.0f)
                    ? light->shadow.near_plane
                    : 0.5f;
            float far_z = (light->shadow.far_plane > 0.0f &&
                           light->shadow.far_plane < light->pub.range)
                              ? light->shadow.far_plane
                              : light->pub.range;
            float persp[16];

            if (!(far_z > near_z)) {
                return LR_ERROR_RENDER;
            }
            target[0] =
                light->pub.position[0] + light->pub.direction[0];
            target[1] =
                light->pub.position[1] + light->pub.direction[1];
            target[2] =
                light->pub.position[2] + light->pub.direction[2];
            lr_shadow_light_view(light->pub.position, target, view);
            lr_mat4_perspective(persp, light->pub.spot_outer * 2.0f, 1.0f,
                                near_z, far_z);
            lr_mat4_multiply(vp, persp, view);
        }
        /* Per-pass VP upload (staged, ordered with the pass). */
        memcpy(pass_vp.view_proj, vp, sizeof(pass_vp.view_proj));
        if (lc_buffer_write(renderer->depth_vp_buffer, 0, &pass_vp,
                            sizeof(pass_vp)) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        lr_frustum_from_viewproj(vp, planes);
        /* Depth-only pass over this slot's target. */
        memset(&datt, 0, sizeof(datt));
        datt.view = renderer->slots[slot].view;
        datt.depth_load_op = LC_LOAD_OP_CLEAR;
        datt.depth_store_op = LC_STORE_OP_STORE;
        datt.clear_depth = 1.0f;
        datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
        datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
        datt.clear_stencil = 0;
        memset(&pdesc, 0, sizeof(pdesc));
        pdesc.color_attachment_count = 0;
        pdesc.depth_attachment = &datt;
        pdesc.width = res;
        pdesc.height = res;
        if (lc_encoder_begin_render_pass(encoder, &pdesc) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        memset(&signature, 0, sizeof(signature));
        signature.width = res;
        signature.height = res;
        signature.color_attachment_count = 0;
        signature.depth_stencil_format = renderer->slots[slot].format;
        signature.samples = LC_SAMPLE_COUNT_1;
        resr = lr_renderer_depth_pipeline_for(renderer, &signature,
                                              &depth_pipeline);
        if (resr != LR_SUCCESS) {
            return resr;
        }
        if (!lc_render_target_is_compatible_with_pipeline(
                renderer->slots[slot].target, depth_pipeline)) {
            return LR_ERROR_INCOMPATIBLE;
        }
        cr = lc_encoder_bind_pipeline(encoder, depth_pipeline);
        if (cr != LC_SUCCESS) {
            return lr_map_result(cr);
        }
        cr = lc_encoder_bind_binding_set(encoder, depth_pipeline, 0,
                                         renderer->depth_set);
        if (cr != LC_SUCCESS) {
            return lr_map_result(cr);
        }
        /* Casters: every queued caster inside the light volume
         * (main-camera culling does not apply here). */
        for (i = 0; i < renderer->queued; i++) {
            lr_queued_item *item = &renderer->queue[i];
            lr_shadow_push push;

            if (!item->casts_shadow) {
                continue;
            }
            if (!lr_mesh_is_live(renderer, item->mesh)) {
                continue;
            }
            if (!lr_frustum_test_sphere(planes, item->sphere_center,
                                        item->sphere_radius)) {
                renderer->stats.shadow_casters_culled++;
                continue;
            }
            if (item->mesh != bound_mesh) {
                cr = lc_encoder_bind_vertex_buffer(
                    encoder, 0, item->mesh->vertex_buffer, 0);
                if (cr != LC_SUCCESS) {
                    return lr_map_result(cr);
                }
                cr = lc_encoder_bind_index_buffer(
                    encoder, item->mesh->index_buffer, 0, LC_INDEX_UINT32);
                if (cr != LC_SUCCESS) {
                    return lr_map_result(cr);
                }
                bound_mesh = item->mesh;
            }
            memcpy(push.model, item->matrix, sizeof(push.model));
            cr = lc_encoder_push_constants(
                encoder, depth_pipeline,
                (uint32_t)LC_SHADER_VISIBILITY_VERTEX, 0, sizeof(push),
                &push);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            cr = lc_encoder_draw_indexed(encoder, item->mesh->index_count,
                                         1, 0, 0, 0);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            renderer->stats.shadow_draw_calls++;
            renderer->stats.shadow_triangles +=
                item->mesh->index_count / 3u;
        }
        if (lc_encoder_end_render_pass(encoder) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        /* Now sampled-readable: (re)bind the slot view. Identity
         * compare (never pointer): a recreated view may reuse the
         * freed wrapper address. */
        if (renderer->shadow_bound[slot] !=
            lc_image_view_get_resource_id(
                renderer->slots[slot].view)) {
            lc_binding_write write;

            write.binding = (uint32_t)(1 + slot);
            write.array_element = 0;
            write.type = LC_BINDING_SAMPLED_IMAGE;
            write.u.image.view = renderer->slots[slot].view;
            if (lc_binding_set_update(renderer->shadow_set, &write, 1) !=
                LC_SUCCESS) {
                return LR_ERROR_RENDER;
            }
            renderer->shadow_bound[slot] =
                lc_image_view_get_resource_id(
                    renderer->slots[slot].view);
        }
        /* Metadata via staged write (ordered with the passes). */
        memcpy(renderer->cpu_meta.slots[slot].view_proj, vp,
               sizeof(renderer->cpu_meta.slots[slot].view_proj));
        renderer->cpu_meta.slots[slot].params[0] = 1.0f / (float)res;
        renderer->cpu_meta.slots[slot].params[1] = normal_bias;
        renderer->cpu_meta.slots[slot].params[2] = const_bias;
        renderer->cpu_meta.slots[slot].params[3] = 0.0f;
        renderer->cpu_meta.slot_count = renderer->shadow_assigned;
        if (lc_buffer_write(renderer->shadow_meta_buffer, 0,
                            &renderer->cpu_meta,
                            sizeof(renderer->cpu_meta)) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        renderer->shadow_slot_active[slot] = 1;
        renderer->stats.shadow_passes++;
        renderer->stats.shadow_maps_rendered++;
    }
    renderer->t_shadow_end = lr_perf_now();
    renderer->profile.cpu_prepare_ms =
        lr_perf_to_ms(renderer->t_shadow_end - renderer->t_shadow_start,
                      renderer->perf_freq);
    renderer->profile.cpu_shadow_ms = renderer->profile.cpu_prepare_ms;
    return LR_SUCCESS;
}

/* Shared item-draw engine (upload + sort + draws into the
 * caller's open pass for `target`). Used by the legacy direct path
 * and by the HDR scene pass alike. */
static lr_result lr_render_items(lr_renderer *renderer,
                                 lc_command_encoder *encoder,
                                 lc_render_target *target) {
    lc_render_target_desc signature;
    lr_result res;
    uint32_t i;
    lc_pipeline *bound_pipeline = NULL;
    lr_material *bound_material = NULL;
    lr_mesh *bound_mesh = NULL;
    const lr_environment *bound_env = NULL;
    uint64_t bound_env_epoch = 0;
    int env_bound = 0;

    if (renderer == NULL || encoder == NULL || target == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!renderer->frame_open) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Target signature through public getters (target must outlive
     * the call per the ownership contract). */
    memset(&signature, 0, sizeof(signature));
    signature.width = lc_render_target_get_width(target);
    signature.height = lc_render_target_get_height(target);
    signature.color_attachment_count =
        lc_render_target_get_color_count(target);
    for (i = 0; i < signature.color_attachment_count &&
                i < LC_MAX_COLOR_ATTACHMENTS;
         i++) {
        signature.color_formats[i] =
            lc_render_target_get_color_format(target, i);
    }
    signature.depth_stencil_format =
        lc_render_target_get_depth_format(target);
    signature.samples = lc_render_target_get_samples(target);
    if (signature.color_attachment_count == 0 ||
        signature.color_attachment_count > LC_MAX_COLOR_ATTACHMENTS) {
        return LR_ERROR_INCOMPATIBLE;
    }
    /* Lights upload once per render (mapped contents, no descriptor
     * rebuilds); stats observe the active count even with no draws. */
    lr_upload_lights(renderer);
    renderer->stats.active_lights = renderer->light_count;
    if (renderer->queued == 0) {
        return LR_SUCCESS;
    }
    lr_queue_sort(renderer->queue, renderer->queued);
    /* GPU-driven PBR draws first (prepared groups, one indirect
     * draw each); the CPU loop below then handles only unlit items
     * (and PBR items when no GPU preparation happened). */
    {
        int gpu_pbr_done = 0;

        if (renderer->render_mode == LR_RENDER_MODE_GPU_DRIVEN &&
            renderer->gpu_prepared_frame == renderer->frame_number) {
            if (lr_gpu_record_draws(renderer, encoder, target,
                                    &signature) != LR_SUCCESS) {
                return LR_ERROR_RENDER;
            }
            gpu_pbr_done = 1;
        }
        for (i = 0; i < renderer->queued; i++) {
            lr_queued_item *item = &renderer->queue[i];
            lc_pipeline *pipeline = NULL;
            lc_result cr;
            int want_pbr;

            /* Defensive skips (never dereference dead entries). */
            if (!lr_mesh_is_live(renderer, item->mesh) ||
                !lr_material_is_live(renderer, item->material)) {
                continue;
            }
            want_pbr = (item->material->type ==
                        LR_MATERIAL_PBR_METALLIC_ROUGHNESS);
            if (gpu_pbr_done && want_pbr) {
                continue;
            }
        /* Main-frustum-culled entries skip main draws but stay
         * queued as shadow casters (prepare already used them). */
        if (!item->main_visible) {
            continue;
        }
        want_pbr = (item->material->type ==
                    LR_MATERIAL_PBR_METALLIC_ROUGHNESS);
        if (!want_pbr &&
            item->material->type != LR_MATERIAL_UNLIT) {
            continue;
        }
        res = lr_renderer_pipeline_for(
            renderer, &signature, item->material->type,
            (want_pbr && item->material->double_sided) ? LC_CULL_NONE
                                                       : LC_CULL_BACK,
            &pipeline);
        if (res != LR_SUCCESS) {
            return res;
        }
        /* Defensive structural check (also rejects dead targets). */
        if (!lc_render_target_is_compatible_with_pipeline(target,
                                                          pipeline)) {
            return LR_ERROR_INCOMPATIBLE;
        }
        if (pipeline != bound_pipeline) {
            cr = lc_encoder_bind_pipeline(encoder, pipeline);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            bound_pipeline = pipeline;
            bound_material = NULL; /* sets are layout-specific */
            /* A pipeline switch disturbs every slot: force the
             * frame sets below to rebind (slot 1 here, slot 2 in
             * the environment block). */
            env_bound = 0;
            if (want_pbr) {
                /* Frame shadow set rides slot 1 (same object all
                 * frame; image bindings update only on slot
                 * recreation, metadata flows mapped). */
                cr = lc_encoder_bind_binding_set(encoder, pipeline, 1,
                                                 renderer->shadow_set);
                if (cr != LC_SUCCESS) {
                    return lr_map_result(cr);
                }
            }
            renderer->stats.pipeline_binds++;
        }
        if (want_pbr) {
            /* Frame environment set rides slot 2 (empty set with
             * no environment). Rebind on pipeline switch, object
             * or epoch change (Phase-16 lesson: wrapper addresses
             * recycle, so identity alone never proves validity;
             * rebuilds bump the epoch). */
            const lr_environment *env = renderer->active_env;
            uint64_t epoch = (env != NULL) ? env->source_epoch : 0;
            lc_binding_set *set = (env != NULL && env->ready)
                                      ? env->frame_set
                                      : renderer->empty_env_set;

            if (set != NULL &&
                (!env_bound || env != bound_env ||
                 epoch != bound_env_epoch)) {
                cr = lc_encoder_bind_binding_set(encoder, pipeline, 2,
                                                 set);
                if (cr != LC_SUCCESS) {
                    return lr_map_result(cr);
                }
                bound_env = env;
                bound_env_epoch = epoch;
                env_bound = 1;
            }
        }
        if (item->material != bound_material) {
            cr = lc_encoder_bind_binding_set(encoder, pipeline, 0,
                                             item->material->set);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            bound_material = item->material;
            renderer->stats.material_binds++;
        }
        if (item->mesh != bound_mesh) {
            cr = lc_encoder_bind_vertex_buffer(encoder, 0,
                                               item->mesh->vertex_buffer, 0);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            cr = lc_encoder_bind_index_buffer(encoder,
                                              item->mesh->index_buffer, 0,
                                              LC_INDEX_UINT32);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            bound_mesh = item->mesh;
        }
        if (want_pbr) {
            lr_pbr_push push;

            memcpy(push.model, item->matrix, sizeof(push.model));
            if (!lr_mat3_normal_from_mat4(item->matrix,
                                          push.normal_matrix)) {
                return LR_ERROR_RENDER;
            }
            push.flags = (item->receives_shadow != 0) ? 1u : 0u;
            cr = lc_encoder_push_constants(
                encoder, pipeline,
                (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
                    (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT,
                0, sizeof(push), &push);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
        } else {
            lr_unlit_push push;

            memcpy(push.model, item->matrix, sizeof(push.model));
            memcpy(push.color, item->material->color, sizeof(push.color));
            cr = lc_encoder_push_constants(
                encoder, pipeline,
                (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
                    (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT,
                0, sizeof(push), &push);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
        }
        cr = lc_encoder_draw_indexed(encoder, item->mesh->index_count, 1,
                                     0, 0, 0);
        if (cr != LC_SUCCESS) {
            return lr_map_result(cr);
        }
        renderer->stats.draw_calls++;
        renderer->stats.triangles += item->mesh->index_count / 3u;
        if (want_pbr) {
            renderer->stats.pbr_draw_calls++;
        } else {
            renderer->stats.unlit_draw_calls++;
        }
    }
    }
    return LR_SUCCESS;
}

/**
 * Record all visible items into the encoder's currently open pass
 * for `target` (caller begins/ends the pass; the target selects the
 * cached pipeline by signature). Sorts opaque items by material then
 * mesh to skip redundant binds. Skips entries whose mesh/material
 * died after submission.
 *
 * Legacy direct path: draws linear direct (+ IBL when an
 * environment is active, without sky) into the caller pass. For
 * the HDR + tonemap flow use render_scene/render_output.
 */
lr_result lr_renderer_render(lr_renderer *renderer,
                             lc_command_encoder *encoder,
                             lc_render_target *target) {
    if (renderer == NULL || encoder == NULL || target == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!renderer->frame_open) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    lr_environment_write_params(renderer);
    if (renderer->active_env != NULL) {
        renderer->stats.ibl_enabled = 1;
    }
    return lr_render_items(renderer, encoder, target);
}

lr_result lr_renderer_set_exposure(lr_renderer *renderer, float ev) {
    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!(ev == ev) || ev * 0.0f != 0.0f) {
        return LR_ERROR_INVALID_ARGUMENT; /* NaN or infinite */
    }
    renderer->exposure_ev = ev;
    return LR_SUCCESS;
}

float lr_renderer_get_exposure(const lr_renderer *renderer) {
    if (renderer == NULL) {
        return 0.0f;
    }
    return renderer->exposure_ev;
}

lr_result lr_renderer_set_tonemap_operator(lr_renderer *renderer,
                                           lr_tonemap_operator op) {
    if (renderer == NULL || (op != LR_TONEMAP_NONE && op != LR_TONEMAP_ACES)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    renderer->tonemap_op = op;
    return LR_SUCCESS;
}

lr_tonemap_operator
lr_renderer_get_tonemap_operator(const lr_renderer *renderer) {
    if (renderer == NULL) {
        return LR_TONEMAP_NONE;
    }
    return renderer->tonemap_op;
}

lr_result lr_renderer_render_scene(lr_renderer *renderer,
                                   lc_command_encoder *encoder,
                                   uint32_t width, uint32_t height) {
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_render_depth_attachment datt;
    lc_result cr;
    lr_result res;

    if (renderer == NULL || encoder == NULL || width == 0 ||
        height == 0) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!renderer->frame_open) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (lr_renderer_ensure_hdr(renderer, width, height) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (renderer->active_env != NULL) {
        res = lr_environment_ensure_built(renderer->active_env, encoder);
        if (res != LR_SUCCESS) {
            return res;
        }
    }
    lr_environment_write_params(renderer);
    if (renderer->active_env != NULL &&
        renderer->active_env->ready) {
        renderer->stats.ibl_enabled = 1;
    }
    /* GPU-driven visibility runs before the pass opens (dispatch
     * is illegal inside a render-pass instance). Legacy
     * lr_renderer_render callers in GPU mode must call
     * lr_renderer_prepare_gpu themselves before opening passes. */
    if (renderer->render_mode == LR_RENDER_MODE_GPU_DRIVEN) {
        uint64_t gpu0 = lr_perf_now();

        res = lr_gpu_prepare(renderer, encoder);
        renderer->gpu_stats.cpu_prepare_ms = lr_perf_to_ms(
            lr_perf_now() - gpu0, renderer->perf_freq);
        if (res != LR_SUCCESS) {
            return res;
        }
    }
    memset(&catt, 0, sizeof(catt));
    catt.view = renderer->hdr_view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    catt.clear_color[0] = 0.0f;
    catt.clear_color[1] = 0.0f;
    catt.clear_color[2] = 0.0f;
    catt.clear_color[3] = 1.0f;
    memset(&datt, 0, sizeof(datt));
    datt.view = renderer->hdr_depth_view;
    datt.depth_load_op = LC_LOAD_OP_CLEAR;
    datt.depth_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_depth = 1.0f;
    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_stencil = 0;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.depth_attachment = &datt;
    pdesc.width = renderer->hdr_width;
    pdesc.height = renderer->hdr_height;
    cr = lc_encoder_begin_render_pass(encoder, &pdesc);
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    /* Sky first (no depth test/write); scene geometry covers it. */
    if (renderer->active_env != NULL &&
        renderer->active_env->ready) {
        uint64_t sky0 = lr_perf_now();

        res = lr_renderer_record_sky(renderer, encoder);
        renderer->t_sky = lr_perf_now() - sky0;
        renderer->profile.cpu_sky_ms =
            lr_perf_to_ms(renderer->t_sky, renderer->perf_freq);
        if (res != LR_SUCCESS) {
            return res;
        }
    }
    renderer->t_main_start = lr_perf_now();
    res = lr_render_items(renderer, encoder, renderer->hdr_target);
    renderer->t_main_end = lr_perf_now();
    renderer->profile.cpu_main_ms =
        lr_perf_to_ms(renderer->t_main_end - renderer->t_main_start,
                      renderer->perf_freq);
    if (res != LR_SUCCESS) {
        return res;
    }
    cr = lc_encoder_end_render_pass(encoder);
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    /* Post chain (empty by default): HDR -> ping-pong -> head. */
    {
        uint64_t post0 = lr_perf_now();

        res = lr_post_record(renderer, encoder, 0);
        renderer->t_post = lr_perf_now() - post0;
        renderer->profile.cpu_post_ms =
            lr_perf_to_ms(renderer->t_post, renderer->perf_freq);
        if (res != LR_SUCCESS) {
            return res;
        }
    }
    renderer->hdr_has_scene = 1;
    return LR_SUCCESS;
}

lr_result lr_renderer_render_output(lr_renderer *renderer,
                                     lc_command_encoder *encoder,
                                     lc_render_target *target) {
    lr_result res;
    uint64_t t0;

    if (renderer == NULL || encoder == NULL || target == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!renderer->frame_open) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    t0 = lr_perf_now();
    res = lr_renderer_record_tonemap(renderer, encoder, target);
    renderer->t_tonemap = lr_perf_now() - t0;
    renderer->profile.cpu_tonemap_ms =
        lr_perf_to_ms(renderer->t_tonemap, renderer->perf_freq);
    return res;
}

lc_image_view *lr_renderer_get_hdr_view(lr_renderer *renderer) {
    if (renderer == NULL || renderer->hdr_view == NULL ||
        !renderer->hdr_has_scene) {
        return NULL;
    }
    return renderer->hdr_view;
}

lc_image_view *lr_renderer_get_brdf_view(lr_renderer *renderer) {
    if (renderer == NULL || renderer->brdf_view == NULL ||
        !renderer->brdf_ready) {
        return NULL;
    }
    return renderer->brdf_view;
}

void lr_renderer_get_environment_info(const lr_renderer *renderer,
                                      lr_environment_info *out_info) {
    if (out_info == NULL) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (renderer == NULL) {
        return;
    }
    out_info->exposure_ev = renderer->exposure_ev;
    out_info->tonemap = renderer->tonemap_op;
    if (renderer->hdr_target != NULL) {
        out_info->hdr_format = LC_FORMAT_RGBA16_FLOAT;
        out_info->hdr_width = renderer->hdr_width;
        out_info->hdr_height = renderer->hdr_height;
    }
    out_info->environment_rebuilds = renderer->environment_rebuilds;
    if (renderer->active_env != NULL) {
        const lr_environment *env = renderer->active_env;

        out_info->active = 1;
        out_info->intensity = env->intensity;
        out_info->rotation = env->rotation;
        out_info->preprocess_state = env->ready ? 1u : 0u;
        out_info->environment_generation = env->source_epoch;
        out_info->last_build_ms = env->last_build_ms;
    }
}

void lr_renderer_end(lr_renderer *renderer) {
    if (renderer == NULL) {
        return;
    }
    renderer->profile.cpu_total_ms = lr_perf_to_ms(
        lr_perf_now() - renderer->t_begin, renderer->perf_freq);
    renderer->frame_open = 0;
    renderer->queued = 0;
}

void lr_renderer_get_stats(const lr_renderer *renderer,
                           lr_render_stats *out_stats) {
    if (out_stats == NULL) {
        return;
    }
    if (renderer == NULL) {
        memset(out_stats, 0, sizeof(*out_stats));
        return;
    }
    *out_stats = renderer->stats;
}

void lr_renderer_get_frame_profile(const lr_renderer *renderer,
                                   lr_frame_profile *out_profile) {
    if (out_profile == NULL) {
        return;
    }
    if (renderer == NULL) {
        memset(out_profile, 0, sizeof(*out_profile));
        return;
    }
    *out_profile = renderer->profile;
}

void lr_renderer_get_frame_diagnostics(const lr_renderer *renderer,
                                       lr_frame_diagnostics *out_diag) {
    lc_pipeline_cache_info cache;

    if (out_diag == NULL) {
        return;
    }
    memset(out_diag, 0, sizeof(*out_diag));
    if (renderer == NULL) {
        return;
    }
    memset(&cache, 0, sizeof(cache));
    lc_device_get_pipeline_cache_info(renderer->device, &cache);
    out_diag->frame_number = renderer->frame_number;
    out_diag->viewport_width = renderer->hdr_width;
    out_diag->viewport_height = renderer->hdr_height;
    out_diag->draw_calls = renderer->stats.draw_calls;
    out_diag->triangles = renderer->stats.triangles;
    out_diag->shadow_passes = renderer->stats.shadow_passes;
    out_diag->ibl_active = renderer->stats.ibl_enabled;
    out_diag->post_passes = renderer->profile.post_passes;
    out_diag->cpu_total_ms = renderer->profile.cpu_total_ms;
    out_diag->pipeline_cache_enabled = cache.enabled;
    out_diag->pipeline_cache_loaded = cache.loaded_from_file;
    out_diag->pipeline_cache_bytes_loaded = cache.bytes_loaded;
}

lr_result lr_renderer_set_post_stage(lr_renderer *renderer,
                                     lr_post_stage stage) {
    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    return lr_post_set_stage(renderer, stage);
}

lr_post_stage lr_renderer_get_post_stage(const lr_renderer *renderer) {
    if (renderer == NULL) {
        return LR_POST_NONE;
    }
    return renderer->post_stage;
}

lr_result lr_renderer_set_post_tint(lr_renderer *renderer,
                                    const float rgb[3]) {
    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    return lr_post_set_tint(renderer, rgb);
}

lr_result lr_renderer_capture_hdr(lr_renderer *renderer, void *dst,
                                  size_t dst_size,
                                  size_t *out_required_size) {
    lc_image *image = NULL;
    lc_image_readback_desc desc;

    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->hdr_view == NULL || !renderer->hdr_has_scene) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Public route only: borrowed view -> image -> readback. */
    image = lc_image_view_get_image(renderer->hdr_view);
    if (image == NULL) {
        return LR_ERROR_RENDER;
    }
    memset(&desc, 0, sizeof(desc));
    desc.mip_level = 0;
    desc.array_layer = 0;
    return lr_map_result(
        lc_image_readback(image, &desc, dst, dst_size,
                          out_required_size));
}

lr_result lr_renderer_capture_output(lr_renderer *renderer,
                                     lc_render_target *target, void *dst,
                                     size_t dst_size,
                                     size_t *out_required_size) {
    lc_image_view *view = NULL;
    lc_image *image = NULL;
    lc_image_readback_desc desc;

    if (renderer == NULL || target == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Public route only: borrowed view -> image -> readback. */
    view = lc_render_target_get_color_view(target, 0);
    if (view == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    image = lc_image_view_get_image(view);
    if (image == NULL) {
        return LR_ERROR_RENDER;
    }
    memset(&desc, 0, sizeof(desc));
    desc.mip_level = 0;
    desc.array_layer = 0;
    return lr_map_result(
        lc_image_readback(image, &desc, dst, dst_size,
                          out_required_size));
}

lc_device *lr_renderer_get_device(const lr_renderer *renderer) {
    if (renderer == NULL) {
        return NULL;
    }
    return renderer->device;
}
