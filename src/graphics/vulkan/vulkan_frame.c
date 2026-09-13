/*
 * Vulkan frame backend (acquire, clear/draw, submit, present).
 *
 * Swapchain-local frame lifecycle with LC_MAX_FRAMES_IN_FLIGHT slots.
 * Each slot owns its semaphores, fence, and reusable command buffer;
 * per-image fences track which submission still owns an image.
 *
 * Recording happens inside one minimal render pass per frame (clear
 * via load op or clear attachments, then bound-pipeline draws), so no
 * manual layout transitions are needed. Queue families never change
 * under a swapchain, and concurrent sharing (when graphics and
 * present families differ) needs no ownership transfers.
 *
 * Out-of-date and suboptimal conditions map to recoverable results;
 * anything else fatal surfaces as a plain error. Submit failure is
 * documented as fatal for the swapchain (device-loss territory).
 */

#include <string.h>

#include "graphics/graphics_internal.h"

static float lc_vk_clamp01(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

/* Begin the render pass instance if not already open, consuming the
 * pending clear color/depth (opaque black / 1.0 when no clear was
 * requested). Viewport and scissor are dynamic, so they are set here
 * every time from the current extent: resizing never requires pipeline
 * rebuilds. */
static void lc_vk_open_render_pass(lc_swapchain *swapchain) {
    lc_vk_flight *flight =
        &swapchain->flights[swapchain->current_frame];
    VkClearValue clear_values[2];
    VkRenderPassBeginInfo begin_info;
    VkViewport viewport;
    VkRect2D scissor;

    clear_values[0].color.float32[0] =
        swapchain->clear_pending ? swapchain->clear_r : 0.0f;
    clear_values[0].color.float32[1] =
        swapchain->clear_pending ? swapchain->clear_g : 0.0f;
    clear_values[0].color.float32[2] =
        swapchain->clear_pending ? swapchain->clear_b : 0.0f;
    clear_values[0].color.float32[3] =
        swapchain->clear_pending ? swapchain->clear_a : 1.0f;
    swapchain->clear_pending = 0;
    clear_values[1].depthStencil.depth =
        swapchain->depth_clear_pending ? swapchain->depth_clear : 1.0f;
    clear_values[1].depthStencil.stencil = 0;
    swapchain->depth_clear_pending = 0;

    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    begin_info.renderPass = swapchain->render_pass;
    begin_info.framebuffer =
        swapchain->framebuffers[swapchain->current_image];
    begin_info.renderArea.offset.x = 0;
    begin_info.renderArea.offset.y = 0;
    begin_info.renderArea.extent = swapchain->extent;
    begin_info.clearValueCount = 2;
    begin_info.pClearValues = clear_values;
    vkCmdBeginRenderPass(flight->cmd, &begin_info,
                         VK_SUBPASS_CONTENTS_INLINE);

    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)swapchain->extent.width;
    viewport.height = (float)swapchain->extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(flight->cmd, 0, 1, &viewport);

    memset(&scissor, 0, sizeof(scissor));
    scissor.extent = swapchain->extent;
    vkCmdSetScissor(flight->cmd, 0, 1, &scissor);

    swapchain->rp_open = 1;
}

/* Best-effort unwind of partially created frame objects. All handles
 * are NULL-checked; the struct is left empty. */
static void lc_vk_frame_unwind(lc_swapchain *swapchain, VkDevice device) {
    uint32_t i;

    for (i = 0; i < LC_MAX_FRAMES_IN_FLIGHT; i++) {
        lc_vk_flight *flight = &swapchain->flights[i];
        if (flight->image_available != VK_NULL_HANDLE) {
            vkDestroySemaphore(device, flight->image_available, NULL);
            flight->image_available = VK_NULL_HANDLE;
        }
        if (flight->fence != VK_NULL_HANDLE) {
            vkDestroyFence(device, flight->fence, NULL);
            flight->fence = VK_NULL_HANDLE;
        }
        flight->cmd = VK_NULL_HANDLE;
    }
    if (swapchain->cmd_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, swapchain->cmd_pool, NULL);
        swapchain->cmd_pool = VK_NULL_HANDLE;
    }
}

