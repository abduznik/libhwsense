#ifndef HWSENSE_H
#define HWSENSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <windows.h>

/* DLL Export/Import macros */
#ifdef HWSENSE_DLL_EXPORTS
#define HWSENSE_API __declspec(dllexport)
#else
#define HWSENSE_API __declspec(dllimport)
#endif

typedef struct {
    int    ok;        /* 1 = success, 0 = error */
    double celsius;   /* valid only when ok == 1 */
    char   error[256]; /* human-readable error when ok == 0 */
} hwsense_temp_result_t;

typedef struct {
    int    ok;        /* 1 = success, 0 = error */
    double volts;     /* voltage in V; valid only when ok == 1 */
    double amps;      /* current in A; valid only when ok == 1 */
    char   error[256]; /* human-readable error when ok == 0 */
} hwsense_voltage_result_t;

/* Maximum number of CCD (Core Complex Die) temperature sensors. */
#define HWSENSE_MAX_CCD 4

typedef struct {
    int    count;                         /* number of valid CCD entries */
    double celsius[HWSENSE_MAX_CCD];     /* CCD temperature in °C */
    int    available[HWSENSE_MAX_CCD];   /* 1 if sensor returned valid data */
} hwsense_ccd_temps_t;

/* Opaque context — holds the WinRing0 driver handle and SCM/service handles. */
typedef struct hwsense_ctx hwsense_ctx_t;

/*
 * Install and start the WinRing0 kernel driver service, open a device handle.
 * Returns NULL on failure (prints error to stderr).
 *
 * Searches for WinRing0x64.sys next to the executable, then in CWD.
 * Requires Administrator privileges.
 */
HWSENSE_API hwsense_ctx_t *hwsense_init(void);

/*
 * Stop the driver service, remove the service registration, close handles.
 * Safe to call with NULL (no-op).
 */
HWSENSE_API void hwsense_shutdown(hwsense_ctx_t *ctx);

/*
 * Read the CPU package temperature.
 *   AMD:   Tctl via SMN register 0x00059800 through PCI config space
 *
 * Dispatches by CPU vendor detected from the registry.
 */
HWSENSE_API hwsense_temp_result_t hwsense_cpu_package_temp(hwsense_ctx_t *ctx);

/*
 * Read AMD CCD (Core Complex Die) temperatures via SMN registers.
 *
 * SMN 0x00059954 + i*4 for CCD i (documented for Matisse/Family 17h Model 0x71).
 * May or may not work on Renoir (Model 0x60) — returns available[] = 0 if
 * the sensor is not present or returns invalid data.
 */
HWSENSE_API hwsense_ccd_temps_t hwsense_amd_ccd_temps(hwsense_ctx_t *ctx);

/*
 * Read AMD core voltage (VDDCR_CPU) via SVI2 Plane0 register.
 * SMN 0x0005A010 bits [24:16] = SVI2 VID, encoding: voltage = VID * 0.00625 V
 *
 * Returns volts (V) and amps (A) if available, or error.
 * Dispatches by CPU vendor: AMD uses SVI2, Intel will use MSR 0x198.
 */
HWSENSE_API hwsense_voltage_result_t hwsense_cpu_core_voltage(hwsense_ctx_t *ctx);

/*
 * AMD SoC voltage (VDDCR_SOC) — vendor-dispatched wrapper.
 * Reads SVI2 Plane1 (SMN 0x0005A00C) on AMD, returns error on Intel.
 */
HWSENSE_API hwsense_voltage_result_t hwsense_amd_soc_voltage_dispatch(hwsense_ctx_t *ctx);

/*
 * CPU package power -- vendor-dispatched wrapper.
 * AMD: P = V_core * I_core + V_soc * I_soc via SVI2 telemetry.
 * Returns watts in the volts field, total amps in the amps field.
 */
HWSENSE_API hwsense_voltage_result_t hwsense_cpu_package_power(hwsense_ctx_t *ctx);

/*
 * Diagnostic: read raw SMN register value.
 * For debugging voltage/power telemetry.
 * Returns 1 on success, 0 on failure.
 */
HWSENSE_API int hwsense_read_smn_diag(hwsense_ctx_t *ctx, unsigned int smn_addr, unsigned int *out_value);

/*
 * Diagnostic: get SMU info (version, PM table version, DRAM base).
 * Returns 1 on success, fills output parameters.
 */
HWSENSE_API int hwsense_smu_diag(hwsense_ctx_t *ctx,
                     char *out_name, int name_len,
                     unsigned int *out_smu_ver, unsigned int *out_pm_ver,
                     unsigned long long *out_dram_base);

/*
 * CPU core clock speed (MHz).
 * Intel: MSR 0x198, AMD: returns -1 (not yet implemented).
 */
HWSENSE_API int hwsense_cpu_core_clock(hwsense_ctx_t *ctx);

/*
 * CPU core voltage.
 * Intel: MSR 0x198 VID/8192.0, AMD: SVI2 Plane0.
 * Returns volts, or -1.0 on failure.
 */
HWSENSE_API double hwsense_cpu_core_voltage_value(hwsense_ctx_t *ctx);

/*
 * CPU package power (Watts).
 * Intel: RAPL MSR 0x610 energy delta, AMD: SVI2 V*I estimate.
 * Returns watts, or -1.0 on failure.
 */
HWSENSE_API double hwsense_cpu_package_power_watts(hwsense_ctx_t *ctx);

/*
 * Maximum number of Super I/O temperature sensors.
 */
#define HWSENSE_MAX_SUPERIO_TEMPS 8
#define HWSENSE_MAX_SUPERIO_FANS 7
#define HWSENSE_MAX_SUPERIO_VOLTAGES 16

/*
 * Super I/O sensor reading result.
 */
