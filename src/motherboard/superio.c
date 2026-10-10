/*
 * superio.c — Super I/O chip detection, temperature, and fan speed reading.
 *
 * Supports Nuvoton NCT6791D/93D/95D/96D/97D/98D/99D, ITE IT8655E/IT8665E/
 * IT8686E/IT8628E/IT8688E/IT8689E/IT8728F, Winbond W83627DHG, and Fintek
 * F71882/F71889 chips.
 *
 * Config space is reached via IO ports 0x2E/0x2F or 0x4E/0x4F. ITE chips are
 * unlocked with 0x87,0x01,0x55,0x55 (0xAA last on 0x4E) and their sensors are
 * read through the hardware monitor's own base address (LDN 4, base+5/base+6);
 * the register decoding is shared with the Linux reader in ite_decode.c.
 */

#include "../core/hwsense_internal.h"
#include "../core/ioctl_codes.h"
#include "ite_decode.h"
#include <stdio.h>
#include <string.h>

/* Super I/O standard IO ports */
#define SIO_ADDR_PORT   0x2E
#define SIO_DATA_PORT   0x2F
#define SIO_ALT_PORT    0x4E
#define SIO_ALT_DATA    0x4F

/* Chip types */
typedef enum {
    SIO_CHIP_UNKNOWN = 0,
    SIO_CHIP_NUVOTON,
    SIO_CHIP_ITE,
    SIO_CHIP_WINBOND,
    SIO_CHIP_FINTEK
} sio_chip_type_t;

typedef struct {
    WORD id;
    sio_chip_type_t type;
    const char *name;
} sio_chip_info_t;

/* Known chip IDs */
static const sio_chip_info_t known_chips[] = {
    /* Nuvoton */
    { 0xC803, SIO_CHIP_NUVOTON, "NCT6791D" },
    { 0xC833, SIO_CHIP_NUVOTON, "NCT6791D" },
    { 0xC911, SIO_CHIP_NUVOTON, "NCT6792D" },
    { 0xC913, SIO_CHIP_NUVOTON, "NCT6792DA" },
    { 0xD121, SIO_CHIP_NUVOTON, "NCT6793D" },
    { 0xD122, SIO_CHIP_NUVOTON, "NCT6793D" },
    { 0xD352, SIO_CHIP_NUVOTON, "NCT6795D" },
    { 0xD423, SIO_CHIP_NUVOTON, "NCT6796D" },
    { 0xD42A, SIO_CHIP_NUVOTON, "NCT6796DR" },
    { 0xD451, SIO_CHIP_NUVOTON, "NCT6797D" },
    { 0xD42B, SIO_CHIP_NUVOTON, "NCT6798D" },
    { 0xD802, SIO_CHIP_NUVOTON, "NCT6799D" },
    { 0xD806, SIO_CHIP_NUVOTON, "NCT6701D" },
    /* Winbond */
    { 0xA020, SIO_CHIP_WINBOND, "W83627DHG" },
    { 0xB070, SIO_CHIP_WINBOND, "W83627DHG-P" },
    /* Fintek */
    { 0x0541, SIO_CHIP_FINTEK, "F71882FG" },
    { 0x0723, SIO_CHIP_FINTEK, "F71889F" },
};

#define KNOWN_CHIPS_COUNT (sizeof(known_chips) / sizeof(known_chips[0]))

/* Fan count registers for Nuvoton NCT679x (13-bit) */
static const WORD nct679x_fan_regs[] = { 0x4B0, 0x4B2, 0x4B4, 0x4B6, 0x4B8, 0x4BA, 0x4CC };

/* Voltage registers for Nuvoton NCT679x */
static const WORD nct679x_voltage_regs[] = {
    0x480, 0x481, 0x482, 0x483, 0x484, 0x485, 0x486, 0x487,
    0x488, 0x489, 0x48A, 0x48B, 0x48C, 0x48D, 0x48E, 0x48F
};

/* Temperature registers for Nuvoton NCT679x */
static const WORD nct679x_temp_regs[] = { 0x073, 0x075, 0x077, 0x079, 0x07B, 0x07D, 0x4A0, 0x4A2 };
static const WORD nct679x_temp_half[] = { 0x074, 0x076, 0x078, 0x07A, 0x07C, 0x07E, 0x49E, 0x4A1 };
static const BYTE nct679x_temp_bits[] = { 7, 7, 7, 7, 7, 7, 6, 7 };

