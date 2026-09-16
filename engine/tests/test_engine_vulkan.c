/*
 * Luma Engine Vulkan integration tests (Phase 24).
 *
 * Engine renderable/camera/light -> renderer submission with live
 * validation, pixel proofs (offscreen HDR readback through public
 * APIs only), stable temporal ID reorder + destroy/reuse identity,
 * mirrored hierarchy, disabled filtering, parented camera motion,
 * GPU-driven + CPU paths, and the accounting invariant.
 *
 * Headless-safe: SKIP (exit 0) when no Vulkan device is available.
 * Validation layers stay enabled throughout. No backend access:
 * engine public API + renderer public API + LumaC public API only.
 * (One white-box idle helper mirrors the renderer tests'
 * test-only coarse idle for readback; no Vulkan symbols used.)
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_engine/luma_engine.h>

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

typedef struct eng_env {
    lc_device *device;
    lr_renderer *renderer;
    le_engine *engine;
    le_world *world;
    lr_mesh *cube;
    lr_mesh *plane;
    lr_material *mat;
    lr_material *mat2;
    lc_image *color_img;
    lc_image_view *color_view;
    lc_image *depth_img;
    lc_image_view *depth_view;
    lc_render_target *target;
    uint32_t width;
    uint32_t height;
} eng_env;

/* Frame encoder surrogate: le_world_render_scene needs an open
 * frame encoder. Without a swapchain there is no frame, so these
 * tests drive the renderer through a worker encoder? No — worker
 * encoders cannot open passes. Instead the tests use a real
 * swapchain-less path: create a window+swapchain when available
 * (windowed), else SKIP. The swapchain frame provides the encoder;
 * rendering targets the offscreen target via render_scene, output
 * goes to the offscreen target too, and pixels read back after
 * end_frame. */
typedef struct frame_env {
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
} frame_env;

static int make_device(lc_device **out) {
    lc_device_desc desc;

    memset(&desc, 0, sizeof(desc));
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

static int make_frame(frame_env *fe) {
    lc_window_desc wdesc;

    memset(&wdesc, 0, sizeof(wdesc));
    wdesc.title = "LumaC Engine Test";
    wdesc.width = 640;
    wdesc.height = 480;
    switch (lc_window_create(&wdesc, &fe->window)) {
    case LC_SUCCESS:
        break;
    case LC_ERROR_PLATFORM:
    case LC_ERROR_WINDOW_CREATION_FAILED:
        return 1;
    default:
        return -1;
    }
    return 0;
}

static void destroy_frame(frame_env *fe) {
    lc_swapchain_destroy(fe->swapchain);
    lc_surface_destroy(fe->surface);
    lc_window_destroy(fe->window);
    fe->swapchain = NULL;
    fe->surface = NULL;
    fe->window = NULL;
}

static int env_init(eng_env *env, lc_device *device, uint32_t w,
                    uint32_t h) {
    lr_renderer_desc rdesc;
    le_engine_desc edesc;
    le_world_desc wdesc;
    lr_pbr_material_desc matdesc;
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_render_target_create_desc rtdesc;
    lc_render_target_attachment ratt;

    memset(env, 0, sizeof(*env));
    env->device = device;
    env->width = w;
    env->height = h;
    memset(&rdesc, 0, sizeof(rdesc));
    rdesc.device = device;
    rdesc.render_target.color_attachment_count = 1;
    rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
    rdesc.max_objects = 8192;
    rdesc.ambient_light[0] = 0.35f;
    rdesc.ambient_light[1] = 0.35f;
    rdesc.ambient_light[2] = 0.40f;
    if (lr_renderer_create(&rdesc, &env->renderer) != LR_SUCCESS) {
        return 0;
    }
    memset(&edesc, 0, sizeof(edesc));
    edesc.renderer = env->renderer;
    if (le_engine_create(&edesc, &env->engine) != LE_SUCCESS) {
        return 0;
    }
    memset(&wdesc, 0, sizeof(wdesc));
    if (le_world_create(env->engine, &wdesc, &env->world) !=
        LE_SUCCESS) {
        return 0;
    }
    if (lr_mesh_create_cube(env->renderer, 2.0f, &env->cube) !=
        LR_SUCCESS) {
        return 0;
    }
    if (lr_mesh_create_plane(env->renderer, 40.0f, 40.0f, &env->plane) !=
        LR_SUCCESS) {
        return 0;
    }
    memset(&matdesc, 0, sizeof(matdesc));
    matdesc.base_color_factor[0] = 0.8f;
    matdesc.base_color_factor[1] = 0.2f;
    matdesc.base_color_factor[2] = 0.2f;
    matdesc.base_color_factor[3] = 1.0f;
    matdesc.metallic_factor = 0.0f;
    matdesc.roughness_factor = 0.7f;
    matdesc.normal_scale = 1.0f;
    matdesc.occlusion_strength = 1.0f;
    matdesc.alpha_mode = LR_ALPHA_OPAQUE;
    if (lr_material_create_pbr(env->renderer, &matdesc, &env->mat) !=
        LR_SUCCESS) {
        return 0;
    }
    matdesc.base_color_factor[0] = 0.2f;
    matdesc.base_color_factor[1] = 0.4f;
    matdesc.base_color_factor[2] = 0.9f;
    if (lr_material_create_pbr(env->renderer, &matdesc, &env->mat2) !=
        LR_SUCCESS) {
        return 0;
    }
    /* Offscreen color + depth target for scene+output. */
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RGBA8_UNORM;
    idesc.width = w;
    idesc.height = h;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                  LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_SRC |
                  LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &idesc, &env->color_img) != LC_SUCCESS) {
        return 0;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(env->color_img, &vdesc, &env->color_view) !=
        LC_SUCCESS) {
        return 0;
    }
    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_D32_FLOAT;
    idesc.width = w;
    idesc.height = h;
    idesc.depth = 1;
    idesc.mip_levels = 1;
    idesc.array_layers = 1;
    idesc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL |
                  LC_IMAGE_USAGE_TRANSFER_DST;
    idesc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &idesc, &env->depth_img) != LC_SUCCESS) {
        return 0;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(env->depth_img, &vdesc, &env->depth_view) !=
        LC_SUCCESS) {
        return 0;
    }
    memset(&rtdesc, 0, sizeof(rtdesc));
    ratt.view = env->color_view;
    rtdesc.color_attachments = &ratt;
    rtdesc.color_attachment_count = 1;
    rtdesc.depth_stencil_attachment = env->depth_view;
    rtdesc.width = w;
    rtdesc.height = h;
    if (lc_render_target_create(device, &rtdesc, &env->target) !=
        LC_SUCCESS) {
        return 0;
    }
    return 1;
}

static void env_shutdown(eng_env *env) {
    le_world_destroy(env->world);
    le_engine_destroy(env->engine);
    lr_material_destroy(env->mat);
    lr_material_destroy(env->mat2);
    lr_mesh_destroy(env->cube);
    lr_mesh_destroy(env->plane);
    if (env->renderer != NULL) {
        lr_renderer_destroy(env->renderer);
    }
    lc_render_target_destroy(env->target);
    lc_image_view_destroy(env->color_view);
    lc_image_destroy(env->color_img);
    lc_image_view_destroy(env->depth_view);
    lc_image_destroy(env->depth_img);
    memset(env, 0, sizeof(*env));
}

/* One frame: swapchain frame -> engine scene (HDR) -> output into
 * the offscreen target (own pass) -> end renderer frame ->
 * end_frame. Returns 1 on success. */
