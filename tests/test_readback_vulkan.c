/*
 * Vulkan readback integration test (Phase 18, PARTs A-G, AN-AQ, M-O).
 *
 * Public image readback ONLY (lc_image_query_readback /
 * lc_image_readback): no white-box copy helpers, no staging buffers,
 * no backend types. Covers the deterministic 4-quadrant pattern
 * (orientation/channels/row pitch), float readback incl. HDR-range
 * values, mip/layer reads, depth reads, the full error matrix, and
 * the ABA resource-identity stress (IDs unique across hundreds of
 * destroy/create cycles, never aliased by pointer recycling).
 *
 * Device-only: no window, surface, or swapchain needed. If the
 * environment cannot provide a usable Vulkan setup, the test
 * reports SKIP and exits 0. Any other failure is a hard FAIL.
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

/* 0 = ready, 1 = environmental SKIP, -1 = hard failure */
static int make_device(lc_device **out) {
    lc_device_desc desc;

    memset(&desc, 0, sizeof(desc));
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

#define SKIP_ENV(what) do { \
    printf("SKIP: environment cannot provide %s\n", what); \
    lc_shutdown(); \
    return 0; \
} while (0)

/* Minimal float -> half (round-to-nearest-even) for the RGBA16F
 * upload side; the GPU returns the same bits back. */
static uint16_t f32_to_f16(float f) {
    union { float f; uint32_t u; } v;
    uint32_t sign;
    int32_t exp;
    uint32_t mant;

    v.f = f;
    sign = (v.u >> 16) & 0x8000u;
    exp = (int32_t)((v.u >> 23) & 0xffu) - 112;
    mant = v.u & 0x7fffffu;
    if (exp >= 31) {
        return (uint16_t)(sign | 0x7bffu); /* clamp to max half */
    }
    if (exp <= 0) {
        return (uint16_t)sign; /* flush subnormals to zero */
    }
    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
}

static float f16_to_f32(uint16_t h) {
    union { float f; uint32_t u; } v;
    uint32_t sign = ((uint32_t)h & 0x8000u) << 16;
    uint32_t exp = ((uint32_t)h >> 10) & 0x1fu;
    uint32_t mant = (uint32_t)h & 0x3ffu;

    if (exp == 0) {
        v.u = sign; /* subnormal/zero -> zero (uploads avoid these) */
    } else if (exp == 31) {
        v.u = sign | 0x7f800000u | (mant << 13);
    } else {
        v.u = sign | ((exp + 112u) << 23) | (mant << 13);
    }
    return v.f;
}

static lc_image *make_image(lc_device *device, lc_format format,
                            uint32_t w, uint32_t h, uint32_t mips,
                            uint32_t layers, uint32_t usage,
                            uint32_t flags) {
    lc_image_desc desc;
    lc_image *image = NULL;

    memset(&desc, 0, sizeof(desc));
    desc.type = LC_IMAGE_TYPE_2D;
    desc.format = format;
    desc.width = w;
    desc.height = h;
    desc.depth = 1;
    desc.mip_levels = mips;
    desc.array_layers = layers;
    desc.usage = usage;
    desc.flags = flags;
    desc.samples = LC_SAMPLE_COUNT_1;
    if (lc_image_create(device, &desc, &image) != LC_SUCCESS) {
        return NULL;
    }
    return image;
}

static int upload(lc_image *image, uint32_t mip, uint32_t layer,
                  uint32_t w, uint32_t h, const void *data,
                  uint64_t size) {
    lc_image_upload_desc up;

    memset(&up, 0, sizeof(up));
    up.mip_level = mip;
    up.array_layer = layer;
    up.width = w;
    up.height = h;
    up.depth = 1;
    up.data = data;
    up.data_size = size;
    return (lc_image_write(image, &up) == LC_SUCCESS) ? 0 : -1;
}

