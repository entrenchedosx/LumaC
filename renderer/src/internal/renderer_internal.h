#ifndef LUMA_RENDERER_INTERNAL_H
#define LUMA_RENDERER_INTERNAL_H

/*
 * Renderer-private state (never public, never includes LumaC
 * internals or backend headers � public lumac.h only).
 */

#include "luma_renderer/luma_renderer.h"

/* Pipeline cache: one pipeline per (target signature, material
 * type, cull mode). Bounded; stable once warm. */
#define LR_PIPELINE_CACHE_MAX 16

typedef struct lr_cached_pipeline {
    lc_render_target_desc signature; /* structural, extent ignored */
    lr_material_type material_type;
    lc_cull_mode cull_mode;
    lc_pipeline *pipeline;
} lr_cached_pipeline;

/* One PBR light in GPU layout (5x vec4 stride, std140-safe). */
typedef struct lr_light_gpu {
    float color_intensity[4]; /* rgb + intensity */
    float pos_range[4];       /* xyz position, w range */
    float dir_kind[4];        /* xyz travel dir, w kind (0/1/2) */
    float spot_angles[4];     /* cosInner, cosOuter, 0, 0 */
    float shadow_info[4];     /* enabled01, slot(-1 none), 0, 0 */
} lr_light_gpu;

/* Light uniform block mirror (ambient + count + fixed array). */
typedef struct lr_lights_gpu {
    float ambient[4];
    uint32_t light_count;
    uint32_t pad[3];
    lr_light_gpu lights[LR_MAX_LIGHTS];
} lr_lights_gpu;

/* One shadow slot in GPU layout (view-proj + sampling params). */
typedef struct lr_shadow_slot_gpu {
    float view_proj[16];
    float params[4]; /* texelSize, normalBias, constBias, 0 */
} lr_shadow_slot_gpu;

/* Shadow metadata block mirror (slot count + fixed slots). */
typedef struct lr_shadows_gpu {
    uint32_t slot_count;
    uint32_t pad[3];
    lr_shadow_slot_gpu slots[LR_MAX_SHADOWS];
} lr_shadows_gpu;

/* PBR push block: model + normal matrix + receive flags (116B). */
typedef struct lr_pbr_push {
    float model[16];
    float normal_matrix[12];
    uint32_t flags; /* bit0: receives shadows */
} lr_pbr_push;

/* Shadow depth push block: model matrix only (64B). */
typedef struct lr_shadow_push {
    float model[16];
} lr_shadow_push;

/* One shadow depth-pass VP (64B, mapped write per light). */
typedef struct lr_depth_vp_gpu {
    float view_proj[16];
} lr_depth_vp_gpu;

/* One PBR material parameter block mirror (64 bytes, std140-safe:
 * flags sits at offset 44 and the vec3 pad at 48 needs no gap). */
typedef struct lr_material_gpu {
    float base_color[4];
    float emissive[3];
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    uint32_t flags; /* bit0 double-sided, bits8-15 alpha mode */
    float pad[4];
} lr_material_gpu;

/* One queued submission (flattened at submit time). Shadow flags
 * ride along for the depth passes (opt-in, nonzero participates).
 * main_visible marks main-frustum survivors; culled entries stay
 * queued (up to max_objects) so off-screen casters still shadow. */
typedef struct lr_queued_item {
    lr_mesh *mesh;
    lr_material *material;
    float matrix[16];
    float sphere_center[3];
    float sphere_radius;
    int casts_shadow;
    int receives_shadow;
    int main_visible;
} lr_queued_item;

/* One submitted light (public copy + shadow assignment). Slot is
 * the shadow-map index or -1 when unshadowed/unassigned. */
typedef struct lr_submitted_light {
    lr_light pub;
    lr_shadow_desc shadow;
    int shadow_slot;
} lr_submitted_light;

/* Renderer-owned shadow-map slot (resources persist across frames;
 * recreated only on resolution change). */