lc_result lc_vulkan_frame_init(lc_swapchain *swapchain) {
    VkDevice device;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo alloc_info;
    VkCommandBuffer cmds[LC_MAX_FRAMES_IN_FLIGHT];
    uint32_t i;

    if (swapchain == NULL || swapchain->device == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    device = swapchain->device->device;
    if (device == VK_NULL_HANDLE) {
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }

    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    /* Per-buffer reset each frame; the pool itself lives as long as
     * the swapchain's frame state (across recreates). */
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = swapchain->device->graphics_queue_family;

    if (vkCreateCommandPool(device, &pool_info, NULL, &swapchain->cmd_pool) !=
        VK_SUCCESS) {
        swapchain->cmd_pool = VK_NULL_HANDLE;
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }

    for (i = 0; i < LC_MAX_FRAMES_IN_FLIGHT; i++) {
        lc_vk_flight *flight = &swapchain->flights[i];
        VkSemaphoreCreateInfo sem_info;
        VkFenceCreateInfo fence_info;

        memset(&sem_info, 0, sizeof(sem_info));
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        memset(&fence_info, 0, sizeof(fence_info));
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        /* Start signaled so the very first frame never deadlocks
         * waiting on a fence no submission has signaled yet. */
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        if (vkCreateSemaphore(device, &sem_info, NULL,
                              &flight->image_available) != VK_SUCCESS ||
            vkCreateFence(device, &fence_info, NULL, &flight->fence) !=
                VK_SUCCESS) {
            lc_vk_frame_unwind(swapchain, device);
            return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
        }
    }

    /* One primary buffer per slot, allocated once and reused. */
    memset(&alloc_info, 0, sizeof(alloc_info));
    alloc_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    alloc_info.commandPool = swapchain->cmd_pool;
    alloc_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc_info.commandBufferCount = LC_MAX_FRAMES_IN_FLIGHT;
    if (vkAllocateCommandBuffers(device, &alloc_info, cmds) != VK_SUCCESS) {
        lc_vk_frame_unwind(swapchain, device);
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }
    for (i = 0; i < LC_MAX_FRAMES_IN_FLIGHT; i++) {
        swapchain->flights[i].cmd = cmds[i];
    }
    return LC_SUCCESS;
}

void lc_vulkan_frame_teardown(lc_swapchain *swapchain) {
    VkDevice device = VK_NULL_HANDLE;
    uint32_t i;

    if (swapchain == NULL) {
        return;
    }
    /* Requires no in-flight submissions: callers run the image teardown
     * (which waits idle) first. Pool destroy implicitly frees the
     * command buffers, including any begun-but-unsubmitted recording. */
    if (swapchain->device != NULL) {
        device = swapchain->device->device;
    }
    for (i = 0; i < LC_MAX_FRAMES_IN_FLIGHT; i++) {
        lc_vk_flight *flight = &swapchain->flights[i];
        if (device != VK_NULL_HANDLE) {
            if (flight->image_available != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, flight->image_available, NULL);
            }
            if (flight->fence != VK_NULL_HANDLE) {
                vkDestroyFence(device, flight->fence, NULL);
            }
        }
        flight->image_available = VK_NULL_HANDLE;
        flight->fence = VK_NULL_HANDLE;
        flight->cmd = VK_NULL_HANDLE;
    }
    if (swapchain->cmd_pool != VK_NULL_HANDLE) {
        if (device != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, swapchain->cmd_pool, NULL);
        }
        swapchain->cmd_pool = VK_NULL_HANDLE;
    }
    swapchain->current_frame = 0;
    swapchain->current_image = UINT32_MAX;
    swapchain->frame_active = 0;
    swapchain->frame_suboptimal = 0;
    memset(&swapchain->encoder, 0, sizeof(swapchain->encoder));
    swapchain->encoder.swapchain = swapchain;
    swapchain->encoder.device = swapchain->device;
}

/* Backend handles must exist; the public layer already proved the
 * swapchain pointer live. Returns 1 when usable, 0 otherwise. */
