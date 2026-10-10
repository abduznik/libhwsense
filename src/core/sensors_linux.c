/*
 * sensors_linux.c — hwmon-backed sensors on Linux: NVMe temperature,
 * motherboard fans/temperatures/voltages, CPU clock, and AMD GPU fallback.
 *
 * Everything here reads world-readable sysfs, so no root and no kernel
 * driver are needed. Fan/voltage data only appears if a kernel driver for
 * the board's Super I/O chip is loaded (nct6775, it87, asus-ec-sensors, ...).
 */

#ifndef _WIN32

#include "hwsense_internal.h"
#include "hwmon_util_linux.h"

/* Highest temperature across all NVMe drives, or -1. */
int hwsense_nvme_temperature(void)
{
    char name[64], path[128];
    long mdeg, best = -1;
    int hw;

    for (hw = 0; hw < HWSENSE_HWMON_MAX; hw++) {
        if (!hwsense_hwmon_name(hw, name, sizeof(name)) || strcmp(name, "nvme"))
            continue;
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/temp1_input", hw);
        if (hwsense_read_long(path, &mdeg) && mdeg > best)
            best = mdeg;
    }
    return best < 0 ? -1 : (int)((best + 500) / 1000);
}

extern int hwsense_ite_read_linux(hwsense_superio_result_t *r);

/* Chips that are CPU/GPU/storage, not motherboard Super I/O. */
static int is_non_board_chip(const char *name)
{
    static const char *skip[] = { "k10temp", "zenpower", "coretemp", "nvme",
                                  "amdgpu", "nouveau", "radeon", "acpitz",
                                  "BAT0", "AC", "iwlwifi_1", "drivetemp" };
    size_t i;
    for (i = 0; i < sizeof(skip) / sizeof(skip[0]); i++)
        if (!strcmp(name, skip[i]))
            return 1;
    return 0;
}

static hwsense_superio_result_t superio_scan(int want_temps, int want_fans,
                                             int want_volts)
{
    hwsense_superio_result_t r;
    char name[64], path[128];
    long v;
    int hw, i;

    memset(&r, 0, sizeof(r));

    for (hw = 0; hw < HWSENSE_HWMON_MAX; hw++) {
        int found = 0;

        if (!hwsense_hwmon_name(hw, name, sizeof(name)) || is_non_board_chip(name))
            continue;

        if (want_fans)
            for (i = 1; i <= 16 && r.fan_count < HWSENSE_MAX_SUPERIO_FANS; i++) {
                snprintf(path, sizeof(path),
                         "/sys/class/hwmon/hwmon%d/fan%d_input", hw, i);
                if (hwsense_read_long(path, &v)) {
                    r.fan_rpms[r.fan_count++] = (int)v;
                    found = 1;
                }
            }
        if (want_temps)
            for (i = 1; i <= 16 && r.count < HWSENSE_MAX_SUPERIO_TEMPS; i++) {
                snprintf(path, sizeof(path),
                         "/sys/class/hwmon/hwmon%d/temp%d_input", hw, i);
                if (hwsense_read_long(path, &v)) {
                    r.temperatures[r.count++] = (int)((v + 500) / 1000);
                    found = 1;
                }
            }
        if (want_volts)
            for (i = 0; i <= 16 && r.voltage_count < HWSENSE_MAX_SUPERIO_VOLTAGES; i++) {
                snprintf(path, sizeof(path),
                         "/sys/class/hwmon/hwmon%d/in%d_input", hw, i);
                if (hwsense_read_long(path, &v)) {
                    r.voltages[r.voltage_count++] = v / 1000.0;
                    found = 1;
                }
            }
        if (found && !r.chip_name[0])
            snprintf(r.chip_name, sizeof(r.chip_name), "%s", name);
    }

    if (!(r.fan_count || r.count || r.voltage_count)) {
        /* No hwmon driver loaded: read the Super I/O chip directly. */
        hwsense_superio_result_t d;
        if (hwsense_ite_read_linux(&d)) {
            if (!want_fans)  { d.fan_count = 0; memset(d.fan_rpms, 0, sizeof(d.fan_rpms)); }
            if (!want_temps) { d.count = 0; memset(d.temperatures, 0, sizeof(d.temperatures)); }
            if (!want_volts) { d.voltage_count = 0; memset(d.voltages, 0, sizeof(d.voltages)); }
            return d;
        }
        snprintf(r.error, sizeof(r.error), "%s", d.error);
    }

    r.ok = (want_fans && r.fan_count) || (want_temps && r.count) ||
           (want_volts && r.voltage_count);
    if (!r.ok && !r.error[0])
        snprintf(r.error, sizeof(r.error), "No motherboard sensors found");
    return r;
}

