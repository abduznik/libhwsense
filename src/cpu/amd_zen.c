/*
 * amd_zen.c — AMD Zen (Family 17h+) core functions.
 *
 * Vendor detection, PCI config read/write, SMN access, temperature reading,
 * CPU frequency via SMN register 0x000598F4.
 */

#include "../core/hwsense_internal.h"
#include "../core/ioctl_codes.h"
#include <stdio.h>
#include <intrin.h>

/*
 * AMD SMN address for core frequency (F17h M00h: 0x000598F4)
 * Bits [15:0] = current frequency in MHz
 */
#define AMD_F17H_CUR_FREQ_SMN  0x000598F4

/* AMD MSR addresses for CPU frequency */
#define MSR_HW_PSTATE_STATUS   0xC0010293  /* Current P-state: Fid[7:0], DfsId[13:8] */
#define MSR_MPERF              0xC00000E7  /* Max frequency reference counter */
#define MSR_APERF              0xC00000E8  /* Actual frequency counter */

/*
 * Detect CPU vendor from the registry.
 * Returns 'I' for Intel, 'A' for AMD, '?' for unknown.
 */
int hwsense_detect_vendor(void)
{
    HKEY key;
    char vendor[64] = {0};
    DWORD size = sizeof(vendor);
    DWORD type = 0;

    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE,
                      "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return '?';

    if (RegQueryValueExA(key, "VendorIdentifier", NULL, &type,
                         (LPBYTE)vendor, &size) != ERROR_SUCCESS) {
        RegCloseKey(key);
        return '?';
    }
    RegCloseKey(key);

    if (strstr(vendor, "Intel"))
        return 'I';
    if (strstr(vendor, "AMD") || strstr(vendor, "AuthenticAMD"))
        return 'A';
    return '?';
}

/*
 * Read a DWORD from PCI config space via WinRing0.
 */
static BOOL pci_read_dword(HANDLE dev, DWORD pci_addr, DWORD reg, DWORD *out_val)
{
    READ_PCI_CONFIG_INPUT inp = { pci_addr, reg };
    DWORD out = 0;
    DWORD bytes_ret = 0;

    BOOL ok = DeviceIoControl(
        dev, IOCTL_OLS_READ_PCI_CONFIG,
        &inp, sizeof(inp),
        &out, sizeof(out),
        &bytes_ret, NULL
    );

    if (!ok)
        return FALSE;

    *out_val = out;
    return TRUE;
}

/*
 * Write a DWORD to PCI config space via WinRing0.
 */
static BOOL pci_write_dword(HANDLE dev, DWORD pci_addr, DWORD reg, DWORD value)
{
    WRITE_PCI_CONFIG_INPUT inp = { pci_addr, reg, value };
    DWORD bytes_ret = 0;

    return DeviceIoControl(
        dev, IOCTL_OLS_WRITE_PCI_CONFIG,
        &inp, sizeof(inp),
        NULL, 0,
        &bytes_ret, NULL
    );
}

/*
 * Pack bus/device/function into the uint32 PCI address WinRing0 expects.
 */
static DWORD pci_address(DWORD bus, DWORD device, DWORD function)
{
    return ((bus & 0xFF) << 8) | ((device & 0x1F) << 3) | (function & 7);
}

/*
 * Read a register via AMD's SMN (System Management Network).
 */
BOOL smn_read(HANDLE dev, DWORD smn_addr, DWORD *out_val)
{
    DWORD nb = pci_address(0, 0, 0);

    if (!pci_write_dword(dev, nb, SMN_INDEX_OFFSET, smn_addr))
        return FALSE;

    return pci_read_dword(dev, nb, SMN_DATA_OFFSET, out_val);
}

/*
 * Write a register via AMD's SMN.
 */
BOOL smn_write_dword(HANDLE driver_handle, DWORD smn_addr, DWORD value)
{
    DWORD nb = pci_address(0, 0, 0);
    if (!pci_write_dword(driver_handle, nb, SMN_INDEX_OFFSET, smn_addr))
        return FALSE;
    return pci_write_dword(driver_handle, nb, SMN_DATA_OFFSET, value);
}

