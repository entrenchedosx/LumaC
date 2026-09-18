/* R-011 minimal shadow lab (recovery hotfix probe).
 *
 * Minimal solid-color scene, renderer-level (NOT the editor): one
 * large grey plane + one grey cube + one slanted directional light,
 * no textures, no checker, no IBL, zero ambient. Three captures:
 *   r011-on.ppm    — shadowed frame (current PCF path)
 *   r011-off.ppm   — twin frame, shadow disabled (reference)
 *   r011-map.ppm   — slot-0 depth map visualized (unlit quad, R out)
 * Plus a shadow-map raw depth readback (D32 float) with min/max/
 * histogram stats, and a scan-line profile across the shadow edge
 * in the ON frame (hard numbers for edge width / partial band).
 *
 * Build: registered in renderer/CMakeLists.txt as test_r011_probe
 * (same link pattern as test_shadow_vulkan). Run from the repo root
 * with an output dir argument; writes PPMs + stats to stdout.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <lumac/lumac.h>
#include <luma_renderer/luma_renderer.h>

#include "graphics/graphics_internal.h"

#define R011_TW 512u
#define R011_TH 512u

static int g_fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("[PASS] %s\n", msg); \
    } else { \
        printf("[FAIL] %s\n", msg); \
        g_fail = 1; \
    } \
} while (0)

typedef struct r011_env {
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
} r011_env;

static void r011_wait_idle(lc_device *device) {
    if (device != NULL && device->device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device->device);
    }
}

static unsigned char *r011_readback(lc_device *device, lc_image *image,
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
    r011_wait_idle(device);
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

static int r011_write_ppm(const char *path, const unsigned char *rgba,
                          uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    uint32_t y;

    if (f == NULL || rgba == NULL) {
        return -1;
    }
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++) {
        uint32_t x;

        for (x = 0; x < w; x++) {
            const unsigned char *p = rgba + ((size_t)y * w + x) * 4u;

            fputc(p[0], f);
            fputc(p[1], f);
            fputc(p[2], f);
        }
    }
    fclose(f);
    return 0;
}

static void r011_pixel(const unsigned char *px, uint32_t w, uint32_t x,
                       uint32_t y, double out[3]) {
    const unsigned char *p = px + ((size_t)y * w + x) * 4u;

    out[0] = (double)p[0] / 255.0;
    out[1] = (double)p[1] / 255.0;
    out[2] = (double)p[2] / 255.0;
}

static double r011_lum(const double c[3]) {
    return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

static void r011_world_to_pixel(r011_env *env, double x, double y, double z,
                                uint32_t *out_x, uint32_t *out_y) {
    float vp[16];
    float px;
    float py;
    float pw;
    double nx;
    double ny;

    lr_mat4_multiply(vp, env->camera.projection, env->camera.view);
    px = vp[0] * (float)x + vp[4] * (float)y + vp[8] * (float)z + vp[12];
    py = vp[1] * (float)x + vp[5] * (float)y + vp[9] * (float)z + vp[13];
    pw = vp[3] * (float)x + vp[7] * (float)y + vp[11] * (float)z + vp[15];
    if (pw == 0.0f) {
        pw = 1.0f;
    }
    nx = (double)(px / pw) * 0.5 + 0.5;
    ny = (double)(py / pw) * 0.5 + 0.5;
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
    *out_x = (uint32_t)(nx * (double)R011_TW);
    *out_y = (uint32_t)(ny * (double)R011_TH);
    if (*out_x >= R011_TW) {
        *out_x = R011_TW - 1;
    }
    if (*out_y >= R011_TH) {
        *out_y = R011_TH - 1;
    }
}

/* One full frame. Returns offscreen RGBA8 pixels or NULL. */
static unsigned char *r011_frame(r011_env *env, lr_light *light,
                                 lr_draw_item *items, uint32_t nitems) {
    lc_command_encoder *enc = NULL;
    lc_render_pass_desc pdesc;
    lc_render_color_attachment catt;
    lc_render_depth_attachment datt;
    lc_result res;
    uint32_t i;
    unsigned char *px = NULL;
    static const float black3[3] = { 0.0f, 0.0f, 0.0f };

    lc_poll_events();
    r011_wait_idle(env->device);
    res = lc_begin_frame(env->swapchain);
    if (res != LC_SUCCESS && res != LC_SUBOPTIMAL) {
        return NULL;
    }
    if (lc_swapchain_get_encoder(env->swapchain, &enc) != LC_SUCCESS) {
        return NULL;
    }
    if (lr_renderer_begin(env->renderer, &env->camera) != LR_SUCCESS) {
        return NULL;
    }
    if (light != NULL &&
        lr_renderer_submit_light(env->renderer, light) != LR_SUCCESS) {
        lr_renderer_end(env->renderer);
        return NULL;
    }
    for (i = 0; i < nitems; i++) {
        if (lr_renderer_submit(env->renderer, &items[i]) != LR_SUCCESS) {
            lr_renderer_end(env->renderer);
            return NULL;
        }
    }
    if (lr_renderer_render_shadows(env->renderer, enc) != LR_SUCCESS) {
        lr_renderer_end(env->renderer);
        return NULL;
    }
    lr_renderer_set_ambient(env->renderer, black3);
    memset(&catt, 0, sizeof(catt));
    catt.view = env->color_view;
    catt.load_op = LC_LOAD_OP_CLEAR;
    catt.store_op = LC_STORE_OP_STORE;
    memset(&pdesc, 0, sizeof(pdesc));
    pdesc.color_attachments = &catt;
    pdesc.color_attachment_count = 1;
    pdesc.width = R011_TW;
    pdesc.height = R011_TH;
    memset(&datt, 0, sizeof(datt));
    datt.view = env->depth_view;
    datt.depth_load_op = LC_LOAD_OP_CLEAR;
    datt.depth_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_depth = 1.0f;
    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
    pdesc.depth_attachment = &datt;
    if (lc_encoder_begin_render_pass(enc, &pdesc) != LC_SUCCESS) {
        lr_renderer_end(env->renderer);
        return NULL;
    }
    if (lr_renderer_render(env->renderer, enc, env->target) != LR_SUCCESS ||
        lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
        lr_renderer_end(env->renderer);
        return NULL;
    }
    lr_renderer_end(env->renderer);
    {
        lc_render_swapchain_pass_desc spass;

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.depth_load_op = LC_LOAD_OP_CLEAR;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        if (lc_encoder_begin_swapchain_pass(enc, env->swapchain, &spass) !=
                LC_SUCCESS ||
            lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            return NULL;
        }
    }
    res = lc_end_frame(env->swapchain);
    if (res != LC_SUCCESS && res != LC_SUBOPTIMAL) {
        return NULL;
    }
    px = r011_readback(env->device, env->color_img, R011_TW, R011_TH);
    return px;
}

