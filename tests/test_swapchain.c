/*
 * Headless-safe swapchain tests (Phase 5).
 *
 * Only validates argument handling and lifecycle paths that never touch
 * Vulkan or the native window system, so ctest passes on machines with
 * no GPU or display server. Real swapchain creation, recreation, and
 * dependency cleanup are covered by test_swapchain_vulkan and
 * examples/swapchain_info.
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
    lc_swapchain *sentinel = (lc_swapchain *)0x1; /* never dereferenced */
    lc_swapchain *out = sentinel;
    lc_device *fake_device = (lc_device *)0x1; /* validity checked first */
    lc_surface *fake_surface = (lc_surface *)0x1; /* validity checked first */
    lc_swapchain_desc desc = { 0 };
    lc_result r;

    printf("Running LumaC swapchain validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    desc.width = 800;
    desc.height = 600;
    desc.image_count = 0;
    desc.vsync = 1;

    /* create before init must fail without touching Vulkan */
    out = sentinel;
    r = lc_swapchain_create(fake_device, fake_surface, &desc, &out);
    TEST_CHECK(r == LC_ERROR_NOT_INITIALIZED, "create before init -> NOT_INITIALIZED");
    TEST_CHECK(out == NULL, "out cleared to NULL on NOT_INITIALIZED");

    /* NULL-safe helpers before init */
    lc_swapchain_destroy(NULL);
    TEST_CHECK(1, "lc_swapchain_destroy(NULL) does not crash");
    TEST_CHECK(lc_swapchain_recreate(NULL, 800, 600) == LC_ERROR_INVALID_ARGUMENT,
               "recreate(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_swapchain_get_width(NULL) == 0, "get_width(NULL) == 0");
    TEST_CHECK(lc_swapchain_get_height(NULL) == 0, "get_height(NULL) == 0");
    TEST_CHECK(lc_swapchain_get_image_count(NULL) == 0, "get_image_count(NULL) == 0");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL device / surface / desc / out */
    out = sentinel;
    TEST_CHECK(lc_swapchain_create(NULL, fake_surface, &desc, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL device) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL device");
    out = sentinel;
    TEST_CHECK(lc_swapchain_create(fake_device, NULL, &desc, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL surface) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL surface");
    out = sentinel;
    TEST_CHECK(lc_swapchain_create(fake_device, fake_surface, NULL, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL desc");
    TEST_CHECK(lc_swapchain_create(fake_device, fake_surface, &desc, NULL) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL out) -> INVALID_ARGUMENT");

    /* zero dimensions rejected before any Vulkan work */
    desc.width = 0;
    out = sentinel;
    TEST_CHECK(lc_swapchain_create(fake_device, fake_surface, &desc, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create width=0 -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on width=0");
    desc.width = 800;
    desc.height = 0;
    out = sentinel;
    TEST_CHECK(lc_swapchain_create(fake_device, fake_surface, &desc, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create height=0 -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on height=0");
    desc.height = 600;

    /* Non-live handles are rejected without dereferencing garbage */
    out = sentinel;
    TEST_CHECK(lc_swapchain_create(fake_device, fake_surface, &desc, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create(dead device/surface) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on dead handles");

    /* recreate guards */
    TEST_CHECK(lc_swapchain_recreate(NULL, 800, 600) == LC_ERROR_INVALID_ARGUMENT,
               "recreate(NULL) after init -> INVALID_ARGUMENT");
    TEST_CHECK(lc_swapchain_recreate(sentinel, 800, 600) == LC_ERROR_INVALID_ARGUMENT,
               "recreate(dead handle) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_swapchain_recreate(sentinel, 0, 600) == LC_ERROR_ZERO_EXTENT,
               "recreate(dead handle, w=0) -> ZERO_EXTENT");

    lc_swapchain_destroy(NULL);
    TEST_CHECK(1, "lc_swapchain_destroy(NULL) after init does not crash");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no swapchains does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
