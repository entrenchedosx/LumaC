/*
 * Renderer GPU-driven submission (Phase 21, PARTs AF-AJ, U-AB, AD-AE).
 *
 * CPU mode stays the default: one draw per item with push-constant
 * transforms. GPU mode groups queued PBR items by (mesh, material,
 * shadow-flag), uploads shared instance buffers with Phase-20 async
 * uploads, runs frustum-culling + indirect-finalize compute
 * dispatches outside any pass, then renders each group with one
 * indexed indirect draw through the PBR-compatible instanced
 * pipeline. Shadows keep the CPU path in both modes; unlit items
 * keep per-draw CPU submission in both modes.
 *
 * Frame safety: visible/counter/indirect buffers plus their three
 * descriptor sets rotate per flight slot (PART AF31); the instance
 * buffer is shared (uploaded before use each frame it changes).
 * No per-object buffers, sets, or command buffers (PART AF33).
 * Descriptors are created once per group/flight, never per frame.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

extern const unsigned char lr_cull_comp_spv[];
extern const unsigned long lr_cull_comp_spv_size;
extern const unsigned char lr_finalize_comp_spv[];
extern const unsigned long lr_finalize_comp_spv_size;
extern const unsigned char lr_pbr_instanced_vert_spv[];
extern const unsigned long lr_pbr_instanced_vert_spv_size;

_Static_assert(sizeof(lr_gpu_instance) == 96,
               "lr_gpu_instance must match the 96-byte shader layout");
_Static_assert(sizeof(((lr_gpu_instance *)0)->model) == 64,
               "model must be a 4x4 float matrix");
_Static_assert(sizeof(((lr_gpu_instance *)0)->bounds) == 16,
               "bounds must be center.xyz + radius");
_Static_assert(sizeof(((lr_gpu_instance *)0)->stable_id) == 8,
               "stable_id must be 64-bit");
_Static_assert(offsetof(lr_gpu_instance, stable_id) == 88,
               "stable_id must sit at offset 88 (std430 u64)");

/* Cull push block: 6 planes + count = 100 bytes (<= 128 minimum). */
typedef struct lr_cull_push {
    float planes[6][4];
    uint32_t instance_count;
} lr_cull_push;

/* Finalize push block: 24 bytes. vertexOffset rides as uint bits. */
typedef struct lr_finalize_push {
    uint32_t vertex_count;
    uint32_t first_vertex;
    uint32_t index_count;
    uint32_t first_index;
    uint32_t vertex_offset_bits;
    uint32_t indexed;
} lr_finalize_push;

static uint32_t lr_ceil_div(uint32_t n, uint32_t d) {
    return (n + d - 1u) / d;
}

static lc_buffer *lr_gpu_make_buffer(lr_renderer *renderer, uint64_t size,
                                     uint32_t usage) {
    lc_buffer_desc desc;
    lc_buffer *buffer = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.size = size;
    desc.usage = usage;
    desc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(renderer->device, &desc, &buffer) !=
        LC_SUCCESS) {
        return NULL;
    }
    return buffer;
}

static lc_binding_layout *lr_gpu_make_layout(
    lr_renderer *renderer, const lc_binding_desc *binds,
    uint32_t count) {
    lc_binding_layout_desc desc;
    lc_binding_layout *layout = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.bindings = binds;
    desc.binding_count = count;
    if (lc_binding_layout_create(renderer->device, &desc, &layout) !=
        LC_SUCCESS) {
        return NULL;
    }
    return layout;
}

static lc_shader *lr_gpu_make_shader(lr_renderer *renderer,
                                     const unsigned char *code,
                                     unsigned long size) {
    lc_shader_desc desc;
    lc_shader *shader = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.stage = LC_SHADER_STAGE_COMPUTE;
    desc.code = code;
    desc.code_size = (size_t)size;
    desc.entry_point = "main";
    if (lc_shader_create(renderer->device, &desc, &shader) !=
        LC_SUCCESS) {
        return NULL;
    }
    return shader;
}

static lc_binding_set *lr_gpu_make_set(lr_renderer *renderer,
                                       lc_binding_layout *layout,
                                       lc_buffer **buffers,
                                       uint32_t count) {
    lc_binding_set *set = NULL;
    lc_binding_write *writes = NULL;
    uint32_t i;

    if (lc_binding_set_create(layout, &set) != LC_SUCCESS) {
        return NULL;
    }
    writes =
        (lc_binding_write *)calloc(count, sizeof(lc_binding_write));
    if (writes == NULL) {
        lc_binding_set_destroy(set);
        return NULL;
    }
    for (i = 0; i < count; i++) {
        writes[i].binding = i;
        writes[i].array_element = 0;
        writes[i].type = LC_BINDING_STORAGE_BUFFER;
        writes[i].u.buffer.buffer = buffers[i];
        writes[i].u.buffer.offset = 0;
        writes[i].u.buffer.size = 0;
    }
    if (lc_binding_set_update(set, writes, count) != LC_SUCCESS) {
        free(writes);
        lc_binding_set_destroy(set);
        return NULL;
    }
    free(writes);
    return set;
}

