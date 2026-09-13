/* Public image readback (Phase 18): validation + sizing + copy.
 *
 * Query computes the deterministic tight CPU layout without touching
 * the GPU. Readback validates loudly (never silent zeros) then
 * delegates to the Vulkan backend staging copy.
 */

#include <string.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

static int lc_readback_live_image(const lc_image *image) {
    lc_state *state = lc_get_internal_state();
    const lc_image *it;

    if (state == NULL || image == NULL) {
        return 0;
    }
    for (it = state->images; it != NULL; it = it->next) {
        if (it == image) {
            return 1;
        }
    }
    return 0;
}

/* Mip extent for one level (minimum 1 per axis). */
static void lc_readback_mip_extent(const lc_image *image, uint32_t mip,
                                   uint32_t *w, uint32_t *h, uint32_t *d) {
    *w = image->width >> mip;
    *h = image->height >> mip;
    *d = image->depth >> mip;
    if (*w == 0) {
        *w = 1;
    }
    if (*h == 0) {
        *h = 1;
    }
    if (*d == 0) {
        *d = 1;
    }
    if (image->type == LC_IMAGE_TYPE_1D) {
        *h = 1;
        *d = 1;
    } else if (image->type == LC_IMAGE_TYPE_2D) {
        *d = 1;
    }
}

/* Shared validation + layout computation. Returns LC_SUCCESS with
 * *out_w/h/d and *out_elem set, or an error (never touches GPU). */
static lc_result lc_readback_layout(const lc_image *image,
                                    const lc_image_readback_desc *desc,
                                    uint32_t *out_w, uint32_t *out_h,
                                    uint32_t *out_d, uint32_t *out_elem) {
    uint32_t elem = 0;

    if (image == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_readback_live_image(image)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->mip_level >= image->mip_levels ||
        desc->array_layer >= image->array_layers) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (image->samples != 1) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((image->usage & LC_IMAGE_USAGE_TRANSFER_SRC) == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    elem = lc_format_byte_size(image->format);
    if (elem == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    lc_readback_mip_extent(image, desc->mip_level, out_w, out_h, out_d);
    *out_elem = elem;
    return LC_SUCCESS;
}

lc_result lc_image_query_readback(const lc_image *image,
                                  const lc_image_readback_desc *desc,
                                  lc_image_readback_info *out_info) {
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t d = 0;
    uint32_t elem = 0;
    lc_result res;

    if (out_info == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(out_info, 0, sizeof(*out_info));
    res = lc_readback_layout(image, desc, &w, &h, &d, &elem);
    if (res != LC_SUCCESS) {
        return res;
    }
    out_info->width = w;
    out_info->height = h;
    out_info->format = image->format;
    out_info->row_pitch = (size_t)w * (size_t)elem;
    out_info->size = out_info->row_pitch * (size_t)h * (size_t)d;
    return LC_SUCCESS;
}

lc_result lc_image_readback(lc_image *image,
                            const lc_image_readback_desc *desc, void *dst,
                            size_t dst_size, size_t *out_required_size) {
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t d = 0;
    uint32_t elem = 0;
    lc_result res;
    size_t need = 0;

    res = lc_readback_layout(image, desc, &w, &h, &d, &elem);
    if (res != LC_SUCCESS) {
        return res;
    }
    need = (size_t)w * (size_t)elem * (size_t)h * (size_t)d;
    if (out_required_size != NULL) {
        *out_required_size = need;
    }
    /* Sizing query: no bytes written. */
    if (dst == NULL || dst_size == 0) {
        return LC_SUCCESS;
    }
    if (dst_size < need) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_image_readback(image, desc->mip_level,
                                    desc->array_layer, w, h, d, dst, need);
}