int main(void) {
    lc_device *device = NULL;
    int rc;
    int exit_code = 1;

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

    /* ---- PART AN: deterministic 4-quadrant pattern ----
     * 8x8 RGBA8: TL red, TR green, BL blue, BR white. Proves
     * orientation (row 0 = top), channels (RGBA order), and tight
     * row pitch (row_pitch == width * 4). */
    {
        static const uint32_t W = 8;
        static const uint32_t H = 8;
        unsigned char pattern[8 * 8 * 4];
        unsigned char out[8 * 8 * 4];
        lc_image_readback_desc rd;
        lc_image_readback_info info;
        lc_image *image;
        uint32_t x;
        uint32_t y;
        size_t need = 0;

        for (y = 0; y < H; y++) {
            for (x = 0; x < W; x++) {
                unsigned char *p = pattern + ((size_t)y * W + x) * 4u;
                int right = (x >= W / 2) ? 1 : 0;
                int bottom = (y >= H / 2) ? 1 : 0;

                p[0] = (unsigned char)(!right || bottom ? 255 : 0);
                p[1] = (unsigned char)(right || bottom ? 255 : 0);
                p[2] = (unsigned char)(bottom ? 255 : 0);
                p[3] = 255;
            }
        }
        image = make_image(device, LC_FORMAT_RGBA8_UNORM, W, H, 1, 1,
                           LC_IMAGE_USAGE_SAMPLED |
                               LC_IMAGE_USAGE_TRANSFER_SRC |
                               LC_IMAGE_USAGE_TRANSFER_DST,
                           0);
        TEST_CHECK(image != NULL, "pattern: image creates");
        if (image != NULL) {
            memset(&rd, 0, sizeof(rd));
            TEST_CHECK(upload(image, 0, 0, W, H, pattern,
                              sizeof(pattern)) == 0,
                       "pattern: upload works");
            TEST_CHECK(lc_image_query_readback(image, &rd, &info) ==
                           LC_SUCCESS,
                       "pattern: query works");
            TEST_CHECK(info.width == W && info.height == H &&
                           info.format == LC_FORMAT_RGBA8_UNORM &&
                           info.row_pitch == (size_t)W * 4u &&
                           info.size == sizeof(pattern),
                       "pattern: tight deterministic layout");
            memset(out, 0xAA, sizeof(out));
            TEST_CHECK(lc_image_readback(image, &rd, out, sizeof(out),
                                         &need) == LC_SUCCESS &&
                           need == sizeof(pattern),
                       "pattern: readback works with size");
            TEST_CHECK(memcmp(out, pattern, sizeof(pattern)) == 0,
                       "pattern: bytes round-trip exactly");
            /* Spot orientation: first row is top (red/green), last
             * row is bottom (blue/white). Pixel x=4 is green. */
            TEST_CHECK(out[0] == 255 && out[1] == 0 && out[2] == 0 &&
                           out[16] == 0 && out[17] == 255,
                       "pattern: top row is red/green");
            TEST_CHECK(out[(7 * 8 * 4) + 2] == 255 &&
                           out[(7 * 8 * 4) + 7 * 4] == 255,
                       "pattern: bottom row is blue/white");
            lc_image_destroy(image);
        }
    }

    /* ---- PART AO-32: exact float round-trip (RGBA32F) ----
     * Values 0 / 0.5 / 1 / 2 / 8 must survive bit-exactly. */
    {
        static const float vals[5] = { 0.0f, 0.5f, 1.0f, 2.0f, 8.0f };
        float pattern[5 * 4];
        float out[5 * 4];
        lc_image_readback_desc rd;
        lc_image *image;
        int i;
        int exact = 1;

        for (i = 0; i < 5; i++) {
            pattern[i * 4 + 0] = vals[i];
            pattern[i * 4 + 1] = vals[i] * 0.5f;
            pattern[i * 4 + 2] = vals[i] * 0.25f;
            pattern[i * 4 + 3] = 1.0f;
        }
        image = make_image(device, LC_FORMAT_RGBA32_FLOAT, 5, 1, 1, 1,
                           LC_IMAGE_USAGE_SAMPLED |
                               LC_IMAGE_USAGE_TRANSFER_SRC |
                               LC_IMAGE_USAGE_TRANSFER_DST,
                           0);
        TEST_CHECK(image != NULL, "float32: image creates");
        if (image != NULL) {
            memset(&rd, 0, sizeof(rd));
            TEST_CHECK(upload(image, 0, 0, 5, 1, pattern,
                              sizeof(pattern)) == 0,
                       "float32: upload works");
            memset(out, 0, sizeof(out));
            TEST_CHECK(lc_image_readback(image, &rd, out, sizeof(out),
                                         NULL) == LC_SUCCESS,
                       "float32: readback works");
            for (i = 0; i < 5 * 4; i++) {
                if (out[i] != pattern[i]) {
                    exact = 0;
                }
            }
            TEST_CHECK(exact, "float32: 0/0.5/1/2/8 bit-exact");
            TEST_CHECK(out[4 * 4] == 8.0f, "float32: HDR value survives");
            lc_image_destroy(image);
        }
    }

    /* ---- PART AO-16: RGBA16F with tolerance ----
     * Same ladder through half floats (1e-3 tolerance covers the
     * 8.0 half step of 2^-8 * 2^3 = 0.0078... use 0.01). */
    {
        static const float vals[5] = { 0.0f, 0.5f, 1.0f, 2.0f, 8.0f };
        uint16_t pattern[5 * 4];
        uint16_t out[5 * 4];
        lc_image_readback_desc rd;
        lc_image *image;
        int i;
        int ok = 1;

        for (i = 0; i < 5; i++) {
            pattern[i * 4 + 0] = f32_to_f16(vals[i]);
            pattern[i * 4 + 1] = f32_to_f16(vals[i] * 0.5f);
            pattern[i * 4 + 2] = f32_to_f16(vals[i] * 0.25f);
            pattern[i * 4 + 3] = f32_to_f16(1.0f);
        }
        image = make_image(device, LC_FORMAT_RGBA16_FLOAT, 5, 1, 1, 1,
                           LC_IMAGE_USAGE_SAMPLED |
                               LC_IMAGE_USAGE_TRANSFER_SRC |
                               LC_IMAGE_USAGE_TRANSFER_DST,
                           0);
        TEST_CHECK(image != NULL, "float16: image creates");
        if (image != NULL) {
            memset(&rd, 0, sizeof(rd));
            TEST_CHECK(upload(image, 0, 0, 5, 1, pattern,
                              sizeof(pattern)) == 0,
                       "float16: upload works");
            memset(out, 0, sizeof(out));
            TEST_CHECK(lc_image_readback(image, &rd, out, sizeof(out),
                                         NULL) == LC_SUCCESS,
                       "float16: readback works");
            for (i = 0; i < 5; i++) {
                float got = f16_to_f32(out[i * 4]);
                float want = vals[i];
                float diff = (got > want) ? got - want : want - got;

                if (diff > 0.01f) {
                    ok = 0;
                }
            }
            TEST_CHECK(ok, "float16: ladder within tolerance");
            TEST_CHECK(f16_to_f32(out[4 * 4]) > 7.9f,
                       "float16: HDR value survives");
            lc_image_destroy(image);
        }
    }

    /* ---- PART AP: mip + layer reads ----
     * Mipmapped 8x8 (mip1 = 4x4 after generate), 6-layer cube image
     * with per-layer colors; read specific mip and specific layers. */
    {
        unsigned char white[8 * 8 * 4];
        lc_image_readback_desc rd;
        lc_image_readback_info info;
        lc_image *mips = NULL;
        lc_image *layers = NULL;

        memset(white, 0xFF, sizeof(white));
        mips = make_image(device, LC_FORMAT_RGBA8_UNORM, 8, 8, 0, 1,
                          LC_IMAGE_USAGE_SAMPLED |
                              LC_IMAGE_USAGE_TRANSFER_SRC |
                              LC_IMAGE_USAGE_TRANSFER_DST,
                          0);
        TEST_CHECK(mips != NULL, "mips: image creates");
        if (mips != NULL) {
            unsigned char m1[4 * 4 * 4];
            int all_white = 1;
            uint32_t i;

            TEST_CHECK(upload(mips, 0, 0, 8, 8, white, sizeof(white)) ==
                           0,
                       "mips: mip0 upload works");
            TEST_CHECK(lc_image_generate_mipmaps(mips) == LC_SUCCESS,
                       "mips: generate works");
            memset(&rd, 0, sizeof(rd));
            rd.mip_level = 1;
            TEST_CHECK(lc_image_query_readback(mips, &rd, &info) ==
                           LC_SUCCESS &&
                           info.width == 4 && info.height == 4,
                       "mips: mip1 reports 4x4");
            memset(m1, 0, sizeof(m1));
            TEST_CHECK(lc_image_readback(mips, &rd, m1, sizeof(m1),
                                         NULL) == LC_SUCCESS,
                       "mips: mip1 reads");
            for (i = 0; i < sizeof(m1); i++) {
                if (m1[i] != 0xFF) {
                    all_white = 0;
                }
            }
            TEST_CHECK(all_white, "mips: white mip0 blits to white");
            lc_image_destroy(mips);
        }
        layers = make_image(device, LC_FORMAT_RGBA8_UNORM, 4, 4, 1, 6,
                            LC_IMAGE_USAGE_SAMPLED |
                                LC_IMAGE_USAGE_TRANSFER_SRC |
                                LC_IMAGE_USAGE_TRANSFER_DST,
                            LC_IMAGE_FLAG_CUBE_COMPATIBLE);
        TEST_CHECK(layers != NULL, "layers: cube image creates");
        if (layers != NULL) {
            unsigned char face[4 * 4 * 4];
            unsigned char got[4 * 4 * 4];
            uint32_t l;
            int ok = 1;

            for (l = 0; l < 6; l++) {
                uint32_t i;

                for (i = 0; i < 4 * 4; i++) {
                    face[i * 4 + 0] = (unsigned char)(l * 40);
                    face[i * 4 + 1] = (unsigned char)(255 - l * 40);
                    face[i * 4 + 2] = (unsigned char)(l * 17);
                    face[i * 4 + 3] = 255;
                }
                if (upload(layers, 0, l, 4, 4, face, sizeof(face)) !=
                    0) {
                    ok = 0;
                }
            }
            TEST_CHECK(ok, "layers: per-layer uploads work");
            for (l = 0; l < 6 && ok; l++) {
                memset(&rd, 0, sizeof(rd));
                rd.array_layer = l;
                memset(got, 0, sizeof(got));
                if (lc_image_readback(layers, &rd, got, sizeof(got),
                                      NULL) != LC_SUCCESS) {
                    ok = 0;
                    break;
                }
                /* Recompute expected face for this layer. */
                {
                    uint32_t i;

                    for (i = 0; i < 4 * 4; i++) {
                        face[i * 4 + 0] = (unsigned char)(l * 40);
                        face[i * 4 + 1] = (unsigned char)(255 - l * 40);
                        face[i * 4 + 2] = (unsigned char)(l * 17);
                        face[i * 4 + 3] = 255;
                    }
                }
                if (memcmp(got, face, sizeof(face)) != 0) {
                    ok = 0;
                }
            }
            TEST_CHECK(ok, "layers: each face reads its own color");
            lc_image_destroy(layers);
        }
    }

    /* ---- PART F: depth readback (D32_FLOAT) ----
     * Upload zeros through the transfer path, read raw floats back.
     * (Depth rendering itself is older machinery; readback only
     * needs the transfer path.) */
    {
        float zeros[4 * 4];
        float got[4 * 4];
        lc_image_readback_desc rd;
        lc_image *depth;
        uint32_t i;
        int ok = 1;

        memset(zeros, 0, sizeof(zeros));
        depth = make_image(device, LC_FORMAT_D32_FLOAT, 4, 4, 1, 1,
                           LC_IMAGE_USAGE_DEPTH_STENCIL |
                               LC_IMAGE_USAGE_TRANSFER_SRC |
                               LC_IMAGE_USAGE_TRANSFER_DST,
                           0);
        TEST_CHECK(depth != NULL, "depth: image creates");
        if (depth != NULL) {
            memset(&rd, 0, sizeof(rd));
            TEST_CHECK(upload(depth, 0, 0, 4, 4, zeros,
                              sizeof(zeros)) == 0,
                       "depth: upload works");
            memset(got, 0xFF, sizeof(got));
            TEST_CHECK(lc_image_readback(depth, &rd, got, sizeof(got),
                                         NULL) == LC_SUCCESS,
                       "depth: readback works");
            for (i = 0; i < 4 * 4; i++) {
                if (got[i] != 0.0f) {
                    ok = 0;
                }
            }
            TEST_CHECK(ok, "depth: zeros round-trip raw");
            lc_image_destroy(depth);
        }
    }

    /* ---- PART AM: BGRA + sRGB round-trips (no conversion) ----
     * Readback preserves target encoding: BGRA stays BGRA, sRGB
     * stays encoded. The screenshot path relies on this. */
    {
        unsigned char pattern[4 * 4 * 4];
        unsigned char out[4 * 4 * 4];
        lc_image_readback_desc rd;
        lc_image *bgra;
        lc_image *srgb;
        uint32_t i;

        for (i = 0; i < 4 * 4; i++) {
            pattern[i * 4 + 0] = (unsigned char)(i * 16);
            pattern[i * 4 + 1] = (unsigned char)(255 - i * 16);
            pattern[i * 4 + 2] = (unsigned char)(i * 32);
            pattern[i * 4 + 3] = 255;
        }
        bgra = make_image(device, LC_FORMAT_BGRA8_UNORM, 4, 4, 1, 1,
                          LC_IMAGE_USAGE_SAMPLED |
                              LC_IMAGE_USAGE_TRANSFER_SRC |
                              LC_IMAGE_USAGE_TRANSFER_DST,
                          0);
        srgb = make_image(device, LC_FORMAT_RGBA8_SRGB, 4, 4, 1, 1,
                          LC_IMAGE_USAGE_SAMPLED |
                              LC_IMAGE_USAGE_TRANSFER_SRC |
                              LC_IMAGE_USAGE_TRANSFER_DST,
                          0);
        TEST_CHECK(bgra != NULL && srgb != NULL,
                   "formats: BGRA/sRGB images create");
        memset(&rd, 0, sizeof(rd));
        if (bgra != NULL) {
            TEST_CHECK(upload(bgra, 0, 0, 4, 4, pattern,
                              sizeof(pattern)) == 0,
                       "formats: BGRA upload works");
            memset(out, 0, sizeof(out));
            TEST_CHECK(lc_image_readback(bgra, &rd, out, sizeof(out),
                                         NULL) == LC_SUCCESS &&
                           memcmp(out, pattern, sizeof(pattern)) == 0,
                       "formats: BGRA bytes preserved exactly");
            lc_image_destroy(bgra);
        }
        if (srgb != NULL) {
            TEST_CHECK(upload(srgb, 0, 0, 4, 4, pattern,
                              sizeof(pattern)) == 0,
                       "formats: sRGB upload works");
            memset(out, 0, sizeof(out));
            TEST_CHECK(lc_image_readback(srgb, &rd, out, sizeof(out),
                                         NULL) == LC_SUCCESS &&
                           memcmp(out, pattern, sizeof(pattern)) == 0,
                       "formats: sRGB bytes preserved exactly");
            lc_image_destroy(srgb);
        }
    }

    /* ---- View->image borrower incl. dead handles ---- */
    {
        lc_image *image = make_image(device, LC_FORMAT_RGBA8_UNORM, 2,
                                     2, 1, 1, LC_IMAGE_USAGE_SAMPLED,
                                     0);
        lc_image_view *view = NULL;
        lc_image_view_desc vdesc;

        memset(&vdesc, 0, sizeof(vdesc));
        vdesc.type = LC_IMAGE_VIEW_2D;
        vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
        vdesc.mip_level_count = 1;
        vdesc.array_layer_count = 1;
        if (image != NULL &&
            lc_image_view_create(image, &vdesc, &view) == LC_SUCCESS) {
            TEST_CHECK(lc_image_view_get_image(view) == image,
                       "borrower: live view resolves its image");
            TEST_CHECK(lc_image_view_get_image(NULL) == NULL,
                       "borrower: NULL view reads NULL");
            /* Destroying the image kills dependent views first. */
            lc_image_destroy(image);
            image = NULL;
            TEST_CHECK(lc_image_view_get_image(view) == NULL,
                       "borrower: dead view reads NULL, no crash");
            view = NULL;
        } else {
            TEST_CHECK(0, "borrower: view creates");
            lc_image_destroy(image);
        }
    }

    /* ---- PART M/N: resource identity basics ----
     * IDs nonzero, unique across objects and types. */
    {
        lc_image *a = make_image(device, LC_FORMAT_RGBA8_UNORM, 2, 2, 1,
                                 1, LC_IMAGE_USAGE_SAMPLED, 0);
        lc_image *b = make_image(device, LC_FORMAT_RGBA8_UNORM, 2, 2, 1,
                                 1, LC_IMAGE_USAGE_SAMPLED, 0);
        lc_buffer *buf = NULL;
        lc_buffer_desc bdesc;
        lc_resource_id ida = 0;
        lc_resource_id idb = 0;
        lc_resource_id idbuf = 0;

        memset(&bdesc, 0, sizeof(bdesc));
        bdesc.size = 64;
        bdesc.usage = LC_BUFFER_USAGE_TRANSFER_DST;
        bdesc.memory = LC_MEMORY_GPU_TO_CPU;
        if (lc_buffer_create(device, &bdesc, &buf) != LC_SUCCESS) {
            buf = NULL;
        }
        if (a != NULL) {
            ida = lc_image_get_resource_id(a);
        }
        if (b != NULL) {
            idb = lc_image_get_resource_id(b);
        }
        if (buf != NULL) {
            idbuf = lc_buffer_get_resource_id(buf);
        }
        TEST_CHECK(ida != 0 && idb != 0 && idbuf != 0,
                   "identity: IDs nonzero");
        TEST_CHECK(ida != idb && ida != idbuf && idb != idbuf,
                   "identity: IDs unique across objects/types");
        TEST_CHECK(lc_image_get_resource_id(NULL) == 0,
                   "identity: NULL image reads 0");
        TEST_CHECK(lc_buffer_get_resource_id(NULL) == 0,
                   "identity: NULL buffer reads 0");
        lc_image_destroy(a);
        lc_image_destroy(b);
        /* Dead handles read 0 (never the recycled ID). */
        TEST_CHECK(lc_image_get_resource_id(a) == 0,
                   "identity: dead image reads 0");
        lc_buffer_destroy(buf);
    }

    /* ---- PART O: ABA stress (500 destroy/create cycles) ----
     * Collect every image + view ID; all must be unique and nonzero
     * even though the allocator recycles wrapper addresses. */
    {
        static const uint32_t ROUNDS = 500;
        lc_resource_id *ids = NULL;
        lc_image *image = NULL;
        lc_image_view *view = NULL;
        lc_image_view_desc vdesc;
        uint32_t i;
        uint32_t j;
        int unique = 1;

        ids =
            (lc_resource_id *)malloc(sizeof(lc_resource_id) * ROUNDS * 2);
        TEST_CHECK(ids != NULL, "aba: scratch allocates");
        if (ids != NULL) {
            for (i = 0; i < ROUNDS; i++) {
                image = make_image(device, LC_FORMAT_RGBA8_UNORM, 2, 2,
                                   1, 1, LC_IMAGE_USAGE_SAMPLED, 0);
                if (image == NULL) {
                    unique = 0;
                    break;
                }
                memset(&vdesc, 0, sizeof(vdesc));
                vdesc.type = LC_IMAGE_VIEW_2D;
                vdesc.aspect = LC_IMAGE_ASPECT_COLOR;
                vdesc.mip_level_count = 1;
                vdesc.array_layer_count = 1;
                if (lc_image_view_create(image, &vdesc, &view) !=
                    LC_SUCCESS) {
                    unique = 0;
                    lc_image_destroy(image);
                    break;
                }
                ids[i * 2] = lc_image_get_resource_id(image);
                ids[i * 2 + 1] =
                    lc_image_view_get_resource_id(view);
                if (ids[i * 2] == 0 || ids[i * 2 + 1] == 0) {
                    unique = 0;
                }
                lc_image_view_destroy(view);
                view = NULL;
                lc_image_destroy(image);
                image = NULL;
            }
            /* Quadratic uniqueness over 1000 IDs: trivial cost. */
            for (i = 0; i < ROUNDS * 2 && unique; i++) {
                for (j = i + 1; j < ROUNDS * 2; j++) {
                    if (ids[i] == ids[j]) {
                        unique = 0;
                        break;
                    }
                }
            }
            TEST_CHECK(unique,
                       "aba: 1000 IDs unique across 500 cycles");
            free(ids);
        }
        lc_image_view_destroy(view);
        lc_image_destroy(image);
    }

    /* ---- PART AQ: error matrix (no crashes, loud codes) ---- */
    {
        lc_image *plain = make_image(device, LC_FORMAT_RGBA8_UNORM, 4,
                                     4, 1, 1,
                                     LC_IMAGE_USAGE_SAMPLED |
                                         LC_IMAGE_USAGE_TRANSFER_DST,
                                     0);
        lc_image *readable = make_image(device, LC_FORMAT_RGBA8_UNORM,
                                        4, 4, 1, 1,
                                        LC_IMAGE_USAGE_SAMPLED |
                                            LC_IMAGE_USAGE_TRANSFER_SRC |
                                            LC_IMAGE_USAGE_TRANSFER_DST,
                                        0);
        lc_image_readback_desc rd;
        lc_image_readback_info info;
        unsigned char tiny[4];
        unsigned char buf[4 * 4 * 4];
        size_t need = 0;

        memset(&rd, 0, sizeof(rd));
        TEST_CHECK(lc_image_query_readback(NULL, &rd, &info) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "errors: NULL image query rejected");
        TEST_CHECK(lc_image_query_readback(readable, NULL, &info) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "errors: NULL desc query rejected");
        TEST_CHECK(lc_image_query_readback(readable, &rd, NULL) ==
                       LC_ERROR_INVALID_ARGUMENT,
                   "errors: NULL out query rejected");
        if (plain != NULL) {
            TEST_CHECK(lc_image_query_readback(plain, &rd, &info) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "errors: missing TRANSFER_SRC rejected loudly");
            lc_image_destroy(plain);
            plain = NULL;
            TEST_CHECK(lc_image_query_readback(plain, &rd, &info) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "errors: dead image rejected");
        }
        if (readable != NULL) {
            rd.mip_level = 7;
            TEST_CHECK(lc_image_query_readback(readable, &rd, &info) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "errors: bad mip rejected");
            memset(&rd, 0, sizeof(rd));
            rd.array_layer = 3;
            TEST_CHECK(lc_image_query_readback(readable, &rd, &info) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "errors: bad layer rejected");
            memset(&rd, 0, sizeof(rd));
            TEST_CHECK(lc_image_readback(NULL, &rd, buf, sizeof(buf),
                                         NULL) ==
                           LC_ERROR_INVALID_ARGUMENT,
                       "errors: NULL image read rejected");
            TEST_CHECK(lc_image_readback(readable, &rd, tiny,
                                         sizeof(tiny),
                                         &need) ==
                           LC_ERROR_INVALID_ARGUMENT &&
                           need == sizeof(buf),
                       "errors: short dst rejected with size");
            /* Sizing query writes nothing but reports need. */
            need = 0;
            memset(buf, 0xAA, sizeof(buf));
            TEST_CHECK(lc_image_readback(readable, &rd, NULL, 0,
                                         &need) == LC_SUCCESS &&
                           need == sizeof(buf) && buf[0] == 0xAA,
                       "errors: sizing query is side-effect free");
            lc_image_destroy(readable);
        }
    }

    printf("readback vulkan: %d passed, %d failed\n", g_passed,
           g_failed);
    exit_code = (g_failed == 0) ? 0 : 1;

    lc_device_destroy(device);
    lc_shutdown();
    return exit_code;
}