typedef struct {
    int ok;
    int temperatures[HWSENSE_MAX_SUPERIO_TEMPS];
    int fan_rpms[HWSENSE_MAX_SUPERIO_FANS];
    double voltages[HWSENSE_MAX_SUPERIO_VOLTAGES];
    int count;
    int fan_count;
    int voltage_count;
    unsigned short chip_id;
    char chip_name[64];
    char error[256];
} hwsense_superio_result_t;

/*
 * Read Super I/O motherboard temperatures.
 */
HWSENSE_API hwsense_superio_result_t hwsense_superio_temps(hwsense_ctx_t *ctx);

/*
 * Read Super I/O fan speeds.
 */
HWSENSE_API hwsense_superio_result_t hwsense_superio_fans(hwsense_ctx_t *ctx);

/*
 * Read Super I/O voltages.
 */
HWSENSE_API hwsense_superio_result_t hwsense_superio_voltages(hwsense_ctx_t *ctx);

/*
 * Get the driver handle from context (for EC access).
 */
HWSENSE_API HANDLE hwsense_get_driver_handle(hwsense_ctx_t *ctx);

/*
 * GPU temperature reading result.
 */
typedef struct {
    int ok;
    int temperature;
    char name[128];
    char error[256];
} hwsense_gpu_result_t;

/*
 * Comprehensive GPU information.
 */
typedef struct {
    int ok;
    int temperature;
    int vram_used_mb;
    int vram_total_mb;
    int power_usage_w;
    int power_limit_w;
    int gpu_load;
    int mem_load;
    int clock_mhz;
    int mem_clock_mhz;
    char name[128];
    char error[256];
} hwsense_gpu_info_t;

/*
 * Read GPU temperature.
 * Tries NVIDIA NVML first, then AMD ADL.
 * gpu_index: which GPU to read (0 = first).
 */
HWSENSE_API hwsense_gpu_result_t hwsense_gpu_temperature(int gpu_index);

/*
 * Read comprehensive GPU information.
 * Returns temperature, VRAM, power, load, and clock speeds.
 */
HWSENSE_API hwsense_gpu_info_t hwsense_gpu_info(int gpu_index);

/*
 * NVMe/SSD temperature reading via SMART.
 * Returns temperature in Celsius, or -1 on failure.
 */
HWSENSE_API int hwsense_nvme_temperature(void);

/* ── Extended API (for advanced users) ──────────────────────────────── */

/*
 * Detect CPU vendor.
 * Returns 'I' for Intel, 'A' for AMD, '?' for unknown.
 */
HWSENSE_API int hwsense_detect_vendor(void);

/*
 * Intel RAPL power domains.
 */
HWSENSE_API double hwsense_intel_pp0_power(HANDLE dev);
HWSENSE_API double hwsense_intel_pp1_power(HANDLE dev);
HWSENSE_API double hwsense_intel_dram_power(HANDLE dev);

/*
 * CPU diagnostic detection.
 */
typedef struct {
    int ok;
    char vendor[16];
    char brand[64];
    int family;
    int model;
    int stepping;
    int ext_family;
    int ext_model;
    int cores;
    int threads;
    int max_threads_per_core;
    int tsc_invariant;
    int rapl_support;
    int hwp_support;
    int aperf_mperf;
    int msr_support;
    int thermal_monitor;
    int family_known;
    char supported_sensors[256];
    char warnings[512];
    char error[256];
} cpu_diag_result_t;

HWSENSE_API cpu_diag_result_t cpu_diag_detect(void);
HWSENSE_API void cpu_diag_print(cpu_diag_result_t *diag);

/* ── WMI API ───────────────────────────────────────────────────────── */

HWSENSE_API int wmi_init(void);
HWSENSE_API void wmi_shutdown(void);

typedef struct {
    char name[128];
    int max_clock_mhz;
    int current_clock_mhz;
    int voltage_mv;
    int load_percent;
    int temperature;
} wmi_cpu_info_t;

HWSENSE_API int wmi_read_cpu_info(wmi_cpu_info_t *info);

/* ── System Stats API ──────────────────────────────────────────────── */

typedef struct {
    double total_gb;
    double used_gb;
    double free_gb;
    double available_gb;
    double swap_used_gb;
    int percent_used;
} win_mem_stats_t;

typedef struct {
    double percent;
    int num_cpus;
} win_cpu_load_t;

typedef struct {
    char name[64];
    long long total_size_gb;
    long long free_gb;
    int percent_used;
} win_disk_stats_t;

typedef struct {
    char name[64];
    long long read_bytes;
    long long write_bytes;
} win_disk_io_t;

typedef struct {
    int days;
    int hours;
    int minutes;
    int seconds;
} win_uptime_t;

#define MAX_WIN_DISKS 26
#define MAX_WIN_DISK_IO 8

HWSENSE_API int win_get_mem_stats(win_mem_stats_t *mem);
HWSENSE_API int win_get_cpu_load_sampled(win_cpu_load_t *load);
HWSENSE_API int win_get_disk_stats(win_disk_stats_t *disks, int max_disks);
HWSENSE_API int win_get_disk_io(win_disk_io_t *disks, int max_disks);
HWSENSE_API win_uptime_t win_get_uptime(void);

/* ── Embedded Controller API ───────────────────────────────────────── */

typedef struct {
    int ok;
    int cpu_fan_rpm;
    int vrm_fan_rpm;
    int chipset_temp;
    int cpu_temp;
    int mb_temp;
    int vrm_temp;
    int cpu_voltage_mv;
    int cpu_current_amps;
    char error[256];
} ec_result_t;

HWSENSE_API ec_result_t ec_read_all_sensors(HANDLE dev);

#ifdef __cplusplus
}
#endif

#endif /* HWSENSE_H */
