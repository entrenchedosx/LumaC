/*
 * Extended visibility: Hi-Z occlusion + GPU LOD (Phase 23).
 *
 * Opt-in enhancement of GPU-driven mode (vis_settings.enabled).
 * Per frame, outside any pass: generate the Hi-Z pyramid from the
 * previous frame's scene depth, upload instance data, run one
 * frustum+occlusion+LOD cull dispatch plus one finalize dispatch
 * per group, and transition indirect/count buffers for the draw
 * path. Draws bind one index buffer per active (group, LOD) and
 * consume GPU-written counts (native indirect-count where
 * supported, zero-instance no-op fallback otherwise).
 *
 * Temporal policy: occlusion always tests against the PREVIOUS
 * frame's pyramid (one frame of latency, no depth prepass). Frames
 * where the depth cannot be trusted (first frame, camera teleport,
 * extent change, Hi-Z disabled) bypass occlusion and keep every
 * frustum-visible instance: stale depth can never hide geometry.
 * New instances carry UNKNOWN history and take a deterministic
 * metric LOD on their first frame.
 *
 * Production frames never read visibility back to the CPU; stats
 * flow through lr_renderer_update_visibility_stats (test-only).
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

extern const unsigned char lr_hiz_copy_comp_spv[];
extern const unsigned long lr_hiz_copy_comp_spv_size;
extern const unsigned char lr_hiz_reduce_comp_spv[];
extern const unsigned long lr_hiz_reduce_comp_spv_size;
extern const unsigned char lr_vis_cull_comp_spv[];
extern const unsigned long lr_vis_cull_comp_spv_size;
extern const unsigned char lr_vis_finalize_comp_spv[];
extern const unsigned long lr_vis_finalize_comp_spv_size;

/* Visibility flags (match vis_cull.comp). */
#define LR_VIS_FLAG_HIZ 1u
#define LR_VIS_FLAG_BYPASS 2u
#define LR_VIS_FLAG_LOD 4u
#define LR_VIS_HISTORY_UNKNOWN 0xFFFFFFFFu
/* Teleport heuristic: per-frame view-projection drift this large
 * cannot be smooth motion (orbit steps are ~1e-2); bypass one
 * frame rather than trust stale depth. Conservative direction:
 * a false positive only costs culling for a frame. */
#define LR_VIS_TELEPORT_EPS 0.25f
#define LR_VIS_MAX_LOD_SEGMENTS 4u

/* Shared cull params (std430 mirror of vis_cull.comp Params). */
typedef struct lr_vis_params {
    float view_proj_curr[16];
    float view_proj_prev[16];
    float planes[6][4];
    float cam_pos_prev[4];
    uint32_t view_w;
    uint32_t view_h;
    uint32_t hiz_w;
    uint32_t hiz_h;
    uint32_t hiz_mips;
    uint32_t lod_capacity;
    uint32_t flags;
    uint32_t pad0;
    float occlusion_bias;
    float lod_hysteresis;
    float pad1[2];
} lr_vis_params;

/* Cull push: 28 bytes (<= 128 minimum). Per-group capacity
 * rides here (NOT in the shared params: that buffer is
 * CPU-written once per frame, so per-group values would race;
 * push constants are recorded per dispatch). */
typedef struct lr_vis_cull_push {
    uint32_t instance_count;
    uint32_t lod_count;
    float switch_px[4];
    uint32_t lod_capacity;
} lr_vis_cull_push;

/* Finalize push: 24 bytes. */
typedef struct lr_vis_finalize_push {
    uint32_t lod_count;
    uint32_t index_counts[4];
    uint32_t capacity;
} lr_vis_finalize_push;

static uint32_t lr_vis_ceil_div(uint32_t n, uint32_t d) {
    return (n + d - 1u) / d;
}

static lc_buffer *lr_vis_make_buffer(lr_renderer *renderer,
                                     uint64_t size, uint32_t usage,
                                     uint32_t memory) {
    lc_buffer_desc desc;
    lc_buffer *buffer = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.size = size;
    desc.usage = usage;
    desc.memory = (lc_memory_usage)memory;
    if (lc_buffer_create(renderer->device, &desc, &buffer) !=
        LC_SUCCESS) {
        return NULL;
    }
    return buffer;
}

