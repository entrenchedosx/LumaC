/*
 * Headless-safe pipeline tests (Phase 7, revised Phase 13).
 *
 * Only validates argument handling that never touches Vulkan, so ctest
 * passes on machines with no GPU. Pipeline creation takes no swapchain
 * (Phase 13); the structural render-target description is mandatory.
 * Stage matching against live shaders plus real pipeline creation are
 * covered by test_triangle_vulkan.
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
    lc_pipeline *sentinel = (lc_pipeline *)0x1; /* never dereferenced */
    lc_pipeline *out = sentinel;
    lc_device *fake_device = (lc_device *)0x1; /* validity checked first */
    lc_swapchain *fake_target = (lc_swapchain *)0x1;
    lc_graphics_pipeline_desc desc = { 0 };

    printf("Running LumaC pipeline validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    desc.vertex_shader = (lc_shader *)0x1;
    desc.fragment_shader = (lc_shader *)0x1;
    desc.render_target.color_attachment_count = 1;
    desc.render_target.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    desc.render_target.samples = LC_SAMPLE_COUNT_1;

    /* create before init must fail without touching Vulkan */
    out = sentinel;
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, &desc, &out) ==
                   LC_ERROR_NOT_INITIALIZED,
               "create before init -> NOT_INITIALIZED");
    TEST_CHECK(out == NULL, "out cleared to NULL on NOT_INITIALIZED");

    /* NULL-safe helpers before init */
    lc_pipeline_destroy(NULL);
    TEST_CHECK(1, "lc_pipeline_destroy(NULL) does not crash");
    TEST_CHECK(lc_bind_pipeline(NULL, out) == LC_ERROR_INVALID_ARGUMENT,
               "bind(NULL swapchain) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_bind_pipeline(fake_target, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind(NULL pipeline) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_draw(NULL, 3, 0) == LC_ERROR_INVALID_ARGUMENT,
               "draw(NULL swapchain) -> INVALID_ARGUMENT");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL device / desc / out */
    out = sentinel;
    TEST_CHECK(lc_graphics_pipeline_create(NULL, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL device) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL device");
    out = sentinel;
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, NULL, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL desc");
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, &desc, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL out) -> INVALID_ARGUMENT");

    /* A missing structural target is rejected (no legacy inference). */
    {
        lc_graphics_pipeline_desc nodesc = { 0 };

        nodesc.vertex_shader = (lc_shader *)0x1;
        nodesc.fragment_shader = (lc_shader *)0x1;
        out = sentinel;
        TEST_CHECK(lc_graphics_pipeline_create(fake_device, &nodesc, &out) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "create(zero target desc) -> INVALID_ARGUMENT");
        TEST_CHECK(out == NULL, "out cleared on zero target desc");
    }

    /* Non-live handles are rejected without dereferencing garbage. */
    out = sentinel;
    TEST_CHECK(lc_graphics_pipeline_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(dead handles) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on dead handles");
    TEST_CHECK(lc_bind_pipeline(fake_target, out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind(dead handles) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_draw(fake_target, 3, 0) == LC_ERROR_INVALID_ARGUMENT,
               "draw(dead swapchain) -> INVALID_ARGUMENT");

    /* Swapchain target-desc helper validation. */
    {
        lc_render_target_desc rtdesc;

        TEST_CHECK(lc_swapchain_get_render_target_desc(NULL, &rtdesc) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "target desc(NULL swapchain) -> INVALID_ARGUMENT");
        TEST_CHECK(lc_swapchain_get_render_target_desc(fake_target, NULL) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "target desc(NULL out) -> INVALID_ARGUMENT");
        TEST_CHECK(lc_swapchain_get_render_target_desc(fake_target,
                                                       &rtdesc) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "target desc(dead swapchain) -> INVALID_ARGUMENT");
    }

    lc_pipeline_destroy(NULL);
    TEST_CHECK(1, "lc_pipeline_destroy(NULL) after init does not crash");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no pipelines does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