typedef struct lr_shadow_slot {
    lc_image *image;
    lc_image_view *view;
    lc_render_target *target;
    uint32_t resolution;
    lc_format format;
} lr_shadow_slot;

/* Environment GPU params mirror (one vec4, std140-safe). */
typedef struct lr_env_params_gpu {
    float intensity;      /* runtime multiplier, >= 0 */
    float rotation;       /* yaw radians */
    float prefilter_mips; /* mip count of the prefilter chain */
    float ibl_active;     /* 1 while an environment lights draws */
} lr_env_params_gpu;

/* Sky push block: inverse view-projection + camera + params (96B,
 * vertex+fragment visibility). */
typedef struct lr_sky_push {
    float inv_view_proj[16];
    float cam_pos[4];
    float params[4]; /* intensity, rotation, 0, 0 */
} lr_sky_push;

/* Preprocessing push block: face index + roughness (8B). */
typedef struct lr_post_push {
    int32_t face; /* 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z */
    float roughness;
} lr_post_push;

/* Tonemap push block: exposure multiplier + operator (8B). */
typedef struct lr_tonemap_push {
    float ev_mult;
    int32_t op; /* matches lr_tonemap_operator */
} lr_tonemap_push;

/* Prefilter mip depth (64 base -> 7 levels, fixed by design). */
#define LR_ENV_PREFILTER_MAX_MIPS 7

/* Base-cube mip depth (128 -> 1, all rendered analytically). */
#define LR_ENV_CUBE_MAX_MIPS 8

/* Renderer-owned environment (preprocessed IBL). Derived images
 * have FIXED sizes; a source change rebuilds them (rare) and the
 * frame set is rewritten at build end, so no binding ever dangles.
 * The source texture/sampler stay borrowed (must outlive the env). */
struct lr_environment {
    lr_renderer *renderer; /* owner back-pointer */
    lc_image_view *source_view; /* borrowed; must outlive the env */
    lc_sampler *source_sampler; /* borrowed; must outlive the env */
    /* Stable source identity (ABA hardening): a destroyed + recreated
     * source may reuse the same wrapper address, so source changes
     * compare IDs as well as pointers. */
    lc_resource_id source_id;
    lc_resource_id sampler_id;
    float intensity;
    float rotation;
    lc_image *cube_image; /* base env cube (all mips rendered) */
    lc_image_view *cube_view;
    lc_image *irradiance_image;
    lc_image_view *irradiance_view;
    lc_image *prefilter_image;
    lc_image_view *prefilter_view;
    uint32_t prefilter_mips;
    lc_image_view *cube_face_views[LR_ENV_CUBE_MAX_MIPS][6];
    lc_image_view *irradiance_face_views[6];
    lc_image_view *prefilter_face_views[LR_ENV_PREFILTER_MAX_MIPS][6];
    lc_render_target *cube_targets[LR_ENV_CUBE_MAX_MIPS][6];
    lc_render_target *irradiance_targets[6];
    lc_render_target *prefilter_targets[LR_ENV_PREFILTER_MAX_MIPS][6];
    lc_binding_set *frame_set; /* PBR slot 2 (written once at build) */
    lc_binding_set *eq_set;   /* equirect source (written once) */
    lc_binding_set *irr_set;  /* base cube for irradiance (once) */
    lc_binding_set *pref_set; /* base cube for prefilter (once) */
    int ready;
    int dirty; /* source changed since last build */
    uint64_t source_epoch;   /* bumped per (re)build */
    uint64_t params_version; /* bumped per intensity/rotation change */
    uint32_t rebuilds;       /* lifetime (re)build count */
    uint32_t last_build_ms;  /* wall time of the last build */
    lr_environment *next;
    lr_environment *prev;
};

/* GPU-driven submission (Phase 21): groups of (mesh, material,
 * shadow-flag) share one instance buffer and one indirect command;
 * per-flight visible/counter/indirect buffers plus descriptor sets
 * rotate with the frame slot (no cross-frame races, no per-object
 * allocation). */
#define LR_GPU_MAX_FLIGHTS 8
#define LR_GPU_MAX_GROUPS 256

