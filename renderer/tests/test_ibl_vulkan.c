/*
 * Luma IBL Vulkan integration test (Phase 17).
 *
 * Offscreen pixel verification of environment lighting: HDR asset
 * upload, equirectangular-to-cubemap conversion, cubemap
 * orientation, diffuse irradiance, GGX prefilter endpoints, BRDF
 * LUT, split-sum IBL, metallic/roughness response, AO on indirect,
 * intensity/rotation, sky behavior, HDR range, exposure, ACES
 * tonemapping + sRGB output, shadow/IBL independence, emissive HDR,
 * resize/replacement/multiview safety, and 500-frame endurance.
 *
 * Design rule: synthetic procedural environments (exact known
 * colors/directions) isolate each stage; photographic fixtures only
 * prove the file path. If the environment cannot provide a window
 * or Vulkan setup, SKIP and exit 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_assets/luma_assets.h>

#include "graphics/graphics_internal.h"

#include "internal/renderer_internal.h"

#ifndef LA_FIXTURE_DIR
#define LA_FIXTURE_DIR "."
#endif

#define IB_TW 256u
#define IB_TH 256u

static int g_passed = 0;
static int g_failed = 0;

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        printf("[PASS] %s\n", msg); \
        g_passed++; \
    } else { \
        printf("[FAIL] %s\n", msg); \
        g_failed++; \
    } \
} while (0)

#define SKIP_ENV(what) do { \
    printf("SKIP: environment cannot provide %s\n", what); \
    lc_shutdown(); \
    return 0; \
} while (0)

#define FAIL_CLEANUP(msg) do { \
    printf("FAIL: %s\n", msg); \
    goto cleanup; \
} while (0)

/* ------------------------------------------------------------------
 * Environment + harness (HDR scene pass + tonemap output pass).
 * ------------------------------------------------------------------ */

typedef struct ibl_env {
    lc_device *device;
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
    lr_renderer *renderer;
    lr_camera camera;
    lc_image *color_img;
    lc_image_view *color_view;
    lc_render_target *target;
    lc_image *srgb_img;
    lc_image_view *srgb_view;
    lc_render_target *srgb_target;
    uint32_t tw;
    uint32_t th;
} ibl_env;

/* Pipeline-cache file for the whole binary (PART P18-AS): set by
 * main before device creation so every PART runs cached; deleted
 * at startup and after the final report. Empty = no file. */
static char g_pcache_path[512] = { 0 };

static int make_device(lc_device **out) {
    lc_device_desc desc = { 0 };

    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 1;
    if (g_pcache_path[0] != '\0') {
        desc.pipeline_cache_path = g_pcache_path;
    }
    *out = NULL;
    switch (lc_device_create(&desc, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_BACKEND_UNAVAILABLE:
    case LC_ERROR_NO_SUPPORTED_DEVICE:
        return 1;
    default:
        return -1;
    }
}

static int make_window(lc_window **out) {
    lc_window_desc desc;

    desc.title = "Luma IBL Test";
    desc.width = 800;
    desc.height = 600;
    *out = NULL;
    switch (lc_window_create(&desc, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_PLATFORM:
    case LC_ERROR_WINDOW_CREATION_FAILED:
        return 1;
    default:
        return -1;
    }
}

static void test_wait_idle(lc_device *device) {
    if (device != NULL && device->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->device);
    }
}

static unsigned char *readback_rgba8(lc_device *device, lc_image *image,
                                     uint32_t w, uint32_t h) {
    lc_buffer *staging = NULL;
    lc_buffer_desc bdesc;
    void *mapped = NULL;
    unsigned char *out = NULL;
    uint64_t bytes = (uint64_t)w * h * 4u;

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = bytes;
    bdesc.usage = LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_TO_CPU;
    if (lc_buffer_create(device, &bdesc, &staging) != LC_SUCCESS) {
        return NULL;
    }
    test_wait_idle(device);
    if (lc_vulkan_copy_image_to_buffer(device, image, 0, 0, w, h, 1,
                                       staging->vk_buffer, 0) !=
        LC_SUCCESS) {
        lc_buffer_destroy(staging);
        return NULL;
    }
    if (lc_buffer_map(staging, &mapped) != LC_SUCCESS || mapped == NULL) {
        lc_buffer_destroy(staging);
        return NULL;
    }
    out = (unsigned char *)malloc((size_t)bytes);
    if (out != NULL) {
        memcpy(out, mapped, (size_t)bytes);
    }
    lc_buffer_unmap(staging);
    lc_buffer_destroy(staging);
    return out;
}

/* Full-image float readback (R32F/R16F, white-box copy + convert). */
static float *readback_float(lc_device *device, lc_image *image,
                             uint32_t w, uint32_t h, int is_half) {
    lc_buffer *staging = NULL;
    lc_buffer_desc bdesc;
    void *mapped = NULL;
    float *out = NULL;
    uint64_t bytes = (uint64_t)w * h * 4u * (is_half ? 2u : 4u);
    uint64_t count = (uint64_t)w * h * 4u;
    uint64_t i;

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = bytes;
    bdesc.usage = LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_TO_CPU;
    if (lc_buffer_create(device, &bdesc, &staging) != LC_SUCCESS) {
        return NULL;
    }
    test_wait_idle(device);
    if (lc_vulkan_copy_image_to_buffer(device, image, 0, 0, w, h, 1,
                                       staging->vk_buffer, 0) !=
        LC_SUCCESS) {
        lc_buffer_destroy(staging);
        return NULL;
    }
    if (lc_buffer_map(staging, &mapped) != LC_SUCCESS || mapped == NULL) {
        lc_buffer_destroy(staging);
        return NULL;
    }
    out = (float *)malloc((size_t)count * sizeof(float));
    if (out != NULL) {
        if (is_half) {
            const uint16_t *src = (const uint16_t *)mapped;

            for (i = 0; i < count; i++) {
                out[i] = la_half_to_float(src[i]);
            }
        } else {
            memcpy(out, mapped, (size_t)count * sizeof(float));
        }
    }
    lc_buffer_unmap(staging);
    lc_buffer_destroy(staging);
    return out;
}

static int make_target(ibl_env *env, lc_format format, uint32_t w,
                       uint32_t h, int srgb_slot) {
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_render_target_create_desc tdesc;
    lc_render_target_attachment att;

    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = format;
    idesc.width = w;
    idesc.height = h;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                  LC_IMAGE_USAGE_TRANSFER_SRC | LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (srgb_slot) {
        if (lc_image_create(env->device, &idesc, &env->srgb_img) !=
            LC_SUCCESS) {
            return -1;
        }
    } else if (lc_image_create(env->device, &idesc, &env->color_img) !=
               LC_SUCCESS) {
        return -1;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (srgb_slot) {
        if (lc_image_view_create(env->srgb_img, &vdesc,
                                 &env->srgb_view) != LC_SUCCESS) {
            lc_image_destroy(env->srgb_img);
            env->srgb_img = NULL;
            return -1;
        }
    } else if (lc_image_view_create(env->color_img, &vdesc,
                                    &env->color_view) != LC_SUCCESS) {
        lc_image_destroy(env->color_img);
        env->color_img = NULL;
        return -1;
    }
    memset(&tdesc, 0, sizeof(tdesc));
    tdesc.width = w;
    tdesc.height = h;
    att.view = srgb_slot ? env->srgb_view : env->color_view;
    tdesc.color_attachments = &att;
    tdesc.color_attachment_count = 1;
    tdesc.depth_stencil_attachment = NULL;
    if (srgb_slot) {
        if (lc_render_target_create(env->device, &tdesc,
                                    &env->srgb_target) != LC_SUCCESS) {
            return -1;
        }
    } else if (lc_render_target_create(env->device, &tdesc,
                                       &env->target) != LC_SUCCESS) {
        return -1;
    }
    if (!srgb_slot) {
        env->tw = w;
        env->th = h;
    }
    return 0;
}

static void destroy_target(ibl_env *env) {
    lc_render_target_destroy(env->target);
    lc_image_view_destroy(env->color_view);
    lc_image_destroy(env->color_img);
    env->target = NULL;
    env->color_view = NULL;
    env->color_img = NULL;
    lc_render_target_destroy(env->srgb_target);
    lc_image_view_destroy(env->srgb_view);
    lc_image_destroy(env->srgb_img);
    env->srgb_target = NULL;
    env->srgb_view = NULL;
    env->srgb_img = NULL;
}

static void pixel_at(const unsigned char *px, uint32_t w, uint32_t x,
                     uint32_t y, double out[3]) {
    const unsigned char *p = px + ((size_t)y * w + x) * 4u;

    out[0] = (double)p[0] / 255.0;
    out[1] = (double)p[1] / 255.0;
    out[2] = (double)p[2] / 255.0;
}

static double luminance(const double c[3]) {
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

/* PBR material (all maps NULL unless given). */
static lr_material *make_pbr(ibl_env *env, const float base[4],
                             float metallic, float roughness) {
    lr_pbr_material_desc desc;
    lr_material *mat = NULL;

    memset(&desc, 0, sizeof(desc));
    memcpy(desc.base_color_factor, base, sizeof(desc.base_color_factor));
    desc.metallic_factor = metallic;
    desc.roughness_factor = roughness;
    desc.normal_scale = 1.0f;
    desc.occlusion_strength = 1.0f;
    desc.alpha_mode = LR_ALPHA_OPAQUE;
    if (lr_material_create_pbr(env->renderer, &desc, &mat) != LR_SUCCESS) {
        return NULL;
    }
    return mat;
}

static void ibl_item(lr_draw_item *item, lr_mesh *mesh, lr_material *mat,
                     float x, float y, float z) {
    memset(item, 0, sizeof(*item));
    lr_transform_identity(&item->transform);
    item->transform.position[0] = x;
    item->transform.position[1] = y;
    item->transform.position[2] = z;
    item->mesh = mesh;
    item->material = mat;
    item->casts_shadow = 1;
    item->receives_shadow = 1;
}

/* Synthetic equirectangular HDR source (R32F CPU upload, no file).
 * Row 0 = v 0 = +Y pole; u = atan2(z,x)/2pi + 0.5. */
typedef void (*equi_paint_fn)(double u, double v, float out[4]);

static int make_equirect(ibl_env *env, uint32_t w, uint32_t h,
                         equi_paint_fn paint, lc_image **out_image,
                         lc_image_view **out_view) {
    float *pixels = NULL;
    lc_image_desc idesc;
    lc_image_upload_desc upload;
    lc_image_view_desc vdesc;
    uint32_t x;
    uint32_t y;

    pixels = (float *)malloc((size_t)w * h * 4u * sizeof(float));
    if (pixels == NULL) {
        return -1;
    }
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            paint((x + 0.5) / (double)w, (double)y / (double)(h - 1),
                  pixels + ((size_t)y * w + x) * 4u);
        }
    }
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RGBA32_FLOAT;
    idesc.width = w;
    idesc.height = h;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(env->device, &idesc, out_image) != LC_SUCCESS) {
        free(pixels);
        return -1;
    }
    memset(&upload, 0, sizeof(upload));
    upload.mip_level = 0;
    upload.array_layer = 0;
    upload.width = w;
    upload.height = h;
    upload.depth = 1;
    upload.data = pixels;
    upload.data_size = (uint64_t)w * h * 4u * sizeof(float);
    if (lc_image_write(*out_image, &upload) != LC_SUCCESS) {
        free(pixels);
        lc_image_destroy(*out_image);
        *out_image = NULL;
        return -1;
    }
    free(pixels);
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(*out_image, &vdesc, out_view) !=
        LC_SUCCESS) {
        lc_image_destroy(*out_image);
        *out_image = NULL;
        return -1;
    }
    return 0;
}

static int make_env_sampler(ibl_env *env, lc_sampler **out) {
    lc_sampler_desc sdesc;

    memset(&sdesc, 0, sizeof(sdesc));
    sdesc.min_filter = LC_FILTER_LINEAR;
    sdesc.mag_filter = LC_FILTER_LINEAR;
    sdesc.mipmap_mode = LC_MIPMAP_MODE_LINEAR;
    sdesc.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
    sdesc.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
    sdesc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
    sdesc.max_anisotropy = 1.0f;
    if (lc_sampler_create(env->device, &sdesc, out) != LC_SUCCESS) {
        return -1;
    }
    return 0;
}

static lr_environment *make_env(ibl_env *env, lc_image_view *view,
                                lc_sampler *samp, float intensity,
                                float rotation) {
    lr_environment_desc desc;
    lr_environment *penv = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.environment_texture = view;
    desc.sampler = samp;
    desc.intensity = intensity;
    desc.rotation = rotation;
    if (lr_environment_create(env->renderer, &desc, &penv) !=
            LR_SUCCESS ||
        lr_renderer_set_environment(env->renderer, penv) !=
            LR_SUCCESS) {
        lr_environment_destroy(penv);
        return NULL;
    }
    return penv;
}

typedef struct ibl_frame_ops {
    lr_light *lights;
    uint32_t light_count;
    lr_draw_item *items;
    uint32_t item_count;
    float ambient[3];
    float exposure_ev;
    int set_exposure;
    lr_tonemap_operator tonemap;
    int set_tonemap;
    int use_srgb_target;
    lr_render_stats stats;
} ibl_frame_ops;

/* One full HDR frame: shadow leg + HDR scene + tonemap output leg.
 * Returns LDR pixels or NULL. */
