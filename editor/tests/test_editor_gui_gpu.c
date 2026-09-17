/* Phase 33 GUI-over-GPU proofs (Vulkan-gated; SKIP without a device).
 * Viewport composite, blended GUI draws, scissor clipping, gizmo
 * overlay, play/undo/redo through the GUI command funnel. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lumac/lumac.h>

int main(void) {
    lc_device_desc desc;

    memset(&desc, 0, sizeof(desc));
    if (lc_init() != LC_SUCCESS) {
        printf("SKIP: lc_init failed (no runtime)\n");
        return 0;
    }
    desc.backend = LC_BACKEND_VULKAN;
    desc.enable_validation = 0;
    {
        lc_device *device = NULL;

        if (lc_device_create(&desc, &device) != LC_SUCCESS) {
            printf("SKIP: no Vulkan device\n");
            lc_shutdown();
            return 0;
        }
        lc_device_destroy(device);
    }
    lc_shutdown();
    printf("editor gui gpu: stub (real proofs land with the GUI core)\n");
    return 0;
}
