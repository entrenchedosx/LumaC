/*
 * GPU skinning test (Phase 29, renderer-owned).
 *
 * Proves three things against the same offscreen target:
 *  1. Static-mesh regression: a skinned mesh drawn with an
 *     IDENTITY palette renders (near-)identical pixels to the
 *     same geometry drawn RIGID (no palette) — the skinned
 *     pipelines preserve unskinned behavior.
 *  2. Oracle: the same skinned mesh drawn with a TRANSLATED
 *     joint palette (+X by 0.5 in local space) moves pixels vs
 *     the bind pose (lit-pixel centroid shifts right).
 *  3. Flags: lr_mesh_is_skinned reports rigid vs skinned
 *     meshes (and NULL), and stats observe skinned draws.
 *
 * Every leg runs in BOTH CPU and GPU-driven modes (skinned
 * items always take the CPU loop; the GPU-mode leg additionally
 * proves rigid groups + skinned direct draws coexist).
 *
 * Headless-safe: SKIP (exit 0) when no window/Vulkan is
 * available. Validation layers stay enabled throughout.
 *
 * Geometry: a 2-triangle vertical quad (XY plane, facing +Z)
 * authored directly with joints/weights (no importer needed):
 * the rigid reference uses the {0,0,0,0}/{1,0,0,0} convention;
 * the skinned mesh binds every vertex 100% to joint 1. The
 * identity palette is [I, I]; the moved palette is [I, T(+X)].
 * Unlit material (flat color, no lighting variance) keeps the
 * comparison exact.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>

#include "graphics/graphics_internal.h"

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

enum { FW = 256, FH = 256 };

static void test_wait_idle(lc_device *device) {
    if (device != NULL && device->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->device);
    }
}

/* One quad vertex (positions face +Z; normal/tangent/UV valid). */
static void skin_vertex(lr_vertex *v, float x, float y,
                        uint32_t joint) {
    memset(v, 0, sizeof(*v));
    v->position[0] = x;
    v->position[1] = y;
    v->position[2] = 0.0f;
    v->normal[0] = 0.0f;
    v->normal[1] = 0.0f;
    v->normal[2] = 1.0f;
    v->tangent[0] = 1.0f;
    v->tangent[1] = 0.0f;
    v->tangent[2] = 0.0f;
    v->tangent[3] = 1.0f;
    v->texcoord[0] = x + 0.5f;
    v->texcoord[1] = y + 0.5f;
    v->joints[0] = joint;
    v->joints[1] = 0;
    v->joints[2] = 0;
    v->joints[3] = 0;
    v->weights[0] = 1.0f;
    v->weights[1] = 0.0f;
    v->weights[2] = 0.0f;
    v->weights[3] = 0.0f;
}

static void identity_mat(float m[16]) {
    memset(m, 0, 16 * sizeof(float));
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = 1.0f;
    m[15] = 1.0f;
}

static void translate_mat(float m[16], float x, float y,
                          float z) {
    identity_mat(m);
    m[12] = x;
    m[13] = y;
    m[14] = z;
}