/*
 * Read AMD package temperature (Tctl) via SMN register 0x00059800.
 */
hwsense_temp_result_t hwsense_amd_package_temp(HANDLE driver_handle)
{
    hwsense_temp_result_t r = {0};
    DWORD raw = 0;
    int offset_flag;
    double temp;

    if (!smn_read(driver_handle, AMD_F17H_TEMP_REGISTER, &raw)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SMN read of register 0x%08X failed (error %lu)",
                    AMD_F17H_TEMP_REGISTER, GetLastError());
        return r;
    }

    offset_flag = ((raw & 0x80000) != 0) || ((raw & 0x30000) == 0x30000);

    temp = ((double)(raw >> 21) * 125.0) / 1000.0;
    if (offset_flag)
        temp -= 49.0;

    r.ok = 1;
    r.celsius = temp;
    return r;
}

/*
 * AMD CCD (Core Complex Die) temperature registers.
 */
#define AMD_CCD_TEMP_BASE    0x00059954
#define AMD_CCD_TEMP_STRIDE  0x4

hwsense_ccd_temps_t hwsense_amd_ccd_temps(hwsense_ctx_t *ctx)
{
    hwsense_ccd_temps_t result = {0};
    int i;

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE)
        return result;

    for (i = 0; i < HWSENSE_MAX_CCD; i++) {
        DWORD raw = 0;
        DWORD smn_addr = AMD_CCD_TEMP_BASE + (i * AMD_CCD_TEMP_STRIDE);

        result.available[i] = 0;

        if (!smn_read(ctx->driver_handle, smn_addr, &raw))
            continue;

        DWORD raw12 = raw & 0xFFF;

        if (raw12 == 0 && raw == 0)
            continue;

        double temp = ((double)(raw12 * 125) - 305000.0) / 1000.0;

        if (temp < -40.0 || temp > 150.0)
            continue;

        result.celsius[i] = temp;
        result.available[i] = 1;
        result.count++;
    }

    return result;
}

/*
 * Diagnostic: read raw SMN register value.
 */
int hwsense_amd_read_smn(HANDLE driver_handle, DWORD smn_addr, DWORD *out_value)
{
    return smn_read(driver_handle, smn_addr, out_value) ? 1 : 0;
}

/*
 * Read AMD CPU frequency via MSR 0xC0010293 (Hardware P-state Status).
 *
 * MSR layout:
 *   [7:0]   = CpuFid (core frequency ID)
 *   [13:8]  = CpuDfsId (core divisor ID)
 *   [21:14] = CpuVid (voltage ID)
 *
 * Formula: CoreCOF = (CpuFid / CpuDfsId) * 200 MHz
 *
 * Returns frequency in MHz, or -1 on failure.
 */
int hwsense_amd_cpu_freq(HANDLE driver_handle)
{
    DWORD64 msr_val;

    /* Pin to core 0 and read MSR */
    HANDLE thread = GetCurrentThread();
    DWORD_PTR old_mask = SetThreadAffinityMask(thread, 1);
    if (!old_mask)
        return -1;

    DWORD in_val = MSR_HW_PSTATE_STATUS;
    DWORD64 out_val = 0;
    DWORD bytes_ret = 0;

    BOOL ok = DeviceIoControl(
        driver_handle, IOCTL_OLS_READ_MSR,
        &in_val, sizeof(in_val),
        &out_val, sizeof(out_val),
        &bytes_ret, NULL
    );

    SetThreadAffinityMask(thread, old_mask);

    if (!ok)
        return -1;

    DWORD eax = (DWORD)(out_val & 0xFFFFFFFF);
    DWORD cpuFid = eax & 0xFF;
    DWORD cpuDfsId = (eax >> 8) & 0x3F;

    if (cpuDfsId == 0)
        return -1;

    /* CoreCOF = (CpuFid / CpuDfsId) * 200 */
    int freq = (int)((cpuFid * 200.0) / cpuDfsId);

    /* Sanity check */
    if (freq < 400 || freq > 8000)
        return -1;

    return freq;
}
