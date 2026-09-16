/* Phase 21 GPU-driven rendering integration test.
 *
 * Proves the full chain on real pixels: compute frustum culling ->
 * compacted visible list -> GPU-written indirect commands ->
 * indexed indirect draws through the PBR-compatible instanced
 * pipeline. CPU frustum math oracles every GPU visible set
 * (independent implementation); CPU and GPU submission paths must
 * produce identical pixels. Covers zero/all-visible, frustum edge
 * cases, non-uniform and negative scale bounds, multi-draw
 * indirect, capacity handling, and descriptor/frame stability.
 * Validation layers stay enabled throughout.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>

#include "graphics/graphics_internal.h"
#include "internal/renderer_internal.h"

static int g_passed;
static int g_failed;

#define CHECK(c, m) do {                                                \
    if (c) { printf("[PASS] %s\n", m); g_passed++; }                    \
    else { printf("[FAIL] %s\n", m); g_failed++; }                     \
} while (0)

#define SKIP_ENV(what) do {                                             \
    printf("SKIP: environment cannot provide %s\n", what);              \
    lc_shutdown();                                                      \
    return 0;                                                           \
} while (0)

typedef struct gpu_env {
    lc_device *device;
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
    lr_renderer *renderer;
    lr_mesh *cube;
    lr_material *mat;
    lc_image *color_img;
    lc_image *depth_img;
    lc_image_view *color_view;
    lc_image_view *depth_view;
    lc_render_target *target;
    unsigned char oracle_want[512];
} gpu_env;

/* ---- independent CPU oracle (not the renderer's math) ---- */

static void oracle_world_sphere(const float m[16], const float c[3],
                                float r, float out_c[3], float *out_r) {
    float sx = sqrtf(m[0] * m[0] + m[1] * m[1] + m[2] * m[2]);
    float sy = sqrtf(m[4] * m[4] + m[5] * m[5] + m[6] * m[6]);
    float sz = sqrtf(m[8] * m[8] + m[9] * m[9] + m[10] * m[10]);
    float ms = sx;
    out_c[0] = m[0] * c[0] + m[4] * c[1] + m[8] * c[2] + m[12];
    out_c[1] = m[1] * c[0] + m[5] * c[1] + m[9] * c[2] + m[13];
    out_c[2] = m[2] * c[0] + m[6] * c[1] + m[10] * c[2] + m[14];
    if (sy > ms) {
        ms = sy;
    }
    if (sz > ms) {
        ms = sz;
    }
    *out_r = r * ms;
}

static void oracle_planes(const float vp[16], float planes[6][4]) {
    int i;

    /* Gribb/Hartmann rows, inward-facing, normalized. Element
     * (row r, col c) of column-major vp[] is vp[c*4+r]. */
    for (i = 0; i < 4; i++) {
        planes[0][i] = vp[3 + i * 4] + vp[i * 4];
        planes[1][i] = vp[3 + i * 4] - vp[i * 4];
        planes[2][i] = vp[3 + i * 4] + vp[1 + i * 4];
        planes[3][i] = vp[3 + i * 4] - vp[1 + i * 4];
        planes[4][i] = vp[3 + i * 4] + vp[2 + i * 4];
        planes[5][i] = vp[3 + i * 4] - vp[2 + i * 4];
    }
    for (i = 0; i < 6; i++) {
        float len = sqrtf(planes[i][0] * planes[i][0] +
                          planes[i][1] * planes[i][1] +
                          planes[i][2] * planes[i][2]);

        if (len > 0.0f) {
            int k;

            for (k = 0; k < 4; k++) {
                planes[i][k] /= len;
            }
        }
    }
}

static int oracle_visible(float planes[6][4], const float c[3], float r) {
    int i;

    for (i = 0; i < 6; i++) {
        if (planes[i][0] * c[0] + planes[i][1] * c[1] +
                planes[i][2] * c[2] + planes[i][3] <
            -r) {
            return 0;
        }
    }
    return 1;
}

/* ---- setup helpers ---- */

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
    case LC_ERROR_SURFACE_UNSUPPORTED:
        return 1;
    default:
        return -1;
    }
}

static int make_window_stack(gpu_env *env) {
    lc_window_desc wdesc;
    lc_swapchain_desc sdesc;

    wdesc.title = "LumaC Phase21 GPU-Driven";
    wdesc.width = 640;
    wdesc.height = 480;
    env->window = NULL;
    if (lc_window_create(&wdesc, &env->window) != LC_SUCCESS) {
        return 1;
    }
    env->surface = NULL;
    if (lc_surface_create(env->device, env->window, &env->surface) !=
        LC_SUCCESS) {
        return 1;
    }
    memset(&sdesc, 0, sizeof(sdesc));
    sdesc.width = 640;
    sdesc.height = 480;
    sdesc.image_count = 0;
    sdesc.vsync = 1;
    sdesc.max_frames_in_flight = 0;
    env->swapchain = NULL;
    if (lc_swapchain_create(env->device, env->surface, &sdesc,
                            &env->swapchain) != LC_SUCCESS) {
        return 1;
    }
    return 0;
}

/* Recreate the swapchain with an explicit flight count (PART AF31:
 * per-flight GPU resources must survive 2- and 3-deep overlap). */
static int remake_swapchain(gpu_env *env, uint32_t flights) {
    lc_swapchain_desc sdesc;

    lc_swapchain_destroy(env->swapchain);
    env->swapchain = NULL;
    memset(&sdesc, 0, sizeof(sdesc));
    sdesc.width = 640;
    sdesc.height = 480;
    sdesc.image_count = 0;
    sdesc.vsync = 1;
    sdesc.max_frames_in_flight = flights;
    if (lc_swapchain_create(env->device, env->surface, &sdesc,
                            &env->swapchain) != LC_SUCCESS) {
        return -1;
    }
    return 0;
}

static int make_target(gpu_env *env, uint32_t w, uint32_t h) {
    lc_image_desc idesc;
    lc_image_view_desc vdesc;
    lc_render_target_create_desc tdesc;
    lc_render_target_attachment att;

    memset(&idesc, 0, sizeof(idesc));
    idesc.type = LC_IMAGE_TYPE_2D;
    idesc.format = LC_FORMAT_RGBA8_UNORM;
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
        return -1;
    }
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
    return 0;
}

