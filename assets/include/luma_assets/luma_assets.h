#ifndef LUMA_ASSETS_H
#define LUMA_ASSETS_H

/*
 * Luma Assets (Phase 14): glTF 2.0 model ingestion on top of PUBLIC
 * Luma Renderer + LumaC only. Neither LumaC nor Luma Renderer depends
 * on this header.
 *
 * Layering (strict):
 *   Luma Assets may use Luma Renderer and LumaC. Neither may depend
 *   on Luma Assets. The renderer stays usable without this module.
 *
 * Import pipeline (deterministic, file order preserved throughout):
 *   .glb / .gltf on disk
 *     -> parse + validate (cgltf, vendored)
 *     -> decode images (stb_image, vendored; PNG/JPEG to RGBA8)
 *     -> upload meshes/textures through public renderer/LumaC APIs
 *     -> keep lightweight metadata only (no duplicate CPU copies of
 *        vertex/index/texel data unless documented otherwise)
 *
 * Ownership:
 *   The asset manager owns shared cached resources (GPU textures and
 *   samplers); models own their meshes, materials, hierarchy, and
 *   metadata while borrowing cached textures/samplers. Destroying a
 *   model releases its references; the manager destroys surviving
 *   cache entries at teardown. Destroy models before their manager,
 *   and managers before their renderer/device (dependents-first).
 *
 * Threading: main thread only (matches LumaC).
 */

#include <stddef.h>
#include <stdint.h>

#include <luma_renderer/luma_renderer.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Asset result codes (lc_result/lr_result values never leak). */
typedef enum la_result {
    LA_SUCCESS = 0,
    LA_ERROR_INVALID_ARGUMENT = 1,
    LA_ERROR_NOT_INITIALIZED = 2,
    LA_ERROR_OUT_OF_MEMORY = 3,
    LA_ERROR_NOT_FOUND = 4,
    LA_ERROR_UNSUPPORTED = 5,
    LA_ERROR_IMPORT = 6,
    LA_ERROR_RENDER = 7
} la_result;

/* Opaque asset objects. Never dereference; use the API below. */
typedef struct la_asset_manager la_asset_manager;
typedef struct la_model la_model;

/* ------------------------------------------------------------------
 * Asset manager (owns shared texture/sampler caches + model list).
 * ------------------------------------------------------------------ */

/* Manager creation parameters. `renderer` is borrowed and must
 * outlive the manager. */
typedef struct la_asset_manager_desc {
    lr_renderer *renderer;
} la_asset_manager_desc;

/**
 * Create an asset manager. Requires initialized LumaC and a live
 * renderer; both must outlive the manager.
 *
 * @return LA_SUCCESS, LA_ERROR_INVALID_ARGUMENT (NULL desc/out or
 *         NULL renderer), LA_ERROR_OUT_OF_MEMORY.
 */
la_result la_asset_manager_create(const la_asset_manager_desc *desc,
                                  la_asset_manager **out_manager);

/**
 * Destroy a manager: destroys all models it loaded that the caller
 * still owns? No — models are caller-owned and must be destroyed
 * first (dependents-first, like every Luma layer); remaining shared
 * cache entries die here. Safe with NULL.
 */
void la_asset_manager_destroy(la_asset_manager *manager);

/**
 * Most recent failure detail for this manager (unsupported mode,
 * missing file, corrupt image, out-of-range accessor, ...). Returns
 * "" when nothing has failed yet or on NULL manager. The pointer is
 * manager-owned: valid until the next failing call or destroy.
 */
const char *la_asset_manager_get_last_error(
    const la_asset_manager *manager);

/** Live shared GPU textures in the manager cache (0 for NULL). */
uint32_t la_asset_manager_get_texture_count(
    const la_asset_manager *manager);

/** Live shared GPU samplers in the manager cache (0 for NULL). */
uint32_t la_asset_manager_get_sampler_count(
    const la_asset_manager *manager);

/* ------------------------------------------------------------------
 * Model representation (imported hierarchy + metadata).
 * ------------------------------------------------------------------ */

/* One node of the imported hierarchy (order = file order, never
 * flattened: children address a model-owned flat link array so
 * future animation and scene import keep working). `mesh_index` is
 * -1 when the node instantiates no mesh. `skin_index` is the
 * file-order skin used by this node (-1 when unskinned; joints
 * index into that skin's joint list, NOT into nodes). `name`
 * points at model-owned storage ("" when unnamed); valid while
 * the model lives. Both the decomposed `local_transform` and the
 * lossless `local_matrix` (column-major) are kept: submission uses
 * the matrix, inspection uses either. */
