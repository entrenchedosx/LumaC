/*
 * Renderer GPU skinning (Phase 29): renderer-owned per-frame skin
 * arena, one GPU storage buffer + one descriptor set, and skinned
 * pipeline variants. Public LumaC only — no backend, platform, or
 * internal headers beyond lumac.h (same audit discipline as every
 * other renderer source).
 *
 * Frame flow: lr_renderer_submit copies each skinned item's palette
 * into the CPU arena (lr_skin_arena_push, synchronous borrow — the
 * caller pointer is never retained). Before draws, lr_skin_upload
 * _frame writes the consumed arena prefix into the GPU buffer once
 * and (re)writes the ONE skin set when the buffer grew. Draws bind
 * that same set for every skinned draw; per-draw offsets ride the
 * push block (no per-draw set rebuild, no invalidation).
 *
 * Buffer states mirror the shadow-metadata discipline: the GPU
 * mirror is host-visible (CPU_TO_GPU, persistently mapped like
 * light_buffer) written via lc_buffer_write, so it stays
 * UNDEFINED/TRANSFER_DST-lenient at set-update time — no
 * transitions are recorded, and nothing runs inside passes except
 * binds/draws. Growth destroys + recreates the buffer and rewrites
 * the set OUTSIDE any pass (upload path), so no in-flight bind can
 * dangle: growth only happens at submit/upload time, while draws
 * only bind.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

/* Embedded skinned SPIR-V (generated at build time from staged
 * .spv, same hex_to_c discipline as every other shader). */
extern const unsigned char lr_pbr_skinned_vert_spv[];
extern const unsigned long lr_pbr_skinned_vert_spv_size;
extern const unsigned char lr_unlit_skinned_vert_spv[];
extern const unsigned long lr_unlit_skinned_vert_spv_size;
extern const unsigned char lr_shadow_skinned_vert_spv[];
extern const unsigned long lr_shadow_skinned_vert_spv_size;

/* Maximum joints per submit (matches the engine skeleton ceiling
 * LE_ANIM_MAX_JOINTS; palettes are paged per draw, never
 * concatenated beyond one draw's window + arena capacity). */
#define LR_SKIN_MAX_JOINTS 4096u
/* Geometric growth quantum (joints). */
#define LR_SKIN_GROW_JOINTS 64u

/* Rigid convention: joints {0,0,0,0} + weights {1,0,0,0}.
 * Exact float compare is fine (importers write exact 1.0f/0.0f;
 * authoring tools quantize to the same convention). */
int lr_skin_scan_vertices(const lr_vertex *vertices,
                           uint32_t vertex_count) {
    uint32_t i;

    if (vertices == NULL || vertex_count == 0) {
        return 0;
    }
    for (i = 0; i < vertex_count; i++) {
        const lr_vertex *v = &vertices[i];

        if (v->joints[0] != 0u || v->joints[1] != 0u ||
            v->joints[2] != 0u || v->joints[3] != 0u ||
            v->weights[0] != 1.0f || v->weights[1] != 0.0f ||
            v->weights[2] != 0.0f || v->weights[3] != 0.0f) {
            return 1;
        }
    }
    return 0;
}

int lr_mesh_is_skinned(const lr_mesh *mesh) {
    if (mesh == NULL || mesh->renderer == NULL) {
        return 0;
    }
    if (!lr_mesh_is_live(mesh->renderer, mesh)) {
        return 0;
    }
    return mesh->skinned ? 1 : 0;
}

