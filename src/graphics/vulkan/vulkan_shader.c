/*
 * Vulkan shader backend (Phase 7: SPIR-V modules, no compilation).
 *
 * Wraps caller-supplied SPIR-V bytecode in a VkShaderModule. LumaC
 * never compiles shaders; compiling GLSL/HLSL is the application's
 * build-time job. Only the module handle is consumed by pipeline
 * creation, so shaders may be destroyed while pipelines live on.
 */

#include <string.h>

#include "graphics/graphics_internal.h"

/* First word of every SPIR-V module, little-endian on all supported
 * platforms. Read via memcpy: caller memory may be unaligned. */
#define LC_SPIRV_MAGIC 0x07230203u

lc_result lc_vulkan_shader_create(lc_shader *shader, lc_device *device,
                                  const lc_shader_desc *desc) {
    VkShaderModuleCreateInfo info;
    const char *entry;
    size_t entry_len = 0;
    size_t i = 0;
    uint32_t magic = 0;

    if (shader == NULL || device == NULL || desc == NULL) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->code == NULL || desc->code_size == 0 ||
        (desc->code_size % sizeof(uint32_t)) != 0) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    if (desc->stage != LC_SHADER_STAGE_VERTEX &&
        desc->stage != LC_SHADER_STAGE_FRAGMENT &&
        desc->stage != LC_SHADER_STAGE_COMPUTE) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    memcpy(&magic, desc->code, sizeof(magic));
    if (magic != LC_SPIRV_MAGIC) {
        return LC_ERROR_INVALID_ARGUMENT;
    }
    entry = (desc->entry_point != NULL) ? desc->entry_point : "main";
    while (entry[entry_len] != '\0') {
        entry_len++;
    }
    if (entry_len == 0 || entry_len >= LC_SHADER_ENTRY_MAX) {
        return LC_ERROR_INVALID_ARGUMENT;
    }

    shader->device = device;
    shader->stage = desc->stage;
    for (i = 0; i <= entry_len; i++) {
        shader->entry_point[i] = entry[i];
    }

    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = desc->code_size;
    /* pCode must be 4-byte aligned; the public API requires it. */
    info.pCode = (const uint32_t *)desc->code;

    if (vkCreateShaderModule(device->device, &info, NULL, &shader->module) !=
        VK_SUCCESS) {
        shader->module = VK_NULL_HANDLE;
        return LC_ERROR_SHADER_CREATION_FAILED;
    }
    return LC_SUCCESS;
}

void lc_vulkan_shader_destroy(lc_shader *shader) {
    if (shader == NULL || shader->module == VK_NULL_HANDLE) {
        return;
    }
    /* Shader modules are consumed during pipeline creation; submitted
     * command buffers reference VkPipeline, not VkShaderModule. */
    if (shader->device != NULL &&
        shader->device->device != VK_NULL_HANDLE) {
        vkDestroyShaderModule(shader->device->device, shader->module, NULL);
    }
    shader->module = VK_NULL_HANDLE;
}