static int lc_vk_frame_ready(const lc_swapchain *swapchain) {
    return swapchain != NULL && swapchain->device != NULL &&
           swapchain->surface != NULL &&
           swapchain->device->device != VK_NULL_HANDLE &&
           swapchain->vk_swapchain != VK_NULL_HANDLE &&
           swapchain->cmd_pool != VK_NULL_HANDLE &&
           swapchain->current_frame < LC_MAX_FRAMES_IN_FLIGHT;
}

lc_result lc_vulkan_frame_begin(lc_swapchain *swapchain) {
    lc_device *device;
    lc_vk_flight *flight;
    VkResult acquire_res;
    uint32_t image_index = UINT32_MAX;
    VkFence owner;
    VkCommandBufferBeginInfo begin_info;

    if (swapchain == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_frame_ready(swapchain)) {
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }
    device = swapchain->device;
    flight = &swapchain->flights[swapchain->current_frame];

    /* A free slot: the previous use of this slot has completed. */
    if (vkWaitForFences(device->device, 1, &flight->fence, VK_TRUE,
                        UINT64_MAX) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }

    acquire_res = vkAcquireNextImageKHR(device->device,
                                        swapchain->vk_swapchain, UINT64_MAX,
                                        flight->image_available,
                                        VK_NULL_HANDLE, &image_index);
    if (acquire_res == VK_ERROR_OUT_OF_DATE_KHR) {
        /* Nothing started: fence untouched (still signaled), no frame
         * open. Caller recreates and retries. */
        return LC_ERROR_SWAPCHAIN_OUT_OF_DATE;
    }
    if (acquire_res == VK_SUBOPTIMAL_KHR) {
        swapchain->frame_suboptimal = 1;
    } else if (acquire_res == VK_ERROR_SURFACE_LOST_KHR) {
        return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
    } else if (acquire_res != VK_SUCCESS) {
        return LC_ERROR_SWAPCHAIN_CREATION_FAILED;
    }
    if (image_index >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }

    /* Frames in flight can exceed image count: an image may still be
     * owned by an earlier submission. Wait it out before reuse. */
    owner = swapchain->images_in_flight[image_index];
    if (owner != VK_NULL_HANDLE && owner != flight->fence) {
        if (vkWaitForFences(device->device, 1, &owner, VK_TRUE,
                            UINT64_MAX) != VK_SUCCESS) {
            return LC_ERROR_UNKNOWN;
        }
    }
    swapchain->images_in_flight[image_index] = flight->fence;

    if (vkResetFences(device->device, 1, &flight->fence) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    if (vkResetCommandBuffer(flight->cmd, 0) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }
    memset(&begin_info, 0, sizeof(begin_info));
    begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(flight->cmd, &begin_info) != VK_SUCCESS) {
        return LC_ERROR_UNKNOWN;
    }

    /* Fresh recording state: no pipeline bound, no index bound, no
     * render pass open, no clear consumed yet. Depth defaults to 1.0.
     * The Phase 12 inline encoder is reset too (no explicit pass).
     * Layout transitions are owned entirely by the render pass
     * (UNDEFINED in, PRESENT out), so every frame is self-contained
     * with no per-image history. */
    swapchain->current_image = image_index;
    swapchain->frame_active = 1;
    swapchain->bound_pipeline = NULL;
    swapchain->rp_open = 0;
    swapchain->clear_pending = 0;
    swapchain->depth_clear_pending = 0;
    swapchain->depth_clear = 1.0f;
    swapchain->bound_index_buffer = NULL;
    swapchain->bound_index_offset = 0;
    swapchain->index_bound = 0;
    memset(&swapchain->encoder, 0, sizeof(swapchain->encoder));
    swapchain->encoder.swapchain = swapchain;
    swapchain->encoder.device = swapchain->device;
    return LC_SUCCESS;
}

/* Phase 12: legacy implicit recording and explicit encoder passes are
 * mutually exclusive — legacy ops fail while an explicit pass is
 * open (use the encoder APIs instead). */
#define LC_ENC_GUARD(sw)                          \
    do {                                          \
        if ((sw)->encoder.in_pass) {              \
            return LC_ERROR_INVALID_ARGUMENT;     \
        }                                         \
    } while (0)

