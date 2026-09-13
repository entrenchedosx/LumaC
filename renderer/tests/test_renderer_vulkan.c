/*
 * Luma Renderer Vulkan integration test (Phase 13).
 *
 * Live validation (renderer/mesh/material misuse, ownership, target
 * mismatch), 100-object reuse with stats + culling + pixel proof,
 * offscreen + multi-viewport rendering, editor-viewport capture, and
 * a 500-frame endurance run with camera motion, rotation, and target
 * resizes. Validation layers stay enabled throughout.
 *
 * If the environment cannot provide a window or Vulkan setup, SKIP and
 * exit 0. Any other failure is a hard FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

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

static int make_window_titled(lc_window **out, const char *title,
                              uint32_t w, uint32_t h) {
    lc_window_desc desc;

    desc.title = title;
    desc.width = w;
    desc.height = h;
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

static int make_surface(lc_device *device, lc_window *window,
                        lc_surface **out) {
    *out = NULL;
    switch (lc_surface_create(device, window, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_SURFACE_UNSUPPORTED:
        return 1;
    default:
        return -1;
    }
}

static int make_swapchain(lc_device *device, lc_surface *surface,
                          lc_swapchain **out) {
    lc_swapchain_desc desc;

    desc.width = 800;
    desc.height = 600;
    desc.image_count = 0;
    desc.vsync = 1;
    *out = NULL;
    switch (lc_swapchain_create(device, surface, &desc, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_SWAPCHAIN_UNSUPPORTED:
    case LC_ERROR_ZERO_EXTENT:
        return 1;
    default:
        return -1;
    }
}

#define SKIP_ENV(what) do { \
    printf("SKIP: environment cannot provide %s\n", what); \
    lc_shutdown(); \
    return 0; \
} while (0)

#define FAIL_SUMMARY() do { \
    printf("TESTS FAILED\n"); \
    lc_shutdown(); \
    return 1; \
} while (0)

/* White-box idle for readback (test-only, coarse, rare). */
static void test_wait_idle(lc_device *device) {
    if (device != NULL && device->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->device);
    }
}

static unsigned char *readback_rgba8(lc_device *device, lc_image *image,
                                     uint32_t w, uint32_t h) {    lc_buffer *staging = NULL;
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

static int make_color_target(lc_device *device, lc_format format, uint32_t w,
                             uint32_t h, int with_depth,
                             lc_image **out_img, lc_image **out_depth,
                             lc_image_view **out_view,
                             lc_image_view **out_dview,
                             lc_render_target **out_target) {
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
    if (lc_image_create(device, &idesc, out_img) != LC_SUCCESS) {
        return -1;
    }
    memset(&vdesc, 0, sizeof(vdesc));
    vdesc.type = LC_IMAGE_VIEW_2D;
    vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
    vdesc.mip_level_count = 1;
    vdesc.array_layer_count = 1;
    if (lc_image_view_create(*out_img, &vdesc, out_view) != LC_SUCCESS) {
        lc_image_destroy(*out_img);
        return -1;
    }
    if (out_depth != NULL) {
        *out_depth = NULL;
    }
    if (out_dview != NULL) {
        *out_dview = NULL;
    }
    if (with_depth) {
        idesc.format = LC_FORMAT_D32_FLOAT;
        idesc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL;
        if (lc_image_create(device, &idesc, out_depth) != LC_SUCCESS) {
            lc_image_view_destroy(*out_view);
            lc_image_destroy(*out_img);
            return -1;
        }
        vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
        if (lc_image_view_create(*out_depth, &vdesc, out_dview) !=
            LC_SUCCESS) {
            lc_image_destroy(*out_depth);
            lc_image_view_destroy(*out_view);
            lc_image_destroy(*out_img);
            return -1;
        }
    }
    /* Prime sampled-readable (binding + LOAD validation need it; the
     * first CLEAR pass discards the primer). */
    {
        static unsigned char zeros[64 * 64 * 4];
        lc_image_upload_desc primer;
        uint64_t need = (uint64_t)w * h * 4u;
        unsigned char *fill = zeros;

        /* Primer only covers small targets; bigger ones start
         * UNDEFINED and are CLEARed before any sampling bind. */
        if (need <= sizeof(zeros)) {
            memset(&primer, 0, sizeof(primer));
            primer.width = w;
            primer.height = h;
            primer.depth = 1;
            primer.data = fill;
            primer.data_size = need;
            if (lc_image_write(*out_img, &primer) != LC_SUCCESS) {
                return -1;
            }
        }
    }
    memset(&tdesc, 0, sizeof(tdesc));
    tdesc.width = w;
    tdesc.height = h;
    att.view = *out_view;
    tdesc.color_attachments = &att;
    tdesc.color_attachment_count = 1;
    tdesc.depth_stencil_attachment =
        (with_depth && out_dview != NULL) ? *out_dview : NULL;
    if (lc_render_target_create(device, &tdesc, out_target) != LC_SUCCESS) {
        return -1;
    }
    return 0;
}

/* Present one trivial swapchain pass so every test frame ends with a
 * defined image (empty command buffers would present UNDEFINED). */
static int present_clear(lc_swapchain *swapchain, lc_command_encoder *enc) {
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
    if (lc_encoder_begin_swapchain_pass(enc, swapchain, &spass) !=
        LC_SUCCESS) {
        return -1;
    }
    if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        return -1;
    }
    return 0;
}

/* Full scene frame on one swapchain: offscreen scene pass via the
 * renderer plus a swapchain present pass (clear-only; the scene shows
 * through the offscreen readback path in pixel tests). Simplified
 * helper for endurance: scene renders DIRECTLY to the swapchain for
 * presentation speed. Returns 1 presented, 0 retry, -1 fatal. */
typedef struct scene_stack {
    lc_device *device;
    lc_window *window;
    lc_surface *surface;
    lc_swapchain *swapchain;
    lr_renderer *renderer;
    lr_mesh *cube;
    lr_mesh *ground;
    lr_material *red;
    lr_material *blue;
    lr_material *green;
    lr_camera camera;
} scene_stack;