typedef struct la_model_node {
    const char *name;
    int32_t parent;
    uint32_t first_child;
    uint32_t child_count;
    lr_transform local_transform;
    float local_matrix[16];
    int32_t mesh_index;
    int32_t skin_index;
} la_model_node;

/* Alpha handling (parsed always, rendered as OPAQUE in Phase 14). */
typedef enum la_alpha_mode {
    LA_ALPHA_OPAQUE = 0,
    LA_ALPHA_MASK = 1,
    LA_ALPHA_BLEND = 2
} la_alpha_mode;

/* Forward-looking PBR material metadata. Texture fields are source
 * image indices (-1 when absent). The renderer consumes only the
 * base-color pair during Phase 14; everything else is preserved for
 * Phase 15/16. */
typedef struct la_pbr_material_data {
    float base_color_factor[4];
    float metallic_factor;
    float roughness_factor;
    int32_t base_color_texture;
    int32_t metallic_roughness_texture;
    int32_t normal_texture;
    int32_t occlusion_texture;
    int32_t emissive_texture;
    float emissive_factor[3];
    float normal_scale;
    float occlusion_strength;
    la_alpha_mode alpha_mode;
    float alpha_cutoff;
    int double_sided;
} la_pbr_material_data;

/* Read-only texture reference for inspection (manager-owned storage;
 * valid while the manager lives). `source` identifies the asset
 * (external path as written, or a stable embedded identity);
 * `srgb` nonzero means sRGB content (base color / emissive). */
typedef struct la_texture_info {
    uint32_t width;
    uint32_t height;
    int srgb;
    const char *source;
} la_texture_info;

/* Read-only sampler parameters for inspection (a value copy; valid
 * anywhere, no lifetime). */
typedef struct la_sampler_info {
    lc_sampler_desc params;
} la_sampler_info;

/**
 * Load a glTF 2.0 model (.gltf or .glb) through `manager`: parse,
 * validate, upload meshes/textures/materials, compute bounds. Needs
 * a live manager; `path` is UTF-8, resolved by the OS as given
 * (external references resolve relative to the file's directory).
 * CPU vertex/index/texel copies are freed after upload; only
 * lightweight metadata is kept.
 *
 * @return LA_SUCCESS, LA_ERROR_INVALID_ARGUMENT (NULL
 *         manager/path/out, dead manager), LA_ERROR_NOT_FOUND
 *         (missing file or missing external reference),
 *         LA_ERROR_UNSUPPORTED (primitive mode, sparse accessor,
 *         component type, ...), LA_ERROR_IMPORT (malformed JSON/GLB,
 *         corrupt image, out-of-range accessor, bad hierarchy, ...),
 *         LA_ERROR_OUT_OF_MEMORY, LA_ERROR_RENDER (GPU upload
 *         failure).
 */
la_result la_model_load(la_asset_manager *manager, const char *path,
                        la_model **out_model);

/**
 * Destroy a model: its meshes, materials, hierarchy, and metadata.
 * Releases its references to manager-cached textures/samplers.
 * Safe with NULL.
 */
void la_model_destroy(la_model *model);

/* ------------------------------------------------------------------
 * Ownership transfer (Phase 25 engine bridge): adopt renderer
 * resources OUT of a model into caller ownership. The model
 * releases the adopted slot (never double-destroys); the caller
 * destroys the resource (or hands it to another owner, e.g. the
 * engine asset registry). Textures/samplers stay manager-cached
 * (borrowed by adopted materials — keep the manager alive while
 * adopted materials live, same dependents-first rule).
 * ------------------------------------------------------------------ */

/** Adopt one mesh primitive's lr_mesh (model releases ownership;
 *  *out_mesh borrowed-then-owned by the caller on success).
 *
 * @return LA_SUCCESS, LA_ERROR_INVALID_ARGUMENT (NULL args, dead
 *         model, out-of-range indices, already-adopted slot),
 *         LA_ERROR_NOT_FOUND (no mesh at that node/primitive).
 */
la_result la_model_adopt_mesh(la_model *model, uint32_t mesh_index,
                              uint32_t primitive_index,
                              lr_mesh **out_mesh);

