/* Phase 23 visibility scene: a large occluded city proving GPU
 * frustum + Hi-Z occlusion culling, GPU LOD, and indirect-count
 * draws at scale through PBR.
 *
 * Blocks of tall buildings form occluding streets; LOD domes fill
 * a plaza and hide behind the blocks; an orbiting camera drives
 * frustum, occlusion, and LOD changes every frame. Counters print
 * totals, rejects, LOD mix, indirect work, and triangles.
 *
 * Usage: visibility_scene [--frames N] [--instances N] [--no-hiz]
 *   [--no-lod] [--debug-hiz] [--no-validation] [--screenshot out.png]
 *
 * --instances N counts scene-generation CANDIDATES: lots on the
 * street grid are intentionally left open (~half of candidates),
 * so submitted == candidates - street_skipped - submit_failed
 * (plus 1 ground slab + 8 trucks). Frame 0 prints the exact
 * breakdown; renderer stats then satisfy
 * submitted == frustum_rejected + occlusion_rejected + visible
 * up to LOD-compaction grouping.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>

#include "png_mini.h"

#define FAIL_CLEANUP(what) do {                                         \
    fprintf(stderr, "%s failed\n", what);                               \
    goto cleanup;                                                       \
} while (0)

static float hash01(uint32_t n) {
    uint32_t x = n * 2654435761u;

    x ^= x >> 15;
    x *= 2246822519u;
    x ^= x >> 13;
    return (float)(x & 0xFFFFFFu) / 16777216.0f;
}

/* 9x9 dome grid (shared-vertex LODs decimate cleanly). */
enum { DOME_GRID = 9 };

static void dome_vertex(lr_vertex *v, int ix, int iz) {
    float x = -3.0f + 0.75f * (float)ix;
    float z = -3.0f + 0.75f * (float)iz;
    float r = sqrtf(x * x + z * z) / 4.25f;
    float y = (r < 1.0f) ? 2.5f * (1.0f - r * r) : 0.0f;

    memset(v, 0, sizeof(*v));
    v->position[0] = x;
    v->position[1] = y;
    v->position[2] = z;
    v->normal[1] = 1.0f;
    v->tangent[0] = 1.0f;
    v->tangent[3] = 1.0f;
    v->texcoord[0] = (float)ix / 8.0f;
    v->texcoord[1] = (float)iz / 8.0f;
}

static uint32_t dome_indices(uint32_t *out, int step) {
    uint32_t n = 0;
    int iz;
    int ix;

    for (iz = 0; iz + step < DOME_GRID; iz += step) {
        for (ix = 0; ix + step < DOME_GRID; ix += step) {
            uint32_t a = (uint32_t)(iz * DOME_GRID + ix);
            uint32_t b = (uint32_t)(iz * DOME_GRID + ix + step);
            uint32_t c =
                (uint32_t)((iz + step) * DOME_GRID + ix);
            uint32_t d =
                (uint32_t)((iz + step) * DOME_GRID + ix + step);

            if (out != NULL) {
                out[n + 0] = a;
                out[n + 1] = b;
                out[n + 2] = c;
                out[n + 3] = b;
                out[n + 4] = d;
                out[n + 5] = c;
            }
            n += 6;
        }
    }
    return n;
}

static int submit_prop(lr_renderer *renderer, lr_mesh *mesh,
                       lr_material *mat, float x, float y, float z,
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
    item.mesh = mesh;
    item.material = mat;
    item.casts_shadow = 0;
    item.receives_shadow = 1;
    return (lr_renderer_submit(renderer, &item) == LR_SUCCESS) ? 1 : 0;
}

