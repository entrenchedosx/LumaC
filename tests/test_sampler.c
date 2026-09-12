/*
 * Headless-safe sampler tests (Phase 9).
 *
 * Only validates argument handling that never touches Vulkan, so ctest
 * passes on machines with no GPU. Descriptor content validation runs
 * against live devices in test_image_vulkan; real sampler creation
 * lives there too.
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
    lc_sampler *sentinel = (lc_sampler *)0x1; /* never dereferenced */
    lc_sampler *out = sentinel;
    lc_device *fake_device = (lc_device *)0x1; /* validity checked first */
    lc_sampler_desc desc;

    printf("Running LumaC sampler validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    desc.min_filter = LC_FILTER_LINEAR;
    desc.mag_filter = LC_FILTER_LINEAR;
    desc.mipmap_mode = LC_MIPMAP_MODE_LINEAR;
    desc.address_u = LC_ADDRESS_REPEAT;
    desc.address_v = LC_ADDRESS_REPEAT;
    desc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
    desc.mip_lod_bias = 0.0f;
    desc.min_lod = 0.0f;
    desc.max_lod = 4.0f;
    desc.max_anisotropy = 1.0f;

    /* create before init must fail without touching Vulkan */
    out = sentinel;
    TEST_CHECK(lc_sampler_create(fake_device, &desc, &out) ==
                   LC_ERROR_NOT_INITIALIZED,
               "create before init -> NOT_INITIALIZED");
    TEST_CHECK(out == NULL, "out cleared to NULL on NOT_INITIALIZED");

    lc_sampler_destroy(NULL);
    TEST_CHECK(1, "lc_sampler_destroy(NULL) does not crash");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL device / desc / out */
    out = sentinel;
    TEST_CHECK(lc_sampler_create(NULL, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL device) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL device");
    out = sentinel;
    TEST_CHECK(lc_sampler_create(fake_device, NULL, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL desc");
    TEST_CHECK(lc_sampler_create(fake_device, &desc, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL out) -> INVALID_ARGUMENT");

    /* Non-live device is rejected without touching Vulkan. */
    out = sentinel;
    TEST_CHECK(lc_sampler_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(dead device) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on dead device");

    lc_sampler_destroy(NULL);
    TEST_CHECK(1, "lc_sampler_destroy(NULL) after init does not crash");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no samplers does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
