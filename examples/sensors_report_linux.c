/*
 * sensors_report_linux.c - Prints every sensor libhwsense supports on Linux.
 * No root needed for hwmon/NVML-backed readings.
 */

#include <stdio.h>
#include "hwsense_unified.h"

int main(void)
{
    hwsense_ctx_t *ctx = hwsense_unified_init();
    hwsense_gpu_info_t g;
    hwsense_superio_result_t s;
    int i;

    if (!ctx)
        return 1;

    hwsense_print_report(ctx);

    g = hwsense_gpu_info(0);
    if (g.ok)
        printf("\n--- GPU detail ---\n  %s\n  %d C, load %d%%, VRAM %d/%d MB\n"
               "  Power %d/%d W, clock %d MHz, mem clock %d MHz\n",
               g.name, g.temperature, g.gpu_load, g.vram_used_mb, g.vram_total_mb,
               g.power_usage_w, g.power_limit_w, g.clock_mhz, g.mem_clock_mhz);
    else
        printf("\nGPU: %s\n", g.error);

    {
        hwsense_thermal_zone_t z[HWSENSE_MAX_THERMAL_ZONES];
        int nz = hwsense_thermal_zones(z, HWSENSE_MAX_THERMAL_ZONES);

        if (nz > 0) {
            printf("\n--- Thermal zones ---\n");
            for (i = 0; i < nz; i++)
                printf("  %-24s %.1f C\n", z[i].type, z[i].celsius);
        }
    }

    s = hwsense_superio_temps(ctx);
    if (s.ok) {
        printf("\n--- Board temps (%s) ---\n", s.chip_name);
        for (i = 0; i < s.count; i++)
            printf("  temp%d: %d C\n", i + 1, s.temperatures[i]);
    }
    s = hwsense_superio_voltages(ctx);
    if (s.ok) {
        printf("\n--- Board voltages (%s) ---\n", s.chip_name);
        for (i = 0; i < s.voltage_count; i++)
            printf("  in%d: %.3f V\n", i, s.voltages[i]);
    }
    if (!hwsense_superio_fans(ctx).ok)
        printf("\nFans: %s\n", hwsense_superio_fans(ctx).error);

    {
        hwsense_nvme_smart_t n;
        if (hwsense_nvme_smart(0, &n)) {
            printf("\n--- NVMe SMART (nvme0) ---\n"
                   "  Composite %.1f C, %d extra sensor(s), critical warning 0x%02x\n"
                   "  Spare %d%% (threshold %d%%), %d%% used\n"
                   "  Written %.2f TB, read %.2f TB\n"
                   "  %llu power cycles, %llu hours, %llu unsafe shutdowns, %llu media errors\n",
                   n.composite_celsius, n.sensor_count, n.critical_warning,
                   n.available_spare_pct, n.available_spare_threshold_pct, n.percentage_used,
                   n.data_units_written * 512000.0 / 1e12, n.data_units_read * 512000.0 / 1e12,
                   n.power_cycles, n.power_on_hours, n.unsafe_shutdowns, n.media_errors);
        } else {
            printf("\nNVMe SMART: %s\n", n.error);
        }
    }

    hwsense_shutdown(ctx);
    return 0;
}
