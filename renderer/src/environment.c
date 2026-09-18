/*
 * Environment lighting (Phase 17): renderer-level IBL objects with
 * GPU preprocessing (equirectangular -> cubemap -> irradiance +
 * GGX prefilter chain), a shared split-sum BRDF LUT, HDR scene
 * targets, sky recording, and the tonemap output pass.
 *
 * Public LumaC only (backend-independence audit enforced). No
 * compute: every pass is a fullscreen-triangle graphics draw.
 *
 * Lifetime rules (Phase 16 lesson): derived environment images
 * have FIXED sizes and are never recreated, so frame-set bindings
 * written at build stay valid; only the HDR scene target recreates
 * (on extent change, after a device drain) with generation-checked
 * rebinding. Intensity/rotation are mapped UBO writes (the buffer
 * itself never moves), never descriptor traffic.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h> /* TEMP build staging */
#include <math.h>
#include <time.h>

#include "luma_renderer/luma_renderer.h"
#include "internal/renderer_internal.h"

extern const unsigned char lr_fulltri_vert_spv[];
extern const unsigned long lr_fulltri_vert_spv_size;
extern const unsigned char lr_eq2cube_frag_spv[];
extern const unsigned long lr_eq2cube_frag_spv_size;
extern const unsigned char lr_irradiance_frag_spv[];
extern const unsigned long lr_irradiance_frag_spv_size;
extern const unsigned char lr_prefilter_frag_spv[];
extern const unsigned long lr_prefilter_frag_spv_size;
extern const unsigned char lr_brdf_frag_spv[];
extern const unsigned long lr_brdf_frag_spv_size;
extern const unsigned char lr_sky_vert_spv[];
extern const unsigned long lr_sky_vert_spv_size;
extern const unsigned char lr_sky_frag_spv[];
extern const unsigned long lr_sky_frag_spv_size;
extern const unsigned char lr_editor_sky_vert_spv[];
extern const unsigned long lr_editor_sky_vert_spv_size;
extern const unsigned char lr_editor_sky_frag_spv[];
extern const unsigned long lr_editor_sky_frag_spv_size;
extern const unsigned char lr_editor_grid_frag_spv[];
extern const unsigned long lr_editor_grid_frag_spv_size;
extern const unsigned char lr_tonemap_frag_spv[];
extern const unsigned long lr_tonemap_frag_spv_size;


/* ------------------------------------------------------------------
 * Small builders (all fail-loud, caller unwinds).
 * ------------------------------------------------------------------ */

