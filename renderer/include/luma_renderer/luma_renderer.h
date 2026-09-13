#ifndef LUMA_RENDERER_H
#define LUMA_RENDERER_H

/*
 * Luma Renderer (Phases 13-16): a small scene renderer on top of
 * PUBLIC LumaC only (lumac/lumac.h). LumaC must never depend on this
 * header. Phase 15 adds PBR metallic-roughness materials + lights
 * alongside the untouched unlit path; Phase 16 adds spot lights and
 * renderer-owned shadow maps (directional + spot) with PCF.
 *
 * Layering (strict):
 *   Luma Engine  may use Luma Renderer. Luma Renderer must never
 *   depend on Luma Engine.
 *   Luma Renderer may use LumaC. LumaC must never depend on
 *   Luma Renderer.
 *
 * Conventions (renderer-local, documented once):
 * - Column-major 4x4 matrices, meters, Y-up right-handed world space,
 *   camera looking along -Z in view space, Vulkan NDC (depth 0..1).
 * - Quaternions are (x, y, z, w) with w last.
 * - Ownership: the application keeps renderers, meshes, and materials
 *   alive while queued or recording;destroying renderer children is
 *   NULL-safe, cross-renderer use is rejected, and recording with
 *   dead objects fails safely instead of crashing. Destroy renderers
 *   before their LumaC device shuts down (dependents-first, like
 *   LumaC itself).
 * - Recording model: the caller owns render passes. A typical frame:
 *     lc_begin_frame(sc); lc_swapchain_get_encoder(sc, &enc);
 *     lc_encoder_begin_swapchain_pass(enc, sc, &spass);   // or offscreen
 *     lr_renderer_render(renderer, enc, target);           // draws only
 *     lc_encoder_end_render_pass(enc); lc_end_frame(sc);
 *   lr_renderer_render never begins or ends passes.
 */

#include <stddef.h>
#include <stdint.h>

#include <lumac/lumac.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Renderer result codes (LumaC results map onto these; Vulkan and
 * lc_result values never leak). */
typedef enum lr_result {
    LR_SUCCESS = 0,
    LR_ERROR_INVALID_ARGUMENT = 1,
    LR_ERROR_NOT_INITIALIZED = 2,
    LR_ERROR_OUT_OF_MEMORY = 3,
    LR_ERROR_UNSUPPORTED = 4,
    LR_ERROR_INCOMPATIBLE = 5,
    LR_ERROR_RENDER = 6
} lr_result;

/* Opaque renderer objects. Never dereference; use the API below. */
typedef struct lr_renderer lr_renderer;
typedef struct lr_mesh lr_mesh;
typedef struct lr_material lr_material;

/* ------------------------------------------------------------------
 * Standard mesh vertex (Phase 13 baseline, PBR-ready).
 *
 * position/normal/tangent/uv packed tightly (56 bytes). Normals are
 * unit length; tangent direction follows the UV convention with
 * MikktSpace-style handedness in tangent.w (-1 or +1) for future
 * normal mapping. Imported (glTF) geometry maps naturally onto it in
 * Phase 14.
 * ------------------------------------------------------------------ */
typedef struct lr_vertex {
    float position[3];
    float normal[3];
    float tangent[4];
    float texcoord[2];
} lr_vertex;

/* Static mesh source data. Vertices are copied and uploaded into
 * GPU-only buffers at creation; the caller keeps its arrays. Indices
 * are 32-bit (renderer-local UINT16 packing is future work). Every
 * index must reference a live vertex. */
typedef struct lr_mesh_desc {
    const lr_vertex *vertices;
    uint32_t vertex_count;
    const uint32_t *indices;
    uint32_t index_count;
} lr_mesh_desc;

/* Local axis-aligned bounds + bounding sphere (world-ready). */
typedef struct lr_bounds {
    float min[3];
    float max[3];
    float center[3];
    float radius;
} lr_bounds;

/**
 * Compute bounds for mesh source data without creating anything
 * (pure function; NULL-safe argument checks, no GPU use).
 */
lr_result lr_mesh_compute_bounds(const lr_mesh_desc *desc,
                                 lr_bounds *out_bounds);

/**
 * Create a static GPU mesh on a renderer (uploads into GPU-only
 * vertex/index buffers through public LumaC staging).
 */
lr_result lr_mesh_create(lr_renderer *renderer,
                         const lr_mesh_desc *desc,
                         lr_mesh **out_mesh);

/**
 * Destroy a mesh and its GPU buffers. Safe with NULL. Meshes queued
 * for the current frame should stay alive until rendering completes;
 * the renderer skips dead entries defensively at render time.
 */
