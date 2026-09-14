/*
 * Vulkan render-target backend (Phase 12: offscreen targets, shared
 * render-pass cache, swapchain target snapshots).
 *
 * Offscreen targets borrow lc_image_views (formats/dimensions/samples
 * inferred, never duplicated); one VkFramebuffer is created lazily on
 * first use and reused across load/store variants (same
 * formats/count/extent stay compatible). Render passes come from the
 * device cache keyed by structure + policy + presentation final, so
 * equivalent targets share passes and pipelines stay structural.
 * Swapchain targets are format snapshots (no views); generic
 * swapchain passes reuse the swapchain's legacy per-image
 * framebuffers, which share the same attachment recipe.
 */

#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

VkSampleCountFlagBits
lc_vulkan_translate_samples(lc_sample_count samples) {
    switch (samples) {
    case LC_SAMPLE_COUNT_2:
        return VK_SAMPLE_COUNT_2_BIT;
    case LC_SAMPLE_COUNT_4:
        return VK_SAMPLE_COUNT_4_BIT;
    case LC_SAMPLE_COUNT_8:
        return VK_SAMPLE_COUNT_8_BIT;
    case LC_SAMPLE_COUNT_1:
    default:
        return VK_SAMPLE_COUNT_1_BIT;
    }
}

VkAttachmentLoadOp lc_vulkan_translate_load(lc_load_op op) {
    switch (op) {
    case LC_LOAD_OP_LOAD:
        return VK_ATTACHMENT_LOAD_OP_LOAD;
    case LC_LOAD_OP_DONT_CARE:
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    case LC_LOAD_OP_CLEAR:
    default:
        return VK_ATTACHMENT_LOAD_OP_CLEAR;
    }
}

VkAttachmentStoreOp lc_vulkan_translate_store(lc_store_op op) {
    switch (op) {
    case LC_STORE_OP_DONT_CARE:
        return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    case LC_STORE_OP_STORE:
    default:
        return VK_ATTACHMENT_STORE_OP_STORE;
    }
}

static int lc_vk_pass_key_equal(const lc_vk_pass_key *a,
                                const lc_vk_pass_key *b) {
    uint32_t i;

    if (a->color_count != b->color_count ||
        a->depth_format != b->depth_format || a->samples != b->samples ||
        a->present != b->present || a->depth_sampled != b->depth_sampled ||
        a->depth_load != b->depth_load ||
        a->depth_store != b->depth_store) {
        return 0;
    }
    for (i = 0; i < a->color_count; i++) {
        if (a->color_formats[i] != b->color_formats[i] ||
            a->color_loads[i] != b->color_loads[i] ||
            a->color_stores[i] != b->color_stores[i]) {
            return 0;
        }
    }
    return 1;
}

