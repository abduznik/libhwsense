/*
 * nct_linux.c — Direct Nuvoton Super I/O hardware-monitor reader for Linux.
 *
 * Counterpart of ite_linux.c for the NCT6779/6791-6799 family, used when no
 * hwmon driver (nct6775) is loaded. Port I/O goes through /dev/port, which
 * needs CAP_SYS_RAWIO (see portio_linux.h).
 *
 * Register layout, chip IDs and scaling follow the Linux nct6775 driver
 * (drivers/hwmon/nct6775-core.c and nct6775-platform.c, GPL-2.0):
 *   1. Enter config mode on 0x2E/0x4E: write 0x87 twice; exit with 0xAA
 *   2. Chip ID = regs 0x20/0x21 (masked with 0xFFF8, as the kernel does)
 *   3. Select LDN 0x0B (hardware monitor), regs 0x60/0x61 = base address
 *   4. Registers are banked: write the bank to index 0x4E, then the register
 *      to base+5 and read base+6
 *
 * Writes: bank select (restored afterwards), and clearing the HM I/O-space
 * lock bit (config reg 0x28 bit 4) on NCT6791 and newer if the BIOS set it,
 * exactly as the kernel driver does. The kernel avoids direct access on many
 * ASUS boards because the firmware also uses these registers via WMI/ACPI;
 * a read here can very rarely race with it.
 *
 * NOT VERIFIED ON REAL HARDWARE. The decode logic is covered by
 * tests/test_nct.c against a simulated register file built from the
 * kernel-driver tables, but no Nuvoton board was available to test with.
 */

#ifndef _WIN32

#include "../core/hwsense_internal.h"
#include "nct_linux.h"
#include "portio_linux.h"

#define NCT_CFG_LDN     0x07
#define NCT_CFG_DEVID   0x20
#define NCT_CFG_ENABLE  0x30
#define NCT_CFG_ADDR    0x60
#define NCT_CFG_LOCK    0x28      /* NCT6791+: bit 4 locks HM I/O space */
#define NCT_LDN_HWM     0x0b
#define NCT_REG_BANK    0x4e
#define NCT_IO_OFFSET   5

typedef struct {
    unsigned short id;
    const char *name;
    int family;        /* 0 = NCT6779..6797 layout, 1 = NCT6798/6799 */
    int in_num;
} nct_chip_t;

static const nct_chip_t nct_chips[] = {
    { 0xc560, "nct6779", 0, 15 },
    { 0xc800, "nct6791", 0, 15 },
    { 0xc910, "nct6792", 0, 15 },
    { 0xd120, "nct6793", 0, 15 },
    { 0xd350, "nct6795", 0, 15 },
    { 0xd420, "nct6796", 0, 15 },
    { 0xd450, "nct6797", 0, 15 },
    { 0xd428, "nct6798", 1, 15 },
    { 0xd800, "nct6799", 1, 18 },
};

/* Voltage registers (bank 4); LSB scaling is 8 mV, doubled on divided inputs. */
static const unsigned short nct_vin_reg[] = {
    0x480, 0x481, 0x482, 0x483, 0x484, 0x485, 0x486, 0x487, 0x488,
    0x489, 0x48a, 0x48b, 0x48c, 0x48d, 0x48e, 0x48f, 0x470, 0x471,
};
static const unsigned short nct_scale_in[] = {
    800, 800, 1600, 1600, 800, 800, 800, 1600, 1600, 800, 800, 800, 800, 800, 800,
};
static const unsigned short nct_scale_in_6798[] = {
    800, 800, 1600, 1600, 800, 800, 800, 1600, 1600, 1600, 1600, 1600, 800,
    800, 800, 800, 1600, 800,
};

/* 16-bit big-endian tachometer registers, value is RPM directly. */
static const unsigned short nct_fan_reg[] = {
    0x4c0, 0x4c2, 0x4c4, 0x4c6, 0x4c8, 0x4ca, 0x4ce,
};

/* Temperature registers with the register holding their source selector. */
static const unsigned short nct_temp_reg0[]  = { 0x27, 0x150 };
static const unsigned short nct_temp_src0[]  = { 0x621, 0x622 };
static const unsigned short nct_temp_reg1[]  = { 0x27, 0x150, 0x670, 0x672,
                                                 0x674, 0x676, 0x678, 0x67a };
static const unsigned short nct_temp_src1[]  = { 0x621, 0x622, 0xc26, 0xc27,
                                                 0xc28, 0xc29, 0xc2a, 0xc2b };
