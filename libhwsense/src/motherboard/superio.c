/*
 * superio.c — Super I/O chip detection and temperature reading.
 *
 * Supports common Super I/O chips (Winbond NCT6775/NCT6776, Nuvoton NCT6798).
 * Accesses temperature registers via IO ports 0x2E/0x2F or 0x4E/0x4F.
 *
 * WinRing0 provides IOCTL_OLS_READ_IO_PORT_BYTE / IOCTL_OLS_WRITE_IO_PORT_BYTE:
 *   IOCTL code: 0x9C4020CC (read) / 0x9C4020D0 (write)
 */

#include "../core/hwsense_internal.h"
#include "../core/ioctl_codes.h"
#include <stdio.h>

/* Super I/O standard IO ports */
#define SIO_ADDR_PORT   0x2E
#define SIO_DATA_PORT   0x2F
#define SIO_ALT_PORT    0x4E
#define SIO_ALT_DATA    0x4F

/* NCT6775/NCT6776 temperature registers (bank 0) */
#define NCT6775_TEMP1   0x27  /* Remote temperature 1 */
#define NCT6775_TEMP2   0x73  /* Remote temperature 2 */
#define NCT6775_TEMP3   0x75  /* Remote temperature 3 */
#define NCT6775_SEL     0x26  /* Select register for bank switching */

/* NCT6798 extended temperature registers */
#define NCT6798_TEMP1   0x27
#define NCT6798_TEMP2   0x73
#define NCT6798_TEMP3   0x75
#define NCT6798_TEMP4   0x77
#define NCT6798_SEL     0x26

/* Read/Write IO port via WinRing0 */
static BOOL sio_read_port(HANDLE dev, WORD port, BYTE *out_value)
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

static BOOL sio_write_port(HANDLE dev, WORD port, BYTE value)
{
    BYTE inp[2];
    DWORD bytes_ret = 0;

    *(WORD*)&inp[0] = port;
    inp[2] = value;

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
static BOOL sio_enter(HANDLE dev, WORD addr_port)
{
    BYTE val;
    BOOL ok;

    /* Enter configuration: write 0x87 twice to address port */
    ok = sio_write_port(dev, addr_port, 0x87);
    if (!ok) return FALSE;
    ok = sio_write_port(dev, addr_port, 0x87);
    return ok;
}

/*
 * Exit Super I/O configuration mode.
 */
static BOOL sio_exit(HANDLE dev, WORD addr_port)
{
    BYTE val;
    return sio_write_port(dev, addr_port, 0xAA);
}

/*
 * Read a Super I/O register.
 */
static BOOL sio_read_reg(HANDLE dev, WORD addr_port, WORD data_port,
                         BYTE reg, BYTE *out_value)
{
    BOOL ok = sio_write_port(dev, addr_port, reg);
    if (!ok) return FALSE;
    return sio_read_port(dev, data_port, out_value);
}

/*
 * Detect Super I/O chip ID.
 * Returns chip ID (high byte = manufacturer, low byte = revision), or 0 on failure.
 */
static WORD sio_detect_chip(HANDLE dev, WORD addr_port, WORD data_port)
{
    BYTE chip_id_high = 0, chip_id_low = 0;

    if (!sio_enter(dev, addr_port))
        return 0;

    /* Read chip ID from registers 0x20 (high) and 0x21 (low) */
    sio_read_reg(dev, addr_port, data_port, 0x20, &chip_id_high);
    sio_read_reg(dev, addr_port, data_port, 0x21, &chip_id_low);

    sio_exit(dev, addr_port);

    return ((WORD)chip_id_high << 8) | chip_id_low;
}

/*
 * Read temperature from NCT6775/NCT6776.
 * channel: 0 = TEMP1, 1 = TEMP2, 2 = TEMP3
 */
static int nct6775_read_temp(HANDLE dev, WORD addr_port, WORD data_port, int channel)
{
    BYTE temp_reg;
    BYTE val;

    if (!sio_enter(dev, addr_port))
        return -1;

    /* Select bank 0 */
    sio_read_reg(dev, addr_port, data_port, NCT6775_SEL, &val);
    sio_write_port(dev, data_port, val & 0x8F);  /* bank 0 */

    switch (channel) {
    case 0: temp_reg = NCT6775_TEMP1; break;
    case 1: temp_reg = NCT6775_TEMP2; break;
    case 2: temp_reg = NCT6775_TEMP3; break;
    default:
        sio_exit(dev, addr_port);
        return -1;
    }

    sio_read_reg(dev, addr_port, data_port, temp_reg, &val);
    sio_exit(dev, addr_port);

    /* NCT6775 temperature is in 8-bit signed format */
    int temp = (int)(signed char)val;
    return temp;
}

/*
 * Read temperature from NCT6798 (extended).
 * channel: 0-3 for different temperature sources
 */
static int nct6798_read_temp(HANDLE dev, WORD addr_port, WORD data_port, int channel)
{
    BYTE temp_reg;
    BYTE val;

    if (!sio_enter(dev, addr_port))
        return -1;

    /* Select bank 0 */
    sio_read_reg(dev, addr_port, data_port, NCT6798_SEL, &val);
    sio_write_port(dev, data_port, val & 0x8F);

    switch (channel) {
    case 0: temp_reg = NCT6798_TEMP1; break;
    case 1: temp_reg = NCT6798_TEMP2; break;
    case 2: temp_reg = NCT6798_TEMP3; break;
    case 3: temp_reg = NCT6798_TEMP4; break;
    default:
        sio_exit(dev, addr_port);
        return -1;
    }

    sio_read_reg(dev, addr_port, data_port, temp_reg, &val);
    sio_exit(dev, addr_port);

    int temp = (int)(signed char)val;
    return temp;
}

/*
 * Maximum number of Super I/O temperature sensors.
 */

/*
 * Read all available Super I/O temperatures.
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
    WORD addr_port = SIO_ADDR_PORT;
    WORD data_port = SIO_DATA_PORT;

    /* Detect chip */
    WORD chip_id = sio_detect_chip(dev, addr_port, data_port);

    if (chip_id == 0) {
        /* Try alternate ports */
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
    _snprintf_s(result.chip_name, sizeof(result.chip_name), _TRUNCATE,
                "NCT67%02X", chip_id & 0xFF);

    /* Read temperatures based on chip type */
    BYTE high = (chip_id >> 8) & 0xFF;
    BYTE low = chip_id & 0xFF;

    if (high == 0xC8 || high == 0xD1) {
        /* NCT6775/NCT6776 family */
        int i;
        for (i = 0; i < 3; i++) {
            int temp = nct6775_read_temp(dev, addr_port, data_port, i);
            if (temp > -40 && temp < 150) {
                result.temperatures[result.count] = temp;
                result.count++;
            }
        }
    } else if (high == 0xC9 || high == 0xD3) {
        /* NCT6798 family */
        int i;
        for (i = 0; i < 4; i++) {
            int temp = nct6798_read_temp(dev, addr_port, data_port, i);
            if (temp > -40 && temp < 150) {
                result.temperatures[result.count] = temp;
                result.count++;
            }
        }
    }

    result.ok = (result.count > 0) ? 1 : 0;
    if (result.count == 0) {
        _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                    "No valid temperature readings from Super I/O chip");
    }

    return result;
}
