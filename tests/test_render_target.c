/*
 * Headless-safe Phase 12 tests: render-target descriptions, target
 * objects, encoder access, and render-pass/compat validation paths
 * that never touch Vulkan.
 *
 * Live-object rendering (offscreen, MRT pixels, two-pass sampling,
 * sharing, resize independence) is covered by test_rendertarget_vulkan.
 */
#include <stdio.h>
#include <lumac/lumac.h>

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

int main(void) {
    lc_device *fake_device = (lc_device *)0x1;
    lc_swapchain *fake_swapchain = (lc_swapchain *)0x1;
    lc_render_target *fake_target = (lc_render_target *)0x1;
    lc_command_encoder *fake_enc = (lc_command_encoder *)0x1;
    lc_pipeline *fake_pipeline = (lc_pipeline *)0x1;
    lc_buffer *fake_buffer = (lc_buffer *)0x1;
    lc_image_view *fake_view = (lc_image_view *)0x1;
    lc_binding_set *fake_set = (lc_binding_set *)0x1;
    lc_render_target *out_target = (lc_render_target *)0x1;
    lc_command_encoder *out_enc = (lc_command_encoder *)0x1;
    lc_render_target_create_desc tdesc = { 0 };
    lc_render_target_attachment att;
    lc_render_pass_desc pass = { 0 };
    lc_render_color_attachment catt;
    lc_render_depth_attachment datt;
    lc_render_swapchain_pass_desc spass = { 0 };
    lc_graphics_pipeline_desc pdesc = { 0 };
    lc_pipeline *out_pipeline = (lc_pipeline *)0x1;
    float mvp[16] = { 0 };

    printf("Running LumaC Phase 12 headless validation tests...\n");

    lc_shutdown();

    /* Pre-init failures (NULL checks first where applicable). */
    TEST_CHECK(lc_render_target_create(fake_device, &tdesc, &out_target) ==
                   LC_ERROR_NOT_INITIALIZED,
               "target create before init -> NOT_INITIALIZED");
    TEST_CHECK(out_target == NULL, "out cleared on NOT_INITIALIZED");
    TEST_CHECK(lc_swapchain_get_encoder(fake_swapchain, &out_enc) ==
                   LC_ERROR_NOT_INITIALIZED,
               "get_encoder before init -> NOT_INITIALIZED");
    TEST_CHECK(lc_encoder_begin_render_pass(fake_enc, &pass) ==
                   LC_ERROR_NOT_INITIALIZED,
               "begin pass before init -> NOT_INITIALIZED");
    TEST_CHECK(lc_encoder_begin_swapchain_pass(fake_enc, fake_swapchain,
                                               &spass) ==
                   LC_ERROR_NOT_INITIALIZED,
               "begin swapchain pass before init -> NOT_INITIALIZED");
    TEST_CHECK(lc_encoder_end_render_pass(fake_enc) ==
                   LC_ERROR_NOT_INITIALIZED,
               "end pass before init -> NOT_INITIALIZED");
    TEST_CHECK(lc_encoder_bind_pipeline(fake_enc, fake_pipeline) ==
                   LC_ERROR_NOT_INITIALIZED,
               "encoder bind before init -> NOT_INITIALIZED");
    TEST_CHECK(lc_encoder_draw(fake_enc, 3, 0) == LC_ERROR_NOT_INITIALIZED,
               "encoder draw before init -> NOT_INITIALIZED");

    /* NULL-safe before init. */
    TEST_CHECK(lc_render_target_create(NULL, &tdesc, &out_target) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "target create(NULL device) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_render_target_create(fake_device, NULL, &out_target) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "target create(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_render_target_create(fake_device, &tdesc, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "target create(NULL out) -> INVALID_ARGUMENT");
    lc_render_target_destroy(NULL);
    TEST_CHECK(1, "target destroy(NULL) does not crash");
    TEST_CHECK(lc_render_target_get_width(NULL) == 0,
               "target width(NULL) -> 0");
    TEST_CHECK(lc_render_target_get_height(NULL) == 0,
               "target height(NULL) -> 0");
    TEST_CHECK(lc_render_target_get_color_count(NULL) == 0,
               "target colors(NULL) -> 0");
    TEST_CHECK(lc_render_target_get_color_format(NULL, 0) ==
                   LC_FORMAT_UNDEFINED,
               "target color format(NULL) -> UNDEFINED");
    TEST_CHECK(lc_render_target_get_depth_format(NULL) ==
                   LC_FORMAT_UNDEFINED,
               "target depth format(NULL) -> UNDEFINED");
    TEST_CHECK(lc_render_target_is_compatible_with_pipeline(NULL, NULL) == 0,
               "compat(NULL, NULL) -> 0");
    TEST_CHECK(lc_swapchain_get_render_target(NULL) == NULL,
               "swapchain target(NULL) -> NULL");
    TEST_CHECK(lc_swapchain_get_encoder(NULL, &out_enc) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "get_encoder(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_begin_render_pass(NULL, &pass) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "begin pass(NULL enc) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_begin_render_pass(fake_enc, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "begin pass(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_end_render_pass(NULL) == LC_ERROR_INVALID_ARGUMENT,
               "end pass(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_bind_pipeline(NULL, fake_pipeline) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "encoder bind(NULL enc) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_draw(NULL, 3, 0) == LC_ERROR_INVALID_ARGUMENT,
               "encoder draw(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_draw_indexed(NULL, 3, 1, 0, 0, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "encoder draw_indexed(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_draw_instanced(NULL, 3, 1, 0, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "encoder draw_instanced(NULL) -> INVALID_ARGUMENT");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* Dead handles rejected without dereferencing. */
    out_target = (lc_render_target *)0x1;
    tdesc.width = 64;
    tdesc.height = 64;
    att.view = fake_view;
    tdesc.color_attachments = &att;
    tdesc.color_attachment_count = 1;
    tdesc.depth_stencil_attachment = NULL;
    TEST_CHECK(lc_render_target_create(fake_device, &tdesc, &out_target) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "target create(dead device) -> INVALID_ARGUMENT");
    TEST_CHECK(out_target == NULL, "out cleared on dead device");
    TEST_CHECK(lc_swapchain_get_encoder(fake_swapchain, &out_enc) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "get_encoder(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(out_enc == NULL, "encoder out cleared on dead swapchain");
    TEST_CHECK(lc_encoder_begin_render_pass(fake_enc, &pass) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "begin pass(dead enc) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_begin_swapchain_pass(fake_enc, fake_swapchain,
                                               &spass) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "begin swapchain pass(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_end_render_pass(fake_enc) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "end pass(dead enc) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_bind_pipeline(fake_enc, fake_pipeline) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "encoder bind(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_bind_binding_set(fake_enc, fake_pipeline, 0,
                                           fake_set) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "encoder bind set(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_bind_vertex_buffer(fake_enc, 0, fake_buffer, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "encoder bind vertex(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_bind_index_buffer(fake_enc, fake_buffer, 0,
                                            LC_INDEX_UINT16) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "encoder bind index(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_push_constants(fake_enc, fake_pipeline,
                                         LC_SHADER_VISIBILITY_VERTEX, 0, 64,
                                         mvp) == LC_ERROR_INVALID_ARGUMENT,
               "encoder push(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_draw(fake_enc, 3, 0) == LC_ERROR_INVALID_ARGUMENT,
               "encoder draw(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_draw_indexed(fake_enc, 3, 1, 0, 0, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "encoder draw_indexed(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_encoder_draw_instanced(fake_enc, 3, 1, 0, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "encoder draw_instanced(dead) -> INVALID_ARGUMENT");

    /* Shape validation with dead views (rejected as dead, never
     * dereferenced). */
    tdesc.width = 0;
    TEST_CHECK(lc_render_target_create(fake_device, &tdesc, &out_target) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "target create(dead, zero extent) -> INVALID_ARGUMENT");
    tdesc.width = 64;
    tdesc.color_attachment_count = 9;
    TEST_CHECK(lc_render_target_create(fake_device, &tdesc, &out_target) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "target create(dead, too many colors) -> INVALID_ARGUMENT");
    tdesc.color_attachment_count = 0;
    TEST_CHECK(lc_render_target_create(fake_device, &tdesc, &out_target) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "target create(dead, no attachments) -> INVALID_ARGUMENT");

    /* Structural pipeline descs with dead devices (rejected before
     * touching Vulkan; Phase 13: no swapchain parameter, no legacy
     * inference — an all-zero target is simply invalid). */
    pdesc.vertex_shader = (lc_shader *)0x1;
    pdesc.fragment_shader = (lc_shader *)0x1;
    pdesc.render_target.color_attachment_count = 0;
    pdesc.render_target.depth_stencil_format = LC_FORMAT_UNDEFINED;
    pdesc.render_target.samples = 0;
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, &pdesc,
                                           &out_pipeline) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "pipeline(zero target) -> INVALID_ARGUMENT");
    pdesc.render_target.color_attachment_count = 1;
    pdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    pdesc.render_target.samples = LC_SAMPLE_COUNT_1;
    pdesc.render_target.depth_stencil_format = LC_FORMAT_UNDEFINED;
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, &pdesc,
                                           &out_pipeline) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "structural pipeline(dead device) -> INVALID_ARGUMENT");
    pdesc.render_target.color_attachment_count = 9;
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, &pdesc,
                                           &out_pipeline) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "pipeline(too many colors) -> INVALID_ARGUMENT");
    pdesc.render_target.color_attachment_count = 1;
    pdesc.render_target.color_formats[0] = LC_FORMAT_D32_FLOAT;
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, &pdesc,
                                           &out_pipeline) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "pipeline(depth as color) -> INVALID_ARGUMENT");
    pdesc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    pdesc.render_target.depth_stencil_format = LC_FORMAT_RGBA8_UNORM;
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, &pdesc,
                                           &out_pipeline) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "pipeline(color as depth) -> INVALID_ARGUMENT");
    pdesc.render_target.depth_stencil_format = LC_FORMAT_UNDEFINED;
    pdesc.depth_test_enable = 1;
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, &pdesc,
                                           &out_pipeline) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "pipeline(depth test, no depth) -> INVALID_ARGUMENT");
    pdesc.depth_test_enable = 0;

    /* Pass-shape validation with dead encoders. */
    catt.view = fake_view;
    catt.load_op = (lc_load_op)99;
    catt.store_op = LC_STORE_OP_STORE;
    pass.color_attachments = &catt;
    pass.color_attachment_count = 1;
    pass.depth_attachment = NULL;
    pass.width = 64;
    pass.height = 64;
    TEST_CHECK(lc_encoder_begin_render_pass(fake_enc, &pass) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "begin pass(dead enc, bad enum) -> INVALID_ARGUMENT");
    pass.color_attachment_count = 0;
    TEST_CHECK(lc_encoder_begin_render_pass(fake_enc, &pass) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "begin pass(dead enc, no attachments) -> INVALID_ARGUMENT");
    datt.view = NULL;
    datt.depth_load_op = LC_LOAD_OP_CLEAR;
    datt.depth_store_op = LC_STORE_OP_STORE;
    datt.clear_depth = 1.0f;
    datt.stencil_load_op = LC_LOAD_OP_DONT_CARE;
    datt.stencil_store_op = LC_STORE_OP_DONT_CARE;
    datt.clear_stencil = 0;
    (void)datt;

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no targets does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
