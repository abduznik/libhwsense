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

/* ── Threshold alerts ──────────────────────────────────────────────── */

/* Alert states. */
#define HWSENSE_ALERT_OK        0
#define HWSENSE_ALERT_BREACHED  1

/* Events returned by hwsense_alert_step / hwsense_check_temp_alert. */
#define HWSENSE_ALERT_EVENT_NONE       0
#define HWSENSE_ALERT_EVENT_BREACH     1
#define HWSENSE_ALERT_EVENT_RECOVERED  2

/*
 * Tracks one threshold over successive readings.
 *
 * Alerts are edge-triggered: a breach fires once when the reading crosses
 * the threshold, not on every poll while it stays above. Recovery requires
 * the reading to fall to (threshold - hysteresis) or below, so a value
 * hovering at the threshold does not flap between states on sensor noise.
 *
 * Treat the fields as read-only; use hwsense_alert_init to set one up.
 */
typedef struct {
    double threshold;   /* breach when a reading goes above this */
    double hysteresis;  /* margin below the threshold required to recover */
    double last_value;  /* most recent reading passed in */
    int    state;       /* HWSENSE_ALERT_OK or HWSENSE_ALERT_BREACHED */
    int    breach_count;/* number of breaches since init */
} hwsense_alert_t;

/*
 * Called when an alert changes state.
 *   event     — HWSENSE_ALERT_EVENT_BREACH or _RECOVERED
 *   value     — the reading that triggered it
 *   user_data — passed through untouched
 */
typedef void (*hwsense_alert_fn)(int event, double value, void *user_data);

/*
 * Set up an alert. A hysteresis of 0 means recovery happens as soon as the
 * reading is back at or below the threshold; a negative value is treated
 * as 0.
 */
HWSENSE_API void hwsense_alert_init(hwsense_alert_t *alert,
                                    double threshold,
                                    double hysteresis);

/*
 * Feed one reading in and return the resulting event (usually
 * HWSENSE_ALERT_EVENT_NONE). Touches no hardware, so it can be driven
 * from readings obtained any way the caller likes.
 */
HWSENSE_API int hwsense_alert_step(hwsense_alert_t *alert, double value);

/* Current state: HWSENSE_ALERT_OK or HWSENSE_ALERT_BREACHED. */
HWSENSE_API int hwsense_alert_state(const hwsense_alert_t *alert);

/*
 * Read the CPU temperature once and feed it to the alert, invoking the
 * callback if the state changed. Returns the event.
 *
 * This is a single step, not a loop — the caller decides the polling
 * interval and which thread it runs on. A failed sensor read leaves the
 * alert state untouched rather than counting as a recovery.
 */
HWSENSE_API int hwsense_check_temp_alert(hwsense_ctx_t *ctx,
                                         hwsense_alert_t *alert,
                                         hwsense_alert_fn callback,
                                         void *user_data);

/*
 * Serialize a sensor snapshot to JSON.
 *
 * Writes into buf and returns the number of characters the full output
 * needs, excluding the terminator — the same contract as snprintf. A
 * return value >= buf_size means the output was truncated; call again
 * with a buffer of at least (return value + 1) bytes. Passing buf_size
 * of 0 (with any buf) measures the required size without writing.
 *
 * Returns -1 if data is NULL, buf_size is negative, or buf is NULL with
 * a nonzero buf_size.
 */
HWSENSE_API int hwsense_export_json(const hwsense_sensor_data_t *data,
                                    char *buf, int buf_size);

/*
 * Write the CSV column header. Same buffer contract as the JSON writer.
 * The column order matches hwsense_export_csv_row.
 */
HWSENSE_API int hwsense_export_csv_header(char *buf, int buf_size);

/*
 * Serialize a sensor snapshot as one CSV row, matching the column order
 * of hwsense_export_csv_header. Same buffer contract as above.
 */
HWSENSE_API int hwsense_export_csv_row(const hwsense_sensor_data_t *data,
                                       char *buf, int buf_size);

#ifdef __cplusplus
}
#endif

#endif /* HWSENSE_UNIFIED_H */
