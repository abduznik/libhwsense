#ifndef HWSENSE_HWMON_UTIL_LINUX_H
#define HWSENSE_HWMON_UTIL_LINUX_H

#ifndef _WIN32
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Read the first line of a small sysfs/procfs file, newline stripped. */
static int hwsense_read_line(const char *path, char *buf, size_t n)
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

/* Read a sysfs integer (e.g. millidegrees). Returns 1 on success. */
static int hwsense_read_long(const char *path, long *out)
{
    char buf[64];
    if (!hwsense_read_line(path, buf, sizeof(buf)))
        return 0;
    *out = atol(buf);
    return 1;
}

#define HWSENSE_HWMON_MAX 64

/* Name of /sys/class/hwmon/hwmonN, or 0 if that index does not exist. */
static int hwsense_hwmon_name(int idx, char *name, size_t n)
{
    char path[96];
    snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/name", idx);
    return hwsense_read_line(path, name, n);
}
#endif

#endif
