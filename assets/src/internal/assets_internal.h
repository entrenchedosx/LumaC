#ifndef LUMA_ASSETS_INTERNAL_H
#define LUMA_ASSETS_INTERNAL_H

/*
 * Assets-private state (never public; may include vendored cgltf —
 * never backend, platform, or LumaC/renderer internals).
 */

#include <stddef.h>
#include <stdint.h>

#include "luma_assets/luma_assets.h"
#include "../../third_party/cgltf/cgltf.h"

/* One cached GPU texture (manager-owned, shared by key). */
typedef struct la_texture_asset {
    char *key; /* owned identity (path or embedded identity) */
    lc_image *image;
    lc_image_view *view;
    uint32_t width;
    uint32_t height;
    int srgb;
    uint32_t refs; /* borrowing models */
    struct la_texture_asset *next;
} la_texture_asset;

/* One cached GPU sampler (manager-owned, deduped by exact params). */
typedef struct la_sampler_asset {
    lc_sampler_desc params; /* normalized copy */
    lc_sampler *sampler;
    uint32_t refs;
    struct la_sampler_asset *next;
} la_sampler_asset;

/* One model mesh: ordered primitives sharing the glTF mesh.
 * `material` borrows the model's material slot (same index space as
 * the glTF file's material list for traceability). */
typedef struct la_model_primitive {
    lr_mesh *mesh; /* owned */
    uint32_t material_index; /* UINT32_MAX when unassigned */
} la_model_primitive;

typedef struct la_model_mesh {
    la_model_primitive *primitives; /* owned array */
    uint32_t primitive_count;
} la_model_mesh;

/* One model material: preserved PBR metadata + renderer instance. */
typedef struct la_model_material {
    la_pbr_material_data data;
    lr_material *material; /* owned */
} la_model_material;

/* One imported skin (file order): joint nodes + inverse-bind
 * matrices, both model-owned. `joint_nodes[j]` is a model node
 * index; `inv_bind` holds joint_count column-major 4x4 matrices
 * (identity per joint when the file omits inverseBindMatrices). */
typedef struct la_model_skin {
    int32_t *joint_nodes; /* owned [joint_count] */
    float *inv_bind; /* owned [joint_count * 16] */
    uint32_t joint_count;
} la_model_skin;

/* One imported animation channel: decoded key times + values,
 * both model-owned. CUBICSPLINE values hold key_count * 3
 * vectors (in/value/out triples); other modes hold key_count. */
typedef struct la_model_anim_channel {
    int32_t target_node;
    uint32_t path; /* 0 = T, 1 = R, 2 = S */
    uint32_t interpolation; /* 0 = STEP, 1 = LINEAR, 2 = CUBICSPLINE */
    uint32_t key_count;
    float *times; /* owned [key_count] */
    float *values; /* owned [key_count * comps] (x3 vectors if cubic) */
} la_model_anim_channel;

/* One imported animation (morph-only animations never reach
 * here — they are excluded at import). `duration` is the max
 * last-key time across kept channels. */
typedef struct la_model_animation {
    la_model_anim_channel *channels; /* owned [channel_count] */
    uint32_t channel_count;
    float duration;
} la_model_animation;

/* One cached HDR environment source (manager-owned, shared by
 * path). Half-float GPU image, single mip level, sampled by
 * renderer-side equirectangular preprocessing. */
typedef struct la_hdr_asset {
    char *key; /* owned identity (path as written) */
    lc_image *image;
    lc_image_view *view;
    uint32_t width;
    uint32_t height;
    uint32_t refs;
    struct la_hdr_asset *next;
} la_hdr_asset;

struct la_asset_manager {
    lr_renderer *renderer; /* borrowed; must outlive the manager */
    la_texture_asset *textures;
    la_sampler_asset *samplers;
    la_hdr_asset *hdr_images;
    la_model *models;
    char last_error[256];
};

