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

#include <string.h>

#include "graphics/graphics_internal.h"

static VkShaderStageFlagBits lc_vk_stage_flag(lc_shader_stage stage) {
    return (stage == LC_SHADER_STAGE_VERTEX) ? VK_SHADER_STAGE_VERTEX_BIT
                                             : VK_SHADER_STAGE_FRAGMENT_BIT;
}

lc_result lc_vulkan_pipeline_create(lc_pipeline *pipeline, lc_device *device,
                                    lc_swapchain *swapchain,
                                    const lc_shader *vertex_shader,
                                    const lc_shader *fragment_shader) {
    VkPipelineShaderStageCreateInfo stages[2];
    VkPipelineVertexInputStateCreateInfo vertex_input;
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
        vertex_shader == NULL || fragment_shader == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (vertex_shader->stage != LC_SHADER_STAGE_VERTEX ||
        fragment_shader->stage != LC_SHADER_STAGE_FRAGMENT) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (vertex_shader->module == VK_NULL_HANDLE ||
        fragment_shader->module == VK_NULL_HANDLE ||
        device->device == VK_NULL_HANDLE ||
        swapchain->render_pass == VK_NULL_HANDLE) {
        return LC_ERROR_PIPELINE_CREATION_FAILED;
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

    /* No vertex buffers yet: empty input state. */
    memset(&vertex_input, 0, sizeof(vertex_input));
    vertex_input.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

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
            return LC_ERROR_PIPELINE_CREATION_FAILED;
        }
    }
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
