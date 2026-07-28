/*
 * hwsense_unified.c — Unified Hardware Sensor API implementation.
 *
 * Provides a unified interface that works transparently on both
 * AMD and Intel platforms.
 */

#include "hwsense_unified.h"
#include "hwsense_internal.h"
#include "../core/win_sysstats.h"
#include "../core/wmi.h"
#include <stdio.h>
#include <string.h>

/*
 * Initialize the unified sensor API.
 */
hwsense_ctx_t *hwsense_unified_init(void)
{
    /* Initialize WMI */
    wmi_init();

    /* Initialize the underlying library */
    return hwsense_init();
}

/*
 * Read all available sensors.
 */
int hwsense_read_all(hwsense_ctx_t *ctx, hwsense_sensor_data_t *data)
{
    if (!ctx || !data)
        return 0;

    memset(data, 0, sizeof(hwsense_sensor_data_t));

    /* Detect CPU vendor */
    int vendor = hwsense_detect_vendor();
    if (vendor == 'I')
        strncpy(data->cpu_vendor, "Intel", sizeof(data->cpu_vendor));
    else if (vendor == 'A')
        strncpy(data->cpu_vendor, "AMD", sizeof(data->cpu_vendor));
    else
        strncpy(data->cpu_vendor, "Unknown", sizeof(data->cpu_vendor));

    /* CPU Temperature */
    hwsense_temp_result_t temp = hwsense_cpu_package_temp(ctx);
    if (temp.ok)
        data->cpu_temp = temp.celsius;

    /* CPU Clock */
    int clock = hwsense_cpu_core_clock(ctx);
    if (clock > 0)
        data->cpu_clock_mhz = clock;

    /* CPU Voltage (try WMI first, then MSR) */
    wmi_cpu_info_t wmi_cpu;
    if (wmi_read_cpu_info(&wmi_cpu)) {
        strncpy(data->cpu_name, wmi_cpu.name, sizeof(data->cpu_name));
        data->cpu_clock_max_mhz = wmi_cpu.max_clock_mhz;
        if (wmi_cpu.voltage_mv > 0)
            data->cpu_voltage = wmi_cpu.voltage_mv / 1000.0;
    }

    /* CPU Load */
    win_cpu_load_t load;
    if (win_get_cpu_load_sampled(&load)) {
        data->cpu_load = load.percent;
        data->cpu_cores = load.num_cpus;
    }

    /* Memory */
    win_mem_stats_t mem;
    if (win_get_mem_stats(&mem)) {
        data->memory_used_gb = mem.used_gb;
        data->memory_total_gb = mem.total_gb;
        data->memory_percent = mem.percent_used;
    }

    /* Uptime */
    win_uptime_t uptime = win_get_uptime();
    data->uptime_days = uptime.days;
    data->uptime_hours = uptime.hours;

    /* Power (if Intel) */
    if (vendor == 'I') {
        HANDLE dev = ctx->driver_handle;
        data->cpu_power = hwsense_intel_package_power(dev);
        data->cpu_power_cores = hwsense_intel_pp0_power(dev);
        data->cpu_power_uncore = hwsense_intel_pp1_power(dev);
        data->dram_power = hwsense_intel_dram_power(dev);
    }

    /* Fan speeds from WMI */
    wmi_fan_info_t wmi_fans[8];
    data->fan_count = wmi_read_fans(wmi_fans, 8);
    for (int i = 0; i < data->fan_count && i < 8; i++) {
        data->fan_rpms[i] = wmi_fans[i].desired_speed;
    }

    return 1;
}

/*
 * Read CPU temperature.
 */
double hwsense_get_cpu_temp(hwsense_ctx_t *ctx)
{
    hwsense_temp_result_t temp = hwsense_cpu_package_temp(ctx);
    return temp.ok ? temp.celsius : -1.0;
}

/*
 * Read CPU clock speed.
 */
int hwsense_get_cpu_clock(hwsense_ctx_t *ctx)
{
    return hwsense_cpu_core_clock(ctx);
}

/*
 * Read CPU voltage.
 */
double hwsense_get_cpu_voltage(hwsense_ctx_t *ctx)
{
    wmi_cpu_info_t cpu;
    if (wmi_read_cpu_info(&cpu) && cpu.voltage_mv > 0)
        return cpu.voltage_mv / 1000.0;
    return -1.0;
}

/*
 * Read CPU package power.
 */
double hwsense_get_cpu_power(hwsense_ctx_t *ctx)
{
    return hwsense_cpu_package_power_watts(ctx);
}

/*
 * Read CPU load.
 */
double hwsense_get_cpu_load(hwsense_ctx_t *ctx)
{
    win_cpu_load_t load;
    if (win_get_cpu_load_sampled(&load))
        return load.percent;
    return -1.0;
}

/*
 * Read memory usage.
 */
double hwsense_get_memory_used(hwsense_ctx_t *ctx)
{
    win_mem_stats_t mem;
    if (win_get_mem_stats(&mem))
        return mem.used_gb;
    return -1.0;
}

/*
 * Print formatted sensor report.
 */
void hwsense_print_report(hwsense_ctx_t *ctx)
{
    hwsense_sensor_data_t data;
    hwsense_read_all(ctx, &data);

    printf("=== System Sensor Report ===\n\n");

    printf("--- CPU ---\n");
    printf("  Vendor:           %s\n", data.cpu_vendor);
    printf("  Name:             %s\n", data.cpu_name);
    printf("  Cores:            %d\n", data.cpu_cores);
    printf("  Temperature:      %.1f C\n", data.cpu_temp);
    printf("  Clock:            %d MHz\n", data.cpu_clock_mhz);
    printf("  Voltage:          %.3f V\n", data.cpu_voltage);
    printf("  Power:            %.2f W\n", data.cpu_power);
    printf("  Load:             %.1f%%\n", data.cpu_load);
    printf("\n");

    printf("--- Memory ---\n");
    printf("  Used:             %.1f / %.1f GB (%d%%)\n",
           data.memory_used_gb, data.memory_total_gb, data.memory_percent);
    printf("\n");

    printf("--- System ---\n");
    printf("  Uptime:           %d days, %d hours\n",
           data.uptime_days, data.uptime_hours);
    printf("\n");

    if (data.fan_count > 0) {
        printf("--- Fans ---\n");
        for (int i = 0; i < data.fan_count; i++)
            printf("  Fan %d:            %d RPM\n", i + 1, data.fan_rpms[i]);
        printf("\n");
    }
}