static int make_renderer_stack(gpu_env *env) {
    lr_renderer_desc rdesc;
    lr_pbr_material_desc matdesc;
    static const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };

    memset(&rdesc, 0, sizeof(rdesc));
    rdesc.device = env->device;
    rdesc.render_target.color_attachment_count = 1;
    rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
    rdesc.max_objects = 4096;
    rdesc.ambient_light[0] = 0.6f;
    rdesc.ambient_light[1] = 0.6f;
    rdesc.ambient_light[2] = 0.6f;
    if (lr_renderer_create(&rdesc, &env->renderer) != LR_SUCCESS) {
        printf("[info] renderer create failed\n");
        return -1;
    }
    if (lr_mesh_create_cube(env->renderer, 1.0f, &env->cube) !=
        LR_SUCCESS) {
        printf("[info] cube create failed\n");
        return -1;
    }
    memset(&matdesc, 0, sizeof(matdesc));
    memcpy(matdesc.base_color_factor, white, sizeof(white));
    matdesc.metallic_factor = 0.0f;
    matdesc.roughness_factor = 0.6f;
    matdesc.normal_scale = 1.0f;
    matdesc.occlusion_strength = 1.0f;
    matdesc.alpha_mode = LR_ALPHA_OPAQUE;
    if (lr_material_create_pbr(env->renderer, &matdesc, &env->mat) !=
        LR_SUCCESS) {
        printf("[info] pbr material create failed\n");
        return -1;
    }
    return 0;
}

static void make_camera(lr_camera *camera, const float eye[3]) {
    static const float center[3] = { 0.0f, 0.0f, 0.0f };
    static const float up[3] = { 0.0f, 1.0f, 0.0f };

    lr_camera_init(camera);
    lr_camera_set_perspective(camera, 1.0471976f, 1.0f, 0.1f, 100.0f);
    lr_camera_look_at(camera, eye, center, up);
}

static void submit_cube(gpu_env *env, float x, float y, float z,
                        float sx, float sy, float sz) {
    lr_draw_item item;

    memset(&item, 0, sizeof(item));
    lr_transform_identity(&item.transform);
    item.transform.position[0] = x;
    item.transform.position[1] = y;
    item.transform.position[2] = z;
    item.transform.scale[0] = sx;
    item.transform.scale[1] = sy;
    item.transform.scale[2] = sz;
    item.mesh = env->cube;
    item.material = env->mat;
    item.casts_shadow = 0;
    item.receives_shadow = 1;
    lr_renderer_submit(env->renderer, &item);
}

static void submit_light(gpu_env *env) {
    lr_light light;
    static const float white[3] = { 1.0f, 1.0f, 1.0f };
    static const float dir[3] = { 0.3f, -1.0f, 0.4f };

    memset(&light, 0, sizeof(light));
    light.type = LR_LIGHT_DIRECTIONAL;
    memcpy(light.color, white, sizeof(white));
    light.intensity = 2.0f;
    memcpy(light.direction, dir, sizeof(dir));
    lr_renderer_submit_light(env->renderer, &light);
}

/* Legacy offscreen frame: begin, submit callback, prepare (GPU
 * mode), pass, render, present-clear, readback. Returns pixels or
 * NULL. Endframe result stored when requested. */
typedef void (*submit_fn)(gpu_env *env, void *user);

static unsigned char *render_offscreen(gpu_env *env, submit_fn submit,
                                       void *user, int use_gpu,
                                       uint32_t w, uint32_t h,
                                       const float eye[3],
                                       const float center[3]) {
    lc_command_encoder *enc = NULL;
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_render_depth_attachment datt;
    lc_image_readback_desc rbdesc;
    lc_image_readback_info rbinfo;
    unsigned char *px = NULL;
    size_t need = 0;
    int frame_open = 0;
    int renderer_open = 0;
    int pass_open = 0;
    static const char *step = "init";

#define FAIL_AT(s) do { step = s; goto fail; } while (0)

    lc_poll_events();
    if (lc_begin_frame(env->swapchain) != LC_SUCCESS) {
        return NULL;
    }
    frame_open = 1;
    if (lc_swapchain_get_encoder(env->swapchain, &enc) != LC_SUCCESS) {
        FAIL_AT("get_encoder");
    }
    {
        lr_camera camera;

        make_camera(&camera, eye);
        {
            /* Custom look-at when requested. */
            static const float def_center[3] = { 0.0f, 0.0f, 0.0f };
            static const float up[3] = { 0.0f, 1.0f, 0.0f };
            const float *c = (center != NULL) ? center : def_center;

            lr_camera_look_at(&camera, eye, c, up);
        }
        if (lr_renderer_begin(env->renderer, &camera) != LR_SUCCESS) {
            FAIL_AT("renderer_begin");
        }
        renderer_open = 1;
    }
    submit_light(env);
    submit(env, user);
    if (use_gpu) {
        if (lr_renderer_prepare_gpu(env->renderer, enc) != LR_SUCCESS) {
            FAIL_AT("prepare");
        }
    }
    memset(&catt, 0, sizeof(catt));
    catt.view = env->color_view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    catt.clear_color[0] = 0.01f;
    catt.clear_color[1] = 0.015f;
    catt.clear_color[2] = 0.03f;
    catt.clear_color[3] = 1.0f;
    memset(&datt, 0, sizeof(datt));
    datt.view = env->depth_view;
    datt.depth_load_op = LC_LOAD_OP_CLEAR;
    datt.depth_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_depth = 1.0f;
    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.depth_attachment = &datt;
    pdesc.width = w;
    pdesc.height = h;
    if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
        FAIL_AT("begin_pass");
    }
    pass_open = 1;
    if (lr_renderer_render(env->renderer, enc, env->target) !=
        LR_SUCCESS) {
        FAIL_AT("render");
    }
    if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        FAIL_AT("end_pass");
    }
    pass_open = 0;
    {
        /* Swapchain leg: clear-only present (defined image). */
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
                                            &spass) != LC_SUCCESS) {
            FAIL_AT("swap_pass");
        }
        pass_open = 1;
        if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            FAIL_AT("swap_end");
        }
        pass_open = 0;
    }
    {
        lc_result end = lc_end_frame(env->swapchain);

        if (end != LC_SUCCESS && end != LC_SUBOPTIMAL) {
            FAIL_AT("end_frame");
        }
        frame_open = 0;
    }
    lr_renderer_end(env->renderer);
    renderer_open = 0;
    memset(&rbdesc, 0, sizeof(rbdesc));
    rbdesc.mip_level = 0;
    rbdesc.array_layer = 0;
    if (lc_image_query_readback(env->color_img, &rbdesc, &rbinfo) !=
        LC_SUCCESS) {
        return NULL;
    }
    need = rbinfo.size;
    px = (unsigned char *)malloc(need);
    if (px == NULL) {
        return NULL;
    }
    if (lc_image_readback(env->color_img, &rbdesc, px, need, NULL) !=
        LC_SUCCESS) {
        free(px);
        return NULL;
    }
    return px;
