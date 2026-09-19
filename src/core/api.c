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
 * Dispatch to Intel or AMD implementations based on CPU vendor.
 */
extern int   hwsense_detect_vendor(void);
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
extern int hwsense_intel_core_clock(HANDLE driver_handle);
extern int hwsense_intel_core_clock_on_core(HANDLE driver_handle, int core_id);
extern double hwsense_intel_core_voltage(HANDLE driver_handle);
extern double hwsense_intel_package_power(HANDLE driver_handle);
extern double hwsense_intel_pp0_power(HANDLE driver_handle);
extern double hwsense_intel_pp1_power(HANDLE driver_handle);
extern double hwsense_intel_dram_power(HANDLE driver_handle);
extern int hwsense_intel_all_core_temps(HANDLE driver_handle, double *temps, int max_cores);
extern int hwsense_amd_cpu_freq(HANDLE driver_handle);

/*
 * Describe why a reading is unavailable on this CPU.
 *
 * "Unknown CPU vendor" on its own leaves the caller with nothing to act on,
 * so pull the CPUID identity out of cpu_diag_detect() and say which part
 * we are actually looking at and what it does or does not report.
 *
 * `sensor` names what was being read, e.g. "Temperature".
 */
static void describe_unsupported_cpu(const char *sensor, char *out, size_t out_len)
{
    cpu_diag_result_t diag = cpu_diag_detect();

    if (diag.vendor[0] == '\0') {
        _snprintf_s(out, out_len, _TRUNCATE,
                    "%s unavailable: CPUID did not report a vendor string", sensor);
        return;
    }

    if (!diag.msr_support) {
        _snprintf_s(out, out_len, _TRUNCATE,
                    "%s unavailable: %s (family 0x%X model 0x%X) does not report "
                    "MSR support via CPUID",
                    sensor, diag.brand[0] ? diag.brand : diag.vendor,
                    diag.family, diag.model);
        return;
    }

    _snprintf_s(out, out_len, _TRUNCATE,
                "%s unavailable: %s (vendor %s, family 0x%X model 0x%X) is not in "
                "the supported table — register offsets are vendor and "
                "family specific, so reading it would return garbage",
                sensor, diag.brand[0] ? diag.brand : "unknown model",
                diag.vendor, diag.family, diag.model);
}

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
    describe_unsupported_cpu("Temperature", r.error, sizeof(r.error));
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
        /* MSR 0x198 (IA32_PERF_STATUS), EDX[15:0] = VID. Intel reports no
         * per-plane current, so amps stays 0 unlike the AMD SVI2 path. */
        double volts = hwsense_intel_core_voltage(ctx->driver_handle);
        if (volts < 0.0) {
            r.ok = 0;
            _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                        "RDMSR 0x198 (IA32_PERF_STATUS) failed (error %lu)",
                        GetLastError());
            return r;
        }
        r.ok = 1;
        r.volts = volts;
        return r;
    }

    r.ok = 0;
    describe_unsupported_cpu("Core voltage", r.error, sizeof(r.error));
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

    /* Not a gap in coverage: VDDCR_SOC is an AMD power plane read over
     * SVI2, and Intel parts have no equivalent rail to report. */
    {
        cpu_diag_result_t diag = cpu_diag_detect();
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SoC voltage unavailable: %s has no VDDCR_SOC rail "
                    "(SVI2 Plane1 is an AMD-only power plane)",
                    diag.brand[0] ? diag.brand : "this CPU");
    }
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

    if (vendor == 'I') {
        /* Intel reports package power through RAPL rather than SVI2
         * telemetry, so watts land in .volts and there is no current. */
        double watts = hwsense_intel_package_power(ctx->driver_handle);
        if (watts < 0.0) {
            r.ok = 0;
            _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                        "RAPL package energy read failed (MSR 0x606/0x610)");
            return r;
        }
        r.ok = 1;
        r.volts = watts;
        return r;
    }

    r.ok = 0;
    describe_unsupported_cpu("Package power", r.error, sizeof(r.error));
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

int hwsense_cpu_core_clock(hwsense_ctx_t *ctx)
{
    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE)
        return -1;

    int vendor = hwsense_detect_vendor();

    if (vendor == 'I')
        return hwsense_intel_core_clock(ctx->driver_handle);

    if (vendor == 'A')
        return hwsense_amd_cpu_freq(ctx->driver_handle);

    return -1;
}

double hwsense_cpu_core_voltage_value(hwsense_ctx_t *ctx)
{
    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE)
        return -1.0;

    int vendor = hwsense_detect_vendor();

    if (vendor == 'I')
        return hwsense_intel_core_voltage(ctx->driver_handle);

    /* AMD: use SVI2 */
    hwsense_voltage_result_t r = hwsense_amd_core_voltage(ctx->driver_handle);
    return r.ok ? r.volts : -1.0;
}

double hwsense_cpu_package_power_watts(hwsense_ctx_t *ctx)
{
    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE)
        return -1.0;

    int vendor = hwsense_detect_vendor();

    if (vendor == 'I')
        return hwsense_intel_package_power(ctx->driver_handle);

    /* AMD: use SVI2 V*I */
    hwsense_voltage_result_t r = hwsense_amd_package_power(ctx->driver_handle);
    return r.ok ? r.volts : -1.0;
}

HANDLE hwsense_get_driver_handle(hwsense_ctx_t *ctx)
{
    if (!ctx)
        return INVALID_HANDLE_VALUE;
    return ctx->driver_handle;
}
