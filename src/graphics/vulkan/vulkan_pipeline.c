/*
 * Vulkan pipeline backend (graphics pipelines with resource slots,
 * raster state, depth, and push constants).
 *
 * Builds a graphics pipeline against the swapchain's color+depth render
 * pass from caller-supplied shaders, optional vertex input, optional
 * binding layouts, raster/depth state, and push-constant ranges:
 * triangle list, dynamic viewport/scissor, one sample, blending off.
 * Only module and set-layout handles are consumed for Vulkan objects;
 * canonical binding signatures plus push ranges are copied into the
 * lc_pipeline for content-based bind/push validation, so layouts may
 * be destroyed once the pipeline exists without invalidating it.
 */

#include <stdlib.h>
#include <string.h>

#include "graphics/graphics_internal.h"

static VkShaderStageFlagBits lc_vk_stage_flag(lc_shader_stage stage) {
    if (stage == LC_SHADER_STAGE_VERTEX) {
        return VK_SHADER_STAGE_VERTEX_BIT;
    }
    if (stage == LC_SHADER_STAGE_COMPUTE) {
        return VK_SHADER_STAGE_COMPUTE_BIT;
    }
    return VK_SHADER_STAGE_FRAGMENT_BIT;
}

static VkCullModeFlags lc_vk_cull_mode(lc_cull_mode mode) {
    switch (mode) {
    case LC_CULL_FRONT:
        return VK_CULL_MODE_FRONT_BIT;
    case LC_CULL_BACK:
        return VK_CULL_MODE_BACK_BIT;
    case LC_CULL_NONE:
    default:
        return VK_CULL_MODE_NONE;
    }
}

static VkFrontFace lc_vk_front_face(lc_front_face face) {
    return (face == LC_FRONT_FACE_COUNTER_CLOCKWISE)
               ? VK_FRONT_FACE_COUNTER_CLOCKWISE
               : VK_FRONT_FACE_CLOCKWISE;
}