static lr_result lr_env_make_image(lc_device *device, lc_format format,
                                   uint32_t size, uint32_t mips,
                                   int renderable, lc_image **out_image) {
    lc_image_desc idesc;

    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = format;
    idesc.width = size;
    idesc.height = size;
    idesc.depth = 1;
    idesc.mip_levels = mips;
    idesc.array_layers = 6;
    idesc.usage = (uint32_t)LC_IMAGE_USAGE_SAMPLED |
                  (uint32_t)LC_IMAGE_USAGE_TRANSFER_SRC |
                  (uint32_t)LC_IMAGE_USAGE_TRANSFER_DST |
                  (renderable ? (uint32_t)LC_IMAGE_USAGE_COLOR_ATTACHMENT
                              : 0u);
    idesc.flags = (uint32_t)LC_IMAGE_FLAG_CUBE_COMPATIBLE;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &idesc, out_image) != LC_SUCCESS) {
        *out_image = NULL;
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

static lr_result lr_env_make_cube_view(lc_image *image, uint32_t mips,
                                       lc_image_view **out_view) {
    lc_image_view_desc vdesc;

    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_CUBE;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.base_mip_level = 0;
    vdesc.mip_level_count = mips;
    vdesc.base_array_layer = 0;
    vdesc.array_layer_count = 6;
    if (lc_image_view_create(image, &vdesc, out_view) != LC_SUCCESS) {
        *out_view = NULL;
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

static lr_result lr_env_make_face_view(lc_image *image, uint32_t mip,
                                       uint32_t face,
                                       lc_image_view **out_view) {
    lc_image_view_desc vdesc;

    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.base_mip_level = mip;
    vdesc.mip_level_count = 1;
    vdesc.base_array_layer = face;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(image, &vdesc, out_view) != LC_SUCCESS) {
        *out_view = NULL;
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

static lr_result lr_env_make_face_target(lc_device *device,
                                         lc_image_view *face_view,
                                         uint32_t extent,
                                         lc_render_target **out_target) {
    lc_render_target_create_desc tdesc;
    lc_render_target_attachment att;

    memset(&tdesc, 0, sizeof(tdesc));
    tdesc.width = extent;
    tdesc.height = extent;
    att.view = face_view;
    tdesc.color_attachments = &att;
    tdesc.color_attachment_count = 1;
    tdesc.depth_stencil_attachment = NULL;
    if (lc_render_target_create(device, &tdesc, out_target) !=
        LC_SUCCESS) {
        *out_target = NULL;
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

/* Env image family helper (image + sampling view + per-face
 * views + face targets for one mip level). `mips` is the image's
 * full chain depth (explicit: 8/1/7 for cube/irradiance/prefilter);
 * the sampling view always spans it. */
static lr_result lr_env_make_family(
    lr_renderer *renderer, uint32_t size, uint32_t mips, uint32_t mip,
    lc_image **out_image, lc_image_view **out_view,
    lc_image_view **face_views, lc_render_target **targets) {
    uint32_t f;

    if (lr_env_make_image(renderer->device, LC_FORMAT_RGBA16_FLOAT,
                          size, mips, 1, out_image) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lr_env_make_cube_view(*out_image, mips, out_view) !=
        LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    for (f = 0; f < 6; f++) {
        if (lr_env_make_face_view(*out_image, mip, f, &face_views[f]) !=
                LR_SUCCESS ||
            lr_env_make_face_target(renderer->device, face_views[f],
                                    size >> mip, &targets[f]) !=
                LR_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    return LR_SUCCESS;
}

/* Post pipeline for one fragment shader over a depthless color
 * signature (shared post layout, fullscreen triangle vertex input,
 * no culling, no blending, no depth). */
static lr_result lr_env_post_pipeline(
    lr_renderer *renderer, lc_shader *frag, uint32_t push_size,
    uint32_t push_vis, lc_format color_format, lc_pipeline **out) {
    lc_graphics_pipeline_desc pd;
    lc_push_constant_range push;
    lc_render_target_desc sig;
    const lc_binding_layout *layouts[1];

    memset(&pd, 0, sizeof(pd));
    pd.vertex_shader = renderer->fulltri_vertex_shader;
    pd.fragment_shader = frag;
    pd.vertex_bindings = NULL;
    pd.vertex_binding_count = 0;
    pd.vertex_attributes = NULL;
    pd.vertex_attribute_count = 0;
    layouts[0] = renderer->post_layout;
    pd.binding_layouts = layouts;
    pd.binding_layout_count = 1;
    pd.cull_mode = LC_CULL_NONE;
    pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
    pd.depth_test_enable = 0;
    pd.depth_write_enable = 0;
    push.visibility = push_vis;
    push.offset = 0;
    push.size = push_size;
    pd.push_constant_ranges = &push;
    pd.push_constant_range_count = 1;
    memset(&sig, 0, sizeof(sig));
    sig.width = 4;
    sig.height = 4;
    sig.color_attachment_count = 1;
    sig.color_formats[0] = color_format;
    sig.depth_stencil_format = LC_FORMAT_UNDEFINED;
    sig.samples = LC_SAMPLE_COUNT_1;
    pd.render_target = sig;
    if (lc_graphics_pipeline_create(renderer->device, &pd, out) !=
        LC_SUCCESS) {
        *out = NULL;
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

static lr_result lr_env_make_shader(lr_renderer *renderer, int is_vertex,
                                    const unsigned char *code,
                                    unsigned long code_size,
                                    lc_shader **out) {
    lc_shader_desc sdesc;

    memset(&sdesc, 0, sizeof(sdesc));
    sdesc.stage = is_vertex ? LC_SHADER_STAGE_VERTEX
                            : LC_SHADER_STAGE_FRAGMENT;
    sdesc.code = code;
    sdesc.code_size = (size_t)code_size;
    sdesc.entry_point = NULL;
    if (lc_shader_create(renderer->device, &sdesc, out) != LC_SUCCESS) {
        *out = NULL;
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

static void lr_environment_free_derived(lr_environment *env) {
    uint32_t f;
    uint32_t m;

    if (env == NULL) {
        return;
    }
    for (m = 0; m < LR_ENV_PREFILTER_MAX_MIPS; m++) {
        for (f = 0; f < 6; f++) {
            lc_render_target_destroy(env->prefilter_targets[m][f]);
            env->prefilter_targets[m][f] = NULL;
            lc_image_view_destroy(env->prefilter_face_views[m][f]);
            env->prefilter_face_views[m][f] = NULL;
        }
    }
    for (f = 0; f < 6; f++) {
        lc_render_target_destroy(env->irradiance_targets[f]);
        env->irradiance_targets[f] = NULL;
        lc_image_view_destroy(env->irradiance_face_views[f]);
        env->irradiance_face_views[f] = NULL;
    }
    for (m = 0; m < LR_ENV_CUBE_MAX_MIPS; m++) {
        for (f = 0; f < 6; f++) {
            lc_render_target_destroy(env->cube_targets[m][f]);
            env->cube_targets[m][f] = NULL;
            lc_image_view_destroy(env->cube_face_views[m][f]);
            env->cube_face_views[m][f] = NULL;
        }
    }
    lc_binding_set_destroy(env->pref_set);
    env->pref_set = NULL;
    lc_binding_set_destroy(env->irr_set);
    env->irr_set = NULL;
    lc_binding_set_destroy(env->eq_set);
    env->eq_set = NULL;
    lc_binding_set_destroy(env->frame_set);
    env->frame_set = NULL;
    lc_image_view_destroy(env->prefilter_view);
    env->prefilter_view = NULL;
    lc_image_destroy(env->prefilter_image);
    env->prefilter_image = NULL;
    lc_image_view_destroy(env->irradiance_view);
    env->irradiance_view = NULL;
    lc_image_destroy(env->irradiance_image);
    env->irradiance_image = NULL;
    lc_image_view_destroy(env->cube_view);
    env->cube_view = NULL;
    lc_image_destroy(env->cube_image);
    env->cube_image = NULL;
    env->ready = 0;
}

/* Write one preprocessing stage set ({image, sampler}, once — never
 * updated between binds in one recording). */
static lr_result lr_env_write_stage_set(lr_renderer *renderer,
                                        lc_binding_set *set,
                                        lc_image_view *view,
                                        lc_sampler *samp) {
    lc_binding_write writes[2];

    (void)renderer;
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_SAMPLED_IMAGE;
    writes[0].u.image.view = view;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLER;
    writes[1].u.sampler.sampler = samp;
    if (lc_binding_set_update(set, writes, 2) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

/* One fullscreen-triangle pass into one face target (the triangle
 * covers every pixel, so DONT_CARE load is exact). The set is
 * pre-written (never updated between binds). Counts stats. */
static lr_result lr_env_face_pass(lr_renderer *renderer,
                                  lc_command_encoder *enc,
                                  lc_pipeline *pipeline,
                                  lc_image_view *face_view, uint32_t extent,
                                  lc_binding_set *set, int32_t face,
                                  float roughness) {
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lr_post_push push;
    lc_result cr;

    memset(&catt, 0, sizeof(catt));
    catt.view = face_view;
    /* STORE (not DONT_CARE): the faces are sampled right after,
     * and DONT_CARE stores leave the layout UNDEFINED on this
     * stack (correct pixels by luck, validation errors for sure). */
    catt.load_op = LC_LOAD_OP_DONT_CARE;
    catt.store_op = LC_STORE_OP_STORE;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.depth_attachment = NULL;
    pdesc.width = extent;
    pdesc.height = extent;
    cr = lc_encoder_begin_render_pass(enc, &pdesc);
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    cr = lc_encoder_bind_pipeline(enc, pipeline);
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    cr = lc_encoder_bind_binding_set(enc, pipeline, 0, set);
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    push.face = face;
    push.roughness = roughness;
    cr = lc_encoder_push_constants(
        enc, pipeline, (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT, 0,
        sizeof(push), &push);
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    cr = lc_encoder_draw(enc, 3, 0);
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    cr = lc_encoder_end_render_pass(enc);
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    renderer->stats.environment_passes++;
    return LR_SUCCESS;
}

/* Lazily create the three preprocessing pipelines (kept out of
 * renderer_create so cold starts pay no compile cost until first
 * environment use). */
static lr_result lr_env_ensure_post_pipes(lr_renderer *renderer) {
    if (renderer->eq2cube_pipeline != NULL &&
        renderer->irradiance_pipeline != NULL &&
        renderer->prefilter_pipeline != NULL) {
        return LR_SUCCESS;
    }
    if (renderer->fulltri_vertex_shader == NULL &&
        lr_env_make_shader(renderer, 1, lr_fulltri_vert_spv,
                           lr_fulltri_vert_spv_size,
                           &renderer->fulltri_vertex_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (renderer->eq2cube_fragment_shader == NULL &&
        lr_env_make_shader(renderer, 0, lr_eq2cube_frag_spv,
                           lr_eq2cube_frag_spv_size,
                           &renderer->eq2cube_fragment_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (renderer->irradiance_fragment_shader == NULL &&
        lr_env_make_shader(renderer, 0, lr_irradiance_frag_spv,
                           lr_irradiance_frag_spv_size,
                           &renderer->irradiance_fragment_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (renderer->prefilter_fragment_shader == NULL &&
        lr_env_make_shader(renderer, 0, lr_prefilter_frag_spv,
                           lr_prefilter_frag_spv_size,
                           &renderer->prefilter_fragment_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* The three preprocessing passes share one signature (single
     * R16F color, no depth); only the fragment stage differs. */
    if (renderer->eq2cube_pipeline == NULL &&
        lr_env_post_pipeline(renderer,
                             renderer->eq2cube_fragment_shader,
                             sizeof(lr_post_push),
                             (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT,
                             LC_FORMAT_RGBA16_FLOAT,
                             &renderer->eq2cube_pipeline) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (renderer->irradiance_pipeline == NULL &&
        lr_env_post_pipeline(renderer,
                             renderer->irradiance_fragment_shader,
                             sizeof(lr_post_push),
                             (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT,
                             LC_FORMAT_RGBA16_FLOAT,
                             &renderer->irradiance_pipeline) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (renderer->prefilter_pipeline == NULL &&
        lr_env_post_pipeline(renderer,
                             renderer->prefilter_fragment_shader,
                             sizeof(lr_post_push),
                             (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT,
                             LC_FORMAT_RGBA16_FLOAT,
                             &renderer->prefilter_pipeline) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

/* ------------------------------------------------------------------
 * Environment objects.
 * ------------------------------------------------------------------ */

lr_result lr_environment_create(lr_renderer *renderer,
                               const lr_environment_desc *desc,
                               lr_environment **out_env) {
    lr_environment *env;

    if (renderer == NULL || desc == NULL || out_env == NULL) {
        if (out_env != NULL) {
            *out_env = NULL;
        }
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (desc->environment_texture == NULL || desc->sampler == NULL ||
        !(desc->intensity >= 0.0f)) {
        *out_env = NULL;
        return LR_ERROR_INVALID_ARGUMENT;
    }
    env = (lr_environment *)calloc(1, sizeof(lr_environment));
    if (env == NULL) {
        *out_env = NULL;
        return LR_ERROR_OUT_OF_MEMORY;
    }
    env->renderer = renderer;
    env->source_view = desc->environment_texture;
    env->source_sampler = desc->sampler;
    env->source_id =
        lc_image_view_get_resource_id(desc->environment_texture);
    env->sampler_id = lc_sampler_get_resource_id(desc->sampler);
    env->intensity = desc->intensity;
    env->rotation = desc->rotation;
    env->dirty = 1;
    env->next = renderer->environments;
    if (renderer->environments != NULL) {
        renderer->environments->prev = env;
    }
    renderer->environments = env;
    *out_env = env;
    return LR_SUCCESS;
}

void lr_environment_destroy(lr_environment *env) {
    lr_renderer *renderer;

    if (env == NULL) {
        return;
    }
    renderer = env->renderer;
    /* Detaching is the app's job; never destroy the active
     * environment mid-frame. Drain first: derived views may still
     * be referenced by submitted work. */
    if (renderer != NULL && renderer->device != NULL) {
        lc_device_wait_idle(renderer->device);
        if (renderer->active_env == env) {
            renderer->active_env = NULL;
        }
        if (renderer->environments == env) {
            renderer->environments = env->next;
        }
        if (env->prev != NULL) {
            env->prev->next = env->next;
        }
        if (env->next != NULL) {
            env->next->prev = env->prev;
        }
    }
    lr_environment_free_derived(env);
    free(env);
}

lr_result lr_environment_update(lr_environment *env,
                               const lr_environment_desc *desc) {
    if (env == NULL || desc == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (desc->environment_texture == NULL || desc->sampler == NULL ||
        !(desc->intensity >= 0.0f)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    /* New source: derived resources rebuild on next use. Identity
     * (not just pointer): a recreated source may reuse the freed
     * wrapper address (Phase 16 ABA class). */
    if (desc->environment_texture != env->source_view ||
        desc->sampler != env->source_sampler ||
        lc_image_view_get_resource_id(desc->environment_texture) !=
            env->source_id ||
        lc_sampler_get_resource_id(desc->sampler) != env->sampler_id) {
        env->source_view = desc->environment_texture;
        env->source_sampler = desc->sampler;
        env->source_id =
            lc_image_view_get_resource_id(desc->environment_texture);
        env->sampler_id = lc_sampler_get_resource_id(desc->sampler);
        env->dirty = 1;
    }
    if (desc->intensity != env->intensity) {
        env->intensity = desc->intensity;
        env->params_version++;
    }
    if (desc->rotation != env->rotation) {
        env->rotation = desc->rotation;
        env->params_version++;
    }
    return LR_SUCCESS;
}

lr_result lr_environment_set_intensity(lr_environment *env,
                                       float intensity) {
    if (env == NULL || !(intensity >= 0.0f)) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (intensity != env->intensity) {
        env->intensity = intensity;
        env->params_version++;
    }
    return LR_SUCCESS;
}

lr_result lr_environment_set_rotation(lr_environment *env,
                                      float rotation) {
    if (env == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (rotation != env->rotation) {
        env->rotation = rotation;
        env->params_version++;
    }
    return LR_SUCCESS;
}

lr_result lr_renderer_set_environment(lr_renderer *renderer,
                                      lr_environment *env) {
    if (renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    renderer->active_env = env;
    return LR_SUCCESS;
}

/* Render all base-cube mips analytically from the equirect
 * source (each mip face gets its own pass — fully ordered, no
 * immediate-submit blits mid-frame, every mip ends STORE-marked
 * sampled-readable for the passes that sample it). */
static lr_result lr_env_build_cube_faces(lr_environment *env,
                                         lc_command_encoder *enc) {
    lr_renderer *renderer = env->renderer;
    uint32_t m;
    uint32_t f;

    for (m = 0; m < LR_ENV_CUBE_MAX_MIPS; m++) {
        for (f = 0; f < 6; f++) {
            if (lr_env_face_pass(
                    renderer, enc, renderer->eq2cube_pipeline,
                    env->cube_face_views[m][f],
                    (uint32_t)LR_ENV_CUBE_SIZE >> m, env->eq_set,
                    (int32_t)f, 0.0f) != LR_SUCCESS) {
                return LR_ERROR_RENDER;
            }
        }
    }
    return LR_SUCCESS;
}

/* Full (re)build of one environment's derived resources. Starts
 * from a clean slate (idempotent) and leaves ready=0 + dirty=1 on
 * any failure so the next frame retries instead of sampling
 * half-built maps. */
lr_result lr_environment_build(lr_environment *env,
                               lc_command_encoder *enc) {
    lr_renderer *renderer;
    clock_t t0;
    uint32_t f;
    uint32_t m;
    lc_binding_write writes[5];

    if (env == NULL || enc == NULL || env->renderer == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    renderer = env->renderer;
    t0 = clock();
    /* Drain first: derived views may still be sampled by in-flight
     * work being replaced here (same Phase-16 class of fault;
     * builds are rare, so the coarse wait is cheap). */
    lc_device_wait_idle(renderer->device);
    lr_environment_free_derived(env);
    env->dirty = 1;
    if (lr_env_ensure_post_pipes(renderer) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* The frame set references the shared BRDF view: build it
     * before writing the set. */
    if (lr_renderer_ensure_brdf(renderer, enc) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Stage sets (written once; never updated between binds).
     * The equirect set is stable now; irradiance/prefilter sets
     * follow the (possibly fallback-rebuilt) cube below. */
    if (lc_binding_set_create(renderer->post_layout, &env->eq_set) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lr_env_write_stage_set(renderer, env->eq_set, env->source_view,
                               env->source_sampler) != LR_SUCCESS) {
        fprintf(stderr, "[dbg] build: eq set failed\n");
        return LR_ERROR_RENDER;
    }

    /* Base cube, full mip chain for sky minification (every mip
     * rendered analytically — ordered, no blits, every level ends
     * STORE-marked sampled-readable). */
    if (lr_env_make_image(renderer->device, LC_FORMAT_RGBA16_FLOAT,
                          LR_ENV_CUBE_SIZE, 8, 1,
                          &env->cube_image) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lr_env_make_cube_view(env->cube_image, 8, &env->cube_view) !=
        LR_SUCCESS) {
        fprintf(stderr, "[dbg] build: cube view failed\n");
        return LR_ERROR_RENDER;
    }
    for (m = 0; m < LR_ENV_CUBE_MAX_MIPS; m++) {
        for (f = 0; f < 6; f++) {
            if (lr_env_make_face_view(env->cube_image, m, f,
                                      &env->cube_face_views[m][f]) !=
                    LR_SUCCESS ||
                lr_env_make_face_target(
                    renderer->device, env->cube_face_views[m][f],
                    (uint32_t)LR_ENV_CUBE_SIZE >> m,
                    &env->cube_targets[m][f]) != LR_SUCCESS) {
                return LR_ERROR_RENDER;
            }
        }
    }
    if (lr_env_build_cube_faces(env, enc) != LR_SUCCESS) {
        fprintf(stderr, "[dbg] build: cube faces failed\n");
        return LR_ERROR_RENDER;
    }

    /* Irradiance, single mip (set written after the cube view is
     * final, i.e. past any mip fallback rebuild). */
    if (lc_binding_set_create(renderer->post_layout, &env->irr_set) !=
            LC_SUCCESS ||
        lc_binding_set_create(renderer->post_layout, &env->pref_set) !=
            LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (lr_env_write_stage_set(renderer, env->irr_set, env->cube_view,
                               env->source_sampler) != LR_SUCCESS ||
        lr_env_write_stage_set(renderer, env->pref_set, env->cube_view,
                               env->source_sampler) != LR_SUCCESS) {
        fprintf(stderr, "[dbg] build: irr/pref sets failed\n");
        return LR_ERROR_RENDER;
    }
    if (lr_env_make_family(renderer, LR_ENV_IRRADIANCE_SIZE, 1, 0,
                           &env->irradiance_image, &env->irradiance_view,
                           env->irradiance_face_views,
                           env->irradiance_targets) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    for (f = 0; f < 6; f++) {
        if (lr_env_face_pass(renderer, enc,
                             renderer->irradiance_pipeline,
                             env->irradiance_face_views[f],
                             LR_ENV_IRRADIANCE_SIZE, env->irr_set,
                             (int32_t)f, 0.0f) != LR_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    /* Prefilter chain: mip m holds roughness m/(mips-1). The chain
     * depth is structural (64 -> 7); anything else fails loudly. */
    if (LR_ENV_PREFILTER_MAX_MIPS != 7) {
        return LR_ERROR_RENDER;
    }
    if (lr_env_make_family(renderer, LR_ENV_PREFILTER_SIZE,
                           LR_ENV_PREFILTER_MAX_MIPS, 0,
                           &env->prefilter_image, &env->prefilter_view,
                           &env->prefilter_face_views[0][0],
                           &env->prefilter_targets[0][0]) !=
        LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* NOTE: make_family builds one mip level's faces+targets; the
     * remaining prefilter mips follow explicitly below. */
    for (m = 1; m < (uint32_t)LR_ENV_PREFILTER_MAX_MIPS; m++) {
        for (f = 0; f < 6; f++) {
            if (lr_env_make_face_view(env->prefilter_image, m, f,
                                      &env->prefilter_face_views[m][f]) !=
                    LR_SUCCESS ||
                lr_env_make_face_target(
                    renderer->device, env->prefilter_face_views[m][f],
                    (uint32_t)LR_ENV_PREFILTER_SIZE >> m,
                    &env->prefilter_targets[m][f]) != LR_SUCCESS) {
                return LR_ERROR_RENDER;
            }
        }
    }
    for (m = 0; m < (uint32_t)LR_ENV_PREFILTER_MAX_MIPS; m++) {
        float rough =
            (float)m / (float)(LR_ENV_PREFILTER_MAX_MIPS - 1);

        for (f = 0; f < 6; f++) {
            if (lr_env_face_pass(
                    renderer, enc, renderer->prefilter_pipeline,
                    env->prefilter_face_views[m][f],
                    (uint32_t)LR_ENV_PREFILTER_SIZE >> m, env->pref_set,
                    (int32_t)f, rough) != LR_SUCCESS) {
                return LR_ERROR_RENDER;
            }
        }
    }
    /* Frame set for PBR slot 2 (written once; handles never move
     * afterwards, so no per-frame traffic and no staleness). */
    if (lc_binding_set_create(renderer->env_layout, &env->frame_set) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_UNIFORM_BUFFER;
    writes[0].u.buffer.buffer = renderer->env_params_buffer;
    writes[0].u.buffer.offset = 0;
    writes[0].u.buffer.size = 0;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLED_IMAGE;
    writes[1].u.image.view = env->irradiance_view;
    writes[2].binding = 2;
    writes[2].array_element = 0;
    writes[2].type = LC_BINDING_SAMPLED_IMAGE;
    writes[2].u.image.view = env->prefilter_view;
    writes[3].binding = 3;
    writes[3].array_element = 0;
    writes[3].type = LC_BINDING_SAMPLED_IMAGE;
    writes[3].u.image.view = renderer->brdf_view;
    writes[4].binding = 4;
    writes[4].array_element = 0;
    writes[4].type = LC_BINDING_SAMPLER;
    writes[4].u.sampler.sampler = env->source_sampler;
    if (lc_binding_set_update(env->frame_set, writes, 5) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    env->prefilter_mips = (uint32_t)LR_ENV_PREFILTER_MAX_MIPS;
    renderer->env_generation++;
    env->source_epoch = renderer->env_generation;
    env->rebuilds++;
    renderer->environment_rebuilds++;
    renderer->stats.environment_rebuilds++;
    env->last_build_ms =
        (uint32_t)((clock() - t0) * 1000 / CLOCKS_PER_SEC);
    env->ready = 1;
    env->dirty = 0;
    return LR_SUCCESS;
}

lr_result lr_environment_ensure_built(lr_environment *env,
                                      lc_command_encoder *enc) {
    if (env == NULL || enc == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (env->ready && !env->dirty) {
        return LR_SUCCESS;
    }
    return lr_environment_build(env, enc);
}

/* ------------------------------------------------------------------
 * Shared BRDF integration LUT (environment-independent, built once
 * per renderer into a 256x256 RG16F target).
 * ------------------------------------------------------------------ */

lr_result lr_renderer_ensure_brdf(lr_renderer *renderer,
                                  lc_command_encoder *enc) {
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_render_target_create_desc tdesc;
    lc_render_target_attachment att;
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_result cr;

    if (renderer == NULL || enc == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->brdf_ready) {
        return LR_SUCCESS;
    }
    if (renderer->brdf_fragment_shader == NULL &&
        lr_env_make_shader(renderer, 0, lr_brdf_frag_spv,
                           lr_brdf_frag_spv_size,
                           &renderer->brdf_fragment_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (renderer->brdf_pipeline == NULL &&
        lr_env_post_pipeline(renderer, renderer->brdf_fragment_shader,
                             sizeof(lr_post_push),
                             (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT,
                             LC_FORMAT_RG16_FLOAT,
                             &renderer->brdf_pipeline) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* NOTE: the BRDF pass samples nothing, but the shared post
     * layout still needs valid descriptors: the write-once post
     * set (fallback bindings from renderer create) is bound. */
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RG16_FLOAT;
    idesc.width = LR_ENV_BRDF_SIZE;
    idesc.height = LR_ENV_BRDF_SIZE;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = (uint32_t)LC_IMAGE_USAGE_SAMPLED |
                  (uint32_t)LC_IMAGE_USAGE_TRANSFER_SRC |
                  (uint32_t)LC_IMAGE_USAGE_TRANSFER_DST |
                  (uint32_t)LC_IMAGE_USAGE_COLOR_ATTACHMENT;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(renderer->device, &idesc,
                        &renderer->brdf_image) != LC_SUCCESS) {
        renderer->brdf_image = NULL;
        return LR_ERROR_RENDER;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.base_mip_level = 0;
    vdesc.mip_level_count = 1;
    vdesc.base_array_layer = 0;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(renderer->brdf_image, &vdesc,
                             &renderer->brdf_view) != LC_SUCCESS) {
        renderer->brdf_view = NULL;
        lc_image_destroy(renderer->brdf_image);
        renderer->brdf_image = NULL;
        return LR_ERROR_RENDER;
    }
    memset(&tdesc, 0, sizeof(tdesc));
    tdesc.width = LR_ENV_BRDF_SIZE;
    tdesc.height = LR_ENV_BRDF_SIZE;
    att.view = renderer->brdf_view;
    tdesc.color_attachments = &att;
    tdesc.color_attachment_count = 1;
    tdesc.depth_stencil_attachment = NULL;
    if (lc_render_target_create(renderer->device, &tdesc,
                                &renderer->brdf_target) != LC_SUCCESS) {
        renderer->brdf_target = NULL;
        return LR_ERROR_RENDER;
    }
    memset(&catt, 0, sizeof(catt));
    catt.view = renderer->brdf_view;
    catt.load_op = LC_LOAD_OP_DONT_CARE;
    catt.store_op = LC_STORE_OP_STORE;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.depth_attachment = NULL;
    pdesc.width = LR_ENV_BRDF_SIZE;
    pdesc.height = LR_ENV_BRDF_SIZE;
    cr = lc_encoder_begin_render_pass(enc, &pdesc);
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    cr = lc_encoder_bind_pipeline(enc, renderer->brdf_pipeline);
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_bind_binding_set(enc, renderer->brdf_pipeline,
                                         0, renderer->post_set);
    }
    if (cr == LC_SUCCESS) {
        /* Shared push shape, ignored by the shader. */
        lr_post_push push;

        push.face = 0;
        push.roughness = 0.0f;
        cr = lc_encoder_push_constants(
            enc, renderer->brdf_pipeline,
            (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT, 0, sizeof(push),
            &push);
    }
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_draw(enc, 3, 0);
    }
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_end_render_pass(enc);
    }
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    renderer->stats.environment_passes++;
    renderer->brdf_ready = 1;
    return LR_SUCCESS;
}

/* ------------------------------------------------------------------
 * HDR scene target (renderer-owned, auto-sized to the output).
 * ------------------------------------------------------------------ */

lr_result lr_renderer_ensure_hdr(lr_renderer *renderer, uint32_t width,
                                 uint32_t height) {
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_render_target_create_desc tdesc;
    lc_render_target_attachment att;

    if (renderer == NULL || width == 0 || height == 0) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->hdr_target != NULL && renderer->hdr_width == width &&
        renderer->hdr_height == height) {
        return LR_SUCCESS;
    }
    /* Drain first: the old target may still be sampled by an
     * in-flight tonemap (same Phase-16 class of fault). */
    lc_device_wait_idle(renderer->device);
    lc_render_target_destroy(renderer->hdr_target);
    renderer->hdr_target = NULL;
    lc_image_view_destroy(renderer->hdr_view);
    renderer->hdr_view = NULL;
    lc_image_destroy(renderer->hdr_image);
    renderer->hdr_image = NULL;
    lc_image_view_destroy(renderer->hdr_depth_view);
    renderer->hdr_depth_view = NULL;
    lc_image_destroy(renderer->hdr_depth_image);
    renderer->hdr_depth_image = NULL;

    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RGBA16_FLOAT;
    idesc.width = width;
    idesc.height = height;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = (uint32_t)LC_IMAGE_USAGE_SAMPLED |
                  (uint32_t)LC_IMAGE_USAGE_TRANSFER_SRC |
                  (uint32_t)LC_IMAGE_USAGE_TRANSFER_DST |
                  (uint32_t)LC_IMAGE_USAGE_COLOR_ATTACHMENT;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(renderer->device, &idesc,
                        &renderer->hdr_image) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.base_mip_level = 0;
    vdesc.mip_level_count = 1;
    vdesc.base_array_layer = 0;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(renderer->hdr_image, &vdesc,
                             &renderer->hdr_view) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    idesc.format = LC_FORMAT_D32_FLOAT;
    /* Capture-capable: depth readback (picking/debugging/SSAO
     * validation) needs TRANSFER_SRC; harmless for depth output.
     * SAMPLED feeds the Phase-23 Hi-Z seed copy (the pass cache
     * then finalizes depth SHADER_READ instead of attachment-only;
     * sampling happens strictly after the pass). */
    idesc.usage = (uint32_t)LC_IMAGE_USAGE_DEPTH_STENCIL |
                  (uint32_t)LC_IMAGE_USAGE_TRANSFER_SRC |
                  (uint32_t)LC_IMAGE_USAGE_SAMPLED;
    if (lc_image_create(renderer->device, &idesc,
                        &renderer->hdr_depth_image) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
    if (lc_image_view_create(renderer->hdr_depth_image, &vdesc,
                             &renderer->hdr_depth_view) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    memset(&tdesc, 0, sizeof(tdesc));
    tdesc.width = width;
    tdesc.height = height;
    att.view = renderer->hdr_view;
    tdesc.color_attachments = &att;
    tdesc.color_attachment_count = 1;
    tdesc.depth_stencil_attachment = renderer->hdr_depth_view;
    if (lc_render_target_create(renderer->device, &tdesc,
                                &renderer->hdr_target) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    renderer->hdr_width = width;
    renderer->hdr_height = height;
    renderer->hdr_generation++;
    return LR_SUCCESS;
}

/* Refresh the mapped env-params block from the active environment
 * (infallible memcpy; the buffer itself never moves, so no binding
 * ever dangles on parameter change). */
void lr_environment_write_params(lr_renderer *renderer) {
    lr_env_params_gpu *gpu;

    if (renderer == NULL || renderer->env_params_mapped == NULL) {
        return;
    }
    gpu = (lr_env_params_gpu *)renderer->env_params_mapped;
    if (renderer->active_env != NULL) {
        gpu->intensity = renderer->active_env->intensity;
        gpu->rotation = renderer->active_env->rotation;
        gpu->prefilter_mips =
            (float)renderer->active_env->prefilter_mips;
        gpu->ibl_active = 1.0f;
    } else {
        gpu->intensity = 1.0f;
        gpu->rotation = 0.0f;
        gpu->prefilter_mips = 0.0f;
        gpu->ibl_active = 0.0f;
    }
}

/* ------------------------------------------------------------------
 * Sky + tonemap passes.
 * ------------------------------------------------------------------ */

/* Lazily create the sky pipeline (HDR signature: R16F + D32, depth
 * test off — the sky draws first and scene covers it). */
static lr_result lr_env_ensure_sky_pipeline(lr_renderer *renderer) {
    lc_push_constant_range push;
    lc_graphics_pipeline_desc pd;
    lc_render_target_desc sig;
    const lc_binding_layout *layouts[1];

    if (renderer->sky_pipeline != NULL) {
        return LR_SUCCESS;
    }
    if (renderer->sky_vertex_shader == NULL &&
        lr_env_make_shader(renderer, 1, lr_sky_vert_spv,
                           lr_sky_vert_spv_size,
                           &renderer->sky_vertex_shader) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (renderer->sky_fragment_shader == NULL &&
        lr_env_make_shader(renderer, 0, lr_sky_frag_spv,
                           lr_sky_frag_spv_size,
                           &renderer->sky_fragment_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    memset(&pd, 0, sizeof(pd));
    pd.vertex_shader = renderer->sky_vertex_shader;
    pd.fragment_shader = renderer->sky_fragment_shader;
    pd.vertex_bindings = NULL;
    pd.vertex_binding_count = 0;
    pd.vertex_attributes = NULL;
    pd.vertex_attribute_count = 0;
    layouts[0] = renderer->post_layout;
    pd.binding_layouts = layouts;
    pd.binding_layout_count = 1;
    pd.cull_mode = LC_CULL_NONE;
    pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
    pd.depth_test_enable = 0;
    pd.depth_write_enable = 0;
    push.visibility = (uint32_t)LC_SHADER_VISIBILITY_ALL_GRAPHICS;
    push.offset = 0;
    push.size = sizeof(lr_sky_push);
    pd.push_constant_ranges = &push;
    pd.push_constant_range_count = 1;
    memset(&sig, 0, sizeof(sig));
    sig.width = 4;
    sig.height = 4;
    sig.color_attachment_count = 1;
    sig.color_formats[0] = LC_FORMAT_RGBA16_FLOAT;
    sig.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    sig.samples = LC_SAMPLE_COUNT_1;
    pd.render_target = sig;
    if (lc_graphics_pipeline_create(renderer->device, &pd,
                                    &renderer->sky_pipeline) !=
        LC_SUCCESS) {
        renderer->sky_pipeline = NULL;
        return LR_ERROR_RENDER;
    }
    return LR_SUCCESS;
}

/* Draw the sky first inside the open HDR pass (env must be built).
 * Translation cancels in the ray (world - camPos); only camera
 * orientation moves the sky. */
lr_result lr_renderer_record_sky(lr_renderer *renderer,
                                 lc_command_encoder *enc) {
    lr_environment *env;
    float vp[16];
    float inv[16];
    lr_sky_push push;
    lc_binding_write writes[2];
    lc_result cr;

    if (renderer == NULL || enc == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    env = renderer->active_env;
    if (env == NULL || !env->ready) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (lr_env_ensure_sky_pipeline(renderer) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    lr_mat4_multiply(vp, renderer->camera.projection,
                     renderer->camera.view);
    if (!lr_mat4_inverse(vp, inv)) {
        return LR_ERROR_RENDER;
    }
    memcpy(push.inv_view_proj, inv, sizeof(inv));
    push.cam_pos[0] = renderer->camera.position[0];
    push.cam_pos[1] = renderer->camera.position[1];
    push.cam_pos[2] = renderer->camera.position[2];
    push.cam_pos[3] = 1.0f;
    push.params[0] = env->intensity;
    push.params[1] = env->rotation;
    push.params[2] = 0.0f;
    push.params[3] = 0.0f;
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_SAMPLED_IMAGE;
    writes[0].u.image.view = env->cube_view;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLER;
    writes[1].u.sampler.sampler = env->source_sampler;
    if (lc_binding_set_update(renderer->sky_set, writes, 2) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    cr = lc_encoder_bind_pipeline(enc, renderer->sky_pipeline);
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_bind_binding_set(enc, renderer->sky_pipeline,
                                         0, renderer->sky_set);
    }
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_push_constants(
            enc, renderer->sky_pipeline,
            (uint32_t)LC_SHADER_VISIBILITY_ALL_GRAPHICS, 0,
            sizeof(push), &push);
    }
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_draw(enc, 3, 0);
    }
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    renderer->stats.sky_draw_calls++;
    return LR_SUCCESS;
}

/* ------------------------------------------------------------------
 * Editor viewport environment (R-012): procedural sky + infinite
 * grid. Backend-neutral (public LumaC only), lazy singletons, no
 * textures, no lights, no IBL contribution. Both record into the
 * caller's OPEN HDR scene pass; both no-op unless the editor bridge
 * enabled them AND no authored environment is active (authored sky
 * takes precedence — the editor never covers game content).
 * ------------------------------------------------------------------ */

/* Shared ray uniforms (invVP + camPos) for both editor passes. */
static void lr_editor_ray_uniforms(lr_renderer *renderer, float inv[16],
                                   float cam[4]) {
    float vp[16];

    lr_mat4_multiply(vp, renderer->camera.projection,
                     renderer->camera.view);
    if (!lr_mat4_inverse(vp, inv)) {
        memset(inv, 0, 16 * sizeof(float));
        inv[0] = inv[5] = inv[10] = inv[15] = 1.0f;
    }
    cam[0] = renderer->camera.position[0];
    cam[1] = renderer->camera.position[1];
    cam[2] = renderer->camera.position[2];
    cam[3] = 1.0f;
}

/* Ensure one editor pipeline (sky: opaque, depth off; grid: blended,
 * depth test on + writes off). Signature is the live HDR pass
 * (R16F + D32); no binding layouts (push-only fullscreen triangle). */
static lr_result lr_editor_pipeline_for(lr_renderer *renderer,
                                        int is_grid,
                                        lc_pipeline **out) {
    lc_push_constant_range push;
    lc_graphics_pipeline_desc pd;
    lc_render_target_desc sig;
    lc_blend_attachment blend;

    if (!is_grid && renderer->editor_sky_pipeline != NULL) {
        *out = renderer->editor_sky_pipeline;
        return LR_SUCCESS;
    }
    if (is_grid && renderer->editor_grid_pipeline != NULL) {
        *out = renderer->editor_grid_pipeline;
        return LR_SUCCESS;
    }
    if (!is_grid &&
        renderer->editor_sky_vertex_shader == NULL &&
        lr_env_make_shader(renderer, 1, lr_editor_sky_vert_spv,
                           lr_editor_sky_vert_spv_size,
                           &renderer->editor_sky_vertex_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (!is_grid &&
        renderer->editor_sky_fragment_shader == NULL &&
        lr_env_make_shader(renderer, 0, lr_editor_sky_frag_spv,
                           lr_editor_sky_frag_spv_size,
                           &renderer->editor_sky_fragment_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    if (is_grid &&
        renderer->editor_grid_fragment_shader == NULL &&
        lr_env_make_shader(renderer, 0, lr_editor_grid_frag_spv,
                           lr_editor_grid_frag_spv_size,
                           &renderer->editor_grid_fragment_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* The grid shares the sky vertex shader (identical ray math +
     * push prefix); make sure it exists for the grid path too. */
    if (is_grid &&
        renderer->editor_sky_vertex_shader == NULL &&
        lr_env_make_shader(renderer, 1, lr_editor_sky_vert_spv,
                           lr_editor_sky_vert_spv_size,
                           &renderer->editor_sky_vertex_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    memset(&pd, 0, sizeof(pd));
    pd.vertex_shader = renderer->editor_sky_vertex_shader;
    pd.fragment_shader = is_grid
                             ? renderer->editor_grid_fragment_shader
                             : renderer->editor_sky_fragment_shader;
    pd.vertex_bindings = NULL;
    pd.vertex_binding_count = 0;
    pd.vertex_attributes = NULL;
    pd.vertex_attribute_count = 0;
    pd.binding_layouts = NULL;
    pd.binding_layout_count = 0;
    pd.cull_mode = LC_CULL_NONE;
    pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
    if (is_grid) {
        /* Depth-tested (LESS default) so authored geometry obscures
         * the grid; writes off so the grid never corrupts depth;
         * source-alpha blend for the distance fade. */
        pd.depth_test_enable = 1;
        pd.depth_write_enable = 0;
        memset(&blend, 0, sizeof(blend));
        blend.blend_enable = 1;
        blend.src_color_factor = LC_BLEND_SRC_ALPHA;
        blend.dst_color_factor = LC_BLEND_ONE_MINUS_SRC_ALPHA;
        blend.color_op = LC_BLEND_OP_ADD;
        blend.src_alpha_factor = LC_BLEND_ONE;
        blend.dst_alpha_factor = LC_BLEND_ONE_MINUS_SRC_ALPHA;
        blend.alpha_op = LC_BLEND_OP_ADD;
        pd.blend = &blend;
        pd.blend_attachment_count = 1;
    } else {
        pd.depth_test_enable = 0;
        pd.depth_write_enable = 0;
    }
    push.visibility = (uint32_t)LC_SHADER_VISIBILITY_ALL_GRAPHICS;
    push.offset = 0;
    push.size = is_grid ? sizeof(lr_editor_grid_push)
                        : sizeof(lr_editor_sky_push);
    pd.push_constant_ranges = &push;
    pd.push_constant_range_count = 1;
    memset(&sig, 0, sizeof(sig));
    sig.width = 4;
    sig.height = 4;
    sig.color_attachment_count = 1;
    sig.color_formats[0] = LC_FORMAT_RGBA16_FLOAT;
    sig.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    sig.samples = LC_SAMPLE_COUNT_1;
    pd.render_target = sig;
    if (lc_graphics_pipeline_create(
            renderer->device, &pd,
            is_grid ? &renderer->editor_grid_pipeline
                    : &renderer->editor_sky_pipeline) != LC_SUCCESS) {
        if (is_grid) {
            renderer->editor_grid_pipeline = NULL;
        } else {
            renderer->editor_sky_pipeline = NULL;
        }
        return LR_ERROR_RENDER;
    }
    *out = is_grid ? renderer->editor_grid_pipeline
                   : renderer->editor_sky_pipeline;
    return LR_SUCCESS;
}

/* Draw the procedural editor sky first in the HDR pass (replaces the
 * black clear where no authored environment is active). */
lr_result lr_renderer_record_editor_sky(lr_renderer *renderer,
                                        lc_command_encoder *enc) {
    lc_pipeline *pipeline = NULL;
    lr_editor_sky_push push;
    lc_result cr;

    if (renderer == NULL || enc == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!renderer->editor_env.enabled) {
        return LR_SUCCESS;
    }
    if (renderer->active_env != NULL && renderer->active_env->ready) {
        return LR_SUCCESS; /* authored sky takes precedence */
    }
    if (lr_editor_pipeline_for(renderer, 0, &pipeline) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    lr_editor_ray_uniforms(renderer, push.inv_view_proj, push.cam_pos);
    push.zone_a[0] = LR_EDITOR_SKY_ZENITH_R;
    push.zone_a[1] = LR_EDITOR_SKY_ZENITH_G;
    push.zone_a[2] = LR_EDITOR_SKY_ZENITH_B;
    push.zone_a[3] = 0.0f;
    push.zone_b[0] = LR_EDITOR_SKY_HORIZON_R;
    push.zone_b[1] = LR_EDITOR_SKY_HORIZON_G;
    push.zone_b[2] = LR_EDITOR_SKY_HORIZON_B;
    push.zone_b[3] = 0.0f;
    push.zone_c[0] = LR_EDITOR_SKY_NADIR_R;
    push.zone_c[1] = LR_EDITOR_SKY_NADIR_G;
    push.zone_c[2] = LR_EDITOR_SKY_NADIR_B;
    push.zone_c[3] = LR_EDITOR_SKY_SOFTNESS;
    cr = lc_encoder_bind_pipeline(enc, pipeline);
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_push_constants(
            enc, pipeline,
            (uint32_t)LC_SHADER_VISIBILITY_ALL_GRAPHICS, 0,
            sizeof(push), &push);
    }
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_draw(enc, 3, 0);
    }
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    renderer->stats.sky_draw_calls++;
    return LR_SUCCESS;
}

/* Draw the procedural infinite grid after scene geometry (depth
 * test on, writes off, blended): authored floors obscure it. */
lr_result lr_renderer_record_editor_grid(lr_renderer *renderer,
                                         lc_command_encoder *enc) {
    lc_pipeline *pipeline = NULL;
    lr_editor_grid_push push;
    lc_result cr;

    if (renderer == NULL || enc == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (!renderer->editor_env.enabled ||
        !renderer->editor_env.grid_enabled) {
        return LR_SUCCESS;
    }
    if (renderer->active_env != NULL && renderer->active_env->ready) {
        return LR_SUCCESS; /* authored environment owns the frame */
    }
    if (lr_editor_pipeline_for(renderer, 1, &pipeline) != LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    lr_editor_ray_uniforms(renderer, push.inv_view_proj, push.cam_pos);
    push.grid_a[0] = LR_EDITOR_GRID_MINOR_R;
    push.grid_a[1] = LR_EDITOR_GRID_MINOR_G;
    push.grid_a[2] = LR_EDITOR_GRID_MINOR_B;
    push.grid_a[3] = LR_EDITOR_GRID_MINOR_A;
    push.grid_b[0] = LR_EDITOR_GRID_MAJOR_R;
    push.grid_b[1] = LR_EDITOR_GRID_MAJOR_G;
    push.grid_b[2] = LR_EDITOR_GRID_MAJOR_B;
    push.grid_b[3] = LR_EDITOR_GRID_MAJOR_A;
    push.grid_c[0] = push.grid_c[1] = push.grid_c[2] = 0.0f;
    push.grid_c[3] = LR_EDITOR_GRID_FADE_DIST;
    push.grid_d[0] = LR_EDITOR_GRID_AXIS_STRENGTH;
    push.grid_d[1] = LR_EDITOR_GRID_AXIS_FALLOFF;
    push.grid_d[2] = push.grid_d[3] = 0.0f;
    cr = lc_encoder_bind_pipeline(enc, pipeline);
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_push_constants(
            enc, pipeline,
            (uint32_t)LC_SHADER_VISIBILITY_ALL_GRAPHICS, 0,
            sizeof(push), &push);
    }
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_draw(enc, 3, 0);
    }
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    renderer->stats.sky_draw_calls++;
    return LR_SUCCESS;
}

/* Tonemap mini-cache lookup (per output signature; bounded). */static lr_result lr_env_tonemap_pipeline_for(
    lr_renderer *renderer, const lc_render_target_desc *signature,
    lc_pipeline **out) {
    uint32_t i;

    for (i = 0; i < renderer->tonemap_pipeline_count; i++) {
        const lc_render_target_desc *cached =
            &renderer->tonemap_pipelines[i].signature;
        uint32_t k;
        int equal = 0;

        if (renderer->tonemap_pipelines[i].material_type ==
                LR_MATERIAL_UNKNOWN &&
            renderer->tonemap_pipelines[i].cull_mode ==
                LC_CULL_NONE &&
            cached->color_attachment_count ==
                signature->color_attachment_count &&
            cached->depth_stencil_format ==
                signature->depth_stencil_format &&
            cached->samples == signature->samples) {
            uint32_t n = cached->color_attachment_count;

            equal = 1;
            for (k = 0; k < n; k++) {
                if (cached->color_formats[k] !=
                    signature->color_formats[k]) {
                    equal = 0;
                    break;
                }
            }
        }
        if (equal) {
            *out = renderer->tonemap_pipelines[i].pipeline;
            return LR_SUCCESS;
        }
    }
    if (renderer->tonemap_pipeline_count >= 4) {
        return LR_ERROR_UNSUPPORTED;
    }
    if (renderer->tonemap_fragment_shader == NULL &&
        lr_env_make_shader(renderer, 0, lr_tonemap_frag_spv,
                           lr_tonemap_frag_spv_size,
                           &renderer->tonemap_fragment_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    {
        lc_push_constant_range push;
        lc_graphics_pipeline_desc pd;
        const lc_binding_layout *layouts[1];

        memset(&pd, 0, sizeof(pd));
        pd.vertex_shader = renderer->fulltri_vertex_shader;
        pd.fragment_shader = renderer->tonemap_fragment_shader;
        pd.vertex_bindings = NULL;
        pd.vertex_binding_count = 0;
        pd.vertex_attributes = NULL;
        pd.vertex_attribute_count = 0;
        layouts[0] = renderer->post_layout;
        pd.binding_layouts = layouts;
        pd.binding_layout_count = 1;
        pd.cull_mode = LC_CULL_NONE;
        pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
        pd.depth_test_enable = 0;
        pd.depth_write_enable = 0;
        push.visibility = (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT;
        push.offset = 0;
        push.size = sizeof(lr_tonemap_push);
        pd.push_constant_ranges = &push;
        pd.push_constant_range_count = 1;
        pd.render_target = *signature;
        if (lc_graphics_pipeline_create(
                renderer->device, &pd,
                &renderer
                     ->tonemap_pipelines[renderer->tonemap_pipeline_count]
                     .pipeline) != LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    renderer->tonemap_pipelines[renderer->tonemap_pipeline_count]
        .signature = *signature;
    renderer->tonemap_pipelines[renderer->tonemap_pipeline_count]
        .material_type = LR_MATERIAL_UNKNOWN;
    renderer->tonemap_pipelines[renderer->tonemap_pipeline_count]
        .cull_mode = LC_CULL_NONE;
    *out = renderer->tonemap_pipelines[renderer->tonemap_pipeline_count]
               .pipeline;
    renderer->tonemap_pipeline_count++;
    return LR_SUCCESS;
}

/* Record the tonemap output pass into the caller's open pass:
 * exposure + operator over the HDR scene, clamped to LDR. No
 * gamma here (the output target format encodes sRGB exactly once,
 * or stays linear for UNORM targets). */
lr_result lr_renderer_record_tonemap(lr_renderer *renderer,
                                     lc_command_encoder *enc,
                                     lc_render_target *target) {
    lc_pipeline *pipeline = NULL;
    lc_render_target_desc sig;
    lr_tonemap_push push;
    lc_binding_write writes[2];
    lc_result cr;
    uint32_t i;

    if (renderer == NULL || enc == NULL || target == NULL) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->hdr_target == NULL || !renderer->hdr_has_scene) {
        return LR_ERROR_INVALID_ARGUMENT;
    }
    if (renderer->fulltri_vertex_shader == NULL &&
        lr_env_make_shader(renderer, 1, lr_fulltri_vert_spv,
                           lr_fulltri_vert_spv_size,
                           &renderer->fulltri_vertex_shader) !=
            LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Output signature follows the actual output target (the
     * tonemap mini-cache is keyed per encountered signature). */
    memset(&sig, 0, sizeof(sig));
    sig.width = lc_render_target_get_width(target);
    sig.height = lc_render_target_get_height(target);
    sig.color_attachment_count =
        lc_render_target_get_color_count(target);
    for (i = 0; i < sig.color_attachment_count &&
                i < LC_MAX_COLOR_ATTACHMENTS;
         i++) {
        sig.color_formats[i] =
            lc_render_target_get_color_format(target, i);
    }
    /* The caller's pass owns depth: swapchain passes always carry
     * the swapchain depth attachment, offscreen output passes
     * usually carry none. Key the pipeline by the target's depth
     * format so both shapes record compatibly. */
    sig.depth_stencil_format =
        lc_render_target_get_depth_format(target);
    sig.samples = lc_render_target_get_samples(target);
    if (sig.color_attachment_count == 0 ||
        sig.color_attachment_count > LC_MAX_COLOR_ATTACHMENTS) {
        return LR_ERROR_INCOMPATIBLE;
    }
    if (lr_env_tonemap_pipeline_for(renderer, &sig, &pipeline) !=
        LR_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_SAMPLED_IMAGE;
    /* Chain head when post stages ran, else the raw HDR view. */
    writes[0].u.image.view = (renderer->post_head_view != NULL)
                                 ? renderer->post_head_view
                                 : renderer->hdr_view;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLER;
    writes[1].u.sampler.sampler = renderer->default_sampler;
    /* Identity-guarded write: updating a set between two binds in
     * one recording invalidates the command buffer, so identical
     * inputs (multiview/screenshot second output) bind as-is.
     * IDs never alias across recreation (Phase 18 identity). */
    if (lc_image_view_get_resource_id(writes[0].u.image.view) !=
            renderer->tonemap_set_view_id ||
        lc_sampler_get_resource_id(writes[1].u.sampler.sampler) !=
            renderer->tonemap_set_sampler_id) {
        if (lc_binding_set_update(renderer->tonemap_set, writes, 2) !=
            LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        renderer->tonemap_set_view_id =
            lc_image_view_get_resource_id(writes[0].u.image.view);
        renderer->tonemap_set_sampler_id =
            lc_sampler_get_resource_id(writes[1].u.sampler.sampler);
    }
    cr = lc_encoder_bind_pipeline(enc, pipeline);
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_bind_binding_set(enc, pipeline, 0,
                                         renderer->tonemap_set);
    }
    if (cr == LC_SUCCESS) {
        push.ev_mult = powf(2.0f, renderer->exposure_ev);
        push.op = (renderer->tonemap_op == LR_TONEMAP_ACES) ? 1 : 0;
        cr = lc_encoder_push_constants(
            enc, pipeline, (uint32_t)LC_SHADER_VISIBILITY_FRAGMENT,
            0, sizeof(push), &push);
    }
    if (cr == LC_SUCCESS) {
        cr = lc_encoder_draw(enc, 3, 0);
    }
    if (cr != LC_SUCCESS) {
        return lr_map_result(cr);
    }
    renderer->stats.tonemap_passes++;
    return LR_SUCCESS;
}

/* ------------------------------------------------------------------
 * Renderer-owned environment resources (cheap: layouts, mapped
 * params buffer, empty set, empty sets for sky/tonemap; no shader
 * compiles, no pipelines — those stay lazy).
 * ------------------------------------------------------------------ */

lr_result lr_renderer_create_env_resources(lr_renderer *renderer) {
    lc_binding_desc env_slots[5];
    lc_binding_desc post_slots[2];
    lc_binding_layout_desc ldesc;
    lc_buffer_desc bdesc;
    lc_binding_write writes[5];
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_image_upload_desc upload;

    /* PBR slot 2: params UBO + irradiance + prefilter + BRDF + one
     * sampler. */
    env_slots[0].binding = 0;
    env_slots[0].type = LC_BINDING_UNIFORM_BUFFER;
    env_slots[0].count = 1;
    env_slots[0].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    env_slots[1].binding = 1;
    env_slots[1].type = LC_BINDING_SAMPLED_IMAGE;
    env_slots[1].count = 1;
    env_slots[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    env_slots[2].binding = 2;
    env_slots[2].type = LC_BINDING_SAMPLED_IMAGE;
    env_slots[2].count = 1;
    env_slots[2].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    env_slots[3].binding = 3;
    env_slots[3].type = LC_BINDING_SAMPLED_IMAGE;
    env_slots[3].count = 1;
    env_slots[3].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    env_slots[4].binding = 4;
    env_slots[4].type = LC_BINDING_SAMPLER;
    env_slots[4].count = 1;
    env_slots[4].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    ldesc.bindings = env_slots;
    ldesc.binding_count = 5;
    if (lc_binding_layout_create(renderer->device, &ldesc,
                                 &renderer->env_layout) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Shared post layout: sampled image + sampler. */
    post_slots[0].binding = 0;
    post_slots[0].type = LC_BINDING_SAMPLED_IMAGE;
    post_slots[0].count = 1;
    post_slots[0].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    post_slots[1].binding = 1;
    post_slots[1].type = LC_BINDING_SAMPLER;
    post_slots[1].count = 1;
    post_slots[1].visibility = LC_SHADER_VISIBILITY_FRAGMENT;
    ldesc.bindings = post_slots;
    ldesc.binding_count = 2;
    if (lc_binding_layout_create(renderer->device, &ldesc,
                                 &renderer->post_layout) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Mapped env-params block (written per render, never moved). */
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = sizeof(lr_env_params_gpu);
    bdesc.usage = LC_BUFFER_USAGE_UNIFORM;
    bdesc.memory = LC_MEMORY_CPU_TO_GPU;
    if (lc_buffer_create(renderer->device, &bdesc,
                         &renderer->env_params_buffer) != LC_SUCCESS ||
        lc_buffer_map(renderer->env_params_buffer,
                      &renderer->env_params_mapped) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Empty environment: black 1x1x6 cube + black 1x1 RG16F, bound
     * when no environment is active (shaders skip IBL anyway). */
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RGBA16_FLOAT;
    idesc.width = 1;
    idesc.height = 1;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 6;
    idesc.usage = (uint32_t)LC_IMAGE_USAGE_SAMPLED |
                   (uint32_t)LC_IMAGE_USAGE_TRANSFER_SRC |
                   (uint32_t)LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.flags = (uint32_t)LC_IMAGE_FLAG_CUBE_COMPATIBLE;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(renderer->device, &idesc,
                        &renderer->empty_cube_image) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_CUBE;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.base_mip_level = 0;
    vdesc.mip_level_count = 1;
    vdesc.base_array_layer = 0;
    vdesc.array_layer_count = 6;
    if (lc_image_view_create(renderer->empty_cube_image, &vdesc,
                             &renderer->empty_cube_view) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    idesc.format = LC_FORMAT_RG16_FLOAT;
    idesc.array_layers = 1;
    idesc.flags = 0;
    if (lc_image_create(renderer->device, &idesc,
                        &renderer->empty_brdf_image) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(renderer->empty_brdf_image, &vdesc,
                             &renderer->empty_brdf_view) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Zero-fill both (black = no IBL contribution either way).
     * Half-float zeros are all-zero bytes. */
    {
        static const unsigned char black48[48] = { 0 };
        static const unsigned char black4[4] = { 0, 0, 0, 0 };

        memset(&upload, 0, sizeof(upload));
        upload.width = 1;
        upload.height = 1;
        upload.depth = 1;
        upload.data = black48;
        upload.data_size = sizeof(black48);
        if (lc_image_write(renderer->empty_cube_image, &upload) !=
            LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
        upload.data = black4;
        upload.data_size = sizeof(black4);
        if (lc_image_write(renderer->empty_brdf_image, &upload) !=
            LC_SUCCESS) {
            return LR_ERROR_RENDER;
        }
    }
    if (lc_binding_set_create(renderer->env_layout,
                              &renderer->empty_env_set) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_UNIFORM_BUFFER;
    writes[0].u.buffer.buffer = renderer->env_params_buffer;
    writes[0].u.buffer.offset = 0;
    writes[0].u.buffer.size = 0;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLED_IMAGE;
    writes[1].u.image.view = renderer->empty_cube_view;
    writes[2].binding = 2;
    writes[2].array_element = 0;
    writes[2].type = LC_BINDING_SAMPLED_IMAGE;
    writes[2].u.image.view = renderer->empty_cube_view;
    writes[3].binding = 3;
    writes[3].array_element = 0;
    writes[3].type = LC_BINDING_SAMPLED_IMAGE;
    writes[3].u.image.view = renderer->empty_brdf_view;
    writes[4].binding = 4;
    writes[4].array_element = 0;
    writes[4].type = LC_BINDING_SAMPLER;
    writes[4].u.sampler.sampler = renderer->default_sampler;
    if (lc_binding_set_update(renderer->empty_env_set, writes, 5) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* Sky + tonemap + shared post sets (images rebound per use;
     * created once). */
    if (lc_binding_set_create(renderer->post_layout,
                              &renderer->post_set) != LC_SUCCESS ||
        lc_binding_set_create(renderer->post_layout,
                              &renderer->sky_set) != LC_SUCCESS ||
        lc_binding_set_create(renderer->post_layout,
                              &renderer->tonemap_set) != LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* The post set carries fallback bindings for the BRDF pass
     * (which samples nothing) and is NEVER updated afterwards. */
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_SAMPLED_IMAGE;
    writes[0].u.image.view = renderer->fallback_view;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLER;
    writes[1].u.sampler.sampler = renderer->default_sampler;
    if (lc_binding_set_update(renderer->post_set, writes, 2) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    /* The post set carries fallback bindings for the BRDF pass
     * (which samples nothing) and is NEVER updated afterwards. */
    writes[0].binding = 0;
    writes[0].array_element = 0;
    writes[0].type = LC_BINDING_SAMPLED_IMAGE;
    writes[0].u.image.view = renderer->fallback_view;
    writes[1].binding = 1;
    writes[1].array_element = 0;
    writes[1].type = LC_BINDING_SAMPLER;
    writes[1].u.sampler.sampler = renderer->default_sampler;
    if (lc_binding_set_update(renderer->post_set, writes, 2) !=
        LC_SUCCESS) {
        return LR_ERROR_RENDER;
    }
    renderer->exposure_ev = 0.0f;
    renderer->tonemap_op = LR_TONEMAP_NONE;
    return LR_SUCCESS;
}

void lr_renderer_destroy_env_resources(lr_renderer *renderer) {
    lr_environment *env;
    lr_environment *next;
    uint32_t i;

    if (renderer == NULL) {
        return;
    }
    for (env = renderer->environments; env != NULL; env = next) {
        next = env->next;
        lr_environment_free_derived(env);
        free(env);
    }
    renderer->environments = NULL;
    renderer->active_env = NULL;
    for (i = 0; i < renderer->tonemap_pipeline_count; i++) {
        lc_pipeline_destroy(renderer->tonemap_pipelines[i].pipeline);
        renderer->tonemap_pipelines[i].pipeline = NULL;
    }
    renderer->tonemap_pipeline_count = 0;
    lc_pipeline_destroy(renderer->editor_grid_pipeline);
    renderer->editor_grid_pipeline = NULL;
    lc_pipeline_destroy(renderer->editor_sky_pipeline);
    renderer->editor_sky_pipeline = NULL;
    lc_pipeline_destroy(renderer->sky_pipeline);
    renderer->sky_pipeline = NULL;
    lc_pipeline_destroy(renderer->brdf_pipeline);
    renderer->brdf_pipeline = NULL;
    lc_pipeline_destroy(renderer->prefilter_pipeline);
    renderer->prefilter_pipeline = NULL;
    lc_pipeline_destroy(renderer->irradiance_pipeline);
    renderer->irradiance_pipeline = NULL;
    lc_pipeline_destroy(renderer->eq2cube_pipeline);
    renderer->eq2cube_pipeline = NULL;
    lc_shader_destroy(renderer->tonemap_fragment_shader);
    renderer->tonemap_fragment_shader = NULL;
    lc_shader_destroy(renderer->editor_grid_fragment_shader);
    renderer->editor_grid_fragment_shader = NULL;
    lc_shader_destroy(renderer->editor_sky_fragment_shader);
    renderer->editor_sky_fragment_shader = NULL;
    lc_shader_destroy(renderer->editor_sky_vertex_shader);
    renderer->editor_sky_vertex_shader = NULL;
    lc_shader_destroy(renderer->sky_fragment_shader);
    renderer->sky_fragment_shader = NULL;
    lc_shader_destroy(renderer->sky_vertex_shader);
    renderer->sky_vertex_shader = NULL;
    lc_shader_destroy(renderer->brdf_fragment_shader);
    renderer->brdf_fragment_shader = NULL;
    lc_shader_destroy(renderer->prefilter_fragment_shader);
    renderer->prefilter_fragment_shader = NULL;
    lc_shader_destroy(renderer->irradiance_fragment_shader);
    renderer->irradiance_fragment_shader = NULL;
    lc_shader_destroy(renderer->eq2cube_fragment_shader);
    renderer->eq2cube_fragment_shader = NULL;
    lc_shader_destroy(renderer->fulltri_vertex_shader);
    renderer->fulltri_vertex_shader = NULL;
    lc_binding_set_destroy(renderer->tonemap_set);
    renderer->tonemap_set = NULL;
    lc_binding_set_destroy(renderer->sky_set);
    renderer->sky_set = NULL;
    lc_render_target_destroy(renderer->brdf_target);
    renderer->brdf_target = NULL;
    lc_binding_set_destroy(renderer->empty_env_set);
    renderer->empty_env_set = NULL;
    lc_binding_set_destroy(renderer->post_set);
    renderer->post_set = NULL;
    lc_image_view_destroy(renderer->empty_brdf_view);
    renderer->empty_brdf_view = NULL;
    lc_image_destroy(renderer->empty_brdf_image);
    renderer->empty_brdf_image = NULL;
    lc_image_view_destroy(renderer->empty_cube_view);
    renderer->empty_cube_view = NULL;
    lc_image_destroy(renderer->empty_cube_image);
    renderer->empty_cube_image = NULL;
    lc_image_view_destroy(renderer->brdf_view);
    renderer->brdf_view = NULL;
    lc_image_destroy(renderer->brdf_image);
    renderer->brdf_image = NULL;
    lc_binding_layout_destroy(renderer->post_layout);
    renderer->post_layout = NULL;
    lc_binding_layout_destroy(renderer->env_layout);
    renderer->env_layout = NULL;
    lc_buffer_destroy(renderer->env_params_buffer);
    renderer->env_params_buffer = NULL;
    renderer->env_params_mapped = NULL;
    lc_render_target_destroy(renderer->hdr_target);
    renderer->hdr_target = NULL;
    lc_image_view_destroy(renderer->hdr_view);
    renderer->hdr_view = NULL;
    lc_image_destroy(renderer->hdr_image);
    renderer->hdr_image = NULL;
    lc_image_view_destroy(renderer->hdr_depth_view);
    renderer->hdr_depth_view = NULL;
    lc_image_destroy(renderer->hdr_depth_image);
    renderer->hdr_depth_image = NULL;
    renderer->hdr_width = 0;
    renderer->hdr_height = 0;
    renderer->hdr_generation = 0;
}
