/*
 * read_cpu_temp.c - CLI that reads all system sensors via libhwsense.
 * Must run as Administrator for hardware sensor access.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "hwsense.h"
#include "../src/core/ioctl_codes.h"
#include "../src/core/win_sysstats.h"

int main(void)
{
    BOOL is_admin = FALSE;
    PSID admin_group = NULL;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    char smu_name[64] = {0};
    unsigned int smu_ver = 0, pm_ver = 0;
    unsigned long long dram_base = 0;
    time_t now;
    struct tm *tm_info;
    char time_buf[64];

    /* Admin check */
    if (AllocateAndInitializeSid(&nt_auth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &admin_group))
    {
        CheckTokenMembership(NULL, admin_group, &is_admin);
        FreeSid(admin_group);
    }

    if (!is_admin) {
        fprintf(stderr, "ERROR: This program must be run as Administrator.\n");
        return 1;
    }

    hwsense_ctx_t *ctx = hwsense_init();
    if (!ctx)
        return 1;

    /* Header */
    time(&now);
    tm_info = localtime(&now);
    strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", tm_info);
    printf("=== System Sensor Report ===\n");
    printf("Timestamp: %s\n\n", time_buf);

    /* ── System Info ── */
    {
        win_mem_stats_t mem;
        win_cpu_load_t cpu_load;
        win_uptime_t uptime;

        uptime = win_get_uptime();
        printf("Uptime: %d days, %d hours, %d minutes\n",
               uptime.days, uptime.hours, uptime.minutes);

        if (win_get_cpu_load(&cpu_load))
            printf("CPU Load: %.1f%% (%d cores)\n", cpu_load.percent, cpu_load.num_cpus);

        if (win_get_mem_stats(&mem))
            printf("Memory: %.1f / %.1f GB (%d%% used)\n",
                   mem.used_gb, mem.total_gb, mem.percent_used);
    }
    printf("\n");

    /* ── CPU Temperature ── */
    printf("--- CPU Temperature ---\n");
    hwsense_temp_result_t pkg = hwsense_cpu_package_temp(ctx);
    if (pkg.ok)
        printf("CPU Package (Tctl):  %.1f C\n", pkg.celsius);
    else
        printf("CPU Package:         N/A\n");

    /* CCD temps */
    hwsense_ccd_temps_t ccd = hwsense_amd_ccd_temps(ctx);
    if (ccd.count > 0) {
        int i;
        for (i = 0; i < HWSENSE_MAX_CCD; i++)
            if (ccd.available[i])
                printf("  CCD %d:             %.1f C\n", i, ccd.celsius[i]);
    } else {
        printf("CCD temps:           N/A\n");
    }
    printf("\n");

    /* ── CPU Voltage & Power ── */
    printf("--- CPU Voltage & Power ---\n");
    hwsense_voltage_result_t vcore = hwsense_cpu_core_voltage(ctx);
    if (vcore.ok)
        printf("Core Voltage:        %.4f V  (%.1f A)\n", vcore.volts, vcore.amps);
    else
        printf("Core Voltage:        N/A\n");

    hwsense_voltage_result_t vsoc = hwsense_amd_soc_voltage_dispatch(ctx);
    if (vsoc.ok)
        printf("SoC Voltage:         %.4f V  (%.1f A)\n", vsoc.volts, vsoc.amps);
    else
        printf("SoC Voltage:         N/A\n");

    double power_w = hwsense_cpu_package_power_watts(ctx);
    if (power_w > 0)
        printf("Package Power:       %.2f W\n", power_w);
    else
        printf("Package Power:       N/A\n");

    int clock = hwsense_cpu_core_clock(ctx);
    if (clock > 0)
        printf("Core Clock:          %d MHz\n", clock);
    else
        printf("Core Clock:          N/A\n");
    printf("\n");

    /* ── GPU Temperature ── */
    printf("--- GPU ---\n");
    hwsense_gpu_result_t gpu = hwsense_gpu_temperature(0);
    if (gpu.ok)
        printf("GPU Temp:            %d C  (%s)\n", gpu.temperature, gpu.name);
    else
        printf("GPU Temp:            N/A\n");
    printf("\n");

    /* ── Drive Stats ── */
    printf("--- Drive Stats ---\n");
    {
        int nvme_temp = hwsense_nvme_temperature();
        if (nvme_temp > 0)
            printf("NVMe/SSD Temp:       %d C\n", nvme_temp);
        else
            printf("NVMe/SSD Temp:       N/A\n");
    }

    /* Disk usage */
    {
        win_disk_stats_t disks[MAX_WIN_DISKS];
        int disk_count = win_get_disk_stats(disks, MAX_WIN_DISKS);
        int i;
        for (i = 0; i < disk_count; i++) {
            printf("  %s: %lld GB / %lld GB (%d%% used)\n",
                   disks[i].name,
                   disks[i].total_size_gb - disks[i].free_gb,
                   disks[i].total_size_gb,
                   disks[i].percent_used);
        }
    }

    /* Disk I/O */
    printf("\n--- Disk I/O ---\n");
    {
        win_disk_io_t diskio[MAX_WIN_DISK_IO];
        int io_count = win_get_disk_io(diskio, MAX_WIN_DISK_IO);
        int i;
        for (i = 0; i < io_count; i++) {
            printf("  %-16s Read: %8.2f GB  Write: %8.2f GB\n",
                   diskio[i].name,
                   diskio[i].read_bytes / 1e9,
                   diskio[i].write_bytes / 1e9);
        }
    }

    /* Super I/O */
    hwsense_superio_result_t sio = hwsense_superio_temps(ctx);
    if (sio.ok && sio.count > 0) {
        printf("Super I/O (%s):    ", sio.chip_name);
        int i;
        for (i = 0; i < sio.count; i++)
            printf("%d C  ", sio.temperatures[i]);
        printf("\n");
    }

    /* Super I/O Fan Speeds */
    hwsense_superio_result_t sio_fans = hwsense_superio_fans(ctx);
    if (sio_fans.ok && sio_fans.fan_count > 0) {
        printf("Fan Speeds:        ");
        int i;
        for (i = 0; i < sio_fans.fan_count; i++)
            printf("Fan%d: %d RPM  ", i + 1, sio_fans.fan_rpms[i]);
        printf("\n");
    }

    /* Super I/O Voltages */
    hwsense_superio_result_t sio_volts = hwsense_superio_voltages(ctx);
    if (sio_volts.ok && sio_volts.voltage_count > 0) {
        printf("SIO Voltages:      ");
        int i;
        for (i = 0; i < sio_volts.voltage_count && i < 4; i++)
            printf("V%d: %.3f V  ", i + 1, sio_volts.voltages[i]);
        printf("\n");
    }
    printf("\n");

    /* ── SMU Diagnostics (AMD only) ── */
    if (hwsense_smu_diag(ctx, smu_name, sizeof(smu_name), &smu_ver, &pm_ver, &dram_base)) {
        printf("--- SMU Diagnostics ---\n");
        printf("  CPU Codename:     %s\n", smu_name);
        printf("  SMU Version:      0x%08X\n", smu_ver);
        printf("  PM Table Version: 0x%08X\n", pm_ver);
        printf("  DRAM Base:        0x%016llX\n", dram_base);
        printf("\n");
    }

    printf("=== End Report ===\n");

    hwsense_shutdown(ctx);
    return 0;
}
