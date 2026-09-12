/*
 * Centralized backend-neutral format translation (Phase 8/9).
 *
 * The single place mapping lc_format <-> VkFormat, plus element byte
 * sizes and color/depth/stencil classification. All vertex-attribute,
 * swapchain-format, image, and future texture code paths go through
 * here instead of scattering switches. Compressed formats are future
 * work; nothing here assumes one uncompressed texel beyond the byte
 * sizes below (block formats will extend the table with block
 * extents).
 */

#include <stddef.h>

#include "graphics/graphics_internal.h"

typedef struct lc_format_entry {
    lc_format lc;
    VkFormat vk;
    uint32_t size; /* bytes per element (uncompressed texel) */
    uint32_t components;
    int is_color;
    int is_depth;
    int is_stencil;
} lc_format_entry;

static const lc_format_entry k_format_table[] = {
    { LC_FORMAT_R8_UNORM, VK_FORMAT_R8_UNORM, 1, 1, 1, 0, 0 },
    { LC_FORMAT_RG8_UNORM, VK_FORMAT_R8G8_UNORM, 2, 2, 1, 0, 0 },
    { LC_FORMAT_RGBA8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4, 4, 1, 0, 0 },
    { LC_FORMAT_RGBA8_SRGB, VK_FORMAT_R8G8B8A8_SRGB, 4, 4, 1, 0, 0 },
    { LC_FORMAT_BGRA8_UNORM, VK_FORMAT_B8G8R8A8_UNORM, 4, 4, 1, 0, 0 },
    { LC_FORMAT_BGRA8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, 4, 4, 1, 0, 0 },
    { LC_FORMAT_R16_FLOAT, VK_FORMAT_R16_SFLOAT, 2, 1, 1, 0, 0 },
    { LC_FORMAT_RG16_FLOAT, VK_FORMAT_R16G16_SFLOAT, 4, 2, 1, 0, 0 },
    { LC_FORMAT_RGBA16_FLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 4, 1, 0, 0 },
    { LC_FORMAT_R32_FLOAT, VK_FORMAT_R32_SFLOAT, 4, 1, 1, 0, 0 },
    { LC_FORMAT_RG32_FLOAT, VK_FORMAT_R32G32_SFLOAT, 8, 2, 1, 0, 0 },
    { LC_FORMAT_RGB32_FLOAT, VK_FORMAT_R32G32B32_SFLOAT, 12, 3, 1, 0, 0 },
    { LC_FORMAT_RGBA32_FLOAT, VK_FORMAT_R32G32B32A32_SFLOAT, 16, 4, 1, 0, 0 },
    { LC_FORMAT_R32_UINT, VK_FORMAT_R32_UINT, 4, 1, 1, 0, 0 },
    { LC_FORMAT_RG32_UINT, VK_FORMAT_R32G32_UINT, 8, 2, 1, 0, 0 },
    { LC_FORMAT_RGB32_UINT, VK_FORMAT_R32G32B32_UINT, 12, 3, 1, 0, 0 },
    { LC_FORMAT_RGBA32_UINT, VK_FORMAT_R32G32B32A32_UINT, 16, 4, 1, 0, 0 },
    /* Depth/stencil: allocatable as images today; depth rendering
     * arrives in a later phase. D24+S8 packs depth+stencil. */
    { LC_FORMAT_D16_UNORM, VK_FORMAT_D16_UNORM, 2, 1, 0, 1, 0 },
    { LC_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT, 4, 2, 0, 1, 1 },
    { LC_FORMAT_D32_FLOAT, VK_FORMAT_D32_SFLOAT, 4, 1, 0, 1, 0 },
};

#define LC_FORMAT_COUNT \
    (sizeof(k_format_table) / sizeof(k_format_table[0]))

VkFormat lc_vulkan_translate_format(lc_format format) {
    size_t i;

    for (i = 0; i < LC_FORMAT_COUNT; i++) {
        if (k_format_table[i].lc == format) {
            return k_format_table[i].vk;
        }
    }
    return VK_FORMAT_UNDEFINED;
}

lc_format lc_vulkan_untranslate_format(VkFormat format) {
    size_t i;

    if (format == VK_FORMAT_UNDEFINED) {
        return LC_FORMAT_UNDEFINED;
    }
    for (i = 0; i < LC_FORMAT_COUNT; i++) {
        if (k_format_table[i].vk == format) {
            return k_format_table[i].lc;
        }
    }
    return LC_FORMAT_UNDEFINED;
}

uint32_t lc_format_byte_size(lc_format format) {
    size_t i;

    for (i = 0; i < LC_FORMAT_COUNT; i++) {
        if (k_format_table[i].lc == format) {
            return k_format_table[i].size;
        }
    }
    return 0;
}

static const lc_format_entry *lc_format_lookup(lc_format format) {
    size_t i;

    for (i = 0; i < LC_FORMAT_COUNT; i++) {
        if (k_format_table[i].lc == format) {
            return &k_format_table[i];
        }
    }
    return NULL;
}

int lc_format_is_color(lc_format format) {
    const lc_format_entry *entry = lc_format_lookup(format);
    return (entry != NULL && entry->is_color) ? 1 : 0;
}

int lc_format_is_depth(lc_format format) {
    const lc_format_entry *entry = lc_format_lookup(format);
    return (entry != NULL && entry->is_depth) ? 1 : 0;
}

int lc_format_is_stencil(lc_format format) {
    const lc_format_entry *entry = lc_format_lookup(format);
    return (entry != NULL && entry->is_stencil) ? 1 : 0;
}

uint32_t lc_format_component_count(lc_format format) {
    const lc_format_entry *entry = lc_format_lookup(format);
    return (entry != NULL) ? entry->components : 0;
}
