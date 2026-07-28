/*
 * intel.c — Intel CPU sensors via MSRs.
 *
 * Complete Intel HAL implementation matching AMD feature parity:
 *   Temperature: MSR 0x1A2 (TjMax) + MSR 0x19C (thermal status)
 *   Per-core:    Pin thread to each core and read thermal status
 *   Clocks:      MSR 0x198 (IA32_PERF_STATUS) — current frequency
 *   Voltage:     MSR 0x198 (IA32_PERF_STATUS) — VID voltage
 *   Power:       MSR 0x610 (PKG_ENERGY_STATUS) — RAPL package power
 *   PP0 Power:   MSR 0x639 (PP0_ENERGY_STATUS) — CPU cores power
 *   PP1 Power:   MSR 0x641 (PP1_ENERGY_STATUS) — Uncore/GPU power
 *   DRAM Power:  MSR 0x619 (DRAM_ENERGY_STATUS) — DRAM power
 *   Power Unit:  MSR 0x606 (RAPL_POWER_UNIT) — energy unit conversion
 */

#include "../core/hwsense_internal.h"
#include "../core/ioctl_codes.h"
#include <stdio.h>

/* Intel MSR addresses */
#define MSR_IA32_TEMPERATURE_TARGET  0x1A2
#define MSR_IA32_THERM_STATUS        0x19C
#define MSR_IA32_PERF_STATUS         0x198
#define MSR_IA32_PERF_CTL            0x199
#define MSR_PKG_ENERGY_STATUS        0x610
#define MSR_PP0_ENERGY_STATUS        0x639
#define MSR_PP1_ENERGY_STATUS        0x641
#define MSR_DRAM_ENERGY_STATUS       0x619
#define MSR_RAPL_POWER_UNIT          0x606
#define MSR_RAPL_PKG_POWER_LIMIT     0x610

/* Read an MSR via WinRing0 DeviceIoControl */
static BOOL read_msr(HANDLE dev, DWORD msr_index, DWORD64 *out_value)
{
    DWORD in_val = msr_index;
    DWORD64 out_val = 0;
    DWORD bytes_ret = 0;

    BOOL ok = DeviceIoControl(
        dev, IOCTL_OLS_READ_MSR,
        &in_val, sizeof(in_val),
        &out_val, sizeof(out_val),
        &bytes_ret, NULL
    );

    if (!ok)
        return FALSE;

    *out_value = out_val;
    return TRUE;
}

/* Pin thread to specific core and read MSR */
static BOOL read_msr_on_core(HANDLE dev, DWORD msr_index, int core_id, DWORD64 *out_value)
{
    HANDLE thread = GetCurrentThread();
    DWORD_PTR old_mask = SetThreadAffinityMask(thread, (DWORD_PTR)(1 << core_id));
    if (!old_mask)
        return FALSE;

    BOOL ok = read_msr(dev, msr_index, out_value);
    SetThreadAffinityMask(thread, old_mask);
    return ok;
}

/* Pin thread to core 0 and read MSR */
static BOOL read_msr_on_core0(HANDLE dev, DWORD msr_index, DWORD64 *out_value)
{
    return read_msr_on_core(dev, msr_index, 0, out_value);
}

/* ── Temperature ──────────────────────────────────────────────────── */

/*
 * Read Intel core temperature on specific core.
 */
hwsense_temp_result_t hwsense_intel_core_temp_on_core(HANDLE driver_handle, int core_id)
{
    hwsense_temp_result_t r = {0};
    DWORD64 msr_val;
    DWORD eax, tj_max, digital_readout;
    int reading_valid;

    if (!read_msr_on_core(driver_handle, MSR_IA32_TEMPERATURE_TARGET, core_id, &msr_val)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "RDMSR 0x%X failed on core %d", MSR_IA32_TEMPERATURE_TARGET, core_id);
        return r;
    }

    eax = (DWORD)(msr_val & 0xFFFFFFFF);
    tj_max = (eax >> 16) & 0xFF;

    if (tj_max < 50 || tj_max > 150)
        tj_max = 100;

    if (!read_msr_on_core(driver_handle, MSR_IA32_THERM_STATUS, core_id, &msr_val)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "RDMSR 0x%X failed on core %d", MSR_IA32_THERM_STATUS, core_id);
        return r;
    }

    eax = (DWORD)(msr_val & 0xFFFFFFFF);
    reading_valid = (eax >> 31) & 1;
    digital_readout = (eax >> 16) & 0x7F;

    if (!reading_valid) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "Thermal reading not valid on core %d", core_id);
        return r;
    }

    r.ok = 1;
    r.celsius = (double)tj_max - (double)digital_readout;
    return r;
}

