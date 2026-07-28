/*
 * superio.c — Super I/O chip detection, temperature, and fan speed reading.
 *
 * Supports Nuvoton NCT6791D/93D/95D/96D/97D/98D/99D, ITE IT8628E/IT8688E/IT8728F,
 * Winbond W83627DHG, and Fintek F71882/F71889 chips.
 *
 * Accesses registers via IO ports 0x2E/0x2F or 0x4E/0x4F.
 */

#include "../core/hwsense_internal.h"
#include "../core/ioctl_codes.h"
#include <stdio.h>

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
    /* ITE */
    { 0x8628, SIO_CHIP_ITE, "IT8628E" },
    { 0x8688, SIO_CHIP_ITE, "IT8688E" },
    { 0x8689, SIO_CHIP_ITE, "IT8689E" },
    { 0x8728, SIO_CHIP_ITE, "IT8728F" },
    { 0x8790, SIO_CHIP_ITE, "IT8790E" },
    { 0x8733, SIO_CHIP_ITE, "IT8792E" },
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

/* ITE temperature registers */
static const BYTE ite_temp_regs[] = { 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E };

/* ITE fan tachometer registers (low, high) */
static const BYTE ite_fan_regs[][2] = {
    { 0x0D, 0x18 }, { 0x0E, 0x19 }, { 0x0F, 0x1A },
    { 0x80, 0x81 }, { 0x82, 0x83 }, { 0x4C, 0x4D }
};

/* ITE voltage registers */
static const BYTE ite_voltage_regs[] = { 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x2F };

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
 * Read temperature from ITE chip.
 */
static int ite_read_temp(HANDLE dev, DWORD addr_port, DWORD data_port, int channel)
{
    if (channel < 0 || channel >= 6)
        return -1;

    BYTE val = 0;
    sio_read_reg(dev, addr_port, data_port, ite_temp_regs[channel], &val);

    int temp = (int)(signed char)val;

    if (temp < -40 || temp > 150)
        return -1;

    return temp;
}

/*
 * Read fan RPM from ITE chip.
 */
static int ite_read_fan(HANDLE dev, DWORD addr_port, DWORD data_port, int fan_num)
{
    if (fan_num < 0 || fan_num >= 6)
        return -1;

    BYTE low = 0, high = 0;
    sio_read_reg(dev, addr_port, data_port, ite_fan_regs[fan_num][0], &low);
    sio_read_reg(dev, addr_port, data_port, ite_fan_regs[fan_num][1], &high);

    int count = (high << 8) | low;

    if (count == 0 || count == 0xFFFF)
        return 0;

    return 1350000 / (count * 2);
}

/*
 * Read voltage from ITE chip.
 */
static int ite_read_voltage(HANDLE dev, DWORD addr_port, DWORD data_port, int channel)
{
    if (channel < 0 || channel >= 10)
        return -1;

    BYTE val = 0;
    sio_read_reg(dev, addr_port, data_port, ite_voltage_regs[channel], &val);

    return (int)(val * 16);  /* 16 mV per step (typical) */
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

            case SIO_CHIP_ITE:
                max_temps = 6;
                for (i = 0; i < max_temps && result.count < HWSENSE_MAX_SUPERIO_TEMPS; i++) {
                    int temp = ite_read_temp(dev, addr_port, data_port, i);
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

            case SIO_CHIP_ITE:
                max_fans = 6;
                for (i = 0; i < max_fans && result.fan_count < HWSENSE_MAX_SUPERIO_FANS; i++) {
                    int rpm = ite_read_fan(dev, addr_port, data_port, i);
                    if (rpm > 0) {
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

            case SIO_CHIP_ITE:
                max_volts = 10;
                for (i = 0; i < max_volts && result.voltage_count < HWSENSE_MAX_SUPERIO_VOLTAGES; i++) {
                    int mv = ite_read_voltage(dev, addr_port, data_port, i);
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