fail:
    printf("[info] render failed at %s\n", step);
    if (pass_open && enc != NULL) {
        lc_encoder_end_render_pass(enc);
    }
    if (frame_open) {
        (void)lc_end_frame(env->swapchain);
    }
    if (renderer_open) {
        lr_renderer_end(env->renderer);
    }
    return NULL;
}

static uint32_t count_nonclear(const unsigned char *px, uint32_t w,
                               uint32_t h) {
    uint32_t n = 0;
    uint32_t i;

    for (i = 0; i < w * h; i++) {
        /* Clear is (2,4,8)-ish; anything brighter drew. */
        if (px[i * 4] > 8 || px[i * 4 + 1] > 8 || px[i * 4 + 2] > 12) {
            n++;
        }
    }
    return n;
}

/* Scene A: deterministic grid, some culled. */
typedef struct scene_a {
    uint32_t total;
} scene_a;

static void submit_scene_a(gpu_env *env, void *user) {
    scene_a *scene = (scene_a *)user;
    int ix;
    int iy;
    int iz;

    scene->total = 0;
    for (ix = 0; ix < 12; ix++) {
        for (iy = 0; iy < 5; iy++) {
            for (iz = 0; iz < 4; iz++) {
                submit_cube(env, (float)(ix - 5) * 2.5f - 1.25f,
                            (float)(iy - 2) * 2.5f,
                            2.0f - (float)iz * 2.5f, 1.0f, 1.0f, 1.0f);
                scene->total++;
            }
        }
    }
    /* Behind the camera (culled) + non-uniform + negative scale. */
    submit_cube(env, 0.0f, 0.0f, 14.0f, 1.0f, 1.0f, 1.0f);
    submit_cube(env, -4.0f, 0.0f, 2.0f, 2.0f, 1.0f, 1.0f);
    submit_cube(env, 4.0f, 0.0f, 2.0f, -1.5f, 1.0f, 1.0f);
    scene->total += 3;
}

/* Single-case submitter for edge frames. */
typedef struct single_case {
    float x;
    float y;
    float z;
    float sx;
    float sy;
    float sz;
} single_case;

static void submit_single(gpu_env *env, void *user) {
    single_case *c = (single_case *)user;

    submit_cube(env, c->x, c->y, c->z, c->sx, c->sy, c->sz);
}

static void submit_nothing(gpu_env *env, void *user) {
    (void)env;
    (void)user;
}

/* Manual single/multi indirect draw through the instanced PBR
 * pipeline (one identity instance, visible=[0]). Returns pixels. */