/* Read/Write IO port via WinRing0 */
static BOOL sio_read_port(HANDLE dev, DWORD port, BYTE *out_value)
{
    DWORD in_val = port;
    DWORD out_val = 0;
    DWORD bytes_ret = 0;

    BOOL ok = DeviceIoControl(
        dev, IOCTL_OLS_READ_IO_PORT_BYTE,
        &in_val, sizeof(in_val),
        &out_val, sizeof(out_val),
        &bytes_ret, NULL
    );

    if (!ok)
        return FALSE;

    *out_value = (BYTE)(out_val & 0xFF);
    return TRUE;
}

/*
 * Security validation for IO port write.
 * Returns TRUE if the port is safe to write to.
 */
/*
 * Monitor index/data ports of an ITE chip found during this call. Writes are
 * allowed to exactly these two ports and only while ite_probe_and_read is
 * running (thread-local, cleared on exit).
 */
static __declspec(thread) DWORD sio_ec_index_port = 0;
static __declspec(thread) DWORD sio_ec_data_port = 0;

static BOOL sio_validate_write_port(DWORD port)
{
    /* Block writes to dangerous ports (DMA, PIC, timer, etc.) */
    if (port <= 0x0F) {
        fprintf(stderr, "SECURITY: Blocked write to dangerous port 0x%03X (DMA/PIC)\n", port);
        return FALSE;
    }

    /* Block writes to keyboard controller (can cause system hang) */
    if (port == 0x60 || port == 0x64) {
        fprintf(stderr, "SECURITY: Blocked write to keyboard controller port 0x%03X\n", port);
        return FALSE;
    }

    /* Block writes to CMOS (can corrupt BIOS settings) */
    if (port == 0x70 || port == 0x71) {
        fprintf(stderr, "SECURITY: Blocked write to CMOS port 0x%03X\n", port);
        return FALSE;
    }

    /* Allow the discovered ITE monitor index/data ports (validated in ite_probe_and_read) */
    if (sio_ec_index_port != 0 &&
        (port == sio_ec_index_port || port == sio_ec_data_port))
        return TRUE;

    /* Only allow writes to known Super I/O port ranges */
    if (port != 0x2E && port != 0x2F && port != 0x4E && port != 0x4F) {
        fprintf(stderr, "SECURITY: Blocked write to unknown port 0x%03X\n", port);
        return FALSE;
    }

    return TRUE;
}

static BOOL sio_write_port(HANDLE dev, DWORD port, BYTE value)
{
    /* Validate port before writing */
    if (!sio_validate_write_port(port))
        return FALSE;

    /* WinRing0 expects: DWORD port, BYTE value (padded to DWORD) */
    DWORD inp[2];
    DWORD bytes_ret = 0;

    inp[0] = port;
    inp[1] = (DWORD)value;

    return DeviceIoControl(
        dev, IOCTL_OLS_WRITE_IO_PORT_BYTE,
        inp, sizeof(inp),
        NULL, 0,
        &bytes_ret, NULL
    );
}

/*
 * Enter Super I/O configuration mode.
 */
static BOOL sio_enter(HANDLE dev, DWORD addr_port)
{
    if (!sio_write_port(dev, addr_port, 0x87))
        return FALSE;
    return sio_write_port(dev, addr_port, 0x87);
}

/*
 * Exit Super I/O configuration mode.
 */
static BOOL sio_exit(HANDLE dev, DWORD addr_port)
{
    return sio_write_port(dev, addr_port, 0xAA);
}

/*
 * Read a Super I/O register.
 */
static BOOL sio_read_reg(HANDLE dev, DWORD addr_port, DWORD data_port,
                         BYTE reg, BYTE *out_value)
{
    if (!sio_write_port(dev, addr_port, reg))
        return FALSE;
    return sio_read_port(dev, data_port, out_value);
}

/*
 * Write a Super I/O register.
 */
static BOOL sio_write_reg(HANDLE dev, DWORD addr_port, DWORD data_port,
                          BYTE reg, BYTE value)
{
    if (!sio_write_port(dev, addr_port, reg))
        return FALSE;
    return sio_write_port(dev, data_port, value);
}