static unsigned char *ibl_render(ibl_env *env, ibl_frame_ops *ops) {
    lc_command_encoder *enc = NULL;
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_result res;
    uint32_t i;
    unsigned char *px = NULL;

    lc_poll_events();
    /* Drain prior in-flight work: lights/camera UBOs are mapped
     * CPU memcpys (Phase-16 rule). */
    test_wait_idle(env->device);
    res = lc_begin_frame(env->swapchain);
    if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
        uint32_t w = lc_window_get_width(env->window);
        uint32_t h = lc_window_get_height(env->window);

        if (w == 0 || h == 0 ||
            lc_swapchain_recreate(env->swapchain, w, h) != LC_SUCCESS ||
            lc_begin_frame(env->swapchain) != LC_SUCCESS) {
            return NULL;
        }
    } else if (res != LC_SUCCESS) {
        return NULL;
    }
    if (lc_swapchain_get_encoder(env->swapchain, &enc) != LC_SUCCESS) {
        return NULL;
    }
    if (lr_renderer_begin(env->renderer, &env->camera) != LR_SUCCESS) {
        fprintf(stderr, "[dbg] frame: begin failed\n");
        return NULL;
    }
    for (i = 0; i < ops->light_count; i++) {
        if (lr_renderer_submit_light(env->renderer, &ops->lights[i]) !=
            LR_SUCCESS) {
            fprintf(stderr, "[dbg] frame: submit_light %u failed\n", i);
            lr_renderer_end(env->renderer);
            return NULL;
        }
    }
    for (i = 0; i < ops->item_count; i++) {
        if (lr_renderer_submit(env->renderer, &ops->items[i]) !=
            LR_SUCCESS) {
            fprintf(stderr, "[dbg] frame: submit_item %u failed\n", i);
            lr_renderer_end(env->renderer);
            return NULL;
        }
    }
    if (lr_renderer_render_shadows(env->renderer, enc) != LR_SUCCESS) {
        fprintf(stderr, "[dbg] frame: prepare failed\n");
        lr_renderer_end(env->renderer);
        return NULL;
    }
    lr_renderer_set_ambient(env->renderer, ops->ambient);
    if (ops->set_exposure &&
        lr_renderer_set_exposure(env->renderer, ops->exposure_ev) !=
            LR_SUCCESS) {
        lr_renderer_end(env->renderer);
        return NULL;
    }
    if (ops->set_tonemap &&
        lr_renderer_set_tonemap_operator(env->renderer, ops->tonemap) !=
            LR_SUCCESS) {
        lr_renderer_end(env->renderer);
        return NULL;
    }
    if (lr_renderer_render_scene(env->renderer, enc, env->tw, env->th) !=
        LR_SUCCESS) {
        fprintf(stderr, "[dbg] frame: scene failed\n");
        lr_renderer_end(env->renderer);
        return NULL;
    }
    memset(&catt, 0, sizeof(catt));
    catt.view = ops->use_srgb_target ? env->srgb_view : env->color_view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.width = env->tw;
    pdesc.height = env->th;
    pdesc.depth_attachment = NULL;
    if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
        fprintf(stderr, "[dbg] frame: output pass begin failed\n");
        lr_renderer_end(env->renderer);
        return NULL;
    }
    if (lr_renderer_render_output(
            env->renderer, enc,
            ops->use_srgb_target ? env->srgb_target : env->target) !=
            LR_SUCCESS ||
        lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        fprintf(stderr, "[dbg] frame: output render/end failed\n");
        lr_renderer_end(env->renderer);
        return NULL;
    }
    lr_renderer_get_stats(env->renderer, &ops->stats);
    lr_renderer_end(env->renderer);
    /* Swapchain leg: clear-only present (one upload per frame). */
    {
        lc_render_swapchain_pass_desc spass;

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.clear_color[0] = 0.02f;
        spass.clear_color[1] = 0.02f;
        spass.clear_color[2] = 0.03f;
        spass.clear_color[3] = 1.0f;
        spass.depth_load_op = LC_LOAD_OP_CLEAR;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        if (lc_encoder_begin_swapchain_pass(enc, env->swapchain,
                                            &spass) != LC_SUCCESS ||
            lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            return NULL;
        }
    }
    res = lc_end_frame(env->swapchain);
    if (res != LC_SUCCESS && res != LC_SUBOPTIMAL) {
        return NULL;
    }
    px = readback_rgba8(env->device,
                        ops->use_srgb_target ? env->srgb_img
                                             : env->color_img,
                        env->tw, env->th);
    return px;
}

/* Six uniquely colored faces (PART AH paint): bands centered at
 * -X(u=0/1, green), -Z(0.25, cyan), +X(0.5, red), +Z(0.75,
 * magenta); poles +Y blue / -Y yellow. Values in [0,1] so
 * tonemap-NONE passthrough reads them back nearly exactly. */
static void paint_6color(double u, double v, float out[4]) {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;

    if (v < 0.125) {
        b = 1.0f; /* +Y */
    } else if (v > 0.875) {
        r = 1.0f;
        g = 1.0f; /* -Y */
    } else if (u < 0.125 || u >= 0.875) {
        g = 1.0f; /* -X */
    } else if (u < 0.375) {
        g = 1.0f;
        b = 1.0f; /* -Z */
    } else if (u < 0.625) {
        r = 1.0f; /* +X */
    } else {
        r = 1.0f;
        b = 1.0f; /* +Z */
    }
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = 1.0f;
}

/* Aim the camera along `dir` from the origin (sky center pixel
 * then sees that direction). */
static void aim_camera(ibl_env *env, const float dir[3]) {
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    static const float origin[3] = { 0.0f, 0.0f, 0.0f };
    float center[3] = { dir[0], dir[1], dir[2] };
    float use_up[3] = { up[0], up[1], up[2] };

    if (dir[1] > 0.99f || dir[1] < -0.99f) {
        use_up[0] = 0.0f;
        use_up[1] = 0.0f;
        use_up[2] = (dir[1] > 0.0f) ? -1.0f : 1.0f;
    }
    lr_camera_init(&env->camera);
    lr_camera_set_perspective(&env->camera, 0.6f, 1.0f, 0.1f, 60.0f);
    lr_camera_look_at(&env->camera, origin, center, use_up);
}

/* ------------------------------------------------------------------
 * Main.
 * ------------------------------------------------------------------ */

/* Bright spot on black, centered at g_spot_uv (PART AG paint). */
static float g_spot_u = 0.5f;
static float g_spot_v = 0.5f;

static void paint_spot(double u, double v, float out[4]) {
    double du = u - (double)g_spot_u;
    double dv = (v - (double)g_spot_v) * 0.5;

    if (du > 0.5) {
        du -= 1.0;
    }
    if (du < -0.5) {
        du += 1.0;
    }
    if (du * du + dv * dv < 0.0016) {
        out[0] = 10.0f;
        out[1] = 10.0f;
        out[2] = 10.0f;
    } else {
        out[0] = 0.0f;
        out[1] = 0.0f;
        out[2] = 0.0f;
    }
    out[3] = 1.0f;
}

/* World point -> offscreen pixel (same matrices the GPU uses). */
static void world_to_pixel(ibl_env *env, double x, double y, double z,
                           uint32_t *out_x, uint32_t *out_y) {
    float vp[16];
    float p[4];
    float w;
    double nx;
    double ny;

    lr_mat4_multiply(vp, env->camera.projection, env->camera.view);
    p[0] = vp[0] * (float)x + vp[4] * (float)y + vp[8] * (float)z + vp[12];
    p[1] = vp[1] * (float)x + vp[5] * (float)y + vp[9] * (float)z + vp[13];
    p[3] = vp[3] * (float)x + vp[7] * (float)y + vp[11] * (float)z + vp[15];
    w = (p[3] != 0.0f) ? p[3] : 1.0f;
    nx = (double)(p[0] / w) * 0.5 + 0.5;
    ny = (double)(p[1] / w) * 0.5 + 0.5;
    if (nx < 0.0) {
        nx = 0.0;
    }
    if (nx > 1.0) {
        nx = 1.0;
    }
    if (ny < 0.0) {
        ny = 0.0;
    }
    if (ny > 1.0) {
        ny = 1.0;
    }
    *out_x = (uint32_t)(nx * (double)(env->tw - 1) + 0.5);
    *out_y = (uint32_t)(ny * (double)(env->th - 1) + 0.5);
}

/* Bright slab around +X on near-black (PART AF paint). */
static void paint_slab(double u, double v, float out[4]) {
    (void)v;
    if (u >= 0.375 && u < 0.625) {
        out[0] = 2.0f;
        out[1] = 2.0f;
        out[2] = 2.0f;
    } else {
        out[0] = 0.02f;
        out[1] = 0.02f;
        out[2] = 0.025f;
    }
    out[3] = 1.0f;
}

static void look_at_origin(ibl_env *env, float ex, float ey, float ez) {
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    float eye[3] = { ex, ey, ez };
    float center[3] = { 0.0f, 0.0f, 0.0f };

    lr_camera_init(&env->camera);
    lr_camera_set_perspective(&env->camera, 0.6f, 1.0f, 0.1f, 60.0f);
    lr_camera_look_at(&env->camera, eye, center, up);
}

/* sRGB opto-electronic conversion (CPU reference for PART V). */
static double srgb_encode(double l) {
    if (l <= 0.0) {
        return 0.0;
    }
    if (l < 0.0031308) {
        return 12.92 * l;
    }
    if (l >= 1.0) {
        return 1.0;
    }
    return 1.055 * pow(l, 1.0 / 2.4) - 0.055;
}

/* Read one HDR scene texel (R16F white-box readback). */
static int hdr_texel(ibl_env *env, uint32_t x, uint32_t y, float out[4]) {
    struct lr_renderer *ri = (struct lr_renderer *)env->renderer;
    float *px = NULL;
    int ok = 0;

    if (ri == NULL || ri->hdr_image == NULL) {
        return 0;
    }
    test_wait_idle(env->device);
    px = readback_float(env->device, ri->hdr_image, env->tw, env->th, 1);
    if (px != NULL) {
        size_t o = ((size_t)y * env->tw + x) * 4u;

        out[0] = px[o];
        out[1] = px[o + 1];
        out[2] = px[o + 2];
        out[3] = px[o + 3];
        ok = 1;
        free(px);
    }
    return ok;
}

/* Read one RG16F texel from a GPU image (white-box copy). */
static int rg16_texel(lc_device *device, lc_image *image, uint32_t w,
                      uint32_t h, uint32_t x, uint32_t y, float out[2]) {
    lc_buffer *staging = NULL;
    lc_buffer_desc bdesc;
    void *mapped = NULL;
    int ok = 0;

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = (uint64_t)w * h * 4u;
    bdesc.usage = LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_TO_CPU;
    if (lc_buffer_create(device, &bdesc, &staging) != LC_SUCCESS) {
        return 0;
    }
    test_wait_idle(device);
    if (lc_vulkan_copy_image_to_buffer(device, image, 0, 0, w, h, 1,
                                       staging->vk_buffer,
                                       0) == LC_SUCCESS &&
        lc_buffer_map(staging, &mapped) == LC_SUCCESS &&
        mapped != NULL) {
        const uint16_t *px = (const uint16_t *)mapped;

        out[0] = la_half_to_float(px[((size_t)y * w + x) * 2u]);
        out[1] = la_half_to_float(px[((size_t)y * w + x) * 2u + 1u]);
        ok = 1;
    }
    if (mapped != NULL) {
        lc_buffer_unmap(staging);
    }
    lc_buffer_destroy(staging);
    return ok;
}

/* ---- PART P18-IBL public-readback helpers ----
 * Min/max over a few texels of a borrowed view's image, using ONLY
 * public LumaC (view -> image -> lc_image_readback). Env internals
 * (field offsets) stay white-box like the rest of this file's
 * harness; no Vulkan handles, staging buffers, or layout queries. */

/* Min/max of the red channel over a strided sample of one mip/layer
 * of `image` (RGBA16F). Returns 0 on any readback failure. */
static int pub_rgba16_range(lc_image *image, uint32_t mip,
                            uint32_t layer, float *out_min,
                            float *out_max) {
    lc_image_readback_desc rd;
    lc_image_readback_info info;
    unsigned char *bytes = NULL;
    uint16_t *h16 = NULL;
    uint32_t x;
    uint32_t y;
    float lo = 1e30f;
    float hi = -1e30f;

    if (image == NULL || out_min == NULL || out_max == NULL) {
        return 0;
    }
    memset(&rd, 0, sizeof(rd));
    rd.mip_level = mip;
    rd.array_layer = layer;
    memset(&info, 0, sizeof(info));
    if (lc_image_query_readback(image, &rd, &info) != LC_SUCCESS ||
        info.size == 0) {
        return 0;
    }
    bytes = (unsigned char *)malloc(info.size);
    if (bytes == NULL) {
        return 0;
    }
    if (lc_image_readback(image, &rd, bytes, info.size, NULL) !=
        LC_SUCCESS) {
        free(bytes);
        return 0;
    }
    h16 = (uint16_t *)bytes;
    for (y = 0; y < info.height; y += 4) {
        for (x = 0; x < info.width; x += 4) {
            float v = la_half_to_float(
                h16[((size_t)y * info.width + x) * 4u]);

            if (v < lo) {
                lo = v;
            }
            if (v > hi) {
                hi = v;
            }
        }
    }
    free(bytes);
    *out_min = lo;
    *out_max = hi;
    return 1;
}

/* Min/max of RG16F channel 0 over a strided sample (BRDF LUT). */
static int pub_rg16_range(lc_image *image, float *out_min,
                          float *out_max) {
    lc_image_readback_desc rd;
    lc_image_readback_info info;
    unsigned char *bytes = NULL;
    uint16_t *h16 = NULL;
    uint32_t x;
    uint32_t y;
    float lo = 1e30f;
    float hi = -1e30f;

    if (image == NULL || out_min == NULL || out_max == NULL) {
        return 0;
    }
    memset(&rd, 0, sizeof(rd));
    memset(&info, 0, sizeof(info));
    if (lc_image_query_readback(image, &rd, &info) != LC_SUCCESS ||
        info.size == 0) {
        return 0;
    }
    bytes = (unsigned char *)malloc(info.size);
    if (bytes == NULL) {
        return 0;
    }
    if (lc_image_readback(image, &rd, bytes, info.size, NULL) !=
        LC_SUCCESS) {
        free(bytes);
        return 0;
    }
    h16 = (uint16_t *)bytes;
    for (y = 0; y < info.height; y += 8) {
        for (x = 0; x < info.width; x += 8) {
            float v = la_half_to_float(
                h16[((size_t)y * info.width + x) * 2u]);

            if (v < lo) {
                lo = v;
            }
            if (v > hi) {
                hi = v;
            }
        }
    }
    free(bytes);
    *out_min = lo;
    *out_max = hi;
    return 1;
}

static int pub_face_range(struct lr_environment *penv, uint32_t mip,
                          uint32_t layer, float *out_min,
                          float *out_max) {
    if (penv == NULL) {
        return 0;
    }
    return pub_rgba16_range(
        lc_image_view_get_image(penv->cube_view), mip, layer, out_min,
        out_max);
}

static int pub_irr_range(struct lr_environment *penv, float *out_min,
                         float *out_max) {
    if (penv == NULL) {
        return 0;
    }
    return pub_rgba16_range(
        lc_image_view_get_image(penv->irradiance_view), 0, 0, out_min,
        out_max);
}

static int pub_pref_range(struct lr_environment *penv, uint32_t mip,
                          float *out_min, float *out_max) {
    if (penv == NULL) {
        return 0;
    }
    return pub_rgba16_range(
        lc_image_view_get_image(penv->prefilter_view), mip, 0, out_min,
        out_max);
}

static int pub_brdf_range(lr_renderer *renderer, float *out_min,
                          float *out_max) {
    if (renderer == NULL) {
        return 0;
    }
    return pub_rg16_range(
        lc_image_view_get_image(lr_renderer_get_brdf_view(renderer)),
        out_min, out_max);
}

