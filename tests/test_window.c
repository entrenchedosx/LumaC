/*
 * Headless-safe window tests (Phase 2).
 *
 * Only validates argument handling and lifecycle paths that never touch
 * the native OS window system, so ctest passes on CI/headless machines
 * with no display server. Real window creation is verified manually via
 * examples/window.
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
    lc_window *dummy = (lc_window *)0x1; /* never dereferenced; validity checked first */
    lc_window_desc bad;
    lc_result r;

    printf("Running LumaC window validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    /* create before init must fail without touching the OS */
    bad.title = "x";
    bad.width = 800;
    bad.height = 600;
    r = lc_window_create(&bad, &dummy);
    TEST_CHECK(r == LC_ERROR_NOT_INITIALIZED, "create before init -> NOT_INITIALIZED");

    /* NULL-safe helpers before init */
    lc_window_destroy(NULL);
    TEST_CHECK(1, "lc_window_destroy(NULL) does not crash");
    lc_poll_events();
    TEST_CHECK(1, "lc_poll_events() without init does not crash");
    TEST_CHECK(lc_window_should_close(NULL) != 0, "should_close(NULL) is true");
    TEST_CHECK(lc_window_get_width(NULL) == 0, "get_width(NULL) == 0");
    TEST_CHECK(lc_window_get_height(NULL) == 0, "get_height(NULL) == 0");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL descriptor / output */
    TEST_CHECK(lc_window_create(NULL, &dummy) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_window_create(&bad, NULL) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL out) -> INVALID_ARGUMENT");

    /* zero dimensions (rejected before any OS call) */
    bad.title = "LumaC";
    bad.width = 0;
    bad.height = 600;
    TEST_CHECK(lc_window_create(&bad, &dummy) == LC_ERROR_INVALID_ARGUMENT,
               "create width=0 -> INVALID_ARGUMENT");
    bad.width = 800;
    bad.height = 0;
    TEST_CHECK(lc_window_create(&bad, &dummy) == LC_ERROR_INVALID_ARGUMENT,
               "create height=0 -> INVALID_ARGUMENT");
    bad.width = 0;
    bad.height = 0;
    TEST_CHECK(lc_window_create(&bad, &dummy) == LC_ERROR_INVALID_ARGUMENT,
               "create 0x0 -> INVALID_ARGUMENT");

    /* NULL title with zero size is still rejected (no OS window attempted) */
    bad.title = NULL;
    TEST_CHECK(lc_window_create(&bad, &dummy) == LC_ERROR_INVALID_ARGUMENT,
               "create NULL title 0x0 -> INVALID_ARGUMENT");

    /* polling with no windows is safe */
    lc_poll_events();
    TEST_CHECK(1, "lc_poll_events() with no windows does not crash");

    lc_window_destroy(NULL);
    TEST_CHECK(1, "lc_window_destroy(NULL) after init does not crash");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no windows does not crash");

    /* poll/destroy safe after shutdown */
    lc_poll_events();
    TEST_CHECK(1, "lc_poll_events() after shutdown does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
