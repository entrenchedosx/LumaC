/*
 * Headless-safe binding tests (Phase 10).
 *
 * Only validates argument handling that never touches Vulkan, so ctest
 * passes on machines with no GPU. Layout content, set updates, and
 * real descriptor allocation live in test_textured_vulkan.
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
    lc_binding_layout *layout_sentinel = (lc_binding_layout *)0x1;
    lc_binding_layout *layout_out = layout_sentinel;
    lc_binding_set *set_sentinel = (lc_binding_set *)0x1;
    lc_binding_set *set_out = set_sentinel;
    lc_device *fake_device = (lc_device *)0x1; /* validity checked first */
    lc_swapchain *fake_swapchain = (lc_swapchain *)0x1;
    lc_pipeline *fake_pipeline = (lc_pipeline *)0x1;
    lc_binding_desc slot;
    lc_binding_layout_desc desc;
    lc_binding_write write;

    printf("Running LumaC binding validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    slot.binding = 0;
    slot.type = LC_BINDING_UNIFORM_BUFFER;
    slot.count = 1;
    slot.visibility = LC_SHADER_VISIBILITY_VERTEX;
    desc.bindings = &slot;
    desc.binding_count = 1;

    /* create before init must fail without touching Vulkan */
    layout_out = layout_sentinel;
    TEST_CHECK(lc_binding_layout_create(fake_device, &desc, &layout_out) ==
                   LC_ERROR_NOT_INITIALIZED,
               "layout before init -> NOT_INITIALIZED");
    TEST_CHECK(layout_out == NULL, "out cleared to NULL on NOT_INITIALIZED");
    set_out = set_sentinel;
    TEST_CHECK(lc_binding_set_create(layout_sentinel, &set_out) ==
                   LC_ERROR_NOT_INITIALIZED,
               "set before init -> NOT_INITIALIZED");
    TEST_CHECK(set_out == NULL, "out cleared to NULL on NOT_INITIALIZED");

    /* NULL-safe helpers before init */
    lc_binding_layout_destroy(NULL);
    lc_binding_set_destroy(NULL);
    TEST_CHECK(1, "destroy(NULL) helpers do not crash");
    TEST_CHECK(lc_bind_binding_set(NULL, fake_pipeline, 0, set_sentinel) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind(NULL swapchain) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_bind_binding_set(fake_swapchain, NULL, 0, set_sentinel) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind(NULL pipeline) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_bind_binding_set(fake_swapchain, fake_pipeline, 0, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind(NULL set) -> INVALID_ARGUMENT");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL device / desc / out */
    layout_out = layout_sentinel;
    TEST_CHECK(lc_binding_layout_create(NULL, &desc, &layout_out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "layout(NULL device) -> INVALID_ARGUMENT");
    TEST_CHECK(layout_out == NULL, "out cleared to NULL on NULL device");
    layout_out = layout_sentinel;
    TEST_CHECK(lc_binding_layout_create(fake_device, NULL, &layout_out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "layout(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(layout_out == NULL, "out cleared to NULL on NULL desc");
    TEST_CHECK(lc_binding_layout_create(fake_device, &desc, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "layout(NULL out) -> INVALID_ARGUMENT");

    /* Non-live handles rejected without Vulkan. */
    layout_out = layout_sentinel;
    TEST_CHECK(lc_binding_layout_create(fake_device, &desc, &layout_out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "layout(dead device) -> INVALID_ARGUMENT");
    TEST_CHECK(layout_out == NULL, "out cleared to NULL on dead device");
    set_out = set_sentinel;
    TEST_CHECK(lc_binding_set_create(layout_sentinel, &set_out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "set(dead layout) -> INVALID_ARGUMENT");
    TEST_CHECK(set_out == NULL, "out cleared to NULL on dead layout");

    /* Update guards (dead set; never Vulkan). */
    write.binding = 0;
    write.array_element = 0;
    write.type = LC_BINDING_UNIFORM_BUFFER;
    write.u.buffer.buffer = (lc_buffer *)0x1;
    write.u.buffer.offset = 0;
    write.u.buffer.size = 0;
    TEST_CHECK(lc_binding_set_update(set_sentinel, &write, 1) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "update(dead set) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_binding_set_update(set_sentinel, NULL, 1) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "update(dead set, NULL writes) -> INVALID_ARGUMENT");

    /* Bind guards (dead handles; never Vulkan). */
    TEST_CHECK(lc_bind_binding_set(fake_swapchain, fake_pipeline, 0,
                                   set_sentinel) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind(dead handles) -> INVALID_ARGUMENT");

    lc_binding_layout_destroy(NULL);
    lc_binding_set_destroy(NULL);
    TEST_CHECK(1, "destroy(NULL) after init does not crash");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no bindings does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
