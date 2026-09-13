/*
 * Headless-safe Phase 11 tests: indexed drawing, instancing, push
 * constants, depth clears, raster state, and depth format queries.
 *
 * Only validates argument handling that never touches Vulkan, so ctest
 * passes on machines with no GPU. Live-object validation plus real
 * indexed/instanced/depth rendering are covered by test_cube_vulkan.
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
    lc_swapchain *fake_swapchain = (lc_swapchain *)0x1;
    lc_pipeline *fake_pipeline = (lc_pipeline *)0x1;
    lc_buffer *fake_buffer = (lc_buffer *)0x1;
    lc_pipeline *out_pipeline = (lc_pipeline *)0x1;
    lc_device *fake_device = (lc_device *)0x1;
    lc_graphics_pipeline_desc desc = { 0 };
    float mvp[16] = { 0 };
    unsigned i;

    for (i = 0; i < 16; i++) {
        mvp[i] = (i % 5 == 0) ? 1.0f : 0.0f;
    }

    printf("Running LumaC Phase 11 headless validation tests...\n");

    lc_shutdown();

    /* Pre-init: new APIs fail without touching Vulkan. */
    TEST_CHECK(lc_bind_index_buffer(fake_swapchain, fake_buffer, 0,
                                    LC_INDEX_UINT16) ==
                   LC_ERROR_NOT_INITIALIZED,
               "bind_index before init -> NOT_INITIALIZED");
    TEST_CHECK(lc_draw_indexed(fake_swapchain, 36, 1, 0, 0, 0) ==
                   LC_ERROR_NOT_INITIALIZED,
               "draw_indexed before init -> NOT_INITIALIZED");
    TEST_CHECK(lc_draw_instanced(fake_swapchain, 3, 1, 0, 0) ==
                   LC_ERROR_NOT_INITIALIZED,
               "draw_instanced before init -> NOT_INITIALIZED");
    TEST_CHECK(lc_push_constants(fake_swapchain, fake_pipeline,
                                 LC_SHADER_VISIBILITY_VERTEX, 0, 64,
                                 mvp) == LC_ERROR_NOT_INITIALIZED,
               "push before init -> NOT_INITIALIZED");
    TEST_CHECK(lc_clear_depth(fake_swapchain, 1.0f) ==
                   LC_ERROR_NOT_INITIALIZED,
               "clear_depth before init -> NOT_INITIALIZED");

    /* NULL-safe before init (NULL checks precede init checks). */
    TEST_CHECK(lc_bind_index_buffer(NULL, fake_buffer, 0,
                                    LC_INDEX_UINT16) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind_index(NULL swapchain) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_bind_index_buffer(fake_swapchain, NULL, 0,
                                    LC_INDEX_UINT16) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind_index(NULL buffer) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_draw_indexed(NULL, 36, 1, 0, 0, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "draw_indexed(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_draw_instanced(NULL, 3, 1, 0, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "draw_instanced(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_push_constants(NULL, fake_pipeline,
                                 LC_SHADER_VISIBILITY_VERTEX, 0, 64,
                                 mvp) == LC_ERROR_INVALID_ARGUMENT,
               "push(NULL swapchain) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_push_constants(fake_swapchain, NULL,
                                 LC_SHADER_VISIBILITY_VERTEX, 0, 64,
                                 mvp) == LC_ERROR_INVALID_ARGUMENT,
               "push(NULL pipeline) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_clear_depth(NULL, 1.0f) == LC_ERROR_INVALID_ARGUMENT,
               "clear_depth(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_swapchain_get_depth_format(NULL) == LC_FORMAT_UNDEFINED,
               "depth_format(NULL) -> UNDEFINED");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL device/desc/out for pipeline creation (Phase 13: no
     * swapchain parameter; structural target mandatory). */
    out_pipeline = (lc_pipeline *)0x1;
    desc.vertex_shader = (lc_shader *)0x1;
    desc.fragment_shader = (lc_shader *)0x1;
    desc.render_target.color_attachment_count = 1;
    desc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    desc.render_target.samples = LC_SAMPLE_COUNT_1;
    TEST_CHECK(lc_graphics_pipeline_create(NULL, &desc, &out_pipeline) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL device) -> INVALID_ARGUMENT");
    TEST_CHECK(out_pipeline == NULL, "out cleared on NULL device");
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, NULL,
                                           &out_pipeline) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, &desc, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL out) -> INVALID_ARGUMENT");

    /* Dead handles rejected without dereferencing. */
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, &desc,
                                           &out_pipeline) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(dead handles) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_bind_index_buffer(fake_swapchain, fake_buffer, 0,
                                    LC_INDEX_UINT16) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind_index(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_bind_index_buffer(fake_swapchain, fake_buffer, 0,
                                    (lc_index_type)99) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind_index(bad type, dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_draw_indexed(fake_swapchain, 36, 1, 0, 0, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "draw_indexed(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_draw_indexed(fake_swapchain, 0, 1, 0, 0, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "draw_indexed(dead, zero count) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_draw_instanced(fake_swapchain, 3, 1, 0, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "draw_instanced(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_draw_instanced(fake_swapchain, 0, 1, 0, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "draw_instanced(dead, zero count) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_draw_instanced(fake_swapchain, 3, 0, 0, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "draw_instanced(dead, zero instances) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_push_constants(fake_swapchain, fake_pipeline,
                                 LC_SHADER_VISIBILITY_VERTEX, 0, 64,
                                 mvp) == LC_ERROR_INVALID_ARGUMENT,
               "push(dead) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_push_constants(fake_swapchain, fake_pipeline, 0, 0, 64,
                                 mvp) == LC_ERROR_INVALID_ARGUMENT,
               "push(dead, zero visibility) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_push_constants(fake_swapchain, fake_pipeline,
                                 LC_SHADER_VISIBILITY_VERTEX, 0, 64,
                                 NULL) == LC_ERROR_INVALID_ARGUMENT,
               "push(dead, NULL data) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_push_constants(fake_swapchain, fake_pipeline,
                                 LC_SHADER_VISIBILITY_VERTEX, 1, 64,
                                 mvp) == LC_ERROR_INVALID_ARGUMENT,
               "push(dead, misaligned offset) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_clear_depth(fake_swapchain, 1.0f) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "clear_depth(dead) -> INVALID_ARGUMENT");

    /* Push validation rejects bad windows even against dead handles
     * (caught before any backend use where detectable). */
    TEST_CHECK(lc_push_constants(fake_swapchain, fake_pipeline,
                                 LC_SHADER_VISIBILITY_VERTEX, 0, 0,
                                 mvp) == LC_ERROR_INVALID_ARGUMENT,
               "push(dead, zero size) -> INVALID_ARGUMENT");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no Phase 11 objects does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
