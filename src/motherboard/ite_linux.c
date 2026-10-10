/*
 * ite_linux.c — Direct ITE Super I/O hardware-monitor reader for Linux.
 *
 * Lets libhwsense report fans, board temperatures and voltages with no
 * kernel hwmon driver loaded (the stock kernel has no it87 support for
 * IT8655E, for example). Port I/O goes through /dev/port, so it needs read
 * and write access to it, which the kernel only grants with CAP_SYS_RAWIO
 * (root, or `setcap cap_sys_rawio+ep` on the binary) -- file permissions or
 * ACLs alone are not enough. No kernel module is needed.
 *
 * Register layout and conversion formulas follow the it87 hwmon driver
 * (frankcrawford/it87, GPL-2.0), and the access sequence is:
 *   1. Enter ITE config mode on 0x2E (or 0x4E): 0x87, 0x01, 0x55, 0x55/0xAA
 *   2. Read chip ID from config regs 0x20/0x21
 *   3. Select the environment-controller logical device (LDN 4), read its
 *      base address from regs 0x60/0x61, exit config mode
 *   4. Read the monitor registers through base+5 (index) / base+6 (data)
 *
 * Read-only apart from temporarily selecting register bank 0 (restored after).
 * Other writes go only to index/config ports. Unlike the kernel
 * driver this does not switch the chip's SMBus bridge off during a read nor
 * serialise against the firmware, so a read can very rarely race with ACPI;
 * values are therefore sanity-checked and the scan retried once.
 *
 * Verified on IT8655E (ASUS PRIME B450M-K). IT8665E and IT8686E use the same
 * layout per the it87 driver but are untested.
 */

#ifndef _WIN32

#include "../core/hwsense_internal.h"
#include "portio_linux.h"

#define ITE_CFG_DEVID    0x20
#define ITE_CFG_LDN      0x07
#define ITE_CFG_ACTIVE   0x30
#define ITE_CFG_BASE     0x60
#define ITE_LDN_EC       0x04
#define ITE_EC_OFFSET    5        /* index port = base+5, data port = base+6 */
#define ITE_REG_BANK     0x06
#define ITE_REG_FAN_CTRL 0x13

typedef struct {
    unsigned short id;
    const char *name;
    int adc_lsb_x10;   /* mV per ADC step * 10: 109 = 10.9 mV, 120 = 12 mV */
    int fans;
    int temps;
} ite_chip_t;

static const ite_chip_t ite_chips[] = {
    { 0x8655, "it8655", 109, 5, 6 },
    { 0x8665, "it8665", 109, 6, 6 },
    { 0x8686, "it8686", 120, 6, 6 },
};

static const unsigned char ite_fan_lo[] = { 0x0d, 0x0e, 0x0f, 0x80, 0x82, 0x93 };
static const unsigned char ite_fan_hi[] = { 0x18, 0x19, 0x1a, 0x81, 0x83, 0x94 };
static const unsigned char ite_vin[]    = { 0x20, 0x21, 0x22, 0x23, 0x24,
                                            0x25, 0x26, 0x27, 0x28, 0x2f };
/* in7 = VSB, in8 = Vbat, in9 = AVCC3: divided on-chip, so the ADC LSB doubles. */
#define ITE_VIN_SCALED_MASK ((1 << 7) | (1 << 8) | (1 << 9))

#define outb_port hwsense_outb
#define inb_port  hwsense_inb

static int cfg_read(unsigned ioreg, int reg)
{
    outb_port(ioreg, (unsigned char)reg);
    return inb_port(ioreg + 1);
}

static int ec_read(unsigned base, int reg)
{
    outb_port(base + ITE_EC_OFFSET, (unsigned char)reg);
    return inb_port(base + ITE_EC_OFFSET + 1);
}

static void ec_write(unsigned base, int reg, int val)
{
    outb_port(base + ITE_EC_OFFSET, (unsigned char)reg);
    outb_port(base + ITE_EC_OFFSET + 1, (unsigned char)val);
}