/*
 * Read Nuvoton register with bank selection.
 */
static BYTE nct679x_read_reg(HANDLE dev, DWORD addr_port, DWORD data_port, WORD reg)
{
    BYTE bank = (reg >> 8) & 0xFF;
    BYTE offset = reg & 0xFF;
    BYTE val = 0;

    sio_write_reg(dev, addr_port, data_port, 0x4E, bank);
    sio_read_reg(dev, addr_port, data_port, offset, &val);

    return val;
}

/*
 * Detect Super I/O chip ID.
 */
static sio_chip_info_t sio_detect_chip(HANDLE dev, DWORD addr_port, DWORD data_port)
{
    sio_chip_info_t result = {0, SIO_CHIP_UNKNOWN, "Unknown"};
    BYTE chip_id_high = 0, chip_id_low = 0;

    if (!sio_enter(dev, addr_port))
        return result;

    sio_read_reg(dev, addr_port, data_port, 0x20, &chip_id_high);
    sio_read_reg(dev, addr_port, data_port, 0x21, &chip_id_low);

    sio_exit(dev, addr_port);

    WORD chip_id = ((WORD)chip_id_high << 8) | chip_id_low;

    /* Look up in known chips table */
    for (int i = 0; i < (int)KNOWN_CHIPS_COUNT; i++) {
        if (known_chips[i].id == chip_id) {
            return known_chips[i];
        }
    }

    result.id = chip_id;
    return result;
}

/*
 * Read temperature from Nuvoton NCT679x chip.
 */
static int nct679x_read_temp(HANDLE dev, DWORD addr_port, DWORD data_port, int channel)
{
    if (channel < 0 || channel >= 8)
        return -1;

    BYTE val = nct679x_read_reg(dev, addr_port, data_port, nct679x_temp_regs[channel]);
    BYTE half = nct679x_read_reg(dev, addr_port, data_port, nct679x_temp_half[channel]);
    BYTE bit = nct679x_temp_bits[channel];

    int temp = (int)(((val << 1) | ((half >> bit) & 1)) / 2);

    if (temp < -40 || temp > 150)
        return -1;

    return temp;
}

/*
 * Read fan RPM from Nuvoton NCT679x chip.
 */
static int nct679x_read_fan(HANDLE dev, DWORD addr_port, DWORD data_port, int fan_num)
{
    if (fan_num < 0 || fan_num >= 7)
        return -1;

    WORD reg = nct679x_fan_regs[fan_num];
    BYTE low = nct679x_read_reg(dev, addr_port, data_port, reg);
    BYTE high = nct679x_read_reg(dev, addr_port, data_port, reg + 1);

    int count = ((high & 0x1F) << 8) | low;

    if (count <= 0x15)
        return 0;

    return 1350000 / count;
}

/*
 * Read voltage from Nuvoton NCT679x chip.
 */
static int nct679x_read_voltage(HANDLE dev, DWORD addr_port, DWORD data_port, int channel)
{
    if (channel < 0 || channel >= 16)
        return -1;

    WORD reg = nct679x_voltage_regs[channel];
    BYTE val = nct679x_read_reg(dev, addr_port, data_port, reg);

    return (int)(val * 8);  /* 8 mV per step */
}

/*
 * ITE chips are unlocked with a different key than Nuvoton/Winbond/Fintek:
 * 0x87, 0x01, 0x55, then 0x55 (or 0xAA when the index port is 0x4E).
 */
static BOOL ite_enter(HANDLE dev, DWORD addr_port)
{
    return sio_write_port(dev, addr_port, 0x87) &&
           sio_write_port(dev, addr_port, 0x01) &&
           sio_write_port(dev, addr_port, 0x55) &&
           sio_write_port(dev, addr_port, addr_port == 0x4E ? 0xAA : 0x55);
}

/* Leave ITE config mode: config control register 0x02, bit 1. */
static void ite_exit(HANDLE dev, DWORD addr_port, DWORD data_port)
{
    sio_write_reg(dev, addr_port, data_port, 0x02, 0x02);
}

typedef struct {
    HANDLE dev;
    DWORD base;
} ite_win_t;

