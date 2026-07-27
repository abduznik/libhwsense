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
    double celsius[HWSENSE_MAX_CCD];     /* CCD temperature in C */
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
 *   Intel: Core 0 temp via MSR 0x19C/0x1A2
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
 * CPU package power — vendor-dispatched wrapper.
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
 * Diagnostic: get SMU info (version, PM table version).
 * Returns 1 on success, fills output parameters.
 */
int hwsense_smu_diag(hwsense_ctx_t *ctx,
                     char *out_name, int name_len,
                     unsigned int *out_smu_ver, unsigned int *out_pm_ver);

#ifdef __cplusplus
}
#endif

#endif /* HWSENSE_H */