void lr_mesh_destroy(lr_mesh *mesh);

/** Mesh vertex count (0 for NULL). */
uint32_t lr_mesh_get_vertex_count(const lr_mesh *mesh);

/** Mesh index count (0 for NULL). */
uint32_t lr_mesh_get_index_count(const lr_mesh *mesh);

/** Mesh local bounds (zeros for NULL; out may be NULL for a no-op). */
void lr_mesh_get_bounds(const lr_mesh *mesh, lr_bounds *out_bounds);

/** Primitive helpers (valid positions/normals/tangents/UVs/indices). */
lr_result lr_mesh_create_cube(lr_renderer *renderer, float size,
                              lr_mesh **out_mesh);
lr_result lr_mesh_create_plane(lr_renderer *renderer, float width,
                               float depth, lr_mesh **out_mesh);
lr_result lr_mesh_create_sphere(lr_renderer *renderer, float radius,
                                uint32_t segments, uint32_t rings,
                                lr_mesh **out_mesh);

/* Pure primitive data (no renderer/GPU): fills caller arrays so unit
 * tests and future importers can validate geometry headlessly.
 * Sizes: cube 24v/36i, plane 4v/6i, sphere (segments+1)*(rings+1)
 * vertices and segments*rings*6 indices. */
#define LR_CUBE_VERTEX_COUNT 24u
#define LR_CUBE_INDEX_COUNT 36u
#define LR_PLANE_VERTEX_COUNT 4u
#define LR_PLANE_INDEX_COUNT 6u
lr_result lr_mesh_cube_data(lr_vertex *out_vertices, uint32_t *out_indices,
                            float size);
lr_result lr_mesh_plane_data(lr_vertex *out_vertices, uint32_t *out_indices,
                             float width, float depth);
lr_result lr_mesh_sphere_data(lr_vertex *out_vertices, uint32_t *out_indices,
                              float radius, uint32_t segments,
                              uint32_t rings);

/* ------------------------------------------------------------------
 * Materials (Phase 13 unlit foundation + Phase 15 PBR).
 *
 * Two explicit kinds share one registry, one queue, and one
 * material-sorted bind path; pipelines vary by kind (plus culling
 * and target signature) so the unlit path is untouched.
 * ------------------------------------------------------------------ */

/* Material kind (UNKNOWN only ever observes NULL handles). */
typedef enum lr_material_type {
    LR_MATERIAL_UNKNOWN = 0,
    LR_MATERIAL_UNLIT = 1,
    LR_MATERIAL_PBR_METALLIC_ROUGHNESS = 2
} lr_material_type;

/* Alpha handling (parsed from glTF, stored in the material UBO).
 * Phase 15 renders OPAQUE only; MASK/BLEND ride along for Phase 16. */
typedef enum lr_alpha_mode {
    LR_ALPHA_OPAQUE = 0,
    LR_ALPHA_MASK = 1,
    LR_ALPHA_BLEND = 2
} lr_alpha_mode;

/* Unlit material parameters. NULL texture/sampler select the
 * renderer's 1x1 white fallback + default sampler, so untextured
 * materials render correctly with no special shader branch. Texture
 * and sampler (when given) must belong to the renderer's device and
 * stay alive with the material. */
typedef struct lr_unlit_material_desc {
    float color[4];
    lc_image_view *base_color_texture;
    lc_sampler *sampler;
} lr_unlit_material_desc;

/**
 * Create an unlit material (textured modulated by color, or flat
 * color over the fallback texture).
 */
lr_result lr_material_create_unlit(lr_renderer *renderer,
                                   const lr_unlit_material_desc *desc,
                                   lr_material **out_material);

/* PBR metallic-roughness material parameters (glTF 2.0 semantics).
 *
 * Factors multiply their texture channel (missing textures bind
 * documented neutral fallbacks, so factors pass through unchanged):
 *   metallic  = metallic_factor  x texel.B (metallic-roughness map)
 *   roughness = roughness_factor x texel.G (metallic-roughness map)
 *   occlusion = mix(1, texel.R, occlusion_strength), ambient only
 *   emissive += emissive_factor x emissive texel (after lighting)
 *   normal   <- tangent-space map x normal_scale (TBN, handedness w)
 * Base-color/emissive textures are expected sRGB; metallic-roughness,
 * normal, and occlusion linear (no manual gamma anywhere — the GPU
 * format handles sRGB). NULL views/samplers select fallbacks (white
 * base, neutral metallic-roughness, flat normal, white occlusion,
 * white emissive so factor-only emission works — the zero default
 * factor keeps it dark, default sampler). Views/sampler must belong to the
 * renderer's device and stay alive with the material (borrowed).
 * double_sided disables culling for this material's pipeline
 * variant; backface shading normals flip in-shader. alpha_mode is
 * stored (introspection/MCP) but Phase 15 renders OPAQUE only. */
