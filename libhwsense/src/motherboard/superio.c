/*
 * superio.c — Super I/O chip detection, temperature, and fan speed reading.
 *
 * Supports Nuvoton NCT6775/NCT6776/NCT6798/NCT6799 chips.
 * Accesses registers via IO ports 0x2E/0x2F or 0x4E/0x4F.
 *
 * WinRing0 provides IOCTL_OLS_READ_IO_PORT_BYTE / IOCTL_OLS_WRITE_IO_PORT_BYTE.
 */

#include "../core/hwsense_internal.h"
#include "../core/ioctl_codes.h"
#include <stdio.h>

/* Super I/O standard IO ports */
#define SIO_ADDR_PORT   0x2E
#define SIO_DATA_PORT   0x2F
#define SIO_ALT_PORT    0x4E
#define SIO_ALT_DATA    0x4F

/* NCT6798D/NCT6799D chip IDs */
#define NCT6798D_CHIP_ID    0xD42B
#define NCT6799D_CHIP_ID    0xD802

/* Fan count registers for NCT6798D/NCT6799D (13-bit) */
static const WORD nct6798_fan_regs[] = { 0x4B0, 0x4B2, 0x4B4, 0x4B6, 0x4B8, 0x4BA, 0x4CC };

/* Voltage registers for NCT6798D/NCT6799D */
static const WORD nct6798_voltage_regs[] = {
    0x480, 0x481, 0x482, 0x483, 0x484, 0x485, 0x486, 0x487,
    0x488, 0x489, 0x48A, 0x48B, 0x48C, 0x48D, 0x48E, 0x48F
};

/* Temperature registers for NCT6798D */
static const WORD nct6798_temp_regs[] = { 0x073, 0x075, 0x077, 0x079, 0x07B, 0x07D, 0x4A0, 0x4A2 };
static const WORD nct6798_temp_half[] = { 0x074, 0x076, 0x078, 0x07A, 0x07C, 0x07E, 0x49E, 0x4A1 };
static const BYTE nct6798_temp_bits[] = { 7, 7, 7, 7, 7, 7, 6, 7 };

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

