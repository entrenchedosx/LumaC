/*
 * Renderer materials (Phases 13-15): unlit foundation plus PBR
 * metallic-roughness. One descriptor set per material over its
 * kind's layout (unlit: camera + base + sampler; PBR: camera +
 * lights + material UBO + five maps + sampler); no Vulkan
 * descriptor calls here — only public LumaC binding APIs.
 */

#include <stdlib.h>
#include <string.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

/* GPU block sizes are ABI: shader mirrors must match exactly. */
typedef char lr_check_material_gpu_size
    [(sizeof(lr_material_gpu) == 64) ? 1 : -1];
typedef char lr_check_lights_gpu_size
    [(sizeof(lr_lights_gpu) == 16 + 16 + 64 * 80) ? 1 : -1];
typedef char lr_check_shadows_gpu_size
    [(sizeof(lr_shadows_gpu) == 16 + 4 * 80) ? 1 : -1];
typedef char lr_check_depth_vp_size
    [(sizeof(lr_depth_vp_gpu) == 64) ? 1 : -1];
typedef char lr_check_pbr_push_size
    [(sizeof(lr_pbr_push) == 124) ? 1 : -1];
typedef char lr_check_shadow_push_size
    [(sizeof(lr_shadow_push) == 72) ? 1 : -1];

void lr_material_list_add(lr_renderer *renderer, lr_material *material) {
    if (renderer == NULL || material == NULL) {
        return;
    }
    material->next = renderer->materials;
    material->prev = NULL;
    if (renderer->materials != NULL) {
        renderer->materials->prev = material;
    }
    renderer->materials = material;
}

void lr_material_list_remove(lr_material *material) {
    lr_renderer *renderer;

    if (material == NULL || material->renderer == NULL) {
        return;
    }
    renderer = material->renderer;
    if (material->prev != NULL) {
        material->prev->next = material->next;
    } else if (renderer->materials == material) {
        renderer->materials = material->next;
    }
    if (material->next != NULL) {
        material->next->prev = material->prev;
    }
    material->next = NULL;
    material->prev = NULL;
}

int lr_material_is_live(const lr_renderer *renderer,
                        const lr_material *material) {
    const lr_material *it;

    if (renderer == NULL || material == NULL) {
        return 0;
    }
    for (it = renderer->materials; it != NULL; it = it->next) {
        if (it == material) {
            return 1;
        }
    }
    return 0;
}

lr_result lr_material_create_unlit(lr_renderer *renderer,
                                   const lr_unlit_material_desc *desc,
                                   lr_material **out_material) {
    lr_material *material;
    lc_binding_write writes[3];

    if (renderer == NULL || desc == NULL || out_material == NULL) {
        if (out_material != NULL) {
            *out_material = NULL;
        }
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->layout == NULL || renderer->camera_buffer == NULL) {
        *out_material = NULL;
        return LR_ERROR_RENDER;
    }

    material = (lr_material *)calloc(1, sizeof(lr_material));
    if (material == NULL) {
        *out_material = NULL;
        return LR_ERROR_OUT_OF_MEMORY;
    }
    material->renderer = renderer;
    material->type = LR_MATERIAL_UNLIT;
    memcpy(material->color, desc->color, sizeof(material->color));

    if (lc_binding_set_create(renderer->layout, &material->set) !=
        LC_SUCCESS) {
        free(material);
        *out_material = NULL;
        return LR_ERROR_RENDER;
    }
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_UNIFORM_BUFFER;
    writes[0].u.buffer.buffer = renderer->camera_buffer;
    writes[0].u.buffer.offset = 0;
    writes[0].u.buffer.size = 0;
    /* Texture/sampler fall back to renderer defaults (never NULL in
     * the set, so no special shader branch is needed). */
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLED_IMAGE;
    writes[1].u.image.view = (desc->base_color_texture != NULL)
                                 ? desc->base_color_texture
                                 : renderer->fallback_view;
    writes[2].binding = 2;
    writes[2].array_element = 0;
    writes[2].type = LC_BINDING_SAMPLER;
    writes[2].u.sampler.sampler =
        (desc->sampler != NULL) ? desc->sampler : renderer->default_sampler;
    if (lc_binding_set_update(material->set, writes, 3) != LC_SUCCESS) {
        lc_binding_set_destroy(material->set);
        free(material);
        *out_material = NULL;
        return LR_ERROR_INVALID_ARGUMENT;
    }

    lr_material_list_add(renderer, material);
    *out_material = material;
    return LR_SUCCESS;
}

