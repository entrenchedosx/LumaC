/*
 * Vulkan pipeline backend (Phase 7: triangle drawing, no vertex buffers).
 *
 * Builds an empty-layout graphics pipeline against the swapchain's
 * render pass: no vertex bindings or attributes (vertices come from the
 * shader's vertex index), triangle list, dynamic viewport/scissor, no
 * culling (winding can't hide the first triangle), one sample, blending
 * off, no depth. Only module handles are consumed, so shaders may be
 * destroyed once the pipeline exists.
 */

#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

static VkShaderStageFlagBits lc_vk_stage_flag(lc_shader_stage stage) {
    return (stage == LC_SHADER_STAGE_VERTEX) ? VK_SHADER_STAGE_VERTEX_BIT
                                             : VK_SHADER_STAGE_FRAGMENT_BIT;
}

/* Validate the vertex layout against device limits and internal
 * consistency: every attribute references a declared binding, no
 * location repeats, strides are nonzero and cover their attributes.
 * Zero bindings with zero attributes (vertex-index generation) always
 * passes. */
static lc_result lc_vk_validate_vertex_layout(
    VkPhysicalDevice physical,
    const lc_vertex_binding_desc *bindings, uint32_t binding_count,
    const lc_vertex_attribute_desc *attributes, uint32_t attribute_count) {
    VkPhysicalDeviceProperties props;
    uint32_t i;
    uint32_t j;

    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(physical, &props);
    if (binding_count > props.limits.maxVertexInputBindings ||
        attribute_count > props.limits.maxVertexInputAttributes) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    for (i = 0; i < binding_count; i++) {
        if (bindings[i].stride == 0 ||
            bindings[i].stride > props.limits.maxVertexInputBindingStride) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
    }
    for (i = 0; i < attribute_count; i++) {
        uint32_t size = lc_format_byte_size(attributes[i].format);
        int bound = 0;

        if (size == 0) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (attributes[i].offset > props.limits.maxVertexInputAttributeOffset) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        for (j = 0; j < binding_count; j++) {
            if (bindings[j].binding != attributes[i].binding) {
                continue;
            }
            bound = 1;
            if (attributes[i].offset + size > bindings[j].stride) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
        if (!bound) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        for (j = 0; j < i; j++) {
            if (attributes[j].location == attributes[i].location) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    return LC_SUCCESS;
}

lc_result lc_vulkan_pipeline_create(lc_pipeline *pipeline, lc_device *device,
                                    lc_swapchain *swapchain,
                                    const lc_graphics_pipeline_desc *desc) {
    const lc_shader *vertex_shader;
    const lc_shader *fragment_shader;
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    VkVertexInputBindingDescription *vk_bindings = NULL;
    VkVertexInputAttributeDescription *vk_attributes = NULL;
    VkPipelineInputAssemblyStateCreateInfo input_assembly;
    VkPipelineViewportStateCreateInfo viewport_state;
    VkPipelineRasterizationStateCreateInfo rasterization;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineColorBlendAttachmentState blend_attachment;
    VkPipelineColorBlendStateCreateInfo blend_state;
    VkPipelineLayoutCreateInfo layout_info;
    VkGraphicsPipelineCreateInfo pipeline_info;
    VkDynamicState dynamic_states[2];

    if (pipeline == NULL || device == NULL || swapchain == NULL ||
        desc == NULL || desc->vertex_shader == NULL ||
        desc->fragment_shader == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    vertex_shader = desc->vertex_shader;
    fragment_shader = desc->fragment_shader;
    if (vertex_shader->stage != LC_SHADER_STAGE_VERTEX ||
        fragment_shader->stage != LC_SHADER_STAGE_FRAGMENT) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((desc->vertex_binding_count > 0 && desc->vertex_bindings == NULL) ||
        (desc->vertex_attribute_count > 0 &&
         desc->vertex_attributes == NULL)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (vertex_shader->module == VK_NULL_HANDLE ||
        fragment_shader->module == VK_NULL_HANDLE ||
        device->device == VK_NULL_HANDLE ||
        device->physical_device == VK_NULL_HANDLE ||
        swapchain->render_pass == VK_NULL_HANDLE) {
        return LC_ERROR_PIPELINE_CREATION_FAILED;
    }
    {
        lc_result layout_res = lc_vk_validate_vertex_layout(
            device->physical_device, desc->vertex_bindings,
            desc->vertex_binding_count, desc->vertex_attributes,
            desc->vertex_attribute_count);
        if (layout_res != LC_SUCCESS) {
            return layout_res;
        }
    }

    pipeline->device = device;
    pipeline->swapchain = swapchain;
    pipeline->format = swapchain->format;

    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = lc_vk_stage_flag(vertex_shader->stage);
    stages[0].module = vertex_shader->module;
    stages[0].pName = vertex_shader->entry_point;
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = lc_vk_stage_flag(fragment_shader->stage);
    stages[1].module = fragment_shader->module;
    stages[1].pName = fragment_shader->entry_point;

    /* Vertex input from the backend-neutral layout (empty when the
     * shader generates vertices from its index). Vulkan copies these
     * arrays at creation, so they are freed before returning. */
    if (desc->vertex_binding_count > 0) {
        uint32_t i;

        vk_bindings = (VkVertexInputBindingDescription *)malloc(
            sizeof(VkVertexInputBindingDescription) *
            desc->vertex_binding_count);
        if (vk_bindings == NULL) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
        for (i = 0; i < desc->vertex_binding_count; i++) {
            vk_bindings[i].binding = desc->vertex_bindings[i].binding;
            vk_bindings[i].stride = desc->vertex_bindings[i].stride;
            vk_bindings[i].inputRate =
                (desc->vertex_bindings[i].input_rate ==
                 LC_VERTEX_INPUT_PER_INSTANCE)
                    ? VK_VERTEX_INPUT_RATE_INSTANCE
                    : VK_VERTEX_INPUT_RATE_VERTEX;
        }
    }
    if (desc->vertex_attribute_count > 0) {
        uint32_t i;

        vk_attributes = (VkVertexInputAttributeDescription *)malloc(
            sizeof(VkVertexInputAttributeDescription) *
            desc->vertex_attribute_count);
        if (vk_attributes == NULL) {
            free(vk_bindings);
            return LC_ERROR_OUT_OF_MEMORY;
        }
        for (i = 0; i < desc->vertex_attribute_count; i++) {
            vk_attributes[i].location = desc->vertex_attributes[i].location;
            vk_attributes[i].binding = desc->vertex_attributes[i].binding;
            vk_attributes[i].format =
                lc_vulkan_translate_format(desc->vertex_attributes[i].format);
            vk_attributes[i].offset = desc->vertex_attributes[i].offset;
        }
    }

    memset(&vertex_input, 0, sizeof(vertex_input));
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = desc->vertex_binding_count;
    vertex_input.pVertexBindingDescriptions = vk_bindings;
    vertex_input.vertexAttributeDescriptionCount =
        desc->vertex_attribute_count;
    vertex_input.pVertexAttributeDescriptions = vk_attributes;

    memset(&input_assembly, 0, sizeof(input_assembly));
    input_assembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    input_assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    input_assembly.primitiveRestartEnable = VK_FALSE;

    /* Viewport/scissor counts are fixed at one; the values are dynamic
     * and set every frame, so resizes never rebuild pipelines. */
    dynamic_states[0] = VK_DYNAMIC_STATE_VIEWPORT;
    dynamic_states[1] = VK_DYNAMIC_STATE_SCISSOR;
    memset(&viewport_state, 0, sizeof(viewport_state));
    viewport_state.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;

    memset(&rasterization, 0, sizeof(rasterization));
    rasterization.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterization.depthClampEnable = VK_FALSE;
    rasterization.rasterizerDiscardEnable = VK_FALSE;
    rasterization.polygonMode = VK_POLYGON_MODE_FILL;
    /* No culling for the first triangle: winding mistakes must not
     * hide it. A cull-mode API can come later. */
    rasterization.cullMode = VK_CULL_MODE_NONE;
    rasterization.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rasterization.lineWidth = 1.0f;

    memset(&multisample, 0, sizeof(multisample));
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    memset(&blend_attachment, 0, sizeof(blend_attachment));
    blend_attachment.blendEnable = VK_FALSE;
    blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                                      VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT |
                                      VK_COLOR_COMPONENT_A_BIT;

    memset(&blend_state, 0, sizeof(blend_state));
    blend_state.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = 1;
    blend_state.pAttachments = &blend_attachment;

    /* Empty layout: no descriptor sets, no push constants yet. */
    memset(&layout_info, 0, sizeof(layout_info));
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    if (vkCreatePipelineLayout(device->device, &layout_info, NULL,
                               &pipeline->layout) != VK_SUCCESS) {
        pipeline->layout = VK_NULL_HANDLE;
        free(vk_bindings);
        free(vk_attributes);
        return LC_ERROR_PIPELINE_CREATION_FAILED;
    }

    memset(&pipeline_info, 0, sizeof(pipeline_info));
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = 2;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pColorBlendState = &blend_state;
    {
        VkPipelineDynamicStateCreateInfo dynamic_info;

        memset(&dynamic_info, 0, sizeof(dynamic_info));
        dynamic_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_info.dynamicStateCount = 2;
        dynamic_info.pDynamicStates = dynamic_states;
        pipeline_info.pDynamicState = &dynamic_info;

        pipeline_info.layout = pipeline->layout;
        pipeline_info.renderPass = swapchain->render_pass;
        pipeline_info.subpass = 0;

        if (vkCreateGraphicsPipelines(device->device, VK_NULL_HANDLE, 1,
                                      &pipeline_info, NULL,
                                      &pipeline->pipeline) != VK_SUCCESS) {
            pipeline->pipeline = VK_NULL_HANDLE;
            vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
            pipeline->layout = VK_NULL_HANDLE;
            free(vk_bindings);
            free(vk_attributes);
            return LC_ERROR_PIPELINE_CREATION_FAILED;
        }
    }
    free(vk_bindings);
    free(vk_attributes);
    return LC_SUCCESS;
}

void lc_vulkan_pipeline_destroy(lc_pipeline *pipeline) {
    VkDevice device_handle = VK_NULL_HANDLE;

    if (pipeline == NULL) {
        return;
    }
    if (pipeline->device != NULL) {
        device_handle = pipeline->device->device;
    }
    /* Submitted command buffers may still reference the pipeline;
     * wait them out first (coarse but correct; destroys are rare). */
    if (device_handle != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device_handle);
    }
    /* Pipeline first, then its layout. The render pass is owned by the
     * swapchain and is only referenced here by compatibility. */
    if (pipeline->pipeline != VK_NULL_HANDLE) {
        if (device_handle != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_handle, pipeline->pipeline, NULL);
        }
        pipeline->pipeline = VK_NULL_HANDLE;
    }
    if (pipeline->layout != VK_NULL_HANDLE) {
        if (device_handle != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_handle, pipeline->layout, NULL);
        }
        pipeline->layout = VK_NULL_HANDLE;
    }
}
