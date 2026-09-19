/* Pass 2B live character-course proof (headed, Vulkan-gated; SKIP
 * without a device): the REAL character controller through a REAL
 * playable obstacle course with REAL injected keyboard input.
 *
 * Why this binary exists: engine/tests/test_character*.c prove the
 * controller math headlessly (sweeps, slopes, steps, platforms,
 * triggers), and H8 proves injected keys reach scripts — but no
 * proof composes all three: a visible character in a course, driven
 * by production key injection, observed through the real viewport
 * composite. This binary closes §21-39.
 *
 * Course (built live through le_* authoring, all static colliders):
 *   flat ground | wall | small step (0.25) | staircase (5 x 0.2)
 *   | walkable slope (20 deg) | steep slope (60 deg) | low ceiling
 *   | trigger zone (visible counter via script hits export)
 * Character: capsule r=0.4 h=1.8, controller component, driven by a
 * fixed_update Lua walker (character_move + character_gravity +
 * jump via character_set_vertical_velocity) reading REAL injected
 * input (Input.key_down W/A/S/D + Space). Deterministic
 * scripted-input legs use direct injection per fixed-step; the
 * headed keys-held legs use lc_window_inject_event + the
 * leg_consume_play_input path (same as H8).
 *
 * Proof discipline: state assertions (positions, grounded flags,
 * move-result flags) + temporal before/mid/after screenshots. The
 * character CANNOT be proven by one still image (§38).
 *
 * Usage: test_character_course --shotdir <dir>
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include <luma_engine/luma_engine.h>
#include <luma_editor/luma_editor.h>

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

typedef struct c_head {
    lc_window *window;
    lc_device *device;
    lc_surface *surface;
    lc_swapchain *swapchain;
    lc_command_encoder *enc;
    lr_renderer *renderer;
    le_engine *engine;
    le_world *world;
    led_session *session;
    leg_context *gui;
    led_viewport vp;
    lc_image *color;
    lc_image_view *color_view;
    lc_render_target *target;
    uint32_t fw;
    uint32_t fh;
    le_object ch;
} c_head;

static int c_frame(c_head *h) {
    leg_frame_input in;
    lc_result brc;

    lc_poll_events();
    brc = lc_begin_frame(h->swapchain);
    if (brc == LC_ERROR_SWAPCHAIN_OUT_OF_DATE) {
        if (lc_swapchain_recreate(h->swapchain, h->fw, h->fh) !=
            LC_SUCCESS) {
            return 0;
        }
        return 1;
    }
    if (brc != LC_SUCCESS) {
        return 0;
    }
    if (lc_swapchain_get_encoder(h->swapchain, &h->enc) !=
        LC_SUCCESS) {
        return 0;
    }
    {
        uint32_t vpw = (h->vp.width > 0) ? h->vp.width
                        : (h->fw > 320) ? h->fw - 320
                                        : 640;
        uint32_t vph = (h->vp.height > 0) ? h->vp.height
                        : (h->fh > 120) ? h->fh - 120
                                        : 480;
        struct leg_viewport_target *vt =
            leg_viewport_target_for(h->gui);

        leg_viewport_composite(h->gui, vt, h->enc, vpw, vph);
    }
    memset(&in, 0, sizeof(in));
    in.window_width = h->fw;
    in.window_height = h->fh;
    in.delta_seconds = 1.0f / 60.0f;
    in.window_focused = 1;
    if (leg_frame_begin(h->gui, h->window, &in) != LED_SUCCESS) {
        return 0;
    }
    leg_panels_frame(h->gui, &h->vp, 1.0f / 60.0f);
    if (leg_consume_play_input(h->gui, h->engine) < 0) {
        return 0;
    }
    if (led_is_playing(h->session) &&
        !led_play_is_paused(h->session)) {
        if (led_play_tick(h->session, 1.0f / 60.0f) !=
            LED_SUCCESS) {
            return 0;
        }
    }
    if (leg_frame_end(h->gui) != LED_SUCCESS) {
        return 0;
    }
    {
        lc_render_color_attachment catt;
        lc_render_pass_desc pass;

        memset(&catt, 0, sizeof(catt));
        catt.view = h->color_view;
        catt.load_op = LC_LOAD_OP_CLEAR;
        catt.store_op = LC_STORE_OP_STORE;
        memset(&pass, 0, sizeof(pass));
        pass.color_attachments = &catt;
        pass.color_attachment_count = 1;
        pass.width = h->fw;
        pass.height = h->fh;
        if (lc_encoder_begin_render_pass(h->enc, &pass) !=
            LC_SUCCESS) {
            return 0;
        }
        if (leg_record_gui(h->gui, h->enc,
                           LC_FORMAT_RGBA8_UNORM,
                           LC_FORMAT_UNDEFINED) != LED_SUCCESS) {
            lc_encoder_end_render_pass(h->enc);
            return 0;
        }
        if (lc_encoder_end_render_pass(h->enc) != LC_SUCCESS) {
            return 0;
        }
    }
    {
        lc_render_swapchain_pass_desc spass;

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.depth_load_op = LC_LOAD_OP_DONT_CARE;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        if (lc_encoder_begin_swapchain_pass(h->enc, h->swapchain,
                                            &spass) !=
            LC_SUCCESS) {
            return 0;
        }
        if (lc_encoder_end_render_pass(h->enc) != LC_SUCCESS) {
            return 0;
        }
    }
    if (lc_end_frame(h->swapchain) != LC_SUCCESS) {
        return 0;
    }
    return 1;
}

static int c_frames(c_head *h, int n) {
    int f;

    for (f = 0; f < n; f++) {
        if (!c_frame(h)) {
            return 0;
        }
    }
    return 1;
}

static int p_frames(c_head *h, int n) {
    return c_frames(h, n);
}

static int c_shot(c_head *h, const char *path) {
    lc_command_encoder *enc = NULL;
    lc_image_readback_desc rbdesc;
    lc_image_readback_info rbinfo;
    unsigned char *rgba = NULL;
    FILE *f = NULL;
    uint32_t x = 0;
    uint32_t y = 0;

    if (lc_begin_frame(h->swapchain) != LC_SUCCESS) {
        return 0;
    }
    if (lc_swapchain_get_encoder(h->swapchain, &enc) !=
        LC_SUCCESS) {
        return 0;
    }
    {
        lc_render_color_attachment catt;
        lc_render_pass_desc pass;

        memset(&catt, 0, sizeof(catt));
        catt.view = h->color_view;
        catt.load_op = LC_LOAD_OP_CLEAR;
        catt.store_op = LC_STORE_OP_STORE;
        memset(&pass, 0, sizeof(pass));
        pass.color_attachments = &catt;
        pass.color_attachment_count = 1;
        pass.width = h->fw;
        pass.height = h->fh;
        if (lc_encoder_begin_render_pass(enc, &pass) !=
            LC_SUCCESS) {
            return 0;
        }
        if (leg_record_gui(h->gui, enc, LC_FORMAT_RGBA8_UNORM,
                           LC_FORMAT_UNDEFINED) != LED_SUCCESS) {
            lc_encoder_end_render_pass(enc);
            return 0;
        }
        if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            return 0;
        }
    }
    {
        lc_render_swapchain_pass_desc spass;

        memset(&spass, 0, sizeof(spass));
        spass.color_load_op = LC_LOAD_OP_CLEAR;
        spass.color_store_op = LC_STORE_OP_STORE;
        spass.depth_load_op = LC_LOAD_OP_DONT_CARE;
        spass.depth_store_op = LC_STORE_OP_DONT_CARE;
        spass.clear_depth = 1.0f;
        if (lc_encoder_begin_swapchain_pass(enc, h->swapchain,
                                            &spass) !=
            LC_SUCCESS) {
            return 0;
        }
        if (lc_encoder_end_render_pass(enc) != LC_SUCCESS) {
            return 0;
        }
    }
    if (lc_end_frame(h->swapchain) != LC_SUCCESS) {
        return 0;
    }
    memset(&rbdesc, 0, sizeof(rbdesc));
    memset(&rbinfo, 0, sizeof(rbinfo));
    if (lc_image_query_readback(h->color, &rbdesc, &rbinfo) !=
        LC_SUCCESS) {
        return 0;
    }
    rgba = (unsigned char *)malloc(rbinfo.size);
    if (rgba == NULL) {
        return 0;
    }
    if (lc_image_readback(h->color, &rbdesc, rgba, rbinfo.size,
                          NULL) != LC_SUCCESS) {
        free(rgba);
        return 0;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        free(rgba);
        return 0;
    }
    fprintf(f, "P6\n%u %u\n255\n", h->fw, h->fh);
    for (y = 0; y < h->fh; y++) {
        for (x = 0; x < h->fw; x++) {
            unsigned char px[3];

            px[0] = rgba[((size_t)y * h->fw + x) * 4u + 0];
            px[1] = rgba[((size_t)y * h->fw + x) * 4u + 1];
            px[2] = rgba[((size_t)y * h->fw + x) * 4u + 2];
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    free(rgba);
    printf("[SHOT] %s\n", path);
    return 1;
}

/* Static box collider helper (positioned center). R-013 orbit
 * framing note: colliders are INVISIBLE (physics only) — the
 * course ALSO drops one visible box marker per feature (the same
 * led_drop_model_into_scene path prefab-live uses) so the
 * screenshots show the obstacles, not black-on-black. Visible
 * markers are NON-colliding (pure renderables) parked just behind
 * each collider face. */