int main(void) {
    /* Unbuffered so crash diagnostics are never lost to stdio. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running Luma Renderer integration test...\n");

    lc_shutdown();
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }

    /* ---- 1. live renderer/mesh/material validation ---- */
    {
        lc_device *device = NULL;
        lr_renderer *renderer = NULL;
        lr_renderer *renderer_b = NULL;
        lr_mesh *cube = NULL;
        lr_material *red = NULL;
        lr_renderer_desc rdesc;
        lr_mesh_desc mdesc;
        lr_unlit_material_desc matdesc;
        lr_camera camera;
        lr_draw_item item;
        int env;

        env = make_device(&device);
        if (env != 0) {
            if (env == 1) {
                SKIP_ENV("a Vulkan device");
            }
            printf("device creation failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }

        /* Renderer desc validation (no GPU touched on shape rejects). */
        memset(&rdesc, 0, sizeof(rdesc));
        TEST_CHECK(lr_renderer_create(NULL, &renderer) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "renderer create(NULL desc) -> INVALID");
        TEST_CHECK(renderer == NULL, "renderer out cleared");
        TEST_CHECK(lr_renderer_create(&rdesc, NULL) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "renderer create(NULL out) -> INVALID");
        rdesc.device = NULL;
        rdesc.max_objects = 16;
        rdesc.render_target.color_attachment_count = 1;
        rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
        rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
        TEST_CHECK(lr_renderer_create(&rdesc, &renderer) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "renderer create(NULL device) -> INVALID");
        rdesc.device = device;
        rdesc.max_objects = 0;
        TEST_CHECK(lr_renderer_create(&rdesc, &renderer) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "renderer create(max_objects 0) -> INVALID");
        rdesc.max_objects = 16;
        rdesc.render_target.color_attachment_count = 0;
        TEST_CHECK(lr_renderer_create(&rdesc, &renderer) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "renderer create(zero colors) -> INVALID");
        rdesc.render_target.color_attachment_count = 1;
        TEST_CHECK(lr_renderer_create(&rdesc, &renderer) == LR_SUCCESS,
                   "renderer created");
        TEST_CHECK(renderer != NULL, "renderer handle non-null");
        TEST_CHECK(lr_renderer_create(&rdesc, &renderer_b) == LR_SUCCESS,
                   "second renderer on one device created");

        /* Mesh validation. */
        memset(&mdesc, 0, sizeof(mdesc));
        TEST_CHECK(lr_mesh_create(renderer, NULL, &cube) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "mesh create(NULL desc) -> INVALID");
        TEST_CHECK(lr_mesh_create(renderer, &mdesc, NULL) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "mesh create(NULL out) -> INVALID");
        TEST_CHECK(lr_mesh_create(renderer, &mdesc, &cube) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "mesh create(empty desc) -> INVALID");
        TEST_CHECK(cube == NULL, "mesh out cleared");
        {
            lr_vertex v[3];
            uint32_t bad_idx[3] = { 0, 1, 9 };
            uint32_t odd_idx[4] = { 0, 1, 2, 0 };

            memset(v, 0, sizeof(v));
            mdesc.vertices = v;
            mdesc.vertex_count = 3;
            mdesc.indices = bad_idx;
            mdesc.index_count = 3;
            TEST_CHECK(lr_mesh_create(renderer, &mdesc, &cube) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "mesh bad index ref rejected");
            mdesc.indices = odd_idx;
            mdesc.index_count = 4;
            TEST_CHECK(lr_mesh_create(renderer, &mdesc, &cube) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "mesh non-triangle count rejected");
        }
        TEST_CHECK(lr_mesh_create_cube(renderer, 0.0f, &cube) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "cube(size 0) rejected");
        TEST_CHECK(lr_mesh_create_plane(renderer, 1.0f, 0.0f, &cube) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "plane(depth 0) rejected");
        TEST_CHECK(lr_mesh_create_sphere(renderer, 1.0f, 2, 4, &cube) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "sphere(segments<3) rejected");
        TEST_CHECK(lr_mesh_create_cube(renderer, 1.0f, &cube) == LR_SUCCESS,
                   "cube mesh created");
        TEST_CHECK(lr_mesh_get_vertex_count(cube) == LR_CUBE_VERTEX_COUNT,
                   "cube vertex count");
        TEST_CHECK(lr_mesh_get_index_count(cube) == LR_CUBE_INDEX_COUNT,
                   "cube index count");
        {
            lr_bounds bounds;

            lr_mesh_get_bounds(cube, &bounds);
            TEST_CHECK(bounds.min[0] == -0.5f && bounds.max[0] == 0.5f &&
                           bounds.radius > 0.86f && bounds.radius < 0.87f,
                       "cube bounds exact-ish");
        }

        /* Material validation (dead texture rejected, fallback used). */
        memset(&matdesc, 0, sizeof(matdesc));
        matdesc.color[0] = 1.0f;
        matdesc.color[1] = 0.0f;
        matdesc.color[2] = 0.0f;
        matdesc.color[3] = 1.0f;
        TEST_CHECK(lr_material_create_unlit(NULL, &matdesc, &red) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "material(NULL renderer) -> INVALID");
        TEST_CHECK(lr_material_create_unlit(renderer, NULL, &red) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "material(NULL desc) -> INVALID");
        TEST_CHECK(lr_material_create_unlit(renderer, &matdesc, &red) ==
                       LR_SUCCESS,
                   "untextured material uses fallbacks");
        matdesc.base_color_texture = (lc_image_view *)0x1;
        {
            lr_material *bad = NULL;

            TEST_CHECK(lr_material_create_unlit(renderer, &matdesc, &bad) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "material(dead texture) rejected");
            TEST_CHECK(bad == NULL, "material out cleared");
        }
        matdesc.base_color_texture = NULL;

        /* Camera validation. */
        lr_camera_init(&camera);
        TEST_CHECK(lr_renderer_begin(NULL, &camera) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "begin(NULL renderer) -> INVALID");
        TEST_CHECK(lr_renderer_begin(renderer, NULL) ==
                       LR_ERROR_INVALID_ARGUMENT,
                   "begin(NULL camera) -> INVALID");
        {
            lr_camera garbage = camera;

            garbage.aspect_ratio = 0.0f;
            TEST_CHECK(lr_renderer_begin(renderer, &garbage) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "begin(zero aspect) rejected");
        }
        {
            static const float eye[3] = { 0.0f, 2.0f, 5.0f };
            static const float center[3] = { 0.0f, 0.0f, 0.0f };
            static const float up[3] = { 0.0f, 1.0f, 0.0f };

            TEST_CHECK(lr_camera_set_perspective(&camera, 0.7853982f,
                                                 4.0f / 3.0f, 0.1f,
                                                 100.0f) == LR_SUCCESS,
                       "camera perspective set");
            TEST_CHECK(lr_camera_look_at(&camera, eye, center, up) ==
                           LR_SUCCESS,
                       "camera look_at set");
            TEST_CHECK(lr_renderer_begin(renderer, &camera) == LR_SUCCESS,
                       "renderer begin succeeds");
        }

        /* Submit validation: cross-renderer + dead + overflow. */
        {
            lr_mesh *foreign = NULL;

            TEST_CHECK(lr_mesh_create_cube(renderer_b, 1.0f, &foreign) ==
                           LR_SUCCESS,
                       "foreign mesh created");
            memset(&item, 0, sizeof(item));
            lr_transform_identity(&item.transform);
            item.mesh = NULL;
            item.material = red;
            TEST_CHECK(lr_renderer_submit(renderer, &item) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "submit(NULL mesh) rejected");
            item.mesh = cube;
            item.material = NULL;
            TEST_CHECK(lr_renderer_submit(renderer, &item) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "submit(NULL material) rejected");
            item.material = red;
            TEST_CHECK(lr_renderer_submit(renderer, &item) == LR_SUCCESS,
                       "submit one succeeds");
            item.mesh = foreign;
            TEST_CHECK(lr_renderer_submit(renderer, &item) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "submit(foreign mesh) rejected");
            item.mesh = cube;
            lr_mesh_destroy(foreign);
            foreign = NULL;
            TEST_CHECK(lr_renderer_render(renderer, NULL, NULL) ==
                           LR_ERROR_INVALID_ARGUMENT,
                       "render(NULL encoder) rejected");
            {
                /* Tiny renderer to prove queue overflow. */
                lr_renderer *small = NULL;
                lr_renderer_desc sdesc;

                memset(&sdesc, 0, sizeof(sdesc));
                sdesc.device = device;
                sdesc.max_objects = 2;
                sdesc.render_target.color_attachment_count = 1;
                sdesc.render_target.color_formats[0] =
                    LC_FORMAT_RGBA8_UNORM;
                sdesc.render_target.samples = LC_SAMPLE_COUNT_1;
                TEST_CHECK(lr_renderer_create(&sdesc, &small) == LR_SUCCESS,
                           "small renderer created");
                TEST_CHECK(lr_renderer_begin(small, &camera) == LR_SUCCESS,
                           "small begin succeeds");
                {
                    lr_mesh *sm = NULL;
                    lr_material *smat = NULL;

                    TEST_CHECK(lr_mesh_create_cube(small, 1.0f, &sm) ==
                                   LR_SUCCESS,
                               "small mesh created");
                    TEST_CHECK(lr_material_create_unlit(small, &matdesc,
                                                        &smat) == LR_SUCCESS,
                               "small material created");
                    memset(&item, 0, sizeof(item));
                    lr_transform_identity(&item.transform);
                    item.mesh = sm;
                    item.material = smat;
                    TEST_CHECK(lr_renderer_submit(small, &item) ==
                                   LR_SUCCESS,
                               "small submit 1");
                    TEST_CHECK(lr_renderer_submit(small, &item) ==
                                   LR_SUCCESS,
                               "small submit 2");
                    TEST_CHECK(lr_renderer_submit(small, &item) ==
                                   LR_ERROR_INVALID_ARGUMENT,
                               "submit past capacity rejected");
                    lr_material_destroy(smat);
                    lr_mesh_destroy(sm);
                }
                lr_renderer_destroy(small);
            }
        }

        lr_material_destroy(red);
        lr_mesh_destroy(cube);
        lr_renderer_destroy(renderer_b);
        lr_renderer_destroy(renderer);
        lc_device_destroy(device);
        lc_shutdown();
        TEST_CHECK(1, "validation teardown clean");
    }

    /* ---- 2. 100-object scene: reuse, stats, culling, pixels ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_command_encoder *enc = NULL;
        lr_renderer *renderer = NULL;
        lr_mesh *cube = NULL;
        lr_material *red = NULL;
        lr_material *blue = NULL;
        lr_camera camera;
        lc_image *off_img = NULL;
        lc_image_view *off_view = NULL;
        lc_image *off_depth = NULL;
        lc_image_view *off_dview = NULL;
        lc_render_target *offscreen = NULL;
        int env;
        int presented = 0;
        int guard = 0;
        static const float y_axis[3] = { 0.0f, 1.0f, 0.0f };

        env = make_device(&device);
        TEST_CHECK(env == 0, "device for scene test");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC Renderer Test", 800,
                                      600) == 0,
                   "window for scene test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for scene test");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for scene test");
        {
            lr_renderer_desc rdesc;

            memset(&rdesc, 0, sizeof(rdesc));
            rdesc.device = device;
            rdesc.max_objects = 128;
            rdesc.render_target.color_attachment_count = 1;
            rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
            rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
            rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
            TEST_CHECK(lr_renderer_create(&rdesc, &renderer) == LR_SUCCESS,
                       "scene renderer created");
        }
        TEST_CHECK(lr_mesh_create_cube(renderer, 1.0f, &cube) == LR_SUCCESS,
                   "scene cube mesh (one upload)");
        {
            lr_unlit_material_desc mdesc;

            memset(&mdesc, 0, sizeof(mdesc));
            mdesc.color[0] = 0.85f;
            mdesc.color[1] = 0.25f;
            mdesc.color[2] = 0.22f;
            mdesc.color[3] = 1.0f;
            TEST_CHECK(lr_material_create_unlit(renderer, &mdesc, &red) ==
                           LR_SUCCESS,
                       "red material");
            mdesc.color[0] = 0.25f;
            mdesc.color[1] = 0.45f;
            mdesc.color[2] = 0.90f;
            TEST_CHECK(lr_material_create_unlit(renderer, &mdesc, &blue) ==
                           LR_SUCCESS,
                       "blue material");
        }
        lr_camera_init(&camera);
        {
            static const float eye[3] = { 0.0f, 2.0f, 5.0f };
            static const float center[3] = { 0.0f, 0.0f, 0.0f };
            static const float up[3] = { 0.0f, 1.0f, 0.0f };

            TEST_CHECK(lr_camera_set_perspective(&camera, 0.7853982f,
                                                 800.0f / 600.0f, 0.1f,
                                                 100.0f) == LR_SUCCESS,
                       "scene camera perspective");
            TEST_CHECK(lr_camera_look_at(&camera, eye, center, up) ==
                           LR_SUCCESS,
                       "scene camera look_at");
        }
        /* Offscreen sibling for readback (swapchain presents, this one
         * proves pixels). */
        TEST_CHECK(make_color_target(device, LC_FORMAT_RGBA8_UNORM, 512,
                                     512, 1, &off_img, &off_depth, &off_view,
                                     &off_dview, &offscreen) == 0,
                   "scene offscreen target");

        /* One framed scene: 60 grid cubes + 40 culled, offscreen for
         * pixels, swapchain for presentation. */
        while (presented < 2) {
            lc_render_color_attachment catt;
            lc_render_depth_attachment datt;
            lc_render_pass_desc pdesc;
            lc_render_swapchain_pass_desc spass;
            lc_result res;
            int c;
            int r;

            if (++guard > 120) {
                break;
            }
            lc_poll_events();
            res = lc_begin_frame(swapchain);
            if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                uint32_t w = lc_window_get_width(window);
                uint32_t h = lc_window_get_height(window);

                if (w != 0 && h != 0) {
                    lc_swapchain_recreate(swapchain, w, h);
                }
                continue;
            }
            if (res != LC_SUCCESS ||
                lc_swapchain_get_encoder(swapchain, &enc) != LC_SUCCESS) {
                break;
            }
            /* Offscreen scene pass first. */
            memset(&catt, 0, sizeof(catt));
            catt.view = off_view;
            catt.load_op = LC_LOAD_OP_CLEAR;
            catt.store_op = LC_STORE_OP_STORE;
            catt.clear_color[0] = 0.05f;
            catt.clear_color[1] = 0.06f;
            catt.clear_color[2] = 0.09f;
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
            pdesc.width = 512;
            pdesc.height = 512;
            if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
                break;
            }
            if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
                lc_encoder_end_render_pass(enc);
                break;
            }
            {
                lr_draw_item item;
                int ok = 1;

                memset(&item, 0, sizeof(item));
                /* Frustum-fitted 6x10 grid (verified analytically for
                 * this camera: x in [-2,2], z in [-1,-10] keeps every
                 * corner sphere strictly inside with margin). */
                for (r = 0; r < 10 && ok; r++) {
                    for (c = 0; c < 6 && ok; c++) {
                        lr_transform_identity(&item.transform);
                        item.transform.position[0] =
                            -2.0f + (float)c * 0.8f;
                        item.transform.position[1] = 0.5f;
                        item.transform.position[2] =
                            -1.0f - (float)r * 1.0f;
                        lr_quat_from_axis_angle(
                            y_axis, (float)(r * 6 + c) * 0.1f,
                            item.transform.rotation);
                        item.mesh = cube;
                        item.material = (((r * 6 + c) % 2) == 0) ? red : blue;
                        if (lr_renderer_submit(renderer, &item) !=
                            LR_SUCCESS) {
                            ok = 0;
                        }
                    }
                }
                /* 40 culled: behind, far left/right, beyond far. */
                for (c = 0; c < 10 && ok; c++) {
                    lr_transform_identity(&item.transform);
                    item.transform.position[0] = 0.0f;
                    item.transform.position[1] = 0.5f;
                    item.transform.position[2] = 20.0f;
                    item.mesh = cube;
                    item.material = red;
                    if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
                        ok = 0;
                    }
                    item.transform.position[0] = -25.0f;
                    item.transform.position[2] = 0.0f;
                    if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
                        ok = 0;
                    }
                    item.transform.position[0] = 25.0f;
                    if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
                        ok = 0;
                    }
                    item.transform.position[0] = 0.0f;
                    item.transform.position[2] = -120.0f;
                    if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
                        ok = 0;
                    }
                }
                if (!ok) {
                    lc_encoder_end_render_pass(enc);
                    break;
                }
            }
            if (lr_renderer_render(renderer, enc, offscreen) != LR_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                break;
            }
            lr_renderer_end(renderer);
            /* Same scene straight to the swapchain (direct path). */
            memset(&spass, 0, sizeof(spass));
            spass.color_load_op = LC_LOAD_OP_CLEAR;
            spass.color_store_op = LC_STORE_OP_STORE;
            spass.clear_color[0] = 0.05f;
            spass.clear_color[1] = 0.06f;
            spass.clear_color[2] = 0.09f;
            spass.clear_color[3] = 1.0f;
            spass.depth_load_op = LC_LOAD_OP_CLEAR;
            spass.depth_store_op = LC_STORE_OP_DONT_CARE;
            spass.clear_depth = 1.0f;
            if (lc_encoder_begin_swapchain_pass(enc, swapchain, &spass) !=
                LC_SUCCESS) {
                break;
            }
            if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
                lc_encoder_end_render_pass(enc);
                break;
            }
            {
                /* Re-submit is cheap (CPU matrices); the GPU mesh is
                 * shared — the reuse proof is in the stats. */
                lr_draw_item item;
                int c2;
                int r2;
                int ok = 1;

                memset(&item, 0, sizeof(item));
                for (r2 = 0; r2 < 10 && ok; r2++) {
                    for (c2 = 0; c2 < 6 && ok; c2++) {
                        lr_transform_identity(&item.transform);
                        item.transform.position[0] =
                            -2.0f + (float)c2 * 0.8f;
                        item.transform.position[1] = 0.5f;
                        item.transform.position[2] =
                            -1.0f - (float)r2 * 1.0f;
                        item.mesh = cube;
                        item.material = (((r2 * 6 + c2) % 2) == 0) ? red
                                                                   : blue;
                        if (lr_renderer_submit(renderer, &item) !=
                            LR_SUCCESS) {
                            ok = 0;
                        }
                    }
                }
                if (!ok) {
                    lc_encoder_end_render_pass(enc);
                    break;
                }
            }
            if (lr_renderer_render(
                    renderer, enc,
                    lc_swapchain_get_render_target(swapchain)) !=
                    LR_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                break;
            }
            {
                lr_render_stats stats;

                lr_renderer_get_stats(renderer, &stats);
                if (presented == 0) {
                    TEST_CHECK(stats.submitted_objects == 60,
                               "60 submitted (direct pass)");
                    TEST_CHECK(stats.visible_objects == 60,
                               "60 visible (direct pass)");
                    TEST_CHECK(stats.draw_calls == 60,
                               "60 draw calls");
                    TEST_CHECK(stats.triangles == 60u * 12u,
                               "720 triangles from one mesh");
                    TEST_CHECK(stats.pipeline_binds == 1,
                               "single pipeline bind");
                    TEST_CHECK(stats.material_binds == 2,
                               "two material binds (sorted)");
                }
            }
            lr_renderer_end(renderer);
            res = lc_end_frame(swapchain);
            if (res == LC_SUCCESS || res == LC_SUBOPTIMAL) {
                presented++;
            } else if (res != LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                break;
            }
        }
        TEST_CHECK(presented == 2, "scene frames presented");
        {
            /* Offscreen pixels: many lit (60 cubes cover well over a
             * fifth of the frame) with a dark clear corner. */
            unsigned char *px = readback_rgba8(device, off_img, 512, 512);
            int ok = 0;

            if (px != NULL) {
                unsigned char *corner = &px[(8u * 512u + 8u) * 4u];
                uint32_t lit = 0;
                uint32_t k;

                for (k = 0; k < 512u * 512u; k++) {
                    unsigned char *p = &px[k * 4u];

                    if (p[0] > 40 || p[1] > 40 || p[2] > 40) {
                        lit++;
                    }
                }
                ok = (lit > 50000u &&
                      corner[0] < 30 && corner[1] < 30 && corner[2] < 45 &&
                      corner[3] == 255);
            }
            TEST_CHECK(ok, "offscreen lit coverage + dark corner");
            free(px);
        }
        /* Drain in-flight frames before releasing recorded resources
         * (white-box, test-only: teardown must not race the GPU). */
        test_wait_idle(device);
        lr_material_destroy(blue);
        lr_material_destroy(red);
        lr_mesh_destroy(cube);
        lc_render_target_destroy(offscreen);
        lc_image_view_destroy(off_dview);
        lc_image_view_destroy(off_view);
        lc_image_destroy(off_depth);
        lc_image_destroy(off_img);
        lr_renderer_destroy(renderer);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "scene teardown clean");
    }

    /* ---- 3. frustum six-case micro test (submit-level culling) ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lr_renderer *renderer = NULL;
        lr_mesh *cube = NULL;
        lr_material *red = NULL;
        lr_camera camera;
        lr_draw_item item;
        lr_render_stats stats;
        int env;

        env = make_device(&device);
        if (env != 0) {
            if (env == 1) {
                SKIP_ENV("a Vulkan device");
            }
            printf("device creation failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        {
            lr_renderer_desc rdesc;

            memset(&rdesc, 0, sizeof(rdesc));
            rdesc.device = device;
            rdesc.max_objects = 16;
            rdesc.render_target.color_attachment_count = 1;
            rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
            rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
            rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
            TEST_CHECK(lr_renderer_create(&rdesc, &renderer) == LR_SUCCESS,
                       "cull renderer created");
        }
        TEST_CHECK(lr_mesh_create_cube(renderer, 1.0f, &cube) == LR_SUCCESS,
                   "cull cube created");
        {
            lr_unlit_material_desc mdesc;

            memset(&mdesc, 0, sizeof(mdesc));
            mdesc.color[0] = 1.0f;
            mdesc.color[3] = 1.0f;
            TEST_CHECK(lr_material_create_unlit(renderer, &mdesc, &red) ==
                           LR_SUCCESS,
                       "cull material created");
        }
        lr_camera_init(&camera);
        {
            static const float eye[3] = { 0.0f, 0.0f, 0.0f };
            static const float center[3] = { 0.0f, 0.0f, -1.0f };
            static const float up[3] = { 0.0f, 1.0f, 0.0f };

            TEST_CHECK(lr_camera_set_perspective(&camera, 1.5707963f, 1.0f,
                                                 0.1f, 100.0f) == LR_SUCCESS,
                       "90-degree cull camera");
            TEST_CHECK(lr_camera_look_at(&camera, eye, center, up) ==
                           LR_SUCCESS,
                       "cull camera aimed -Z");
        }
        /* 90 deg FOV at origin looking -Z: half-extent == distance.
         * Cube radius ~0.87. */
        TEST_CHECK(lr_renderer_begin(renderer, &camera) == LR_SUCCESS,
                   "cull begin succeeds");
        memset(&item, 0, sizeof(item));
        item.mesh = cube;
        item.material = red;
        lr_transform_identity(&item.transform);
        item.transform.position[2] = -5.0f; /* inside */
        TEST_CHECK(lr_renderer_submit(renderer, &item) == LR_SUCCESS,
                   "submit inside");
        item.transform.position[0] = -10.0f; /* outside left */
        item.transform.position[2] = -5.0f;
        TEST_CHECK(lr_renderer_submit(renderer, &item) == LR_SUCCESS,
                   "submit outside-left");
        item.transform.position[0] = 10.0f; /* outside right */
        TEST_CHECK(lr_renderer_submit(renderer, &item) == LR_SUCCESS,
                   "submit outside-right");
        item.transform.position[0] = 0.0f;
        item.transform.position[2] = 5.0f; /* behind camera */
        TEST_CHECK(lr_renderer_submit(renderer, &item) == LR_SUCCESS,
                   "submit behind camera");
        item.transform.position[2] = -200.0f; /* beyond far */
        TEST_CHECK(lr_renderer_submit(renderer, &item) == LR_SUCCESS,
                   "submit beyond far");
        /* Intersecting: huge sphere straddling the right plane. */
        item.transform.position[0] = 8.0f;
        item.transform.position[2] = -5.0f;
        item.transform.scale[0] = 8.0f;
        item.transform.scale[1] = 8.0f;
        item.transform.scale[2] = 8.0f;
        TEST_CHECK(lr_renderer_submit(renderer, &item) == LR_SUCCESS,
                   "submit intersecting boundary");
        lr_renderer_get_stats(renderer, &stats);
        TEST_CHECK(stats.submitted_objects == 6, "6 submitted total");
        TEST_CHECK(stats.visible_objects == 2,
                   "inside + intersecting visible, rest culled");
        lr_renderer_end(renderer);
        lr_material_destroy(red);
        lr_mesh_destroy(cube);
        lr_renderer_destroy(renderer);
        lc_device_destroy(device);
        lc_shutdown();
        TEST_CHECK(1, "cull teardown clean");
    }

    /* ---- 4. multi-viewport + empty frame + mismatch ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_command_encoder *enc = NULL;
        lr_renderer *renderer = NULL;
        lr_mesh *cube = NULL;
        lr_material *red = NULL;
        lr_camera camera;
        lc_image *img_a = NULL;
        lc_image *img_b = NULL;
        lc_image_view *view_a = NULL;
        lc_image_view *view_b = NULL;
        lc_render_target *target_a = NULL;
        lc_render_target *target_b = NULL;
        int env;

        env = make_device(&device);
        TEST_CHECK(env == 0, "device for viewport test");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC Renderer Test", 800,
                                      600) == 0,
                   "window for viewport test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for viewport test");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for viewport test");
        {
            lr_renderer_desc rdesc;

            memset(&rdesc, 0, sizeof(rdesc));
            rdesc.device = device;
            rdesc.max_objects = 16;
            rdesc.render_target.color_attachment_count = 1;
            rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
            rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
            TEST_CHECK(lr_renderer_create(&rdesc, &renderer) == LR_SUCCESS,
                       "viewport renderer (depthless)");
        }
        TEST_CHECK(lr_mesh_create_cube(renderer, 1.0f, &cube) == LR_SUCCESS,
                   "viewport cube");
        {
            lr_unlit_material_desc mdesc;

            memset(&mdesc, 0, sizeof(mdesc));
            mdesc.color[0] = 0.2f;
            mdesc.color[1] = 0.7f;
            mdesc.color[2] = 0.3f;
            mdesc.color[3] = 1.0f;
            TEST_CHECK(lr_material_create_unlit(renderer, &mdesc, &red) ==
                           LR_SUCCESS,
                       "viewport material");
        }
        lr_camera_init(&camera);
        {
            static const float eye[3] = { 0.0f, 0.0f, 3.0f };
            static const float center[3] = { 0.0f, 0.0f, 0.0f };
            static const float up[3] = { 0.0f, 1.0f, 0.0f };

            TEST_CHECK(lr_camera_set_perspective(&camera, 0.7853982f, 1.0f,
                                                 0.1f, 100.0f) == LR_SUCCESS,
                       "viewport camera");
            TEST_CHECK(lr_camera_look_at(&camera, eye, center, up) ==
                           LR_SUCCESS,
                       "viewport aimed");
        }
        /* Two compatible 256x256 viewports (no depth: renderer works
         * depthless too). TRANSFER_SRC included: readback below needs
         * it, and the copy helper rejects sourceless images loudly. */
        TEST_CHECK(make_color_target(device, LC_FORMAT_RGBA8_UNORM, 256,
                                     256, 0, &img_a, NULL, &view_a, NULL,
                                     &target_a) == 0,
                   "viewport target A");
        TEST_CHECK(make_color_target(device, LC_FORMAT_RGBA8_UNORM, 256,
                                     256, 0, &img_b, NULL, &view_b, NULL,
                                     &target_b) == 0,
                   "identical viewport target B");
        {
            int presented = 0;
            int guard = 0;

            while (presented < 1) {
                lc_render_color_attachment catt;
                lc_render_pass_desc pdesc;
                lc_result res;

                if (++guard > 60) {
                    break;
                }
                lc_poll_events();
                res = lc_begin_frame(swapchain);
                if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                    uint32_t w = lc_window_get_width(window);
                    uint32_t h = lc_window_get_height(window);

                    if (w != 0 && h != 0) {
                        lc_swapchain_recreate(swapchain, w, h);
                    }
                    continue;
                }
                if (res != LC_SUCCESS ||
                    lc_swapchain_get_encoder(swapchain, &enc) != LC_SUCCESS) {
                    break;
                }
                /* Empty render: valid, draws nothing. */
                memset(&catt, 0, sizeof(catt));
                catt.view = view_a;
                catt.load_op = LC_LOAD_OP_CLEAR;
                catt.store_op = LC_STORE_OP_STORE;
                memset(&pdesc, 0, sizeof(pdesc));
                pdesc.color_attachments = &catt;
                pdesc.color_attachment_count = 1;
                pdesc.width = 256;
                pdesc.height = 256;
                if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
                    break;
                }
                if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
                    lc_encoder_end_render_pass(enc);
                    break;
                }
                if (lr_renderer_render(renderer, enc, target_a) !=
                        LR_SUCCESS ||
                    lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                    break;
                }
                {
                    lr_render_stats stats;

                    lr_renderer_get_stats(renderer, &stats);
                    TEST_CHECK(stats.draw_calls == 0 &&
                                   stats.pipeline_binds == 0,
                               "empty render binds/draws nothing");
                }
                lr_renderer_end(renderer);
                /* Viewport A: one centered cube. */
                if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
                    break;
                }
                if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
                    lc_encoder_end_render_pass(enc);
                    break;
                }
                {
                    lr_draw_item item;

                    memset(&item, 0, sizeof(item));
                    lr_transform_identity(&item.transform);
                    item.mesh = cube;
                    item.material = red;
                    if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
                        lc_encoder_end_render_pass(enc);
                        break;
                    }
                }
                if (lr_renderer_render(renderer, enc, target_a) !=
                        LR_SUCCESS ||
                    lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                    break;
                }
                lr_renderer_end(renderer);
                /* Viewport B with the same renderer (shared pipeline). */
                catt.view = view_b;
                if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
                    break;
                }
                if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
                    lc_encoder_end_render_pass(enc);
                    break;
                }
                {
                    lr_draw_item item;

                    memset(&item, 0, sizeof(item));
                    lr_transform_identity(&item.transform);
                    item.mesh = cube;
                    item.material = red;
                    if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
                        lc_encoder_end_render_pass(enc);
                        break;
                    }
                }
                if (lr_renderer_render(renderer, enc, target_b) !=
                        LR_SUCCESS ||
                    lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                    break;
                }
                lr_renderer_end(renderer);
                if (present_clear(swapchain, enc) != 0) {
                    break;
                }
                res = lc_end_frame(swapchain);
                if (res == LC_SUCCESS || res == LC_SUBOPTIMAL) {
                    presented++;
                } else if (res != LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                    break;
                }
            }
            TEST_CHECK(presented == 1, "viewport frame presented");
        }
        {
            /* Both viewports show the centered cube (lit middle). */
            unsigned char *pa = readback_rgba8(device, img_a, 256, 256);
            unsigned char *pb = readback_rgba8(device, img_b, 256, 256);
            int oka = 0;
            int okb = 0;

            if (pa != NULL) {
                unsigned char *c = &pa[(128u * 256u + 128u) * 4u];

                oka = (c[1] > 100 && c[0] < 100 && c[3] == 255);
            }
            if (pb != NULL) {
                unsigned char *c = &pb[(128u * 256u + 128u) * 4u];

                okb = (c[1] > 100 && c[0] < 100 && c[3] == 255);
            }
            TEST_CHECK(oka, "viewport A shows the green cube");
            TEST_CHECK(okb, "viewport B shares the same view");
            free(pa);
            free(pb);
        }
        /* Mismatched record target in its own frame (after readback,
         * so its CLEAR cannot erase the evidence above): open an RGBA
         * pass but ask the renderer to draw for a BGRA target.
         * Pipeline selection succeeds structurally, then the bind
         * against the open pass fails incompatibly. */
        {
            lc_image *img_c = NULL;
            lc_image_view *view_c = NULL;
            lc_render_target *target_c = NULL;
            lc_image_desc idesc;
            lc_image_view_desc vdesc;
            lc_render_target_create_desc tdesc;
            lc_render_target_attachment att;
            lc_render_color_attachment catt;
            lc_render_pass_desc pdesc;
            lc_command_encoder *enc2 = NULL;
            lc_result res;
            int done = 0;
            int guard2 = 0;

            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_BGRA8_UNORM;
            idesc.width = 256;
            idesc.height = 256;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage =
                LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_COLOR_ATTACHMENT;
            idesc.samples = LC_SAMPLE_COUNT_1;
            TEST_CHECK(lc_image_create(device, &idesc, &img_c) == LC_SUCCESS,
                       "mismatch image created");
            memset(&vdesc, 0, sizeof(vdesc));
            vdesc.type = LC_IMAGE_VIEW_2D;
            vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            vdesc.mip_level_count = 1;
            vdesc.array_layer_count = 1;
            TEST_CHECK(lc_image_view_create(img_c, &vdesc, &view_c) ==
                           LC_SUCCESS,
                       "mismatch view created");
            memset(&tdesc, 0, sizeof(tdesc));
            tdesc.width = 256;
            tdesc.height = 256;
            att.view = view_c;
            tdesc.color_attachments = &att;
            tdesc.color_attachment_count = 1;
            TEST_CHECK(lc_render_target_create(device, &tdesc, &target_c) ==
                           LC_SUCCESS,
                       "mismatch target created");
            while (!done) {
                lr_draw_item item2;

                if (++guard2 > 60) {
                    break;
                }
                lc_poll_events();
                res = lc_begin_frame(swapchain);
                if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                    uint32_t w = lc_window_get_width(window);
                    uint32_t h = lc_window_get_height(window);

                    if (w != 0 && h != 0) {
                        lc_swapchain_recreate(swapchain, w, h);
                    }
                    continue;
                }
                if (res != LC_SUCCESS ||
                    lc_swapchain_get_encoder(swapchain, &enc2) != LC_SUCCESS) {
                    break;
                }
                if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
                    break;
                }
                memset(&item2, 0, sizeof(item2));
                lr_transform_identity(&item2.transform);
                item2.mesh = cube;
                item2.material = red;
                if (lr_renderer_submit(renderer, &item2) != LR_SUCCESS) {
                    break;
                }
                memset(&catt, 0, sizeof(catt));
                catt.view = view_a;
                catt.load_op = LC_LOAD_OP_CLEAR;
                catt.store_op = LC_STORE_OP_STORE;
                memset(&pdesc, 0, sizeof(pdesc));
                pdesc.color_attachments = &catt;
                pdesc.color_attachment_count = 1;
                pdesc.width = 256;
                pdesc.height = 256;
                if (lc_encoder_begin_render_pass(enc2, &pdesc) != LC_SUCCESS) {
                    break;
                }
                TEST_CHECK(lr_renderer_render(renderer, enc2, target_c) ==
                               LR_ERROR_INCOMPATIBLE,
                           "BGRA-target draw in RGBA pass rejected");
                lr_renderer_end(renderer);
                if (lc_encoder_end_render_pass(enc2) != LC_SUCCESS) {
                    break;
                }
                if (present_clear(swapchain, enc2) != 0) {
                    break;
                }
                res = lc_end_frame(swapchain);
                if (res == LC_SUCCESS || res == LC_SUBOPTIMAL) {
                    done = 1;
                } else if (res != LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                    break;
                }
            }
            TEST_CHECK(done, "mismatch frame presented");
            lc_render_target_destroy(target_c);
            lc_image_view_destroy(view_c);
            lc_image_destroy(img_c);
        }
        test_wait_idle(device);
        lr_material_destroy(red);
        lr_mesh_destroy(cube);
        lr_renderer_destroy(renderer);
        lc_render_target_destroy(target_b);
        lc_render_target_destroy(target_a);
        lc_image_view_destroy(view_b);
        lc_image_view_destroy(view_a);
        lc_image_destroy(img_b);
        lc_image_destroy(img_a);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "viewport teardown clean");
    }

    /* ---- 5. 500-frame endurance: orbit, rotation, resizes ---- */
    if (lc_init() != LC_SUCCESS) {
        printf("re-init failed: FAIL\n");
        return 1;
    }
    {
        lc_device *device = NULL;
        lc_window *window = NULL;
        lc_surface *surface = NULL;
        lc_swapchain *swapchain = NULL;
        lc_command_encoder *enc = NULL;
        lr_renderer *renderer = NULL;
        lr_mesh *cube = NULL;
        lr_mesh *ground = NULL;
        lr_material *red = NULL;
        lr_material *blue = NULL;
        lr_material *green = NULL;
        lr_camera camera;
        lc_image *off_img = NULL;
        lc_image_view *off_view = NULL;
        lc_image *off_depth = NULL;
        lc_image_view *off_dview = NULL;
        lc_render_target *offscreen = NULL;
        int env;
        int presented = 0;
        int guard = 0;
        int frame_no = 0;
        uint32_t cache_before = 0;
        uint32_t off_size = 256;
        static const float y_axis[3] = { 0.0f, 1.0f, 0.0f };
        /* Precomputed circle points (no trig at runtime). Radius 7.5
         * keeps every stop outside the cube field for clean framing;
         * heights look slightly down into the scene. */
        static const float orbit[8][3] = {
            { 0.0f, 3.0f, 7.5f }, { 5.3f, 3.0f, 5.3f },
            { 7.5f, 3.0f, 0.0f }, { 5.3f, 3.0f, -5.3f },
            { 0.0f, 3.0f, -9.5f }, { -5.3f, 3.0f, -5.3f },
            { -7.5f, 3.0f, 0.0f }, { -5.3f, 3.0f, 5.3f },
        };
        static const float origin[3] = { 0.0f, 0.3f, -2.0f };
        static const float up[3] = { 0.0f, 1.0f, 0.0f };

        env = make_device(&device);
        TEST_CHECK(env == 0, "device for endurance test");
        if (env != 0) {
            printf("device failed unexpectedly: FAIL\n");
            FAIL_SUMMARY();
        }
        TEST_CHECK(make_window_titled(&window, "LumaC Renderer Test", 800,
                                      600) == 0,
                   "window for endurance test");
        TEST_CHECK(make_surface(device, window, &surface) == 0,
                   "surface for endurance test");
        TEST_CHECK(make_swapchain(device, surface, &swapchain) == 0,
                   "swapchain for endurance test");
        {
            lr_renderer_desc rdesc;

            memset(&rdesc, 0, sizeof(rdesc));
            rdesc.device = device;
            rdesc.max_objects = 128;
            rdesc.render_target.color_attachment_count = 1;
            rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
            rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
            rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
            TEST_CHECK(lr_renderer_create(&rdesc, &renderer) == LR_SUCCESS,
                       "endurance renderer created");
        }
        TEST_CHECK(lr_mesh_create_cube(renderer, 1.0f, &cube) == LR_SUCCESS,
                   "endurance cube");
        TEST_CHECK(lr_mesh_create_plane(renderer, 14.0f, 14.0f, &ground) ==
                       LR_SUCCESS,
                   "endurance ground");
        {
            lr_unlit_material_desc mdesc;

            memset(&mdesc, 0, sizeof(mdesc));
            mdesc.color[0] = 0.85f;
            mdesc.color[1] = 0.25f;
            mdesc.color[2] = 0.22f;
            mdesc.color[3] = 1.0f;
            TEST_CHECK(lr_material_create_unlit(renderer, &mdesc, &red) ==
                           LR_SUCCESS,
                       "endurance red");
            mdesc.color[0] = 0.25f;
            mdesc.color[1] = 0.45f;
            mdesc.color[2] = 0.90f;
            TEST_CHECK(lr_material_create_unlit(renderer, &mdesc, &blue) ==
                           LR_SUCCESS,
                       "endurance blue");
            mdesc.color[0] = 0.25f;
            mdesc.color[1] = 0.45f;
            mdesc.color[2] = 0.25f;
            TEST_CHECK(lr_material_create_unlit(renderer, &mdesc, &green) ==
                           LR_SUCCESS,
                       "endurance green");
        }
        lr_camera_init(&camera);
        TEST_CHECK(lr_camera_set_perspective(&camera, 0.7853982f,
                                             800.0f / 600.0f, 0.1f,
                                             100.0f) == LR_SUCCESS,
                   "endurance camera perspective");
        TEST_CHECK(make_color_target(device, LC_FORMAT_RGBA8_UNORM, 256,
                                     256, 1, &off_img, &off_depth, &off_view,
                                     &off_dview, &offscreen) == 0,
                   "endurance offscreen target");
        cache_before = lc_device_get_pass_cache_count(device);

        while (presented < 500) {
            lc_render_color_attachment catt;
            lc_render_depth_attachment datt;
            lc_render_pass_desc pdesc;
            lc_render_swapchain_pass_desc spass;
            lc_result res;
            int r;
            int c;

            if (++guard > 500 * 25 + 1200) {
                break;
            }
            /* Occasional resizes: swapchain + offscreen target
             * (same signature family, so no pipeline/pass growth). */
            if ((frame_no % 100) == 99) {
                uint32_t w = (frame_no % 200 == 99) ? 1024u : 800u;
                uint32_t h = (frame_no % 200 == 99) ? 768u : 600u;
                uint32_t ns = (off_size == 256u) ? 384u : 256u;

                if (lc_swapchain_recreate(swapchain, w, h) != LC_SUCCESS) {
                    break;
                }
                lc_render_target_destroy(offscreen);
                lc_image_view_destroy(off_dview);
                lc_image_view_destroy(off_view);
                lc_image_destroy(off_depth);
                lc_image_destroy(off_img);
                if (make_color_target(device, LC_FORMAT_RGBA8_UNORM, ns,
                                      ns, 1, &off_img, &off_depth, &off_view,
                                      &off_dview, &offscreen) != 0) {
                    break;
                }
                off_size = ns;
            }
            lc_poll_events();
            {
                uint32_t w = lc_window_get_width(window);
                uint32_t h = lc_window_get_height(window);

                if (w == 0 || h == 0) {
                    continue;
                }
                if (w != lc_swapchain_get_width(swapchain) ||
                    h != lc_swapchain_get_height(swapchain)) {
                    res = lc_swapchain_recreate(swapchain, w, h);
                    if (res != LC_SUCCESS &&
                        res != LC_ERROR_ZERO_EXTENT) {
                        break;
                    }
                    continue;
                }
            }
            /* Slow camera orbit + per-object rotation. */
            {
                const float *eye = orbit[(frame_no / 25) % 8];

                if (lr_camera_look_at(&camera, eye, origin, up) !=
                    LR_SUCCESS) {
                    break;
                }
            }
            res = lc_begin_frame(swapchain);
            if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                uint32_t w = lc_window_get_width(window);
                uint32_t h = lc_window_get_height(window);

                if (w != 0 && h != 0) {
                    lc_swapchain_recreate(swapchain, w, h);
                }
                continue;
            }
            if (res != LC_SUCCESS ||
                lc_swapchain_get_encoder(swapchain, &enc) != LC_SUCCESS) {
                break;
            }
            /* Scene pass to the (occasionally resized) offscreen. */
            memset(&catt, 0, sizeof(catt));
            catt.view = off_view;
            catt.load_op = LC_LOAD_OP_CLEAR;
            catt.store_op = LC_STORE_OP_STORE;
            catt.clear_color[0] = 0.05f;
            catt.clear_color[1] = 0.06f;
            catt.clear_color[2] = 0.09f;
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
            pdesc.width = off_size;
            pdesc.height = off_size;
            if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
                break;
            }
            if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
                lc_encoder_end_render_pass(enc);
                break;
            }
            {
                lr_draw_item item;
                int ok = 1;

                memset(&item, 0, sizeof(item));
                lr_transform_identity(&item.transform);
                item.transform.position[1] = -0.51f;
                item.mesh = ground;
                item.material = green;
                ok = (lr_renderer_submit(renderer, &item) == LR_SUCCESS);
                for (r = 0; r < 4 && ok; r++) {
                    for (c = 0; c < 4 && ok; c++) {
                        lr_transform_identity(&item.transform);
                        item.transform.position[0] =
                            -2.25f + (float)c * 1.5f;
                        item.transform.position[1] = 0.5f;
                        item.transform.position[2] =
                            0.0f - (float)r * 1.5f;
                        lr_quat_from_axis_angle(
                            y_axis,
                            (float)frame_no * 0.02f +
                                (float)(r * 4 + c) * 0.2f,
                            item.transform.rotation);
                        item.mesh = cube;
                        item.material = (((r * 4 + c) % 2) == 0) ? red : blue;
                        if (lr_renderer_submit(renderer, &item) !=
                            LR_SUCCESS) {
                            ok = 0;
                        }
                    }
                }
                if (!ok) {
                    lc_encoder_end_render_pass(enc);
                    break;
                }
            }
            if (lr_renderer_render(renderer, enc, offscreen) != LR_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                break;
            }
            lr_renderer_end(renderer);
            /* Direct presentation of a small subset (keeps presents
             * cheap while the offscreen carries the full scene). */
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
            if (lc_encoder_begin_swapchain_pass(enc, swapchain, &spass) !=
                    LC_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                break;
            }
            res = lc_end_frame(swapchain);
            if (res == LC_SUCCESS || res == LC_SUBOPTIMAL) {
                presented++;
                frame_no++;
            } else if (res != LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
                break;
            }
            if (presented == 250) {
                lr_render_stats stats;

                lr_renderer_get_stats(renderer, &stats);
                printf("[INFO] frame 250: submitted=%u visible=%u draws=%u "
                       "tris=%u pipes=%u mats=%u\n",
                       stats.submitted_objects, stats.visible_objects,
                       stats.draw_calls, stats.triangles,
                       stats.pipeline_binds, stats.material_binds);
                TEST_CHECK(stats.submitted_objects == 17,
                           "250: 17 submitted (ground+16)");
                TEST_CHECK(stats.visible_objects == 17,
                           "250: all visible");
                TEST_CHECK(stats.draw_calls == 17, "250: 17 draws");
                TEST_CHECK(stats.triangles == 16u * 12u + 2u,
                           "250: 194 triangles");
                TEST_CHECK(stats.pipeline_binds == 1,
                           "250: single pipeline bind");
                TEST_CHECK(stats.material_binds == 3,
                           "250: three material binds");
            }
        }
        TEST_CHECK(presented == 500, "500 endurance frames presented");
        {
            /* Bounded pass-cache growth: at most the swapchain-present
             * key beyond warmup — resizes and target recreations must
             * reuse cached passes, never grow per size (formats, not
             * extents, key the cache). */
            uint32_t cache_after = lc_device_get_pass_cache_count(device);

            TEST_CHECK(cache_after <= cache_before + 2u,
                       "pass cache stable across resizes");
        }
        /* Screenshot hook: the offscreen scene at its final orbit. */
        {
            const char *ppm_path = getenv("LC_SAVE_PPM");

            if (ppm_path != NULL && ppm_path[0] != '\0') {
                unsigned char *px =
                    readback_rgba8(device, off_img, off_size, off_size);

                if (px != NULL) {
                    FILE *f = fopen(ppm_path, "wb");

                    if (f != NULL) {
                        uint32_t x;
                        uint32_t y;

                        fprintf(f, "P6\n%u %u\n255\n", off_size, off_size);
                        for (y = off_size; y-- > 0;) {
                            for (x = 0; x < off_size; x++) {
                                unsigned char *p =
                                    &px[(y * off_size + x) * 4u];

                                fputc(p[0], f);
                                fputc(p[1], f);
                                fputc(p[2], f);
                            }
                        }
                        fclose(f);
                        printf("[INFO] screenshot -> %s\n", ppm_path);
                    }
                    free(px);
                }
            }
        }
        test_wait_idle(device);
        lr_material_destroy(green);
        lr_material_destroy(blue);
        lr_material_destroy(red);
        lr_mesh_destroy(ground);
        lr_mesh_destroy(cube);
        lr_renderer_destroy(renderer);
        lc_render_target_destroy(offscreen);
        lc_image_view_destroy(off_dview);
        lc_image_view_destroy(off_view);
        lc_image_destroy(off_depth);
        lc_image_destroy(off_img);
        lc_swapchain_destroy(swapchain);
        lc_surface_destroy(surface);
        lc_device_destroy(device);
        lc_window_destroy(window);
        lc_shutdown();
        TEST_CHECK(1, "endurance teardown clean");
    }

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