typedef struct lr_pbr_material_desc {
    float base_color_factor[4];
    float metallic_factor;
    float roughness_factor;
    lc_image_view *base_color_texture;
    lc_image_view *metallic_roughness_texture;
    lc_image_view *normal_texture;
    lc_image_view *occlusion_texture;
    lc_image_view *emissive_texture;
    lc_sampler *sampler;
    float emissive_factor[3];
    float normal_scale;
    float occlusion_strength;
    int double_sided;
    lr_alpha_mode alpha_mode;
} lr_pbr_material_desc;

/**
 * Create a PBR metallic-roughness material (own GPU parameter
 * buffer + descriptor set over the renderer's PBR layout).
 */
lr_result lr_material_create_pbr(lr_renderer *renderer,
                                 const lr_pbr_material_desc *desc,
                                 lr_material **out_material);

/** Destroy a material (descriptor set + parameter buffer freed;
 *  shared GPU resources stay owned by the renderer). Safe with NULL. */
void lr_material_destroy(lr_material *material);

/** Material kind (LR_MATERIAL_UNKNOWN for NULL). */
lr_material_type lr_material_get_type(const lr_material *material);

/* Read-only PBR introspection (MCP-ready, no private inspection
 * needed). Views/sampler borrow material slots (NULL when that slot
 * uses a renderer fallback); factors/scales/flags are value copies.
 * Unlit materials (or NULL out) yield a zeroed struct with
 * type = actual type (UNKNOWN for NULL material). */
typedef struct lr_pbr_material_info {
    lr_material_type type;
    float base_color_factor[4];
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
} lr_pbr_material_info;

/** Copy out PBR metadata (NULL material zeroes with type UNKNOWN;
 *  out may be NULL for a no-op). */
void lr_material_get_pbr_info(const lr_material *material,
                              lr_pbr_material_info *out_info);

/* ------------------------------------------------------------------
 * Camera (value type; no lifetime beyond its bytes).
 * ------------------------------------------------------------------ */
typedef struct lr_camera {
    float position[3];
    float view[16];
    float projection[16];
    float near_plane;
    float far_plane;
    float vertical_fov;
    float aspect_ratio;
} lr_camera;

/** Zero/initialize a camera (identity view/proj, default planes). */
void lr_camera_init(lr_camera *camera);

/** Perspective projection (fov_y_rad in (0, PI), aspect > 0,
 *  0 < near < far). Returns previous-style result codes. */
lr_result lr_camera_set_perspective(lr_camera *camera, float fov_y_rad,
                                    float aspect, float near_plane,
                                    float far_plane);

/** View from eye/center/up (center != eye, up non-degenerate). */
lr_result lr_camera_look_at(lr_camera *camera, const float eye[3],
                            const float center[3], const float up[3]);

/** Move the camera keeping its current orientation. */
void lr_camera_set_position(lr_camera *camera, const float position[3]);

/* ------------------------------------------------------------------
 * Transforms (value types; quaternion rotation, T*R*S compose).
 * ------------------------------------------------------------------ */
typedef struct lr_transform {
    float position[3];
    float rotation[4]; /* quaternion (x, y, z, w) */
    float scale[3];
} lr_transform;

/** Identity transform (no translation/rotation, unit scale). */
void lr_transform_identity(lr_transform *transform);

/** Compose a column-major matrix (T * R * S). */
void lr_transform_to_matrix(const lr_transform *transform,
                            float out_matrix[16]);

/** Quaternion from axis (need not be unit) + angle radians. */
void lr_quat_from_axis_angle(const float axis[3], float angle_rad,
                             float out_quat[4]);

/** Hamilton product (a applied after b: out = a * b). */
void lr_quat_multiply(const float a[4], const float b[4],
                      float out[4]);

/* ------------------------------------------------------------------
 * Draw list (per-frame submissions; NOT a scene graph).
 * ------------------------------------------------------------------ */

/* One submission: mesh + material rendered with a transform.
 * lr_transform keeps the API pleasant; the renderer flattens it once
 * at submit (queue stores matrices). Shadow participation is
 * explicit opt-in (nonzero participates; zeroed items neither cast
 * nor receive — C zero-init must stay inert, matching the
 * double_sided opt-in precedent; Luma Assets enables both for
 * imported opaque models). */
