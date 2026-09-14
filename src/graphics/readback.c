/* Public image readback (Phase 18): validation + sizing + copy.
 *
 * Query computes the deterministic tight CPU layout without touching
 * the GPU. Readback validates loudly (never silent zeros) then
 * delegates to the Vulkan backend staging copy.
 */

#include <stdlib.h>
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
    lc_readback_request *request = NULL;

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
    /* Phase 20: the synchronous API is one policy wrapper over the
     * reusable async implementation, not a second copy path with a
     * device-wide idle. */
    res = lc_image_readback_async(image->device, image, desc, &request);
    if (res == LC_SUCCESS) {
        res = lc_readback_request_wait(request, LC_TIMEOUT_INFINITE);
    }
    if (res == LC_SUCCESS) {
        res = lc_readback_request_map(request, dst, dst_size,
                                      out_required_size);
    }
    lc_readback_request_destroy(request);
    return res;
}

/* ------------------------------------------------------------------ */
/* Async readbacks (Phase 20): schedule validated by the transfer      */
/* engine; the request holds staged bytes until destroy.               */
/* ------------------------------------------------------------------ */

static int lc_async_ready(void) {
    lc_state *state = lc_get_internal_state();

    return (state != NULL && state->initialized) ? 1 : 0;
}

lc_result lc_image_readback_async(lc_device *device, lc_image *image,
                                  const lc_image_readback_desc *desc,
                                  lc_readback_request **out_request) {
    lc_result res;

    if (out_request != NULL) {
        *out_request = NULL;
    }
    if (device == NULL || image == NULL || desc == NULL ||
        out_request == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_async_ready()) {
        return LC_ERROR_NOT_INITIALIZED;
    }
    res = lc_vk_transfer_readback_image(device, image, desc->mip_level,
                                        desc->array_layer, out_request);
    if (res == LC_SUCCESS) {
        lc_vk_reclaim_completed(device);
    }
    return res;
}

int lc_readback_request_is_ready(lc_readback_request *request) {
    if (request == NULL || request->device == NULL) {
        return 0;
    }
    return lc_vk_signal_ready(request->device, request->ready_value);
}

lc_result lc_readback_request_wait(lc_readback_request *request,
                                   uint64_t timeout_ns) {
    lc_result res;

    if (request == NULL || request->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    res = lc_vk_signal_wait(request->device, request->ready_value,
                            timeout_ns);
    if (res == LC_SUCCESS) {
        lc_vk_reclaim_completed(request->device);
    }
    return res;
}

lc_result lc_readback_request_map(lc_readback_request *request, void *dst,
                                  size_t dst_size,
                                  size_t *out_required_size) {
    lc_transfer_entry *entry = NULL;
    size_t need = 0;

    if (request == NULL || request->device == NULL ||
        request->entry == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    need = request->info.size;
    if (out_required_size != NULL) {
        *out_required_size = need;
    }
    if (dst == NULL || dst_size == 0) {
        return LC_SUCCESS;
    }
    if (dst_size < need || need == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Mapping requires completion; waiting is the caller's job. */
    if (!lc_vk_signal_ready(request->device, request->ready_value)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    entry = request->entry;
    if (entry->stage_mem.mapped == NULL) {
        return LC_ERROR_UNKNOWN;
    }
    if (lc_vk_mem_invalidate(request->device, &entry->stage_mem, 0,
                             entry->stage_bytes) != LC_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    memcpy(dst, entry->stage_mem.mapped, need);
    request->mapped = 1;
    return LC_SUCCESS;
}

void lc_readback_request_destroy(lc_readback_request *request) {
    if (request == NULL) {
        return;
    }
    if (request->device == NULL) {
        free(request);
        return;
    }
    lc_vk_request_discard(request->device, request);
}
