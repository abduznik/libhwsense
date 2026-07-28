/*
 * cpu_diag.c — CPU diagnostic tools for detecting unsupported processors.
 *
 * Detects CPU vendor, model, family, stepping, and reports supported features.
 * Helps identify why certain sensors might not work on specific CPUs.
 */

#include "../core/hwsense_internal.h"
#include <stdio.h>
#include <intrin.h>

/* CPUID leaf definitions */
#define CPUID_LEAF_VENDOR      0x00000000
#define CPUID_LEAF_FEATURES    0x00000001
#define CPUID_LEAF_EXT_FEATURES 0x80000001
#define CPUID_LEAF_BRAND       0x80000002

/* CPUID feature bits */
#define CPUID_FEATURE_MSR      (1 << 5)    /* EDX bit 5 */
#define CPUID_FEATURE_TSC      (1 << 4)    /* EDX bit 4 */
#define CPUID_FEATURE_HWP      (1 << 10)   /* EAX bit 10 (Hardware P-states) */
#define CPUID_FEATURE_TM       (1 << 29)   /* EDX bit 29 (Thermal Monitor) */
#define CPUID_FEATURE_TM2      (1 << 8)    /* ECX bit 8 (Thermal Monitor 2) */
#define CPUID_FEATURE_RAPL     (1 << 13)   /* EBX bit 13 (RAPL) - Intel specific */
#define CPUID_FEATURE_APERF    (1 << 1)    /* ECX bit 1 (APERF/MPERF) - Intel specific */
#define CPUID_FEATURE_INV_TSC  (1 << 8)    /* EDX bit 8 (Invariant TSC) */

/* AMD-specific CPUID leaves */
#define AMD_CPUID_EXT_FN80000008  0x80000008
#define AMD_CPUID_EXT_FN80000001  0x80000001

/* CPU result structure */
typedef struct {
    int ok;
    char vendor[16];          /* "GenuineIntel" or "AuthenticAMD" */
    char brand[64];           /* CPU brand string */
    int family;               /* CPU family */
    int model;                /* CPU model */
    int stepping;             /* CPU stepping */
    int ext_family;           /* Extended family */
    int ext_model;            /* Extended model */
    int cores;                /* Physical cores */
    int threads;              /* Logical threads */
    int max_threads_per_core; /* Max threads per core */
    int tsc_invariant;        /* Invariant TSC */
    int rapl_support;         /* RAPL support */
    int hwp_support;          /* Hardware P-states */
    int aperf_mperf;          /* APERF/MPERF support */
    int msr_support;          /* MSR access support */
    int thermal_monitor;      /* Thermal Monitor */
    int family_known;         /* Known family for sensor support */
    char supported_sensors[256]; /* List of supported sensors */
    char warnings[512];       /* Warnings about unsupported features */
    char error[256];
} cpu_diag_result_t;

/*
 * Run CPUID instruction.
 */
static void run_cpuid(int leaf, int subleaf, int *eax, int *ebx, int *ecx, int *edx)
{
    int cpuinfo[4];
    __cpuidex(cpuinfo, leaf, subleaf);
    *eax = cpuinfo[0];
    *ebx = cpuinfo[1];
    *ecx = cpuinfo[2];
    *edx = cpuinfo[3];
}

/*
 * Get CPU brand string via CPUID.
 */
static void get_cpu_brand(char *brand, int size)
{
    int *p = (int *)brand;
    run_cpuid(0x80000002, 0, &p[0], &p[1], &p[2], &p[3]);
    run_cpuid(0x80000003, 0, &p[4], &p[5], &p[6], &p[7]);
    run_cpuid(0x80000004, 0, &p[8], &p[9], &p[10], &p[11]);
    brand[size - 1] = '\0';
}

/*
 * Detect CPU vendor and features.
 */