typedef struct lr_draw_item {
    lr_mesh *mesh;
    lr_material *material;
    lr_transform transform;
    int casts_shadow;
    int receives_shadow;
} lr_draw_item;

/* Opaque per-frame queue statistics. Light counts and per-kind
 * draw calls exist for the future profiler/MCP path; shadow fields
 * make per-frame shadow cost visible (PART AT); environment fields
 * make IBL/sky/tonemap cost visible (Phase 17). */
typedef struct lr_render_stats {
    uint32_t submitted_objects;
    uint32_t visible_objects;
    uint32_t draw_calls;
    uint32_t triangles;
    uint32_t pipeline_binds;
    uint32_t material_binds;
    uint32_t submitted_lights;
    uint32_t active_lights;
    uint32_t pbr_draw_calls;
    uint32_t unlit_draw_calls;
    uint32_t shadow_casting_lights;
    uint32_t shadow_passes;
    uint32_t shadow_draw_calls;
    uint32_t shadow_triangles;
    uint32_t shadow_maps_rendered;
    uint32_t shadow_casters_culled;
    uint32_t environment_passes;   /* preprocessing passes this frame */
    uint32_t sky_draw_calls;
    uint32_t tonemap_passes;
    uint32_t ibl_enabled;          /* 1 when an environment lit the frame */
    uint32_t environment_rebuilds; /* env (re)builds this frame */
} lr_render_stats;

/* ------------------------------------------------------------------
 * Lights (Phase 15: submitted per-frame data, NOT scene entities).
 *
 * Direction convention (explicit): `direction` is the direction the
 * light TRAVELS (from the light toward the scene); shaders use
 * L = -direction. Renderer normalizes on submit; zero direction is
 * rejected. Point lights attenuate inverse-square-style with smooth
 * finite-range cutoff (see PBR docs); range must be > 0.
 * ------------------------------------------------------------------ */

/* Maximum lights per frame (uniform-buffer sized, modest by design;
 * clustered lighting is future work). */
#define LR_MAX_LIGHTS 64u

/* Maximum shadow-casting lights per frame (fixed renderer-owned
 * slots; deliberately far below LR_MAX_LIGHTS — 64 full-resolution
 * depth maps are never allocated). */
#define LR_MAX_SHADOWS 4u

/* Default shadow-map resolution (power of two, 128..4096). */
#define LR_SHADOW_RESOLUTION_DEFAULT 1024u

typedef enum lr_light_type {
    LR_LIGHT_DIRECTIONAL = 0,
    LR_LIGHT_POINT = 1,
    LR_LIGHT_SPOT = 2
} lr_light_type;

/* Shadow configuration (renderer-side; NOT an engine object).
 * Negative depth_bias/normal_bias select tuned defaults (currently
 * 0.0015 NDC constant, 0.02 world normal); zero disables that bias
 * (useful for acne tests). resolution 0 selects 1024. near_plane /
 * far_plane <= 0 auto-resolve (spot: 0.5 / range; directional: fit
 * derived). shadow_distance <= 0 selects 25m of camera-frustum fit
 * for directional lights (ignored by spots). Point lights never
 * shadow in Phase 16 (enabled on a point is INVALID_ARGUMENT). */
typedef struct lr_shadow_desc {
    int enabled;
    uint32_t resolution;
    float depth_bias;
    float normal_bias;
    float near_plane;
    float far_plane;
    float shadow_distance;
} lr_shadow_desc;

typedef struct lr_light {
    lr_light_type type;
    float color[3];
    float intensity;
    float position[3];  /* point + spot */
    float range;        /* point + spot, > 0 */
    float direction[3]; /* directional + spot travel direction */
    float spot_inner;   /* spot only, radians, 0 <= inner <= outer */
    float spot_outer;   /* spot only, radians, < pi/2 */
    lr_shadow_desc shadow;
} lr_light;

/* Renderer creation parameters. `render_target` is the primary
 * structural signature (validated, pre-builds the unlit + PBR
 * pipelines so configuration failures surface here); additional
 * compatible signatures are cached on demand per encountered target.
 * `ambient_light` is the temporary non-physical fallback that keeps
 * unlit portions from going fully black until IBL lands (Phase 16+);
 * all-zero selects the 0.03 gray default. */
typedef struct lr_renderer_desc {
    lc_device *device;
    lc_render_target_desc render_target;
    uint32_t max_objects;
    float ambient_light[3];
} lr_renderer_desc;

/**
 * Create a renderer (pipelines, standard layouts, camera buffer,
 * fallback texture/sampler). Requires initialized LumaC and a live
 * device; the device must outlive the renderer.
 */