hwsense_superio_result_t hwsense_superio_temps(hwsense_ctx_t *ctx)
{ (void)ctx; return superio_scan(1, 0, 0); }

hwsense_superio_result_t hwsense_superio_fans(hwsense_ctx_t *ctx)
{ (void)ctx; return superio_scan(0, 1, 0); }

hwsense_superio_result_t hwsense_superio_voltages(hwsense_ctx_t *ctx)
{ (void)ctx; return superio_scan(0, 0, 1); }

/* Current CPU clock in MHz: mean of all cores' scaling_cur_freq. */
int hwsense_cpu_core_clock(hwsense_ctx_t *ctx)
{
    char path[96];
    long khz, sum = 0;
    int cpu, n = 0;
    (void)ctx;

    for (cpu = 0; cpu < 1024; cpu++) {
        snprintf(path, sizeof(path),
                 "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", cpu);
        if (!hwsense_read_long(path, &khz))
            break;
        sum += khz;
        n++;
    }
    if (n)
        return (int)(sum / n / 1000);

    /* No cpufreq: fall back to /proc/cpuinfo "cpu MHz". */
    {
        FILE *f = fopen("/proc/cpuinfo", "r");
        char line[256];
        double mhz, total = 0;
        if (!f)
            return -1;
        while (fgets(line, sizeof(line), f))
            if (!strncmp(line, "cpu MHz", 7) && sscanf(strchr(line, ':') + 1, "%lf", &mhz) == 1) {
                total += mhz;
                n++;
            }
        fclose(f);
        return n ? (int)(total / n) : -1;
    }
}

/* AMD GPU through amdgpu's hwmon node. Used by gpu_linux.c. */
int hwsense_amdgpu_info_linux(int gpu_index, hwsense_gpu_info_t *info)
{
    char name[64], path[160];
    long v;
    int hw, seen = 0;

    for (hw = 0; hw < HWSENSE_HWMON_MAX; hw++) {
        if (!hwsense_hwmon_name(hw, name, sizeof(name)) || strcmp(name, "amdgpu"))
            continue;
        if (seen++ != gpu_index)
            continue;

        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/temp1_input", hw);
        if (!hwsense_read_long(path, &v))
            return 0;
        info->temperature = (int)((v + 500) / 1000);

        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/power1_average", hw);
        if (hwsense_read_long(path, &v))
            info->power_usage_w = (int)(v / 1000000);
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/power1_cap", hw);
        if (hwsense_read_long(path, &v))
            info->power_limit_w = (int)(v / 1000000);
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/freq1_input", hw);
        if (hwsense_read_long(path, &v))
            info->clock_mhz = (int)(v / 1000000);
        snprintf(path, sizeof(path), "/sys/class/hwmon/hwmon%d/freq2_input", hw);
        if (hwsense_read_long(path, &v))
            info->mem_clock_mhz = (int)(v / 1000000);

        /* load and VRAM live under the DRM device, one level up */
        snprintf(path, sizeof(path),
                 "/sys/class/hwmon/hwmon%d/device/gpu_busy_percent", hw);
        if (hwsense_read_long(path, &v))
            info->gpu_load = (int)v;
        snprintf(path, sizeof(path),
                 "/sys/class/hwmon/hwmon%d/device/mem_info_vram_used", hw);
        if (hwsense_read_long(path, &v))
            info->vram_used_mb = (int)(v / (1024 * 1024));
        snprintf(path, sizeof(path),
                 "/sys/class/hwmon/hwmon%d/device/mem_info_vram_total", hw);
        if (hwsense_read_long(path, &v))
            info->vram_total_mb = (int)(v / (1024 * 1024));

        snprintf(info->name, sizeof(info->name), "AMD GPU %d", gpu_index);
        info->ok = 1;
        return 1;
    }
    return 0;
}

#endif /* !_WIN32 */
