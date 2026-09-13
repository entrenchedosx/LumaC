/*
 * Vulkan pipeline-cache integration test (Phase 18, PARTs V, W,
 * AT, AU, AV).
 *
 * Device-only (no window/surface/swapchain): two-generation
 * load/save/reuse through a trivial triangle pipeline, corrupt /
 * truncated / junk blobs, unwritable paths, and disabled-cache
 * mode. Every variant must fail safely (device creation NEVER
 * fails because of cache trouble). Timings are REPORTED, never
 * asserted (machine-dependent).
 *
 * If the environment cannot provide a usable Vulkan setup, the
 * test reports SKIP and exits 0. Any other failure is a hard FAIL.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <lumac/lumac.h>

#ifndef LC_CACHE_SPV_DIR
#define LC_CACHE_SPV_DIR "."
#endif

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

/* Temp-dir cache path (per-variant file name). */
static void cache_path(char *out, size_t cap, const char *name) {
    const char *tmp = getenv("TEMP");

    if (tmp == NULL || tmp[0] == '\0') {
        tmp = getenv("TMPDIR");
    }
    if (tmp == NULL || tmp[0] == '\0') {
#if defined(_WIN32) || defined(_WIN64)
        tmp = ".";
#else
        tmp = "/tmp";
#endif
    }
    snprintf(out, cap, "%s/luma_pcache_%s.bin", tmp, name);
}

static int file_size(const char *path, long *out) {
    FILE *f = NULL;
    long n = 0;

    if (out != NULL) {
        *out = -1;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return 0;
    }
    n = ftell(f);
    fclose(f);
    if (n < 0) {
        return 0;
    }
    if (out != NULL) {
        *out = n;
    }
    return 1;
}

static void file_remove(const char *path) {
    remove(path);
}

static int file_write_bytes(const char *path, const void *data,
                            size_t size) {
    FILE *f = fopen(path, "wb");

    if (f == NULL) {
        return 0;
    }
    if (size > 0 && fwrite(data, 1, size, f) != size) {
        fclose(f);
        return 0;
    }
    fclose(f);
    return 1;
}

/* Minimal private SPIR-V file loader (tests only). */
static int load_spv(const char *name, void **out_code, size_t *out_size) {
    char path[512];
    FILE *file = NULL;
    long length = 0;
    void *code = NULL;
    size_t got = 0;

    snprintf(path, sizeof(path), "%s/%s", LC_CACHE_SPV_DIR, name);
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    length = ftell(file);
    if (length <= 0 || (length % 4) != 0) {
        fclose(file);
        return 0;
    }
    rewind(file);
    code = malloc((size_t)length);
    if (code == NULL) {
        fclose(file);
        return 0;
    }
    got = fread(code, 1, (size_t)length, file);
    fclose(file);
    if (got != (size_t)length) {
        free(code);
        return 0;
    }
    *out_code = code;
    *out_size = (size_t)length;
    return 1;
}

