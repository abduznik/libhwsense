/*
 * soc_linux.c — Sensors on phones and ARM boards, where hwmon is usually
 * missing.
 *
 *   thermal zones   /sys/class/thermal   CPU and GPU temperature
 *   KGSL            /sys/class/kgsl      Qualcomm Adreno model/clock/load
 *   SoC name        system properties    replaces cpuinfo "model name"
 *
 * All of it is world-readable on a stock, unrooted Android device (checked
 * from Termux on a Snapdragon 8 Gen 2), so no root and no kernel driver are
 * needed. Individual zones and files may still be denied; every reader skips
 * what it cannot open rather than failing.
 */

#ifndef _WIN32

#include "soc_linux.h"
#include "hwmon_util_linux.h"
#include "soc_math.h"

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#define THERMAL_ZONE_MAX 256

/* 1 = valid reading, 0 = zone present but not reporting, -1 = no such zone. */
static int read_zone(const char *root, int idx, char *type, size_t type_n,
                     double *celsius)
{
    char path[160];
    long raw;

    snprintf(path, sizeof(path), "%s/thermal_zone%d/type", root, idx);
    if (!hwsense_read_line(path, type, type_n))
        return -1;

    snprintf(path, sizeof(path), "%s/thermal_zone%d/temp", root, idx);
    if (!hwsense_read_long(path, &raw))
        return 0;
    return hwsense_thermal_decode(raw, celsius);
}

int hwsense_thermal_scan(const char *root, hwsense_thermal_zone_t *out, int max)
{
    char type[64];
    double c;
    int idx, n = 0;

    if (!root || !out || max <= 0)
        return 0;

    for (idx = 0; idx < THERMAL_ZONE_MAX && n < max; idx++) {
        if (read_zone(root, idx, type, sizeof(type), &c) != 1)
            continue;
        snprintf(out[n].type, sizeof(out[n].type), "%s", type);
        out[n].celsius = c;
        n++;
    }
    return n;
}

int hwsense_thermal_zones(hwsense_thermal_zone_t *out, int max)
{
    return hwsense_thermal_scan(HWSENSE_THERMAL_ROOT, out, max);
}

/* Hottest valid zone whose type satisfies `match`. Returns 1 if there was one. */
static int hottest_zone(const char *root, int (*match)(const char *),
                        double *celsius)
{
    char type[64];
    double c, best = 0.0;
    int idx, found = 0;

    for (idx = 0; idx < THERMAL_ZONE_MAX; idx++) {
        if (read_zone(root, idx, type, sizeof(type), &c) != 1 || !match(type))
            continue;
        if (!found || c > best)
            best = c;
        found = 1;
    }
    if (found)
        *celsius = best;
    return found;
}

/*
 * "Package" temperature on a SoC is the hottest CPU sensor: phones expose a
 * sensor per core or cluster rather than one package value.
 */
hwsense_temp_result_t hwsense_thermal_cpu_temp(const char *root)
{
    hwsense_temp_result_t r = {0};

    if (hottest_zone(root, hwsense_thermal_is_cpu_zone, &r.celsius))
        r.ok = 1;
    else
        snprintf(r.error, sizeof(r.error),
                 "No CPU thermal zone with a valid reading in %s", root);
    return r;
}

int hwsense_thermal_gpu_temp(const char *root, double *celsius)
{
    return hottest_zone(root, hwsense_thermal_is_gpu_zone, celsius);
}

/* ── Qualcomm KGSL ─────────────────────────────────────────────────── */

int hwsense_kgsl_info(const char *root, hwsense_gpu_info_t *g)
{
    char path[160], buf[64];
    long v;
    int found = 0;

    snprintf(path, sizeof(path), "%s/gpu_model", root);
    if (hwsense_read_line(path, buf, sizeof(buf)) && buf[0]) {
        snprintf(g->name, sizeof(g->name), "%s", buf);
        found = 1;
    }

    snprintf(path, sizeof(path), "%s/clock_mhz", root);
    if (hwsense_read_long(path, &v)) {
        g->clock_mhz = (int)v;
        found = 1;
    }

    /* Reads as "37 %". */
    snprintf(path, sizeof(path), "%s/gpu_busy_percentage", root);
    if (hwsense_read_long(path, &v)) {
        g->gpu_load = (int)v;
        found = 1;
    }
    return found;
}

/* ── SoC name ──────────────────────────────────────────────────────── */

/* ARM /proc/cpuinfo has no "model name"; boards report "Hardware" instead. */
int hwsense_soc_name_cpuinfo(const char *cpuinfo_path, char *buf, size_t n)
{
    FILE *f = fopen(cpuinfo_path, "r");
    char line[256];
    int found = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        char *c = strchr(line, ':');

        if (!c || strncmp(line, "Hardware", 8))
            continue;
        c += 1 + (c[1] == ' ');
        c[strcspn(c, "\n")] = '\0';
        if (*c) {
            snprintf(buf, n, "%s", c);
            found = 1;
        }
        break;
    }
    fclose(f);
    return found;
}

int hwsense_soc_name(char *buf, size_t n)
{
#ifdef __ANDROID__
    char mfr[PROP_VALUE_MAX], model[PROP_VALUE_MAX];

    mfr[0] = model[0] = '\0';
    __system_property_get("ro.soc.manufacturer", mfr);
    if (__system_property_get("ro.soc.model", model) > 0) {
        if (mfr[0])
            snprintf(buf, n, "%s %s", mfr, model);
        else
            snprintf(buf, n, "%s", model);
        return 1;
    }
#endif
    return hwsense_soc_name_cpuinfo("/proc/cpuinfo", buf, n);
}

#endif /* !_WIN32 */
