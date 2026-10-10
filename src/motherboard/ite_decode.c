/*
 * ite_decode.c — ITE Super I/O monitor register decoding.
 *
 * Register layout and conversion formulas follow the it87 hwmon driver
 * (frankcrawford/it87, GPL-2.0). Chip parameters come from its per-chip
 * feature table:
 *   - ADC LSB: 10.9 mV (IT8655E, IT8665E) or 12 mV (the others)
 *   - 16-bit tachometers, RPM = 1350000 / (count * 2)
 *   - fan and temperature input counts
 *
 * Verified on real hardware for IT8655E only (ASUS PRIME B450M-K, Linux).
 * The other chips use the same layout per the driver source but are untested.
 */

#include "ite_decode.h"
#include <stdio.h>
#include <string.h>

#define ITE_REG_BANK 0x06     /* bits 7:5 = register bank */

typedef struct {
    unsigned short id;
    const char *name;
    int adc_lsb_x10;   /* mV per ADC step * 10: 109 = 10.9 mV, 120 = 12 mV */
    int fans;
    int temps;
} ite_chip_t;

static const ite_chip_t ite_chips[] = {
    { 0x8655, "it8655", 109, 3, 6 },
    { 0x8665, "it8665", 109, 6, 6 },
    { 0x8686, "it8686", 120, 6, 6 },
    { 0x8628, "it8628", 120, 6, 6 },
    { 0x8688, "it8688", 120, 6, 6 },
    { 0x8689, "it8689", 120, 6, 6 },
    { 0x8728, "it8728", 120, 5, 3 },
};

static const unsigned char ite_fan_lo[] = { 0x0d, 0x0e, 0x0f, 0x80, 0x82, 0x93 };
static const unsigned char ite_fan_hi[] = { 0x18, 0x19, 0x1a, 0x81, 0x83, 0x94 };
static const unsigned char ite_vin[]    = { 0x20, 0x21, 0x22, 0x23, 0x24,
                                            0x25, 0x26, 0x27, 0x28, 0x2f };
/* in7 = VSB, in8 = Vbat, in9 = AVCC3: divided on-chip, so the ADC LSB doubles. */
#define ITE_VIN_SCALED_MASK ((1 << 7) | (1 << 8) | (1 << 9))

static const ite_chip_t *find_chip(unsigned short id)
{
    size_t i;
    for (i = 0; i < sizeof(ite_chips) / sizeof(ite_chips[0]); i++)
        if (ite_chips[i].id == id)
            return &ite_chips[i];
    return NULL;
}

const char *hwsense_ite_chip_name(unsigned short id)
{
    const ite_chip_t *c = find_chip(id);
    return c ? c->name : NULL;
}

int hwsense_ite_decode(unsigned short id, ite_read_fn rd, ite_write_fn wr,
                       void *ctx, hwsense_superio_result_t *r)
{
    const ite_chip_t *chip = find_chip(id);
    int i, saved_bank;

    if (!chip) {
        snprintf(r->error, sizeof(r->error), "unsupported ITE chip 0x%04x", id);
        return 0;
    }

    /*
     * Registers below 0x100 live in bank 0. Another driver (or firmware) can
     * leave the chip in a different bank, so switch to 0 for the reads and
     * restore the original bank afterwards, as the it87 driver does.
     */
    saved_bank = rd(ctx, ITE_REG_BANK);
    if (saved_bank < 0) {
        snprintf(r->error, sizeof(r->error), "cannot read ITE bank register");
        return 0;
    }
    if (saved_bank & 0x60)
        wr(ctx, ITE_REG_BANK, saved_bank & ~0x60);

    for (i = 0; i < chip->temps && r->count < HWSENSE_MAX_SUPERIO_TEMPS; i++) {
        int t = rd(ctx, 0x29 + i);
        if (t < 0)
            continue;
        t = (signed char)t;
        if (t == 0 || t == -128)       /* unconnected input */
            continue;
        r->temperatures[r->count++] = t;
    }

    for (i = 0; i < chip->fans && r->fan_count < HWSENSE_MAX_SUPERIO_FANS; i++) {
        int lo = rd(ctx, ite_fan_lo[i]);
        int hi = rd(ctx, ite_fan_hi[i]);
        int raw;
        if (lo < 0 || hi < 0)
            continue;
        raw = lo | (hi << 8);
        if (raw == 0 || raw == 0xffff)     /* no tach signal */
            continue;
        r->fan_rpms[r->fan_count++] = 1350000 / (raw * 2);
    }

    for (i = 0; i < (int)sizeof(ite_vin) && r->voltage_count < HWSENSE_MAX_SUPERIO_VOLTAGES; i++) {
        int v = rd(ctx, ite_vin[i]);
        int lsb = chip->adc_lsb_x10;
        if (v < 0)
            continue;
        if (ITE_VIN_SCALED_MASK & (1 << i))
            lsb *= 2;
        r->voltages[r->voltage_count++] = v * lsb / 10000.0;
    }

    if (saved_bank & 0x60)
        wr(ctx, ITE_REG_BANK, saved_bank);

    snprintf(r->chip_name, sizeof(r->chip_name), "%s", chip->name);
    r->chip_id = id;
    r->ok = r->count || r->fan_count || r->voltage_count;
    if (!r->ok)
        snprintf(r->error, sizeof(r->error), "ITE chip %s returned no readings", chip->name);
    return r->ok;
}