cpu_diag_result_t cpu_diag_detect(void)
{
    cpu_diag_result_t result = {0};

    /* Get vendor string */
    int eax, ebx, ecx, edx;
    run_cpuid(CPUID_LEAF_VENDOR, 0, &eax, &ebx, &ecx, &edx);

    /* Vendor is in EBX, EDX, ECX */
    int *vendor = (int *)result.vendor;
    vendor[0] = ebx;
    vendor[1] = edx;
    vendor[2] = ecx;
    vendor[3] = 0;

    /* Get brand string */
    get_cpu_brand(result.brand, sizeof(result.brand));

    /* Get feature flags */
    run_cpuid(CPUID_LEAF_FEATURES, 0, &eax, &ebx, &ecx, &edx);

    /* Decode family/model/stepping */
    result.stepping = eax & 0xF;
    result.model = (eax >> 4) & 0xF;
    result.family = (eax >> 8) & 0xF;
    result.ext_model = (eax >> 16) & 0xF;
    result.ext_family = (eax >> 20) & 0xFF;

    /* Compute actual family and model */
    int actual_family = result.family;
    int actual_model = result.model;

    if (result.family == 0x0F)
        actual_family += result.ext_family;
    if (result.family == 0x06 || result.family == 0x0F)
        actual_model += (result.ext_model << 4);

    result.family = actual_family;
    result.model = actual_model;

    /* Get thread count */
    int max_logical = ((eax >> 16) & 0xFF) + 1;
    int cores_per_package = ((ebx >> 16) & 0xFF) + 1;
    result.threads = max_logical;
    result.cores = cores_per_package;
    result.max_threads_per_core = max_logical / cores_per_package;

    /* Check feature flags */
    result.msr_support = (edx & CPUID_FEATURE_MSR) ? 1 : 0;
    result.tsc_invariant = (edx & CPUID_FEATURE_INV_TSC) ? 1 : 0;
    result.thermal_monitor = (edx & CPUID_FEATURE_TM) ? 1 : 0;
    result.hwp_support = (eax & CPUID_FEATURE_HWP) ? 1 : 0;
    result.aperf_mperf = (ecx & CPUID_FEATURE_APERF) ? 1 : 0;

    /* Check Intel-specific features */
    if (strstr(result.vendor, "Intel")) {
        /* RAPL support via CPUID.06H:ECX.RAPL */
        int cpuid_eax, cpuid_ebx, cpuid_ecx, cpuid_edx;
        run_cpuid(0x06, 0, &cpuid_eax, &cpuid_ebx, &cpuid_ecx, &cpuid_edx);
        result.rapl_support = (cpuid_ecx >> 13) & 1;
    }

    /* Determine if family is known for sensor support */
    result.family_known = 0;
    if (strstr(result.vendor, "Intel")) {
        /* Intel families: 6 (Core), 7 (Atom), etc. */
        if (result.family == 6)
            result.family_known = 1;
    } else if (strstr(result.vendor, "AMD")) {
        /* AMD families: 15h (Bulldozer), 17h (Zen), 19h (Zen3) */
        if (result.family == 15 || result.family == 17 || result.family == 19 || result.family == 25)
            result.family_known = 1;
    }

    /* Build supported sensors list */
    char *p = result.supported_sensors;
    int remaining = sizeof(result.supported_sensors);

    if (result.msr_support) {
        int n = snprintf(p, remaining, "MSR ");
        p += n; remaining -= n;
    }
    if (result.thermal_monitor) {
        int n = snprintf(p, remaining, "Thermal ");
        p += n; remaining -= n;
    }
    if (result.rapl_support) {
        int n = snprintf(p, remaining, "RAPL ");
        p += n; remaining -= n;
    }
    if (result.hwp_support) {
        int n = snprintf(p, remaining, "HWP ");
        p += n; remaining -= n;
    }
    if (result.aperf_mperf) {
        int n = snprintf(p, remaining, "APERF/MPERF ");
        p += n; remaining -= n;
    }

    /* Build warnings */
    p = result.warnings;
    remaining = sizeof(result.warnings);

    if (!result.family_known) {
        int n = snprintf(p, remaining, "CPU family 0x%X model 0x%X may not be fully supported; ", result.family, result.model);
        p += n; remaining -= n;
    }
    if (!result.rapl_support && strstr(result.vendor, "Intel")) {
        int n = snprintf(p, remaining, "RAPL not supported - power readings unavailable; ");
        p += n; remaining -= n;
    }
    if (!result.thermal_monitor) {
        int n = snprintf(p, remaining, "Thermal Monitor not supported; ");
        p += n; remaining -= n;
    }

    result.ok = 1;
    return result;
}

/*
 * Print CPU diagnostic report.
 */
void cpu_diag_print(cpu_diag_result_t *diag)
{
    if (!diag || !diag->ok) {
        printf("CPU Diagnostics: FAILED\n");
        return;
    }

    printf("=== CPU Diagnostics ===\n");
    printf("  Vendor:           %s\n", diag->vendor);
    printf("  Brand:            %s\n", diag->brand);
    printf("  Family:           0x%X\n", diag->family);
    printf("  Model:            0x%X\n", diag->model);
    printf("  Stepping:         %d\n", diag->stepping);
    printf("  Cores:            %d\n", diag->cores);
    printf("  Threads:          %d\n", diag->threads);
    printf("  Supported:        %s\n", diag->family_known ? "Yes" : "Unknown");
    printf("\n");

    printf("--- Feature Flags ---\n");
    printf("  MSR Access:       %s\n", diag->msr_support ? "Yes" : "No");
    printf("  Thermal Monitor:  %s\n", diag->thermal_monitor ? "Yes" : "No");
    printf("  RAPL:             %s\n", diag->rapl_support ? "Yes" : "No");
    printf("  HWP:              %s\n", diag->hwp_support ? "Yes" : "No");
    printf("  APERF/MPERF:      %s\n", diag->aperf_mperf ? "Yes" : "No");
    printf("  Invariant TSC:    %s\n", diag->tsc_invariant ? "Yes" : "No");
    printf("\n");

    printf("--- Supported Sensors ---\n");
    printf("  %s\n", diag->supported_sensors[0] ? diag->supported_sensors : "None detected");
    printf("\n");

    if (diag->warnings[0]) {
        printf("--- Warnings ---\n");
        printf("  %s\n", diag->warnings);
        printf("\n");
    }
}