static int ite_win_read(void *ctx, unsigned reg)
{
    ite_win_t *w = (ite_win_t *)ctx;
    BYTE v = 0;

    if (!sio_write_port(w->dev, w->base + ITE_EC_INDEX_OFF, (BYTE)reg))
        return -1;
    if (!sio_read_port(w->dev, w->base + ITE_EC_DATA_OFF, &v))
        return -1;
    return v;
}

static void ite_win_write(void *ctx, unsigned reg, int val)
{
    ite_win_t *w = (ite_win_t *)ctx;

    if (sio_write_port(w->dev, w->base + ITE_EC_INDEX_OFF, (BYTE)reg))
        sio_write_port(w->dev, w->base + ITE_EC_DATA_OFF, (BYTE)val);
}

/*
 * A plausible monitor base: above the legacy ISA range, 8-byte aligned, and
 * clear of the PCI config ports (0xCF8-0xCFF) that the index/data ports must
 * never alias.
 */
static BOOL ite_base_ok(DWORD base)
{
    if (base < 0x100 || base > 0xFFF8 || (base & 7))
        return FALSE;
    if (base + ITE_EC_INDEX_OFF <= 0xCFF && base + ITE_EC_DATA_OFF >= 0xCF8)
        return FALSE;
    return TRUE;
}

/*
 * Look for an ITE chip behind addr_port/data_port and, if present, read all
 * its sensors into *out. Returns TRUE if an ITE chip was found (even if it
 * then returned nothing; out->error says why), FALSE if there is no ITE
 * chip here and the caller should try the other chip families.
 */
static BOOL ite_probe_and_read(HANDLE dev, DWORD addr_port, DWORD data_port,
                               hwsense_superio_result_t *out)
{
    BYTE id_hi = 0, id_lo = 0, active = 0, base_hi = 0, base_lo = 0;
    WORD id;
    DWORD base;
    ite_win_t w;

    memset(out, 0, sizeof(*out));

    /* ITE chips only answer on the two standard config ports. */
    if (addr_port != 0x2E && addr_port != 0x4E)
        return FALSE;
    if (!ite_enter(dev, addr_port))
        return FALSE;

    sio_read_reg(dev, addr_port, data_port, ITE_CFG_DEVID, &id_hi);
    sio_read_reg(dev, addr_port, data_port, ITE_CFG_DEVID + 1, &id_lo);
    id = ((WORD)id_hi << 8) | id_lo;

    if (!hwsense_ite_chip_name(id)) {
        ite_exit(dev, addr_port, data_port);
        return FALSE;
    }

    sio_write_reg(dev, addr_port, data_port, ITE_CFG_LDN, ITE_LDN_EC);
    sio_read_reg(dev, addr_port, data_port, ITE_CFG_ACTIVE, &active);
    sio_read_reg(dev, addr_port, data_port, ITE_CFG_BASE, &base_hi);
    sio_read_reg(dev, addr_port, data_port, ITE_CFG_BASE + 1, &base_lo);
    ite_exit(dev, addr_port, data_port);

    base = (((DWORD)base_hi << 8) | base_lo) & ~7u;
    _snprintf_s(out->chip_name, sizeof(out->chip_name), _TRUNCATE, "%s",
                hwsense_ite_chip_name(id));
    out->chip_id = id;

    if (!(active & 1) || !ite_base_ok(base)) {
        _snprintf_s(out->error, sizeof(out->error), _TRUNCATE,
                    "ITE %s hardware monitor is disabled or has an invalid base address (0x%X)",
                    out->chip_name, base);
        return TRUE;
    }

    w.dev = dev;
    w.base = base;
    sio_ec_index_port = base + ITE_EC_INDEX_OFF;
    sio_ec_data_port = base + ITE_EC_DATA_OFF;
    hwsense_ite_decode(id, ite_win_read, ite_win_write, &w, out);
    sio_ec_index_port = 0;
    sio_ec_data_port = 0;
    return TRUE;
}