lr_result lr_renderer_create(const lr_renderer_desc *desc,
                             lr_renderer **out_renderer);

/** Destroy a renderer and everything it owns (never the device).
 *  Safe with NULL. */
void lr_renderer_destroy(lr_renderer *renderer);

/**
 * Begin a frame: latch the camera (uploads view/proj/viewproj/pos to
 * the shared camera buffer once) and reset the queue, the light
 * list, and statistics.
 */
lr_result lr_renderer_begin(lr_renderer *renderer,
                            const lr_camera *camera);

/**
 * Submit one light for the open frame (copied; NOT a scene entity).
 * Lights reset every begin. The (LR_MAX_LIGHTS + 1)-th submit is
 * rejected (INVALID_ARGUMENT); zero direction, non-positive point/
 * spot range, bad spot cone (inner > outer, outer >= pi/2), or a
 * shadow request on a point light is rejected. Shadow-casting
 * directional/spot lights take the first LR_MAX_SHADOWS slots in
 * submit order (deterministic); further shadow requests render
 * unshadowed. Submit order is upload order.
 */
lr_result lr_renderer_submit_light(lr_renderer *renderer,
                                   const lr_light *light);

/**
 * Render shadow maps for this frame's shadow-casting lights (call
 * after all submits, before opening the main pass; at most once per
 * frame — a second call is INVALID_ARGUMENT). Records one depth-only
 * pass per assigned slot into renderer-owned targets (reused across
 * frames), culls casters per light frustum/volume, and fills the
 * shadow metadata uploaded by lr_renderer_render. With no shadowed
 * lights this only zeroes metadata (still marks prepared). Main
 * rendering without this call treats every light as unshadowed.
 */
lr_result lr_renderer_render_shadows(lr_renderer *renderer,
                                     lc_command_encoder *encoder);

/* Read-only shadow-slot state (MCP-ready; views are borrowed
 * LumaC handles, valid while the slot keeps its resolution —
 * re-query after resolution changes). */
typedef struct lr_shadow_slot_info {
    int active; /* assigned this frame */
    uint32_t resolution;
    lc_format format;
    lr_light_type light_type;
} lr_shadow_slot_info;

/** Live shadow slots (LR_MAX_SHADOWS; 0 for NULL). */
uint32_t lr_renderer_get_shadow_count(const lr_renderer *renderer);

/** Copy out one slot's info (zeros for NULL renderer or out-of-range
 *  index; info may be NULL for a no-op). */
void lr_renderer_get_shadow_slot_info(const lr_renderer *renderer,
                                      uint32_t index,
                                      lr_shadow_slot_info *out_info);

/** Borrow one slot's depth view for debug visualization (NULL for
 *  NULL renderer, out-of-range index, or a slot with no image yet).
 *  Sample .x as depth; lifetime follows the slot (re-query after
 *  resolution changes). */
lc_image_view *lr_renderer_get_shadow_view(lr_renderer *renderer,
                                           uint32_t index);

/** Submitted lights this frame (0 for NULL; valid between begin and
 *  end). */
uint32_t lr_renderer_get_light_count(const lr_renderer *renderer);

/** Copy out one submitted light + its shadow config (zeros for NULL
 *  renderer or out-of-range; either out pointer may be NULL). */
void lr_renderer_get_light(const lr_renderer *renderer, uint32_t index,
                           lr_light *out_light,
                           lr_shadow_desc *out_shadow);

/** Override the temporary ambient fallback (NULL-safe no-op on NULL
 *  renderer; NULL color reads as black). */
void lr_renderer_set_ambient(lr_renderer *renderer,
                             const float ambient[3]);

/** Copy out the current ambient fallback (zeros for NULL renderer;
 *  out may be NULL for a no-op). */
void lr_renderer_get_ambient(const lr_renderer *renderer,
                             float out_ambient[3]);

/** Live pipeline variants in the cache (0 for NULL). Bounded by the
 *  cache cap; stable across frames once warm (endurance signal). */
uint32_t lr_renderer_get_pipeline_count(const lr_renderer *renderer);

/**
 * Submit one item (frustum-culled against the latched camera;
 * culled items count as submitted but never draw in the main pass,
 * yet stay queued as shadow casters). Items reference live
 * meshes/materials owned by this renderer; cross-renderer or
 * dead objects are rejected. Queue overflow is rejected.
 */
lr_result lr_renderer_submit(lr_renderer *renderer,
                             const lr_draw_item *item);

