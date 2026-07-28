/*
 * amd_svi2.c — AMD SVI2 voltage and power telemetry.
 *
 * Reads SVI2 Plane0 (core voltage) and Plane1 (SoC voltage) via SMN.
 * Computes package power from V*I estimates.
 */

#include "../core/hwsense_internal.h"
#include "../core/ioctl_codes.h"
#include <stdio.h>

extern BOOL smn_read(HANDLE dev, DWORD smn_addr, DWORD *out_val);

/*
 * Read AMD core voltage (VDDCR_CPU) via SVI2 Plane0 register.
 *
 * SMN 0x0005A010 bit layout:
 *   [24:16]  — SVI2_VID: 9-bit voltage ID
 *              Encoding: voltage = VID * 0.00625 V
 *   [7:0]    — SVI2_TEF: current telemetry (amps)
 */
hwsense_voltage_result_t hwsense_amd_core_voltage(HANDLE driver_handle)
{
    hwsense_voltage_result_t r = {0};
    DWORD raw = 0;

    if (!smn_read(driver_handle, AMD_SVI2_PLANE0, &raw)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SMN read of SVI2 Plane0 (0x%08X) failed (error %lu)",
                    AMD_SVI2_PLANE0, GetLastError());
        return r;
    }

    DWORD vid = (raw >> 16) & 0x1FF;

    if (vid == 0x1FF) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SVI2 Plane0 VID = 0x1FF (plane off / not present)");
        return r;
    }

    DWORD current_raw = raw & 0xFF;

    r.ok = 1;
    r.volts = AMD_SVI2_VID_TO_V(vid);
    r.amps = (double)current_raw;
    return r;
}

/*
 * Read AMD SoC voltage (VDDCR_SOC) via SVI2 Plane1 register.
 *
 * SMN 0x0005A00C — same bit layout as Plane0.
 */
hwsense_voltage_result_t hwsense_amd_soc_voltage(HANDLE driver_handle)
{
    hwsense_voltage_result_t r = {0};
    DWORD raw = 0;

    if (!smn_read(driver_handle, AMD_SVI2_PLANE1, &raw)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SMN read of SVI2 Plane1 (0x%08X) failed (error %lu)",
                    AMD_SVI2_PLANE1, GetLastError());
        return r;
    }

    DWORD vid = (raw >> 16) & 0x1FF;

    if (vid == 0x1FF) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SVI2 Plane1 VID = 0x1FF (plane off / not present)");
        return r;
    }

    DWORD current_raw = raw & 0xFF;

    r.ok = 1;
    r.volts = AMD_SVI2_VID_TO_V(vid);
    r.amps = (double)current_raw;
    return r;
}

/*
 * Package power via SVI2 telemetry:
 * P = V_core * I_core + V_soc * I_soc
 */
hwsense_voltage_result_t hwsense_amd_package_power(HANDLE driver_handle)
{
    hwsense_voltage_result_t r = {0};
    DWORD raw0 = 0, raw1 = 0;

    if (!smn_read(driver_handle, AMD_SVI2_PLANE0, &raw0)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SMN read of SVI2 Plane0 failed (error %lu)", GetLastError());
        return r;
    }

    if (!smn_read(driver_handle, AMD_SVI2_PLANE1, &raw1)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SMN read of SVI2 Plane1 failed (error %lu)", GetLastError());
        return r;
    }

    DWORD vid_core = (raw0 >> 16) & 0x1FF;
    DWORD cur_core = raw0 & 0xFF;
    DWORD vid_soc  = (raw1 >> 16) & 0x1FF;
    DWORD cur_soc  = raw1 & 0xFF;

    double v_core = (vid_core != 0x1FF) ? AMD_SVI2_VID_TO_V(vid_core) : 0.0;
    double v_soc  = (vid_soc  != 0x1FF) ? AMD_SVI2_VID_TO_V(vid_soc)  : 0.0;

    double power = (v_core * (double)cur_core) + (v_soc * (double)cur_soc);

    r.ok = 1;
    r.volts = power;
    r.amps = (double)cur_core + (double)cur_soc;
    return r;
}
