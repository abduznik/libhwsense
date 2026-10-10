/*
 * unified_linux.c — Unified sensor API on Linux, built from /proc and the
 * hwmon-backed readers.
 */

#ifndef _WIN32

#include "hwsense_unified.h"
#include "hwsense_internal.h"
#include "hwmon_util_linux.h"
#include <unistd.h>
#include <sys/statvfs.h>

hwsense_ctx_t *hwsense_unified_init(void)
{
    return hwsense_init();
}

/* CPU busy percentage over a short sampling window. */
double hwsense_get_cpu_load(hwsense_ctx_t *ctx)
{
    unsigned long long a[8], b[8], ta = 0, tb = 0;
    int i, got = 0;
    FILE *f;
    (void)ctx;

    for (i = 0; i < 2; i++) {
        unsigned long long *v = i ? b : a;
        f = fopen("/proc/stat", "r");
        if (!f)
            return -1.0;
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8)
            got++;
        fclose(f);
        if (!i)
            usleep(100000);
    }
    if (got != 2)
        return -1.0;

    for (i = 0; i < 8; i++) {
        ta += a[i];
        tb += b[i];
    }
    {
        unsigned long long idle = (b[3] + b[4]) - (a[3] + a[4]);
        unsigned long long total = tb - ta;
        return total ? 100.0 * (double)(total - idle) / (double)total : 0.0;
    }
}

static int meminfo(double *total_gb, double *used_gb)
{
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    unsigned long long total = 0, avail = 0;

    if (!f)
        return 0;
    while (fgets(line, sizeof(line), f)) {
        sscanf(line, "MemTotal: %llu kB", &total);
        sscanf(line, "MemAvailable: %llu kB", &avail);
    }
    fclose(f);
    if (!total)
        return 0;
    *total_gb = total / 1048576.0;
    *used_gb = (total - avail) / 1048576.0;
    return 1;
}

double hwsense_get_memory_used(hwsense_ctx_t *ctx)
{
    double t, u;
    (void)ctx;
    return meminfo(&t, &u) ? u : -1.0;
}

double hwsense_get_cpu_temp(hwsense_ctx_t *ctx)
{
    hwsense_temp_result_t t = hwsense_cpu_package_temp(ctx);
    return t.ok ? t.celsius : -1.0;
}

int hwsense_get_cpu_clock(hwsense_ctx_t *ctx)
{
    return hwsense_cpu_core_clock(ctx);
}

/* Not exposed by mainline Linux for most CPUs. */
double hwsense_get_cpu_voltage(hwsense_ctx_t *ctx) { (void)ctx; return -1.0; }
double hwsense_get_cpu_power(hwsense_ctx_t *ctx)   { (void)ctx; return -1.0; }

