#ifndef HWSENSE_INTERNAL_H
#define HWSENSE_INTERNAL_H

#include "../../include/hwsense.h"

/* Internal struct definition — shared between driver.c and cpu/*.c */
#ifdef _WIN32
struct hwsense_ctx {
    HANDLE driver_handle;     /* WinRing0 device handle */
    SC_HANDLE scm_handle;
    SC_HANDLE svc_handle;
};

/* Forward declarations — Intel CPU functions */
extern double hwsense_intel_package_power(HANDLE driver_handle);
#else
struct hwsense_ctx {
    int vendor;               /* 'I' Intel, 'A' AMD, '?' unknown — cached at init */
};
#endif

#endif /* HWSENSE_INTERNAL_H */