int main(void) {
    ibl_env env;
    lc_device *device = NULL;
    lc_window *window = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    int rc;
    int exit_code = 1;
    static const float black3[3] = { 0.0f, 0.0f, 0.0f };

    memset(&env, 0, sizeof(env));
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (lc_init() != LC_SUCCESS) {
        printf("SKIP: lc_init failed\n");
        return 0;
    }
    /* Cache file in the OS temp dir (PART P18-AS coverage for every
     * PART in this binary; removed before and after the run). */
    {
        const char *tmp = getenv("TEMP");

        if (tmp == NULL || tmp[0] == '\0') {
            tmp = getenv("TMPDIR");
        }
        if (tmp == NULL || tmp[0] == '\0') {
            tmp = ".";
        }
        snprintf(g_pcache_path, sizeof(g_pcache_path),
                 "%s/luma_ibl_pcache.bin", tmp);
        remove(g_pcache_path);
    }
    rc = make_device(&device);
    if (rc != 0) {
        if (rc > 0) {
            SKIP_ENV("a Vulkan device");
        }
        printf("FAIL: lc_device_create\n");
        lc_shutdown();
        return 1;
    }
    rc = make_window(&window);
    if (rc != 0) {
        lc_device_destroy(device);
        if (rc > 0) {
            SKIP_ENV("a window");
        }
        printf("FAIL: lc_window_create\n");
        lc_shutdown();
        return 1;
    }
    env.device = device;
    env.window = window;
    if (lc_surface_create(device, window, &surface) != LC_SUCCESS) {
        printf("FAIL: lc_surface_create\n");
        goto cleanup;
    }
    env.surface = surface;
    {
        lc_swapchain_desc sdesc;

        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.width = lc_window_get_width(window);
        sdesc.height = lc_window_get_height(window);
        sdesc.image_count = 0;
        sdesc.vsync = 1;
        if (lc_swapchain_create(device, surface, &sdesc, &swapchain) !=
            LC_SUCCESS) {
            printf("FAIL: lc_swapchain_create\n");
            goto cleanup;
        }
    }
    env.swapchain = swapchain;
    {
        lr_renderer_desc rdesc;

        memset(&rdesc, 0, sizeof(rdesc));
        rdesc.device = device;
        if (lc_swapchain_get_render_target_desc(
                swapchain, &rdesc.render_target) != LC_SUCCESS) {
            printf("FAIL: render target desc\n");
            goto cleanup;
        }
        rdesc.max_objects = 128;
        if (lr_renderer_create(&rdesc, &env.renderer) != LR_SUCCESS) {
            printf("FAIL: lr_renderer_create\n");
            goto cleanup;
        }
    }
    if (make_target(&env, LC_FORMAT_RGBA8_UNORM, IB_TW, IB_TH, 0) != 0) {
        printf("FAIL: offscreen target\n");
        goto cleanup;
    }
    if (make_target(&env, LC_FORMAT_RGBA8_SRGB, IB_TW, IB_TH, 1) != 0) {
        printf("FAIL: srgb target\n");
        goto cleanup;
    }
    lr_camera_init(&env.camera);
    if (lr_camera_set_perspective(&env.camera, 0.6f, 1.0f, 0.1f, 60.0f) !=
        LR_SUCCESS) {
        printf("FAIL: camera perspective\n");
        goto cleanup;
    }

    /* ---- PART AV0: environment API validation (no GPU frames) ---- */
    {
        lr_environment *null_env = NULL;
        lr_environment_desc bad;
        lr_environment_info info;
        float one = 1.0f;
        float bigv;

        memset(&bad, 0, sizeof(bad));
        TEST_CHECK(lr_environment_create(NULL, &bad, &null_env) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "env: NULL renderer rejected");
        TEST_CHECK(lr_environment_create(env.renderer, NULL,
                                         &null_env) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "env: NULL desc rejected");
        TEST_CHECK(lr_environment_create(env.renderer, &bad, NULL) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "env: NULL out rejected");
        TEST_CHECK(lr_environment_create(env.renderer, &bad,
                                         &null_env) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "env: NULL source rejected");
        TEST_CHECK(lr_environment_update(NULL, &bad) ==
                       LR_ERROR_INVALID_ARGUMENT &&
                   lr_environment_set_intensity(NULL, 1.0f) ==
                       LR_ERROR_INVALID_ARGUMENT &&
                   lr_environment_set_rotation(NULL, 0.0f) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "env: NULL-handle updates rejected");
        TEST_CHECK(lr_renderer_set_environment(NULL, NULL) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "env: NULL renderer attach rejected");
        TEST_CHECK(lr_renderer_set_environment(env.renderer, NULL) ==
                       LR_SUCCESS,
                   "env: detach NULL accepted");
        bigv = one / (one - one); /* +Inf at runtime */
        TEST_CHECK(lr_renderer_set_exposure(NULL, 0.0f) ==
                       LR_ERROR_INVALID_ARGUMENT &&
                   lr_renderer_set_exposure(env.renderer, bigv) ==
                       LR_ERROR_INVALID_ARGUMENT &&
                   lr_renderer_set_exposure(env.renderer,
                                            bigv - bigv) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "env: exposure NULL/Inf/NaN rejected");
        TEST_CHECK(lr_renderer_get_exposure(NULL) == 0.0f &&
                   lr_renderer_get_exposure(env.renderer) == 0.0f,
                   "env: exposure defaults to EV 0");
        TEST_CHECK(lr_renderer_set_tonemap_operator(
                       NULL, LR_TONEMAP_ACES) == LR_ERROR_INVALID_ARGUMENT &&
                   lr_renderer_set_tonemap_operator(
                       env.renderer,
                       (lr_tonemap_operator)7) == LR_ERROR_INVALID_ARGUMENT,
                   "env: tonemap NULL/bad-op rejected");
        TEST_CHECK(lr_renderer_get_tonemap_operator(NULL) ==
                       LR_TONEMAP_NONE &&
                   lr_renderer_get_tonemap_operator(env.renderer) ==
                       LR_TONEMAP_NONE,
                   "env: tonemap defaults to NONE");
        TEST_CHECK(lr_renderer_render_scene(NULL, NULL, 0, 0) ==
                       LR_ERROR_INVALID_ARGUMENT &&
                   lr_renderer_render_output(NULL, NULL, NULL) ==
                       LR_ERROR_INVALID_ARGUMENT &&
                   lr_renderer_get_hdr_view(NULL) == NULL,
                   "env: NULL frame calls rejected");
        lr_renderer_get_environment_info(NULL, &info);
        TEST_CHECK(info.active == 0 && info.hdr_format ==
                       LC_FORMAT_UNDEFINED,
                   "env: NULL info is zeros");
        lr_environment_destroy(NULL);
    }

    /* ---- PART AH: cubemap orientation (six colored faces) ----
     * Sky center pixels with the camera aimed along each axis must
     * read the face color (tonemap NONE passthrough). */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const struct {
            float dir[3];
            const char *name;
            double want[3];
        } faces[6] = {
            { { 1.0f, 0.0f, 0.0f }, "+X red", { 1.0, 0.0, 0.0 } },
            { { -1.0f, 0.0f, 0.0f }, "-X green", { 0.0, 1.0, 0.0 } },
            { { 0.0f, 1.0f, 0.0f }, "+Y blue", { 0.0, 0.0, 1.0 } },
            { { 0.0f, -1.0f, 0.0f }, "-Y yellow", { 1.0, 1.0, 0.0 } },
            { { 0.0f, 0.0f, 1.0f }, "+Z magenta", { 1.0, 0.0, 1.0 } },
            { { 0.0f, 0.0f, -1.0f }, "-Z cyan", { 0.0, 1.0, 1.0 } },
        };
        int i;

        if (make_equirect(&env, 64, 32, paint_6color, &eq, &eq_view) !=
                0 ||
            make_env_sampler(&env, &samp) != 0) {
            TEST_CHECK(0, "orientation: synthetic equirect uploads");
        } else {
            penv = make_env(&env, eq_view, samp, 1.0f, 0.0f);
            TEST_CHECK(penv != NULL, "orientation: environment builds");
        }
        for (i = 0; i < 6; i++) {
            ibl_frame_ops ops;
            unsigned char *px;
            double c[3];

            if (penv == NULL) {
                TEST_CHECK(0, faces[i].name);
                continue;
            }
            aim_camera(&env, faces[i].dir);
            memset(&ops, 0, sizeof(ops));
            ops.light_count = 0;
            ops.item_count = 0;
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            px = ibl_render(&env, &ops);
            if (px == NULL) {
                TEST_CHECK(0, faces[i].name);
                continue;
            }
            pixel_at(px, IB_TW, IB_TW / 2, IB_TH / 2, c);
            fprintf(stderr, "[dbg] sky %s = (%.3f,%.3f,%.3f)\n",
                    faces[i].name, c[0], c[1], c[2]);
            TEST_CHECK(fabs(c[0] - faces[i].want[0]) < 0.08 &&
                       fabs(c[1] - faces[i].want[1]) < 0.08 &&
                       fabs(c[2] - faces[i].want[2]) < 0.08,
                       faces[i].name);
            free(px);
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART AF: diffuse irradiance follows the bright
     * hemisphere ---- */
    {
        lr_mesh *ball = NULL;
        lr_material *mat = NULL;
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float gray[4] = { 0.6f, 0.6f, 0.62f, 1.0f };

        if (lr_mesh_create_sphere(env.renderer, 1.0f, 32, 16, &ball) !=
                LR_SUCCESS ||
            (mat = make_pbr(&env, gray, 0.0f, 0.5f)) == NULL) {
            TEST_CHECK(0, "irradiance: scene builds");
        } else if (make_equirect(&env, 64, 32, paint_slab, &eq,
                                   &eq_view) != 0 ||
                   make_env_sampler(&env, &samp) != 0) {
            TEST_CHECK(0, "irradiance: equirect builds");
        } else if ((penv = make_env(&env, eq_view, samp, 1.0f,
                                    0.0f)) == NULL) {
            TEST_CHECK(0, "irradiance: environment builds");
        } else {
            lr_draw_item items[1];
            ibl_frame_ops ops;
            unsigned char *px;
            uint32_t ax;
            uint32_t ay;
            uint32_t bx;
            uint32_t by;
            double ca[3];
            double cb[3];

            ibl_item(&items[0], ball, mat, 0.0f, 0.0f, 0.0f);
            look_at_origin(&env, 0.0f, 0.6f, 5.0f);
            memset(&ops, 0, sizeof(ops));
            ops.light_count = 0;
            ops.items = items;
            ops.item_count = 1;
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            /* Surface points facing +X-ish vs -X-ish. */
            world_to_pixel(&env, 0.7071, 0.0, 0.7071, &ax, &ay);
            world_to_pixel(&env, -0.7071, 0.0, 0.7071, &bx, &by);
            px = ibl_render(&env, &ops);
            if (px == NULL) {
                TEST_CHECK(0, "irradiance: frame reads back");
            } else {
                pixel_at(px, IB_TW, ax, ay, ca);
                pixel_at(px, IB_TW, bx, by, cb);
                fprintf(stderr, "[dbg] irr +X=%.4f -X=%.4f\n",
                        luminance(ca), luminance(cb));
                TEST_CHECK(luminance(ca) > luminance(cb) + 0.08,
                           "irradiance: bright side wins");
                TEST_CHECK(luminance(cb) > 0.005,
                           "irradiance: dark side still lit");
                free(px);
            }
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        if (mat != NULL) {
            lr_material_destroy(mat);
        }
        if (ball != NULL) {
            lr_mesh_destroy(ball);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART AG: specular highlight follows reflection ----
     * The spot center is computed from the exact reflection
     * direction (no margin games). */
    {
        lr_mesh *ball = NULL;
        lr_material *mat = NULL;
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        /* Camera and hit normal (mirror of the miss normal). */
        float cam[3] = { 0.0f, 0.0f, 8.0f };
        float n[3] = { 0.7071f, 0.0f, 0.7071f };
        float v[3];
        float r[3];
        float vl;
        float ndv;
        float spot_u;
        float spot_v;

        vl = sqrtf(cam[0] * cam[0] + cam[1] * cam[1] + cam[2] * cam[2]);
        v[0] = (cam[0] - n[0]) / vl;
        v[1] = (cam[1] - n[1]) / vl;
        v[2] = (cam[2] - n[2]) / vl;
        vl = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        v[0] /= vl;
        v[1] /= vl;
        v[2] /= vl;
        ndv = -(v[0] * n[0] + v[1] * n[1] + v[2] * n[2]);
        r[0] = -v[0] - 2.0f * ndv * n[0];
        r[1] = -v[1] - 2.0f * ndv * n[1];
        r[2] = -v[2] - 2.0f * ndv * n[2];
        spot_u = atan2f(r[2], r[0]) / 6.2831853f + 0.5f;
        spot_v = acosf(r[1] > 1.0f ? 1.0f : (r[1] < -1.0f ? -1.0f : r[1])) /
                 3.14159265f;
        fprintf(stderr, "[dbg] specular R=(%.3f,%.3f,%.3f) uv=(%.3f,%.3f)\n",
                r[0], r[1], r[2], spot_u, spot_v);
        if (lr_mesh_create_sphere(env.renderer, 1.0f, 32, 16, &ball) !=
                LR_SUCCESS ||
            (mat = make_pbr(&env, white, 1.0f, 0.05f)) == NULL) {
            TEST_CHECK(0, "specular: scene builds");
        } else {
            /* Paint with the computed center. */
            g_spot_u = spot_u;
            g_spot_v = spot_v;
            if (make_equirect(&env, 64, 32, paint_spot, &eq, &eq_view) !=
                    0 ||
                make_env_sampler(&env, &samp) != 0) {
                TEST_CHECK(0, "specular: equirect uploads");
            } else if ((penv = make_env(&env, eq_view, samp, 1.0f,
                                        0.0f)) == NULL) {
                TEST_CHECK(0, "specular: environment builds");
            } else {
                lr_draw_item items[1];
                ibl_frame_ops ops;
                unsigned char *px;
                uint32_t hx;
                uint32_t hy;
                uint32_t mx;
                uint32_t my;
                double ch[3];
                double cm[3];

                ibl_item(&items[0], ball, mat, 0.0f, 0.0f, 0.0f);
                look_at_origin(&env, cam[0], cam[1], cam[2]);
                memset(&ops, 0, sizeof(ops));
                ops.light_count = 0;
                ops.items = items;
                ops.item_count = 1;
                memcpy(ops.ambient, black3, sizeof(black3));
                ops.set_tonemap = 1;
                ops.tonemap = LR_TONEMAP_NONE;
                world_to_pixel(&env, n[0], n[1], n[2], &hx, &hy);
                world_to_pixel(&env, -n[0], n[1], n[2], &mx, &my);
                px = ibl_render(&env, &ops);
                if (px == NULL) {
                    TEST_CHECK(0, "specular: frame reads back");
                } else {
                    pixel_at(px, IB_TW, hx, hy, ch);
                    pixel_at(px, IB_TW, mx, my, cm);
                    fprintf(stderr, "[dbg] specular hit=%.4f miss=%.4f\n",
                            luminance(ch), luminance(cm));
                    TEST_CHECK(luminance(ch) > 0.5,
                               "specular: highlight at reflection");
                    TEST_CHECK(luminance(cm) < 0.1,
                               "specular: mirror direction dark");
                    free(px);
                }
            }
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        if (mat != NULL) {
            lr_material_destroy(mat);
        }
        if (ball != NULL) {
            lr_mesh_destroy(ball);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART J/BRDF: GPU LUT matches the CPU kernel ----
     * Values from test_ibl.c (same Hammersley/GGX formulation);
     * tolerance covers CPU/GPU float reassociation only. */
    {
        struct lr_renderer *ri = (struct lr_renderer *)env.renderer;
        float ab[2];

        if (ri == NULL || ri->brdf_image == NULL) {
            TEST_CHECK(0, "brdf: LUT image exists");
        } else {
            /* Force the build (no environment needed for BRDF). */
            test_wait_idle(device);
            if (rg16_texel(device, ri->brdf_image, 256, 256, 255, 0,
                           ab) &&
                fabsf(ab[0] - 1.0f) < 0.02f && fabsf(ab[1]) < 0.02f) {
                TEST_CHECK(1, "brdf: smooth normal incidence ~(1,0)");
            } else {
                fprintf(stderr, "[dbg] brdf(1,0) = (%.4f,%.4f)\n", ab[0],
                        ab[1]);
                TEST_CHECK(0, "brdf: smooth normal incidence ~(1,0)");
            }
            /* (NdotV, rough): u = NdotV, v = rough. */
            if (rg16_texel(device, ri->brdf_image, 256, 256, 128, 128,
                           ab) &&
                fabsf(ab[0] - 0.6537f) < 0.03f &&
                fabsf(ab[1] - 0.0154f) < 0.02f) {
                TEST_CHECK(1, "brdf: mid matches CPU kernel");
            } else {
                fprintf(stderr, "[dbg] brdf(0.5,0.5) = (%.4f,%.4f)\n",
                        ab[0], ab[1]);
                TEST_CHECK(0, "brdf: mid matches CPU kernel");
            }
        }
    }

    /* ---- PART AK/AP/AQ/AM/AN: emissive HDR ladder ----
     * Six emissive cubes (0 / 0.18 / 1 / 2 / 4 / 16) prove HDR
     * capture, ACES rolloff vs the CPU reference, NONE clamping,
     * and unclamped direct light. No lights, no env, black
     * ambient: pixels are pure emissive. */
    {
        lr_mesh *cube = NULL;
        lr_material *mats[6];
        lr_draw_item items[6];
        ibl_frame_ops ops;
        static const float levels[6] = { 0.0f, 0.18f, 1.0f,
                                         2.0f, 4.0f, 16.0f };
        /* CPU ACES reference (matches test_ibl.c exactly). */
        static const double aces_ref[6] = { 0.0, 0.266899, 0.803797,
                                            0.914855, 0.973417, 1.0 };
        int i;
        int ok = 1;

        for (i = 0; i < 6; i++) {
            mats[i] = NULL;
        }
        if (lr_mesh_create_cube(env.renderer, 0.9f, &cube) !=
            LR_SUCCESS) {
            TEST_CHECK(0, "emissive: strip mesh builds");
            ok = 0;
        }
        for (i = 0; ok && i < 6; i++) {
            lr_pbr_material_desc desc;

            memset(&desc, 0, sizeof(desc));
            desc.base_color_factor[0] = 0.02f;
            desc.base_color_factor[1] = 0.02f;
            desc.base_color_factor[2] = 0.02f;
            desc.base_color_factor[3] = 1.0f;
            desc.metallic_factor = 0.0f;
            desc.roughness_factor = 0.9f;
            desc.emissive_factor[0] = levels[i];
            desc.emissive_factor[1] = levels[i];
            desc.emissive_factor[2] = levels[i];
            desc.normal_scale = 1.0f;
            desc.occlusion_strength = 1.0f;
            desc.alpha_mode = LR_ALPHA_OPAQUE;
            if (lr_material_create_pbr(env.renderer, &desc, &mats[i]) !=
                LR_SUCCESS) {
                TEST_CHECK(0, "emissive: strip material builds");
                ok = 0;
            } else {
                ibl_item(&items[i], cube, mats[i],
                         -2.5f + (float)i, 1.0f, 0.0f);
            }
        }
        if (ok) {
            unsigned char *px;
            double got[6];

            look_at_origin(&env, 0.0f, 1.0f, 7.0f);
            memset(&ops, 0, sizeof(ops));
            ops.light_count = 0;
            ops.items = items;
            ops.item_count = 6;
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_ACES;
            px = ibl_render(&env, &ops);
            if (px == NULL) {
                TEST_CHECK(0, "emissive: ACES frame reads back");
                ok = 0;
            } else {
                for (i = 0; i < 6; i++) {
                    uint32_t sx;
                    uint32_t sy;
                    double c[3];

                    world_to_pixel(&env, -2.5 + (double)i, 1.0, 0.45,
                                   &sx, &sy);
                    pixel_at(px, IB_TW, sx, sy, c);
                    got[i] = c[0];
                }
                fprintf(stderr,
                        "[dbg] aces strips = %.3f %.3f %.3f %.3f %.3f "
                        "%.3f\n",
                        got[0], got[1], got[2], got[3], got[4], got[5]);
                for (i = 0; i < 6; i++) {
                    char msg[64];

                    snprintf(msg, sizeof(msg),
                             "tonemap: ACES matches CPU at %g", levels[i]);
                    TEST_CHECK(fabs(got[i] - aces_ref[i]) < 0.012, msg);
                }
                TEST_CHECK(got[5] > got[4],
                           "emissive: HDR rolls off monotonically");
                free(px);
            }
        }
        if (ok) {
            /* HDR pre-tonemap values (PART AK/AP/AQ readback). */
            float hv[4];
            uint32_t sx;
            uint32_t sy;
            int hok = 1;

            world_to_pixel(&env, 2.5, 1.0, 0.45, &sx, &sy);
            if (!hdr_texel(&env, sx, sy, hv) ||
                fabsf(hv[0] - 16.0f) > 0.9f) {
                hok = 0;
            }
            world_to_pixel(&env, 1.5, 1.0, 0.45, &sx, &sy);
            if (!hdr_texel(&env, sx, sy, hv) ||
                fabsf(hv[0] - 4.0f) > 0.3f) {
                hok = 0;
            }
            fprintf(stderr, "[dbg] hdr strips ok=%d\n", hok);
            TEST_CHECK(hok, "hdr: emissive stays unclamped pre-tonemap");
            /* NONE mode clamps (PART AN). */
            {
                unsigned char *px;
                double got[6];
                int i2;

                memset(&ops, 0, sizeof(ops));
                ops.light_count = 0;
                ops.items = items;
                ops.item_count = 6;
                memcpy(ops.ambient, black3, sizeof(black3));
                ops.set_tonemap = 1;
                ops.tonemap = LR_TONEMAP_NONE;
                px = ibl_render(&env, &ops);
                if (px == NULL) {
                    TEST_CHECK(0, "emissive: NONE frame reads back");
                } else {
                    int all = 1;

                    for (i2 = 0; i2 < 6; i2++) {
                        uint32_t sx2;
                        uint32_t sy2;
                        double c[3];
                        double want = levels[i2] > 1.0 ? 1.0 : levels[i2];

                        world_to_pixel(&env, -2.5 + (double)i2, 1.0,
                                       0.45, &sx2, &sy2);
                        pixel_at(px, IB_TW, sx2, sy2, c);
                        got[i2] = c[0];
                        if (fabs(c[0] - want) > 0.012) {
                            all = 0;
                        }
                    }
                    fprintf(stderr,
                            "[dbg] none strips = %.3f %.3f %.3f %.3f "
                            "%.3f %.3f\n",
                            got[0], got[1], got[2], got[3], got[4],
                            got[5]);
                    TEST_CHECK(all, "tonemap: NONE clamps to LDR");
                    free(px);
                }
            }
        }
        if (cube != NULL) {
            lr_mesh_destroy(cube);
        }
        for (i = 0; i < 6; i++) {
            if (mats[i] != NULL) {
                lr_material_destroy(mats[i]);
            }
        }
    }


    /* ---- PART AL: exposure EV -1/0/+1 monotonic ----
     * Same emissive-1.0 strip under ACES at three EVs. Flat guard
     * structure (no deep if/else nesting). */
    {
        lr_mesh *cube = NULL;
        lr_material *mat = NULL;
        lr_draw_item items[1];
        ibl_frame_ops ops;
        double got[3] = { -1.0, -1.0, -1.0 };
        int i;
        static const float evs[3] = { -1.0f, 0.0f, 1.0f };
        lr_pbr_material_desc desc;

        if (lr_mesh_create_cube(env.renderer, 0.9f, &cube) !=
            LR_SUCCESS) {
            TEST_CHECK(0, "exposure: scene builds");
        }
        if (cube != NULL) {
            memset(&desc, 0, sizeof(desc));
            desc.base_color_factor[0] = 0.02f;
            desc.base_color_factor[1] = 0.02f;
            desc.base_color_factor[2] = 0.02f;
            desc.base_color_factor[3] = 1.0f;
            desc.emissive_factor[0] = 1.0f;
            desc.emissive_factor[1] = 1.0f;
            desc.emissive_factor[2] = 1.0f;
            desc.roughness_factor = 0.9f;
            desc.normal_scale = 1.0f;
            desc.occlusion_strength = 1.0f;
            desc.alpha_mode = LR_ALPHA_OPAQUE;
            if (lr_material_create_pbr(env.renderer, &desc, &mat) !=
                LR_SUCCESS) {
                TEST_CHECK(0, "exposure: emissive material builds");
            }
        }
        if (cube != NULL && mat != NULL) {
            ibl_item(&items[0], cube, mat, 0.0f, 1.0f, 0.0f);
            look_at_origin(&env, 0.0f, 1.0f, 7.0f);
            for (i = 0; i < 3; i++) {
                unsigned char *px;
                uint32_t sx;
                uint32_t sy;
                double c[3];

                memset(&ops, 0, sizeof(ops));
                ops.light_count = 0;
                ops.items = items;
                ops.item_count = 1;
                memcpy(ops.ambient, black3, sizeof(black3));
                ops.set_exposure = 1;
                ops.exposure_ev = evs[i];
                ops.set_tonemap = 1;
                ops.tonemap = LR_TONEMAP_ACES;
                world_to_pixel(&env, 0.0, 1.0, 0.45, &sx, &sy);
                px = ibl_render(&env, &ops);
                if (px == NULL) {
                    TEST_CHECK(0, "exposure: frame reads back");
                } else {
                    pixel_at(px, IB_TW, sx, sy, c);
                    got[i] = c[0];
                    free(px);
                }
            }
            fprintf(stderr, "[dbg] exposure ev-1/0/+1 = %.3f %.3f %.3f\n",
                    got[0], got[1], got[2]);
            TEST_CHECK(got[0] > 0.3 && got[0] < got[1] &&
                       got[1] < got[2] && got[2] <= 1.0,
                       "exposure: EV steps monotonic in ACES");
            /* Back to EV 0 for later blocks. */
            lr_renderer_set_exposure(env.renderer, 0.0f);
        }
        if (mat != NULL) {
            lr_material_destroy(mat);
        }
        if (cube != NULL) {
            lr_mesh_destroy(cube);
        }
    }

    /* ---- PART O: environment intensity 0/0.5/1/2 ----
     * Zero must remove IBL (direct/emissive remain); intensity
     * changes must not reprocess (rebuild counter stable). */
    {
        lr_mesh *ball = NULL;
        lr_material *mat = NULL;
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float gray[4] = { 0.6f, 0.6f, 0.62f, 1.0f };
        static const float levels[4] = { 0.0f, 0.5f, 1.0f, 2.0f };
        double got[4];
        uint32_t rebuilds0 = 0;
        uint32_t rebuilds1 = 0;
        int i;

        if (lr_mesh_create_sphere(env.renderer, 1.0f, 32, 16, &ball) !=
                LR_SUCCESS ||
            (mat = make_pbr(&env, gray, 0.0f, 0.5f)) == NULL ||
            make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) !=
                0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) ==
                NULL) {
            TEST_CHECK(0, "intensity: scene builds");
        } else {
            lr_draw_item items[1];
            ibl_frame_ops ops;

            ibl_item(&items[0], ball, mat, 0.0f, 0.0f, 0.0f);
            look_at_origin(&env, 0.0f, 0.6f, 5.0f);
            for (i = 0; i < 4; i++) {
                unsigned char *px;
                uint32_t sx;
                uint32_t sy;
                double c[3];

                if (lr_environment_set_intensity(penv, levels[i]) !=
                    LR_SUCCESS) {
                    TEST_CHECK(0, "intensity: set accepted");
                    got[i] = -1.0;
                    continue;
                }
                memset(&ops, 0, sizeof(ops));
                ops.light_count = 0;
                ops.items = items;
                ops.item_count = 1;
                memcpy(ops.ambient, black3, sizeof(black3));
                ops.set_tonemap = 1;
                ops.tonemap = LR_TONEMAP_NONE;
                world_to_pixel(&env, 0.7071, 0.0, 0.7071, &sx, &sy);
                px = ibl_render(&env, &ops);
                if (i == 1) {
                    rebuilds0 = ops.stats.environment_rebuilds;
                }
                if (i == 3) {
                    rebuilds1 = ops.stats.environment_rebuilds;
                }
                if (px == NULL) {
                    TEST_CHECK(0, "intensity: frame reads back");
                    got[i] = -1.0;
                } else {
                    pixel_at(px, IB_TW, sx, sy, c);
                    got[i] = luminance(c);
                    free(px);
                }
            }
            fprintf(stderr, "[dbg] intensity 0/.5/1/2 = %.4f %.4f %.4f %.4f "
                            "(rebuilds %u->%u)\n",
                    got[0], got[1], got[2], got[3], rebuilds0,
                    rebuilds1);
            TEST_CHECK(got[0] < 0.01,
                       "intensity: zero removes IBL");
            TEST_CHECK(got[1] > got[0] && got[2] > got[1] &&
                       got[3] > got[2],
                       "intensity: response monotonic");
            TEST_CHECK(rebuilds0 == 0 && rebuilds1 == 0,
                       "intensity: no reprocessing on parameter change");
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        if (mat != NULL) {
            lr_material_destroy(mat);
        }
        if (ball != NULL) {
            lr_mesh_destroy(ball);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART V: sRGB output encodes exactly once ----
     * Same linear emissive to UNORM (raw) and sRGB (encoded)
     * targets under tonemap NONE. */
    {
        lr_mesh *cube = NULL;
        lr_material *mat = NULL;
        lr_draw_item items[1];
        ibl_frame_ops ops;
        double lin = -1.0;
        double enc = -1.0;

        if (lr_mesh_create_cube(env.renderer, 0.9f, &cube) !=
            LR_SUCCESS) {
            TEST_CHECK(0, "srgb: scene builds");
        } else {
            lr_pbr_material_desc desc;

            memset(&desc, 0, sizeof(desc));
            desc.base_color_factor[0] = 0.02f;
            desc.base_color_factor[1] = 0.02f;
            desc.base_color_factor[2] = 0.02f;
            desc.base_color_factor[3] = 1.0f;
            desc.emissive_factor[0] = 0.18f;
            desc.emissive_factor[1] = 0.18f;
            desc.emissive_factor[2] = 0.18f;
            desc.roughness_factor = 0.9f;
            desc.normal_scale = 1.0f;
            desc.occlusion_strength = 1.0f;
            desc.alpha_mode = LR_ALPHA_OPAQUE;
            if (lr_material_create_pbr(env.renderer, &desc, &mat) !=
                LR_SUCCESS) {
                TEST_CHECK(0, "srgb: emissive material builds");
            } else {
                unsigned char *px;
                uint32_t sx;
                uint32_t sy;
                double c[3];

                ibl_item(&items[0], cube, mat, 0.0f, 1.0f, 0.0f);
                look_at_origin(&env, 0.0f, 1.0f, 7.0f);
                world_to_pixel(&env, 0.0, 1.0, 0.45, &sx, &sy);
                memset(&ops, 0, sizeof(ops));
                ops.light_count = 0;
                ops.items = items;
                ops.item_count = 1;
                memcpy(ops.ambient, black3, sizeof(black3));
                ops.set_tonemap = 1;
                ops.tonemap = LR_TONEMAP_NONE;
                px = ibl_render(&env, &ops);
                if (px != NULL) {
                    pixel_at(px, IB_TW, sx, sy, c);
                    lin = c[0];
                    free(px);
                }
                ops.use_srgb_target = 1;
                px = ibl_render(&env, &ops);
                if (px != NULL) {
                    pixel_at(px, IB_TW, sx, sy, c);
                    enc = c[0];
                    free(px);
                }
                fprintf(stderr, "[dbg] srgb linear=%.4f encoded=%.4f\n",
                        lin, enc);
                TEST_CHECK(fabs(lin - 0.18) < 0.012,
                           "srgb: UNORM target stays linear");
                TEST_CHECK(fabs(enc - srgb_encode(0.18)) < 0.012,
                           "srgb: sRGB target encodes exactly once");
            }
        }
        if (mat != NULL) {
            lr_material_destroy(mat);
        }
        if (cube != NULL) {
            lr_mesh_destroy(cube);
        }
    }

    /* ---- PART AO: shadows gate direct, spare IBL ---- */
    {
        lr_mesh *ground = NULL;
        lr_mesh *cube = NULL;
        lr_material *ground_mat = NULL;
        lr_material *cube_mat = NULL;
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        lr_light light;
        lr_draw_item items[2];
        ibl_frame_ops ops;
        static const float gray[4] = { 0.6f, 0.6f, 0.62f, 1.0f };
        uint32_t cx;
        uint32_t cy;
        uint32_t lx;
        uint32_t ly;
        double shadowed[3] = { -1.0, -1.0, -1.0 };
        double lit[3] = { -1.0, -1.0, -1.0 };
        double ctrl[3] = { -1.0, -1.0, -1.0 };

        memset(&light, 0, sizeof(light));
        light.type = LR_LIGHT_DIRECTIONAL;
        light.color[0] = 1.0f;
        light.color[1] = 1.0f;
        light.color[2] = 1.0f;
        light.intensity = 3.0f;
        light.direction[0] = 0.5f;
        light.direction[1] = -1.0f;
        light.direction[2] = 0.2f;
        light.shadow.enabled = 1;
        light.shadow.resolution = 1024;
        light.shadow.depth_bias = -1.0f;
        light.shadow.normal_bias = -1.0f;
        if (lr_mesh_create_plane(env.renderer, 12.0f, 12.0f, &ground) !=
                LR_SUCCESS ||
            lr_mesh_create_cube(env.renderer, 2.0f, &cube) !=
                LR_SUCCESS ||
            (ground_mat = make_pbr(&env, gray, 0.0f, 0.8f)) == NULL ||
            (cube_mat = make_pbr(&env, gray, 0.0f, 0.8f)) == NULL ||
            make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) !=
                0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) ==
                NULL) {
            TEST_CHECK(0, "shadow-ibl: scene builds");
        } else {
            ibl_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
            ibl_item(&items[1], cube, cube_mat, 0.0f, 1.5f, 0.0f);
            look_at_origin(&env, 0.0f, 4.0f, 9.0f);
            memset(&ops, 0, sizeof(ops));
            ops.lights = &light;
            ops.light_count = 1;
            ops.items = items;
            ops.item_count = 2;
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            world_to_pixel(&env, 1.7, 0.01, -0.2, &cx, &cy);
            world_to_pixel(&env, 1.7, 0.01, 1.6, &lx, &ly);
            {
                unsigned char *px = ibl_render(&env, &ops);

                if (px != NULL) {
                    pixel_at(px, IB_TW, cx, cy, shadowed);
                    pixel_at(px, IB_TW, lx, ly, lit);
                    free(px);
                }
            }
            /* Control: same frame without environment. */
            lr_renderer_set_environment(env.renderer, NULL);
            {
                unsigned char *px = ibl_render(&env, &ops);

                if (px != NULL) {
                    double c[3];

                    pixel_at(px, IB_TW, cx, cy, c);
                    ctrl[0] = c[0];
                    ctrl[1] = c[1];
                    ctrl[2] = c[2];
                    free(px);
                }
            }
            fprintf(stderr,
                    "[dbg] shadowibl sh=%.4f lit=%.4f noenv=%.4f\n",
                    luminance(shadowed), luminance(lit),
                    luminance(ctrl));
            TEST_CHECK(luminance(shadowed) > 0.05,
                       "shadow-ibl: IBL survives inside shadow");
            TEST_CHECK(luminance(shadowed) < luminance(lit) * 0.5,
                       "shadow-ibl: shadow still gates direct");
            TEST_CHECK(luminance(ctrl) < 0.01,
                       "shadow-ibl: no-env control is black");
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        if (cube_mat != NULL) {
            lr_material_destroy(cube_mat);
        }
        if (ground_mat != NULL) {
            lr_material_destroy(ground_mat);
        }
        if (cube != NULL) {
            lr_mesh_destroy(cube);
        }
        if (ground != NULL) {
            lr_mesh_destroy(ground);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART AC/AD/AE: metallic/roughness grid under IBL ----
     * 2x2 spheres (metal 0/1 x rough 0.1/0.9), no direct lights:
     * per-sphere mean/stddev statistics over an interior window.
     * Thresholds calibrated from measured data with wide margins;
     * values print every run for drift detection. */
    {
        lr_mesh *ball = NULL;
        lr_material *mats[4];
        lr_draw_item items[4];
        ibl_frame_ops ops;
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float gray[4] = { 0.6f, 0.6f, 0.62f, 1.0f };
        static const float copper[4] = { 0.9f, 0.5f, 0.25f, 1.0f };
        static const float mets[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        static const float rghs[4] = { 0.1f, 0.9f, 0.1f, 0.9f };
        /* Four disjoint spheres (no overlap: depthless target
         * cannot resolve cover). */
        static const float spx[4] = { -1.2f, 1.2f, -1.2f, 1.2f };
        static const float spy[4] = { 0.6f, 0.6f, 2.0f, 2.0f };
        int i;
        int ok = 1;
        double means[4] = { 0.0, 0.0, 0.0, 0.0 };
        double sds[4] = { 0.0, 0.0, 0.0, 0.0 };
        double diel_rb = 0.0;
        double metal_rb = 0.0;

        for (i = 0; i < 4; i++) {
            mats[i] = NULL;
        }
        if (lr_mesh_create_sphere(env.renderer, 0.5f, 32, 16, &ball) !=
                LR_SUCCESS ||
            make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) !=
                0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) ==
                NULL) {
            TEST_CHECK(0, "grid: scene builds");
            ok = 0;
        }
        for (i = 0; ok && i < 4; i++) {
            if ((mats[i] = make_pbr(&env, i < 2 ? gray : copper,
                                    mets[i], rghs[i])) == NULL) {
                TEST_CHECK(0, "grid: sphere material builds");
                ok = 0;
            } else {
                ibl_item(&items[i], ball, mats[i], spx[i], spy[i], 0.0f);
            }
        }
        if (ok) {
            unsigned char *px;

            look_at_origin(&env, 0.0f, 1.0f, 6.0f);
            memset(&ops, 0, sizeof(ops));
            ops.light_count = 0;
            ops.items = items;
            ops.item_count = 4;
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            px = ibl_render(&env, &ops);
            if (px == NULL) {
                TEST_CHECK(0, "grid: frame reads back");
            } else {
                for (i = 0; i < 4; i++) {
                    double lo = 1.0;
                    double hi = 0.0;
                    double sum = 0.0;
                    double sum2 = 0.0;
                    double hir = 0.0;
                    double hib = 0.0;
                    int n = 0;
                    int k;

                    /* Horizontal scan across the visible disk. */
                    for (k = -4; k <= 4; k++) {
                        double wx = (double)spx[i] + 0.125 * (double)k;
                        uint32_t sx;
                        uint32_t sy;
                        double c[3];
                        double l;

                        world_to_pixel(&env, wx, (double)spy[i], 0.0,
                                       &sx, &sy);
                        pixel_at(px, IB_TW, sx, sy, c);
                        l = luminance(c);
                        if (l < lo) {
                            lo = l;
                        }
                        if (l > hi) {
                            hi = l;
                            hir = c[0];
                            hib = c[2];
                        }
                        sum += l;
                        sum2 += l * l;
                        n++;
                    }
                    means[i] = sum / (double)n;
                    sds[i] = hi - lo;
                    fprintf(stderr,
                            "[dbg] grid s%d lo=%.3f hi=%.3f mean=%.3f "
                            "hiRB=(%.2f,%.2f)\n",
                            i, lo, hi, means[i], hir, hib);
                    if (i == 0) {
                        diel_rb = hir - hib;
                    }
                    if (i == 2) {
                        metal_rb = hir - hib;
                    }
                }
                free(px);
            }
            fprintf(stderr,
                    "[dbg] grid means = %.3f %.3f %.3f %.3f\n"
                    "[dbg] grid sds   = %.3f %.3f %.3f %.3f\n",
                    means[0], means[1], means[2], means[3], sds[0],
                    sds[1], sds[2], sds[3]);
            fprintf(stderr,
                    "[dbg] grid stats draws=%u pbr=%u ibl=%u rebuilds=%u "
                    "sky=%u tm=%u\n",
                    ops.stats.draw_calls, ops.stats.pbr_draw_calls,
                    ops.stats.ibl_enabled, ops.stats.environment_rebuilds,
                    ops.stats.sky_draw_calls, ops.stats.tonemap_passes);
            /* idx: 0=diel smooth, 1=diel rough, 2=metal smooth,
             * 3=metal rough. sds[] holds disk contrast (hi-lo). */
            TEST_CHECK(sds[0] > sds[1] && sds[2] > sds[3] * 1.5,
                       "grid: smooth varies more than rough");
            TEST_CHECK(metal_rb > 0.25 && diel_rb < 0.2,
                       "grid: metals reflect tinted, dielectrics neutral");
            TEST_CHECK(means[2] > 0.05 && means[3] > 0.02,
                       "grid: metals reflect environment");
            TEST_CHECK(means[0] > 0.05 && means[1] > 0.05,
                       "grid: IBL lights dielectrics without direct");
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        for (i = 0; i < 4; i++) {
            if (mats[i] != NULL) {
                lr_material_destroy(mats[i]);
            }
        }
        if (ball != NULL) {
            lr_mesh_destroy(ball);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART AI: yaw rotation moves lighting AND sky ---- */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float px_dir[3] = { 1.0f, 0.0f, 0.0f };
        ibl_frame_ops ops;
        unsigned char *px = NULL;
        unsigned char *px2 = NULL;
        double c0[3] = { 0.0, 0.0, 0.0 };
        double c1[3] = { 0.0, 0.0, 0.0 };
        uint32_t rb0 = 0;
        uint32_t rb1 = 0;
        lr_environment_info info;

        memset(&info, 0, sizeof(info));
        if (make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) == NULL) {
            TEST_CHECK(0, "rotation: environment builds");
        } else {
            aim_camera(&env, px_dir);
            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            px = ibl_render(&env, &ops);
            if (px == NULL) {
                TEST_CHECK(0, "rotation: unrotated frame reads back");
            } else {
                pixel_at(px, env.tw, env.tw / 2, env.th / 2, c0);
                rb0 = ops.stats.environment_rebuilds;
                free(px);
                px = NULL;
            }
            if (lr_environment_set_rotation(
                    penv, 3.14159265f) != LR_SUCCESS) {
                TEST_CHECK(0, "rotation: yaw setter accepts pi");
            }
            px2 = ibl_render(&env, &ops);
            if (px2 == NULL) {
                TEST_CHECK(0, "rotation: rotated frame reads back");
            } else {
                pixel_at(px2, env.tw, env.tw / 2, env.th / 2, c1);
                rb1 = ops.stats.environment_rebuilds;
                free(px2);
                px2 = NULL;
            }
            lr_renderer_get_environment_info(env.renderer, &info);
            TEST_CHECK(luminance(c0) > 0.9 && luminance(c1) < 0.1,
                       "rotation: pi yaw swaps bright slab for dark sky");
            TEST_CHECK(rb0 == 1 && rb1 == 0,
                       "rotation: params-only, no rebuild");
            TEST_CHECK(info.rotation > 3.14f && info.rotation < 3.15f &&
                           info.active != 0,
                       "rotation: info echoes yaw and active");
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART AJ: sky samples the base cube (no geometry) ---- */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float up[3] = { 0.0f, 1.0f, 0.0f };
        static const float down[3] = { 0.0f, -1.0f, 0.0f };
        static const float east[3] = { 1.0f, 0.0f, 0.0f };
        ibl_frame_ops ops;
        double cu[3] = { 0.0, 0.0, 0.0 };
        double cd[3] = { 0.0, 0.0, 0.0 };
        double ce[3] = { 0.0, 0.0, 0.0 };
        uint32_t sky_draws = 0;

        if (make_equirect(&env, 64, 32, paint_6color, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) == NULL) {
            TEST_CHECK(0, "sky: environment builds");
        } else {
            static const float *dirs[3] = { up, down, east };
            double *outs[3] = { cu, cd, ce };
            int d;

            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            for (d = 0; d < 3; d++) {
                unsigned char *px;

                aim_camera(&env, dirs[d]);
                px = ibl_render(&env, &ops);
                if (px == NULL) {
                    TEST_CHECK(0, "sky: frame reads back");
                    break;
                }
                pixel_at(px, env.tw, env.tw / 2, env.th / 2, outs[d]);
                sky_draws = ops.stats.sky_draw_calls;
                free(px);
            }
            TEST_CHECK(cu[2] > 0.9 && cu[0] < 0.1 && cu[1] < 0.1,
                       "sky: +Y pole renders blue");
            TEST_CHECK(cd[0] > 0.9 && cd[1] > 0.9 && cd[2] < 0.1,
                       "sky: -Y pole renders yellow");
            TEST_CHECK(ce[0] > 0.9 && ce[1] < 0.1 && ce[2] < 0.1,
                       "sky: +X renders red");
            TEST_CHECK(sky_draws == 1, "sky: exactly one sky draw");
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART BB: descriptor replacement + env swap ---- */
    {
        lc_image *slab = NULL;
        lc_image_view *slab_view = NULL;
        lc_image *color = NULL;
        lc_image_view *color_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        lr_environment *penv2 = NULL;
        static const float px_dir[3] = { 1.0f, 0.0f, 0.0f };
        ibl_frame_ops ops;
        unsigned char *px = NULL;
        double before[3] = { 0.0, 0.0, 0.0 };
        double after[3] = { 0.0, 0.0, 0.0 };
        double swapped[3] = { 0.0, 0.0, 0.0 };

        if (make_equirect(&env, 64, 32, paint_slab, &slab, &slab_view) !=
                0 ||
            make_equirect(&env, 64, 32, paint_6color, &color,
                          &color_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, slab_view, samp, 1.0f, 0.0f)) == NULL) {
            TEST_CHECK(0, "replacement: environment builds");
        } else {
            lr_environment_desc desc;

            aim_camera(&env, px_dir);
            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            px = ibl_render(&env, &ops);
            if (px != NULL) {
                pixel_at(px, env.tw, env.tw / 2, env.th / 2, before);
                free(px);
                px = NULL;
            }
            memset(&desc, 0, sizeof(desc));
            desc.environment_texture = color_view;
            desc.sampler = samp;
            desc.intensity = 1.0f;
            desc.rotation = 0.0f;
            if (lr_environment_update(penv, &desc) != LR_SUCCESS) {
                TEST_CHECK(0, "replacement: update accepts new source");
            }
            px = ibl_render(&env, &ops);
            if (px != NULL) {
                pixel_at(px, env.tw, env.tw / 2, env.th / 2, after);
                free(px);
                px = NULL;
            }
            TEST_CHECK(luminance(before) > 0.9 && after[0] > 0.9 &&
                           after[1] < 0.1 && after[2] < 0.1,
                       "replacement: update swaps slab for red sky");
            /* Swap the whole object: renderer survives env churn. */
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            penv = NULL;
            test_wait_idle(device);
            penv2 = make_env(&env, color_view, samp, 1.0f, 0.0f);
            px = (penv2 != NULL) ? ibl_render(&env, &ops) : NULL;
            if (px != NULL) {
                pixel_at(px, env.tw, env.tw / 2, env.th / 2, swapped);
                free(px);
                px = NULL;
            }
            TEST_CHECK(swapped[0] > 0.9 && swapped[1] < 0.1,
                       "replacement: fresh env on same renderer renders");
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        if (penv2 != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv2);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(slab_view);
        lc_image_destroy(slab);
        lc_image_view_destroy(color_view);
        lc_image_destroy(color);
    }

    /* ---- PART BC: HDR target follows the scene extent ---- */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float px_dir[3] = { 1.0f, 0.0f, 0.0f };
        ibl_frame_ops ops;
        lr_environment_info big;
        lr_environment_info small;
        uint32_t rb_big = 0;
        uint32_t rb_small = 0;

        memset(&big, 0, sizeof(big));
        memset(&small, 0, sizeof(small));
        if (make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) == NULL) {
            TEST_CHECK(0, "resize: environment builds");
        } else {
            unsigned char *px;

            aim_camera(&env, px_dir);
            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            px = ibl_render(&env, &ops);
            if (px != NULL) {
                free(px);
            }
            lr_renderer_get_environment_info(env.renderer, &big);
            rb_big = ops.stats.environment_rebuilds;
            destroy_target(&env);
            if (make_target(&env, LC_FORMAT_RGBA8_UNORM, 128, 128, 0) !=
                0) {
                TEST_CHECK(0, "resize: small target builds");
            } else {
                px = ibl_render(&env, &ops);
                if (px != NULL) {
                    free(px);
                }
                lr_renderer_get_environment_info(env.renderer, &small);
                rb_small = ops.stats.environment_rebuilds;
                TEST_CHECK(big.hdr_width == 256 && big.hdr_height == 256,
                           "resize: HDR matches 256 scene");
                TEST_CHECK(small.hdr_width == 128 &&
                               small.hdr_height == 128,
                           "resize: HDR follows 128 scene");
                TEST_CHECK(rb_big == 1 && rb_small == 0,
                           "resize: no env reprocess on resize");
                destroy_target(&env);
                if (make_target(&env, LC_FORMAT_RGBA8_UNORM, IB_TW, IB_TH,
                                0) != 0) {
                    TEST_CHECK(0, "resize: restore 256 target");
                }
            }
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART BD: sequential views share one preprocessing ---- */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float px_dir[3] = { 1.0f, 0.0f, 0.0f };
        static const float nx_dir[3] = { -1.0f, 0.0f, 0.0f };
        ibl_frame_ops ops;
        unsigned char *px = NULL;
        double ce[3] = { 0.0, 0.0, 0.0 };
        double cw[3] = { 0.0, 0.0, 0.0 };

        if (make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) == NULL) {
            TEST_CHECK(0, "multiview: environment builds");
        } else {
            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            aim_camera(&env, px_dir);
            px = ibl_render(&env, &ops);
            if (px != NULL) {
                pixel_at(px, env.tw, env.tw / 2, env.th / 2, ce);
                free(px);
                px = NULL;
            }
            aim_camera(&env, nx_dir);
            px = ibl_render(&env, &ops);
            if (px != NULL) {
                pixel_at(px, env.tw, env.tw / 2, env.th / 2, cw);
                free(px);
                px = NULL;
            }
            TEST_CHECK(luminance(ce) > 0.9 && luminance(cw) < 0.1,
                       "multiview: each view sees its own direction");
            TEST_CHECK(ops.stats.environment_rebuilds == 0,
                       "multiview: second view reuses preprocessing");
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART AV: info introspection mirrors live state ---- */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float px_dir[3] = { 1.0f, 0.0f, 0.0f };
        ibl_frame_ops ops;
        lr_environment_info live;
        lr_environment_info detached;
        unsigned char *px;

        memset(&live, 0, sizeof(live));
        memset(&detached, 0, sizeof(detached));
        if (make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 0.5f, 0.25f)) == NULL) {
            TEST_CHECK(0, "info: environment builds");
        } else {
            aim_camera(&env, px_dir);
            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_exposure = 1;
            ops.exposure_ev = 1.0f;
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_ACES;
            px = ibl_render(&env, &ops);
            if (px != NULL) {
                free(px);
            }
            lr_renderer_get_environment_info(env.renderer, &live);
            lr_renderer_set_environment(env.renderer, NULL);
            px = ibl_render(&env, &ops);
            if (px != NULL) {
                free(px);
            }
            lr_renderer_get_environment_info(env.renderer, &detached);
            TEST_CHECK(live.active != 0 && live.intensity > 0.49f &&
                           live.intensity < 0.51f &&
                           live.rotation > 0.24f && live.rotation < 0.26f,
                       "info: active/intensity/rotation echo");
            TEST_CHECK(live.exposure_ev > 0.99f &&
                           live.exposure_ev < 1.01f &&
                           live.tonemap == LR_TONEMAP_ACES,
                       "info: exposure/tonemap echo");
            TEST_CHECK(live.hdr_format != LC_FORMAT_UNDEFINED &&
                           live.hdr_width == IB_TW &&
                           live.preprocess_state == 1 &&
                           live.environment_generation > 0 &&
                           live.environment_rebuilds >= 1,
                       "info: hdr target and build state sane");
            TEST_CHECK(detached.active == 0,
                       "info: detach clears active flag");
            /* Leave detached for the shared cleanup below. */
        }
        if (penv != NULL) {
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART BA: endurance (steady frames, rotating yaw) ---- */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        lr_mesh *ball = NULL;
        lr_material *mat = NULL;
        static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        ibl_frame_ops ops;
        int f;
        int frames_ok = 0;
        uint32_t rb_first = 0;
        uint32_t rb_last = 0;

        if (make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) == NULL ||
            lr_mesh_create_sphere(env.renderer, 1.0f, 24, 12, &ball) !=
                LR_SUCCESS ||
            (mat = make_pbr(&env, white, 0.0f, 0.5f)) == NULL) {
            TEST_CHECK(0, "endurance: scene builds");
        } else {
            lr_draw_item items[1];

            ibl_item(&items[0], ball, mat, 0.0f, 0.0f, 0.0f);
            look_at_origin(&env, 0.0f, 0.5f, 6.0f);
            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.items = items;
            ops.item_count = 1;
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            for (f = 0; f < 150; f++) {
                unsigned char *px;

                if ((f % 10) == 0 &&
                    lr_environment_set_rotation(
                        penv, 0.1f * (float)f) != LR_SUCCESS) {
                    break;
                }
                px = ibl_render(&env, &ops);
                if (px == NULL) {
                    break;
                }
                free(px);
                frames_ok++;
                if (f == 0) {
                    rb_first = ops.stats.environment_rebuilds;
                }
                rb_last = ops.stats.environment_rebuilds;
            }
            TEST_CHECK(frames_ok == 150,
                       "endurance: 150 frames render without failure");
            TEST_CHECK(rb_first == 1 && rb_last == 0,
                       "endurance: yaw churn never reprocesses");
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        if (mat != NULL) {
            lr_material_destroy(mat);
        }
        if (ball != NULL) {
            lr_mesh_destroy(ball);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART BE: output records into passes carrying depth ----
     * Regression: the tonemap pipeline used to key depth=UNDEFINED,
     * which recorded INCOMPATIBLE into swapchain-style passes that
     * always carry depth (found by examples/ibl_scene). */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        lc_image *dimg = NULL;
        lc_image_view *dview = NULL;
        lc_image *cimg = NULL;
        lc_image_view *cview = NULL;
        lc_render_target *dtarget = NULL;
        static const float px_dir[3] = { 1.0f, 0.0f, 0.0f };
        unsigned char *px = NULL;
        double c[3] = { 0.0, 0.0, 0.0 };

        if (make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) == NULL) {
            TEST_CHECK(0, "depthpass: environment builds");
        } else {
            lc_image_desc idesc;
            lc_image_view_desc vdesc;
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment atts[1];
            lc_command_encoder *enc = NULL;
            lc_render_pass_desc pdesc;
            lc_render_color_attachment catt;
            lc_render_depth_attachment datt;
            lr_result opr = LR_ERROR_RENDER;

            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = IB_TW;
            idesc.height = IB_TH;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                          LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                          LC_IMAGE_USAGE_TRANSFER_SRC |
                          LC_IMAGE_USAGE_TRANSFER_DST;
            idesc.samples = LC_SAMPLE_COUNT_1;
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.mip_level_count = 1;
            vdesc.array_layer_count = 1;
            if (lc_image_create(device, &idesc, &cimg) != LC_SUCCESS ||
                lc_image_view_create(cimg, &vdesc, &cview) !=
                    LC_SUCCESS) {
                TEST_CHECK(0, "depthpass: color target builds");
            } else {
                memset(&idesc, 0, sizeof(idesc));
                idesc.type = LC_IMAGE_TYPE_2D;
                idesc.format = LC_FORMAT_D32_FLOAT;
                idesc.width = IB_TW;
                idesc.height = IB_TH;
                idesc.depth = 1;
                idesc.mip_levels = 1;
                idesc.array_layers = 1;
                idesc.usage =
                    (uint32_t)LC_IMAGE_USAGE_DEPTH_STENCIL;
                idesc.samples = LC_SAMPLE_COUNT_1;
                memset(&vdesc, 0, sizeof(vdesc));
                vdesc.type = LC_IMAGE_VIEW_2D;
                vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
                vdesc.mip_level_count = 1;
                vdesc.array_layer_count = 1;
                if (lc_image_create(device, &idesc, &dimg) !=
                        LC_SUCCESS ||
                    lc_image_view_create(dimg, &vdesc, &dview) !=
                        LC_SUCCESS) {
                    TEST_CHECK(0, "depthpass: depth builds");
                } else {
                    memset(&tdesc, 0, sizeof(tdesc));
                    tdesc.width = IB_TW;
                    tdesc.height = IB_TH;
                    atts[0].view = cview;
                    tdesc.color_attachments = atts;
                    tdesc.color_attachment_count = 1;
                    tdesc.depth_stencil_attachment = dview;
                    if (lc_render_target_create(device, &tdesc,
                                                &dtarget) != LC_SUCCESS) {
                        TEST_CHECK(0, "depthpass: target builds");
                    } else {
                        ibl_frame_ops ops;

                        aim_camera(&env, px_dir);
                        lc_poll_events();
                        test_wait_idle(device);
                        if (lc_begin_frame(env.swapchain) ==
                                LC_SUCCESS &&
                            lc_swapchain_get_encoder(env.swapchain,
                                                     &enc) ==
                                LC_SUCCESS &&
                            lr_renderer_begin(env.renderer,
                                              &env.camera) == LR_SUCCESS) {
                            memset(&ops, 0, sizeof(ops));
                            memcpy(ops.ambient, black3,
                                   sizeof(black3));
                            ops.set_tonemap = 1;
                            ops.tonemap = LR_TONEMAP_NONE;
                            lr_renderer_set_ambient(env.renderer,
                                                    ops.ambient);
                            if (lr_renderer_render_shadows(
                                    env.renderer, enc) == LR_SUCCESS &&
                                lr_renderer_render_scene(
                                    env.renderer, enc, IB_TW,
                                    IB_TH) == LR_SUCCESS) {
                                memset(&catt, 0, sizeof(catt));
                                catt.view = cview;
                                catt.load_op = LC_LOAD_OP_CLEAR;
                                catt.store_op = LC_STORE_OP_STORE;
                                memset(&pdesc, 0, sizeof(pdesc));
                                pdesc.color_attachments = &catt;
                                pdesc.color_attachment_count = 1;
                                pdesc.width = IB_TW;
                                pdesc.height = IB_TH;
                                memset(&datt, 0, sizeof(datt));
                                datt.view = dview;
                                datt.depth_load_op = LC_LOAD_OP_CLEAR;
                                datt.depth_store_op =
                                    LC_STORE_OP_DONT_CARE;
                                datt.clear_depth = 1.0f;
                                datt.stencil_load_op =
                                    LC_LOAD_OP_DONT_CARE;
                                datt.stencil_store_op =
                                    LC_STORE_OP_DONT_CARE;
                                pdesc.depth_attachment = &datt;
                                if (lc_encoder_begin_render_pass(
                                        enc, &pdesc) == LC_SUCCESS) {
                                    opr = lr_renderer_render_output(
                                        env.renderer, enc, dtarget);
                                    if (lc_encoder_end_render_pass(
                                            enc) != LC_SUCCESS) {
                                        opr = LR_ERROR_RENDER;
                                    }
                                }
                            }
                            lr_renderer_end(env.renderer);
                        }
                        if (opr == LR_SUCCESS) {
                            lc_result er = lc_end_frame(env.swapchain);

                            if (er == LC_SUCCESS ||
                                er == LC_SUBOPTIMAL) {
                                px = readback_rgba8(device, cimg, IB_TW,
                                                    IB_TH);
                            } else {
                                opr = LR_ERROR_RENDER;
                            }
                        } else {
                            /* Keep the swapchain frame balanced. */
                            lc_end_frame(env.swapchain);
                        }
                        TEST_CHECK(opr == LR_SUCCESS,
                                   "depthpass: output records with depth");
                        if (px != NULL) {
                            pixel_at(px, IB_TW, IB_TW / 2, IB_TH / 2, c);
                            free(px);
                            px = NULL;
                        }
                        TEST_CHECK(luminance(c) > 0.9,
                                   "depthpass: sky pixels survive depth");
                    }
                }
            }
        }
        lc_render_target_destroy(dtarget);
        lc_image_view_destroy(dview);
        lc_image_destroy(dimg);
        lc_image_view_destroy(cview);
        lc_image_destroy(cimg);
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART P18-POST: tint stage proves the chain ----
     * Identity tint is a byte-exact no-op (chain OFF vs ON/identity);
     * a 2x-red tint doubles red through intermediate -> tonemap;
     * post_passes and the CPU profile observe the stage. */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        /* Public-readback scratch (allocated per use, freed). */
        unsigned char *pub8 = NULL;
        float *pubf = NULL;
        static const float px_dir[3] = { 1.0f, 0.0f, 0.0f };
        unsigned char *off = NULL;
        unsigned char *ident = NULL;
        unsigned char *tinted = NULL;
        double c_off[3] = { 0.0, 0.0, 0.0 };
        double c_ident[3] = { 0.0, 0.0, 0.0 };
        double c_tint[3] = { 0.0, 0.0, 0.0 };
        ibl_frame_ops ops;
        lr_frame_profile prof;
        lr_frame_diagnostics diag;

        memset(&prof, 0, sizeof(prof));
        memset(&diag, 0, sizeof(diag));
        if (make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) == NULL) {
            TEST_CHECK(0, "post: environment builds");
        } else {
            static const float red2[3] = { 2.0f, 1.0f, 1.0f };
            lr_frame_profile prof_tint;

            aim_camera(&env, px_dir);
            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            TEST_CHECK(lr_renderer_set_post_stage(env.renderer,
                                                  LR_POST_NONE) ==
                           LR_SUCCESS,
                       "post: default stage accepted");
            TEST_CHECK(lr_renderer_get_post_stage(env.renderer) ==
                           LR_POST_NONE,
                       "post: default stage reads NONE");
            off = ibl_render(&env, &ops);
            TEST_CHECK(off != NULL, "post: chain-off frame renders");
            TEST_CHECK(lr_renderer_set_post_stage(
                           env.renderer, LR_POST_TINT_VERIFY) ==
                           LR_SUCCESS &&
                           lr_renderer_set_post_tint(env.renderer,
                                                     black3) ==
                               LR_SUCCESS,
                       "post: tint stage + black tint accepted");
            /* Black tint would zero the scene; restore identity. */
            {
                static const float ident3[3] = { 1.0f, 1.0f, 1.0f };

                TEST_CHECK(lr_renderer_set_post_tint(env.renderer,
                                                     ident3) ==
                               LR_SUCCESS,
                           "post: identity tint accepted");
            }
            ident = ibl_render(&env, &ops);
            TEST_CHECK(ident != NULL, "post: identity frame renders");
            lr_renderer_get_frame_profile(env.renderer, &prof);
            TEST_CHECK(prof.post_passes == 1,
                       "post: identity records one post pass");
            TEST_CHECK(lr_renderer_set_post_tint(env.renderer, red2) ==
                           LR_SUCCESS,
                       "post: 2x-red tint accepted");
            tinted = ibl_render(&env, &ops);
            TEST_CHECK(tinted != NULL, "post: tinted frame renders");
            lr_renderer_get_frame_profile(env.renderer, &prof_tint);
            if (off != NULL) {
                pixel_at(off, IB_TW, IB_TW / 2, IB_TH / 2, c_off);
            }
            if (ident != NULL) {
                pixel_at(ident, IB_TW, IB_TW / 2, IB_TH / 2, c_ident);
            }
            if (tinted != NULL) {
                pixel_at(tinted, IB_TW, IB_TW / 2, IB_TH / 2, c_tint);
            }
            TEST_CHECK(off != NULL && ident != NULL &&
                           c_off[0] == c_ident[0] &&
                           c_off[1] == c_ident[1] &&
                           c_off[2] == c_ident[2],
                       "post: identity tint is byte-exact no-op");
            /* Slab center is white (1,1,1): 2x red pre-tonemap still
             * clamps red to 1 but must not touch green/blue. Green
             * and blue survive identically; red stays saturated. */
            TEST_CHECK(tinted != NULL && c_tint[1] == c_off[1] &&
                           c_tint[2] == c_off[2] && c_tint[0] >= 0.99,
                       "post: tint routes through the chain");
            TEST_CHECK(prof_tint.post_passes == 1 &&
                           prof_tint.cpu_post_ms >= 0.0 &&
                           prof_tint.cpu_total_ms > 0.0 &&
                           prof_tint.frame_number > prof.frame_number,
                       "post: profile observes stage + frames advance");
            lr_renderer_get_frame_diagnostics(env.renderer, &diag);
            TEST_CHECK(diag.frame_number == prof_tint.frame_number &&
                           diag.post_passes == 1 &&
                           diag.ibl_active != 0 &&
                           diag.viewport_width == IB_TW &&
                           diag.viewport_height == IB_TH &&
                           diag.cpu_total_ms > 0.0,
                       "post: diagnostics snapshot is coherent");
            TEST_CHECK(lr_renderer_set_post_stage(env.renderer, 99) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "post: bogus stage rejected");
            TEST_CHECK(lr_renderer_set_post_tint(env.renderer, NULL) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "post: NULL tint rejected");
            /* Leave the chain OFF for later PARTs. */
            lr_renderer_set_post_stage(env.renderer, LR_POST_NONE);
            free(off);
            free(ident);
            free(tinted);
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART P18-CAP: public HDR + LDR capture ----
     * lr_renderer_capture_hdr reads floats > 1.0 through public
     * readback (replaces white-box HDR verification); the output
     * target image reads back through lc_image_readback with the
     * slab center saturating white. */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float px_dir[3] = { 1.0f, 0.0f, 0.0f };
        unsigned char *px = NULL;
        float *hdr = NULL;
        size_t need = 0;
        ibl_frame_ops ops;

        if (make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) == NULL) {
            TEST_CHECK(0, "capture: environment builds");
        } else {
            lc_image_readback_desc rd;
            unsigned char *ldr = NULL;
            size_t ldr_need = 0;

            aim_camera(&env, px_dir);
            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            px = ibl_render(&env, &ops);
            TEST_CHECK(px != NULL, "capture: frame renders");
            free(px);
            px = NULL;
            /* HDR sizing query first (public API, no pixels yet). */
            TEST_CHECK(lr_renderer_capture_hdr(env.renderer, NULL, 0,
                                               &need) == LR_SUCCESS &&
                           need == (size_t)IB_TW * IB_TH * 8u,
                       "capture: HDR size query reports RGBA16F");
            hdr = (float *)malloc(need);
            TEST_CHECK(hdr != NULL, "capture: HDR scratch allocates");
            if (hdr != NULL) {
                /* Read back as float: the helper below converts
                 * half->float per texel for numeric asserts. */
                size_t half_need = 0;
                unsigned char *halfp = NULL;
                lc_image *himage = NULL;

                TEST_CHECK(lr_renderer_capture_hdr(
                               env.renderer, NULL, 0, &half_need) ==
                               LR_SUCCESS,
                           "capture: HDR sizing is repeatable");
                halfp = (unsigned char *)malloc(half_need);
                TEST_CHECK(halfp != NULL,
                           "capture: half scratch allocates");
                if (halfp != NULL) {
                    uint16_t *h16 = NULL;
                    double center = 0.0;
                    uint32_t cx = IB_TW / 2;
                    uint32_t cy = IB_TH / 2;

                    TEST_CHECK(lr_renderer_capture_hdr(
                                   env.renderer, halfp, half_need,
                                   NULL) == LR_SUCCESS,
                               "capture: HDR pixels read publicly");
                    h16 = (uint16_t *)halfp;
                    /* Sky slab center exceeds 1.0 pre-tonemap. */
                    center = (double)la_half_to_float(
                        h16[((size_t)cy * IB_TW + cx) * 4u]);
                    TEST_CHECK(center > 1.0,
                               "capture: HDR center exceeds 1.0");
                    free(halfp);
                }
                /* LDR output target through plain public readback. */
                memset(&rd, 0, sizeof(rd));
                himage = env.color_img;
                {
                    lc_image_readback_info info;

                    memset(&info, 0, sizeof(info));
                    TEST_CHECK(lc_image_query_readback(himage, &rd,
                                                       &info) ==
                                   LC_SUCCESS &&
                                   info.width == IB_TW &&
                                   info.height == IB_TH &&
                                   info.format ==
                                       LC_FORMAT_RGBA8_UNORM &&
                                   info.row_pitch == (size_t)IB_TW * 4u,
                               "capture: LDR layout is tight RGBA8");
                    ldr = (unsigned char *)malloc(info.size);
                    if (ldr != NULL) {
                        double lc_[3];
                        unsigned char *ldr2 = NULL;

                        TEST_CHECK(lc_image_readback(himage, &rd, ldr,
                                                     info.size,
                                                     &ldr_need) ==
                                       LC_SUCCESS,
                                   "capture: LDR pixels read publicly");
                        pixel_at(ldr, IB_TW, IB_TW / 2, IB_TH / 2, lc_);
                        TEST_CHECK(luminance(lc_) > 0.9,
                                   "capture: LDR center saturates");
                        /* Symmetric renderer helper reads identically. */
                        ldr2 = (unsigned char *)malloc(info.size);
                        if (ldr2 != NULL) {
                            TEST_CHECK(
                                lr_renderer_capture_output(
                                    env.renderer, env.target, ldr2,
                                    info.size, NULL) == LR_SUCCESS &&
                                    memcmp(ldr, ldr2, info.size) == 0,
                                "capture: output helper matches");
                            free(ldr2);
                        }
                        free(ldr);
                    }
                }
                free(hdr);
            }
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

        /* ---- PART P18-IBL: derived maps via public readback ----
     * Cube face, irradiance face, prefilter mip, and BRDF LUT read
     * through borrowed view -> image -> lc_image_readback only.
     * Asserts meaningful non-zero variation (no Vulkan internals). */
        {
            lc_image *eq = NULL;
            lc_image_view *eq_view = NULL;
            lc_sampler *samp = NULL;
            lr_environment *penv = NULL;
            unsigned char *px = NULL;
            ibl_frame_ops ops;
            float vmin = 0.0f;
            float vmax = 0.0f;
            int got_cube = 0;
            int got_irr = 0;
            int got_pref = 0;
            int got_brdf = 0;

            if (make_equirect(&env, 64, 32, paint_6color, &eq,
                              &eq_view) != 0 ||
                make_env_sampler(&env, &samp) != 0 ||
                (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) ==
                    NULL) {
                TEST_CHECK(0, "iblpub: environment builds");
            } else {
                static const float px_dir[3] = { 1.0f, 0.0f, 0.0f };

                aim_camera(&env, px_dir);
                memset(&ops, 0, sizeof(ops));
                memcpy(ops.ambient, black3, sizeof(black3));
                ops.set_tonemap = 1;
                ops.tonemap = LR_TONEMAP_NONE;
                px = ibl_render(&env, &ops);
                TEST_CHECK(px != NULL, "iblpub: frame renders");
                free(px);
                px = NULL;
                /* Base cube +X face (layer 0) must show the red slab
                 * band of the 6-color source. */
                got_cube = pub_face_range(penv, 0, 0, &vmin, &vmax);
                TEST_CHECK(got_cube && vmax > 0.9f,
                           "iblpub: cube +X face carries source");
                /* Irradiance layer 0 varies (6-color convolution is
                 * never flat). */
                got_irr = pub_irr_range(penv, &vmin, &vmax);
                TEST_CHECK(got_irr && (vmax - vmin) > 0.05f,
                           "iblpub: irradiance face varies");
                /* Prefilter mip 2 still varies (blurred but alive). */
                got_pref = pub_pref_range(penv, 2, &vmin, &vmax);
                TEST_CHECK(got_pref && (vmax - vmin) > 0.01f,
                           "iblpub: prefilter mip varies");
                /* BRDF LUT (renderer-global) spans dark->bright. */
                got_brdf = pub_brdf_range(env.renderer, &vmin, &vmax);
                TEST_CHECK(got_brdf && vmin < 0.1f && vmax > 0.9f,
                           "iblpub: BRDF LUT spans range");
            }
            if (penv != NULL) {
                lr_renderer_set_environment(env.renderer, NULL);
                lr_environment_destroy(penv);
                penv = NULL;
                test_wait_idle(device);
            }
            lc_sampler_destroy(samp);
            lc_image_view_destroy(eq_view);
            lc_image_destroy(eq);
        }

    /* ---- PART P18-AS/AI: 500-frame endurance + multiview ----
     * Full stack (PBR + shadowed light + IBL + HDR + tonemap + post)
     * for 1000 frames: camera orbit, ball motion, env yaw, moving
     * shadow light, mid-run resize (256->128->256), tint-identity
     * post for a stretch, public-readback captures every 100 frames
     * (the ONLY readbacks in the run — PART AR), cache enabled for
     * the whole binary. Then two orientations must differ (AI). */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        lr_mesh *ball = NULL;
        lr_material *mat = NULL;
        lc_buffer *stream_buffer = NULL;
        lc_gpu_signal stream_done = {0};
        static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
        static const float ident3[3] = { 1.0f, 1.0f, 1.0f };
        ibl_frame_ops ops;
        lr_draw_item items[1];
        lr_light light;
        int f;
        int frames_ok = 0;
        double cap_lum[10];
        int cap_n = 0;
        uint32_t pipes_warm = 0;
        uint32_t pipes_end = 0;
        lr_environment_info info;
        lr_environment_info info_base;

        memset(&info, 0, sizeof(info));
        memset(&info_base, 0, sizeof(info_base));
        if (make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) == NULL ||
            lr_mesh_create_sphere(env.renderer, 1.0f, 24, 12, &ball) !=
                LR_SUCCESS ||
            (mat = make_pbr(&env, white, 0.0f, 0.5f)) == NULL) {
            TEST_CHECK(0, "endurance1000: scene builds");
        } else {
            lc_buffer_desc stream_desc;

            memset(&stream_desc, 0, sizeof(stream_desc));
            stream_desc.size = 4096;
            stream_desc.usage = LC_BUFFER_USAGE_STORAGE;
            stream_desc.memory = LC_MEMORY_GPU_ONLY;
            if (lc_buffer_create(env.device, &stream_desc,
                                 &stream_buffer) != LC_SUCCESS) {
                TEST_CHECK(0, "endurance1000: streaming buffer creates");
            }
            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.items = items;
            ops.item_count = 1;
            ops.lights = &light;
            ops.light_count = 1;
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            memset(&light, 0, sizeof(light));
            light.type = LR_LIGHT_DIRECTIONAL;
            light.color[0] = 1.0f;
            light.color[1] = 1.0f;
            light.color[2] = 1.0f;
            light.intensity = 3.0f;
            light.direction[0] = 0.5f;
            light.direction[1] = -1.0f;
            light.direction[2] = 0.2f;
            light.shadow.enabled = 1;
            light.shadow.resolution = 512;
            /* Lifetime baseline: this renderer is shared by all
             * PARTs, so the counter already holds earlier builds. */
            lr_renderer_get_environment_info(env.renderer, &info_base);
            for (f = 0; f < 1000 && stream_buffer != NULL; f++) {
                float a = 0.02f * (float)f;
                float ca = cosf(a);
                float sa = sinf(a);
                unsigned char *px = NULL;
                uint32_t stream_words[64];
                lc_buffer *temporary = NULL;
                lc_buffer_desc temporary_desc;

                memset(stream_words, f, sizeof(stream_words));
                if (lc_upload_buffer_async(env.device, stream_buffer, 0,
                                           stream_words,
                                           sizeof(stream_words),
                                           &stream_done) != LC_SUCCESS) {
                    break;
                }
                /* Independent short-lived resources model streaming metadata
                 * churn while rendering remains in flight. */
                if ((f % 10) == 0) {
                    memset(&temporary_desc, 0, sizeof(temporary_desc));
                    temporary_desc.size = 1024;
                    temporary_desc.usage = LC_BUFFER_USAGE_UNIFORM;
                    temporary_desc.memory = LC_MEMORY_CPU_TO_GPU;
                    if (lc_buffer_create(env.device, &temporary_desc,
                                         &temporary) != LC_SUCCESS) {
                        break;
                    }
                    lc_buffer_destroy(temporary);
                }

                /* Motion on every axis the phase owns. */
                look_at_origin(&env, 6.0f * ca, 0.5f + 0.2f * sa,
                               6.0f * sa);
                ibl_item(&items[0], ball, mat, 1.5f * ca, 0.0f,
                         1.5f * sa);
                light.direction[0] = 0.5f * ca + 0.2f * sa;
                light.direction[2] = -0.5f * sa + 0.2f * ca;
                if ((f % 10) == 0 &&
                    lr_environment_set_rotation(
                        penv, 0.05f * (float)f) != LR_SUCCESS) {
                    break;
                }
                /* Tint-identity post for frames 100-199 (chaining
                 * under load; byte-exact no-op per P18-POST). */
                if (f == 100) {
                    if (lr_renderer_set_post_stage(
                            env.renderer,
                            LR_POST_TINT_VERIFY) != LR_SUCCESS ||
                        lr_renderer_set_post_tint(env.renderer,
                                                  ident3) !=
                            LR_SUCCESS) {
                        break;
                    }
                }
                if (f == 200) {
                    if (lr_renderer_set_post_stage(env.renderer,
                                                   LR_POST_NONE) !=
                        LR_SUCCESS) {
                        break;
                    }
                }
                /* Mid-run resize and back (AG: HDR/post refit, env
                 * and pipelines stay). */
                if (f == 250) {
                    destroy_target(&env);
                    if (make_target(&env, LC_FORMAT_RGBA8_UNORM, 128,
                                    128, 0) != 0) {
                        break;
                    }
                }
                if (f == 300) {
                    destroy_target(&env);
                    if (make_target(&env, LC_FORMAT_RGBA8_UNORM, IB_TW,
                                    IB_TH, 0) != 0) {
                        break;
                    }
                }
                px = ibl_render(&env, &ops);
                if (px == NULL) {
                    break;
                }
                free(px);
                frames_ok++;
                if (f == 10) {
                    pipes_warm = lr_renderer_get_pipeline_count(
                        env.renderer);
                }
                /* Requested captures only (PART AR): every 100. */
                if ((f % 100) == 0 && cap_n < 10) {
                    lc_image_readback_desc rd;
                    lc_image_readback_info ri;
                    unsigned char *cap = NULL;
                    double sum = 0.0;
                    uint32_t n = 0;
                    uint32_t x;
                    uint32_t y;

                    memset(&rd, 0, sizeof(rd));
                    memset(&ri, 0, sizeof(ri));
                    if (lc_image_query_readback(env.color_img, &rd,
                                                &ri) == LC_SUCCESS &&
                        ri.size > 0) {
                        cap = (unsigned char *)malloc(ri.size);
                        if (cap != NULL &&
                            lc_image_readback(env.color_img, &rd, cap,
                                              ri.size,
                                              NULL) == LC_SUCCESS) {
                            for (y = ri.height / 2 - 1;
                                 y <= ri.height / 2 + 1; y++) {
                                for (x = ri.width / 2 - 1;
                                     x <= ri.width / 2 + 1; x++) {
                                    double c[3];

                                    pixel_at(cap, ri.width, x, y, c);
                                    sum += luminance(c);
                                    n++;
                                }
                            }
                        }
                        free(cap);
                    }
                    cap_lum[cap_n++] = (n > 0) ? sum / (double)n : -1.0;
                }
            }
            pipes_end =
                lr_renderer_get_pipeline_count(env.renderer);
            lr_renderer_get_environment_info(env.renderer, &info);
            if (stream_done.value != 0) {
                (void)lc_gpu_signal_wait(env.device, stream_done,
                                         LC_TIMEOUT_INFINITE);
            }
            TEST_CHECK(frames_ok == 1000,
                       "endurance1000: 1000 frames with streaming");
            TEST_CHECK(info.environment_rebuilds ==
                           info_base.environment_rebuilds + 1,
                       "endurance1000: env builds once, never reprocesses");
            {
                double lo = cap_lum[0];
                double hi = cap_lum[0];
                int i;

                for (i = 1; i < cap_n; i++) {
                    if (cap_lum[i] < lo) {
                        lo = cap_lum[i];
                    }
                    if (cap_lum[i] > hi) {
                        hi = cap_lum[i];
                    }
                }
                /* Ten captures at 100-frame intervals. Bounds
                 * are validity (not black, not over 1.0 LDR) plus
                 * motion (range across orbit positions). */
                TEST_CHECK(cap_n == 10 && lo >= 0.01 && hi <= 1.0 &&
                               (hi - lo) > 0.03,
                           "endurance1000: captures sane and vary");
            }
            TEST_CHECK(pipes_warm > 0 &&
                           pipes_end == pipes_warm &&
                           pipes_end <= 16,
                       "endurance1000: pipelines bounded and stable");
            {
                lc_transfer_stats transfer_stats;
                lc_memory_stats memory_stats;

                lc_device_poll_completed(env.device);
                memset(&transfer_stats, 0, sizeof(transfer_stats));
                memset(&memory_stats, 0, sizeof(memory_stats));
                lc_device_get_transfer_stats(env.device, &transfer_stats);
                lc_device_get_memory_stats(env.device, &memory_stats);
                TEST_CHECK(transfer_stats.uploads_in_flight == 0 &&
                               transfer_stats.staging_used == 0,
                           "endurance1000: streaming staging fully reclaimed");
                printf("[info] endurance1000 uploads=%llu bytes=%llu "
                       "submissions=%llu staging_high=%llu retired_high=%llu "
                       "blocks=%llu\n",
                       (unsigned long long)1000,
                       (unsigned long long)transfer_stats.bytes_uploaded,
                       (unsigned long long)transfer_stats.transfer_submissions,
                       (unsigned long long)transfer_stats.staging_high_water,
                       (unsigned long long)transfer_stats.retired_high_water,
                       (unsigned long long)memory_stats.block_count);
            }
            /* AI: two orientations through public readback differ. */
            {
                static const float pxd[3] = { 1.0f, 0.0f, 0.0f };
                static const float nxd[3] = { -1.0f, 0.0f, 0.0f };
                unsigned char *pa = NULL;
                unsigned char *pb = NULL;
                double ca[3] = { 0.0, 0.0, 0.0 };
                double cb[3] = { 0.0, 0.0, 0.0 };

                ops.item_count = 0;
                ops.light_count = 0;
                aim_camera(&env, pxd);
                pa = ibl_render(&env, &ops);
                aim_camera(&env, nxd);
                pb = ibl_render(&env, &ops);
                if (pa != NULL) {
                    pixel_at(pa, env.tw, env.tw / 2, env.th / 2, ca);
                    free(pa);
                }
                if (pb != NULL) {
                    pixel_at(pb, env.tw, env.tw / 2, env.th / 2, cb);
                    free(pb);
                }
                TEST_CHECK(luminance(ca) - luminance(cb) > 0.5,
                           "multiview: orientations differ publicly");
            }
            lr_renderer_set_post_stage(env.renderer, LR_POST_NONE);
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        if (mat != NULL) {
        lc_buffer_destroy(stream_buffer);
        lr_material_destroy(mat);
        }
        if (ball != NULL) {
            lr_mesh_destroy(ball);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART P19-MEM: no per-frame GPU allocations ----
     * Sky-only frames after warmup: allocator used bytes and block
     * count must be identical (PART 17). */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float px_dir[3] = { 1.0f, 0.0f, 0.0f };
        ibl_frame_ops ops;
        lc_memory_stats warm;
        lc_memory_stats end;
        int f;
        int ok = 1;

        memset(&warm, 0, sizeof(warm));
        memset(&end, 0, sizeof(end));
        if (make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) == NULL) {
            TEST_CHECK(0, "memstable: environment builds");
        } else {
            aim_camera(&env, px_dir);
            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            for (f = 0; f < 60; f++) {
                unsigned char *px = ibl_render(&env, &ops);

                if (px == NULL) {
                    ok = 0;
                    break;
                }
                free(px);
                if (f == 5) {
                    lc_device_get_memory_stats(device, &warm);
                }
            }
            lc_device_get_memory_stats(device, &end);
            TEST_CHECK(ok, "memstable: 60 frames render");
            TEST_CHECK(
                end.device_local_used == warm.device_local_used &&
                    end.host_visible_used == warm.host_visible_used &&
                    end.block_count == warm.block_count &&
                    end.allocation_count == warm.allocation_count,
                "memstable: zero allocation churn after warmup");
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART P19-CHURN (AQ): per-frame temp resources ----
     * Each frame creates and destroys buffers + images while
     * rendering continues; allocator must stay bounded. */
    {
        lc_image *eq = NULL;
        lc_image_view *eq_view = NULL;
        lc_sampler *samp = NULL;
        lr_environment *penv = NULL;
        static const float px_dir[3] = { 1.0f, 0.0f, 0.0f };
        ibl_frame_ops ops;
        lc_memory_stats warm;
        lc_memory_stats end;
        int f;
        int ok = 1;

        memset(&warm, 0, sizeof(warm));
        memset(&end, 0, sizeof(end));
        if (make_equirect(&env, 64, 32, paint_slab, &eq, &eq_view) != 0 ||
            make_env_sampler(&env, &samp) != 0 ||
            (penv = make_env(&env, eq_view, samp, 1.0f, 0.0f)) == NULL) {
            TEST_CHECK(0, "churn: environment builds");
        } else {
            int w;

            /* Warmup renders first (env preprocessing allocates
             * its persistent images once); snapshot after. */
            for (w = 0; w < 3; w++) {
                unsigned char *wp = NULL;

                aim_camera(&env, px_dir);
                memset(&ops, 0, sizeof(ops));
                memcpy(ops.ambient, black3, sizeof(black3));
                ops.set_tonemap = 1;
                ops.tonemap = LR_TONEMAP_NONE;
                wp = ibl_render(&env, &ops);
                if (wp == NULL) {
                    ok = 0;
                    break;
                }
                free(wp);
            }
            lc_device_get_memory_stats(device, &warm);
            aim_camera(&env, px_dir);
            memset(&ops, 0, sizeof(ops));
            memcpy(ops.ambient, black3, sizeof(black3));
            ops.set_tonemap = 1;
            ops.tonemap = LR_TONEMAP_NONE;
            for (f = 0; f < 60 && ok; f++) {
                lc_buffer *tmpb[10];
                lc_image *tmpi[3];
                int i;
                unsigned char *px = NULL;

                for (i = 0; i < 10; i++) {
                    lc_buffer_desc bd;

                    memset(&bd, 0, sizeof(bd));
                    bd.size = 256 + (uint64_t)(i * 64);
                    bd.usage = LC_BUFFER_USAGE_UNIFORM;
                    bd.memory = LC_MEMORY_GPU_ONLY;
                    tmpb[i] = NULL;
                    if (lc_buffer_create(device, &bd, &tmpb[i]) !=
                        LC_SUCCESS) {
                        ok = 0;
                    }
                }
                for (i = 0; i < 3 && ok; i++) {
                    lc_image_desc idesc;

                    memset(&idesc, 0, sizeof(idesc));
                    idesc.type = LC_IMAGE_TYPE_2D;
                    idesc.format = LC_FORMAT_RGBA8_UNORM;
                    idesc.width = 64;
                    idesc.height = 64;
                    idesc.depth = 1;
                    idesc.mip_levels = 1;
                    idesc.array_layers = 1;
                    idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                                  LC_IMAGE_USAGE_TRANSFER_DST;
                    idesc.samples = LC_SAMPLE_COUNT_1;
                    tmpi[i] = NULL;
                    if (lc_image_create(device, &idesc, &tmpi[i]) !=
                        LC_SUCCESS) {
                        ok = 0;
                    }
                }
                px = ok ? ibl_render(&env, &ops) : NULL;
                if (px == NULL) {
                    ok = 0;
                }
                free(px);
                for (i = 0; i < 10; i++) {
                    lc_buffer_destroy(tmpb[i]);
                }
                for (i = 0; i < 3; i++) {
                    lc_image_destroy(tmpi[i]);
                }
            }
            lc_device_get_memory_stats(device, &end);
            TEST_CHECK(ok, "churn: 60 frames with temp resources");
            printf("[info] churn: warm blocks=%llu allocs=%llu | "
                   "end blocks=%llu allocs=%llu\n",
                   (unsigned long long)warm.block_count,
                   (unsigned long long)warm.allocation_count,
                   (unsigned long long)end.block_count,
                   (unsigned long long)end.allocation_count);
            TEST_CHECK(end.block_count <= warm.block_count + 2 &&
                           end.allocation_count ==
                               warm.allocation_count,
                       "churn: allocator stays bounded");
        }
        if (penv != NULL) {
            lr_renderer_set_environment(env.renderer, NULL);
            lr_environment_destroy(penv);
            test_wait_idle(device);
        }
        lc_sampler_destroy(samp);
        lc_image_view_destroy(eq_view);
        lc_image_destroy(eq);
    }

    /* ---- PART P19-AR: imported asset load/unload ----
     * Load fixture.glb (meshes/materials/textures/samplers),
     * record GPU memory before/after, destroy model + manager,
     * expect full return to the allocator. */
    {
        la_asset_manager *mgr = NULL;
        la_asset_manager_desc mdesc;
        la_model *model = NULL;
        char path[512];
        lc_memory_stats before;
        lc_memory_stats loaded;
        lc_memory_stats after;
        int n;

        memset(&before, 0, sizeof(before));
        memset(&loaded, 0, sizeof(loaded));
        memset(&after, 0, sizeof(after));
        memset(&mdesc, 0, sizeof(mdesc));
        mdesc.renderer = env.renderer;
        n = snprintf(path, sizeof(path), "%s/fixture.glb",
                     LA_FIXTURE_DIR);
        TEST_CHECK(n > 0 && (size_t)n < sizeof(path),
                   "asset: fixture path fits");
        if (n > 0 && (size_t)n < sizeof(path) &&
            la_asset_manager_create(&mdesc, &mgr) == LA_SUCCESS) {
            lc_device_get_memory_stats(device, &before);
            if (la_model_load(mgr, path, &model) == LA_SUCCESS) {
                lc_device_get_memory_stats(device, &loaded);
                TEST_CHECK(la_model_get_mesh_count(model) > 0 &&
                               la_model_get_material_count(model) > 0 &&
                               la_model_get_texture_count(model) > 0,
                           "asset: model has meshes/materials/"
                           "textures");
                printf("[info] asset: meshes=%u materials=%u "
                       "textures=%u dev_used=%llu\n",
                       la_model_get_mesh_count(model),
                       la_model_get_material_count(model),
                       la_model_get_texture_count(model),
                       (unsigned long long)(loaded.device_local_used -
                                            before.device_local_used));
                TEST_CHECK(loaded.device_local_used >
                               before.device_local_used,
                           "asset: load allocates GPU memory");
                la_model_destroy(model);
                model = NULL;
            } else {
                TEST_CHECK(0, "asset: fixture loads");
            }
            la_asset_manager_destroy(mgr);
            mgr = NULL;
            lc_device_get_memory_stats(device, &after);
            TEST_CHECK(after.device_local_used ==
                               before.device_local_used &&
                           after.allocation_count ==
                               before.allocation_count,
                       "asset: unload returns all GPU memory");
        } else {
            TEST_CHECK(0, "asset: manager creates");
        }
    }

    printf("ibl vulkan: %d passed, %d failed\n", g_passed,
           g_failed);
    exit_code = (g_failed == 0) ? 0 : 1;

cleanup:
    test_wait_idle(device);
    destroy_target(&env);
    lr_renderer_destroy(env.renderer);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    /* Cache report (PART P18-AS): file must exist after a cached
     * run; then remove it so reruns start cold-safe. */
    {
        FILE *cf = fopen(g_pcache_path, "rb");
        long cn = -1;

        if (cf != NULL) {
            if (fseek(cf, 0, SEEK_END) == 0) {
                cn = ftell(cf);
            }
            fclose(cf);
        }
        printf("[info] pipeline cache file: %s (%ld bytes)\n",
               g_pcache_path, cn);
        remove(g_pcache_path);
    }
    lc_window_destroy(window);
    lc_shutdown();
    return exit_code;
}