static lr_material *r011_grey(lr_renderer *r) {
    lr_pbr_material_desc desc;
    lr_material *mat = NULL;
    static const float grey[4] = { 0.6f, 0.6f, 0.62f, 1.0f };

    memset(&desc, 0, sizeof(desc));
    memcpy(desc.base_color_factor, grey, sizeof(desc.base_color_factor));
    desc.metallic_factor = 0.0f;
    desc.roughness_factor = 0.8f;
    desc.normal_scale = 1.0f;
    desc.occlusion_strength = 1.0f;
    desc.alpha_mode = LR_ALPHA_OPAQUE;
    if (lr_material_create_pbr(r, &desc, &mat) != LR_SUCCESS) {
        return NULL;
    }
    return mat;
}

int main(int argc, char **argv) {
    const char *outdir = (argc > 1) ? argv[1] : ".";
    /* R-011 sweep knobs (defaults preserve the original capture):
     *   argv[2] = "default" (renderer defaults, -1) or "zero" (both
     *             biases disabled — reproduces the pass-2 scene config)
     *   argv[3] = resolution (512/1024/2048; default 1024) */
    int zero_bias = (argc > 2 && strcmp(argv[2], "zero") == 0);
    uint32_t resolution = 1024;
    r011_env env;
    lr_mesh *ground = NULL;
    lr_mesh *cube = NULL;
    lr_material *mat = NULL;
    lr_light light_on;
    lr_light light_off;
    lr_draw_item items[2];
    char path[1024];
    unsigned char *px_on = NULL;
    unsigned char *px_off = NULL;
    unsigned char *px_map = NULL;
    lr_render_stats stats_on;
    lr_render_stats stats_off;
    uint32_t shadow_count = 0;
    lr_shadow_slot_info slot0;
    static const float eye[3] = { 0.0f, 4.5f, 8.0f };
    static const float center[3] = { 0.0f, 0.5f, 0.0f };
    static const float up[3] = { 0.0f, 1.0f, 0.0f };
    lc_device_desc ddesc;
    lr_renderer_desc rdesc;
    lc_render_target_desc sig;

    setvbuf(stdout, NULL, _IONBF, 0);
    memset(&env, 0, sizeof(env));
    memset(&slot0, 0, sizeof(slot0));
    if (argc > 3) {
        unsigned long r = strtoul(argv[3], NULL, 10);

        if (r == 512 || r == 1024 || r == 2048) {
            resolution = (uint32_t)r;
        }
    }
    printf("R-011 lab: solid-grey plane+cube, slanted dir light "
           "(bias=%s res=%u)\n",
           zero_bias ? "ZERO" : "default", (unsigned)resolution);
    if (lc_init() != LC_SUCCESS) {
        printf("SKIP: lc_init failed\n");
        return 0;
    }
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.backend = LC_BACKEND_VULKAN;
    ddesc.enable_validation = 1;
    if (lc_device_create(&ddesc, &env.device) != LC_SUCCESS) {
        printf("SKIP: no Vulkan device\n");
        lc_shutdown();
        return 0;
    }
    {
        lc_window_desc wdesc;

        wdesc.title = "R011 Shadow Lab";
        wdesc.width = 800;
        wdesc.height = 600;
        if (lc_window_create(&wdesc, &env.window) != LC_SUCCESS) {
            printf("SKIP: no window\n");
            lc_device_destroy(env.device);
            lc_shutdown();
            return 0;
        }
    }
    if (lc_surface_create(env.device, env.window, &env.surface) !=
        LC_SUCCESS) {
        printf("FAIL: surface\n");
        goto cleanup;
    }
    {
        lc_swapchain_desc sdesc;

        memset(&sdesc, 0, sizeof(sdesc));
        sdesc.width = 800;
        sdesc.height = 600;
        sdesc.vsync = 1;
        if (lc_swapchain_create(env.device, env.surface, &sdesc,
                                &env.swapchain) != LC_SUCCESS) {
            printf("FAIL: swapchain\n");
            goto cleanup;
        }
    }
    {
        lc_image_desc idesc;
        lc_image_view_desc vdesc;
        lc_render_target_create_desc tdesc;
        lc_render_target_attachment att;

        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_RGBA8_UNORM;
        idesc.width = R011_TW;
        idesc.height = R011_TH;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                      LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                      LC_IMAGE_USAGE_TRANSFER_SRC |
                      LC_IMAGE_USAGE_TRANSFER_DST;
        idesc.samples = LC_SAMPLE_COUNT_1;
        if (lc_image_create(env.device, &idesc, &env.color_img) !=
            LC_SUCCESS) {
            printf("FAIL: color image\n");
            goto cleanup;
        }
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.mip_level_count = 1;
        vdesc.array_layer_count = 1;
        if (lc_image_view_create(env.color_img, &vdesc, &env.color_view) !=
            LC_SUCCESS) {
            printf("FAIL: color view\n");
            goto cleanup;
        }
        idesc.format = LC_FORMAT_D32_FLOAT;
        idesc.usage = LC_IMAGE_USAGE_DEPTH_STENCIL;
        if (lc_image_create(env.device, &idesc, &env.depth_img) !=
            LC_SUCCESS) {
            printf("FAIL: depth image\n");
            goto cleanup;
        }
        vdesc.aspect = LC_IMAGE_ASPECT_DEPTH;
        if (lc_image_view_create(env.depth_img, &vdesc, &env.depth_view) !=
            LC_SUCCESS) {
            printf("FAIL: depth view\n");
            goto cleanup;
        }
        memset(&tdesc, 0, sizeof(tdesc));
        tdesc.width = R011_TW;
        tdesc.height = R011_TH;
        att.view = env.color_view;
        tdesc.color_attachments = &att;
        tdesc.color_attachment_count = 1;
        tdesc.depth_stencil_attachment = env.depth_view;
        if (lc_render_target_create(env.device, &tdesc, &env.target) !=
            LC_SUCCESS) {
            printf("FAIL: target\n");
            goto cleanup;
        }
    }
    memset(&sig, 0, sizeof(sig));
    sig.width = R011_TW;
    sig.height = R011_TH;
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
    lr_camera_init(&env.camera);
    if (lr_camera_set_perspective(&env.camera, 0.6f, 1.0f, 0.1f, 60.0f) !=
            LR_SUCCESS ||
        lr_camera_look_at(&env.camera, eye, center, up) != LR_SUCCESS) {
        printf("FAIL: camera\n");
        goto cleanup;
    }
    if (lr_mesh_create_plane(env.renderer, 20.0f, 20.0f, &ground) !=
            LR_SUCCESS ||
        lr_mesh_create_cube(env.renderer, 2.0f, &cube) != LR_SUCCESS) {
        printf("FAIL: meshes\n");
        goto cleanup;
    }
    mat = r011_grey(env.renderer);
    CHECK(mat != NULL, "grey material");
    if (g_fail) {
        goto cleanup;
    }
    /* Shadowed light: slanted, 1024, renderer-default biases
     * (negative = defaults) unless "zero" reproduces the pass-2
     * scene config (both biases explicitly disabled). */
    memset(&light_on, 0, sizeof(light_on));
    light_on.type = LR_LIGHT_DIRECTIONAL;
    light_on.color[0] = light_on.color[1] = light_on.color[2] = 1.0f;
    light_on.intensity = 3.0f;
    light_on.direction[0] = 0.5f;
    light_on.direction[1] = -1.0f;
    light_on.direction[2] = 0.2f;
    light_on.shadow.enabled = 1;
    light_on.shadow.resolution = resolution;
    light_on.shadow.depth_bias = zero_bias ? 0.0f : -1.0f;
    light_on.shadow.normal_bias = zero_bias ? 0.0f : -1.0f;
    /* Twin: identical but unshadowed. */
    memcpy(&light_off, &light_on, sizeof(light_off));
    light_off.shadow.enabled = 0;
    memset(&items[0], 0, sizeof(items[0]));
    lr_transform_identity(&items[0].transform);
    items[0].mesh = ground;
    items[0].material = mat;
    items[0].casts_shadow = 1;
    items[0].receives_shadow = 1;
    memset(&items[1], 0, sizeof(items[1]));
    lr_transform_identity(&items[1].transform);
    items[1].transform.position[1] = 1.5f;
    items[1].mesh = cube;
    items[1].material = mat;
    items[1].casts_shadow = 1;
    items[1].receives_shadow = 1;

    /* ---- capture ON ---- */
    px_on = r011_frame(&env, &light_on, items, 2);
    CHECK(px_on != NULL, "r011 ON frame reads back");
    lr_renderer_get_stats(env.renderer, &stats_on);
    shadow_count = lr_renderer_get_shadow_count(env.renderer);
    lr_renderer_get_shadow_slot_info(env.renderer, 0, &slot0);
    printf("[r011] ON stats: passes=%u draws=%u tris=%u slots=%u "
           "slot0res=%u slot0active=%d\n",
           (unsigned)stats_on.shadow_passes,
           (unsigned)stats_on.shadow_draw_calls,
           (unsigned)stats_on.shadow_triangles, (unsigned)shadow_count,
           (unsigned)slot0.resolution, slot0.active);
    if (px_on != NULL) {
        snprintf(path, sizeof(path), "%s/r011-on.ppm", outdir);
        CHECK(r011_write_ppm(path, px_on, R011_TW, R011_TH) == 0,
              "r011-on.ppm written");
    }
    /* ---- capture OFF (twin) ---- */
    px_off = r011_frame(&env, &light_off, items, 2);
    CHECK(px_off != NULL, "r011 OFF frame reads back");
    lr_renderer_get_stats(env.renderer, &stats_off);
    printf("[r011] OFF stats: passes=%u draws=%u tris=%u\n",
           (unsigned)stats_off.shadow_passes,
           (unsigned)stats_off.shadow_draw_calls,
           (unsigned)stats_off.shadow_triangles);
    if (px_off != NULL) {
        snprintf(path, sizeof(path), "%s/r011-off.ppm", outdir);
        CHECK(r011_write_ppm(path, px_off, R011_TW, R011_TH) == 0,
              "r011-off.ppm written");
    }
    /* ---- shadow-map viz: unlit quad textured with slot 0 ---- */
    {
        lc_image_view *dbg = lr_renderer_get_shadow_view(env.renderer, 0);

        CHECK(dbg != NULL, "r011 slot 0 debug view borrows");
        if (dbg != NULL) {
            lr_unlit_material_desc udesc;
            lr_material *debug_mat = NULL;
            lr_mesh *quad = NULL;
            lr_draw_item ditem;
            lr_camera qcam;
            lr_camera saved = env.camera;
            unsigned char *dpx = NULL;
            static const float qeye[3] = { 0.0f, 0.0f, 3.0f };
            static const float qc[3] = { 0.0f, 0.0f, 0.0f };
            static const float qup[3] = { 0.0f, 1.0f, 0.0f };

            memset(&udesc, 0, sizeof(udesc));
            udesc.color[0] = 1.0f;
            udesc.color[1] = 1.0f;
            udesc.color[2] = 1.0f;
            udesc.color[3] = 1.0f;
            udesc.base_color_texture = dbg;
            if (lr_material_create_unlit(env.renderer, &udesc,
                                         &debug_mat) == LR_SUCCESS &&
                lr_mesh_create_plane(env.renderer, 2.0f, 2.0f, &quad) ==
                    LR_SUCCESS) {
                static const float xaxis[3] = { 1.0f, 0.0f, 0.0f };

                lr_camera_init(&qcam);
                lr_camera_set_perspective(&qcam, 0.6f, 1.0f, 0.1f, 60.0f);
                lr_camera_look_at(&qcam, qeye, qc, qup);
                env.camera = qcam;
                memset(&ditem, 0, sizeof(ditem));
                lr_transform_identity(&ditem.transform);
                lr_quat_from_axis_angle(xaxis, 1.5707963f,
                                        ditem.transform.rotation);
                ditem.mesh = quad;
                ditem.material = debug_mat;
                dpx = r011_frame(&env, NULL, &ditem, 1);
                env.camera = saved;
                px_map = dpx;
                CHECK(dpx != NULL, "r011 MAP frame reads back");
                if (dpx != NULL) {
                    snprintf(path, sizeof(path), "%s/r011-map.ppm",
                             outdir);
                    CHECK(r011_write_ppm(path, dpx, R011_TW,
                                         R011_TH) == 0,
                          "r011-map.ppm written");
                }
                lr_material_destroy(debug_mat);
                lr_mesh_destroy(quad);
            } else {
                CHECK(0, "r011 debug quad resources ready");
            }
        }
    }
    /* ---- raw shadow-map depth readback (D32 float) ---- */
    {
        lc_image_view *dbg = lr_renderer_get_shadow_view(env.renderer, 0);
        lc_image *dimg = (dbg != NULL) ? lc_image_view_get_image(dbg)
                                       : NULL;

        if (dimg != NULL) {
            lc_image_readback_desc rbdesc;
            lc_image_readback_info rbinfo;
            size_t need = 0;

            memset(&rbdesc, 0, sizeof(rbdesc));
            if (lc_image_query_readback(dimg, &rbdesc, &rbinfo) ==
                    LC_SUCCESS &&
                lc_image_readback(dimg, &rbdesc, NULL, 0, &need) ==
                    LC_SUCCESS) {
                float *depth = (float *)malloc(need);
                size_t got = 0;

                printf("[r011] map format=%d size=%ux%u pitch=%u "
                       "bytes=%u\n",
                       (int)rbinfo.format, (unsigned)rbinfo.width,
                       (unsigned)rbinfo.height,
                       (unsigned)rbinfo.row_pitch,
                       (unsigned)rbinfo.size);
                if (depth != NULL &&
                    lc_image_readback(dimg, &rbdesc, depth, need,
                                      &got) == LC_SUCCESS) {
                    uint32_t w = rbinfo.width;
                    uint32_t h = rbinfo.height;
                    uint32_t x;
                    uint32_t y;
                    float mn = 1e30f;
                    float mx = -1e30f;
                    double acc = 0.0;
                    uint64_t n = 0;
                    uint64_t hist[10];

                    memset(hist, 0, sizeof(hist));
                    for (y = 0; y < h; y++) {
                        for (x = 0; x < w; x++) {
                            float v = depth[(size_t)y * w + x];

                            if (v < mn) {
                                mn = v;
                            }
                            if (v > mx) {
                                mx = v;
                            }
                            acc += v;
                            n++;
                            {
                                int b = (int)(v * 10.0f);

                                if (b < 0) {
                                    b = 0;
                                }
                                if (b > 9) {
                                    b = 9;
                                }
                                hist[b]++;
                            }
                        }
                    }
                    printf("[r011] depth min=%.6f max=%.6f mean=%.6f\n",
                           mn, mx, acc / (double)n);
                    printf("[r011] depth hist10:");
                    for (x = 0; x < 10; x++) {
                        printf(" %llu",
                               (unsigned long long)hist[x]);
                    }
                    printf("\n");
                    CHECK(mx <= 1.0f && mn >= 0.0f,
                          "r011 depth in 0..1");
                    CHECK(mx - mn > 0.01f,
                          "r011 depth content varies");
                    free(depth);
                } else {
                    CHECK(0, "r011 depth bytes read back");
                    free(depth);
                }
            } else {
                CHECK(0, "r011 depth readback eligible");
            }
        } else {
            CHECK(0, "r011 depth image borrows");
        }
    }
    /* ---- numeric edge profile on the ON frame ---- */
    if (px_on != NULL && px_off != NULL) {
        uint32_t cx;
        uint32_t cy;
        uint32_t lx;
        uint32_t ly;
        double shadowed[3];
        double lit[3];

        r011_world_to_pixel(&env, 1.7, 0.01, -0.2, &cx, &cy);
        r011_world_to_pixel(&env, 1.7, 0.01, 1.6, &lx, &ly);
        r011_pixel(px_on, R011_TW, cx, cy, shadowed);
        r011_pixel(px_off, R011_TW, lx, ly, lit);
        printf("[r011] crescent lum=%.4f lit lum=%.4f ratio=%.4f\n",
               r011_lum(shadowed), r011_lum(lit),
               r011_lum(shadowed) / (r011_lum(lit) + 1e-6));
        CHECK(r011_lum(shadowed) < 0.15 * r011_lum(lit) &&
                  r011_lum(lit) > 0.2,
              "r011 footprint: crescent dark, outside lit");
        /* Edge width in SCREEN pixels: row y=150 crosses the
         * ground shadow (lit ROW on both sides, dark umbra
         * between). Measure the lit->dark fall on the left edge:
         * last x with lum > 85% of range, first x with lum < 15%.
         * Correct PCF at any map resolution gives a 1-3px step
         * (the 3x3 kernel); the R-011 complaint (large diffuse
         * darkening) would read tens of px. Resolution-independent
         * because the frame is fixed 512x512. */
        {
            const uint32_t ey = 150;
            double row_lo = 1.0;
            double row_hi = 0.0;
            uint32_t x;
            uint32_t x_lit = 0;
            uint32_t x_dark = 0;
            uint32_t edge_w = 0;

            for (x = 0; x < R011_TW; x++) {
                double c[3];

                r011_pixel(px_on, R011_TW, x, ey, c);
                if (r011_lum(c) < row_lo) {
                    row_lo = r011_lum(c);
                }
                if (r011_lum(c) > row_hi) {
                    row_hi = r011_lum(c);
                }
            }
            for (x = 0; x < R011_TW; x++) {
                double c[3];
                double lum;

                r011_pixel(px_on, R011_TW, x, ey, c);
                lum = r011_lum(c);
                if (lum > row_lo + 0.85 * (row_hi - row_lo) &&
                    x < R011_TW / 2u) {
                    x_lit = x;
                }
            }
            for (x = x_lit; x < R011_TW; x++) {
                double c[3];
                double lum;

                r011_pixel(px_on, R011_TW, x, ey, c);
                lum = r011_lum(c);
                if (lum < row_lo + 0.15 * (row_hi - row_lo)) {
                    x_dark = x;
                    break;
                }
            }
            edge_w = (x_dark > x_lit) ? (x_dark - x_lit) : 0;
            printf("[r011] screen edge y=%u lo=%.4f hi=%.4f "
                   "xlit=%u xdark=%u width=%upx\n",
                   (unsigned)ey, row_lo, row_hi, (unsigned)x_lit,
                   (unsigned)x_dark, (unsigned)edge_w);
            CHECK(row_hi - row_lo > 0.2 && edge_w >= 1 &&
                      edge_w <= 8,
                  "r011 screen edge is a 1-8px PCF step");
        }
        /* ON-vs-OFF difference stats. */
        {
            uint64_t diff_n = 0;
            uint64_t tot = 0;
            uint32_t x;
            uint32_t y;

            for (y = 0; y < R011_TH; y++) {
                for (x = 0; x < R011_TW; x++) {
                    double a[3];
                    double b[3];

                    r011_pixel(px_on, R011_TW, x, y, a);
                    r011_pixel(px_off, R011_TW, x, y, b);
                    tot++;
                    if (fabs(r011_lum(a) - r011_lum(b)) > 0.02) {
                        diff_n++;
                    }
                }
            }
            printf("[r011] on/off diff frac=%.4f\n",
                   (double)diff_n / (double)tot);
            CHECK(diff_n > 0, "r011 ON differs from OFF");
        }
    }
    printf("r011 probe: %s\n", g_fail ? "FAILED" : "OK");

cleanup:
    free(px_on);
    free(px_off);
    free(px_map);
    lr_material_destroy(mat);
    lr_mesh_destroy(cube);
    lr_mesh_destroy(ground);
    lc_render_target_destroy(env.target);
    lc_image_view_destroy(env.color_view);
    lc_image_destroy(env.color_img);
    lc_image_view_destroy(env.depth_view);
    lc_image_destroy(env.depth_img);
    lr_renderer_destroy(env.renderer);
    if (env.swapchain != NULL) {
        lc_swapchain_destroy(env.swapchain);
    }
    if (env.surface != NULL) {
        lc_surface_destroy(env.surface);
    }
    if (env.device != NULL) {
        /* Drain before teardown (probe owns no other refs). */
        r011_wait_idle(env.device);
        lc_device_destroy(env.device);
    }
    if (env.window != NULL) {
        lc_window_destroy(env.window);
    }
    lc_shutdown();
    return g_fail ? 1 : 0;
}
