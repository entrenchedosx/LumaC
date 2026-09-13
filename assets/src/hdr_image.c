/*
 * HDR environment sources (Phase 17): Radiance .hdr only.
 *
 * Files decode through stb_image float loading to tightly packed
 * RGBA float32 (alpha forced to 1), convert to IEEE half on the
 * CPU, and upload as RGBA16_FLOAT GPU images (single mip level;
 * environment preprocessing samples LOD 0 with linear filtering).
 * Non-HDR bytes are rejected, never silently regraded. Manager
 * caches by path; views are borrowed for renderer environments.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_assets/luma_assets.h"
#include "internal/assets_internal.h"
#include "../../third_party/stb/stb_image.h"

la_result la_hdr_decode(const unsigned char *bytes, size_t size,
                        float **out_rgba, uint32_t *out_width,
                        uint32_t *out_height) {
    int w = 0;
    int h = 0;
    int comp = 0;
    float *pixels;

    if (bytes == NULL || size == 0 || out_rgba == NULL ||
        out_width == NULL || out_height == NULL) {
        return LA_ERROR_INVALID_ARGUMENT;
    }
    *out_rgba = NULL;
    *out_width = 0;
    *out_height = 0;
    if (size > (size_t)0x7fffffff ||
        !stbi_is_hdr_from_memory(bytes, (int)size)) {
        return LA_ERROR_UNSUPPORTED;
    }
    pixels = stbi_loadf_from_memory(bytes, (int)size, &w, &h, &comp, 4);
    if (pixels == NULL || w <= 0 || h <= 0) {
        return LA_ERROR_IMPORT;
    }
    *out_rgba = pixels;
    *out_width = (uint32_t)w;
    *out_height = (uint32_t)h;
    return LA_SUCCESS;
}

void la_hdr_decode_free(float *rgba) {
    if (rgba != NULL) {
        stbi_image_free(rgba);
    }
}

/* IEEE-754 binary32 -> binary16, round-to-nearest-even. Follows the
 * classic bit-manipulation formulation: exponent rebias with
 * mantissa rounding, subnormal packing on underflow, Inf/NaN
 * preserved per standard range behavior. */
uint16_t la_float_to_half(float value) {
    uint32_t bits;
    uint32_t sign;
    int32_t exp;
    uint32_t mant;
    uint32_t half;

    memcpy(&bits, &value, sizeof(bits));
    sign = (bits >> 16) & 0x8000u;
    exp = (int32_t)((bits >> 23) & 0xffu) - 112; /* 127 - 15 */
    mant = bits & 0x7fffffu;
    if (((bits >> 23) & 0xffu) == 0xffu) {
        /* True Inf or NaN inputs. */
        if (mant == 0) {
            return (uint16_t)(sign | 0x7c00u);
        }
        return (uint16_t)(sign | 0x7e00u); /* canonical quiet NaN */
    }
    if (exp >= 31) {
        /* Finite overflow past max half (65504). */
        return (uint16_t)(sign | 0x7c00u);
    }
    if (exp <= 0) {
        /* Subnormal or zero: pack the leading 1 explicitly. */
        uint32_t shifted;

        if (exp < -10) {
            return (uint16_t)sign;
        }
        mant |= 0x800000u;
        shifted = mant >> (uint32_t)(14 - exp);
        /* Round to nearest even on the dropped bits. */
        {
            uint32_t dropped = mant >> (uint32_t)(13 - exp);
            uint32_t keep = shifted & 1u;

            if ((dropped & 3u) == 3u ||
                ((dropped & 3u) == 2u && keep != 0u)) {
                shifted++;
            }
        }
        return (uint16_t)(sign | shifted);
    }
    /* Normalized: round mantissa to 10 bits, nearest even. */
    half = ((uint32_t)exp << 10) | (mant >> 13);
    {
        uint32_t round_bits = mant & 0x1fffu;

        if (round_bits > 0x1000u ||
            (round_bits == 0x1000u && (half & 1u) != 0u)) {
            half++;
        }
    }
    return (uint16_t)(sign | half);
}

