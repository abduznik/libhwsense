/*
 * read_sensors.c — Simple unified sensor reading example.
 *
 * This example demonstrates the unified API that works on both
 * AMD and Intel platforms transparently.
 *
 * Compile: cl /I include /I src read_sensors.c hwsense.lib
 * Run:     read_sensors.exe
 */

#include <stdio.h>
#include "hwsense_unified.h"

int main(void)
{
    printf("Initializing sensors...\n");

    hwsense_ctx_t *ctx = hwsense_unified_init();
    if (!ctx) {
        fprintf(stderr, "Failed to initialize sensors\n");
        return 1;
    }

    printf("Reading sensors...\n\n");

    /* Print formatted report */
    hwsense_print_report(ctx);

    /* Or read individual values */
    printf("--- Individual Readings ---\n");
    printf("CPU Temp:    %.1f C\n", hwsense_get_cpu_temp(ctx));
    printf("CPU Clock:   %d MHz\n", hwsense_get_cpu_clock(ctx));
    printf("CPU Voltage: %.3f V\n", hwsense_get_cpu_voltage(ctx));
    printf("CPU Power:   %.2f W\n", hwsense_get_cpu_power(ctx));
    printf("CPU Load:    %.1f%%\n", hwsense_get_cpu_load(ctx));
    printf("Memory:      %.1f GB\n", hwsense_get_memory_used(ctx));

    hwsense_shutdown(ctx);
    return 0;
}
