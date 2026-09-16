/*
 * Renderer Hi-Z depth pyramid (Phase 23).
 *
 * A dedicated R32F image with a complete mip chain shadows the
 * main-scene depth buffer (standard-Z: near 0, far 1, LESS, clear
 * 1.0). Mip 0 is seeded from the previous frame's HDR depth by a
 * compute copy; higher mips are MAX reductions (farthest depth
 * wins, conservative for occlusion). Generation records into the
 * open frame encoder outside any pass, using per-mip semantic
 * transitions (SHADER_READ_WRITE while producing, SHADER_READ
 * once sampled). The image persists across frames (one frame of
 * occlusion latency, by design); extent changes rebuild it through
 * normal retirement (no global idle).
 */

#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

extern const unsigned char lr_hiz_copy_comp_spv[];
extern const unsigned long lr_hiz_copy_comp_spv_size;
extern const unsigned char lr_hiz_reduce_comp_spv[];
extern const unsigned long lr_hiz_reduce_comp_spv_size;

#define LR_HIZ_MAX_MIPS 16u

struct lr_hiz {
    lc_image *image;
    lc_image_view *views[LR_HIZ_MAX_MIPS];
    lc_image_view *sample_view; /* full chain (occlusion reads) */
    uint32_t width;
    uint32_t height;
    uint32_t mips;
};

uint32_t lr_hiz_mip_count(uint32_t width, uint32_t height) {
    uint32_t extent = (width > height) ? width : height;
    uint32_t mips = 1;

    if (extent == 0) {
        return 1;
    }
    /* floor(log2(max)) + 1: the complete chain down to 1x1. */
    while (extent > 1u) {
        extent >>= 1;
        mips++;
    }
    return mips;
}

uint32_t lr_hiz_width(const lr_renderer *renderer) {
    if (renderer == NULL || renderer->hiz == NULL) {
        return 0;
    }
    return renderer->hiz->width;
}

uint32_t lr_hiz_height(const lr_renderer *renderer) {
    if (renderer == NULL || renderer->hiz == NULL) {
        return 0;
    }
    return renderer->hiz->height;
}

uint32_t lr_hiz_levels(const lr_renderer *renderer) {
    if (renderer == NULL || renderer->hiz == NULL) {
        return 0;
    }
    return renderer->hiz->mips;
}

lc_image *lr_hiz_image(const lr_renderer *renderer) {
    if (renderer == NULL || renderer->hiz == NULL) {
        return NULL;
    }
    return renderer->hiz->image;
}

lc_image_view *lr_hiz_view(const lr_renderer *renderer, uint32_t mip) {
    if (renderer == NULL || renderer->hiz == NULL) {
        return NULL;
    }
    if (mip >= renderer->hiz->mips || mip >= LR_HIZ_MAX_MIPS) {
        return NULL;
    }
    return renderer->hiz->views[mip];
}

lc_image_view *lr_hiz_sample_view(const lr_renderer *renderer) {
    if (renderer == NULL || renderer->hiz == NULL) {
        return NULL;
    }
    return renderer->hiz->sample_view;
}

lc_image_view *lr_renderer_get_hiz_view(lr_renderer *renderer,
                                        uint32_t mip) {
    if (renderer == NULL) {
        return NULL;
    }
    return lr_hiz_view(renderer, mip);
}

uint32_t lr_renderer_get_hiz_mip_count(const lr_renderer *renderer) {
    if (renderer == NULL) {
        return 0;
    }
    return lr_hiz_levels(renderer);
}

void lr_hiz_destroy(lr_renderer *renderer) {
    uint32_t i;

    if (renderer == NULL || renderer->hiz == NULL) {
        return;
    }
    /* Views die before their image (borrow contract). Image
     * destroy retires GPU objects; no global idle. */
    lc_image_view_destroy(renderer->hiz->sample_view);
    renderer->hiz->sample_view = NULL;
    for (i = 0; i < renderer->hiz->mips && i < LR_HIZ_MAX_MIPS; i++) {
        lc_image_view_destroy(renderer->hiz->views[i]);
        renderer->hiz->views[i] = NULL;
    }
    lc_image_destroy(renderer->hiz->image);
    free(renderer->hiz);
    renderer->hiz = NULL;
}

void lr_hiz_free_reduce_sets(lr_renderer *renderer) {
    uint32_t mips;
    uint32_t m;

    if (renderer == NULL ||
        renderer->vis_hiz_reduce_sets == NULL) {
        return;
    }
    mips = lr_hiz_levels(renderer);
    for (m = 0; m < mips; m++) {
        lc_binding_set_destroy(renderer->vis_hiz_reduce_sets[m]);
    }
    free(renderer->vis_hiz_reduce_sets);
    renderer->vis_hiz_reduce_sets = NULL;
}