typedef struct lr_gpu_flight_res {
    lc_buffer *visible;
    lc_buffer *counter;
    lc_buffer *indirect;
    lc_binding_set *cull_set;
    lc_binding_set *finalize_set;
    lc_binding_set *inst_set;
    int initialized;
} lr_gpu_flight_res;

typedef struct lr_gpu_group {
    lr_mesh *mesh;
    lr_material *material;
    int receives_shadow;
    uint32_t capacity;
    uint32_t count;
    lr_gpu_instance *cpu; /* malloc'd staging copy, capacity */
    lc_buffer *instances; /* shared across flights */
    uint32_t flights_owned;
    lr_gpu_flight_res flights[LR_GPU_MAX_FLIGHTS];
} lr_gpu_group;

struct lr_renderer {
    lc_device *device; /* borrowed; must outlive the renderer */
    lc_render_target_desc primary; /* validated primary signature */
    uint32_t max_objects;

    /* Owned GPU resources (created at lr_renderer_create). */
    lc_buffer *camera_buffer;
    void *camera_mapped;
    lc_binding_layout *layout;     /* unlit: camera + base + sampler */
    lc_binding_layout *pbr_layout; /* PBR: camera + lights + material
                                    * + 5 maps + sampler */
    lc_image *fallback_image;      /* 1x1 white (base color) */
    lc_image_view *fallback_view;
    lc_image *fallback_mr_image;   /* 1x1 neutral metal/rough (G=B=1) */
    lc_image_view *fallback_mr_view;
    lc_image *fallback_normal_image; /* 1x1 flat tangent-space normal */
    lc_image_view *fallback_normal_view;
    lc_image *fallback_occlusion_image; /* 1x1 white */
    lc_image_view *fallback_occlusion_view;
    lc_image *fallback_emissive_image; /* 1x1 white (factor-only
                                        * emission works) */
    lc_image_view *fallback_emissive_view;
    lc_sampler *default_sampler;

    /* Owned shaders (modules freed after pipelines exist). */
    lc_shader *vertex_shader;
    lc_shader *fragment_shader;
    lc_shader *pbr_vertex_shader;
    lc_shader *pbr_fragment_shader;

    /* Shared light uniform buffer (persistently mapped; contents
     * updated per render, descriptor sets never rebuilt). */
    lc_buffer *light_buffer;
    void *light_mapped;
    lr_submitted_light lights[LR_MAX_LIGHTS];
    uint32_t light_count;
    uint32_t shadow_assigned;
    float ambient_light[3];

    /* Shadow state (renderer-owned, reused across frames). */
    lr_shadow_slot slots[LR_MAX_SHADOWS];
    lc_binding_layout *shadow_layout; /* metadata + 4 maps + sampler */
    lc_binding_set *shadow_set;       /* frame set (rare updates) */
    lc_buffer *shadow_meta_buffer;
    void *shadow_meta_mapped;
    lc_binding_layout *depth_layout; /* per-pass VP only */
    lc_binding_set *depth_set;       /* binds the VP buffer, reused */
    lc_buffer *depth_vp_buffer;
    void *depth_vp_mapped;
    lc_shader *depth_vertex_shader;
    int shadows_prepared;
    uint32_t shadow_slot_active[LR_MAX_SHADOWS];
    /* Views currently bound in the frame set (update on change).
     * Compared by stable resource ID, never by pointer: wrapper
     * addresses recycle, so a recreated view can alias a freed one
     * (Phase 16 ABA class). 0 means "nothing bound". */
    lc_resource_id shadow_bound[LR_MAX_SHADOWS];
    /* CPU mirror of the metadata block (staged per light). */
    lr_shadows_gpu cpu_meta;

    /* Pipeline cache (unlit primary + PBR primary pre-built). */
    lr_cached_pipeline pipelines[LR_PIPELINE_CACHE_MAX];
    uint32_t pipeline_count;