/* Keep only the requested kind of reading from a full ITE result. */
static hwsense_superio_result_t ite_filter(const hwsense_superio_result_t *src,
                                           int temps, int fans, int volts)
{
    hwsense_superio_result_t r = *src;

    if (!temps) {
        r.count = 0;
        memset(r.temperatures, 0, sizeof(r.temperatures));
    }
    if (!fans) {
        r.fan_count = 0;
        memset(r.fan_rpms, 0, sizeof(r.fan_rpms));
    }
    if (!volts) {
        r.voltage_count = 0;
        memset(r.voltages, 0, sizeof(r.voltages));
    }

    r.ok = (temps && r.count) || (fans && r.fan_count) || (volts && r.voltage_count);
    if (!r.ok && !r.error[0])
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "No valid readings from %s", r.chip_name);
    return r;
}

/*
 * Read all available Super I/O sensors.
 */
hwsense_superio_result_t hwsense_superio_temps(hwsense_ctx_t *ctx)
{
    hwsense_superio_result_t result = {0};

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE) {
        result.ok = 0;
        _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                    "Invalid context or driver handle");
        return result;
    }

    HANDLE dev = ctx->driver_handle;

    /* Try all known IO port pairs for Super I/O chips */
    DWORD port_pairs[][2] = {
        { 0x2E, 0x2F },  /* Standard Nuvoton/Winbond/Fintek */
        { 0x4E, 0x4F },  /* Alternate (IT8792E, some Nuvoton) */
        { 0x162, 0x163 }, /* Some ITE chips */
        { 0x3E0, 0x3E1 }, /* Some older chips */
        { 0x4E0, 0x4E1 }, /* Some older chips */
    };

    int num_port_pairs = sizeof(port_pairs) / sizeof(port_pairs[0]);

    for (int p = 0; p < num_port_pairs; p++) {
        DWORD addr_port = port_pairs[p][0];
        DWORD data_port = port_pairs[p][1];

        /* Try to detect chip */
        hwsense_superio_result_t ite;
        if (ite_probe_and_read(dev, addr_port, data_port, &ite))
            return ite_filter(&ite, 1, 0, 0);

        sio_chip_info_t chip = sio_detect_chip(dev, addr_port, data_port);

        if (chip.type != SIO_CHIP_UNKNOWN) {
            /* Found a chip! */
            result.chip_id = chip.id;
            _snprintf_s(result.chip_name, sizeof(result.chip_name), _TRUNCATE, "%s (port 0x%X)", chip.name, addr_port);

            /* Disable IO space lock */
            if (sio_enter(dev, addr_port)) {
                BYTE lock_val = 0;
                sio_read_reg(dev, addr_port, data_port, 0x28, &lock_val);
                lock_val &= ~0x10;
                sio_write_reg(dev, addr_port, data_port, 0x28, lock_val);
                sio_exit(dev, addr_port);
            }

            /* Read temperatures based on chip type */
            int i;
            int max_temps = 0;

            switch (chip.type) {
            case SIO_CHIP_NUVOTON:
                max_temps = 8;
                for (i = 0; i < max_temps && result.count < HWSENSE_MAX_SUPERIO_TEMPS; i++) {
                    int temp = nct679x_read_temp(dev, addr_port, data_port, i);
                    if (temp > 0) {
                        result.temperatures[result.count] = temp;
                        result.count++;
                    }
                }
                break;

            default:
                break;
            }

            result.ok = (result.count > 0) ? 1 : 0;
            if (result.count == 0) {
                _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                            "No valid temperature readings from %s", chip.name);
            }

            return result;
        }
    }

    /* No chip found - try reading raw IO ports to detect any hardware */
    result.ok = 0;
    _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                "No Super I/O chip detected (scanned %d port pairs)", num_port_pairs);
    return result;
}

/*
 * Read fan speeds from Super I/O.
 */
