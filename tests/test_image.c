/*
 * Headless-safe image tests (Phase 9).
 *
 * Only validates argument handling and lifecycle paths that never touch
 * Vulkan, so ctest passes on machines with no GPU. Descriptor content
 * validation runs against live devices in test_image_vulkan; real
 * allocation, upload, mipmaps, and round-trips live there too.
 */
#include <stdint.h>
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
    lc_image *sentinel = (lc_image *)0x1; /* never dereferenced */
    lc_image *out = sentinel;
    lc_device *fake_device = (lc_device *)0x1; /* validity checked first */
    lc_image_desc desc;
    lc_image_upload_desc upload;
    unsigned char byte = 0xAB;

    printf("Running LumaC image validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    desc.type = LC_IMAGE_TYPE_2D;
    desc.format = LC_FORMAT_RGBA8_UNORM;
    desc.width = 8;
    desc.height = 8;
    desc.depth = 1;
    desc.mip_levels = 1;
    desc.array_layers = 1;
    desc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_DST;
    desc.flags = LC_IMAGE_FLAG_NONE;
    desc.samples = LC_SAMPLE_COUNT_1;

    upload.mip_level = 0;
    upload.array_layer = 0;
    upload.width = 8;
    upload.height = 8;
    upload.depth = 1;
    upload.data = &byte;
    upload.data_size = 1;

    /* create before init must fail without touching Vulkan */
    out = sentinel;
    TEST_CHECK(lc_image_create(fake_device, &desc, &out) ==
                   LC_ERROR_NOT_INITIALIZED,
               "create before init -> NOT_INITIALIZED");
    TEST_CHECK(out == NULL, "out cleared to NULL on NOT_INITIALIZED");

    /* NULL-safe helpers before init */
    lc_image_destroy(NULL);
    TEST_CHECK(1, "lc_image_destroy(NULL) does not crash");
    TEST_CHECK(lc_image_get_format(NULL) == LC_FORMAT_UNDEFINED,
               "get_format(NULL) -> UNDEFINED");
    TEST_CHECK(lc_image_get_width(NULL) == 0, "get_width(NULL) == 0");
    TEST_CHECK(lc_image_get_height(NULL) == 0, "get_height(NULL) == 0");
    TEST_CHECK(lc_image_get_mip_levels(NULL) == 0, "get_mips(NULL) == 0");
    TEST_CHECK(lc_image_get_array_layers(NULL) == 0, "get_layers(NULL) == 0");
    TEST_CHECK(lc_image_write(NULL, &upload) == LC_ERROR_INVALID_ARGUMENT,
               "write(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_image_generate_mipmaps(NULL) == LC_ERROR_INVALID_ARGUMENT,
               "mipmaps(NULL) -> INVALID_ARGUMENT");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL device / desc / out */
    out = sentinel;
    TEST_CHECK(lc_image_create(NULL, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL device) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL device");
    out = sentinel;
    TEST_CHECK(lc_image_create(fake_device, NULL, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL desc");
    TEST_CHECK(lc_image_create(fake_device, &desc, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL out) -> INVALID_ARGUMENT");

    /* Non-live device is rejected without dereferencing garbage or
     * touching Vulkan (content validation needs a live device and
     * runs in the integration test). */
    out = sentinel;
    TEST_CHECK(lc_image_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(dead device) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on dead device");

    /* Upload and mipmap guards (dead handles, never Vulkan). */
    TEST_CHECK(lc_image_write(sentinel, &upload) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "write(dead image) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_image_write(sentinel, NULL) == LC_ERROR_INVALID_ARGUMENT,
               "write(dead image, NULL upload) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_image_generate_mipmaps(sentinel) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "mipmaps(dead image) -> INVALID_ARGUMENT");

    lc_image_destroy(NULL);
    TEST_CHECK(1, "lc_image_destroy(NULL) after init does not crash");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no images does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
