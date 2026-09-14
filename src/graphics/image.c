#include <stdlib.h>
#include <string.h>

#include "lumac/lumac.h"
#include "internal/lumac_internal.h"
#include "graphics/graphics_internal.h"

/*
 * Public image API + lifetime tracking (Phase 9).
 *
 * Images belong to one device and never to a swapchain, so they
 * survive swapchain recreation. Device liveness is verified before
 * any dereference; device teardown and shutdown destroy dependent
 * images before VkDevice. Deep descriptor validation lives in the
 * backend (vulkan_image.c), shared by every creation path.
 */

static void lc_image_list_add(lc_image *image) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || image == NULL) {
        return;
    }
    image->next = state->images;
    image->prev = NULL;
    if (state->images != NULL) {
        state->images->prev = image;
    }
    state->images = image;
}

static void lc_image_list_remove(lc_image *image) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL || image == NULL) {
        return;
    }
    if (image->prev != NULL) {
        image->prev->next = image->next;
    } else if (state->images == image) {
        state->images = image->next;
    }
    if (image->next != NULL) {
        image->next->prev = image->prev;
    }
    image->next = NULL;
    image->prev = NULL;
}

static int lc_is_live_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    const lc_device *it;

    if (state == NULL || device == NULL) {
        return 0;
    }
    for (it = state->devices; it != NULL; it = it->next) {
        if (it == device) {
            return 1;
        }
    }
    return 0;
}

static int lc_is_live_image(const lc_image *image) {
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

lc_result lc_image_create(lc_device *device, const lc_image_desc *desc,
                          lc_image **out_image) {
    lc_state *state = lc_get_internal_state();
    lc_image *image;
    lc_result res;

    if (device == NULL || desc == NULL || out_image == NULL) {
        if (out_image != NULL) {
            *out_image = NULL;
        }
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (state == NULL || !state->initialized) {
        *out_image = NULL;
        return LC_ERROR_NOT_INITIALIZED;
    }
    /* Liveness before any dereference of the caller handle. */
    if (!lc_is_live_device(device)) {
        *out_image = NULL;
        return LC_ERROR_INVALID_ARGUMENT;
    }

    image = (lc_image *)calloc(1, sizeof(lc_image));
    if (image == NULL) {
        *out_image = NULL;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    image->resource_id = lc_issue_resource_id();

    res = lc_vulkan_image_create(image, device, desc);
    if (res != LC_SUCCESS) {
        *out_image = NULL;
        free(image);
        return res;
    }

    lc_image_list_add(image);
    *out_image = image;
    return LC_SUCCESS;
}

void lc_image_destroy(lc_image *image) {
    lc_device *device;
    lc_retire_entry entry;

    if (image == NULL) {
        return;
    }
    device = image->device;
    if (device != NULL) {
        lc_vk_cmdlist_poison_for(device, image);
    }
    /* Views reference the VkImage; they die before it. */
    lc_image_view_destroy_for_image(image);
    lc_image_list_remove(image);
    /* Unexecuted worker lists referencing this image fail loudly
     * at execute instead of dereferencing a freed wrapper. */
    if (device != NULL) {
        lc_vk_cmdlist_poison_for(device, image);
    }
    /* In-flight async work drains first (bounded, driving the
     * graphics acquire); the VkImage, default view, and binding
     * then retire until GPU completion. Tracking arrays die here:
     * no transfer path touches them afterwards (entries claimed
     * by emit restore ownership from snapshots, destroy claimed
     * them first). */
    if (device != NULL) {
        lc_vk_transfer_wait_for(device, image, NULL);
    }
    memset(&entry, 0, sizeof(entry));
    entry.kind = LC_RETIRE_IMAGE;
    entry.image = image->vk_image;
    entry.default_view = image->default_view;
    entry.binding.memory = image->vk_memory;
    entry.binding.offset = image->vk_memory_offset;
    entry.binding.size = image->allocation_size;
    entry.binding.dedicated = image->memory_dedicated;
    entry.binding.block = image->memory_block;
    entry.bytes = image->allocation_size;
    image->vk_image = VK_NULL_HANDLE;
    image->default_view = VK_NULL_HANDLE;
    image->vk_memory = VK_NULL_HANDLE;
    image->vk_memory_offset = 0;
    image->memory_block = NULL;
    if (image->states != NULL) {
        free(image->states);
        image->states = NULL;
    }
    if (image->owners != NULL) {
        free(image->owners);
        image->owners = NULL;
    }
    if (image->epochs != NULL) {
        free(image->epochs);
        image->epochs = NULL;
    }
    if (device != NULL) {
        lc_vk_retire(device, &entry);
    }
    free(image);
}

lc_format lc_image_get_format(const lc_image *image) {
    if (image == NULL) {
        return LC_FORMAT_UNDEFINED;
    }
    return image->format;
}

uint32_t lc_image_get_width(const lc_image *image) {
    if (image == NULL) {
        return 0;
    }
    return image->width;
}

uint32_t lc_image_get_height(const lc_image *image) {
    if (image == NULL) {
        return 0;
    }
    return image->height;
}

uint32_t lc_image_get_mip_levels(const lc_image *image) {
    if (image == NULL) {
        return 0;
    }
    return image->mip_levels;
}

uint32_t lc_image_get_array_layers(const lc_image *image) {
    if (image == NULL) {
        return 0;
    }
    return image->array_layers;
}

void lc_image_get_memory_info(const lc_image *image,
                              lc_resource_memory_info *out_info) {
    if (out_info == NULL) {
        return;
    }
    memset(out_info, 0, sizeof(*out_info));
    if (image == NULL || !lc_is_live_image(image)) {
        return;
    }
    /* Mip-0 footprint (mips add on top; allocation_size is exact). */
    out_info->requested_size =
        (uint64_t)image->width * (uint64_t)image->height *
        (uint64_t)image->depth * (uint64_t)image->array_layers *
        (uint64_t)lc_format_byte_size(image->format);
    out_info->allocation_size = image->allocation_size;
    out_info->dedicated = image->memory_dedicated;
    out_info->memory_class = image->memory_class;
}

lc_result lc_image_write(lc_image *image,
                         const lc_image_upload_desc *upload) {
    if (image == NULL || upload == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_is_live_image(image)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_image_write(image, upload);
}

lc_result lc_image_generate_mipmaps(lc_image *image) {
    if (image == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_is_live_image(image)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return lc_vulkan_image_generate_mipmaps(image);
}

void lc_image_destroy_all(void) {
    lc_state *state = lc_get_internal_state();
    if (state == NULL) {
        return;
    }
    while (state->images != NULL) {
        lc_image_destroy(state->images);
    }
}

void lc_image_destroy_for_device(const lc_device *device) {
    lc_state *state = lc_get_internal_state();
    lc_image *it;
    lc_image *next;

    if (state == NULL || device == NULL) {
        return;
    }
    for (it = state->images; it != NULL; it = next) {
        next = it->next;
        if (it->device == device) {
            lc_image_destroy(it);
        }
    }
}
