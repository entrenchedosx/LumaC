/*
 * Headless-safe frame tests (Phase 6).
 *
 * Only validates argument handling and lifecycle paths that never touch
 * Vulkan or the native window system, so ctest passes on machines with
 * no GPU or display server. Misuse against live swapchains (clear/end
 * without a frame, begin twice) plus real rendering are covered by
 * test_frame_vulkan and examples/clear_screen.
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
    lc_swapchain *fake = (lc_swapchain *)0x1; /* validity checked first */

    printf("Running LumaC frame validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    /* Frame calls before init must fail without touching Vulkan. */
    TEST_CHECK(lc_begin_frame(fake) == LC_ERROR_NOT_INITIALIZED,
               "begin before init -> NOT_INITIALIZED");
    TEST_CHECK(lc_clear_color(fake, 0.0f, 0.0f, 0.0f, 1.0f) ==
                   LC_ERROR_NOT_INITIALIZED,
               "clear before init -> NOT_INITIALIZED");
    TEST_CHECK(lc_end_frame(fake) == LC_ERROR_NOT_INITIALIZED,
               "end before init -> NOT_INITIALIZED");

    /* NULL-safe helpers before init */
    TEST_CHECK(lc_begin_frame(NULL) == LC_ERROR_INVALID_ARGUMENT,
               "begin(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_clear_color(NULL, 0.0f, 0.0f, 0.0f, 1.0f) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "clear(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_end_frame(NULL) == LC_ERROR_INVALID_ARGUMENT,
               "end(NULL) -> INVALID_ARGUMENT");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* Non-live handles are rejected without dereferencing garbage. */
    TEST_CHECK(lc_begin_frame(fake) == LC_ERROR_INVALID_ARGUMENT,
               "begin(dead handle) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_clear_color(fake, 0.0f, 0.0f, 0.0f, 1.0f) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "clear(dead handle) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_end_frame(fake) == LC_ERROR_INVALID_ARGUMENT,
               "end(dead handle) -> INVALID_ARGUMENT");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no frames does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