/* Read Intel core temperature on core 0 (legacy wrapper) */
hwsense_temp_result_t hwsense_intel_core_temp(HANDLE driver_handle)
{
    return hwsense_intel_core_temp_on_core(driver_handle, 0);
}

/*
 * Read all Intel per-core temperatures.
 * Returns number of cores with valid readings.
 */
int hwsense_intel_all_core_temps(HANDLE driver_handle, double *temps, int max_cores)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int num_cores = si.dwNumberOfProcessors;
    int count = 0;

    for (int i = 0; i < num_cores && i < max_cores; i++) {
        hwsense_temp_result_t r = hwsense_intel_core_temp_on_core(driver_handle, i);
        if (r.ok) {
            temps[i] = r.celsius;
            count++;
        } else {
            temps[i] = -1.0;
        }
    }

    return count;
}

/* ── Clock Speed ──────────────────────────────────────────────────── */

/*
 * Read Intel CPU core clock speed via MSR 0x198 (IA32_PERF_STATUS).
 * Returns clock in MHz.
 */
int hwsense_intel_core_clock(HANDLE driver_handle)
{
    DWORD64 msr_val;

    if (!read_msr_on_core0(driver_handle, MSR_IA32_PERF_STATUS, &msr_val))
        return -1;

    DWORD eax = (DWORD)(msr_val & 0xFFFFFFFF);
    DWORD multiplier = (eax >> 8) & 0xFF;

    return (int)multiplier * 100;
}

/*
 * Read Intel CPU core clock on specific core.
 */
int hwsense_intel_core_clock_on_core(HANDLE driver_handle, int core_id)
{
    DWORD64 msr_val;

    if (!read_msr_on_core(driver_handle, MSR_IA32_PERF_STATUS, core_id, &msr_val))
        return -1;

    DWORD eax = (DWORD)(msr_val & 0xFFFFFFFF);
    DWORD multiplier = (eax >> 8) & 0xFF;

    return (int)multiplier * 100;
}

/* ── Voltage ──────────────────────────────────────────────────────── */

/*
 * Read Intel CPU core voltage via MSR 0x198 (IA32_PERF_STATUS).
 * Returns voltage in volts, or -1.0 on failure.
 */
double hwsense_intel_core_voltage(HANDLE driver_handle)
{
    DWORD64 msr_val;

    if (!read_msr_on_core0(driver_handle, MSR_IA32_PERF_STATUS, &msr_val))
        return -1.0;

    DWORD edx = (DWORD)((msr_val >> 32) & 0xFFFFFFFF);
    DWORD vid = edx & 0xFFFF;

    return (double)vid / 8192.0;
}

/* ── RAPL Power ───────────────────────────────────────────────────── */

/*
 * Read RAPL power unit from MSR 0x606.
 * Returns energy unit in Joules per unit.
 */
static double get_rapl_energy_unit(HANDLE driver_handle)
{
    DWORD64 msr_val;

    if (!read_msr_on_core0(driver_handle, MSR_RAPL_POWER_UNIT, &msr_val))
        return 1.0 / 8.0;  /* Default: 1/8 Joules */

    DWORD eax = (DWORD)(msr_val & 0xFFFFFFFF);
    int unit_shift = eax & 0x1F;

    return 1.0 / (double)(1 << unit_shift);
}

/*
 * Read Intel RAPL package power via MSR 0x610.
 * Returns power in watts.
 */
