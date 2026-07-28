#ifndef EC_H
#define EC_H

#include "../../include/hwsense.h"

/* ASUS EC register addresses */
#define EC_REG_CPU_FAN_RPM      0x00BC
#define EC_REG_VRM_FAN_RPM      0x00B2
#define EC_REG_CHIPSET_TEMP     0x003A
#define EC_REG_CPU_TEMP         0x003B
#define EC_REG_MB_TEMP          0x003C
#define EC_REG_VRM_TEMP         0x003E
#define EC_REG_CPU_VOLTAGE      0x00A2
#define EC_REG_CPU_CURRENT      0x00F4

/* ec_result_t defined in hwsense.h */
HWSENSE_API ec_result_t ec_read_all_sensors(HANDLE dev);

#endif /* EC_H */