/* "Monitor" temperatures (fan-control sources) and their selector registers. */
static const unsigned short nct_mon_reg0[]   = { 0x73, 0x75, 0x77, 0x79, 0x7b, 0x7d };
static const unsigned short nct_mon_reg1[]   = { 0x73, 0x75, 0x77, 0x79, 0x7b, 0x7d, 0x4a0 };
static const unsigned short nct_mon_sel[]    = { 0x100, 0x200, 0x300, 0x800,
                                                 0x900, 0xa00, 0xb00 };

/* Registers read as 16 bits (integer degrees in the high byte). */
static int nct_word_reg(unsigned reg)
{
    return reg == 0x150 || (reg & 0xfff0) == 0x4c0 ||
           reg == 0x73 || reg == 0x75 || reg == 0x77 || reg == 0x79 ||
           reg == 0x7b || reg == 0x7d;
}

static int nct_rd(nct_read_fn rd, void *ctx, unsigned reg)
{
    int v = rd(ctx, reg);
    if (v < 0)
        return -1;
    if (nct_word_reg(reg)) {
        int lo = rd(ctx, reg + 1);
        if (lo < 0)
            return -1;
        v = (v << 8) | lo;
    }
    return v;
}

const char *hwsense_nct_chip_name(unsigned short id)
{
    size_t i;
    for (i = 0; i < sizeof(nct_chips) / sizeof(nct_chips[0]); i++)
        if (nct_chips[i].id == (id & 0xfff8))
            return nct_chips[i].name;
    return NULL;
}

int hwsense_nct_decode(unsigned short chip_id, nct_read_fn rd, void *ctx,
                       hwsense_superio_result_t *r)
{
    const nct_chip_t *chip = NULL;
    const unsigned short *treg, *tsrc, *mreg;
    const unsigned short *scale;
    int ntemp, nmon, i;
    unsigned src_seen = 0;
    size_t c;

    for (c = 0; c < sizeof(nct_chips) / sizeof(nct_chips[0]); c++)
        if (nct_chips[c].id == (chip_id & 0xfff8))
            chip = &nct_chips[c];
    if (!chip)
        return 0;

    if (chip->family) {
        treg = nct_temp_reg1; tsrc = nct_temp_src1;
        ntemp = (int)(sizeof(nct_temp_reg1) / sizeof(nct_temp_reg1[0]));
        mreg = nct_mon_reg1;
        nmon = (int)(sizeof(nct_mon_reg1) / sizeof(nct_mon_reg1[0]));
        scale = nct_scale_in_6798;
    } else {
        treg = nct_temp_reg0; tsrc = nct_temp_src0;
        ntemp = (int)(sizeof(nct_temp_reg0) / sizeof(nct_temp_reg0[0]));
        mreg = nct_mon_reg0;
        nmon = (int)(sizeof(nct_mon_reg0) / sizeof(nct_mon_reg0[0]));
        scale = nct_scale_in;
    }

    /*
     * Each temperature register is fed by a selectable source; sources are
     * reported once even if several registers point at the same one.
     */
    for (i = 0; i < ntemp + nmon && r->count < HWSENSE_MAX_SUPERIO_TEMPS; i++) {
        unsigned reg = i < ntemp ? treg[i] : mreg[i - ntemp];
        unsigned sel = i < ntemp ? tsrc[i] : nct_mon_sel[i - ntemp];
        int src = nct_rd(rd, ctx, sel);
        int v, t;

        if (src < 0)
            continue;
        src &= 0x1f;
        if (!src || (src_seen & (1u << src)))
            continue;

        v = nct_rd(rd, ctx, reg);
        if (v < 0)
            continue;
        t = nct_word_reg(reg) ? (signed char)(v >> 8) : (signed char)v;
        if (t <= -55 || t >= 125 || t == 0)    /* unconnected / invalid */
            continue;
        src_seen |= 1u << src;
        r->temperatures[r->count++] = t;
    }

    for (i = 0; i < (int)(sizeof(nct_fan_reg) / sizeof(nct_fan_reg[0])) &&
                r->fan_count < HWSENSE_MAX_SUPERIO_FANS; i++) {
        int rpm = nct_rd(rd, ctx, nct_fan_reg[i]);
        if (rpm <= 0 || rpm == 0xffff)
            continue;
        r->fan_rpms[r->fan_count++] = rpm;
    }

    for (i = 0; i < chip->in_num && r->voltage_count < HWSENSE_MAX_SUPERIO_VOLTAGES; i++) {
        int raw = nct_rd(rd, ctx, nct_vin_reg[i]);
        if (raw < 0)
            continue;
        r->voltages[r->voltage_count++] = raw * scale[i] / 100000.0;
    }

    snprintf(r->chip_name, sizeof(r->chip_name), "%s", chip->name);
    r->chip_id = chip_id;
    r->ok = r->count || r->fan_count || r->voltage_count;
    if (!r->ok)
        snprintf(r->error, sizeof(r->error), "Nuvoton %s returned no readings", chip->name);
    return r->ok;
}

