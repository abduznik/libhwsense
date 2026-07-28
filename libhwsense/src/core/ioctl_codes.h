#ifndef IOCTL_CODES_H
#define IOCTL_CODES_H

/*
 * WinRing0 IOCTL codes and PCI config structures.
 *
 * All constants are byte-for-byte identical to OpenHardwareMonitor's
 * IOControlCode.cs, derived from:
 *
 *   CTL_CODE = (device_type << 16) | (access << 14) | (function << 2) | method
 *
 *   device_type = 40000 (0x9C40)  — WinRing0's custom type
 *   method      = METHOD_BUFFERED (0)
 *   access:
 *     FILE_ANY_ACCESS    (0) — used by MSR read
 *     FILE_READ_ACCESS   (1) — used by PCI config read
 *     FILE_WRITE_ACCESS  (2) — used by PCI config write
 */

#define OLS_TYPE 40000

/* Read an MSR.  Input: DWORD msr_index.  Output: DWORD64 msr_value. */
#define IOCTL_OLS_READ_MSR \
    CTL_CODE(OLS_TYPE, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)

/* Read PCI config.  Input: READ_PCI_CONFIG_INPUT.  Output: DWORD value. */
#define IOCTL_OLS_READ_PCI_CONFIG \
    CTL_CODE(OLS_TYPE, 0x851, METHOD_BUFFERED, FILE_READ_ACCESS)

/* Write PCI config.  Input: WRITE_PCI_CONFIG_INPUT.  Output: none. */
#define IOCTL_OLS_WRITE_PCI_CONFIG \
    CTL_CODE(OLS_TYPE, 0x852, METHOD_BUFFERED, FILE_WRITE_ACCESS)

/* Read physical memory.  Input: OLS_READ_MEMORY_INPUT.  Output: UnitSize*Count bytes. */
#define IOCTL_OLS_READ_MEMORY \
    CTL_CODE(OLS_TYPE, 0x841, METHOD_BUFFERED, FILE_READ_ACCESS)

/*
 * Physical memory read input struct — must match WinRing0 driver layout.
 * Address is LARGE_INTEGER (8 bytes) — NOT DWORD (4 bytes).
 * This is the #1 cause of "reads succeed but return zero":
 * if you only send 4 bytes for the address, the driver reads garbage.
 *
 * From OpenLibSys OlsIoctl.h: pack(4) to match the driver's expected layout.
 */
#pragma pack(push, 4)
typedef struct {
    long long Address;    /* LARGE_INTEGER physical address (8 bytes) */
    unsigned int UnitSize; /* 1, 2, or 4 bytes per unit */
    unsigned int Count;    /* number of units to read */
} OLS_READ_MEMORY_INPUT;
#pragma pack(pop)

/*
 * PCI config input structures — must match WinRing0 driver layout exactly.
 * Both are packed (no padding); DWORD alignment is natural here.
 */

typedef struct {
    DWORD PciAddress;   /* (bus << 8) | (device << 3) | function */
    DWORD RegAddress;   /* PCI config register offset */
} READ_PCI_CONFIG_INPUT;

typedef struct {
    DWORD PciAddress;
    DWORD RegAddress;
    DWORD Value;
} WRITE_PCI_CONFIG_INPUT;

/*
 * Intel MSR addresses (Intel SDM Vol. 4).
 */
#define MSR_IA32_THERM_STATUS        0x19C
#define MSR_IA32_TEMPERATURE_TARGET  0x1A2

/*
 * AMD SMN (System Management Network) constants.
 *
 * SMN is accessed through PCI config space on Bus 0 / Device 0 / Function 0
 * (the Root Complex / Northbridge):
 *   1. Write SMN address to PCI config offset 0x60
 *   2. Read data from PCI config offset 0x64
 */
#define AMD_F17H_TEMP_REGISTER  0x00059800
#define SMN_INDEX_OFFSET        0x60
#define SMN_DATA_OFFSET         0x64

/*
 * AMD SVI2 (Serial Voltage Identification 2) telemetry registers.
 *
 * SVI2 provides real-time voltage and current telemetry for CPU power planes.
 * Accessed via the same SMN bus as temperature (PCI config 0x60/0x64).
 *
 * SMN 0x0005A008 — SVI0_TELEM_SVI: telemetry status/config
 * SMN 0x0005A010 — SVI0_PLANE0:    core voltage + current (VDDCR_CPU)
 * SMN 0x0005A00C — SVI0_PLANE1:    SoC voltage + current (VDDCR_SOC)
 *
 * PLANE0 bit layout (from LibreHardwareMonitor / ryzen_smu):
 *   [24:16]  — SVI2_VID: voltage ID (9 bits), encoding: voltage = SVI2_VID * 0.00625 V
 *   [7:0]    — SVI2_TEF: current telemetry (amps), encoding depends on configuration
 *
 * PLANE1 bit layout:
 *   [24:16]  — SVI2_VID: SoC voltage ID (same encoding as PLANE0)
 *   [7:0]    — SVI2_TEF: SoC current telemetry
 *
 * Note: SVI2_VID = 0x1FF (all bits set) typically means the plane is off / not present.
 */