static lc_result lc_vk_create_pass_object(VkDevice device,
                                          const lc_vk_pass_key *key,
                                          VkRenderPass *out_pass) {
    VkAttachmentDescription attachments[LC_MAX_COLOR_ATTACHMENTS + 1];
    VkAttachmentReference color_refs[LC_MAX_COLOR_ATTACHMENTS];
    VkAttachmentReference depth_ref;
    VkSubpassDescription subpass;
    VkSubpassDependency deps[2];
    VkRenderPassCreateInfo info;
    uint32_t count = key->color_count;
    uint32_t has_depth = (key->depth_format != VK_FORMAT_UNDEFINED) ? 1 : 0;
    uint32_t i;

    if (count > LC_MAX_COLOR_ATTACHMENTS ||
        (count == 0 && !has_depth)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(attachments, 0, sizeof(attachments));
    for (i = 0; i < count; i++) {
        int is_present_final =
            (key->present && i == 0) ? 1 : 0;

        attachments[i].format = key->color_formats[i];
        attachments[i].samples = key->samples;
        attachments[i].loadOp = key->color_loads[i];
        attachments[i].storeOp = key->color_stores[i];
        attachments[i].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[i].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        /* Initials derive from policy: LOAD keeps previous contents
         * (offscreen SHADER_READ), otherwise discard. Swapchain images
         * always start UNDEFINED (LOAD is rejected earlier). */
        if (key->color_loads[i] == VK_ATTACHMENT_LOAD_OP_LOAD) {
            attachments[i].initialLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        } else {
            attachments[i].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        }
        /* Finals are always presentation/sample/attachment-valid:
         * UNDEFINED is never a legal finalLayout. DONT_CARE stores
         * still land in a valid layout; the discarded contents are
         * tracked separately (UNDEFINED) so later LOADs fail. */
        attachments[i].finalLayout = is_present_final
                                         ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                         : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        memset(&color_refs[i], 0, sizeof(color_refs[i]));
        color_refs[i].attachment = i;
        color_refs[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (has_depth) {
        attachments[count].format = key->depth_format;
        attachments[count].samples = key->samples;
        attachments[count].loadOp = key->depth_load;
        attachments[count].storeOp = key->depth_store;
        attachments[count].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[count].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[count].initialLayout =
            (key->depth_load == VK_ATTACHMENT_LOAD_OP_LOAD)
                ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                : VK_IMAGE_LAYOUT_UNDEFINED;
        /* Sampled-usage depth finalizes sampled-readable (mirroring
         * color STORE); plain depth stays attachment-optimal. */
        attachments[count].finalLayout = key->depth_sampled
                                             ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                             : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        memset(&depth_ref, 0, sizeof(depth_ref));
        depth_ref.attachment = count;
        depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }

    memset(&subpass, 0, sizeof(subpass));
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = count;
    subpass.pColorAttachments = (count > 0) ? color_refs : NULL;
    subpass.pDepthStencilAttachment = has_depth ? &depth_ref : NULL;

    memset(deps, 0, sizeof(deps));
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    /* Phase 19 (closes Phase 18 debt): attachment write -> later
     * shader read needs EXPLICIT visibility, not just a correct
     * finalLayout. The old dst (BOTTOM_OF_PIPE / no access) left
     * STORE-then-sample flows visibility-weak on strict hardware.
     * dst FRAGMENT_SHADER + SHADER_READ covers color STORE and
     * sampled-usage depth STORE alike (both srcAccess bits feed
     * it); layout transitions still come from finalLayout. */
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                           VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = count + has_depth;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = deps;

    if (vkCreateRenderPass(device, &info, NULL, out_pass) != VK_SUCCESS) {
        *out_pass = VK_NULL_HANDLE;
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }
    return LC_SUCCESS;
}

lc_result lc_vulkan_pass_cache_get(lc_device *device,
                                   const lc_vk_pass_key *key,
                                   VkRenderPass *out_pass) {
    uint32_t i;

    if (device == NULL || key == NULL || out_pass == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    /* Shared cache across threads (PART 22): lookup + insert under
     * the cache shard. */
    lc_device_lock_cache(device);
    for (i = 0; i < device->pass_cache_count; i++) {
        if (lc_vk_pass_key_equal(&device->pass_cache[i].key, key)) {
            *out_pass = device->pass_cache[i].pass;
            lc_device_unlock_cache(device);
            return LC_SUCCESS;
        }
    }
    if (device->pass_cache_count == device->pass_cache_capacity) {
        uint32_t grow = (device->pass_cache_capacity == 0) ? 8u : 8u;
        lc_vk_cached_pass *grown = (lc_vk_cached_pass *)realloc(
            device->pass_cache,
            sizeof(lc_vk_cached_pass) *
                (device->pass_cache_capacity + grow));

        if (grown == NULL) {
            lc_device_unlock_cache(device);
            return LC_ERROR_OUT_OF_MEMORY;
        }
        device->pass_cache = grown;
        device->pass_cache_capacity += grow;
    }
    {
        VkRenderPass pass = VK_NULL_HANDLE;
        lc_result res = lc_vk_create_pass_object(device->device, key, &pass);

        if (res != LC_SUCCESS) {
            lc_device_unlock_cache(device);
            return res;
        }
        device->pass_cache[device->pass_cache_count].key = *key;
        device->pass_cache[device->pass_cache_count].pass = pass;
        device->pass_cache_count++;
        *out_pass = pass;
        lc_device_unlock_cache(device);
        return LC_SUCCESS;
    }
}

void lc_vulkan_pass_cache_teardown(lc_device *device) {
    uint32_t i;

    if (device == NULL) {
        return;
    }
    if (device->pass_cache != NULL) {
        if (device->device != VK_NULL_HANDLE) {
            for (i = 0; i < device->pass_cache_count; i++) {
                if (device->pass_cache[i].pass != VK_NULL_HANDLE) {
                    vkDestroyRenderPass(device->device,
                                        device->pass_cache[i].pass, NULL);
                }
            }
        }
        free(device->pass_cache);
        device->pass_cache = NULL;
    }
    device->pass_cache_count = 0;
    device->pass_cache_capacity = 0;
}

lc_result lc_vulkan_target_desc_validate(lc_device *device,
                                         const lc_render_target_desc *desc) {
    VkPhysicalDeviceProperties props;
    uint32_t i;

    if (device == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->color_attachment_count > LC_MAX_COLOR_ATTACHMENTS ||
        (desc->color_attachment_count == 0 &&
         desc->depth_stencil_format == LC_FORMAT_UNDEFINED)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->physical_device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(device->physical_device, &props);
    if (desc->color_attachment_count > props.limits.maxColorAttachments) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < desc->color_attachment_count; i++) {
        if (desc->color_formats[i] == LC_FORMAT_UNDEFINED ||
            !lc_format_is_color(desc->color_formats[i])) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    if (desc->depth_stencil_format != LC_FORMAT_UNDEFINED &&
        !lc_format_is_depth(desc->depth_stencil_format)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->samples != LC_SAMPLE_COUNT_1 &&
        desc->samples != LC_SAMPLE_COUNT_2 &&
        desc->samples != LC_SAMPLE_COUNT_4 &&
        desc->samples != LC_SAMPLE_COUNT_8) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return LC_SUCCESS;
}

/* Effective mip extent (minimum 1 per axis). */
static void lc_vk_view_mip_extent(const lc_image *image, uint32_t mip,
                                  uint32_t *w, uint32_t *h) {
    *w = image->width >> mip;
    *h = image->height >> mip;
    if (*w == 0) {
        *w = 1;
    }
    if (*h == 0) {
        *h = 1;
    }
}

lc_result lc_vulkan_render_target_create(lc_render_target *target,
                                         lc_device *device,
                                         const lc_render_target_create_desc *desc) {
    VkPhysicalDeviceProperties props;
    uint32_t i;
    uint32_t samples = 0;

    if (target == NULL || device == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE ||
        device->physical_device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(device->physical_device, &props);
    if (desc->color_attachment_count > props.limits.maxColorAttachments ||
        desc->color_attachment_count > LC_MAX_COLOR_ATTACHMENTS) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->width == 0 || desc->height == 0 ||
        desc->width > props.limits.maxFramebufferWidth ||
        desc->height > props.limits.maxFramebufferHeight) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    target->device = device;
    target->width = desc->width;
    target->height = desc->height;
    target->color_views = NULL;
    target->color_count = 0;
    target->depth_view = NULL;
    target->depth_format = LC_FORMAT_UNDEFINED;
    target->samples = LC_SAMPLE_COUNT_1;
    target->hash = 0;
    target->is_swapchain_borrow = 0;
    target->framebuffer = VK_NULL_HANDLE;
    target->framebuffer_valid = 0;

    if (desc->color_attachment_count > 0) {
        target->color_views = (lc_image_view **)calloc(
            desc->color_attachment_count, sizeof(lc_image_view *));
        if (target->color_views == NULL) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
    }

    for (i = 0; i < desc->color_attachment_count; i++) {
        const lc_image_view *view = desc->color_attachments[i].view;
        const lc_image *image;
        lc_format effective;
        uint32_t mw = 0;
        uint32_t mh = 0;
        VkFormatProperties format_props;

        if (view == NULL || view->device != device || view->image == NULL ||
            view->vk_view == VK_NULL_HANDLE) {
            goto invalid;
        }
        image = view->image;
        if (image->device != device ||
            image->vk_image == VK_NULL_HANDLE) {
            goto invalid;
        }
        /* Attachments are single 2D color views (no arrays, no cubes). */
        if (view->type != LC_IMAGE_VIEW_2D ||
            view->mip_level_count != 1 || view->array_layer_count != 1) {
            goto invalid;
        }
        if ((view->aspect & LC_IMAGE_ASPECT_COLOR) == 0) {
            goto invalid;
        }
        effective = (view->format == LC_FORMAT_UNDEFINED) ? image->format
                                                          : view->format;
        if (!lc_format_is_color(effective)) {
            goto invalid;
        }
        if ((image->usage & LC_IMAGE_USAGE_COLOR_ATTACHMENT) == 0) {
            goto invalid;
        }
        /* Phase 12 finals always land sampled-readable, so color
         * attachments must be samplable (render-to-texture is the
         * point; pure-transient targets arrive with a later phase). */
        if ((image->usage & LC_IMAGE_USAGE_SAMPLED) == 0) {
            goto invalid;
        }
        lc_vk_view_mip_extent(image, view->base_mip_level, &mw, &mh);
        if (mw != desc->width || mh != desc->height) {
            goto invalid;
        }
        if (samples == 0) {
            samples = image->samples;
        } else if (samples != image->samples) {
            goto invalid;
        }
        /* Backend must support color-attachment rendering in this
         * format (capability gap, not a usage error). */
        memset(&format_props, 0, sizeof(format_props));
        vkGetPhysicalDeviceFormatProperties(
            device->physical_device, lc_vulkan_translate_format(effective),
            &format_props);
        if ((format_props.optimalTilingFeatures &
             VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) == 0) {
            free(target->color_views);
            target->color_views = NULL;
            return LC_ERROR_UNSUPPORTED;
        }
        target->color_views[i] = (lc_image_view *)view;
        target->color_formats[i] = effective;
    }
    target->color_count = desc->color_attachment_count;

    if (desc->depth_stencil_attachment != NULL) {
        const lc_image_view *view = desc->depth_stencil_attachment;
        const lc_image *image;
        lc_format effective;
        uint32_t mw = 0;
        uint32_t mh = 0;
        VkFormatProperties format_props;

        if (view->device != device || view->image == NULL ||
            view->vk_view == VK_NULL_HANDLE) {
            goto invalid;
        }
        image = view->image;
        if (image->device != device ||
            image->vk_image == VK_NULL_HANDLE) {
            goto invalid;
        }
        if (view->type != LC_IMAGE_VIEW_2D ||
            view->mip_level_count != 1 || view->array_layer_count != 1) {
            goto invalid;
        }
        if ((view->aspect & LC_IMAGE_ASPECT_DEPTH) == 0) {
            goto invalid;
        }
        effective = (view->format == LC_FORMAT_UNDEFINED) ? image->format
                                                          : view->format;
        if (!lc_format_is_depth(effective)) {
            goto invalid;
        }
        if ((image->usage & LC_IMAGE_USAGE_DEPTH_STENCIL) == 0) {
            goto invalid;
        }
        lc_vk_view_mip_extent(image, view->base_mip_level, &mw, &mh);
        if (mw != desc->width || mh != desc->height) {
            goto invalid;
        }
        if (samples == 0) {
            samples = image->samples;
        } else if (samples != image->samples) {
            goto invalid;
        }
        memset(&format_props, 0, sizeof(format_props));
        vkGetPhysicalDeviceFormatProperties(
            device->physical_device, lc_vulkan_translate_format(effective),
            &format_props);
        if ((format_props.optimalTilingFeatures &
             VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
            free(target->color_views);
            target->color_views = NULL;
            target->color_count = 0;
            return LC_ERROR_UNSUPPORTED;
        }
        target->depth_view = (lc_image_view *)view;
        target->depth_format = effective;
    }

    if (samples != LC_SAMPLE_COUNT_1 && samples != LC_SAMPLE_COUNT_2 &&
        samples != LC_SAMPLE_COUNT_4 && samples != LC_SAMPLE_COUNT_8) {
        goto invalid;
    }
    target->samples = (lc_sample_count)samples;
    target->hash =
        lc_render_target_hash(target->color_count, target->color_formats,
                              target->depth_format, target->samples);
    return LC_SUCCESS;

invalid:
    free(target->color_views);
    target->color_views = NULL;
    target->color_count = 0;
    target->depth_view = NULL;
    return LC_ERROR_INVALID_ARGUMENT;
}

void lc_vulkan_render_target_destroy(lc_render_target *target) {
    VkDevice device_handle = VK_NULL_HANDLE;

    if (target == NULL) {
        return;
    }
    if (target->device != NULL) {
        device_handle = target->device->device;
    }
    /* The public destroy path steals and retires the framebuffer.
     * No device-wide wait belongs in normal target destruction. */
    if (target->framebuffer_valid &&
        target->framebuffer != VK_NULL_HANDLE) {
        if (device_handle != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device_handle, target->framebuffer, NULL);
        }
        target->framebuffer = VK_NULL_HANDLE;
        target->framebuffer_valid = 0;
    }
    free(target->color_views);
    target->color_views = NULL;
    target->color_count = 0;
    target->depth_view = NULL;
}

void lc_vulkan_swapchain_target_sync(lc_swapchain *swapchain) {
    lc_render_target *target;

    if (swapchain == NULL) {
        return;
    }
    target = &swapchain->swapchain_target;
    /* Stable per-swapchain identity: assigned once, survives every
     * recreation (pointer equality would already hold here, but the
     * ID documents the rule and stays valid if the target ever
     * moves). */
    if (target->resource_id == 0) {
        target->resource_id = lc_issue_resource_id();
    }
    target->device = swapchain->device;
    target->width = swapchain->extent.width;
    target->height = swapchain->extent.height;
    target->color_count =
        (swapchain->vk_swapchain != VK_NULL_HANDLE) ? 1 : 0;
    if (target->color_count == 1) {
        target->color_formats[0] =
            lc_vulkan_untranslate_format(swapchain->format);
        target->color_views = NULL;
    }
    target->depth_view = NULL;
    target->depth_format =
        (swapchain->depth_format != VK_FORMAT_UNDEFINED)
            ? lc_vulkan_untranslate_format(swapchain->depth_format)
            : LC_FORMAT_UNDEFINED;
    target->samples = LC_SAMPLE_COUNT_1;
    target->hash = lc_render_target_hash(
        target->color_count, target->color_formats, target->depth_format,
        target->samples);
    target->is_swapchain_borrow = 1;
    target->framebuffer = VK_NULL_HANDLE;
    target->framebuffer_valid = 0;
    target->next = NULL;
    target->prev = NULL;
}

lc_result lc_vulkan_target_ensure_framebuffer(lc_render_target *target,
                                              VkRenderPass pass) {
    VkDevice device_handle;
    VkImageView attachments[LC_MAX_COLOR_ATTACHMENTS + 1];
    VkFramebufferCreateInfo info;
    uint32_t count;
    uint32_t i;

    if (target == NULL || target->device == NULL || pass == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (target->framebuffer_valid) {
        return LC_SUCCESS;
    }
    device_handle = target->device->device;
    if (device_handle == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    count = target->color_count;
    for (i = 0; i < count; i++) {
        if (target->color_views[i] == NULL ||
            target->color_views[i]->vk_view == VK_NULL_HANDLE) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        attachments[i] = target->color_views[i]->vk_view;
    }
    if (target->depth_view != NULL) {
        if (target->depth_view->vk_view == VK_NULL_HANDLE) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        attachments[count] = target->depth_view->vk_view;
        count++;
    }
    if (count == 0 || target->width == 0 || target->height == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = pass;
    info.attachmentCount = count;
    info.pAttachments = attachments;
    info.width = target->width;
    info.height = target->height;
    info.layers = 1;
    if (vkCreateFramebuffer(device_handle, &info, NULL,
                            &target->framebuffer) != VK_SUCCESS) {
        target->framebuffer = VK_NULL_HANDLE;
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }
    target->framebuffer_valid = 1;
    return LC_SUCCESS;
}
