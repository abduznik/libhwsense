/*
 * ec.c — ACPI Embedded Controller access for ASUS motherboards.
 *
 * Reads fan speeds, voltages, and temperatures from EC registers.
 * EC is accessed via IO ports 0x62 (data) and 0x66 (command/status).
 *
 * SECURITY: EC read is safe - only reads standard ACPI EC ports.
 * Write access is restricted to admin only.
 */

#include "../core/hwsense_internal.h"
#include "../core/ioctl_codes.h"
#include "ec.h"
#include <stdio.h>

/* ACPI EC IO ports */
#define EC_DATA_PORT    0x62
#define EC_CMD_PORT     0x66

/* EC commands */
#define EC_CMD_READ     0x80

/* EC status bits */
#define EC_STATUS_OBF   0x01
#define EC_STATUS_IBF   0x02

/* Timeout for EC operations (ms) */
#define EC_TIMEOUT_MS   100

/* IO port read/write via WinRing0 */
static BOOL ec_read_port(HANDLE dev, DWORD port, BYTE *out_value)
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

static BOOL ec_write_port(HANDLE dev, DWORD port, BYTE value)
{
    if (port != EC_DATA_PORT && port != EC_CMD_PORT) {
        fprintf(stderr, "SECURITY: EC write blocked to port 0x%03X\n", port);
        return FALSE;
    }

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

static BOOL ec_wait_ibf(HANDLE dev)
{
    DWORD start = GetTickCount();
    BYTE status;

    while ((GetTickCount() - start) < EC_TIMEOUT_MS) {
        if (!ec_read_port(dev, EC_CMD_PORT, &status))
            return FALSE;
        if (!(status & EC_STATUS_IBF))
            return TRUE;
    }
    return FALSE;
}

static BOOL ec_wait_obf(HANDLE dev)
{
    DWORD start = GetTickCount();
    BYTE status;

    while ((GetTickCount() - start) < EC_TIMEOUT_MS) {
        if (!ec_read_port(dev, EC_CMD_PORT, &status))
            return FALSE;
        if (status & EC_STATUS_OBF)
            return TRUE;
    }
    return FALSE;
}

static BOOL ec_read_reg(HANDLE dev, BYTE reg, BYTE *value)
{
    if (!ec_wait_ibf(dev)) return FALSE;
    if (!ec_write_port(dev, EC_CMD_PORT, EC_CMD_READ)) return FALSE;
    if (!ec_wait_ibf(dev)) return FALSE;
    if (!ec_write_port(dev, EC_DATA_PORT, reg)) return FALSE;
    if (!ec_wait_obf(dev)) return FALSE;
    return ec_read_port(dev, EC_DATA_PORT, value);
}

static BOOL ec_read_word(HANDLE dev, BYTE reg, WORD *value)
{
    BYTE low, high;
    if (!ec_read_reg(dev, reg, &low)) return FALSE;
    if (!ec_read_reg(dev, reg + 1, &high)) return FALSE;
    *value = ((WORD)high << 8) | low;
    return TRUE;
}

BOOL ec_is_available(HANDLE dev)
{
    BYTE status;
    if (!ec_read_port(dev, EC_CMD_PORT, &status))
        return FALSE;
    return (status != 0xFF);
}

int ec_read_fan_rpm(HANDLE dev, BYTE fan_reg)
{
    WORD raw;
    if (!ec_read_word(dev, fan_reg, &raw))
        return -1;
    if (raw == 0xFFFF || raw == 0x0000)
        return 0;
    return (int)raw;
}

int ec_read_temp(HANDLE dev, BYTE temp_reg)
{
    BYTE raw;
    if (!ec_read_reg(dev, temp_reg, &raw))
        return -1;
    return (int)raw;
}

int ec_read_voltage_mv(HANDLE dev, BYTE voltage_reg)
{
    WORD raw;
    if (!ec_read_word(dev, voltage_reg, &raw))
        return -1;
    return (int)raw;
}

ec_result_t ec_read_all_sensors(HANDLE dev)
{
    ec_result_t result = {0};

    if (!ec_is_available(dev)) {
        result.ok = 0;
        _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                    "EC not available or not responding");
        return result;
    }

    result.cpu_fan_rpm = ec_read_fan_rpm(dev, EC_REG_CPU_FAN_RPM);
    result.vrm_fan_rpm = ec_read_fan_rpm(dev, EC_REG_VRM_FAN_RPM);
    result.chipset_temp = ec_read_temp(dev, EC_REG_CHIPSET_TEMP);
    result.cpu_temp = ec_read_temp(dev, EC_REG_CPU_TEMP);
    result.mb_temp = ec_read_temp(dev, EC_REG_MB_TEMP);
    result.vrm_temp = ec_read_temp(dev, EC_REG_VRM_TEMP);
    result.cpu_voltage_mv = ec_read_voltage_mv(dev, EC_REG_CPU_VOLTAGE);

    result.ok = 1;
    return result;
}
