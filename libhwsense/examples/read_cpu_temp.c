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
#include "../src/motherboard/ec.h"
#include "../src/core/wmi.h"

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

        /* Use sampled load for accurate reading */
        printf("Measuring CPU load (1 second)...\n");
        if (win_get_cpu_load_sampled(&cpu_load))
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
    printf("Super I/O:         ID=0x%04X", sio.chip_id);
    if (sio.ok && sio.count > 0) {
        printf(" (%s) ", sio.chip_name);
        int i;
        for (i = 0; i < sio.count; i++)
            printf("%d C  ", sio.temperatures[i]);
    } else {
        printf(" - %s", sio.error);
    }
    printf("\n");

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

    /* ASUS Embedded Controller (EC) sensors */
    printf("\n--- ASUS EC Sensors ---\n");
    {
        HANDLE dev = hwsense_get_driver_handle(ctx);
        ec_result_t ec = ec_read_all_sensors(dev);
        if (ec.ok) {
            if (ec.cpu_fan_rpm > 0)
                printf("CPU Fan:           %d RPM\n", ec.cpu_fan_rpm);
            if (ec.vrm_fan_rpm > 0)
                printf("VRM Fan:           %d RPM\n", ec.vrm_fan_rpm);
            if (ec.chipset_temp > 0)
                printf("Chipset Temp:      %d C\n", ec.chipset_temp);
            if (ec.cpu_temp > 0)
                printf("CPU Temp (EC):     %d C\n", ec.cpu_temp);
            if (ec.mb_temp > 0)
                printf("Motherboard Temp:  %d C\n", ec.mb_temp);
            if (ec.vrm_temp > 0)
                printf("VRM Temp:          %d C\n", ec.vrm_temp);
            if (ec.cpu_voltage_mv > 0)
                printf("CPU Voltage (EC):  %.3f V\n", ec.cpu_voltage_mv / 1000.0);
        } else {
            printf("EC:                %s\n", ec.error);
        }
    }

    /* IO Port Scan Diagnostic */
    printf("\n--- IO Port Scan ---\n");
    {
        HANDLE dev = hwsense_get_driver_handle(ctx);
        DWORD ports_to_scan[] = { 0x2E, 0x4E, 0x162, 0x3E0, 0x4E0, 0x62, 0x66 };
        int num_ports = sizeof(ports_to_scan) / sizeof(ports_to_scan[0]);
        int i;
        for (i = 0; i < num_ports; i++) {
            BYTE val = 0;
            DWORD in_val = ports_to_scan[i];
            DWORD out_val = 0;
            DWORD bytes_ret = 0;
            BOOL ok = DeviceIoControl(
                dev, IOCTL_OLS_READ_IO_PORT_BYTE,
                &in_val, sizeof(in_val),
                &out_val, sizeof(out_val),
                &bytes_ret, NULL
            );
            if (ok)
                printf("  Port 0x%03X: 0x%02X\n", ports_to_scan[i], (BYTE)(out_val & 0xFF));
            else
                printf("  Port 0x%03X: FAIL\n", ports_to_scan[i]);
        }
    }
    printf("\n");

    /* WMI Sensor Data */
    printf("--- WMI Sensors ---\n");
    if (wmi_init()) {
        /* Thermal zones */
        double temps[32];
        char names[32][64];
        int zone_count = wmi_read_thermal_zones(temps, 32, names, 32);
        if (zone_count > 0) {
            printf("Thermal Zones: %d found\n", zone_count);
            for (int i = 0; i < zone_count; i++)
                printf("  %s: %.1f C\n", names[i], temps[i]);
        } else {
            printf("Thermal Zones: None found\n");
        }

        /* CPU info */
        wmi_cpu_info_t cpu;
        if (wmi_read_cpu_info(&cpu)) {
            printf("CPU: %s\n", cpu.name);
            printf("  Max Clock: %d MHz\n", cpu.max_clock_mhz);
            printf("  Current Clock: %d MHz\n", cpu.current_clock_mhz);
            if (cpu.voltage_mv > 0)
                printf("  Voltage: %.3f V\n", cpu.voltage_mv / 1000.0);
            if (cpu.load_percent > 0)
                printf("  Load: %d%%\n", cpu.load_percent);
        }

        /* Fans */
        wmi_fan_info_t fans[16];
        int fan_count = wmi_read_fans(fans, 16);
        if (fan_count > 0) {
            printf("Fans: %d found\n", fan_count);
            for (int i = 0; i < fan_count; i++)
                printf("  %s: %s\n", fans[i].name,
                       fans[i].active_cooling ? "Active" : "Passive");
        }

        /* Voltages */
        wmi_voltage_info_t voltages[16];
        int voltage_count = wmi_read_voltages(voltages, 16);
        if (voltage_count > 0) {
            printf("Voltages: %d found\n", voltage_count);
            for (int i = 0; i < voltage_count; i++)
                printf("  %s: %.3f V\n", voltages[i].name, voltages[i].voltage);
        }

        wmi_shutdown();
    } else {
        printf("WMI: Failed to initialize\n");
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
