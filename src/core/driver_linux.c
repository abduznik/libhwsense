/*
 * driver_linux.c — Linux context lifecycle and vendor dispatch.
 *
 * CPU temperature comes from hwmon sysfs first (no root). Fallback: MSR access goes through /dev/cpu/N/msr,
 * which requires CAP_SYS_RAWIO (typically root) and the `msr` kernel
 * module loaded (`modprobe msr`). Last resort is the kernel thermal zones,
 * which is the only source on Android and most ARM boards.
 *
 * Only the Intel CPU temperature path is implemented so far. Everything
 * else in the public API is Windows-only (WinRing0, WMI, ADL, Super I/O,
 * Embedded Controller) and is not compiled on Linux.
 */

#ifndef _WIN32

#include "hwsense_internal.h"
#include "soc_linux.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

extern hwsense_temp_result_t hwsense_intel_core_temp_linux(void);
extern hwsense_temp_result_t hwsense_hwmon_cpu_temp_linux(void);

/*
 * Detect CPU vendor from /proc/cpuinfo.
 * Returns 'I' for Intel, 'A' for AMD, 'R' for ARM, '?' for unknown.
 */
int hwsense_detect_vendor(void)
{
    FILE *f;
    char line[256];
    int vendor = '?';

    f = fopen("/proc/cpuinfo", "r");
    if (!f)
        return '?';

    while (fgets(line, sizeof(line), f)) {
        /* ARM has no vendor_id; every core lists a "CPU implementer". */
        if (strncmp(line, "CPU implementer", 15) == 0) {
            vendor = 'R';
            break;
        }

        if (strncmp(line, "vendor_id", 9) != 0)
            continue;

        if (strstr(line, "GenuineIntel"))
            vendor = 'I';
        else if (strstr(line, "AuthenticAMD"))
            vendor = 'A';
        break;
    }

    fclose(f);
    return vendor;
}

hwsense_ctx_t *hwsense_init(void)
{
    hwsense_ctx_t *ctx;

    ctx = (hwsense_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx)
        return NULL;

    ctx->vendor = hwsense_detect_vendor();
    return ctx;
}

void hwsense_shutdown(hwsense_ctx_t *ctx)
{
    free(ctx);
}

hwsense_temp_result_t hwsense_cpu_package_temp(hwsense_ctx_t *ctx)
{
    hwsense_temp_result_t r = {0}, t;

    if (!ctx) {
        snprintf(r.error, sizeof(r.error), "Invalid context");
        return r;
    }

    /* hwmon needs no privileges and covers AMD and Intel; try it first,
     * then fall back to raw MSR reads (Intel only, needs root + msr module). */
    r = hwsense_hwmon_cpu_temp_linux();
    if (r.ok)
        return r;

    if (ctx->vendor == 'I') {
        hwsense_temp_result_t msr = hwsense_intel_core_temp_linux();
        if (msr.ok)
            return msr;
        /* x86_pkg_temp is world-readable when the MSR is not. */
        t = hwsense_thermal_cpu_temp(HWSENSE_THERMAL_ROOT);
        return t.ok ? t : msr;
    }

    /* Android and ARM boards: no hwmon CPU sensor, but thermal zones. */
    t = hwsense_thermal_cpu_temp(HWSENSE_THERMAL_ROOT);
    if (t.ok)
        return t;

    /* r.error already explains the hwmon failure */
    return r;
}

#endif /* !_WIN32 */