    /* Depth-pipeline mini-cache (keyed by signature only; all shadow
     * targets share depth format/samples so this stays tiny). */
    lr_cached_pipeline depth_pipelines[4];
    uint32_t depth_pipeline_count;

    /* Environment/IBL state (Phase 17). The active environment is
     * borrowed (NULL = ambient fallback); derived resources are
     * per-environment, the BRDF LUT is renderer-global. */
    lc_binding_layout *env_layout;  /* PBR slot 2 */
    lc_binding_layout *post_layout; /* post passes: image + sampler */
    lc_buffer *env_params_buffer;   /* lr_env_params_gpu, mapped */
    void *env_params_mapped;
    lr_environment *environments; /* registry */
    lr_environment *active_env;   /* borrowed, NULL when off */
    uint64_t env_generation;      /* lifetime env build counter */
    uint32_t environment_rebuilds; /* lifetime total */
    /* Shared post shaders (modules freed after pipelines exist). */
    lc_shader *fulltri_vertex_shader;
    lc_shader *eq2cube_fragment_shader;
    lc_shader *irradiance_fragment_shader;
    lc_shader *prefilter_fragment_shader;
    lc_shader *brdf_fragment_shader;
    lc_shader *sky_vertex_shader;
    lc_shader *sky_fragment_shader;
    lc_shader *tonemap_fragment_shader;
    /* Post pipelines (lazy singletons, never per-frame). */
    lc_pipeline *eq2cube_pipeline;
    lc_pipeline *irradiance_pipeline;
    lc_pipeline *prefilter_pipeline;
    lc_pipeline *brdf_pipeline;
    lc_pipeline *sky_pipeline;
    /* NOTE: no shared mutable post set — one set updated across
     * passes would invalidate prior binds in the same recording
     * (VUID). Preprocessing sets live per stage on the environment
     * (write-once); sky and tonemap sets update per frame after the
     * prior frame drained (frame serialization makes that legal);
     * the BRDF pass uses the write-once post set below. */
    lc_binding_set *post_set; /* fallback bindings, written once */
    /* BRDF integration LUT (environment-independent, built once). */
    lc_image *brdf_image;
    lc_image_view *brdf_view;
    lc_render_target *brdf_target;
    int brdf_ready;
    /* Empty environment set (bound when no env is active). */
    lc_image *empty_cube_image;
    lc_image_view *empty_cube_view;
    lc_image *empty_brdf_image;
    lc_image_view *empty_brdf_view;
    lc_binding_set *empty_env_set;
    /* HDR scene target (auto-sized to the output extent). */
    lc_image *hdr_image;
    lc_image_view *hdr_view;
    lc_image *hdr_depth_image;
    lc_image_view *hdr_depth_view;
    lc_render_target *hdr_target;
    uint32_t hdr_width;
    uint32_t hdr_height;
    uint64_t hdr_generation;
    int hdr_has_scene; /* render_scene ran since begin */
    /* Output state. */
    float exposure_ev;
    lr_tonemap_operator tonemap_op;
    lc_binding_set *sky_set;     /* updated per render while active */
    lc_binding_set *tonemap_set; /* updated per output (HDR view) */
    /* Last values written into tonemap_set (identity-guarded skip:
     * rebinding the same set in one recording after an update
     * invalidates the command buffer, so a second output with
     * identical inputs binds without rewriting). */
    lc_resource_id tonemap_set_view_id;
    lc_resource_id tonemap_set_sampler_id;
    lr_cached_pipeline tonemap_pipelines[4];
    uint32_t tonemap_pipeline_count;

    /* Mesh/material registries (renderer-owned liveness). */
    lr_mesh *meshes;
    lr_material *materials;

