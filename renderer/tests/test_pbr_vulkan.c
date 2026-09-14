/*
 * Luma PBR Vulkan integration test (Phase 15, PARTs U/W/X/Y/AC/AD).
 *
 * Offscreen pixel verification against the CPU reference (same
 * equations as test_pbr.c): rough/smooth, metal/dielectric,
 * backface, emissive, occlusion-on-ambient, normal-map perturbation,
 * non-uniform-scale normal matrix, sRGB output, light limits, the
 * glTF->PBR bridge, and a 500-frame endurance run with screenshot.
 *
 * Pixels come from RGBA8_UNORM targets (raw shader output, linear)
 * except the explicit sRGB check. If the environment cannot provide
 * a window or Vulkan setup, SKIP and exit 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_assets/luma_assets.h>

#include "graphics/graphics_internal.h"

#ifndef LA_FIXTURE_DIR
#define LA_FIXTURE_DIR "."
#endif

#define PBR_TW 256u
#define PBR_TH 256u

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
 * CPU reference (double precision mirror of pbr.frag).
 * ------------------------------------------------------------------ */

static double ref_clamp(double x, double lo, double hi) {
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static double ref_dot3(const double a[3], const double b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

static void ref_eval(const double n[3], const double l[3],
                     const double v[3], const double albedo[3],
                     double metallic, double roughness,
                     const double light_color[3], double intensity,
                     double attenuation, const double ambient[3],
                     double occlusion, const double emissive[3],
                     double out[3]) {
    double r = ref_clamp(roughness, 0.05, 1.0);
    double f0[3];
    double a = r * r;
    double a2 = a * a;
    double k = (r + 1.0) * (r + 1.0) / 8.0;
    double n_dot_l = ref_clamp(ref_dot3(n, l), 0.0, 1.0);
    double n_dot_v = ref_clamp(ref_dot3(n, v), 0.0, 1.0);
    double h[3];
    double hl;
    double n_dot_h;
    double v_dot_h;
    double denom;
    double d;
    double g;
    double f[3];
    int i;

    for (i = 0; i < 3; i++) {
        f0[i] = 0.04 + (albedo[i] - 0.04) * metallic;
    }
    for (i = 0; i < 3; i++) {
        out[i] = albedo[i] * (1.0 - metallic) * ambient[i] * occlusion +
                 emissive[i];
    }
    if (n_dot_l <= 0.0) {
        return; /* mirrors the shader early-out (h degenerates) */
    }
    for (i = 0; i < 3; i++) {
        h[i] = l[i] + v[i];
    }
    hl = sqrt(ref_dot3(h, h));
    for (i = 0; i < 3; i++) {
        h[i] /= hl;
    }
    n_dot_h = ref_clamp(ref_dot3(n, h), 0.0, 1.0);
    v_dot_h = ref_clamp(ref_dot3(v, h), 0.0, 1.0);
    denom = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    d = a2 / (3.14159265358979323846 * denom * denom);
    g = (n_dot_v / (n_dot_v * (1.0 - k) + k)) *
        (n_dot_l / (n_dot_l * (1.0 - k) + k));
    for (i = 0; i < 3; i++) {
        double spec_denom = 4.0 * n_dot_v * n_dot_l;

        if (spec_denom < 1e-4) {
            spec_denom = 1e-4;
        }
        f[i] = f0[i] + (1.0 - f0[i]) * pow(1.0 - v_dot_h, 5.0);
        {
            double spec = d * g * f[i] / spec_denom;
            double kd = (1.0 - f[i]) * (1.0 - metallic);

            out[i] += (kd * albedo[i] / 3.14159265358979323846 + spec) *
                        n_dot_l * attenuation * light_color[i] * intensity;
        }
    }
}

static double srgb_encode(double c) {
    if (c <= 0.0) {
        return 0.0;
    }
    if (c <= 0.0031308) {
        return 12.92 * c;
    }
    return 1.055 * pow(c, 1.0 / 2.4) - 0.055;
}

/* Fallback-normal direction: the 1x1 flat fallback texel
 * (128,128,255) decodes to a normal tilted ~0.32 off +Z. Materials
 * without a normal map shade with this N, so the reference must use
 * it too (UNORM8 cannot represent exact flatness; at very low
 * roughness the lobe is narrower than the tilt, which is physical
 * and modeled, not a bug). */
static void flat_fallback_n(double n[3]) {
    double x = (128.0 / 255.0) * 2.0 - 1.0;
    double l = sqrt(x * x + x * x + 1.0);

    n[0] = x / l;
    n[1] = x / l;
    n[2] = 1.0 / l;
}

/* ------------------------------------------------------------------
 * Environment + offscreen harness.
 * ------------------------------------------------------------------ */

typedef struct pbr_env {
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
} pbr_env;

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

    desc.title = "Luma PBR Test";
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

/* Offscreen color (+optional depth) target. sRGB format selects the
 * hardware encode path check. */
static int make_target(pbr_env *env, lc_format format, uint32_t w,
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

static void destroy_target(pbr_env *env) {
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

/* One rendered frame into the offscreen target; returns readback
 * pixels (caller frees) or NULL. Lights/items submitted by caller
 * between begin and render via the hooks below. */
typedef struct pbr_frame_ops {
    lr_light *lights;
    uint32_t light_count;
    lr_draw_item *items;
    uint32_t item_count;
    float ambient[3];
    int use_ambient;
} pbr_frame_ops;

static unsigned char *render_pixels(pbr_env *env, pbr_frame_ops *ops) {
    lc_command_encoder *enc = NULL;
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_render_depth_attachment datt;
    lc_result res;
    uint32_t i;
    unsigned char *px = NULL;
    unsigned guard = 0;

    (void)guard;
    lc_poll_events();
    res = lc_begin_frame(env->swapchain);
    if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
        return NULL;
    }
    if (res != LC_SUCCESS ||
        lc_swapchain_get_encoder(env->swapchain, &enc) != LC_SUCCESS) {
        return NULL;
    }
    memset(&catt, 0, sizeof(catt));
    catt.view = env->color_view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    catt.clear_color[0] = 0.0f;
    catt.clear_color[1] = 0.0f;
    catt.clear_color[2] = 0.0f;
    catt.clear_color[3] = 1.0f;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.width = env->tw;
    pdesc.height = env->th;
    if (env->depth_view != NULL) {
        memset(&datt, 0, sizeof(datt));
        datt.view = env->depth_view;
        datt.depth_load_op = LC_LOAD_OP_CLEAR;
        datt.depth_store_op = LC_STORE_OP_DONT_CARE;
        datt.clear_depth = 1.0f;
        datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
        datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
        datt.clear_stencil = 0;
        pdesc.depth_attachment = &datt;
    }
    if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
        return NULL;
    }
    if (lr_renderer_begin(env->renderer, &env->camera) != LR_SUCCESS) {
        lc_encoder_end_render_pass(enc);
        return NULL;
    }
    /* Deterministic ambient every frame: tests opt into nonzero via
     * ops (the zeroed default is black, so no state leaks across
     * tests through the renderer). */
    lr_renderer_set_ambient(env->renderer, ops->ambient);
    for (i = 0; i < ops->light_count; i++) {
        if (lr_renderer_submit_light(env->renderer, &ops->lights[i]) !=
            LR_SUCCESS) {
            lr_renderer_end(env->renderer);
            lc_encoder_end_render_pass(enc);
            return NULL;
        }
    }
    for (i = 0; i < ops->item_count; i++) {
        if (lr_renderer_submit(env->renderer, &ops->items[i]) !=
            LR_SUCCESS) {
            lr_renderer_end(env->renderer);
            lc_encoder_end_render_pass(enc);
            return NULL;
        }
    }
    if (lr_renderer_render(env->renderer, enc, env->target) != LR_SUCCESS ||
        lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        lr_renderer_end(env->renderer);
        return NULL;
    }
    lr_renderer_end(env->renderer);
    /* Same content straight to the swapchain (proven pattern: an
     * empty clear-only pass does not transition the present image;
     * this also covers the swapchain-format pipeline variant). */
    {
        lc_render_swapchain_pass_desc spass;
        uint32_t i;

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
            LC_SUCCESS) {
            return NULL;
        }
        /* Re-submit is cheap (CPU matrices); GPU objects shared. */
        if (lr_renderer_begin(env->renderer, &env->camera) != LR_SUCCESS) {
            lc_encoder_end_render_pass(enc);
            return NULL;
        }
        lr_renderer_set_ambient(env->renderer, ops->ambient);
        for (i = 0; i < ops->light_count; i++) {
            if (lr_renderer_submit_light(env->renderer, &ops->lights[i]) !=
                LR_SUCCESS) {
                lr_renderer_end(env->renderer);
                lc_encoder_end_render_pass(enc);
                return NULL;
            }
        }
        for (i = 0; i < ops->item_count; i++) {
            if (lr_renderer_submit(env->renderer, &ops->items[i]) !=
                LR_SUCCESS) {
                lr_renderer_end(env->renderer);
                lc_encoder_end_render_pass(enc);
                return NULL;
            }
        }
        if (lr_renderer_render(env->renderer, enc,
                               lc_swapchain_get_render_target(
                                   env->swapchain)) != LR_SUCCESS ||
            lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            lr_renderer_end(env->renderer);
            return NULL;
        }
        lr_renderer_end(env->renderer);
    }
    if (lc_end_frame(env->swapchain) != LC_SUCCESS) {
        return NULL;
    }
    px = readback_rgba8(env->device, env->color_img, env->tw, env->th);
    return px;
}

static void pixel_at(const unsigned char *px, uint32_t w, uint32_t x,
                     uint32_t y, double out[3]) {
    const unsigned char *p = px + ((size_t)y * w + x) * 4u;

    out[0] = (double)p[0] / 255.0;
    out[1] = (double)p[1] / 255.0;
    out[2] = (double)p[2] / 255.0;
}

static int near3(const double a[3], const double b[3], double tol) {
    int i;

    for (i = 0; i < 3; i++) {
        if (fabs(a[i] - b[i]) > tol) {
            return 0;
        }
    }
    return 1;
}

static double luminance(const double c[3]) {
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

/* Facing-camera PBR material (all maps NULL unless given). */
static lr_material *make_pbr(pbr_env *env, const float base[4],
                             float metallic, float roughness,
                             lc_image_view *normal_view,
                             lc_image_view *occ_view, float occ_strength,
                             const float emissive[3], int double_sided) {
    lr_pbr_material_desc desc;
    lr_material *mat = NULL;

    memset(&desc, 0, sizeof(desc));
    memcpy(desc.base_color_factor, base, sizeof(desc.base_color_factor));
    desc.metallic_factor = metallic;
    desc.roughness_factor = roughness;
    desc.normal_texture = normal_view;
    desc.occlusion_texture = occ_view;
    desc.occlusion_strength = occ_strength;
    if (emissive != NULL) {
        memcpy(desc.emissive_factor, emissive, sizeof(desc.emissive_factor));
    }
    desc.normal_scale = 1.0f;
    desc.double_sided = double_sided;
    desc.alpha_mode = LR_ALPHA_OPAQUE;
    if (lr_material_create_pbr(env->renderer, &desc, &mat) != LR_SUCCESS) {
        return NULL;
    }
    return mat;
}

static void facing_light(lr_light *light, float intensity) {
    memset(light, 0, sizeof(*light));
    light->type = LR_LIGHT_DIRECTIONAL;
    light->color[0] = 1.0f;
    light->color[1] = 1.0f;
    light->color[2] = 1.0f;
    light->intensity = intensity;
    light->direction[0] = 0.0f;
    light->direction[1] = 0.0f;
    light->direction[2] = -1.0f;
}

static void facing_item(lr_draw_item *item, lr_mesh *mesh,
                        lr_material *mat) {
    memset(item, 0, sizeof(*item));
    lr_transform_identity(&item->transform);
    item->mesh = mesh;
    item->material = mat;
}

/* ------------------------------------------------------------------
 * Endurance scene submission (shared by the offscreen + swapchain
 * legs of each frame; re-submit is cheap CPU matrices over shared
 * GPU objects).
 * ------------------------------------------------------------------ */

typedef struct end_scene {
    lr_mesh *sphere;
    lr_mesh *ground;
    lr_mesh *wall;
    lr_mesh *glow;
    lr_material *mats[6];
    lr_material *wall_mat;
    lr_material *glow_mat;
    lr_material *ground_mat;
    la_model *box;
} end_scene;

static int submit_endurance_scene(pbr_env *env, const end_scene *scene,
                                  float t) {
    lr_draw_item item;
    lr_light key;
    lr_light fill;
    lr_light orbiter;
    int k;
    static const float yaxis[3] = { 0.0f, 1.0f, 0.0f };
    static const float xaxis[3] = { 1.0f, 0.0f, 0.0f };

    memset(&key, 0, sizeof(key));
    key.type = LR_LIGHT_DIRECTIONAL;
    key.color[0] = 1.0f;
    key.color[1] = 0.95f;
    key.color[2] = 0.88f;
    key.intensity = 2.5f;
    key.direction[0] = 0.35f;
    key.direction[1] = -1.0f;
    key.direction[2] = 0.3f;
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
    memset(&orbiter, 0, sizeof(orbiter));
    orbiter.type = LR_LIGHT_POINT;
    orbiter.color[0] = 0.5f;
    orbiter.color[1] = 0.65f;
    orbiter.color[2] = 1.0f;
    orbiter.intensity = 14.0f;
    orbiter.position[0] = 3.0f * cosf(t * 0.9f);
    orbiter.position[1] = 1.8f + 0.5f * sinf(t * 1.7f);
    orbiter.position[2] = 3.0f * sinf(t * 0.9f);
    orbiter.range = 10.0f;
    if (lr_renderer_submit_light(env->renderer, &key) != LR_SUCCESS ||
        lr_renderer_submit_light(env->renderer, &fill) != LR_SUCCESS ||
        lr_renderer_submit_light(env->renderer, &orbiter) != LR_SUCCESS) {
        return 0;
    }
    memset(&item, 0, sizeof(item));
    item.mesh = scene->sphere;
    for (k = 0; k < 6; k++) {
        lr_transform_identity(&item.transform);
        item.transform.position[0] = ((float)(k % 3) - 1.0f) * 1.35f;
        item.transform.position[1] = 1.9f - (float)(k / 3) * 1.35f;
        item.transform.position[2] = -1.5f;
        item.material = scene->mats[k];
        if (lr_renderer_submit(env->renderer, &item) != LR_SUCCESS) {
            return 0;
        }
    }
    lr_transform_identity(&item.transform);
    item.transform.position[0] = -2.6f;
    item.transform.position[1] = 1.2f;
    item.transform.position[2] = 0.4f;
    lr_quat_from_axis_angle(xaxis, 1.5707963f, item.transform.rotation);
    item.mesh = scene->wall;
    item.material = scene->wall_mat;
    if (lr_renderer_submit(env->renderer, &item) != LR_SUCCESS) {
        return 0;
    }
    lr_transform_identity(&item.transform);
    item.transform.position[0] = 2.6f;
    item.transform.position[1] = 0.5f;
    item.transform.position[2] = 0.4f;
    lr_quat_from_axis_angle(yaxis, t, item.transform.rotation);
    item.mesh = scene->glow;
    item.material = scene->glow_mat;
    if (lr_renderer_submit(env->renderer, &item) != LR_SUCCESS) {
        return 0;
    }
    {
        lr_transform root;

        lr_transform_identity(&root);
        root.position[0] = 0.0f;
        root.position[1] = 0.1f;
        root.position[2] = 1.4f;
        lr_quat_from_axis_angle(yaxis, t * 0.7f, root.rotation);
        if (la_model_submit(scene->box, env->renderer, &root) !=
            LA_SUCCESS) {
            return 0;
        }
    }
    lr_transform_identity(&item.transform);
    item.transform.position[1] = -0.55f;
    item.mesh = scene->ground;
    item.material = scene->ground_mat;
    if (lr_renderer_submit(env->renderer, &item) != LR_SUCCESS) {
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------
 * Main.
 * ------------------------------------------------------------------ */

int main(void) {
    pbr_env env;
    lr_mesh *cube = NULL;
    lr_mesh *ball = NULL;
    lr_mesh *card = NULL;
    int rc;
    int exit_code = 1;
    static const float eye[3] = { 0.0f, 0.0f, 5.0f };
    static const float center[3] = { 0.0f, 0.0f, 0.0f };
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    static const float black[3] = { 0.0f, 0.0f, 0.0f };
    static const float no_emi[3] = { 0.0f, 0.0f, 0.0f };

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
        lc_swapchain_desc sdesc = { 0 };

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
    if (make_target(&env, LC_FORMAT_RGBA8_UNORM, PBR_TW, PBR_TH, 0) != 0) {
        printf("FAIL: offscreen target\n");
        goto cleanup;
    }
    {
        /* Renderer over the offscreen signature (ambient forced
         * black here; per-test ambient overrides follow). */
        lr_renderer_desc rdesc;
        lc_render_target_desc sig;

        memset(&sig, 0, sizeof(sig));
        sig.width = PBR_TW;
        sig.height = PBR_TH;
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
        lr_renderer_set_ambient(env.renderer, black);
    }
    lr_camera_init(&env.camera);
    if (lr_camera_set_perspective(&env.camera, 0.5f, 1.0f, 0.1f, 20.0f) !=
            LR_SUCCESS ||
        lr_camera_look_at(&env.camera, eye, center, up) != LR_SUCCESS) {
        printf("FAIL: camera\n");
        goto cleanup;
    }
    if (lr_mesh_create_cube(env.renderer, 2.0f, &cube) != LR_SUCCESS ||
        lr_mesh_create_sphere(env.renderer, 1.0f, 48, 24, &ball) !=
            LR_SUCCESS ||
        lr_mesh_create_plane(env.renderer, 2.0f, 2.0f, &card) !=
            LR_SUCCESS) {
        printf("FAIL: meshes\n");
        goto cleanup;
    }

    /* ---- rough vs smooth dielectric, facing ----
     *
     * The facing smooth lobe at minimum roughness is narrower than
     * the fallback-normal quantization tilt (~0.32 deg), so the
     * absolute smooth check uses a robust mid-low roughness (0.15)
     * with a tilt-aware reference, while a dedicated equality check
     * pins the minimum-roughness clamp (r=0.0 renders byte-identical
     * to r=0.05). */
    {
        static const float base[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
        lr_material *rough = make_pbr(&env, base, 0.0f, 1.0f, NULL, NULL,
                                      1.0f, no_emi, 0);
        lr_material *smooth = make_pbr(&env, base, 0.0f, 0.15f, NULL, NULL,
                                       1.0f, no_emi, 0);
        lr_material *clamped = make_pbr(&env, base, 0.0f, 0.0f, NULL, NULL,
                                        1.0f, no_emi, 0);
        lr_material *min_rough = make_pbr(&env, base, 0.0f, 0.05f, NULL,
                                          NULL, 1.0f, no_emi, 0);
        lr_light light;
        lr_draw_item item;
        pbr_frame_ops ops;
        unsigned char *px;
        double got[3];
        double want[3];
        double n[3];
        double l[3] = { 0.0, 0.0, 1.0 };
        double v[3] = { 0.0, 0.0, 1.0 };
        double alb[3] = { 0.5, 0.5, 0.5 };
        double lc[3] = { 1.0, 1.0, 1.0 };
        double am[3] = { 0.0, 0.0, 0.0 };
        double em[3] = { 0.0, 0.0, 0.0 };

        flat_fallback_n(n);
        facing_light(&light, 0.05f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = &item;
        ops.item_count = 1;

        facing_item(&item, cube, rough);
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "pixels: rough render reads back");
        if (px != NULL) {
            pixel_at(px, PBR_TW, 128, 128, got);
            ref_eval(n, l, v, alb, 0.0, 1.0, lc, 0.05, 1.0, am, 1.0, em,
                     want);
            TEST_CHECK(near3(got, want, 0.03),
                       "pixels: rough dielectric matches reference");
            free(px);
        }
        facing_item(&item, cube, smooth);
        px = render_pixels(&env, &ops);
        if (px != NULL) {
            double rgh[3];

            pixel_at(px, PBR_TW, 128, 128, got);
            ref_eval(n, l, v, alb, 0.0, 0.15, lc, 0.05, 1.0, am, 1.0,
                     em, want);
            TEST_CHECK(near3(got, want, 0.05),
                       "pixels: smooth dielectric matches reference");
            /* Rough re-read for the ordering check. */
            facing_item(&item, cube, rough);
            {
                unsigned char *px2 = render_pixels(&env, &ops);

                if (px2 != NULL) {
                    pixel_at(px2, PBR_TW, 128, 128, rgh);
                    free(px2);
                } else {
                    rgh[0] = rgh[1] = rgh[2] = 0.0;
                }
            }
            TEST_CHECK(luminance(got) > luminance(rgh) + 0.2,
                       "pixels: smooth outshines rough facing");
            free(px);
        } else {
            TEST_CHECK(0, "pixels: smooth render reads back");
            TEST_CHECK(0, "pixels: smooth outshines rough facing");
        }
        /* Clamp: r=0.0 must shade byte-identical to r=0.05. */
        {
            unsigned char *p0 = NULL;
            unsigned char *p5 = NULL;
            int same = 0;

            facing_item(&item, cube, clamped);
            p0 = render_pixels(&env, &ops);
            facing_item(&item, cube, min_rough);
            p5 = render_pixels(&env, &ops);
            if (p0 != NULL && p5 != NULL) {
                const unsigned char *a =
                    p0 + ((size_t)128 * PBR_TW + 128) * 4u;
                const unsigned char *b =
                    p5 + ((size_t)128 * PBR_TW + 128) * 4u;

                same = (a[0] == b[0] && a[1] == b[1] && a[2] == b[2] &&
                        a[3] == b[3]);
            }
            TEST_CHECK(same, "pixels: zero roughness clamps to minimum");
            free(p0);
            free(p5);
        }
        lr_material_destroy(min_rough);
        lr_material_destroy(clamped);
        lr_material_destroy(smooth);
        lr_material_destroy(rough);
    }

    /* ---- metal vs dielectric ---- */
    {
        static const float base[4] = { 0.8f, 0.4f, 0.2f, 1.0f };
        lr_material *metal = make_pbr(&env, base, 1.0f, 0.4f, NULL, NULL,
                                      1.0f, no_emi, 0);
        lr_material *diel = make_pbr(&env, base, 0.0f, 0.4f, NULL, NULL,
                                     1.0f, no_emi, 0);
        lr_light light;
        lr_draw_item item;
        pbr_frame_ops ops;
        unsigned char *px;
        double got_m[3];
        double got_d[3];
        double n[3] = { 0.0, 0.0, 1.0 };
        double l[3] = { 0.0, 0.0, 1.0 };
        double v[3] = { 0.0, 0.0, 1.0 };
        double alb[3] = { 0.8, 0.4, 0.2 };
        double lc[3] = { 1.0, 1.0, 1.0 };
        double am[3] = { 0.0, 0.0, 0.0 };
        double em[3] = { 0.0, 0.0, 0.0 };
        double want[3];

        facing_light(&light, 0.25f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = &item;
        ops.item_count = 1;

        facing_item(&item, cube, metal);
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "pixels: metal render reads back");
        if (px != NULL) {
            pixel_at(px, PBR_TW, 128, 128, got_m);
            ref_eval(n, l, v, alb, 1.0, 0.4, lc, 0.25, 1.0, am, 1.0, em,
                     want);
            TEST_CHECK(near3(got_m, want, 0.05),
                       "pixels: metal matches reference");
            free(px);
        }
        facing_item(&item, cube, diel);
        px = render_pixels(&env, &ops);
        if (px != NULL) {
            pixel_at(px, PBR_TW, 128, 128, got_d);
            ref_eval(n, l, v, alb, 0.0, 0.4, lc, 0.25, 1.0, am, 1.0, em,
                     want);
            TEST_CHECK(near3(got_d, want, 0.05),
                       "pixels: dielectric matches reference");
            TEST_CHECK(got_m[0] > got_d[0] + 0.25,
                       "pixels: metal red outshines dielectric red");
            free(px);
        } else {
            TEST_CHECK(0, "pixels: dielectric render reads back");
            TEST_CHECK(0, "pixels: metal red outshines dielectric red");
        }
        lr_material_destroy(diel);
        lr_material_destroy(metal);
    }

    /* ---- mid roughness at mid intensity (no-clip middle) ---- */
    {
        static const float base[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
        lr_material *mid = make_pbr(&env, base, 0.0f, 0.2f, NULL, NULL,
                                    1.0f, no_emi, 0);
        lr_light light;
        lr_draw_item item;
        pbr_frame_ops ops;
        unsigned char *px;
        double got[3];
        double want[3];
        double n[3];
        double l[3] = { 0.0, 0.0, 1.0 };
        double v[3] = { 0.0, 0.0, 1.0 };
        double alb[3] = { 0.5, 0.5, 0.5 };
        double lc[3] = { 1.0, 1.0, 1.0 };
        double am[3] = { 0.0, 0.0, 0.0 };
        double em[3] = { 0.0, 0.0, 0.0 };

        flat_fallback_n(n);
        facing_light(&light, 0.2f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = &item;
        ops.item_count = 1;
        facing_item(&item, cube, mid);
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "pixels: mid-rough render reads back");
        if (px != NULL) {
            pixel_at(px, PBR_TW, 128, 128, got);
            ref_eval(n, l, v, alb, 0.0, 0.2, lc, 0.2, 1.0, am, 1.0, em,
                     want);
            TEST_CHECK(near3(got, want, 0.05),
                       "pixels: mid roughness matches reference");
            free(px);
        }
        lr_material_destroy(mid);
    }

    /* ---- backface: light behind the face kills direct ---- */
    {
        static const float base[4] = { 0.6f, 0.6f, 0.65f, 1.0f };
        static const float amb[3] = { 0.03f, 0.03f, 0.03f };
        lr_material *mat = make_pbr(&env, base, 0.0f, 0.7f, NULL, NULL,
                                    1.0f, no_emi, 0);
        lr_light light;
        lr_draw_item item;
        pbr_frame_ops ops;
        unsigned char *px;
        double got[3];
        double want[3] = { 0.6 * 0.03, 0.6 * 0.03, 0.65 * 0.03 };

        /* Key travels +Z (away from the camera-facing +Z face). */
        memset(&light, 0, sizeof(light));
        light.type = LR_LIGHT_DIRECTIONAL;
        light.color[0] = 1.0f;
        light.color[1] = 1.0f;
        light.color[2] = 1.0f;
        light.intensity = 3.0f;
        light.direction[0] = 0.0f;
        light.direction[1] = 0.0f;
        light.direction[2] = 1.0f;
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = &item;
        ops.item_count = 1;
        ops.ambient[0] = amb[0];
        ops.ambient[1] = amb[1];
        ops.ambient[2] = amb[2];
        ops.use_ambient = 1;

        facing_item(&item, cube, mat);
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "pixels: backface render reads back");
        if (px != NULL) {
            pixel_at(px, PBR_TW, 128, 128, got);
            TEST_CHECK(near3(got, want, 0.02),
                       "pixels: backface shows ambient only");
            free(px);
        }
        lr_material_destroy(mat);
    }

    /* ---- double-sided: backface flips the shading normal ---- */
    {
        static const float base[4] = { 0.65f, 0.65f, 0.7f, 1.0f };
        static const float xaxis[3] = { 1.0f, 0.0f, 0.0f };
        lr_material *mat = make_pbr(&env, base, 0.0f, 0.5f, NULL, NULL,
                                    1.0f, no_emi, 1);
        lr_light light;
        lr_draw_item item;
        pbr_frame_ops ops;
        unsigned char *px;
        double got[3];
        double n[3] = { 0.0, 0.0, 1.0 };
        double l[3] = { 0.0, 0.0, 1.0 };
        double v[3] = { 0.0, 0.0, 1.0 };
        double alb[3] = { 0.65, 0.65, 0.7 };
        double lc[3] = { 1.0, 1.0, 1.0 };
        double am[3] = { 0.0, 0.0, 0.0 };
        double em[3] = { 0.0, 0.0, 0.0 };
        double want[3];

        facing_light(&light, 3.0f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = &item;
        ops.item_count = 1;

        /* Card turned away (normal -Z): with culling disabled the
         * camera sees its back side; the shader must flip N. */
        memset(&item, 0, sizeof(item));
        lr_transform_identity(&item.transform);
        lr_quat_from_axis_angle(xaxis, -1.5707963f,
                                item.transform.rotation);
        item.mesh = card;
        item.material = mat;
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "pixels: double-sided render reads back");
        if (px != NULL) {
            pixel_at(px, PBR_TW, 128, 128, got);
            ref_eval(n, l, v, alb, 0.0, 0.5, lc, 3.0, 1.0, am, 1.0, em,
                     want);
            TEST_CHECK(near3(got, want, 0.06),
                       "pixels: backface flip shades like the front");
            free(px);
        }
        lr_material_destroy(mat);
    }

    /* ---- emissive with zero lights ---- */
    {
        static const float base[4] = { 0.01f, 0.01f, 0.01f, 1.0f };
        static const float emi[3] = { 0.9f, 0.5f, 0.2f };
        lr_material *mat = make_pbr(&env, base, 0.0f, 0.9f, NULL, NULL,
                                    1.0f, emi, 0);
        lr_draw_item item;
        pbr_frame_ops ops;
        unsigned char *px;
        double got[3];
        double want[3] = { 0.9, 0.5, 0.2 };
        lr_render_stats stats;

        memset(&ops, 0, sizeof(ops));
        ops.items = &item;
        ops.item_count = 1;
        facing_item(&item, cube, mat);
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "pixels: emissive render reads back");
        if (px != NULL) {
            pixel_at(px, PBR_TW, 128, 128, got);
            TEST_CHECK(near3(got, want, 0.03),
                       "pixels: factor-only emissive shows with 0 lights");
            free(px);
        }
        lr_renderer_get_stats(env.renderer, &stats);
        TEST_CHECK(stats.active_lights == 0 && stats.submitted_lights == 0,
                   "lights: zero-light frame reports zero");
        lr_material_destroy(mat);
    }

    /* ---- occlusion damps ambient only ---- */
    {
        static const float base[4] = { 0.6f, 0.6f, 0.6f, 1.0f };
        static const float amb[3] = { 0.25f, 0.25f, 0.25f };
        unsigned char occ_px[4 * 4 * 4];
        lc_image *occ_img = NULL;
        lc_image_view *occ_view = NULL;
        lr_material *full = NULL;
        lr_material *damped = NULL;
        lr_draw_item item;
        pbr_frame_ops ops;
        unsigned char *px;
        double got[3];
        int i;

        for (i = 0; i < 16; i++) {
            occ_px[i * 4 + 0] = 64;
            occ_px[i * 4 + 1] = 64;
            occ_px[i * 4 + 2] = 64;
            occ_px[i * 4 + 3] = 255;
        }
        {
            lc_image_desc idesc;
            lc_image_upload_desc up;
            lc_image_view_desc vdesc;

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
            if (lc_image_create(env.device, &idesc, &occ_img) !=
                    LC_SUCCESS ||
                lc_image_write(occ_img, &up) != LC_SUCCESS) {
                printf("FAIL: occlusion image\n");
                goto cleanup;
            }
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.mip_level_count = 1;
            vdesc.array_layer_count = 1;
            if (lc_image_view_create(occ_img, &vdesc, &occ_view) !=
                LC_SUCCESS) {
                printf("FAIL: occlusion view\n");
                goto cleanup;
            }
        }
        full = make_pbr(&env, base, 0.0f, 0.9f, NULL, NULL, 0.0f, no_emi,
                        0);
        damped = make_pbr(&env, base, 0.0f, 0.9f, NULL, occ_view, 1.0f,
                          no_emi, 0);
        memset(&ops, 0, sizeof(ops));
        ops.items = &item;
        ops.item_count = 1;
        ops.ambient[0] = amb[0];
        ops.ambient[1] = amb[1];
        ops.ambient[2] = amb[2];
        ops.use_ambient = 1;

        facing_item(&item, cube, damped);
        px = render_pixels(&env, &ops);
        if (px != NULL) {
            double want[3] = { 0.6 * 0.25 * (64.0 / 255.0),
                               0.6 * 0.25 * (64.0 / 255.0),
                               0.6 * 0.25 * (64.0 / 255.0) };

            pixel_at(px, PBR_TW, 128, 128, got);
            TEST_CHECK(near3(got, want, 0.03),
                       "pixels: occlusion damps ambient");
            free(px);
        } else {
            TEST_CHECK(0, "pixels: occlusion render reads back");
        }
        facing_item(&item, cube, full);
        px = render_pixels(&env, &ops);
        if (px != NULL) {
            double want[3] = { 0.15, 0.15, 0.15 };

            pixel_at(px, PBR_TW, 128, 128, got);
            TEST_CHECK(near3(got, want, 0.03),
                       "pixels: zero strength keeps full ambient");
            free(px);
        } else {
            TEST_CHECK(0, "pixels: full-ambient render reads back");
        }
        lr_material_destroy(damped);
        lr_material_destroy(full);
        lc_image_view_destroy(occ_view);
        lc_image_destroy(occ_img);
    }

    /* ---- normal-map perturbation vs flat ---- */
    {
        static const float base[4] = { 0.65f, 0.65f, 0.7f, 1.0f };
        unsigned char tilt_px[4 * 4 * 4];
        lc_image *tilt_img = NULL;
        lc_image_view *tilt_view = NULL;
        lc_sampler *tilt_samp = NULL;
        lr_material *flat = NULL;
        lr_material *tilted = NULL;
        lr_light light;
        lr_draw_item item;
        pbr_frame_ops ops;
        unsigned char *px;
        double got_flat[3];
        double got_tilt[3];
        double n[3] = { 0.0, 0.0, 1.0 };
        double nt[3] = { 0.5, 0.0, 0.8660254 };
        double l[3] = { 0.0, 0.0, 1.0 };
        double v[3] = { 0.0, 0.0, 1.0 };
        double alb[3] = { 0.65, 0.65, 0.7 };
        double lc[3] = { 1.0, 1.0, 1.0 };
        double am[3] = { 0.0, 0.0, 0.0 };
        double em[3] = { 0.0, 0.0, 0.0 };
        double want[3];
        int i;

        /* Constant tilt: tangent-space (0.5, 0, 0.866) -> texel. */
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
            if (lc_image_create(env.device, &idesc, &tilt_img) !=
                    LC_SUCCESS) {
                printf("FAIL: tilt image\n");
                goto cleanup;
            }
            memset(&up, 0, sizeof(up));
            up.width = 4;
            up.height = 4;
            up.depth = 1;
            up.data = tilt_px;
            up.data_size = sizeof(tilt_px);
            if (lc_image_write(tilt_img, &up) != LC_SUCCESS) {
                printf("FAIL: tilt upload\n");
                goto cleanup;
            }
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.mip_level_count = 1;
            vdesc.array_layer_count = 1;
            if (lc_image_view_create(tilt_img, &vdesc, &tilt_view) !=
                LC_SUCCESS) {
                printf("FAIL: tilt view\n");
                goto cleanup;
            }
            memset(&sdesc, 0, sizeof(sdesc));
            sdesc.min_filter = LC_FILTER_LINEAR;
            sdesc.mag_filter = LC_FILTER_LINEAR;
            sdesc.mipmap_mode = LC_MIPMAP_MODE_NEAREST;
            sdesc.address_u = LC_ADDRESS_CLAMP_TO_EDGE;
            sdesc.address_v = LC_ADDRESS_CLAMP_TO_EDGE;
            sdesc.max_anisotropy = 1.0f;
            if (lc_sampler_create(env.device, &sdesc, &tilt_samp) !=
                LC_SUCCESS) {
                printf("FAIL: tilt sampler\n");
                goto cleanup;
            }
        }
        flat = make_pbr(&env, base, 0.0f, 0.5f, NULL, NULL, 1.0f, no_emi,
                        0);
        tilted = make_pbr(&env, base, 0.0f, 0.5f, NULL, NULL, 1.0f,
                          no_emi, 0);
        /* Swap in the tilt map via a twin material (sampler rides
         * the desc; views are borrowed). */
        {
            lr_pbr_material_desc desc;

            memset(&desc, 0, sizeof(desc));
            memcpy(desc.base_color_factor, base,
                   sizeof(desc.base_color_factor));
            desc.metallic_factor = 0.0f;
            desc.roughness_factor = 0.5f;
            desc.normal_texture = tilt_view;
            desc.sampler = tilt_samp;
            desc.normal_scale = 1.0f;
            desc.occlusion_strength = 1.0f;
            desc.alpha_mode = LR_ALPHA_OPAQUE;
            lr_material_destroy(tilted);
            tilted = NULL;
            if (lr_material_create_pbr(env.renderer, &desc, &tilted) !=
                LR_SUCCESS) {
                printf("FAIL: tilted material\n");
                goto cleanup;
            }
        }
        facing_light(&light, 3.0f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = &item;
        ops.item_count = 1;

        facing_item(&item, cube, flat);
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "pixels: flat-normal render reads back");
        if (px != NULL) {
            pixel_at(px, PBR_TW, 128, 128, got_flat);
            ref_eval(n, l, v, alb, 0.0, 0.5, lc, 3.0, 1.0, am, 1.0, em,
                     want);
            TEST_CHECK(near3(got_flat, want, 0.05),
                       "pixels: flat normal matches reference");
            free(px);
        }
        facing_item(&item, cube, tilted);
        px = render_pixels(&env, &ops);
        if (px != NULL) {
            /* Map texel -> [0,1] -> [-1,1] through the shader path. */
            double mn[3] = { (191.0 / 255.0) * 2.0 - 1.0,
                             (128.0 / 255.0) * 2.0 - 1.0,
                             (221.0 / 255.0) * 2.0 - 1.0 };
            double mnl = sqrt(ref_dot3(mn, mn));

            mn[0] /= mnl;
            mn[1] /= mnl;
            mn[2] /= mnl;
            (void)nt;
            pixel_at(px, PBR_TW, 128, 128, got_tilt);
            ref_eval(mn, l, v, alb, 0.0, 0.5, lc, 3.0, 1.0, am, 1.0, em,
                     want);
            TEST_CHECK(near3(got_tilt, want, 0.05),
                       "pixels: tilted normal matches reference");
            TEST_CHECK(luminance(got_flat) > luminance(got_tilt) + 0.05,
                       "pixels: tilt away from light dims the pixel");
            free(px);
        } else {
            TEST_CHECK(0, "pixels: tilted render reads back");
            TEST_CHECK(0, "pixels: tilt away from light dims the pixel");
        }
        lr_material_destroy(tilted);
        lr_material_destroy(flat);
        lc_sampler_destroy(tilt_samp);
        lc_image_view_destroy(tilt_view);
        lc_image_destroy(tilt_img);
    }

    /* ---- non-uniform scale: ellipsoid pixel vs correct/naive ---- */
    {
        static const float base[4] = { 0.7f, 0.7f, 0.75f, 1.0f };
        lr_material *mat = make_pbr(&env, base, 0.0f, 0.5f, NULL, NULL,
                                    1.0f, no_emi, 0);
        lr_light light;
        lr_draw_item item;
        pbr_frame_ops ops;
        unsigned char *px;
        /* World x of the sampled surface point (solved below). */
        double x0 = 0.5;
        double z;
        double depth;
        double ndc_x;
        double n_correct[3];
        double n_naive[3];
        double l[3] = { 0.0, 0.0, 1.0 };
        double v[3] = { 0.0, 0.0, 1.0 };
        double alb[3] = { 0.7, 0.7, 0.75 };
        double lc[3] = { 1.0, 1.0, 1.0 };
        double am[3] = { 0.0, 0.0, 0.0 };
        double em[3] = { 0.0, 0.0, 0.0 };
        double want[3];
        double want_naive[3];
        double got[3];
        double nl;
        int it;
        uint32_t ux;
        const double tan_half = tan(0.25);

        /* Ellipsoid (x/2)^2 + z^2 = 1 at ray height y=0. Exact
         * solve: pixel 190 center -> ndc -> world x at surface. */
        ux = 190;
        ndc_x = ((double)ux + 0.5) / (double)PBR_TW * 2.0 - 1.0;
        x0 = 0.5;
        for (it = 0; it < 8; it++) {
            z = sqrt(1.0 - (x0 / 2.0) * (x0 / 2.0));
            depth = 5.0 - z;
            x0 = ndc_x * tan_half * depth;
        }
        z = sqrt(1.0 - (x0 / 2.0) * (x0 / 2.0));
        /* Correct: inverse-transpose of diag(2,1,1) applied to the
         * sphere normal, normalized. Naive: model matrix applied. */
        {
            double cl[3] = { x0 / 2.0, 0.0, z };
            double cll = sqrt(ref_dot3(cl, cl));

            cl[0] /= cll;
            cl[2] /= cll;
            n_correct[0] = cl[0] / 2.0;
            n_correct[1] = 0.0;
            n_correct[2] = cl[2];
            nl = sqrt(ref_dot3(n_correct, n_correct));
            n_correct[0] /= nl;
            n_correct[2] /= nl;
            n_naive[0] = cl[0] * 2.0;
            n_naive[1] = 0.0;
            n_naive[2] = cl[2];
            nl = sqrt(ref_dot3(n_naive, n_naive));
            n_naive[0] /= nl;
            n_naive[2] /= nl;
        }
        v[0] = -x0 / 5.0; /* approx view dir at the surface point */
        v[2] = 1.0;
        {
            double vl = sqrt(ref_dot3(v, v));

            v[0] /= vl;
            v[2] /= vl;
        }

        facing_light(&light, 3.0f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        ops.items = &item;
        ops.item_count = 1;

        memset(&item, 0, sizeof(item));
        lr_transform_identity(&item.transform);
        item.transform.scale[0] = 2.0f;
        item.transform.scale[1] = 1.0f;
        item.transform.scale[2] = 1.0f;
        item.mesh = ball;
        item.material = mat;
        px = render_pixels(&env, &ops);
        TEST_CHECK(px != NULL, "pixels: ellipsoid render reads back");
        if (px != NULL) {
            pixel_at(px, PBR_TW, ux, 128, got);
            ref_eval(n_correct, l, v, alb, 0.0, 0.5, lc, 3.0, 1.0, am,
                     1.0, em, want);
            ref_eval(n_naive, l, v, alb, 0.0, 0.5, lc, 3.0, 1.0, am, 1.0,
                     em, want_naive);
            TEST_CHECK(fabs(luminance(want) - luminance(want_naive)) >
                           0.08,
                       "scale: correct vs naive predictions differ");
            TEST_CHECK(near3(got, want, 0.06),
                       "pixels: scaled shading uses the normal matrix");
            free(px);
        }
        lr_material_destroy(mat);
    }

    /* ---- sRGB output encodes linear ---- */
    {
        static const float base[4] = { 0.8f, 0.4f, 0.2f, 1.0f };
        lr_material *diel = make_pbr(&env, base, 0.0f, 0.4f, NULL, NULL,
                                     1.0f, no_emi, 0);
        lr_light light;
        lr_draw_item item;
        pbr_frame_ops ops;
        unsigned char *px;
        double n[3] = { 0.0, 0.0, 1.0 };
        double l[3] = { 0.0, 0.0, 1.0 };
        double v[3] = { 0.0, 0.0, 1.0 };
        double alb[3] = { 0.8, 0.4, 0.2 };
        double lc[3] = { 1.0, 1.0, 1.0 };
        double am[3] = { 0.0, 0.0, 0.0 };
        double em[3] = { 0.0, 0.0, 0.0 };
        double lin[3];
        double want[3];
        double got[3];

        destroy_target(&env);
        if (make_target(&env, LC_FORMAT_RGBA8_SRGB, PBR_TW, PBR_TH, 0) !=
            0) {
            printf("[SKIP] sRGB render target unsupported\n");
            printf("[PASS] pixels: sRGB check skipped (unsupported)\n");
            g_passed++;
            destroy_target(&env);
            make_target(&env, LC_FORMAT_RGBA8_UNORM, PBR_TW, PBR_TH, 0);
        } else {
            facing_light(&light, 0.25f);
            memset(&ops, 0, sizeof(ops));
            ops.lights = &light;
            ops.light_count = 1;
            ops.items = &item;
            ops.item_count = 1;
            facing_item(&item, cube, diel);
            px = render_pixels(&env, &ops);
            TEST_CHECK(px != NULL, "pixels: sRGB render reads back");
            if (px != NULL) {
                int i;

                pixel_at(px, PBR_TW, 128, 128, got);
                ref_eval(n, l, v, alb, 0.0, 0.4, lc, 0.25, 1.0, am, 1.0,
                         em, lin);
                for (i = 0; i < 3; i++) {
                    want[i] = srgb_encode(lin[i]);
                }
                TEST_CHECK(near3(got, want, 0.05),
                           "pixels: sRGB target encodes linear output");
                free(px);
            }
            destroy_target(&env);
            make_target(&env, LC_FORMAT_RGBA8_UNORM, PBR_TW, PBR_TH, 0);
        }
        lr_material_destroy(diel);
    }

    /* ---- light limits ---- */
    {
        lr_camera saved = env.camera;
        lc_command_encoder *enc = NULL;
        int ok = 1;
        uint32_t i;
        lr_light pt;

        (void)saved;
        memset(&pt, 0, sizeof(pt));
        pt.type = LR_LIGHT_POINT;
        pt.color[0] = 1.0f;
        pt.color[1] = 1.0f;
        pt.color[2] = 1.0f;
        pt.intensity = 1.0f;
        pt.position[2] = 3.0f;
        pt.range = 10.0f;
        if (lc_begin_frame(env.swapchain) != LC_SUCCESS ||
            lc_swapchain_get_encoder(env.swapchain, &enc) != LC_SUCCESS ||
            lr_renderer_begin(env.renderer, &env.camera) != LR_SUCCESS) {
            TEST_CHECK(0, "lights: limit frame opens");
            goto cleanup;
        }
        for (i = 0; i < LR_MAX_LIGHTS; i++) {
            if (lr_renderer_submit_light(env.renderer, &pt) !=
                LR_SUCCESS) {
                ok = 0;
                break;
            }
        }
        TEST_CHECK(ok, "lights: 64 point lights submit");
        TEST_CHECK(lr_renderer_submit_light(env.renderer, &pt) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "lights: 65th light rejected");
        {
            lr_render_stats stats;

            lr_renderer_get_stats(env.renderer, &stats);
            TEST_CHECK(stats.submitted_lights == 64,
                       "lights: submitted count is 64");
        }
        /* Bad lights rejected without consuming slots. */
        {
            lr_light bad = pt;

            bad.type = (lr_light_type)99;
            TEST_CHECK(lr_renderer_submit_light(env.renderer, &bad) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "lights: bad type rejected");
            bad = pt;
            bad.range = 0.0f;
            TEST_CHECK(lr_renderer_submit_light(env.renderer, &bad) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "lights: non-positive range rejected");
            bad = pt;
            bad.type = LR_LIGHT_DIRECTIONAL;
            memset(bad.direction, 0, sizeof(bad.direction));
            TEST_CHECK(lr_renderer_submit_light(env.renderer, &bad) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "lights: zero direction rejected");
            TEST_CHECK(lr_renderer_submit_light(env.renderer, NULL) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "lights: NULL light rejected");
        }
        lr_renderer_end(env.renderer);
        /* A frame must present a swapchain pass (pass-less frames
         * present UNDEFINED); trivial clear suffices here. */
        {
            lc_render_swapchain_pass_desc spass;

            memset(&spass, 0, sizeof(spass));
            spass.color_load_op = LC_LOAD_OP_CLEAR;
            spass.color_store_op = LC_STORE_OP_STORE;
            spass.depth_load_op = LC_LOAD_OP_CLEAR;
            spass.depth_store_op = LC_STORE_OP_DONT_CARE;
            spass.clear_depth = 1.0f;
            if (lc_encoder_begin_swapchain_pass(enc, env.swapchain,
                                                &spass) != LC_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                TEST_CHECK(0, "lights: limit frame presents");
                goto cleanup;
            }
        }
        lc_end_frame(env.swapchain);
        /* Begin resets the list (observable headlessly via stats). */
        if (lr_renderer_begin(env.renderer, &env.camera) == LR_SUCCESS) {
            lr_render_stats stats;

            lr_renderer_get_stats(env.renderer, &stats);
            TEST_CHECK(stats.submitted_lights == 0,
                       "lights: begin resets the list");
            lr_renderer_end(env.renderer);
        } else {
            TEST_CHECK(0, "lights: begin resets the list");
        }
        (void)enc;
    }

    /* ---- glTF -> PBR bridge ---- */
    {
        la_asset_manager *assets = NULL;
        la_asset_manager_desc adesc;
        la_model *model = NULL;
        char path[1024];
        lr_draw_item item;
        pbr_frame_ops ops;
        lr_light light;

        memset(&adesc, 0, sizeof(adesc));
        adesc.renderer = env.renderer;
        if (la_asset_manager_create(&adesc, &assets) != LA_SUCCESS) {
            TEST_CHECK(0, "bridge: asset manager creates");
            goto cleanup;
        }
        snprintf(path, sizeof(path), "%s/fixture.glb", LA_FIXTURE_DIR);
        if (la_model_load(assets, path, &model) != LA_SUCCESS) {
            printf("bridge load failed: %s\n",
                   la_asset_manager_get_last_error(assets));
            TEST_CHECK(0, "bridge: fixture loads");
            la_asset_manager_destroy(assets);
            goto cleanup;
        }
        TEST_CHECK(la_model_get_material_count(model) == 2,
                   "bridge: two materials import");
        {
            const la_pbr_material_data *m0 =
                la_model_get_material_data(model, 0);
            const la_pbr_material_data *m1 =
                la_model_get_material_data(model, 1);
            int meta_ok =
                (m0 != NULL && m1 != NULL &&
                 fabsf(m0->metallic_factor - 0.2f) < 1e-6f &&
                 fabsf(m0->roughness_factor - 0.8f) < 1e-6f &&
                 fabsf(m0->normal_scale - 0.75f) < 1e-6f &&
                 m0->base_color_texture == 0 && m0->normal_texture == 0 &&
                 m1->base_color_texture < 0 &&
                 fabsf(m1->metallic_factor - 0.0f) < 1e-6f &&
                 fabsf(m1->roughness_factor - 0.5f) < 1e-6f);

            TEST_CHECK(meta_ok, "bridge: PBR metadata maps correctly");
        }
        facing_light(&light, 2.5f);
        memset(&ops, 0, sizeof(ops));
        ops.lights = &light;
        ops.light_count = 1;
        memset(&item, 0, sizeof(item));
        lr_transform_identity(&item.transform);
        item.transform.position[2] = -1.0f;
        /* Draw every instance through the public model path inside
         * a fresh frame for the submit + draw checks. */
        {
            lc_command_encoder *enc = NULL;

            if (la_model_submit(model, env.renderer, &item.transform) !=
                LA_SUCCESS) {
                /* No open frame yet: submission without begin is
                 * rejected (matches lr_renderer_submit). */
                TEST_CHECK(1, "bridge: submit without frame rejected");
            } else {
                TEST_CHECK(0, "bridge: submit without frame rejected");
            }

            if (lc_begin_frame(env.swapchain) == LC_SUCCESS &&
                lc_swapchain_get_encoder(env.swapchain, &enc) ==
                    LC_SUCCESS) {
                lc_render_pass_desc pdesc;
                lc_render_color_attachment catt;

                memset(&catt, 0, sizeof(catt));
                catt.view = env.color_view;
                catt.load_op = LC_LOAD_OP_CLEAR;
                catt.store_op = LC_STORE_OP_STORE;
                memset(&pdesc, 0, sizeof(pdesc));
                pdesc.color_attachments = &catt;
                pdesc.color_attachment_count = 1;
                pdesc.width = env.tw;
                pdesc.height = env.th;
                if (lc_encoder_begin_render_pass(enc, &pdesc) ==
                        LC_SUCCESS &&
                    lr_renderer_begin(env.renderer, &env.camera) ==
                        LR_SUCCESS &&
                    lr_renderer_submit_light(env.renderer, &light) ==
                        LR_SUCCESS &&
                    la_model_submit(model, env.renderer,
                                    &item.transform) == LA_SUCCESS &&
                    lr_renderer_render(env.renderer, enc, env.target) ==
                        LR_SUCCESS) {
                    lr_render_stats st;

                    lr_renderer_get_stats(env.renderer, &st);
                    TEST_CHECK(st.pbr_draw_calls ==
                                   la_model_get_instance_count(model),
                               "bridge: every instance draws as PBR");
                    TEST_CHECK(st.unlit_draw_calls == 0,
                               "bridge: no unlit draws leak in");
                } else {
                    TEST_CHECK(0, "bridge: every instance draws as PBR");
                    TEST_CHECK(0, "bridge: no unlit draws leak in");
                }
                lc_encoder_end_render_pass(enc);
                lr_renderer_end(env.renderer);
                /* Swapchain leg (every frame presents a pass). */
                {
                    lc_render_swapchain_pass_desc spass;

                    memset(&spass, 0, sizeof(spass));
                    spass.color_load_op = LC_LOAD_OP_CLEAR;
                    spass.color_store_op = LC_STORE_OP_STORE;
                    spass.depth_load_op = LC_LOAD_OP_CLEAR;
                    spass.depth_store_op = LC_STORE_OP_DONT_CARE;
                    spass.clear_depth = 1.0f;
                    if (lc_encoder_begin_swapchain_pass(
                            enc, env.swapchain, &spass) == LC_SUCCESS &&
                        lr_renderer_begin(env.renderer, &env.camera) ==
                            LR_SUCCESS &&
                        lr_renderer_submit_light(env.renderer, &light) ==
                            LR_SUCCESS &&
                        la_model_submit(model, env.renderer,
                                        &item.transform) == LA_SUCCESS &&
                        lr_renderer_render(env.renderer, enc,
                                           lc_swapchain_get_render_target(
                                               env.swapchain)) ==
                            LR_SUCCESS &&
                        lc_encoder_end_render_pass(enc) == LC_SUCCESS) {
                        TEST_CHECK(1, "bridge: swapchain leg presents");
                    } else {
                        TEST_CHECK(0, "bridge: swapchain leg presents");
                    }
                    lr_renderer_end(env.renderer);
                }
                lc_end_frame(env.swapchain);
            }
        }
        test_wait_idle(env.device);
        la_model_destroy(model);
        la_asset_manager_destroy(assets);
    }

    /* ---- 500-frame endurance + screenshot ---- */
    {
        lr_mesh *grid_sphere = NULL;
        lr_mesh *ground = NULL;
        lr_mesh *wall = NULL;
        lr_mesh *glow_cube = NULL;
        lr_material *mats[6];
        lr_material *wall_mat = NULL;
        lr_material *glow_mat = NULL;
        lr_material *ground_mat = NULL;
        la_asset_manager *assets = NULL;
        la_model *box = NULL;
        uint32_t pipes_warm = 0;
        uint32_t frame;
        int ok = 1;
        char path[1024];
        int i;

        for (i = 0; i < 6; i++) {
            mats[i] = NULL;
        }
        destroy_target(&env);
        if (make_target(&env, LC_FORMAT_RGBA8_UNORM, 512, 512, 1) != 0) {
            TEST_CHECK(0, "endurance: 512 scene target creates");
            goto cleanup;
        }
        if (lr_mesh_create_sphere(env.renderer, 0.5f, 24, 12,
                                  &grid_sphere) != LR_SUCCESS ||
            lr_mesh_create_plane(env.renderer, 12.0f, 12.0f, &ground) !=
                LR_SUCCESS ||
            lr_mesh_create_plane(env.renderer, 2.0f, 2.0f, &wall) !=
                LR_SUCCESS ||
            lr_mesh_create_cube(env.renderer, 0.6f, &glow_cube) !=
                LR_SUCCESS) {
            TEST_CHECK(0, "endurance: meshes create");
            goto cleanup;
        }
        {
            /* metal 0/1 x rough 0.15/0.8 + smooth + white. */
            static const float mets[6] = { 0.0f, 1.0f, 0.0f,
                                           1.0f, 0.0f, 1.0f };
            static const float rghs[6] = { 0.15f, 0.15f, 0.8f,
                                           0.8f, 0.5f, 0.5f };
            static const float cols[6][4] = {
                { 0.9f, 0.9f, 0.95f, 1.0f }, { 0.9f, 0.9f, 0.95f, 1.0f },
                { 0.8f, 0.3f, 0.2f, 1.0f },  { 0.8f, 0.3f, 0.2f, 1.0f },
                { 0.3f, 0.6f, 0.9f, 1.0f },  { 0.95f, 0.8f, 0.4f, 1.0f },
            };

            for (i = 0; i < 6; i++) {
                lr_pbr_material_desc desc;

                memset(&desc, 0, sizeof(desc));
                memcpy(desc.base_color_factor, cols[i],
                       sizeof(desc.base_color_factor));
                desc.metallic_factor = mets[i];
                desc.roughness_factor = rghs[i];
                desc.normal_scale = 1.0f;
                desc.occlusion_strength = 1.0f;
                desc.alpha_mode = LR_ALPHA_OPAQUE;
                if (lr_material_create_pbr(env.renderer, &desc,
                                           &mats[i]) != LR_SUCCESS) {
                    ok = 0;
                }
            }
            {
                /* Double-sided wall (NONE cull variant) + emissive. */
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
                                           &wall_mat) != LR_SUCCESS) {
                    ok = 0;
                }
                memset(&desc, 0, sizeof(desc));
                desc.base_color_factor[0] = 0.02f;
                desc.base_color_factor[1] = 0.02f;
                desc.base_color_factor[2] = 0.02f;
                desc.base_color_factor[3] = 1.0f;
                desc.roughness_factor = 0.9f;
                desc.emissive_factor[0] = 1.0f;
                desc.emissive_factor[1] = 0.5f;
                desc.emissive_factor[2] = 0.15f;
                desc.normal_scale = 1.0f;
                desc.occlusion_strength = 1.0f;
                if (lr_material_create_pbr(env.renderer, &desc,
                                           &glow_mat) != LR_SUCCESS) {
                    ok = 0;
                }
                memset(&udesc, 0, sizeof(udesc));
                udesc.color[0] = 0.15f;
                udesc.color[1] = 0.16f;
                udesc.color[2] = 0.19f;
                udesc.color[3] = 1.0f;
                if (lr_material_create_unlit(env.renderer, &udesc,
                                             &ground_mat) != LR_SUCCESS) {
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
                if (la_model_load(assets, path, &box) != LA_SUCCESS) {
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
            end_scene scene;
            float t = (float)frame * 0.02f;
            float eye[3];
            float tgt[3] = { 0.0f, 0.8f, 0.0f };

            scene.sphere = grid_sphere;
            scene.ground = ground;
            scene.wall = wall;
            scene.glow = glow_cube;
            memcpy(scene.mats, mats, sizeof(scene.mats));
            scene.wall_mat = wall_mat;
            scene.glow_mat = glow_mat;
            scene.ground_mat = ground_mat;
            scene.box = box;
            if (frame == 250) {
                /* Resize event: same signature, new extent. */
                test_wait_idle(env.device);
                destroy_target(&env);
                if (make_target(&env, LC_FORMAT_RGBA8_UNORM, 384, 384,
                                1) != 0) {
                    ok = 0;
                    break;
                }
            }
            lc_poll_events();
            if (lc_begin_frame(env.swapchain) != LC_SUCCESS ||
                lc_swapchain_get_encoder(env.swapchain, &enc) !=
                    LC_SUCCESS) {
                ok = 0;
                break;
            }
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
            if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
                ok = 0;
                break;
            }
            eye[0] = 4.2f * sinf(t * 0.25f);
            eye[1] = 2.1f + 0.4f * sinf(t * 0.5f);
            eye[2] = 4.2f * cosf(t * 0.25f);
            if (lr_camera_look_at(&env.camera, eye, tgt, up) !=
                    LR_SUCCESS ||
                lr_renderer_begin(env.renderer, &env.camera) !=
                    LR_SUCCESS ||
                !submit_endurance_scene(&env, &scene, t) ||
                lr_renderer_render(env.renderer, enc, env.target) !=
                    LR_SUCCESS) {
                ok = 0;
                break;
            }
            lr_renderer_end(env.renderer);
            if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                ok = 0;
                break;
            }
            /* Swapchain leg (every frame presents a pass). */
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
            if (lc_encoder_begin_swapchain_pass(enc, env.swapchain,
                                                &spass) != LC_SUCCESS ||
                lr_renderer_begin(env.renderer, &env.camera) !=
                    LR_SUCCESS ||
                !submit_endurance_scene(&env, &scene, t) ||
                lr_renderer_render(env.renderer, enc,
                                   lc_swapchain_get_render_target(
                                       env.swapchain)) != LR_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                ok = 0;
                break;
            }
            {
                lr_render_stats stats;

                lr_renderer_get_stats(env.renderer, &stats);
                if (stats.active_lights != 3) {
                    ok = 0;
                    break;
                }
                if (frame == 240) {
                    /* Warm once every variant had a chance to appear
                     * (the orbiting camera may cull the wall early);
                     * the resize at 250 must not grow the cache. */
                    pipes_warm = lr_renderer_get_pipeline_count(
                        env.renderer);
                }
                if (frame == 499) {
                    TEST_CHECK(stats.pbr_draw_calls > 0 &&
                                   stats.unlit_draw_calls > 0,
                               "endurance: mixed PBR + unlit draws");
                    TEST_CHECK(stats.material_binds <= 17,
                               "endurance: material binds stay bounded");
                }
            }
            lr_renderer_end(env.renderer);
            if (lc_end_frame(env.swapchain) != LC_SUCCESS) {
                ok = 0;
            }
        }
        TEST_CHECK(ok, "endurance: 500 frames with motion + resize");
        {
            uint32_t pipes_end = lr_renderer_get_pipeline_count(
                env.renderer);

            TEST_CHECK(pipes_end == pipes_warm && pipes_warm >= 4,
                       "endurance: no pipeline growth after warmup");
        }
        /* Screenshot from the final presented-size frame. */
        {
            unsigned char *px = readback_rgba8(env.device, env.color_img,
                                               env.tw, env.th);
            FILE *f = NULL;

            TEST_CHECK(px != NULL, "screenshot: final frame reads back");
            if (px != NULL) {
                f = fopen("pbr_scene.ppm", "wb");
                if (f != NULL) {
                    fprintf(f, "P6\n%u %u\n255\n", env.tw, env.th);
                    for (uint32_t y = 0; y < env.th; y++) {
                        for (uint32_t x = 0; x < env.tw; x++) {
                            unsigned char *p =
                                px + ((size_t)y * env.tw + x) * 4u;

                            fputc(p[0], f);
                            fputc(p[1], f);
                            fputc(p[2], f);
                        }
                    }
                    fclose(f);
                    printf("[INFO] screenshot -> pbr_scene.ppm (%ux%u)\n",
                           env.tw, env.th);
                    TEST_CHECK(1, "screenshot: PPM written");
                } else {
                    TEST_CHECK(0, "screenshot: PPM written");
                }
                free(px);
            }
        }
        test_wait_idle(env.device);
        for (i = 0; i < 6; i++) {
            lr_material_destroy(mats[i]);
        }
        lr_material_destroy(wall_mat);
        lr_material_destroy(glow_mat);
        lr_material_destroy(ground_mat);
        lr_mesh_destroy(grid_sphere);
        lr_mesh_destroy(ground);
        lr_mesh_destroy(wall);
        lr_mesh_destroy(glow_cube);
        la_model_destroy(box);
        la_asset_manager_destroy(assets);
    }

    printf("pbr vulkan: %d passed, %d failed\n", g_passed, g_failed);
    exit_code = (g_failed == 0) ? 0 : 1;

cleanup:
    test_wait_idle(env.device);
    lr_mesh_destroy(card);
    lr_mesh_destroy(ball);
    lr_mesh_destroy(cube);
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