static unsigned char *draw_manual_indirect(gpu_env *env,
                                           uint32_t draw_count,
                                           lc_buffer *ibuf) {
    static const float eye[3] = { 0.0f, 1.5f, 10.0f };
    lr_camera camera;
    lc_command_encoder *enc = NULL;
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_render_depth_attachment datt;
    lc_render_target_desc sig;
    lc_pipeline *pipe = NULL;
    lc_buffer *inst = NULL;
    lc_buffer *vis = NULL;
    lc_binding_set *iset = NULL;
    lr_pbr_push push;
    lr_gpu_instance ident;
    uint32_t slot = 0;
    lc_buffer_desc bdesc;
    lc_image_readback_desc rbdesc;
    lc_image_readback_info rbinfo;
    unsigned char *px = NULL;
    int frame_open = 0;
    int pass_open = 0;

    memset(&ident, 0, sizeof(ident));
    ident.model[0] = 1.0f;
    ident.model[5] = 1.0f;
    ident.model[10] = 1.0f;
    ident.model[15] = 1.0f;
    {
        lr_bounds b;

        lr_mesh_get_bounds(env->cube, &b);
        ident.bounds[0] = b.center[0];
        ident.bounds[1] = b.center[1];
        ident.bounds[2] = b.center[2];
        ident.bounds[3] = b.radius;
    }
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = sizeof(ident);
    bdesc.usage = LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(env->device, &bdesc, &inst) != LC_SUCCESS) {
        return NULL;
    }
    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = sizeof(slot);
    bdesc.usage = LC_BUFFER_USAGE_STORAGE | LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    if (lc_buffer_create(env->device, &bdesc, &vis) != LC_SUCCESS) {
        lc_buffer_destroy(inst);
        return NULL;
    }
    /* Set first (UNDEFINED lenient), uploads after. */
    {
        lc_buffer *pair[2] = { inst, vis };
        lc_binding_write writes[2];
        uint32_t k;

        if (lc_binding_set_create(env->renderer->instanced_layout,
                                  &iset) != LC_SUCCESS) {
            lc_buffer_destroy(vis);
            lc_buffer_destroy(inst);
            return NULL;
        }
        for (k = 0; k < 2; k++) {
            memset(&writes[k], 0, sizeof(writes[k]));
            writes[k].binding = k;
            writes[k].array_element = 0;
            writes[k].type = LC_BINDING_STORAGE_BUFFER;
            writes[k].u.buffer.buffer = pair[k];
            writes[k].u.buffer.offset = 0;
            writes[k].u.buffer.size = 0;
        }
        if (lc_binding_set_update(iset, writes, 2) != LC_SUCCESS) {
            lc_binding_set_destroy(iset);
            lc_buffer_destroy(vis);
            lc_buffer_destroy(inst);
            return NULL;
        }
    }
    if (lc_buffer_write(inst, 0, &ident, sizeof(ident)) != LC_SUCCESS ||
        lc_buffer_write(vis, 0, &slot, sizeof(slot)) != LC_SUCCESS) {
        lc_binding_set_destroy(iset);
        lc_buffer_destroy(vis);
        lc_buffer_destroy(inst);
        return NULL;
    }
    memset(&sig, 0, sizeof(sig));
    sig.width = 256;
    sig.height = 256;
    sig.color_attachment_count = 1;
    sig.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    sig.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    sig.samples = LC_SAMPLE_COUNT_1;
    if (lr_renderer_instanced_pipeline_for(
            env->renderer, &sig, LR_MATERIAL_PBR_METALLIC_ROUGHNESS,
            LC_CULL_BACK, LC_FRONT_FACE_COUNTER_CLOCKWISE,
            &pipe) != LR_SUCCESS) {
        lc_binding_set_destroy(iset);
        lc_buffer_destroy(vis);
        lc_buffer_destroy(inst);
        return NULL;
    }
    lc_poll_events();
    if (lc_begin_frame(env->swapchain) != LC_SUCCESS) {
        goto fail;
    }
    frame_open = 1;
    if (lc_swapchain_get_encoder(env->swapchain, &enc) != LC_SUCCESS) {
        goto fail;
    }
    make_camera(&camera, eye);
    if (lr_renderer_begin(env->renderer, &camera) != LR_SUCCESS) {
        goto fail;
    }
    submit_light(env);
    if (lc_encoder_transition_buffer(enc, inst,
                                     LC_RESOURCE_STATE_STORAGE_READ) !=
            LC_SUCCESS ||
        lc_encoder_transition_buffer(enc, vis,
                                     LC_RESOURCE_STATE_STORAGE_READ) !=
            LC_SUCCESS ||
        lc_encoder_transition_buffer(enc, ibuf,
                                     LC_RESOURCE_STATE_INDIRECT_READ) !=
            LC_SUCCESS) {
        goto fail;
    }
    memset(&catt, 0, sizeof(catt));
    catt.view = env->color_view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    catt.clear_color[0] = 0.01f;
    catt.clear_color[1] = 0.015f;
    catt.clear_color[2] = 0.03f;
    catt.clear_color[3] = 1.0f;
    memset(&datt, 0, sizeof(datt));
    datt.view = env->depth_view;
    datt.depth_load_op = LC_LOAD_OP_CLEAR;
    datt.depth_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_depth = 1.0f;
    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.depth_attachment = &datt;
    pdesc.width = 256;
    pdesc.height = 256;
    if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
        goto fail;
    }
    pass_open = 1;
    if (lc_encoder_bind_pipeline(enc, pipe) != LC_SUCCESS ||
        lc_encoder_bind_binding_set(enc, pipe, 0,
                                    env->mat->set) != LC_SUCCESS ||
        lc_encoder_bind_binding_set(enc, pipe, 1,
                                    env->renderer->shadow_set) !=
            LC_SUCCESS ||
        lc_encoder_bind_binding_set(enc, pipe, 2,
                                    env->renderer->empty_env_set) !=
            LC_SUCCESS ||
        lc_encoder_bind_binding_set(enc, pipe, 3, iset) !=
            LC_SUCCESS ||
        lc_encoder_bind_vertex_buffer(enc, 0, env->cube->vertex_buffer,
                                      0) != LC_SUCCESS ||
        lc_encoder_bind_index_buffer(enc, env->cube->index_buffer, 0,
                                     LC_INDEX_UINT32) != LC_SUCCESS) {
        goto fail;
    }
    memset(&push, 0, sizeof(push));
    push.model[0] = 1.0f;
    push.model[5] = 1.0f;
    push.model[10] = 1.0f;
    push.model[15] = 1.0f;
    push.normal_matrix[0] = 1.0f;
    push.normal_matrix[5] = 1.0f;
    push.normal_matrix[10] = 1.0f;
    push.flags = 1u;
    if (lc_encoder_push_constants(enc, pipe,
                                  (uint32_t)
                                      LC_SHADER_VISIBILITY_VERTEX |
                                  (uint32_t)
                                      LC_SHADER_VISIBILITY_FRAGMENT,
                                  0, sizeof(push), &push) !=
            LC_SUCCESS ||
        lc_encoder_draw_indexed_indirect(enc, ibuf, 0, draw_count,
                                         20) != LC_SUCCESS) {
        goto fail;
    }
    if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        goto fail;
    }
    pass_open = 0;
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
            goto fail;
        }
    }
    {
        lc_result end = lc_end_frame(env->swapchain);

        if (end != LC_SUCCESS && end != LC_SUBOPTIMAL) {
            goto fail;
        }
    }
    lr_renderer_end(env->renderer);
    memset(&rbdesc, 0, sizeof(rbdesc));
    if (lc_image_query_readback(env->color_img, &rbdesc, &rbinfo) !=
        LC_SUCCESS) {
        goto done;
    }
    px = (unsigned char *)malloc(rbinfo.size);
    if (px != NULL &&
        lc_image_readback(env->color_img, &rbdesc, px, rbinfo.size,
                          NULL) != LC_SUCCESS) {
        free(px);
        px = NULL;
    }
done:
    lc_binding_set_destroy(iset);
    lc_buffer_destroy(vis);
    lc_buffer_destroy(inst);
    return px;
fail:
    /* Unwind an open pass/frame so later cases still run. */
    if (pass_open) {
        lc_encoder_end_render_pass(enc);
        pass_open = 0;
    }
    if (frame_open) {
        (void)lc_end_frame(env->swapchain);
    }
    lr_renderer_end(env->renderer);
    lc_binding_set_destroy(iset);
    lc_buffer_destroy(vis);
    lc_buffer_destroy(inst);
    return NULL;
}

/* Shared transform generator (submit + oracle stay identical). */
typedef struct instance_xform {
    float x;
    float y;
    float z;
    float sx;
    float sy;
    float sz;
} instance_xform;

static uint32_t gen_scene_a(instance_xform *out, uint32_t cap) {
    uint32_t n = 0;
    int ix;
    int iy;
    int iz;

    for (ix = 0; ix < 12 && n < cap; ix++) {
        for (iy = 0; iy < 5 && n < cap; iy++) {
            for (iz = 0; iz < 4 && n < cap; iz++) {
                out[n].x = (float)(ix - 5) * 2.5f - 1.25f;
                out[n].y = (float)(iy - 2) * 2.5f;
                out[n].z = 2.0f - (float)iz * 2.5f;
                out[n].sx = 1.0f;
                out[n].sy = 1.0f;
                out[n].sz = 1.0f;
                n++;
            }
        }
    }
    if (n + 3 <= cap) {
        out[n].x = 0.0f;
        out[n].y = 0.0f;
        out[n].z = 14.0f;
        out[n].sx = 1.0f;
        out[n].sy = 1.0f;
        out[n].sz = 1.0f;
        n++;
        out[n].x = -4.0f;
        out[n].y = 0.0f;
        out[n].z = 2.0f;
        out[n].sx = 2.0f;
        out[n].sy = 1.0f;
        out[n].sz = 1.0f;
        n++;
        out[n].x = 4.0f;
        out[n].y = 0.0f;
        out[n].z = 2.0f;
        out[n].sx = -1.5f;
        out[n].sy = 1.0f;
        out[n].sz = 1.0f;
        n++;
    }
    return n;
}

/* CPU oracle over generated transforms (independent math).
 * When want_set is provided, marks visible indices. */
