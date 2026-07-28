/*
 * cpu_diag.h — CPU diagnostic tools API.
 */

#ifndef CPU_DIAG_H
#define CPU_DIAG_H

/* CPU diagnostic result structure */
typedef struct {
    int ok;
    char vendor[16];
    char brand[64];
    int family;
    int model;
    int stepping;
    int cores;
    int threads;
    int tsc_invariant;
    int rapl_support;
    int hwp_support;
    int aperf_mperf;
    int msr_support;
    int thermal_monitor;
    int family_known;
    char supported_sensors[256];
    char warnings[512];
    char error[256];
} cpu_diag_result_t;

/* Detect CPU and features */
cpu_diag_result_t cpu_diag_detect(void);

/* Print diagnostic report */
void cpu_diag_print(cpu_diag_result_t *diag);

#endif /* CPU_DIAG_H */
