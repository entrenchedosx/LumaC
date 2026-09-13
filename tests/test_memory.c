/*
 * Vulkan GPU memory allocator stress test (Phase 19, PARTs
 * AE–AK, AA–AD, 11, 13–16, 18).
 *
 * Device-only (no window/surface/swapchain): thousands of buffers
 * and images through the block suballocator, fragmentation
 * patterns, dedicated policy, reclamation, alignment, mapping
 * isolation, OOM rollback, CPU benchmarks, and ID uniqueness
 * across offset reuse.
 *
 * SKIP without a Vulkan device; hard FAIL otherwise.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
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

#define SKIP_ENV(what) do { \
    printf("SKIP: environment cannot provide %s\n", what); \
    lc_shutdown(); \
    return 0; \
} while (0)

/* Deterministic LCG (fixed seed): reproducible "random" orders. */
static uint64_t g_rng = 0x123456789ABCDEFu;

static uint64_t next_rand(void) {
    g_rng = g_rng * 6364136223846793005u + 1442695040888963407u;
    g_rng ^= g_rng >> 29;
    return g_rng;
}

static int make_device(lc_device **out) {
    lc_device_desc desc = { 0 };

    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 1;
    *out = NULL;
    switch (lc_device_create(&desc, out)) {
    case LC_SUCCESS:
        return 0;
    case LC_ERROR_BACKEND_UNAVAILABLE:
    case LC_ERROR_NO_SUPPORTED_DEVICE:
    case LC_ERROR_SURFACE_UNSUPPORTED:
        return 1;
    default:
        return -1;
    }
}

static lc_buffer *make_buffer(lc_device *device, uint64_t size,
                              uint32_t usage, lc_memory_usage mem) {
    lc_buffer_desc desc;
    lc_buffer *buf = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.size = size;
    desc.usage = usage;
    desc.memory = mem;
    if (lc_buffer_create(device, &desc, &buf) != LC_SUCCESS) {
        return NULL;
    }
    return buf;
}

static lc_image *make_image(lc_device *device, lc_format format,
                            uint32_t w, uint32_t h) {
    lc_image_desc desc;
    lc_image *image = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.type = LC_IMAGE_TYPE_2D;
    desc.format = format;
    desc.width = w;
    desc.height = h;
    desc.depth = 1;
    desc.mip_levels = 1;
    desc.array_layers = 1;
    desc.usage = LC_IMAGE_USAGE_SAMPLED |
                 LC_IMAGE_USAGE_TRANSFER_SRC |
                 LC_IMAGE_USAGE_TRANSFER_DST;
    desc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &desc, &image) != LC_SUCCESS) {
        return NULL;
    }
    return image;
}

static double clock_ms(uint64_t t0, uint64_t t1) {
    uint64_t freq = lc_clock_frequency();

    if (freq == 0) {
        return 0.0;
    }
    return ((double)(t1 - t0) * 1000.0) / (double)freq;
}

