/*
 * read_cpu_temp_linux.c - Reads CPU temperature via libhwsense on Linux.
 *
 * Requires root (or CAP_SYS_RAWIO) and the msr kernel module:
 *   sudo modprobe msr
 *   sudo ./read_cpu_temp
 */

#include <stdio.h>
#include "hwsense.h"

int main(void)
{
    hwsense_ctx_t *ctx = hwsense_init();
    if (!ctx)
        return 1;

    hwsense_temp_result_t temp = hwsense_cpu_package_temp(ctx);
    if (temp.ok)
        printf("CPU temp: %.1f C\n", temp.celsius);
    else
        fprintf(stderr, "CPU temp read failed: %s\n", temp.error);

    hwsense_shutdown(ctx);
    return temp.ok ? 0 : 1;
}