lr_result lr_skin_arena_push(lr_renderer *renderer,
                             const float (*palette)[16],
                             uint32_t joint_count,
                             uint32_t *out_offset) {
    uint32_t need;
    uint32_t grown;
    float *arena;

    if (renderer == NULL || out_offset == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Bounded: reject degenerate or oversized palettes at submit
     * (callers treat these as rigid — see draw_list.c). */
    if (palette == NULL || joint_count == 0 ||
        joint_count > LR_SKIN_MAX_JOINTS) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Arena capacity is joints; guard the addition against
     * wraparound (both operands are bounded: used <= cap and
     * joint_count <= 4096, but checked arithmetic stays cheap). */
    if (renderer->skin_cpu_used > UINT32_MAX - joint_count) {
        return LR_ERROR_OUT_OF_MEMORY;
    }
    need = renderer->skin_cpu_used + joint_count;
    if (need > renderer->skin_cpu_cap) {
        grown = (renderer->skin_cpu_cap == 0) ? LR_SKIN_GROW_JOINTS
                                              : renderer->skin_cpu_cap;
        while (grown < need) {
            if (grown > UINT32_MAX - LR_SKIN_GROW_JOINTS) {
                return LR_ERROR_OUT_OF_MEMORY;
            }
            grown += LR_SKIN_GROW_JOINTS;
        }
        if (grown > SIZE_MAX / (16u * sizeof(float))) {
            return LR_ERROR_OUT_OF_MEMORY;
        }
        arena = (float *)realloc(renderer->skin_cpu,
                                 (size_t)grown * 16u * sizeof(float));
        if (arena == NULL) {
            return LR_ERROR_OUT_OF_MEMORY;
        }
        renderer->skin_cpu = arena;
        renderer->skin_cpu_cap = grown;
    }
    memcpy(renderer->skin_cpu + (size_t)renderer->skin_cpu_used * 16u,
           palette, (size_t)joint_count * 16u * sizeof(float));
    *out_offset = renderer->skin_cpu_used;
    renderer->skin_cpu_used = need;
    return LR_SUCCESS;
}

/* One storage-buffer slot layout (binding 0, vertex-visible). */
static lr_result lr_skin_make_layout(lr_renderer *renderer,
                                     lc_binding_layout **out_layout) {
    lc_binding_desc slot;
    lc_binding_layout_desc ldesc;

    memset(&slot, 0, sizeof(slot));
    slot.binding = 0;
    slot.type = LC_BINDING_STORAGE_BUFFER;
    slot.count = 1;
    slot.visibility = (uint32_t)LC_SHADER_VISIBILITY_VERTEX;
    memset(&ldesc, 0, sizeof(ldesc));
    ldesc.bindings = &slot;
    ldesc.binding_count = 1;
    if (lc_binding_layout_create(renderer->device, &ldesc,
                                 out_layout) != LC_SUCCESS) {
        *out_layout = NULL;
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

static lr_result lr_skin_make_shader(lr_renderer *renderer,
                                     const unsigned char *code,
                                     unsigned long size,
                                     lc_shader **out_shader) {
    lc_shader_desc sdesc;

    memset(&sdesc, 0, sizeof(sdesc));
    sdesc.stage = LC_SHADER_STAGE_VERTEX;
    sdesc.code = code;
    sdesc.code_size = (size_t)size;
    sdesc.entry_point = "main";
    if (lc_shader_create(renderer->device, &sdesc, out_shader) !=
        LC_SUCCESS) {
        *out_shader = NULL;
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

lr_result lr_skin_ensure_shared(lr_renderer *renderer) {
    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->skin_ready) {
        return LR_SUCCESS;
    }
    if (lr_skin_make_layout(renderer, &renderer->skin_pbr_layout) !=
        LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lr_skin_make_layout(renderer, &renderer->skin_unlit_layout) !=
        LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lr_skin_make_layout(renderer, &renderer->skin_depth_layout) !=
        LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lr_skin_make_shader(renderer, lr_pbr_skinned_vert_spv,
                            lr_pbr_skinned_vert_spv_size,
                            &renderer->skin_pbr_vertex_shader) !=
        LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lr_skin_make_shader(renderer, lr_unlit_skinned_vert_spv,
                            lr_unlit_skinned_vert_spv_size,
                            &renderer->skin_unlit_vertex_shader) !=
        LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lr_skin_make_shader(renderer, lr_shadow_skinned_vert_spv,
                            lr_shadow_skinned_vert_spv_size,
                            &renderer->skin_depth_vertex_shader) !=
        LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    renderer->skin_ready = 1;
    return LR_SUCCESS;
}

void lr_skin_destroy(lr_renderer *renderer) {
    uint32_t i;

    if (renderer == NULL) {
        return;
    }
    for (i = 0; i < renderer->skin_pbr_pipeline_count; i++) {
        lc_pipeline_destroy(renderer->skin_pbr_pipelines[i].pipeline);
        renderer->skin_pbr_pipelines[i].pipeline = NULL;
    }
    renderer->skin_pbr_pipeline_count = 0;
    for (i = 0; i < renderer->skin_unlit_pipeline_count; i++) {
        lc_pipeline_destroy(
            renderer->skin_unlit_pipelines[i].pipeline);
        renderer->skin_unlit_pipelines[i].pipeline = NULL;
    }
    renderer->skin_unlit_pipeline_count = 0;
    for (i = 0; i < renderer->skin_depth_pipeline_count; i++) {
        lc_pipeline_destroy(
            renderer->skin_depth_pipelines[i].pipeline);
        renderer->skin_depth_pipelines[i].pipeline = NULL;
    }
    renderer->skin_depth_pipeline_count = 0;
    lc_binding_set_destroy(renderer->skin_set);
    renderer->skin_set = NULL;
    renderer->skin_set_cap = 0;
    lc_binding_layout_destroy(renderer->skin_depth_layout);
    renderer->skin_depth_layout = NULL;
    lc_binding_layout_destroy(renderer->skin_unlit_layout);
    renderer->skin_unlit_layout = NULL;
    lc_binding_layout_destroy(renderer->skin_pbr_layout);
    renderer->skin_pbr_layout = NULL;
    lc_shader_destroy(renderer->skin_depth_vertex_shader);
    renderer->skin_depth_vertex_shader = NULL;
    lc_shader_destroy(renderer->skin_unlit_vertex_shader);
    renderer->skin_unlit_vertex_shader = NULL;
    lc_shader_destroy(renderer->skin_pbr_vertex_shader);
    renderer->skin_pbr_vertex_shader = NULL;
    lc_buffer_destroy(renderer->skin_buffer);
    renderer->skin_buffer = NULL;
    renderer->skin_mapped = NULL;
    renderer->skin_buffer_cap = 0;
    free(renderer->skin_cpu);
    renderer->skin_cpu = NULL;
    renderer->skin_cpu_used = 0;
    renderer->skin_cpu_cap = 0;
    renderer->skin_ready = 0;
    renderer->skin_uploaded_frame = 0;
}

/* Grow the GPU mirror to at least `need` joints (geometric, same
 * quantum as the CPU arena). Destroys + recreates the buffer and
 * drops the set so the upload path rewrites it outside any pass.
 * Retirement-safe: destruction is logical + deferred, and growth
 * only happens pre-draw (never while a recording binds the set). */
static lr_result lr_skin_grow_buffer(lr_renderer *renderer,
                                     uint32_t need) {
    uint32_t grown;
    lc_buffer_desc bdesc;
    lc_buffer *buffer = NULL;

    grown = (renderer->skin_buffer_cap == 0) ? LR_SKIN_GROW_JOINTS
                                             : renderer->skin_buffer_cap;
    while (grown < need) {
        if (grown > UINT32_MAX - LR_SKIN_GROW_JOINTS) {
            return LR_ERROR_RENDER;
        }
        grown += LR_SKIN_GROW_JOINTS;
    }
    if (grown > (uint64_t)SIZE_MAX / (16u * sizeof(float))) {
        return LR_ERROR_RENDER;
    }
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = (uint64_t)grown * 16u * sizeof(float);
    bdesc.usage = LC_BUFFER_USAGE_STORAGE;
    bdesc.memory = LC_MEMORY_CPU_TO_GPU;
    if (lc_buffer_create(renderer->device, &bdesc, &buffer) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Destroy-before-replace: the old buffer retires through the
     * deferred path (no global idle); nothing in flight can fault
     * because growth precedes every bind of the new set. */
    lc_buffer_destroy(renderer->skin_buffer);
    renderer->skin_buffer = buffer;
    renderer->skin_mapped = NULL;
    (void)lc_buffer_map(renderer->skin_buffer, &renderer->skin_mapped);
    renderer->skin_buffer_cap = grown;
    /* The set names the buffer object: drop it so the write below
     * recreates it against the new buffer. */
    lc_binding_set_destroy(renderer->skin_set);
    renderer->skin_set = NULL;
    renderer->skin_set_cap = 0;
    return LR_SUCCESS;
}

lr_result lr_skin_upload_frame(lr_renderer *renderer) {
    lc_binding_write write;

    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->skin_cpu_used == 0) {
        return LR_SUCCESS;
    }
    if (renderer->skin_uploaded_frame == renderer->frame_number) {
        return LR_SUCCESS;
    }
    if (lr_skin_ensure_shared(renderer) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (renderer->skin_buffer == NULL ||
        renderer->skin_buffer_cap < renderer->skin_cpu_used) {
        if (lr_skin_grow_buffer(renderer,
                                renderer->skin_cpu_used) != LR_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    /* One upload of the consumed prefix (host-visible mirror;
     * lc_buffer_write memcpys + flushes when non-coherent). */
    if (lc_buffer_write(
            renderer->skin_buffer, 0, renderer->skin_cpu,
            (uint64_t)renderer->skin_cpu_used * 16u *
                sizeof(float)) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* (Re)write the ONE set when the buffer changed since the last
     * write. Same buffer + same set across frames: no update, no
     * invalidation — binds stay valid. */
    if (renderer->skin_set == NULL ||
        renderer->skin_set_cap != renderer->skin_buffer_cap) {
        if (renderer->skin_set == NULL) {
            if (lc_binding_set_create(renderer->skin_pbr_layout,
                                      &renderer->skin_set) !=
                LC_SUCCESS) {
                return LR_ERROR_RENDER;
            }
        }
        memset(&write, 0, sizeof(write));
        write.binding = 0;
        write.array_element = 0;
        write.type = LC_BINDING_STORAGE_BUFFER;
        write.u.buffer.buffer = renderer->skin_buffer;
        write.u.buffer.offset = 0;
        write.u.buffer.size = 0;
        /* All three skin layouts are structurally identical
         * (binding 0 storage, vertex-visible), so one set object
         * satisfies every skinned pipeline's skin slot: binds
         * validate by structural signature, not layout identity. */
        if (lc_binding_set_update(renderer->skin_set, &write, 1) !=
            LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        renderer->skin_set_cap = renderer->skin_buffer_cap;
    }
    renderer->skin_uploaded_frame = renderer->frame_number;
    return LR_SUCCESS;
}

/* Structural signature match (extent-independent, mirrors the
 * LumaC rule + the lr_renderer_pipeline_for key discipline). */
static int lr_skin_signature_equal(const lc_render_target_desc *a,
                                   const lc_render_target_desc *b) {
    uint32_t k;

    if (a->color_attachment_count != b->color_attachment_count ||
        a->depth_stencil_format != b->depth_stencil_format ||
        a->samples != b->samples) {
        return 0;
    }
    for (k = 0; k < a->color_attachment_count; k++) {
        if (a->color_formats[k] != b->color_formats[k]) {
            return 0;
        }
    }
    return 1;
}

/* Shared skinned vertex-attribute table: rigid locations 0-3 plus
 * joints (uvec4, loc 4) + weights (vec4, loc 5) over the IDENTICAL
 * lr_vertex stride. Offsets: pos 0, normal 12, tangent 24,
 * uv 40, joints 48, weights 64 (matches the 88-byte struct). */
static void lr_skin_fill_vattrs(lc_vertex_attribute_desc *vattrs,
                                int pbr) {
    vattrs[0].location = 0;
    vattrs[0].binding = 0;
    vattrs[0].format = LC_FORMAT_RGB32_FLOAT;
    vattrs[0].offset = 0;
    if (pbr) {
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
        vattrs[4].location = 4;
        vattrs[4].binding = 0;
        vattrs[4].format = LC_FORMAT_RGBA32_UINT;
        vattrs[4].offset = 48u;
        vattrs[5].location = 5;
        vattrs[5].binding = 0;
        vattrs[5].format = LC_FORMAT_RGBA32_FLOAT;
        vattrs[5].offset = 64u;
    } else {
        /* Unlit consumes position + UV only (same Stage-83 rule
         * as the rigid unlit pipeline: never declare unconsumed
         * attributes) plus the skin pair. */
        vattrs[1].location = 3;
        vattrs[1].binding = 0;
        vattrs[1].format = LC_FORMAT_RG32_FLOAT;
        vattrs[1].offset = sizeof(float) * 10u;
        vattrs[2].location = 4;
        vattrs[2].binding = 0;
        vattrs[2].format = LC_FORMAT_RGBA32_UINT;
        vattrs[2].offset = 48u;
        vattrs[3].location = 5;
        vattrs[3].binding = 0;
        vattrs[3].format = LC_FORMAT_RGBA32_FLOAT;
        vattrs[3].offset = 64u;
    }
}

/* Skinned PBR: same 3 rigid slots + skin storage at slot 3. */
lr_result lr_renderer_skinned_pbr_pipeline_for(
    lr_renderer *renderer, const lc_render_target_desc *signature,
    lc_cull_mode cull_mode, lc_front_face front_face,
    lc_pipeline **out_pipeline) {
    uint32_t i;
    lc_graphics_pipeline_desc pd;
    lc_vertex_binding_desc vbinding;
    lc_vertex_attribute_desc vattrs[6];
    lc_push_constant_range push;
    lc_pipeline *pipeline = NULL;

    if (renderer == NULL || signature == NULL ||
        out_pipeline == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (cull_mode == LC_CULL_NONE) {
        front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    for (i = 0; i < renderer->skin_pbr_pipeline_count; i++) {
        if (renderer->skin_pbr_pipelines[i].material_type ==
                LR_MATERIAL_PBR_METALLIC_ROUGHNESS &&
            renderer->skin_pbr_pipelines[i].cull_mode == cull_mode &&
            renderer->skin_pbr_pipelines[i].front_face == front_face &&
            lr_skin_signature_equal(
                &renderer->skin_pbr_pipelines[i].signature,
                signature)) {
            *out_pipeline = renderer->skin_pbr_pipelines[i].pipeline;
            return LR_SUCCESS;
        }
    }
    if (renderer->skin_pbr_pipeline_count >= LR_PIPELINE_CACHE_MAX) {
        return LR_ERROR_UNSUPPORTED;
    }
    if (lr_skin_ensure_shared(renderer) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    vbinding.binding = 0;
    vbinding.stride = sizeof(lr_vertex);
    vbinding.input_rate = LC_VERTEX_INPUT_PER_VERTEX;
    lr_skin_fill_vattrs(vattrs, 1);
    push.visibility = (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
                      (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT;
    push.offset = 0;
    push.size = sizeof(lr_pbr_push);
    memset(&pd, 0, sizeof(pd));
    pd.vertex_shader = renderer->skin_pbr_vertex_shader;
    pd.fragment_shader = renderer->pbr_fragment_shader;
    pd.vertex_bindings = &vbinding;
    pd.vertex_binding_count = 1;
    pd.vertex_attributes = vattrs;
    pd.vertex_attribute_count = 6u;
    {
        const lc_binding_layout *slots[4];

        slots[0] = renderer->pbr_layout;
        slots[1] = renderer->shadow_layout;
        slots[2] = renderer->env_layout;
        slots[3] = renderer->skin_pbr_layout;
        pd.binding_layouts = slots;
        pd.binding_layout_count = 4;
        pd.cull_mode = cull_mode;
        pd.front_face = front_face;
        pd.depth_test_enable =
            (signature->depth_stencil_format == LC_FORMAT_UNDEFINED)
                ? 0
                : 1;
        pd.depth_write_enable = pd.depth_test_enable;
        pd.push_constant_ranges = &push;
        pd.push_constant_range_count = 1;
        pd.render_target = *signature;
        if (lc_graphics_pipeline_create(renderer->device, &pd,
                                        &pipeline) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    renderer->skin_pbr_pipelines[renderer->skin_pbr_pipeline_count]
        .signature = *signature;
    renderer->skin_pbr_pipelines[renderer->skin_pbr_pipeline_count]
        .material_type = LR_MATERIAL_PBR_METALLIC_ROUGHNESS;
    renderer->skin_pbr_pipelines[renderer->skin_pbr_pipeline_count]
        .cull_mode = cull_mode;
    renderer->skin_pbr_pipelines[renderer->skin_pbr_pipeline_count]
        .front_face = front_face;
    renderer->skin_pbr_pipelines[renderer->skin_pbr_pipeline_count]
        .pipeline = pipeline;
    renderer->skin_pbr_pipeline_count++;
    *out_pipeline = pipeline;
    return LR_SUCCESS;
}

/* Skinned unlit: rigid unlit slot + skin storage at slot 1. */
lr_result lr_renderer_skinned_unlit_pipeline_for(
    lr_renderer *renderer, const lc_render_target_desc *signature,
    lc_cull_mode cull_mode, lc_front_face front_face,
    lc_pipeline **out_pipeline) {
    uint32_t i;
    lc_graphics_pipeline_desc pd;
    lc_vertex_binding_desc vbinding;
    lc_vertex_attribute_desc vattrs[6];
    lc_push_constant_range push;
    lc_pipeline *pipeline = NULL;

    if (renderer == NULL || signature == NULL ||
        out_pipeline == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (cull_mode == LC_CULL_NONE) {
        front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    for (i = 0; i < renderer->skin_unlit_pipeline_count; i++) {
        if (renderer->skin_unlit_pipelines[i].material_type ==
                LR_MATERIAL_UNLIT &&
            renderer->skin_unlit_pipelines[i].cull_mode == cull_mode &&
            renderer->skin_unlit_pipelines[i].front_face ==
                front_face &&
            lr_skin_signature_equal(
                &renderer->skin_unlit_pipelines[i].signature,
                signature)) {
            *out_pipeline =
                renderer->skin_unlit_pipelines[i].pipeline;
            return LR_SUCCESS;
        }
    }
    if (renderer->skin_unlit_pipeline_count >=
        LR_PIPELINE_CACHE_MAX) {
        return LR_ERROR_UNSUPPORTED;
    }
    if (lr_skin_ensure_shared(renderer) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    vbinding.binding = 0;
    vbinding.stride = sizeof(lr_vertex);
    vbinding.input_rate = LC_VERTEX_INPUT_PER_VERTEX;
    lr_skin_fill_vattrs(vattrs, 0);
    push.visibility = (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
                      (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT;
    push.offset = 0;
    push.size = sizeof(lr_unlit_push);
    memset(&pd, 0, sizeof(pd));
    pd.vertex_shader = renderer->skin_unlit_vertex_shader;
    pd.fragment_shader = renderer->fragment_shader;
    pd.vertex_bindings = &vbinding;
    pd.vertex_binding_count = 1;
    pd.vertex_attributes = vattrs;
    pd.vertex_attribute_count = 4u;
    {
        const lc_binding_layout *slots[2];

        slots[0] = renderer->layout;
        slots[1] = renderer->skin_unlit_layout;
        pd.binding_layouts = slots;
        pd.binding_layout_count = 2;
        pd.cull_mode = cull_mode;
        pd.front_face = front_face;
        pd.depth_test_enable =
            (signature->depth_stencil_format == LC_FORMAT_UNDEFINED)
                ? 0
                : 1;
        pd.depth_write_enable = pd.depth_test_enable;
        pd.push_constant_ranges = &push;
        pd.push_constant_range_count = 1;
        pd.render_target = *signature;
        if (lc_graphics_pipeline_create(renderer->device, &pd,
                                        &pipeline) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    renderer->skin_unlit_pipelines[renderer->skin_unlit_pipeline_count]
        .signature = *signature;
    renderer->skin_unlit_pipelines[renderer->skin_unlit_pipeline_count]
        .material_type = LR_MATERIAL_UNLIT;
    renderer->skin_unlit_pipelines[renderer->skin_unlit_pipeline_count]
        .cull_mode = cull_mode;
    renderer->skin_unlit_pipelines[renderer->skin_unlit_pipeline_count]
        .front_face = front_face;
    renderer->skin_unlit_pipelines[renderer->skin_unlit_pipeline_count]
        .pipeline = pipeline;
    renderer->skin_unlit_pipeline_count++;
    *out_pipeline = pipeline;
    return LR_SUCCESS;
}

/* Skinned depth: VP uniform at slot 0 + skin storage at slot 1,
 * position + joints + weights attributes, skin push window. Cull
 * NONE like the rigid depth pipeline (one documented rule). */
lr_result lr_renderer_skinned_depth_pipeline_for(
    lr_renderer *renderer, const lc_render_target_desc *signature,
    lc_pipeline **out_pipeline) {
    uint32_t i;
    lc_graphics_pipeline_desc pd;
    lc_vertex_binding_desc vbinding;
    lc_vertex_attribute_desc vattrs[3];
    lc_push_constant_range push;
    lc_pipeline *pipeline = NULL;

    if (renderer == NULL || signature == NULL ||
        out_pipeline == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < renderer->skin_depth_pipeline_count; i++) {
        if (lr_skin_signature_equal(
                &renderer->skin_depth_pipelines[i].signature,
                signature)) {
            *out_pipeline =
                renderer->skin_depth_pipelines[i].pipeline;
            return LR_SUCCESS;
        }
    }
    if (renderer->skin_depth_pipeline_count >= 4) {
        return LR_ERROR_UNSUPPORTED;
    }
    if (lr_skin_ensure_shared(renderer) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    vbinding.binding = 0;
    vbinding.stride = sizeof(lr_vertex);
    vbinding.input_rate = LC_VERTEX_INPUT_PER_VERTEX;
    vattrs[0].location = 0;
    vattrs[0].binding = 0;
    vattrs[0].format = LC_FORMAT_RGB32_FLOAT;
    vattrs[0].offset = 0;
    vattrs[1].location = 4;
    vattrs[1].binding = 0;
    vattrs[1].format = LC_FORMAT_RGBA32_UINT;
    vattrs[1].offset = 48u;
    vattrs[2].location = 5;
    vattrs[2].binding = 0;
    vattrs[2].format = LC_FORMAT_RGBA32_FLOAT;
    vattrs[2].offset = 64u;
    push.visibility = (uint32_t)LC_SHADER_VISIBILITY_VERTEX;
    push.offset = 0;
    push.size = sizeof(lr_shadow_push);
    memset(&pd, 0, sizeof(pd));
    pd.vertex_shader = renderer->skin_depth_vertex_shader;
    pd.fragment_shader = NULL;
    pd.vertex_bindings = &vbinding;
    pd.vertex_binding_count = 1;
    pd.vertex_attributes = vattrs;
    pd.vertex_attribute_count = 3;
    {
        const lc_binding_layout *slots[2];

        slots[0] = renderer->depth_layout;
        slots[1] = renderer->skin_depth_layout;
        pd.binding_layouts = slots;
        pd.binding_layout_count = 2;
        pd.cull_mode = LC_CULL_NONE;
        pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
        pd.depth_test_enable =
            (signature->depth_stencil_format == LC_FORMAT_UNDEFINED)
                ? 0
                : 1;
        pd.depth_write_enable = pd.depth_test_enable;
        pd.push_constant_ranges = &push;
        pd.push_constant_range_count = 1;
        pd.render_target = *signature;
        if (lc_graphics_pipeline_create(renderer->device, &pd,
                                        &pipeline) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    renderer->skin_depth_pipelines[renderer->skin_depth_pipeline_count]
        .signature = *signature;
    renderer->skin_depth_pipelines[renderer->skin_depth_pipeline_count]
        .material_type = LR_MATERIAL_UNKNOWN;
    renderer->skin_depth_pipelines[renderer->skin_depth_pipeline_count]
        .cull_mode = LC_CULL_NONE;
    renderer->skin_depth_pipelines[renderer->skin_depth_pipeline_count]
        .pipeline = pipeline;
    renderer->skin_depth_pipeline_count++;
    *out_pipeline = pipeline;
    return LR_SUCCESS;
}