/**
 * Record all visible items into the encoder's currently open pass
 * for `target` (caller begins/ends the pass; the target selects the
 * cached pipeline by signature). Sorts opaque items by material then
 * mesh to skip redundant binds. Skips entries whose mesh/material
 * died after submission.
 */
lr_result lr_renderer_render(lr_renderer *renderer,
                             lc_command_encoder *encoder,
                             lc_render_target *target);

/** End the frame (releases queue references; stats stay readable). */
void lr_renderer_end(lr_renderer *renderer);

/** Copy out the current frame statistics (zeros for NULL renderer;
 *  out may be NULL for a no-op). */
void lr_renderer_get_stats(const lr_renderer *renderer,
                           lr_render_stats *out_stats);

/* ------------------------------------------------------------------
 * Frame profiling (Phase 18: CPU-side recording timings).
 *
 * High-resolution monotonic CPU timings of the renderer's own
 * recording work (never GPU execution): prepare, shadow recording,
 * main scene recording, sky, post chain, tonemap/output, and the
 * total. Kept separate from lr_render_stats so integer counters
 * stay stable; both reset every begin. GPU timestamps are deferred
 * (no query pools in the renderer — see POST_PROCESS docs).
 * ------------------------------------------------------------------ */

/* CPU recording profile for the current frame (milliseconds). */
typedef struct lr_frame_profile {
    uint32_t frame_number;  /* 1-based, increments every begin */
    double cpu_prepare_ms;  /* render_shadows (slot passes + metadata) */
    double cpu_shadow_ms;   /* same as prepare today (split reserved) */
    double cpu_main_ms;     /* scene item draws (HDR or legacy) */
    double cpu_sky_ms;      /* sky pass recording */
    double cpu_post_ms;     /* post chain (verification effect etc.) */
    double cpu_tonemap_ms;  /* output/tonemap recording */
    double cpu_total_ms;    /* begin-to-end recording, all stages */
    uint32_t post_passes;   /* post chain passes recorded this frame */
} lr_frame_profile;

/** Copy out the current frame CPU profile (zeros for NULL renderer;
 *  out may be NULL for a no-op). */
void lr_renderer_get_frame_profile(const lr_renderer *renderer,
                                   lr_frame_profile *out_profile);

/* Structured frame diagnostics (MCP/AI-ready; no native handles).
 * Everything here is also available piecemeal (stats, profile,
 * environment info, cache info); this struct is the one-call
 * snapshot for tooling: capture_game_view-class futures build on
 * it, never on backend-private access. */
typedef struct lr_frame_diagnostics {
    uint32_t frame_number;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t draw_calls;
    uint32_t triangles;
    uint32_t shadow_passes;
    uint32_t ibl_active;
    uint32_t post_passes;
    double cpu_total_ms;
    int pipeline_cache_enabled;
    int pipeline_cache_loaded;
    uint64_t pipeline_cache_bytes_loaded;
} lr_frame_diagnostics;

/** Copy out the one-call diagnostics snapshot (zeros for NULL
 *  renderer; out may be NULL for a no-op). */
void lr_renderer_get_frame_diagnostics(const lr_renderer *renderer,
                                       lr_frame_diagnostics *out_diag);

/* ------------------------------------------------------------------
 * Post-process chain (Phase 18: internal foundation).
 *
 * HDR scene -> [optional post stages] -> tonemap/output. Exactly one
 * verification stage exists today (PART AE); bloom/SSAO/TAA arrive
 * later without rewriting frame flow:
 *
 *   render_scene records: sky + items into HDR, then each enabled
 *   post stage (ping-pong intermediates), leaving the chain head in
 *   `post_head_view` (or the HDR view when the chain is empty).
 *   render_output tonemaps from the chain head.
 *
 * Ping-pong intermediates are renderer-owned, allocated lazily,
 * reused across frames, recreated only on extent/format change —
 * never per frame. Multiview: one chain per output extent (two
 * slots; environments stay shared). Default: chain empty (OFF).
 * ------------------------------------------------------------------ */

/* Optional internal post stages (exactly one exists today). */
typedef enum lr_post_stage {
    LR_POST_NONE = 0,      /* chain empty (default) */
    LR_POST_TINT_VERIFY = 1 /* color-multiply verification effect */
} lr_post_stage;

/** Enable/disable one verification stage (out-of-range rejected).
 *  Only one stage can be active at a time in Phase 18. */
lr_result lr_renderer_set_post_stage(lr_renderer *renderer,
                                     lr_post_stage stage);

/** Copy out the active stage (NONE for NULL). */
lr_post_stage lr_renderer_get_post_stage(const lr_renderer *renderer);