/* ── Hardware access ─────────────────────────────────────────────────── */

typedef struct {
    unsigned base;
    int bank;       /* bank last selected, -1 = unknown */
} nct_hw_t;

static int nct_hw_read(void *ctx, unsigned reg)
{
    nct_hw_t *hw = ctx;
    int bank = (reg >> 8) & 0xff;

    if (hw->bank != bank) {
        hwsense_outb(hw->base + NCT_IO_OFFSET, NCT_REG_BANK);
        hwsense_outb(hw->base + NCT_IO_OFFSET + 1, (unsigned char)bank);
        hw->bank = bank;
    }
    hwsense_outb(hw->base + NCT_IO_OFFSET, (unsigned char)(reg & 0xff));
    return hwsense_inb(hw->base + NCT_IO_OFFSET + 1);
}

static int cfg_read(unsigned io, int reg)
{
    hwsense_outb(io, (unsigned char)reg);
    return hwsense_inb(io + 1);
}

static void cfg_write(unsigned io, int reg, int val)
{
    hwsense_outb(io, (unsigned char)reg);
    hwsense_outb(io + 1, (unsigned char)val);
}

static int nct_find(unsigned short *chip_id, unsigned *base)
{
    static const unsigned ioregs[] = { 0x2e, 0x4e };
    size_t i;

    for (i = 0; i < sizeof(ioregs) / sizeof(ioregs[0]); i++) {
        unsigned io = ioregs[i];
        int id, active, hi, lo;

        hwsense_outb(io, 0x87);
        hwsense_outb(io, 0x87);

        id = (cfg_read(io, NCT_CFG_DEVID) << 8) | cfg_read(io, NCT_CFG_DEVID + 1);
        if (id < 0 || !hwsense_nct_chip_name((unsigned short)id)) {
            hwsense_outb(io, 0xaa);
            continue;
        }

        cfg_write(io, NCT_CFG_LDN, NCT_LDN_HWM);
        active = cfg_read(io, NCT_CFG_ENABLE);
        hi = cfg_read(io, NCT_CFG_ADDR);
        lo = cfg_read(io, NCT_CFG_ADDR + 1);

        /* NCT6791+: the BIOS may lock the HM I/O space; the kernel clears it. */
        if ((id & 0xfff8) >= 0xc800) {
            int lock = cfg_read(io, NCT_CFG_LOCK);
            if (lock >= 0 && (lock & 0x10))
                cfg_write(io, NCT_CFG_LOCK, lock & ~0x10);
        }

        hwsense_outb(io, 0xaa);

        *base = (unsigned)(((hi << 8) | lo) & ~0x7);
        if (!(active & 1) || *base == 0 || *base == 0xfff8)
            return 0;
        *chip_id = (unsigned short)id;
        return 1;
    }
    return 0;
}

int hwsense_nct_read_linux(hwsense_superio_result_t *r)
{
    nct_hw_t hw;
    unsigned short id;
    int saved_bank, ok;

    memset(r, 0, sizeof(*r));

    if (!hwsense_port_open(r->error, sizeof(r->error)))
        return 0;

    if (!nct_find(&id, &hw.base)) {
        snprintf(r->error, sizeof(r->error),
                 "no supported Nuvoton Super I/O chip found (NCT6779, NCT6791-NCT6799)");
        hwsense_port_close();
        return 0;
    }

    hwsense_outb(hw.base + NCT_IO_OFFSET, NCT_REG_BANK);
    saved_bank = hwsense_inb(hw.base + NCT_IO_OFFSET + 1);
    hw.bank = saved_bank;

    ok = hwsense_nct_decode(id, nct_hw_read, &hw, r);

    if (saved_bank >= 0 && hw.bank != saved_bank) {
        hwsense_outb(hw.base + NCT_IO_OFFSET, NCT_REG_BANK);
        hwsense_outb(hw.base + NCT_IO_OFFSET + 1, (unsigned char)saved_bank);
    }
    hwsense_port_close();
    return ok;
}

#endif /* !_WIN32 */