hwsense_superio_result_t hwsense_superio_fans(hwsense_ctx_t *ctx)
{
    hwsense_superio_result_t result = {0};

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE) {
        result.ok = 0;
        _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                    "Invalid context or driver handle");
        return result;
    }

    HANDLE dev = ctx->driver_handle;

    /* Try multiple IO port pairs */
    DWORD port_pairs[][2] = {
        { 0x2E, 0x2F },
        { 0x4E, 0x4F },
        { 0x162, 0x163 },
    };

    int num_port_pairs = sizeof(port_pairs) / sizeof(port_pairs[0]);

    for (int p = 0; p < num_port_pairs; p++) {
        DWORD addr_port = port_pairs[p][0];
        DWORD data_port = port_pairs[p][1];

        hwsense_superio_result_t ite;
        if (ite_probe_and_read(dev, addr_port, data_port, &ite))
            return ite_filter(&ite, 0, 1, 0);

        sio_chip_info_t chip = sio_detect_chip(dev, addr_port, data_port);

        if (chip.type != SIO_CHIP_UNKNOWN) {
            result.chip_id = chip.id;
            _snprintf_s(result.chip_name, sizeof(result.chip_name), _TRUNCATE, "%s", chip.name);

            /* Disable IO space lock */
            if (sio_enter(dev, addr_port)) {
                BYTE lock_val = 0;
                sio_read_reg(dev, addr_port, data_port, 0x28, &lock_val);
                lock_val &= ~0x10;
                sio_write_reg(dev, addr_port, data_port, 0x28, lock_val);
                sio_exit(dev, addr_port);
            }

            /* Read fan speeds based on chip type */
            int i;
            int max_fans = 0;

            switch (chip.type) {
            case SIO_CHIP_NUVOTON:
                max_fans = 7;
                for (i = 0; i < max_fans && result.fan_count < HWSENSE_MAX_SUPERIO_FANS; i++) {
                    int rpm = nct679x_read_fan(dev, addr_port, data_port, i);
                    if (rpm >= 0) {
                        result.fan_rpms[result.fan_count] = rpm;
                        result.fan_count++;
                        result.count++;
                    }
                }
                break;

            default:
                break;
            }

            result.ok = (result.fan_count > 0) ? 1 : 0;
            if (result.fan_count == 0) {
                _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                            "No valid fan readings from %s", chip.name);
            }

            return result;
        }
    }

    result.ok = 0;
    _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                "No Super I/O chip detected");
    return result;
}

/*
 * Read voltages from Super I/O.
 */
hwsense_superio_result_t hwsense_superio_voltages(hwsense_ctx_t *ctx)
{
    hwsense_superio_result_t result = {0};

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE) {
        result.ok = 0;
        _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                    "Invalid context or driver handle");
        return result;
    }

    HANDLE dev = ctx->driver_handle;

    /* Try multiple IO port pairs */
    DWORD port_pairs[][2] = {
        { 0x2E, 0x2F },
        { 0x4E, 0x4F },
        { 0x162, 0x163 },
    };

    int num_port_pairs = sizeof(port_pairs) / sizeof(port_pairs[0]);

    for (int p = 0; p < num_port_pairs; p++) {
        DWORD addr_port = port_pairs[p][0];
        DWORD data_port = port_pairs[p][1];

        hwsense_superio_result_t ite;
        if (ite_probe_and_read(dev, addr_port, data_port, &ite))
            return ite_filter(&ite, 0, 0, 1);

        sio_chip_info_t chip = sio_detect_chip(dev, addr_port, data_port);

        if (chip.type != SIO_CHIP_UNKNOWN) {
            result.chip_id = chip.id;
            _snprintf_s(result.chip_name, sizeof(result.chip_name), _TRUNCATE, "%s", chip.name);

            /* Disable IO space lock */
            if (sio_enter(dev, addr_port)) {
                BYTE lock_val = 0;
                sio_read_reg(dev, addr_port, data_port, 0x28, &lock_val);
                lock_val &= ~0x10;
                sio_write_reg(dev, addr_port, data_port, 0x28, lock_val);
                sio_exit(dev, addr_port);
            }

            /* Read voltages based on chip type */
            int i;
            int max_volts = 0;

            switch (chip.type) {
            case SIO_CHIP_NUVOTON:
                max_volts = 16;
                for (i = 0; i < max_volts && result.voltage_count < HWSENSE_MAX_SUPERIO_VOLTAGES; i++) {
                    int mv = nct679x_read_voltage(dev, addr_port, data_port, i);
                    if (mv > 0) {
                        result.voltages[result.voltage_count] = mv / 1000.0;
                        result.voltage_count++;
                        result.count++;
                    }
                }
                break;

            default:
                break;
            }

            result.ok = (result.voltage_count > 0) ? 1 : 0;
            if (result.voltage_count == 0) {
                _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                            "No valid voltage readings from %s", chip.name);
            }

            return result;
        }
    }

    result.ok = 0;
    _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                "No Super I/O chip detected");
    return result;
}