    /* Per-frame state. */
    lr_camera camera; /* latched copy */
    int frame_open;
    lr_queued_item *queue;
    uint32_t queued;
    float frustum_planes[6][4]; /* normalized, inward-facing */
    lr_render_stats stats;
    /* Phase 21 GPU-driven submission. Groups persist across frames
     * (capacity high-water); per-flight resources rotate with the
     * frame slot. gpu_prepared_frame stamps the profile frame
     * number prepared (0 = none this frame). */
    lr_render_mode render_mode;
    lr_gpu_group *groups;
    uint32_t group_count;
    uint32_t group_capacity;
    uint32_t gpu_prepared_frame;
    uint32_t gpu_last_flight; /* slot prepared this frame */
    lr_gpu_driven_stats gpu_stats;
    int gpu_ready; /* shared pipelines/layouts/shaders created */
    lc_shader *cull_shader;
    lc_shader *finalize_shader;
    lc_shader *instanced_vertex_shader;
    lc_compute_pipeline *cull_pipeline;
    lc_compute_pipeline *finalize_pipeline;
    lc_binding_layout *cull_layout;
    lc_binding_layout *finalize_layout;
    lc_binding_layout *instanced_layout;
    lr_cached_pipeline instanced_pipelines[4];
    uint32_t instanced_pipeline_count;
    /* Phase 18: CPU recording profile + post chain. Timers use a
     * monotonic high-resolution clock (platform-local); all fields
     * reset every begin. */
    lr_frame_profile profile;
    uint32_t frame_number; /* 1-based, incremented by begin */
    uint64_t perf_freq;    /* ticks per second (0 = uninit) */
    uint64_t t_begin;      /* begin timestamp */
    uint64_t t_shadow_start;
    uint64_t t_shadow_end;
    uint64_t t_main_start;
    uint64_t t_main_end;
    uint64_t t_sky;        /* sky recording duration */
    uint64_t t_post;       /* post chain recording duration */
    uint64_t t_tonemap;    /* output recording duration */
    /* Post chain (Phase 18 foundation). Ping-pong intermediates are
     * extent-keyed (two slots for multiview); the chain head feeds
     * render_output. */
    lr_post_stage post_stage;
    float post_tint[3];
    lc_image *post_images[2][2]; /* [slot][ping/pong] RGBA16F */
    lc_image_view *post_views[2][2];
    lc_render_target *post_targets[2][2];
    uint32_t post_width[2];
    uint32_t post_height[2];
    int post_head; /* 0/1: which side holds the chain head */
    lc_image_view *post_head_view; /* NULL when chain empty */
    lc_shader *post_tint_fragment_shader;
    lc_pipeline *post_tint_pipelines[2]; /* per slot signature */
    lc_binding_set *post_stage_sets[2];  /* per slot, rewritten */
    uint64_t post_pipeline_epochs[2];
};

struct lr_mesh {
    lr_renderer *renderer; /* owner (borrowed back-pointer) */
    lc_buffer *vertex_buffer;
    lc_buffer *index_buffer;
    uint32_t vertex_count;
    uint32_t index_count;
    lr_bounds bounds;
    lr_mesh *next;
    lr_mesh *prev;
};

struct lr_material {
    lr_renderer *renderer;
    lc_binding_set *set;
    lr_material_type type;
    float color[4]; /* unlit only */
    /* PBR only (borrowed views/sampler; owned param buffer). */
    lc_buffer *param_buffer;
    float base_color[4];
    float metallic_factor;
    float roughness_factor;
    const lc_image_view *base_color_texture;
    const lc_image_view *metallic_roughness_texture;
    const lc_image_view *normal_texture;
    const lc_image_view *occlusion_texture;
    const lc_image_view *emissive_texture;
    const lc_sampler *sampler;
    float emissive_factor[3];
    float normal_scale;
    float occlusion_strength;
    int double_sided;
    lr_alpha_mode alpha_mode;
    lr_material *next;
    lr_material *prev;
};

/* Result mapping (lc_result -> lr_result). */
lr_result lr_map_result(lc_result res);
/* Renderer-local math (column-major, Y-up RH world, Vulkan NDC). */
void lr_mat4_identity(float *m);
void lr_mat4_multiply(float *out, const float *a, const float *b);
void lr_mat4_perspective(float *m, float fov_y, float aspect, float near_z,
                         float far_z);
