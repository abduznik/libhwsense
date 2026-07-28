/*
 * storage.c — NVMe/SSD temperature reading via SMART passthrough.
 *
 * Uses IOCTL_ATA_PASS_THROUGH to read SMART data from drives.
 * Supports both SATA (ATA SMART) and NVMe drives.
 */

#include "../core/hwsense_internal.h"
#include <stdio.h>
#include <stdlib.h>

#define MAX_DRIVES 16

/* ATA/SMART defines */
#define ATA_IDENTIFY_DEVICE    0xEC
#define ATA_SMART_READ_DATA    0xD0
#define ATA_SMART_RETURN_SMART_STATUS  0xDA

/* ATA pass-through structure */
#pragma pack(push, 1)
typedef struct {
    WORD protocol;
    WORD flags;
    WORD words;
    BYTE features;
    BYTE features_hi;
    BYTE sector_count;
    BYTE sector_count_hi;
    BYTE lba_low;
    BYTE lba_mid;
    BYTE lba_high;
    BYTE device;
    BYTE command;
    BYTE reserved;
} ATA_PASS_THROUGH;

typedef struct {
    WORD status;
    WORD error;
} ATA_STATUS;
#pragma pack(pop)

/* SMART attribute IDs */
#define SMART_TEMPERATURE_ATTRIBUTE  0xE2

/*
 * Read temperature from a drive using ATA SMART.
 * Returns temperature in Celsius, or -1 on failure.
 */
static int read_drive_temp_ata(const char *drive_path)
{
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
        return -1;

    /* Try ATA IDENTIFY DEVICE to get temperature */
    BYTE data[512] = {0};
    ATA_PASS_THROUGH pt = {0};
    DWORD bytes_returned = 0;

    pt.protocol = 3;    /* ATA protocol */
    pt.flags = 0;       /* No data transfer */
    pt.command = ATA_IDENTIFY_DEVICE;
    pt.lba_mid = 0x48;  /* "ATAPID" signature */
    pt.lba_high = 0x01;

    BOOL ok = DeviceIoControl(
        hDev,
        0x002D0C08,  /* IOCTL_ATA_PASS_THROUGH */
        &pt, sizeof(pt),
        data, sizeof(data),
        &bytes_returned,
        NULL
    );

    CloseHandle(hDev);

    if (!ok)
        return -1;

    /* Parse IDENTIFY DEVICE response */
    WORD *words = (WORD *)data;

    /* Word 209 contains temperature */
    int temp = (int)(words[209] & 0xFF);

    /* Sanity check */
    if (temp < 0 || temp > 127)
        return -1;

    return temp;
}

/*
 * Read SMART temperature attribute.
 */
static int read_smart_temp(const char *drive_path)
{
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
        return -1;

    /* Send ATA SMART READ DATA command */
    BYTE data[512] = {0};
    ATA_PASS_THROUGH pt = {0};
    DWORD bytes_returned = 0;

    pt.protocol = 3;
    pt.flags = 0x02;  /* Data in from device */
    pt.words = 256;   /* 256 words = 512 bytes */
    pt.sector_count = 1;
    pt.command = ATA_SMART_READ_DATA;
    pt.lba_mid = 0x4F;
    pt.lba_high = 0xC2;

    BOOL ok = DeviceIoControl(
        hDev,
        0x002D0C08,  /* IOCTL_ATA_PASS_THROUGH */
        &pt, sizeof(pt),
        data, sizeof(data),
        &bytes_returned,
        NULL
    );

    CloseHandle(hDev);

    if (!ok)
        return -1;

    /* SMART attributes start at offset 2 */
    /* Each attribute is 12 bytes */
    for (int i = 0; i < 30; i++) {
        BYTE attr_id = data[2 + i * 12];
        if (attr_id == 0xE2 || attr_id == 0xC2) {  /* Temperature */
            BYTE value = data[2 + i * 12 + 3];
            if (value > 0 && value < 127)
                return (int)value;
        }
    }

    return -1;
}

/*
 * Result structure for drive temperature reading.
 */
typedef struct {
    int ok;
    int temperature;
    char drive_name[128];
    char error[256];
} hwsense_storage_result_t;

/*
 * Read NVMe/SSD temperature.
 * Iterates through physical drives and returns the first valid reading.
 */
hwsense_storage_result_t hwsense_nvme_temperature_full(void)
{
    hwsense_storage_result_t result = {0};
    int i;

    for (i = 0; i < MAX_DRIVES; i++) {
        char drive_path[64];
        _snprintf_s(drive_path, sizeof(drive_path), _TRUNCATE,
                    "\\\\.\\PhysicalDrive%d", i);

        int temp = read_smart_temp(drive_path);
        if (temp < 0)
            temp = read_drive_temp_ata(drive_path);

        if (temp > 0) {
            result.ok = 1;
            result.temperature = temp;
            _snprintf_s(result.drive_name, sizeof(result.drive_name), _TRUNCATE,
                        "PhysicalDrive%d", i);
            return result;
        }
    }

    result.ok = 0;
    _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                "No NVMe/SSD temperature available");
    return result;
}

/*
 * Simple temperature read (returns first found).
 */
int hwsense_nvme_temperature(void)
{
    hwsense_storage_result_t result = hwsense_nvme_temperature_full();
    return result.ok ? result.temperature : -1;
}
