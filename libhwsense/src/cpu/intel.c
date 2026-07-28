/*
 * intel.c — Intel CPU per-core temperature reading via MSRs.
 *
 * Reads:
 *   MSR 0x1A2 (IA32_TEMPERATURE_TARGET) → TjMax from bits [23:16]
 *   MSR 0x19C (IA32_THERM_STATUS)       → digital readout from bits [22:16],
 *                                          valid flag at bit 31
 *
 * Temperature = TjMax - digital_readout
 *
 * Each MSR read is pinned to a specific logical core via
 * SetThreadAffinityMask, matching OpenHardwareMonitor's RdmsrTx pattern.
 */

#include "../core/hwsense_internal.h"
#include "../core/ioctl_codes.h"
#include <stdio.h>

/*
 * Read an MSR via WinRing0 DeviceIoControl.
 * Must be called AFTER pinning the thread to the target core.
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
 * Read Intel core temperature on core 0.
 *
 * Steps:
 *   1. Pin thread to core 0 via SetThreadAffinityMask
 *   2. RDMSR 0x1A2 → TjMax (bits [23:16])
 *   3. RDMSR 0x19C → digital readout (bits [22:16]), valid (bit 31)
 *   4. Temperature = TjMax - digital_readout
 *   5. Restore thread affinity
 *
 * Returns core 0 temperature as the "package" representative.
 */
hwsense_temp_result_t hwsense_intel_core_temp(HANDLE driver_handle)
{
    hwsense_temp_result_t r = {0};
    HANDLE thread;
    DWORD_PTR old_mask;
    DWORD64 msr_val;
    DWORD eax;
    DWORD tj_max;
    DWORD digital_readout;
    int reading_valid;
    double temp;

    thread = GetCurrentThread();
    old_mask = SetThreadAffinityMask(thread, 1); /* pin to core 0 */
    if (!old_mask) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SetThreadAffinityMask failed (error %lu)", GetLastError());
        return r;
    }

    /* Step 1: Read TjMax from IA32_TEMPERATURE_TARGET (MSR 0x1A2) */
    if (!read_msr(driver_handle, MSR_IA32_TEMPERATURE_TARGET, &msr_val)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "RDMSR 0x%X failed (error %lu)", MSR_IA32_TEMPERATURE_TARGET, GetLastError());
        SetThreadAffinityMask(thread, old_mask);
        return r;
    }

    eax = (DWORD)(msr_val & 0xFFFFFFFF);
    tj_max = (eax >> 16) & 0xFF;

    /* Sanity check */
    if (tj_max < 50 || tj_max > 150)
        tj_max = 100;

    /* Step 2: Read thermal status from IA32_THERM_STATUS (MSR 0x19C) */
    if (!read_msr(driver_handle, MSR_IA32_THERM_STATUS, &msr_val)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "RDMSR 0x%X failed (error %lu)", MSR_IA32_THERM_STATUS, GetLastError());
        SetThreadAffinityMask(thread, old_mask);
        return r;
    }

    SetThreadAffinityMask(thread, old_mask);

    eax = (DWORD)(msr_val & 0xFFFFFFFF);
    reading_valid = (eax >> 31) & 1;
    digital_readout = (eax >> 16) & 0x7F;

    if (!reading_valid) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "Thermal reading not valid (bit 31 = 0)");
        return r;
    }

    /* Step 3: Temperature = TjMax - digital_readout */
    temp = (double)tj_max - (double)digital_readout;

    r.ok = 1;
    r.celsius = temp;
    return r;
}
