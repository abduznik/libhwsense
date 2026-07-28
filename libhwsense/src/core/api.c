/*
 * api.c — Public API implementations for libhwsense.
 *
 * Vendor-dispatch functions: detect CPU vendor, call into AMD or Intel
 * implementations.  Diagnostic wrappers for SMU and SMN reads.
 */

#include "hwsense_internal.h"
#include "ioctl_codes.h"
#include <stdio.h>

/*
 * Dispatch to Intel or AMD temperature reading based on CPU vendor.
 * Declared in cpu/amd_zen.c and cpu/intel.c.
 */
extern int   hwsense_detect_vendor(void); /* returns 'I' for Intel, 'A' for AMD */
extern hwsense_temp_result_t hwsense_amd_package_temp(HANDLE driver_handle);
extern hwsense_temp_result_t hwsense_intel_core_temp(HANDLE driver_handle);
extern hwsense_voltage_result_t hwsense_amd_core_voltage(HANDLE driver_handle);
extern hwsense_voltage_result_t hwsense_amd_soc_voltage(HANDLE driver_handle);
extern hwsense_voltage_result_t hwsense_amd_package_power(HANDLE driver_handle);
extern int hwsense_amd_read_smn(HANDLE driver_handle, DWORD smn_addr, DWORD *out_value);
extern int hwsense_amd_smu_diag(HANDLE driver_handle,
                               char *out_name, int name_len,
                               DWORD *out_smu_ver, DWORD *out_pm_ver,
                               DWORD64 *out_dram_base);
extern float hwsense_amd_pmtable_power_raw(HANDLE driver_handle);

hwsense_temp_result_t hwsense_cpu_package_temp(hwsense_ctx_t *ctx)
{
    hwsense_temp_result_t r = {0};

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE, "Invalid context or driver handle");
        return r;
    }

    int vendor = hwsense_detect_vendor();

    if (vendor == 'I')
        return hwsense_intel_core_temp(ctx->driver_handle);

    if (vendor == 'A')
        return hwsense_amd_package_temp(ctx->driver_handle);

    r.ok = 0;
    _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                "Unknown CPU vendor (registry VendorIdentifier not Intel/AMD)");
    return r;
}

/*
 * Dispatch to Intel or AMD core voltage reading based on CPU vendor.
 * AMD: SVI2 Plane0 via SMN (core voltage)
 * Intel: MSR 0x198 IA32_PERF_STATUS (VID in EDX[15:0])
 */
hwsense_voltage_result_t hwsense_cpu_core_voltage(hwsense_ctx_t *ctx)
{
    hwsense_voltage_result_t r = {0};

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE, "Invalid context or driver handle");
        return r;
    }

    int vendor = hwsense_detect_vendor();

    if (vendor == 'A')
        return hwsense_amd_core_voltage(ctx->driver_handle);

    if (vendor == 'I') {
        /* Intel: read MSR 0x198 (IA32_PERF_STATUS), EDX[15:0] = VID */
        /* For now, return not-implemented — Intel voltage will be added later */
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "Intel core voltage not yet implemented (MSR 0x198)");
        return r;
    }

    r.ok = 0;
    _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                "Unknown CPU vendor");
    return r;
}

/*
 * AMD SoC voltage (VDDCR_SOC) via SVI2 Plane1.
 * Only available on AMD — returns error on Intel.
 */
hwsense_voltage_result_t hwsense_amd_soc_voltage_dispatch(hwsense_ctx_t *ctx)
{
    hwsense_voltage_result_t r = {0};

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE, "Invalid context or driver handle");
        return r;
    }

    int vendor = hwsense_detect_vendor();

    if (vendor == 'A')
        return hwsense_amd_soc_voltage(ctx->driver_handle);

    r.ok = 0;
    _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                "SoC voltage is AMD-only (SVI2 Plane1)");
    return r;
}

/*
 * AMD CPU package power via SVI2 telemetry.
 * P = V_core * I_core + V_soc * I_soc
 * Only available on AMD.
 */
hwsense_voltage_result_t hwsense_cpu_package_power(hwsense_ctx_t *ctx)
{
    hwsense_voltage_result_t r = {0};

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE, "Invalid context or driver handle");
        return r;
    }

    int vendor = hwsense_detect_vendor();

    if (vendor == 'A')
        return hwsense_amd_package_power(ctx->driver_handle);

    r.ok = 0;
    _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                "Package power via SVI2 is AMD-only");
    return r;
}

int hwsense_read_smn_diag(hwsense_ctx_t *ctx, unsigned int smn_addr, unsigned int *out_value)
{
    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE)
        return 0;
    return hwsense_amd_read_smn(ctx->driver_handle, (DWORD)smn_addr, (DWORD *)out_value);
}

int hwsense_smu_diag(hwsense_ctx_t *ctx,
                     char *out_name, int name_len,
                     unsigned int *out_smu_ver, unsigned int *out_pm_ver,
                     unsigned long long *out_dram_base)
{
    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE)
        return 0;
    return hwsense_amd_smu_diag(ctx->driver_handle, out_name, name_len,
                                (DWORD *)out_smu_ver, (DWORD *)out_pm_ver,
                                (DWORD64 *)out_dram_base);
}

float hwsense_amd_pmtable_power(hwsense_ctx_t *ctx)
{
    if (!ctx)
        return -1.0f;

    if (ctx->driver_handle && ctx->driver_handle != INVALID_HANDLE_VALUE)
        return hwsense_amd_pmtable_power_raw(ctx->driver_handle);

    return -1.0f;
}
