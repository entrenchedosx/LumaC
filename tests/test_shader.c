/*
 * Headless-safe shader tests (Phase 7).
 *
 * Only validates argument and SPIR-V header handling that never touches
 * Vulkan, so ctest passes on machines with no GPU. Real module creation
 * is covered by test_triangle_vulkan.
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

/* Minimal header-shaped blobs: magic determines validity here. */
static const uint32_t k_good_spv[] = { 0x07230203u, 0x00010000u };
static const uint32_t k_bad_magic[] = { 0xdeadbeefu, 0x00010000u };
static const unsigned char k_odd_size[6] = { 0x03, 0x02, 0x23, 0x07, 0, 0 };

int main(void) {
    lc_shader *sentinel = (lc_shader *)0x1; /* never dereferenced */
    lc_shader *out = sentinel;
    lc_device *fake_device = (lc_device *)0x1; /* validity checked first */
    lc_shader_desc desc;

    printf("Running LumaC shader validation tests...\n");

    /* Start from a known state */
    lc_shutdown();

    desc.stage = LC_SHADER_STAGE_VERTEX;
    desc.code = k_good_spv;
    desc.code_size = sizeof(k_good_spv);
    desc.entry_point = NULL;

    /* create before init must fail without touching Vulkan */
    out = sentinel;
    TEST_CHECK(lc_shader_create(fake_device, &desc, &out) ==
                   LC_ERROR_NOT_INITIALIZED,
               "create before init -> NOT_INITIALIZED");
    TEST_CHECK(out == NULL, "out cleared to NULL on NOT_INITIALIZED");

    /* NULL-safe helper before init */
    lc_shader_destroy(NULL);
    TEST_CHECK(1, "lc_shader_destroy(NULL) does not crash");

    TEST_CHECK(lc_init() == LC_SUCCESS, "lc_init() == LC_SUCCESS");

    /* NULL device / desc / code / out */
    out = sentinel;
    TEST_CHECK(lc_shader_create(NULL, &desc, &out) == LC_ERROR_INVALID_ARGUMENT,
               "create(NULL device) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL device");
    out = sentinel;
    TEST_CHECK(lc_shader_create(fake_device, NULL, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL desc) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL desc");
    TEST_CHECK(lc_shader_create(fake_device, &desc, NULL) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL out) -> INVALID_ARGUMENT");

    desc.code = NULL;
    out = sentinel;
    TEST_CHECK(lc_shader_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(NULL code) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on NULL code");
    desc.code = k_good_spv;

    desc.code_size = 0;
    out = sentinel;
    TEST_CHECK(lc_shader_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(size 0) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on size 0");
    desc.code_size = sizeof(k_good_spv);

    /* SPIR-V shape checks happen before any Vulkan call. */
    desc.code = k_odd_size;
    desc.code_size = sizeof(k_odd_size);
    out = sentinel;
    TEST_CHECK(lc_shader_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(misaligned size) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on misaligned size");
    desc.code = k_good_spv;
    desc.code_size = sizeof(k_good_spv);

    desc.code = k_bad_magic;
    out = sentinel;
    TEST_CHECK(lc_shader_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(bad magic) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on bad magic");
    desc.code = k_good_spv;

    desc.stage = (lc_shader_stage)99;
    out = sentinel;
    TEST_CHECK(lc_shader_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(bad stage) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on bad stage");
    desc.stage = LC_SHADER_STAGE_VERTEX;

    /* Overlong entry point is rejected without Vulkan. */
    {
        char long_entry[80];
        size_t i;
        for (i = 0; i < sizeof(long_entry) - 1; i++) {
            long_entry[i] = 'm';
        }
        long_entry[sizeof(long_entry) - 1] = '\0';
        desc.entry_point = long_entry;
        out = sentinel;
        TEST_CHECK(lc_shader_create(fake_device, &desc, &out) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "create(long entry) -> INVALID_ARGUMENT");
        TEST_CHECK(out == NULL, "out cleared to NULL on long entry");
        desc.entry_point = NULL;
    }

    /* Non-live device is rejected without dereferencing garbage. */
    out = sentinel;
    TEST_CHECK(lc_shader_create(fake_device, &desc, &out) ==
                   LC_ERROR_INVALID_ARGUMENT,
               "create(dead device) -> INVALID_ARGUMENT");
    TEST_CHECK(out == NULL, "out cleared to NULL on dead device");

    lc_shader_destroy(NULL);
    TEST_CHECK(1, "lc_shader_destroy(NULL) after init does not crash");

    lc_shutdown();
    TEST_CHECK(1, "lc_shutdown() with no shaders does not crash");

    printf("\nTests passed: %d, failed: %d\n", g_passed, g_failed);
    if (g_failed > 0) {
        printf("TESTS FAILED\n");
        return 1;
    }
    printf("ALL TESTS PASSED\n");
    return 0;
}