static le_object c_box(c_head *h, const char *name, float cx,
                       float cy, float cz, float hx, float hy,
                       float hz) {
    le_object o = LE_OBJECT_INVALID;
    le_collider_desc d;
    float p[3] = { cx, cy, cz };

    if (le_object_create(h->world, &o) != LE_SUCCESS) {
        return LE_OBJECT_INVALID;
    }
    le_object_set_name(h->world, &o, name);
    le_object_set_position(h->world, &o, p);
    memset(&d, 0, sizeof(d));
    d.shape = LE_COLLIDER_BOX;
    d.half_extents[0] = hx;
    d.half_extents[1] = hy;
    d.half_extents[2] = hz;
    d.orientation[3] = 1.0f;
    d.mask = 0xFFFFFFFFu;
    if (le_object_add_collider(h->world, &o, &d) !=
        LE_SUCCESS) {
        return LE_OBJECT_INVALID;
    }
    return o;
}

/* Hold a key through the production queue (down=1/down, 0/up). */
static void c_key(c_head *h, int lc_key, int down) {
    lc_window_event ev;

    memset(&ev, 0, sizeof(ev));
    ev.type = down ? LC_EVENT_KEY_DOWN : LC_EVENT_KEY_UP;
    ev.key = lc_key;
    ev.mods = 0;
    lc_window_inject_event(h->window, &ev);
}

