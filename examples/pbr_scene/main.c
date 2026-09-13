#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>
#include <luma_assets/luma_assets.h>

/*
 * Phase 15 example: a PBR diagnostic scene. A sphere grid sweeps
 * metallic (X) against roughness (Y); an imported glTF box proves
 * the asset bridge; a sine normal-mapped double-sided wall, a
 * factor-only emissive cube, and an unlit ground prove the remaining
 * paths — all under one directional + two point lights with an
 * orbiting camera. No manual LumaC draw commands.
 *
 * Usage: pbr_scene [path/to/model.glb]
 */

#ifndef LA_MODEL_PATH
#define LA_MODEL_PATH "assets/BoxTextured.glb"
#endif

#ifndef LR_PIF
#define LR_PIF 3.14159265358979323846f
#endif

#define GRID_COLS 5
#define GRID_ROWS 4
#define NORMAL_W 64
#define NORMAL_H 64

static unsigned char s_normal_texels[NORMAL_W * NORMAL_H * 4];

/* Deterministic sine normal map (linear UNORM, top-left origin). */
static void fill_sine_normals(void) {
    uint32_t x;
    uint32_t y;
    const float strength = 2.0f;
    const float freq = 2.0f * LR_PIF * 4.0f / (float)NORMAL_W;

    for (y = 0; y < NORMAL_H; y++) {
        for (x = 0; x < NORMAL_W; x++) {
            float fx = (float)x;
            float fy = (float)y;
            float dhdx = cosf(fx * freq) * sinf(fy * freq) * freq *
                         strength;
            float dhdy = sinf(fx * freq) * cosf(fy * freq) * freq *
                         strength;
            float inv = 1.0f / sqrtf(dhdx * dhdx + dhdy * dhdy + 1.0f);
            unsigned char *px = &s_normal_texels[(y * NORMAL_W + x) * 4];

            px[0] = (unsigned char)((-dhdx * inv * 0.5f + 0.5f) * 255.0f);
            px[1] = (unsigned char)((-dhdy * inv * 0.5f + 0.5f) * 255.0f);
            px[2] = (unsigned char)((inv * 0.5f + 0.5f) * 255.0f);
            px[3] = 255;
        }
    }
}

#define FAIL_CLEANUP(msg)                                                 \
    do {                                                                  \
        fprintf(stderr, "%s failed\n", msg);                              \
        goto cleanup;                                                     \
    } while (0)

#define CHECK_LR(expr, what)                                              \
    do {                                                                  \
        if ((expr) != LR_SUCCESS) {                                       \
            fprintf(stderr, "%s failed\n", what);                         \
            goto cleanup;                                                 \
        }                                                                 \
    } while (0)

#define CHECK_LA(expr, what)                                              \
    do {                                                                  \
        if ((expr) != LA_SUCCESS) {                                       \
            fprintf(stderr, "%s failed\n", what);                         \
            goto cleanup;                                                 \
        }                                                                 \
    } while (0)

#define CHECK_LC(expr, what)                                              \
    do {                                                                  \
        lc_result _r = (expr);                                            \
        if (_r != LC_SUCCESS) {                                           \
            fprintf(stderr, "%s failed (%d)\n", what, _r);                \
            goto cleanup;                                                 \
        }                                                                 \
    } while (0)

