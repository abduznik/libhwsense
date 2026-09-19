/*
 * sensor_math.h — Pure register-decode math, free of any OS or driver calls.
 *
 * These are the formulas that turn a raw register value into a physical
 * reading. They are kept separate from the read paths so they can be
 * unit-tested without hardware (see tests/test_sensor_math.c), and so the
 * Windows and Linux backends decode identically.
 *
 * Sources for each formula are cited on the function.
 */

#ifndef HWSENSE_SENSOR_MATH_H
#define HWSENSE_SENSOR_MATH_H

#include <stdint.h>

/*
 * Intel: temperature from IA32_TEMPERATURE_TARGET (MSR 0x1A2) and
 * IA32_THERM_STATUS (MSR 0x19C). Intel SDM Vol. 4.
 *
 *   MSR 0x1A2 bits [23:16] = TjMax
 *   MSR 0x19C bit 31       = reading valid
 *   MSR 0x19C bits [22:16] = digital readout (degrees below TjMax)
 */

/* TjMax in C. Falls back to 100 when the register reports an implausible
 * value, which some mobile parts do. */
static int hwsense_intel_tjmax(uint32_t temp_target_eax)
{
    int tj_max = (int)((temp_target_eax >> 16) & 0xFF);
    if (tj_max < 50 || tj_max > 150)
        return 100;
    return tj_max;
}

static int hwsense_intel_therm_valid(uint32_t therm_status_eax)
{
    return (int)((therm_status_eax >> 31) & 1);
}

static double hwsense_intel_temp_c(uint32_t temp_target_eax, uint32_t therm_status_eax)
{
    int tj_max = hwsense_intel_tjmax(temp_target_eax);
    int readout = (int)((therm_status_eax >> 16) & 0x7F);
    return (double)tj_max - (double)readout;
}

/*
 * AMD: Tctl from SMN 0x00059800 (F17H_M01H_THM_TCON_CUR_TMP).
 * Formula per LibreHardwareMonitor.
 *
 *   bits [31:21] = CUR_TEMP in 0.125 C units
 *   bit  [19]    = RANGE_SEL      -> -49 C offset
 *   bits [17:16] = TJ_SEL == 0b11 -> -49 C offset
 */
static double hwsense_amd_tctl_c(uint32_t raw)
{
    double temp = ((double)(raw >> 21) * 125.0) / 1000.0;
    if ((raw & 0x80000) != 0 || (raw & 0x30000) == 0x30000)
        temp -= 49.0;
    return temp;
}

/*
 * AMD: CCD die temperature from SMN 0x00059954 + i*4.
 *
 *   bits [11:0] = 12-bit raw value
 *   temp = (raw12 * 125 - 305000) / 1000
 */
static double hwsense_amd_ccd_temp_c(uint32_t raw)
{
    uint32_t raw12 = raw & 0xFFF;
    return ((double)(raw12 * 125) - 305000.0) / 1000.0;
}

/* A CCD sensor that is absent reads back as all zeroes, and a decoded value
 * outside this range means the register is not a temperature on this part. */
static int hwsense_amd_ccd_temp_plausible(uint32_t raw)
{
    double temp;
    if (raw == 0)
        return 0;
    temp = hwsense_amd_ccd_temp_c(raw);
    return temp >= -40.0 && temp <= 150.0;
}

/*
 * AMD: SVI2 voltage from SMN 0x0005A010 (Plane0) / 0x0005A00C (Plane1).
 *
 *   bits [24:16] = 9-bit SVI2 VID, 6.25 mV per LSB
 *   bits [7:0]   = current telemetry
 *
 * VID 0x1FF means the plane is off / not present.
 */
#define HWSENSE_SVI2_VID_OFF 0x1FF

static uint32_t hwsense_svi2_vid(uint32_t raw)
{
    return (raw >> 16) & 0x1FF;
}

static double hwsense_svi2_vid_to_volts(uint32_t vid)
{
    return (double)vid * 0.00625;
}

/*
 * Intel: RAPL energy unit from MSR 0x606 bits [12:8].
 * Energy is reported in 1/2^unit joules.
 */
static double hwsense_rapl_energy_unit(uint32_t power_unit_eax)
{
    uint32_t unit = (power_unit_eax >> 8) & 0x1F;
    double d = 1.0;
    uint32_t i;
    for (i = 0; i < unit; i++)
        d *= 2.0;
    return 1.0 / d;
}

/*
 * Power from two RAPL energy counter samples.
 * The counter is 32 bits and wraps, so the delta is taken modulo 2^32.
 */
static double hwsense_rapl_power_w(uint32_t energy_before, uint32_t energy_after,
                                   double energy_unit, double seconds)
{
    uint32_t delta = energy_after - energy_before; /* wraps correctly */
    if (seconds <= 0.0)
        return -1.0;
    return ((double)delta * energy_unit) / seconds;
}

#endif /* HWSENSE_SENSOR_MATH_H */