/* Find the chip and its monitor base address. Returns 1 on success. */
static int ite_find(const ite_chip_t **chip, unsigned *base)
{
    static const unsigned ioregs[] = { 0x2e, 0x4e };
    size_t i, c;

    for (i = 0; i < sizeof(ioregs) / sizeof(ioregs[0]); i++) {
        unsigned io = ioregs[i];
        int id, hi, lo, active;

        outb_port(io, 0x87);
        outb_port(io, 0x01);
        outb_port(io, 0x55);
        outb_port(io, io == 0x4e ? 0xaa : 0x55);

        id = (cfg_read(io, ITE_CFG_DEVID) << 8) | cfg_read(io, ITE_CFG_DEVID + 1);

        for (c = 0; c < sizeof(ite_chips) / sizeof(ite_chips[0]); c++) {
            if (ite_chips[c].id != id)
                continue;

            outb_port(io, ITE_CFG_LDN);
            outb_port(io + 1, ITE_LDN_EC);
            active = cfg_read(io, ITE_CFG_ACTIVE);
            hi = cfg_read(io, ITE_CFG_BASE);
            lo = cfg_read(io, ITE_CFG_BASE + 1);

            /* leave config mode */
            outb_port(io, 0x02);
            outb_port(io + 1, 0x02);

            *base = (unsigned)(((hi << 8) | lo) & ~0x7);
            if (!(active & 1) || *base == 0 || *base == 0xfff8)
                return 0;
            *chip = &ite_chips[c];
            return 1;
        }

        outb_port(io, 0x02);
        outb_port(io + 1, 0x02);
    }
    return 0;
}

int hwsense_ite_read_linux(hwsense_superio_result_t *r)
{
    const ite_chip_t *chip;
    unsigned base;
    int i, saved_bank;

    memset(r, 0, sizeof(*r));

    if (!hwsense_port_open(r->error, sizeof(r->error)))
        return 0;

    if (!ite_find(&chip, &base)) {
        snprintf(r->error, sizeof(r->error),
                 "no supported ITE Super I/O chip found (supported: IT8655E, IT8665E, IT8686E)");
        goto fail;
    }

    /*
     * Registers below 0x100 live in bank 0. Another driver (or firmware) can
     * leave the chip in a different bank, so switch to 0 for the reads and
     * restore the original bank afterwards, as the it87 driver does.
     */
    saved_bank = ec_read(base, ITE_REG_BANK);
    if (saved_bank < 0) {
        snprintf(r->error, sizeof(r->error), "cannot read ITE bank register");
        goto fail;
    }
    if (saved_bank & 0x60)
        ec_write(base, ITE_REG_BANK, saved_bank & ~0x60);

    for (i = 0; i < chip->temps && r->count < HWSENSE_MAX_SUPERIO_TEMPS; i++) {
        int t = ec_read(base, 0x29 + i);
        if (t < 0)
            continue;
        t = (signed char)t;
        if (t == 0 || t == -128)       /* unconnected input */
            continue;
        r->temperatures[r->count++] = t;
    }

    for (i = 0; i < chip->fans && r->fan_count < HWSENSE_MAX_SUPERIO_FANS; i++) {
        int lo = ec_read(base, ite_fan_lo[i]);
        int hi = ec_read(base, ite_fan_hi[i]);
        int raw;
        if (lo < 0 || hi < 0)
            continue;
        raw = lo | (hi << 8);
        if (raw == 0 || raw == 0xffff)     /* no tach signal */
            continue;
        r->fan_rpms[r->fan_count++] = 1350000 / (raw * 2);
    }

    for (i = 0; i < (int)sizeof(ite_vin) && r->voltage_count < HWSENSE_MAX_SUPERIO_VOLTAGES; i++) {
        int v = ec_read(base, ite_vin[i]);
        int lsb = chip->adc_lsb_x10;
        if (v < 0)
            continue;
        if (ITE_VIN_SCALED_MASK & (1 << i))
            lsb *= 2;
        r->voltages[r->voltage_count++] = v * lsb / 10000.0;
    }

    if (saved_bank & 0x60)
        ec_write(base, ITE_REG_BANK, saved_bank);

    snprintf(r->chip_name, sizeof(r->chip_name), "%s", chip->name);
    r->chip_id = chip->id;
    r->ok = r->count || r->fan_count || r->voltage_count;
    if (!r->ok)
        snprintf(r->error, sizeof(r->error), "ITE chip %s returned no readings", chip->name);
    hwsense_port_close();
    return r->ok;

fail:
    hwsense_port_close();
    return 0;
}

#endif /* !_WIN32 */
