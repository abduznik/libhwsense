/*
 * driver_linux.c — Linux context lifecycle and vendor dispatch.
 *
 * No kernel driver to install: MSR access goes through /dev/cpu/N/msr,
 * which requires CAP_SYS_RAWIO (typically root) and the `msr` kernel
 * module loaded (`modprobe msr`).
 *
 * Only the Intel CPU temperature path is implemented so far. Everything
 * else in the public API is Windows-only (WinRing0, WMI, ADL, Super I/O,
 * Embedded Controller) and is not compiled on Linux.
 */

#ifndef _WIN32

#include "hwsense_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

extern hwsense_temp_result_t hwsense_intel_core_temp_linux(void);

/*
 * Detect CPU vendor from /proc/cpuinfo.
 * Returns 'I' for Intel, 'A' for AMD, '?' for unknown.
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
    int fd;

    fd = open("/dev/cpu/0/msr", O_RDONLY);
    if (fd < 0) {
        fprintf(stderr,
                "ERROR: cannot open /dev/cpu/0/msr (%s).\n"
                "Run as root (or grant CAP_SYS_RAWIO) and `modprobe msr` first.\n",
                strerror(errno));
        return NULL;
    }
    close(fd);

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
    hwsense_temp_result_t r = {0};

    if (!ctx) {
        snprintf(r.error, sizeof(r.error), "Invalid context");
        return r;
    }

    if (ctx->vendor == 'I')
        return hwsense_intel_core_temp_linux();

    snprintf(r.error, sizeof(r.error),
             ctx->vendor == 'A'
                 ? "AMD temperature reading is not implemented on Linux yet"
                 : "Unknown or unsupported CPU vendor");
    return r;
}

#endif /* !_WIN32 */