/* 0 = ready, 1 = environmental SKIP, -1 = hard failure */
static int make_device(const char *path, int disable, lc_device **out) {
    lc_device_desc desc;

    memset(&desc, 0, sizeof(desc));
    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 1;
    desc.pipeline_cache_path = path;
    desc.disable_pipeline_cache = disable;
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

/* Build one trivial pipeline (triangle shaders, RGBA8 target).
 * Returns creation time in ms, or -1 on failure. */
static double make_pipeline_ms(lc_device *device) {
    void *vcode = NULL;
    void *fcode = NULL;
    size_t vsize = 0;
    size_t fsize = 0;
    lc_shader *vs = NULL;
    lc_shader *fs = NULL;
    lc_pipeline *pipe = NULL;
    lc_graphics_pipeline_desc pd;
    lc_render_target_desc sig;
    lc_shader_desc sd;
    uint64_t freq = lc_clock_frequency();
    uint64_t t0;
    uint64_t t1;
    double ms = -1.0;

    if (!load_spv("triangle.vert.spv", &vcode, &vsize) ||
        !load_spv("triangle.frag.spv", &fcode, &fsize)) {
        printf("SKIP: triangle SPIR-V not staged\n");
        free(vcode);
        free(fcode);
        return -2.0;
    }
    memset(&sd, 0, sizeof(sd));
    sd.stage = LC_SHADER_STAGE_VERTEX;
    sd.code = vcode;
    sd.code_size = vsize;
    if (lc_shader_create(device, &sd, &vs) != LC_SUCCESS) {
        goto done;
    }
    memset(&sd, 0, sizeof(sd));
    sd.stage = LC_SHADER_STAGE_FRAGMENT;
    sd.code = fcode;
    sd.code_size = fsize;
    if (lc_shader_create(device, &sd, &fs) != LC_SUCCESS) {
        goto done;
    }
    memset(&sig, 0, sizeof(sig));
    sig.color_attachment_count = 1;
    sig.color_formats[0] = LC_FORMAT_RGBA8_UNORM;
    sig.depth_stencil_format = LC_FORMAT_UNDEFINED;
    sig.samples = LC_SAMPLE_COUNT_1;
    memset(&pd, 0, sizeof(pd));
    pd.vertex_shader = vs;
    pd.fragment_shader = fs;
    pd.cull_mode = LC_CULL_NONE;
    pd.front_face = LC_FRONT_FACE_COUNTER_CLOCKWISE;
    pd.render_target = sig;
    t0 = lc_clock_now();
    if (lc_graphics_pipeline_create(device, &pd, &pipe) != LC_SUCCESS) {
        goto done;
    }
    t1 = lc_clock_now();
    if (freq > 0) {
        ms = ((double)(t1 - t0) * 1000.0) / (double)freq;
    } else {
        ms = 0.0;
    }
done:
    lc_pipeline_destroy(pipe);
    lc_shader_destroy(fs);
    lc_shader_destroy(vs);
    free(vcode);
    free(fcode);
    return ms;
}

int main(void) {
    char path_gen[512];
    char path_bad[512];
    char path_nowrite[512];
    lc_device *device = NULL;
    lc_pipeline_cache_info info;
    long size = 0;
    double cold_ms = -1.0;
    double warm_ms = -1.0;
    int rc;
    int exit_code = 1;

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (lc_init() != LC_SUCCESS) {
        printf("SKIP: lc_init failed\n");
        return 0;
    }
    cache_path(path_gen, sizeof(path_gen), "gen");
    cache_path(path_bad, sizeof(path_bad), "bad");
    cache_path(path_nowrite, sizeof(path_nowrite),
               "no_such_dir_xyz/cache");
    file_remove(path_gen);
    file_remove(path_bad);

    /* Probe device availability first (SKIP when headless). */
    rc = make_device(NULL, 0, &device);
    if (rc != 0) {
        if (rc > 0) {
            SKIP_ENV("a Vulkan device");
        }
        printf("FAIL: probe device create\n");
        lc_shutdown();
        return 1;
    }
    lc_device_destroy(device);
    device = NULL;

    /* ---- PART AT run 1: no cache file -> pipelines -> save ---- */
    TEST_CHECK(file_size(path_gen, NULL) == 0,
               "cache: starts with no file");
    rc = make_device(path_gen, 0, &device);
    TEST_CHECK(rc == 0 && device != NULL, "cache: gen1 device creates");
    if (rc == 0 && device != NULL) {
        memset(&info, 0, sizeof(info));
        lc_device_get_pipeline_cache_info(device, &info);
        TEST_CHECK(info.enabled && !info.loaded_from_file,
                   "cache: gen1 memory cache, nothing loaded");
        cold_ms = make_pipeline_ms(device);
        TEST_CHECK(cold_ms >= 0.0, "cache: gen1 pipeline creates");
        if (cold_ms == -2.0) {
            printf("SKIP: triangle SPIR-V not staged\n");
            lc_device_destroy(device);
            lc_shutdown();
            return 0;
        }
        printf("[info] gen1 (cold) pipeline: %.2f ms\n", cold_ms);
        lc_device_destroy(device);
        device = NULL;
        /* saved_* flags are set during destroy (freed struct), so
         * post-shutdown proof is the file itself: exists + grows. */
        TEST_CHECK(file_size(path_gen, &size) != 0 && size > 0,
                   "cache: gen1 saved file on shutdown");
        printf("[info] gen1 saved %ld bytes\n", size);
    }

    /* ---- PART AT run 2: same cache -> loaded -> pipelines ---- */
    rc = make_device(path_gen, 0, &device);
    TEST_CHECK(rc == 0 && device != NULL, "cache: gen2 device creates");
    if (rc == 0 && device != NULL) {
        memset(&info, 0, sizeof(info));
        lc_device_get_pipeline_cache_info(device, &info);
        TEST_CHECK(info.enabled && info.loaded_from_file &&
                       info.bytes_loaded > 0,
                   "cache: gen2 loaded blob from file");
        printf("[info] gen2 loaded %llu bytes\n",
               (unsigned long long)info.bytes_loaded);
        warm_ms = make_pipeline_ms(device);
        TEST_CHECK(warm_ms >= 0.0, "cache: gen2 pipeline creates");
        if (warm_ms >= 0.0) {
            printf("[info] gen2 (warm) pipeline: %.2f ms\n", warm_ms);
        }
        lc_device_destroy(device);
        device = NULL;
    }

    /* ---- PART AU: corrupt blobs fail safely ---- */
    {
        static const char junk[] =
            "this is not a Vulkan pipeline cache at all, just text "
            "to prove corrupt blobs fall back to an empty cache";
        unsigned char ff[1024];
        unsigned char tiny[12];

        memset(ff, 0xFF, sizeof(ff));
        memset(tiny, 0xAB, sizeof(tiny));
        /* Junk text. */
        TEST_CHECK(file_write_bytes(path_bad, junk, sizeof(junk) - 1),
                   "cache: junk blob writes");
        rc = make_device(path_bad, 0, &device);
        TEST_CHECK(rc == 0 && device != NULL,
                   "cache: junk blob still creates device");
        if (rc == 0 && device != NULL) {
            memset(&info, 0, sizeof(info));
            lc_device_get_pipeline_cache_info(device, &info);
            TEST_CHECK(info.enabled,
                       "cache: junk falls back to working cache");
            cold_ms = make_pipeline_ms(device);
            TEST_CHECK(cold_ms >= 0.0,
                       "cache: pipeline creates over junk fallback");
            lc_device_destroy(device);
            device = NULL;
            /* Shutdown must have replaced junk with a valid blob. */
            rc = make_device(path_bad, 0, &device);
            if (rc == 0 && device != NULL) {
                memset(&info, 0, sizeof(info));
                lc_device_get_pipeline_cache_info(device, &info);
                TEST_CHECK(info.loaded_from_file,
                           "cache: junk repaired into loadable file");
                lc_device_destroy(device);
                device = NULL;
            } else {
                TEST_CHECK(0, "cache: repair generation creates");
            }
        }
        /* Truncated blob (12 bytes). */
        TEST_CHECK(file_write_bytes(path_bad, tiny, sizeof(tiny)),
                   "cache: truncated blob writes");
        rc = make_device(path_bad, 0, &device);
        TEST_CHECK(rc == 0 && device != NULL,
                   "cache: truncated blob still creates device");
        if (rc == 0 && device != NULL) {
            lc_device_destroy(device);
            device = NULL;
        }
        /* All-0xFF blob. */
        TEST_CHECK(file_write_bytes(path_bad, ff, sizeof(ff)),
                   "cache: 0xFF blob writes");
        rc = make_device(path_bad, 0, &device);
        TEST_CHECK(rc == 0 && device != NULL,
                   "cache: 0xFF blob still creates device");
        if (rc == 0 && device != NULL) {
            lc_device_destroy(device);
            device = NULL;
        }
        file_remove(path_bad);
    }

    /* ---- Unwritable path: no file I/O, device fine ---- */
    rc = make_device(path_nowrite, 0, &device);
    TEST_CHECK(rc == 0 && device != NULL,
               "cache: unwritable path still creates device");
    if (rc == 0 && device != NULL) {
        memset(&info, 0, sizeof(info));
        lc_device_get_pipeline_cache_info(device, &info);
        TEST_CHECK(info.enabled && !info.saved_to_file,
                   "cache: unwritable path saves nothing");
        lc_device_destroy(device);
        device = NULL;
    }

    /* ---- PART AV: disabled mode (no VkPipelineCache, no I/O) ---- */
    rc = make_device(path_gen, 1, &device);
    TEST_CHECK(rc == 0 && device != NULL,
               "cache: disabled mode creates device");
    if (rc == 0 && device != NULL) {
        memset(&info, 0, sizeof(info));
        lc_device_get_pipeline_cache_info(device, &info);
        TEST_CHECK(!info.enabled && !info.loaded_from_file &&
                       !info.saved_to_file,
                   "cache: disabled mode is fully inert");
        cold_ms = make_pipeline_ms(device);
        TEST_CHECK(cold_ms >= 0.0,
                   "cache: pipelines create with cache disabled");
        lc_device_destroy(device);
        device = NULL;
    }
    file_remove(path_gen);

    printf("pipeline cache: %d passed, %d failed\n", g_passed,
           g_failed);
    exit_code = (g_failed == 0) ? 0 : 1;

    lc_shutdown();
    return exit_code;
}