int hwsense_read_all(hwsense_ctx_t *ctx, hwsense_sensor_data_t *data)
{
    char line[256];
    long v;
    FILE *f;
    double up;
    hwsense_superio_result_t fans;
    hwsense_gpu_info_t gpu;
    struct statvfs sv;
    int t;

    if (!ctx || !data)
        return 0;
    memset(data, 0, sizeof(*data));

    snprintf(data->cpu_vendor, sizeof(data->cpu_vendor), "%s",
             ctx->vendor == 'I' ? "Intel" : ctx->vendor == 'A' ? "AMD" : "Unknown");

    f = fopen("/proc/cpuinfo", "r");
    if (f) {
        int siblings = 0, cores = 0;
        while (fgets(line, sizeof(line), f)) {
            char *c = strchr(line, ':');
            if (!c)
                continue;
            if (!data->cpu_name[0] && !strncmp(line, "model name", 10)) {
                snprintf(data->cpu_name, sizeof(data->cpu_name), "%s", c + 2);
                data->cpu_name[strcspn(data->cpu_name, "\n")] = '\0';
            } else if (!strncmp(line, "siblings", 8) && !siblings) {
                siblings = atoi(c + 1);
            } else if (!strncmp(line, "cpu cores", 9) && !cores) {
                cores = atoi(c + 1);
            }
        }
        fclose(f);
        data->cpu_threads = siblings;
        data->cpu_cores = cores;
    }
    if (!data->cpu_threads)
        data->cpu_threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (!data->cpu_cores)
        data->cpu_cores = data->cpu_threads;

    data->cpu_temp = hwsense_get_cpu_temp(ctx);
    if (data->cpu_temp < 0)
        data->cpu_temp = 0;
    data->cpu_clock_mhz = hwsense_cpu_core_clock(ctx);
    if (data->cpu_clock_mhz < 0)
        data->cpu_clock_mhz = 0;
    if (hwsense_read_long("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq", &v))
        data->cpu_clock_max_mhz = (int)(v / 1000);

    data->cpu_load = hwsense_get_cpu_load(ctx);
    if (data->cpu_load < 0)
        data->cpu_load = 0;

    if (meminfo(&data->memory_total_gb, &data->memory_used_gb))
        data->memory_percent = (int)(100.0 * data->memory_used_gb / data->memory_total_gb);

    f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) == 1) {
            data->uptime_days = (int)(up / 86400);
            data->uptime_hours = (int)(up / 3600) % 24;
        }
        fclose(f);
    }

    fans = hwsense_superio_fans(ctx);
    if (fans.ok) {
        data->fan_count = fans.fan_count > 8 ? 8 : fans.fan_count;
        memcpy(data->fan_rpms, fans.fan_rpms, data->fan_count * sizeof(int));
    }

    gpu = hwsense_gpu_info(0);
    if (gpu.ok)
        data->gpu_temp = gpu.temperature;

    t = hwsense_nvme_temperature();
    if (t >= 0) {
        snprintf(data->drives[0].name, sizeof(data->drives[0].name), "nvme");
        data->drives[0].temperature = t;
        if (statvfs("/", &sv) == 0) {
            data->drives[0].size_gb = (long long)((double)sv.f_blocks * sv.f_frsize / 1e9);
            data->drives[0].used_gb =
                (long long)((double)(sv.f_blocks - sv.f_bfree) * sv.f_frsize / 1e9);
        }
        data->drive_count = 1;
    }
    
    return 1;
}

void hwsense_print_report(hwsense_ctx_t *ctx)
{
    hwsense_sensor_data_t d;
    int i;

    hwsense_read_all(ctx, &d);

    printf("=== System Sensor Report ===\n\n--- CPU ---\n");
    printf("  Vendor:       %s\n  Name:         %s\n", d.cpu_vendor, d.cpu_name);
    printf("  Cores/Threads: %d/%d\n", d.cpu_cores, d.cpu_threads);
    printf("  Temperature:  %.1f C\n  Clock:        %d MHz (max %d)\n",
           d.cpu_temp, d.cpu_clock_mhz, d.cpu_clock_max_mhz);
    printf("  Load:         %.1f%%\n\n", d.cpu_load);
    printf("--- Memory ---\n  Used: %.1f / %.1f GB (%d%%)\n\n",
           d.memory_used_gb, d.memory_total_gb, d.memory_percent);
    printf("--- GPU ---\n  Temperature:  %.0f C\n\n", d.gpu_temp);
    for (i = 0; i < d.drive_count; i++)
        printf("--- Drive: %s ---\n  Temperature:  %d C\n  Used: %lld / %lld GB\n\n",
               d.drives[i].name, d.drives[i].temperature,
               d.drives[i].used_gb, d.drives[i].size_gb);
    if (d.fan_count) {
        printf("--- Fans ---\n");
        for (i = 0; i < d.fan_count; i++)
            printf("  Fan %d: %d RPM\n", i + 1, d.fan_rpms[i]);
        printf("\n");
    }
    printf("--- System ---\n  Uptime: %d days, %d hours\n", d.uptime_days, d.uptime_hours);
}

#endif /* !_WIN32 */
