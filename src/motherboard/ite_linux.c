/*
 * ite_linux.c — Direct ITE Super I/O hardware-monitor reader for Linux.
 *
 * Lets libhwsense report fans, board temperatures and voltages with no
 * kernel hwmon driver loaded (the stock kernel has no it87 support for
 * IT8655E, for example). Port I/O goes through /dev/port, which the kernel
 * only opens with CAP_SYS_RAWIO (root, or `setcap cap_sys_rawio+ep` on the
 * binary) -- file permissions or ACLs alone are not enough. No kernel module
 * is needed.
 *
 * Register decoding lives in ite_decode.c (shared with the Windows reader).
 * The access sequence here is:
 *   1. Enter ITE config mode on 0x2E (or 0x4E): 0x87, 0x01, 0x55, 0x55/0xAA
 *   2. Read chip ID from config regs 0x20/0x21
 *   3. Select the environment-controller logical device (LDN 4), read its
 *      base address from regs 0x60/0x61, exit config mode
 *   4. Read the monitor registers through base+5 (index) / base+6 (data)
 *
 * Read-only apart from temporarily selecting register bank 0 (restored after).
 * Other writes go only to index/config ports. Unlike the kernel driver this
 * does not switch the chip's SMBus bridge off during a read nor serialise
 * against the firmware, so a read can very rarely race with ACPI.
 *
 * Verified on IT8655E (ASUS PRIME B450M-K).
 */

#ifndef _WIN32

#include "../core/hwsense_internal.h"
#include "ite_decode.h"
#include "portio_linux.h"

static int cfg_read(unsigned ioreg, int reg)
{
    hwsense_outb(ioreg, (unsigned char)reg);
    return hwsense_inb(ioreg + 1);
}

static int ec_read(void *ctx, unsigned reg)
{
    unsigned base = *(unsigned *)ctx;
    hwsense_outb(base + ITE_EC_INDEX_OFF, (unsigned char)reg);
    return hwsense_inb(base + ITE_EC_DATA_OFF);
}

static void ec_write(void *ctx, unsigned reg, int val)
{
    unsigned base = *(unsigned *)ctx;
    hwsense_outb(base + ITE_EC_INDEX_OFF, (unsigned char)reg);
    hwsense_outb(base + ITE_EC_DATA_OFF, (unsigned char)val);
}

/* Find the chip and its monitor base address. Returns 1 on success. */
static int ite_find(unsigned short *chip_id, unsigned *base)
{
    static const unsigned ioregs[] = { 0x2e, 0x4e };
    size_t i;

    for (i = 0; i < sizeof(ioregs) / sizeof(ioregs[0]); i++) {
        unsigned io = ioregs[i];
        int id, hi, lo, active;

        hwsense_outb(io, 0x87);
        hwsense_outb(io, 0x01);
        hwsense_outb(io, 0x55);
        hwsense_outb(io, io == 0x4e ? 0xaa : 0x55);

        id = (cfg_read(io, ITE_CFG_DEVID) << 8) | cfg_read(io, ITE_CFG_DEVID + 1);

        if (id > 0 && hwsense_ite_chip_name((unsigned short)id)) {
            hwsense_outb(io, ITE_CFG_LDN);
            hwsense_outb(io + 1, ITE_LDN_EC);
            active = cfg_read(io, ITE_CFG_ACTIVE);
            hi = cfg_read(io, ITE_CFG_BASE);
            lo = cfg_read(io, ITE_CFG_BASE + 1);

            /* leave config mode */
            hwsense_outb(io, 0x02);
            hwsense_outb(io + 1, 0x02);

            *base = (unsigned)(((hi << 8) | lo) & ~0x7);
            if (!(active & 1) || *base == 0 || *base == 0xfff8)
                return 0;
            *chip_id = (unsigned short)id;
            return 1;
        }

        hwsense_outb(io, 0x02);
        hwsense_outb(io + 1, 0x02);
    }
    return 0;
}

int hwsense_ite_read_linux(hwsense_superio_result_t *r)
{
    unsigned short id;
    unsigned base;
    int ok;

    memset(r, 0, sizeof(*r));

    if (!hwsense_port_open(r->error, sizeof(r->error)))
        return 0;

    if (!ite_find(&id, &base)) {
        snprintf(r->error, sizeof(r->error),
                 "no supported ITE Super I/O chip found (IT8655E, IT8665E, IT8686E, IT8628E, IT8688E, IT8689E, IT8728F)");
        hwsense_port_close();
        return 0;
    }

    ok = hwsense_ite_decode(id, ec_read, ec_write, &base, r);
    hwsense_port_close();
    return ok;
}

#endif /* !_WIN32 */