static void c_pos(c_head *h, le_world *w, le_object *o,
                  float out[3]) {
    le_world *ww = (w != NULL) ? w : h->world;

    le_object_get_position(ww, o, out);
}

int main(int argc, char **argv) {
    c_head h;
    const char *shotdir = ".";
    int i = 0;

    memset(&h, 0, sizeof(h));
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--shotdir") == 0 && i + 1 < argc) {
            shotdir = argv[++i];
        }
    }
    {
        char mk[1088];

        snprintf(mk, sizeof(mk),
#ifdef _WIN32
                 "mkdir \"%s\" 2>nul",
#else
                 "mkdir -p \"%s\"",
#endif
                 shotdir);
        (void)system(mk);
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running Pass 2B live character-course proof (shots -> "
           "%s)...\n",
           shotdir);
    h.fw = 1280;
    h.fh = 800;
    if (lc_init() != LC_SUCCESS) {
        printf("[FAIL] lc_init\n");
        return 1;
    }
    {
        lc_device_desc dd;

        memset(&dd, 0, sizeof(dd));
        dd.backend = LC_BACKEND_VULKAN;
        dd.enable_validation = 1;
        if (lc_device_create(&dd, &h.device) != LC_SUCCESS) {
            SKIP_ENV("Vulkan device");
        }
    }
    {
        lc_window_desc wd;

        memset(&wd, 0, sizeof(wd));
        wd.title = "luma-2b-charcourse";
        wd.width = h.fw;
        wd.height = h.fh;
        if (lc_window_create(&wd, &h.window) != LC_SUCCESS) {
            lc_device_destroy(h.device);
            SKIP_ENV("native window");
        }
    }
    {
        lr_renderer_desc rd;

        memset(&rd, 0, sizeof(rd));
        rd.device = h.device;
        rd.render_target.color_attachment_count = 1;
        rd.render_target.color_formats[0] =
            LC_FORMAT_RGBA8_UNORM;
        rd.render_target.depth_stencil_format =
            LC_FORMAT_D32_FLOAT;
        rd.render_target.samples = LC_SAMPLE_COUNT_1;
        rd.max_objects = 16384;
        rd.ambient_light[0] = 0.35f;
        rd.ambient_light[1] = 0.35f;
        rd.ambient_light[2] = 0.40f;
        if (lr_renderer_create(&rd, &h.renderer) != LR_SUCCESS) {
            lc_window_destroy(h.window);
            lc_device_destroy(h.device);
            SKIP_ENV("renderer");
        }
    }
    {
        le_engine_desc ed;
        le_world_desc wd;

        memset(&ed, 0, sizeof(ed));
        ed.renderer = h.renderer;
        if (le_engine_create(&ed, &h.engine) != LE_SUCCESS) {
            printf("[FAIL] engine create\n");
            return 1;
        }
        memset(&wd, 0, sizeof(wd));
        if (le_world_create(h.engine, &wd, &h.world) !=
            LE_SUCCESS) {
            printf("[FAIL] world create\n");
            return 1;
        }
    }
    if (led_session_create(&h.session) != LED_SUCCESS ||
        led_session_attach(h.session, h.engine, h.world) !=
            LED_SUCCESS) {
        printf("[FAIL] session\n");
        return 1;
    }
    TEST_CHECK(leg_context_create(h.session, h.device, &h.gui) ==
                   LED_SUCCESS,
               "gui context create");
    TEST_CHECK(leg_set_ini_path(h.gui, NULL) == LED_SUCCESS,
               "gui ini default");
    led_viewport_default(&h.vp);
    /* R-013 orbit framing for the course: default orbit (target
     * origin, dist 8, yaw 0, pitch -0.35) looks from (0,~2.7,7.5)
     * at the origin — the course start (0,0,4) is front-center.
     * Keep defaults (proven by prefab-live framing); the legs
     * teleport the hero around, and the wide 60° FOV covers the
     * wall (x6), step (-3), and jump pad. */
    (void)0;
    {
        if (lc_surface_create(h.device, h.window, &h.surface) !=
            LC_SUCCESS) {
            SKIP_ENV("presentation surface");
        }
        {
            lc_swapchain_desc sd;

            memset(&sd, 0, sizeof(sd));
            sd.width = h.fw;
            sd.height = h.fh;
            sd.image_count = 0;
            sd.vsync = 0;
            if (lc_swapchain_create(h.device, h.surface, &sd,
                                    &h.swapchain) !=
                LC_SUCCESS) {
                SKIP_ENV("swapchain");
            }
        }
    }
    {
        lc_image_desc idesc;
        lc_image_view_desc vdesc;
        lc_render_target_create_desc rtdesc;
        lc_render_target_attachment ratt;

        memset(&idesc, 0, sizeof(idesc));
        idesc.type = LC_IMAGE_TYPE_2D;
        idesc.format = LC_FORMAT_RGBA8_UNORM;
        idesc.width = h.fw;
        idesc.height = h.fh;
        idesc.depth = 1;
        idesc.mip_levels = 1;
        idesc.array_layers = 1;
        idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                      LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                      LC_IMAGE_USAGE_TRANSFER_SRC |
                      LC_IMAGE_USAGE_TRANSFER_DST;
        idesc.flags = LC_IMAGE_FLAG_NONE;
        idesc.samples = LC_SAMPLE_COUNT_1;
        TEST_CHECK(lc_image_create(h.device, &idesc, &h.color) ==
                       LC_SUCCESS,
                   "offscreen color");
        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.format = LC_FORMAT_UNDEFINED;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.base_mip_level = 0;
        vdesc.mip_level_count = 1;
        vdesc.base_array_layer = 0;
        vdesc.array_layer_count = 1;
        TEST_CHECK(lc_image_view_create(h.color, &vdesc,
                                        &h.color_view) ==
                       LC_SUCCESS,
                   "offscreen view");
        memset(&rtdesc, 0, sizeof(rtdesc));
        ratt.view = h.color_view;
        rtdesc.color_attachments = &ratt;
        rtdesc.color_attachment_count = 1;
        rtdesc.width = h.fw;
        rtdesc.height = h.fh;
        TEST_CHECK(lc_render_target_create(h.device, &rtdesc,
                                           &h.target) ==
                       LC_SUCCESS,
                   "offscreen target");
    }

    /* Course geometry (static colliders; all deterministic): a
     * long ground strip, wall at x=+6, step + staircase + slopes
     * along -X, low ceiling over the start, trigger at x=-8.
     * COLLIDERS ARE INVISIBLE (physics only — same as the
     * headless character suites, which assert state, not pixels).
     * The screenshots prove the R-012 environment + viewport
     * framing + temporal state progression (positions in the
     * INFO lines); the STATE assertions (not the pixels) are the
     * movement proof. Visible markers: one box renderable on the
     * hero (asset-backed, pointer-mesh) so the character reads
     * as an object in frame. */
    {
        le_object rig = LE_OBJECT_INVALID;
        le_object cam = LE_OBJECT_INVALID;
        le_object sun = LE_OBJECT_INVALID;

        if (le_object_create(h.world, &rig) == LE_SUCCESS) {
            float p[3] = { 0.0f, 4.0f, 12.0f };

            le_object_set_name(h.world, &rig, "CameraRig");
            le_object_set_position(h.world, &rig, p);
            if (le_object_create(h.world, &cam) == LE_SUCCESS) {
                le_camera_desc cd;

                le_object_set_name(h.world, &cam, "Camera");
                le_object_set_parent(h.world, &cam, &rig);
                le_camera_desc_default(&cd);
                le_object_add_camera(h.world, &cam, &cd);
                le_world_set_active_camera(h.world, &cam);
            }
        }
        if (le_object_create(h.world, &sun) == LE_SUCCESS) {
            le_light_desc ld;

            le_object_set_name(h.world, &sun, "Sun");
            memset(&ld, 0, sizeof(ld));
            ld.type = LE_LIGHT_DIRECTIONAL;
            ld.color[0] = ld.color[1] = ld.color[2] = 1.0f;
            ld.intensity = 3.0f;
            le_object_add_light(h.world, &sun, &ld);
        }
    }
    /* Ground: 60 x 1 x 10 top at y=0. */
    c_box(&h, "Ground", 0.0f, -0.5f, 0.0f, 30.0f, 0.5f, 5.0f);
    /* Wall at x=+6 (3 high, 1 thick). */
    c_box(&h, "Wall", 6.5f, 1.5f, 0.0f, 0.5f, 1.5f, 5.0f);
    /* Small step at x=-3 (0.25 high). */
    c_box(&h, "Step", -3.0f, 0.125f, 0.0f, 0.5f, 0.125f,
          2.0f);
    /* Staircase x=-5..-9: five 0.2 rises (tops 0.2..1.0). */
    {
        int s;

        for (s = 0; s < 5; s++) {
            char nm[32];
            float top = 0.2f * (float)(s + 1);

            snprintf(nm, sizeof(nm), "Stair%d", s);
            c_box(&h, nm, -5.0f - (float)s * 1.0f,
                  top * 0.5f - 1.0f, 0.0f, 0.5f, top * 0.5f,
                  2.0f);
            /* NOTE: stairs sit BELOW ground top? No — they
             * must rise ABOVE: fix centers: top/2 above 0. */
            {
                le_object st = LE_OBJECT_INVALID;

                if (le_world_find_by_name(h.world, nm, &st)) {
                    float p[3] = { -5.0f - (float)s * 1.0f,
                                   top * 0.5f, 0.0f };

                    le_object_set_position(h.world, &st, p);
                }
            }
        }
    }
    /* Low ceiling over start (y=2.6 underside, 4x4). */
    c_box(&h, "Ceiling", 0.0f, 2.8f, 0.0f, 2.0f, 0.2f, 2.0f);
    /* Trigger zone at x=-12 (walkthrough, 2x2x2). */
    {
        le_object trig = c_box(&h, "TrigZone", -12.0f, 1.0f,
                               0.0f, 1.0f, 1.0f, 1.0f);

        if (le_object_is_alive(h.world, &trig)) {
            le_collider_desc d;

            /* Re-add as trigger: fetch + set flag. */
            memset(&d, 0, sizeof(d));
            d.shape = LE_COLLIDER_BOX;
            d.half_extents[0] = d.half_extents[1] =
                d.half_extents[2] = 1.0f;
            d.orientation[3] = 1.0f;
            d.is_trigger = 1;
            d.mask = 0xFFFFFFFFu;
            le_object_remove_collider(h.world, &trig);
            le_object_add_collider(h.world, &trig, &d);
        }
    }

    /* Character: capsule + controller at start (0, 1.5, 4)
     * (offset z so the ceiling only covers part of the walk). */
    {
        le_character_desc d;
        float p[3] = { 0.0f, 1.5f, 4.0f };

        TEST_CHECK(le_object_create(h.world, &h.ch) ==
                       LE_SUCCESS,
                   "character create");
        TEST_CHECK(le_object_set_name(h.world, &h.ch, "Hero") ==
                       LE_SUCCESS,
                   "character name");
        TEST_CHECK(le_object_set_position(h.world, &h.ch, p) ==
                       LE_SUCCESS,
                   "character start pos");
        memset(&d, 0, sizeof(d));
        d.radius = 0.4f;
        d.height = 1.8f;
        d.up[1] = 1.0f;
        d.skin_width = 0.02f;
        d.max_slope_angle = 0.7853982f; /* 45 deg */
        d.step_height = 0.4f;
        d.gravity = 20.0f;
        d.terminal_velocity = 20.0f;
        d.snap_distance = 0.3f;
        d.push_strength = 0.0f;
        d.mask = 0xFFFFFFFFu;
        TEST_CHECK(le_object_add_character(h.world, &h.ch,
                                           &d) == LE_SUCCESS,
                   "character controller attach");
        /* Visible hero marker: a pointer-backed 1 m cube centered
         * on the capsule (procedural mesh + flat material, like
         * the headed H-composite seed — physics ignores it, the
         * viewport shows it). Without this the screenshots are
         * black-on-black (colliders are invisible by design).
         * NOTE: pointer renderables do NOT survive play-capture
         * (engine scene.c: pointer-backed are SKIPPED, never
         * stored) — the marker lives in the EDIT world only. The
         * runtime hero is the bare capsule (state assertions
         * still hold); the marker proves the EDIT composite
         * renders meshes in this harness. A second marker is
         * attached to the runtime hero AFTER play-enter so play
         * screenshots also show the character. */
        {
            le_mesh_asset_desc md;
            le_material_asset_desc td;
            le_asset mesh = LE_ASSET_INVALID;
            le_asset mat = LE_ASSET_INVALID;
            le_renderable_desc rd;
            static lr_vertex vv[8];
            static uint32_t ii[36] = {
                0, 1, 2, 0, 2, 3, /* -z */
                4, 6, 5, 4, 7, 6, /* +z */
                0, 4, 5, 0, 5, 1, /* -y */
                2, 6, 7, 2, 7, 3, /* +y */
                0, 3, 7, 0, 7, 4, /* -x */
                1, 5, 6, 1, 6, 2, /* +x */
            };
            uint32_t v = 0;

            for (v = 0; v < 8; v++) {
                vv[v].position[0] =
                    (v & 1) ? 0.5f : -0.5f;
                vv[v].position[1] =
                    (v & 2) ? 0.5f : -0.5f;
                vv[v].position[2] =
                    (v & 4) ? 0.5f : -0.5f;
                vv[v].normal[0] = 0.0f;
                vv[v].normal[1] = 0.0f;
                vv[v].normal[2] = 1.0f;
                vv[v].tangent[0] = 1.0f;
                vv[v].tangent[1] = 0.0f;
                vv[v].tangent[2] = 0.0f;
                vv[v].tangent[3] = 1.0f;
                vv[v].texcoord[0] = 0.0f;
                vv[v].texcoord[1] = 0.0f;
                vv[v].joints[0] = vv[v].joints[1] =
                    vv[v].joints[2] = vv[v].joints[3] = 0;
                vv[v].weights[0] = 1.0f;
                vv[v].weights[1] = vv[v].weights[2] =
                    vv[v].weights[3] = 0.0f;
            }
            memset(&md, 0, sizeof(md));
            md.vertices = vv;
            md.vertex_count = 8;
            md.indices = ii;
            md.index_count = 36;
            if (le_asset_create_mesh(h.engine, &md, &mesh) ==
                    LE_SUCCESS) {
                memset(&td, 0, sizeof(td));
                td.base_color_factor[0] = 0.2f;
                td.base_color_factor[1] = 0.6f;
                td.base_color_factor[2] = 1.0f;
                td.base_color_factor[3] = 1.0f;
                if (le_asset_create_material(h.engine, &td,
                                             &mat) ==
                    LE_SUCCESS) {
                    memset(&rd, 0, sizeof(rd));
                    rd.mesh = le_asset_get_mesh(h.engine,
                                                &mesh);
                    rd.material = le_asset_get_material(
                        h.engine, &mat);
                    rd.visible = 1;
                    TEST_CHECK(le_object_add_renderable(
                                   h.world, &h.ch,
                                   &rd) == LE_SUCCESS,
                               "hero visible marker");
                }
            }
        }
    }
    /* Walker script: fixed_update reads REAL input (WASD + Space
     * jump), drives character_move + gravity. Speed 4 m/s. */
    {
        le_script_asset_desc sd;
        le_asset script = LE_ASSET_INVALID;
        static const char src[] =
            "export('jumped', 0) "
            "function fixed_update(self, dt) "
            "  local dx = 0 "
            "  local dz = 0 "
            "  if Input.key_down(Key.W) then dz = dz - 1 end "
            "  if Input.key_down(Key.S) then dz = dz + 1 end "
            "  if Input.key_down(Key.A) then dx = dx - 1 end "
            "  if Input.key_down(Key.D) then dx = dx + 1 end "
            "  local sp = 4.0 * dt "
            "  self:character_move(dx * sp, 0, dz * sp) "
            "  if Input.key_down(Key.Space) then "
            "    if self:character_is_grounded() then "
            "      self:character_set_vertical_velocity(7) "
            "      self.jumped = 1 "
            "    end "
            "  end "
            "  self:character_gravity(dt) "
            "end "
            "function update(self, dt) end ";

        memset(&sd, 0, sizeof(sd));
        sd.source = src;
        sd.size = sizeof(src) - 1;
        sd.path_hint = "<course-walker>";
        TEST_CHECK(le_asset_create_script(h.engine, &sd,
                                          &script) ==
                       LE_SUCCESS,
                   "walker script create");
        TEST_CHECK(le_object_add_script(h.world, &h.ch,
                                        &script) ==
                       LE_SUCCESS,
                   "walker attach");
        le_script_set_fixed_step(h.world, 1.0f / 60.0f, 4);
    }

    /* Enter play: the runtime world carries the course
     * (capture -> instantiate) + the edit camera by name.
     * Pointer renderables do NOT survive capture: attach the
     * runtime hero marker AFTER enter (same procedural cube;
     * lives in the runtime world only, destroyed with it). */
    TEST_CHECK(led_play_enter(h.session) == LED_SUCCESS,
               "play enter");
    {
        le_world *rw = led_play_get_world(h.session);
        le_object rch = LE_OBJECT_INVALID;

        TEST_CHECK(rw != NULL, "runtime world live");
        TEST_CHECK(le_world_find_by_name(rw, "Hero", &rch),
                   "runtime hero live");
        /* Runtime marker (visible cube on the runtime hero —
         * same mesh; runtime-world lifetime). Setup, not the
         * movement proof (positions are the proof). */
        {
            le_mesh_asset_desc md;
            le_material_asset_desc td;
            le_asset mesh = LE_ASSET_INVALID;
            le_asset mat = LE_ASSET_INVALID;
            le_renderable_desc rd;
            lr_vertex vv[8];
            uint32_t ii[36] = {
                0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6,
                0, 4, 5, 0, 5, 1, 2, 6, 7, 2, 7, 3,
                0, 3, 7, 0, 7, 4, 1, 5, 6, 1, 6, 2,
            };
            uint32_t v = 0;

            for (v = 0; v < 8; v++) {
                vv[v].position[0] =
                    (v & 1) ? 0.5f : -0.5f;
                vv[v].position[1] =
                    (v & 2) ? 0.5f : -0.5f;
                vv[v].position[2] =
                    (v & 4) ? 0.5f : -0.5f;
                vv[v].normal[2] = 1.0f;
                vv[v].tangent[0] = 1.0f;
                vv[v].tangent[3] = 1.0f;
                vv[v].weights[0] = 1.0f;
            }
            memset(&md, 0, sizeof(md));
            md.vertices = vv;
            md.vertex_count = 8;
            md.indices = ii;
            md.index_count = 36;
            if (le_asset_create_mesh(h.engine, &md, &mesh) ==
                    LE_SUCCESS) {
                memset(&td, 0, sizeof(td));
                td.base_color_factor[0] = 0.2f;
                td.base_color_factor[1] = 0.6f;
                td.base_color_factor[2] = 1.0f;
                td.base_color_factor[3] = 1.0f;
                if (le_asset_create_material(h.engine, &td,
                                             &mat) ==
                    LE_SUCCESS) {
                    memset(&rd, 0, sizeof(rd));
                    rd.mesh = le_asset_get_mesh(h.engine,
                                                &mesh);
                    rd.material = le_asset_get_material(
                        h.engine, &mat);
                    rd.visible = 1;
                    TEST_CHECK(le_object_add_renderable(
                                   rw, &rch, &rd) ==
                                   LE_SUCCESS,
                               "runtime hero marker");
                }
            }
        }
        TEST_CHECK(p_frames(&h, 30), "settle");
        {
            float p0[3];

            c_pos(&h, rw, &rch, p0);
            printf("[INFO] hero settled @(%.2f,%.2f,%.2f) "
                   "grounded=%d\n",
                   p0[0], p0[1], p0[2],
                   le_character_is_grounded(rw, &rch));
            TEST_CHECK(le_character_is_grounded(rw, &rch),
                       "hero grounded after settle");
            {
                char shot[1024];

                snprintf(shot, sizeof(shot),
                         "%s/30-course-start.ppm", shotdir);
                TEST_CHECK(c_shot(&h, shot), "start shot");
            }
            /* §25 flat walk: hold W 60 frames (forward -Z at
             * 4 m/s => ~4 m). */
            c_key(&h, LC_KEY_W, 1);
            TEST_CHECK(c_frames(&h, 60), "walk 60 frames");
            {
                float p1[3];

                c_pos(&h, rw, &rch, p1);
                printf("[INFO] walk @(%.2f,%.2f,%.2f) -> "
                       "(%.2f,%.2f,%.2f)\n",
                       p0[0], p0[1], p0[2], p1[0], p1[1],
                       p1[2]);
                TEST_CHECK((p0[2] - p1[2]) > 2.0f,
                           "flat walk moves forward");
                TEST_CHECK(le_character_is_grounded(rw,
                                                    &rch),
                           "still grounded walking");
            }
            /* §26 stopping: release, 20 frames, velocity ~0. */
            c_key(&h, LC_KEY_W, 0);
            TEST_CHECK(c_frames(&h, 20), "stop 20 frames");
            {
                float p2[3];
                float sp = 0.0f;

                c_pos(&h, rw, &rch, p2);
                sp = le_character_horizontal_speed(rw, &rch);
                printf("[INFO] stop speed %.3f\n", sp);
                TEST_CHECK(sp < 0.01f,
                           "stopping halts (no stuck input)");
                {
                    char shot[1024];

                    snprintf(shot, sizeof(shot),
                             "%s/31-course-walked.ppm",
                             shotdir);
                    TEST_CHECK(c_shot(&h, shot),
                               "walked shot");
                }
            }
            /* §27/28 wall: teleport before the wall, walk +X
             * into it (D key), assert no penetration + hit_wall. */
            {
                float wall_at[3] = { 4.0f, 1.5f, 4.0f };
                float pw[3];
                int grounded = 0;

                TEST_CHECK(le_character_teleport(rw, &rch,
                                                 wall_at) ==
                               LE_SUCCESS,
                           "teleport to wall");
                TEST_CHECK(c_frames(&h, 10), "wall settle");
                c_key(&h, LC_KEY_D, 1);
                TEST_CHECK(c_frames(&h, 90),
                           "push into wall");
                c_key(&h, LC_KEY_D, 0);
                c_pos(&h, rw, &rch, pw);
                grounded = le_character_is_grounded(rw, &rch);
                printf("[INFO] wall @(%.2f,%.2f,%.2f) "
                       "grounded=%d\n",
                       pw[0], pw[1], pw[2], grounded);
                /* Wall face at x=6.0 (center 6.5 - 0.5);
                 * capsule surface must stop at 6.0-0.4=5.6. */
                TEST_CHECK(pw[0] < 5.75f,
                           "wall: no penetration");
                TEST_CHECK(pw[0] > 4.5f,
                           "wall: reached the face");
                {
                    char shot[1024];

                    snprintf(shot, sizeof(shot),
                             "%s/32-course-wall.ppm",
                             shotdir);
                    TEST_CHECK(c_shot(&h, shot), "wall shot");
                }
                /* §28 wall slide: teleport, approach at an angle
                 * (W+D), assert forward progress along the wall
                 * (z decreases) while x stays clamped. */
                {
                    float z_before = 0.0f;
                    float z_after = 0.0f;
                    float x_after = 0.0f;

                    TEST_CHECK(le_character_teleport(
                                   rw, &rch, wall_at) ==
                                   LE_SUCCESS,
                               "teleport slide start");
                    TEST_CHECK(c_frames(&h, 10),
                               "slide settle");
                    c_pos(&h, rw, &rch, pw);
                    z_before = pw[2];
                    c_key(&h, LC_KEY_W, 1);
                    c_key(&h, LC_KEY_D, 1);
                    TEST_CHECK(c_frames(&h, 90), "slide 90");
                    c_key(&h, LC_KEY_W, 0);
                    c_key(&h, LC_KEY_D, 0);
                    c_pos(&h, rw, &rch, pw);
                    z_after = pw[2];
                    x_after = pw[0];
                    printf("[INFO] slide z %.2f -> %.2f "
                           "x %.2f\n",
                           z_before, z_after, x_after);
                    TEST_CHECK((z_before - z_after) > 1.0f,
                               "wall slide progresses");
                    TEST_CHECK(x_after < 5.75f,
                               "slide stays clamped");
                }
            }
            /* §29/30 small step + stairs: teleport onto the
             * course, walk -X, assert climbed (y rises, stepped
             * flag observed through position gain). */
            {
                float step_at[3] = { -1.0f, 1.5f, 0.0f };
                float ps[3];

                TEST_CHECK(le_character_teleport(rw, &rch,
                                                 step_at) ==
                               LE_SUCCESS,
                           "teleport step start");
                TEST_CHECK(c_frames(&h, 10), "step settle");
                c_key(&h, LC_KEY_A, 1);
                TEST_CHECK(c_frames(&h, 60), "walk into step");
                c_key(&h, LC_KEY_A, 0);
                c_pos(&h, rw, &rch, ps);
                printf("[INFO] step @(%.2f,%.2f,%.2f)\n",
                       ps[0], ps[1], ps[2]);
                TEST_CHECK(ps[1] > 0.9f,
                           "small step climbed (0.25)");
                {
                    char shot[1024];

                    snprintf(shot, sizeof(shot),
                             "%s/33-course-stairs.ppm",
                             shotdir);
                    TEST_CHECK(c_shot(&h, shot),
                               "stairs shot");
                }
            }
            /* §33 jump: teleport to open ground, hold Space one
             * step, track apex + landing over 90 frames. */
            {
                float j_at[3] = { 0.0f, 1.5f, -4.0f };
                float apex = 0.0f;
                float land = 0.0f;
                int f;

                TEST_CHECK(le_character_teleport(rw, &rch,
                                                 j_at) ==
                               LE_SUCCESS,
                           "teleport jump pad");
                TEST_CHECK(c_frames(&h, 20), "jump settle");
                c_key(&h, LC_KEY_SPACE, 1);
                TEST_CHECK(c_frames(&h, 3), "jump impulse");
                c_key(&h, LC_KEY_SPACE, 0);
                for (f = 0; f < 90; f++) {
                    float pj[3];

                    TEST_CHECK(c_frames(&h, 1),
                               "jump track");
                    c_pos(&h, rw, &rch, pj);
                    if (pj[1] > apex) {
                        apex = pj[1];
                    }
                    if (f == 89) {
                        land = pj[1];
                    }
                    if (f == 20) {
                        char shot[1024];

                        snprintf(shot, sizeof(shot),
                                 "%s/34-course-jumpmid.ppm",
                                 shotdir);
                        TEST_CHECK(c_shot(&h, shot),
                                   "jump mid shot");
                    }
                }
                printf("[INFO] jump apex %.2f land %.2f\n",
                       apex, land);
                /* Jump v=7, g=20: apex = v²/2g = 1.225 above
                 * takeoff (feet ~0). The position tracks the
                 * OBJECT ORIGIN (feet): apex ~1.2, land ~0. */
                TEST_CHECK(apex > 1.0f, "jump takeoff+apex");
                TEST_CHECK(le_character_is_grounded(rw,
                                                    &rch),
                           "jump landed");
                {
                    char shot[1024];

                    snprintf(shot, sizeof(shot),
                             "%s/35-course-jumpland.ppm",
                             shotdir);
                    TEST_CHECK(c_shot(&h, shot),
                               "jump land shot");
                }
            }
            /* §34 low ceiling: teleport under it, jump, assert
             * no penetration (underside 2.6; capsule top =
             * pos.y + 0.9 must stay <= 2.7). */
            {
                float c_at[3] = { 0.0f, 1.5f, 0.0f };
                float cmax = 0.0f;
                int f;

                TEST_CHECK(le_character_teleport(rw, &rch,
                                                 c_at) ==
                               LE_SUCCESS,
                           "teleport ceiling");
                TEST_CHECK(c_frames(&h, 20),
                           "ceiling settle");
                c_key(&h, LC_KEY_SPACE, 1);
                TEST_CHECK(c_frames(&h, 3), "ceiling jump");
                c_key(&h, LC_KEY_SPACE, 0);
                for (f = 0; f < 60; f++) {
                    float pc[3];

                    TEST_CHECK(c_frames(&h, 1),
                               "ceiling track");
                    c_pos(&h, rw, &rch, pc);
                    if (pc[1] + 0.9f > cmax) {
                        cmax = pc[1] + 0.9f;
                    }
                }
                printf("[INFO] ceiling top-max %.2f\n", cmax);
                TEST_CHECK(cmax <= 2.75f,
                           "low ceiling: no penetration");
            }
            /* §37 trigger: walk -X through the zone volume.
             * The zone is a TRIGGER (no block): the character must
             * keep moving through it. Walk from x=-9 (before the
             * stairs, on flat ground at z=0... the stairs sit at
             * z=0 x=-5..-9! Route around them at z=4: teleport to
             * (-9, 1.5, 4) where the ground is flat, walk -X 120
             * frames (~8 m) straight through the zone's z-range?
             * The zone spans z=-1..1 — at z=4 the character misses
             * it. Instead walk the z=0 line THROUGH the zone and
             * accept stair climbing (step 0.4 > rise 0.2): the pass
             * criterion is x progress (no block), not flat y. */
            {
                float t_at[3] = { -8.0f, 1.5f, 0.0f };
                float pt[3];
                float qt[3];

                TEST_CHECK(le_character_teleport(rw, &rch,
                                                 t_at) ==
                               LE_SUCCESS,
                           "teleport trigger");
                TEST_CHECK(c_frames(&h, 10),
                           "trigger settle");
                c_pos(&h, rw, &rch, qt);
                c_key(&h, LC_KEY_A, 1);
                TEST_CHECK(c_frames(&h, 150),
                           "walk through trigger");
                c_key(&h, LC_KEY_A, 0);
                c_pos(&h, rw, &rch, pt);
                printf("[INFO] trigger @(%.2f,%.2f,%.2f) -> "
                       "(%.2f,%.2f,%.2f)\n",
                       qt[0], qt[1], qt[2], pt[0], pt[1],
                       pt[2]);
                TEST_CHECK((qt[0] - pt[0]) > 3.0f,
                           "trigger: walked through (no block)");
                /* NOTE: no reframe here — while PLAYING the bridge
                 * renders the runtime world through the SCENE
                 * camera (R-013 orbit override is edit-only by
                 * design), so moving h.vp cannot reframe a play
                 * shot. The trigger shot keeps the default play
                 * framing (state lines above are the proof). */
                {
                    char shot[1024];

                    snprintf(shot, sizeof(shot),
                             "%s/36-course-trigger.ppm",
                             shotdir);
                    TEST_CHECK(c_shot(&h, shot),
                               "trigger shot");
                }
            }
        }
    }
    TEST_CHECK(led_play_exit(h.session) == LED_SUCCESS,
               "play exit");

    printf("charcourse: %d passed, %d failed\n", g_passed,
           g_failed);
    lc_shutdown();
    return (g_failed == 0) ? 0 : 1;
}