lr_result lr_hiz_ensure(lr_renderer *renderer, uint32_t width,
                        uint32_t height) {
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    uint32_t mips;
    uint32_t i;

    if (renderer == NULL || width == 0 || height == 0) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->hiz != NULL && renderer->hiz->width == width &&
        renderer->hiz->height == height) {
        return LR_SUCCESS;
    }
    mips = lr_hiz_mip_count(width, height);
    if (mips > LR_HIZ_MAX_MIPS) {
        return LR_ERROR_UNSUPPORTED;
    }
    /* View-dependent sets die with the views they reference. */
    lr_hiz_free_reduce_sets(renderer);
    lr_hiz_destroy(renderer);
    renderer->hiz = (lr_hiz *)calloc(1, sizeof(lr_hiz));
    if (renderer->hiz == NULL) {
        return LR_ERROR_OUT_OF_MEMORY;
    }
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_R32_FLOAT;
    idesc.width = width;
    idesc.height = height;
    idesc.depth = 1;
    idesc.mip_levels = mips;
    idesc.array_layers = 1;
    /* Compute-written, compute/vertex-sampled, CPU-verified:
     * STORAGE for the pyramid passes, SAMPLED for culling reads,
     * TRANSFER_SRC for test/debug readback. Never a render
     * target (no attachment aliasing with scene depth). */
    idesc.usage = (uint32_t)LC_IMAGE_USAGE_SAMPLED |
                  (uint32_t)LC_IMAGE_USAGE_STORAGE |
                  (uint32_t)LC_IMAGE_USAGE_TRANSFER_SRC;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(renderer->device, &idesc,
                        &renderer->hiz->image) != LC_SUCCESS) {
        free(renderer->hiz);
        renderer->hiz = NULL;
        return LR_ERROR_RENDER;
    }
    for (i = 0; i < mips; i++) {
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.format = LC_FORMAT_UNDEFINED;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.base_mip_level = i;
        vdesc.mip_level_count = 1;
        vdesc.base_array_layer = 0;
        vdesc.array_layer_count = 1;
        if (lc_image_view_create(renderer->hiz->image, &vdesc,
                                 &renderer->hiz->views[i]) !=
            LC_SUCCESS) {
            lr_hiz_destroy(renderer);
            return LR_ERROR_RENDER;
        }
    }
    renderer->hiz->width = width;
    renderer->hiz->height = height;
    renderer->hiz->mips = mips;
    /* Full-chain sampling view: occlusion reads any mip through
     * one descriptor (single-level views only ever expose lod
     * 0; fetching higher lods through them is undefined). */
    {
        lc_image_view_desc full;

        memset(&full, 0, sizeof(full));
        full.type = LC_IMAGE_VIEW_2D;
        full.format = LC_FORMAT_UNDEFINED;
        full.aspect = LC_IMAGE_ASPECT_COLOR;
        full.base_mip_level = 0;
        full.mip_level_count = mips;
        full.base_array_layer = 0;
        full.array_layer_count = 1;
        if (lc_image_view_create(renderer->hiz->image, &full,
                                 &renderer->hiz->sample_view) !=
            LC_SUCCESS) {
            lr_hiz_destroy(renderer);
            return LR_ERROR_RENDER;
        }
    }
    /* One reduce set per mip (views stable until the next
     * rebuild); contents update every generation. Requires the
     * shared reduce layout: callers ensure visibility shared
     * state before sizing the pyramid. */
    if (mips > 1) {
        if (renderer->vis_hiz_reduce_layout == NULL) {
            lr_hiz_destroy(renderer);
            return LR_ERROR_INVALID_ARGUMENT;
        }
        renderer->vis_hiz_reduce_sets = (lc_binding_set **)calloc(
            mips, sizeof(lc_binding_set *));
        if (renderer->vis_hiz_reduce_sets == NULL) {
            lr_hiz_destroy(renderer);
            return LR_ERROR_OUT_OF_MEMORY;
        }
        for (i = 1; i < mips; i++) {
            if (lc_binding_set_create(
                    renderer->vis_hiz_reduce_layout,
                    &renderer->vis_hiz_reduce_sets[i]) !=
                LC_SUCCESS) {
                lr_hiz_free_reduce_sets(renderer);
                lr_hiz_destroy(renderer);
                return LR_ERROR_RENDER;
            }
        }
    }
    return LR_SUCCESS;
}

/* One mip-extent step: max(1, extent >> level). */
static void lr_hiz_mip_extent(uint32_t width, uint32_t height,
                              uint32_t mip, uint32_t *out_w,
                              uint32_t *out_h) {
    *out_w = width >> mip;
    *out_h = height >> mip;
    if (*out_w == 0) {
        *out_w = 1;
    }
    if (*out_h == 0) {
        *out_h = 1;
    }
}

