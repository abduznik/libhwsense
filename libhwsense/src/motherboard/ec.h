/*
 * ec.h — ACPI Embedded Controller access API.
 */

#ifndef EC_H
#define EC_H

#include <windows.h>

/* ASUS EC register addresses */
#define EC_REG_CPU_FAN_RPM      0x00BC
#define EC_REG_VRM_FAN_RPM      0x00B2
#define EC_REG_CHIPSET_TEMP     0x003A
#define EC_REG_CPU_TEMP         0x003B
#define EC_REG_MB_TEMP          0x003C
#define EC_REG_VRM_TEMP         0x003E
#define EC_REG_CPU_VOLTAGE      0x00A2
#define EC_REG_CPU_CURRENT      0x00F4

/* EC result structure */
typedef struct {
    int ok;
    int cpu_fan_rpm;
    int vrm_fan_rpm;
    int chipset_temp;
    int cpu_temp;
    int mb_temp;
    int vrm_temp;
    int cpu_voltage_mv;
    int cpu_current_amps;
    char error[256];
} ec_result_t;

/* Check if EC is available */
BOOL ec_is_available(HANDLE dev);

/* Read EC sensors */
ec_result_t ec_read_all_sensors(HANDLE dev);

/* Individual sensor reads */
int ec_read_fan_rpm(HANDLE dev, BYTE fan_reg);
int ec_read_temp(HANDLE dev, BYTE temp_reg);
int ec_read_voltage_mv(HANDLE dev, BYTE voltage_reg);

#endif /* EC_H */
