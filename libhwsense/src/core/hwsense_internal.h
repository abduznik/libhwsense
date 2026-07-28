#ifndef HWSENSE_INTERNAL_H
#define HWSENSE_INTERNAL_H

#include "../../include/hwsense.h"

/* Internal struct definition — shared between driver.c and cpu/*.c */
struct hwsense_ctx {
    HANDLE driver_handle;     /* WinRing0 device handle */
    SC_HANDLE scm_handle;
    SC_HANDLE svc_handle;
};

#endif /* HWSENSE_INTERNAL_H */