/** Set the tint-verify multiplier (default 1,1,1 = identity).
 *  NULL color rejected. Applies only to LR_POST_TINT_VERIFY. */
lr_result lr_renderer_set_post_tint(lr_renderer *renderer,
                                    const float rgb[3]);

/* ------------------------------------------------------------------
 * Capture (Phase 18: public-API screenshots).
 *
 * No new capture recording is needed in the common case: the
 * borrowed HDR view (render_scene) and the caller's own LDR target
 * image read through lc_image_readback. These helpers only route
 * borrowed views to their images through public LumaC (never
 * backend-private access):
 * - lr_renderer_capture_hdr: read selected HDR pixels (floats > 1.0
 *   prove HDR) into caller memory (tightly packed RGBA16F).
 * - lr_renderer_capture_output: read color attachment 0 of any
 *   caller target the same way (LDR pixels for screenshots).
 * A future async API extends these names.
 * ------------------------------------------------------------------ */

/** Read HDR scene pixels via public readback (NULL-safe arg checks;
 *  dst rules mirror lc_image_readback, incl. sizing queries). Fails
 *  without a render_scene this frame. */
lr_result lr_renderer_capture_hdr(lr_renderer *renderer, void *dst,
                                  size_t dst_size,
                                  size_t *out_required_size);

/** Read color attachment 0 of an output target via public readback
 *  (same dst rules). The target image needs TRANSFER_SRC (loud
 *  failure otherwise — capture-capable targets include it). Fails
 *  without a render_output into `target` this frame is NOT
 *  required: any rendered target reads back (contents are whatever
 *  the last pass stored). */
lr_result lr_renderer_capture_output(lr_renderer *renderer,
                                     lc_render_target *target, void *dst,
                                     size_t dst_size,
                                     size_t *out_required_size);

/** Borrow the device this renderer was created with (NULL for NULL).
 *  The device outlives the renderer by contract; callers must not
 *  destroy it. Exists so upper layers (Luma Assets) can allocate
 *  through public LumaC APIs without reaching into internals. */
lc_device *lr_renderer_get_device(const lr_renderer *renderer);

/* ------------------------------------------------------------------
 * Environment lighting / IBL / HDR / tonemapping (Phase 17).
 *
 * An lr_environment is a renderer-level (NOT scene, NOT light,
 * NOT LumaC) object owning preprocessed image-based lighting
 * derived from one equirectangular HDR source: a base cubemap
 * (visible sky), a diffuse irradiance cubemap, and a GGX
 * prefiltered specular cubemap chain. The split-sum BRDF
 * integration LUT is environment-independent and owned once by
 * the renderer. Preprocessing runs lazily on first use (and again
 * only when the source changes); intensity/rotation are runtime
 * shader parameters that never reprocess.
 *
 * Frame flow with an environment:
 *   begin -> submits -> render_shadows ->
 *   render_scene (HDR scene + sky) -> render_output (tonemap) -> end
 * render() stays as the legacy one-call path (scene + output).
 * ------------------------------------------------------------------ */

/* Environment resolutions (face pixels; prefilter carries a full
 * mip chain down to 1x1, irradiance is a single mip). */
#define LR_ENV_CUBE_SIZE 128u
#define LR_ENV_IRRADIANCE_SIZE 32u
#define LR_ENV_PREFILTER_SIZE 64u
#define LR_ENV_BRDF_SIZE 256u

/* Preprocessing tap counts (deterministic fixed kernels). */
#define LR_ENV_IRRADIANCE_TAPS 128u
#define LR_ENV_PREFILTER_TAPS 256u
#define LR_ENV_BRDF_TAPS 128u

/* Environment source description. The texture and sampler are
 * BORROWED and must outlive the environment. intensity >= 0
 * (0 removes IBL while direct lights/emissive remain); rotation
 * is a horizontal yaw in radians applied to lighting AND sky. */
typedef struct lr_environment_desc {
    lc_image_view *environment_texture;
    lc_sampler *sampler;
    float intensity;
    float rotation;
} lr_environment_desc;

typedef struct lr_environment lr_environment;

/** Create an environment object (cheap; preprocessing is lazy on
 *  first use). Copies the descriptor; source texture/sampler stay
 *  borrowed. NULL renderer/desc/out or NULL source texture/sampler
 *  or negative intensity is INVALID_ARGUMENT. */
lr_result lr_environment_create(lr_renderer *renderer,
                               const lr_environment_desc *desc,
                               lr_environment **out_env);

/** Destroy an environment and its derived resources (never the
 *  borrowed source texture/sampler). Safe with NULL. */
void lr_environment_destroy(lr_environment *env);