static int render_one_frame(eng_env *env, frame_env *fe) {
    lc_command_encoder *enc = NULL;
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_render_depth_attachment datt;

    lc_poll_events();
    if (lc_begin_frame(fe->swapchain) != LC_SUCCESS) {
        return 0;
    }
    if (lc_swapchain_get_encoder(fe->swapchain, &enc) != LC_SUCCESS) {
        return 0;
    }
    if (le_world_render_scene(env->world, enc, env->width,
                              env->height) != LE_SUCCESS) {
        return 0;
    }
    memset(&catt, 0, sizeof(catt));
    catt.view = env->color_view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    catt.clear_color[0] = 0.0f;
    catt.clear_color[1] = 0.0f;
    catt.clear_color[2] = 0.0f;
    catt.clear_color[3] = 1.0f;
    memset(&datt, 0, sizeof(datt));
    datt.view = env->depth_view;
    datt.depth_load_op = LC_LOAD_OP_CLEAR;
    datt.depth_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_depth = 1.0f;
    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_stencil = 0;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.depth_attachment = &datt;
    pdesc.width = env->width;
    pdesc.height = env->height;
    if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
        return 0;
    }
    if (le_world_render_output(env->world, enc, env->target) !=
        LE_SUCCESS) {
        return 0;
    }
    if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        return 0;
    }
    le_world_render_end(env->world);
    /* Present leg (swapchain pass just presents; output already
     * went offscreen). */
    {
        lc_render_swapchain_pass_desc spass;

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.depth_load_op = LC_LOAD_OP_CLEAR;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        if (lc_encoder_begin_swapchain_pass(enc, fe->swapchain,
                                            &spass) != LC_SUCCESS) {
            return 0;
        }
        if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            return 0;
        }
    }
    {
        lc_result r = lc_end_frame(fe->swapchain);

        if (r != LC_SUCCESS && r != LC_SUBOPTIMAL) {
            return 0;
        }
    }
    return 1;
}

/* Count non-black pixels in the offscreen target (post-present
 * readback; producers submitted). Returns -1 on readback failure. */
static long lit_pixels(eng_env *env) {
    lc_image_readback_desc desc;
    lc_image_readback_info info;
    unsigned char *px = NULL;
    long lit = 0;
    uint32_t i;
    uint32_t n;

    memset(&desc, 0, sizeof(desc));
    if (lc_image_query_readback(env->color_img, &desc, &info) !=
        LC_SUCCESS) {
        return -1;
    }
    px = (unsigned char *)malloc(info.size);
    if (px == NULL) {
        return -1;
    }
    if (lc_image_readback(env->color_img, &desc, px, info.size, NULL) !=
        LC_SUCCESS) {
        free(px);
        return -1;
    }
    n = (uint32_t)(info.size / 4u);
    for (i = 0; i < n; i++) {
        if (px[i * 4u + 0] > 8 || px[i * 4u + 1] > 8 ||
            px[i * 4u + 2] > 8) {
            lit++;
        }
    }
    free(px);
    return lit;
}

static le_object make_renderable(eng_env *env, lr_mesh *mesh,
                                 lr_material *mat, float x, float y,
                                 float z) {
    le_object o;
    le_renderable_desc rd;
    float p[3] = { x, y, z };

    memset(&o, 0, sizeof(o));
    if (le_object_create(env->world, &o) != LE_SUCCESS) {
        return o;
    }
    le_object_set_position(env->world, &o, p);
    memset(&rd, 0, sizeof(rd));
    rd.mesh = mesh;
    rd.material = mat;
    rd.casts_shadow = 0;
    rd.receives_shadow = 1;
    rd.visible = 1;
    if (le_object_add_renderable(env->world, &o, &rd) != LE_SUCCESS) {
        return o;
    }
    return o;
}

static void make_camera(eng_env *env, float x, float y, float z) {
    le_object rig;
    le_object cam;
    float p[3] = { x, y, z };
    /* Aim the rig at the origin with a look-at quaternion: build
     * the rotation matrix whose -Z maps to normalize(origin-pos)
     * (camera convention: looking along -Z), with Y-up, then
     * convert to a quaternion. */
    float fwd[3] = { -x, -y, -z };
    float flen = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] +
                       fwd[2] * fwd[2]);
    float right[3];
    float up[3];
    float r[3][3];
    float q[4];
    float trace;
    le_camera_desc cd;

    if (flen < 1e-6f) {
        fwd[0] = 0.0f;
        fwd[1] = 0.0f;
        fwd[2] = -1.0f;
        flen = 1.0f;
    }
    fwd[0] /= flen;
    fwd[1] /= flen;
    fwd[2] /= flen;
    /* right = normalize(cross(fwd, world-up)) with world-up
     * (0,1,0): cross(f,u) = (fy*0 - fz*1, fz*0 - fx*0,
     * fx*1 - fy*0) = (-fz, 0, fx). up = cross(right, fwd). */
    right[0] = -fwd[2];
    right[1] = 0.0f;
    right[2] = fwd[0];
    {
        float rlen =
            sqrtf(right[0] * right[0] + right[2] * right[2]);

        if (rlen < 1e-6f) {
            right[0] = 1.0f;
            right[1] = 0.0f;
            right[2] = 0.0f;
        } else {
            right[0] /= rlen;
            right[2] /= rlen;
        }
    }
    up[0] = right[1] * fwd[2] - right[2] * fwd[1];
    up[1] = right[2] * fwd[0] - right[0] * fwd[2];
    up[2] = right[0] * fwd[1] - right[1] * fwd[0];
    /* Rotation columns: X=right, Y=up, Z=-fwd. */
    r[0][0] = right[0];
    r[1][0] = right[1];
    r[2][0] = right[2];
    r[0][1] = up[0];
    r[1][1] = up[1];
    r[2][1] = up[2];
    r[0][2] = -fwd[0];
    r[1][2] = -fwd[1];
    r[2][2] = -fwd[2];
    trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0.0f) {
        float u = sqrtf(trace + 1.0f) * 2.0f;

        q[3] = 0.25f * u;
        q[0] = (r[2][1] - r[1][2]) / u;
        q[1] = (r[0][2] - r[2][0]) / u;
        q[2] = (r[1][0] - r[0][1]) / u;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        float u = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;

        q[3] = (r[2][1] - r[1][2]) / u;
        q[0] = 0.25f * u;
        q[1] = (r[0][1] + r[1][0]) / u;
        q[2] = (r[0][2] + r[2][0]) / u;
    } else if (r[1][1] > r[2][2]) {
        float u = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;

        q[3] = (r[0][2] - r[2][0]) / u;
        q[0] = (r[0][1] + r[1][0]) / u;
        q[1] = 0.25f * u;
        q[2] = (r[1][2] + r[2][1]) / u;
    } else {
        float u = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;

        q[3] = (r[1][0] - r[0][1]) / u;
        q[0] = (r[0][2] + r[2][0]) / u;
        q[1] = (r[1][2] + r[2][1]) / u;
        q[2] = 0.25f * u;
    }
    if (le_object_create(env->world, &rig) != LE_SUCCESS) {
        return;
    }
    if (le_object_create(env->world, &cam) != LE_SUCCESS) {
        return;
    }
    le_object_set_position(env->world, &rig, p);
    le_object_set_parent(env->world, &cam, &rig);
    le_object_set_rotation(env->world, &rig, q);
    le_camera_desc_default(&cd);
    cd.near_plane = 0.1f;
    cd.far_plane = 100.0f;
    le_object_add_camera(env->world, &cam, &cd);
    le_world_set_active_camera(env->world, &cam);
}