static uint32_t lr_hiz_ceil_div(uint32_t n, uint32_t d) {
    return (n + d - 1u) / d;
}

lr_result lr_hiz_generate(lr_renderer *renderer,
                          lc_command_encoder *encoder) {
    lc_image_subresource_range range;
    lc_binding_write writes[3];
    uint32_t i;
    struct {
        uint32_t w;
        uint32_t h;
    } copy_push;
    struct {
        uint32_t src_w;
        uint32_t src_h;
        uint32_t dst_w;
        uint32_t dst_h;
        int32_t src_level;
    } reduce_push;

    if (renderer == NULL || encoder == NULL ||
        renderer->hiz == NULL || renderer->hdr_depth_view == NULL ||
        !renderer->vis_ready) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->hiz->mips > 1 &&
        renderer->vis_hiz_reduce_sets == NULL) {
        return LR_ERROR_RENDER;
    }
    /* Whole pyramid into production state first; each mip drops
     * to SHADER_READ as it finishes (per-mip subresource sync). */
    memset(&range, 0, sizeof(range));
    range.base_mip_level = 0;
    range.level_count = renderer->hiz->mips;
    range.base_array_layer = 0;
    range.layer_count = 1;
    if (lc_encoder_transition_image(encoder, renderer->hiz->image,
                                    &range,
                                    LC_RESOURCE_STATE_SHADER_READ_WRITE) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Seed set is rewritten every generation: the HDR depth view
     * changes on rebuilds, and update-before-bind in the same
     * recording is the established sky-set pattern. */
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_SAMPLED_IMAGE;
    writes[0].u.image.view = renderer->hdr_depth_view;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_STORAGE_IMAGE;
    writes[1].u.image.view = renderer->hiz->views[0];
    writes[2].binding = 2;
    writes[2].array_element = 0;
    writes[2].type = LC_BINDING_SAMPLER;
    writes[2].u.sampler.sampler = renderer->vis_sampler;
    if (lc_binding_set_update(renderer->vis_hiz_copy_set, writes, 3) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_bind_compute_pipeline(
            encoder, renderer->vis_hiz_copy_pipeline) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_bind_compute_set(encoder,
                                    renderer->vis_hiz_copy_pipeline,
                                    0,
                                    renderer->vis_hiz_copy_set) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    copy_push.w = renderer->hiz->width;
    copy_push.h = renderer->hiz->height;
    if (lc_encoder_push_compute_constants(
            encoder, renderer->vis_hiz_copy_pipeline,
            LC_SHADER_VISIBILITY_COMPUTE, 0, sizeof(copy_push),
            &copy_push) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lc_encoder_dispatch(encoder,
                            lr_hiz_ceil_div(copy_push.w, 8u),
                            lr_hiz_ceil_div(copy_push.h, 8u),
                            1) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Mip 0 becomes sampled for the first reduction (and stays
     * sampled for culling reads afterwards). */
    memset(&range, 0, sizeof(range));
    range.base_mip_level = 0;
    range.level_count = 1;
    range.base_array_layer = 0;
    range.layer_count = 1;
    if (lc_encoder_transition_image(encoder, renderer->hiz->image,
                                    &range,
                                    LC_RESOURCE_STATE_SHADER_READ) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    for (i = 1; i < renderer->hiz->mips; i++) {
        uint32_t src_w;
        uint32_t src_h;
        uint32_t dst_w;
        uint32_t dst_h;

        lr_hiz_mip_extent(renderer->hiz->width, renderer->hiz->height,
                          i - 1u, &src_w, &src_h);
        lr_hiz_mip_extent(renderer->hiz->width, renderer->hiz->height,
                          i, &dst_w, &dst_h);
        writes[0].binding = 0;
        writes[0].array_element = 0;
        writes[0].type = LC_BINDING_SAMPLED_IMAGE;
        writes[0].u.image.view = renderer->hiz->views[i - 1u];
        writes[1].binding = 1;
        writes[1].array_element = 0;
        writes[1].type = LC_BINDING_STORAGE_IMAGE;
        writes[1].u.image.view = renderer->hiz->views[i];
        writes[2].binding = 2;
        writes[2].array_element = 0;
        writes[2].type = LC_BINDING_SAMPLER;
        writes[2].u.sampler.sampler = renderer->vis_sampler;
        if (lc_binding_set_update(renderer->vis_hiz_reduce_sets[i],
                                  writes, 3) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        if (lc_encoder_bind_compute_pipeline(
                encoder,
                renderer->vis_hiz_reduce_pipeline) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        if (lc_encoder_bind_compute_set(
                encoder, renderer->vis_hiz_reduce_pipeline, 0,
                renderer->vis_hiz_reduce_sets[i]) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        reduce_push.src_w = src_w;
        reduce_push.src_h = src_h;
        reduce_push.dst_w = dst_w;
        reduce_push.dst_h = dst_h;
        /* Views are single-level: the sampled level is always
         * relative lod 0 (the mip choice rides in the VIEW, and
         * in src_w/src_h, never in the fetch lod). */
        reduce_push.src_level = 0;
        if (lc_encoder_push_compute_constants(
                encoder, renderer->vis_hiz_reduce_pipeline,
                LC_SHADER_VISIBILITY_COMPUTE, 0, sizeof(reduce_push),
                &reduce_push) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        if (lc_encoder_dispatch(encoder, lr_hiz_ceil_div(dst_w, 8u),
                                lr_hiz_ceil_div(dst_h, 8u),
                                1) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        memset(&range, 0, sizeof(range));
        range.base_mip_level = i;
        range.level_count = 1;
        range.base_array_layer = 0;
        range.layer_count = 1;
        if (lc_encoder_transition_image(
                encoder, renderer->hiz->image, &range,
                LC_RESOURCE_STATE_SHADER_READ) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    return LR_SUCCESS;
}

lr_result lr_hiz_read_mip(lr_renderer *renderer, uint32_t mip,
                          float *out_pixels) {
    lc_image_readback_desc desc;
    lc_image_readback_info info;
    size_t need = 0;

    if (renderer == NULL || renderer->hiz == NULL ||
        out_pixels == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (mip >= renderer->hiz->mips) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    memset(&desc, 0, sizeof(desc));
    desc.mip_level = mip;
    desc.array_layer = 0;
    if (lc_image_query_readback(renderer->hiz->image, &desc, &info) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (info.format != LC_FORMAT_R32_FLOAT) {
        return LR_ERROR_RENDER;
    }
    if (lc_image_readback(renderer->hiz->image, &desc, NULL, 0,
                          &need) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (need != info.size) {
        return LR_ERROR_RENDER;
    }
    if (lc_image_readback(renderer->hiz->image, &desc, out_pixels,
                          need, NULL) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

void lr_hiz_cpu_pyramid(const float *mip0, uint32_t width,
                        uint32_t height, uint32_t mip, float *out) {
    uint32_t src_w = width;
    uint32_t src_h = height;
    uint32_t dst_w;
    uint32_t dst_h;
    uint32_t level;
    const float *src = mip0;
    float *owned = NULL;
    float *buf_a = NULL;
    float *buf_b = NULL;
    uint32_t x;
    uint32_t y;

    if (mip0 == NULL || out == NULL || width == 0 ||
        height == 0) {
        return;
    }
    if (mip == 0) {
        memcpy(out, mip0,
               (size_t)width * (size_t)height * sizeof(float));
        return;
    }
    /* Ping-pong through caller-sized scratch: allocate two
     * max-extent buffers (tests use small fixtures; the function
     * stays dependency-free). */
    buf_a = (float *)malloc((size_t)width * (size_t)height *
                            sizeof(float));
    buf_b = (float *)malloc((size_t)width * (size_t)height *
                            sizeof(float));
    if (buf_a == NULL || buf_b == NULL) {
        free(buf_a);
        free(buf_b);
        return;
    }
    memcpy(buf_a, mip0,
           (size_t)width * (size_t)height * sizeof(float));
    src = buf_a;
    owned = buf_b;
    for (level = 1; level <= mip; level++) {
        float *dst;

        dst_w = src_w >> 1;
        dst_h = src_h >> 1;
        if (dst_w == 0) {
            dst_w = 1;
        }
        if (dst_h == 0) {
            dst_h = 1;
        }
        dst = (level == mip) ? out : owned;
        for (y = 0; y < dst_h; y++) {
            for (x = 0; x < dst_w; x++) {
                uint32_t sx = x * 2u;
                uint32_t sy = y * 2u;
                float m = 0.0f;
                uint32_t dx;
                uint32_t dy;

                for (dy = 0; dy < 2u; dy++) {
                    for (dx = 0; dx < 2u; dx++) {
                        uint32_t ix = sx + dx;
                        uint32_t iy = sy + dy;

                        if (ix >= src_w || iy >= src_h) {
                            continue;
                        }
                        if (src[(size_t)iy * src_w + ix] > m) {
                            m = src[(size_t)iy * src_w + ix];
                        }
                    }
                }
                dst[(size_t)y * dst_w + x] = m;
            }
        }
        if (level != mip) {
            const float *tmp = src;

            src = owned;
            owned = (float *)tmp;
            src_w = dst_w;
            src_h = dst_h;
        }
    }
    free(buf_a);
    free(buf_b);
}
