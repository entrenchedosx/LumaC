/*
 * Asset textures and samplers (Phase 14, PART O/Q/P).
 *
 * Decoded RGBA8 (stb_image, isolated TU) uploads through public LumaC
 * staging with full mipmap chains. Color-space role is carried by the
 * GPU format, never by manual gamma math: base-color/emissive content
 * uses RGBA8_SRGB, data content (normal, metallic-roughness,
 * occlusion) uses RGBA8_UNORM.
 *
 * Manager caches (insertion-ordered lists, never hash iteration):
 * identical source textures share one GPU image+view; identical
 * sampler parameters share one sampler. Refs count borrowing models.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_assets/luma_assets.h"
#include "internal/assets_internal.h"
#include "../../third_party/stb/stb_image.h"

/* Decode PNG/JPEG (or anything stb_image accepts) to tightly packed
 * RGBA8. Always 4 channels out (alpha forced opaque when absent). */
la_result la_decode_rgba(const unsigned char *bytes, size_t size,
                         unsigned char **out_rgba, uint32_t *out_width,
                         uint32_t *out_height) {
    int w = 0;
    int h = 0;
    int comp = 0;
    unsigned char *pixels;

    if (bytes == NULL || size == 0 || out_rgba == NULL ||
        out_width == NULL || out_height == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    *out_rgba = NULL;
    *out_width = 0;
    *out_height = 0;
    pixels = stbi_load_from_memory(bytes, (int)size, &w, &h, &comp, 4);
    if (pixels == NULL || w <= 0 || h <= 0) {
        return LA_ERROR_IMPORT;
    }
    *out_rgba = pixels;
    *out_width = (uint32_t)w;
    *out_height = (uint32_t)h;
    return LA_SUCCESS;
}

void la_decode_free(unsigned char *rgba) {
    if (rgba != NULL) {
        stbi_image_free(rgba);
    }
}

static la_texture_asset *la_texture_find(la_asset_manager *manager,
                                         const char *key) {
    la_texture_asset *it;

    for (it = manager->textures; it != NULL; it = it->next) {
        if (strcmp(it->key, key) == 0) {
            return it;
        }
    }
    return NULL;
}

la_result la_texture_get_or_create(la_asset_manager *manager, const char *key,
                                   const unsigned char *rgba, uint32_t width,
                                   uint32_t height, int srgb,
                                   la_texture_asset **out_asset) {
    la_texture_asset *asset;
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_image_upload_desc upload;
    char *key_copy;

    if (manager == NULL || key == NULL || rgba == NULL || width == 0 ||
        height == 0 || out_asset == NULL) {
        if (out_asset != NULL) {
            *out_asset = NULL;
        }
        return LA_ERROR_INVALID_ARGUMENT;
    }
    asset = la_texture_find(manager, key);
    if (asset != NULL) {
        asset->refs++;
        *out_asset = asset;
        return LA_SUCCESS;
    }

    asset = (la_texture_asset *)calloc(1, sizeof(la_texture_asset));
    if (asset == NULL) {
        *out_asset = NULL;
        return LA_ERROR_OUT_OF_MEMORY;
    }
    key_copy = (char *)malloc(strlen(key) + 1);
    if (key_copy == NULL) {
        free(asset);
        *out_asset = NULL;
        return LA_ERROR_OUT_OF_MEMORY;
    }
    memcpy(key_copy, key, strlen(key) + 1);
    asset->key = key_copy;
    asset->width = width;
    asset->height = height;
    asset->srgb = (srgb != 0) ? 1 : 0;

    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format =
        asset->srgb ? LC_FORMAT_RGBA8_SRGB : LC_FORMAT_RGBA8_UNORM;
    idesc.width = width;
    idesc.height = height;
    idesc.depth = 1;
    idesc.mip_levels = 0; /* full chain */
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_SRC |
                  LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(lr_renderer_get_device(manager->renderer), &idesc,
                        &asset->image) != LC_SUCCESS) {
        free(asset->key);
        free(asset);
        *out_asset = NULL;
        return LA_ERROR_RENDER;
    }
    memset(&upload, 0, sizeof(upload));
    upload.mip_level = 0;
    upload.array_layer = 0;
    upload.width = width;
    upload.height = height;
    upload.depth = 1;
    upload.data = rgba;
    upload.data_size = (uint64_t)width * height * 4u;
    if (lc_image_write(asset->image, &upload) != LC_SUCCESS ||
        lc_image_generate_mipmaps(asset->image) != LC_SUCCESS) {
        lc_image_destroy(asset->image);
        free(asset->key);
        free(asset);
        *out_asset = NULL;
        return LA_ERROR_RENDER;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.base_mip_level = 0;
    vdesc.mip_level_count = lc_image_get_mip_levels(asset->image);
    vdesc.base_array_layer = 0;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(asset->image, &vdesc, &asset->view) !=
        LC_SUCCESS) {
        lc_image_destroy(asset->image);
        free(asset->key);
        free(asset);
        *out_asset = NULL;
        return LA_ERROR_RENDER;
    }
    asset->refs = 1;
    asset->next = manager->textures;
    manager->textures = asset;
    *out_asset = asset;
    return LA_SUCCESS;
}

void la_texture_release(la_texture_asset *asset) {
    if (asset == NULL || asset->refs == 0) {
        return;
    }
    asset->refs--;
    /* Storage reclamation happens at manager teardown (PART AF keeps
     * cache ownership simple and deterministic). */
}