static VkShaderStageFlags lc_vk_push_stages(uint32_t visibility) {
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

/* Validate push-constant ranges: caller-provided visibility mask,
 * 4-byte alignment, nonzero size, inside the device limit, no
 * overlaps. Graphics passes VERTEX|FRAGMENT; compute also admits
 * COMPUTE. */
static lc_result lc_vk_validate_push_ranges(
    VkPhysicalDevice physical, const lc_push_constant_range *ranges,
    uint32_t range_count, uint32_t known) {
    VkPhysicalDeviceProperties props;
    uint32_t i;
    uint32_t j;

    if (range_count == 0) {
        return LC_SUCCESS;
    }
    if (ranges == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memset(&props, 0, sizeof(props));
    vkGetPhysicalDeviceProperties(physical, &props);
    /* Vulkan guarantees at least 128 bytes; clamp defensively. */
    if (props.limits.maxPushConstantsSize < 4) {
        return LC_ERROR_PIPELINE_CREATION_FAILED;
    }
    for (i = 0; i < range_count; i++) {
        uint32_t vis = ranges[i].visibility;
        uint32_t size = ranges[i].size;
        uint32_t offset = ranges[i].offset;

        if (vis == 0 || (vis & ~known) != 0) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (size == 0 || (offset % 4u) != 0u || (size % 4u) != 0u) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        if (offset + size < offset ||
            offset + size > props.limits.maxPushConstantsSize) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        for (j = 0; j < i; j++) {
            uint32_t a0 = ranges[j].offset;
            uint32_t a1 = ranges[j].offset + ranges[j].size;
            uint32_t b0 = offset;
            uint32_t b1 = offset + size;

            if (!(b1 <= a0 || b0 >= a1)) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    return LC_SUCCESS;
}

/* Pointer comparison only: dead anchors are rejected without ever
 * dereferencing freed memory. */
static int lc_pipeline_binding_layout_live(const lc_binding_layout *layout) {
    lc_state *state = lc_get_internal_state();
    const lc_binding_layout *it;

    if (state == NULL || layout == NULL) {
        return 0;
    }
    for (it = state->binding_layouts; it != NULL; it = it->next) {
        if (it == layout) {
            return 1;
        }
    }
    return 0;
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

/* Free canonical copies on failure paths (pipeline struct itself is
 * owned by the caller). Safe on partial state. */
static void lc_vk_pipeline_free_copies(lc_pipeline *pipeline) {
    uint32_t i;

    if (pipeline == NULL) {
        return;
    }
    if (pipeline->slot_signatures != NULL) {
        for (i = 0; i < pipeline->layout_count; i++) {
            free(pipeline->slot_signatures[i]);
        }
        free(pipeline->slot_signatures);
        pipeline->slot_signatures = NULL;
    }
    free(pipeline->slot_signature_counts);
    pipeline->slot_signature_counts = NULL;
    free(pipeline->layouts);
    pipeline->layouts = NULL;
    pipeline->layout_count = 0;
    free(pipeline->push_ranges);
    pipeline->push_ranges = NULL;
    pipeline->push_range_count = 0;
    free(pipeline->blend_recipes);
    pipeline->blend_recipes = NULL;
    pipeline->blend_recipe_count = 0;
}

lc_result lc_vulkan_pipeline_create(lc_pipeline *pipeline, lc_device *device,
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
    VkPipelineDepthStencilStateCreateInfo depth_stencil;
    VkPipelineColorBlendAttachmentState
        blend_attachments[LC_MAX_COLOR_ATTACHMENTS];
    VkPipelineColorBlendStateCreateInfo blend_state;
    VkPipelineLayoutCreateInfo layout_info;
    VkGraphicsPipelineCreateInfo pipeline_info;
    VkDynamicState dynamic_states[2];
    VkPushConstantRange *vk_push_ranges = NULL;
    lc_render_target_desc target_sig;

    if (pipeline == NULL || device == NULL || desc == NULL ||
        desc->vertex_shader == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Phase 13: the structural target signature is mandatory (public
     * validation already checked shape; device limits re-checked
     * here). No swapchain is involved. */
    {
        lc_result target_res =
            lc_vulkan_target_desc_validate(device, &desc->render_target);

        if (target_res != LC_SUCCESS) {
            return target_res;
        }
        target_sig = desc->render_target;
    }
    vertex_shader = desc->vertex_shader;
    fragment_shader = desc->fragment_shader;
    if (vertex_shader->stage != LC_SHADER_STAGE_VERTEX ||
        (fragment_shader != NULL &&
         fragment_shader->stage != LC_SHADER_STAGE_FRAGMENT)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((desc->vertex_binding_count > 0 && desc->vertex_bindings == NULL) ||
        (desc->vertex_attribute_count > 0 &&
         desc->vertex_attributes == NULL)) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->cull_mode != LC_CULL_NONE && desc->cull_mode != LC_CULL_FRONT &&
        desc->cull_mode != LC_CULL_BACK) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->front_face != LC_FRONT_FACE_CLOCKWISE &&
        desc->front_face != LC_FRONT_FACE_COUNTER_CLOCKWISE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if ((desc->depth_test_enable != 0 || desc->depth_write_enable != 0) &&
        target_sig.depth_stencil_format == LC_FORMAT_UNDEFINED) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (vertex_shader->module == VK_NULL_HANDLE ||
        (fragment_shader != NULL &&
         fragment_shader->module == VK_NULL_HANDLE) ||
        device->device == VK_NULL_HANDLE ||
        device->physical_device == VK_NULL_HANDLE) {
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
    /* Binding layouts: live, same device, within set limits. Canonical
     * signatures are copied for content-based bind matching, so layouts
     * may die while the pipeline lives. */
    {
        VkPhysicalDeviceProperties props;
        uint32_t i;

        if (desc->binding_layout_count > 0 &&
            desc->binding_layouts == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        memset(&props, 0, sizeof(props));
        vkGetPhysicalDeviceProperties(device->physical_device, &props);
        if (desc->binding_layout_count >
            props.limits.maxBoundDescriptorSets) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        for (i = 0; i < desc->binding_layout_count; i++) {
            if (desc->binding_layouts[i] == NULL ||
                !lc_pipeline_binding_layout_live(desc->binding_layouts[i]) ||
                desc->binding_layouts[i]->device != device ||
                desc->binding_layouts[i]->vk_layout == VK_NULL_HANDLE) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    /* Push-constant ranges: graphics visibility, alignment, limit,
     * no overlaps. */
    {
        const uint32_t known =
            (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
            (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT;
        lc_result push_res = lc_vk_validate_push_ranges(
            device->physical_device, desc->push_constant_ranges,
            desc->push_constant_range_count, known);
        if (push_res != LC_SUCCESS) {
            return push_res;
        }
    }

    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->device = device;
    pipeline->target_color_count = target_sig.color_attachment_count;
    {
        uint32_t i;

        for (i = 0; i < target_sig.color_attachment_count; i++) {
            pipeline->target_color_formats[i] = target_sig.color_formats[i];
        }
    }
    pipeline->target_depth_format = target_sig.depth_stencil_format;
    pipeline->target_samples = target_sig.samples;
    pipeline->target_hash = lc_render_target_hash(
        target_sig.color_attachment_count, target_sig.color_formats,
        target_sig.depth_stencil_format, target_sig.samples);
    pipeline->cull_mode = desc->cull_mode;
    pipeline->front_face = desc->front_face;
    pipeline->depth_test_enable = (desc->depth_test_enable != 0) ? 1 : 0;
    pipeline->depth_write_enable = (desc->depth_write_enable != 0) ? 1 : 0;
    pipeline->layouts = NULL;
    pipeline->layout_count = 0;
    pipeline->slot_signatures = NULL;
    pipeline->slot_signature_counts = NULL;
    pipeline->push_ranges = NULL;
    pipeline->push_range_count = 0;
    pipeline->blend_recipes = NULL;
    pipeline->blend_recipe_count = 0;
    /* Phase 33 blend recipes: public validation already checked
     * counts + enums; copy canonically here so the backend mapping
     * below cannot fail on content. */
    if (desc->blend != NULL && desc->blend_attachment_count > 0) {
        pipeline->blend_recipes = (lc_blend_attachment *)malloc(
            sizeof(lc_blend_attachment) * desc->blend_attachment_count);
        if (pipeline->blend_recipes == NULL) {
            return LC_ERROR_OUT_OF_MEMORY;
        }
        memcpy(pipeline->blend_recipes, desc->blend,
               sizeof(lc_blend_attachment) * desc->blend_attachment_count);
        pipeline->blend_recipe_count = desc->blend_attachment_count;
    }
    if (desc->binding_layout_count > 0) {
        uint32_t i;

        pipeline->layouts = (const lc_binding_layout **)malloc(
            sizeof(const lc_binding_layout *) * desc->binding_layout_count);
        pipeline->slot_signatures = (lc_binding_desc **)calloc(
            desc->binding_layout_count, sizeof(lc_binding_desc *));
        pipeline->slot_signature_counts = (uint32_t *)calloc(
            desc->binding_layout_count, sizeof(uint32_t));
        if (pipeline->layouts == NULL || pipeline->slot_signatures == NULL ||
            pipeline->slot_signature_counts == NULL) {
            free(pipeline->layouts);
            free(pipeline->slot_signatures);
            free(pipeline->slot_signature_counts);
            pipeline->layouts = NULL;
            pipeline->slot_signatures = NULL;
            pipeline->slot_signature_counts = NULL;
            return LC_ERROR_OUT_OF_MEMORY;
        }
        for (i = 0; i < desc->binding_layout_count; i++) {
            const lc_binding_layout *layout = desc->binding_layouts[i];

            pipeline->layouts[i] = layout;
            pipeline->slot_signature_counts[i] = layout->binding_count;
            if (layout->binding_count > 0) {
                pipeline->slot_signatures[i] = (lc_binding_desc *)malloc(
                    sizeof(lc_binding_desc) * layout->binding_count);
                if (pipeline->slot_signatures[i] == NULL) {
                    uint32_t k;
                    for (k = 0; k < i; k++) {
                        free(pipeline->slot_signatures[k]);
                    }
                    free(pipeline->layouts);
                    free(pipeline->slot_signatures);
                    free(pipeline->slot_signature_counts);
                    pipeline->layouts = NULL;
                    pipeline->slot_signatures = NULL;
                    pipeline->slot_signature_counts = NULL;
                    pipeline->layout_count = 0;
                    return LC_ERROR_OUT_OF_MEMORY;
                }
                memcpy(pipeline->slot_signatures[i], layout->bindings,
                       sizeof(lc_binding_desc) * layout->binding_count);
            }
        }
        pipeline->layout_count = desc->binding_layout_count;
    }
    if (desc->push_constant_range_count > 0) {
        pipeline->push_ranges = (lc_push_constant_range *)malloc(
            sizeof(lc_push_constant_range) *
            desc->push_constant_range_count);
        if (pipeline->push_ranges == NULL) {
            uint32_t k;
            if (pipeline->slot_signatures != NULL) {
                for (k = 0; k < pipeline->layout_count; k++) {
                    free(pipeline->slot_signatures[k]);
                }
            }
            free(pipeline->layouts);
            free(pipeline->slot_signatures);
            free(pipeline->slot_signature_counts);
            pipeline->layouts = NULL;
            pipeline->slot_signatures = NULL;
            pipeline->slot_signature_counts = NULL;
            pipeline->layout_count = 0;
            return LC_ERROR_OUT_OF_MEMORY;
        }
        memcpy(pipeline->push_ranges, desc->push_constant_ranges,
               sizeof(lc_push_constant_range) *
                   desc->push_constant_range_count);
        pipeline->push_range_count = desc->push_constant_range_count;
    }

    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = lc_vk_stage_flag(vertex_shader->stage);
    stages[0].module = vertex_shader->module;
    stages[0].pName = vertex_shader->entry_point;
    if (fragment_shader != NULL) {
        stages[1].sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = lc_vk_stage_flag(fragment_shader->stage);
        stages[1].module = fragment_shader->module;
        stages[1].pName = fragment_shader->entry_point;
    }

    /* Vertex input from the backend-neutral layout (empty when the
     * shader generates vertices from its index). Vulkan copies these
     * arrays at creation, so they are freed before returning. */
    if (desc->vertex_binding_count > 0) {
        uint32_t i;

        vk_bindings = (VkVertexInputBindingDescription *)malloc(
            sizeof(VkVertexInputBindingDescription) *
            desc->vertex_binding_count);
        if (vk_bindings == NULL) {
            lc_vk_pipeline_free_copies(pipeline);
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
            lc_vk_pipeline_free_copies(pipeline);
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
    rasterization.cullMode = lc_vk_cull_mode(desc->cull_mode);
    rasterization.frontFace = lc_vk_front_face(desc->front_face);
    rasterization.lineWidth = 1.0f;

    memset(&multisample, 0, sizeof(multisample));
    multisample.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples =
        lc_vulkan_translate_samples(target_sig.samples);

    /* Depth-stencil state is always present; test/write are gated by
     * the desc. Depthless targets must leave depth disabled (checked
     * above). When the target carries depth but the pipeline disables
     * it, the attachment still exists (cleared/stored per pass ops)
     * and is simply unused by this pipeline. */
    memset(&depth_stencil, 0, sizeof(depth_stencil));
    depth_stencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth_stencil.depthTestEnable =
        (desc->depth_test_enable != 0) ? VK_TRUE : VK_FALSE;
    depth_stencil.depthWriteEnable =
        (desc->depth_write_enable != 0) ? VK_TRUE : VK_FALSE;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depth_stencil.depthBoundsTestEnable = VK_FALSE;
    depth_stencil.stencilTestEnable = VK_FALSE;

    /* One blending-off write mask per color attachment (MRT-ready).
     * Depthless pipelines still need blend state when colors exist;
     * zero colors (depth-only) leave the count at 0. Phase 33: per-
     * attachment recipes from the canonical copies (legacy opaque
     * when no recipe was provided). */
    {
        uint32_t i;

        memset(blend_attachments, 0, sizeof(blend_attachments));
        for (i = 0; i < target_sig.color_attachment_count; i++) {
            const lc_blend_attachment *recipe = NULL;
            VkPipelineColorBlendAttachmentState *dst =
                &blend_attachments[i];

            if (i < pipeline->blend_recipe_count &&
                pipeline->blend_recipes != NULL) {
                recipe = &pipeline->blend_recipes[i];
            }
            if (recipe != NULL && recipe->blend_enable != 0) {
                static const VkBlendFactor kBlendMap[] = {
                    VK_BLEND_FACTOR_ZERO,
                    VK_BLEND_FACTOR_ONE,
                    VK_BLEND_FACTOR_SRC_ALPHA,
                    VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                };
                dst->blendEnable = VK_TRUE;
                dst->srcColorBlendFactor =
                    kBlendMap[recipe->src_color_factor];
                dst->dstColorBlendFactor =
                    kBlendMap[recipe->dst_color_factor];
                dst->colorBlendOp = VK_BLEND_OP_ADD;
                dst->srcAlphaBlendFactor =
                    kBlendMap[recipe->src_alpha_factor];
                dst->dstAlphaBlendFactor =
                    kBlendMap[recipe->dst_alpha_factor];
                dst->alphaBlendOp = VK_BLEND_OP_ADD;
            } else {
                dst->blendEnable = VK_FALSE;
            }
            dst->colorWriteMask =
                VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        }
    }

    memset(&blend_state, 0, sizeof(blend_state));
    blend_state.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend_state.attachmentCount = target_sig.color_attachment_count;
    blend_state.pAttachments =
        (target_sig.color_attachment_count > 0) ? blend_attachments : NULL;

    /* Pipeline layout from the binding-layout slots plus push-constant
     * ranges (both empty when unused). Vulkan copies everything at
     * creation. */
    {
        VkDescriptorSetLayout *vk_layouts = NULL;
        uint32_t i;

        if (pipeline->push_range_count > 0) {
            vk_push_ranges = (VkPushConstantRange *)malloc(
                sizeof(VkPushConstantRange) * pipeline->push_range_count);
            if (vk_push_ranges == NULL) {
                free(vk_bindings);
                free(vk_attributes);
                lc_vk_pipeline_free_copies(pipeline);
                return LC_ERROR_OUT_OF_MEMORY;
            }
            for (i = 0; i < pipeline->push_range_count; i++) {
                vk_push_ranges[i].stageFlags =
                    lc_vk_push_stages(pipeline->push_ranges[i].visibility);
                vk_push_ranges[i].offset = pipeline->push_ranges[i].offset;
                vk_push_ranges[i].size = pipeline->push_ranges[i].size;
            }
        }
        if (pipeline->layout_count > 0) {
            vk_layouts = (VkDescriptorSetLayout *)malloc(
                sizeof(VkDescriptorSetLayout) * pipeline->layout_count);
            if (vk_layouts == NULL) {
                free(vk_push_ranges);
                free(vk_bindings);
                free(vk_attributes);
                lc_vk_pipeline_free_copies(pipeline);
                return LC_ERROR_OUT_OF_MEMORY;
            }
            for (i = 0; i < pipeline->layout_count; i++) {
                vk_layouts[i] = pipeline->layouts[i]->vk_layout;
            }
        }
        memset(&layout_info, 0, sizeof(layout_info));
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = pipeline->layout_count;
        layout_info.pSetLayouts = vk_layouts;
        layout_info.pushConstantRangeCount = pipeline->push_range_count;
        layout_info.pPushConstantRanges = vk_push_ranges;
        if (vkCreatePipelineLayout(device->device, &layout_info, NULL,
                                   &pipeline->layout) != VK_SUCCESS) {
            pipeline->layout = VK_NULL_HANDLE;
            free(vk_layouts);
            free(vk_push_ranges);
            free(vk_bindings);
            free(vk_attributes);
            lc_vk_pipeline_free_copies(pipeline);
            return LC_ERROR_PIPELINE_CREATION_FAILED;
        }
        free(vk_layouts);
        free(vk_push_ranges);
        vk_push_ranges = NULL;
    }

    memset(&pipeline_info, 0, sizeof(pipeline_info));
    pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipeline_info.stageCount = (fragment_shader != NULL) ? 2u : 1u;
    pipeline_info.pStages = stages;
    pipeline_info.pVertexInputState = &vertex_input;
    pipeline_info.pInputAssemblyState = &input_assembly;
    pipeline_info.pViewportState = &viewport_state;
    pipeline_info.pRasterizationState = &rasterization;
    pipeline_info.pMultisampleState = &multisample;
    pipeline_info.pDepthStencilState = &depth_stencil;
    pipeline_info.pColorBlendState = &blend_state;
    {
        VkPipelineDynamicStateCreateInfo dynamic_info;
        lc_vk_pass_key compat_key;
        VkRenderPass compat_pass = VK_NULL_HANDLE;
        lc_result pass_res;
        uint32_t i;

        /* Creation pass from the device cache (canonical CLEAR/STORE
         * policy, offscreen finals): pipeline compatibility in Vulkan
         * ignores load/store, so any policy with these formats works.
         * The pass is cached (never owned here). */
        memset(&compat_key, 0, sizeof(compat_key));
        compat_key.color_count = target_sig.color_attachment_count;
        for (i = 0; i < target_sig.color_attachment_count; i++) {
            compat_key.color_formats[i] =
                lc_vulkan_translate_format(target_sig.color_formats[i]);
            compat_key.color_loads[i] = VK_ATTACHMENT_LOAD_OP_CLEAR;
            compat_key.color_stores[i] = VK_ATTACHMENT_STORE_OP_STORE;
        }
        compat_key.depth_format =
            (target_sig.depth_stencil_format == LC_FORMAT_UNDEFINED)
                ? VK_FORMAT_UNDEFINED
                : lc_vulkan_translate_format(
                      target_sig.depth_stencil_format);
        compat_key.depth_load = VK_ATTACHMENT_LOAD_OP_CLEAR;
        compat_key.depth_store = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        compat_key.samples =
            lc_vulkan_translate_samples(target_sig.samples);
        compat_key.present = 0;

        memset(&dynamic_info, 0, sizeof(dynamic_info));
        dynamic_info.sType =
            VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic_info.dynamicStateCount = 2;
        dynamic_info.pDynamicStates = dynamic_states;
        pipeline_info.pDynamicState = &dynamic_info;

        pipeline_info.layout = pipeline->layout;
        pass_res =
            lc_vulkan_pass_cache_get(device, &compat_key, &compat_pass);
        if (pass_res != LC_SUCCESS || compat_pass == VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
            pipeline->layout = VK_NULL_HANDLE;
            free(vk_bindings);
            free(vk_attributes);
            lc_vk_pipeline_free_copies(pipeline);
            return (pass_res != LC_SUCCESS)
                       ? pass_res
                       : LC_ERROR_PIPELINE_CREATION_FAILED;
        }
        pipeline_info.renderPass = compat_pass;
        pipeline_info.subpass = 0;

        /* Shared pipeline cache across threads (PART 22): the
         * VkPipelineCache object requires external synchronization.
         * The cache shard also covers the pass-cache lookup above
         * conceptually... pass lookup has its own shard inside. */
        lc_device_lock_cache(device);
        if (vkCreateGraphicsPipelines(device->device,
                                       device->pipeline_cache, 1,
                                       &pipeline_info, NULL,
                                       &pipeline->pipeline) != VK_SUCCESS) {
            lc_device_unlock_cache(device);
            pipeline->pipeline = VK_NULL_HANDLE;
            vkDestroyPipelineLayout(device->device, pipeline->layout,
                                    NULL);
            pipeline->layout = VK_NULL_HANDLE;
            free(vk_bindings);
            free(vk_attributes);
            lc_vk_pipeline_free_copies(pipeline);
            return LC_ERROR_PIPELINE_CREATION_FAILED;
        }
        lc_device_unlock_cache(device);
    }
    free(vk_bindings);
    free(vk_attributes);
    return LC_SUCCESS;
}

void lc_vulkan_pipeline_destroy(lc_pipeline *pipeline) {
    VkDevice device_handle = VK_NULL_HANDLE;
    uint32_t i;

    if (pipeline == NULL) {
        return;
    }
    if (pipeline->device != NULL) {
        device_handle = pipeline->device->device;
    }
    /* Submitted command buffers may still reference the pipeline;
     * lifetime is ordered by retirement (the destroy wrapper defers
     * the VkPipeline/VkPipelineLayout until GPU completion), so no
     * global idle is needed here. */
    /* Pipeline first, then its layout, then canonical copies. The
     * creation render pass lives in the device cache (never owned
     * here); compatibility was structural. */
    if (pipeline->slot_signatures != NULL) {
        for (i = 0; i < pipeline->layout_count; i++) {
            free(pipeline->slot_signatures[i]);
        }
        free(pipeline->slot_signatures);
        pipeline->slot_signatures = NULL;
    }
    free(pipeline->slot_signature_counts);
    pipeline->slot_signature_counts = NULL;
    if (pipeline->layouts != NULL) {
        free(pipeline->layouts);
        pipeline->layouts = NULL;
    }
    pipeline->layout_count = 0;
    free(pipeline->push_ranges);
    pipeline->push_ranges = NULL;
    pipeline->push_range_count = 0;
    free(pipeline->blend_recipes);
    pipeline->blend_recipes = NULL;
    pipeline->blend_recipe_count = 0;
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

/* ------------------------------------------------------------------ */
/* Compute pipelines (Phase 21).                                       */
/*                                                                     */
/* Same cache, same canonical layout/signature discipline as graphics */
/* pipelines, minus render-pass/raster state. Validation mirrors the  */
/* graphics path: live compute shader, live layouts, push ranges.     */
/* ------------------------------------------------------------------ */

static int lc_compute_shader_live(const lc_shader *shader) {
    lc_state *state = lc_get_internal_state();
    const lc_shader *it;

    if (state == NULL || shader == NULL) {
        return 0;
    }
    for (it = state->shaders; it != NULL; it = it->next) {
        if (it == shader) {
            return 1;
        }
    }
    return 0;
}

static void lc_vk_compute_free_copies(lc_compute_pipeline *pipeline) {
    uint32_t i;

    if (pipeline->slot_signatures != NULL) {
        for (i = 0; i < pipeline->layout_count; i++) {
            free(pipeline->slot_signatures[i]);
        }
        free(pipeline->slot_signatures);
        pipeline->slot_signatures = NULL;
    }
    free(pipeline->slot_signature_counts);
    pipeline->slot_signature_counts = NULL;
    free(pipeline->layouts);
    pipeline->layouts = NULL;
    pipeline->layout_count = 0;
    free(pipeline->push_ranges);
    pipeline->push_ranges = NULL;
    pipeline->push_range_count = 0;
}

lc_result lc_vulkan_compute_pipeline_create(
    lc_compute_pipeline *pipeline, lc_device *device,
    const lc_compute_pipeline_desc *desc) {
    VkComputePipelineCreateInfo pipeline_info;
    VkPipelineShaderStageCreateInfo stage_info;
    VkPipelineLayoutCreateInfo layout_info;
    VkPushConstantRange *vk_push_ranges = NULL;
    VkDescriptorSetLayout *vk_layouts = NULL;
    const lc_shader *shader = NULL;
    const uint32_t known =
        (uint32_t)LC_SHADER_VISIBILITY_VERTEX |
        (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT |
        (uint32_t)LC_SHADER_VISIBILITY_COMPUTE;
    uint32_t i;

    if (pipeline == NULL || device == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (device->device == VK_NULL_HANDLE) {
        return LC_ERROR_BACKEND_UNAVAILABLE;
    }
    if (!device->compute_supported) {
        return LC_ERROR_UNSUPPORTED;
    }
    shader = desc->compute_shader;
    if (shader == NULL || !lc_compute_shader_live(shader) ||
        shader->device != device ||
        shader->stage != LC_SHADER_STAGE_COMPUTE ||
        shader->module == VK_NULL_HANDLE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    /* Binding layouts: live, same device, within set limits. */
    {
        VkPhysicalDeviceProperties props;

        if (desc->binding_layout_count > 0 &&
            desc->binding_layouts == NULL) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        memset(&props, 0, sizeof(props));
        vkGetPhysicalDeviceProperties(device->physical_device, &props);
        if (desc->binding_layout_count >
            props.limits.maxBoundDescriptorSets) {
            return LC_ERROR_INVALID_ARGUMENT;
        }
        for (i = 0; i < desc->binding_layout_count; i++) {
            if (desc->binding_layouts[i] == NULL ||
                !lc_pipeline_binding_layout_live(desc->binding_layouts[i]) ||
                desc->binding_layouts[i]->device != device ||
                desc->binding_layouts[i]->vk_layout == VK_NULL_HANDLE) {
                return LC_ERROR_INVALID_ARGUMENT;
            }
        }
    }
    /* Push-constant ranges: compute visibility admitted. */
    {
        lc_result push_res = lc_vk_validate_push_ranges(
            device->physical_device, desc->push_constant_ranges,
            desc->push_constant_range_count, known);
        if (push_res != LC_SUCCESS) {
            return push_res;
        }
    }

    memset(pipeline, 0, sizeof(*pipeline));
    pipeline->device = device;
    pipeline->compute_shader = (lc_shader *)shader;
    if (desc->binding_layout_count > 0) {
        pipeline->layouts = (const lc_binding_layout **)malloc(
            sizeof(const lc_binding_layout *) * desc->binding_layout_count);
        pipeline->slot_signatures = (lc_binding_desc **)calloc(
            desc->binding_layout_count, sizeof(lc_binding_desc *));
        pipeline->slot_signature_counts = (uint32_t *)calloc(
            desc->binding_layout_count, sizeof(uint32_t));
        if (pipeline->layouts == NULL || pipeline->slot_signatures == NULL ||
            pipeline->slot_signature_counts == NULL) {
            lc_vk_compute_free_copies(pipeline);
            return LC_ERROR_OUT_OF_MEMORY;
        }
        for (i = 0; i < desc->binding_layout_count; i++) {
            const lc_binding_layout *layout = desc->binding_layouts[i];

            pipeline->layouts[i] = layout;
            pipeline->slot_signature_counts[i] = layout->binding_count;
            if (layout->binding_count > 0) {
                pipeline->slot_signatures[i] = (lc_binding_desc *)malloc(
                    sizeof(lc_binding_desc) * layout->binding_count);
                if (pipeline->slot_signatures[i] == NULL) {
                    lc_vk_compute_free_copies(pipeline);
                    return LC_ERROR_OUT_OF_MEMORY;
                }
                memcpy(pipeline->slot_signatures[i], layout->bindings,
                       sizeof(lc_binding_desc) * layout->binding_count);
            }
        }
        pipeline->layout_count = desc->binding_layout_count;
    }
    if (desc->push_constant_range_count > 0) {
        pipeline->push_ranges = (lc_push_constant_range *)malloc(
            sizeof(lc_push_constant_range) *
            desc->push_constant_range_count);
        if (pipeline->push_ranges == NULL) {
            lc_vk_compute_free_copies(pipeline);
            return LC_ERROR_OUT_OF_MEMORY;
        }
        memcpy(pipeline->push_ranges, desc->push_constant_ranges,
               sizeof(lc_push_constant_range) *
                   desc->push_constant_range_count);
        pipeline->push_range_count = desc->push_constant_range_count;
    }

    /* Pipeline layout from canonical copies (layouts may die). */
    if (pipeline->push_range_count > 0) {
        vk_push_ranges = (VkPushConstantRange *)malloc(
            sizeof(VkPushConstantRange) * pipeline->push_range_count);
        if (vk_push_ranges == NULL) {
            lc_vk_compute_free_copies(pipeline);
            return LC_ERROR_OUT_OF_MEMORY;
        }
        for (i = 0; i < pipeline->push_range_count; i++) {
            vk_push_ranges[i].stageFlags =
                lc_vk_push_stages(pipeline->push_ranges[i].visibility);
            vk_push_ranges[i].offset = pipeline->push_ranges[i].offset;
            vk_push_ranges[i].size = pipeline->push_ranges[i].size;
        }
    }
    if (pipeline->layout_count > 0) {
        vk_layouts = (VkDescriptorSetLayout *)malloc(
            sizeof(VkDescriptorSetLayout) * pipeline->layout_count);
        if (vk_layouts == NULL) {
            free(vk_push_ranges);
            lc_vk_compute_free_copies(pipeline);
            return LC_ERROR_OUT_OF_MEMORY;
        }
        for (i = 0; i < pipeline->layout_count; i++) {
            vk_layouts[i] = pipeline->layouts[i]->vk_layout;
        }
    }
    memset(&layout_info, 0, sizeof(layout_info));
    layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout_info.setLayoutCount = pipeline->layout_count;
    layout_info.pSetLayouts = vk_layouts;
    layout_info.pushConstantRangeCount = pipeline->push_range_count;
    layout_info.pPushConstantRanges = vk_push_ranges;
    if (vkCreatePipelineLayout(device->device, &layout_info, NULL,
                               &pipeline->layout) != VK_SUCCESS) {
        pipeline->layout = VK_NULL_HANDLE;
        free(vk_layouts);
        free(vk_push_ranges);
        lc_vk_compute_free_copies(pipeline);
        return LC_ERROR_PIPELINE_CREATION_FAILED;
    }
    free(vk_layouts);
    free(vk_push_ranges);

    /* Single compute stage; the SHARED device pipeline cache
     * (VkPipelineCache is type-agnostic) under the cache shard. */
    memset(&stage_info, 0, sizeof(stage_info));
    stage_info.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage_info.module = shader->module;
    stage_info.pName = shader->entry_point;
    memset(&pipeline_info, 0, sizeof(pipeline_info));
    pipeline_info.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pipeline_info.stage = stage_info;
    pipeline_info.layout = pipeline->layout;
    lc_device_lock_cache(device);
    if (vkCreateComputePipelines(device->device, device->pipeline_cache,
                                 1, &pipeline_info, NULL,
                                 &pipeline->pipeline) != VK_SUCCESS) {
        lc_device_unlock_cache(device);
        pipeline->pipeline = VK_NULL_HANDLE;
        vkDestroyPipelineLayout(device->device, pipeline->layout, NULL);
        pipeline->layout = VK_NULL_HANDLE;
        lc_vk_compute_free_copies(pipeline);
        return LC_ERROR_PIPELINE_CREATION_FAILED;
    }
    lc_device_unlock_cache(device);
    return LC_SUCCESS;
}

void lc_vulkan_compute_pipeline_destroy(lc_compute_pipeline *pipeline) {
    VkDevice device_handle = VK_NULL_HANDLE;

    if (pipeline == NULL) {
        return;
    }
    if (pipeline->device != NULL) {
        device_handle = pipeline->device->device;
    }
    /* Lifetime is ordered by retirement (destroy wrapper defers),
     * so no global idle here. Canonical copies free on every path. */
    lc_vk_compute_free_copies(pipeline);
    if (pipeline->pipeline != VK_NULL_HANDLE) {
        if (device_handle != VK_NULL_HANDLE) {
            vkDestroyPipeline(device_handle, pipeline->pipeline, NULL);
        }
        pipeline->pipeline = VK_NULL_HANDLE;
    }
    if (pipeline->layout != VK_NULL_HANDLE) {
        if (device_handle != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device_handle, pipeline->layout,
                                    NULL);
        }
        pipeline->layout = VK_NULL_HANDLE;
    }
    pipeline->compute_shader = NULL;
}
