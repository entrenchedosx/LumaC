/*
 * Headless-safe image-view tests (Phase 10).
 *
 * Only validates argument handling that never touches Vulkan, so ctest
 * passes on machines with no GPU. Descriptor content validation runs
 * against live images in test_textured_vulkan; real view creation
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
    lc_image_view *sentinel = (lc_image_view *)0x1; /* never dereferenced */
    lc_image_view *out = sentinel;
    lc_image *fake_image = (lc_image *)0x1; /* validity checked first */
    lc_image_view_desc desc;

    printf("Running LumaC image-view validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    desc.type = LC_IMAGE_VIEW_2D;
    desc.format = LC_FORMAT_UNDEFINED;
    desc.aspect = LC_IMAGE_ASPECT_COLOR;
    desc.base_mip_level = 0;
    desc.mip_level_count = 1;
    desc.base_array_layer = 0;
    desc.array_layer_count = 1;

    /* create before init must fail without touching Vulkan */
    out = sentinel;
    TEST_CHECK(lc_image_view_create(fake_image, &desc, &out) ==
                   LC_ERROR_NOT_INITIALIZED,
               "create before init -> NOT_INITIALIZED");
    TEST_CHECK(out == NULL, "out cleared to NULL on NOT_INITIALIZED");

    /* NULL-safe helper before init */
    lc_image_view_destroy(NULL);
    TEST_CHECK(1, "lc_image_view_destroy(NULL) does not crash");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL image / desc / out */
    out = sentinel;
    TEST_CHECK(lc_image_view_create(NULL, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL image) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL image");
    out = sentinel;
    TEST_CHECK(lc_image_view_create(fake_image, NULL, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL desc");
    TEST_CHECK(lc_image_view_create(fake_image, &desc, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL out) -> INVALID_ARGUMENT");

    /* Non-live image is rejected without dereferencing garbage. */
    out = sentinel;
    TEST_CHECK(lc_image_view_create(fake_image, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(dead image) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on dead image");

    lc_image_view_destroy(NULL);
    TEST_CHECK(1, "lc_image_view_destroy(NULL) after init does not crash");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no views does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
