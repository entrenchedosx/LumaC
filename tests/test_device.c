/*
 * Headless-safe device tests (Phase 3).
 *
 * Only validates argument handling and lifecycle paths that never touch
 * Vulkan, so ctest passes on machines with no GPU. Real device creation
 * is covered by test_device_vulkan and examples/device_info.
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
    lc_device *sentinel = (lc_device *)0x1; /* never dereferenced */
    lc_device *out = sentinel;
    lc_device_desc desc;
    lc_result r;

    printf("Running LumaC device validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    /* create before init must fail without touching Vulkan */
    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 0;
    out = sentinel;
    r = lc_device_create(&desc, &out);
    TEST_CHECK(r == LC_ERROR_NOT_INITIALIZED, "create before init -> NOT_INITIALIZED");
    TEST_CHECK(out == NULL, "out cleared to NULL on NOT_INITIALIZED");

    /* NULL-safe helpers before init */
    lc_device_destroy(NULL);
    TEST_CHECK(1, "lc_device_destroy(NULL) does not crash");
    TEST_CHECK(lc_device_get_name(NULL) == NULL, "get_name(NULL) == NULL");
    TEST_CHECK(lc_device_get_backend(NULL) == 0, "get_backend(NULL) == 0");
    TEST_CHECK(lc_device_get_vendor_id(NULL) == 0, "get_vendor_id(NULL) == 0");
    TEST_CHECK(lc_device_get_device_id(NULL) == 0, "get_device_id(NULL) == 0");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL descriptor / output */
    out = sentinel;
    TEST_CHECK(lc_device_create(NULL, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL desc");
    /* NULL out must not crash and must not write through NULL */
    TEST_CHECK(lc_device_create(&desc, NULL) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL out) -> INVALID_ARGUMENT");

    /* Unknown backends are rejected before any Vulkan call */
    desc.backend = (lc_backend)0;
    out = sentinel;
    TEST_CHECK(lc_device_create(&desc, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create(backend 0) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on bad backend");
    desc.backend = (lc_backend)999;
    out = sentinel;
    TEST_CHECK(lc_device_create(&desc, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create(backend 999) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on backend 999");

    lc_device_destroy(NULL);
    TEST_CHECK(1, "lc_device_destroy(NULL) after init does not crash");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no devices does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