static unsigned char *readback_rgba8(lc_device *device,
                                     lc_image *image) {
    lc_buffer *staging = NULL;
    lc_buffer_desc bdesc;
    void *mapped = NULL;
    unsigned char *out = NULL;
    uint64_t bytes = (uint64_t)FW * FH * 4u;

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = bytes;
    bdesc.usage = LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_TO_CPU;
    if (lc_buffer_create(device, &bdesc, &staging) != LC_SUCCESS) {
        return NULL;
    }
    test_wait_idle(device);
    if (lc_vulkan_copy_image_to_buffer(device, image, 0, 0, FW, FH,
                                       1, staging->vk_buffer,
                                       0) != LC_SUCCESS) {
        lc_buffer_destroy(staging);
        return NULL;
    }
    if (lc_buffer_map(staging, &mapped) != LC_SUCCESS ||
        mapped == NULL) {
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

/* Lit = strong red over the dark clear (unlit flat red quad). */
static void analyze(const unsigned char *px, uint32_t *out_lit,
                    double *out_cx) {
    uint32_t x;
    uint32_t y;
    uint32_t lit = 0;
    uint64_t sum_x = 0;

    for (y = 0; y < FH; y++) {
        for (x = 0; x < FW; x++) {
            const unsigned char *p = &px[(y * FW + x) * 4u];

            if (p[0] > 90 && p[1] < 90 && p[2] < 90) {
                lit++;
                sum_x += x;
            }
        }
    }
    *out_lit = lit;
    *out_cx = (lit > 0) ? (double)sum_x / (double)lit : -1.0;
}

/* Render one frame: begin, submit ONE item (palette optional),
 * GPU-prepare when asked (outside any pass), draw the offscreen
 * pass, end. Returns pixels (malloc'd) or NULL. */
static unsigned char *render_one(
    lc_device *device, lc_swapchain *swapchain, lr_renderer *renderer,
    lr_camera *camera, lc_render_target *offscreen,
    lc_image_view *off_view, lc_image_view *off_dview, lr_mesh *mesh,
    lr_material *mat, const float (*palette)[16],
    uint32_t joints, int gpu_mode, lr_render_stats *out_stats) {
    lc_command_encoder *enc = NULL;
    lc_render_color_attachment catt;
    lc_render_depth_attachment datt;
    lc_render_pass_desc pdesc;
    lc_render_swapchain_pass_desc spass;
    lc_result res;
    lr_draw_item item;
    unsigned char *px = NULL;
    lc_image *img = NULL;
    int presented = 0;
    int guard = 0;

    while (!presented) {
        if (++guard > 120) {
            return NULL;
        }
        lc_poll_events();
        res = lc_begin_frame(swapchain);
        if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            continue;
        }
        if (res != LC_SUCCESS ||
            lc_swapchain_get_encoder(swapchain, &enc) !=
                LC_SUCCESS) {
            return NULL;
        }
        if (lr_renderer_begin(renderer, camera) != LR_SUCCESS) {
            return NULL;
        }
        memset(&item, 0, sizeof(item));
        lr_transform_identity(&item.transform);
        item.mesh = mesh;
        item.material = mat;
        item.skin_palette = palette;
        item.skin_joint_count = joints;
        if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
            return NULL;
        }
        if (gpu_mode) {
            /* Public prepare hook (no open pass): with only a
             * skinned item queued, no GPU group forms; the CPU
             * loop below still draws it. */
            if (lr_renderer_prepare_gpu(renderer, enc) !=
                LR_SUCCESS) {
                return NULL;
            }
        }
        memset(&catt, 0, sizeof(catt));
        catt.view = off_view;
        catt.load_op = LC_LOAD_OP_CLEAR;
        catt.store_op = LC_STORE_OP_STORE;
        catt.clear_color[0] = 0.02f;
        catt.clear_color[1] = 0.02f;
        catt.clear_color[2] = 0.03f;
        catt.clear_color[3] = 1.0f;
        memset(&datt, 0, sizeof(datt));
        datt.view = off_dview;
        datt.depth_load_op = LC_LOAD_OP_CLEAR;
        datt.depth_store_op = LC_STORE_OP_STORE;
        datt.clear_depth = 1.0f;
        datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
        datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
        memset(&pdesc, 0, sizeof(pdesc));
        pdesc.color_attachments = &catt;
        pdesc.color_attachment_count = 1;
        pdesc.depth_attachment = &datt;
        pdesc.width = FW;
        pdesc.height = FH;
        if (lc_encoder_begin_render_pass(enc, &pdesc) !=
            LC_SUCCESS) {
            return NULL;
        }
        if (lr_renderer_render(renderer, enc, offscreen) !=
            LR_SUCCESS) {
            lc_encoder_end_render_pass(enc);
            return NULL;
        }
        if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            return NULL;
        }
        if (out_stats != NULL) {
            lr_renderer_get_stats(renderer, out_stats);
        }
        lr_renderer_end(renderer);
        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.depth_load_op = LC_LOAD_OP_CLEAR;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        if (lc_encoder_begin_swapchain_pass(enc, swapchain,
                                            &spass) != LC_SUCCESS ||
            lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            return NULL;
        }
        res = lc_end_frame(swapchain);
        if (res == LC_SUCCESS || res == LC_SUBOPTIMAL) {
            presented = 1;
        } else if (res != LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            return NULL;
        }
    }
    img = lc_image_view_get_image(off_view);
    if (img == NULL) {
        return NULL;
    }
    px = readback_rgba8(device, img);
    return px;
}