/** Adopt one material's lr_material (same contract). */
la_result la_model_adopt_material(la_model *model,
                                  uint32_t material_index,
                                  lr_material **out_material);

/** Borrow one primitive's mesh WITHOUT transfer (NULL for NULL
 *  model, out-of-range, or adopted-away slots). Lifetime follows
 *  the model. */
lr_mesh *la_model_borrow_mesh(const la_model *model,
                              uint32_t mesh_index,
                              uint32_t primitive_index);

/** Primitive count for one mesh slot (0 for NULL/out-of-range). */
uint32_t la_model_get_primitive_count(const la_model *model,
                                      uint32_t mesh_index);

/** Material index for one primitive (UINT32_MAX when unassigned;
 *  UINT32_MAX for NULL/out-of-range too). */
uint32_t la_model_get_primitive_material(const la_model *model,
                                         uint32_t mesh_index,
                                         uint32_t primitive_index);

/* Inspection (all NULL-safe; indices out of range behave as documented;
 * returned pointers borrow model/manager storage — see lifetimes). */
uint32_t la_model_get_node_count(const la_model *model);
uint32_t la_model_get_mesh_count(const la_model *model);
uint32_t la_model_get_material_count(const la_model *model);
uint32_t la_model_get_texture_count(const la_model *model);
uint32_t la_model_get_sampler_count(const la_model *model);

/* Source-level totals for sharing reports (0 for NULL model). */
uint32_t la_model_get_source_primitive_count(const la_model *model);
uint32_t la_model_get_source_texture_count(const la_model *model);
uint32_t la_model_get_source_sampler_count(const la_model *model);

/* Static mesh-instance bindings (node x primitive); deterministic. */
uint32_t la_model_get_instance_count(const la_model *model);

/** Borrow one node (NULL for NULL model or out-of-range index). */
const la_model_node *la_model_get_node(const la_model *model,
                                       uint32_t index);

/** Borrow one material's PBR metadata (NULL for NULL/out-of-range). */
const la_pbr_material_data *la_model_get_material_data(
    const la_model *model, uint32_t index);

/** Borrow one mesh/material/skin/animation/image name ("" when
 *  unnamed or unavailable; model-owned storage, valid while the
 *  model lives; NULL model yields ""). Names are file-authored
 *  (glTF `name` fields) and feed stable sub-asset keys — see
 *  docs/PORTABLE_IDENTITY.md. Mesh names are the FIRST node name
 *  referencing the mesh slot ("" when unreferenced); material,
 *  skin, animation names are file-order names; image names prefer
 *  the glTF image `name`, falling back to the uri. */
const char *la_model_get_mesh_name(const la_model *model,
                                   uint32_t mesh_index);
const char *la_model_get_material_name(const la_model *model,
                                       uint32_t material_index);
const char *la_model_get_skin_name(const la_model *model,
                                   uint32_t skin_index);
const char *la_model_get_animation_name(const la_model *model,
                                        uint32_t anim_index);
const char *la_model_get_image_name(const la_model *model,
                                    uint32_t image_index);

/** Borrow one referenced texture's info (0-filled when unavailable;
 *  `out` may be NULL for a no-op). */
void la_model_get_texture_info(const la_model *model, uint32_t index,
                               la_texture_info *out);

/** Borrow one referenced sampler's params (same conventions). */
void la_model_get_sampler_info(const la_model *model, uint32_t index,
                               la_sampler_info *out);

/** Model-space bounds (zeros for NULL; `out` may be NULL). */
void la_model_get_bounds(const la_model *model, lr_bounds *out);

/** Source path copy (model-owned; "" when unknown, NULL model safe
 *  via "" return? Returns NULL for NULL model). */
const char *la_model_get_source_path(const la_model *model);

/* ------------------------------------------------------------------
 * Skin + animation queries (Phase 29; decoded, engine-ready).
 *
 * Skins map 1:1 to the file's skin list (file order). Each skin
 * owns its joint-node list and its inverse-bind matrices
 * (column-major, decoded ONCE at import from the skin's
 * inverseBindMatrices accessor; identity per joint when the file
 * omits the accessor). The engine derives skeleton parents from
 * the node hierarchy and bind TRS from node local transforms
 * (see docs/GLTF_ANIMATION_IMPORT.md).
 *
 * Animations map to the file's animation list MINUS morph-only
 * leftovers: morph-target (weights) channels are skipped at
 * import (deferred feature, never silent — the engine reports
 * surviving-track counts), and an animation left with zero
 * importable channels is excluded from the count entirely.
 *
 * Lifetimes: all returned pointers borrow model storage (valid
 * while the model lives). Every query is NULL-safe; out-of-range
 * indices yield documented zeros (counts), NULL (borrowed
 * pointers), -1 (node/skin links), identity (bad inverse bind),
 * or 0 (bad channel/duration/bad-args).
 * ------------------------------------------------------------------ */