void lr_material_destroy(lr_material *material) {
    if (material == NULL) {
        return;
    }
    lr_material_list_remove(material);
    lc_binding_set_destroy(material->set);
    lc_buffer_destroy(material->param_buffer);
    free(material);
}

lr_material_type lr_material_get_type(const lr_material *material) {
    if (material == NULL) {
        return LR_MATERIAL_UNKNOWN;
    }
    if (material->type != LR_MATERIAL_UNLIT &&
        material->type != LR_MATERIAL_PBR_METALLIC_ROUGHNESS) {
        return LR_MATERIAL_UNKNOWN;
    }
    return material->type;
}

void lr_material_get_pbr_info(const lr_material *material,
                              lr_pbr_material_info *out_info) {
    lr_pbr_material_info empty;

    memset(&empty, 0, sizeof(empty));
    empty.type = LR_MATERIAL_UNKNOWN;
    if (out_info == NULL) {
        return;
    }
    if (material == NULL) {
        *out_info = empty;
        return;
    }
    empty.type = lr_material_get_type(material);
    if (material->type != LR_MATERIAL_PBR_METALLIC_ROUGHNESS) {
        *out_info = empty;
        return;
    }
    empty.base_color_factor[0] = material->base_color[0];
    empty.base_color_factor[1] = material->base_color[1];
    empty.base_color_factor[2] = material->base_color[2];
    empty.base_color_factor[3] = material->base_color[3];
    empty.metallic_factor = material->metallic_factor;
    empty.roughness_factor = material->roughness_factor;
    empty.base_color_texture = material->base_color_texture;
    empty.metallic_roughness_texture =
        material->metallic_roughness_texture;
    empty.normal_texture = material->normal_texture;
    empty.occlusion_texture = material->occlusion_texture;
    empty.emissive_texture = material->emissive_texture;
    empty.sampler = material->sampler;
    empty.emissive_factor[0] = material->emissive_factor[0];
    empty.emissive_factor[1] = material->emissive_factor[1];
    empty.emissive_factor[2] = material->emissive_factor[2];
    empty.normal_scale = material->normal_scale;
    empty.occlusion_strength = material->occlusion_strength;
    empty.double_sided = material->double_sided;
    empty.alpha_mode = material->alpha_mode;
    *out_info = empty;
}

