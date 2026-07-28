#ifndef HWSENSE_H
#define HWSENSE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <windows.h>

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
hwsense_ctx_t *hwsense_init(void);

/*
 * Stop the driver service, remove the service registration, close handles.
 * Safe to call with NULL (no-op).
 */
void hwsense_shutdown(hwsense_ctx_t *ctx);

/*
 * Read the CPU package temperature.
 *   AMD:   Tctl via SMN register 0x00059800 through PCI config space
 *
 * Dispatches by CPU vendor detected from the registry.
 */
hwsense_temp_result_t hwsense_cpu_package_temp(hwsense_ctx_t *ctx);

/*
 * Read AMD CCD (Core Complex Die) temperatures via SMN registers.
 *
 * SMN 0x00059954 + i*4 for CCD i (documented for Matisse/Family 17h Model 0x71).
 * May or may not work on Renoir (Model 0x60) — returns available[] = 0 if
 * the sensor is not present or returns invalid data.
 */
hwsense_ccd_temps_t hwsense_amd_ccd_temps(hwsense_ctx_t *ctx);

/*
 * Read AMD core voltage (VDDCR_CPU) via SVI2 Plane0 register.
 * SMN 0x0005A010 bits [24:16] = SVI2 VID, encoding: voltage = VID * 0.00625 V
 *
 * Returns volts (V) and amps (A) if available, or error.
 * Dispatches by CPU vendor: AMD uses SVI2, Intel will use MSR 0x198.
 */
hwsense_voltage_result_t hwsense_cpu_core_voltage(hwsense_ctx_t *ctx);

/*
 * AMD SoC voltage (VDDCR_SOC) — vendor-dispatched wrapper.
 * Reads SVI2 Plane1 (SMN 0x0005A00C) on AMD, returns error on Intel.
 */
hwsense_voltage_result_t hwsense_amd_soc_voltage_dispatch(hwsense_ctx_t *ctx);

/*
 * CPU package power -- vendor-dispatched wrapper.
 * AMD: P = V_core * I_core + V_soc * I_soc via SVI2 telemetry.
 * Returns watts in the volts field, total amps in the amps field.
 */
hwsense_voltage_result_t hwsense_cpu_package_power(hwsense_ctx_t *ctx);

/*
 * Diagnostic: read raw SMN register value.
 * For debugging voltage/power telemetry.
 * Returns 1 on success, 0 on failure.
 */
int hwsense_read_smn_diag(hwsense_ctx_t *ctx, unsigned int smn_addr, unsigned int *out_value);

/*
 * Diagnostic: get SMU info (version, PM table version, DRAM base).
 * Returns 1 on success, fills output parameters.
 */
int hwsense_smu_diag(hwsense_ctx_t *ctx,
                     char *out_name, int name_len,
                     unsigned int *out_smu_ver, unsigned int *out_pm_ver,
                     unsigned long long *out_dram_base);

/*
 * CPU core clock speed (MHz).
 * Intel: MSR 0x198, AMD: returns -1 (not yet implemented).
 */
int hwsense_cpu_core_clock(hwsense_ctx_t *ctx);

/*
 * CPU core voltage.
 * Intel: MSR 0x198 VID/8192.0, AMD: SVI2 Plane0.
 * Returns volts, or -1.0 on failure.
 */
double hwsense_cpu_core_voltage_value(hwsense_ctx_t *ctx);

/*
 * CPU package power (Watts).
 * Intel: RAPL MSR 0x610 energy delta, AMD: SVI2 V*I estimate.
 * Returns watts, or -1.0 on failure.
 */
double hwsense_cpu_package_power_watts(hwsense_ctx_t *ctx);

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
hwsense_superio_result_t hwsense_superio_temps(hwsense_ctx_t *ctx);

/*
 * Read Super I/O fan speeds.
 */
hwsense_superio_result_t hwsense_superio_fans(hwsense_ctx_t *ctx);

/*
 * Read Super I/O voltages.
 */
hwsense_superio_result_t hwsense_superio_voltages(hwsense_ctx_t *ctx);

/*
 * Get the driver handle from context (for EC access).
 */
HANDLE hwsense_get_driver_handle(hwsense_ctx_t *ctx);

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
 * Read GPU temperature.
 * Tries NVIDIA NVML first, then AMD ADL.
 * gpu_index: which GPU to read (0 = first).
 */
hwsense_gpu_result_t hwsense_gpu_temperature(int gpu_index);

/*
 * NVMe/SSD temperature reading via SMART.
 * Returns temperature in Celsius, or -1 on failure.
 */
int hwsense_nvme_temperature(void);

#ifdef __cplusplus
}
#endif

#endif /* HWSENSE_H */