void lr_mat4_look_at(float *m, const float eye[3], const float center[3],
                     const float up[3]);
void lr_mat4_translate_view(float *m, const float basis[9],
                            const float eye[3]);
/* General 4x4 inverse (adjugate/determinant); nonzero on success,
 * zero on singular input (out untouched on failure). */
int lr_mat4_inverse(const float m[16], float out_inv[16]);
/* Orthographic projection WITH the renderer Y-flip convention
 * (matching lr_mat4_perspective, so one clip->UV rule serves both):
 * maps [l,r]x[b,t]x[n,f] to clip, depth 0..1. */
void lr_mat4_ortho(float *m, float left, float right, float bottom,
                   float top, float near_z, float far_z);
int lr_vec3_normalize(const float in[3], float out[3]);
float lr_vec3_length(const float v[3]);

/* Frustum w/ normalized inward planes from a view-projection matrix;
 * sphere test returns nonzero when visible (touching counts). Array
 * parameters are deliberately non-const: pre-C23 ISO C rejects
 * implicit float(*)[4] -> const float(*)[4] conversion (-Wpedantic);
 * both functions are read-only over planes. */
void lr_frustum_from_viewproj(const float vp[16], float planes[6][4]);
int lr_frustum_test_sphere(float planes[6][4], const float center[3],
                           float radius);

/* Registry helpers. */
int lr_mesh_is_live(const lr_renderer *renderer, const lr_mesh *mesh);
int lr_material_is_live(const lr_renderer *renderer,
                        const lr_material *material);
void lr_mesh_list_add(lr_renderer *renderer, lr_mesh *mesh);
void lr_mesh_list_remove(lr_mesh *mesh);
void lr_material_list_add(lr_renderer *renderer, lr_material *material);
void lr_material_list_remove(lr_material *material);

/* Opaque sort for the queued items (material, then mesh). */
void lr_queue_sort(lr_queued_item *items, uint32_t count);

/* Pipeline cache: get-or-create the pipeline for (signature,
 * material type, cull mode). Returns lr_result; *out_pipeline
 * borrows the cache entry. */
lr_result lr_renderer_pipeline_for(lr_renderer *renderer,
                                   const lc_render_target_desc *signature,
                                   lr_material_type material_type,
                                   lc_cull_mode cull_mode,
                                   lc_pipeline **out_pipeline);

/* Depth-pipeline cache: get-or-create the depth-only pipeline for a
 * depth signature (fragment-less, model-only push, cull NONE). */
lr_result lr_renderer_depth_pipeline_for(
    lr_renderer *renderer, const lc_render_target_desc *signature,
    lc_pipeline **out_pipeline);

/* Shadow-camera math (all column-major; pure functions):
 * - frustum slice corners (world) from the latched camera VP at two
 *   NDC depths;
 * - directional fit: slice corners -> light-space AABB -> CENTERED
 *   ortho bounds + eye + view (eye-frame symmetric, so translation
 *   moves the slice rigidly and extents � hence the projection �
 *   stay put; that structural property, not grid snapping, is the
 *   stability mechanism; depth extends past the slice both ways to
 *   cover out-of-slice casters since ortho depth is linear/cheap);
 * - light view helper (look-at with degenerate-up fallback). */
void lr_shadow_frustum_corners(const float view_proj[16], float z_near_ndc,
                               float z_far_ndc, float out_corners[8][3]);
void lr_shadow_fit_directional(const float corners[8][3],
                               const float light_dir[3],
                               float out_bounds[6], float out_eye[3],
                               float out_view[16]);
void lr_shadow_light_view(const float eye[3], const float target[3],
                          float out_view[16]);

/* Normal matrix (inverse-transpose 3x3) from a column-major model
 * matrix for correct normals/tangents under non-uniform scale.
 * Returns nonzero on success (fails only on singular matrices). */
int lr_mat3_normal_from_mat4(const float m[16], float out_normal[12]);

/* Environment preprocessing + passes (environment.c). All record
 * into the caller's encoder with no open pass (passes open/close
 * per face); none recreate per frame. */
