#ifndef HWSENSE_NCT_LINUX_H
#define HWSENSE_NCT_LINUX_H

#include "../../include/hwsense.h"

/* Register read callback: returns the byte at (bank<<8 | index), or -1. */
typedef int (*nct_read_fn)(void *ctx, unsigned reg);

/* Chip name for a Super I/O device ID, or NULL if unsupported. */
const char *hwsense_nct_chip_name(unsigned short id);

/*
 * Decode fans, temperatures and voltages through rd(). Separated from the
 * port I/O so it can be tested against a simulated register file.
 * Returns 1 if anything was read.
 */
int hwsense_nct_decode(unsigned short chip_id, nct_read_fn rd, void *ctx,
                       hwsense_superio_result_t *r);

/* Probe and read a Nuvoton chip over /dev/port. */
int hwsense_nct_read_linux(hwsense_superio_result_t *r);

#endif