lc_result lc_vulkan_frame_clear(lc_swapchain *swapchain, float r, float g,
                                float b, float a) {
    float cr = lc_vk_clamp01(r);
    float cg = lc_vk_clamp01(g);
    float cb = lc_vk_clamp01(b);
    float ca = lc_vk_clamp01(a);

    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    LC_ENC_GUARD(swapchain);
    if (!lc_vk_frame_ready(swapchain) ||
        swapchain->current_image >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }
    if (swapchain->rp_open) {
        /* A pass is already recording (bind or draw happened): clear
         * inside it so ordering stays exact. */
        lc_vk_flight *flight =
            &swapchain->flights[swapchain->current_frame];
        VkClearAttachment attachment;
        VkClearRect rect;

        memset(&attachment, 0, sizeof(attachment));
        attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        attachment.colorAttachment = 0;
        attachment.clearValue.color.float32[0] = cr;
        attachment.clearValue.color.float32[1] = cg;
        attachment.clearValue.color.float32[2] = cb;
        attachment.clearValue.color.float32[3] = ca;

        memset(&rect, 0, sizeof(rect));
        rect.rect.extent = swapchain->extent;
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;

        vkCmdClearAttachments(flight->cmd, 1, &attachment, 1, &rect);
        return LC_SUCCESS;
    }
    /* Otherwise the color is consumed when the pass begins; the latest
     * clear wins. */
    swapchain->clear_r = cr;
    swapchain->clear_g = cg;
    swapchain->clear_b = cb;
    swapchain->clear_a = ca;
    swapchain->clear_pending = 1;
    return LC_SUCCESS;
}

static float lc_vk_clamp_depth(float v) {
    if (v < 0.0f) {
        return 0.0f;
    }
    if (v > 1.0f) {
        return 1.0f;
    }
    return v;
}

lc_result lc_vulkan_frame_clear_depth(lc_swapchain *swapchain, float depth) {
    float d = lc_vk_clamp_depth(depth);

    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    LC_ENC_GUARD(swapchain);
    if (!lc_vk_frame_ready(swapchain) ||
        swapchain->current_image >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }
    if (swapchain->rp_open) {
        lc_vk_flight *flight =
            &swapchain->flights[swapchain->current_frame];
        VkClearAttachment attachment;
        VkClearRect rect;

        memset(&attachment, 0, sizeof(attachment));
        attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        attachment.clearValue.depthStencil.depth = d;
        attachment.clearValue.depthStencil.stencil = 0;

        memset(&rect, 0, sizeof(rect));
        rect.rect.extent = swapchain->extent;
        rect.baseArrayLayer = 0;
        rect.layerCount = 1;

        vkCmdClearAttachments(flight->cmd, 1, &attachment, 1, &rect);
        return LC_SUCCESS;
    }
    swapchain->depth_clear = d;
    swapchain->depth_clear_pending = 1;
    return LC_SUCCESS;
}

