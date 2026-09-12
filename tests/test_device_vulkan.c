/*
 * Vulkan integration test (Phase 3).
 *
 * Creates real Vulkan devices: instance, GPU enumeration, selection,
 * logical device, queue retrieval, destruction, re-creation, and
 * shutdown-with-live-device cleanup. Requires a Vulkan runtime + GPU.
 *
 * If the environment has no usable Vulkan setup (missing runtime or no
 * supported GPU), the test reports SKIP and exits 0, since there is no
 * LumaC code left to verify. Any other failure is a hard FAIL.
 */
#include <stdio.h>
#include <string.h>
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
    lc_device *a = NULL;
    lc_device *b = NULL;
    lc_device *leaked = NULL;
    lc_device_desc desc;
    lc_result r;

    printf("Running LumaC Vulkan integration test...\n");

    lc_shutdown();
    if (lc_init() != LC_SUCCESS) {
        printf("lc_init failed: FAIL\n");
        return 1;
    }

    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 1; /* exercises messenger when layers exist */

    r = lc_device_create(&desc, &a);
    if (r == LC_ERROR_BACKEND_UNAVAILABLE || r == LC_ERROR_NO_SUPPORTED_DEVICE) {
        printf("SKIP: no usable Vulkan runtime/GPU in this environment (lc=%d)\n", r);
        lc_shutdown();
        return 0;
    }
    TEST_CHECK(r == LC_SUCCESS, "lc_device_create(validation on) == LC_SUCCESS");
    if (r != LC_SUCCESS || a == NULL) {
        printf("TESTS FAILED\n");
        lc_shutdown();
        return 1;
    }

    /* Backend-neutral GPU information */
    TEST_CHECK(lc_device_get_backend(a) == LC_BACKEND_VULKAN,
               "backend == LC_BACKEND_VULKAN");
    TEST_CHECK(lc_device_get_name(a) != NULL, "name != NULL");
    TEST_CHECK(lc_device_get_name(a) != NULL && strlen(lc_device_get_name(a)) > 0,
               "name is non-empty");
    TEST_CHECK(lc_device_get_vendor_id(a) != 0, "vendor id != 0");
    printf("GPU: %s (vendor 0x%04x, device 0x%04x)\n",
           lc_device_get_name(a),
           lc_device_get_vendor_id(a),
           lc_device_get_device_id(a));

    /* Second concurrent device: no single-global-device assumption */
    desc.enable_validation = 0;
    TEST_CHECK(lc_device_create(&desc, &b) == LC_SUCCESS,
               "second concurrent device creation succeeds");
    TEST_CHECK(b != NULL && b != a, "devices are distinct handles");
    lc_device_destroy(b);
    b = NULL;
    TEST_CHECK(lc_device_get_name(a) != NULL, "first device survives second destroy");

    /* Destroy + re-create */
    lc_device_destroy(a);
    a = NULL;
    TEST_CHECK(lc_device_create(&desc, &a) == LC_SUCCESS,
               "re-create after destroy succeeds");
    lc_device_destroy(a);
    a = NULL;

    /* Shutdown with a live device must clean up safely */
    TEST_CHECK(lc_device_create(&desc, &leaked) == LC_SUCCESS,
               "create device for shutdown-cleanup check");
    leaked = NULL; /* drop the handle; shutdown must destroy it */
    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with live device does not crash");
    TEST_CHECK(lc_init() == LC_SUCCESS, "re-init after device cleanup succeeds");
    {
        lc_device *w = NULL;
        TEST_CHECK(lc_device_create(&desc, &w) == LC_SUCCESS,
                   "create after shutdown-cleanup succeeds");
        lc_device_destroy(w);
    }
    lc_shutdown();

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
