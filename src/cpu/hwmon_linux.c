/*
 * hwmon_linux.c — CPU temperature via the kernel's hwmon sysfs interface.
 *
 * Works for AMD (k10temp / zenpower: Tctl) and Intel (coretemp: Package id 0)
 * without root and without the msr module, so it is the default path on Linux
 * and the only one that covers AMD.
 */

#ifndef _WIN32

#include "../core/hwsense_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int read_line(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    if (!fgets(buf, (int)n, f)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    buf[strcspn(buf, "\n")] = '\0';
    return 1;
}

hwsense_temp_result_t hwsense_hwmon_cpu_temp_linux(void)
{
    hwsense_temp_result_t r = {0};
    char path[128], name[64], label[64], val[32];
    int hw, i;

    for (hw = 0; hw < 32; hw++) {
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", hw);
        if (!read_line(path, name, sizeof(name)))
            continue;
        if (strcmp(name, "k10temp") && strcmp(name, "zenpower") &&
            strcmp(name, "coretemp"))
            continue;

        /* Prefer the package/control reading, else the first input. */
        for (i = 1; i <= 16; i++) {
            int preferred;

            snprintf(path, sizeof(path),
                     "/sys/class/hwmon/hwmon%d/temp%d_label", hw, i);
            preferred = read_line(path, label, sizeof(label)) &&
                        (!strcmp(label, "Tctl") ||
                         !strncmp(label, "Package id", 10));
            if (!preferred && i != 1)
                continue;

            snprintf(path, sizeof(path),
                     "/sys/class/hwmon/hwmon%d/temp%d_input", hw, i);
            if (read_line(path, val, sizeof(val))) {
                r.ok = 1;
                r.celsius = (float)atof(val) / 1000.0f;
                if (preferred)
                    return r;
            }
        }
        if (r.ok)
            return r;
    }

    snprintf(r.error, sizeof(r.error),
             "No k10temp/zenpower/coretemp hwmon device found in /sys/class/hwmon");
    return r;
}

#endif /* !_WIN32 */