double hwsense_intel_package_power(HANDLE driver_handle)
{
    static BOOL has_prev = FALSE;
    static DWORD prev_energy = 0;
    static DWORD prev_tick = 0;

    DWORD64 msr_val;

    if (!read_msr_on_core0(driver_handle, MSR_PKG_ENERGY_STATUS, &msr_val))
        return -1.0;

    DWORD energy_raw = (DWORD)(msr_val & 0xFFFFFFFF);
    DWORD now_tick = GetTickCount();

    if (!has_prev) {
        prev_energy = energy_raw;
        prev_tick = now_tick;
        has_prev = TRUE;
        return 0.0;
    }

    DWORD energy_delta = energy_raw - prev_energy;
    DWORD time_delta_ms = now_tick - prev_tick;

    if (time_delta_ms == 0)
        return 0.0;

    double energy_unit = get_rapl_energy_unit(driver_handle);
    double energy_joules = (double)energy_delta * energy_unit;
    double time_seconds = (double)time_delta_ms / 1000.0;

    prev_energy = energy_raw;
    prev_tick = now_tick;

    return energy_joules / time_seconds;
}

/*
 * Read Intel PP0 (CPU cores) power via MSR 0x639.
 */
double hwsense_intel_pp0_power(HANDLE driver_handle)
{
    static BOOL has_prev = FALSE;
    static DWORD prev_energy = 0;
    static DWORD prev_tick = 0;

    DWORD64 msr_val;

    if (!read_msr_on_core0(driver_handle, MSR_PP0_ENERGY_STATUS, &msr_val))
        return -1.0;

    DWORD energy_raw = (DWORD)(msr_val & 0xFFFFFFFF);
    DWORD now_tick = GetTickCount();

    if (!has_prev) {
        prev_energy = energy_raw;
        prev_tick = now_tick;
        has_prev = TRUE;
        return 0.0;
    }

    DWORD energy_delta = energy_raw - prev_energy;
    DWORD time_delta_ms = now_tick - prev_tick;

    if (time_delta_ms == 0)
        return 0.0;

    double energy_unit = get_rapl_energy_unit(driver_handle);
    double energy_joules = (double)energy_delta * energy_unit;
    double time_seconds = (double)time_delta_ms / 1000.0;

    prev_energy = energy_raw;
    prev_tick = now_tick;

    return energy_joules / time_seconds;
}

/*
 * Read Intel PP1 (Uncore/GPU) power via MSR 0x641.
 */
double hwsense_intel_pp1_power(HANDLE driver_handle)
{
    static BOOL has_prev = FALSE;
    static DWORD prev_energy = 0;
    static DWORD prev_tick = 0;

    DWORD64 msr_val;

    if (!read_msr_on_core0(driver_handle, MSR_PP1_ENERGY_STATUS, &msr_val))
        return -1.0;

    DWORD energy_raw = (DWORD)(msr_val & 0xFFFFFFFF);
    DWORD now_tick = GetTickCount();

    if (!has_prev) {
        prev_energy = energy_raw;
        prev_tick = now_tick;
        has_prev = TRUE;
        return 0.0;
    }

    DWORD energy_delta = energy_raw - prev_energy;
    DWORD time_delta_ms = now_tick - prev_tick;

    if (time_delta_ms == 0)
        return 0.0;

    double energy_unit = get_rapl_energy_unit(driver_handle);
    double energy_joules = (double)energy_delta * energy_unit;
    double time_seconds = (double)time_delta_ms / 1000.0;

    prev_energy = energy_raw;
    prev_tick = now_tick;

    return energy_joules / time_seconds;
}

/*
 * Read Intel DRAM power via MSR 0x619.
 */
double hwsense_intel_dram_power(HANDLE driver_handle)
{
    static BOOL has_prev = FALSE;
    static DWORD prev_energy = 0;
    static DWORD prev_tick = 0;

    DWORD64 msr_val;

    if (!read_msr_on_core0(driver_handle, MSR_DRAM_ENERGY_STATUS, &msr_val))
        return -1.0;

    DWORD energy_raw = (DWORD)(msr_val & 0xFFFFFFFF);
    DWORD now_tick = GetTickCount();

    if (!has_prev) {
        prev_energy = energy_raw;
        prev_tick = now_tick;
        has_prev = TRUE;
        return 0.0;
    }

    DWORD energy_delta = energy_raw - prev_energy;
    DWORD time_delta_ms = now_tick - prev_tick;

    if (time_delta_ms == 0)
        return 0.0;

    double energy_unit = get_rapl_energy_unit(driver_handle);
    double energy_joules = (double)energy_delta * energy_unit;
    double time_seconds = (double)time_delta_ms / 1000.0;

    prev_energy = energy_raw;
    prev_tick = now_tick;

    return energy_joules / time_seconds;
}