static uint32_t oracle_count(const instance_xform *list, uint32_t n,
                             const float eye[3], const float center[3],
                             const float bc[3], float bradius,
                             unsigned char *want_set) {
    lr_camera camera;
    float vp[16];
    float planes[6][4];
    float m[16];
    uint32_t visible = 0;
    uint32_t i;

    lr_camera_init(&camera);
    lr_camera_set_perspective(&camera, 1.0471976f, 1.0f, 0.1f, 100.0f);
    {
        static const float up[3] = { 0.0f, 1.0f, 0.0f };

        lr_camera_look_at(&camera, eye, center, up);
    }
    /* viewProj = proj * view (column-major). */
    {
        int c;
        int r;

        for (c = 0; c < 4; c++) {
            for (r = 0; r < 4; r++) {
                vp[c * 4 + r] = camera.projection[r] * camera.view[c * 4] +
                                camera.projection[4 + r] *
                                    camera.view[c * 4 + 1] +
                                camera.projection[8 + r] *
                                    camera.view[c * 4 + 2] +
                                camera.projection[12 + r] *
                                    camera.view[c * 4 + 3];
            }
        }
    }
    oracle_planes(vp, planes);
    /* The extraction must match the renderer's own math exactly
     * (same convention by construction). */
    {
        float ref[6][4];
        float worst = 0.0f;
        int a;
        int b;

        lr_frustum_from_viewproj(vp, ref);
        for (a = 0; a < 6; a++) {
            for (b = 0; b < 4; b++) {
                float d = planes[a][b] - ref[a][b];

                if (d < 0.0f) {
                    d = -d;
                }
                if (d > worst) {
                    worst = d;
                }
            }
        }
        if (worst != 0.0f) {
            printf("[info] plane extraction diff vs renderer: %g\n",
                   (double)worst);
        }
    }
    for (i = 0; i < n; i++) {
        /* Rigid+scale matrix (no rotation in these scenes). */
        float wc[3];
        float wr = 0.0f;

        memset(m, 0, sizeof(m));
        m[0] = list[i].sx;
        m[5] = list[i].sy;
        m[10] = list[i].sz;
        m[12] = list[i].x;
        m[13] = list[i].y;
        m[14] = list[i].z;
        m[15] = 1.0f;
        oracle_world_sphere(m, bc, bradius, wc, &wr);
        if (oracle_visible(planes, wc, wr)) {
            visible++;
            if (want_set != NULL) {
                want_set[i] = 1;
            }
        }
    }
    return visible;
}

