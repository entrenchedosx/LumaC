/*
 * Asset manager (Phase 14, PART AE/AF): owns shared texture/sampler
 * caches plus the model list. Models are caller-owned and must die
 * first; surviving cache entries die here.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "luma_assets/luma_assets.h"
#include "internal/assets_internal.h"

void la_set_error(la_asset_manager *manager, const char *fmt, ...) {
    va_list args;

    if (manager == NULL || fmt == NULL) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(manager->last_error, sizeof(manager->last_error), fmt, args);
    va_end(args);
    manager->last_error[sizeof(manager->last_error) - 1] = '\0';
}

la_result la_map_lc(lc_result res) {
    switch (res) {
    case LC_SUCCESS:
        return LA_SUCCESS;
    case LC_ERROR_INVALID_ARGUMENT:
        return LA_ERROR_INVALID_ARGUMENT;
    case LC_ERROR_NOT_INITIALIZED:
        return LA_ERROR_NOT_INITIALIZED;
    case LC_ERROR_OUT_OF_MEMORY:
        return LA_ERROR_OUT_OF_MEMORY;
    case LC_ERROR_UNSUPPORTED:
        return LA_ERROR_UNSUPPORTED;
    default:
        return LA_ERROR_RENDER;
    }
}

la_result la_map_lr(lr_result res) {
    switch (res) {
    case LR_SUCCESS:
        return LA_SUCCESS;
    case LR_ERROR_INVALID_ARGUMENT:
        return LA_ERROR_INVALID_ARGUMENT;
    case LR_ERROR_NOT_INITIALIZED:
        return LA_ERROR_NOT_INITIALIZED;
    case LR_ERROR_OUT_OF_MEMORY:
        return LA_ERROR_OUT_OF_MEMORY;
    case LR_ERROR_UNSUPPORTED:
        return LA_ERROR_UNSUPPORTED;
    case LR_ERROR_INCOMPATIBLE:
        /* No la-level incompatibility code: structural mismatches at
         * submit time are caller bugs, reported as invalid use. */
        return LA_ERROR_INVALID_ARGUMENT;
    default:
        return LA_ERROR_RENDER;
    }
}

la_result la_asset_manager_create(const la_asset_manager_desc *desc,
                                  la_asset_manager **out_manager) {
    la_asset_manager *manager;

    if (desc == NULL || out_manager == NULL) {
        if (out_manager != NULL) {
            *out_manager = NULL;
        }
        return LA_ERROR_INVALID_ARGUMENT;
    }
    if (desc->renderer == NULL) {
        *out_manager = NULL;
        return LA_ERROR_INVALID_ARGUMENT;
    }
    manager = (la_asset_manager *)calloc(1, sizeof(la_asset_manager));
    if (manager == NULL) {
        *out_manager = NULL;
        return LA_ERROR_OUT_OF_MEMORY;
    }
    manager->renderer = desc->renderer;
    manager->last_error[0] = '\0';
    *out_manager = manager;
    return LA_SUCCESS;
}

void la_asset_manager_destroy(la_asset_manager *manager) {
    la_texture_asset *tex;
    la_texture_asset *tex_next;
    la_sampler_asset *samp;
    la_sampler_asset *samp_next;

    if (manager == NULL) {
        return;
    }
    /* Models must already be gone (dependents-first contract); cache
     * entries still referenced would indicate a caller bug, but
     * teardown stays safe regardless. */
    for (tex = manager->textures; tex != NULL; tex = tex_next) {
        tex_next = tex->next;
        lc_image_view_destroy(tex->view);
        lc_image_destroy(tex->image);
        free(tex->key);
        free(tex);
    }
    manager->textures = NULL;
    la_hdr_teardown(manager);
    for (samp = manager->samplers; samp != NULL; samp = samp_next) {
        samp_next = samp->next;
        lc_sampler_destroy(samp->sampler);
        free(samp);
    }
    manager->samplers = NULL;
    free(manager);
}

const char *la_asset_manager_get_last_error(
    const la_asset_manager *manager) {
    if (manager == NULL) {
        return "";
    }
    return manager->last_error;
}

uint32_t la_asset_manager_get_texture_count(
    const la_asset_manager *manager) {
    uint32_t count = 0;
    const la_texture_asset *it;

    if (manager == NULL) {
        return 0;
    }
    for (it = manager->textures; it != NULL; it = it->next) {
        count++;
    }
    return count;
}

uint32_t la_asset_manager_get_sampler_count(
    const la_asset_manager *manager) {
    uint32_t count = 0;
    const la_sampler_asset *it;

    if (manager == NULL) {
        return 0;
    }
    for (it = manager->samplers; it != NULL; it = it->next) {
        count++;
    }
    return count;
}