/** Replace the descriptor. A new source texture/sampler marks the
 *  environment dirty (reprocessed on next use); intensity/rotation
 *  changes only touch runtime parameters. */
lr_result lr_environment_update(lr_environment *env,
                               const lr_environment_desc *desc);

/** Runtime intensity (no reprocessing). Negative is rejected. */
lr_result lr_environment_set_intensity(lr_environment *env,
                                       float intensity);

/** Runtime yaw rotation in radians (no reprocessing). */
lr_result lr_environment_set_rotation(lr_environment *env,
                                      float rotation);

/** Attach/detach the active environment (borrowed; NULL disables
 *  IBL and restores the ambient fallback). Safe with NULL
 *  renderer (no-op); destroying the active environment without
 *  detaching first is an app error (set NULL first). */
lr_result lr_renderer_set_environment(lr_renderer *renderer,
                                      lr_environment *env);

/* Tonemap operator for the output pass (separate pass, never in
 * the PBR shader). NONE clamps exposure-scaled HDR into [0,1];
 * ACES is the Narkowicz fitted filmic operator. Default NONE. */
typedef enum lr_tonemap_operator {
    LR_TONEMAP_NONE = 0,
    LR_TONEMAP_ACES = 1
} lr_tonemap_operator;

/** Exposure in EV (multiplier = 2^EV applied pre-tonemap).
 *  Default 0. Any finite value accepted; NaN/inf rejected. */
lr_result lr_renderer_set_exposure(lr_renderer *renderer, float ev);

/** Copy out the exposure EV (0 for NULL). */
float lr_renderer_get_exposure(const lr_renderer *renderer);

/** Output-pass operator (out-of-range rejected; default NONE). */
lr_result lr_renderer_set_tonemap_operator(lr_renderer *renderer,
                                           lr_tonemap_operator op);

/** Copy out the operator (NONE for NULL). */
lr_tonemap_operator
lr_renderer_get_tonemap_operator(const lr_renderer *renderer);

/**
 * Record the HDR scene pass into the renderer's internal floating
 * point target (auto-sized to width x height, recreated only on
 * extent change): all visible items, then the sky where no
 * geometry wrote depth. Caller must NOT have a pass open (passes
 * cannot nest); requires begin + submits beforehand (prepare via
 * render_shadows when shadowed lights exist, else lights render
 * unshadowed). Zero width/height is INVALID_ARGUMENT.
 */
lr_result lr_renderer_render_scene(lr_renderer *renderer,
                                   lc_command_encoder *encoder,
                                   uint32_t width, uint32_t height);

/**
 * Record the output pass: tonemap (exposure + operator) from the
 * last HDR scene into `target` (caller begins/ends the pass; any
 * LDR target — swapchain or offscreen — works). sRGB encoding, if
 * any, happens exactly once via the target format (never in the
 * shader). Fails without a preceding render_scene this frame.
 */
lr_result lr_renderer_render_output(lr_renderer *renderer,
                                    lc_command_encoder *encoder,
                                    lc_render_target *target);

/** Borrow the internal HDR scene color view (NULL when none was
 *  rendered yet). Read-only diagnostics (bloom/screenshots later);
 *  lifetime follows the HDR target (re-query after resizes). */
lc_image_view *lr_renderer_get_hdr_view(lr_renderer *renderer);

/** Borrow the shared BRDF integration LUT view (NULL until first
 *  environment preprocessing builds it). Read-only diagnostics;
 *  read pixels through lc_image_view_get_image + lc_image_readback
 *  (public LumaC only). Lifetime follows the renderer. */
lc_image_view *lr_renderer_get_brdf_view(lr_renderer *renderer);

/* Read-only environment state (MCP-ready; no handles). */
typedef struct lr_environment_info {
    int active;                    /* environment attached this frame */
    float intensity;
    float rotation;
    float exposure_ev;
    lr_tonemap_operator tonemap;
    lc_format hdr_format;          /* LC_FORMAT_UNDEFINED when none */
    uint32_t hdr_width;
    uint32_t hdr_height;
    uint32_t preprocess_state;     /* 0 = none, 1 = ready */
    uint32_t environment_rebuilds; /* lifetime total */
    uint64_t environment_generation;
    uint32_t last_build_ms;        /* wall time of the last env build */
} lr_environment_info;

/** Copy out environment/output state (zeros for NULL renderer;
 *  out may be NULL for a no-op). */
void lr_renderer_get_environment_info(const lr_renderer *renderer,
                                      lr_environment_info *out_info);

#ifdef __cplusplus
}
#endif

#endif /* LUMA_RENDERER_H */
