/* Phase 20 asynchronous transfer integration test.
 *
 * Device-only: proves caller-data ownership, monotonic completion,
 * real GPU-only buffer copies, one thousand queued image uploads,
 * bounded staging pressure, oversized staging, async readback, and
 * synchronous-readback compatibility.  Validation stays enabled.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>
#include "graphics/graphics_internal.h"

static int g_passed;
static int g_failed;

#define CHECK(c, m) do {                                                \
    if (c) { printf("[PASS] %s\n", m); g_passed++; }                    \
    else { printf("[FAIL] %s\n", m); g_failed++; }                     \
} while (0)

static int make_device(lc_device **out) {
    lc_device_desc desc;
    lc_result result;

    memset(&desc, 0, sizeof(desc));
    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 1;
    desc.upload_staging_cap = 64u * 1024u;
    result = lc_device_create(&desc, out);
    if (result == LC_SUCCESS) return 0;
    if (result == LC_ERROR_BACKEND_UNAVAILABLE ||
        result == LC_ERROR_NO_SUPPORTED_DEVICE ||
        result == LC_ERROR_SURFACE_UNSUPPORTED) return 1;
    return -1;
}

static lc_image *make_image(lc_device *device, uint32_t w, uint32_t h) {
    lc_image_desc desc;
    lc_image *image = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.type = LC_IMAGE_TYPE_2D;
    desc.format = LC_FORMAT_RGBA8_UNORM;
    desc.width = w;
    desc.height = h;
    desc.depth = 1;
    desc.mip_levels = 1;
    desc.array_layers = 1;
    desc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_SRC |
                 LC_IMAGE_USAGE_TRANSFER_DST;
    desc.samples = LC_SAMPLE_COUNT_1;
    return lc_image_create(device, &desc, &image) == LC_SUCCESS ? image : NULL;
}

int main(void) {
    enum { W = 16, H = 16, UPLOADS = 1000 };
    lc_device *device = NULL;
    lc_buffer *gpu = NULL;
    lc_buffer *readback = NULL;
    lc_image *image = NULL;
    lc_image *large = NULL;
    lc_gpu_signal last = {0};
    lc_gpu_signal previous = {0};
    lc_buffer_desc bdesc;
    lc_image_upload_async_desc upload;
    lc_image_readback_desc rbdesc = {0};
    lc_readback_request *request = NULL;
    lc_transfer_stats stats;
    lc_memory_stats memory_stats;
    lc_queue_info transfer_queue;
    uint8_t pixels[W * H * 4];
    uint8_t expected[sizeof(pixels)];
    uint8_t output[sizeof(pixels)];
    uint8_t buffer_src[4096];
    uint8_t *buffer_out = NULL;
    uint8_t *large_data = NULL;
    uint8_t *large_out = NULL;
    size_t required = 0;
    uint32_t i;
    int rc;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (lc_init() != LC_SUCCESS) {
        printf("SKIP: lc_init failed\n");
        return 0;
    }
    rc = make_device(&device);
    if (rc != 0) {
        printf(rc > 0 ? "SKIP: no Vulkan device\n" :
                        "FAIL: device creation failed\n");
        lc_shutdown();
        return rc < 0;
    }

    memset(&bdesc, 0, sizeof(bdesc));
    bdesc.size = sizeof(buffer_src);
    bdesc.usage = LC_BUFFER_USAGE_TRANSFER_SRC | LC_BUFFER_USAGE_TRANSFER_DST;
    bdesc.memory = LC_MEMORY_GPU_ONLY;
    CHECK(lc_buffer_create(device, &bdesc, &gpu) == LC_SUCCESS,
          "GPU-only upload destination creates");
    bdesc.memory = LC_MEMORY_GPU_TO_CPU;
    CHECK(lc_buffer_create(device, &bdesc, &readback) == LC_SUCCESS,
          "buffer readback destination creates");
    for (i = 0; i < sizeof(buffer_src); i++) buffer_src[i] = (uint8_t)(i * 37u);
    CHECK(lc_upload_buffer_async(device, gpu, 0, buffer_src,
                                 sizeof(buffer_src), &last) == LC_SUCCESS &&
              last.value != 0,
          "GPU-only buffer upload schedules asynchronously");
    memset(buffer_src, 0, sizeof(buffer_src));
    CHECK(lc_gpu_signal_wait(device, last, LC_TIMEOUT_INFINITE) == LC_SUCCESS,
          "buffer upload completion waits");
    CHECK(lc_vulkan_copy_buffer(device, readback->vk_buffer, 0,
                                gpu->vk_buffer, 0,
                                sizeof(buffer_src)) == LC_SUCCESS,
          "uploaded buffer copies back on GPU");
    CHECK(lc_buffer_map(readback, (void **)&buffer_out) == LC_SUCCESS &&
              buffer_out != NULL,
          "buffer readback maps");
    if (buffer_out != NULL) {
        int ok = 1;
        for (i = 0; i < sizeof(buffer_src); i++) {
            if (buffer_out[i] != (uint8_t)(i * 37u)) { ok = 0; break; }
        }
        CHECK(ok, "async buffer upload bytes survive caller overwrite");
        lc_buffer_unmap(readback);
    }

    image = make_image(device, W, H);
    CHECK(image != NULL, "stress image creates");
    memset(&upload, 0, sizeof(upload));
    upload.width = W;
    upload.height = H;
    upload.depth = 1;
    upload.data = pixels;
    upload.data_size = sizeof(pixels);
    for (i = 0; i < UPLOADS; i++) {
        uint32_t j;
        lc_result upload_result;
        for (j = 0; j < sizeof(pixels); j++)
            pixels[j] = (uint8_t)(i * 13u + j * 17u);
        if (i == UPLOADS - 1) memcpy(expected, pixels, sizeof(expected));
        previous = last;
        upload_result = lc_upload_image_async(device, image, &upload, &last);
        if (upload_result != LC_SUCCESS) {
            printf("[info] image upload %u failed with %d\n", i,
                   (int)upload_result);
            break;
        }
        if (previous.value != 0 && last.value <= previous.value) break;
        memset(pixels, 0xA5, sizeof(pixels));
    }
    CHECK(i == UPLOADS, "1000 image uploads schedule without per-upload waits");
    CHECK(last.value != 0 &&
              lc_gpu_signal_wait(device, last, LC_TIMEOUT_INFINITE) == LC_SUCCESS,
          "last upload completion covers prior same-queue uploads");

    CHECK(lc_image_readback_async(device, image, &rbdesc, &request) == LC_SUCCESS &&
              request != NULL,
          "async image readback schedules");
    CHECK(request != NULL &&
              lc_readback_request_wait(request, LC_TIMEOUT_INFINITE) == LC_SUCCESS,
          "async readback completion waits");
    memset(output, 0, sizeof(output));
    CHECK(request != NULL &&
              lc_readback_request_map(request, output, sizeof(output),
                                      &required) == LC_SUCCESS &&
              required == sizeof(output),
          "async readback maps tight output");
    CHECK(memcmp(output, expected, sizeof(output)) == 0,
          "1000th upload pixels round-trip exactly");
    lc_readback_request_destroy(request);
    request = NULL;

    /* 8 MiB is intentionally larger than the 64 KiB staging cap.
     * The engine admits one oversized request after draining older work. */
    large = make_image(device, 2048, 1024);
    large_data = (uint8_t *)malloc(2048u * 1024u * 4u);
    large_out = (uint8_t *)malloc(2048u * 1024u * 4u);
    CHECK(large != NULL && large_data != NULL && large_out != NULL,
          "oversized-upload resources allocate");
    if (large && large_data && large_out) {
        size_t n = 2048u * 1024u * 4u;
        for (i = 0; i < n; i++) large_data[i] = (uint8_t)(i * 19u + 7u);
        upload.width = 2048;
        upload.height = 1024;
        upload.data = large_data;
        upload.data_size = n;
        CHECK(lc_upload_image_async(device, large, &upload, &last) == LC_SUCCESS &&
                  lc_gpu_signal_wait(device, last, LC_TIMEOUT_INFINITE) == LC_SUCCESS,
              "upload larger than staging cap completes");
        memset(large_data, 0, n);
        CHECK(lc_image_readback(large, &rbdesc, large_out, n, &required) ==
                  LC_SUCCESS && required == n,
              "sync readback remains compatible after async upload");
        CHECK(large_out[0] == 7u && large_out[1] == 26u &&
                  large_out[n - 1] == (uint8_t)((n - 1) * 19u + 7u),
              "oversized upload sample bytes verify");
    }

    lc_device_poll_completed(device);
    memset(&stats, 0, sizeof(stats));
    lc_device_get_transfer_stats(device, &stats);
    memset(&transfer_queue, 0, sizeof(transfer_queue));
    lc_device_get_queue_info(device, LC_QUEUE_TRANSFER, &transfer_queue);
    memset(&memory_stats, 0, sizeof(memory_stats));
    lc_device_get_memory_stats(device, &memory_stats);
    CHECK(stats.transfer_submissions >= UPLOADS + 3,
          "transfer submission diagnostics count real work");
    CHECK(stats.staging_high_water <= 2048u * 1024u * 4u,
          "staging high-water is bounded by one oversized request");
    CHECK(stats.uploads_in_flight == 0 && stats.staging_used == 0,
          "completed upload staging is reclaimed");
    printf("[info] uploads=%llu bytes=%llu submissions=%llu staging_high=%llu "
           "retired_high=%llu memory_blocks=%llu\n",
           (unsigned long long)(UPLOADS + 2),
           (unsigned long long)stats.bytes_uploaded,
           (unsigned long long)stats.transfer_submissions,
           (unsigned long long)stats.staging_high_water,
           (unsigned long long)stats.retired_high_water,
           (unsigned long long)memory_stats.block_count);
    printf("[info] transfer_queue available=%d dedicated=%d async=%d\n",
           transfer_queue.available, transfer_queue.dedicated,
           transfer_queue.async_supported);

    free(large_out);
    free(large_data);
    lc_image_destroy(large);
    lc_image_destroy(image);
    lc_buffer_destroy(readback);
    lc_buffer_destroy(gpu);
    lc_device_destroy(device);
    lc_shutdown();
    printf("phase20 transfer: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
