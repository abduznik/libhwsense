/*
 * hwsense_unified.h — Unified Hardware Sensor API.
 *
 * This header provides a unified interface that works transparently
 * on both AMD and Intel platforms. The API automatically detects the
 * CPU vendor and dispatches to the appropriate implementation.
 *
 * Usage:
 *   hwsense_ctx_t *ctx = hwsense_init();
 *   hwsense_sensor_data_t data;
 *   hwsense_read_all(ctx, &data);
 *   printf("CPU Temp: %.1f C\n", data.cpu_temp);
 *   printf("CPU Power: %.2f W\n", data.cpu_power);
 */

#ifndef HWSENSE_UNIFIED_H
#define HWSENSE_UNIFIED_H

#include "hwsense.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Unified sensor data structure.
 * Contains all available sensor readings.
 */
typedef struct {
    /* CPU Information */
    char cpu_vendor[16];      /* "Intel" or "AMD" */
    char cpu_name[128];       /* CPU model name */
    int cpu_cores;            /* Number of cores */
    int cpu_threads;          /* Number of threads */

    /* Temperature */
    double cpu_temp;          /* CPU package temperature (C) */
    double cpu_temp_min;      /* Minimum core temperature (C) */
    double cpu_temp_max;      /* Maximum core temperature (C) */
    double cpu_temp_avg;      /* Average core temperature (C) */
    double gpu_temp;          /* GPU temperature (C) */

    /* Clock Speed */
    int cpu_clock_mhz;        /* Current CPU clock (MHz) */
    int cpu_clock_max_mhz;    /* Maximum CPU clock (MHz) */

    /* Voltage */
    double cpu_voltage;       /* CPU core voltage (V) */
    double soc_voltage;       /* SoC/System Agent voltage (V) */

    /* Power */
    double cpu_power;         /* CPU package power (W) */
    double cpu_power_cores;   /* CPU cores power (PP0) (W) */
    double cpu_power_uncore;  /* Uncore/GPU power (PP1) (W) */
    double dram_power;        /* DRAM power (W) */

    /* Memory */
    double memory_used_gb;    /* Used memory (GB) */
    double memory_total_gb;   /* Total memory (GB) */
    int memory_percent;       /* Memory usage percentage */

    /* System */
    double cpu_load;          /* CPU load percentage */
    int uptime_days;          /* System uptime days */
    int uptime_hours;         /* System uptime hours */

    /* Fan Speeds */
    int fan_count;            /* Number of fans detected */
    int fan_rpms[8];          /* Fan speeds in RPM */

    /* Drive Information */
    int drive_count;          /* Number of drives */
    struct {
        char name[64];        /* Drive name */
        int temperature;      /* Drive temperature (C) */
        long long size_gb;    /* Drive size (GB) */
        long long used_gb;    /* Used space (GB) */
    } drives[8];

    /* Platform-specific data */
    void *platform_data;      /* Pointer to platform-specific data */
} hwsense_sensor_data_t;

/*
 * Initialize the unified sensor API.
 * Returns context handle, or NULL on failure.
 */
hwsense_ctx_t *hwsense_unified_init(void);

/*
 * Read all available sensors.
 * Fills the sensor_data structure with current values.
 */
int hwsense_read_all(hwsense_ctx_t *ctx, hwsense_sensor_data_t *data);

/*
 * Read CPU temperature.
 * Returns temperature in Celsius, or -1.0 on failure.
 */
double hwsense_get_cpu_temp(hwsense_ctx_t *ctx);

/*
 * Read CPU clock speed.
 * Returns clock in MHz, or -1 on failure.
 */
int hwsense_get_cpu_clock(hwsense_ctx_t *ctx);

/*
 * Read CPU voltage.
 * Returns voltage in volts, or -1.0 on failure.
 */
double hwsense_get_cpu_voltage(hwsense_ctx_t *ctx);

/*
 * Read CPU package power.
 * Returns power in watts, or -1.0 on failure.
 */
double hwsense_get_cpu_power(hwsense_ctx_t *ctx);

/*
 * Read CPU load.
 * Returns load percentage (0-100), or -1.0 on failure.
 */
double hwsense_get_cpu_load(hwsense_ctx_t *ctx);

/*
 * Read memory usage.
 * Returns used memory in GB, or -1.0 on failure.
 */
double hwsense_get_memory_used(hwsense_ctx_t *ctx);

/*
 * Print formatted sensor report.
 */
void hwsense_print_report(hwsense_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* HWSENSE_UNIFIED_H */