static BOOL sio_write_port(HANDLE dev, DWORD port, BYTE value)
{
    BYTE inp[5];
    DWORD bytes_ret = 0;

    *(DWORD*)&inp[0] = port;
    inp[4] = value;

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
 * Detect Super I/O chip ID.
 */
static WORD sio_detect_chip(HANDLE dev, DWORD addr_port, DWORD data_port)
{
    BYTE chip_id_high = 0, chip_id_low = 0;

    if (!sio_enter(dev, addr_port))
        return 0;

    sio_read_reg(dev, addr_port, data_port, 0x20, &chip_id_high);
    sio_read_reg(dev, addr_port, data_port, 0x21, &chip_id_low);

    sio_exit(dev, addr_port);

    return ((WORD)chip_id_high << 8) | chip_id_low;
}

/*
 * Read NCT6798D/NCT6799D register with bank selection.
 */
static BYTE nct6798_read_reg(HANDLE dev, DWORD addr_port, DWORD data_port, WORD reg)
{
    BYTE bank = (reg >> 8) & 0xFF;
    BYTE offset = reg & 0xFF;
    BYTE val = 0;

    /* Select bank */
    sio_write_reg(dev, addr_port, data_port, 0x4E, bank);
    /* Read register */
    sio_read_reg(dev, addr_port, data_port, offset, &val);

    return val;
}

/*
 * Read temperature from NCT6798D.
 * Returns temperature in Celsius, or -1 on failure.
 */
static int nct6798_read_temp(HANDLE dev, DWORD addr_port, DWORD data_port, int channel)
{
    if (channel < 0 || channel >= 8)
        return -1;

    BYTE val = nct6798_read_reg(dev, addr_port, data_port, nct6798_temp_regs[channel]);
    BYTE half = nct6798_read_reg(dev, addr_port, data_port, nct6798_temp_half[channel]);
    BYTE bit = nct6798_temp_bits[channel];

    /* Temperature = 0.5 * ((val << 1) | ((half >> bit) & 1)) */
    int temp = (int)(((val << 1) | ((half >> bit) & 1)) / 2);

    /* Sanity check */
    if (temp < -40 || temp > 150)
        return -1;

    return temp;
}

/*
 * Read fan RPM from NCT6798D.
 * Returns RPM, or -1 on failure.
 */
static int nct6798_read_fan(HANDLE dev, DWORD addr_port, DWORD data_port, int fan_num)
{
    if (fan_num < 0 || fan_num >= 7)
        return -1;

    WORD reg = nct6798_fan_regs[fan_num];
    BYTE low = nct6798_read_reg(dev, addr_port, data_port, reg);
    BYTE high = nct6798_read_reg(dev, addr_port, data_port, reg + 1);

    /* 13-bit counter */
    int count = ((high & 0x1F) << 8) | low;

    /* RPM = 1,350,000 / count (if count > 0x15) */
    if (count <= 0x15)
        return 0;

    return 1350000 / count;
}

/*
 * Read voltage from NCT6798D.
 * Returns voltage in mV, or -1 on failure.
 */
static int nct6798_read_voltage(HANDLE dev, DWORD addr_port, DWORD data_port, int channel)
{
    if (channel < 0 || channel >= 16)
        return -1;

    WORD reg = nct6798_voltage_regs[channel];
    BYTE val = nct6798_read_reg(dev, addr_port, data_port, reg);

    /* Voltage = 0.008 * register_value (in volts) = 8 * register_value (in mV) */
    return (int)(val * 8);
}

/*
 * Maximum number of Super I/O sensors.
 */
#define MAX_SUPERIO_TEMPS   8
#define MAX_SUPERIO_FANS    7
#define MAX_SUPERIO_VOLTAGES 16

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
    DWORD addr_port = SIO_ADDR_PORT;
    DWORD data_port = SIO_DATA_PORT;

    /* Detect chip */
    WORD chip_id = sio_detect_chip(dev, addr_port, data_port);

    if (chip_id == 0) {
        addr_port = SIO_ALT_PORT;
        data_port = SIO_ALT_DATA;
        chip_id = sio_detect_chip(dev, addr_port, data_port);
    }

    if (chip_id == 0) {
        result.ok = 0;
        _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                    "No Super I/O chip detected");
        return result;
    }

    result.chip_id = chip_id;

    /* Identify chip */
    BYTE high = (chip_id >> 8) & 0xFF;
    BYTE low = chip_id & 0xFF;

    if (chip_id == NCT6798D_CHIP_ID) {
        _snprintf_s(result.chip_name, sizeof(result.chip_name), _TRUNCATE, "NCT6798D");
    } else if (chip_id == NCT6799D_CHIP_ID) {
        _snprintf_s(result.chip_name, sizeof(result.chip_name), _TRUNCATE, "NCT6799D");
    } else {
        _snprintf_s(result.chip_name, sizeof(result.chip_name), _TRUNCATE,
                    "Nuvoton 0x%04X", chip_id);
    }

    /* Enter config mode to disable IO space lock */
    if (sio_enter(dev, addr_port)) {
        BYTE lock_val = 0;
        sio_read_reg(dev, addr_port, data_port, 0x28, &lock_val);
        lock_val &= ~0x10;  /* Clear bit 4 (IO space lock) */
        sio_write_reg(dev, addr_port, data_port, 0x28, lock_val);
        sio_exit(dev, addr_port);
    }

    /* Read temperatures */
    int i;
    for (i = 0; i < 8 && result.count < MAX_SUPERIO_TEMPS; i++) {
        int temp = nct6798_read_temp(dev, addr_port, data_port, i);
        if (temp > 0) {
            result.temperatures[result.count] = temp;
            result.count++;
        }
    }

    result.ok = (result.count > 0) ? 1 : 0;
    if (result.count == 0) {
        _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                    "No valid temperature readings from Super I/O chip");
    }

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
    DWORD addr_port = SIO_ADDR_PORT;
    DWORD data_port = SIO_DATA_PORT;

    /* Detect chip */
    WORD chip_id = sio_detect_chip(dev, addr_port, data_port);

    if (chip_id == 0) {
        addr_port = SIO_ALT_PORT;
        data_port = SIO_ALT_DATA;
        chip_id = sio_detect_chip(dev, addr_port, data_port);
    }

    if (chip_id == 0) {
        result.ok = 0;
        _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                    "No Super I/O chip detected");
        return result;
    }

    result.chip_id = chip_id;

    /* Enter config mode to disable IO space lock */
    if (sio_enter(dev, addr_port)) {
        BYTE lock_val = 0;
        sio_read_reg(dev, addr_port, data_port, 0x28, &lock_val);
        lock_val &= ~0x10;
        sio_write_reg(dev, addr_port, data_port, 0x28, lock_val);
        sio_exit(dev, addr_port);
    }

    /* Read fan speeds */
    int i;
    for (i = 0; i < 7 && result.count < MAX_SUPERIO_FANS; i++) {
        int rpm = nct6798_read_fan(dev, addr_port, data_port, i);
        if (rpm >= 0) {
            result.fan_rpms[result.count] = rpm;
            result.fan_count++;
            result.count++;
        }
    }

    result.ok = (result.fan_count > 0) ? 1 : 0;
    if (result.fan_count == 0) {
        _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                    "No valid fan readings from Super I/O chip");
    }

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
    DWORD addr_port = SIO_ADDR_PORT;
    DWORD data_port = SIO_DATA_PORT;

    /* Detect chip */
    WORD chip_id = sio_detect_chip(dev, addr_port, data_port);

    if (chip_id == 0) {
        addr_port = SIO_ALT_PORT;
        data_port = SIO_ALT_DATA;
        chip_id = sio_detect_chip(dev, addr_port, data_port);
    }

    if (chip_id == 0) {
        result.ok = 0;
        _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                    "No Super I/O chip detected");
        return result;
    }

    result.chip_id = chip_id;

    /* Enter config mode to disable IO space lock */
    if (sio_enter(dev, addr_port)) {
        BYTE lock_val = 0;
        sio_read_reg(dev, addr_port, data_port, 0x28, &lock_val);
        lock_val &= ~0x10;
        sio_write_reg(dev, addr_port, data_port, 0x28, lock_val);
        sio_exit(dev, addr_port);
    }

    /* Read voltages */
    int i;
    for (i = 0; i < 16 && result.voltage_count < MAX_SUPERIO_VOLTAGES; i++) {
        int mv = nct6798_read_voltage(dev, addr_port, data_port, i);
        if (mv > 0) {
            result.voltages[result.voltage_count] = mv / 1000.0;  /* Convert to volts */
            result.voltage_count++;
            result.count++;
        }
    }

    result.ok = (result.voltage_count > 0) ? 1 : 0;
    if (result.voltage_count == 0) {
        _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                    "No valid voltage readings from Super I/O chip");
    }

    return result;
}
