/*
 * soc_math.h — Pure helpers for the sysfs sources phones and ARM boards use
 * instead of hwmon, free of any OS or driver calls.
 *
 * Kept separate from the read paths so they can be unit-tested without
 * hardware (see tests/test_soc.c).
 */

#ifndef HWSENSE_SOC_MATH_H
#define HWSENSE_SOC_MATH_H

#include <string.h>

/* A decoded value outside this range is a sensor sentinel, not a temperature. */
#define HWSENSE_THERMAL_MIN_C (-40.0)
#define HWSENSE_THERMAL_MAX_C 150.0

/*
 * Decode a thermal_zone "temp" value to C.
 *
 * The kernel ABI is millidegrees, but drivers disagree in practice. On a
 * Snapdragon 8 Gen 2 (SM8550) inactive sensors read -273000, some read 0,
 * and one zone ("socd") reports whole degrees (27). So:
 *
 *   |raw| < 1000 -> whole degrees (a real millidegree value that low would
 *                    be under 1 C, which no live CPU/GPU sensor reports)
 *   otherwise    -> millidegrees
 *
 * Returns 1 and sets *celsius for a plausible reading. Returns 0 for a
 * raw value of 0 (zone not reporting) and for anything outside
 * [HWSENSE_THERMAL_MIN_C, HWSENSE_THERMAL_MAX_C].
 */
static int hwsense_thermal_decode(long raw, double *celsius)
{
    double c;

    if (raw == 0)
        return 0;
    c = (raw > -1000 && raw < 1000) ? (double)raw : (double)raw / 1000.0;
    if (c < HWSENSE_THERMAL_MIN_C || c > HWSENSE_THERMAL_MAX_C)
        return 0;
    *celsius = c;
    return 1;
}

static int hwsense_zone_type_starts(const char *type, const char *prefix)
{
    return strncmp(type, prefix, strlen(prefix)) == 0;
}

/*
 * Does this thermal_zone "type" name a CPU sensor?
 *
 *   cpu*          Qualcomm cpu-0-0 / cpuss-0, Raspberry Pi cpu-thermal,
 *                 Allwinner/Amlogic cpu_thermal
 *   mtktscpu*     MediaTek
 *   x86_pkg_temp  Intel package sensor
 *   soc-thermal   Rockchip and similar SoCs that have one shared sensor
 *   soc_thermal
 *
 * Only the Qualcomm names have been checked on real hardware.
 */
static int hwsense_thermal_is_cpu_zone(const char *type)
{
    return hwsense_zone_type_starts(type, "cpu") ||
           hwsense_zone_type_starts(type, "mtktscpu") ||
           hwsense_zone_type_starts(type, "x86_pkg_temp") ||
           hwsense_zone_type_starts(type, "soc-thermal") ||
           hwsense_zone_type_starts(type, "soc_thermal");
}

/* GPU sensors: Qualcomm gpuss-N, and the common gpu-thermal / gpu_thermal. */
static int hwsense_thermal_is_gpu_zone(const char *type)
{
    return hwsense_zone_type_starts(type, "gpu");
}

#endif /* HWSENSE_SOC_MATH_H */