static lc_binding_layout *lr_vis_make_layout(
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

static lc_shader *lr_vis_make_shader(lr_renderer *renderer,
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

static lr_result lr_vis_make_compute(
    lr_renderer *renderer, lc_shader *shader,
    const lc_binding_layout *layout, uint32_t push_size,
    lc_compute_pipeline **out_pipeline) {
    lc_compute_pipeline_desc desc;
    lc_push_constant_range push;
    const lc_binding_layout *slots[1];

    memset(&desc, 0, sizeof(desc));
    memset(&push, 0, sizeof(push));
    slots[0] = layout;
    push.visibility = LC_SHADER_VISIBILITY_COMPUTE;
    push.offset = 0;
    push.size = push_size;
    desc.compute_shader = shader;
    desc.binding_layouts = slots;
    desc.binding_layout_count = 1;
    desc.push_constant_ranges = &push;
    desc.push_constant_range_count = 1;
    if (lc_compute_pipeline_create(renderer->device, &desc,
                                   out_pipeline) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

lr_result lr_vis_ensure_shared(lr_renderer *renderer) {
    lc_binding_desc binds[9];
    lc_sampler_desc sdesc;
    uint32_t i;

    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->vis_ready) {
        return LR_SUCCESS;
    }
    renderer->vis_hiz_copy_shader = lr_vis_make_shader(
        renderer, lr_hiz_copy_comp_spv, lr_hiz_copy_comp_spv_size);
    renderer->vis_hiz_reduce_shader = lr_vis_make_shader(
        renderer, lr_hiz_reduce_comp_spv,
        lr_hiz_reduce_comp_spv_size);
    renderer->vis_cull_shader = lr_vis_make_shader(
        renderer, lr_vis_cull_comp_spv, lr_vis_cull_comp_spv_size);
    renderer->vis_finalize_shader = lr_vis_make_shader(
        renderer, lr_vis_finalize_comp_spv,
        lr_vis_finalize_comp_spv_size);
    if (renderer->vis_hiz_copy_shader == NULL ||
        renderer->vis_hiz_reduce_shader == NULL ||
        renderer->vis_cull_shader == NULL ||
        renderer->vis_finalize_shader == NULL) {
        return LR_ERROR_RENDER;
    }
    /* Hi-Z copy/reduce layouts: sampled src + storage dst + sampler. */
    for (i = 0; i < 3; i++) {
        binds[i].binding = i;
        binds[i].count = 1;
        binds[i].visibility = LC_SHADER_VISIBILITY_COMPUTE;
    }
    binds[0].type = LC_BINDING_SAMPLED_IMAGE;
    binds[1].type = LC_BINDING_STORAGE_IMAGE;
    binds[2].type = LC_BINDING_SAMPLER;
    renderer->vis_hiz_copy_layout =
        lr_vis_make_layout(renderer, binds, 3);
    renderer->vis_hiz_reduce_layout =
        lr_vis_make_layout(renderer, binds, 3);
    /* Cull layout: instances + visible + counters + history +
     * history owner tags + params + Hi-Z + sampler + stats
     * (COMPUTE). Binding 8 carries the u64 owner tag per history
     * slot (Phase 24 stable identity). */
    for (i = 0; i < 9; i++) {
        binds[i].binding = i;
        binds[i].count = 1;
        binds[i].visibility = LC_SHADER_VISIBILITY_COMPUTE;
    }
    binds[0].type = LC_BINDING_STORAGE_BUFFER;
    binds[1].type = LC_BINDING_STORAGE_BUFFER;
    binds[2].type = LC_BINDING_STORAGE_BUFFER;
    binds[3].type = LC_BINDING_STORAGE_BUFFER;
    binds[4].type = LC_BINDING_STORAGE_BUFFER;
    binds[5].type = LC_BINDING_SAMPLED_IMAGE;
    binds[6].type = LC_BINDING_SAMPLER;
    binds[7].type = LC_BINDING_STORAGE_BUFFER;
    binds[8].type = LC_BINDING_STORAGE_BUFFER;
    renderer->vis_cull_layout = lr_vis_make_layout(renderer, binds, 9);
    /* Finalize layout: counters + indirect + count (COMPUTE). */
    for (i = 0; i < 3; i++) {
        binds[i].binding = i;
        binds[i].count = 1;
        binds[i].visibility = LC_SHADER_VISIBILITY_COMPUTE;
    }
    binds[0].type = LC_BINDING_STORAGE_BUFFER;
    binds[1].type = LC_BINDING_STORAGE_BUFFER;
    binds[2].type = LC_BINDING_STORAGE_BUFFER;
    renderer->vis_finalize_layout =
        lr_vis_make_layout(renderer, binds, 3);
    /* Vertex-fetch layout: instances + visible (VERTEX). */
    for (i = 0; i < 2; i++) {
        binds[i].binding = i;
        binds[i].count = 1;
        binds[i].visibility = LC_SHADER_VISIBILITY_VERTEX;
    }
    binds[0].type = LC_BINDING_STORAGE_BUFFER;
    binds[1].type = LC_BINDING_STORAGE_BUFFER;
    renderer->vis_vert_layout = lr_vis_make_layout(renderer, binds, 2);
    if (renderer->vis_hiz_copy_layout == NULL ||
        renderer->vis_hiz_reduce_layout == NULL ||
        renderer->vis_cull_layout == NULL ||
        renderer->vis_finalize_layout == NULL ||
        renderer->vis_vert_layout == NULL) {
        return LR_ERROR_RENDER;
    }
    if (lr_vis_make_compute(renderer, renderer->vis_hiz_copy_shader,
                            renderer->vis_hiz_copy_layout, 8u,
                            &renderer->vis_hiz_copy_pipeline) !=
            LR_SUCCESS ||
        lr_vis_make_compute(renderer,
                            renderer->vis_hiz_reduce_shader,
                            renderer->vis_hiz_reduce_layout, 20u,
                            &renderer->vis_hiz_reduce_pipeline) !=
            LR_SUCCESS ||
        lr_vis_make_compute(renderer, renderer->vis_cull_shader,
                            renderer->vis_cull_layout, 28u,
                            &renderer->vis_cull_pipeline) !=
            LR_SUCCESS ||
        lr_vis_make_compute(renderer, renderer->vis_finalize_shader,
                            renderer->vis_finalize_layout, 24u,
                            &renderer->vis_finalize_pipeline) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Nearest clamp sampler: occlusion uses texelFetch (exact
     * texels, no filtering), so the sampler only satisfies the
     * combined-image descriptor. */
    memset(&sdesc, 0, sizeof(sdesc));
    sdesc.min_filter = LC_FILTER_NEAREST;
    sdesc.mag_filter = LC_FILTER_NEAREST;
    sdesc.mipmap_mode = LC_MIPMAP_MODE_NEAREST;
    sdesc.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
    sdesc.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
    sdesc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
    sdesc.max_anisotropy = 1.0f;
    if (lc_sampler_create(renderer->device, &sdesc,
                          &renderer->vis_sampler) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Shared params block (CPU-written each frame, GPU read-only
     * afterwards; transitioned once, then stable). */
    renderer->vis_params_buffer = lr_vis_make_buffer(
        renderer, sizeof(lr_vis_params), LC_BUFFER_USAGE_STORAGE,
        LC_MEMORY_CPU_TO_GPU);
    if (renderer->vis_params_buffer == NULL) {
        return LR_ERROR_RENDER;
    }
    if (lc_binding_set_create(renderer->vis_hiz_copy_layout,
                              &renderer->vis_hiz_copy_set) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    renderer->vis_ready = 1;
    return LR_SUCCESS;
}

static void lr_vis_flight_teardown(lr_renderer *renderer,
                                   lr_vis_group *group, uint32_t flight) {
    lr_vis_flight *fl = &group->flights[flight];

    (void)renderer;
    /* No initialized early-out: ensure-failure paths call this with
     * partially created buffers, and every destroy below is
     * NULL-safe. Returning early here would leak the partial set
     * (fail-cleanly violation). */
    lc_binding_set_destroy(fl->vert_set);
    lc_binding_set_destroy(fl->finalize_set);
    lc_binding_set_destroy(fl->cull_set);
    lc_buffer_destroy(fl->stats);
    lc_buffer_destroy(fl->history_ids);
    lc_buffer_destroy(fl->history);
    lc_buffer_destroy(fl->count);
    lc_buffer_destroy(fl->indirect);
    lc_buffer_destroy(fl->lod_counters);
    lc_buffer_destroy(fl->visible);
    memset(fl, 0, sizeof(*fl));
}

void lr_vis_destroy_shared(lr_renderer *renderer) {
    uint32_t i;

    if (renderer == NULL) {
        return;
    }
    if (renderer->vis_graph != NULL) {
        lr_render_graph_destroy(renderer->vis_graph);
        renderer->vis_graph = NULL;
    }
    lr_hiz_free_reduce_sets(renderer);
    lr_hiz_destroy(renderer);
    for (i = 0; i < renderer->vis_group_count; i++) {
        lr_vis_group *group = &renderer->vis_groups[i];
        uint32_t f;

        for (f = 0; f < group->flights_owned; f++) {
            lr_vis_flight_teardown(renderer, group, f);
        }
        lc_buffer_destroy(group->instances);
        free(group->cpu);
    }
    free(renderer->vis_groups);
    renderer->vis_groups = NULL;
    renderer->vis_group_count = 0;
    renderer->vis_group_capacity = 0;
    lc_binding_set_destroy(renderer->vis_hiz_copy_set);
    lc_buffer_destroy(renderer->vis_params_buffer);
    lc_sampler_destroy(renderer->vis_sampler);
    lc_compute_pipeline_destroy(renderer->vis_finalize_pipeline);
    lc_compute_pipeline_destroy(renderer->vis_cull_pipeline);
    lc_compute_pipeline_destroy(renderer->vis_hiz_reduce_pipeline);
    lc_compute_pipeline_destroy(renderer->vis_hiz_copy_pipeline);
    lc_binding_layout_destroy(renderer->vis_finalize_layout);
    lc_binding_layout_destroy(renderer->vis_vert_layout);
    lc_binding_layout_destroy(renderer->vis_cull_layout);
    lc_binding_layout_destroy(renderer->vis_hiz_reduce_layout);
    lc_binding_layout_destroy(renderer->vis_hiz_copy_layout);
    lc_shader_destroy(renderer->vis_finalize_shader);
    lc_shader_destroy(renderer->vis_cull_shader);
    lc_shader_destroy(renderer->vis_hiz_reduce_shader);
    lc_shader_destroy(renderer->vis_hiz_copy_shader);
    renderer->vis_hiz_copy_set = NULL;
    renderer->vis_params_buffer = NULL;
    renderer->vis_sampler = NULL;
    renderer->vis_finalize_pipeline = NULL;
    renderer->vis_cull_pipeline = NULL;
    renderer->vis_hiz_reduce_pipeline = NULL;
    renderer->vis_hiz_copy_pipeline = NULL;
    renderer->vis_finalize_layout = NULL;
    renderer->vis_vert_layout = NULL;
    renderer->vis_cull_layout = NULL;
    renderer->vis_hiz_reduce_layout = NULL;
    renderer->vis_hiz_copy_layout = NULL;
    renderer->vis_finalize_shader = NULL;
    renderer->vis_cull_shader = NULL;
    renderer->vis_hiz_reduce_shader = NULL;
    renderer->vis_hiz_copy_shader = NULL;
    renderer->vis_ready = 0;
    renderer->vis_params_primed = 0;
}

void lr_visibility_settings_default(lr_visibility_settings *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->occlusion_depth_bias = 0.0f;
    out->lod_hysteresis_margin = 0.15f;
    out->indirect_count_enabled = 1;
    out->graph_enabled = 1;
}

lr_result lr_renderer_set_visibility(
    lr_renderer *renderer, const lr_visibility_settings *settings) {
    if (renderer == NULL || settings == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!(settings->occlusion_depth_bias >= 0.0f) ||
        !(settings->lod_hysteresis_margin >= 0.0f) ||
        !(settings->lod_hysteresis_margin <= 1.0f)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    renderer->vis_settings = *settings;
    return LR_SUCCESS;
}

void lr_renderer_get_visibility_settings(
    const lr_renderer *renderer, lr_visibility_settings *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (renderer == NULL) {
        return;
    }
    *out = renderer->vis_settings;
}

void lr_renderer_get_visibility_stats(
    const lr_renderer *renderer, lr_visibility_stats *out) {
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (renderer == NULL) {
        return;
    }
    *out = renderer->vis_stats;
}

/* Latch current-frame matrices; decide the occlusion bypass.
 * LOD hysteresis history is keyed by stable instance identity
 * (per-slot owner tags), so camera motion alone NEVER invalidates
 * it: only submission-order churn (slot reuse by a different
 * stable ID) restarts UNKNOWN. */
void lr_vis_on_begin(lr_renderer *renderer, const float view_proj[16],
                     const float cam_pos[3]) {
    float worst = 0.0f;
    int i;

    if (renderer == NULL || view_proj == NULL || cam_pos == NULL) {
        return;
    }
    if (renderer->vis_has_prev) {
        for (i = 0; i < 16; i++) {
            float d = view_proj[i] - renderer->vis_prev_view_proj[i];

            if (d < 0.0f) {
                d = -d;
            }
            if (d > worst) {
                worst = d;
            }
        }
    }
    memcpy(renderer->vis_view_proj, view_proj, sizeof(float) * 16u);
    memcpy(renderer->vis_cam_pos, cam_pos, sizeof(float) * 3u);
    /* Bypass when the previous pyramid cannot be trusted. The
     * extent test runs in prepare (HDR size known there); first
     * frame and teleports bypass here. */
    renderer->vis_bypass =
        (!renderer->vis_has_prev || worst > LR_VIS_TELEPORT_EPS)
            ? 1
            : 0;
    if (renderer->render_mode != LR_RENDER_MODE_GPU_DRIVEN ||
        !renderer->vis_settings.enabled) {
        /* Legacy/CPU frames still advance the history so a later
         * enable starts from fresh, valid matrices. Depth is
         * discarded on these frames, so a later enable bypasses
         * occlusion once (no trusted pyramid yet). */
        memcpy(renderer->vis_prev_view_proj, view_proj,
               sizeof(float) * 16u);
        memcpy(renderer->vis_prev_cam_pos, cam_pos,
               sizeof(float) * 3u);
        renderer->vis_has_prev = 1;
        renderer->vis_bypass = 1;
        renderer->vis_hiz_was_enabled = 0;
    }
}

/* Group management (mirrors the legacy key discipline). */

static void lr_vis_prune_dead_groups(lr_renderer *renderer) {
    uint32_t i = 0;

    while (i < renderer->vis_group_count) {
        lr_vis_group *group = &renderer->vis_groups[i];

        if (lr_mesh_is_live(renderer, group->mesh) &&
            lr_material_is_live(renderer, group->material)) {
            i++;
            continue;
        }
        {
            uint32_t f;

            for (f = 0; f < group->flights_owned; f++) {
                lr_vis_flight_teardown(renderer, group, f);
            }
            lc_buffer_destroy(group->instances);
            free(group->cpu);
            renderer->vis_group_count--;
            if (i < renderer->vis_group_count) {
                renderer->vis_groups[i] =
                    renderer->vis_groups[renderer->vis_group_count];
            }
        }
    }
}

static lr_vis_group *lr_vis_group_for(lr_renderer *renderer,
                                      lr_mesh *mesh,
                                      lr_material *material,
                                      int receives_shadow,
                                      int mirrored,
                                      uint32_t need) {
    uint32_t i;

    for (i = 0; i < renderer->vis_group_count; i++) {
        lr_vis_group *group = &renderer->vis_groups[i];

        if (group->mesh == mesh && group->material == material &&
            group->receives_shadow == receives_shadow &&
            group->mirrored == mirrored) {
            if (group->count + need > group->capacity) {
                uint32_t grown =
                    (group->capacity == 0) ? 64u
                                           : group->capacity * 2u;
                lr_gpu_instance *cpu = NULL;
                lr_gpu_instance *kept = NULL;
                uint32_t f;

                while (grown < group->count + need) {
                    grown *= 2u;
                }
                /* Preserve already-grouped instances across the
                 * rebuild (growth happens mid-grouping). History
                 * restarts UNKNOWN (stability hint only). */
                if (group->count > 0) {
                    kept = (lr_gpu_instance *)malloc(
                        (size_t)group->count *
                        sizeof(lr_gpu_instance));
                    if (kept == NULL) {
                        return NULL;
                    }
                    memcpy(kept, group->cpu,
                           (size_t)group->count *
                               sizeof(lr_gpu_instance));
                }
                for (f = 0; f < group->flights_owned; f++) {
                    lr_vis_flight_teardown(renderer, group, f);
                }
                lc_buffer_destroy(group->instances);
                free(group->cpu);
                group->instances = NULL;
                group->cpu = NULL;
                group->capacity = 0;
                group->flights_owned = 0;
                cpu = (lr_gpu_instance *)calloc(
                    grown, sizeof(lr_gpu_instance));
                if (cpu == NULL) {
                    free(kept);
                    return NULL;
                }
                if (kept != NULL) {
                    memcpy(cpu, kept,
                           (size_t)group->count *
                               sizeof(lr_gpu_instance));
                    free(kept);
                }
                group->cpu = cpu;
                group->instances = lr_vis_make_buffer(
                    renderer,
                    (uint64_t)grown * sizeof(lr_gpu_instance),
                    LC_BUFFER_USAGE_STORAGE |
                        LC_BUFFER_USAGE_TRANSFER_DST,
                    LC_MEMORY_GPU_ONLY);
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
    if (renderer->vis_group_count >= LR_GPU_MAX_GROUPS) {
        return NULL;
    }
    if (renderer->vis_group_count == renderer->vis_group_capacity) {
        uint32_t grown = (renderer->vis_group_capacity == 0)
                             ? 8u
                             : renderer->vis_group_capacity * 2u;
        lr_vis_group *groups = (lr_vis_group *)realloc(
            renderer->vis_groups, sizeof(lr_vis_group) * grown);

        if (groups == NULL) {
            return NULL;
        }
        renderer->vis_groups = groups;
        renderer->vis_group_capacity = grown;
    }
    {
        lr_vis_group *group =
            &renderer->vis_groups[renderer->vis_group_count];

        memset(group, 0, sizeof(*group));
        group->mesh = mesh;
        group->material = material;
        group->receives_shadow = receives_shadow;
        group->mirrored = mirrored;
        group->cpu = (lr_gpu_instance *)calloc(
            64u, sizeof(lr_gpu_instance));
        if (group->cpu == NULL) {
            return NULL;
        }
        group->instances = lr_vis_make_buffer(
            renderer, (uint64_t)64u * sizeof(lr_gpu_instance),
            LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST,
            LC_MEMORY_GPU_ONLY);
        if (group->instances == NULL) {
            free(group->cpu);
            group->cpu = NULL;
            return NULL;
        }
        group->capacity = 64u;
        renderer->vis_group_count++;
        return group;
    }
}

static int lr_vis_flight_ensure(lr_renderer *renderer,
                                lr_vis_group *group, uint32_t flight) {
    lr_vis_flight *fl;

    if (flight >= LR_GPU_MAX_FLIGHTS) {
        return 0;
    }
    while (group->flights_owned <= flight) {
        memset(&group->flights[group->flights_owned], 0,
               sizeof(lr_vis_flight));
        group->flights_owned++;
    }
    fl = &group->flights[flight];
    if (fl->initialized) {
        return 1;
    }
    fl->visible = lr_vis_make_buffer(
        renderer,
        (uint64_t)group->capacity * LR_VIS_MAX_LOD_SEGMENTS *
            sizeof(uint32_t),
        LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST,
        LC_MEMORY_GPU_ONLY);
    fl->lod_counters = lr_vis_make_buffer(
        renderer, (uint64_t)LR_VIS_MAX_LOD_SEGMENTS * 2u *
                      sizeof(uint32_t),
        LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST,
        LC_MEMORY_GPU_ONLY);
    fl->indirect = lr_vis_make_buffer(
        renderer,
        (uint64_t)LR_VIS_MAX_LOD_SEGMENTS * 5u * sizeof(uint32_t),
        LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST |
            LC_BUFFER_USAGE_INDIRECT,
        LC_MEMORY_GPU_ONLY);
    fl->count = lr_vis_make_buffer(
        renderer,
        (uint64_t)LR_VIS_MAX_LOD_SEGMENTS * sizeof(uint32_t),
        LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST |
            LC_BUFFER_USAGE_INDIRECT,
        LC_MEMORY_GPU_ONLY);
    fl->history = lr_vis_make_buffer(
        renderer, (uint64_t)group->capacity * sizeof(uint32_t),
        LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST,
        LC_MEMORY_GPU_ONLY);
    /* Owner tags for the LOD history (Phase 24 stable identity):
     * one u64 per slot holding the lr_gpu_instance.stable_id that
     * primed it. A slot reused by a different stable ID restarts
     * UNKNOWN instead of inheriting history. */
    fl->history_ids = lr_vis_make_buffer(
        renderer, (uint64_t)group->capacity * sizeof(uint64_t),
        LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST,
        LC_MEMORY_GPU_ONLY);
    fl->stats = lr_vis_make_buffer(renderer, 2u * sizeof(uint32_t),
                                   LC_BUFFER_USAGE_STORAGE |
                                       LC_BUFFER_USAGE_TRANSFER_DST,
                                   LC_MEMORY_GPU_ONLY);
    if (fl->visible == NULL || fl->lod_counters == NULL ||
        fl->indirect == NULL || fl->count == NULL ||
        fl->history == NULL || fl->history_ids == NULL ||
        fl->stats == NULL) {
        lr_vis_flight_teardown(renderer, group, flight);
        return 0;
    }
    /* History starts UNKNOWN (deterministic first-frame LOD). */
    {
        uint32_t *blank = (uint32_t *)malloc(
            (size_t)group->capacity * sizeof(uint32_t));
        lc_gpu_signal sig;

        if (blank == NULL) {
            lr_vis_flight_teardown(renderer, group, flight);
            return 0;
        }
        memset(blank, 0xFF,
               (size_t)group->capacity * sizeof(uint32_t));
        memset(&sig, 0, sizeof(sig));
        if (lc_upload_buffer_async(renderer->device, fl->history, 0,
                                   blank,
                                   (uint64_t)group->capacity *
                                       sizeof(uint32_t),
                                   &sig) != LC_SUCCESS) {
            free(blank);
            lr_vis_flight_teardown(renderer, group, flight);
            return 0;
        }
        free(blank);
    }
    /* Owner tags start all-ones: no real stable ID (allocated from
     * 1 up) can match, so every slot restarts UNKNOWN. */
    {
        uint64_t *blank_ids = (uint64_t *)malloc(
            (size_t)group->capacity * sizeof(uint64_t));
        lc_gpu_signal sig;

        if (blank_ids == NULL) {
            lr_vis_flight_teardown(renderer, group, flight);
            return 0;
        }
        memset(blank_ids, 0xFF,
               (size_t)group->capacity * sizeof(uint64_t));
        memset(&sig, 0, sizeof(sig));
        if (lc_upload_buffer_async(renderer->device, fl->history_ids,
                                   0, blank_ids,
                                   (uint64_t)group->capacity *
                                       sizeof(uint64_t),
                                   &sig) != LC_SUCCESS) {
            free(blank_ids);
            lr_vis_flight_teardown(renderer, group, flight);
            return 0;
        }
        free(blank_ids);
    }
    /* The cull/finalize sets are written later (lr_vis_write_*),
     * once the frame's transitions make every image view valid;
     * only the set objects are created here. */
    if (lc_binding_set_create(renderer->vis_cull_layout,
                              &fl->cull_set) != LC_SUCCESS) {
        lr_vis_flight_teardown(renderer, group, flight);
        return 0;
    }
    if (lc_binding_set_create(renderer->vis_finalize_layout,
                              &fl->finalize_set) != LC_SUCCESS) {
        lr_vis_flight_teardown(renderer, group, flight);
        return 0;
    }
    /* Vertex-fetch set (instances + visible): buffer-only writes
     * validate at ensure time (UNDEFINED-lenient or prior-frame
     * STORAGE_READ both pass storage slots). */
    if (lc_binding_set_create(renderer->vis_vert_layout,
                              &fl->vert_set) != LC_SUCCESS) {
        lr_vis_flight_teardown(renderer, group, flight);
        return 0;
    }
    {
        lc_binding_write vwrites[2];

        vwrites[0].binding = 0;
        vwrites[0].array_element = 0;
        vwrites[0].type = LC_BINDING_STORAGE_BUFFER;
        vwrites[0].u.buffer.buffer = group->instances;
        vwrites[0].u.buffer.offset = 0;
        vwrites[0].u.buffer.size = 0;
        vwrites[1].binding = 1;
        vwrites[1].array_element = 0;
        vwrites[1].type = LC_BINDING_STORAGE_BUFFER;
        vwrites[1].u.buffer.buffer = fl->visible;
        vwrites[1].u.buffer.offset = 0;
        vwrites[1].u.buffer.size = 0;
        if (lc_binding_set_update(fl->vert_set, vwrites, 2) !=
            LC_SUCCESS) {
            lr_vis_flight_teardown(renderer, group, flight);
            return 0;
        }
    }
    fl->initialized = 1;
    return 1;
}

/* Fill one set's storage/image/sampler writes (states must already
 * be valid: call after the frame's transitions for image views). */
static lr_result lr_vis_write_cull_set(lr_renderer *renderer,
                                       lr_vis_group *group,
                                       lr_vis_flight *fl) {
    lc_binding_write writes[9];
    lc_buffer *bufs[6];
    uint32_t b;

    bufs[0] = group->instances;
    bufs[1] = fl->visible;
    bufs[2] = fl->lod_counters;
    bufs[3] = fl->history;
    bufs[4] = renderer->vis_params_buffer;
    bufs[5] = fl->stats;
    for (b = 0; b < 5; b++) {
        writes[b].binding = b;
        writes[b].array_element = 0;
        writes[b].type = LC_BINDING_STORAGE_BUFFER;
        writes[b].u.buffer.buffer = bufs[b];
        writes[b].u.buffer.offset = 0;
        writes[b].u.buffer.size = 0;
    }
    writes[5].binding = 5;
    writes[5].array_element = 0;
    writes[5].type = LC_BINDING_SAMPLED_IMAGE;
    writes[5].u.image.view = lr_hiz_sample_view(renderer);
    writes[6].binding = 6;
    writes[6].array_element = 0;
    writes[6].type = LC_BINDING_SAMPLER;
    writes[6].u.sampler.sampler = renderer->vis_sampler;
    writes[7].binding = 7;
    writes[7].array_element = 0;
    writes[7].type = LC_BINDING_STORAGE_BUFFER;
    writes[7].u.buffer.buffer = bufs[5];
    writes[7].u.buffer.offset = 0;
    writes[7].u.buffer.size = 0;
    writes[8].binding = 8;
    writes[8].array_element = 0;
    writes[8].type = LC_BINDING_STORAGE_BUFFER;
    writes[8].u.buffer.buffer = fl->history_ids;
    writes[8].u.buffer.offset = 0;
    writes[8].u.buffer.size = 0;
    if (lc_binding_set_update(fl->cull_set, writes, 9) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

static lr_result lr_vis_write_finalize_set(lr_renderer *renderer,
                                           lr_vis_flight *fl) {
    lc_binding_write writes[3];

    (void)renderer;
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_STORAGE_BUFFER;
    writes[0].u.buffer.buffer = fl->lod_counters;
    writes[0].u.buffer.offset = 0;
    writes[0].u.buffer.size = 0;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_STORAGE_BUFFER;
    writes[1].u.buffer.buffer = fl->indirect;
    writes[1].u.buffer.offset = 0;
    writes[1].u.buffer.size = 0;
    writes[2].binding = 2;
    writes[2].array_element = 0;
    writes[2].type = LC_BINDING_STORAGE_BUFFER;
    writes[2].u.buffer.buffer = fl->count;
    writes[2].u.buffer.offset = 0;
    writes[2].u.buffer.size = 0;
    if (lc_binding_set_update(fl->finalize_set, writes, 3) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

float lr_vis_projected_diameter(const float view_proj[16],
                                const float center[3], float radius,
                                uint32_t view_w, uint32_t view_h,
                                int *out_behind) {
    float w;

    if (out_behind != NULL) {
        *out_behind = 0;
    }
    if (view_proj == NULL || center == NULL || view_w == 0 ||
        view_h == 0 || !(radius > 0.0f)) {
        return 0.0f;
    }
    /* Clip w of the center (column-major: w = m[3]x+m[7]y+m[11]z+m[15]). */
    w = view_proj[3] * center[0] + view_proj[7] * center[1] +
        view_proj[11] * center[2] + view_proj[15];
    if (!(w > 0.0001f)) {
        if (out_behind != NULL) {
            *out_behind = 1;
        }
        return 1e9f;
    }
    {
        float ax = view_proj[0];
        float ay = view_proj[5];

        if (ax < 0.0f) {
            ax = -ax;
        }
        if (ay < 0.0f) {
            ay = -ay;
        }
        {
            float ppu = (ax * (float)view_w > ay * (float)view_h
                             ? ax * (float)view_w
                             : ay * (float)view_h) /
                        (2.0f * w);
            return 2.0f * radius * ppu;
        }
    }
}

lc_buffer *lr_mesh_lod_buffer(const lr_mesh *mesh, uint32_t level) {
    if (mesh == NULL || level >= mesh->lod_count ||
        level >= LR_MESH_MAX_LODS) {
        return NULL;
    }
    if (level == 0) {
        return mesh->index_buffer;
    }
    return mesh->lod_index_buffers[level];
}

uint32_t lr_mesh_lod_index_count(const lr_mesh *mesh, uint32_t level) {
    if (mesh == NULL || level >= mesh->lod_count ||
        level >= LR_MESH_MAX_LODS) {
        return 0;
    }
    if (level == 0) {
        return mesh->index_count;
    }
    return mesh->lod_index_counts[level];
}

/* ---- graph record callbacks (also the manual path) ----
 *
 * Each callback records one schedule node's GPU work with its own
 * public-LumaC transitions (the graph owns ORDER; passes own
 * barriers). The manual path calls the same three in the same
 * order, so graph on/off can only differ by scheduling (which is
 * identical here by construction: insertion order is a valid
 * topological order). */

static uint32_t lr_vis_group_lod_count(lr_renderer *renderer,
                                       const lr_vis_group *group) {
    uint32_t lod_count;

    if (group == NULL || group->mesh == NULL) {
        return 1;
    }
    lod_count = group->mesh->lod_count;
    if (lod_count < 1) {
        lod_count = 1;
    }
    if (lod_count > LR_VIS_MAX_LOD_SEGMENTS) {
        lod_count = LR_VIS_MAX_LOD_SEGMENTS;
    }
    if (!renderer->vis_settings.lod_enabled) {
        lod_count = 1;
    }
    return lod_count;
}

/* Hi-Z node: generate from previous-frame depth, or adopt the
 * readable state on bypass frames (never sample distrusted
 * depth). */
static lr_result lr_vis_record_hiz(lr_renderer *renderer,
                                   lc_command_encoder *encoder) {
    if (!renderer->vis_bypass &&
        renderer->vis_settings.hiz_enabled) {
        return lr_hiz_generate(renderer, encoder);
    }
    {
        lc_image_subresource_range range;

        memset(&range, 0, sizeof(range));
        range.base_mip_level = 0;
        range.level_count = lr_hiz_levels(renderer);
        range.base_array_layer = 0;
        range.layer_count = 1;
        if (lc_encoder_transition_image(
                encoder, lr_hiz_image(renderer), &range,
                LC_RESOURCE_STATE_SHADER_READ) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    return LR_SUCCESS;
}

/* Cull node for one group: transitions, bind, push, dispatch,
 * then publish counters/visible for finalize. */
static lr_result lr_vis_record_cull(lr_renderer *renderer,
                                    lc_command_encoder *encoder,
                                    uint32_t gi, uint32_t flight) {
    lr_vis_group *group;
    lr_vis_flight *fl;
    lr_vis_cull_push cull_push;
    uint32_t lod_count;
    uint32_t l;

    if (gi >= renderer->vis_group_count ||
        flight >= LR_GPU_MAX_FLIGHTS) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    group = &renderer->vis_groups[gi];
    if (group->count == 0 || flight >= group->flights_owned) {
        return LR_SUCCESS;
    }
    fl = &group->flights[flight];
    if (!fl->initialized) {
        return LR_ERROR_RENDER;
    }
    lod_count = lr_vis_group_lod_count(renderer, group);
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
            encoder, fl->lod_counters,
            LC_RESOURCE_STATE_STORAGE_WRITE) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_transition_buffer(
            encoder, fl->stats,
            LC_RESOURCE_STATE_STORAGE_WRITE) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_transition_buffer(
            encoder, fl->indirect,
            LC_RESOURCE_STATE_STORAGE_WRITE) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_transition_buffer(
            encoder, fl->count,
            LC_RESOURCE_STATE_STORAGE_WRITE) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* LOD history is read-modify-write EVERY frame (not just the
     * first): the cull shader reads prevLod + owner tags and writes
     * both in one dispatch. Re-record the READ_WRITE barrier every
     * frame unconditionally: same-state transitions are legal
     * no-ops that still emit an availability→visibility barrier,
     * which is exactly what orders this frame's reads after the
     * prior frame's writes. */
    {
        lc_result hr1;
        lc_result hr2;

        hr1 = lc_encoder_transition_buffer(
            encoder, fl->history,
            LC_RESOURCE_STATE_STORAGE_READ_WRITE);
        /* Owner tags ride the same transition: the cull shader
         * reads and writes both in one dispatch. */
        hr2 = lc_encoder_transition_buffer(
            encoder, fl->history_ids,
            LC_RESOURCE_STATE_STORAGE_READ_WRITE);
        if (hr1 != LC_SUCCESS || hr2 != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        fl->history_primed = 1;
    }
    if (!fl->sets_ready) {
        if (lr_vis_write_cull_set(renderer, group, fl) !=
                LR_SUCCESS ||
            lr_vis_write_finalize_set(renderer, fl) != LR_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        fl->sets_ready = 1;
    }
    if (lc_encoder_bind_compute_pipeline(
            encoder, renderer->vis_cull_pipeline) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_bind_compute_set(
            encoder, renderer->vis_cull_pipeline, 0,
            fl->cull_set) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    memset(&cull_push, 0, sizeof(cull_push));
    cull_push.instance_count = group->count;
    cull_push.lod_count = lod_count;
    for (l = 0; l < LR_VIS_MAX_LOD_SEGMENTS; l++) {
        if (l < group->mesh->lod_count) {
            cull_push.switch_px[l] = group->mesh->lod_min_px[l];
        } else {
            cull_push.switch_px[l] = 0.0f;
        }
    }
    cull_push.lod_capacity = group->capacity;
    if (lc_encoder_push_compute_constants(
            encoder, renderer->vis_cull_pipeline,
            LC_SHADER_VISIBILITY_COMPUTE, 0, sizeof(cull_push),
            &cull_push) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_dispatch(encoder,
                            lr_vis_ceil_div(group->count, 64u), 1,
                            1) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_transition_buffer(
            encoder, fl->lod_counters,
            LC_RESOURCE_STATE_STORAGE_READ) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_transition_buffer(
            encoder, fl->visible,
            LC_RESOURCE_STATE_STORAGE_READ) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    renderer->vis_stats.compute_dispatches++;
    return LR_SUCCESS;
}

/* Finalize node for one group: consume counts into indirect
 * commands + draw counts, then publish for the draw path. */
static lr_result lr_vis_record_fin(lr_renderer *renderer,
                                   lc_command_encoder *encoder,
                                   uint32_t gi, uint32_t flight) {
    lr_vis_group *group;
    lr_vis_flight *fl;
    lr_vis_finalize_push fin_push;
    uint32_t lod_count;
    uint32_t l;

    if (gi >= renderer->vis_group_count ||
        flight >= LR_GPU_MAX_FLIGHTS) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    group = &renderer->vis_groups[gi];
    if (group->count == 0 || flight >= group->flights_owned) {
        return LR_SUCCESS;
    }
    fl = &group->flights[flight];
    if (!fl->initialized) {
        return LR_ERROR_RENDER;
    }
    lod_count = lr_vis_group_lod_count(renderer, group);
    if (lc_encoder_bind_compute_pipeline(
            encoder, renderer->vis_finalize_pipeline) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_bind_compute_set(
            encoder, renderer->vis_finalize_pipeline, 0,
            fl->finalize_set) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    memset(&fin_push, 0, sizeof(fin_push));
    fin_push.lod_count = lod_count;
    for (l = 0; l < LR_VIS_MAX_LOD_SEGMENTS; l++) {
        fin_push.index_counts[l] =
            lr_mesh_lod_index_count(group->mesh, l);
    }
    fin_push.capacity = group->capacity;
    if (lc_encoder_push_compute_constants(
            encoder, renderer->vis_finalize_pipeline,
            LC_SHADER_VISIBILITY_COMPUTE, 0, sizeof(fin_push),
            &fin_push) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_dispatch(encoder, 1, 1, 1) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    renderer->vis_stats.compute_dispatches++;
    if (lc_encoder_transition_buffer(
            encoder, fl->indirect,
            LC_RESOURCE_STATE_INDIRECT_READ) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_transition_buffer(
            encoder, fl->count,
            LC_RESOURCE_STATE_INDIRECT_READ) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

/* Graph pass shims (user = group index, resolved live). */
static lr_result lr_vis_graph_hiz(lr_renderer *renderer,
                                  lc_command_encoder *encoder,
                                  void *user) {
    (void)user;
    return lr_vis_record_hiz(renderer, encoder);
}

static lr_result lr_vis_graph_cull(lr_renderer *renderer,
                                   lc_command_encoder *encoder,
                                   void *user) {
    uint32_t gi = (uint32_t)(uintptr_t)user;

    return lr_vis_record_cull(renderer, encoder, gi,
                              renderer->vis_prepared_flight);
}

static lr_result lr_vis_graph_fin(lr_renderer *renderer,
                                  lc_command_encoder *encoder,
                                  void *user) {
    uint32_t gi = (uint32_t)(uintptr_t)user;

    return lr_vis_record_fin(renderer, encoder, gi,
                             renderer->vis_prepared_flight);
}

/* Scheduling marker for the externally recorded main draw (the
 * HDR pass records draws after graph execution; this node keeps
 * the producer edge visible in dumps/diagnostics). */
static lr_result lr_vis_graph_main(lr_renderer *renderer,
                                   lc_command_encoder *encoder,
                                   void *user) {
    (void)renderer;
    (void)encoder;
    (void)user;
    return LR_SUCCESS;
}

/* Topology key: anything that changes pass/resource identity
 * (per-frame counts ride in push data, never in topology).
 * Buffer HANDLEs enter by stable resource ID, never by pointer:
 * wrapper addresses recycle after destroy/create (Phase 16 ABA
 * class), so a pointer key could alias a dead schedule onto live
 * buffers. IDs are never reused: any replacement rebuilds. */
static uint64_t lr_vis_graph_key(lr_renderer *renderer,
                                 uint32_t flight) {
    uint64_t key = 1469598103934665603ull;
    uint32_t i;

#define LR_VIS_MIX(v)                                   \
    do {                                                \
        key ^= (uint64_t)(uintptr_t)(v);                \
        key *= 1099511628211ull;                        \
    } while (0)

#define LR_VIS_MIX_ID(h)                                \
    do {                                                \
        key ^= (uint64_t)(h);                           \
        key *= 1099511628211ull;                        \
    } while (0)

    LR_VIS_MIX(flight);
    LR_VIS_MIX(renderer->vis_width);
    LR_VIS_MIX(renderer->vis_height);
    LR_VIS_MIX(renderer->vis_group_count);
    LR_VIS_MIX(renderer->vis_settings.hiz_enabled);
    LR_VIS_MIX(renderer->vis_settings.lod_enabled);
    LR_VIS_MIX_ID(
        lc_image_get_resource_id(renderer->hdr_depth_image));
    LR_VIS_MIX_ID(lc_image_get_resource_id(lr_hiz_image(renderer)));
    LR_VIS_MIX_ID(
        lc_buffer_get_resource_id(renderer->vis_params_buffer));
    for (i = 0; i < renderer->vis_group_count; i++) {
        const lr_vis_group *group = &renderer->vis_groups[i];
        const lr_vis_flight *fl = NULL;

        LR_VIS_MIX(group->capacity);
        LR_VIS_MIX(group->mesh != NULL ? group->mesh->lod_count
                                       : 0);
        LR_VIS_MIX_ID(
            lc_buffer_get_resource_id(group->instances));
        if (flight < group->flights_owned) {
            fl = &group->flights[flight];
        }
        if (fl != NULL && fl->initialized) {
            LR_VIS_MIX_ID(lc_buffer_get_resource_id(fl->visible));
            LR_VIS_MIX_ID(
                lc_buffer_get_resource_id(fl->lod_counters));
            LR_VIS_MIX_ID(lc_buffer_get_resource_id(fl->indirect));
            LR_VIS_MIX_ID(lc_buffer_get_resource_id(fl->count));
            LR_VIS_MIX_ID(lc_buffer_get_resource_id(fl->history));
            LR_VIS_MIX_ID(
                lc_buffer_get_resource_id(fl->history_ids));
            LR_VIS_MIX_ID(lc_buffer_get_resource_id(fl->stats));
        } else {
            LR_VIS_MIX(flight);
        }
    }
#undef LR_VIS_MIX
#undef LR_VIS_MIX_ID
    return key;
}

/* (Re)build the visibility schedule when topology changed;
 * otherwise reuse the compiled order (transients persist). */
static lr_result lr_vis_build_graph(lr_renderer *renderer,
                                    uint32_t flight) {
    lr_render_graph *graph = NULL;
    lr_graph_resource *depth = NULL;
    lr_graph_resource *hiz = NULL;
    lr_graph_resource *params = NULL;
    lr_graph_resource **visible = NULL;
    lr_graph_resource **counters = NULL;
    lr_graph_resource **indirect = NULL;
    lr_graph_resource **counts = NULL;
    lr_graph_pass *hiz_pass = NULL;
    lr_graph_pass *cull_pass = NULL;
    lr_graph_pass *fin_pass = NULL;
    lr_graph_pass *main_pass = NULL;
    uint64_t key;
    uint32_t i;

    key = lr_vis_graph_key(renderer, flight);
    if (renderer->vis_graph != NULL &&
        renderer->vis_graph_built && key == renderer->vis_graph_topology) {
        return LR_SUCCESS; /* compiled order reused */
    }
    if (renderer->vis_graph != NULL) {
        lr_render_graph_destroy(renderer->vis_graph);
        renderer->vis_graph = NULL;
        renderer->vis_graph_built = 0;
    }
    if (lr_render_graph_create(renderer, &graph) != LR_SUCCESS) {
        return LR_ERROR_OUT_OF_MEMORY;
    }
    depth = lr_graph_import_image(graph, "scene_depth",
                                  renderer->hdr_depth_image);
    hiz = lr_graph_import_image(graph, "hiz",
                                lr_hiz_image(renderer));
    params = lr_graph_import_buffer(graph, "vis_params",
                                    renderer->vis_params_buffer);
    if (depth == NULL || hiz == NULL || params == NULL) {
        lr_render_graph_destroy(graph);
        return LR_ERROR_RENDER;
    }
    visible = (lr_graph_resource **)calloc(
        renderer->vis_group_count + 1, sizeof(*visible));
    counters = (lr_graph_resource **)calloc(
        renderer->vis_group_count + 1, sizeof(*counters));
    indirect = (lr_graph_resource **)calloc(
        renderer->vis_group_count + 1, sizeof(*indirect));
    counts = (lr_graph_resource **)calloc(
        renderer->vis_group_count + 1, sizeof(*counts));
    if (visible == NULL || counters == NULL || indirect == NULL ||
        counts == NULL) {
        free(visible);
        free(counters);
        free(indirect);
        free(counts);
        lr_render_graph_destroy(graph);
        return LR_ERROR_OUT_OF_MEMORY;
    }
    hiz_pass = lr_graph_add_pass(graph, "hiz_gen",
                                 LR_GRAPH_PASS_COMPUTE,
                                 lr_vis_graph_hiz, NULL);
    if (hiz_pass == NULL) {
        free(visible);
        free(counters);
        free(indirect);
        free(counts);
        lr_render_graph_destroy(graph);
        return LR_ERROR_RENDER;
    }
    lr_graph_pass_read(hiz_pass, depth, LR_GRAPH_USE_SAMPLED_READ);
    lr_graph_pass_write(hiz_pass, hiz, LR_GRAPH_USE_STORAGE_WRITE);
    for (i = 0; i < renderer->vis_group_count; i++) {
        lr_vis_group *group = &renderer->vis_groups[i];
        lr_vis_flight *fl;
        char name[64];

        if (group->count == 0 || flight >= group->flights_owned) {
            continue;
        }
        fl = &group->flights[flight];
        if (!fl->initialized) {
            continue;
        }
        snprintf(name, sizeof(name), "g%u_visible", i);
        visible[i] = lr_graph_import_buffer(graph, name,
                                            fl->visible);
        snprintf(name, sizeof(name), "g%u_counters", i);
        counters[i] = lr_graph_import_buffer(graph, name,
                                             fl->lod_counters);
        snprintf(name, sizeof(name), "g%u_indirect", i);
        indirect[i] = lr_graph_import_buffer(graph, name,
                                             fl->indirect);
        snprintf(name, sizeof(name), "g%u_count", i);
        counts[i] =
            lr_graph_import_buffer(graph, name, fl->count);
        if (visible[i] == NULL || counters[i] == NULL ||
            indirect[i] == NULL || counts[i] == NULL) {
            free(visible);
            free(counters);
            free(indirect);
            free(counts);
            lr_render_graph_destroy(graph);
            return LR_ERROR_RENDER;
        }
        snprintf(name, sizeof(name), "vis_cull_g%u", i);
        cull_pass = lr_graph_add_pass(graph, name,
                                      LR_GRAPH_PASS_COMPUTE,
                                      lr_vis_graph_cull,
                                      (void *)(uintptr_t)i);
        snprintf(name, sizeof(name), "vis_fin_g%u", i);
        fin_pass = lr_graph_add_pass(graph, name,
                                     LR_GRAPH_PASS_COMPUTE,
                                     lr_vis_graph_fin,
                                     (void *)(uintptr_t)i);
        if (cull_pass == NULL || fin_pass == NULL) {
            free(visible);
            free(counters);
            free(indirect);
            free(counts);
            lr_render_graph_destroy(graph);
            return LR_ERROR_RENDER;
        }
        lr_graph_pass_read(cull_pass, hiz, LR_GRAPH_USE_SAMPLED_READ);
        lr_graph_pass_read(cull_pass, params,
                           LR_GRAPH_USE_STORAGE_READ);
        lr_graph_pass_write(cull_pass, visible[i],
                            LR_GRAPH_USE_STORAGE_WRITE);
        lr_graph_pass_write(cull_pass, counters[i],
                            LR_GRAPH_USE_STORAGE_WRITE);
        lr_graph_pass_read(fin_pass, counters[i],
                           LR_GRAPH_USE_STORAGE_READ);
        lr_graph_pass_read(fin_pass, visible[i],
                           LR_GRAPH_USE_STORAGE_READ);
        lr_graph_pass_write(fin_pass, indirect[i],
                            LR_GRAPH_USE_STORAGE_WRITE);
        lr_graph_pass_write(fin_pass, counts[i],
                            LR_GRAPH_USE_STORAGE_WRITE);
    }
    main_pass = lr_graph_add_pass(graph, "main_draw",
                                  LR_GRAPH_PASS_GRAPHICS,
                                  lr_vis_graph_main, NULL);
    if (main_pass == NULL) {
        free(visible);
        free(counters);
        free(indirect);
        free(counts);
        lr_render_graph_destroy(graph);
        return LR_ERROR_RENDER;
    }
    for (i = 0; i < renderer->vis_group_count; i++) {
        if (visible[i] != NULL) {
            lr_graph_pass_read(main_pass, indirect[i],
                               LR_GRAPH_USE_INDIRECT_READ);
            lr_graph_pass_read(main_pass, counts[i],
                               LR_GRAPH_USE_INDIRECT_READ);
        }
    }
    free(visible);
    free(counters);
    free(indirect);
    free(counts);
    if (lr_render_graph_compile(graph) != LR_SUCCESS) {
        lr_render_graph_destroy(graph);
        return LR_ERROR_RENDER;
    }
    renderer->vis_graph = graph;
    renderer->vis_graph_built = 1;
    renderer->vis_graph_topology = key;
    return LR_SUCCESS;
}

lr_render_graph *lr_renderer_borrow_visibility_graph(
    lr_renderer *renderer) {
    if (renderer == NULL) {
        return NULL;
    }
    return renderer->vis_graph;
}

/* Extended prepare (outside any pass). */
lr_result lr_vis_prepare(lr_renderer *renderer,
                         lc_command_encoder *encoder) {
    uint64_t t0 = 0;
    uint32_t flight = 0;
    uint32_t flight_count = 0;
    uint32_t i;
    uint32_t hw;
    uint32_t hh;
    lr_vis_params params;

    if (renderer == NULL || encoder == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!renderer->frame_open) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->render_mode != LR_RENDER_MODE_GPU_DRIVEN ||
        !renderer->vis_settings.enabled) {
        return LR_SUCCESS;
    }
    if (renderer->vis_prepared_frame == renderer->frame_number) {
        return LR_SUCCESS;
    }
    if (lr_vis_ensure_shared(renderer) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_get_flight_slot(encoder, &flight,
                                   &flight_count) != LC_SUCCESS) {
        return LR_SUCCESS;
    }
    if (flight >= LR_GPU_MAX_FLIGHTS || flight_count == 0 ||
        flight_count > LR_GPU_MAX_FLIGHTS) {
        return LR_SUCCESS;
    }
    t0 = lr_perf_now();
    hw = renderer->hdr_width;
    hh = renderer->hdr_height;
    if (hw == 0 || hh == 0) {
        return LR_ERROR_RENDER;
    }
    /* Extent change invalidates the pyramid AND the previous
     * matrices (screen-space history is meaningless). */
    if (renderer->vis_has_prev && (renderer->vis_width != hw ||
                                   renderer->vis_height != hh)) {
        renderer->vis_bypass = 1;
        renderer->vis_has_prev = 0;
    }
    /* A replaced pyramid drops every view the cull sets
     * reference (reduce sets die inside hiz_ensure): mark
     * cull/finalize sets stale. The FULL-CHAIN sample view also
     * dies with the pyramid (lr_hiz_destroy destroys it), so the
     * cull set — which binds it at slot 5 — must be rewritten,
     * not just re-recorded. */
    if (renderer->hiz == NULL || lr_hiz_width(renderer) != hw ||
        lr_hiz_height(renderer) != hh) {
        uint32_t g;

        for (g = 0; g < renderer->vis_group_count; g++) {
            lr_vis_group *group = &renderer->vis_groups[g];
            uint32_t f;

            for (f = 0; f < group->flights_owned; f++) {
                group->flights[f].sets_ready = 0;
            }
        }
    }
    renderer->vis_width = hw;
    renderer->vis_height = hh;
    if (lr_hiz_ensure(renderer, hw, hh) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* The pyramid is only trustworthy when the previous frame
     * stored depth for it (first enable, or a legacy/off gap
     * since, bypasses once while this frame stores). */
    if (!renderer->vis_hiz_was_enabled) {
        renderer->vis_bypass = 1;
    }
    memset(&renderer->vis_stats, 0, sizeof(renderer->vis_stats));
    lr_vis_prune_dead_groups(renderer);
    for (i = 0; i < renderer->vis_group_count; i++) {
        renderer->vis_groups[i].count = 0;
    }
    /* Group PBR items (same key discipline as the legacy path).
     * Phase 29: skinned items NEVER group (CPU loop draws them
     * with per-draw palettes, unculled by conservative policy). */
    for (i = 0; i < renderer->queued; i++) {
        lr_queued_item *item = &renderer->queue[i];
        lr_vis_group *group = NULL;
        lr_gpu_instance *dst = NULL;

        if (!lr_mesh_is_live(renderer, item->mesh) ||
            !lr_material_is_live(renderer, item->material)) {
            continue;
        }
        if (item->skinned) {
            continue;
        }
        if (item->material->type !=
            LR_MATERIAL_PBR_METALLIC_ROUGHNESS) {
            continue;
        }
        group = lr_vis_group_for(renderer, item->mesh, item->material,
                                 item->receives_shadow,
                                 item->mirrored, 1);
        if (group == NULL) {
            return LR_ERROR_RENDER;
        }
        dst = &group->cpu[group->count];
        memcpy(dst->model, item->matrix, sizeof(dst->model));
        dst->bounds[0] = item->mesh->bounds.center[0];
        dst->bounds[1] = item->mesh->bounds.center[1];
        dst->bounds[2] = item->mesh->bounds.center[2];
        dst->bounds[3] = item->mesh->bounds.radius;
        dst->mesh_index = 0;
        dst->object_id = i;
        dst->stable_id = item->instance_id;
        group->count++;
        renderer->vis_stats.total_instances++;
    }
    /* Pyramid first (previous-frame depth is already SHADER_READ;
     * bypass frames only adopt the readable state, never sample
     * untrusted depth). The graph schedules this node; the manual
     * path records it inline below. */
    /* Shared params (CPU-written, GPU read-only). */
    memset(&params, 0, sizeof(params));
    memcpy(params.view_proj_curr, renderer->vis_view_proj,
           sizeof(params.view_proj_curr));
    if (renderer->vis_has_prev) {
        memcpy(params.view_proj_prev, renderer->vis_prev_view_proj,
               sizeof(params.view_proj_prev));
    } else {
        memcpy(params.view_proj_prev, renderer->vis_view_proj,
               sizeof(params.view_proj_prev));
    }
    for (i = 0; i < 6; i++) {
        params.planes[i][0] = renderer->frustum_planes[i][0];
        params.planes[i][1] = renderer->frustum_planes[i][1];
        params.planes[i][2] = renderer->frustum_planes[i][2];
        params.planes[i][3] = renderer->frustum_planes[i][3];
    }
    params.cam_pos_prev[0] = renderer->vis_prev_cam_pos[0];
    params.cam_pos_prev[1] = renderer->vis_prev_cam_pos[1];
    params.cam_pos_prev[2] = renderer->vis_prev_cam_pos[2];
    params.view_w = hw;
    params.view_h = hh;
    params.hiz_w = lr_hiz_width(renderer);
    params.hiz_h = lr_hiz_height(renderer);
    params.hiz_mips = lr_hiz_levels(renderer);
    params.flags = 0;
    if (renderer->vis_settings.hiz_enabled &&
        !renderer->vis_bypass) {
        params.flags |= LR_VIS_FLAG_HIZ;
    }
    if (renderer->vis_bypass) {
        params.flags |= LR_VIS_FLAG_BYPASS;
    }
    if (renderer->vis_settings.lod_enabled) {
        params.flags |= LR_VIS_FLAG_LOD;
    }
    params.occlusion_bias = renderer->vis_settings.occlusion_depth_bias;
    params.lod_hysteresis =
        renderer->vis_settings.lod_hysteresis_margin;
    /* Single shared write per frame (per-group capacity rides in
     * push constants, never in this shared block). */
    if (lc_buffer_write(renderer->vis_params_buffer, 0, &params,
                        sizeof(params)) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (!renderer->vis_params_primed) {
        if (lc_encoder_transition_buffer(
                encoder, renderer->vis_params_buffer,
                LC_RESOURCE_STATE_STORAGE_READ) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        renderer->vis_params_primed = 1;
    }
    /* Per-group build: flight resources + uploads. GPU data
     * movement stays outside graph scheduling by design (async
     * transfer has its own ordering contract); the graph derives
     * the compute order below. */
    for (i = 0; i < renderer->vis_group_count; i++) {
        lr_vis_group *group = &renderer->vis_groups[i];
        lc_gpu_signal sig = { 0 };
        uint32_t zero8[8];
        uint32_t zero2[2];
        uint32_t lod_count;
        uint32_t l;

        if (group->count == 0) {
            continue;
        }
        if (!lr_vis_flight_ensure(renderer, group, flight)) {
            return LR_ERROR_RENDER;
        }
        lod_count = lr_vis_group_lod_count(renderer, group);
        if (lc_upload_buffer_async(
                renderer->device, group->instances, 0, group->cpu,
                (uint64_t)group->count * sizeof(lr_gpu_instance),
                &sig) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        for (l = 0; l < 8; l += 2) {
            zero8[l] = 0;
            zero8[l + 1] = group->capacity;
        }
        if (lc_upload_buffer_async(renderer->device,
                                   group->flights[flight].lod_counters,
                                   0, zero8, sizeof(zero8),
                                   &sig) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        zero2[0] = 0;
        zero2[1] = 0;
        if (lc_upload_buffer_async(renderer->device,
                                   group->flights[flight].stats, 0,
                                   zero2, sizeof(zero2),
                                   &sig) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        renderer->vis_stats.indirect_commands += lod_count;
        /* One counted draw per LOD level on both paths (native
         * skips empties on the GPU; fallback no-ops them). */
        renderer->vis_stats.indirect_draw_calls += lod_count;
    }
    if (renderer->vis_settings.graph_enabled) {
        /* The flight slot is live from here (graph callbacks
         * resolve it for per-flight resources). */
        renderer->vis_prepared_flight = flight;
        if (lr_vis_build_graph(renderer, flight) != LR_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        if (lr_render_graph_execute(renderer->vis_graph, encoder) !=
            LR_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    } else {
        if (lr_vis_record_hiz(renderer, encoder) != LR_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        for (i = 0; i < renderer->vis_group_count; i++) {
            if (renderer->vis_groups[i].count == 0) {
                continue;
            }
            if (lr_vis_record_cull(renderer, encoder, i, flight) !=
                LR_SUCCESS) {
                return LR_ERROR_RENDER;
            }
            if (lr_vis_record_fin(renderer, encoder, i, flight) !=
                LR_SUCCESS) {
                return LR_ERROR_RENDER;
            }
        }
    }
    /* Previous-frame history advances once the frame's visibility
     * work is recorded (queue order keeps GPU execution sane). */
    memcpy(renderer->vis_prev_view_proj, renderer->vis_view_proj,
           sizeof(renderer->vis_prev_view_proj));
    memcpy(renderer->vis_prev_cam_pos, renderer->vis_cam_pos,
           sizeof(renderer->vis_prev_cam_pos));
    renderer->vis_has_prev = 1;
    renderer->vis_hiz_was_enabled =
        renderer->vis_settings.hiz_enabled ? 1 : 0;
    renderer->vis_prepared_frame = renderer->frame_number;
    renderer->vis_prepared_flight = flight;
    renderer->vis_stats.cpu_prepare_ms = lr_perf_to_ms(
        lr_perf_now() - t0, renderer->perf_freq);
    return LR_SUCCESS;
}

/* Extended draws (inside an open pass): one counted draw per
 * active (group, LOD) with that LOD's index buffer. */
lr_result lr_vis_record_draws(lr_renderer *renderer,
                              lc_command_encoder *encoder,
                              lc_render_target *target,
                              const lc_render_target_desc *signature) {
    uint32_t flight = 0;
    uint32_t flight_count = 0;
    uint32_t i;
    lc_pipeline *bound_pipeline = NULL;
    lr_material *bound_material = NULL;
    lc_buffer *bound_vertex = NULL;
    lc_buffer *bound_index = NULL;
    const lr_environment *bound_env = NULL;
    uint64_t bound_env_epoch = 0;
    int env_bound = 0;
    lc_compute_capabilities caps;
    int use_count;

    if (renderer == NULL || encoder == NULL || target == NULL ||
        signature == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->render_mode != LR_RENDER_MODE_GPU_DRIVEN ||
        !renderer->vis_settings.enabled) {
        return LR_SUCCESS;
    }
    if (renderer->vis_prepared_frame != renderer->frame_number) {
        return LR_SUCCESS;
    }
    if (lc_encoder_get_flight_slot(encoder, &flight,
                                   &flight_count) != LC_SUCCESS) {
        return LR_SUCCESS;
    }
    if (flight >= LR_GPU_MAX_FLIGHTS) {
        return LR_SUCCESS;
    }
    memset(&caps, 0, sizeof(caps));
    lc_device_get_compute_capabilities(renderer->device, &caps);
    use_count = (renderer->vis_settings.indirect_count_enabled != 0 &&
                 caps.indirect_count != 0);
    for (i = 0; i < renderer->vis_group_count; i++) {
        lr_vis_group *group = &renderer->vis_groups[i];
        lr_vis_flight *fl = NULL;
        lc_pipeline *pipeline = NULL;
        lr_result res;
        lc_result cr;
        lr_pbr_push push;
        uint32_t lod_count;
        uint32_t l;
        int want_cull;

        if (group->count == 0 || flight >= group->flights_owned) {
            continue;
        }
        fl = &group->flights[flight];
        if (!fl->initialized) {
            continue;
        }
        lod_count = group->mesh->lod_count;
        if (lod_count < 1) {
            lod_count = 1;
        }
        if (lod_count > LR_VIS_MAX_LOD_SEGMENTS) {
            lod_count = LR_VIS_MAX_LOD_SEGMENTS;
        }
        if (!renderer->vis_settings.lod_enabled) {
            lod_count = 1;
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
            return res;
        }
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
            bound_vertex = NULL;
            bound_index = NULL;
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
                                         fl->vert_set);
        if (cr != LC_SUCCESS) {
            return lr_map_result(cr);
        }
        if (group->mesh->vertex_buffer != bound_vertex) {
            cr = lc_encoder_bind_vertex_buffer(
                encoder, 0, group->mesh->vertex_buffer, 0);
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            bound_vertex = group->mesh->vertex_buffer;
            bound_index = NULL;
        }
        /* Group flag block (identity model, shadow-receive flag);
         * instances carry their own transforms. */
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
        /* One counted draw per LOD level with that level's index
         * buffer. firstInstance (written by finalize) lands each
         * draw in its own visible segment, so the instanced vertex
         * shader stays LOD-unaware. */
        for (l = 0; l < lod_count; l++) {
            lc_buffer *index_buffer = lr_mesh_lod_buffer(group->mesh,
                                                         l);

            if (index_buffer == NULL) {
                return LR_ERROR_RENDER;
            }
            if (index_buffer != bound_index) {
                cr = lc_encoder_bind_index_buffer(
                    encoder, index_buffer, 0, LC_INDEX_UINT32);
                if (cr != LC_SUCCESS) {
                    return lr_map_result(cr);
                }
                bound_index = index_buffer;
            }
            if (use_count) {
                cr = lc_encoder_draw_indexed_indirect_count(
                    encoder, fl->indirect, (uint64_t)l * 20u,
                    fl->count, (uint64_t)l * 4u, 1, 20);
            } else {
                cr = lc_encoder_draw_indexed_indirect(
                    encoder, fl->indirect, (uint64_t)l * 20u, 1,
                    20);
            }
            if (cr != LC_SUCCESS) {
                return lr_map_result(cr);
            }
            renderer->stats.draw_calls++;
            renderer->stats.pbr_draw_calls++;
        }
    }
    return LR_SUCCESS;
}

lr_result lr_renderer_update_visibility_stats(lr_renderer *renderer) {
    uint64_t visible = 0;
    uint64_t frustum_rejected = 0;
    uint64_t occlusion_rejected = 0;
    uint64_t tris = 0;
    uint64_t draws = 0;
    uint64_t overflows = 0;
    uint32_t i;
    lc_compute_capabilities caps;
    int native_count;

    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->render_mode != LR_RENDER_MODE_GPU_DRIVEN ||
        !renderer->vis_settings.enabled) {
        return LR_SUCCESS;
    }
    memset(&caps, 0, sizeof(caps));
    lc_device_get_compute_capabilities(renderer->device, &caps);
    native_count =
        (renderer->vis_settings.indirect_count_enabled != 0 &&
         caps.indirect_count != 0);
    /* Last prepared flight slot only (deterministic: other slots
     * hold other frames' counters). */
    for (i = 0; i < renderer->vis_group_count; i++) {
        lr_vis_group *group = &renderer->vis_groups[i];
        lr_vis_flight *fl = NULL;
        uint32_t counters[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };
        uint32_t rejects[2] = { 0, 0 };
        uint32_t counts[4] = { 0, 0, 0, 0 };
        uint32_t lod_count;
        uint32_t l;

        if (renderer->vis_prepared_flight >= group->flights_owned) {
            continue;
        }
        fl = &group->flights[renderer->vis_prepared_flight];
        if (!fl->initialized || group->count == 0) {
            continue;
        }
        lod_count = group->mesh->lod_count;
        if (lod_count < 1) {
            lod_count = 1;
        }
        if (lod_count > LR_VIS_MAX_LOD_SEGMENTS) {
            lod_count = LR_VIS_MAX_LOD_SEGMENTS;
        }
        if (!renderer->vis_settings.lod_enabled) {
            lod_count = 1;
        }
        if (lc_buffer_read(fl->lod_counters, 0, counters,
                           sizeof(counters)) != LC_SUCCESS ||
            lc_buffer_read(fl->stats, 0, rejects,
                           sizeof(rejects)) != LC_SUCCESS ||
            lc_buffer_read(fl->count, 0, counts,
                           sizeof(counts)) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        frustum_rejected += rejects[0];
        occlusion_rejected += rejects[1];
        for (l = 0; l < lod_count; l++) {
            uint32_t got = counters[l * 2u];

            if (got > group->capacity) {
                overflows++;
                got = group->capacity;
            }
            visible += got;
            renderer->vis_stats.lod_visible[l] += got;
            tris += (uint64_t)got *
                    (lr_mesh_lod_index_count(group->mesh, l) / 3u);
            /* Executed draws: native consumes the GPU count,
             * fallback draws every level (empties no-op). */
            if (native_count) {
                draws += (counts[l] != 0) ? 1u : 0u;
            } else {
                draws += 1u;
            }
        }
    }
    renderer->vis_stats.visible = visible;
    renderer->vis_stats.frustum_rejected = frustum_rejected;
    renderer->vis_stats.occlusion_rejected = occlusion_rejected;
    renderer->vis_stats.triangles_submitted = tris;
    renderer->vis_stats.indirect_draw_calls = draws;
    renderer->stats.triangles += (uint32_t)tris;
    renderer->stats.visible_objects = (uint32_t)visible;
#if !defined(NDEBUG)
    if (overflows > 0) {
        fprintf(stderr,
                "[lumac] visibility overflow: %llu LOD segment(s) "
                "exceeded instance capacity (count clamped)\n",
                (unsigned long long)overflows);
    }
#endif
    return LR_SUCCESS;
}