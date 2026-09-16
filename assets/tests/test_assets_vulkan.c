/*
 * Luma Assets Vulkan integration test (Phase 14).
 *
 * Loads the deterministic fixtures (embedded .glb, external
 * .gltf/.bin/.png, example BoxTextured.glb) through a live renderer,
 * then checks sharing/dedup counts, hierarchy, material metadata,
 * bounds, submission, and a short validated render run.
 *
 * If the environment cannot provide a window or Vulkan setup, SKIP
 * and exit 0. Any other failure is a hard FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_assets/luma_assets.h>
#include "graphics/graphics_internal.h"
#include "internal/renderer_internal.h"
#include "internal/assets_internal.h"

#ifndef LA_FIXTURE_DIR
#define LA_FIXTURE_DIR "."
#endif
#ifndef LA_MALFORMED_DIR
#define LA_MALFORMED_DIR "."
#endif
#ifndef LA_BOX_PATH
#define LA_BOX_PATH "BoxTextured.glb"
#endif
#ifndef LA_SKINNED_PATH
#define LA_SKINNED_PATH "skinned.glb"
#endif

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

    desc.title = "Luma Assets Test";
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
    lc_swapchain_desc desc = { 0 };

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

static int feq(float a, float b) {
    return fabsf(a - b) < 1e-4f;
}

/* White-box idle before teardown (test-only): destroying meshes,
 * materials, and cached textures while the last frame is still in
 * flight trips "in use by VkCommandBuffer" validation errors. Same
 * privilege as LumaC's own integration tests. */
static void test_wait_idle(lc_device *device) {
    if (device != NULL && device->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->device);
    }
}

