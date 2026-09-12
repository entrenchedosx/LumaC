#include <stdio.h>
#include <lumac/lumac.h>

int main(void) {
    printf("LumaC version: %s\n", lc_get_version_string());

    lc_result res = lc_init();
    if (res != LC_SUCCESS) {
        fprintf(stderr, "lc_init failed: %d\n", res);
        return 1;
    }
    printf("lc_init succeeded\n");

    /* Future rendering code would go here */

    lc_shutdown();
    printf("lc_shutdown completed\n");

    return 0;
}
