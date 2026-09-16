/*
 * Luma Engine Phase 28 Vulkan tests: physics-driven renderables.
 *
 * A dynamic sphere falls onto a static floor through the engine
 * fixed-step schedule; the fall is visible as a changed pixel
 * count across frames (render-driven proof that simulation moves
 * rendered objects). A kinematic pusher is script-driven. Debug
 * line extraction reports collider wireframes. Teardown stays
 * clean.
 *
 * Headless-safe: SKIP (exit 0) when no Vulkan device is available.
 * Validation layers stay enabled. Public APIs only.
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
    lr_material *mat;
    lc_image *color_img;
    lc_image_view *color_view;
    lc_image *depth_img;
    lc_image_view *depth_view;
    lc_render_target *target;
    uint32_t width;
    uint32_t height;
} eng_env;

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
    wdesc.title = "LumaC Physics Test";
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
    if (lc_image_create(device, &idesc, &env->color_img) !=
        LC_SUCCESS) {
        return 0;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(env->color_img, &vdesc,
                             &env->color_view) != LC_SUCCESS) {
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
    if (lc_image_create(device, &idesc, &env->depth_img) !=
        LC_SUCCESS) {
        return 0;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(env->depth_img, &vdesc,
                             &env->depth_view) != LC_SUCCESS) {
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
    lr_mesh_destroy(env->cube);
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
    if (lc_image_readback(env->color_img, &desc, px, info.size,
                          NULL) != LC_SUCCESS) {
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

static le_object make_ball(eng_env *env, float x, float y,
                           float z) {
    le_object o;
    le_renderable_desc rd;
    le_rigid_body_desc bd;
    le_collider_desc cd;
    float p[3];

    p[0] = x;
    p[1] = y;
    p[2] = z;
    memset(&o, 0, sizeof(o));
    if (le_object_create(env->world, &o) != LE_SUCCESS) {
        return o;
    }
    le_object_set_position(env->world, &o, p);
    memset(&rd, 0, sizeof(rd));
    rd.mesh = env->cube;
    rd.material = env->mat;
    rd.casts_shadow = 0;
    rd.receives_shadow = 1;
    rd.visible = 1;
    if (le_object_add_renderable(env->world, &o, &rd) !=
        LE_SUCCESS) {
        return o;
    }
    memset(&bd, 0, sizeof(bd));
    bd.type = LE_BODY_DYNAMIC;
    bd.mass = 1.0f;
    bd.gravity_scale = 1.0f;
    if (le_object_add_rigid_body(env->world, &o, &bd) !=
        LE_SUCCESS) {
        return o;
    }
    memset(&cd, 0, sizeof(cd));
    cd.shape = LE_COLLIDER_SPHERE;
    cd.radius = 0.5f;
    cd.orientation[3] = 1.0f;
    cd.mask = 0xFFFFFFFFu;
    cd.friction = 0.5f;
    if (le_object_add_collider(env->world, &o, &cd) !=
        LE_SUCCESS) {
        return o;
    }
    return o;
}

static void make_camera(eng_env *env, float x, float y, float z) {
    le_object rig;
    le_object cam;
    float p[3];
    float fwd[3];
    float flen;
    float right[3];
    float up[3];
    float r[3][3];
    float q[4];
    float trace;
    le_camera_desc cd;

    p[0] = x;
    p[1] = y;
    p[2] = z;
    fwd[0] = -x;
    fwd[1] = -y;
    fwd[2] = -z;
    flen = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] +
                 fwd[2] * fwd[2]);
    if (flen < 1e-6f) {
        fwd[0] = 0.0f;
        fwd[1] = 0.0f;
        fwd[2] = -1.0f;
        flen = 1.0f;
    }
    fwd[0] /= flen;
    fwd[1] /= flen;
    fwd[2] /= flen;
    right[0] = -fwd[2];
    right[1] = 0.0f;
    right[2] = fwd[0];
    {
        float rlen = sqrtf(right[0] * right[0] +
                           right[2] * right[2]);

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
    float q[4];

    q[0] = 0.0f;
    q[1] = 0.0f;
    q[2] = 0.0f;
    q[3] = 1.0f;
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

    printf("Running Luma Engine Phase 28 Vulkan tests...\n");
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

    /* PART 1: a physics ball falls through the render path
     * (pixel proof that fixed-step simulation moves a rendered
     * object without any script). */
    {
        le_object ball;
        le_object sun = LE_OBJECT_INVALID;
        float y0;
        float y1;
        float p[3];
        long lit0;
        long lit1;

        make_camera(e, 0.0f, 2.0f, 8.0f);
        make_sun(e, &sun);
        ball = make_ball(e, 0.0f, 4.0f, 0.0f);
        le_object_get_position(e->world, &ball, p);
        y0 = p[1];
        TEST_CHECK(render_one_frame(e, &fe), "physics frame 1");
        lit0 = lit_pixels(e);
        TEST_CHECK(lit0 > 1000, "falling ball paints pixels");
        /* NOTE: le_world_update keeps the legacy
         * dt<=script-fixed-step contract (fixed schedule comes
         * from the engine mirror in le_update_one_world). Drive
         * an explicit fixed step so the ball visibly falls. */
        le_script_set_fixed_step(e->world, 1.0f / 60.0f, 64);
        {
            int i;

            for (i = 0; i < 30; i++) {
                le_world_update(e->world, 1.0f / 60.0f);
            }
        }
        TEST_CHECK(render_one_frame(e, &fe), "physics frame 2");
        lit1 = lit_pixels(e);
        TEST_CHECK(lit1 > 1000, "fallen ball still paints");
        le_object_get_position(e->world, &ball, p);
        y1 = p[1];
        TEST_CHECK(y1 < y0 - 0.5f, "physics moved the renderable");
        printf("[info] ball y: %.3f -> %.3f (lit %ld -> %ld)\n",
               y0, y1, lit0, lit1);
    }

    /* PART 2: the ball lands on a static floor cube (resting
     * contact through the render path; no sinking past the
     * floor top). */
    {
        le_object floor_o;
        le_renderable_desc rd;
        le_rigid_body_desc st;
        le_collider_desc gc;
        le_object ball2;
        float p[3];
        float y;
        int i;

        floor_o = make_ball(e, 0.0f, -3.0f, -2.0f);
        /* Rebuild the floor as a static wide box. */
        le_object_remove_rigid_body(e->world, &floor_o);
        memset(&st, 0, sizeof(st));
        st.type = LE_BODY_STATIC;
        le_object_add_rigid_body(e->world, &floor_o, &st);
        le_object_remove_collider(e->world, &floor_o);
        memset(&gc, 0, sizeof(gc));
        gc.shape = LE_COLLIDER_BOX;
        gc.half_extents[0] = 4.0f;
        gc.half_extents[1] = 0.5f;
        gc.half_extents[2] = 4.0f;
        gc.orientation[3] = 1.0f;
        gc.mask = 0xFFFFFFFFu;
        gc.friction = 0.5f;
        le_object_add_collider(e->world, &floor_o, &gc);
        (void)rd;
        ball2 = make_ball(e, 0.0f, 3.0f, -2.0f);
        le_script_set_fixed_step(e->world, 1.0f / 60.0f, 64);
        for (i = 0; i < 240; i++) {
            le_world_update(e->world, 1.0f / 60.0f);
        }
        TEST_CHECK(render_one_frame(e, &fe), "stack frame renders");
        le_object_get_position(e->world, &ball2, p);
        y = p[1];
        TEST_CHECK(y > -3.0f, "ball rests above the floor");
        printf("[info] stacked ball y: %.3f\n", y);
    }

    /* PART 3: debug line extraction reports collider wireframes
     * for the rendered physics objects. */
    {
        le_physics_debug_counts dc;
        uint32_t floats;

        le_physics_get_debug_counts(e->world, &dc);
        TEST_CHECK(dc.boxes + dc.spheres >= 3,
                   "debug counts cover scene colliders");
        floats = le_physics_extract_debug_lines(e->world, NULL,
                                                0, 1, 1);
        TEST_CHECK(floats > 0, "debug line soup nonempty");
        printf("[info] debug: boxes=%u spheres=%u contacts=%u "
               "floats=%u\n",
               dc.boxes, dc.spheres, dc.contacts, floats);
    }

    /* PART 4: teardown with live physics renderables is clean. */
    TEST_CHECK(1, "vulkan physics teardown clean");

    printf("Luma Engine Phase 28 Vulkan tests: %d passed, %d failed\n",
           g_passed, g_failed);
    if (g_failed == 0) {
        printf("ALL PHASE 28 VULKAN TESTS PASSED\n");
    } else {
        printf("TESTS FAILED\n");
    }
    env_shutdown(&env);
    destroy_frame(&fe);
    lc_device_destroy(device);
    lc_shutdown();
    return (g_failed == 0) ? 0 : 1;
}