static int la_sampler_params_equal(const lc_sampler_desc *a,
                                   const lc_sampler_desc *b) {
    return a->min_filter == b->min_filter && a->mag_filter == b->mag_filter &&
           a->mipmap_mode == b->mipmap_mode && a->address_u == b->address_u &&
           a->address_v == b->address_v && a->address_w == b->address_w &&
           a->mip_lod_bias == b->mip_lod_bias && a->min_lod == b->min_lod &&
           a->max_lod == b->max_lod &&
           a->max_anisotropy == b->max_anisotropy;
}

la_result la_sampler_get_or_create(la_asset_manager *manager,
                                   const lc_sampler_desc *params,
                                   la_sampler_asset **out_asset) {
    la_sampler_asset *asset;

    if (manager == NULL || params == NULL || out_asset == NULL) {
        if (out_asset != NULL) {
            *out_asset = NULL;
        }
        return LA_ERROR_INVALID_ARGUMENT;
    }
    for (asset = manager->samplers; asset != NULL; asset = asset->next) {
        if (la_sampler_params_equal(&asset->params, params)) {
            asset->refs++;
            *out_asset = asset;
            return LA_SUCCESS;
        }
    }
    asset = (la_sampler_asset *)calloc(1, sizeof(la_sampler_asset));
    if (asset == NULL) {
        *out_asset = NULL;
        return LA_ERROR_OUT_OF_MEMORY;
    }
    asset->params = *params;
    if (lc_sampler_create(lr_renderer_get_device(manager->renderer),
                          &asset->params, &asset->sampler) != LC_SUCCESS) {
        free(asset);
        *out_asset = NULL;
        return LA_ERROR_RENDER;
    }
    asset->refs = 1;
    asset->next = manager->samplers;
    manager->samplers = asset;
    *out_asset = asset;
    return LA_SUCCESS;
}

void la_sampler_release(la_sampler_asset *asset) {
    if (asset == NULL || asset->refs == 0) {
        return;
    }
    asset->refs--;
}

/* glTF sampler mapping (PART P). Unspecified fields follow the glTF
 * 2.0 defaults (LINEAR mag, LINEAR_MIPMAP_LINEAR min, REPEAT wraps).
 * LumaC carries one min filter plus a mip mode; the six glTF min
 * filters fold onto the four combinations losslessly. */
la_result la_sampler_from_gltf(const cgltf_sampler *sampler,
                               lc_sampler_desc *out_params) {
    cgltf_filter_type mag = cgltf_filter_type_linear;
    cgltf_filter_type min = cgltf_filter_type_linear_mipmap_linear;
    cgltf_wrap_mode wrap_s = cgltf_wrap_mode_repeat;
    cgltf_wrap_mode wrap_t = cgltf_wrap_mode_repeat;

    if (out_params == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (sampler != NULL) {
        mag = sampler->mag_filter;
        min = sampler->min_filter;
        wrap_s = sampler->wrap_s;
        wrap_t = sampler->wrap_t;
    }
    memset(out_params, 0, sizeof(*out_params));
    out_params->mag_filter = (mag == cgltf_filter_type_nearest)
                                 ? LC_FILTER_NEAREST
                                 : LC_FILTER_LINEAR;
    switch (min) {
    case cgltf_filter_type_nearest:
        out_params->min_filter = LC_FILTER_NEAREST;
        out_params->mipmap_mode = LC_MIPMAP_MODE_NEAREST;
        break;
    case cgltf_filter_type_linear:
        out_params->min_filter = LC_FILTER_LINEAR;
        out_params->mipmap_mode = LC_MIPMAP_MODE_NEAREST;
        break;
    case cgltf_filter_type_nearest_mipmap_nearest:
        out_params->min_filter = LC_FILTER_NEAREST;
        out_params->mipmap_mode = LC_MIPMAP_MODE_NEAREST;
        break;
    case cgltf_filter_type_linear_mipmap_nearest:
        out_params->min_filter = LC_FILTER_LINEAR;
        out_params->mipmap_mode = LC_MIPMAP_MODE_NEAREST;
        break;
    case cgltf_filter_type_nearest_mipmap_linear:
        out_params->min_filter = LC_FILTER_NEAREST;
        out_params->mipmap_mode = LC_MIPMAP_MODE_LINEAR;
        break;
    case cgltf_filter_type_linear_mipmap_linear:
    default:
        /* Also covers wire value 0 ("unspecified"), which the glTF
         * spec defaults exactly this way. */
        out_params->min_filter = LC_FILTER_LINEAR;
        out_params->mipmap_mode = LC_MIPMAP_MODE_LINEAR;
        break;
    }
    switch (wrap_s) {
    case cgltf_wrap_mode_clamp_to_edge:
        out_params->address_u = LC_ADDRESS_CLAMP_TO_EDGE;
        break;
    case cgltf_wrap_mode_mirrored_repeat:
        out_params->address_u = LC_ADDRESS_MIRRORED_REPEAT;
        break;
    case cgltf_wrap_mode_repeat:
    default:
        /* Unspecified (0) defaults to REPEAT per spec. */
        out_params->address_u = LC_ADDRESS_REPEAT;
        break;
    }
    switch (wrap_t) {
    case cgltf_wrap_mode_clamp_to_edge:
        out_params->address_v = LC_ADDRESS_CLAMP_TO_EDGE;
        break;
    case cgltf_wrap_mode_mirrored_repeat:
        out_params->address_v = LC_ADDRESS_MIRRORED_REPEAT;
        break;
    case cgltf_wrap_mode_repeat:
    default:
        out_params->address_v = LC_ADDRESS_REPEAT;
        break;
    }
    out_params->address_w = LC_ADDRESS_CLAMP_TO_EDGE;
    out_params->mip_lod_bias = 0.0f;
    out_params->min_lod = 0.0f;
    out_params->max_lod = 32.0f;
    out_params->max_anisotropy = 1.0f;
    return LA_SUCCESS;
}
