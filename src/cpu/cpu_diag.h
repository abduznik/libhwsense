/*
 * cpu_diag.h — CPU diagnostic tools API.
 */

#ifndef CPU_DIAG_H
#define CPU_DIAG_H

#include "../../include/hwsense.h"

/* Detect CPU and features */
HWSENSE_API cpu_diag_result_t cpu_diag_detect(void);

/* Print diagnostic report */
HWSENSE_API void cpu_diag_print(cpu_diag_result_t *diag);

#endif /* CPU_DIAG_H */
