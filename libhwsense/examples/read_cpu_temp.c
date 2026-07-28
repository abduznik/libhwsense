/*
 * read_cpu_temp.c - CLI that reads CPU sensors via libhwsense.
 * Must run as Administrator.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "hwsense.h"
#include "../src/core/ioctl_codes.h"

int main(void)
{
    BOOL is_admin = FALSE;
    PSID admin_group = NULL;
    SID_IDENTIFIER_AUTHORITY nt_auth = SECURITY_NT_AUTHORITY;
    char smu_name[64] = {0};
    unsigned int smu_ver = 0, pm_ver = 0;
    unsigned long long dram_base = 0;

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

    /* Temperature */
    hwsense_temp_result_t pkg = hwsense_cpu_package_temp(ctx);
    if (pkg.ok)
        printf("CPU Package (Tctl):  %.1f C\n", pkg.celsius);
    else
        fprintf(stderr, "CPU Package: ERROR -- %s\n", pkg.error);

    /* CCD temps */
    hwsense_ccd_temps_t ccd = hwsense_amd_ccd_temps(ctx);
    if (ccd.count > 0) {
        int i;
        for (i = 0; i < HWSENSE_MAX_CCD; i++)
            if (ccd.available[i])
                printf("CCD %d:              %.1f C\n", i, ccd.celsius[i]);
    } else {
        printf("CCD temps:           Not available on this CPU\n");
    }

    /* Voltages */
    hwsense_voltage_result_t vcore = hwsense_cpu_core_voltage(ctx);
    if (vcore.ok)
        printf("CPU Core V:         %.4f V  (%.1f A)\n", vcore.volts, vcore.amps);
    else
        fprintf(stderr, "CPU Core V:         ERROR -- %s\n", vcore.error);

    hwsense_voltage_result_t vsoc = hwsense_amd_soc_voltage_dispatch(ctx);
    if (vsoc.ok)
        printf("SoC V:              %.4f V  (%.1f A)\n", vsoc.volts, vsoc.amps);
    else
        fprintf(stderr, "SoC V:              ERROR -- %s\n", vsoc.error);

    /* Package Power */
    hwsense_voltage_result_t power = hwsense_cpu_package_power(ctx);
    if (power.ok)
        printf("Package Power:      %.2f W  (%.1f A total)\n", power.volts, power.amps);
    else
        fprintf(stderr, "Package Power:      ERROR -- %s\n", power.error);

    /* NVMe/SSD Temperature */
    int nvme_temp = hwsense_nvme_temperature();
    if (nvme_temp > 0)
        printf("NVMe/SSD Temp:      %d C\n", nvme_temp);
    else
        printf("NVMe/SSD Temp:      N/A\n");

    /* IOCTL scan diagnostic - use existing driver handle */
    printf("\n--- IOCTL Memory Read Scan ---\n");
    {
        extern int hwsense_read_smn_diag(hwsense_ctx_t *ctx, unsigned int smn_addr, unsigned int *out_value);
        /* We need the driver handle from ctx - let's test via the read_physical_memory path */
        BYTE inp[16];
        BYTE out[4];
        DWORD br;
        *(DWORD64*)&inp[0] = 0x10000;
        *(DWORD*)&inp[8] = 4;
        *(DWORD*)&inp[12] = 1;

        printf("  IOCTL_OLS_READ_MSR = 0x%08X\n", IOCTL_OLS_READ_MSR);
        printf("  IOCTL_OLS_READ_PCI_CONFIG = 0x%08X\n", IOCTL_OLS_READ_PCI_CONFIG);
        printf("  CTL_CODE(40000,0x841,0,0) = 0x%08X  (MEM ANY BUF)\n", CTL_CODE(40000, 0x841, 0, 0));
        printf("  CTL_CODE(40000,0x841,0,1) = 0x%08X  (MEM READ BUF)\n", CTL_CODE(40000, 0x841, 0, 1));
    }

    /* SMU Diagnostics */
    printf("\n--- SMU Diagnostics ---\n");
    if (hwsense_smu_diag(ctx, smu_name, sizeof(smu_name), &smu_ver, &pm_ver, &dram_base)) {
        printf("  CPU Codename:     %s\n", smu_name);
        printf("  SMU Version:      0x%08X\n", smu_ver);
        printf("  PM Table Version: 0x%08X\n", pm_ver);
        printf("  DRAM Base:        0x%016llX\n", dram_base);
    } else {
        printf("  SMU diag failed (not AMD or unsupported CPU)\n");
    }

    /* Raw mailbox register check */
    printf("\n--- Mailbox Register Check ---\n");
    {
        unsigned int raw_val;
        /* MP1 registers for Renoir */
        if (hwsense_read_smn_diag(ctx, 0x3B10528, &raw_val))
            printf("  MP1 MSG  (0x3B10528): 0x%08X\n", raw_val);
        if (hwsense_read_smn_diag(ctx, 0x3B10564, &raw_val))
            printf("  MP1 RSP  (0x3B10564): 0x%08X\n", raw_val);
        if (hwsense_read_smn_diag(ctx, 0x3B10998, &raw_val))
            printf("  MP1 ARG0 (0x3B10998): 0x%08X\n", raw_val);
        /* RSMU registers for Renoir */
        if (hwsense_read_smn_diag(ctx, 0x3B10A20, &raw_val))
            printf("  RSMU MSG (0x3B10A20): 0x%08X\n", raw_val);
        if (hwsense_read_smn_diag(ctx, 0x3B10A80, &raw_val))
            printf("  RSMU RSP (0x3B10A80): 0x%08X\n", raw_val);
        if (hwsense_read_smn_diag(ctx, 0x3B10A88, &raw_val))
            printf("  RSMU ARG0(0x3B10A88): 0x%08X\n", raw_val);
        /* Try Matisse registers too */
        if (hwsense_read_smn_diag(ctx, 0x3B10530, &raw_val))
            printf("  MAT MSG  (0x3B10530): 0x%08X\n", raw_val);
        if (hwsense_read_smn_diag(ctx, 0x3B1057C, &raw_val))
            printf("  MAT RSP  (0x3B1057C): 0x%08X\n", raw_val);
    }

    hwsense_shutdown(ctx);
    return (pkg.ok) ? 0 : 1;
}