lr_result lr_material_create_pbr(lr_renderer *renderer,
                                 const lr_pbr_material_desc *desc,
                                 lr_material **out_material) {
    lr_material *material;
    lc_buffer_desc bdesc;
    lr_material_gpu gpu;
    lc_binding_write writes[9];

    if (renderer == NULL || desc == NULL || out_material == NULL) {
        if (out_material != NULL) {
            *out_material = NULL;
        }
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->pbr_layout == NULL || renderer->camera_buffer == NULL ||
        renderer->light_buffer == NULL) {
        *out_material = NULL;
        return LR_ERROR_RENDER;
    }
    if (desc->alpha_mode != LR_ALPHA_OPAQUE &&
        desc->alpha_mode != LR_ALPHA_MASK &&
        desc->alpha_mode != LR_ALPHA_BLEND) {
        *out_material = NULL;
        return LR_ERROR_INVALID_ARGUMENT;
    }

    material = (lr_material *)calloc(1, sizeof(lr_material));
    if (material == NULL) {
        *out_material = NULL;
        return LR_ERROR_OUT_OF_MEMORY;
    }
    material->renderer = renderer;
    material->type = LR_MATERIAL_PBR_METALLIC_ROUGHNESS;
    memcpy(material->base_color, desc->base_color_factor,
           sizeof(material->base_color));
    material->metallic_factor = desc->metallic_factor;
    material->roughness_factor = desc->roughness_factor;
    material->base_color_texture = desc->base_color_texture;
    material->metallic_roughness_texture = desc->metallic_roughness_texture;
    material->normal_texture = desc->normal_texture;
    material->occlusion_texture = desc->occlusion_texture;
    material->emissive_texture = desc->emissive_texture;
    material->sampler = desc->sampler;
    memcpy(material->emissive_factor, desc->emissive_factor,
           sizeof(material->emissive_factor));
    material->normal_scale = desc->normal_scale;
    material->occlusion_strength = desc->occlusion_strength;
    material->double_sided = (desc->double_sided != 0) ? 1 : 0;
    material->alpha_mode = desc->alpha_mode;

    /* Parameter buffer: written once, never per frame. */
    memset(&gpu, 0, sizeof(gpu));
    memcpy(gpu.base_color, material->base_color, sizeof(gpu.base_color));
    memcpy(gpu.emissive, material->emissive_factor, sizeof(gpu.emissive));
    gpu.metallic = material->metallic_factor;
    gpu.roughness = material->roughness_factor;
    gpu.normal_scale = material->normal_scale;
    gpu.occlusion_strength = material->occlusion_strength;
    gpu.flags = (uint32_t)(material->double_sided ? 1u : 0u) |
                ((uint32_t)material->alpha_mode << 8);
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = sizeof(gpu);
    bdesc.usage = LC_BUFFER_USAGE_UNIFORM;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(renderer->device, &bdesc, &material->param_buffer) !=
            LC_SUCCESS ||
        lc_buffer_write(material->param_buffer, 0, &gpu, sizeof(gpu)) !=
            LC_SUCCESS) {
        free(material);
        *out_material = NULL;
        return LR_ERROR_RENDER;
    }

    if (lc_binding_set_create(renderer->pbr_layout, &material->set) !=
        LC_SUCCESS) {
        lc_buffer_destroy(material->param_buffer);
        free(material);
        *out_material = NULL;
        return LR_ERROR_RENDER;
    }
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_UNIFORM_BUFFER;
    writes[0].u.buffer.buffer = renderer->camera_buffer;
    writes[0].u.buffer.offset = 0;
    writes[0].u.buffer.size = 0;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_UNIFORM_BUFFER;
    writes[1].u.buffer.buffer = renderer->light_buffer;
    writes[1].u.buffer.offset = 0;
    writes[1].u.buffer.size = 0;
    writes[2].binding = 2;
    writes[2].array_element = 0;
    writes[2].type = LC_BINDING_UNIFORM_BUFFER;
    writes[2].u.buffer.buffer = material->param_buffer;
    writes[2].u.buffer.offset = 0;
    writes[2].u.buffer.size = 0;
    /* Missing maps bind neutral fallbacks (no shader branches). */
    writes[3].binding = 3;
    writes[3].array_element = 0;
    writes[3].type = LC_BINDING_SAMPLED_IMAGE;
    writes[3].u.image.view = (desc->base_color_texture != NULL)
                                 ? desc->base_color_texture
                                 : renderer->fallback_view;
    writes[4].binding = 4;
    writes[4].array_element = 0;
    writes[4].type = LC_BINDING_SAMPLED_IMAGE;
    writes[4].u.image.view = (desc->metallic_roughness_texture != NULL)
                                 ? desc->metallic_roughness_texture
                                 : renderer->fallback_mr_view;
    writes[5].binding = 5;
    writes[5].array_element = 0;
    writes[5].type = LC_BINDING_SAMPLED_IMAGE;
    writes[5].u.image.view = (desc->normal_texture != NULL)
                                 ? desc->normal_texture
                                 : renderer->fallback_normal_view;
    writes[6].binding = 6;
    writes[6].array_element = 0;
    writes[6].type = LC_BINDING_SAMPLED_IMAGE;
    writes[6].u.image.view = (desc->occlusion_texture != NULL)
                                 ? desc->occlusion_texture
                                 : renderer->fallback_occlusion_view;
    writes[7].binding = 7;
    writes[7].array_element = 0;
    writes[7].type = LC_BINDING_SAMPLED_IMAGE;
    writes[7].u.image.view = (desc->emissive_texture != NULL)
                                 ? desc->emissive_texture
                                 : renderer->fallback_emissive_view;
    writes[8].binding = 8;
    writes[8].array_element = 0;
    writes[8].type = LC_BINDING_SAMPLER;
    writes[8].u.sampler.sampler =
        (desc->sampler != NULL) ? desc->sampler : renderer->default_sampler;
    if (lc_binding_set_update(material->set, writes, 9) != LC_SUCCESS) {
        lc_binding_set_destroy(material->set);
        lc_buffer_destroy(material->param_buffer);
        free(material);
        *out_material = NULL;
        return LR_ERROR_INVALID_ARGUMENT;
    }

    lr_material_list_add(renderer, material);
    *out_material = material;
    return LR_SUCCESS;
}