lc_result lc_vulkan_frame_bind(lc_swapchain *swapchain,
                               const lc_pipeline *pipeline) {
    lc_vk_flight *flight;

    if (swapchain == NULL || pipeline == NULL ||
        !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    LC_ENC_GUARD(swapchain);
    if (!lc_vk_frame_ready(swapchain) ||
        swapchain->current_image >= swapchain->image_count ||
        swapchain->render_pass == VK_NULL_HANDLE) {
        return LC_ERROR_UNKNOWN;
    }
    flight = &swapchain->flights[swapchain->current_frame];

    if (!swapchain->rp_open) {
        lc_vk_open_render_pass(swapchain);
    }
    vkCmdBindPipeline(flight->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      pipeline->pipeline);
    swapchain->bound_pipeline = pipeline;
    return LC_SUCCESS;
}

lc_result lc_vulkan_frame_draw(lc_swapchain *swapchain, uint32_t vertex_count,
                               uint32_t first_vertex) {
    lc_vk_flight *flight;

    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    LC_ENC_GUARD(swapchain);
    if (!lc_vk_frame_ready(swapchain) ||
        swapchain->current_image >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }
    if (swapchain->bound_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = &swapchain->flights[swapchain->current_frame];

    if (!swapchain->rp_open) {
        lc_vk_open_render_pass(swapchain);
    }
    vkCmdDraw(flight->cmd, vertex_count, 1, first_vertex, 0);
    return LC_SUCCESS;
}

lc_result lc_vulkan_frame_draw_instanced(lc_swapchain *swapchain,
                                         uint32_t vertex_count,
                                         uint32_t instance_count,
                                         uint32_t first_vertex,
                                         uint32_t first_instance) {
    lc_vk_flight *flight;

    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    LC_ENC_GUARD(swapchain);
    if (!lc_vk_frame_ready(swapchain) ||
        swapchain->current_image >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }
    if (swapchain->bound_pipeline == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = &swapchain->flights[swapchain->current_frame];

    if (!swapchain->rp_open) {
        lc_vk_open_render_pass(swapchain);
    }
    vkCmdDraw(flight->cmd, vertex_count, instance_count, first_vertex,
              first_instance);
    return LC_SUCCESS;
}

lc_result lc_vulkan_frame_draw_indexed(lc_swapchain *swapchain,
                                       uint32_t index_count,
                                       uint32_t instance_count,
                                       uint32_t first_index,
                                       int32_t vertex_offset,
                                       uint32_t first_instance) {
    lc_vk_flight *flight;

    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    LC_ENC_GUARD(swapchain);
    if (!lc_vk_frame_ready(swapchain) ||
        swapchain->current_image >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }
    if (swapchain->bound_pipeline == NULL || !swapchain->index_bound ||
        swapchain->bound_index_buffer == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = &swapchain->flights[swapchain->current_frame];

    if (!swapchain->rp_open) {
        lc_vk_open_render_pass(swapchain);
    }
    vkCmdDrawIndexed(flight->cmd, index_count, instance_count, first_index,
                     vertex_offset, first_instance);
    return LC_SUCCESS;
}

lc_result lc_vulkan_frame_bind_index(lc_swapchain *swapchain,
                                     const lc_buffer *buffer,
                                     uint64_t offset,
                                     lc_index_type index_type) {
    VkPhysicalDeviceProperties props;
    lc_vk_flight *flight;
    VkDeviceSize vk_offset;
    VkIndexType vk_type;

    if (swapchain == NULL || buffer == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    LC_ENC_GUARD(swapchain);
    if (swapchain->current_frame >= LC_MAX_FRAMES_IN_FLIGHT ||
        buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_UNKNOWN;
    }
    if (index_type != LC_INDEX_UINT16 && index_type != LC_INDEX_UINT32) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(swapchain->device->physical_device, &props);
    if ( LC_INDEX_UINT16 == index_type ) {
        vk_type = VK_INDEX_TYPE_UINT16;
    } else {
        vk_type = VK_INDEX_TYPE_UINT32;
    }

    flight = &swapchain->flights[swapchain->current_frame];
    vk_offset = (VkDeviceSize)offset;
    vkCmdBindIndexBuffer(flight->cmd, buffer->vk_buffer, vk_offset, vk_type);
    /* Caller validated liveness, device, usage, and alignment; record
     * the binding for indexed-draw validation. */
    swapchain->bound_index_buffer = buffer;
    swapchain->bound_index_offset = offset;
    swapchain->bound_index_type = index_type;
    swapchain->index_bound = 1;
    return LC_SUCCESS;
}

static VkShaderStageFlags lc_vk_push_stages_frame(uint32_t visibility) {
    VkShaderStageFlags stages = 0;

    if ((visibility & LC_SHADER_VISIBILITY_VERTEX) != 0) {
        stages |= VK_SHADER_STAGE_VERTEX_BIT;
    }
    if ((visibility & LC_SHADER_VISIBILITY_FRAGMENT) != 0) {
        stages |= VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    return stages;
}

lc_result lc_vulkan_frame_push(lc_swapchain *swapchain,
                               const lc_pipeline *pipeline,
                               uint32_t visibility, uint32_t offset,
                               uint32_t size, const void *data) {
    lc_vk_flight *flight;
    VkShaderStageFlags stages;

    if (swapchain == NULL || pipeline == NULL || data == NULL ||
        !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    LC_ENC_GUARD(swapchain);
    if (!lc_vk_frame_ready(swapchain) ||
        swapchain->current_image >= swapchain->image_count ||
        pipeline->layout == VK_NULL_HANDLE) {
        return LC_ERROR_UNKNOWN;
    }
    if (swapchain->bound_pipeline != pipeline) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Caller validated the window against the pipeline's declared
     * ranges; map stages and record. */
    stages = lc_vk_push_stages_frame(visibility);
    if (stages == 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = &swapchain->flights[swapchain->current_frame];
    vkCmdPushConstants(flight->cmd, pipeline->layout, stages, offset, size,
                       data);
    return LC_SUCCESS;
}

lc_result lc_vulkan_frame_bind_set(lc_swapchain *swapchain,
                                   const lc_pipeline *pipeline,
                                   uint32_t slot, const lc_binding_set *set) {
    lc_vk_flight *flight;

    if (swapchain == NULL || pipeline == NULL || set == NULL ||
        !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    LC_ENC_GUARD(swapchain);
    if (!lc_vk_frame_ready(swapchain) ||
        swapchain->current_image >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }
    if (slot >= pipeline->layout_count ||
        set->vk_set == VK_NULL_HANDLE ||
        pipeline->layout == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    flight = &swapchain->flights[swapchain->current_frame];

    /* Descriptor binds need no open render pass; any record order
     * relative to pipeline binds is legal. Compatibility was verified
     * by the caller via layout anchors; Vulkan validation re-checks
     * the real objects. */
    vkCmdBindDescriptorSets(flight->cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipeline->layout, slot, 1, &set->vk_set, 0,
                            NULL);
    return LC_SUCCESS;
}

lc_result lc_vulkan_frame_end(lc_swapchain *swapchain) {
    lc_device *device;
    lc_surface *surface;
    lc_vk_flight *flight;
    VkSubmitInfo submit_info;
    VkPipelineStageFlags wait_stage =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    VkPresentInfoKHR present_info;
    VkResult present_res;
    VkSemaphore present_sem = VK_NULL_HANDLE;
    int reported_suboptimal;

    if (swapchain == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Phase 12: submitting while an explicit pass is open would emit
     * an unterminated render pass. Keep the frame alive so the caller
     * can end the pass and retry. */
    if (swapchain->encoder.in_pass) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (!lc_vk_frame_ready(swapchain) ||
        swapchain->current_image >= swapchain->image_count) {
        return LC_ERROR_UNKNOWN;
    }
    device = swapchain->device;
    surface = swapchain->surface;
    flight = &swapchain->flights[swapchain->current_frame];
    /* Per-image present semaphore: the submit signals it and the
     * presentation engine consumes it asynchronously, so a per-slot
     * semaphore could be re-signaled for a new image while an older
     * present still holds it. */
    if (swapchain->present_semaphores == NULL) {
        return LC_ERROR_UNKNOWN;
    }
    present_sem = swapchain->present_semaphores[swapchain->current_image];
    if (present_sem == VK_NULL_HANDLE) {
        return LC_ERROR_UNKNOWN;
    }

    /* Realize a clear-only frame: open the pass with the pending
     * color/depth and close it immediately. Drawing frames already
     * have the pass open from bind/draw. */
    if (!swapchain->rp_open &&
        (swapchain->clear_pending || swapchain->depth_clear_pending)) {
        lc_vk_open_render_pass(swapchain);
    }
    if (swapchain->rp_open) {
        vkCmdEndRenderPass(flight->cmd);
        swapchain->rp_open = 0;
    }

    if (vkEndCommandBuffer(flight->cmd) != VK_SUCCESS) {
        /* Recording failed: drop the frame without submitting. The
         * buffer is reset clean on the next begin. */
        swapchain->frame_active = 0;
        return LC_ERROR_UNKNOWN;
    }

    memset(&submit_info, 0, sizeof(submit_info));
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit_info.waitSemaphoreCount = 1;
    submit_info.pWaitSemaphores = &flight->image_available;
    /* Rendering waits at color output plus early-fragment tests: the
     * acquire semaphore guarantees both the color image and the depth
     * buffer are free before the pass writes them. */
    submit_info.pWaitDstStageMask = &wait_stage;
    submit_info.commandBufferCount = 1;
    submit_info.pCommandBuffers = &flight->cmd;
    submit_info.signalSemaphoreCount = 1;
    submit_info.pSignalSemaphores = &present_sem;

    if (vkQueueSubmit(device->graphics_queue, 1, &submit_info,
                      flight->fence) != VK_SUCCESS) {
        /* Fatal for this swapchain (device-loss territory): the fence
         * will never signal, so the frame is dropped and the caller
         * must destroy/recreate rather than continue. */
        swapchain->frame_active = 0;
        memset(&swapchain->encoder, 0, sizeof(swapchain->encoder));
        swapchain->encoder.swapchain = swapchain;
        swapchain->encoder.device = swapchain->device;
        return LC_ERROR_UNKNOWN;
    }

    memset(&present_info, 0, sizeof(present_info));
    present_info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    present_info.waitSemaphoreCount = 1;
    present_info.pWaitSemaphores = &present_sem;
    present_info.swapchainCount = 1;
    present_info.pSwapchains = &swapchain->vk_swapchain;
    present_info.pImageIndices = &swapchain->current_image;

    /* Present on the present queue, which may differ from graphics. */
    present_res =
        vkQueuePresentKHR(surface->present_queue, &present_info);

    /* The submission owns a fence now; the slot rotates regardless of
     * how presentation went. Layout history needs no tracking: every
     * frame starts its pass from UNDEFINED. */
    swapchain->current_frame =
        (swapchain->current_frame + 1u) % LC_MAX_FRAMES_IN_FLIGHT;
    swapchain->current_image = UINT32_MAX;
    swapchain->frame_active = 0;
    swapchain->bound_pipeline = NULL;
    swapchain->bound_index_buffer = NULL;
    swapchain->index_bound = 0;
    memset(&swapchain->encoder, 0, sizeof(swapchain->encoder));
    swapchain->encoder.swapchain = swapchain;
    swapchain->encoder.device = swapchain->device;
    reported_suboptimal = swapchain->frame_suboptimal;
    swapchain->frame_suboptimal = 0;

    if (present_res == VK_SUCCESS) {
        return reported_suboptimal ? LC_SUBOPTIMAL : LC_SUCCESS;
    }
    if (present_res == VK_SUBOPTIMAL_KHR) {
        return LC_SUBOPTIMAL;
    }
    if (present_res == VK_ERROR_OUT_OF_DATE_KHR) {
        return LC_ERROR_SWAPCHAIN_OUT_OF_DATE;
    }
    if (present_res == VK_ERROR_SURFACE_LOST_KHR) {
        return LC_ERROR_SWAPCHAIN_UNSUPPORTED;
    }
    return LC_ERROR_UNKNOWN;
}

lc_result lc_vulkan_frame_bind_vertex(lc_swapchain *swapchain,
                                      uint32_t binding,
                                      const lc_buffer *buffer,
                                      uint64_t offset) {
    VkPhysicalDeviceProperties props;
    lc_vk_flight *flight;
    VkDeviceSize vk_offset;

    if (swapchain == NULL || buffer == NULL || !swapchain->frame_active) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    LC_ENC_GUARD(swapchain);
    if (swapchain->current_frame >= LC_MAX_FRAMES_IN_FLIGHT ||
        buffer->vk_buffer == VK_NULL_HANDLE) {
        return LC_ERROR_UNKNOWN;
    }
    /* Binding numbers are validated against the device so a stray
     * index fails here instead of inside vkCmdBindVertexBuffers. */
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(swapchain->device->physical_device, &props);
    if (binding >= props.limits.maxVertexInputBindings) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    flight = &swapchain->flights[swapchain->current_frame];
    vk_offset = (VkDeviceSize)offset;
    vkCmdBindVertexBuffers(flight->cmd, binding, 1, &buffer->vk_buffer,
                           &vk_offset);
    return LC_SUCCESS;
}
