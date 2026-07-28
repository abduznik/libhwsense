/*
 * intel.c — Intel CPU sensors via MSRs.
 *
 * Temperature: MSR 0x1A2 (TjMax) + MSR 0x19C (thermal status)
 * Clocks:      MSR 0x198 (IA32_PERF_STATUS) — current frequency
 * Power:       MSR 0x610 (PKG_ENERGY_STATUS) — RAPL package power
 */

#include "../core/hwsense_internal.h"
#include "../core/ioctl_codes.h"
#include <stdio.h>

/* Intel MSR addresses */
#define MSR_IA32_PERF_STATUS      0x198
#define MSR_PKG_ENERGY_STATUS     0x610
#define MSR_RAPL_POWER_UNIT       0x606

/*
 * Read an MSR via WinRing0 DeviceIoControl.
 */
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

/*
 * Pin thread to core 0 and read MSR.
 * Returns 1 on success, 0 on failure.
 */
static BOOL read_msr_on_core0(HANDLE dev, DWORD msr_index, DWORD64 *out_value)
{
    HANDLE thread = GetCurrentThread();
    DWORD_PTR old_mask = SetThreadAffinityMask(thread, 1);
    if (!old_mask)
        return FALSE;

    BOOL ok = read_msr(dev, msr_index, out_value);
    SetThreadAffinityMask(thread, old_mask);
    return ok;
}

/*
 * Read Intel core temperature on core 0.
 */
hwsense_temp_result_t hwsense_intel_core_temp(HANDLE driver_handle)
{
    hwsense_temp_result_t r = {0};
    DWORD64 msr_val;
    DWORD eax, tj_max, digital_readout;
    int reading_valid;
    double temp;

    if (!read_msr_on_core0(driver_handle, MSR_IA32_TEMPERATURE_TARGET, &msr_val)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "RDMSR 0x%X failed (error %lu)", MSR_IA32_TEMPERATURE_TARGET, GetLastError());
        return r;
    }

    eax = (DWORD)(msr_val & 0xFFFFFFFF);
    tj_max = (eax >> 16) & 0xFF;

    if (tj_max < 50 || tj_max > 150)
        tj_max = 100;

    if (!read_msr_on_core0(driver_handle, MSR_IA32_THERM_STATUS, &msr_val)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "RDMSR 0x%X failed (error %lu)", MSR_IA32_THERM_STATUS, GetLastError());
        return r;
    }

    eax = (DWORD)(msr_val & 0xFFFFFFFF);
    reading_valid = (eax >> 31) & 1;
    digital_readout = (eax >> 16) & 0x7F;

    if (!reading_valid) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "Thermal reading not valid (bit 31 = 0)");
        return r;
    }

    temp = (double)tj_max - (double)digital_readout;

    r.ok = 1;
    r.celsius = temp;
    return r;
}

/*
 * Read Intel CPU core clock speed via MSR 0x198 (IA32_PERF_STATUS).
 *
 * EAX[15:8] = current bus ratio (multiplier)
 * EDX[15:0] = voltage ID (VID)
 *
 * Clock = multiplier * bus_freq (typically 100 MHz)
 *
 * Returns clock in MHz.
 */
int hwsense_intel_core_clock(HANDLE driver_handle)
{
    DWORD64 msr_val;

    if (!read_msr_on_core0(driver_handle, MSR_IA32_PERF_STATUS, &msr_val))
        return -1;

    DWORD eax = (DWORD)(msr_val & 0xFFFFFFFF);
    DWORD multiplier = (eax >> 8) & 0xFF;

    /* Bus frequency is typically 100 MHz for modern Intel */
    return (int)multiplier * 100;
}

/*
 * Read Intel CPU core voltage via MSR 0x198 (IA32_PERF_STATUS).
 *
 * EDX[15:0] = voltage ID (VID)
 * Voltage = VID / 8192.0  (in volts)
 *
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

/*
 * Read Intel RAPL package power via MSR 0x610 (PKG_ENERGY_STATUS).
 *
 * Requires time delta between two readings to compute power.
 * This function reads current energy and returns delta from last call.
 * First call returns 0 (no previous reading).
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

    /* Get current tick count in ms */
    DWORD now_tick = GetTickCount();

    if (!has_prev) {
        prev_energy = energy_raw;
        prev_tick = now_tick;
        has_prev = TRUE;
        return 0.0;
    }

    /* Calculate energy delta (handle wraparound) */
    DWORD energy_delta = energy_raw - prev_energy;
    DWORD time_delta_ms = now_tick - prev_tick;

    if (time_delta_ms == 0)
        return 0.0;

    /* Energy unit from MSR 0x606: bits [3:0] = energy unit multiplier
     * Energy (Joules) = energy_delta * (1 / 2^unit_shift)
     * Power (Watts) = Energy_Joules / time_seconds
     *
     * For most Intel CPUs: unit = 3 → 1/8 Joules per unit */
    int unit_shift = 3;  /* Default; could read from MSR 0x606 */
    double energy_joules = (double)energy_delta / (double)(1 << unit_shift);
    double time_seconds = (double)time_delta_ms / 1000.0;

    double power = energy_joules / time_seconds;

    prev_energy = energy_raw;
    prev_tick = now_tick;

    return power;
}
