/*
 * Luma shadow Vulkan integration test (Phase 16).
 *
 * Offscreen pixel verification of the renderer-owned shadow system:
 * depth-only targets, directional/spot footprints, PCF edges,
 * bias behavior (acne/peter-panning), moving occluders/lights,
 * multi-light independence, emissive/AO separation, normal-mapped
 * receivers, scaled/mirrored casters, imported models, debug views,
 * resolution switches, resizes, multiviewport, perf stats, and a
 * 500-frame endurance run with screenshot.
 *
 * Design rule: differential asserts (shadowed vs lit in the same or
 * a twin frame) isolate the shadow factor; absolute values only
 * where analytic (ambient/emissive). If the environment cannot
 * provide a window or Vulkan setup, SKIP and exit 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_assets/luma_assets.h>

#ifdef _MSC_VER
#include <crtdbg.h>
#endif

#include "graphics/graphics_internal.h"

#include "internal/renderer_internal.h"

#ifndef LA_FIXTURE_DIR
#define LA_FIXTURE_DIR "."
#endif

#define SH_TW 256u
#define SH_TH 256u

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
 * Environment + harness (mirrors the PBR test patterns, plus the
 * shadow leg: begin -> submits -> render_shadows -> main pass).
 * ------------------------------------------------------------------ */

typedef struct shadow_env {
    lc_device *device;
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
    lr_renderer *renderer;
    lr_camera camera;
    lc_image *color_img;
    lc_image_view *color_view;
    lc_image *depth_img;
    lc_image_view *depth_view;
    lc_render_target *target;
    uint32_t tw;
    uint32_t th;
} shadow_env;

static int make_device(lc_device **out) {
    lc_device_desc desc = { 0 };

    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 1;
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

    desc.title = "Luma Shadow Test";
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
                                       staging->vk_buffer, 0) != LC_SUCCESS) {
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

static int make_target(shadow_env *env, lc_format format, uint32_t w,
                       uint32_t h, int with_depth) {
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
    if (lc_image_create(env->device, &idesc, &env->color_img) !=
        LC_SUCCESS) {
        return -1;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(env->color_img, &vdesc, &env->color_view) !=
        LC_SUCCESS) {
        lc_image_destroy(env->color_img);
        env->color_img = NULL;
        return -1;
    }
    env->depth_img = NULL;
    env->depth_view = NULL;
    if (with_depth) {
        idesc.format = LC_FORMAT_D32_FLOAT;
        idesc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL;
        if (lc_image_create(env->device, &idesc, &env->depth_img) !=
            LC_SUCCESS) {
            return -1;
        }
        vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
        if (lc_image_view_create(env->depth_img, &vdesc,
                                 &env->depth_view) != LC_SUCCESS) {
            return -1;
        }
    }
    memset(&tdesc, 0, sizeof(tdesc));
    tdesc.width = w;
    tdesc.height = h;
    att.view = env->color_view;
    tdesc.color_attachments = &att;
    tdesc.color_attachment_count = 1;
    tdesc.depth_stencil_attachment = env->depth_view;
    if (lc_render_target_create(env->device, &tdesc, &env->target) !=
        LC_SUCCESS) {
        return -1;
    }
    env->tw = w;
    env->th = h;
    return 0;
}

static void destroy_target(shadow_env *env) {
    lc_render_target_destroy(env->target);
    lc_image_view_destroy(env->color_view);
    lc_image_destroy(env->color_img);
    lc_image_view_destroy(env->depth_view);
    lc_image_destroy(env->depth_img);
    env->target = NULL;
    env->color_view = NULL;
    env->color_img = NULL;
    env->depth_view = NULL;
    env->depth_img = NULL;
}

typedef struct shadow_frame_ops {
    lr_light *lights;
    uint32_t light_count;
    lr_draw_item *items;
    uint32_t item_count;
    la_model *model; /* optional extra submit (assets path) */
    lr_transform model_root;
    float ambient[3];
    lr_render_stats stats; /* captured after the offscreen leg */
    uint32_t shadow_count; /* captured after the offscreen leg */
    lr_shadow_slot_info slot0; /* captured after the offscreen leg */
} shadow_frame_ops;

/* One full frame: shadow leg + offscreen main leg (stats captured)
 * + swapchain present leg (ambient re-submit, keeps present valid).
 * Returns offscreen pixels or NULL. */
static unsigned char *render_pixels(shadow_env *env, shadow_frame_ops *ops) {
    lc_command_encoder *enc = NULL;
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_render_depth_attachment datt;
    lc_result res;
    uint32_t i;
    unsigned char *px = NULL;
#ifdef _MSC_VER
    clock_t t0 = clock();
#endif

    lc_poll_events();
    /* Drain any prior in-flight frame first: the lights/camera UBOs
     * are persistently-mapped CPU memcpys, so a previous frame
     * (a manual flow, or a NULL frame that skipped its readback
     * idle) could otherwise still be reading them when this frame
     * uploads. With 2 frames in flight, begin_frame's slot fence
     * alone does not cover the other slot. */
    test_wait_idle(env->device);
    res = lc_begin_frame(env->swapchain);
    if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
        /* Compositor lost the image (occluded/resized window in
         * long runs): recreate once and retry, else give up. */
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
    if (ops->model != NULL &&
        la_model_submit(ops->model, env->renderer, &ops->model_root) !=
            LA_SUCCESS) {
        fprintf(stderr, "[dbg] frame: model submit failed\n");
        lr_renderer_end(env->renderer);
        return NULL;
    }
    if (lr_renderer_render_shadows(env->renderer, enc) != LR_SUCCESS) {
        fprintf(stderr, "[dbg] frame: prepare failed\n");
        lr_renderer_end(env->renderer);
        return NULL;
    }
    lr_renderer_set_ambient(env->renderer, ops->ambient);
    memset(&catt, 0, sizeof(catt));
    catt.view = env->color_view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.width = env->tw;
    pdesc.height = env->th;
    /* Depth-tested main pass (painter order is undefined without
     * it: overlapping draws flickered with heap-address sort). */
    memset(&datt, 0, sizeof(datt));
    datt.view = env->depth_view;
    datt.depth_load_op = LC_LOAD_OP_CLEAR;
    datt.depth_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_depth = 1.0f;
    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
    pdesc.depth_attachment = &datt;
    if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
        fprintf(stderr, "[dbg] frame: main pass begin failed\n");
        lr_renderer_end(env->renderer);
        return NULL;
    }
    if (lr_renderer_render(env->renderer, enc, env->target) != LR_SUCCESS ||
        lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        fprintf(stderr, "[dbg] frame: main render/end failed\n");
        lr_renderer_end(env->renderer);
        return NULL;
    }
    lr_renderer_get_stats(env->renderer, &ops->stats);
    ops->shadow_count = lr_renderer_get_shadow_count(env->renderer);
    lr_renderer_get_shadow_slot_info(env->renderer, 0, &ops->slot0);
    lr_renderer_end(env->renderer);
    /* Swapchain leg: clear-only present (no renderer calls).
     *
     * The frame records into ONE command buffer submitted at
     * end_frame, while the lights UBO is a persistently-mapped CPU
     * memcpy. A second renderer leg here would memcpy unprepared
     * values (and begin() would zero the shadow metadata) AFTER the
     * offscreen leg recorded but BEFORE anything executes,
     * deterministically unshadowing the readback. One upload per
     * frame keeps the offscreen leg's prepared values live at
     * submit time (same precedent as the multiview "Present leg"). */
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
        if (lc_encoder_begin_swapchain_pass(enc, env->swapchain, &spass) !=
                LC_SUCCESS ||
            lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            return NULL;
        }
    }
    /* SUBOPTIMAL presented fine (recreate hint only); the
     * offscreen pixels are valid either way. */
    res = lc_end_frame(env->swapchain);
    if (res != LC_SUCCESS && res != LC_SUBOPTIMAL) {
        return NULL;
    }
    px = readback_rgba8(env->device, env->color_img, env->tw, env->th);
#ifdef _MSC_VER
    {
        double ms = 1000.0 * (double)(clock() - t0) / CLOCKS_PER_SEC;

        if (ms > 500.0) {
            fprintf(stderr, "[dbg] SLOW frame %.0fms\n", ms);
        }
    }
#endif
    return px;
}

/* ------------------------------------------------------------------
 * Small helpers.
 * ------------------------------------------------------------------ */

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

/* World point -> offscreen pixel (same matrices the GPU uses).
 * Clamped to the target (out-of-view projections saturate). */
static void world_to_pixel(shadow_env *env, double x, double y, double z,
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
    *out_x = (uint32_t)(nx * (double)env->tw);
    *out_y = (uint32_t)(ny * (double)env->th);
    if (*out_x >= env->tw) {
        *out_x = env->tw - 1;
    }
    if (*out_y >= env->th) {
        *out_y = env->th - 1;
    }
}