float la_half_to_float(uint16_t bits) {
    uint32_t sign = ((uint32_t)bits & 0x8000u) << 16;
    uint32_t exp = ((uint32_t)bits >> 10) & 0x1fu;
    uint32_t mant = (uint32_t)bits & 0x3ffu;
    uint32_t out;

    if (exp == 0) {
        if (mant == 0) {
            out = sign;
        } else {
            /* Subnormal: normalize the mantissa. */
            uint32_t e = 127 - 14;

            while ((mant & 0x400u) == 0) {
                mant <<= 1;
                e--;
            }
            mant &= 0x3ffu;
            out = sign | (e << 23) | (mant << 13);
        }
    } else if (exp == 31) {
        out = sign | 0x7f800000u | (mant << 13);
    } else {
        out = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    {
        float value;

        memcpy(&value, &out, sizeof(value));
        return value;
    }
}

static la_hdr_asset *la_hdr_find(la_asset_manager *manager,
                                 const char *path) {
    la_hdr_asset *it;

    for (it = manager->hdr_images; it != NULL; it = it->next) {
        if (strcmp(it->key, path) == 0) {
            return it;
        }
    }
    return NULL;
}

la_hdr_asset *la_hdr_lookup(la_asset_manager *manager, const char *path) {
    if (manager == NULL || path == NULL) {
        return NULL;
    }
    return la_hdr_find(manager, path);
}

void la_hdr_teardown(la_asset_manager *manager) {
    la_hdr_asset *it;
    la_hdr_asset *next;

    if (manager == NULL) {
        return;
    }
    for (it = manager->hdr_images; it != NULL; it = next) {
        next = it->next;
        lc_image_view_destroy(it->view);
        lc_image_destroy(it->image);
        free(it->key);
        free(it);
    }
    manager->hdr_images = NULL;
}

la_result la_hdr_load(la_asset_manager *manager, const char *path,
                      la_hdr_image_info *out_info) {
    la_hdr_asset *asset;
    unsigned char *bytes = NULL;
    size_t size = 0;
    float *rgba = NULL;
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t *half = NULL;
    uint32_t count = 0;
    uint32_t i;
    char *key_copy;
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_image_upload_desc upload;

    if (manager == NULL || path == NULL) {
        if (out_info != NULL) {
            memset(out_info, 0, sizeof(*out_info));
        }
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (out_info != NULL) {
        memset(out_info, 0, sizeof(*out_info));
    }
    asset = la_hdr_find(manager, path);
    if (asset != NULL) {
        asset->refs++;
        if (out_info != NULL) {
            out_info->width = asset->width;
            out_info->height = asset->height;
            out_info->source = asset->key;
        }
        return LA_SUCCESS;
    }
    if (la_fs_read(path, &bytes, &size) != LA_SUCCESS) {
        return LA_ERROR_NOT_FOUND;
    }
    {
        la_result dec =
            la_hdr_decode(bytes, size, &rgba, &width, &height);

        la_fs_free(bytes);
        bytes = NULL;
        if (dec != LA_SUCCESS) {
            return dec;
        }
    }
    count = width * height * 4u;
    half = (uint16_t *)malloc((size_t)count * sizeof(uint16_t));
    if (half == NULL) {
        la_hdr_decode_free(rgba);
        return LA_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < count; i++) {
        half[i] = la_float_to_half(rgba[i]);
    }
    la_hdr_decode_free(rgba);
    rgba = NULL;

    asset = (la_hdr_asset *)calloc(1, sizeof(la_hdr_asset));
    if (asset == NULL) {
        free(half);
        return LA_ERROR_OUT_OF_MEMORY;
    }
    key_copy = (char *)malloc(strlen(path) + 1);
    if (key_copy == NULL) {
        free(half);
        free(asset);
        return LA_ERROR_OUT_OF_MEMORY;
    }
    memcpy(key_copy, path, strlen(path) + 1);
    asset->key = key_copy;
    asset->width = width;
    asset->height = height;

    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RGBA16_FLOAT;
    idesc.width = width;
    idesc.height = height;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_SRC |
                  LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(lr_renderer_get_device(manager->renderer),
                        &idesc, &asset->image) != LC_SUCCESS) {
        free(half);
        free(asset->key);
        free(asset);
        return LA_ERROR_RENDER;
    }
    memset(&upload, 0, sizeof(upload));
    upload.mip_level = 0;
    upload.array_layer = 0;
    upload.width = width;
    upload.height = height;
    upload.depth = 1;
    upload.data = half;
    upload.data_size = (uint64_t)count * sizeof(uint16_t);
    if (lc_image_write(asset->image, &upload) != LC_SUCCESS) {
        free(half);
        lc_image_destroy(asset->image);
        free(asset->key);
        free(asset);
        return LA_ERROR_RENDER;
    }
    free(half);
    half = NULL;
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.base_mip_level = 0;
    vdesc.mip_level_count = 1;
    vdesc.base_array_layer = 0;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(asset->image, &vdesc, &asset->view) !=
        LC_SUCCESS) {
        lc_image_destroy(asset->image);
        free(asset->key);
        free(asset);
        return LA_ERROR_RENDER;
    }
    asset->refs = 1;
    asset->next = manager->hdr_images;
    manager->hdr_images = asset;
    if (out_info != NULL) {
        out_info->width = width;
        out_info->height = height;
        out_info->source = asset->key;
    }
    return LA_SUCCESS;
}

lc_image_view *la_hdr_get_view(la_asset_manager *manager,
                               const char *path) {
    la_hdr_asset *asset;

    if (manager == NULL || path == NULL) {
        return NULL;
    }
    asset = la_hdr_find(manager, path);
    return (asset != NULL) ? asset->view : NULL;
}