#define AMD_SVI2_TELEM_SVI     0x0005A008
#define AMD_SVI2_PLANE0        0x0005A010
#define AMD_SVI2_PLANE1        0x0005A00C

/* Voltage encoding: VID * 6.25 mV = voltage in mV, or VID * 0.00625 V */
#define AMD_SVI2_VID_TO_MV(vid)  ((vid) * 6.25)
#define AMD_SVI2_VID_TO_V(vid)   ((vid) * 0.00625)

/*
 * AMD SMU (System Management Unit) mailbox registers.
 *
 * The SMU has multiple mailboxes (RSMU, MP1, HSMP) accessed via SMN.
 * We use the RSMU mailbox for PM table queries and transfer.
 *
 * Register layout per mailbox:
 *   MSG  — write command ID here to send a request
 *   RSP  — read response status (0x01 = OK, 0xFF = failed, 0x00 = busy)
 *   ARG  — read/write arguments and results (6 DWORDs at ARG, ARG+4, ..., ARG+20)
 *
 * Addresses vary by CPU family/model.
 */

/* Zen2 Matisse (Family 17h Model 71h) — RSMU mailbox */
#define AMD_SMU_RSMU_MSG_Matisse    0x03B10524
#define AMD_SMU_RSMU_RSP_Matisse    0x03B10570
#define AMD_SMU_RSMU_ARG_Matisse    0x03B10A40

/* Zen2 Renoir (Family 17h Model 60h) — RSMU mailbox */
#define AMD_SMU_RSMU_MSG_Renoir     0x03B10A20
#define AMD_SMU_RSMU_RSP_Renoir     0x03B10A80
#define AMD_SMU_RSMU_ARG_Renoir     0x03B10A88

/* Zen3 Cezanne (Family 19h Model 50h) — uses same as Renoir */
#define AMD_SMU_RSMU_MSG_Cezanne    0x03B10A20
#define AMD_SMU_RSMU_RSP_Cezanne    0x03B10A80
#define AMD_SMU_RSMU_ARG_Cezanne    0x03B10A88

/* SMU response status codes */
#define SMU_RSP_OK       0x01
#define SMU_RSP_FAIL     0xFF
#define SMU_RSP_UNKNOWN  0xFE

/* SMU PM table command IDs by CPU family */
#define SMU_CMD_GET_TABLE_VERSION_Matisse   0x08
#define SMU_CMD_GET_DRAM_BASE_Matisse       0x06
#define SMU_CMD_TRANSFER_TO_DRAM_Matisse    0x05

#define SMU_CMD_GET_TABLE_VERSION_Renoir    0x06
#define SMU_CMD_GET_DRAM_BASE_Renoir        0x66
#define SMU_CMD_TRANSFER_TO_DRAM_Renoir     0x65

#define SMU_CMD_GET_TABLE_VERSION_Cezanne   0x06
#define SMU_CMD_GET_DRAM_BASE_Cezanne       0x66
#define SMU_CMD_TRANSFER_TO_DRAM_Cezanne    0x65

/* PM table power offset (SoC power in watts, as float) by CPU family.
 * These are byte offsets into the PM table DRAM buffer.
 * Index 67 * 4 = 0x10C for Renoir/Cezanne, varies for Matisse. */
#define AMD_PM_TABLE_POWER_OFFSET_Matisse   0x1CC  /* SoC temp, not power — Matisse needs different offset */
#define AMD_PM_TABLE_POWER_OFFSET_Renoir    0x10C  /* SoC Power (float, watts) at index 67 */
#define AMD_PM_TABLE_POWER_OFFSET_Cezanne   0x10C  /* SoC Power (float, watts) at index 67 */

/*
 * WinRing0 driver service and device names.
 */
#define WINRING0_SERVICE_NAME  L"WinRing0_1_2_0"
#define WINRING0_DEVICE_PATH   L"\\\\.\\WinRing0_1_2_0"
#define WINRING0_SYS_FILENAME  L"WinRing0x64.sys"
#define WINRING0_SYS_FILENAME_A "WinRing0x64.sys"

#endif /* IOCTL_CODES_H */