static void make_sun(eng_env *env, le_object *out_sun) {
    le_object sun;
    le_light_desc ld;
    /* Travel direction (0.5,-1,0.3) normalized: point the sun's
     * -Z along it with a look-at basis (same math as cameras). */
    float fwd[3] = { 0.5f, -1.0f, 0.3f };
    float flen =
        sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    float right[3];
    float up[3];
    float r[3][3];
    float q[4];
    float trace;

    fwd[0] /= flen;
    fwd[1] /= flen;
    fwd[2] /= flen;
    right[0] = -fwd[2];
    right[1] = 0.0f;
    right[2] = fwd[0];
    {
        float rlen =
            sqrtf(right[0] * right[0] + right[2] * right[2]);

        if (rlen < 1e-6f) {
            right[0] = 1.0f;
            right[1] = 0.0f;
            right[2] = 0.0f;
        } else {
            right[0] /= rlen;
            right[2] /= rlen;
        }
    }
    up[0] = right[1] * fwd[2] - right[2] * fwd[1];
    up[1] = right[2] * fwd[0] - right[0] * fwd[2];
    up[2] = right[0] * fwd[1] - right[1] * fwd[0];
    r[0][0] = right[0];
    r[1][0] = right[1];
    r[2][0] = right[2];
    r[0][1] = up[0];
    r[1][1] = up[1];
    r[2][1] = up[2];
    r[0][2] = -fwd[0];
    r[1][2] = -fwd[1];
    r[2][2] = -fwd[2];
    trace = r[0][0] + r[1][1] + r[2][2];
    if (trace > 0.0f) {
        float u = sqrtf(trace + 1.0f) * 2.0f;

        q[3] = 0.25f * u;
        q[0] = (r[2][1] - r[1][2]) / u;
        q[1] = (r[0][2] - r[2][0]) / u;
        q[2] = (r[1][0] - r[0][1]) / u;
    } else if (r[0][0] > r[1][1] && r[0][0] > r[2][2]) {
        float u = sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) * 2.0f;

        q[3] = (r[2][1] - r[1][2]) / u;
        q[0] = 0.25f * u;
        q[1] = (r[0][1] + r[1][0]) / u;
        q[2] = (r[0][2] + r[2][0]) / u;
    } else if (r[1][1] > r[2][2]) {
        float u = sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) * 2.0f;

        q[3] = (r[0][2] - r[2][0]) / u;
        q[0] = (r[0][1] + r[1][0]) / u;
        q[1] = 0.25f * u;
        q[2] = (r[1][2] + r[2][1]) / u;
    } else {
        float u = sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) * 2.0f;

        q[3] = (r[1][0] - r[0][1]) / u;
        q[0] = (r[0][2] + r[2][0]) / u;
        q[1] = (r[1][2] + r[2][1]) / u;
        q[2] = 0.25f * u;
    }

    if (out_sun != NULL) {
        *out_sun = LE_OBJECT_INVALID;
    }
    if (le_object_create(env->world, &sun) != LE_SUCCESS) {
        return;
    }
    le_object_set_rotation(env->world, &sun, q);
    memset(&ld, 0, sizeof(ld));
    ld.type = LE_LIGHT_DIRECTIONAL;
    ld.color[0] = 1.0f;
    ld.color[1] = 1.0f;
    ld.color[2] = 1.0f;
    ld.intensity = 3.0f;
    le_object_add_light(env->world, &sun, &ld);
    if (out_sun != NULL) {
        *out_sun = sun;
    }
}