/* PBR material (all maps NULL unless given). */
static lr_material *make_pbr(shadow_env *env, const float base[4],
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

static void shadow_item(lr_draw_item *item, lr_mesh *mesh, lr_material *mat,
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

/* Directional light with explicit shadow config (negative biases =
 * renderer defaults; resolution 0 = 1024). */
static void dir_shadow_light(lr_light *light, float dx, float dy, float dz,
                             float intensity, uint32_t res, float depth_bias,
                             float normal_bias) {
    memset(light, 0, sizeof(*light));
    light->type = LR_LIGHT_DIRECTIONAL;
    light->color[0] = 1.0f;
    light->color[1] = 1.0f;
    light->color[2] = 1.0f;
    light->intensity = intensity;
    light->direction[0] = dx;
    light->direction[1] = dy;
    light->direction[2] = dz;
    light->shadow.enabled = 1;
    light->shadow.resolution = res;
    light->shadow.depth_bias = depth_bias;
    light->shadow.normal_bias = normal_bias;
}

static void spot_shadow_light(lr_light *light, float px, float py, float pz,
                              float dx, float dy, float dz, float intensity,
                              float range, float inner, float outer,
                              uint32_t res) {
    memset(light, 0, sizeof(*light));
    light->type = LR_LIGHT_SPOT;
    light->color[0] = 1.0f;
    light->color[1] = 1.0f;
    light->color[2] = 1.0f;
    light->intensity = intensity;
    light->position[0] = px;
    light->position[1] = py;
    light->position[2] = pz;
    light->range = range;
    light->direction[0] = dx;
    light->direction[1] = dy;
    light->direction[2] = dz;
    light->spot_inner = inner;
    light->spot_outer = outer;
    light->shadow.enabled = 1;
    light->shadow.resolution = res;
    light->shadow.depth_bias = -1.0f;
    light->shadow.normal_bias = -1.0f;
}

/* ------------------------------------------------------------------
 * Main.
 * ------------------------------------------------------------------ */

int main(void) {
    shadow_env env;
    lr_mesh *ground = NULL;
    lr_mesh *cube = NULL;
    lr_mesh *ball = NULL;
    lr_material *ground_mat = NULL;
    int rc;
    int exit_code = 1;
    static const float eye[3] = { 0.0f, 4.5f, 8.0f };
    static const float center[3] = { 0.0f, 0.5f, 0.0f };
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    static const float gray[4] = { 0.6f, 0.6f, 0.62f, 1.0f };
    static const float black3[3] = { 0.0f, 0.0f, 0.0f };

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    memset(&env, 0, sizeof(env));
    if (lc_init() != LC_SUCCESS) {
        printf("SKIP: lc_init failed\n");
        return 0;
    }
    rc = make_device(&env.device);
    if (rc != 0) {
        if (rc > 0) {
            SKIP_ENV("a Vulkan device");
        }
        printf("FAIL: lc_device_create\n");
        lc_shutdown();
        return 1;
    }
    rc = make_window(&env.window);
    if (rc != 0) {
        lc_device_destroy(env.device);
        if (rc > 0) {
            SKIP_ENV("a window");
        }
        printf("FAIL: lc_window_create\n");
        lc_shutdown();
        return 1;
    }
    {
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_swapchain_desc sdesc;

        if (lc_surface_create(env.device, env.window, &surface) !=
            LC_SUCCESS) {
            printf("FAIL: surface\n");
            goto cleanup;
        }
        env.surface = surface;
        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.width = 800;
        sdesc.height = 600;
        sdesc.vsync = 1;
        if (lc_swapchain_create(env.device, surface, &sdesc, &swapchain) !=
            LC_SUCCESS) {
            printf("FAIL: swapchain\n");
            goto cleanup;
        }
        env.swapchain = swapchain;
    }
    if (make_target(&env, LC_FORMAT_RGBA8_UNORM, SH_TW, SH_TH, 1) != 0) {
        printf("FAIL: offscreen target\n");
        goto cleanup;
    }
    {
        /* Renderer over the offscreen signature. */
        lr_renderer_desc rdesc;
        lc_render_target_desc sig;

        memset(&sig, 0, sizeof(sig));
        sig.width = SH_TW;
        sig.height = SH_TH;
        sig.color_attachment_count = 1;
        sig.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
        sig.depth_stencil_format = LC_FORMAT_UNDEFINED;
        sig.samples = LC_SAMPLE_COUNT_1;
        memset(&rdesc, 0, sizeof(rdesc));
        rdesc.device = env.device;
        rdesc.render_target = sig;
        rdesc.max_objects = 256;
        if (lr_renderer_create(&rdesc, &env.renderer) != LR_SUCCESS) {
            printf("FAIL: renderer\n");
            goto cleanup;
        }
    }
    lr_camera_init(&env.camera);
    if (lr_camera_set_perspective(&env.camera, 0.6f, 1.0f, 0.1f, 60.0f) !=
            LR_SUCCESS ||
        lr_camera_look_at(&env.camera, eye, center, up) != LR_SUCCESS) {
        printf("FAIL: camera\n");
        goto cleanup;
    }
    if (lr_mesh_create_plane(env.renderer, 20.0f, 20.0f, &ground) !=
            LR_SUCCESS ||
        lr_mesh_create_cube(env.renderer, 2.0f, &cube) != LR_SUCCESS ||
        lr_mesh_create_sphere(env.renderer, 1.0f, 32, 16, &ball) !=
            LR_SUCCESS) {
        printf("FAIL: meshes\n");
        goto cleanup;
    }
    ground_mat = make_pbr(&env, gray, 0.0f, 0.8f);
    if (ground_mat == NULL) {
        printf("FAIL: ground material\n");
        goto cleanup;
    }

    /* ---- PART E: depth-only targets are first-class ---- */
    {
        lc_image *dimg = NULL;
        lc_image_view *dview = NULL;
        lc_render_target *dtarget = NULL;
        lc_image_desc idesc;
        lc_image_view_desc vdesc;
        lc_render_target_create_desc tdesc;

        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_D32_FLOAT;
        idesc.width = 128;
        idesc.height = 128;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        idesc.usage =
            LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_DEPTH_STENCIL;
        idesc.samples = LC_SAMPLE_COUNT_1;
        TEST_CHECK(lc_image_create(env.device, &idesc, &dimg) ==
                       LC_SUCCESS,
                   "depth-only: sampled depth image creates");
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
        vdesc.mip_level_count = 1;
        vdesc.array_layer_count = 1;
        TEST_CHECK(lc_image_view_create(dimg, &vdesc, &dview) ==
                       LC_SUCCESS,
                   "depth-only: depth view creates");
        memset(&tdesc, 0, sizeof(tdesc));
        tdesc.width = 128;
        tdesc.height = 128;
        tdesc.color_attachment_count = 0;
        tdesc.depth_stencil_attachment = dview;
        TEST_CHECK(lc_render_target_create(env.device, &tdesc,
                                           &dtarget) == LC_SUCCESS,
                   "depth-only: 0-color target creates (no dummy)");
        if (dtarget != NULL) {
            TEST_CHECK(lc_render_target_get_color_count(dtarget) == 0 &&
                       lc_render_target_get_depth_format(dtarget) ==
                           LC_FORMAT_D32_FLOAT,
                       "depth-only: getters report 0 colors + depth");
        } else {
            TEST_CHECK(0, "depth-only: getters report 0 colors + depth");
        }
        lc_render_target_destroy(dtarget);
        lc_image_view_destroy(dview);
        lc_image_destroy(dimg);
    }

    /* ---- validation block (no frames needed) ---- */
    {
        lr_camera cam;
        lr_light bad;

        lr_camera_init(&cam);
        lr_camera_set_perspective(&cam, 0.6f, 1.0f, 0.1f, 60.0f);
        lr_camera_look_at(&cam, eye, center, up);
        TEST_CHECK(lr_renderer_begin(env.renderer, &cam) == LR_SUCCESS,
                   "validation: frame opens");
        memset(&bad, 0, sizeof(bad));
        bad.type = LR_LIGHT_SPOT;
        bad.intensity = 1.0f;
        bad.position[1] = 5.0f;
        bad.range = 10.0f;
        bad.direction[1] = -1.0f;
        bad.spot_inner = 0.5f;
        bad.spot_outer = 0.3f;
        TEST_CHECK(lr_renderer_submit_light(env.renderer, &bad) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "spot: inner > outer rejected");
        bad.spot_inner = 0.2f;
        bad.spot_outer = 1.5707964f;
        TEST_CHECK(lr_renderer_submit_light(env.renderer, &bad) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "spot: outer >= pi/2 rejected");
        bad.spot_outer = 0.4f;
        memset(bad.direction, 0, sizeof(bad.direction));
        TEST_CHECK(lr_renderer_submit_light(env.renderer, &bad) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "spot: zero direction rejected");
        bad.direction[1] = -1.0f;
        bad.range = 0.0f;
        TEST_CHECK(lr_renderer_submit_light(env.renderer, &bad) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "spot: non-positive range rejected");
        bad.range = 10.0f;
        bad.type = LR_LIGHT_POINT;
        bad.shadow.enabled = 1;
        TEST_CHECK(lr_renderer_submit_light(env.renderer, &bad) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "point: shadow request rejected");
        {
            lr_light dl;

            dir_shadow_light(&dl, 0.0f, -1.0f, 0.0f, 2.0f, 1000, -1.0f,
                             -1.0f);
            TEST_CHECK(lr_renderer_submit_light(env.renderer, &dl) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "shadow: non-POT resolution rejected");
            dir_shadow_light(&dl, 0.0f, -1.0f, 0.0f, 2.0f, 64, -1.0f,
                             -1.0f);
            TEST_CHECK(lr_renderer_submit_light(env.renderer, &dl) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "shadow: tiny resolution rejected");
            dir_shadow_light(&dl, 0.0f, -1.0f, 0.0f, 2.0f, 1024, -1.0f,
                             -1.0f);
            dl.shadow.near_plane = 2.0f;
            TEST_CHECK(lr_renderer_submit_light(env.renderer, &dl) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "shadow: directional near/far rejected");
        }
        {
            lr_light sl;

            memset(&sl, 0, sizeof(sl));
            sl.type = LR_LIGHT_SPOT;
            sl.intensity = 1.0f;
            sl.position[1] = 5.0f;
            sl.range = 5.0f;
            sl.direction[1] = -1.0f;
            sl.spot_inner = 0.1f;
            sl.spot_outer = 0.4f;
            sl.shadow.enabled = 1;
            sl.shadow.near_plane = 10.0f;
            TEST_CHECK(lr_renderer_submit_light(env.renderer, &sl) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "shadow: spot near beyond far rejected");
        }
        lr_renderer_end(env.renderer);
    }

    /* ---- PART AA: directional footprint (tilted light) ----
     *
     * Tilted key so the footprint separates from the caster
     * silhouette on screen: C is visible shadowed ground (ray
     * verified to miss the cube), L is lit ground outside. (Center
     * pixels show cube faces here, never ground — measuring them
     * would conflate silhouette with shadow.)
     */
    {
        lr_material *cube_mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lr_light light;
        lr_draw_item items[2];
        shadow_frame_ops ops;
        unsigned char *px;
        uint32_t cx;
        uint32_t cy;
        uint32_t lx;
        uint32_t ly;
        double shadowed[3];
        double lit[3];

        dir_shadow_light(&light, 0.5f, -1.0f, 0.2f, 3.0f, 1024, -1.0f,
                         -1.0f);
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], cube, cube_mat, 0.0f, 1.5f, 0.0f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = items;
        ops.item_count = 2;
        memcpy(ops.ambient, black3, sizeof(black3));
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "dirshadow: footprint render reads back");
        if (px != NULL) {
            world_to_pixel(&env, 1.7, 0.01, -0.2, &cx, &cy);
            world_to_pixel(&env, 1.7, 0.01, 1.6, &lx, &ly);
            pixel_at(px, SH_TW, cx, cy, shadowed);
            pixel_at(px, SH_TW, lx, ly, lit);
            fprintf(stderr, "[dbg] aa crescent=(%.4f) lit=(%.4f)\n",
                    luminance(shadowed), luminance(lit));
            TEST_CHECK(luminance(shadowed) < 0.15 * luminance(lit) &&
                           luminance(lit) > 0.2,
                       "dirshadow: crescent dark, outside lit");
            free(px);
        }
        /* PCF edge: scan the row across the expected edge for a
         * strict partial between dark and lit plateaus. */
        {
            unsigned char *px2 = render_pixels(&env, &ops);
            double lo = 1.0;
            double hi = 0.0;
            int partial = 0;
            int k;

            if (px2 != NULL) {
                /* World x from 0.5 to 3.0 along z=-0.2. */
                for (k = 0; k <= 50; k++) {
                    double c[3];
                    uint32_t sx;
                    uint32_t sy;
                    double lum;

                    world_to_pixel(&env, 0.5 + 0.05 * (double)k, 0.01,
                                   -0.2, &sx, &sy);
                    pixel_at(px2, SH_TW, sx, sy, c);
                    lum = luminance(c);
                    if (lum < lo) {
                        lo = lum;
                    }
                    if (lum > hi) {
                        hi = lum;
                    }
                }
                for (k = 0; k <= 50; k++) {
                    double c[3];
                    uint32_t sx;
                    uint32_t sy;
                    double lum;

                    world_to_pixel(&env, 0.5 + 0.05 * (double)k, 0.01,
                                   -0.2, &sx, &sy);
                    pixel_at(px2, SH_TW, sx, sy, c);
                    lum = luminance(c);
                    if (lum > lo + 0.15 * (hi - lo) &&
                        lum < lo + 0.85 * (hi - lo)) {
                        partial = 1;
                    }
                }
                free(px2);
            }
            fprintf(stderr, "[dbg] pcf lo=%.4f hi=%.4f partial=%d\n",
                    lo, hi, partial);
            TEST_CHECK(hi - lo > 0.2 && partial,
                       "pcf: footprint edge has a soft partial band");
        }
        /* Stats + introspection pin the machinery. */
        TEST_CHECK(ops.stats.shadow_casting_lights == 1 &&
                   ops.stats.shadow_passes == 1 &&
                   ops.stats.shadow_maps_rendered == 1,
                   "stats: one shadow light/pass/map");
        TEST_CHECK(ops.stats.shadow_draw_calls == 2,
                   "stats: both casters drawn to the map");
        TEST_CHECK(ops.shadow_count == 1,
                   "introspect: one active slot");
        TEST_CHECK(ops.slot0.resolution == 1024 &&
                   ops.slot0.format != LC_FORMAT_UNDEFINED &&
                   ops.slot0.active,
                   "introspect: slot reports 1024 + depth format");
        TEST_CHECK(lr_renderer_get_shadow_view(env.renderer, 0) != NULL &&
                   lr_renderer_get_shadow_view(env.renderer, 4) == NULL,
                   "introspect: debug view borrows slot 0");
        {
            uint32_t nl = lr_renderer_get_light_count(env.renderer);
            lr_light got;
            lr_shadow_desc scfg;

            (void)nl;
            lr_renderer_get_light(env.renderer, 0, &got, &scfg);
            TEST_CHECK(got.type == LR_LIGHT_DIRECTIONAL &&
                       scfg.enabled && scfg.resolution == 1024,
                       "introspect: submitted light reads back");
        }
        lr_material_destroy(cube_mat);
    }

    /* ---- vertical degenerate basis (GPU smoke) ----
     *
     * Straight-down light takes the degenerate-up basis branch.
     * Pixel proof of vertical footprints conflates with the caster
     * silhouette from any camera, so this block pins machinery
     * (pass/draws/no errors); the branch math itself is golden-tested
     * headlessly, and tilted pixels prove the full path below.
     */
    {
        lr_material *cube_mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lr_light light;
        lr_draw_item items[2];
        shadow_frame_ops ops;
        unsigned char *px;

        dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 3.0f, 1024, -1.0f,
                         -1.0f);
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], cube, cube_mat, 0.0f, 1.5f, 0.0f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = items;
        ops.item_count = 2;
        memcpy(ops.ambient, black3, sizeof(black3));
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "degenerate: vertical frame reads back");
        if (px != NULL) {
            free(px);
        }
        TEST_CHECK(ops.stats.shadow_passes == 1 &&
                   ops.stats.shadow_draw_calls == 2,
                   "degenerate: vertical pass draws casters");
        lr_material_destroy(cube_mat);
    }


    /* ---- PART AB/AC: moving occluder, moving light ---- */
    {
        lr_material *cube_mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lr_light light;
        lr_draw_item items[2];
        shadow_frame_ops ops;
        uint32_t ax;
        uint32_t ay;
        uint32_t bx;
        uint32_t by;
        double dark_a[3];
        double lit_b[3];

        dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 3.0f, 1024, -1.0f,
                         -1.0f);
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], cube, cube_mat, 0.0f, 1.5f, 0.0f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = items;
        ops.item_count = 2;
        memcpy(ops.ambient, black3, sizeof(black3));
        world_to_pixel(&env, 0.0, 0.01, 0.0, &ax, &ay);
        world_to_pixel(&env, 2.5, 0.01, 0.0, &bx, &by);
        /* Frame 1: shadow at A. */
        {
            unsigned char *px = render_pixels(&env, &ops);

            TEST_CHECK(px != NULL, "moving: first frame reads back");
            if (px != NULL) {
                pixel_at(px, SH_TW, ax, ay, dark_a);
                TEST_CHECK(luminance(dark_a) < 0.05,
                           "moving: occluder footprint starts dark");
                free(px);
            }
        }
        /* Frame 2: cube moved +2.5 in x; old spot must be lit. */
        items[1].transform.position[0] = 2.5f;
        {
            unsigned char *px = render_pixels(&env, &ops);
            double now_lit[3];
            double now_dark[3];

            TEST_CHECK(px != NULL, "moving: second frame reads back");
            if (px != NULL) {
                pixel_at(px, SH_TW, ax, ay, now_lit);
                pixel_at(px, SH_TW, bx, by, now_dark);
                TEST_CHECK(luminance(now_lit) > 0.2 &&
                               luminance(now_dark) < 0.05,
                           "moving: shadow follows caster, old spot lit");
                free(px);
            }
        }
        /* Moving light: mirrored direction displaces the footprint.
         * Travel is -x (and +z), so the shadow falls toward -x of the
         * caster: scan a row that misses the cube (z=-0.2, verified to
         * miss like the AA crescent) for the darkest ground pixel. */
        dir_shadow_light(&light, -0.5f, -1.0f, 0.25f, 3.0f, 1024, -1.0f,
                         -1.0f);
        items[1].transform.position[0] = 0.0f;
        {
            unsigned char *px = render_pixels(&env, &ops);
            double moved[3] = { 1.0, 1.0, 1.0 };
            double wx_found = 0.0;
            double wx;

            if (px != NULL) {
                /* World x from -3.0 to 0.5 along z=-0.2. Only the
                 * zone x in [-3,-1.3] counts: nearer the caster the
                 * ray hits the dark front face (not ground). */
                for (wx = -3.0; wx <= 0.51; wx += 0.05) {
                    double c[3];
                    uint32_t sx;
                    uint32_t sy;

                    world_to_pixel(&env, wx, 0.01, -0.2, &sx, &sy);
                    pixel_at(px, SH_TW, sx, sy, c);
                    if (wx <= -1.3 && luminance(c) < luminance(moved)) {
                        moved[0] = c[0];
                        moved[1] = c[1];
                        moved[2] = c[2];
                        wx_found = wx;
                    }
                }
                fprintf(stderr, "[dbg] tilted wx=%.2f lum=%.4f\n", wx_found,
                        luminance(moved));
                TEST_CHECK(luminance(moved) < 0.05 && wx_found < -0.5,
                           "moving: tilted light displaces footprint");
                free(px);
            } else {
                TEST_CHECK(0, "moving: tilted frame reads back");
            }
        }
        lr_material_destroy(cube_mat);
    }

    /* ---- PART N/O/AD: acne vs peter-panning ----
     *
     * Curved self-shadow with zero bias speckles (acne); default
     * bias smooths it; grossly excessive bias detaches the shadow
     * (edge band that should be dark reads lit). Each property is
     * asserted in the direction that proves the test is sensitive.
     */
    {
        lr_material *sphere_mat = make_pbr(&env, gray, 0.0f, 0.6f);
        lr_material *box_mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lr_light light;
        lr_draw_item items[2];
        shadow_frame_ops ops;
        uint32_t scx;
        uint32_t scy;

        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], ball, sphere_mat, 0.0f, 1.6f, 0.0f);
        world_to_pixel(&env, 0.0, 2.2, 0.0, &scx, &scy);
        /* Zero bias: expect speckle on the lit crescent. */
        dir_shadow_light(&light, 0.35f, -1.0f, 0.2f, 3.0f, 1024, 0.0f,
                         0.0f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = items;
        ops.item_count = 2;
        memcpy(ops.ambient, black3, sizeof(black3));
        {
            unsigned char *px = render_pixels(&env, &ops);
            double maxjump = 0.0;
            int dx;
            int dy;

            TEST_CHECK(px != NULL, "acne: zero-bias frame reads back");
            if (px != NULL) {
                for (dy = -12; dy <= 12; dy++) {
                    for (dx = -12; dx <= 12; dx++) {
                        int x0 = (int)scx + dx;
                        int y0 = (int)scy + dy;
                        double a[3];
                        double b[3];

                        if (x0 < 1 || y0 < 1 || x0 + 1 >= (int)SH_TW ||
                            y0 + 1 >= (int)SH_TH) {
                            continue;
                        }
                        pixel_at(px, SH_TW, (uint32_t)x0, (uint32_t)y0,
                                 a);
                        pixel_at(px, SH_TW, (uint32_t)(x0 + 1),
                                 (uint32_t)y0, b);
                        {
                            double j = luminance(a) - luminance(b);

                            if (j < 0.0) {
                                j = -j;
                            }
                            if (j > maxjump) {
                                maxjump = j;
                            }
                        }
                    }
                }
                fprintf(stderr, "[dbg] acne maxjump=%.4f\n", maxjump);
                /* Zero bias speckles an order of magnitude above the
                 * default-bias smooth shading (defjump ~0.008). */
                TEST_CHECK(maxjump > 0.05,
                           "acne: zero bias speckles curved shading");
                free(px);
            }
        }
        /* Default bias: smooth crescent (no catastrophic acne). */
        dir_shadow_light(&light, 0.35f, -1.0f, 0.2f, 3.0f, 1024, -1.0f,
                         -1.0f);
        {
            unsigned char *px = render_pixels(&env, &ops);
            double worst = 0.0;
            double median = 0.0;
            double defjump = 0.0;
            double samples[625];
            uint32_t nsamp = 0;
            int dx;
            int dy;

            if (px != NULL) {
                for (dy = -12; dy <= 12; dy++) {
                    for (dx = -12; dx <= 12; dx++) {
                        int x0 = (int)scx + dx;
                        int y0 = (int)scy + dy;
                        double a[3];

                        if (x0 < 0 || y0 < 0 || x0 >= (int)SH_TW ||
                            y0 >= (int)SH_TH) {
                            continue;
                        }
                        pixel_at(px, SH_TW, (uint32_t)x0, (uint32_t)y0,
                                 a);
                        if (nsamp < 625) {
                            samples[nsamp++] = luminance(a);
                        }
                    }
                }
                {
                    /* Adjacent-pixel jump BEFORE the median sort
                     * below (the sort destroys grid adjacency). */
                    uint32_t a;

                    for (a = 0; a + 1 < nsamp; a++) {
                        double j;

                        if ((a + 1) % 25 == 0) {
                            continue;
                        }
                        j = samples[a] - samples[a + 1];
                        if (j < 0.0) {
                            j = -j;
                        }
                        if (j > defjump) {
                            defjump = j;
                        }
                    }
                }
                {
                    uint32_t a;

                    for (a = 1; a < nsamp; a++) {
                        double v = samples[a];
                        uint32_t b = a;

                        while (b > 0 && samples[b - 1] > v) {
                            samples[b] = samples[b - 1];
                            b--;
                        }
                        samples[b] = v;
                    }
                }
                median = (nsamp > 0) ? samples[nsamp / 2] : 0.0;
                for (dy = -12; dy <= 12; dy++) {
                    for (dx = -12; dx <= 12; dx++) {
                        int x0 = (int)scx + dx;
                        int y0 = (int)scy + dy;
                        double a[3];
                        double dev;

                        if (x0 < 0 || y0 < 0 || x0 >= (int)SH_TW ||
                            y0 >= (int)SH_TH) {
                            continue;
                        }
                        pixel_at(px, SH_TW, (uint32_t)x0, (uint32_t)y0,
                                 a);
                        dev = luminance(a) - median;
                        if (dev < 0.0) {
                            dev = -dev;
                        }
                        if (dev > worst) {
                            worst = dev;
                        }
                    }
                }
                TEST_CHECK(median > 0.1 && worst < 0.2,
                           "acne: default bias keeps shading smooth");
                fprintf(stderr, "[dbg] acne default defjump=%.4f\n",
                        defjump);
                free(px);
            } else {
                TEST_CHECK(0, "acne: default-bias frame reads back");
            }
        }
        /* Excessive bias detaches: footprint edge band reads lit.
         * Tilted light puts the band on visible ground (rays miss
         * the cube); under a vertical light the band conflates with
         * the caster silhouette. */
        {
            lr_draw_item edge_items[2];
            shadow_frame_ops eops;
            unsigned char *px;
            uint32_t k;
            uint32_t dark_default = 0;
            uint32_t lit_huge = 0;

            dir_shadow_light(&light, 0.5f, -1.0f, 0.2f, 3.0f, 1024, -1.0f,
                             -1.0f);
            shadow_item(&edge_items[0], ground, ground_mat, 0.0f, 0.0f,
                        0.0f);
            shadow_item(&edge_items[1], cube, box_mat, 0.0f, 1.5f, 0.0f);
            memset(&eops, 0, sizeof(eops));
            eops.lights = &light;
            eops.light_count = 1;
            eops.items = edge_items;
            eops.item_count = 2;
            memcpy(eops.ambient, black3, sizeof(black3));
            px = render_pixels(&env, &eops);
            if (px != NULL) {
                for (k = 0; k < 12; k++) {
                    double c[3];
                    uint32_t sx;
                    uint32_t sy;

                    world_to_pixel(&env, 1.3 + 0.02 * (double)k, 0.01,
                                   -0.2, &sx, &sy);
                    pixel_at(px, SH_TW, sx, sy, c);
                    if (luminance(c) < 0.1) {
                        dark_default++;
                    }
                }
                free(px);
            }
            light.shadow.depth_bias = 0.15f;
            px = render_pixels(&env, &eops);
            if (px != NULL) {
                uint32_t k;

                for (k = 0; k < 12; k++) {
                    double c[3];
                    uint32_t sx;
                    uint32_t sy;

                    world_to_pixel(&env, 1.3 + 0.02 * (double)k, 0.01,
                                   -0.2, &sx, &sy);
                    pixel_at(px, SH_TW, sx, sy, c);
                    if (luminance(c) > 0.2) {
                        lit_huge++;
                    }
                }
                free(px);
            }
            fprintf(stderr, "[dbg] pan hold=%u detach=%u\n", dark_default,
                    lit_huge);
            TEST_CHECK(dark_default >= 9,
                       "panning: default bias holds the edge band");
            TEST_CHECK(lit_huge >= 6,
                       "panning: gross bias detaches the edge band");
        }
        lr_material_destroy(box_mat);
        lr_material_destroy(sphere_mat);
    }

    /* ---- PART AM/AO/AO: multi-light, emissive, occlusion ----
     *
     * Occluding the directional must not darken the point light;
     * emissive must glow through shadow; ambient occlusion must
     * damp ambient only, independently of the shadow factor.
     */
    {
        lr_material *cube_mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lr_light dir;
        lr_light point;
        lr_draw_item items[2];
        shadow_frame_ops ops;
        uint32_t cx;
        uint32_t cy;
        double both[3];
        double point_only[3];
        double dir_only[3];

        dir_shadow_light(&dir, 0.0f, -1.0f, 0.0f, 2.0f, 1024, -1.0f,
                         -1.0f);
        memset(&point, 0, sizeof(point));
        point.type = LR_LIGHT_POINT;
        point.color[0] = 1.0f;
        point.color[1] = 0.75f;
        point.color[2] = 0.5f;
        point.intensity = 40.0f;
        point.position[0] = 4.0f;
        point.position[1] = 3.0f;
        point.position[2] = 1.0f;
        point.range = 15.0f;
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], cube, cube_mat, 0.0f, 1.5f, 0.0f);
        /* Probe VISIBLE ground inside the vertical footprint (the ray
         * misses the cube below its front face); the under-cube
         * pixel shows the unlit bottom face, never ground. */
        world_to_pixel(&env, 0.7, 0.01, 0.7, &cx, &cy);
        /* Both lights (dir occluded at the pixel). */
        {
            lr_light both_lights[2];
            unsigned char *px;

            both_lights[0] = dir;
            both_lights[1] = point;
            memset(&ops, 0, sizeof(ops));
            ops.lights = both_lights;
            ops.light_count = 2;
            ops.items = items;
            ops.item_count = 2;
            memcpy(ops.ambient, black3, sizeof(black3));
            px = render_pixels(&env, &ops);
            TEST_CHECK(px != NULL, "multilight: combined frame reads");
            if (px != NULL) {
                pixel_at(px, SH_TW, cx, cy, both);
                free(px);
            }
        }
        /* Point only (dir removed): must equal the combined pixel. */
        {
            unsigned char *px;

            memset(&ops, 0, sizeof(ops));
            ops.lights = &point;
            ops.light_count = 1;
            ops.items = items;
            ops.item_count = 2;
            memcpy(ops.ambient, black3, sizeof(black3));
            px = render_pixels(&env, &ops);
            if (px != NULL) {
                pixel_at(px, SH_TW, cx, cy, point_only);
                free(px);
            }
        }
        /* Dir only (point removed): footprint core, ambient only. */
        {
            unsigned char *px;

            memset(&ops, 0, sizeof(ops));
            ops.lights = &dir;
            ops.light_count = 1;
            ops.items = items;
            ops.item_count = 2;
            memcpy(ops.ambient, black3, sizeof(black3));
            px = render_pixels(&env, &ops);
            if (px != NULL) {
                pixel_at(px, SH_TW, cx, cy, dir_only);
                free(px);
            }
        }
        TEST_CHECK(fabs(luminance(both) - luminance(point_only)) < 0.04,
                   "multilight: occluded dir leaves point intact");
        TEST_CHECK(luminance(both) > luminance(dir_only) + 0.10,
                   "multilight: point adds light the dir cannot");
        lr_material_destroy(cube_mat);
    }

    /* ---- emissive in shadow + occlusion separation ----
     *
     * Five items: ground, central occluder cube (shadows the occ
     * plane), occ plane (R=64 map, analytic expectation), sphere
     * occluder (shadows the glow cube), glow cube (factor-only
     * emissive, must glow through shadow).
     */
    {
        lr_material *glow_mat = NULL;
        lr_material *occ_mat = NULL;
        lr_material *cube_mat = NULL;
        lr_light light;
        lr_draw_item items[5];
        shadow_frame_ops ops;
        unsigned char *px;
        uint32_t gx;
        uint32_t gy;
        uint32_t ox;
        uint32_t oy;
        double glow_in[3];
        double occ_in[3];
        lc_image *occ_img = NULL;
        lc_image_view *occ_view = NULL;
        lr_mesh *occ_plane = NULL;

        {
            /* Factor-only emissive cube (no emissive texture). */
            lr_pbr_material_desc desc;

            memset(&desc, 0, sizeof(desc));
            desc.base_color_factor[0] = 0.02f;
            desc.base_color_factor[1] = 0.02f;
            desc.base_color_factor[2] = 0.02f;
            desc.base_color_factor[3] = 1.0f;
            desc.roughness_factor = 0.9f;
            desc.emissive_factor[0] = 0.9f;
            desc.emissive_factor[1] = 0.45f;
            desc.emissive_factor[2] = 0.15f;
            desc.normal_scale = 1.0f;
            desc.occlusion_strength = 1.0f;
            if (lr_material_create_pbr(env.renderer, &desc, &glow_mat) !=
                LR_SUCCESS) {
                printf("FAIL: emissive material\n");
                goto cleanup;
            }
        }
        {
            /* Occlusion plane (R=64/255) inside the footprint. */
            unsigned char occ_px[64];
            lc_image_desc idesc;
            lc_image_upload_desc up;
            lc_image_view_desc vdesc;
            lr_pbr_material_desc desc;
            int i;

            for (i = 0; i < 16; i++) {
                occ_px[i * 4 + 0] = 64;
                occ_px[i * 4 + 1] = 64;
                occ_px[i * 4 + 2] = 64;
                occ_px[i * 4 + 3] = 255;
            }
            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = 4;
            idesc.height = 4;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                          LC_IMAGE_USAGE_TRANSFER_DST;
            idesc.samples = LC_SAMPLE_COUNT_1;
            memset(&up, 0, sizeof(up));
            up.width = 4;
            up.height = 4;
            up.depth = 1;
            up.data = occ_px;
            up.data_size = sizeof(occ_px);
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.mip_level_count = 1;
            vdesc.array_layer_count = 1;
            if (lc_image_create(env.device, &idesc, &occ_img) !=
                    LC_SUCCESS ||
                lc_image_write(occ_img, &up) != LC_SUCCESS ||
                lc_image_view_create(occ_img, &vdesc, &occ_view) !=
                    LC_SUCCESS ||
                lr_mesh_create_plane(env.renderer, 1.5f, 1.5f,
                                     &occ_plane) != LR_SUCCESS) {
                printf("FAIL: occlusion resources\n");
                goto cleanup;
            }
            memset(&desc, 0, sizeof(desc));
            desc.base_color_factor[0] = 0.6f;
            desc.base_color_factor[1] = 0.6f;
            desc.base_color_factor[2] = 0.6f;
            desc.base_color_factor[3] = 1.0f;
            desc.roughness_factor = 0.9f;
            desc.occlusion_texture = occ_view;
            desc.occlusion_strength = 1.0f;
            desc.normal_scale = 1.0f;
            if (lr_material_create_pbr(env.renderer, &desc, &occ_mat) !=
                LR_SUCCESS) {
                printf("FAIL: occlusion material\n");
                goto cleanup;
            }
            cube_mat = make_pbr(&env, gray, 0.0f, 0.8f);
            if (cube_mat == NULL) {
                printf("FAIL: occluder material\n");
                goto cleanup;
            }
            shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
            shadow_item(&items[1], cube, cube_mat, 0.0f, 1.5f, 0.0f);
            shadow_item(&items[2], occ_plane, occ_mat, 0.0f, 0.02f, 0.0f);
            shadow_item(&items[3], ball, cube_mat, -2.5f, 2.0f, -1.0f);
            shadow_item(&items[4], cube, glow_mat, -2.5f, 0.35f, -1.0f);
            /* occ_plane stays alive until after the render below. */
        }
        dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 3.0f, 1024, -1.0f,
                         -1.0f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = items;
        ops.item_count = 5;
        ops.ambient[0] = 0.25f;
        ops.ambient[1] = 0.25f;
        ops.ambient[2] = 0.25f;
        world_to_pixel(&env, -2.5, 0.7, -1.0, &gx, &gy);
        world_to_pixel(&env, 0.0, 0.03, 0.0, &ox, &oy);
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "emissive/ao: frame reads back");
        if (px != NULL) {
            double want_occ[3] = { 0.6 * 0.25 * (64.0 / 255.0),
                                   0.6 * 0.25 * (64.0 / 255.0),
                                   0.6 * 0.25 * (64.0 / 255.0) };

            pixel_at(px, SH_TW, gx, gy, glow_in);
            /* The occ plane is a thin strip on screen; take the min
             * over a small vertical window so ±8px aim error cannot
             * land on the (brighter) cube face or ground. */
            occ_in[0] = 1.0;
            occ_in[1] = 1.0;
            occ_in[2] = 1.0;
            {
                int dy;

                for (dy = -8; dy <= 8; dy++) {
                    int yy = (int)oy + dy;
                    double c[3];

                    if (yy < 0 || yy >= (int)SH_TH) {
                        continue;
                    }
                    pixel_at(px, SH_TW, ox, (uint32_t)yy, c);
                    if (luminance(c) < luminance(occ_in)) {
                        occ_in[0] = c[0];
                        occ_in[1] = c[1];
                        occ_in[2] = c[2];
                    }
                }
            }
            fprintf(stderr, "[dbg] ao occ=(%.4f,%.4f) want=%.4f\n",
                    occ_in[0], occ_in[1], want_occ[0]);
            TEST_CHECK(fabs(glow_in[0] - 0.9) < 0.06 &&
                       fabs(glow_in[1] - 0.45) < 0.06 &&
                       fabs(glow_in[2] - 0.15) < 0.06,
                       "emissive: glows through shadow undimmed");
            TEST_CHECK(fabs(occ_in[0] - want_occ[0]) < 0.03 &&
                       fabs(occ_in[1] - want_occ[1]) < 0.03,
                       "ao: shadowed ambient matches occ math");
            free(px);
        }
        lr_material_destroy(cube_mat);
        lr_material_destroy(occ_mat);
        lr_material_destroy(glow_mat);
        lr_mesh_destroy(occ_plane);
        lc_image_view_destroy(occ_view);
        lc_image_destroy(occ_img);
    }

    /* ---- PART AE: normal-mapped receiver ---- */
    {
        /* Constant-tilt map (tangent +X lean); the shadow must apply
         * on top of tilted shading, and unshadowed tilt must stay
         * dimmer than flat (mapping intact under shadows). */
        unsigned char tilt_px[64];
        lc_image *tilt_img = NULL;
        lc_image_view *tilt_view = NULL;
        lc_sampler *tilt_samp = NULL;
        lr_material *flat_mat = NULL;
        lr_material *tilt_mat = NULL;
        lr_mesh *recv_plane = NULL;
        lr_light light;
        lr_draw_item items[3];
        shadow_frame_ops ops;
        unsigned char *px;
        uint32_t tx;
        uint32_t ty;
        double got_tilt[3];
        int i;

        for (i = 0; i < 16; i++) {
            tilt_px[i * 4 + 0] = 191;
            tilt_px[i * 4 + 1] = 128;
            tilt_px[i * 4 + 2] = 221;
            tilt_px[i * 4 + 3] = 255;
        }
        {
            lc_image_desc idesc;
            lc_image_upload_desc up;
            lc_image_view_desc vdesc;
            lc_sampler_desc sdesc;
            lr_pbr_material_desc desc;
            static const float tbase[4] = { 0.65f, 0.65f, 0.7f, 1.0f };

            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = 4;
            idesc.height = 4;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                          LC_IMAGE_USAGE_TRANSFER_DST;
            idesc.samples = LC_SAMPLE_COUNT_1;
            memset(&up, 0, sizeof(up));
            up.width = 4;
            up.height = 4;
            up.depth = 1;
            up.data = tilt_px;
            up.data_size = sizeof(tilt_px);
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.mip_level_count = 1;
            vdesc.array_layer_count = 1;
            memset(&sdesc, 0, sizeof(sdesc));
            sdesc.min_filter = LC_FILTER_LINEAR;
            sdesc.mag_filter = LC_FILTER_LINEAR;
            sdesc.mipmap_mode = LC_MIPMAP_MODE_NEAREST;
            sdesc.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
            sdesc.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
            sdesc.max_anisotropy = 1.0f;
            if (lc_image_create(env.device, &idesc, &tilt_img) !=
                    LC_SUCCESS ||
                lc_image_write(tilt_img, &up) != LC_SUCCESS ||
                lc_image_view_create(tilt_img, &vdesc, &tilt_view) !=
                    LC_SUCCESS ||
                lc_sampler_create(env.device, &sdesc, &tilt_samp) !=
                    LC_SUCCESS ||
                lr_mesh_create_plane(env.renderer, 1.6f, 1.6f,
                                     &recv_plane) != LR_SUCCESS) {
                printf("FAIL: tilt resources\n");
                goto cleanup;
            }
            flat_mat = make_pbr(&env, tbase, 0.0f, 0.5f);
            memset(&desc, 0, sizeof(desc));
            memcpy(desc.base_color_factor, tbase,
                   sizeof(desc.base_color_factor));
            desc.roughness_factor = 0.5f;
            desc.normal_texture = tilt_view;
            desc.sampler = tilt_samp;
            desc.normal_scale = 1.0f;
            desc.occlusion_strength = 1.0f;
            if (flat_mat == NULL ||
                lr_material_create_pbr(env.renderer, &desc, &tilt_mat) !=
                    LR_SUCCESS) {
                printf("FAIL: tilt materials\n");
                goto cleanup;
            }
        }
        dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 3.0f, 1024, -1.0f,
                         -1.0f);
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], cube, make_pbr(&env, gray, 0.0f, 0.8f),
                    0.0f, 3.2f, 0.0f);
        /* Elevated occluder (bottom y=2.2) shadows the receiver. */
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = items;
        ops.item_count = 3;
        memcpy(ops.ambient, black3, sizeof(black3));
        {
            /* Receiver plane at origin (inside footprint). */
            shadow_item(&items[2], recv_plane, tilt_mat, 0.0f, 0.02f,
                        0.0f);
            ops.item_count = 3;
        }
        world_to_pixel(&env, 0.0, 0.03, 0.0, &tx, &ty);
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "normalrecv: shadowed tilt reads back");
        if (px != NULL) {
            pixel_at(px, SH_TW, tx, ty, got_tilt);
            TEST_CHECK(luminance(got_tilt) < 0.05,
                       "normalrecv: shadow darkens tilted shading");
            free(px);
        }
        /* Unshadowed twins: tilt ordering intact (mapping works). */
        light.shadow.enabled = 0;
        {
            unsigned char *p1 = NULL;
            unsigned char *p2 = NULL;

            shadow_item(&items[2], recv_plane, tilt_mat, 0.0f, 0.02f,
                        0.0f);
            ops.item_count = 3;
            p1 = render_pixels(&env, &ops);
            shadow_item(&items[2], recv_plane, flat_mat, 0.0f, 0.02f,
                        0.0f);
            p2 = render_pixels(&env, &ops);
            if (p1 != NULL && p2 != NULL) {
                double a[3];
                double b[3];

                pixel_at(p1, SH_TW, tx, ty, a);
                pixel_at(p2, SH_TW, tx, ty, b);
                TEST_CHECK(luminance(a) < luminance(b) - 0.05,
                           "normalrecv: tilt mapping intact unshadowed");
            } else {
                TEST_CHECK(0, "normalrecv: unshadowed twins read back");
            }
            free(p1);
            free(p2);
        }
        lr_mesh_destroy(recv_plane);
        lr_material_destroy(tilt_mat);
        lr_material_destroy(flat_mat);
        lr_material_destroy(items[1].material);
        lc_sampler_destroy(tilt_samp);
        lc_image_view_destroy(tilt_view);
        lc_image_destroy(tilt_img);
    }

    /* ---- PART AF/AG/AV: scaled, double-sided, mirrored casters ----
     *
     * Non-uniform scale must widen the silhouette; double-sided and
     * single-sided horizontals must agree (documented CULL_NONE
     * rule); mirrored meshes cast correct silhouettes even though
     * their main-pass winding is inside-out (documented debt:
     * mirrored main centers differ, shadows match).
     */
    {
        lr_material *mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lr_material *unlit = NULL;
        lr_light light;
        lr_draw_item items[2];
        shadow_frame_ops ops;
        uint32_t dark_plain = 0;
        uint32_t dark_scaled = 0;
        uint32_t k;

        {
            lr_unlit_material_desc udesc;

            memset(&udesc, 0, sizeof(udesc));
            udesc.color[0] = 0.9f;
            udesc.color[1] = 0.9f;
            udesc.color[2] = 0.9f;
            udesc.color[3] = 1.0f;
            if (lr_material_create_unlit(env.renderer, &udesc, &unlit) !=
                LR_SUCCESS) {
                printf("FAIL: unlit caster material\n");
                goto cleanup;
            }
        }
        dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 3.0f, 1024, -1.0f,
                         -1.0f);
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], cube, mat, 0.0f, 1.5f, 0.0f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = items;
        ops.item_count = 2;
        memcpy(ops.ambient, black3, sizeof(black3));
        /* Plain footprint width (dark run along z=0). */
        {
            unsigned char *px = render_pixels(&env, &ops);
            uint32_t cx;
            uint32_t cy;

            if (px != NULL) {
                world_to_pixel(&env, 0.0, 0.01, 0.0, &cx, &cy);
                for (k = 0; k < 80; k++) {
                    int sx = (int)cx - 40 + (int)k;
                    double c[3];

                    if (sx < 0 || sx >= (int)SH_TW) {
                        continue;
                    }
                    pixel_at(px, SH_TW, (uint32_t)sx, cy, c);
                    if (luminance(c) < 0.1) {
                        dark_plain++;
                    }
                }
                free(px);
            }
        }
        /* Non-uniform scale (2,0.5,1.5): ~2x wider in x. */
        items[1].transform.scale[0] = 2.0f;
        items[1].transform.scale[1] = 0.5f;
        items[1].transform.scale[2] = 1.5f;
        items[1].transform.position[1] = 1.0f;
        {
            unsigned char *px = render_pixels(&env, &ops);

            if (px != NULL) {
                uint32_t cx;
                uint32_t cy;

                world_to_pixel(&env, 0.0, 0.01, 0.0, &cx, &cy);
                for (k = 0; k < 120; k++) {
                    int sx = (int)cx - 60 + (int)k;
                    double c[3];

                    if (sx < 0 || sx >= (int)SH_TW) {
                        continue;
                    }
                    pixel_at(px, SH_TW, (uint32_t)sx, cy, c);
                    if (luminance(c) < 0.1) {
                        dark_scaled++;
                    }
                }
                free(px);
            }
        }
            fprintf(stderr, "[dbg] scaled plain=%u scaled=%u\n", dark_plain,
                    dark_scaled);
            TEST_CHECK(dark_plain > 10 && dark_scaled * 4 > dark_plain * 5,
                       "scaled: non-uniform caster widens the silhouette");
        /* Unlit caster: same footprint (may cast unless disabled).
         * Mirrored tilt (-x shadow) with a pure-ground zone (the
         * +x crescent row clips the caster itself). */
        items[1].transform.scale[0] = 1.0f;
        items[1].transform.scale[1] = 1.0f;
        items[1].transform.scale[2] = 1.0f;
        items[1].transform.position[1] = 1.5f;
        dir_shadow_light(&light, -0.5f, -1.0f, 0.25f, 3.0f, 1024, -1.0f,
                         -1.0f);
        {
            uint32_t dark_pbr = 0;
            uint32_t dark_unlit = 0;
            int kk;

            items[1].material = mat;
            {
                unsigned char *px = render_pixels(&env, &ops);

                if (px != NULL) {
                    for (kk = 0; kk <= 34; kk++) {
                        double c[3];
                        uint32_t sx;
                        uint32_t sy;

                        world_to_pixel(&env, -3.0 + 0.05 * (double)kk,
                                       0.01, -0.2, &sx, &sy);
                        pixel_at(px, SH_TW, sx, sy, c);
                        if (luminance(c) < 0.1) {
                            dark_pbr++;
                        }
                    }
                    free(px);
                }
            }
            items[1].material = unlit;
            {
                unsigned char *px = render_pixels(&env, &ops);

                if (px != NULL) {
                    for (kk = 0; kk <= 34; kk++) {
                        double c[3];
                        uint32_t sx;
                        uint32_t sy;

                        world_to_pixel(&env, -3.0 + 0.05 * (double)kk,
                                       0.01, -0.2, &sx, &sy);
                        pixel_at(px, SH_TW, sx, sy, c);
                        if (luminance(c) < 0.1) {
                            dark_unlit++;
                        }
                    }
                    free(px);
                }
            }
            fprintf(stderr, "[dbg] unlit pbr=%u unlit=%u\n", dark_pbr,
                    dark_unlit);
            TEST_CHECK(dark_pbr > 10 && dark_unlit + 4 >= dark_pbr &&
                           dark_pbr + 4 >= dark_unlit,
                       "unlit caster: footprint matches PBR caster");
            /* Restore the vertical light for the sections below. */
            dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 3.0f, 1024, -1.0f,
                             -1.0f);
            items[1].material = mat;
        }
        /* Double-sided vs single-sided horizontals agree. */
        {
            lr_mesh *flat = NULL;
            lr_material *ds_mat = NULL;
            uint32_t dark_single = 0;
            uint32_t dark_double = 0;

            if (lr_mesh_create_plane(env.renderer, 2.0f, 2.0f, &flat) !=
                LR_SUCCESS) {
                printf("FAIL: flat caster mesh\n");
                goto cleanup;
            }
            {
                lr_pbr_material_desc desc;

                memset(&desc, 0, sizeof(desc));
                desc.base_color_factor[0] = 0.6f;
                desc.base_color_factor[1] = 0.6f;
                desc.base_color_factor[2] = 0.62f;
                desc.base_color_factor[3] = 1.0f;
                desc.roughness_factor = 0.8f;
                desc.normal_scale = 1.0f;
                desc.occlusion_strength = 1.0f;
                desc.double_sided = 1;
                if (lr_material_create_pbr(env.renderer, &desc,
                                           &ds_mat) != LR_SUCCESS) {
                    printf("FAIL: double-sided material\n");
                    goto cleanup;
                }
            }
            shadow_item(&items[1], flat, mat, 0.0f, 1.0f, 0.0f);
            {
                unsigned char *px = render_pixels(&env, &ops);

                if (px != NULL) {
                    uint32_t cx;
                    uint32_t cy;

                    world_to_pixel(&env, 0.0, 0.01, 0.0, &cx, &cy);
                    for (k = 0; k < 80; k++) {
                        int sx = (int)cx - 40 + (int)k;
                        double c[3];

                        if (sx < 0 || sx >= (int)SH_TW) {
                            continue;
                        }
                        pixel_at(px, SH_TW, (uint32_t)sx, cy, c);
                        if (luminance(c) < 0.1) {
                            dark_single++;
                        }
                    }
                    free(px);
                }
            }
            items[1].material = ds_mat;
            {
                unsigned char *px = render_pixels(&env, &ops);

                if (px != NULL) {
                    uint32_t cx;
                    uint32_t cy;

                    world_to_pixel(&env, 0.0, 0.01, 0.0, &cx, &cy);
                    for (k = 0; k < 80; k++) {
                        int sx = (int)cx - 40 + (int)k;
                        double c[3];

                        if (sx < 0 || sx >= (int)SH_TW) {
                            continue;
                        }
                        pixel_at(px, SH_TW, (uint32_t)sx, cy, c);
                        if (luminance(c) < 0.1) {
                            dark_double++;
                        }
                    }
                    free(px);
                }
            }
            fprintf(stderr, "[dbg] ds single=%u double=%u\n", dark_single,
                    dark_double);
            TEST_CHECK(dark_single > 10 &&
                           dark_double >= (dark_single * 3) / 4 &&
                           dark_double <= dark_single + dark_single / 2,
                       "doublesided: matches single-sided silhouette");
            lr_material_destroy(ds_mat);
            lr_mesh_destroy(flat);
        }
        /* Mirrored caster (negative scale): shadow matches, but the
         * main pass sees through the flipped-winding top to unlit
         * interior (documented debt: mirrored main top differs). */
        {
            unsigned char *p_norm = NULL;
            unsigned char *p_mirr = NULL;
            double top_norm[3] = { 0.0, 0.0, 0.0 };
            double top_mirr[3] = { 0.0, 0.0, 0.0 };
            uint32_t dark_norm = 0;
            uint32_t dark_mirr = 0;
            uint32_t cx;
            uint32_t cy;
            uint32_t nx;
            uint32_t ny;

            shadow_item(&items[1], cube, mat, -2.0f, 1.5f, 0.0f);
            p_norm = render_pixels(&env, &ops);
            items[1].transform.scale[0] = -1.0f;
            p_mirr = render_pixels(&env, &ops);
            world_to_pixel(&env, -2.0, 0.01, 0.0, &cx, &cy);
            world_to_pixel(&env, -2.0, 2.5, 0.0, &nx, &ny);
            if (p_norm != NULL && p_mirr != NULL) {
                pixel_at(p_norm, SH_TW, nx, ny, top_norm);
                pixel_at(p_mirr, SH_TW, nx, ny, top_mirr);
                for (k = 0; k < 80; k++) {
                    int sx = (int)cx - 40 + (int)k;
                    double a[3];
                    double b[3];

                    if (sx < 0 || sx >= (int)SH_TW) {
                        continue;
                    }
                    pixel_at(p_norm, SH_TW, (uint32_t)sx, cy, a);
                    pixel_at(p_mirr, SH_TW, (uint32_t)sx, cy, b);
                    if (luminance(a) < 0.1) {
                        dark_norm++;
                    }
                    if (luminance(b) < 0.1) {
                        dark_mirr++;
                    }
                }
            }
            fprintf(stderr, "[dbg] mirr dark=%u/%u top=%.4f/%.4f\n",
                    dark_norm, dark_mirr, luminance(top_norm),
                    luminance(top_mirr));
            TEST_CHECK(p_norm != NULL && p_mirr != NULL &&
                           dark_norm > 10 && dark_mirr > 10 &&
                           dark_norm <= dark_mirr + 4 &&
                           dark_mirr <= dark_norm + 4,
                       "mirrored: shadow silhouette matches");
            TEST_CHECK(p_norm != NULL && p_mirr != NULL &&
                           luminance(top_norm) > 0.4 &&
                           luminance(top_mirr) < 0.1,
                       "mirrored: main top differs (known debt)");
            free(p_norm);
            free(p_mirr);
        }
        lr_material_destroy(unlit);
        lr_material_destroy(mat);
    }

    /* ---- PART AH: imported model casts + receives ---- */
    {
        la_asset_manager *assets = NULL;
        la_asset_manager_desc adesc;
        la_model *model = NULL;
        char path[1024];
        lr_light light;
        lr_draw_item items[2];
        shadow_frame_ops ops;
        unsigned char *px;

        memset(&adesc, 0, sizeof(adesc));
        adesc.renderer = env.renderer;
        if (la_asset_manager_create(&adesc, &assets) != LA_SUCCESS) {
            printf("FAIL: asset manager\n");
            goto cleanup;
        }
        snprintf(path, sizeof(path), "%s/fixture.glb", LA_FIXTURE_DIR);
        if (la_model_load(assets, path, &model) != LA_SUCCESS) {
            printf("FAIL: fixture load: %s\n",
                   la_asset_manager_get_last_error(assets));
            la_asset_manager_destroy(assets);
            goto cleanup;
        }
        dir_shadow_light(&light, 0.5f, -1.0f, 0.2f, 3.0f, 1024, -1.0f,
                         -1.0f);
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        memset(&items[1], 0, sizeof(items[1]));
        lr_transform_identity(&items[1].transform);
        items[1].transform.position[1] = 2.5f;
        items[1].mesh = NULL;
        items[1].material = NULL;
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = items;
        ops.item_count = 1;
        memcpy(ops.ambient, black3, sizeof(black3));
        /* Model submits through the public assets path (its items
         * carry casts+receives); ground renders in the same frame. */
        {
            lc_command_encoder *enc = NULL;

            if (lc_begin_frame(env.swapchain) == LC_SUCCESS &&
                lc_swapchain_get_encoder(env.swapchain, &enc) ==
                    LC_SUCCESS &&
                lr_renderer_begin(env.renderer, &env.camera) ==
                    LR_SUCCESS &&
                lr_renderer_submit_light(env.renderer, &light) ==
                    LR_SUCCESS &&
                lr_renderer_submit(env.renderer, &items[0]) ==
                    LR_SUCCESS &&
                la_model_submit(model, env.renderer,
                                &items[1].transform) == LA_SUCCESS &&
                lr_renderer_render_shadows(env.renderer, enc) ==
                    LR_SUCCESS) {
                lr_render_stats st;

                lr_renderer_get_stats(env.renderer, &st);
                fprintf(stderr, "[dbg] imported draws=%u culled=%u\n",
                        st.shadow_draw_calls, st.shadow_casters_culled);
                TEST_CHECK(st.shadow_draw_calls >= 7,
                           "imported: all instances reach the map");
            } else {
                TEST_CHECK(0, "imported: all instances reach the map");
            }
            lr_renderer_end(env.renderer);
            lc_end_frame(env.swapchain);
        }
        /* Footprint darkness under the model. */
        {
            lr_draw_item mitems[2];
            shadow_frame_ops mops;
            lr_transform root;

            lr_transform_identity(&root);
            root.position[1] = 2.5f;
            shadow_item(&mitems[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
            memset(&mitems[1], 0, sizeof(mitems[1]));
            memset(&mops, 0, sizeof(mops));
            mops.lights = &light;
            mops.light_count = 1;
            mops.items = mitems;
            mops.item_count = 1;
            mops.model = model;
            mops.model_root = root;
            memcpy(mops.ambient, black3, sizeof(black3));
            /* Custom frame: ground via harness items, model via root.
             * Reuse render_pixels for ground, then submit model in a
             * dedicated frame below for stats; pixels from a combined
             * manual frame here. */
            (void)root;
            px = render_pixels(&env, &mops);
            if (px != NULL) {
                /* Fixture blades throw long shadows; scan a grid of
                 * ground rows (the exact blade rows depend on the
                 * fixture pose, so one band cannot aim at them). */
                uint32_t dark = 0;
                uint32_t k;
                uint32_t j;
                static const double rows[7] = { -1.5, -1.0, -0.5, 0.0,
                                                0.5, 1.0, 1.5 };

                for (j = 0; j < 7; j++) {
                    for (k = 0; k < 160; k++) {
                        double c[3];
                        uint32_t sx;
                        uint32_t sy;

                        world_to_pixel(&env, -4.0 + 0.05 * (double)k,
                                       0.01, rows[j], &sx, &sy);
                        pixel_at(px, SH_TW, sx, sy, c);
                        if (luminance(c) < 0.12) {
                            dark++;
                        }
                    }
                }
                fprintf(stderr, "[dbg] imported foot dark=%u\n", dark);
                TEST_CHECK(dark >= 60,
                           "imported: model footprint darkens ground");
                free(px);
            } else {
                TEST_CHECK(0, "imported: footprint frame reads back");
            }
        }
        test_wait_idle(env.device);
        la_model_destroy(model);
        la_asset_manager_destroy(assets);
    }

    /* ---- spot shadows: cone footprint, rim, border ---- */
    {
        lr_material *mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lr_light spot;
        lr_draw_item items[2];
        shadow_frame_ops ops;
        unsigned char *px;
        uint32_t ccx;
        uint32_t ccy;
        double core[3];
        double rim[3];
        double outside[3];

        spot_shadow_light(&spot, 0.0f, 7.0f, 3.0f, 0.0f, -0.75f, -0.32f,
                          100.0f, 20.0f, 0.3f, 0.45f, 1024);
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], ball, mat, 0.0f, 2.0f, 0.5f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &spot;
        ops.light_count = 1;
        ops.items = items;
        ops.item_count = 2;
        memcpy(ops.ambient, black3, sizeof(black3));
        /* Cone axis hits the ground near (0,0,-0.5) (occluder
         * shadow center); project nearby for the core sample. */
        world_to_pixel(&env, 0.0, 0.01, -0.4, &ccx, &ccy);
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "spot: cone frame reads back");
        if (px != NULL) {
            uint32_t rx;
            uint32_t ry;
            uint32_t ox;
            uint32_t oy;
            double lo = 1.0;
            double hi = 0.0;
            int partial = 0;
            int k;

            pixel_at(px, SH_TW, ccx, ccy, core);
            /* Rim: below the ball shadow, in the lit cone pool
             * (same column; the shadow bottom edge sits between). */
            rx = ccx;
            ry = (ccy + 45 < SH_TH) ? ccy + 45 : SH_TH - 1;
            pixel_at(px, SH_TW, rx, ry, rim);
            /* Far outside the cone. */
            world_to_pixel(&env, 6.0, 0.01, 4.0, &ox, &oy);
            pixel_at(px, SH_TW, ox, oy, outside);
            fprintf(stderr, "[dbg] spot core=%.4f rim=%.4f out=%.4f\n",
                    luminance(core), luminance(rim), luminance(outside));
            TEST_CHECK(luminance(core) < 0.15,
                       "spot: occluded cone core is dark");
            TEST_CHECK(luminance(outside) < 0.05,
                       "spot: outside cone stays ambient");
            TEST_CHECK(luminance(rim) > 0.2,
                       "spot: pool below the shadow stays lit");
            /* Rim column gradient: shadow (dark) -> pool (lit) crosses
             * the PCF edge with a soft partial band. */
            for (k = -10; k <= 60; k++) {
                int sy = (int)ccy + k;
                double c[3];
                double lum;

                if (sy < 0 || sy >= (int)SH_TH) {
                    continue;
                }
                pixel_at(px, SH_TW, ccx, (uint32_t)sy, c);
                lum = luminance(c);
                if (lum < lo) {
                    lo = lum;
                }
                if (lum > hi) {
                    hi = lum;
                }
            }
            for (k = -10; k <= 60; k++) {
                int sy = (int)ccy + k;
                double c[3];
                double lum;

                if (sy < 0 || sy >= (int)SH_TH) {
                    continue;
                }
                pixel_at(px, SH_TW, ccx, (uint32_t)sy, c);
                lum = luminance(c);
                if (hi - lo > 0.001 && lum > lo + 0.2 * (hi - lo) &&
                    lum < lo + 0.8 * (hi - lo)) {
                    partial = 1;
                }
            }
            TEST_CHECK(hi - lo > 0.25 && partial,
                       "spot: rim shows a soft partial band");
            free(px);
        }
        TEST_CHECK(ops.stats.shadow_casting_lights == 1 &&
                   ops.stats.shadow_passes == 1,
                   "spot: one shadow light/pass in stats");
        lr_material_destroy(mat);
    }

    /* ---- flow safety: prepare rules are loud ---- */
    {
        lr_camera cam;
        lr_light light;
        lr_draw_item item;

        lr_camera_init(&cam);
        lr_camera_set_perspective(&cam, 0.6f, 1.0f, 0.1f, 60.0f);
        lr_camera_look_at(&cam, eye, center, up);
        TEST_CHECK(lr_renderer_begin(env.renderer, &cam) == LR_SUCCESS,
                   "flow: frame opens");
        dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 2.0f, 1024, -1.0f,
                         -1.0f);
        TEST_CHECK(lr_renderer_submit_light(env.renderer, &light) ==
                       LR_SUCCESS,
                   "flow: shadow light submits");
        memset(&item, 0, sizeof(item));
        lr_transform_identity(&item.transform);
        item.mesh = cube;
        item.material = ground_mat;
        item.casts_shadow = 1;
        item.receives_shadow = 1;
        TEST_CHECK(lr_renderer_submit(env.renderer, &item) == LR_SUCCESS,
                   "flow: item submits");
        {
            lc_command_encoder *enc = NULL;

            if (lc_begin_frame(env.swapchain) == LC_SUCCESS &&
                lc_swapchain_get_encoder(env.swapchain, &enc) ==
                    LC_SUCCESS) {
                TEST_CHECK(lr_renderer_render_shadows(env.renderer, enc) ==
                               LR_SUCCESS,
                           "flow: prepare succeeds once");
                TEST_CHECK(lr_renderer_render_shadows(env.renderer, enc) ==
                               LR_ERROR_INVALID_ARGUMENT,
                           "flow: double prepare rejected");
                TEST_CHECK(lr_renderer_submit_light(env.renderer,
                                                    &light) ==
                               LR_ERROR_INVALID_ARGUMENT,
                           "flow: submit-after-prepare rejected (light)");
                TEST_CHECK(lr_renderer_submit(env.renderer, &item) ==
                               LR_ERROR_INVALID_ARGUMENT,
                           "flow: submit-after-prepare rejected (item)");
                lc_end_frame(env.swapchain);
            } else {
                TEST_CHECK(0, "flow: frame encoder available");
            }
        }
        lr_renderer_end(env.renderer);
        /* Rendering without prepare: shadowed lights go unshadowed
         * (safe), which the next block verifies by pixel. */
    }

    /* ---- no-prepare safety: footprint absent without prepare ---- */
    {
        lr_material *cube_mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lr_light light;
        lr_draw_item items[2];
        unsigned char *px;
        uint32_t cx;
        uint32_t cy;
        double c[3];

        dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 3.0f, 1024, -1.0f,
                         -1.0f);
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], cube, cube_mat, 0.0f, 1.5f, 0.0f);
        {
            /* Manual frame identical to the harness but skipping
             * prepare: the footprint must NOT appear. */
            lc_command_encoder *enc = NULL;
            lc_render_pass_desc pdesc;
            lc_render_color_attachment catt;
            lc_render_depth_attachment datt;

            px = NULL;
            if (lc_begin_frame(env.swapchain) == LC_SUCCESS &&
                lc_swapchain_get_encoder(env.swapchain, &enc) ==
                    LC_SUCCESS &&
                lr_renderer_begin(env.renderer, &env.camera) ==
                    LR_SUCCESS &&
                lr_renderer_submit_light(env.renderer, &light) ==
                    LR_SUCCESS &&
                lr_renderer_submit(env.renderer, &items[0]) ==
                    LR_SUCCESS &&
                lr_renderer_submit(env.renderer, &items[1]) ==
                    LR_SUCCESS) {
                lr_renderer_set_ambient(env.renderer, black3);
                memset(&catt, 0, sizeof(catt));
                catt.view = env.color_view;
                catt.load_op = LC_LOAD_OP_CLEAR;
                catt.store_op = LC_STORE_OP_STORE;
                memset(&pdesc, 0, sizeof(pdesc));
                pdesc.color_attachments = &catt;
                pdesc.color_attachment_count = 1;
                pdesc.width = env.tw;
                pdesc.height = env.th;
                memset(&datt, 0, sizeof(datt));
                datt.view = env.depth_view;
                datt.depth_load_op = LC_LOAD_OP_CLEAR;
                datt.depth_store_op = LC_STORE_OP_DONT_CARE;
                datt.clear_depth = 1.0f;
                datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
                datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
                pdesc.depth_attachment = &datt;
                if (lc_encoder_begin_render_pass(enc, &pdesc) ==
                        LC_SUCCESS &&
                    lr_renderer_render(env.renderer, enc, env.target) ==
                        LR_SUCCESS &&
                    lc_encoder_end_render_pass(enc) == LC_SUCCESS) {
                    lr_renderer_end(env.renderer);
                    {
                        lc_render_swapchain_pass_desc spass;

                        memset(&spass, 0, sizeof(spass));
                        spass.color_load_op = LC_LOAD_OP_CLEAR;
                        spass.color_store_op = LC_STORE_OP_STORE;
                        spass.depth_load_op = LC_LOAD_OP_CLEAR;
                        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
                        spass.clear_depth = 1.0f;
                        if (lc_encoder_begin_swapchain_pass(
                                enc, env.swapchain, &spass) ==
                            LC_SUCCESS) {
                            lc_encoder_end_render_pass(enc);
                        }
                    }
                    if (lc_end_frame(env.swapchain) == LC_SUCCESS) {
                        px = readback_rgba8(env.device, env.color_img,
                                            env.tw, env.th);
                    }
                } else {
                    lr_renderer_end(env.renderer);
                }
            }
        }
        world_to_pixel(&env, 0.7, 0.01, 0.7, &cx, &cy);
        if (px != NULL) {
            pixel_at(px, SH_TW, cx, cy, c);
            TEST_CHECK(luminance(c) > 0.2,
                       "noprepare: footprint absent without prepare");
            free(px);
        } else {
            TEST_CHECK(0, "noprepare: frame reads back");
        }
        lr_material_destroy(cube_mat);
    }

    /* ---- over-capacity: first 4 shadow lights win ---- */
    {
        lr_material *cube_mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lr_light lights[5];
        lr_draw_item items[2];
        shadow_frame_ops ops;
        unsigned char *px;
        uint32_t cx;
        uint32_t cy;
        double c[3];
        int i;

        for (i = 0; i < 5; i++) {
            dir_shadow_light(&lights[i], 0.0f, -1.0f, 0.0f, 0.5f, 1024,
                             -1.0f, -1.0f);
        }
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], cube, cube_mat, 0.0f, 1.5f, 0.0f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = lights;
        ops.light_count = 5;
        ops.items = items;
        ops.item_count = 2;
        memcpy(ops.ambient, black3, sizeof(black3));
        /* Visible ground inside the vertical footprint (under-cube
         * shows the bottom face, never ground). */
        world_to_pixel(&env, 0.7, 0.01, 0.7, &cx, &cy);
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "overcap: 5-light frame reads back");
        if (px != NULL) {
            /* Four shadowed (contribute ~0 at core) + fifth full:
             * center ~ one unshadowed 0.5 light on dielectric. */
            double want_lo = 0.03;
            double want_hi = 0.20;

            pixel_at(px, SH_TW, cx, cy, c);
            fprintf(stderr, "[dbg] overcap lum=%.4f draws=%u\n",
                    luminance(c), ops.stats.shadow_draw_calls);
            TEST_CHECK(luminance(c) > want_lo && luminance(c) < want_hi,
                       "overcap: fifth light renders unshadowed");
            free(px);
        }
        TEST_CHECK(ops.stats.shadow_casting_lights == 4,
                   "overcap: exactly 4 shadow slots assigned");
        lr_material_destroy(cube_mat);
    }

    /* ---- PART AQ/AR: resolution switches + main resize ---- */
    {
        lr_material *cube_mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lr_light light;
        lr_draw_item items[2];
        shadow_frame_ops ops;
        uint32_t pipes_before;
        static const uint32_t ress[3] = { 256, 512, 256 };
        int ri;

        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], cube, cube_mat, 0.0f, 1.5f, 0.0f);
        pipes_before = lr_renderer_get_pipeline_count(env.renderer);
        for (ri = 0; ri < 3; ri++) {
            lr_shadow_slot_info info;
            unsigned char *px;
            uint32_t cx;
            uint32_t cy;
            double c[3];

            dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 3.0f, ress[ri],
                             -1.0f, -1.0f);
            memset(&ops, 0, sizeof(ops));
            ops.lights = &light;
            ops.light_count = 1;
            ops.items = items;
            ops.item_count = 2;
            memcpy(ops.ambient, black3, sizeof(black3));
            world_to_pixel(&env, 0.0, 0.01, 0.0, &cx, &cy);
            px = render_pixels(&env, &ops);
            lr_renderer_get_shadow_slot_info(env.renderer, 0, &info);
            if (px != NULL) {
                pixel_at(px, SH_TW, cx, cy, c);
                TEST_CHECK(info.resolution == ress[ri] &&
                           luminance(c) < 0.05,
                           "resolution: slot follows 256/512/256");
                free(px);
            } else {
                TEST_CHECK(0, "resolution: frame reads back");
            }
        }
        TEST_CHECK(lr_renderer_get_pipeline_count(env.renderer) ==
                       pipes_before,
                   "resolution: no pipeline growth on switches");
        /* Main-target resize must not touch shadow slots. */
        {
            lc_image_view *view_before =
                lr_renderer_get_shadow_view(env.renderer, 0);
            uint32_t pipes_pre = lr_renderer_get_pipeline_count(
                env.renderer);

            test_wait_idle(env.device);
            destroy_target(&env);
            if (make_target(&env, LC_FORMAT_RGBA8_UNORM, 384, 384, 1) !=
                0) {
                TEST_CHECK(0, "resize: 384 target recreates");
            } else {
                unsigned char *px;
                uint32_t cx;
                uint32_t cy;
                double c[3];
                lr_shadow_slot_info info;

                /* Viewport-only change: keep the slot resolution so
                 * the slot must survive untouched. */
                dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 3.0f, 256,
                                 -1.0f, -1.0f);
                memset(&ops, 0, sizeof(ops));
                ops.lights = &light;
                ops.light_count = 1;
                ops.items = items;
                ops.item_count = 2;
                memcpy(ops.ambient, black3, sizeof(black3));
                world_to_pixel(&env, 0.0, 0.01, 0.0, &cx, &cy);
                px = render_pixels(&env, &ops);
                lr_renderer_get_shadow_slot_info(env.renderer, 0, &info);
                TEST_CHECK(lr_renderer_get_shadow_view(env.renderer,
                                                       0) == view_before &&
                           info.resolution == 256,
                           "resize: shadow slot untouched by viewport");
                if (px != NULL) {
                    pixel_at(px, 384, cx, cy, c);
                    TEST_CHECK(luminance(c) < 0.05,
                               "resize: footprint correct at 384");
                    free(px);
                } else {
                    TEST_CHECK(0, "resize: frame reads back");
                }
                TEST_CHECK(lr_renderer_get_pipeline_count(env.renderer) ==
                               pipes_pre,
                           "resize: viewport resize reuses pipelines");
                /* Back to 256 for the remaining blocks. */
                test_wait_idle(env.device);
                destroy_target(&env);
                make_target(&env, LC_FORMAT_RGBA8_UNORM, SH_TW, SH_TH,
                            1);
            }
        }
        lr_material_destroy(cube_mat);
    }

    /* ---- PART AS: one shadow render, two viewports ---- */
    {
        lr_material *cube_mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lc_image *img_b = NULL;
        lc_image_view *view_b = NULL;
        lc_render_target *target_b = NULL;
        lc_image *img_bd = NULL;
        lc_image_view *view_bd = NULL;
        lr_light light;
        lr_draw_item items[2];
        int ok = 1;

        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], cube, cube_mat, 0.0f, 1.5f, 0.0f);
        dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 3.0f, 1024, -1.0f,
                         -1.0f);
        /* Second viewport target (128, same format). */
        {
            lc_image_desc idesc;
            lc_image_view_desc vdesc;
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment att;

            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = 128;
            idesc.height = 128;
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
            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = 128;
            tdesc.height = 128;
            if (lc_image_create(env.device, &idesc, &img_b) !=
                    LC_SUCCESS ||
                lc_image_view_create(img_b, &vdesc, &view_b) !=
                    LC_SUCCESS) {
                ok = 0;
            } else {
                /* Viewport B depth (matches the depth-tested main
                 * pipeline; painter order is undefined without it). */
                lc_image_desc ddesc;
                lc_image_view_desc dvdesc;

                memset(&ddesc, 0, sizeof(ddesc));
                ddesc.type = LC_IMAGE_TYPE_2D;
                ddesc.format = LC_FORMAT_D32_FLOAT;
                ddesc.width = 128;
                ddesc.height = 128;
                ddesc.depth = 1;
                ddesc.mip_levels = 1;
                ddesc.array_layers = 1;
                ddesc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL;
                ddesc.samples = LC_SAMPLE_COUNT_1;
                memset(&dvdesc, 0, sizeof(dvdesc));
                dvdesc.type = LC_IMAGE_VIEW_2D;
                dvdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
                dvdesc.mip_level_count = 1;
                dvdesc.array_layer_count = 1;
                if (lc_image_create(env.device, &ddesc, &img_bd) !=
                        LC_SUCCESS ||
                    lc_image_view_create(img_bd, &dvdesc, &view_bd) !=
                        LC_SUCCESS) {
                    ok = 0;
                } else {
                    att.view = view_b;
                    tdesc.color_attachments = &att;
                    tdesc.color_attachment_count = 1;
                    tdesc.depth_stencil_attachment = view_bd;
                    if (lc_render_target_create(env.device, &tdesc,
                                                &target_b) != LC_SUCCESS) {
                        ok = 0;
                    }
                }
            }
        }
        if (ok) {
            lc_command_encoder *enc = NULL;

            if (lc_begin_frame(env.swapchain) == LC_SUCCESS &&
                lc_swapchain_get_encoder(env.swapchain, &enc) ==
                    LC_SUCCESS &&
                lr_renderer_begin(env.renderer, &env.camera) ==
                    LR_SUCCESS &&
                lr_renderer_submit_light(env.renderer, &light) ==
                    LR_SUCCESS &&
                lr_renderer_submit(env.renderer, &items[0]) ==
                    LR_SUCCESS &&
                lr_renderer_submit(env.renderer, &items[1]) ==
                    LR_SUCCESS &&
                lr_renderer_render_shadows(env.renderer, enc) ==
                    LR_SUCCESS) {
                /* Viewport A (256) and viewport B (128) share ONE
                 * prepare: the shadow maps render once, both main
                 * renders sample them (PART AS reuse). */
                lc_render_pass_desc pdesc;
                lc_render_color_attachment catt;
                lc_render_depth_attachment datt;
                lr_render_stats sta;
                lr_render_stats stb;

                memset(&catt, 0, sizeof(catt));
                catt.view = env.color_view;
                catt.load_op = LC_LOAD_OP_CLEAR;
                catt.store_op = LC_STORE_OP_STORE;
                memset(&pdesc, 0, sizeof(pdesc));
                pdesc.color_attachments = &catt;
                pdesc.color_attachment_count = 1;
                pdesc.width = env.tw;
                pdesc.height = env.th;
                memset(&datt, 0, sizeof(datt));
                datt.view = env.depth_view;
                datt.depth_load_op = LC_LOAD_OP_CLEAR;
                datt.depth_store_op = LC_STORE_OP_DONT_CARE;
                datt.clear_depth = 1.0f;
                datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
                datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
                pdesc.depth_attachment = &datt;
                if (lc_encoder_begin_render_pass(enc, &pdesc) !=
                        LC_SUCCESS ||
                    lr_renderer_render(env.renderer, enc, env.target) !=
                        LR_SUCCESS ||
                    lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                    ok = 0;
                }
                lr_renderer_get_stats(env.renderer, &sta);
                memset(&catt, 0, sizeof(catt));
                catt.view = view_b;
                catt.load_op = LC_LOAD_OP_CLEAR;
                catt.store_op = LC_STORE_OP_STORE;
                memset(&pdesc, 0, sizeof(pdesc));
                pdesc.color_attachments = &catt;
                pdesc.color_attachment_count = 1;
                pdesc.width = 128;
                pdesc.height = 128;
                memset(&datt, 0, sizeof(datt));
                datt.view = view_bd;
                datt.depth_load_op = LC_LOAD_OP_CLEAR;
                datt.depth_store_op = LC_STORE_OP_DONT_CARE;
                datt.clear_depth = 1.0f;
                datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
                datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
                pdesc.depth_attachment = &datt;
                if (ok && lc_encoder_begin_render_pass(enc, &pdesc) ==
                              LC_SUCCESS &&
                    lr_renderer_render(env.renderer, enc, target_b) ==
                        LR_SUCCESS &&
                    lc_encoder_end_render_pass(enc) == LC_SUCCESS) {
                    ok = 1;
                } else {
                    ok = 0;
                }
                lr_renderer_get_stats(env.renderer, &stb);
                /* Second render re-ran the same queue: draws double,
                 * shadow maps did not (one prepare). */
                if (stb.draw_calls != sta.draw_calls * 2 ||
                    stb.shadow_maps_rendered != 1) {
                    ok = 0;
                }
                lr_renderer_end(env.renderer);
                /* Present leg. */
                {
                    lc_render_swapchain_pass_desc spass;

                    memset(&spass, 0, sizeof(spass));
                    spass.color_load_op = LC_LOAD_OP_CLEAR;
                    spass.color_store_op = LC_STORE_OP_STORE;
                    spass.depth_load_op = LC_LOAD_OP_CLEAR;
                    spass.depth_store_op = LC_STORE_OP_DONT_CARE;
                    spass.clear_depth = 1.0f;
                    if (lc_encoder_begin_swapchain_pass(
                            enc, env.swapchain, &spass) == LC_SUCCESS) {
                        lc_encoder_end_render_pass(enc);
                    }
                }
                if (lc_end_frame(env.swapchain) != LC_SUCCESS) {
                    ok = 0;
                }
            } else {
                ok = 0;
            }
            TEST_CHECK(ok, "multiview: two viewports share one prepare");
            if (ok) {
                unsigned char *px = readback_rgba8(env.device,
                                                   env.color_img, env.tw,
                                                   env.th);
                unsigned char *pxb = readback_rgba8(env.device, img_b,
                                                    128, 128);

                if (px != NULL && pxb != NULL) {
                    uint32_t cx;
                    uint32_t cy;
                    double a[3];
                    double b[3];

                    world_to_pixel(&env, 0.0, 0.01, 0.0, &cx, &cy);
                    pixel_at(px, SH_TW, cx, cy, a);
                    pixel_at(pxb, 128, cx / 2, cy / 2, b);
                    TEST_CHECK(luminance(a) < 0.05 &&
                                   luminance(b) < 0.08,
                               "multiview: both viewports shadowed");
                    free(px);
                    free(pxb);
                } else {
                    TEST_CHECK(0, "multiview: readbacks available");
                    free(px);
                    free(pxb);
                }
            }
        } else {
            TEST_CHECK(0, "multiview: second target creates");
        }
        test_wait_idle(env.device);
        lc_render_target_destroy(target_b);
        lc_image_view_destroy(view_b);
        lc_image_destroy(img_b);
        lc_image_view_destroy(view_bd);
        lc_image_destroy(img_bd);
        lr_material_destroy(cube_mat);
    }

    /* ---- PART AT: perf scene (100 objects, 2 shadow lights) ---- */
    {
        lr_material *shared = make_pbr(&env, gray, 0.2f, 0.6f);
        lr_mesh *pebble = NULL;
        lr_light dir;
        lr_light spot;
        lr_light both[2];
        lr_draw_item items[101];
        shadow_frame_ops ops;
        unsigned char *px;
        int gx;
        int gz;

        if (lr_mesh_create_sphere(env.renderer, 0.35f, 12, 8, &pebble) !=
            LR_SUCCESS) {
            /* Non-fatal: a transient device-side allocation failure
             * must not abort the rest of the suite (no summary). */
            TEST_CHECK(0, "perf: pebble mesh creates");
            lr_material_destroy(shared);
        } else {
        dir_shadow_light(&dir, 0.3f, -1.0f, 0.15f, 2.0f, 1024, -1.0f,
                         -1.0f);
        spot_shadow_light(&spot, 0.0f, 8.0f, 4.0f, 0.0f, -0.8f, -0.4f,
                          80.0f, 25.0f, 0.35f, 0.6f, 1024);
        both[0] = dir;
        both[1] = spot;
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        for (gx = 0; gx < 10; gx++) {
            for (gz = 0; gz < 10; gz++) {
                lr_draw_item *it = &items[1 + gx * 10 + gz];

                shadow_item(it, pebble, shared,
                            -4.5f + (float)gx * 1.0f, 0.35f,
                            -4.5f + (float)gz * 1.0f);
            }
        }
        memset(&ops, 0, sizeof(ops));
        ops.lights = both;
        ops.light_count = 2;
        ops.items = items;
        ops.item_count = 101;
        memcpy(ops.ambient, black3, sizeof(black3));
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "perf: 100-object frame reads back");
        if (px != NULL) {
            free(px);
        }
        /* Conservation: every caster is drawn or culled per light. */
        TEST_CHECK(ops.stats.submitted_objects == 101,
                   "perf: 101 objects submitted");
        TEST_CHECK(ops.stats.shadow_draw_calls +
                           ops.stats.shadow_casters_culled ==
                       202,
                   "perf: shadow draws + culled = 2 x casters");
        TEST_CHECK(ops.stats.shadow_draw_calls > 100,
                   "perf: both lights drew casters");
        TEST_CHECK(ops.stats.material_binds <= 6,
                   "perf: shared material stays bound");
        printf("[INFO] perf: main draws=%u tris=%u shadow draws=%u "
               "culled=%u shadow tris=%u\n",
               ops.stats.draw_calls, ops.stats.triangles,
               ops.stats.shadow_draw_calls,
               ops.stats.shadow_casters_culled,
               ops.stats.shadow_triangles);
        lr_mesh_destroy(pebble);
        lr_material_destroy(shared);
        }
    }

    /* ---- PART AI: debug view samples the real map ---- */
    {
        lr_material *cube_mat = make_pbr(&env, gray, 0.0f, 0.8f);
        lr_material *debug_mat = NULL;
        lr_mesh *quad = NULL;
        lr_light light;
        lr_draw_item items[2];
        shadow_frame_ops ops;
        unsigned char *px;
        lc_image_view *dbg = NULL;

        dir_shadow_light(&light, 0.0f, -1.0f, 0.0f, 3.0f, 1024, -1.0f,
                         -1.0f);
        shadow_item(&items[0], ground, ground_mat, 0.0f, 0.0f, 0.0f);
        shadow_item(&items[1], cube, cube_mat, 0.0f, 1.5f, 0.0f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = items;
        ops.item_count = 2;
        memcpy(ops.ambient, black3, sizeof(black3));
        px = render_pixels(&env, &ops);
        if (px != NULL) {
            free(px);
        }
        dbg = lr_renderer_get_shadow_view(env.renderer, 0);
        TEST_CHECK(dbg != NULL, "debugview: slot 0 view borrows");
        if (dbg != NULL && lr_mesh_create_plane(env.renderer, 2.0f, 2.0f,
                                                &quad) == LR_SUCCESS) {
            /* Unlit quad textured with the depth view: .r carries
             * depth (g/b undefined for depth formats — assert R). */
            lr_unlit_material_desc udesc;
            lr_draw_item ditem;
            shadow_frame_ops dops;
            unsigned char *dpx;
            uint32_t qx;
            uint32_t qy;
            double c[3];

            memset(&udesc, 0, sizeof(udesc));
            udesc.color[0] = 1.0f;
            udesc.color[1] = 1.0f;
            udesc.color[2] = 1.0f;
            udesc.color[3] = 1.0f;
            udesc.base_color_texture = dbg;
            if (lr_material_create_unlit(env.renderer, &udesc,
                                         &debug_mat) == LR_SUCCESS) {
                lr_camera qcam;
                lr_camera saved = env.camera;
                static const float qeye[3] = { 0.0f, 0.0f, 3.0f };
                static const float qc[3] = { 0.0f, 0.0f, 0.0f };
                static const float qup[3] = { 0.0f, 1.0f, 0.0f };

                lr_camera_init(&qcam);
                lr_camera_set_perspective(&qcam, 0.6f, 1.0f, 0.1f,
                                          60.0f);
                lr_camera_look_at(&qcam, qeye, qc, qup);
                env.camera = qcam;
                memset(&ditem, 0, sizeof(ditem));
                lr_transform_identity(&ditem.transform);
                {
                    /* Face the quad toward the debug camera. */
                    static const float xaxis[3] = { 1.0f, 0.0f, 0.0f };

                    lr_quat_from_axis_angle(xaxis, 1.5707963f,
                                            ditem.transform.rotation);
                }
                ditem.mesh = quad;
                ditem.material = debug_mat;
                memset(&dops, 0, sizeof(dops));
                dops.items = &ditem;
                dops.item_count = 1;
                memcpy(dops.ambient, black3, sizeof(black3));
                dpx = render_pixels(&env, &dops);
                env.camera = saved;
                if (dpx != NULL) {
                    /* The debug quad fills the view; its R is shadow
                     * depth in [0,1] and varies (map is not flat). */
                    double lo = 1.0;
                    double hi = 0.0;
                    int x;
                    int y;

                    pixel_at(dpx, SH_TW, 128, 128, c);
                    for (y = 32; y < 224; y += 8) {
                        for (x = 32; x < 224; x += 8) {
                            double s[3];

                            pixel_at(dpx, SH_TW, (uint32_t)x,
                                     (uint32_t)y, s);
                            if (s[0] < lo) {
                                lo = s[0];
                            }
                            if (s[0] > hi) {
                                hi = s[0];
                            }
                        }
                    }
                    TEST_CHECK(hi - lo > 0.05,
                               "debugview: depth content varies");
                    free(dpx);
                } else {
                    TEST_CHECK(0, "debugview: quad frame reads back");
                }
                lr_material_destroy(debug_mat);
            } else {
                TEST_CHECK(0, "debugview: unlit debug material creates");
            }
            lr_mesh_destroy(quad);
        } else {
            TEST_CHECK(0, "debugview: resources ready");
        }
        lr_material_destroy(cube_mat);
    }

    /* ---- PART AU: 500-frame endurance ---- */
    {
        lr_mesh *esphere = NULL;
        lr_mesh *eground = NULL;
        lr_mesh *ewall = NULL;
        lr_mesh *ecube = NULL;
        lr_material *emats[4];
        lr_material *ewall_mat = NULL;
        lr_material *eglow_mat = NULL;
        lr_material *eground_mat = NULL;
        la_asset_manager *assets = NULL;
        la_model *emodel = NULL;
        uint32_t pipes_warm = 0;
        uint32_t frame;
        int ok = 1;
        char path[1024];
        int i;
        static const float yaxis[3] = { 0.0f, 1.0f, 0.0f };
        static const float xaxis[3] = { 1.0f, 0.0f, 0.0f };

        for (i = 0; i < 4; i++) {
            emats[i] = NULL;
        }
        destroy_target(&env);
        if (make_target(&env, LC_FORMAT_RGBA8_UNORM, 512, 512, 1) != 0) {
            TEST_CHECK(0, "endurance: 512 scene target creates");
            goto cleanup;
        }
        if (lr_mesh_create_sphere(env.renderer, 0.5f, 24, 12, &esphere) !=
                LR_SUCCESS ||
            lr_mesh_create_plane(env.renderer, 12.0f, 12.0f, &eground) !=
                LR_SUCCESS ||
            lr_mesh_create_plane(env.renderer, 2.0f, 2.0f, &ewall) !=
                LR_SUCCESS ||
            lr_mesh_create_cube(env.renderer, 0.6f, &ecube) !=
                LR_SUCCESS) {
            TEST_CHECK(0, "endurance: meshes create");
            goto cleanup;
        }
        {
            static const float mets[4] = { 0.0f, 1.0f, 0.0f, 1.0f };
            static const float rghs[4] = { 0.15f, 0.15f, 0.8f, 0.8f };
            int k;

            for (k = 0; k < 4; k++) {
                lr_pbr_material_desc desc;

                memset(&desc, 0, sizeof(desc));
                desc.base_color_factor[0] = 0.85f;
                desc.base_color_factor[1] = 0.85f;
                desc.base_color_factor[2] = 0.9f;
                desc.base_color_factor[3] = 1.0f;
                desc.metallic_factor = mets[k];
                desc.roughness_factor = rghs[k];
                desc.normal_scale = 1.0f;
                desc.occlusion_strength = 1.0f;
                if (lr_material_create_pbr(env.renderer, &desc,
                                           &emats[k]) != LR_SUCCESS) {
                    ok = 0;
                }
            }
            {
                lr_pbr_material_desc desc;
                lr_unlit_material_desc udesc;

                memset(&desc, 0, sizeof(desc));
                desc.base_color_factor[0] = 0.7f;
                desc.base_color_factor[1] = 0.7f;
                desc.base_color_factor[2] = 0.72f;
                desc.base_color_factor[3] = 1.0f;
                desc.roughness_factor = 0.6f;
                desc.normal_scale = 1.0f;
                desc.occlusion_strength = 1.0f;
                desc.double_sided = 1;
                if (lr_material_create_pbr(env.renderer, &desc,
                                           &ewall_mat) != LR_SUCCESS) {
                    ok = 0;
                }
                memset(&desc, 0, sizeof(desc));
                desc.base_color_factor[3] = 1.0f;
                desc.roughness_factor = 0.9f;
                desc.emissive_factor[0] = 1.0f;
                desc.emissive_factor[1] = 0.5f;
                desc.emissive_factor[2] = 0.15f;
                desc.normal_scale = 1.0f;
                desc.occlusion_strength = 1.0f;
                if (lr_material_create_pbr(env.renderer, &desc,
                                           &eglow_mat) != LR_SUCCESS) {
                    ok = 0;
                }
                memset(&udesc, 0, sizeof(udesc));
                udesc.color[0] = 0.15f;
                udesc.color[1] = 0.16f;
                udesc.color[2] = 0.19f;
                udesc.color[3] = 1.0f;
                if (lr_material_create_unlit(env.renderer, &udesc,
                                             &eground_mat) != LR_SUCCESS) {
                    ok = 0;
                }
            }
        }
        TEST_CHECK(ok, "endurance: scene materials create");
        {
            la_asset_manager_desc adesc;

            memset(&adesc, 0, sizeof(adesc));
            adesc.renderer = env.renderer;
            if (la_asset_manager_create(&adesc, &assets) != LA_SUCCESS) {
                ok = 0;
            } else {
                snprintf(path, sizeof(path), "%s/fixture.glb",
                         LA_FIXTURE_DIR);
                if (la_model_load(assets, path, &emodel) != LA_SUCCESS) {
                    ok = 0;
                }
            }
        }
        TEST_CHECK(ok, "endurance: imported model loads");
        if (!ok) {
            goto cleanup;
        }
        for (frame = 0; frame < 500 && ok; frame++) {
            lc_command_encoder *enc = NULL;
            lc_render_pass_desc pdesc;
            lc_render_color_attachment catt;
            lc_render_depth_attachment datt;
            lc_render_swapchain_pass_desc spass;
            lr_draw_item item;
            lr_light key;
            lr_light fill;
            lr_light cone;
            float t = (float)frame * 0.02f;
            float eye[3];
            float tgt[3] = { 0.0f, 0.8f, 0.0f };
            int k;

            if (frame == 250) {
                /* Main-target resize (shadow slots untouched). */
                test_wait_idle(env.device);
                destroy_target(&env);
                if (make_target(&env, LC_FORMAT_RGBA8_UNORM, 384, 384,
                                1) != 0) {
                    ok = 0;
                    break;
                }
            }
            if (frame == 300) {
                /* Controlled shadow-resolution change is expressed
                 * per submitted light (1024 -> 512 from here on).
                 * Drain first: prepare will destroy/recreate the slot
                 * image while the previous frame may still sample
                 * it (GPU fault / TDR otherwise). */
                test_wait_idle(env.device);
            }
            lc_poll_events();
            /* Compositor loss mid-run must not kill 500 frames:
             * same recreate-and-retry policy as the harness. */
            {
                lc_result bres = lc_begin_frame(env.swapchain);

                if (bres == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                    uint32_t w = lc_window_get_width(env.window);
                    uint32_t h = lc_window_get_height(env.window);

                    if (w == 0 || h == 0 ||
                        lc_swapchain_recreate(env.swapchain, w, h) !=
                            LC_SUCCESS ||
                        lc_begin_frame(env.swapchain) != LC_SUCCESS) {
                        ok = 0;
                        break;
                    }
                } else if (bres != LC_SUCCESS) {
                    ok = 0;
                    break;
                }
            }
            if (lc_swapchain_get_encoder(env.swapchain, &enc) !=
                    LC_SUCCESS) {
                ok = 0;
                break;
            }
            /* Main-pass descriptors are filled here but the pass
             * opens AFTER prepare below (passes cannot nest: the
             * shadow depth passes must run first). */
            memset(&catt, 0, sizeof(catt));
            catt.view = env.color_view;
            catt.load_op = LC_LOAD_OP_CLEAR;
            catt.store_op = LC_STORE_OP_STORE;
            memset(&pdesc, 0, sizeof(pdesc));
            pdesc.color_attachments = &catt;
            pdesc.color_attachment_count = 1;
            pdesc.width = env.tw;
            pdesc.height = env.th;
            memset(&datt, 0, sizeof(datt));
            datt.view = env.depth_view;
            datt.depth_load_op = LC_LOAD_OP_CLEAR;
            datt.depth_store_op = LC_STORE_OP_DONT_CARE;
            datt.clear_depth = 1.0f;
            datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
            datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
            pdesc.depth_attachment = &datt;
            eye[0] = 4.2f * sinf(t * 0.25f);
            eye[1] = 2.1f + 0.4f * sinf(t * 0.5f);
            eye[2] = 4.2f * cosf(t * 0.25f);
            if (lr_camera_look_at(&env.camera, eye, tgt, up) !=
                    LR_SUCCESS ||
                lr_renderer_begin(env.renderer, &env.camera) !=
                    LR_SUCCESS) {
                ok = 0;
                break;
            }
            /* Rotating directional shadow light. */
            memset(&key, 0, sizeof(key));
            key.type = LR_LIGHT_DIRECTIONAL;
            key.color[0] = 1.0f;
            key.color[1] = 0.95f;
            key.color[2] = 0.88f;
            key.intensity = 2.5f;
            key.direction[0] = 0.55f * cosf(t * 0.3f);
            key.direction[1] = -1.0f;
            key.direction[2] = 0.55f * sinf(t * 0.3f);
            key.shadow.enabled = 1;
            key.shadow.resolution = (frame >= 300) ? 512 : 1024;
            key.shadow.depth_bias = -1.0f;
            key.shadow.normal_bias = -1.0f;
            memset(&fill, 0, sizeof(fill));
            fill.type = LR_LIGHT_POINT;
            fill.color[0] = 1.0f;
            fill.color[1] = 0.75f;
            fill.color[2] = 0.55f;
            fill.intensity = 20.0f;
            fill.position[0] = 2.5f;
            fill.position[1] = 2.5f;
            fill.position[2] = 2.0f;
            fill.range = 12.0f;
            /* Moving spot shadow light. */
            memset(&cone, 0, sizeof(cone));
            cone.type = LR_LIGHT_SPOT;
            cone.color[0] = 0.5f;
            cone.color[1] = 0.65f;
            cone.color[2] = 1.0f;
            cone.intensity = 60.0f;
            cone.position[0] = 3.0f * cosf(t * 0.9f);
            cone.position[1] = 3.2f;
            cone.position[2] = 3.0f * sinf(t * 0.9f);
            cone.range = 14.0f;
            cone.direction[0] = -cone.position[0];
            cone.direction[1] = -2.4f;
            cone.direction[2] = -cone.position[2];
            cone.spot_inner = 0.25f;
            cone.spot_outer = 0.5f;
            cone.shadow.enabled = 1;
            cone.shadow.resolution = 512;
            cone.shadow.depth_bias = -1.0f;
            cone.shadow.normal_bias = -1.0f;
            if (lr_renderer_submit_light(env.renderer, &key) !=
                    LR_SUCCESS ||
                lr_renderer_submit_light(env.renderer, &fill) !=
                    LR_SUCCESS ||
                lr_renderer_submit_light(env.renderer, &cone) !=
                    LR_SUCCESS) {
                ok = 0;
                break;
            }
            memset(&item, 0, sizeof(item));
            item.mesh = esphere;
            for (k = 0; k < 4 && ok; k++) {
                lr_transform_identity(&item.transform);
                item.transform.position[0] = ((float)(k % 2) - 0.5f) *
                                             2.7f;
                item.transform.position[1] = 1.9f - (float)(k / 2) *
                                             1.7f;
                item.transform.position[2] = -1.5f;
                item.material = emats[k];
                item.casts_shadow = 1;
                item.receives_shadow = 1;
                if (lr_renderer_submit(env.renderer, &item) !=
                    LR_SUCCESS) {
                    ok = 0;
                }
            }
            lr_transform_identity(&item.transform);
            item.transform.position[0] = -2.6f;
            item.transform.position[1] = 1.2f;
            item.transform.position[2] = 0.4f;
            lr_quat_from_axis_angle(xaxis, 1.5707963f,
                                    item.transform.rotation);
            item.mesh = ewall;
            item.material = ewall_mat;
            item.casts_shadow = 1;
            item.receives_shadow = 1;
            if (lr_renderer_submit(env.renderer, &item) != LR_SUCCESS) {
                ok = 0;
            }
            lr_transform_identity(&item.transform);
            item.transform.position[0] = 2.6f + 0.8f * sinf(t * 1.3f);
            item.transform.position[1] = 0.5f;
            item.transform.position[2] = 0.4f;
            lr_quat_from_axis_angle(yaxis, t, item.transform.rotation);
            item.mesh = ecube;
            item.material = eglow_mat;
            item.casts_shadow = 1;
            item.receives_shadow = 1;
            if (lr_renderer_submit(env.renderer, &item) != LR_SUCCESS) {
                ok = 0;
            }
            {
                lr_transform root;

                lr_transform_identity(&root);
                root.position[0] = 0.0f;
                root.position[1] = 0.1f;
                root.position[2] = 1.4f;
                lr_quat_from_axis_angle(yaxis, t * 0.7f, root.rotation);
                if (la_model_submit(emodel, env.renderer, &root) !=
                    LA_SUCCESS) {
                    ok = 0;
                }
            }
            lr_transform_identity(&item.transform);
            item.transform.position[1] = -0.55f;
            item.mesh = eground;
            item.material = eground_mat;
            item.casts_shadow = 1;
            item.receives_shadow = 0;
            if (lr_renderer_submit(env.renderer, &item) != LR_SUCCESS) {
                ok = 0;
            }
            {
                lr_result pr;
                lr_result rr;

                pr = lr_renderer_render_shadows(env.renderer, enc);
                if (pr != LR_SUCCESS) {
                    fprintf(stderr,
                            "[dbg] endurance pass f=%u prepare=%d "
                            "render=%d\n",
                            frame, (int)pr, -1);
                    ok = 0;
                } else if (lc_encoder_begin_render_pass(enc, &pdesc) !=
                               LC_SUCCESS) {
                    fprintf(stderr,
                            "[dbg] endurance pass f=%u main begin "
                            "failed\n",
                            frame);
                    ok = 0;
                } else {
                    rr = lr_renderer_render(env.renderer, enc,
                                            env.target);
                    if (rr != LR_SUCCESS) {
                        fprintf(stderr,
                                "[dbg] endurance pass f=%u prepare=%d "
                                "render=%d\n",
                                frame, (int)pr, (int)rr);
                        ok = 0;
                    }
                }
            }
            {
                lr_render_stats stats;

                lr_renderer_get_stats(env.renderer, &stats);
                if (stats.active_lights != 3 ||
                    stats.shadow_casting_lights != 2) {
                    fprintf(stderr,
                            "[dbg] endurance stats f=%u lights=%u "
                            "shadow=%u\n",
                            frame, stats.active_lights,
                            stats.shadow_casting_lights);
                    ok = 0;
                    break;
                }
                if (frame == 350) {
                    lr_shadow_slot_info info;

                    pipes_warm = lr_renderer_get_pipeline_count(
                        env.renderer);
                    lr_renderer_get_shadow_slot_info(env.renderer, 0,
                                                     &info);
                    if (info.resolution != 512) {
                        ok = 0;
                        break;
                    }
                }
                if (frame == 499) {
                    TEST_CHECK(stats.pbr_draw_calls > 0 &&
                                   stats.unlit_draw_calls > 0,
                               "endurance: mixed PBR + unlit draws");
                    TEST_CHECK(stats.material_binds <= 20,
                               "endurance: material binds stay bounded");
                }
            }
            lr_renderer_end(env.renderer);
            if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                ok = 0;
                break;
            }
            /* Swapchain leg: clear-only (a renderer leg here would
             * zero the shadow metadata mid-frame, as in the
             * harness). */
            memset(&spass, 0, sizeof(spass));
            spass.color_load_op = LC_LOAD_OP_CLEAR;
            spass.color_store_op = LC_STORE_OP_STORE;
            spass.depth_load_op = LC_LOAD_OP_CLEAR;
            spass.depth_store_op = LC_STORE_OP_DONT_CARE;
            spass.clear_depth = 1.0f;
            if (lc_encoder_begin_swapchain_pass(enc, env.swapchain,
                                                &spass) != LC_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                ok = 0;
                break;
            }
            {
                lc_result eres = lc_end_frame(env.swapchain);

                if (eres != LC_SUCCESS && eres != LC_SUBOPTIMAL) {
                    ok = 0;
                } else if (frame == 100) {
                    /* Screenshot from a high three-quarter view
                     * (frame 499 ends grazing/dark). */
                    unsigned char *px = readback_rgba8(
                        env.device, env.color_img, env.tw, env.th);
                    FILE *f = NULL;

                    TEST_CHECK(px != NULL,
                               "screenshot: frame reads back");
                    if (px != NULL) {
                        f = fopen("shadow_scene.ppm", "wb");
                        if (f != NULL) {
                            uint32_t y;

                            fprintf(f, "P6\n%u %u\n255\n", env.tw,
                                    env.th);
                            for (y = 0; y < env.th; y++) {
                                uint32_t x;

                                for (x = 0; x < env.tw; x++) {
                                    unsigned char *p =
                                        px + ((size_t)y * env.tw + x) *
                                             4u;

                                    fputc(p[0], f);
                                    fputc(p[1], f);
                                    fputc(p[2], f);
                                }
                            }
                            fclose(f);
                            printf("[INFO] screenshot -> shadow_scene.ppm "
                                   "(%ux%u)\n",
                                   env.tw, env.th);
                            TEST_CHECK(1, "screenshot: PPM written");
                        } else {
                            TEST_CHECK(0, "screenshot: PPM written");
                        }
                        free(px);
                    }
                }
            }
        }
        if (!ok) {
            fprintf(stderr, "[dbg] endurance broke frame=%u\n", frame);
        }
        TEST_CHECK(ok, "endurance: 500 frames with motion + resize");
        TEST_CHECK(lr_renderer_get_pipeline_count(env.renderer) ==
                       pipes_warm,
                   "endurance: no pipeline growth after warmup");
        test_wait_idle(env.device);
        for (i = 0; i < 4; i++) {
            lr_material_destroy(emats[i]);
        }
        lr_material_destroy(ewall_mat);
        lr_material_destroy(eglow_mat);
        lr_material_destroy(eground_mat);
        lr_mesh_destroy(esphere);
        lr_mesh_destroy(eground);
        lr_mesh_destroy(ewall);
        lr_mesh_destroy(ecube);
        la_model_destroy(emodel);
        la_asset_manager_destroy(assets);
    }

    printf("shadow vulkan: %d passed, %d failed\n", g_passed, g_failed);
    exit_code = (g_failed == 0) ? 0 : 1;

cleanup:
    test_wait_idle(env.device);
    lr_mesh_destroy(ball);
    lr_mesh_destroy(cube);
    lr_mesh_destroy(ground);
    lr_material_destroy(ground_mat);
    destroy_target(&env);
    lr_renderer_destroy(env.renderer);
    if (env.swapchain != NULL) {
        lc_swapchain_destroy(env.swapchain);
    }
    if (env.surface != NULL) {
        lc_surface_destroy(env.surface);
    }
    if (env.device != NULL) {
        lc_device_destroy(env.device);
    }
    if (env.window != NULL) {
        lc_window_destroy(env.window);
    }
    lc_shutdown();
    return exit_code;
}