lr_result lr_environment_build(lr_environment *env,
                               lc_command_encoder *enc);
lr_result lr_environment_ensure_built(lr_environment *env,
                                      lc_command_encoder *enc);
lr_result lr_renderer_ensure_brdf(lr_renderer *renderer,
                                  lc_command_encoder *enc);
lr_result lr_renderer_ensure_hdr(lr_renderer *renderer, uint32_t width,
                                 uint32_t height);
lr_result lr_renderer_record_sky(lr_renderer *renderer,
                                 lc_command_encoder *enc);
lr_result lr_renderer_record_tonemap(lr_renderer *renderer,
                                     lc_command_encoder *enc,
                                     lc_render_target *target);
/* Refresh the mapped env-params block from the active environment
 * (infallible memcpy; the buffer itself never moves). */
void lr_environment_write_params(lr_renderer *renderer);
/* Renderer-owned env resources (layouts, params buffer, empty set,
 * HDR fields zeroed, exposure defaults). Create is cheap (no
 * compiles); destroy is NULL-safe. */
lr_result lr_renderer_create_env_resources(lr_renderer *renderer);
void lr_renderer_destroy_env_resources(lr_renderer *renderer);

/* Monotonic clock helpers (platform-local, Phase 18 profiling).
 * Ticks are opaque; to_ms converts with the renderer-cached
 * frequency. */
uint64_t lr_perf_now(void);
uint64_t lr_perf_frequency(void);
double lr_perf_to_ms(uint64_t ticks, uint64_t frequency);

/* Post-process chain (post.c): stage setters, extent-keyed
 * ping-pong intermediates, chain recording into render_scene,
 * chain-head routing for render_output/capture. */
lr_result lr_post_set_stage(lr_renderer *renderer, lr_post_stage stage);
lr_result lr_post_set_tint(lr_renderer *renderer, const float rgb[3]);
lr_result lr_post_ensure(lr_renderer *renderer, uint32_t slot,
                         uint32_t width, uint32_t height);
lr_result lr_post_record(lr_renderer *renderer, lc_command_encoder *enc,
                         uint32_t slot);
void lr_post_destroy(lr_renderer *renderer);
void lr_post_invalidate(lr_renderer *renderer);

/* GPU-driven submission (Phase 21, gpu_driven.c). Shared
 * resources, grouping, prepare/draw entry points (structs moved
 * above lr_renderer). */

/* Shared GPU-driven resources (created lazily, destroyed with the
 * renderer). Pipelines/layouts/shaders are renderer-global; groups
 * are per (mesh, material, shadow-flag). */
lr_result lr_gpu_ensure_shared(lr_renderer *renderer);
void lr_gpu_destroy_shared(lr_renderer *renderer);
/* Group queued PBR items, upload instances, run cull + finalize
 * dispatches (outside any pass). Idempotent per frame. */
lr_result lr_gpu_prepare(lr_renderer *renderer,
                         lc_command_encoder *encoder);
/* Draw prepared groups with indirect draws (inside an open pass).
 * PBR items NOT covered (unprepared/unlit) fall back to CPU draws
 * via the existing loop — callers skip PBR items themselves. */
lr_result lr_gpu_record_draws(lr_renderer *renderer,
                              lc_command_encoder *encoder,
                              lc_render_target *target,
                              const lc_render_target_desc *signature);
/* Drop groups whose mesh/material died. */
void lr_gpu_prune_dead_groups(lr_renderer *renderer);
/* Destroy every group and its GPU resources. */
void lr_gpu_destroy_groups(lr_renderer *renderer);
/* Instanced-PBR pipeline variant (own mini-cache, same signature
 * rules plus the instance slot). */
lr_result lr_renderer_instanced_pipeline_for(
    lr_renderer *renderer, const lc_render_target_desc *signature,
    lr_material_type material_type, lc_cull_mode cull_mode,
    lc_pipeline **out_pipeline);

#endif /* LUMA_RENDERER_INTERNAL_H */
