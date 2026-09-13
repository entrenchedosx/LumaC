#include <stdio.h>

#include <lumac/lumac.h>

/*
 * Phase 9 example: texture resource workflow without any window.
 * Creates an 8x8 RGBA image with a full mip chain, uploads a
 * checkerboard through staging, generates mipmaps on the GPU, and
 * builds a sampler. No descriptors exist yet, so nothing is rendered;
 * exact pixel verification lives in test_image_vulkan.
 */

#define TEX_W 8
#define TEX_H 8

static unsigned char s_texels[TEX_W * TEX_H * 4];

static void fill_checkerboard(void) {
    uint32_t x;
    uint32_t y;

    for (y = 0; y < TEX_H; y++) {
        for (x = 0; x < TEX_W; x++) {
            unsigned char *px = &s_texels[(y * TEX_W + x) * 4];
            int white = (int)(((x / 2u) + (y / 2u)) % 2u);

            px[0] = white ? 255 : 200; /* R */
            px[1] = white ? 255 : 60; /* G */
            px[2] = white ? 255 : 60; /* B */
            px[3] = 255; /* A */
        }
    }
}

int main(void) {
    lc_device *device = NULL;
    lc_image *image = NULL;
    lc_sampler *sampler = NULL;
    lc_device_desc device_desc = { 0 };
    lc_image_desc image_desc;
    lc_image_upload_desc upload;
    lc_sampler_desc sampler_desc;
    lc_device_limits limits;
    lc_result res;

    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    device_desc.backend = LC_BACKEND_VULKAN;
    device_desc.enable_validation = 1; /* best-effort; falls back without layers */
    res = lc_device_create(&device_desc, &device);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_device_create failed (%d)\n", res);
        lc_shutdown();
        return 1;
    }

    lc_device_get_limits(device, &limits);
    printf("LumaC %s\n", lc_get_version_string());
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("max anisotropy: %.1f, max array layers: %u\n",
           (double)limits.max_sampler_anisotropy,
           limits.max_image_array_layers);

    fill_checkerboard();

    image_desc.type = LC_IMAGE_TYPE_2D;
    image_desc.format = LC_FORMAT_RGBA8_UNORM;
    image_desc.width = TEX_W;
    image_desc.height = TEX_H;
    image_desc.depth = 1;
    image_desc.mip_levels = 0; /* full chain */
    image_desc.array_layers = 1;
    image_desc.usage = LC_IMAGE_USAGE_SAMPLED | LC_IMAGE_USAGE_TRANSFER_SRC |
                       LC_IMAGE_USAGE_TRANSFER_DST;
    image_desc.flags = LC_IMAGE_FLAG_NONE;
    image_desc.samples = LC_SAMPLE_COUNT_1;
    res = lc_image_create(device, &image_desc, &image);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_image_create failed (%d)\n", res);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    printf("Image: %ux%u, mips: %u, layers: %u\n", lc_image_get_width(image),
           lc_image_get_height(image), lc_image_get_mip_levels(image),
           lc_image_get_array_layers(image));

    upload.mip_level = 0;
    upload.array_layer = 0;
    upload.width = TEX_W;
    upload.height = TEX_H;
    upload.depth = 1;
    upload.data = s_texels;
    upload.data_size = sizeof(s_texels);
    res = lc_image_write(image, &upload);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_image_write failed (%d)\n", res);
        lc_image_destroy(image);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    printf("Base level uploaded (%llu bytes)\n",
           (unsigned long long)sizeof(s_texels));

    res = lc_image_generate_mipmaps(image);
    if (res == LC_ERROR_UNSUPPORTED) {
        printf("Mipmap generation unsupported for this format\n");
    } else if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_image_generate_mipmaps failed (%d)\n", res);
        lc_image_destroy(image);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    } else {
        printf("Mipmaps generated\n");
    }

    sampler_desc.min_filter = LC_FILTER_LINEAR;
    sampler_desc.mag_filter = LC_FILTER_LINEAR;
    sampler_desc.mipmap_mode = LC_MIPMAP_MODE_LINEAR;
    sampler_desc.address_u = LC_ADDRESS_REPEAT;
    sampler_desc.address_v = LC_ADDRESS_REPEAT;
    sampler_desc.address_w = LC_ADDRESS_CLAMP_TO_EDGE;
    sampler_desc.mip_lod_bias = 0.0f;
    sampler_desc.min_lod = 0.0f;
    sampler_desc.max_lod = 4.0f;
    sampler_desc.max_anisotropy = limits.max_sampler_anisotropy;
    res = lc_sampler_create(device, &sampler_desc, &sampler);
    if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_sampler_create failed (%d)\n", res);
        lc_image_destroy(image);
        lc_device_destroy(device);
        lc_shutdown();
        return 1;
    }
    printf("Sampler created (anisotropy %.1f)\n",
           (double)limits.max_sampler_anisotropy);

    lc_sampler_destroy(sampler);
    lc_image_destroy(image);
    lc_device_destroy(device);
    lc_shutdown();
    printf("Texture workflow complete\n");
    return 0;
}