int main(void) {
    lc_device *device = NULL;
    int rc;
    int exit_code = 1;
    lc_memory_stats base;
    lc_memory_stats cur;

    memset(&base, 0, sizeof(base));
    memset(&cur, 0, sizeof(cur));
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (lc_init() != LC_SUCCESS) {
        printf("SKIP: lc_init failed\n");
        return 0;
    }
    rc = make_device(&device);
    if (rc != 0) {
        if (rc > 0) {
            SKIP_ENV("a Vulkan device");
        }
        printf("FAIL: lc_device_create\n");
        lc_shutdown();
        return 1;
    }
    lc_device_get_memory_stats(device, &base);

    /* ---- PART AE: 10,000 small buffers ----
     * Suballocation: a handful of blocks must back all of them.
     * Destroyed in deterministic-shuffled order. */
    {
        static const uint32_t N = 10000;
        lc_buffer **bufs = NULL;
        uint32_t i;
        int ok = 1;

        bufs = (lc_buffer **)calloc(N, sizeof(lc_buffer *));
        TEST_CHECK(bufs != NULL, "many-buffers: scratch allocates");
        if (bufs != NULL) {
            uint64_t t0 = lc_clock_now();

            for (i = 0; i < N; i++) {
                bufs[i] = make_buffer(device, 256,
                                      LC_BUFFER_USAGE_UNIFORM,
                                      LC_MEMORY_GPU_ONLY);
                if (bufs[i] == NULL) {
                    ok = 0;
                    break;
                }
            }
            printf("[info] 10k buffer create: %.1f ms\n",
                   clock_ms(t0, lc_clock_now()));
            TEST_CHECK(ok, "many-buffers: 10000 create");
            lc_device_get_memory_stats(device, &cur);
            printf("[info] blocks=%llu allocs=%llu dedicated=%llu "
                   "used=%llu\n",
                   (unsigned long long)cur.block_count,
                   (unsigned long long)cur.allocation_count,
                   (unsigned long long)cur.dedicated_allocation_count,
                   (unsigned long long)(cur.device_local_used +
                                        cur.host_visible_used));
            TEST_CHECK(cur.block_count <= 6,
                       "many-buffers: blocks << resources");
            TEST_CHECK(cur.dedicated_allocation_count == 0,
                       "many-buffers: nothing dedicated");
            /* Deterministic shuffle (Fisher-Yates, fixed seed). */
            for (i = N - 1; i > 0; i--) {
                uint32_t r = (uint32_t)(next_rand() % (uint64_t)(i + 1));
                lc_buffer *t = bufs[i];

                bufs[i] = bufs[r];
                bufs[r] = t;
            }
            t0 = lc_clock_now();
            for (i = 0; i < N; i++) {
                lc_buffer_destroy(bufs[i]);
            }
            printf("[info] 10k buffer destroy: %.1f ms\n",
                   clock_ms(t0, lc_clock_now()));
            free(bufs);
            lc_device_get_memory_stats(device, &cur);
            TEST_CHECK(cur.allocation_count == base.allocation_count &&
                           cur.device_local_used == base.device_local_used,
                       "many-buffers: all memory returns");
        }
    }

    /* ---- PART AF: hundreds of small images ---- */
    {
        static const uint32_t N = 600;
        lc_image **imgs = NULL;
        uint32_t i;
        int ok = 1;

        imgs = (lc_image **)calloc(N, sizeof(lc_image *));
        TEST_CHECK(imgs != NULL, "many-images: scratch allocates");
        if (imgs != NULL) {
            for (i = 0; i < N; i++) {
                imgs[i] = make_image(device, LC_FORMAT_RGBA8_UNORM,
                                     64, 64);
                if (imgs[i] == NULL) {
                    ok = 0;
                    break;
                }
            }
            TEST_CHECK(ok, "many-images: 600 create");
            lc_device_get_memory_stats(device, &cur);
            printf("[info] images: blocks=%llu allocs=%llu "
                   "used=%llu\n",
                   (unsigned long long)cur.block_count,
                   (unsigned long long)cur.allocation_count,
                   (unsigned long long)cur.device_local_used);
            TEST_CHECK(cur.block_count <= 4,
                       "many-images: blocks << resources");
            for (i = 0; i < N; i++) {
                lc_image_destroy(imgs[i]);
            }
            free(imgs);
            lc_device_get_memory_stats(device, &cur);
            TEST_CHECK(cur.device_local_used == base.device_local_used,
                       "many-images: all memory returns");
        }
    }

    /* ---- PART AG: fragmentation (alternating free + regrow) ---- */
    {
        static const uint32_t N = 200;
        lc_buffer **bufs = NULL;
        uint32_t i;
        int ok = 1;

        bufs = (lc_buffer **)calloc(N, sizeof(lc_buffer *));
        if (bufs == NULL) {
            TEST_CHECK(0, "frag: scratch allocates");
        } else {
            for (i = 0; i < N; i++) {
                bufs[i] =
                    make_buffer(device, 1024 + (uint64_t)(i * 32),
                                LC_BUFFER_USAGE_STORAGE,
                                LC_MEMORY_GPU_ONLY);
                if (bufs[i] == NULL) {
                    ok = 0;
                    break;
                }
            }
            TEST_CHECK(ok, "frag: varied sizes create");
            /* Free every other one (checkerboard holes). */
            for (i = 0; i < N; i += 2) {
                lc_buffer_destroy(bufs[i]);
                bufs[i] = NULL;
            }
            /* Differently sized replacements must reuse the holes. */
            for (i = 0; i < N; i += 2) {
                bufs[i] =
                    make_buffer(device, 512 + (uint64_t)(i * 16),
                                LC_BUFFER_USAGE_STORAGE,
                                LC_MEMORY_GPU_ONLY);
                if (bufs[i] == NULL) {
                    ok = 0;
                    break;
                }
            }
            TEST_CHECK(ok, "frag: replacements reuse holes");
            lc_device_get_memory_stats(device, &cur);
            printf("[info] frag: blocks=%llu largest_free=%llu\n",
                   (unsigned long long)cur.block_count,
                   (unsigned long long)cur.largest_free_range);
            for (i = 0; i < N; i++) {
                lc_buffer_destroy(bufs[i]);
            }
            free(bufs);
            lc_device_get_memory_stats(device, &cur);
            TEST_CHECK(cur.device_local_used == base.device_local_used,
                       "frag: ends clean");
        }
    }

    /* ---- PART AH: large resource goes dedicated ---- */
    {
        /* 2048x2048 RGBA32F = 64 MiB > half the 32 MiB default. */
        lc_image *big = make_image(device, LC_FORMAT_RGBA32_FLOAT,
                                   2048, 2048);
        lc_resource_memory_info info;

        memset(&info, 0, sizeof(info));
        TEST_CHECK(big != NULL, "dedicated: large image creates");
        if (big != NULL) {
            lc_image_get_memory_info(big, &info);
            TEST_CHECK(info.dedicated &&
                           info.memory_class ==
                               LC_MEMORY_CLASS_DEVICE_LOCAL &&
                           info.allocation_size >= 64u * 1024u * 1024u,
                       "dedicated: policy reports dedicated");
            printf("[info] dedicated: requested=%llu allocated=%llu\n",
                   (unsigned long long)info.requested_size,
                   (unsigned long long)info.allocation_size);
            lc_image_destroy(big);
            lc_device_get_memory_stats(device, &cur);
            TEST_CHECK(cur.dedicated_allocation_count ==
                           base.dedicated_allocation_count,
                       "dedicated: count returns to baseline");
        }
    }

    /* ---- PART AI: block reclamation ----
     * Grow to several blocks, free everything, expect no retained
     * growth (warm reserves for touched classes may remain). */
    {
        static const uint32_t N = 40;
        lc_image **imgs = NULL;
        uint32_t i;
        lc_memory_stats grown;

        memset(&grown, 0, sizeof(grown));
        imgs = (lc_image **)calloc(N, sizeof(lc_image *));
        if (imgs == NULL) {
            TEST_CHECK(0, "reclaim: scratch allocates");
        } else {
            /* 1024x1024 RGBA8 = 4 MiB x 40 = 160 MiB: forces
             * several 32 MiB blocks. */
            for (i = 0; i < N; i++) {
                imgs[i] = make_image(device, LC_FORMAT_RGBA8_UNORM,
                                     1024, 1024);
                if (imgs[i] == NULL) {
                    break;
                }
            }
            TEST_CHECK(i == N, "reclaim: grows to many blocks");
            lc_device_get_memory_stats(device, &grown);
            printf("[info] reclaim: grown blocks=%llu\n",
                   (unsigned long long)grown.block_count);
            TEST_CHECK(grown.block_count >= 3,
                       "reclaim: multiple blocks live");
            for (i = 0; i < N; i++) {
                if (imgs[i] != NULL) {
                    lc_image_destroy(imgs[i]);
                }
            }
            free(imgs);
            lc_device_get_memory_stats(device, &cur);
            printf("[info] reclaim: kept blocks=%llu\n",
                   (unsigned long long)cur.block_count);
            TEST_CHECK(cur.block_count < grown.block_count &&
                           cur.block_count <= 4,
                       "reclaim: excess empty blocks released");
        }
    }

    /* ---- PART AJ/AK: alignment + mapping isolation ----
     * Odd sizes across placements; patterns must not leak into
     * neighbors (proves no overlaps + correct offsets). */
    {
        static const uint64_t sizes[8] = { 1, 3, 17, 100, 1000, 4095,
                                           4096, 65537 };
        lc_buffer *bufs[8] = { NULL, NULL, NULL, NULL,
                               NULL, NULL, NULL, NULL };
        int i;
        int ok = 1;

        for (i = 0; i < 8; i++) {
            bufs[i] = make_buffer(device, sizes[i],
                                  LC_BUFFER_USAGE_STORAGE,
                                  (i % 2) ? LC_MEMORY_CPU_TO_GPU
                                          : LC_MEMORY_GPU_TO_CPU);
            if (bufs[i] == NULL) {
                ok = 0;
                break;
            }
        }
        TEST_CHECK(ok, "align: odd sizes create both placements");
        if (ok) {
            for (i = 0; i < 8; i++) {
                unsigned char *pat = NULL;
                unsigned char *check = NULL;
                uint64_t k;

                pat = (unsigned char *)malloc((size_t)sizes[i]);
                if (pat == NULL) {
                    ok = 0;
                    break;
                }
                for (k = 0; k < sizes[i]; k++) {
                    pat[k] = (unsigned char)((i * 37 + k * 11) & 0xFF);
                }
                if (lc_buffer_write(bufs[i], 0, pat,
                                    sizes[i]) != LC_SUCCESS) {
                    free(pat);
                    ok = 0;
                    break;
                }
                if (lc_buffer_map(bufs[i], (void **)&check) !=
                        LC_SUCCESS ||
                    check == NULL ||
                    memcmp(check, pat, (size_t)sizes[i]) != 0) {
                    free(pat);
                    ok = 0;
                    break;
                }
                lc_buffer_unmap(bufs[i]);
                free(pat);
            }
            TEST_CHECK(ok, "align: patterns round-trip isolated");
            /* Neighbor check: rewrite buffer 0, verify buffer 1
             * still holds its pattern (no overlap). */
            if (ok) {
                unsigned char *z = NULL;
                unsigned char *check = NULL;

                z = (unsigned char *)calloc(1, (size_t)sizes[0] > 0
                                                   ? (size_t)sizes[0]
                                                   : 1);
                if (z != NULL) {
                    unsigned char expect =
                        (unsigned char)((1 * 37) & 0xFF);

                    lc_buffer_write(bufs[0], 0, z, sizes[0]);
                    free(z);
                    if (lc_buffer_map(bufs[1], (void **)&check) ==
                            LC_SUCCESS &&
                        check != NULL && check[0] == expect) {
                        TEST_CHECK(1, "align: neighbors untouched");
                    } else {
                        TEST_CHECK(0, "align: neighbors untouched");
                    }
                    lc_buffer_unmap(bufs[1]);
                }
            }
        }
        for (i = 0; i < 8; i++) {
            lc_buffer_destroy(bufs[i]);
        }
    }

    /* ---- PART 11: IDs unique across offset reuse ----
     * 2000 buffer cycles; allocator certainly reuses offsets —
     * IDs must never repeat. */
    {
        static const uint32_t N = 2000;
        lc_resource_id *ids = NULL;
        uint32_t i;
        uint32_t j;
        int unique = 1;

        ids = (lc_resource_id *)malloc(sizeof(lc_resource_id) * N);
        TEST_CHECK(ids != NULL, "ids: scratch allocates");
        if (ids != NULL) {
            for (i = 0; i < N; i++) {
                lc_buffer *b = make_buffer(device, 512,
                                           LC_BUFFER_USAGE_UNIFORM,
                                           LC_MEMORY_GPU_ONLY);

                if (b == NULL) {
                    unique = 0;
                    break;
                }
                ids[i] = lc_buffer_get_resource_id(b);
                if (ids[i] == 0) {
                    unique = 0;
                }
                lc_buffer_destroy(b);
            }
            for (i = 0; i < N && unique; i++) {
                for (j = i + 1; j < N; j++) {
                    if (ids[i] == ids[j]) {
                        unique = 0;
                        break;
                    }
                }
            }
            TEST_CHECK(unique, "ids: 2000 buffer IDs unique");
            free(ids);
        }
    }

    /* ---- PARTs 14/15: OOM + rollback ----
     * 1 TiB can never allocate; stats must not move and no leak
     * may remain. Bad args fail before touching the allocator. */
    {
        lc_memory_stats before;
        lc_memory_stats after;
        lc_buffer *huge = NULL;
        lc_buffer_desc bad;

        lc_device_get_memory_stats(device, &before);
        memset(&bad, 0, sizeof(bad));
        bad.size = (uint64_t)1024 * 1024 * 1024 * 1024;
        bad.usage = LC_BUFFER_USAGE_STORAGE;
        bad.memory = LC_MEMORY_GPU_ONLY;
        TEST_CHECK(lc_buffer_create(device, &bad, &huge) ==
                           LC_ERROR_OUT_OF_MEMORY &&
                       huge == NULL,
                   "oom: 1 TiB buffer fails OUT_OF_MEMORY");
        {
            lc_image_desc idesc;
            lc_image *huge_img = NULL;

            memset(&idesc, 0, sizeof(idesc));
            idesc.type = LC_IMAGE_TYPE_2D;
            idesc.format = LC_FORMAT_RGBA8_UNORM;
            idesc.width = 1048576;
            idesc.height = 1048576;
            idesc.depth = 1;
            idesc.mip_levels = 1;
            idesc.array_layers = 1;
            idesc.usage = LC_IMAGE_USAGE_SAMPLED;
            idesc.samples = LC_SAMPLE_COUNT_1;
            TEST_CHECK(lc_image_create(device, &idesc, &huge_img) ==
                               LC_ERROR_OUT_OF_MEMORY &&
                           huge_img == NULL,
                       "oom: 1 terapixel image fails cleanly");
        }
        lc_device_get_memory_stats(device, &after);
        TEST_CHECK(memcmp(&before, &after, sizeof(before)) == 0,
                   "oom: stats unchanged, nothing leaked");
    }

    /* ---- PART 18: AAA-scale simulated workload ----
     * Thousands of buffers + hundreds of textures + several
     * render targets; report sharing. Must show resources >>
     * backing VkDeviceMemory allocations. */
    {
        static const uint32_t NB = 4000;
        static const uint32_t NI = 300;
        lc_buffer **bufs = NULL;
        lc_image **imgs = NULL;
        lc_render_target **tgts = NULL;
        lc_image **timgs = NULL;
        lc_image_view **tviews = NULL;
        uint32_t i;
        int ok = 1;

        bufs = (lc_buffer **)calloc(NB, sizeof(lc_buffer *));
        imgs = (lc_image **)calloc(NI, sizeof(lc_image *));
        tgts = (lc_render_target **)calloc(4, sizeof(lc_render_target *));
        timgs = (lc_image **)calloc(4, sizeof(lc_image *));
        tviews = (lc_image_view **)calloc(4, sizeof(lc_image_view *));
        TEST_CHECK(bufs != NULL && imgs != NULL && tgts != NULL &&
                       timgs != NULL && tviews != NULL,
                   "scale: scratch allocates");
        if (bufs != NULL && imgs != NULL && tgts != NULL &&
            timgs != NULL && tviews != NULL) {
            for (i = 0; i < NB && ok; i++) {
                bufs[i] = make_buffer(
                    device, 512 + (uint64_t)(i % 16) * 256,
                    LC_BUFFER_USAGE_VERTEX, LC_MEMORY_GPU_ONLY);
                if (bufs[i] == NULL) {
                    ok = 0;
                }
            }
            for (i = 0; i < NI && ok; i++) {
                imgs[i] = make_image(device, LC_FORMAT_RGBA8_UNORM,
                                     128 + (i % 4) * 64,
                                     128 + (i % 4) * 64);
                if (imgs[i] == NULL) {
                    ok = 0;
                }
            }
            for (i = 0; i < 4 && ok; i++) {
                lc_image_desc idesc;
                lc_image_view_desc vd;
                lc_render_target_create_desc td;
                lc_render_target_attachment att;

                memset(&idesc, 0, sizeof(idesc));
                idesc.type = LC_IMAGE_TYPE_2D;
                idesc.format = LC_FORMAT_RGBA8_UNORM;
                idesc.width = 512;
                idesc.height = 512;
                idesc.depth = 1;
                idesc.mip_levels = 1;
                idesc.array_layers = 1;
                idesc.usage = LC_IMAGE_USAGE_SAMPLED |
                              LC_IMAGE_USAGE_COLOR_ATTACHMENT |
                              LC_IMAGE_USAGE_TRANSFER_SRC |
                              LC_IMAGE_USAGE_TRANSFER_DST;
                idesc.samples = LC_SAMPLE_COUNT_1;
                if (lc_image_create(device, &idesc, &timgs[i]) !=
                    LC_SUCCESS) {
                    ok = 0;
                    break;
                }
                memset(&vd, 0, sizeof(vd));
                vd.type = LC_IMAGE_VIEW_2D;
                vd.aspect = LC_IMAGE_ASPECT_COLOR;
                vd.mip_level_count = 1;
                vd.array_layer_count = 1;
                if (lc_image_view_create(timgs[i], &vd, &tviews[i]) !=
                    LC_SUCCESS) {
                    ok = 0;
                    break;
                }
                memset(&td, 0, sizeof(td));
                td.width = 512;
                td.height = 512;
                att.view = tviews[i];
                td.color_attachments = &att;
                td.color_attachment_count = 1;
                td.depth_stencil_attachment = NULL;
                if (lc_render_target_create(device, &td, &tgts[i]) !=
                    LC_SUCCESS) {
                    ok = 0;
                    break;
                }
            }
            TEST_CHECK(ok, "scale: 4000 buffers + 300 textures + "
                           "4 targets create");
            lc_device_get_memory_stats(device, &cur);
            printf("[info] scale: resources=%u blocks=%llu "
                   "dedicated=%llu committed=%llu used=%llu "
                   "largest_free=%llu\n",
                   NB + NI + 12,
                   (unsigned long long)cur.block_count,
                   (unsigned long long)cur.dedicated_allocation_count,
                   (unsigned long long)(cur.device_local_allocated +
                                        cur.host_visible_allocated),
                   (unsigned long long)(cur.device_local_used +
                                        cur.host_visible_used),
                   (unsigned long long)cur.largest_free_range);
            TEST_CHECK(ok && cur.block_count +
                                    cur.dedicated_allocation_count <
                                (NB + NI) / 10,
                       "scale: backing allocs << resources");
            for (i = 0; i < 4; i++) {
                lc_render_target_destroy(tgts[i]);
                lc_image_view_destroy(tviews[i]);
                lc_image_destroy(timgs[i]);
            }
            for (i = 0; i < NI; i++) {
                if (imgs[i] != NULL) {
                    lc_image_destroy(imgs[i]);
                }
            }
            for (i = 0; i < NB; i++) {
                if (bufs[i] != NULL) {
                    lc_buffer_destroy(bufs[i]);
                }
            }
            free(tviews);
            free(timgs);
            free(tgts);
            free(imgs);
            free(bufs);
            lc_device_get_memory_stats(device, &cur);
            TEST_CHECK(cur.allocation_count == base.allocation_count,
                       "scale: all memory returns");
        } else {
            free(tviews);
            free(timgs);
            free(tgts);
            free(imgs);
            free(bufs);
        }
    }

    /* ---- Budget query (PART AB): report, never fake ---- */
    {
        lc_memory_budget budget;

        memset(&budget, 0, sizeof(budget));
        lc_device_get_memory_budget(device, &budget);
        printf("[info] budget: available=%d heaps=%u\n",
               budget.available, budget.heap_count);
        TEST_CHECK(1, "budget: query never crashes");
    }

    printf("memory vulkan: %d passed, %d failed\n", g_passed,
           g_failed);
    exit_code = (g_failed == 0) ? 0 : 1;

    lc_device_destroy(device);
    lc_shutdown();
    return exit_code;
}
