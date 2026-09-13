#include <stdio.h>

#include <lumac/lumac.h>

int main(void) {
    lc_device *device = NULL;
    lc_device_desc desc = { 0 };

    if (lc_init() != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed\n");
        return 1;
    }

    /* No window needed: a GPU device and an OS window are separate concepts. */
    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 1; /* best-effort; falls back without layers */

    if (lc_device_create(&desc, &device) != LC_SUCCESS) {
        fprintf(stderr, "lc_device_create failed\n");
        lc_shutdown();
        return 1;
    }

    printf("LumaC %s\n", lc_get_version_string());
    printf("Backend: %s\n",
           lc_device_get_backend(device) == LC_BACKEND_VULKAN ? "Vulkan"
                                                              : "Unknown");
    printf("GPU: %s\n", lc_device_get_name(device));
    printf("Vendor ID: 0x%04x\n", lc_device_get_vendor_id(device));
    printf("Device ID: 0x%04x\n", lc_device_get_device_id(device));

    lc_device_destroy(device);
    lc_shutdown();
    return 0;
}
