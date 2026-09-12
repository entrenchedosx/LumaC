/*
 * Headless-safe surface tests (Phase 4).
 *
 * Only validates argument handling and lifecycle paths that never touch
 * Vulkan or the native window system, so ctest passes on machines with
 * no GPU or display server. Real surface creation is covered by
 * test_surface_vulkan and examples/surface_info.
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
    lc_surface *sentinel = (lc_surface *)0x1; /* never dereferenced */
    lc_surface *out = sentinel;
    lc_device *fake_device = (lc_device *)0x1; /* validity checked first */
    lc_window *fake_window = (lc_window *)0x1; /* validity checked first */
    lc_result r;

    printf("Running LumaC surface validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    /* create before init must fail without touching Vulkan */
    out = sentinel;
    r = lc_surface_create(fake_device, fake_window, &out);
    TEST_CHECK(r == LC_ERROR_NOT_INITIALIZED, "create before init -> NOT_INITIALIZED");
    TEST_CHECK(out == NULL, "out cleared to NULL on NOT_INITIALIZED");

    /* NULL-safe helpers before init */
    lc_surface_destroy(NULL);
    TEST_CHECK(1, "lc_surface_destroy(NULL) does not crash");
    TEST_CHECK(lc_surface_is_present_supported(NULL) == 0,
               "is_present_supported(NULL) == 0");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL device / window / output */
    out = sentinel;
    TEST_CHECK(lc_surface_create(NULL, fake_window, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL device) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL device");
    out = sentinel;
    TEST_CHECK(lc_surface_create(fake_device, NULL, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL window) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL window");
    TEST_CHECK(lc_surface_create(fake_device, fake_window, NULL) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL out) -> INVALID_ARGUMENT");

    /* Non-live handles are rejected (no dereference of garbage) */
    out = sentinel;
    TEST_CHECK(lc_surface_create(fake_device, fake_window, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create(dead device/window) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on dead handles");

    lc_surface_destroy(NULL);
    TEST_CHECK(1, "lc_surface_destroy(NULL) after init does not crash");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no surfaces does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
