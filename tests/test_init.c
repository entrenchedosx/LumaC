#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <lumac/lumac.h>

static int g_tests_passed = 0;
static int g_tests_failed = 0;

#define TEST_CHECK(cond, msg) do { \
    if (cond) { \
        printf("[PASS] %s\n", msg); \
        g_tests_passed++; \
    } else { \
        printf("[FAIL] %s\n", msg); \
        g_tests_failed++; \
    } \
} while (0)

int main(void) {
    printf("Running LumaC init tests...\n");

    /* Test 1: lc_get_version_string returns valid string */
    const char *version = lc_get_version_string();
    TEST_CHECK(version != NULL, "lc_get_version_string() != NULL");
    TEST_CHECK(strcmp(version, "0.1.0") == 0, "version == \"0.1.0\"");
    TEST_CHECK(strcmp(version, LC_VERSION_STRING) == 0, "version == LC_VERSION_STRING");
    TEST_CHECK(LC_VERSION_MAJOR == 0 && LC_VERSION_MINOR == 1 && LC_VERSION_PATCH == 0,
               "version macros == 0.1.0");

    /* Ensure clean state - shutdown if previously initialized */
    lc_shutdown();

    /* Test 2: first lc_init succeeds */
    lc_result r1 = lc_init();
    TEST_CHECK(r1 == LC_SUCCESS, "first lc_init() == LC_SUCCESS");

    /* Test 3: second lc_init fails with LC_ERROR_ALREADY_INITIALIZED */
    lc_result r2 = lc_init();
    TEST_CHECK(r2 == LC_ERROR_ALREADY_INITIALIZED,
               "second lc_init() == LC_ERROR_ALREADY_INITIALIZED");

    /* Test 4: lc_shutdown works */
    lc_shutdown();
    /* No crash is implicit pass; verify we can re-init */
    TEST_CHECK(1, "lc_shutdown() after init does not crash");

    /* Test 5: lc_init succeeds again after shutdown */
    lc_result r3 = lc_init();
    TEST_CHECK(r3 == LC_SUCCESS, "lc_init() after shutdown == LC_SUCCESS");
    lc_shutdown();

    /* Test 6: calling lc_shutdown twice does not crash */
    lc_shutdown();
    lc_shutdown();
    TEST_CHECK(1, "calling lc_shutdown() twice does not crash");

    /* Test 7: verify re-init after double shutdown still works */
    lc_result r4 = lc_init();
    TEST_CHECK(r4 == LC_SUCCESS, "lc_init() after double shutdown == LC_SUCCESS");
    lc_shutdown();

    printf("\nTests passed: %d, failed: %d\n", g_tests_passed, g_tests_failed);
    if (g_tests_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