int main(void) {
    gpu_env env;
    scene_a scene;
    instance_xform list[512];
    lr_bounds bounds;
    lr_camera camera;
    static const float eye[3] = { 0.0f, 1.5f, 10.0f };
    static const float center[3] = { 0.0f, 0.0f, 0.0f };
    static const float away[3] = { 0.0f, 1.5f, 20.0f };
    static const float far_eye[3] = { 0.0f, 1.5f, 40.0f };
    uint32_t n = 0;
    uint32_t oracle = 0;
    unsigned char *px_gpu = NULL;
    unsigned char *px_cpu = NULL;
    lr_gpu_driven_stats stats;
    uint32_t i;

    memset(&env, 0, sizeof(env));
    memset(&scene, 0, sizeof(scene));
    if (lc_init() != LC_SUCCESS) {
        printf("SKIP: lc_init failed\n");
        return 0;
    }
    {
        int dev = make_device(&env.device);

        if (dev == 1) {
            SKIP_ENV("a Vulkan device");
        }
        CHECK(dev == 0, "device creates");
    }
    {
        lc_compute_capabilities caps;

        memset(&caps, 0, sizeof(caps));
        lc_device_get_compute_capabilities(env.device, &caps);
        if (!caps.compute_supported || !caps.indirect_draw_supported) {
            SKIP_ENV("compute + indirect support");
        }
        CHECK(caps.max_workgroup_count[0] > 0, "compute limits sane");
        printf("[info] dedicated_compute=%d multi_draw=%d "
               "indirect_count=%d\n",
               caps.dedicated_compute, caps.multi_draw_indirect,
               caps.indirect_count);
    }
    if (make_window_stack(&env) != 0) {
        SKIP_ENV("window + surface + swapchain");
    }
    if (make_target(&env, 256, 256) != 0) {
        CHECK(0, "offscreen target creates");
        lc_shutdown();
        return 1;
    }
    if (make_renderer_stack(&env) != 0) {
        CHECK(0, "renderer + cube + pbr creates");
        lc_shutdown();
        return 1;
    }
    lr_mesh_get_bounds(env.cube, &bounds);

    /* Mode validation. */
    CHECK(lr_renderer_set_render_mode(NULL, LR_RENDER_MODE_GPU_DRIVEN) ==
              LR_ERROR_INVALID_ARGUMENT,
          "set mode NULL rejected");
    CHECK(lr_renderer_set_render_mode(env.renderer,
                                      (lr_render_mode)99) ==
              LR_ERROR_INVALID_ARGUMENT,
          "set mode bad enum rejected");
    CHECK(lr_renderer_get_render_mode(NULL) == LR_RENDER_MODE_CPU,
          "get mode NULL is CPU");
    CHECK(lr_renderer_set_render_mode(env.renderer,
                                      LR_RENDER_MODE_GPU_DRIVEN) ==
              LR_SUCCESS,
          "gpu mode enables");
    CHECK(lr_renderer_get_render_mode(env.renderer) ==
              LR_RENDER_MODE_GPU_DRIVEN,
          "gpu mode reported");

    n = gen_scene_a(list, 512);
    CHECK(n == 243, "scene generator count");
    {
        static unsigned char want[512];

        memset(want, 0, sizeof(want));
        oracle = oracle_count(list, n, eye, center, bounds.center,
                              bounds.radius, want);
        memcpy(env.oracle_want, want, sizeof(env.oracle_want));
    }
    printf("[info] scene instances=%u oracle visible=%u\n", n, oracle);
    CHECK(oracle > 100 && oracle < n, "oracle partial visibility sane");

    /* Scene A, GPU path. */
    px_gpu = render_offscreen(&env, submit_scene_a, &scene, 1, 256, 256,
                              eye, center);
    CHECK(px_gpu != NULL, "gpu scene renders");
    CHECK(scene.total == n, "gpu submitted all instances");
    if (px_gpu != NULL) {
        uint32_t lit = count_nonclear(px_gpu, 256, 256);

        printf("[info] gpu lit pixels=%u\n", lit);
        CHECK(lit > 2000 && lit < 256u * 256u, "gpu pixels non-trivial");
        lr_renderer_update_gpu_visibility_stats(env.renderer);
        lr_renderer_get_gpu_driven_stats(env.renderer, &stats);
        printf("[info] submitted=%llu visible=%llu culled=%llu "
               "dispatches=%llu indirect=%llu batches=%llu\n",
               (unsigned long long)stats.instances_submitted,
               (unsigned long long)stats.instances_visible,
               (unsigned long long)stats.instances_culled,
               (unsigned long long)stats.compute_dispatches,
               (unsigned long long)stats.indirect_draw_calls,
               (unsigned long long)stats.gpu_driven_batches);
        CHECK(stats.instances_submitted == n, "submitted count exact");
        CHECK(stats.instances_visible == oracle,
              "GPU visible == CPU oracle");
        /* Full SET comparison (sorted GPU indices vs oracle flags):
         * proves compaction content, not just the count. Aggregates
         * across ALL groups: winding parity splits mirrored items
         * into their own group (CW front-face variant), so a
         * single-group read would miss them. */
        {
            lr_renderer *rr = env.renderer;
            uint32_t got[512];
            uint32_t want[512];
            uint32_t ngot = 0;
            uint32_t nwant = 0;
            uint32_t a;
            uint32_t g;
            int same = 1;

            memset(got, 0, sizeof(got));
            memset(want, 0, sizeof(want));
            for (g = 0; g < rr->group_count; g++) {
                uint32_t words[2] = { 0, 0 };
                uint32_t cnt = 0;

                if (rr->groups[g].count == 0 ||
                    rr->gpu_last_flight >=
                        rr->groups[g].flights_owned ||
                    !rr->groups[g]
                         .flights[rr->gpu_last_flight]
                         .initialized ||
                    rr->groups[g]
                            .flights[rr->gpu_last_flight]
                            .visible == NULL ||
                    rr->groups[g]
                            .flights[rr->gpu_last_flight]
                            .counter == NULL) {
                    continue;
                }
                if (lc_buffer_read(
                        rr->groups[g]
                            .flights[rr->gpu_last_flight]
                            .counter,
                        0, words, sizeof(words)) != LC_SUCCESS) {
                    same = 0;
                    break;
                }
                cnt = words[0];
                if (cnt > rr->groups[g].capacity) {
                    cnt = rr->groups[g].capacity;
                }
                if (ngot + cnt > 512u) {
                    same = 0;
                    break;
                }
                if (cnt > 0 &&
                    lc_buffer_read(
                        rr->groups[g]
                            .flights[rr->gpu_last_flight]
                            .visible,
                        0, &got[ngot],
                        cnt * sizeof(uint32_t)) != LC_SUCCESS) {
                    same = 0;
                    break;
                }
                /* Visible entries are group-local compaction slots
                 * (the vertex shader indexes instances[] with them);
                 * translate to submission objectIds via the group's
                 * CPU mirror before comparing with the oracle. */
                {
                    uint32_t s;

                    for (s = 0; s < cnt; s++) {
                        uint32_t slot = got[ngot + s];

                        if (slot >= rr->groups[g].count ||
                            rr->groups[g].cpu == NULL) {
                            same = 0;
                            break;
                        }
                        got[ngot + s] =
                            rr->groups[g].cpu[slot].object_id;
                    }
                    if (!same) {
                        break;
                    }
                }
                ngot += cnt;
            }
            if (ngot != (uint32_t)stats.instances_visible) {
                same = 0;
            }
            for (a = 0; a < n; a++) {
                if (env.oracle_want[a]) {
                    want[nwant++] = a;
                }
            }
            /* Insertion sort on the GPU side (N small here). */
            {
                uint32_t a2;
                uint32_t b2;

                for (a2 = 0; a2 < ngot; a2++) {
                    for (b2 = a2 + 1; b2 < ngot; b2++) {
                        if (got[b2] < got[a2]) {
                            uint32_t t = got[a2];

                            got[a2] = got[b2];
                            got[b2] = t;
                        }
                    }
                }
            }
            if (ngot != nwant) {
                same = 0;
            } else {
                for (a = 0; a < ngot; a++) {
                    if (got[a] != want[a]) {
                        same = 0;
                        break;
                    }
                }
            }
            CHECK(same && ngot > 0, "GPU visible SET == oracle set");
        }
        CHECK(stats.instances_visible + stats.instances_culled == n,
              "visible + culled == total");
        CHECK(stats.instances_visible <= n, "visible bounded");
        /* Two parity groups (regular + the one mirrored cube):
         * one cull+finalize pair and one indirect draw each. */
        CHECK(stats.compute_dispatches == 4, "two cull+finalize pairs");
        CHECK(stats.indirect_draw_calls == 2, "two indirect draws");
        CHECK(stats.gpu_driven_batches == 2, "two batches");
        CHECK(stats.counter_overflows == 0, "no overflow");
    }

    /* Scene A, CPU path: byte-exact equivalence. */
    CHECK(lr_renderer_set_render_mode(env.renderer,
                                      LR_RENDER_MODE_CPU) == LR_SUCCESS,
          "cpu mode restores");
    px_cpu = render_offscreen(&env, submit_scene_a, &scene, 0, 256, 256,
                              eye, center);
    CHECK(px_cpu != NULL, "cpu scene renders");
    if (px_gpu != NULL && px_cpu != NULL) {
        CHECK(memcmp(px_gpu, px_cpu, 256u * 256u * 4u) == 0,
              "cpu/gpu pixels identical");
    }
    CHECK(lr_renderer_set_render_mode(env.renderer,
                                      LR_RENDER_MODE_GPU_DRIVEN) ==
              LR_SUCCESS,
          "gpu mode re-enables");

    /* Zero-visible: look away (only the behind-camera cube can
     * appear; the oracle decides the exact expectation). */
    {
        unsigned char *px = render_offscreen(&env, submit_scene_a,
                                             &scene, 1, 256, 256, eye,
                                             away);
        uint32_t away_oracle;

        CHECK(px != NULL, "away scene renders");
        if (px != NULL) {
            free(px);
        }
        away_oracle = oracle_count(list, n, eye, away, bounds.center,
                                   bounds.radius, NULL);
        printf("[info] away oracle=%u\n", away_oracle);
        lr_renderer_update_gpu_visibility_stats(env.renderer);
        lr_renderer_get_gpu_driven_stats(env.renderer, &stats);
        CHECK(stats.instances_visible == away_oracle,
              "away count matches oracle");
    }

    /* True zero-visible: empty submission after a populated frame
     * (no stale commands, no accidental draws). */
    {
        unsigned char *px = render_offscreen(&env, submit_nothing,
                                             NULL, 1, 256, 256, eye,
                                             center);

        CHECK(px != NULL, "empty scene renders");
        if (px != NULL) {
            CHECK(count_nonclear(px, 256, 256) == 0,
                  "empty frame draws nothing");
            free(px);
        }
        lr_renderer_update_gpu_visibility_stats(env.renderer);
        lr_renderer_get_gpu_driven_stats(env.renderer, &stats);
        CHECK(stats.instances_visible == 0, "empty count is zero");
        CHECK(stats.instances_submitted == 0, "empty submits zero");
    }

    /* All-visible: far camera (oracle decides; expectation is
     * everything). */
    {
        unsigned char *px = render_offscreen(&env, submit_scene_a,
                                             &scene, 1, 256, 256,
                                             far_eye, center);
        uint32_t far_oracle;

        CHECK(px != NULL, "far scene renders");
        if (px != NULL) {
            free(px);
        }
        far_oracle = oracle_count(list, n, far_eye, center,
                                  bounds.center, bounds.radius, NULL);
        printf("[info] far oracle=%u\n", far_oracle);
        lr_renderer_update_gpu_visibility_stats(env.renderer);
        lr_renderer_get_gpu_driven_stats(env.renderer, &stats);
        CHECK(stats.instances_visible == far_oracle,
              "far count matches oracle");
        CHECK(far_oracle == n, "far oracle is all-visible");
    }

    /* Frustum edge cases, one instance per frame. */
    {
        static const struct {
            float x;
            float y;
            float z;
            float sx;
            float sy;
            float sz;
            int expect;
            const char *name;
        } cases[] = {
            { 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 1, "inside" },
            { -30.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, "far-left" },
            { 30.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, "far-right" },
            { 0.0f, 25.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, "far-top" },
            { 0.0f, -25.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0, "far-bottom" },
            { 0.0f, 0.0f, 14.0f, 1.0f, 1.0f, 1.0f, 0, "behind" },
            { 0.0f, 0.0f, 9.0f, 1.0f, 1.0f, 1.0f, 1, "near-inside" },
            { 0.0f, 0.0f, -80.0f, 1.0f, 1.0f, 1.0f, 1, "far-inside" },
            { 0.0f, 0.0f, 0.0f, 8.0f, 8.0f, 8.0f, 1, "huge" },
            { 0.0f, 1.5f, 10.0f, 5.0f, 5.0f, 5.0f, 1,
              "camera-inside" },
            { 0.0f, 0.0f, 0.0f, 2.0f, 1.0f, 1.0f, 1, "non-uniform" },
            { 0.0f, 0.0f, 0.0f, -1.5f, 1.0f, 1.0f, 1,
              "negative-scale" },
            { 0.0f, 0.0f, 0.0f, -2.0f, -2.0f, -2.0f, 1,
              "negative-all" },
        };

        for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            instance_xform one;
            uint32_t want;
            char msg[128];

            one.x = cases[i].x;
            one.y = cases[i].y;
            one.z = cases[i].z;
            one.sx = cases[i].sx;
            one.sy = cases[i].sy;
            one.sz = cases[i].sz;
            want = oracle_count(&one, 1, eye, center, bounds.center,
                                bounds.radius, NULL);
            snprintf(msg, sizeof(msg), "oracle %s == %d",
                     cases[i].name, cases[i].expect);
            CHECK((int)want == cases[i].expect, msg);
            /* Drive the same single instance through the GPU. */
            {
                single_case sc;
                unsigned char *px = NULL;

                sc.x = cases[i].x;
                sc.y = cases[i].y;
                sc.z = cases[i].z;
                sc.sx = cases[i].sx;
                sc.sy = cases[i].sy;
                sc.sz = cases[i].sz;
                px = render_offscreen(&env, submit_single, &sc, 1, 256,
                                      256, eye, center);
                snprintf(msg, sizeof(msg), "gpu %s renders",
                         cases[i].name);
                CHECK(px != NULL, msg);
                if (px != NULL) {
                    free(px);
                }
                lr_renderer_update_gpu_visibility_stats(env.renderer);
                lr_renderer_get_gpu_driven_stats(env.renderer, &stats);
                snprintf(msg, sizeof(msg), "gpu %s == oracle (%u)",
                         cases[i].name, want);
                CHECK(stats.instances_visible == want, msg);
            }
        }
    }

    /* Multi-draw indirect: 3 identical commands in one call draw
     * exactly what a single command draws (idempotent overdraw).
     * Exercises the native multi path (or the honest loop) with
     * pixel proof. */
    {
        uint32_t index_count = lr_mesh_get_index_count(env.cube);
        lc_indirect_draw_indexed_command cmds[3];
        lc_buffer *ibuf = NULL;
        lc_buffer_desc bdesc;
        unsigned char *px1 = NULL;
        unsigned char *px3 = NULL;
        uint32_t k;

        memset(cmds, 0, sizeof(cmds));
        for (k = 0; k < 3; k++) {
            cmds[k].index_count = index_count;
            cmds[k].instance_count = 1;
            cmds[k].first_index = 0;
            cmds[k].vertex_offset = 0;
            cmds[k].first_instance = 0;
        }
        memset(&bdesc, 0, sizeof(bdesc));
        bdesc.size = sizeof(cmds);
        bdesc.usage = LC_BUFFER_USAGE_INDIRECT |
                      LC_BUFFER_USAGE_TRANSFER_DST;
        bdesc.memory = LC_MEMORY_CPU_TO_GPU;
        CHECK(lc_buffer_create(env.device, &bdesc, &ibuf) ==
                  LC_SUCCESS,
              "hand indirect buffer creates");
        if (ibuf != NULL) {
            CHECK(lc_buffer_write(ibuf, 0, cmds, sizeof(cmds)) ==
                      LC_SUCCESS,
                  "hand indirect commands written");
        }
        px1 = draw_manual_indirect(&env, 1, ibuf);
        px3 = draw_manual_indirect(&env, 3, ibuf);
        CHECK(px1 != NULL && px3 != NULL, "multi draws render");
        if (px1 != NULL && px3 != NULL) {
            CHECK(count_nonclear(px1, 256, 256) > 500,
                  "single indirect draws pixels");
            CHECK(memcmp(px1, px3, 256u * 256u * 4u) == 0,
                  "3x multi == 1x single pixels");
        }
        free(px1);
        free(px3);
        lc_buffer_destroy(ibuf);
    }

    /* render_scene smoke: automatic prepare inside the HDR flow. */
    {
        lc_command_encoder *enc2 = NULL;

        lc_poll_events();
        if (lc_begin_frame(env.swapchain) == LC_SUCCESS &&
            lc_swapchain_get_encoder(env.swapchain, &enc2) ==
                LC_SUCCESS) {
            lr_camera camera;

            make_camera(&camera, eye);
            if (lr_renderer_begin(env.renderer, &camera) ==
                    LR_SUCCESS) {
                submit_light(&env);
                submit_scene_a(&env, &scene);
                if (lr_renderer_render_scene(env.renderer, enc2, 256,
                                             256) == LR_SUCCESS) {
                    lr_gpu_driven_stats s2;

                    lr_renderer_get_gpu_driven_stats(env.renderer,
                                                     &s2);
                    CHECK(s2.gpu_driven_batches == 2,
                          "scene path prepares two batches");
                    CHECK(s2.compute_dispatches == 4,
                          "scene path four dispatches");
                } else {
                    CHECK(0, "render_scene gpu succeeds");
                }
                lr_renderer_end(env.renderer);
            }
            /* Present leg (defined image either way). */
            {
                lc_render_swapchain_pass_desc spass;

                memset(&spass, 0, sizeof(spass));
                spass.color_load_op = LC_LOAD_OP_CLEAR;
                spass.color_store_op = LC_STORE_OP_STORE;
                if (lc_encoder_begin_swapchain_pass(
                        enc2, env.swapchain, &spass) == LC_SUCCESS) {
                    (void)lc_encoder_end_render_pass(enc2);
                }
            }
            {
                lc_result end = lc_end_frame(env.swapchain);

                CHECK(end == LC_SUCCESS || end == LC_SUBOPTIMAL,
                      "scene frame presents");
            }
        } else {
            CHECK(0, "scene frame begins");
        }
    }

    /* Descriptor stability: sets alive constant across frames. */
    {
        lr_gpu_driven_stats s0;
        lr_gpu_driven_stats s1;
        unsigned char *px = NULL;

        lr_renderer_get_gpu_driven_stats(env.renderer, &s0);
        px = render_offscreen(&env, submit_scene_a, &scene, 1, 256,
                              256, eye, center);
        CHECK(px != NULL, "stability frame renders");
        free(px);
        lr_renderer_get_gpu_driven_stats(env.renderer, &s1);
        CHECK(s1.descriptor_sets_alive == s0.descriptor_sets_alive,
              "descriptor sets stable across frames");
        CHECK(s1.descriptor_updates == s0.descriptor_updates,
              "no per-frame descriptor updates");
    }

    /* CPU fallback still correct after GPU use. */
    {
        unsigned char *px = NULL;

        CHECK(lr_renderer_set_render_mode(env.renderer,
                                          LR_RENDER_MODE_CPU) ==
                  LR_SUCCESS,
              "cpu fallback restores");
        px = render_offscreen(&env, submit_scene_a, &scene, 0, 256,
                              256, eye, center);
        CHECK(px != NULL, "fallback frame renders");
        if (px != NULL && px_cpu != NULL) {
            CHECK(memcmp(px, px_cpu, 256u * 256u * 4u) == 0,
                  "fallback pixels match cpu reference");
            free(px);
        }
        CHECK(lr_renderer_set_render_mode(env.renderer,
                                          LR_RENDER_MODE_GPU_DRIVEN) ==
                  LR_SUCCESS,
              "gpu mode re-enables at end");
    }

    /* Timings: CPU submit+cull vs GPU prepare (CPU-side only; GPU
     * timestamps deferred, reported NOT MEASURED). */
    {
        uint64_t t0;
        uint64_t t1;
        double cpu_ms;
        lr_gpu_driven_stats s3;

        CHECK(lr_renderer_set_render_mode(env.renderer,
                                          LR_RENDER_MODE_CPU) ==
                  LR_SUCCESS,
              "cpu mode for timing");
        t0 = lr_perf_now();
        {
            unsigned char *px = render_offscreen(&env, submit_scene_a,
                                                 &scene, 0, 256, 256,
                                                 eye, center);

            free(px);
        }
        t1 = lr_perf_now();
        cpu_ms = lr_perf_to_ms(t1 - t0, lr_perf_frequency());
        CHECK(lr_renderer_set_render_mode(env.renderer,
                                          LR_RENDER_MODE_GPU_DRIVEN) ==
                  LR_SUCCESS,
              "gpu mode for timing");
        {
            unsigned char *px = render_offscreen(&env, submit_scene_a,
                                                 &scene, 1, 256, 256,
                                                 eye, center);

            free(px);
        }
        lr_renderer_get_gpu_driven_stats(env.renderer, &s3);
        printf("[info] cpu frame ms=%.3f gpu prepare ms=%.3f "
               "gpu cull/draw ms=NOT MEASURED\n",
               cpu_ms, s3.cpu_prepare_ms);
        CHECK(cpu_ms >= 0.0 && s3.cpu_prepare_ms >= 0.0,
              "timings recorded");
    }

    /* Three flights in flight (PART AF31): same scene, deeper
     * overlap; per-flight resources rotate without races. */
    if (remake_swapchain(&env, 3) != 0) {
        CHECK(0, "swapchain recreates with 3 flights");
    } else {
        unsigned char *px = render_offscreen(&env, submit_scene_a,
                                             &scene, 1, 256, 256, eye,
                                             center);

        CHECK(px != NULL, "3-flight scene renders");
        lr_renderer_update_gpu_visibility_stats(env.renderer);
        lr_renderer_get_gpu_driven_stats(env.renderer, &stats);
        CHECK(stats.instances_visible == oracle,
              "3-flight visible == oracle");
        if (px != NULL && px_cpu != NULL) {
            CHECK(memcmp(px, px_cpu, 256u * 256u * 4u) == 0,
                  "3-flight pixels match cpu reference");
            free(px);
        }
    }

    free(px_cpu);
    free(px_gpu);
    printf("phase21 gpu-driven: %d passed, %d failed\n", g_passed,
           g_failed);
    lr_material_destroy(env.mat);
    lr_mesh_destroy(env.cube);
    lr_renderer_destroy(env.renderer);
    lc_render_target_destroy(env.target);
    lc_image_view_destroy(env.depth_view);
    lc_image_destroy(env.depth_img);
    lc_image_view_destroy(env.color_view);
    lc_image_destroy(env.color_img);
    lc_swapchain_destroy(env.swapchain);
    lc_surface_destroy(env.surface);
    lc_window_destroy(env.window);
    lc_device_destroy(env.device);
    lc_shutdown();
    return g_failed ? 1 : 0;
}