/* Shared pipelines/layouts/shaders (idempotent, renderer-global). */
lr_result lr_gpu_ensure_shared(lr_renderer *renderer) {
    lc_binding_desc binds[3];
    lc_push_constant_range push;
    uint32_t i;

    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->gpu_ready) {
        return LR_SUCCESS;
    }
    renderer->cull_shader = lr_gpu_make_shader(
        renderer, lr_cull_comp_spv, lr_cull_comp_spv_size);
    renderer->finalize_shader = lr_gpu_make_shader(
        renderer, lr_finalize_comp_spv, lr_finalize_comp_spv_size);
    if (renderer->cull_shader == NULL ||
        renderer->finalize_shader == NULL) {
        return LR_ERROR_RENDER;
    }
    /* Instanced vertex shader (graphics stage). */
    {
        lc_shader_desc desc;

        memset(&desc, 0, sizeof(desc));
        desc.stage = LC_SHADER_STAGE_VERTEX;
        desc.code = lr_pbr_instanced_vert_spv;
        desc.code_size = (size_t)lr_pbr_instanced_vert_spv_size;
        desc.entry_point = "main";
        if (lc_shader_create(renderer->device, &desc,
                             &renderer->instanced_vertex_shader) !=
            LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    /* Cull layout: instances + visible + counter (COMPUTE). */
    for (i = 0; i < 3; i++) {
        binds[i].binding = i;
        binds[i].type = LC_BINDING_STORAGE_BUFFER;
        binds[i].count = 1;
        binds[i].visibility = LC_SHADER_VISIBILITY_COMPUTE;
    }
    renderer->cull_layout = lr_gpu_make_layout(renderer, binds, 3);
    /* Finalize layout: counter + indirect (COMPUTE). */
    for (i = 0; i < 2; i++) {
        binds[i].binding = i;
        binds[i].type = LC_BINDING_STORAGE_BUFFER;
        binds[i].count = 1;
        binds[i].visibility = LC_SHADER_VISIBILITY_COMPUTE;
    }
    renderer->finalize_layout = lr_gpu_make_layout(renderer, binds, 2);
    /* Instance layout: instances + visible (VERTEX fetch). */
    for (i = 0; i < 2; i++) {
        binds[i].binding = i;
        binds[i].type = LC_BINDING_STORAGE_BUFFER;
        binds[i].count = 1;
        binds[i].visibility = LC_SHADER_VISIBILITY_VERTEX;
    }
    renderer->instanced_layout = lr_gpu_make_layout(renderer, binds, 2);
    if (renderer->cull_layout == NULL ||
        renderer->finalize_layout == NULL ||
        renderer->instanced_layout == NULL) {
        return LR_ERROR_RENDER;
    }
    /* Cull pipeline (100-byte push). */
    {
        lc_compute_pipeline_desc desc;
        const lc_binding_layout *slots[1];

        memset(&desc, 0, sizeof(desc));
        memset(&push, 0, sizeof(push));
        slots[0] = renderer->cull_layout;
        push.visibility = LC_SHADER_VISIBILITY_COMPUTE;
        push.offset = 0;
        push.size = sizeof(lr_cull_push);
        desc.compute_shader = renderer->cull_shader;
        desc.binding_layouts = slots;
        desc.binding_layout_count = 1;
        desc.push_constant_ranges = &push;
        desc.push_constant_range_count = 1;
        if (lc_compute_pipeline_create(renderer->device, &desc,
                                       &renderer->cull_pipeline) !=
            LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    /* Finalize pipeline (24-byte push). */
    {
        lc_compute_pipeline_desc desc;
        const lc_binding_layout *slots[1];

        memset(&desc, 0, sizeof(desc));
        memset(&push, 0, sizeof(push));
        slots[0] = renderer->finalize_layout;
        push.visibility = LC_SHADER_VISIBILITY_COMPUTE;
        push.offset = 0;
        push.size = sizeof(lr_finalize_push);
        desc.compute_shader = renderer->finalize_shader;
        desc.binding_layouts = slots;
        desc.binding_layout_count = 1;
        desc.push_constant_ranges = &push;
        desc.push_constant_range_count = 1;
        if (lc_compute_pipeline_create(renderer->device, &desc,
                                       &renderer->finalize_pipeline) !=
            LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    renderer->gpu_ready = 1;
    return LR_SUCCESS;
}

void lr_gpu_destroy_shared(lr_renderer *renderer) {
    if (renderer == NULL) {
        return;
    }
    lr_gpu_destroy_groups(renderer);
    lc_compute_pipeline_destroy(renderer->finalize_pipeline);
    lc_compute_pipeline_destroy(renderer->cull_pipeline);
    lc_binding_layout_destroy(renderer->instanced_layout);
    lc_binding_layout_destroy(renderer->finalize_layout);
    lc_binding_layout_destroy(renderer->cull_layout);
    lc_shader_destroy(renderer->instanced_vertex_shader);
    lc_shader_destroy(renderer->finalize_shader);
    lc_shader_destroy(renderer->cull_shader);
    renderer->finalize_pipeline = NULL;
    renderer->cull_pipeline = NULL;
    renderer->instanced_layout = NULL;
    renderer->finalize_layout = NULL;
    renderer->cull_layout = NULL;
    renderer->instanced_vertex_shader = NULL;
    renderer->finalize_shader = NULL;
    renderer->cull_shader = NULL;
    renderer->gpu_ready = 0;
}

/* Instanced-PBR pipeline variant (mini-cache keyed like the main
 * cache; slot 3 carries the instance layout, push matches PBR so
 * the fragment stage runs unmodified). */
lr_result lr_renderer_instanced_pipeline_for(
    lr_renderer *renderer, const lc_render_target_desc *signature,
    lr_material_type material_type, lc_cull_mode cull_mode,
    lc_front_face front_face, lc_pipeline **out_pipeline) {
    uint32_t i;

    if (renderer == NULL || signature == NULL ||
        out_pipeline == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (material_type != LR_MATERIAL_PBR_METALLIC_ROUGHNESS) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (cull_mode == LC_CULL_NONE) {
        front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
    }
    for (i = 0; i < renderer->instanced_pipeline_count; i++) {
        const lc_render_target_desc *cached =
            &renderer->instanced_pipelines[i].signature;
        uint32_t k;
        int equal = 0;

        if (renderer->instanced_pipelines[i].material_type ==
                material_type &&
            renderer->instanced_pipelines[i].cull_mode == cull_mode &&
            renderer->instanced_pipelines[i].front_face ==
                front_face &&
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
            *out_pipeline = renderer->instanced_pipelines[i].pipeline;
            return LR_SUCCESS;
        }
    }
    if (renderer->instanced_pipeline_count >= 4) {
        return LR_ERROR_UNSUPPORTED;
    }
    {
        lc_graphics_pipeline_desc pd;
        lc_vertex_binding_desc vbinding;
        lc_vertex_attribute_desc vattrs[4];
        lc_push_constant_range push;
        const lc_binding_layout *slots[4];
        lc_pipeline *pipeline = NULL;
        lc_result res;

        /* Same vertex layout as PBR (56-byte stride). */
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
        /* Same push shape as PBR (fragment reads drawFlags). */
        push.visibility = (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
                          (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT;
        push.offset = 0;
        push.size = sizeof(lr_pbr_push);
        memset(&pd, 0, sizeof(pd));
        pd.vertex_shader = renderer->instanced_vertex_shader;
        pd.fragment_shader = renderer->pbr_fragment_shader;
        pd.vertex_bindings = &vbinding;
        pd.vertex_binding_count = 1;
        pd.vertex_attributes = vattrs;
        pd.vertex_attribute_count = 4;
        slots[0] = renderer->pbr_layout;
        slots[1] = renderer->shadow_layout;
        slots[2] = renderer->env_layout;
        slots[3] = renderer->instanced_layout;
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
        res = lc_graphics_pipeline_create(renderer->device, &pd,
                                          &pipeline);
        if (res != LC_SUCCESS) {
            return lr_map_result(res);
        }
        renderer->instanced_pipelines[renderer->instanced_pipeline_count]
            .signature = *signature;
        renderer->instanced_pipelines[renderer->instanced_pipeline_count]
            .material_type = material_type;
        renderer->instanced_pipelines[renderer->instanced_pipeline_count]
            .cull_mode = cull_mode;
        renderer->instanced_pipelines[renderer->instanced_pipeline_count]
            .front_face = front_face;
        renderer->instanced_pipelines[renderer->instanced_pipeline_count]
            .pipeline = pipeline;
        renderer->instanced_pipeline_count++;
        *out_pipeline = pipeline;
        return LR_SUCCESS;
    }
}

/* Destroy one flight slot's resources (buffers + three sets).
 * Retirement-safe; keeps the alive counter honest. */
static void lr_gpu_flight_teardown(lr_renderer *renderer,
                                   lr_gpu_group *group, uint32_t flight) {
    lr_gpu_flight_res *fl = &group->flights[flight];

    if (!fl->initialized) {
        return;
    }
    lc_binding_set_destroy(fl->inst_set);
    lc_binding_set_destroy(fl->finalize_set);
    lc_binding_set_destroy(fl->cull_set);
    lc_buffer_destroy(fl->indirect);
    lc_buffer_destroy(fl->counter);
    lc_buffer_destroy(fl->visible);
    fl->inst_set = NULL;
    fl->finalize_set = NULL;
    fl->cull_set = NULL;
    fl->indirect = NULL;
    fl->counter = NULL;
    fl->visible = NULL;
    fl->initialized = 0;
    if (renderer->gpu_stats.descriptor_sets_alive >= 3) {
        renderer->gpu_stats.descriptor_sets_alive -= 3;
    }
}

void lr_gpu_prune_dead_groups(lr_renderer *renderer) {
    uint32_t i = 0;

    if (renderer == NULL) {
        return;
    }
    while (i < renderer->group_count) {
        lr_gpu_group *group = &renderer->groups[i];

        if (lr_mesh_is_live(renderer, group->mesh) &&
            lr_material_is_live(renderer, group->material)) {
            i++;
            continue;
        }
        /* Dead key: destroy GPU resources (retirement-safe) and
         * swap-remove. */
        {
            uint32_t f;

            for (f = 0; f < group->flights_owned; f++) {
                lr_gpu_flight_teardown(renderer, group, f);
            }
            lc_buffer_destroy(group->instances);
            free(group->cpu);
            renderer->group_count--;
            if (i < renderer->group_count) {
                renderer->groups[i] = renderer->groups[renderer->group_count];
            }
        }
    }
}

void lr_gpu_destroy_groups(lr_renderer *renderer) {
    uint32_t i;

    if (renderer == NULL) {
        return;
    }
    for (i = 0; i < renderer->group_count; i++) {
        lr_gpu_group *group = &renderer->groups[i];
        uint32_t f;

        for (f = 0; f < group->flights_owned; f++) {
            lr_gpu_flight_teardown(renderer, group, f);
        }
        lc_buffer_destroy(group->instances);
        free(group->cpu);
    }
    free(renderer->groups);
    renderer->groups = NULL;
    renderer->group_count = 0;
    renderer->group_capacity = 0;
}

/* Find or create the group for (mesh, material, shadow-flag,
 * winding parity). Mirrored items group apart so their draws can
 * flip the raster front face (Stage 40 audit fix). Grows capacity
 * (doubling) with full resource recreation (retirement keeps
 * in-flight frames safe, PART 48). */
static lr_gpu_group *lr_gpu_group_for(lr_renderer *renderer,
                                      lr_mesh *mesh, lr_material *material,
                                      int receives_shadow, int mirrored,
                                      uint32_t need) {
    uint32_t i;

    for (i = 0; i < renderer->group_count; i++) {
        lr_gpu_group *group = &renderer->groups[i];

        if (group->mesh == mesh && group->material == material &&
            group->receives_shadow == receives_shadow &&
            group->mirrored == mirrored) {
            if (group->count + need > group->capacity) {
                uint32_t grown = (group->capacity == 0)
                                     ? 64u
                                     : group->capacity * 2u;
                lr_gpu_instance *kept = NULL;

                while (grown < group->count + need) {
                    grown *= 2u;
                }
                /* Preserve already-filled instances across the
                 * rebuild (growth happens mid-grouping). */
                if (group->count > 0) {
                    kept = (lr_gpu_instance *)malloc(
                        (size_t)group->count * sizeof(lr_gpu_instance));
                    if (kept == NULL) {
                        return NULL;
                    }
                    memcpy(kept, group->cpu,
                           (size_t)group->count *
                               sizeof(lr_gpu_instance));
                }
                /* Full rebuild (sets reference the buffers). */
                {
                    uint32_t f;

                    for (f = 0; f < group->flights_owned; f++) {
                        lr_gpu_flight_teardown(renderer, group, f);
                    }
                    lc_buffer_destroy(group->instances);
                    free(group->cpu);
                    group->instances = NULL;
                    group->cpu = NULL;
                    group->capacity = 0;
                    group->flights_owned = 0;
                }
                group->cpu = (lr_gpu_instance *)calloc(
                    grown, sizeof(lr_gpu_instance));
                if (group->cpu == NULL) {
                    free(kept);
                    return NULL;
                }
                if (kept != NULL) {
                    memcpy(group->cpu, kept,
                           (size_t)group->count *
                               sizeof(lr_gpu_instance));
                    free(kept);
                }
                group->instances = lr_gpu_make_buffer(
                    renderer, (uint64_t)grown * sizeof(lr_gpu_instance),
                    LC_BUFFER_USAGE_STORAGE |
                        LC_BUFFER_USAGE_TRANSFER_DST);
                if (group->instances == NULL) {
                    free(group->cpu);
                    group->cpu = NULL;
                    return NULL;
                }
                group->capacity = grown;
            }
            return group;
        }
    }
    if (renderer->group_count >= LR_GPU_MAX_GROUPS) {
        return NULL;
    }
    if (renderer->group_count == renderer->group_capacity) {
        uint32_t grown = (renderer->group_capacity == 0)
                             ? 8u
                             : renderer->group_capacity * 2u;
        lr_gpu_group *groups = (lr_gpu_group *)realloc(
            renderer->groups, sizeof(lr_gpu_group) * grown);

        if (groups == NULL) {
            return NULL;
        }
        renderer->groups = groups;
        renderer->group_capacity = grown;
    }
    {
        lr_gpu_group *group = &renderer->groups[renderer->group_count];
        uint32_t grown = 64u;

        while (grown < need) {
            grown *= 2u;
        }
        memset(group, 0, sizeof(*group));
        group->mesh = mesh;
        group->material = material;
        group->receives_shadow = receives_shadow;
        group->mirrored = mirrored;
        group->cpu = (lr_gpu_instance *)calloc(grown,
                                              sizeof(lr_gpu_instance));
        if (group->cpu == NULL) {
            return NULL;
        }
        group->instances = lr_gpu_make_buffer(
            renderer, (uint64_t)grown * sizeof(lr_gpu_instance),
            LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST);
        if (group->instances == NULL) {
            free(group->cpu);
            group->cpu = NULL;
            return NULL;
        }
        group->capacity = grown;
        group->count = 0;
        group->flights_owned = 0;
        renderer->group_count++;
        return group;
    }
}

/* Ensure one flight slot's resources (buffers + three sets).
 * Buffers start UNDEFINED so set creation takes the lenient path;
 * uploads each frame drive the tracked states after. */
static int lr_gpu_flight_ensure(lr_renderer *renderer, lr_gpu_group *group,
                                uint32_t flight) {
    lr_gpu_flight_res *fl;

    if (flight >= LR_GPU_MAX_FLIGHTS) {
        return 0;
    }
    while (group->flights_owned <= flight) {
        group->flights[group->flights_owned].initialized = 0;
        group->flights_owned++;
    }
    fl = &group->flights[flight];
    if (fl->initialized) {
        return 1;
    }
    fl->visible = lr_gpu_make_buffer(
        renderer, (uint64_t)group->capacity * sizeof(uint32_t),
        LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST);
    fl->counter = lr_gpu_make_buffer(renderer, 8,
                                     LC_BUFFER_USAGE_STORAGE |
                                         LC_BUFFER_USAGE_TRANSFER_DST);
    fl->indirect = lr_gpu_make_buffer(
        renderer, 32,
        LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST |
            LC_BUFFER_USAGE_INDIRECT);
    if (fl->visible == NULL || fl->counter == NULL ||
        fl->indirect == NULL) {
        lc_buffer_destroy(fl->indirect);
        lc_buffer_destroy(fl->counter);
        lc_buffer_destroy(fl->visible);
        fl->indirect = NULL;
        fl->counter = NULL;
        fl->visible = NULL;
        return 0;
    }
    {
        lc_buffer *cull_bufs[3] = { group->instances, fl->visible,
                                    fl->counter };
        lc_buffer *fin_bufs[2] = { fl->counter, fl->indirect };
        lc_buffer *inst_bufs[2] = { group->instances, fl->visible };

        fl->cull_set =
            lr_gpu_make_set(renderer, renderer->cull_layout, cull_bufs,
                            3);
        fl->finalize_set = lr_gpu_make_set(
            renderer, renderer->finalize_layout, fin_bufs, 2);
        fl->inst_set = lr_gpu_make_set(renderer,
                                       renderer->instanced_layout,
                                       inst_bufs, 2);
        if (fl->cull_set == NULL || fl->finalize_set == NULL ||
            fl->inst_set == NULL) {
            lc_binding_set_destroy(fl->inst_set);
            lc_binding_set_destroy(fl->finalize_set);
            lc_binding_set_destroy(fl->cull_set);
            lc_buffer_destroy(fl->indirect);
            lc_buffer_destroy(fl->counter);
            lc_buffer_destroy(fl->visible);
            fl->inst_set = NULL;
            fl->finalize_set = NULL;
            fl->cull_set = NULL;
            fl->indirect = NULL;
            fl->counter = NULL;
            fl->visible = NULL;
            return 0;
        }
    }
    fl->initialized = 1;
    renderer->gpu_stats.descriptor_sets_alive += 3;
    renderer->gpu_stats.descriptor_updates += 3;
    return 1;
}

/* Prepare one frame (outside any pass): group PBR items, upload
 * instances + zeroed counters, run cull + finalize dispatches with
 * exact transfer->compute->compute barriers. */
lr_result lr_gpu_prepare(lr_renderer *renderer,
                         lc_command_encoder *encoder) {
    uint64_t t0 = 0;
    uint32_t flight = 0;
    uint32_t flight_count = 0;
    uint32_t i;
    lc_result cr;

    if (renderer == NULL || encoder == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!renderer->frame_open) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->render_mode != LR_RENDER_MODE_GPU_DRIVEN) {
        return LR_SUCCESS;
    }
    if (renderer->gpu_prepared_frame == renderer->frame_number) {
        return LR_SUCCESS;
    }
    if (lr_gpu_ensure_shared(renderer) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_get_flight_slot(encoder, &flight,
                                   &flight_count) != LC_SUCCESS) {
        /* Not a frame encoder (or no open frame): leave
         * unprepared; the draw path falls back to CPU draws. */
        return LR_SUCCESS;
    }
    if (flight >= LR_GPU_MAX_FLIGHTS || flight_count == 0 ||
        flight_count > LR_GPU_MAX_FLIGHTS) {
        return LR_SUCCESS;
    }
    t0 = lr_perf_now();
    lr_gpu_prune_dead_groups(renderer);
    /* Reset per-frame group counts (capacities persist). */
    for (i = 0; i < renderer->group_count; i++) {
        renderer->groups[i].count = 0;
    }
    renderer->gpu_stats.instances_submitted = 0;
    renderer->gpu_stats.compute_dispatches = 0;
    renderer->gpu_stats.indirect_draw_calls = 0;
    renderer->gpu_stats.indirect_commands = 0;
    renderer->gpu_stats.gpu_driven_batches = 0;
    /* Group every live PBR item (CPU culling skipped: the GPU
     * decides visibility; shadows re-cull per light themselves). */
    for (i = 0; i < renderer->queued; i++) {
        lr_queued_item *item = &renderer->queue[i];
        lr_gpu_group *group = NULL;
        lr_gpu_instance *dst = NULL;

        if (!lr_mesh_is_live(renderer, item->mesh) ||
            !lr_material_is_live(renderer, item->material)) {
            continue;
        }
        if (item->material->type !=
            LR_MATERIAL_PBR_METALLIC_ROUGHNESS) {
            continue;
        }
        group = lr_gpu_group_for(renderer, item->mesh, item->material,
                                 item->receives_shadow,
                                 item->mirrored, 1);
        if (group == NULL) {
            return LR_ERROR_RENDER;
        }
        dst = &group->cpu[group->count];
        memcpy(dst->model, item->matrix, sizeof(dst->model));
        /* Local bounds ride along; the shader world-transforms
         * them (PART Z: max-axis-scale conservative). */
        dst->bounds[0] = item->mesh->bounds.center[0];
        dst->bounds[1] = item->mesh->bounds.center[1];
        dst->bounds[2] = item->mesh->bounds.center[2];
        dst->bounds[3] = item->mesh->bounds.radius;
        dst->mesh_index = 0;
        dst->object_id = i;
        dst->stable_id = item->instance_id;
        group->count++;
        renderer->gpu_stats.instances_submitted++;
    }
    /* Upload + dispatch per non-empty group. */
    for (i = 0; i < renderer->group_count; i++) {
        lr_gpu_group *group = &renderer->groups[i];
        lr_gpu_flight_res *fl = NULL;
        lc_gpu_signal sig = { 0 };
        uint32_t zero_counter[2];
        lr_cull_push cull_push;
        lr_finalize_push fin_push;
        uint32_t k;

        if (group->count == 0) {
            continue;
        }
        if (!lr_gpu_flight_ensure(renderer, group, flight)) {
            return LR_ERROR_RENDER;
        }
        fl = &group->flights[flight];
        /* Instance upload + zeroed counter (PART AB: small uploads,
         * fire-and-forget; the frame submit orders them before the
         * dispatches below via the transfer wait). */
        if (lc_upload_buffer_async(
                renderer->device, group->instances, 0, group->cpu,
                (uint64_t)group->count * sizeof(lr_gpu_instance),
                &sig) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        zero_counter[0] = 0;
        zero_counter[1] = group->capacity;
        if (lc_upload_buffer_async(renderer->device, fl->counter, 0,
                                   zero_counter, sizeof(zero_counter),
                                   &sig) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        /* Transitions into compute states. */
        if (lc_encoder_transition_buffer(
                encoder, group->instances,
                LC_RESOURCE_STATE_STORAGE_READ) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        if (lc_encoder_transition_buffer(
                encoder, fl->visible,
                LC_RESOURCE_STATE_STORAGE_WRITE) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        if (lc_encoder_transition_buffer(
                encoder, fl->counter,
                LC_RESOURCE_STATE_STORAGE_WRITE) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        if (lc_encoder_transition_buffer(
                encoder, fl->indirect,
                LC_RESOURCE_STATE_STORAGE_WRITE) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        /* Cull dispatch. */
        if (lc_encoder_bind_compute_pipeline(encoder,
                                             renderer->cull_pipeline) !=
            LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        if (lc_encoder_bind_compute_set(encoder, renderer->cull_pipeline,
                                        0, fl->cull_set) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        memset(&cull_push, 0, sizeof(cull_push));
        for (k = 0; k < 6; k++) {
            cull_push.planes[k][0] = renderer->frustum_planes[k][0];
            cull_push.planes[k][1] = renderer->frustum_planes[k][1];
            cull_push.planes[k][2] = renderer->frustum_planes[k][2];
            cull_push.planes[k][3] = renderer->frustum_planes[k][3];
        }
        cull_push.instance_count = group->count;
        if (lc_encoder_push_compute_constants(
                encoder, renderer->cull_pipeline,
                LC_SHADER_VISIBILITY_COMPUTE, 0, sizeof(cull_push),
                &cull_push) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        cr = lc_encoder_dispatch(encoder, (group->count + 63u) / 64u,
                                 1, 1);
        if (cr != LC_SUCCESS) {
            return lr_map_result(cr);
        }
        renderer->gpu_stats.compute_dispatches++;
        /* Counter/visible become shader-readable for finalize. */
        if (lc_encoder_transition_buffer(
                encoder, fl->counter,
                LC_RESOURCE_STATE_STORAGE_READ) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        if (lc_encoder_transition_buffer(
                encoder, fl->visible,
                LC_RESOURCE_STATE_STORAGE_READ) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        /* Finalize dispatch (one workgroup writes one command). */
        if (lc_encoder_bind_compute_pipeline(
                encoder, renderer->finalize_pipeline) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        if (lc_encoder_bind_compute_set(
                encoder, renderer->finalize_pipeline, 0,
                fl->finalize_set) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        memset(&fin_push, 0, sizeof(fin_push));
        fin_push.vertex_count = 0;
        fin_push.first_vertex = 0;
        fin_push.index_count = group->mesh->index_count;
        fin_push.first_index = 0;
        fin_push.vertex_offset_bits = 0;
        fin_push.indexed = 1;
        if (lc_encoder_push_compute_constants(
                encoder, renderer->finalize_pipeline,
                LC_SHADER_VISIBILITY_COMPUTE, 0, sizeof(fin_push),
                &fin_push) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        cr = lc_encoder_dispatch(encoder, 1, 1, 1);
        if (cr != LC_SUCCESS) {
            return lr_map_result(cr);
        }
        renderer->gpu_stats.compute_dispatches++;
        renderer->gpu_stats.gpu_driven_batches++;
        /* The draw call strictly requires INDIRECT_READ (barriers
         * are illegal inside passes, so this runs here, still
         * outside the pass, ordered after the finalize write). */
        if (lc_encoder_transition_buffer(
                encoder, fl->indirect,
                LC_RESOURCE_STATE_INDIRECT_READ) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    renderer->gpu_prepared_frame = renderer->frame_number;
    renderer->gpu_last_flight = flight;
    renderer->gpu_stats.cpu_prepare_ms = lr_perf_to_ms(
        lr_perf_now() - t0, renderer->perf_freq);
    return LR_SUCCESS;
}

/* Draw prepared groups (inside an open pass): bind the instanced
 * PBR variant + material/shadow/env/instance sets, push the group
 * flag block, one indexed indirect draw each. Unlit items are NOT
 * drawn here (the existing CPU loop handles them). */
lr_result lr_gpu_record_draws(lr_renderer *renderer,
                              lc_command_encoder *encoder,
                              lc_render_target *target,
                              const lc_render_target_desc *signature) {
    uint32_t flight = 0;
    uint32_t flight_count = 0;
    uint32_t i;
    lc_pipeline *bound_pipeline = NULL;
    lr_material *bound_material = NULL;
    lc_buffer *bound_mesh_vb = NULL;
    const lr_environment *bound_env = NULL;
    uint64_t bound_env_epoch = 0;
    int env_bound = 0;

    if (renderer == NULL || encoder == NULL || target == NULL ||
        signature == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->render_mode != LR_RENDER_MODE_GPU_DRIVEN) {
        return LR_SUCCESS;
    }
    if (renderer->gpu_prepared_frame != renderer->frame_number) {
        /* Not prepared (legacy path without prepare, or prepare
         * failed): draw nothing here; the CPU loop covers PBR. */
        return LR_SUCCESS;
    }
    if (lc_encoder_get_flight_slot(encoder, &flight,
                                   &flight_count) != LC_SUCCESS) {
        return LR_SUCCESS;
    }
    if (flight >= LR_GPU_MAX_FLIGHTS) {
        return LR_SUCCESS;
    }
    for (i = 0; i < renderer->group_count; i++) {
        lr_gpu_group *group = &renderer->groups[i];
        lr_gpu_flight_res *fl = NULL;
        lc_pipeline *pipeline = NULL;
        lr_result res;
        lc_result cr;
        lr_pbr_push push;
        int want_cull;

        if (group->count == 0 || flight >= group->flights_owned) {
            continue;
        }
        fl = &group->flights[flight];
        if (!fl->initialized) {
            continue;
        }
        want_cull = group->material->double_sided ? LC_CULL_NONE
                                                  : LC_CULL_BACK;
        res = lr_renderer_instanced_pipeline_for(
            renderer, signature, LR_MATERIAL_PBR_METALLIC_ROUGHNESS,
            want_cull,
            group->mirrored ? LC_FRONT_FACE_CLOCKWISE
                            : LC_FRONT_FACE_COUNTER_CLOCKWISE,
            &pipeline);
        if (res != LR_SUCCESS) {
            fprintf(stderr, "[dbg] gpu draws: pipeline_for failed\n");
            return res;
        }
        /* Same defensive structural check as the CPU loop. */
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
            bound_material = NULL;
            bound_env = NULL;
            bound_env_epoch = 0;
            env_bound = 0;
            cr = lc_encoder_bind_binding_set(encoder, pipeline, 1,
                                             renderer->shadow_set);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            renderer->stats.pipeline_binds++;
        }
        {
            const lr_environment *env = renderer->active_env;
            uint64_t epoch =
                (env != NULL) ? env->source_epoch : 0;
            lc_binding_set *set =
                (env != NULL && env->ready)
                    ? env->frame_set
                    : renderer->empty_env_set;

            if (!env_bound || bound_env != env ||
                bound_env_epoch != epoch) {
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
        if (group->material != bound_material) {
            cr = lc_encoder_bind_binding_set(encoder, pipeline, 0,
                                             group->material->set);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            bound_material = group->material;
            renderer->stats.material_binds++;
        }
        cr = lc_encoder_bind_binding_set(encoder, pipeline, 3,
                                         fl->inst_set);
        if (cr != LC_SUCCESS) {
            return lr_map_result(cr);
        }
        if (bound_mesh_vb != group->mesh->vertex_buffer) {
            cr = lc_encoder_bind_vertex_buffer(
                encoder, 0, group->mesh->vertex_buffer, 0);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            cr = lc_encoder_bind_index_buffer(
                encoder, group->mesh->index_buffer, 0,
                LC_INDEX_UINT32);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            bound_mesh_vb = group->mesh->vertex_buffer;
        }
        /* Group flag block (identity model: instances carry their
         * own transforms; only the shadow-receive flag varies).
         * normal_matrix is 3 column-major vec4s (w lanes zero). */
        memset(&push, 0, sizeof(push));
        push.model[0] = 1.0f;
        push.model[5] = 1.0f;
        push.model[10] = 1.0f;
        push.model[15] = 1.0f;
        push.normal_matrix[0] = 1.0f;
        push.normal_matrix[5] = 1.0f;
        push.normal_matrix[10] = 1.0f;
        push.flags = group->receives_shadow ? 1u : 0u;
        cr = lc_encoder_push_constants(encoder, pipeline,
                                       (uint32_t)
                                           LC_SHADER_VISIBILITY_VERTEX |
                                       (uint32_t)
                                           LC_SHADER_VISIBILITY_FRAGMENT,
                                       0, sizeof(push), &push);
        if (cr != LC_SUCCESS) {
            return lr_map_result(cr);
        }
        cr = lc_encoder_draw_indexed_indirect(encoder, fl->indirect, 0,
                                              1, 20);
        if (cr != LC_SUCCESS) {
            return lr_map_result(cr);
        }
        renderer->stats.draw_calls++;
        renderer->stats.pbr_draw_calls++;
        renderer->gpu_stats.indirect_draw_calls++;
        renderer->gpu_stats.indirect_commands++;
    }
    return LR_SUCCESS;
}

/* TEST-ONLY visibility download (stalls by design). Fills
 * instances_visible/culled, detects overflow loudly in Debug. */
lr_result lr_renderer_update_gpu_visibility_stats(lr_renderer *renderer) {
    uint64_t visible = 0;
    uint64_t overflows = 0;
    uint32_t i;

    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->render_mode != LR_RENDER_MODE_GPU_DRIVEN) {
        return LR_SUCCESS;
    }
    /* TEST-ONLY visibility download (stalls by design). Reads the
     * last prepared flight slot only (deterministic: other slots
     * hold other frames' counters). Groups with count == 0 submitted
     * nothing this frame: their counter buffers still hold a
     * previous frame's value, so they contribute zero without a
     * read (audit fix: parity-split groups made empty groups
     * common; reading stale counters inflated visible). */
    for (i = 0; i < renderer->group_count; i++) {
        lr_gpu_group *group = &renderer->groups[i];
        lr_gpu_flight_res *fl = NULL;
        uint32_t words[2] = { 0, 0 };
        uint32_t got = 0;

        if (group->count == 0) {
            continue;
        }
        if (renderer->gpu_last_flight >= group->flights_owned) {
            continue;
        }
        fl = &group->flights[renderer->gpu_last_flight];
        if (!fl->initialized) {
            continue;
        }
        if (lc_buffer_read(fl->counter, 0, words, sizeof(words)) !=
            LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        got = words[0];
        if (got > group->capacity) {
            overflows++;
            got = group->capacity;
        }
        visible += got;
        renderer->stats.triangles +=
            got * (group->mesh->index_count / 3u);
    }
    renderer->gpu_stats.instances_visible = visible;
    renderer->gpu_stats.instances_culled =
        (renderer->gpu_stats.instances_submitted > visible)
            ? renderer->gpu_stats.instances_submitted - visible
            : 0;
    renderer->gpu_stats.counter_overflows = overflows;
    renderer->stats.visible_objects = (uint32_t)visible;
#if !defined(NDEBUG)
    if (overflows > 0) {
        fprintf(stderr,
                "[lumac] GPU culling overflow: %llu group(s) exceeded "
                "instance capacity (count clamped)\n",
                (unsigned long long)overflows);
    }
#endif
    return LR_SUCCESS;
}

/* Public mode + stats API. */

lr_result lr_renderer_set_render_mode(lr_renderer *renderer,
                                      lr_render_mode mode) {
    lc_compute_capabilities caps;

    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (mode != LR_RENDER_MODE_CPU &&
        mode != LR_RENDER_MODE_GPU_DRIVEN) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (mode == LR_RENDER_MODE_GPU_DRIVEN) {
        memset(&caps, 0, sizeof(caps));
        lc_device_get_compute_capabilities(renderer->device, &caps);
        if (!caps.compute_supported || !caps.indirect_draw_supported) {
            return LR_ERROR_INVALID_ARGUMENT;
        }
        if (lr_gpu_ensure_shared(renderer) != LR_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    renderer->render_mode = mode;
    return LR_SUCCESS;
}

lr_render_mode lr_renderer_get_render_mode(const lr_renderer *renderer) {
    if (renderer == NULL) {
        return LR_RENDER_MODE_CPU;
    }
    return renderer->render_mode;
}

void lr_renderer_get_gpu_driven_stats(
    const lr_renderer *renderer, lr_gpu_driven_stats *out_stats) {
    if (out_stats == NULL) {
        return;
    }
    memset(out_stats, 0, sizeof(*out_stats));
    if (renderer == NULL) {
        return;
    }
    *out_stats = renderer->gpu_stats;
}

lr_result lr_renderer_prepare_gpu(lr_renderer *renderer,
                                   lc_command_encoder *encoder) {
    if (renderer == NULL || encoder == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* Extended visibility (Hi-Z + LOD) replaces the legacy
     * grouping/culling when enabled; legacy stays byte-identical
     * otherwise. */
    if (renderer->render_mode == LR_RENDER_MODE_GPU_DRIVEN &&
        renderer->vis_settings.enabled) {
        return lr_vis_prepare(renderer, encoder);
    }
    return lr_gpu_prepare(renderer, encoder);
}