/** Skin count (0 for NULL model, or a model whose file names no
 *  skins). */
uint32_t la_model_get_skin_count(const la_model *model);

/** Joint count for one skin (0 for NULL model or bad skin). */
uint32_t la_model_get_skin_joint_count(const la_model *model,
                                       uint32_t skin_index);

/** Model node index of one joint (file order; -1 for NULL model,
 *  bad skin, or bad joint). */
int32_t la_model_get_skin_joint_node(const la_model *model,
                                     uint32_t skin_index,
                                     uint32_t joint);

/** Copy out one joint's inverse-bind matrix (column-major). Bad
 *  skin/joint (or NULL model) yields identity; `out_inv_bind`
 *  may be NULL for a no-op. */
void la_model_get_skin_inverse_bind(const la_model *model,
                                    uint32_t skin_index,
                                    uint32_t joint,
                                    float out_inv_bind[16]);

/** Skin used by one node (file-order skin index, -1 when the
 *  node is unskinned; -1 for NULL model or bad node). */
int32_t la_model_get_node_skin(const la_model *model,
                               uint32_t node_index);

/** Imported animation count (morph-only animations excluded; 0
 *  for NULL model). */
uint32_t la_model_get_animation_count(const la_model *model);

/** Importable channel count for one animation (0 for NULL model
 *  or bad animation). */
uint32_t la_model_get_animation_channel_count(const la_model *model,
                                              uint32_t anim_index);

/* One decoded animation channel. `path`: 0 = translation (vec3),
 * 1 = rotation (quat xyzw, vec4), 2 = scale (vec3).
 * `interpolation`: 0 = STEP, 1 = LINEAR, 2 = CUBICSPLINE.
 * `times` holds `key_count` seconds (>= 0, non-decreasing).
 * `values` holds `key_count` vectors of `comps` floats — EXCEPT
 * under CUBICSPLINE, where each key expands to an in/value/out
 * Hermite triple, so `values` holds `key_count * 3` vectors
 * (3x the floats; the engine scales tangents by the key
 * interval at sample time). Both arrays borrow model storage. */
typedef struct la_anim_channel {
    int32_t target_node; /* model node index */
    uint32_t path; /* 0 = T, 1 = R, 2 = S */
    uint32_t interpolation; /* 0 = STEP, 1 = LINEAR, 2 = CUBICSPLINE */
    uint32_t key_count;
    const float *times; /* [key_count] */
    const float *values; /* [key_count * comps] (x3 vectors if cubic) */
} la_anim_channel;

/** Borrow one channel's decoded keys (1 on success with `out`
 *  filled; 0 for NULL model, bad indices, or NULL out). */
int la_model_get_animation_channel(const la_model *model,
                                   uint32_t anim_index,
                                   uint32_t channel_index,
                                   la_anim_channel *out);

/** Animation duration in seconds (max last-key time across the
 *  animation's imported channels; 0 for NULL model, bad index,
 *  or an empty animation). */
float la_model_get_animation_duration(const la_model *model,
                                      uint32_t anim_index);

/**
 * Submit every mesh instance with hierarchy-composed world matrices:
 * world = parent_world * local, depth-first in import order
 * (deterministic). `root_transform` (may be NULL for identity) seeds
 * the traversal. Issues renderer submissions only — never raw LumaC
 * commands (Assets -> Renderer -> LumaC).
 *
 * @return LA_SUCCESS, LA_ERROR_INVALID_ARGUMENT (NULL model or
 *         renderer, dead model, foreign renderer), LA_ERROR_RENDER
 *         (submission failure).
 */
la_result la_model_submit(const la_model *model, lr_renderer *renderer,
                          const lr_transform *root_transform);

/* ------------------------------------------------------------------
 * Pure geometry utilities (no GPU, headless-testable).
 * ------------------------------------------------------------------ */

/**
 * Area-weighted per-vertex normals over indexed triangles; degenerate
 * triangles contribute nothing (no NaNs); fully degenerate fans fall
 * back to +Y. `positions` holds vertex_count xyz triples, `indices`
 * holds index_count triplets, `out_normals` receives xyz triples.
 */
