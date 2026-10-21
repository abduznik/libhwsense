#ifndef HWSENSE_ITE_DECODE_H
#define HWSENSE_ITE_DECODE_H

#include "../../include/hwsense.h"

/*
 * ITE Super I/O hardware-monitor decoding, shared by the Linux (/dev/port)
 * and Windows (WinRing0) readers. Only the register layout and conversion
 * formulas live here; the callers do the port I/O.
 *
 * Register offsets are the monitor's own index space, reached through the
 * chip's base address (LDN 4, config regs 0x60/0x61): write the index to
 * base+5, then read or write the value at base+6.
 */

/* Read/write one monitor register. rd returns the byte, or -1 on failure. */
typedef int  (*ite_read_fn)(void *ctx, unsigned reg);
typedef void (*ite_write_fn)(void *ctx, unsigned reg, int value);

/* ITE config-space constants, used by the callers' discovery code. */
#define ITE_CFG_DEVID    0x20     /* chip ID, 2 bytes */
#define ITE_CFG_LDN      0x07
#define ITE_CFG_ACTIVE   0x30
#define ITE_CFG_BASE     0x60     /* logical device base address, 2 bytes */
#define ITE_LDN_EC       0x04     /* environment controller (hardware monitor) */
#define ITE_EC_INDEX_OFF 5        /* index port = base + 5 */
#define ITE_EC_DATA_OFF  6        /* data port  = base + 6 */

/* Chip name for a Super I/O device ID, or NULL if this reader does not support it. */
const char *hwsense_ite_chip_name(unsigned short id);

/*
 * Read fans, temperatures and voltages of chip `id` into *r. Switches the
 * monitor to register bank 0 for the reads and restores the original bank.
 * Returns 1 if anything was read; on failure r->error says why.
 */
int hwsense_ite_decode(unsigned short id, ite_read_fn rd, ite_write_fn wr,
                       void *ctx, hwsense_superio_result_t *r);

#endif
