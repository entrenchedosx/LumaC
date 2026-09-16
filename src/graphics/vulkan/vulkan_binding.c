/*
 * Vulkan binding backend (Phase 10: backend-neutral descriptors).
 *
 * Maps binding layouts to VkDescriptorSetLayouts, owns a private
 * growable descriptor allocator per device (pools are never exposed),
 * allocates/frees VkDescriptorSets, and records validated writes with
 * vkUpdateDescriptorSets. Validation here is deliberately thorough:
 * slots, types, array bounds, buffer ranges + alignment, image sampled
 * state over the view's exact range, device match, and liveness are
 * all checked before anything is recorded.
 */

#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

/* Sets per pool; pools grow by count, so this only bounds how often a
 * fresh pool is created. Per-type counts are generous against the
 * tens-of-thousands Vulkan minimums. */
#define LC_DESC_POOL_SETS 64
#define LC_DESC_POOL_UNIFORM_COUNT 64
#define LC_DESC_POOL_STORAGE_COUNT 64
#define LC_DESC_POOL_SAMPLED_COUNT 64
#define LC_DESC_POOL_STORAGE_IMAGE_COUNT 16
#define LC_DESC_POOL_SAMPLER_COUNT 64
#define LC_DESC_POOL_GROW 4

static VkShaderStageFlags lc_vk_visibility_stages(uint32_t visibility) {
    VkShaderStageFlags stages = 0;

    if ((visibility & LC_SHADER_VISIBILITY_VERTEX) != 0) {
        stages |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if ((visibility & LC_SHADER_VISIBILITY_FRAGMENT) != 0) {
        stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    if ((visibility & LC_SHADER_VISIBILITY_COMPUTE) != 0) {
        stages |= VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return stages;
}

static int lc_vk_binding_vk_type(lc_binding_type type,
                                 VkDescriptorType *out) {
    switch (type) {
    case LC_BINDING_UNIFORM_BUFFER:
        *out = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        return 1;
    case LC_BINDING_STORAGE_BUFFER:
        *out = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        return 1;
    case LC_BINDING_SAMPLED_IMAGE:
        *out = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        return 1;
    case LC_BINDING_STORAGE_IMAGE:
        *out = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        return 1;
    case LC_BINDING_SAMPLER:
        *out = VK_DESCRIPTOR_TYPE_SAMPLER;
        return 1;
    default:
        return 0;
    }
}

/* Cross-object liveness for update validation. Pointer comparison
 * only, so dead handles are rejected without ever dereferencing
 * freed memory. (Device liveness is established by the caller.) */
static int lc_binding_is_live_buffer(const lc_buffer *buffer) {
    lc_state *state = lc_get_internal_state();
    const lc_buffer *it;

    if (state == NULL || buffer == NULL) {
        return 0;
    }
    for (it = state->buffers; it != NULL; it = it->next) {
        if (it == buffer) {
            return 1;
        }
    }
    return 0;
}

static int lc_binding_is_live_view(const lc_image_view *view) {
    lc_state *state = lc_get_internal_state();
    const lc_image_view *it;

    if (state == NULL || view == NULL) {
        return 0;
    }
    for (it = state->image_views; it != NULL; it = it->next) {
        if (it == view) {
            return 1;
        }
    }
    return 0;
}

static int lc_binding_is_live_sampler(const lc_sampler *sampler) {
    lc_state *state = lc_get_internal_state();
    const lc_sampler *it;

    if (state == NULL || sampler == NULL) {
        return 0;
    }
    for (it = state->samplers; it != NULL; it = it->next) {
        if (it == sampler) {
            return 1;
        }
    }
    return 0;
}


/* Find a slot by number in a sorted snapshot (linear; counts are tiny). */
static const lc_binding_desc *lc_vk_find_slot(const lc_binding_desc *slots,
                                              uint32_t slot_count,
                                              uint32_t binding) {
    uint32_t i;

    if (slots == NULL) {
        return NULL;
    }
    for (i = 0; i < slot_count; i++) {
        if (slots[i].binding == binding) {
            return &slots[i];
        }
    }
    return NULL;
}

lc_result lc_vulkan_binding_layout_create(lc_binding_layout *layout,
                                          lc_device *device,
                                          const lc_binding_layout_desc *desc) {
    VkDescriptorSetLayoutBinding *vk_bindings = NULL;
    VkDescriptorSetLayoutCreateInfo info;
    uint32_t i;
    uint32_t j;

    if (layout == NULL || device == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->binding_count == 0 || desc->bindings == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE ||
        device->physical_device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    /* maxBoundDescriptorSets limits simultaneously bound descriptor-set
     * layouts in a pipeline layout, not bindings inside one set.  Vulkan
     * validates the applicable per-stage/per-type descriptor limits when
     * this layout is consumed by a pipeline. */
    for (i = 0; i < desc->binding_count; i++) {
        VkDescriptorType ignored;

        if (desc->bindings[i].count == 0 ||
            !lc_vk_binding_vk_type(desc->bindings[i].type, &ignored)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (lc_vk_visibility_stages(desc->bindings[i].visibility) == 0) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        for (j = 0; j < i; j++) {
            if (desc->bindings[j].binding == desc->bindings[i].binding) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
    }

    /* Sorted internal copy (insertion sort; counts are small). */
    layout->bindings = (lc_binding_desc *)malloc(
        sizeof(lc_binding_desc) * desc->binding_count);
    if (layout->bindings == NULL) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < desc->binding_count; i++) {
        uint32_t k = i;

        layout->bindings[i] = desc->bindings[i];
        while (k > 0 &&
               layout->bindings[k - 1].binding > layout->bindings[k].binding) {
            lc_binding_desc tmp = layout->bindings[k - 1];
            layout->bindings[k - 1] = layout->bindings[k];
            layout->bindings[k] = tmp;
            k--;
        }
    }
    layout->binding_count = desc->binding_count;
    layout->device = device;

    vk_bindings = (VkDescriptorSetLayoutBinding *)malloc(
        sizeof(VkDescriptorSetLayoutBinding) * desc->binding_count);
    if (vk_bindings == NULL) {
        free(layout->bindings);
        layout->bindings = NULL;
        layout->binding_count = 0;
        return LC_ERROR_OUT_OF_MEMORY;
    }
    for (i = 0; i < desc->binding_count; i++) {
        VkDescriptorType vk_type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;

        lc_vk_binding_vk_type(layout->bindings[i].type, &vk_type);
        memset(&vk_bindings[i], 0, sizeof(vk_bindings[i]));
        vk_bindings[i].binding = layout->bindings[i].binding;
        vk_bindings[i].descriptorType = vk_type;
        vk_bindings[i].descriptorCount = layout->bindings[i].count;
        vk_bindings[i].stageFlags =
            lc_vk_visibility_stages(layout->bindings[i].visibility);
        vk_bindings[i].pImmutableSamplers = NULL;
    }

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = desc->binding_count;
    info.pBindings = vk_bindings;
    if (vkCreateDescriptorSetLayout(device->device, &info, NULL,
                                    &layout->vk_layout) != VK_SUCCESS) {
        layout->vk_layout = VK_NULL_HANDLE;
        free(vk_bindings);
        free(layout->bindings);
        layout->bindings = NULL;
        layout->binding_count = 0;
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    free(vk_bindings);
    return LC_SUCCESS;
}

void lc_vulkan_binding_layout_destroy(lc_binding_layout *layout) {
    if (layout == NULL) {
        return;
    }
    /* Layouts are only referenced by anchor (never dereferenced after
     * death); the Vulkan object dies with its device still alive. */
    if (layout->vk_layout != VK_NULL_HANDLE && layout->device != NULL &&
        layout->device->device != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(layout->device->device,
                                     layout->vk_layout, NULL);
        layout->vk_layout = VK_NULL_HANDLE;
    }
    if (layout->bindings != NULL) {
        free(layout->bindings);
        layout->bindings = NULL;
    }
    layout->binding_count = 0;
}

/* Create one pool sized for LC_DESC_POOL_SETS sets of mixed content. */
static lc_result lc_vk_desc_pool_grow(lc_device *device) {
    VkDescriptorPoolSize sizes[5];
    VkDescriptorPoolCreateInfo info;
    VkDescriptorPool *grown = NULL;
    VkDescriptorPool pool = VK_NULL_HANDLE;

    memset(sizes, 0, sizeof(sizes));
    sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    sizes[0].descriptorCount = LC_DESC_POOL_UNIFORM_COUNT;
    sizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sizes[1].descriptorCount = LC_DESC_POOL_STORAGE_COUNT;
    sizes[2].type = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    sizes[2].descriptorCount = LC_DESC_POOL_SAMPLED_COUNT;
    sizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sizes[3].descriptorCount = LC_DESC_POOL_STORAGE_IMAGE_COUNT;
    sizes[4].type = VK_DESCRIPTOR_TYPE_SAMPLER;
    sizes[4].descriptorCount = LC_DESC_POOL_SAMPLER_COUNT;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    /* Individually freeable sets: destruction returns allocations. */
    info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    info.maxSets = LC_DESC_POOL_SETS;
    info.poolSizeCount = 5;
    info.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device->device, &info, NULL, &pool) !=
        VK_SUCCESS) {
        return LC_ERROR_OUT_OF_MEMORY;
    }

    grown = (VkDescriptorPool *)realloc(
        device->desc_pools,
        sizeof(VkDescriptorPool) *
            (device->desc_pool_capacity + LC_DESC_POOL_GROW));
    if (grown == NULL) {
        vkDestroyDescriptorPool(device->device, pool, NULL);
        return LC_ERROR_OUT_OF_MEMORY;
    }
    device->desc_pools = grown;
    device->desc_pool_capacity += LC_DESC_POOL_GROW;
    device->desc_pools[device->desc_pool_count] = pool;
    device->desc_pool_count++;
    return LC_SUCCESS;
}

/* Allocate one set, growing the pool list on demand. */
static lc_result lc_vk_desc_alloc(lc_device *device,
                                  VkDescriptorSetLayout vk_layout,
                                  VkDescriptorPool *out_pool,
                                  VkDescriptorSet *out_set) {
    VkDescriptorSetAllocateInfo info;
    uint32_t i;

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &vk_layout;
    for (i = 0; i < device->desc_pool_count; i++) {
        info.descriptorPool = device->desc_pools[i];
        if (vkAllocateDescriptorSets(device->device, &info, out_set) ==
            VK_SUCCESS) {
            *out_pool = device->desc_pools[i];
            return LC_SUCCESS;
        }
    }
    if (lc_vk_desc_pool_grow(device) != LC_SUCCESS) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    info.descriptorPool = device->desc_pools[device->desc_pool_count - 1];
    if (vkAllocateDescriptorSets(device->device, &info, out_set) !=
        VK_SUCCESS) {
        return LC_ERROR_OUT_OF_MEMORY;
    }
    *out_pool = device->desc_pools[device->desc_pool_count - 1];
    return LC_SUCCESS;
}

void lc_vulkan_desc_teardown(lc_device *device) {
    uint32_t i;

    if (device == NULL) {
        return;
    }
    /* All sets must already be freed (set destroy runs first via
     * hooks); pools then die unconditionally with the device. */
    if (device->desc_pools != NULL) {
        if (device->device != VK_NULL_HANDLE) {
            for (i = 0; i < device->desc_pool_count; i++) {
                if (device->desc_pools[i] != VK_NULL_HANDLE) {
                    vkDestroyDescriptorPool(device->device,
                                            device->desc_pools[i], NULL);
                }
            }
        }
        free(device->desc_pools);
        device->desc_pools = NULL;
    }
    device->desc_pool_count = 0;
    device->desc_pool_capacity = 0;
}

lc_result lc_vulkan_binding_set_create(lc_binding_set *set,
                                       const lc_binding_layout *layout) {
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet vk_set = VK_NULL_HANDLE;
    lc_result res;

    if (set == NULL || layout == NULL || layout->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (layout->vk_layout == VK_NULL_HANDLE ||
        layout->device->device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    /* Shared pool allocator across threads (PART 24 shard: one
     * dedicated lock today, pool-per-thread sharding reserved). */
    lc_device_lock_desc(layout->device);
    res = lc_vk_desc_alloc(layout->device, layout->vk_layout, &pool,
                           &vk_set);
    lc_device_unlock_desc(layout->device);
    if (res != LC_SUCCESS) {
        return res;
    }

    /* Snapshot the slots so later updates never touch the layout
     * object (which may legally die first). */
    set->slots = NULL;
    set->slot_count = 0;
    if (layout->binding_count > 0) {
        set->slots = (lc_binding_desc *)malloc(sizeof(lc_binding_desc) *
                                               layout->binding_count);
        if (set->slots == NULL) {
            lc_device_lock_desc(layout->device);
            vkFreeDescriptorSets(layout->device->device, pool, 1,
                                 &vk_set);
            lc_device_unlock_desc(layout->device);
            return LC_ERROR_OUT_OF_MEMORY;
        }
        memcpy(set->slots, layout->bindings,
               sizeof(lc_binding_desc) * layout->binding_count);
        set->slot_count = layout->binding_count;
    }
    set->device = layout->device;
    set->layout = layout;
    set->vk_set = vk_set;
    set->vk_pool = pool;
    return LC_SUCCESS;
}

void lc_vulkan_binding_set_destroy(lc_binding_set *set) {
    if (set == NULL) {
        return;
    }
    if (set->vk_set != VK_NULL_HANDLE && set->device != NULL &&
        set->device->device != VK_NULL_HANDLE &&
        set->vk_pool != VK_NULL_HANDLE) {
        lc_device_lock_desc(set->device);
        vkFreeDescriptorSets(set->device->device, set->vk_pool, 1,
                             &set->vk_set);
        lc_device_unlock_desc(set->device);
        set->vk_set = VK_NULL_HANDLE;
        set->vk_pool = VK_NULL_HANDLE;
    }
    if (set->slots != NULL) {
        free(set->slots);
        set->slots = NULL;
    }
    set->slot_count = 0;
    set->layout = NULL;
}

/* Validate one buffer write: liveness, device match, VERTEX... no -
 * any usage is bindable here (type match is checked by the caller
 * against the slot), range inside the buffer, alignment per kind. */
static lc_result lc_vk_validate_buffer_write(
    lc_device *device, const lc_binding_desc *slot,
    const lc_buffer_binding *binding, uint64_t *out_size) {
    VkPhysicalDeviceProperties props;
    uint64_t align = 1;

    if (binding->buffer == NULL || !lc_binding_is_live_buffer(binding->buffer)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (binding->buffer->device != device ||
        binding->buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Tracked-state agreement (Phase 21, PART J): UNDEFINED is the
     * legacy lenient path (buffers predate tracking); TRANSFER_DST
     * is transfer-ordered data (every frame submit waits all
     * in-flight transfers, and completion is monotonic, so reads
     * after any upload are safe without an extra barrier). Other
     * states must name a compatible read: this still catches
     * compute/vertex/index/indirect misuse and wrong-slot reuse.
     * Writers transition explicitly via
     * lc_encoder_transition_buffer. */
    {
        lc_resource_state st = binding->buffer->buffer_state;

        if (st != LC_RESOURCE_STATE_UNDEFINED &&
            st != LC_RESOURCE_STATE_TRANSFER_DST) {
            if (slot->type == LC_BINDING_UNIFORM_BUFFER &&
                st != LC_RESOURCE_STATE_UNIFORM_READ) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
            if (slot->type == LC_BINDING_STORAGE_BUFFER &&
                st != LC_RESOURCE_STATE_STORAGE_READ &&
                st != LC_RESOURCE_STATE_STORAGE_WRITE &&
                st != LC_RESOURCE_STATE_STORAGE_READ_WRITE) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    if (binding->offset >= binding->buffer->size) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (binding->size == 0) {
        *out_size = binding->buffer->size - binding->offset;
    } else {
        if (binding->size > binding->buffer->size - binding->offset) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        *out_size = binding->size;
    }
    /* Offset alignment is mandatory for uniform/storage bindings even
     * without dynamic offsets. */
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(device->physical_device, &props);
    if (slot->type == LC_BINDING_UNIFORM_BUFFER) {
        align = props.limits.minUniformBufferOffsetAlignment;
    } else if (slot->type == LC_BINDING_STORAGE_BUFFER) {
        align = props.limits.minStorageBufferOffsetAlignment;
    }
    if (align > 1 && (binding->offset % align) != 0u) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return LC_SUCCESS;
}

/* Validate one image-view write: liveness, device match, and
 * tracked readability over the view's exact range. Sampled images
 * require SHADER_READ; storage images require SHADER_READ_WRITE
 * (GENERAL layout), matching the compute storage path. */
static lc_result lc_vk_validate_image_write(
    lc_device *device, const lc_image_view *view,
    lc_binding_type type) {
    if (view == NULL || !lc_binding_is_live_view(view)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (view->device != device || view->vk_view == VK_NULL_HANDLE ||
        view->image == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_sync_all_equal(view->image, view->base_mip_level,
                              view->mip_level_count, view->base_array_layer,
                              view->array_layer_count,
                              (type == LC_BINDING_STORAGE_IMAGE)
                                  ? LC_RESOURCE_STATE_SHADER_READ_WRITE
                                  : LC_RESOURCE_STATE_SHADER_READ)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    return LC_SUCCESS;
}

/* Validate one write into temp Vulkan structs (no recording yet).
 * Buffer/image/sampler infos live in caller arrays indexed by write. */
static lc_result lc_vk_validate_write(
    lc_device *device, const lc_binding_set *set,
    const lc_binding_write *write, VkWriteDescriptorSet *vk_write,
    VkDescriptorBufferInfo *buffer_info, VkDescriptorImageInfo *image_info) {
    const lc_binding_desc *slot;

    slot = lc_vk_find_slot(set->slots, set->slot_count, write->binding);
    if (slot == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (write->type != slot->type) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (write->array_element >= slot->count) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    memset(vk_write, 0, sizeof(*vk_write));
    vk_write->sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    vk_write->dstSet = set->vk_set;
    vk_write->dstBinding = write->binding;
    vk_write->dstArrayElement = write->array_element;
    vk_write->descriptorCount = 1;

    switch (write->type) {
    case LC_BINDING_UNIFORM_BUFFER:
    case LC_BINDING_STORAGE_BUFFER: {
        uint64_t size = 0;
        lc_result res = lc_vk_validate_buffer_write(device, slot,
                                                    &write->u.buffer, &size);

        if (res != LC_SUCCESS) {
            return res;
        }
        memset(buffer_info, 0, sizeof(*buffer_info));
        buffer_info->buffer = write->u.buffer.buffer->vk_buffer;
        buffer_info->offset = (VkDeviceSize)write->u.buffer.offset;
        buffer_info->range = (VkDeviceSize)size;
        vk_write->descriptorType =
            (write->type == LC_BINDING_UNIFORM_BUFFER)
                ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        vk_write->pBufferInfo = buffer_info;
        return LC_SUCCESS;
    }
    case LC_BINDING_SAMPLED_IMAGE:
    case LC_BINDING_STORAGE_IMAGE: {
        lc_result res;
        const lc_image_view *view = write->u.image.view;

        if (view == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        res = lc_vk_validate_image_write(device, view, write->type);
        if (res != LC_SUCCESS) {
            return res;
        }
        memset(image_info, 0, sizeof(*image_info));
        image_info->imageView = view->vk_view;
        image_info->imageLayout =
            (write->type == LC_BINDING_SAMPLED_IMAGE)
                ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                : VK_IMAGE_LAYOUT_GENERAL;
        vk_write->descriptorType =
            (write->type == LC_BINDING_SAMPLED_IMAGE)
                ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
                : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        vk_write->pImageInfo = image_info;
        return LC_SUCCESS;
    }
    case LC_BINDING_SAMPLER: {
        if (write->u.sampler.sampler == NULL ||
            !lc_binding_is_live_sampler(write->u.sampler.sampler)) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (write->u.sampler.sampler->device != device ||
            write->u.sampler.sampler->vk_sampler == VK_NULL_HANDLE) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        memset(image_info, 0, sizeof(*image_info));
        image_info->sampler = write->u.sampler.sampler->vk_sampler;
        vk_write->descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        vk_write->pImageInfo = image_info;
        return LC_SUCCESS;
    }
    default:
        return LC_ERROR_INVALID_ARGUMENT;
    }
}

lc_result lc_vulkan_binding_set_update(lc_binding_set *set,
                                       const lc_binding_write *writes,
                                       uint32_t write_count) {
    lc_device *device;
    VkWriteDescriptorSet *vk_writes = NULL;
    VkDescriptorBufferInfo *buffer_infos = NULL;
    VkDescriptorImageInfo *image_infos = NULL;
    uint32_t i;
    lc_result res = LC_SUCCESS;

    if (set == NULL || set->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (write_count > 0 && writes == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (write_count == 0) {
        return LC_SUCCESS;
    }
    device = set->device;
    if (device->device == VK_NULL_HANDLE || set->vk_set == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }

    vk_writes = (VkWriteDescriptorSet *)calloc(write_count,
                                               sizeof(VkWriteDescriptorSet));
    buffer_infos = (VkDescriptorBufferInfo *)calloc(
        write_count, sizeof(VkDescriptorBufferInfo));
    image_infos = (VkDescriptorImageInfo *)calloc(
        write_count, sizeof(VkDescriptorImageInfo));
    if (vk_writes == NULL || buffer_infos == NULL || image_infos == NULL) {
        free(vk_writes);
        free(buffer_infos);
        free(image_infos);
        return LC_ERROR_OUT_OF_MEMORY;
    }
    /* Validate everything first: a bad write leaves prior state of
     * this call unrecorded (all-or-nothing per batch). */
    for (i = 0; i < write_count; i++) {
        res = lc_vk_validate_write(device, set, &writes[i], &vk_writes[i],
                                   &buffer_infos[i], &image_infos[i]);
        if (res != LC_SUCCESS) {
            break;
        }
    }
    if (res == LC_SUCCESS) {
        /* Same-set concurrent updates are an application bug, but
         * cross-set updates from threads must not race pool
         * internals: serialize the Vulkan call itself. */
        lc_device_lock_desc(device);
        vkUpdateDescriptorSets(device->device, write_count, vk_writes, 0,
                               NULL);
        lc_device_unlock_desc(device);
    }
    free(vk_writes);
    free(buffer_infos);
    free(image_infos);
    return res;
}