int main(int argc, char **argv) {
    const char *model_path = (argc > 1) ? argv[1] : LA_MODEL_PATH;
    lc_window *window = NULL;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain *swapchain = NULL;
    lr_renderer *renderer = NULL;
    la_asset_manager *assets = NULL;
    la_model *model = NULL;
    lr_mesh *sphere = NULL;
    lr_mesh *ground_mesh = NULL;
    lr_mesh *wall_mesh = NULL;
    lr_mesh *cube_mesh = NULL;
    lr_material *ground_mat = NULL;
    lr_material *wall_mat = NULL;
    lr_material *glow_mat = NULL;
    lr_material *grid_mats[GRID_ROWS][GRID_COLS];
    lc_image *normal_image = NULL;
    lc_image_view *normal_view = NULL;
    lc_sampler *normal_sampler = NULL;
    lr_camera camera;
    lc_window_desc window_desc;
    lc_device_desc device_desc = { 0 };
    lc_swapchain_desc swapchain_desc;
    la_asset_manager_desc asset_desc;
    unsigned long frame = 0;
    unsigned r;
    unsigned c;
    int exit_code = 1;
    static const float y_axis[3] = { 0.0f, 1.0f, 0.0f };
    static const float x_axis[3] = { 1.0f, 0.0f, 0.0f };
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    static const float look_target[3] = { 0.0f, 1.0f, -1.0f };
    static const float metallics[GRID_COLS] = { 0.0f, 0.25f, 0.5f, 0.75f,
                                                1.0f };
    static const float roughnesses[GRID_ROWS] = { 0.15f, 0.35f, 0.6f,
                                                  0.9f };

    memset(grid_mats, 0, sizeof(grid_mats));
    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    window_desc.title = "Luma PBR Scene";
    window_desc.width = 800;
    window_desc.height = 600;
    if (lc_window_create(&window_desc, &window) != LC_SUCCESS) {
        fprintf(stderr, "lc_window_create failed\n");
        lc_shutdown();
        return 1;
    }

    device_desc.backend = LC_BACKEND_VULKAN;
    device_desc.enable_validation = 1;
    if (lc_device_create(&device_desc, &device) != LC_SUCCESS) {
        fprintf(stderr, "lc_device_create failed\n");
        lc_window_destroy(window);
        lc_shutdown();
        return 1;
    }

    if (lc_surface_create(device, window, &surface) != LC_SUCCESS) {
        fprintf(stderr, "lc_surface_create failed\n");
        FAIL_CLEANUP("lc_surface_create");
    }

    swapchain_desc.width = lc_window_get_width(window);
    swapchain_desc.height = lc_window_get_height(window);
    swapchain_desc.image_count = 0;
    swapchain_desc.vsync = 1;
    if (lc_swapchain_create(device, surface, &swapchain_desc, &swapchain) !=
        LC_SUCCESS) {
        fprintf(stderr, "lc_swapchain_create failed\n");
        FAIL_CLEANUP("lc_swapchain_create");
    }

    {
        lr_renderer_desc rdesc;

        memset(&rdesc, 0, sizeof(rdesc));
        rdesc.device = device;
        if (lc_swapchain_get_render_target_desc(
                swapchain, &rdesc.render_target) != LC_SUCCESS) {
            FAIL_CLEANUP("lc_swapchain_get_render_target_desc");
        }
        rdesc.max_objects = 256;
        CHECK_LR(lr_renderer_create(&rdesc, &renderer), "lr_renderer_create");
    }

    lr_camera_init(&camera);
    CHECK_LR(lr_camera_set_perspective(&camera, 0.7853982f, 800.0f / 600.0f,
                                       0.1f, 100.0f),
             "lr_camera_set_perspective");

    /* Shared meshes (one GPU upload each). */
    CHECK_LR(lr_mesh_create_sphere(renderer, 0.42f, 32, 16, &sphere),
             "sphere mesh");
    CHECK_LR(lr_mesh_create_plane(renderer, 14.0f, 14.0f, &ground_mesh),
             "ground mesh");
    CHECK_LR(lr_mesh_create_plane(renderer, 2.6f, 2.6f, &wall_mesh),
             "wall mesh");
    CHECK_LR(lr_mesh_create_cube(renderer, 0.7f, &cube_mesh), "cube mesh");

    /* Sine normal map (linear UNORM, full mip chain). */
    fill_sine_normals();
    {
        lc_image_desc image_desc;
        lc_image_upload_desc upload;
        lc_image_view_desc view_desc;
        lc_sampler_desc sampler_desc;

        memset(&image_desc, 0, sizeof(image_desc));
        image_desc.type = LC_IMAGE_TYPE_2D;
        image_desc.format = LC_FORMAT_RGBA8_UNORM;
        image_desc.width = NORMAL_W;
        image_desc.height = NORMAL_H;
        image_desc.depth = 1;
        image_desc.mip_levels = 0;
        image_desc.array_layers = 1;
        image_desc.usage = LC_IMAGE_USAGE_SAMPLED |
                           LC_IMAGE_USAGE_TRANSFER_SRC |
                           LC_IMAGE_USAGE_TRANSFER_DST;
        image_desc.samples = LC_SAMPLE_COUNT_1;
        CHECK_LC(lc_image_create(device, &image_desc, &normal_image),
                 "lc_image_create(normal)");
        memset(&upload, 0, sizeof(upload));
        upload.width = NORMAL_W;
        upload.height = NORMAL_H;
        upload.depth = 1;
        upload.data = s_normal_texels;
        upload.data_size = sizeof(s_normal_texels);
        CHECK_LC(lc_image_write(normal_image, &upload),
                 "lc_image_write(normal)");
        CHECK_LC(lc_image_generate_mipmaps(normal_image),
                 "lc_image_generate_mipmaps(normal)");
        memset(&view_desc, 0, sizeof(view_desc));
        view_desc.type = LC_IMAGE_VIEW_2D;
        view_desc.aspect = LC_IMAGE_ASPECT_COLOR;
        view_desc.mip_level_count = lc_image_get_mip_levels(normal_image);
        view_desc.array_layer_count = 1;
        CHECK_LC(lc_image_view_create(normal_image, &view_desc,
                                      &normal_view),
                 "lc_image_view_create(normal)");
        memset(&sampler_desc, 0, sizeof(sampler_desc));
        sampler_desc.min_filter = LC_FILTER_LINEAR;
        sampler_desc.mag_filter = LC_FILTER_LINEAR;
        sampler_desc.mipmap_mode = LC_MIPMAP_MODE_LINEAR;
        sampler_desc.address_u = LC_ADDRESS_REPEAT;
        sampler_desc.address_v = LC_ADDRESS_REPEAT;
        sampler_desc.max_anisotropy = 1.0f;
        CHECK_LC(lc_sampler_create(device, &sampler_desc, &normal_sampler),
                 "lc_sampler_create(normal)");
    }

    /* Diagnostic grid: metallic on X, roughness on Y. */
    for (r = 0; r < GRID_ROWS; r++) {
        for (c = 0; c < GRID_COLS; c++) {
            lr_pbr_material_desc mdesc;

            memset(&mdesc, 0, sizeof(mdesc));
            mdesc.base_color_factor[0] = 1.0f;
            mdesc.base_color_factor[1] = 1.0f;
            mdesc.base_color_factor[2] = 1.0f;
            mdesc.base_color_factor[3] = 1.0f;
            mdesc.metallic_factor = metallics[c];
            mdesc.roughness_factor = roughnesses[r];
            mdesc.normal_scale = 1.0f;
            mdesc.occlusion_strength = 1.0f;
            CHECK_LR(lr_material_create_pbr(renderer, &mdesc,
                                            &grid_mats[r][c]),
                     "grid material");
        }
    }

    /* Normal-mapped double-sided wall (exercises TBN + backfaces). */
    {
        lr_pbr_material_desc mdesc;

        memset(&mdesc, 0, sizeof(mdesc));
        mdesc.base_color_factor[0] = 0.75f;
        mdesc.base_color_factor[1] = 0.72f;
        mdesc.base_color_factor[2] = 0.70f;
        mdesc.base_color_factor[3] = 1.0f;
        mdesc.metallic_factor = 0.0f;
        mdesc.roughness_factor = 0.55f;
        mdesc.normal_texture = normal_view;
        mdesc.sampler = normal_sampler;
        mdesc.normal_scale = 1.0f;
        mdesc.occlusion_strength = 1.0f;
        mdesc.double_sided = 1;
        CHECK_LR(lr_material_create_pbr(renderer, &mdesc, &wall_mat),
                 "wall material");
    }

    /* Factor-only emissive cube (no emissive texture: proves the
     * white emissive fallback keeps factors working). */
    {
        lr_pbr_material_desc mdesc;

        memset(&mdesc, 0, sizeof(mdesc));
        mdesc.base_color_factor[0] = 0.02f;
        mdesc.base_color_factor[1] = 0.02f;
        mdesc.base_color_factor[2] = 0.02f;
        mdesc.base_color_factor[3] = 1.0f;
        mdesc.metallic_factor = 0.0f;
        mdesc.roughness_factor = 0.9f;
        mdesc.emissive_factor[0] = 1.0f;
        mdesc.emissive_factor[1] = 0.45f;
        mdesc.emissive_factor[2] = 0.12f;
        mdesc.normal_scale = 1.0f;
        mdesc.occlusion_strength = 1.0f;
        CHECK_LR(lr_material_create_pbr(renderer, &mdesc, &glow_mat),
                 "emissive material");
    }

    /* Unlit ground (the old path coexists with PBR draws). */
    {
        lr_unlit_material_desc mdesc;

        memset(&mdesc, 0, sizeof(mdesc));
        mdesc.color[0] = 0.16f;
        mdesc.color[1] = 0.17f;
        mdesc.color[2] = 0.20f;
        mdesc.color[3] = 1.0f;
        CHECK_LR(lr_material_create_unlit(renderer, &mdesc, &ground_mat),
                 "ground material");
    }

    /* Imported glTF model (real PBR metadata through Luma Assets). */
    memset(&asset_desc, 0, sizeof(asset_desc));
    asset_desc.renderer = renderer;
    CHECK_LA(la_asset_manager_create(&asset_desc, &assets),
             "la_asset_manager_create");
    if (la_model_load(assets, model_path, &model) != LA_SUCCESS) {
        fprintf(stderr, "la_model_load('%s') failed: %s\n", model_path,
                la_asset_manager_get_last_error(assets));
        goto cleanup;
    }

    printf("LumaC %s\n", lc_get_version_string());
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("PBR scene: %dx%d grid + imported '%s' + normal wall + "
           "emissive + unlit ground. Close to exit.\n",
           GRID_COLS, GRID_ROWS, model_path);

    while (!lc_window_should_close(window)) {
        uint32_t w;
        uint32_t h;
        lc_command_encoder *enc = NULL;
        lc_render_swapchain_pass_desc spass;
        lr_draw_item item;
        lc_result res;
        float t;
        float eye[3];

        lc_poll_events();
        w = lc_window_get_width(window);
        h = lc_window_get_height(window);
        if (w == 0 || h == 0) {
            continue;
        }
        if (w != lc_swapchain_get_width(swapchain) ||
            h != lc_swapchain_get_height(swapchain)) {
            res = lc_swapchain_recreate(swapchain, w, h);
            if (res == LC_ERROR_ZERO_EXTENT) {
                continue;
            }
            if (res != LC_SUCCESS) {
                fprintf(stderr, "lc_swapchain_recreate failed (%d)\n", res);
                goto cleanup;
            }
        }

        res = lc_begin_frame(swapchain);
        if (res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            res = lc_swapchain_recreate(swapchain, w, h);
            if (res != LC_SUCCESS && res != LC_ERROR_ZERO_EXTENT) {
                fprintf(stderr, "lc_swapchain_recreate failed (%d)\n", res);
                goto cleanup;
            }
            continue;
        }
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_begin_frame failed (%d)\n", res);
            goto cleanup;
        }
        CHECK_LC(lc_swapchain_get_encoder(swapchain, &enc),
                 "lc_swapchain_get_encoder");

        t = (float)frame * 0.01f;

        /* Orbiting camera. */
        eye[0] = 4.5f * sinf(t * 0.12f);
        eye[1] = 2.3f;
        eye[2] = 4.5f * cosf(t * 0.12f) + 0.5f;
        CHECK_LR(lr_camera_look_at(&camera, eye, look_target, up),
                 "lr_camera_look_at");

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.clear_color[0] = 0.03f;
        spass.clear_color[1] = 0.035f;
        spass.clear_color[2] = 0.05f;
        spass.clear_color[3] = 1.0f;
        spass.depth_load_op = LC_LOAD_OP_CLEAR;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        CHECK_LC(lc_encoder_begin_swapchain_pass(enc, swapchain, &spass),
                 "swapchain pass begin");

        if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
            fprintf(stderr, "lr_renderer_begin failed\n");
            goto cleanup;
        }

        /* Lights: warm key + static fill point + orbiting cool point. */
        {
            lr_light key;
            lr_light fill;
            lr_light orbiter;

            memset(&key, 0, sizeof(key));
            key.type = LR_LIGHT_DIRECTIONAL;
            key.color[0] = 1.0f;
            key.color[1] = 0.93f;
            key.color[2] = 0.84f;
            key.intensity = 2.5f;
            key.direction[0] = 0.35f;
            key.direction[1] = -1.0f;
            key.direction[2] = 0.30f;
            CHECK_LR(lr_renderer_submit_light(renderer, &key), "key light");

            memset(&fill, 0, sizeof(fill));
            fill.type = LR_LIGHT_POINT;
            fill.color[0] = 1.0f;
            fill.color[1] = 0.72f;
            fill.color[2] = 0.50f;
            fill.intensity = 22.0f;
            fill.position[0] = 3.0f;
            fill.position[1] = 2.6f;
            fill.position[2] = 2.2f;
            fill.range = 12.0f;
            CHECK_LR(lr_renderer_submit_light(renderer, &fill),
                     "fill light");

            memset(&orbiter, 0, sizeof(orbiter));
            orbiter.type = LR_LIGHT_POINT;
            orbiter.color[0] = 0.45f;
            orbiter.color[1] = 0.60f;
            orbiter.color[2] = 1.0f;
            orbiter.intensity = 16.0f;
            orbiter.position[0] = 3.4f * cosf(t * 0.45f);
            orbiter.position[1] = 1.9f + 0.5f * sinf(t * 0.9f);
            orbiter.position[2] = 3.4f * sinf(t * 0.45f) - 1.0f;
            orbiter.range = 10.0f;
            CHECK_LR(lr_renderer_submit_light(renderer, &orbiter),
                     "orbiter light");
        }

        /* Grid spheres. */
        memset(&item, 0, sizeof(item));
        item.mesh = sphere;
        for (r = 0; r < GRID_ROWS; r++) {
            for (c = 0; c < GRID_COLS; c++) {
                lr_transform_identity(&item.transform);
                item.transform.position[0] = ((float)c - 2.0f) * 1.05f;
                item.transform.position[1] = 2.75f - (float)r * 1.05f;
                item.transform.position[2] = -2.5f;
                item.material = grid_mats[r][c];
                if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
                    fprintf(stderr, "submit grid failed\n");
                    goto cleanup;
                }
            }
        }

        /* Normal-mapped wall (faces +Z, double-sided for the orbit). */
        memset(&item, 0, sizeof(item));
        lr_transform_identity(&item.transform);
        item.transform.position[0] = -3.4f;
        item.transform.position[1] = 1.3f;
        item.transform.position[2] = -0.6f;
        lr_quat_from_axis_angle(x_axis, LR_PIF * 0.5f,
                                item.transform.rotation);
        item.mesh = wall_mesh;
        item.material = wall_mat;
        if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
            fprintf(stderr, "submit wall failed\n");
            goto cleanup;
        }

        /* Emissive cube. */
        lr_transform_identity(&item.transform);
        item.transform.position[0] = 3.4f;
        item.transform.position[1] = 0.55f;
        item.transform.position[2] = -0.6f;
        lr_quat_from_axis_angle(y_axis, t * 0.8f, item.transform.rotation);
        item.mesh = cube_mesh;
        item.material = glow_mat;
        if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
            fprintf(stderr, "submit emissive failed\n");
            goto cleanup;
        }

        /* Imported model on a slow turntable. */
        {
            lr_transform root;

            lr_transform_identity(&root);
            root.position[0] = 0.0f;
            root.position[1] = 0.15f;
            root.position[2] = 1.6f;
            root.scale[0] = 1.4f;
            root.scale[1] = 1.4f;
            root.scale[2] = 1.4f;
            lr_quat_from_axis_angle(y_axis, t * 0.5f, root.rotation);
            if (la_model_submit(model, renderer, &root) != LA_SUCCESS) {
                fprintf(stderr, "la_model_submit failed\n");
                goto cleanup;
            }
        }

        /* Unlit ground last (sorts by material regardless). */
        lr_transform_identity(&item.transform);
        item.transform.position[1] = -0.55f;
        item.mesh = ground_mesh;
        item.material = ground_mat;
        if (lr_renderer_submit(renderer, &item) != LR_SUCCESS) {
            fprintf(stderr, "submit ground failed\n");
            goto cleanup;
        }

        if (lr_renderer_render(renderer, enc,
                               lc_swapchain_get_render_target(swapchain)) !=
            LR_SUCCESS) {
            fprintf(stderr, "lr_renderer_render failed\n");
            goto cleanup;
        }
        lr_renderer_end(renderer);
        CHECK_LC(lc_encoder_end_render_pass(enc), "pass end");

        res = lc_end_frame(swapchain);
        if (res == LC_SUBOPTIMAL ||
            res == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
            res = lc_swapchain_recreate(swapchain, w, h);
            if (res != LC_SUCCESS && res != LC_ERROR_ZERO_EXTENT) {
                fprintf(stderr, "lc_swapchain_recreate failed (%d)\n", res);
                goto cleanup;
            }
            continue;
        }
        if (res != LC_SUCCESS) {
            fprintf(stderr, "lc_end_frame failed (%d)\n", res);
            goto cleanup;
        }
        if ((frame % 120u) == 0u) {
            lr_render_stats stats;

            lr_renderer_get_stats(renderer, &stats);
            printf("frame %lu: submitted=%u visible=%u draws=%u "
                   "(pbr=%u unlit=%u) tris=%u pipes=%u mats=%u "
                   "lights=%u/%u\n",
                   frame, stats.submitted_objects, stats.visible_objects,
                   stats.draw_calls, stats.pbr_draw_calls,
                   stats.unlit_draw_calls, stats.triangles,
                   stats.pipeline_binds, stats.material_binds,
                   stats.active_lights, stats.submitted_lights);
        }
        frame++;
    }

    exit_code = 0;

cleanup:
    /* Drain the last frame before tearing down GPU objects. */
    if (device != NULL) {
        lc_device_wait_idle(device);
    }
    for (r = 0; r < GRID_ROWS; r++) {
        for (c = 0; c < GRID_COLS; c++) {
            lr_material_destroy(grid_mats[r][c]);
        }
    }
    lr_material_destroy(glow_mat);
    lr_material_destroy(wall_mat);
    lr_material_destroy(ground_mat);
    lr_mesh_destroy(cube_mesh);
    lr_mesh_destroy(wall_mesh);
    lr_mesh_destroy(ground_mesh);
    lr_mesh_destroy(sphere);
    la_model_destroy(model);
    la_asset_manager_destroy(assets);
    lr_renderer_destroy(renderer);
    lc_sampler_destroy(normal_sampler);
    lc_image_view_destroy(normal_view);
    lc_image_destroy(normal_image);
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return exit_code;
}