int main(int argc, char **argv) {
    unsigned long max_frames = 600;
    unsigned long want_instances = 5000;
    int use_hiz = 1;
    int use_lod = 1;
    int debug_hiz = 0;
    int use_validation = 1;
    int use_vsync = 1;
    const char *screenshot_path = NULL;
    lc_window_desc window_desc;
    lc_window *window = NULL;
    lc_device_desc device_desc;
    lc_device *device = NULL;
    lc_surface *surface = NULL;
    lc_swapchain_desc swapchain_desc;
    lc_swapchain *swapchain = NULL;
    lr_renderer_desc rdesc;
    lr_renderer *renderer = NULL;
    lr_mesh *box_mesh = NULL;
    lr_mesh *dome_mesh = NULL;
    lr_mesh *ground_mesh = NULL;
    lr_material *box_mat = NULL;
    lr_material *dome_mat = NULL;
    lr_material *ground_mat = NULL;
    lr_visibility_settings vis;
    unsigned long frame = 0;
    unsigned long i;
    uint64_t perf_freq = lc_clock_frequency();
    /* Screenshot capture spans the frame boundary: the output
     * pass records inside frame 0, but readback must wait until
     * after present (never read a pending producer). */
    lc_image *shot_img = NULL;
    lc_image_view *shot_view = NULL;
    lc_render_target *shot_target = NULL;
    uint32_t shot_w = 0;
    uint32_t shot_h = 0;
    int ok = 1;

    if (perf_freq == 0) {
        perf_freq = 1;
    }
    for (i = 1; i < (unsigned long)argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--instances") == 0 &&
                   i + 1 < argc) {
            want_instances = strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--no-hiz") == 0) {
            use_hiz = 0;
        } else if (strcmp(argv[i], "--no-lod") == 0) {
            use_lod = 0;
        } else if (strcmp(argv[i], "--debug-hiz") == 0) {
            debug_hiz = 1;
        } else if (strcmp(argv[i], "--screenshot") == 0 &&
                   i + 1 < argc) {
            screenshot_path = argv[++i];
        } else if (strcmp(argv[i], "--no-validation") == 0) {
            use_validation = 0;
        } else if (strcmp(argv[i], "--no-vsync") == 0) {
            use_vsync = 0;
        } else {
            fprintf(stderr,
                    "usage: visibility_scene [--frames N] "
                    "[--instances N] [--no-hiz] [--no-lod] "
                    "[--debug-hiz] [--no-validation] [--no-vsync] "
                    "[--screenshot out.png]\n");
            return 1;
        }
    }
    if (want_instances < 1) {
        want_instances = 1;
    }
    if (want_instances > 200000) {
        want_instances = 200000;
    }
    memset(&window_desc, 0, sizeof(window_desc));
    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }
    window_desc.title = "Luma Visibility Scene";
    window_desc.width = 800;
    window_desc.height = 600;
    if (lc_window_create(&window_desc, &window) != LC_SUCCESS) {
        fprintf(stderr, "lc_window_create failed\n");
        lc_shutdown();
        return 1;
    }
    memset(&device_desc, 0, sizeof(device_desc));
    device_desc.backend = LC_BACKEND_VULKAN;
    device_desc.enable_validation = use_validation;
    if (lc_device_create(&device_desc, &device) != LC_SUCCESS) {
        FAIL_CLEANUP("lc_device_create");
    }
    {
        lc_compute_capabilities caps;

        memset(&caps, 0, sizeof(caps));
        lc_device_get_compute_capabilities(device, &caps);
        printf("compute=%d indirect=%d multi=%d indirect_count=%d\n",
               caps.compute_supported, caps.indirect_draw_supported,
               caps.multi_draw_indirect, caps.indirect_count);
        if (!caps.compute_supported ||
            !caps.indirect_draw_supported) {
            FAIL_CLEANUP("capabilities");
        }
    }
    if (lc_surface_create(device, window, &surface) != LC_SUCCESS) {
        FAIL_CLEANUP("lc_surface_create");
    }
    memset(&swapchain_desc, 0, sizeof(swapchain_desc));
    swapchain_desc.width = lc_window_get_width(window);
    swapchain_desc.height = lc_window_get_height(window);
    swapchain_desc.image_count = 0;
    swapchain_desc.vsync = use_vsync;
    if (lc_swapchain_create(device, surface, &swapchain_desc,
                            &swapchain) != LC_SUCCESS) {
        FAIL_CLEANUP("lc_swapchain_create");
    }
    memset(&rdesc, 0, sizeof(rdesc));
    rdesc.device = device;
    rdesc.render_target.color_attachment_count = 1;
    rdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    rdesc.render_target.depth_stencil_format = LC_FORMAT_D32_FLOAT;
    rdesc.render_target.samples = LC_SAMPLE_COUNT_1;
    rdesc.max_objects = (uint32_t)want_instances + 8192u;
    rdesc.ambient_light[0] = 0.35f;
    rdesc.ambient_light[1] = 0.35f;
    rdesc.ambient_light[2] = 0.4f;
    if (lr_renderer_create(&rdesc, &renderer) != LR_SUCCESS) {
        FAIL_CLEANUP("lr_renderer_create");
    }
    if (lr_mesh_create_cube(renderer, 2.0f, &box_mesh) !=
        LR_SUCCESS) {
        FAIL_CLEANUP("box mesh");
    }
    if (lr_mesh_create_plane(renderer, 240.0f, 240.0f,
                             &ground_mesh) != LR_SUCCESS) {
        FAIL_CLEANUP("ground mesh");
    }
    /* Dome with three real decimated levels. */
    {
        lr_vertex verts[DOME_GRID * DOME_GRID];
        uint32_t full[8 * 8 * 6];
        uint32_t mid[4 * 4 * 6];
        uint32_t coarse[2 * 2 * 6];
        uint32_t tip[3] = { 0, 8, 72 };
        lr_mesh_desc mdesc;
        int ix;
        int iz;

        for (iz = 0; iz < DOME_GRID; iz++) {
            for (ix = 0; ix < DOME_GRID; ix++) {
                dome_vertex(&verts[iz * DOME_GRID + ix], ix, iz);
            }
        }
        memset(&mdesc, 0, sizeof(mdesc));
        mdesc.vertices = verts;
        mdesc.vertex_count = DOME_GRID * DOME_GRID;
        mdesc.indices = full;
        mdesc.index_count = dome_indices(full, 1);
        if (lr_mesh_create(renderer, &mdesc, &dome_mesh) !=
            LR_SUCCESS) {
            FAIL_CLEANUP("dome mesh");
        }
        if (lr_mesh_add_lod(dome_mesh, mid, dome_indices(mid, 2),
                            200.0f) != LR_SUCCESS ||
            lr_mesh_add_lod(dome_mesh, coarse,
                            dome_indices(coarse, 4),
                            80.0f) != LR_SUCCESS ||
            lr_mesh_add_lod(dome_mesh, tip, 3, 25.0f) !=
                LR_SUCCESS) {
            FAIL_CLEANUP("dome lods");
        }
    }
    {
        lr_pbr_material_desc matdesc;
        static const float box_color[4] = { 0.55f, 0.58f, 0.62f,
                                            1.0f };
        static const float dome_color[4] = { 0.25f, 0.55f, 0.3f,
                                             1.0f };
        static const float ground_color[4] = { 0.16f, 0.16f,
                                               0.18f, 1.0f };

        memset(&matdesc, 0, sizeof(matdesc));
        memcpy(matdesc.base_color_factor, box_color,
               sizeof(box_color));
        matdesc.metallic_factor = 0.0f;
        matdesc.roughness_factor = 0.7f;
        matdesc.normal_scale = 1.0f;
        matdesc.occlusion_strength = 1.0f;
        matdesc.alpha_mode = LR_ALPHA_OPAQUE;
        if (lr_material_create_pbr(renderer, &matdesc, &box_mat) !=
            LR_SUCCESS) {
            FAIL_CLEANUP("box material");
        }
        memcpy(matdesc.base_color_factor, dome_color,
               sizeof(dome_color));
        if (lr_material_create_pbr(renderer, &matdesc,
                                   &dome_mat) != LR_SUCCESS) {
            FAIL_CLEANUP("dome material");
        }
        memcpy(matdesc.base_color_factor, ground_color,
               sizeof(ground_color));
        if (lr_material_create_pbr(renderer, &matdesc,
                                   &ground_mat) != LR_SUCCESS) {
            FAIL_CLEANUP("ground material");
        }
    }
    if (lr_renderer_set_render_mode(renderer,
                                    LR_RENDER_MODE_GPU_DRIVEN) !=
        LR_SUCCESS) {
        FAIL_CLEANUP("gpu-driven mode");
    }
    lr_visibility_settings_default(&vis);
    vis.enabled = 1;
    vis.hiz_enabled = use_hiz;
    vis.lod_enabled = use_lod;
    vis.indirect_count_enabled = 1;
    vis.graph_enabled = 1;
    if (lr_renderer_set_visibility(renderer, &vis) != LR_SUCCESS) {
        FAIL_CLEANUP("visibility settings");
    }
    printf("visibility: hiz=%d lod=%d instances=%lu frames=%lu\n",
           use_hiz, use_lod, want_instances, max_frames);
    for (frame = 0; frame < max_frames; frame++) {
        lc_command_encoder *enc = NULL;
        lr_camera camera;
        float t = (float)frame * 0.02f;
        float eye[3] = { 60.0f * cosf(t), 38.0f + 2.0f * sinf(t * 0.7f),
                         60.0f * sinf(t) };
        static const float look[3] = { 0.0f, 2.0f, 0.0f };
        static const float up[3] = { 0.0f, 1.0f, 0.0f };
        unsigned long n;
        uint32_t w;
        uint32_t h;

        lc_poll_events();
        if (lc_begin_frame(swapchain) != LC_SUCCESS ||
            lc_swapchain_get_encoder(swapchain, &enc) !=
                LC_SUCCESS) {
            FAIL_CLEANUP("begin_frame");
        }
        lr_camera_init(&camera);
        lr_camera_set_perspective(&camera, 1.0471976f, 4.0f / 3.0f,
                                  0.5f, 400.0f);
        lr_camera_look_at(&camera, eye, look, up);
        if (lr_renderer_begin(renderer, &camera) != LR_SUCCESS) {
            FAIL_CLEANUP("renderer_begin");
        }
        /* Key light with shadows (proves shadow maps coexist with
         * visibility) plus a warm fill. */
        {
            lr_light key;
            lr_light fill;

            memset(&key, 0, sizeof(key));
            key.type = LR_LIGHT_DIRECTIONAL;
            key.color[0] = 1.0f;
            key.color[1] = 1.0f;
            key.color[2] = 1.0f;
            key.intensity = 3.0f;
            key.direction[0] = 0.5f;
            key.direction[1] = -1.0f;
            key.direction[2] = 0.3f;
            key.shadow.enabled = 1;
            key.shadow.resolution = 1024;
            key.shadow.depth_bias = -1.0f;
            key.shadow.normal_bias = -1.0f;
            if (lr_renderer_submit_light(renderer, &key) !=
                LR_SUCCESS) {
                FAIL_CLEANUP("key light");
            }
            memset(&fill, 0, sizeof(fill));
            fill.type = LR_LIGHT_POINT;
            fill.color[0] = 1.0f;
            fill.color[1] = 0.75f;
            fill.color[2] = 0.5f;
            fill.intensity = 200.0f;
            fill.position[0] = 10.0f;
            fill.position[1] = 12.0f;
            fill.position[2] = 6.0f;
            fill.range = 60.0f;
            if (lr_renderer_submit_light(renderer, &fill) !=
                LR_SUCCESS) {
                FAIL_CLEANUP("fill light");
            }
        }
        /* Ground slab. */
        {
            /* Stage-51 accounting invariant: generated candidates =
             * submitted + intentionally rejected before the renderer
             * (street filter + submit failures). Every category is
             * reported so --instances N semantics stay exact. */
            unsigned long submitted = 0;
            unsigned long street_skipped = 0;
            unsigned long submit_failed = 0;

            if (!submit_prop(renderer, ground_mesh, ground_mat, 0.0f,
                             -0.5f, 0.0f, 1.0f, 1.0f, 1.0f)) {
                submit_failed++;
            } else {
                submitted++;
            }
            /* City blocks: tall boxes on a grid leave occluding
             * streets; deterministic sizes from the hash. */
            for (n = 0; n < want_instances; n++) {
                float hx = hash01((uint32_t)(n * 2u));
                float hz = hash01((uint32_t)(n * 2u + 1u));
                float bx = -60.0f + 120.0f * hx;
                float bz = -60.0f + 120.0f * hz;
                float lane_x = fmodf(bx + 60.0f, 20.0f);
                float lane_z = fmodf(bz + 60.0f, 20.0f);

                if (lane_x < 6.0f || lane_z < 6.0f) {
                    street_skipped++; /* streets stay open */
                    continue;
                }
                if ((n % 3u) == 0u) {
                    /* LOD dome (plaza + behind-block scatter). */
                    float s = 1.0f + 2.0f * hash01((uint32_t)n + 7u);

                    if (!submit_prop(renderer, dome_mesh, dome_mat,
                                     bx, 0.0f, bz, s, s, s)) {
                        submit_failed++;
                    } else {
                        submitted++;
                    }
                } else {
                    /* Tower block (heavy occluder). */
                    float sx = 2.5f + 2.0f * hx;
                    float sy = 5.0f + 14.0f * hz;
                    float sz = 2.5f + 2.0f * hash01((uint32_t)n + 3u);

                    if (!submit_prop(renderer, box_mesh, box_mat, bx,
                                     sy - 0.5f, bz, sx, sy, sz)) {
                        submit_failed++;
                    } else {
                        submitted++;
                    }
                }
            }
            /* Moving occluder trucks cross the avenues (dynamic
             * occluders join the Hi-Z every frame they are drawn). */
            for (n = 0; n < 8u; n++) {
                float lane = -45.0f + 15.0f * (float)n;
                float span = 130.0f;
                float ph = fmodf(t * (6.0f + (float)n) + span,
                                  span * 2.0f) -
                           span;

                if (!submit_prop(renderer, box_mesh, box_mat, ph, 3.0f,
                                 lane, 6.0f, 3.5f, 2.5f)) {
                    submit_failed++;
                } else {
                    submitted++;
                }
            }
            if (frame == 0) {
                /* +1 ground slab, +8 trucks beyond the candidates. */
                printf("instances: candidates=%lu submitted=%lu "
                       "street_skipped=%lu submit_failed=%lu "
                       "(+1 ground +8 trucks in submitted)\n",
                       want_instances, submitted, street_skipped,
                       submit_failed);
            }
        }
        w = lc_window_get_width(window);
        h = lc_window_get_height(window);
        if (w == 0 || h == 0) {
            w = 800;
            h = 600;
        }
        /* Shadow maps first (passes cannot nest: depth work runs
         * before the HDR scene pass opens). */
        if (lr_renderer_render_shadows(renderer, enc) !=
            LR_SUCCESS) {
            FAIL_CLEANUP("render_shadows");
        }
        if (lr_renderer_render_scene(renderer, enc, w, h) !=
            LR_SUCCESS) {
            FAIL_CLEANUP("render_scene");
        }
        /* Screenshot leg: tonemap into an offscreen LDR target
         * (documented render_output use), then present. */
        if (screenshot_path != NULL && frame + 1u == max_frames) {
            lc_image_desc idesc;
            lc_image_view_desc svdesc;
            lc_render_target_attachment ratt;
            lc_render_target_create_desc rtdesc;
            lc_render_color_attachment satt;
            lc_render_pass_desc spass;

            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = w;
            idesc.height = h;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                          LC_IMAGE_USAGE_SAMPLED |
                          LC_IMAGE_USAGE_TRANSFER_SRC |
                          LC_IMAGE_USAGE_TRANSFER_DST;
            idesc.samples = LC_SAMPLE_COUNT_1;
            if (lc_image_create(device, &idesc, &shot_img) !=
                    LC_SUCCESS) {
                FAIL_CLEANUP("screenshot target");
            }
            memset(&svdesc, 0, sizeof(svdesc));
            svdesc.type = LC_IMAGE_VIEW_2D;
            svdesc.aspect = LC_IMAGE_ASPECT_COLOR;
            svdesc.mip_level_count = 1;
            svdesc.array_layer_count = 1;
            if (lc_image_view_create(shot_img, &svdesc,
                                     &shot_view) != LC_SUCCESS) {
                FAIL_CLEANUP("screenshot target");
            }
            memset(&rtdesc, 0, sizeof(rtdesc));
            ratt.view = shot_view;
            rtdesc.color_attachments = &ratt;
            rtdesc.color_attachment_count = 1;
            rtdesc.width = w;
            rtdesc.height = h;
            if (lc_render_target_create(device, &rtdesc,
                                        &shot_target) !=
                LC_SUCCESS) {
                FAIL_CLEANUP("screenshot target");
            }
            memset(&satt, 0, sizeof(satt));
            satt.view = shot_view;
            satt.load_op = LC_LOAD_OP_CLEAR;
            satt.store_op = LC_STORE_OP_STORE;
            memset(&spass, 0, sizeof(spass));
            spass.color_attachments = &satt;
            spass.color_attachment_count = 1;
            spass.width = w;
            spass.height = h;
            if (lc_encoder_begin_render_pass(enc, &spass) !=
                    LC_SUCCESS ||
                lr_renderer_render_output(renderer, enc,
                                          shot_target) !=
                    LR_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                FAIL_CLEANUP("screenshot output");
            }
            shot_w = w;
            shot_h = h;
        }
        {
            lc_render_swapchain_pass_desc spass;

            memset(&spass, 0, sizeof(spass));
            spass.color_load_op = LC_LOAD_OP_CLEAR;
            spass.color_store_op = LC_STORE_OP_STORE;
            spass.depth_load_op = LC_LOAD_OP_CLEAR;
            spass.depth_store_op = LC_STORE_OP_DONT_CARE;
            spass.clear_depth = 1.0f;
            if (lc_encoder_begin_swapchain_pass(enc, swapchain,
                                                &spass) !=
                    LC_SUCCESS ||
                lr_renderer_render_output(
                    renderer, enc,
                    lc_swapchain_get_render_target(swapchain)) !=
                    LR_SUCCESS ||
                lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
                FAIL_CLEANUP("present");
            }
        }
        lr_renderer_end(renderer);
        if (lc_end_frame(swapchain) != LC_SUCCESS) {
            /* workers may report suboptimal; keep running */
        }
        if (lr_renderer_update_visibility_stats(renderer) !=
            LR_SUCCESS) {
            FAIL_CLEANUP("visibility stats");
        }
        {
            lr_visibility_stats vs;

            lr_renderer_get_visibility_stats(renderer, &vs);
            if (frame % 60u == 0u || frame + 1u == max_frames) {
                lc_memory_stats ms;

                lc_device_get_memory_stats(device, &ms);
                printf("frame %lu: total=%llu frustum=%llu "
                       "occluded=%llu visible=%llu lod0=%llu "
                       "lod1=%llu lod2=%llu lod3=%llu indirect=%llu "
                       "draws=%llu tris=%llu dispatches=%llu "
                       "allocs=%llu blocks=%llu\n",
                       frame,
                       (unsigned long long)vs.total_instances,
                       (unsigned long long)vs.frustum_rejected,
                       (unsigned long long)vs.occlusion_rejected,
                       (unsigned long long)vs.visible,
                       (unsigned long long)vs.lod_visible[0],
                       (unsigned long long)vs.lod_visible[1],
                       (unsigned long long)vs.lod_visible[2],
                       (unsigned long long)vs.lod_visible[3],
                       (unsigned long long)vs.indirect_commands,
                       (unsigned long long)vs.indirect_draw_calls,
                       (unsigned long long)vs.triangles_submitted,
                       (unsigned long long)vs.compute_dispatches,
                       (unsigned long long)ms.allocation_count,
                       (unsigned long long)ms.block_count);
            }
        }
        /* Post-present captures (producers submitted; readback is
         * safe here, never inside the frame). */
        if (screenshot_path != NULL && frame + 1u == max_frames &&
            shot_target != NULL) {
            lc_image_readback_desc rbdesc;
            lc_image_readback_info rbinfo;
            unsigned char *rgba = NULL;
            unsigned char *rgb = NULL;
            uint32_t x;
            uint32_t y;

            memset(&rbdesc, 0, sizeof(rbdesc));
            if (lc_image_query_readback(shot_img, &rbdesc,
                                        &rbinfo) != LC_SUCCESS) {
                FAIL_CLEANUP("screenshot query");
            }
            rgba = (unsigned char *)malloc(rbinfo.size);
            rgb = (unsigned char *)malloc((size_t)shot_w *
                                          shot_h * 3u);
            if (rgba == NULL || rgb == NULL) {
                free(rgba);
                free(rgb);
                FAIL_CLEANUP("screenshot memory");
            }
            if (lc_image_readback(shot_img, &rbdesc, rgba,
                                  rbinfo.size, NULL) !=
                LC_SUCCESS) {
                free(rgba);
                free(rgb);
                FAIL_CLEANUP("screenshot readback");
            }
            for (y = 0; y < shot_h; y++) {
                for (x = 0; x < shot_w; x++) {
                    rgb[((size_t)y * shot_w + x) * 3u + 0] =
                        rgba[((size_t)y * shot_w + x) * 4u + 0];
                    rgb[((size_t)y * shot_w + x) * 3u + 1] =
                        rgba[((size_t)y * shot_w + x) * 4u + 1];
                    rgb[((size_t)y * shot_w + x) * 3u + 2] =
                        rgba[((size_t)y * shot_w + x) * 4u + 2];
                }
            }
            free(rgba);
            if (lpng_write(screenshot_path, shot_w, shot_h, 3, rgb,
                           (size_t)shot_w * 3u) != 0) {
                free(rgb);
                FAIL_CLEANUP("screenshot write");
            }
            free(rgb);
            printf("screenshot wrote %s (%ux%u)\n", screenshot_path,
                   shot_w, shot_h);
            lc_render_target_destroy(shot_target);
            lc_image_view_destroy(shot_view);
            lc_image_destroy(shot_img);
            shot_target = NULL;
            shot_view = NULL;
            shot_img = NULL;
        }
        if (debug_hiz && frame + 1u == max_frames) {
            uint32_t mips = lr_renderer_get_hiz_mip_count(renderer);
            lc_image_view *view =
                (mips > 2)
                    ? lr_renderer_get_hiz_view(renderer, 2)
                    : NULL;
            lc_image *img =
                (view != NULL) ? lc_image_view_get_image(view)
                               : NULL;

            printf("[info] hiz mips=%u\n", mips);
            if (img != NULL) {
                lc_image_readback_desc hd;
                lc_image_readback_info hi;
                float *depth = NULL;
                unsigned char *gray = NULL;
                uint32_t mw = shot_w > 0 ? shot_w >> 2 : 200u;
                uint32_t mh = shot_h > 0 ? shot_h >> 2 : 150u;
                uint32_t t;

                if (mw == 0) {
                    mw = 1;
                }
                if (mh == 0) {
                    mh = 1;
                }
                memset(&hd, 0, sizeof(hd));
                hd.mip_level = 2;
                if (lc_image_query_readback(img, &hd, &hi) ==
                        LC_SUCCESS &&
                    hi.width == mw && hi.height == mh) {
                    depth = (float *)malloc(hi.size);
                    gray = (unsigned char *)malloc((size_t)mw *
                                                   mh * 3u);
                    if (depth != NULL && gray != NULL &&
                        lc_image_readback(img, &hd, depth, hi.size,
                                          NULL) == LC_SUCCESS) {
                        for (t = 0; t < mw * mh; t++) {
                            float d = depth[t];

                            if (!(d >= 0.0f && d <= 1.0f)) {
                                d = 1.0f;
                            }
                            gray[t * 3u + 0] =
                                gray[t * 3u + 1] =
                                    gray[t * 3u + 2] =
                                        (unsigned char)(255.0f *
                                                        d);
                        }
                        if (lpng_write("hiz-debug.png", mw, mh, 3,
                                       gray,
                                       (size_t)mw * 3u) == 0) {
                            printf("hiz debug wrote hiz-debug.png"
                                   " (%ux%u)\n",
                                   mw, mh);
                        }
                    }
                    free(depth);
                    free(gray);
                }
            }
        }
    }
    {
        lr_visibility_stats vs;

        lr_renderer_get_visibility_stats(renderer, &vs);
        printf("summary: visible=%llu culled=%llu "
               "lod0=%llu lod1=%llu lod2=%llu lod3=%llu "
               "indirect=%llu draws=%llu tris=%llu "
               "dispatches=%llu prepare_ms=%.3f gpu_ms=NOT MEASURED\n",
               (unsigned long long)vs.visible,
               (unsigned long long)(vs.frustum_rejected +
                                    vs.occlusion_rejected),
               (unsigned long long)vs.lod_visible[0],
               (unsigned long long)vs.lod_visible[1],
               (unsigned long long)vs.lod_visible[2],
               (unsigned long long)vs.lod_visible[3],
               (unsigned long long)vs.indirect_commands,
               (unsigned long long)vs.indirect_draw_calls,
               (unsigned long long)vs.triangles_submitted,
               (unsigned long long)vs.compute_dispatches,
               vs.cpu_prepare_ms);
    }
cleanup:
    lc_render_target_destroy(shot_target);
    lc_image_view_destroy(shot_view);
    lc_image_destroy(shot_img);
    lr_material_destroy(dome_mat);
    lr_material_destroy(box_mat);
    lr_material_destroy(ground_mat);
    lr_mesh_destroy(dome_mesh);
    lr_mesh_destroy(box_mesh);
    lr_mesh_destroy(ground_mesh);
    if (renderer != NULL) {
        lr_renderer_destroy(renderer);
    }
    lc_swapchain_destroy(swapchain);
    lc_surface_destroy(surface);
    lc_device_destroy(device);
    lc_window_destroy(window);
    lc_shutdown();
    return ok ? 0 : 1;
}