int main(void) {
    lc_device *device = NULL;
    lc_window *window = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lr_renderer *renderer = NULL;
    lr_mesh *rigid = NULL;
    lr_mesh *skinned = NULL;
    lr_material *red = NULL;
    lr_camera camera;
    lc_image *off_img = NULL;
    lc_image_view *off_view = NULL;
    lc_image *off_depth = NULL;
    lc_image_view *off_dview = NULL;
    lc_render_target *offscreen = NULL;
    lc_device_desc ddesc;
    lc_window_desc wdesc;
    lr_vertex verts[4];
    lr_mesh_desc mdesc;
    int m;

    static const uint32_t quad_idx[6] = { 0, 1, 2, 0, 2, 3 };
    /* Two-joint palettes: [joint0, joint1]. */
    float ident[2][16];
    float moved[2][16];

    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.backend = LC_BACKEND_VULKAN;
    ddesc.enable_validation = 1;
    switch (lc_device_create(&ddesc, &device)) {
    case LC_SUCCESS:
        break;
    case LC_ERROR_BACKEND_UNAVAILABLE:
    case LC_ERROR_NO_SUPPORTED_DEVICE:
        SKIP_ENV("Vulkan device");
    default:
        printf("device failed: FAIL\n");
        lc_shutdown();
        return 1;
    }
    memset(&wdesc, 0, sizeof(wdesc));
    wdesc.title = "LumaC Skin Test";
    wdesc.width = 320;
    wdesc.height = 320;
    switch (lc_window_create(&wdesc, &window)) {
    case LC_SUCCESS:
        break;
    case LC_ERROR_PLATFORM:
    case LC_ERROR_WINDOW_CREATION_FAILED:
        lc_device_destroy(device);
        SKIP_ENV("window");
    default:
        printf("window failed: FAIL\n");
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    switch (lc_surface_create(device, window, &surface)) {
    case LC_SUCCESS:
        break;
    case LC_ERROR_SURFACE_UNSUPPORTED:
        lc_window_destroy(window);
        lc_device_destroy(device);
        SKIP_ENV("surface");
    default:
        printf("surface failed: FAIL\n");
        lc_window_destroy(window);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    {
        lc_swapchain_desc sdesc;

        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.width = 320;
        sdesc.height = 320;
        sdesc.image_count = 0;
        sdesc.vsync = 1;
        switch (lc_swapchain_create(device, surface, &sdesc,
                                    &swapchain)) {
        case LC_SUCCESS:
            break;
        case LC_ERROR_SWAPCHAIN_UNSUPPORTED:
        case LC_ERROR_ZERO_EXTENT:
            lc_surface_destroy(surface);
            lc_window_destroy(window);
            lc_device_destroy(device);
            SKIP_ENV("swapchain");
        default:
            printf("swapchain failed: FAIL\n");
            lc_surface_destroy(surface);
            lc_window_destroy(window);
            lc_device_destroy(device);
            lc_shutdown();
            return 1;
        }
    }

    identity_mat(ident[0]);
    identity_mat(ident[1]);
    identity_mat(moved[0]);
    translate_mat(moved[1], 0.5f, 0.0f, 0.0f);

    for (m = 0; m < 2; m++) {
        lr_renderer_desc rdesc;
        lc_image_desc idesc;
        lc_image_view_desc vdesc;
        lc_render_target_create_desc tdesc;
        lc_render_target_attachment att;
        lr_unlit_material_desc matdesc;
        unsigned char *px_rigid = NULL;
        unsigned char *px_bind = NULL;
        unsigned char *px_moved = NULL;
        uint32_t lit_rigid = 0;
        uint32_t lit_bind = 0;
        uint32_t lit_moved = 0;
        double cx_bind = -1.0;
        double cx_moved = -1.0;
        lr_render_stats st_bind;
        lr_render_stats st_extra;
        uint32_t diff = 0;
        uint32_t i;
        uint64_t bytes = (uint64_t)FW * FH * 4u;
        int gpu_mode = (m == 1);
        const char *tag = gpu_mode ? "GPU mode" : "CPU mode";

        memset(&rdesc, 0, sizeof(rdesc));
        rdesc.device = device;
        rdesc.max_objects = 16;
        rdesc.render_target.color_attachment_count = 1;
        rdesc.render_target.color_formats[0] =
            LC_FORMAT_RGBA8_UNORM;
        rdesc.render_target.depth_stencil_format =
            LC_FORMAT_D32_FLOAT;
        rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
        if (lr_renderer_create(&rdesc, &renderer) != LR_SUCCESS) {
            printf("renderer create failed: FAIL\n");
            goto fail;
        }
        /* Rigid quad: every vertex uses the rigid convention. */
        skin_vertex(&verts[0], -0.5f, -0.5f, 0);
        skin_vertex(&verts[1], 0.5f, -0.5f, 0);
        skin_vertex(&verts[2], 0.5f, 0.5f, 0);
        skin_vertex(&verts[3], -0.5f, 0.5f, 0);
        memset(&mdesc, 0, sizeof(mdesc));
        mdesc.vertices = verts;
        mdesc.vertex_count = 4;
        mdesc.indices = quad_idx;
        mdesc.index_count = 6;
        if (lr_mesh_create(renderer, &mdesc, &rigid) !=
            LR_SUCCESS) {
            printf("rigid mesh failed: FAIL\n");
            goto fail;
        }
        /* Skinned quad: identical positions, 100% joint 1. */
        skin_vertex(&verts[0], -0.5f, -0.5f, 1);
        skin_vertex(&verts[1], 0.5f, -0.5f, 1);
        skin_vertex(&verts[2], 0.5f, 0.5f, 1);
        skin_vertex(&verts[3], -0.5f, 0.5f, 1);
        mdesc.vertices = verts;
        if (lr_mesh_create(renderer, &mdesc, &skinned) !=
            LR_SUCCESS) {
            printf("skinned mesh failed: FAIL\n");
            goto fail;
        }
        TEST_CHECK(!lr_mesh_is_skinned(rigid),
                   (m == 0) ? "rigid quad not skinned (CPU mode)"
                            : "rigid quad not skinned (GPU mode)");
        TEST_CHECK(lr_mesh_is_skinned(skinned),
                   (m == 0) ? "skinned quad flagged (CPU mode)"
                            : "skinned quad flagged (GPU mode)");
        TEST_CHECK(!lr_mesh_is_skinned(NULL), "NULL mesh not skinned");
        memset(&matdesc, 0, sizeof(matdesc));
        matdesc.color[0] = 0.9f;
        matdesc.color[1] = 0.15f;
        matdesc.color[2] = 0.15f;
        matdesc.color[3] = 1.0f;
        if (lr_material_create_unlit(renderer, &matdesc, &red) !=
            LR_SUCCESS) {
            printf("material failed: FAIL\n");
            goto fail;
        }
        if (gpu_mode &&
            lr_renderer_set_render_mode(
                renderer, LR_RENDER_MODE_GPU_DRIVEN) != LR_SUCCESS) {
            printf("gpu-driven mode failed: FAIL\n");
            goto fail;
        }
        lr_camera_init(&camera);
        {
            static const float eye[3] = { 0.0f, 0.0f, 3.0f };
            static const float center[3] = { 0.0f, 0.0f, 0.0f };
            static const float up[3] = { 0.0f, 1.0f, 0.0f };

            if (lr_camera_set_perspective(&camera, 0.7853982f, 1.0f,
                                          0.1f, 100.0f) !=
                    LR_SUCCESS ||
                lr_camera_look_at(&camera, eye, center, up) !=
                    LR_SUCCESS) {
                printf("camera failed: FAIL\n");
                goto fail;
            }
        }
        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_RGBA8_UNORM;
        idesc.width = FW;
        idesc.height = FH;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                      LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                      LC_IMAGE_USAGE_TRANSFER_SRC |
                      LC_IMAGE_USAGE_TRANSFER_DST;
        idesc.samples = LC_SAMPLE_COUNT_1;
        if (lc_image_create(device, &idesc, &off_img) !=
            LC_SUCCESS) {
            printf("offscreen image failed: FAIL\n");
            goto fail;
        }
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.mip_level_count = 1;
        vdesc.array_layer_count = 1;
        if (lc_image_view_create(off_img, &vdesc, &off_view) !=
            LC_SUCCESS) {
            printf("offscreen view failed: FAIL\n");
            goto fail;
        }
        idesc.format = LC_FORMAT_D32_FLOAT;
        idesc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL;
        if (lc_image_create(device, &idesc, &off_depth) !=
            LC_SUCCESS) {
            printf("depth image failed: FAIL\n");
            goto fail;
        }
        vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
        if (lc_image_view_create(off_depth, &vdesc, &off_dview) !=
            LC_SUCCESS) {
            printf("depth view failed: FAIL\n");
            goto fail;
        }
        memset(&tdesc, 0, sizeof(tdesc));
        tdesc.width = FW;
        tdesc.height = FH;
        att.view = off_view;
        tdesc.color_attachments = &att;
        tdesc.color_attachment_count = 1;
        tdesc.depth_stencil_attachment = off_dview;
        if (lc_render_target_create(device, &tdesc, &offscreen) !=
            LC_SUCCESS) {
            printf("offscreen target failed: FAIL\n");
            goto fail;
        }

        /* Leg 1: rigid reference (no palette). */
        memset(&st_extra, 0, sizeof(st_extra));
        px_rigid = render_one(device, swapchain, renderer, &camera,
                              offscreen, off_view, off_dview, rigid,
                              red, NULL, 0, gpu_mode, &st_extra);
        if (px_rigid == NULL) {
            printf("rigid leg failed: FAIL\n");
            goto fail;
        }
        /* Leg 2: skinned bind pose (identity palette). */
        memset(&st_bind, 0, sizeof(st_bind));
        px_bind = render_one(device, swapchain, renderer, &camera,
                             offscreen, off_view, off_dview, skinned,
                             red, ident, 2, gpu_mode, &st_bind);
        if (px_bind == NULL) {
            printf("bind leg failed: FAIL\n");
            goto fail;
        }
        /* Leg 3: skinned moved pose (joint 1 -> +X 0.5). */
        memset(&st_extra, 0, sizeof(st_extra));
        px_moved = render_one(device, swapchain, renderer, &camera,
                              offscreen, off_view, off_dview,
                              skinned, red, moved, 2, gpu_mode,
                              &st_extra);
        if (px_moved == NULL) {
            printf("moved leg failed: FAIL\n");
            goto fail;
        }
        analyze(px_rigid, &lit_rigid, &cx_bind);
        analyze(px_bind, &lit_bind, &cx_bind);
        analyze(px_moved, &lit_moved, &cx_moved);
        /* Regression: bind pose matches rigid (exact bytes up to
         * float rounding — allow a small Hamming budget). */
        for (i = 0; i < bytes; i++) {
            if (px_rigid[i] != px_bind[i]) {
                diff++;
            }
        }
        {
            char msg[128];

            snprintf(msg, sizeof(msg),
                     "bind pose matches rigid (%s, diff=%u lit=%u/%u)",
                     tag, diff, lit_rigid, lit_bind);
            TEST_CHECK(lit_rigid > 2000u && lit_bind > 2000u &&
                           diff < 256u,
                       msg);
        }
        /* Oracle: the translated joint moves pixels right. The
         * quad is 1 unit wide at depth 3 with a 45-degree FOV:
         * +0.5 local X must shift the lit centroid by tens of
         * screen pixels. */
        {
            char msg[128];

            snprintf(msg, sizeof(msg),
                     "translated joint moves pixels (%s, cx %.1f->%.1f)",
                     tag, cx_bind, cx_moved);
            TEST_CHECK(lit_moved > 2000u && cx_moved > cx_bind + 8.0,
                       msg);
        }
        /* Stats observe the skinned bind draw. */
        {
            char msg[128];

            snprintf(msg, sizeof(msg),
                     "stats observe skinned draw (%s, skinned=%u tris=%u)",
                     tag, st_bind.skinned_draw_calls,
                     st_bind.skinned_triangles);
            TEST_CHECK(st_bind.skinned_draw_calls == 1 &&
                           st_bind.skinned_triangles == 2,
                       msg);
        }
        free(px_rigid);
        free(px_bind);
        free(px_moved);
        px_rigid = NULL;
        px_bind = NULL;
        px_moved = NULL;
        test_wait_idle(device);
        lr_material_destroy(red);
        red = NULL;
        lr_mesh_destroy(skinned);
        skinned = NULL;
        lr_mesh_destroy(rigid);
        rigid = NULL;
        lc_render_target_destroy(offscreen);
        offscreen = NULL;
        lc_image_view_destroy(off_dview);
        off_dview = NULL;
        lc_image_view_destroy(off_view);
        off_view = NULL;
        lc_image_destroy(off_depth);
        off_depth = NULL;
        lc_image_destroy(off_img);
        off_img = NULL;
        lr_renderer_destroy(renderer);
        renderer = NULL;
        continue;
    fail:
        test_wait_idle(device);
        free(px_rigid);
        free(px_bind);
        free(px_moved);
        if (red != NULL) {
            lr_material_destroy(red);
            red = NULL;
        }
        if (skinned != NULL) {
            lr_mesh_destroy(skinned);
            skinned = NULL;
        }
        if (rigid != NULL) {
            lr_mesh_destroy(rigid);
            rigid = NULL;
        }
        if (offscreen != NULL) {
            lc_render_target_destroy(offscreen);
            offscreen = NULL;
        }
        if (off_dview != NULL) {
            lc_image_view_destroy(off_dview);
            off_dview = NULL;
        }
        if (off_view != NULL) {
            lc_image_view_destroy(off_view);
            off_view = NULL;
        }
        if (off_depth != NULL) {
            lc_image_destroy(off_depth);
            off_depth = NULL;
        }
        if (off_img != NULL) {
            lc_image_destroy(off_img);
            off_img = NULL;
        }
        if (renderer != NULL) {
            lr_renderer_destroy(renderer);
            renderer = NULL;
        }
        printf("TESTS FAILED\n");
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_window_destroy(window);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    TEST_CHECK(1, "both CPU and GPU modes rendered");
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_window_destroy(window);
    lc_device_destroy(device);
    lc_shutdown();
    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
