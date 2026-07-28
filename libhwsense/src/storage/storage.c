/*
 * storage.c — NVMe/SSD temperature reading via IOCTL_STORAGE_QUERY_PROPERTY.
 *
 * Enumerates physical drives and reads SMART temperature attribute.
 */

#include "../core/hwsense_internal.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX_DRIVES 16

int hwsense_nvme_temperature(void)
{
    int i;

    for (i = 0; i < MAX_DRIVES; i++) {
        char drive_path[64];
        _snprintf_s(drive_path, sizeof(drive_path), _TRUNCATE,
                    "\\\\.\\PhysicalDrive%d", i);

        HANDLE hDev = CreateFileA(
            drive_path,
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );

        if (hDev == INVALID_HANDLE_VALUE)
            continue;

        /* Try IOCTL_STORAGE_QUERY_PROPERTY with StorageDeviceTemperature */
        {
            STORAGE_PROPERTY_QUERY query = {0};
            DWORD bytesReturned = 0;
            DWORD temp = 0;

            query.PropertyId = 52;  /* StorageDeviceTemperature */
            query.QueryType = 0;    /* PropertyStandardQuery */

            BOOL ok = DeviceIoControl(
                hDev,
                IOCTL_STORAGE_QUERY_PROPERTY,
                &query, sizeof(query),
                &temp, sizeof(temp),
                &bytesReturned, NULL
            );

            CloseHandle(hDev);

            if (ok && bytesReturned >= sizeof(DWORD) && temp > 0 && temp < 200)
                return (int)temp;
        }

        /* IOCTL failed, try alternative path */
        CloseHandle(hDev);
    }

    return -1;
}