la_result la_compute_normals(const float *positions, uint32_t vertex_count,
                             const uint32_t *indices, uint32_t index_count,
                             float *out_normals);

/**
 * UV-driven tangents (Lengyel-style accumulation + Gram-Schmidt +
 * handedness in w). Requires normals + uvs; degenerate UV triangles
 * are skipped (affected vertices keep accumulating from neighbors,
 * else fall back to +X/unit). MikkTSpace conformance is explicitly
 * deferred (see docs/ASSET_ARCHITECTURE.md).
 */
la_result la_compute_tangents(const float *positions, const float *normals,
                              const float *uvs, uint32_t vertex_count,
                              const uint32_t *indices, uint32_t index_count,
                              float *out_tangents);

/**
 * Decompose a column-major matrix into T*R*S (inverse of
 * lr_transform_to_matrix up to the determinant-sign convention:
 * negative determinants absorb one mirror into scale.x, and
 * rotation recovery holds up to float32 rounding — the stored
 * matrix stays the source of truth, so error never accumulates).
 * Returns INVALID_ARGUMENT on NULL.
 */
la_result la_matrix_to_transform(const float matrix[16],
                                 lr_transform *out_transform);

/* ------------------------------------------------------------------
 * HDR environment images (Phase 17): Radiance .hdr only, no image
 * framework. Files decode to tightly packed RGBA float32 (alpha
 * forced to 1), upload as RGBA16_FLOAT GPU images (single level,
 * sampled linearly by environment preprocessing). Non-HDR bytes
 * are rejected (UNSUPPORTED), never silently regraded.
 * ------------------------------------------------------------------ */

/* Read-only HDR reference for inspection (manager-owned storage;
 * valid while the manager lives). */
typedef struct la_hdr_image_info {
    uint32_t width;
    uint32_t height;
    const char *source;
} la_hdr_image_info;

/**
 * Decode .hdr bytes to tightly packed RGBA float32 (alpha = 1).
 * Rejects non-HDR content (stbi_is_hdr gate) and empty/degenerate
 * images. Caller frees with la_hdr_decode_free.
 *
 * @return LA_SUCCESS, LA_ERROR_INVALID_ARGUMENT (NULL/empty args),
 *         LA_ERROR_UNSUPPORTED (not Radiance HDR),
 *         LA_ERROR_IMPORT (corrupt pixels), LA_ERROR_OUT_OF_MEMORY.
 */
la_result la_hdr_decode(const unsigned char *bytes, size_t size,
                        float **out_rgba, uint32_t *out_width,
                        uint32_t *out_height);

/** Free a decoded HDR buffer (NULL-safe). */
void la_hdr_decode_free(float *rgba);

/**
 * IEEE-754 binary32 -> binary16, round-to-nearest-even. Inf maps
 * to Inf, NaN to canonical quiet NaN, finite overflow to Inf
 * (standard range behavior, no silent clamping).
 */
uint16_t la_float_to_half(float value);

/** Exact binary16 -> binary32 inverse (subnormals included). */
float la_half_to_float(uint16_t bits);

/**
 * Load a Radiance .hdr file through `manager`: decode, convert to
 * half float, upload as RGBA16_FLOAT (SAMPLED + TRANSFER_DST,
 * single mip level). Cached by path (repeat loads share one GPU
 * image); `out_info` (may be NULL) receives dimensions + source.
 *
 * @return LA_SUCCESS, LA_ERROR_INVALID_ARGUMENT (NULL args),
 *         LA_ERROR_NOT_FOUND (missing file),
 *         LA_ERROR_UNSUPPORTED (not HDR),
 *         LA_ERROR_IMPORT (corrupt), LA_ERROR_OUT_OF_MEMORY,
 *         LA_ERROR_RENDER (GPU upload failure).
 */
la_result la_hdr_load(la_asset_manager *manager, const char *path,
                      la_hdr_image_info *out_info);

/** Borrow a loaded HDR image's GPU view for environment sources
 *  (NULL for NULL manager/path or a path never loaded). */
lc_image_view *la_hdr_get_view(la_asset_manager *manager,
                               const char *path);

/** Borrowed view lifetime follows the manager (valid while the
 *  manager lives; destroyed with it). */

#ifdef __cplusplus
}
#endif

#endif /* LUMA_ASSETS_H */
