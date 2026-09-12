/*
 * Headless-safe buffer tests (Phase 8).
 *
 * Only validates argument handling and lifecycle paths that never touch
 * Vulkan, so ctest passes on machines with no GPU. Real allocation,
 * mapping, staging uploads, and round-trips are covered by
 * test_buffer_vulkan.
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
    lc_buffer *sentinel = (lc_buffer *)0x1; /* never dereferenced */
    lc_buffer *out = sentinel;
    lc_device *fake_device = (lc_device *)0x1; /* validity checked first */
    lc_swapchain *fake_swapchain = (lc_swapchain *)0x1;
    lc_buffer_desc desc;
    unsigned char byte = 0xAB;

    printf("Running LumaC buffer validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    desc.size = 1024;
    desc.usage = LC_BUFFER_USAGE_VERTEX;
    desc.memory = LC_MEMORY_GPU_ONLY;

    /* create before init must fail without touching Vulkan */
    out = sentinel;
    TEST_CHECK(lc_buffer_create(fake_device, &desc, &out) ==
                   LC_ERROR_NOT_INITIALIZED,
               "create before init -> NOT_INITIALIZED");
    TEST_CHECK(out == NULL, "out cleared to NULL on NOT_INITIALIZED");

    /* NULL-safe helpers before init */
    lc_buffer_destroy(NULL);
    TEST_CHECK(1, "lc_buffer_destroy(NULL) does not crash");
    TEST_CHECK(lc_buffer_get_size(NULL) == 0, "get_size(NULL) == 0");
    TEST_CHECK(lc_buffer_map(NULL, (void **)&out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "map(NULL) -> INVALID_ARGUMENT");
    lc_buffer_unmap(NULL);
    TEST_CHECK(1, "lc_buffer_unmap(NULL) does not crash");
    TEST_CHECK(lc_buffer_write(NULL, 0, &byte, 1) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "write(NULL) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_bind_vertex_buffer(NULL, 0, out, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind_vertex_buffer(NULL swapchain) -> INVALID_ARGUMENT");
    {
        lc_device_limits zeroed;

        zeroed.max_texture_2d_dimension = 1234;
        lc_device_get_limits(NULL, &zeroed);
        TEST_CHECK(zeroed.max_texture_2d_dimension == 0,
                   "limits(NULL device) zeroed");
    }

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL device / desc / out */
    out = sentinel;
    TEST_CHECK(lc_buffer_create(NULL, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL device) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL device");
    out = sentinel;
    TEST_CHECK(lc_buffer_create(fake_device, NULL, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL desc");
    TEST_CHECK(lc_buffer_create(fake_device, &desc, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL out) -> INVALID_ARGUMENT");

    /* size == 0, empty usage, unknown usage/memory rejected. */
    desc.size = 0;
    out = sentinel;
    TEST_CHECK(lc_buffer_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(size 0) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on size 0");
    desc.size = 1024;
    desc.usage = 0;
    out = sentinel;
    TEST_CHECK(lc_buffer_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(empty usage) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on empty usage");
    desc.usage = 0x40000000u;
    out = sentinel;
    TEST_CHECK(lc_buffer_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(unknown usage) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on unknown usage");
    desc.usage = LC_BUFFER_USAGE_VERTEX;
    desc.memory = (lc_memory_usage)99;
    out = sentinel;
    TEST_CHECK(lc_buffer_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(unknown memory) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on unknown memory");
    desc.memory = LC_MEMORY_GPU_ONLY;

    /* Non-live device is rejected without dereferencing garbage. */
    out = sentinel;
    TEST_CHECK(lc_buffer_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(dead device) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on dead device");

    /* Mapping and write guards (dead handles, never Vulkan). */
    {
        void *ptr = (void *)0x1;
        TEST_CHECK(lc_buffer_map(sentinel, &ptr) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "map(dead buffer) -> INVALID_ARGUMENT");
        TEST_CHECK(ptr == NULL, "map out cleared on dead buffer");
        TEST_CHECK(lc_buffer_map(sentinel, NULL) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "map(dead buffer, NULL out) -> INVALID_ARGUMENT");
    }
    lc_buffer_unmap(sentinel);
    TEST_CHECK(1, "lc_buffer_unmap(dead buffer) does not crash");
    TEST_CHECK(lc_buffer_write(sentinel, 0, &byte, 1) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "write(dead buffer) -> INVALID_ARGUMENT");

    /* bind_vertex_buffer guards. */
    TEST_CHECK(lc_bind_vertex_buffer(fake_swapchain, 0, sentinel, 0) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "bind(dead swapchain) -> INVALID_ARGUMENT");
    TEST_CHECK(lc_swapchain_get_format(NULL) == LC_FORMAT_UNDEFINED,
               "swapchain format(NULL) -> UNDEFINED");

    lc_buffer_destroy(NULL);
    TEST_CHECK(1, "lc_buffer_destroy(NULL) after init does not crash");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no buffers does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