int main(void) {
    lc_window *window = NULL;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lr_renderer *renderer = NULL;
    la_asset_manager *assets = NULL;
    la_model *glb = NULL;
    la_model *gltf = NULL;
    la_model *box = NULL;
    la_model *again = NULL;
    la_model *skinned = NULL;
    char path[1024];
    int rc;
    int exit_code = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (lc_init() != LC_SUCCESS) {
        printf("SKIP: lc_init failed\n");
        return 0;
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
    rc = make_surface(device, window, &surface);
    if (rc != 0) {
        lc_window_destroy(window);
        lc_device_destroy(device);
        if (rc > 0) {
            SKIP_ENV("a surface");
        }
        printf("FAIL: lc_surface_create\n");
        lc_shutdown();
        return 1;
    }
    rc = make_swapchain(device, surface, &swapchain);
    if (rc != 0) {
        lc_surface_destroy(surface);
        lc_window_destroy(window);
        lc_device_destroy(device);
        if (rc > 0) {
            SKIP_ENV("a swapchain");
        }
        printf("FAIL: lc_swapchain_create\n");
        lc_shutdown();
        return 1;
    }

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
        if (lr_renderer_create(&rdesc, &renderer) != LR_SUCCESS) {
            printf("FAIL: lr_renderer_create\n");
            goto cleanup;
        }
    }

    {
        la_asset_manager_desc adesc;

        memset(&adesc, 0, sizeof(adesc));
        adesc.renderer = renderer;
        if (la_asset_manager_create(&adesc, &assets) != LA_SUCCESS) {
            printf("FAIL: la_asset_manager_create\n");
            goto cleanup;
        }
    }

    /* Embedded sharing/hierarchy fixture. */
    snprintf(path, sizeof(path), "%s/fixture.glb", LA_FIXTURE_DIR);
    if (la_model_load(assets, path, &glb) != LA_SUCCESS) {
        printf("FAIL: load fixture.glb: %s\n",
               la_asset_manager_get_last_error(assets));
        goto cleanup;
    }
    TEST_CHECK(la_model_get_node_count(glb) == 5, "glb: 5 nodes");
    TEST_CHECK(la_model_get_mesh_count(glb) == 4, "glb: 4 unique meshes");
    TEST_CHECK(la_model_get_material_count(glb) == 2, "glb: 2 materials");
    TEST_CHECK(la_model_get_texture_count(glb) == 2, "glb: 2 GPU textures");
    TEST_CHECK(la_model_get_sampler_count(glb) == 2, "glb: 2 GPU samplers");
    TEST_CHECK(la_model_get_instance_count(glb) == 7, "glb: 7 instances");
    TEST_CHECK(la_model_get_source_primitive_count(glb) == 4,
               "glb: 4 source primitives");
    TEST_CHECK(la_model_get_source_texture_count(glb) == 1,
               "glb: 1 source image");
    TEST_CHECK(la_model_get_source_sampler_count(glb) == 2,
               "glb: 2 source samplers");
    TEST_CHECK(la_asset_manager_get_texture_count(assets) == 2 &&
               la_asset_manager_get_sampler_count(assets) == 2,
               "glb: manager caches hold 2+2");

    /* Hierarchy: TriC (2) is parented under QuadB (1). */
    {
        const la_model_node *b = la_model_get_node(glb, 1);
        const la_model_node *c = la_model_get_node(glb, 2);
        const la_model_node *e = la_model_get_node(glb, 4);
        int links_ok = (b != NULL && c != NULL && b->child_count == 1 &&
                        c->parent == 1 && e != NULL && e->parent < 0 &&
                        e->mesh_index == 1);

        TEST_CHECK(links_ok, "glb: QuadB parents TriC, MatrixE is a root");
        TEST_CHECK(e != NULL && feq(e->local_transform.position[0], 1.5f) &&
                   feq(e->local_matrix[12], 1.5f),
                   "glb: matrix node keeps raw + decomposed translation");
        TEST_CHECK(b != NULL && b->name != NULL &&
                   strcmp(b->name, "QuadB") == 0,
                   "glb: node names survive import");
    }

    /* Materials: textured green + flat blue metadata. */
    {
        const la_pbr_material_data *m0 = la_model_get_material_data(glb, 0);
        const la_pbr_material_data *m1 = la_model_get_material_data(glb, 1);
        int m0_ok = (m0 != NULL && feq(m0->metallic_factor, 0.2f) &&
                     feq(m0->roughness_factor, 0.8f) &&
                     m0->base_color_texture == 0 && m0->normal_texture == 0 &&
                     feq(m0->normal_scale, 0.75f) &&
                     m0->alpha_mode == LA_ALPHA_OPAQUE &&
                     !m0->double_sided);
        int m1_ok = (m1 != NULL && feq(m1->base_color_factor[0], 0.25f) &&
                     feq(m1->base_color_factor[1], 0.45f) &&
                     feq(m1->base_color_factor[2], 0.9f) &&
                     m1->base_color_texture < 0 && m1->normal_texture < 0);

        TEST_CHECK(m0_ok, "glb: TexGreen metadata (roles, scale, factors)");
        TEST_CHECK(m1_ok, "glb: FlatBlue metadata (factor, no textures)");
        TEST_CHECK(la_model_get_material_data(glb, 2) == NULL,
                   "glb: material OOB is NULL");
    }

    /* Texture roles: base color sRGB, normal linear; 4x4 both. */
    {
        la_texture_info t0;
        la_texture_info t1;

        la_model_get_texture_info(glb, 0, &t0);
        la_model_get_texture_info(glb, 1, &t1);
        TEST_CHECK(t0.width == 4 && t0.height == 4 && t0.srgb &&
                   t1.width == 4 && t1.height == 4 && !t1.srgb,
                   "glb: dual-role textures (sRGB base, linear normal)");
    }

    /* Bounds cover the translated quads (x spans at least +-2). */
    {
        lr_bounds b;

        la_model_get_bounds(glb, &b);
        TEST_CHECK(b.min[0] <= -2.0f && b.max[0] >= 2.0f &&
                   b.radius > 0.0f &&
                   feq(b.center[0], (b.min[0] + b.max[0]) * 0.5f),
                   "glb: bounds span instances with sane center");
    }

    /* External-reference trio: identical scene, distinct cache keys
     * for images (different source path) but shared sampler params. */
    snprintf(path, sizeof(path), "%s/fixture.gltf", LA_FIXTURE_DIR);
    if (la_model_load(assets, path, &gltf) != LA_SUCCESS) {
        printf("FAIL: load fixture.gltf: %s\n",
               la_asset_manager_get_last_error(assets));
        goto cleanup;
    }
    TEST_CHECK(la_model_get_node_count(gltf) == 5 &&
               la_model_get_instance_count(gltf) == 7 &&
               la_model_get_texture_count(gltf) == 2,
               "gltf: external refs resolve to the same scene");
    TEST_CHECK(la_asset_manager_get_texture_count(assets) == 4 &&
               la_asset_manager_get_sampler_count(assets) == 2,
               "gltf: images re-keyed per path, samplers shared");

    /* Example asset: one textured box. */
    if (la_model_load(assets, LA_BOX_PATH, &box) != LA_SUCCESS) {
        printf("FAIL: load BoxTextured: %s\n",
               la_asset_manager_get_last_error(assets));
        goto cleanup;
    }
    TEST_CHECK(la_model_get_node_count(box) == 1 &&
               la_model_get_mesh_count(box) == 1 &&
               la_model_get_material_count(box) == 1 &&
               la_model_get_texture_count(box) == 1 &&
               la_model_get_sampler_count(box) == 1 &&
               la_model_get_instance_count(box) == 1,
               "box: single textured instance");
    {
        la_texture_info ti;

        la_model_get_texture_info(box, 0, &ti);
        TEST_CHECK(ti.width == 64 && ti.height == 64 && ti.srgb,
                   "box: 64x64 sRGB checker");
    }
    TEST_CHECK(la_asset_manager_get_texture_count(assets) == 5 &&
               la_asset_manager_get_sampler_count(assets) == 3,
               "box: caches grow by 1 texture + 1 sampler");

    /* Reload: cache hits, no growth. */
    snprintf(path, sizeof(path), "%s/fixture.glb", LA_FIXTURE_DIR);
    if (la_model_load(assets, path, &again) != LA_SUCCESS) {
        printf("FAIL: reload fixture.glb: %s\n",
               la_asset_manager_get_last_error(assets));
        goto cleanup;
    }
    TEST_CHECK(la_asset_manager_get_texture_count(assets) == 5 &&
               la_asset_manager_get_sampler_count(assets) == 3,
               "reload: identical sources share cache entries");

    /* ---- PART AQ-29: skinned fixture (Phase 29) ----
     * 2 joints, 1 skin, 2 primitives covering every
     * JOINTS/WEIGHTS encoding, 1 kept animation (T LINEAR + R
     * CUBICSPLINE) + 1 morph-only animation excluded at import.
     * No images: texture/sampler caches must not grow. */
    if (la_model_load(assets, LA_SKINNED_PATH, &skinned) != LA_SUCCESS) {
        printf("FAIL: load skinned.glb: %s\n",
               la_asset_manager_get_last_error(assets));
        goto cleanup;
    }
    TEST_CHECK(la_model_get_node_count(skinned) == 4 &&
               la_model_get_mesh_count(skinned) == 2 &&
               la_model_get_material_count(skinned) == 1 &&
               la_model_get_texture_count(skinned) == 0 &&
               la_model_get_sampler_count(skinned) == 0 &&
               la_model_get_instance_count(skinned) == 2 &&
               la_model_get_skin_count(skinned) == 1 &&
               la_model_get_animation_count(skinned) == 1,
               "skinned: nodes/prims/material + 1 skin + 1 anim");
    TEST_CHECK(la_model_get_skin_joint_count(skinned, 0) == 2 &&
               la_model_get_skin_joint_node(skinned, 0, 0) == 1 &&
               la_model_get_skin_joint_node(skinned, 0, 1) == 2 &&
               la_model_get_node_skin(skinned, 3) == 0 &&
               la_model_get_node_skin(skinned, 0) == -1 &&
               la_model_get_node_skin(skinned, 1) == -1,
               "skinned: joint nodes + node skin links");
    {
        float ibm[16];

        la_model_get_skin_inverse_bind(skinned, 0, 0, ibm);
        TEST_CHECK(feq(ibm[0], 1.0f) && feq(ibm[5], 1.0f) &&
                   feq(ibm[10], 1.0f) && feq(ibm[15], 1.0f) &&
                   feq(ibm[12], 0.0f),
                   "skinned: joint0 inverse bind is identity");
        la_model_get_skin_inverse_bind(skinned, 0, 1, ibm);
        TEST_CHECK(feq(ibm[12], 1.0f) && feq(ibm[13], 2.0f) &&
                   feq(ibm[14], 3.0f) && feq(ibm[15], 1.0f),
                   "skinned: joint1 inverse bind decodes");
    }
    {
        la_anim_channel ch0;
        la_anim_channel ch1;
        int ok = 0;

        memset(&ch0, 0, sizeof(ch0));
        memset(&ch1, 0, sizeof(ch1));
        ok = (la_model_get_animation_channel_count(skinned, 0) == 2 &&
              la_model_get_animation_channel(skinned, 0, 0, &ch0) &&
              la_model_get_animation_channel(skinned, 0, 1, &ch1));
        TEST_CHECK(ok, "skinned: Wave keeps 2 channels");
        TEST_CHECK(ch0.target_node == 1 && ch0.path == 0 &&
                   ch0.interpolation == 1 && ch0.key_count == 2 &&
                   feq(ch0.times[1], 1.0f) &&
                   feq(ch0.values[3], 1.0f),
                   "skinned: translation track (LINEAR, 2 keys)");
        TEST_CHECK(ch1.target_node == 2 && ch1.path == 1 &&
                   ch1.interpolation == 2 && ch1.key_count == 2 &&
                   feq(ch1.times[1], 2.0f) &&
                   feq(ch1.values[4 + 3], 1.0f) &&
                   feq(ch1.values[12 + 6], 0.7071068f),
                   "skinned: cubic rotation triples (file order)");
        TEST_CHECK(feq(la_model_get_animation_duration(skinned, 0),
                       2.0f),
                   "skinned: duration is max last-key time");
    }
    /* Vertex skin decode end-to-end (white-box readback of the
     * uploaded vertex buffers; idle first like the HDR path). */
    {
        lr_mesh *p0 = la_model_borrow_mesh(skinned, 0, 0);
        lr_mesh *p1 = la_model_borrow_mesh(skinned, 0, 1);
        lr_vertex *cpu = NULL;
        int ok = 0;

        test_wait_idle(device);
        if (p0 != NULL && p1 != NULL &&
            p0->vertex_buffer != NULL && p1->vertex_buffer != NULL) {
            cpu = (lr_vertex *)malloc(sizeof(lr_vertex) * 4u);
            if (cpu != NULL &&
                lc_buffer_read(p0->vertex_buffer, 0, cpu,
                               sizeof(lr_vertex) * 4u) == LC_SUCCESS) {
                /* v0 rigid; v1 half/half; v2 quarter sums
                 * normalize; v3 all-zero falls back to rigid. */
                ok = (cpu[0].joints[0] == 0 &&
                      cpu[0].weights[0] == 1.0f &&
                      cpu[1].joints[0] == 0 &&
                      cpu[1].joints[1] == 1 &&
                      feq(cpu[1].weights[0], 0.5f) &&
                      feq(cpu[1].weights[1], 0.5f) &&
                      feq(cpu[2].weights[0], 0.5f) &&
                      feq(cpu[2].weights[1], 0.5f) &&
                      cpu[3].weights[0] == 1.0f &&
                      cpu[3].weights[1] == 0.0f);
            }
            TEST_CHECK(ok, "skinned: prim0 joints/weights upload");
            ok = 0;
            if (cpu != NULL &&
                lc_buffer_read(p1->vertex_buffer, 0, cpu,
                               sizeof(lr_vertex) * 3u) == LC_SUCCESS) {
                /* u16 joints (incl. 300) + normalized u16
                 * weights decode; zero weights go rigid. */
                ok = (cpu[0].joints[0] == 0 &&
                      cpu[0].weights[0] == 1.0f &&
                      cpu[1].joints[0] == 1 &&
                      cpu[1].joints[1] == 1 &&
                      fabsf(cpu[1].weights[0] - 0.5f) < 1e-4f &&
                      fabsf(cpu[1].weights[1] - 0.5f) < 1e-4f &&
                      cpu[2].joints[0] == 300 &&
                      cpu[2].weights[0] == 1.0f);
            }
            TEST_CHECK(ok, "skinned: prim1 u16 joints/weights upload");
            free(cpu);
        } else {
            TEST_CHECK(0, "skinned: prim meshes borrow");
        }
    }
    TEST_CHECK(la_asset_manager_get_texture_count(assets) == 5 &&
               la_asset_manager_get_sampler_count(assets) == 3,
               "skinned: imageless import grows no caches");

    /* GPU-reaching negatives: valid geometry, failing images. Meshes
     * upload, then the import unwinds — caches must not grow. */
    snprintf(path, sizeof(path), "%s/missing_image.gltf",
             LA_MALFORMED_DIR);
    {
        la_model *bad = NULL;

        TEST_CHECK(la_model_load(assets, path, &bad) ==
                       LA_ERROR_NOT_FOUND && bad == NULL,
                   "negative: missing external image is NOT_FOUND");
    }
    snprintf(path, sizeof(path), "%s/corrupt_image.glb",
             LA_MALFORMED_DIR);
    {
        la_model *bad = NULL;

        TEST_CHECK(la_model_load(assets, path, &bad) ==
                       LA_ERROR_IMPORT && bad == NULL,
                   "negative: undecodable image is IMPORT");
    }
    TEST_CHECK(la_asset_manager_get_texture_count(assets) == 5 &&
               la_asset_manager_get_sampler_count(assets) == 3,
               "negative: failed imports leave no cache residue");

    /* Submission: every instance reaches the renderer. */
    {
        lr_camera camera;
        static const float eye[3] = { 0.0f, 2.4f, 6.0f };
        static const float center[3] = { 0.0f, 0.4f, 0.0f };
        static const float up[3] = { 0.0f, 1.0f, 0.0f };
        lr_render_stats stats;
        int ok;

        lr_camera_init(&camera);
        ok = (lr_camera_set_perspective(&camera, 0.7853982f,
                                        800.0f / 600.0f, 0.1f,
                                        100.0f) == LR_SUCCESS &&
              lr_camera_look_at(&camera, eye, center, up) == LR_SUCCESS &&
              lr_renderer_begin(renderer, &camera) == LR_SUCCESS &&
              la_model_submit(glb, renderer, NULL) == LA_SUCCESS &&
              la_model_submit(gltf, renderer, NULL) == LA_SUCCESS &&
              la_model_submit(box, renderer, NULL) == LA_SUCCESS &&
              la_model_submit(again, renderer, NULL) == LA_SUCCESS &&
              la_model_submit(skinned, renderer, NULL) == LA_SUCCESS);
        lr_renderer_get_stats(renderer, &stats);
        ok = ok && (stats.submitted_objects == 7 + 7 + 1 + 7 + 2);
        lr_renderer_end(renderer);
        TEST_CHECK(ok, "submit: 24 instances reach the renderer");
        TEST_CHECK(la_model_submit(glb, NULL, NULL) ==
                       LA_ERROR_INVALID_ARGUMENT,
                   "submit: NULL renderer rejected");
    }

    /* Short validated render run (5 frames, present path). */
    {
        lr_camera camera;
        static const float eye[3] = { 0.0f, 2.4f, 6.0f };
        static const float center[3] = { 0.0f, 0.4f, 0.0f };
        static const float up[3] = { 0.0f, 1.0f, 0.0f };
        unsigned frame;
        int ok = 1;

        lr_camera_init(&camera);
        ok = ok && (lr_camera_set_perspective(&camera, 0.7853982f,
                                              800.0f / 600.0f, 0.1f,
                                              100.0f) == LR_SUCCESS);
        ok = ok &&
             (lr_camera_look_at(&camera, eye, center, up) == LR_SUCCESS);
        for (frame = 0; frame < 5 && ok; frame++) {
            lc_command_encoder *enc = NULL;
            lc_render_swapchain_pass_desc spass;
            lc_result res;

            lc_poll_events();
            res = lc_begin_frame(swapchain);
            if (res != LC_SUCCESS) {
                ok = 0;
                break;
            }
            if (lc_swapchain_get_encoder(swapchain, &enc) != LC_SUCCESS) {
                ok = 0;
                break;
            }
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
            ok = ok && (lc_encoder_begin_swapchain_pass(enc, swapchain,
                                                        &spass) ==
                        LC_SUCCESS);
            ok = ok && (lr_renderer_begin(renderer, &camera) == LR_SUCCESS);
            ok = ok && (la_model_submit(box, renderer, NULL) == LA_SUCCESS);
            ok = ok && (lr_renderer_render(
                            renderer, enc,
                            lc_swapchain_get_render_target(swapchain)) ==
                        LR_SUCCESS);
            lr_renderer_end(renderer);
            ok = ok && (lc_encoder_end_render_pass(enc) == LC_SUCCESS);
            res = lc_end_frame(swapchain);
            ok = ok && (res == LC_SUCCESS || res == LC_SUBOPTIMAL);
        }
        TEST_CHECK(ok, "render: 5 validated BoxTextured frames");
    }

    /* ---- PART AQ: HDR environment source (Phase 17) ----
     * .hdr file -> RGBA16F GPU image; values round-trip through a
     * white-box readback (upload fidelity for IBL preprocessing). */
    {
        char path[1024];
        la_hdr_image_info info;
        la_hdr_image_info info2;
        lc_image_view *view;

        snprintf(path, sizeof(path), "%s/env_gradient.hdr",
                 LA_FIXTURE_DIR);
        TEST_CHECK(la_hdr_load(assets, path, &info) == LA_SUCCESS &&
                   info.width == 64 && info.height == 32 &&
                   info.source != NULL,
                   "hdr: fixture loads as 64x32");
        TEST_CHECK(la_hdr_load(assets, path, &info2) == LA_SUCCESS &&
                   info2.width == 64,
                   "hdr: repeat load shares the cache entry");
        view = la_hdr_get_view(assets, path);
        TEST_CHECK(view != NULL, "hdr: GPU view borrows");
        TEST_CHECK(la_hdr_get_view(assets, "nope.hdr") == NULL &&
                   la_hdr_get_view(NULL, path) == NULL,
                   "hdr: unknown/NULL lookups are NULL");
        if (view != NULL) {
            la_hdr_asset *entry = la_hdr_lookup(assets, path);
            lc_buffer *staging = NULL;
            lc_buffer_desc bdesc;
            void *mapped = NULL;
            int ok = 0;

            if (entry != NULL && entry->image != NULL) {
                memset(&bdesc, 0, sizeof(bdesc));
                bdesc.size = (uint64_t)64 * 32 * 8u;
                bdesc.usage = LC_BUFFER_USAGE_TRANSFER_DST;
                bdesc.memory = LC_MEMORY_GPU_TO_CPU;
                if (lc_buffer_create(device, &bdesc, &staging) ==
                    LC_SUCCESS) {
                    lc_result cres;
                    lc_result mres;

                    test_wait_idle(device);
                    cres = lc_vulkan_copy_image_to_buffer(
                        device, entry->image, 0, 0, 64, 32, 1,
                        staging->vk_buffer, 0);
                    mres = (cres == LC_SUCCESS)
                               ? lc_buffer_map(staging, &mapped)
                               : (lc_result)999;
                    if (cres == LC_SUCCESS && mres == LC_SUCCESS &&
                        mapped != NULL) {
                        const uint16_t *px =
                            (const uint16_t *)mapped;
                        float top_r =
                            la_half_to_float(px[(0u * 64u + 0u) * 4u]);
                        float spot_r =
                            la_half_to_float(px[(11u * 64u + 32u) * 4u]);

                        fprintf(stderr, "[dbg] hdr top=%.3f spot=%.1f\n",
                                top_r, spot_r);
                        ok = top_r > 3.5f && top_r < 4.1f &&
                             spot_r > 20.0f;
                    }
                    if (mapped != NULL) {
                        lc_buffer_unmap(staging);
                    }
                    lc_buffer_destroy(staging);
                }
            }
            TEST_CHECK(ok, "hdr: GPU texels match file values");
        }
        TEST_CHECK(la_hdr_load(assets, "nope.hdr", NULL) ==
                       LA_ERROR_NOT_FOUND,
                   "hdr: missing file is NOT_FOUND");
        {
            la_hdr_image_info bad;

            TEST_CHECK(la_hdr_load(NULL, path, &bad) ==
                           LA_ERROR_INVALID_ARGUMENT,
                       "hdr: NULL manager rejected");
        }
    }

    printf("assets vulkan: %d passed, %d failed\n", g_passed, g_failed);
    exit_code = (g_failed == 0) ? 0 : 1;

cleanup:
    /* Idle first: the last presented frame may still reference model
     * and cache resources on the GPU. */
    test_wait_idle(device);
    la_model_destroy(skinned);
    la_model_destroy(again);
    la_model_destroy(box);
    la_model_destroy(gltf);
    la_model_destroy(glb);
    la_asset_manager_destroy(assets);
    lr_renderer_destroy(renderer);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return exit_code;
}