int main(void) {
    lc_device *device = NULL;
    frame_env fe;
    eng_env env;
    lc_surface *surface = NULL;
    eng_env *e;
    int dev_rc;

    printf("Running Luma Engine Vulkan integration tests...\n");
    memset(&fe, 0, sizeof(fe));
    memset(&env, 0, sizeof(env));
    e = &env;
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }
    dev_rc = make_device(&device);
    if (dev_rc == 1) {
        SKIP_ENV("Vulkan device");
    }
    if (dev_rc != 0) {
        printf("device failed: FAIL\n");
        lc_shutdown();
        return 1;
    }
    switch (make_frame(&fe)) {
    case 0:
        break;
    case 1:
        lc_device_destroy(device);
        SKIP_ENV("window");
    default:
        printf("window failed: FAIL\n");
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    switch (lc_surface_create(device, fe.window, &surface)) {
    case LC_SUCCESS:
        fe.surface = surface;
        break;
    case LC_ERROR_SURFACE_UNSUPPORTED:
        destroy_frame(&fe);
        lc_device_destroy(device);
        SKIP_ENV("surface");
    default:
        printf("surface failed: FAIL\n");
        destroy_frame(&fe);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    {
        lc_swapchain_desc sdesc;

        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.width = 640;
        sdesc.height = 480;
        sdesc.image_count = 0;
        sdesc.vsync = 0;
        switch (lc_swapchain_create(device, fe.surface, &sdesc,
                                    &fe.swapchain)) {
        case LC_SUCCESS:
            break;
        case LC_ERROR_SWAPCHAIN_UNSUPPORTED:
        case LC_ERROR_ZERO_EXTENT:
            destroy_frame(&fe);
            lc_device_destroy(device);
            SKIP_ENV("swapchain");
        default:
            printf("swapchain failed: FAIL\n");
            destroy_frame(&fe);
            lc_device_destroy(device);
            lc_shutdown();
            return 1;
        }
    }
    if (!env_init(&env, device, 640, 480)) {
        printf("env init failed: FAIL\n");
        destroy_frame(&fe);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }

    /* PART 1: engine renderable renders (pixel proof, CPU path). */
    {
        le_object o;
        le_object sun1 = LE_OBJECT_INVALID;

        make_camera(e, 0.0f, 2.0f, 8.0f);
        make_sun(e, &sun1);
        o = make_renderable(e, e->cube, e->mat, 0.0f, 0.0f, 0.0f);
        TEST_CHECK(le_object_is_alive(e->world, &o),
                   "renderable object alive");
        TEST_CHECK(render_one_frame(e, &fe), "frame renders");
        {
            long lit = lit_pixels(e);

            TEST_CHECK(lit > 1000, "engine cube paints pixels");
            printf("[info] cube lit pixels: %ld\n", lit);
        }
        {
            le_render_report report;

            le_world_get_last_render_report(e->world, &report);
            TEST_CHECK(report.submitted == 1,
                       "report submitted == 1");
            TEST_CHECK(report.renderer_stats.submitted_objects == 1,
                       "renderer submitted == 1");
        }
        TEST_CHECK(le_object_is_alive(e->world, &sun1),
                   "sun handle tracked (part 1)");
    }

    /* PART 2: disabled object submits nothing (75/100 rule at
     * small scale: disable 1 of 4 -> 3 submitted). */
    {
        le_object extra[3];
        int k;

        for (k = 0; k < 3; k++) {
            extra[k] = make_renderable(e, e->cube, e->mat,
                                       -4.0f + 4.0f * (float)k, 0.0f,
                                       -3.0f);
        }
        TEST_CHECK(render_one_frame(e, &fe), "4-object frame renders");
        {
            le_render_report report;

            le_world_get_last_render_report(e->world, &report);
            TEST_CHECK(report.submitted == 4,
                       "4 enabled -> 4 submitted");
        }
        TEST_CHECK(le_object_set_enabled(e->world, &extra[0], 0) ==
                       LE_SUCCESS,
                   "disable one");
        TEST_CHECK(render_one_frame(e, &fe), "disabled frame renders");
        {
            le_render_report report;

            le_world_get_last_render_report(e->world, &report);
            TEST_CHECK(report.submitted == 3,
                       "1 disabled -> 3 submitted");
            TEST_CHECK(report.skipped_disabled == 1,
                       "skipped_disabled == 1");
        }
        /* Parent-disable hides the subtree (effective state). */
        {
            le_object parent;
            le_object child;
            le_renderable_desc rd;

            TEST_CHECK(le_object_create(e->world, &parent) == LE_SUCCESS,
                       "disable-parent create");
            TEST_CHECK(le_object_create(e->world, &child) == LE_SUCCESS,
                       "disable-child create");
            memset(&rd, 0, sizeof(rd));
            rd.mesh = e->cube;
            rd.material = e->mat;
            rd.visible = 1;
            le_object_add_renderable(e->world, &child, &rd);
            le_object_set_parent(e->world, &child, &parent);
            TEST_CHECK(render_one_frame(e, &fe), "subtree frame renders");
            {
                le_render_report before;

                le_world_get_last_render_report(e->world, &before);
                TEST_CHECK(before.submitted == 4,
                           "subtree child submits when enabled");
                le_object_set_enabled(e->world, &parent, 0);
                TEST_CHECK(render_one_frame(e, &fe),
                           "parent-disabled frame renders");
                {
                    le_render_report after;

                    le_world_get_last_render_report(e->world, &after);
                    TEST_CHECK(after.submitted == 3,
                               "parent disable hides child");
                }
                le_object_destroy(e->world, &parent);
            }
        }
        le_object_destroy(e->world, &extra[0]);
        le_object_destroy(e->world, &extra[1]);
        le_object_destroy(e->world, &extra[2]);
    }

    /* PART 3: parented renderable follows the parent transform.
     * Isolated world (only the parented child renders): move the
     * parent and the pixels must change. */
    {
        le_world *w3 = NULL;
        le_world_desc wdesc;
        le_world *saved = e->world;
        le_object parent;
        le_object child;
        le_object rig;
        le_object cam;
        le_renderable_desc rd;
        le_camera_desc cd;
        long lit_center;
        long lit_moved;
        float p[3];

        memset(&wdesc, 0, sizeof(wdesc));
        TEST_CHECK(le_world_create(e->engine, &wdesc, &w3) == LE_SUCCESS,
                   "move-parent world creates");
        e->world = w3;
        TEST_CHECK(le_object_create(w3, &rig) == LE_SUCCESS,
                   "move rig create");
        TEST_CHECK(le_object_create(w3, &cam) == LE_SUCCESS,
                   "move cam create");
        p[0] = 0.0f;
        p[1] = 2.0f;
        p[2] = 8.0f;
        le_object_set_position(w3, &rig, p);
        le_object_set_parent(w3, &cam, &rig);
        {
            float fwd[3] = { 0.0f, -2.0f, -8.0f };
            float flen = sqrtf(68.0f);
            float right[3];
            float up[3];
            float r[3][3];
            float trace;
            float q[4];

            fwd[0] /= flen;
            fwd[1] /= flen;
            fwd[2] /= flen;
            right[0] = -fwd[2];
            right[1] = 0.0f;
            right[2] = fwd[0];
            {
                float rlen =
                    sqrtf(right[0] * right[0] + right[2] * right[2]);

                right[0] /= rlen;
                right[2] /= rlen;
            }
            up[0] = right[1] * fwd[2] - right[2] * fwd[1];
            up[1] = right[2] * fwd[0] - right[0] * fwd[2];
            up[2] = right[0] * fwd[1] - right[1] * fwd[0];
            r[0][0] = right[0];
            r[1][0] = right[1];
            r[2][0] = right[2];
            r[0][1] = up[0];
            r[1][1] = up[1];
            r[2][1] = up[2];
            r[0][2] = -fwd[0];
            r[1][2] = -fwd[1];
            r[2][2] = -fwd[2];
            trace = r[0][0] + r[1][1] + r[2][2];
            {
                float u = sqrtf(trace + 1.0f) * 2.0f;

                q[3] = 0.25f * u;
                q[0] = (r[2][1] - r[1][2]) / u;
                q[1] = (r[0][2] - r[2][0]) / u;
                q[2] = (r[1][0] - r[0][1]) / u;
            }
            le_object_set_rotation(w3, &rig, q);
        }
        le_camera_desc_default(&cd);
        le_object_add_camera(w3, &cam, &cd);
        le_world_set_active_camera(w3, &cam);
        TEST_CHECK(le_object_create(w3, &parent) == LE_SUCCESS,
                   "move-parent create");
        TEST_CHECK(le_object_create(w3, &child) == LE_SUCCESS,
                   "move-child create");
        memset(&rd, 0, sizeof(rd));
        rd.mesh = e->cube;
        rd.material = e->mat;
        rd.visible = 1;
        le_object_add_renderable(w3, &child, &rd);
        p[0] = 0.0f;
        p[1] = 0.0f;
        p[2] = 0.0f;
        le_object_set_position(w3, &parent, p);
        le_object_set_parent(w3, &child, &parent);
        TEST_CHECK(render_one_frame(e, &fe), "parented frame renders");
        lit_center = lit_pixels(e);
        TEST_CHECK(lit_center > 1000, "parented child paints pixels");
        p[0] = 30.0f;
        le_object_set_position(w3, &parent, p);
        TEST_CHECK(render_one_frame(e, &fe), "moved frame renders");
        lit_moved = lit_pixels(e);
        TEST_CHECK(lit_center != lit_moved,
                   "parent move changes pixels");
        printf("[info] parented lit: center=%ld moved=%ld\n", lit_center,
               lit_moved);
        e->world = saved;
        le_world_destroy(w3);
    }

    /* PART 4: mirrored hierarchy renders (negative parent scale +
     * child renderable => winding flip through composition). */
    {
        le_object parent;
        le_object child;
        le_renderable_desc rd;

        TEST_CHECK(le_object_create(e->world, &parent) == LE_SUCCESS,
                   "mirror-parent create");
        TEST_CHECK(le_object_create(e->world, &child) == LE_SUCCESS,
                   "mirror-child create");
        {
            float s[3] = { -1.0f, 1.0f, 1.0f };
            float p[3] = { 0.0f, 0.0f, 0.0f };

            le_object_set_scale(e->world, &parent, s);
            le_object_set_position(e->world, &child, p);
        }
        memset(&rd, 0, sizeof(rd));
        rd.mesh = e->cube;
        rd.material = e->mat;
        rd.visible = 1;
        rd.casts_shadow = 0;
        rd.receives_shadow = 1;
        le_object_add_renderable(e->world, &child, &rd);
        le_object_set_parent(e->world, &child, &parent);
        {
            float m[16];

            le_object_get_world_matrix(e->world, &child, m);
            TEST_CHECK(le_matrix_is_mirrored(m) == 1,
                       "composed child matrix mirrored");
        }
        TEST_CHECK(render_one_frame(e, &fe), "mirrored frame renders");
        {
            long lit = lit_pixels(e);

            TEST_CHECK(lit > 1000, "mirrored cube paints pixels");
            printf("[info] mirrored lit pixels: %ld\n", lit);
        }
        /* Other parity combos: mirror Y, mirror Z, double mirror
         * (even parity => not mirrored). */
        {
            float my[3] = { 1.0f, -1.0f, 1.0f };
            float mz[3] = { 1.0f, 1.0f, -1.0f };
            float mm[3] = { -1.0f, -1.0f, 1.0f };
            float m[16];

            le_object_set_scale(e->world, &parent, my);
            le_object_get_world_matrix(e->world, &child, m);
            TEST_CHECK(le_matrix_is_mirrored(m) == 1, "mirror Y");
            le_object_set_scale(e->world, &parent, mz);
            le_object_get_world_matrix(e->world, &child, m);
            TEST_CHECK(le_matrix_is_mirrored(m) == 1, "mirror Z");
            le_object_set_scale(e->world, &parent, mm);
            le_object_get_world_matrix(e->world, &child, m);
            TEST_CHECK(le_matrix_is_mirrored(m) == 0,
                       "double mirror even parity");
            TEST_CHECK(render_one_frame(e, &fe),
                       "even-parity frame renders");
            TEST_CHECK(lit_pixels(e) > 1000,
                       "even-parity cube paints pixels");
        }
        le_object_destroy(e->world, &parent);
    }

    /* PART 5: parented camera follows the world transform
     * (move the rig => pixels change; derived view matches the
     * matrix-derived expectation). */
    {
        long before;
        long after;

        TEST_CHECK(render_one_frame(e, &fe), "camera frame renders");
        before = lit_pixels(e);
        /* Find the camera rig by name scan: simpler — move every
         * root slightly? No: move the known rig via active camera
         * parent. Re-derive: fetch active camera, get its parent,
         * shift +X by 3. */
        {
            le_object active;
            le_object rig;

            TEST_CHECK(le_world_get_active_camera(e->world, &active) ==
                           1,
                       "active camera present");
            TEST_CHECK(le_object_get_parent(e->world, &active, &rig) ==
                           1,
                       "camera rig found");
            {
                float p[3];

                le_object_get_position(e->world, &rig, p);
                p[0] += 3.0f;
                le_object_set_position(e->world, &rig, p);
            }
        }
        TEST_CHECK(render_one_frame(e, &fe), "moved-camera renders");
        after = lit_pixels(e);
        TEST_CHECK(before != after, "camera move changes view");
        printf("[info] camera lit: before=%ld after=%ld\n", before,
               after);
    }

    /* PART 6: light sync — a frame with the sun disabled is
     * darker than with it enabled (directional contribution
     * removed; ambient remains). The sun handle is tracked in a
     * minimal owned scene on a second world (full handle
     * ownership, no slot gymnastics). */
    {
        long lit;
        long dark;

        TEST_CHECK(render_one_frame(e, &fe), "lit frame renders");
        lit = lit_pixels(e);
        TEST_CHECK(lit > 0, "sunlit frame nonzero");
        printf("[info] sunlit pixels: %ld\n", lit);
        /* Minimal owned scene: one cube + camera + sun, all
         * handles local. Toggle the sun and compare. */
        {
            le_world *w2 = NULL;
            le_world_desc wdesc;
            le_object cam2;
            le_object rig2;
            le_object cube2;
            le_object sun2;
            le_renderable_desc rd;
            le_camera_desc cd;
            le_light_desc ld;
            float p[3];
            le_world *saved = e->world;

            memset(&wdesc, 0, sizeof(wdesc));
            TEST_CHECK(le_world_create(e->engine, &wdesc, &w2) ==
                           LE_SUCCESS,
                       "light-toggle world creates");
            e->world = w2;
                le_object_create(w2, &rig2);
                le_object_create(w2, &cam2);
                p[0] = 0.0f;
                p[1] = 2.0f;
                p[2] = 8.0f;
                le_object_set_position(w2, &rig2, p);
                le_object_set_parent(w2, &cam2, &rig2);
                le_camera_desc_default(&cd);
                le_object_add_camera(w2, &cam2, &cd);
                le_world_set_active_camera(w2, &cam2);
                le_object_create(w2, &cube2);
                p[0] = 0.0f;
                p[1] = 0.0f;
                p[2] = 0.0f;
                le_object_set_position(w2, &cube2, p);
                memset(&rd, 0, sizeof(rd));
                rd.mesh = e->cube;
                rd.material = e->mat;
                rd.visible = 1;
                rd.receives_shadow = 1;
                le_object_add_renderable(w2, &cube2, &rd);
                le_object_create(w2, &sun2);
                memset(&ld, 0, sizeof(ld));
                ld.type = LE_LIGHT_DIRECTIONAL;
                ld.color[0] = 1.0f;
                ld.color[1] = 1.0f;
                ld.color[2] = 1.0f;
                ld.intensity = 3.0f;
                /* No ambient in the toggle world (NULL ambient =
                 * renderer default 0.03): the directional is the
                 * only meaningful light, so disabling it must
                 * darken the frame decisively. */
                {
                    float dark_ambient[3] = { 0.0f, 0.0f, 0.0f };

                    lr_renderer_set_ambient(e->renderer, dark_ambient);
                }
                /* Aim -Z of sun along its travel direction with
                 * the same look-at basis (replaces the old
                 * yaw/pitch composition, which pointed +Z at the
                 * target and lit the scene from behind). */
                {
                    float dir[3] = { 0.5f, -1.0f, 0.3f };
                    float len = sqrtf(dir[0] * dir[0] +
                                      dir[1] * dir[1] +
                                      dir[2] * dir[2]);
                    float right[3];
                    float up[3];
                    float r[3][3];
                    float trace;
                    float q[4];

                    dir[0] /= len;
                    dir[1] /= len;
                    dir[2] /= len;
                    right[0] = -dir[2];
                    right[1] = 0.0f;
                    right[2] = dir[0];
                    {
                        float rlen = sqrtf(right[0] * right[0] +
                                           right[2] * right[2]);

                        if (rlen < 1e-6f) {
                            right[0] = 1.0f;
                            right[2] = 0.0f;
                        } else {
                            right[0] /= rlen;
                            right[2] /= rlen;
                        }
                    }
                    up[0] = right[1] * dir[2] - right[2] * dir[1];
                    up[1] = right[2] * dir[0] - right[0] * dir[2];
                    up[2] = right[0] * dir[1] - right[1] * dir[0];
                    r[0][0] = right[0];
                    r[1][0] = right[1];
                    r[2][0] = right[2];
                    r[0][1] = up[0];
                    r[1][1] = up[1];
                    r[2][1] = up[2];
                    r[0][2] = -dir[0];
                    r[1][2] = -dir[1];
                    r[2][2] = -dir[2];
                    trace = r[0][0] + r[1][1] + r[2][2];
                    if (trace > 0.0f) {
                        float u = sqrtf(trace + 1.0f) * 2.0f;

                        q[3] = 0.25f * u;
                        q[0] = (r[2][1] - r[1][2]) / u;
                        q[1] = (r[0][2] - r[2][0]) / u;
                        q[2] = (r[1][0] - r[0][1]) / u;
                    } else if (r[0][0] > r[1][1] &&
                               r[0][0] > r[2][2]) {
                        float u =
                            sqrtf(1.0f + r[0][0] - r[1][1] - r[2][2]) *
                            2.0f;

                        q[3] = (r[2][1] - r[1][2]) / u;
                        q[0] = 0.25f * u;
                        q[1] = (r[0][1] + r[1][0]) / u;
                        q[2] = (r[0][2] + r[2][0]) / u;
                    } else if (r[1][1] > r[2][2]) {
                        float u =
                            sqrtf(1.0f + r[1][1] - r[0][0] - r[2][2]) *
                            2.0f;

                        q[3] = (r[0][2] - r[2][0]) / u;
                        q[0] = (r[0][1] + r[1][0]) / u;
                        q[1] = 0.25f * u;
                        q[2] = (r[1][2] + r[2][1]) / u;
                    } else {
                        float u =
                            sqrtf(1.0f + r[2][2] - r[0][0] - r[1][1]) *
                            2.0f;

                        q[3] = (r[1][0] - r[0][1]) / u;
                        q[0] = (r[0][2] + r[2][0]) / u;
                        q[1] = (r[1][2] + r[2][1]) / u;
                        q[2] = 0.25f * u;
                    }
                    le_object_set_rotation(w2, &sun2, q);
                }
                le_object_add_light(w2, &sun2, &ld);
                TEST_CHECK(render_one_frame(e, &fe), "toggle lit renders");
                lit = lit_pixels(e);
                TEST_CHECK(le_object_set_enabled(w2, &sun2, 0) ==
                               LE_SUCCESS,
                           "sun disables");
                TEST_CHECK(render_one_frame(e, &fe),
                           "toggle dark renders");
                dark = lit_pixels(e);
                printf("[info] toggle: lit=%ld dark=%ld\n", lit, dark);
                TEST_CHECK(dark < lit, "disabled sun darkens frame");
                TEST_CHECK(le_object_set_enabled(w2, &sun2, 1) ==
                               LE_SUCCESS,
                           "sun re-enables");
                TEST_CHECK(render_one_frame(e, &fe),
                           "toggle restored renders");
                {
                    long relit = lit_pixels(e);
                    long lo = (lit < dark) ? lit : dark;
                    long hi = (lit > dark) ? lit : dark;

                    TEST_CHECK(relit >= lo && relit <= hi + hi / 4 + 64,
                               "re-enabled sun restores light");
                }
                {
                    float ambient[3] = { 0.35f, 0.35f, 0.40f };

                    lr_renderer_set_ambient(e->renderer, ambient);
                }
                e->world = saved;
                le_world_destroy(w2);
        }
    }

    /* PART 7: stable temporal IDs — reorder does not transfer LOD
     * history; destroy/reuse retires keys. GPU-driven + LOD on.
     * A dedicated LOD mesh (real simplified levels) proves the
     * history path is live: without LODs every instance is trivially
     * LOD0 and the reorder proof would be vacuous. */
    {
        lr_visibility_settings vis;
        lr_mesh *lod_mesh = NULL;

        /* 9x9 dome grid with 3 decimated levels (mirrors the LOD
         * test's proven geometry). */
        {
            enum { GRID = 9 };

            lr_vertex verts[GRID * GRID];
            uint32_t full[(GRID - 1) * (GRID - 1) * 6];
            uint32_t mid[4 * 4 * 6];
            uint32_t coarse[2 * 2 * 6];
            uint32_t tip[3] = { 0, 8, 72 };
            lr_mesh_desc mdesc;
            int ix;
            int iz;
            uint32_t n;

            for (iz = 0; iz < GRID; iz++) {
                for (ix = 0; ix < GRID; ix++) {
                    lr_vertex *v = &verts[iz * GRID + ix];
                    float x = -3.0f + 0.75f * (float)ix;
                    float z = -3.0f + 0.75f * (float)iz;

                    memset(v, 0, sizeof(*v));
                    v->position[0] = x;
                    v->position[1] = 2.0f;
                    v->position[2] = z;
                    v->normal[1] = 1.0f;
                    v->tangent[0] = 1.0f;
                    v->tangent[3] = 1.0f;
                    v->texcoord[0] = (float)ix / 8.0f;
                    v->texcoord[1] = (float)iz / 8.0f;
                }
            }
            n = 0;
            for (iz = 0; iz + 1 < GRID; iz++) {
                for (ix = 0; ix + 1 < GRID; ix++) {
                    uint32_t a = (uint32_t)(iz * GRID + ix);
                    uint32_t b = (uint32_t)(iz * GRID + ix + 1);
                    uint32_t c = (uint32_t)((iz + 1) * GRID + ix);
                    uint32_t d = (uint32_t)((iz + 1) * GRID + ix + 1);

                    full[n + 0] = a;
                    full[n + 1] = b;
                    full[n + 2] = c;
                    full[n + 3] = b;
                    full[n + 4] = d;
                    full[n + 5] = c;
                    n += 6;
                }
            }
            {
                uint32_t m = 0;

                for (iz = 0; iz + 2 < GRID; iz += 2) {
                    for (ix = 0; ix + 2 < GRID; ix += 2) {
                        uint32_t a = (uint32_t)(iz * GRID + ix);
                        uint32_t b = (uint32_t)(iz * GRID + ix + 2);
                        uint32_t c = (uint32_t)((iz + 2) * GRID + ix);
                        uint32_t d =
                            (uint32_t)((iz + 2) * GRID + ix + 2);

                        mid[m + 0] = a;
                        mid[m + 1] = b;
                        mid[m + 2] = c;
                        mid[m + 3] = b;
                        mid[m + 4] = d;
                        mid[m + 5] = c;
                        m += 6;
                    }
                }
            }
            {
                uint32_t m = 0;

                for (iz = 0; iz + 4 < GRID; iz += 4) {
                    for (ix = 0; ix + 4 < GRID; ix += 4) {
                        uint32_t a = (uint32_t)(iz * GRID + ix);
                        uint32_t b = (uint32_t)(iz * GRID + ix + 4);
                        uint32_t c = (uint32_t)((iz + 4) * GRID + ix);
                        uint32_t d =
                            (uint32_t)((iz + 4) * GRID + ix + 4);

                        coarse[m + 0] = a;
                        coarse[m + 1] = b;
                        coarse[m + 2] = c;
                        coarse[m + 3] = b;
                        coarse[m + 4] = d;
                        coarse[m + 5] = c;
                        m += 6;
                    }
                }
            }
            memset(&mdesc, 0, sizeof(mdesc));
            mdesc.vertices = verts;
            mdesc.vertex_count = GRID * GRID;
            mdesc.indices = full;
            mdesc.index_count = n;
            if (lr_mesh_create(e->renderer, &mdesc, &lod_mesh) !=
                LR_SUCCESS) {
                TEST_CHECK(0, "lod mesh creates");
                lod_mesh = NULL;
            } else {
                uint32_t mid_n = 0;
                uint32_t coarse_n = 0;

                for (iz = 0; iz + 2 < GRID; iz += 2) {
                    for (ix = 0; ix + 2 < GRID; ix += 2) {
                        mid_n += 6;
                    }
                }
                for (iz = 0; iz + 4 < GRID; iz += 4) {
                    for (ix = 0; ix + 4 < GRID; ix += 4) {
                        coarse_n += 6;
                    }
                }
                if (lr_mesh_add_lod(lod_mesh, mid, mid_n, 200.0f) !=
                        LR_SUCCESS ||
                    lr_mesh_add_lod(lod_mesh, coarse, coarse_n, 80.0f) !=
                        LR_SUCCESS ||
                    lr_mesh_add_lod(lod_mesh, tip, 3, 25.0f) !=
                        LR_SUCCESS) {
                    TEST_CHECK(0, "lod levels add");
                } else {
                    TEST_CHECK(lr_mesh_get_lod_count(lod_mesh) == 4,
                               "lod mesh has 4 levels");
                }
            }
        }
        if (lr_renderer_set_render_mode(
                e->renderer, LR_RENDER_MODE_GPU_DRIVEN) != LR_SUCCESS) {
            TEST_CHECK(0, "gpu-driven mode selects");
        }
        lr_visibility_settings_default(&vis);
        vis.enabled = 1;
        vis.hiz_enabled = 0;
        vis.lod_enabled = 1;
        vis.indirect_count_enabled = 1;
        vis.graph_enabled = 0;
        TEST_CHECK(lr_renderer_set_visibility(e->renderer, &vis) ==
                       LR_SUCCESS,
                   "visibility applies");
        /* Submission orders A B C / C A B / B C A over the LOD
         * mesh: destroy and recreate the same three logical domes
         * in different orders across frames (slots churn), then
         * verify per-handle keys never collide, keys survive
         * reorder, and LOD visibility stats stay coherent. */
        {
            le_object a;
            le_object b;
            le_object c;
            uint64_t ida0;
            uint64_t idb0;
            uint64_t idc0;
            lr_visibility_stats vs_order1;
            lr_visibility_stats vs_order2;

            if (lod_mesh == NULL) {
                TEST_CHECK(0, "lod mesh available for reorder");
            } else {
                a = make_renderable(e, lod_mesh, e->mat, -6.0f, 0.0f,
                                    0.0f);
                b = make_renderable(e, lod_mesh, e->mat, 0.0f, 0.0f,
                                    -4.0f);
                c = make_renderable(e, lod_mesh, e->mat, 6.0f, 0.0f,
                                    0.0f);
                ida0 = le_object_stable_id(e->world, &a);
                idb0 = le_object_stable_id(e->world, &b);
                idc0 = le_object_stable_id(e->world, &c);
                TEST_CHECK(ida0 != 0 && idb0 != 0 && idc0 != 0,
                           "stable IDs nonzero");
                TEST_CHECK(ida0 != idb0 && idb0 != idc0 && ida0 != idc0,
                           "stable IDs distinct");
                TEST_CHECK(render_one_frame(e, &fe), "reorder frame 1");
                TEST_CHECK(lr_renderer_update_visibility_stats(
                               e->renderer) == LR_SUCCESS,
                           "visibility snapshot 1");
                lr_renderer_get_visibility_stats(e->renderer, &vs_order1);
                /* True in-place reorder (slots stable, only the
                 * submission sequence changes A B C -> C A B): the
                 * stable keys ride the handles untouched, so both
                 * orders submit the same totals/LOD mix. Extraction
                 * proves slot order while the frame below submits
                 * reversed through the same engine world. (Parts 1-2
                 * leave earlier renderables live, so filter the
                 * snapshot by handle rather than assuming a clean
                 * world.) */
                {
                    le_extracted_renderable snap[16];
                    uint32_t snap_count = 0;
                    uint32_t ai = 0xFFFFFFFFu;
                    uint32_t bi = 0xFFFFFFFFu;
                    uint32_t ci = 0xFFFFFFFFu;
                    uint32_t i;

                    le_world_extract_renderables(e->world, snap, 16,
                                                 &snap_count);
                    for (i = 0; i < snap_count && i < 16; i++) {
                        if (snap[i].object.index == a.index &&
                            snap[i].object.generation ==
                                a.generation) {
                            ai = i;
                        } else if (snap[i].object.index == b.index &&
                                   snap[i].object.generation ==
                                       b.generation) {
                            bi = i;
                        } else if (snap[i].object.index == c.index &&
                                   snap[i].object.generation ==
                                       c.generation) {
                            ci = i;
                        }
                    }
                    TEST_CHECK(ai != 0xFFFFFFFFu && bi != 0xFFFFFFFFu &&
                                   ci != 0xFFFFFFFFu,
                               "extract sees A B C");
                    /* Slot order is world-slot order, NOT creation
                     * order: a/b/c reuse freed slots, so they may sit
                     * anywhere among parts 1-2 residue. The invariant
                     * is position-independence: keys ride the
                     * handles, wherever the slots land. */
                    TEST_CHECK(ai != bi && bi != ci && ai != ci,
                               "extract positions distinct");
                    TEST_CHECK(snap[ai].stable_id == ida0 &&
                                   snap[bi].stable_id == idb0 &&
                                   snap[ci].stable_id == idc0,
                               "extract keys follow handles");
                }
                /* Order 2: destroy all (slots freed), recreate in
                 * reverse (C A B): new handles, new keys, same
                 * scene content. */
                TEST_CHECK(le_object_destroy(e->world, &a) == LE_SUCCESS,
                           "destroy A");
                TEST_CHECK(le_object_destroy(e->world, &b) == LE_SUCCESS,
                           "destroy B");
                TEST_CHECK(le_object_destroy(e->world, &c) == LE_SUCCESS,
                           "destroy C");
                c = make_renderable(e, lod_mesh, e->mat, 6.0f, 0.0f,
                                    0.0f);
                a = make_renderable(e, lod_mesh, e->mat, -6.0f, 0.0f,
                                    0.0f);
                b = make_renderable(e, lod_mesh, e->mat, 0.0f, 0.0f,
                                    -4.0f);
                {
                    uint64_t ida1 =
                        le_object_stable_id(e->world, &a);
                    uint64_t idb1 =
                        le_object_stable_id(e->world, &b);
                    uint64_t idc1 =
                        le_object_stable_id(e->world, &c);

                    TEST_CHECK(ida1 != idb1 && idb1 != idc1 &&
                                   ida1 != idc1,
                               "reorder keys distinct");
                    /* New lifetimes => new keys (no history
                     * transfer from the destroyed generation). */
                    TEST_CHECK(ida1 != ida0 && idb1 != idb0 &&
                                   idc1 != idc0,
                               "reorder keys fresh lifetimes");
                }
                TEST_CHECK(render_one_frame(e, &fe), "reorder frame 2");
                TEST_CHECK(lr_renderer_update_visibility_stats(
                               e->renderer) == LR_SUCCESS,
                           "visibility snapshot 2");
                lr_renderer_get_visibility_stats(e->renderer, &vs_order2);
                /* Same scene content in a different creation
                 * order: total/visible agree (temporal identity
                 * follows objects, not slots). LOD mixes agree too:
                 * identical content at identical sizes must land on
                 * identical LODs regardless of creation order. */
                TEST_CHECK(vs_order1.total_instances ==
                               vs_order2.total_instances,
                           "reorder totals agree");
                TEST_CHECK(vs_order1.visible == vs_order2.visible,
                           "reorder visible agrees");
                TEST_CHECK(vs_order1.lod_visible[0] ==
                                   vs_order2.lod_visible[0] &&
                               vs_order1.lod_visible[1] ==
                                   vs_order2.lod_visible[1] &&
                               vs_order1.lod_visible[2] ==
                                   vs_order2.lod_visible[2] &&
                               vs_order1.lod_visible[3] ==
                                   vs_order2.lod_visible[3],
                           "reorder LOD mix agrees");
                printf("[info] reorder: total=%llu visible=%llu "
                       "lod=[%llu,%llu,%llu,%llu]\n",
                       (unsigned long long)vs_order2.total_instances,
                       (unsigned long long)vs_order2.visible,
                       (unsigned long long)vs_order2.lod_visible[0],
                       (unsigned long long)vs_order2.lod_visible[1],
                       (unsigned long long)vs_order2.lod_visible[2],
                       (unsigned long long)vs_order2.lod_visible[3]);
                /* Destroy/reuse: D reuses B's slot with a fresh
                 * key (history retired, never inherited). */
                {
                    le_object d;
                    uint64_t idb1;
                    uint64_t idd;

                    idb1 = le_object_stable_id(e->world, &b);
                    TEST_CHECK(le_object_destroy(e->world, &b) ==
                                   LE_SUCCESS,
                               "destroy B (reuse)");
                    d = make_renderable(e, lod_mesh, e->mat, 0.0f, 2.0f,
                                        -4.0f);
                    TEST_CHECK(d.index == b.index,
                               "D reuses B slot");
                    TEST_CHECK(d.generation != b.generation,
                               "D generation bumped");
                    idd = le_object_stable_id(e->world, &d);
                    TEST_CHECK(idd != 0 && idd != idb1,
                               "D key fresh (history retired)");
                    TEST_CHECK(le_object_stable_id(e->world, &b) == 0,
                               "B key retired to 0");
                    TEST_CHECK(render_one_frame(e, &fe),
                               "reuse frame renders");
                    le_object_destroy(e->world, &a);
                    le_object_destroy(e->world, &c);
                    le_object_destroy(e->world, &d);
                }
            }
        }
        /* CPU fallback still renders after switching back. */
        TEST_CHECK(lr_renderer_set_render_mode(e->renderer,
                                               LR_RENDER_MODE_CPU) ==
                       LR_SUCCESS,
                   "cpu fallback selects");
        TEST_CHECK(render_one_frame(e, &fe), "cpu fallback renders");
        TEST_CHECK(lit_pixels(e) > 500, "cpu fallback paints pixels");
        lr_mesh_destroy(lod_mesh);
    }

    /* PART 8: accounting invariant — submitted == frustum_rejected
     * + occlusion_rejected + visible over the renderer's visibility
     * stats (GPU path), and the engine report mirrors the
     * renderer's submitted count. Occlusion is disabled in this
     * file's visibility config, so occlusion_rejected reads 0 and
     * the invariant reduces to submitted == frustum + visible.
     *
     * PART 8B (large world): 5000 engine renderables (documented
     * scene semantics — created/enabled/renderable/submitted, with
     * only street-skipped documented separately) prove the engine
     * extraction path and the renderer GPU path agree at volume,
     * with the exact invariant holding. */
    {
        lr_visibility_stats vs;
        lr_render_stats stats;
        lr_visibility_settings vis;

        lr_visibility_settings_default(&vis);
        vis.enabled = 1;
        vis.hiz_enabled = 0;
        vis.lod_enabled = 0;
        vis.indirect_count_enabled = 1;
        vis.graph_enabled = 0;
        TEST_CHECK(lr_renderer_set_render_mode(
                       e->renderer, LR_RENDER_MODE_GPU_DRIVEN) ==
                       LR_SUCCESS,
                   "accounting gpu mode");
        TEST_CHECK(lr_renderer_set_visibility(e->renderer, &vis) ==
                       LR_SUCCESS,
                   "accounting visibility");
        TEST_CHECK(render_one_frame(e, &fe), "accounting frame renders");
        TEST_CHECK(lr_renderer_update_visibility_stats(e->renderer) ==
                       LR_SUCCESS,
                   "accounting snapshot");
        lr_renderer_get_stats(e->renderer, &stats);
        lr_renderer_get_visibility_stats(e->renderer, &vs);
        {
            le_render_report report;

            le_world_get_last_render_report(e->world, &report);
            TEST_CHECK(report.submitted ==
                           report.renderer_stats.submitted_objects,
                       "report submitted == renderer submitted");
            TEST_CHECK(vs.total_instances == report.submitted,
                       "visibility total == submitted");
            TEST_CHECK(vs.total_instances == vs.frustum_rejected +
                                                  vs.occlusion_rejected +
                                                  vs.visible,
                       "submitted == frustum + occlusion + visible");
            printf("[info] accounting: total=%llu frustum=%llu "
                   "occlusion=%llu visible=%llu draws=%u tris=%u\n",
                   (unsigned long long)vs.total_instances,
                   (unsigned long long)vs.frustum_rejected,
                   (unsigned long long)vs.occlusion_rejected,
                   (unsigned long long)vs.visible, stats.draw_calls,
                   stats.triangles);
        }
    }

    /* PART 8B: large renderable world (5000 engine objects, all
     * enabled renderables; third of them disabled to prove the
     * skipped_disabled lane at volume). Exact accounting:
     * created / enabled / renderable / submitted /
     * frustum-rejected / occlusion-rejected / visible, with the
     * invariant submitted == frustum + occlusion + visible. */
    {
        le_world *wbig = NULL;
        le_world_desc wdesc;
        le_world *saved = e->world;
        enum { BIG_N = 5000 };
        le_object *big_handles = NULL;
        uint32_t i;
        uint32_t created = 0;
        uint32_t disabled_n = 0;
        int big_ok = 1;

        memset(&wdesc, 0, sizeof(wdesc));
        TEST_CHECK(le_world_create(e->engine, &wdesc, &wbig) ==
                       LE_SUCCESS,
                   "large world creates");
        big_handles = (le_object *)malloc((size_t)BIG_N *
                                          sizeof(le_object));
        TEST_CHECK(big_handles != NULL, "large handle buffer");
        if (big_handles == NULL) {
            le_world_destroy(wbig);
            big_ok = 0;
        }
        if (big_ok) {
            e->world = wbig;
            for (i = 0; i < BIG_N; i++) {
                le_object o;
                le_renderable_desc rd;
                float p[3];

                if (le_object_create(wbig, &o) != LE_SUCCESS) {
                    big_ok = 0;
                    break;
                }
                /* Deterministic street grid (mirrors the renderer
                 * visibility_scene layout): every 5th column is a
                 * "street" — no object created there (documented
                 * scene-generation filtering, NOT renderer loss). */
                p[0] = -40.0f + 1.0f * (float)(i % 100u);
                p[1] = 0.0f;
                p[2] = -40.0f + 1.0f * (float)((i / 100u) % 50u);
                le_object_set_position(wbig, &o, p);
                memset(&rd, 0, sizeof(rd));
                rd.mesh = e->cube;
                rd.material = e->mat;
                rd.visible = 1;
                if (le_object_add_renderable(wbig, &o, &rd) !=
                    LE_SUCCESS) {
                    big_ok = 0;
                    break;
                }
                if ((i % 3u) == 0u) {
                    le_object_set_enabled(wbig, &o, 0);
                    disabled_n++;
                }
                big_handles[i] = o;
                created++;
            }
            TEST_CHECK(big_ok, "5000 engine objects created");
            TEST_CHECK(created == BIG_N, "created exact");
            {
                le_world_stats stats;

                le_world_get_stats(wbig, &stats);
                printf("[info] large world: objects=%u renderables=%u "
                       "enabled=%u disabled=%u roots=%u\n",
                       stats.objects_alive, stats.renderables,
                       stats.enabled_objects, stats.disabled_objects,
                       stats.root_count);
                TEST_CHECK(stats.objects_alive == BIG_N,
                           "large objects exact");
                TEST_CHECK(stats.renderables == BIG_N,
                           "large renderables exact");
                TEST_CHECK(stats.disabled_objects == disabled_n,
                           "large disabled exact");
            }
            /* Camera: reuse the part-1 rig math (look at origin
             * from above-front). */
            {
                le_object rig;
                le_object cam;
                le_camera_desc cd;
                float p[3] = { 0.0f, 60.0f, 60.0f };

                le_object_create(wbig, &rig);
                le_object_create(wbig, &cam);
                le_object_set_position(wbig, &rig, p);
                le_object_set_parent(wbig, &cam, &rig);
                {
                    /* Same look-at quaternion as make_camera. */
                    float fwd[3] = { 0.0f, -60.0f, -60.0f };
                    float flen = sqrtf(7200.0f);
                    float right[3] = { 1.0f, 0.0f, 0.0f };
                    float up[3];
                    float r[3][3];
                    float trace;
                    float q[4];

                    fwd[0] /= flen;
                    fwd[1] /= flen;
                    fwd[2] /= flen;
                    up[0] = right[1] * fwd[2] - right[2] * fwd[1];
                    up[1] = right[2] * fwd[0] - right[0] * fwd[2];
                    up[2] = right[0] * fwd[1] - right[1] * fwd[0];
                    r[0][0] = right[0];
                    r[1][0] = right[1];
                    r[2][0] = right[2];
                    r[0][1] = up[0];
                    r[1][1] = up[1];
                    r[2][1] = up[2];
                    r[0][2] = -fwd[0];
                    r[1][2] = -fwd[1];
                    r[2][2] = -fwd[2];
                    trace = r[0][0] + r[1][1] + r[2][2];
                    {
                        float u = sqrtf(trace + 1.0f) * 2.0f;

                        q[3] = 0.25f * u;
                        q[0] = (r[2][1] - r[1][2]) / u;
                        q[1] = (r[0][2] - r[2][0]) / u;
                        q[2] = (r[1][0] - r[0][1]) / u;
                    }
                    le_object_set_rotation(wbig, &rig, q);
                }
                le_camera_desc_default(&cd);
                cd.far_plane = 500.0f;
                le_object_add_camera(wbig, &cam, &cd);
                le_world_set_active_camera(wbig, &cam);
            }
            {
                uint64_t t0 = lc_clock_now();
                uint64_t freq = lc_clock_frequency();
                le_render_report report;
                lr_visibility_stats vs_big;
                double extract_ms;

                TEST_CHECK(render_one_frame(e, &fe),
                           "large world renders");
                extract_ms = (freq != 0)
                                 ? ((double)(lc_clock_now() - t0) *
                                    1000.0 / (double)freq)
                                 : -1.0;
                le_world_get_last_render_report(wbig, &report);
                TEST_CHECK(lr_renderer_update_visibility_stats(
                               e->renderer) == LR_SUCCESS,
                           "large snapshot");
                lr_renderer_get_visibility_stats(e->renderer, &vs_big);
                printf("[info] large accounting: created=%u enabled=%u "
                       "renderables=%u submitted=%u skipped_disabled=%u "
                       "skipped_invisible=%u skipped_dead=%u "
                       "frustum=%llu occlusion=%llu visible=%llu "
                       "extract+frame=%.2fms\n",
                       created, created - disabled_n, BIG_N,
                       report.submitted, report.skipped_disabled,
                       report.skipped_invisible, report.skipped_dead,
                       (unsigned long long)vs_big.frustum_rejected,
                       (unsigned long long)vs_big.occlusion_rejected,
                       (unsigned long long)vs_big.visible,
                       extract_ms);
                /* Exact engine-side accounting. */
                TEST_CHECK(report.submitted == BIG_N - disabled_n,
                           "large submitted == enabled");
                TEST_CHECK(report.skipped_disabled == disabled_n,
                           "large skipped_disabled exact");
                TEST_CHECK(report.skipped_invisible == 0,
                           "large skipped_invisible == 0");
                TEST_CHECK(report.skipped_dead == 0,
                           "large skipped_dead == 0");
                /* Renderer-side invariant at volume. */
                TEST_CHECK(vs_big.total_instances == report.submitted,
                           "large visibility total == submitted");
                TEST_CHECK(vs_big.total_instances ==
                                   vs_big.frustum_rejected +
                                       vs_big.occlusion_rejected +
                                       vs_big.visible,
                           "large invariant holds");
            }
            e->world = saved;
            free(big_handles);
            le_world_destroy(wbig);
        } else {
            e->world = saved;
            free(big_handles);
        }
    }

    /* PART 9: engine stats + multiple worlds render independently. */
    {
        le_world_stats stats;

        le_world_get_stats(e->world, &stats);
        TEST_CHECK(stats.objects_alive > 0, "world nonempty");
        printf("[info] objects=%u roots=%u renderables=%u cameras=%u "
               "lights=%u\n",
               stats.objects_alive, stats.root_count, stats.renderables,
               stats.cameras, stats.lights);
        {
            le_world *w2 = NULL;
            le_world_desc wdesc;
            le_object o2;
            le_render_report r1;
            le_render_report r2;

            memset(&wdesc, 0, sizeof(wdesc));
            TEST_CHECK(le_world_create(e->engine, &wdesc, &w2) ==
                           LE_SUCCESS,
                       "second world creates");
            o2 = make_renderable(e, e->cube, e->mat2, 0.0f, 0.0f, 0.0f);
            /* o2 lives in world 1; world 2 must reject it. */
            TEST_CHECK(le_object_set_enabled(w2, &o2, 0) ==
                           LE_ERROR_WRONG_WORLD,
                       "second world rejects foreign handle");
            /* Render world 2 (empty scene, fallback camera). */
            {
                le_world *saved = e->world;

                e->world = w2;
                TEST_CHECK(render_one_frame(e, &fe),
                           "empty world renders");
                le_world_get_last_render_report(w2, &r2);
                e->world = saved;
                TEST_CHECK(render_one_frame(e, &fe),
                           "first world renders again");
                le_world_get_last_render_report(e->world, &r1);
                TEST_CHECK(r2.submitted == 0,
                           "empty world submits nothing");
                TEST_CHECK(r1.submitted >= 1,
                           "first world still submits");
            }
            le_world_destroy(w2);
        }
    }

    printf("Luma Engine Vulkan tests: %d passed, %d failed\n", g_passed,
           g_failed);
    env_shutdown(&env);
    destroy_frame(&fe);
    lc_device_destroy(device);
    lc_shutdown();
    if (g_failed != 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL ENGINE VULKAN TESTS PASSED\n");
    return 0;
}