struct la_model {
    la_asset_manager *manager; /* borrowed back-pointer */
    char *source_path; /* owned copy */
    la_model_node *nodes; /* owned, file order */
    uint32_t node_count;
    uint32_t *child_links; /* owned flat children */
    uint32_t child_link_count;
    la_model_mesh *meshes; /* owned */
    uint32_t mesh_count;
    la_model_material *materials; /* owned */
    uint32_t material_count;
    la_model_skin *skins; /* owned, file order */
    uint32_t skin_count;
    la_model_animation *anims; /* owned (morph-only excluded) */
    uint32_t anim_count;
    la_texture_asset **textures; /* borrowed refs */
    uint32_t texture_count;
    la_sampler_asset **samplers; /* borrowed refs */
    uint32_t sampler_count;
    uint32_t source_primitive_count;
    uint32_t source_texture_count;
    uint32_t source_sampler_count;
    uint32_t instance_count;
    lr_bounds bounds;
    la_model *next;
    la_model *prev;
};

/* Error buffer helper (printf-style, always NUL-terminated). */
void la_set_error(la_asset_manager *manager, const char *fmt, ...);

/* Result mapping (lc_result/lr_result -> la_result). */
la_result la_map_lc(lc_result res);
la_result la_map_lr(lr_result res);

/* Centralized filesystem helpers (PART AH: all file access flows
 * through these; future packaging replaces them in one place). */
la_result la_fs_read(const char *path, unsigned char **out_bytes,
                     size_t *out_size);
void la_fs_free(unsigned char *bytes);
la_result la_fs_join(const char *dir, const char *leaf, char **out_path);
la_result la_fs_dirname(const char *path, char **out_dir);

/* glTF import entry (gltf_import.c): parse + validate + instantiate.
 * Takes ownership of nothing on failure (partial state unwound). */
la_result la_import_gltf(la_asset_manager *manager, const char *path,
                         const unsigned char *bytes, size_t size,
                         la_model **out_model);

/* Texture pipeline (texture.c). `srgb` selects the GPU format role;
 * `key` is copied. Returns a borrowed cache entry (refs++). */
la_result la_texture_get_or_create(la_asset_manager *manager, const char *key,
                                   const unsigned char *rgba, uint32_t width,
                                   uint32_t height, int srgb,
                                   la_texture_asset **out_asset);
void la_texture_release(la_texture_asset *asset);

/* Image decoding (texture.c, stb_image isolated to its own TU). */
la_result la_decode_rgba(const unsigned char *bytes, size_t size,
                         unsigned char **out_rgba, uint32_t *out_width,
                         uint32_t *out_height);
void la_decode_free(unsigned char *rgba);

/* HDR pipeline (hdr_image.c). Manager-owned cache indexed by path;
 * views are borrowed for renderer environment sources. */
void la_hdr_teardown(la_asset_manager *manager);

/* Borrow a cached HDR entry for white-box test inspection (NULL
 * when absent; lifetime follows the manager). */
la_hdr_asset *la_hdr_lookup(la_asset_manager *manager, const char *path);

/* Sampler pipeline (texture.c). Params are normalized then deduped. */
la_result la_sampler_get_or_create(la_asset_manager *manager,
                                   const lc_sampler_desc *params,
                                   la_sampler_asset **out_asset);
void la_sampler_release(la_sampler_asset *asset);
la_result la_sampler_from_gltf(const cgltf_sampler *sampler,
                               lc_sampler_desc *out_params);

/* Model helpers (model.c). */
void la_model_teardown(la_model *model);

/* Skin-vertex decode helpers (gltf_import.c; white-box tested).
 * la_decode_joints widens a JOINTS_0 accessor (VEC4 u8/u16,
 * UNnormalized) to uint32 per component. la_normalize_weights
 * rescales one vertex's 4 weights to sum 1 (all-zero sum falls
 * back to {1,0,0,0}; never NaN). la_decode_skin_vertex runs both
 * (NULL joints/weights accessors select rigid defaults) and is
 * the single policy point used by primitive import. */
la_result la_decode_joints(const cgltf_accessor *accessor,
                           uint32_t vertex_count, uint32_t *out);
void la_normalize_weights(float w[4]);
la_result la_decode_skin_vertex(const cgltf_accessor *joints,
                                const cgltf_accessor *weights,
                                uint32_t vertex_count, uint32_t *out_joints,
                                float *out_weights);

#endif /* LUMA_ASSETS_INTERNAL_H */
