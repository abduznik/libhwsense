#ifndef HWSENSE_SOC_LINUX_H
#define HWSENSE_SOC_LINUX_H

#ifndef _WIN32

#include "../../include/hwsense.h"
#include <stddef.h>

/* Default sysfs roots. Every reader takes its root as a parameter so the
 * tests can point it at a fake tree. */
#define HWSENSE_THERMAL_ROOT "/sys/class/thermal"
#define HWSENSE_KGSL_ROOT    "/sys/class/kgsl/kgsl-3d0"

/* Thermal zones: every zone under `root` with a plausible reading. */
int hwsense_thermal_scan(const char *root, hwsense_thermal_zone_t *out, int max);

/* Hottest CPU / GPU zone under `root`. */
hwsense_temp_result_t hwsense_thermal_cpu_temp(const char *root);
int hwsense_thermal_gpu_temp(const char *root, double *celsius);

/* Qualcomm KGSL (Adreno) model, clock and load. Fills what it finds and
 * returns 1 if any of it was present; does not touch g->ok. */
int hwsense_kgsl_info(const char *root, hwsense_gpu_info_t *g);

/* SoC name: Android system properties first, then /proc/cpuinfo "Hardware". */
int hwsense_soc_name(char *buf, size_t n);
int hwsense_soc_name_cpuinfo(const char *cpuinfo_path, char *buf, size_t n);

#endif /* !_WIN32 */

#endif
