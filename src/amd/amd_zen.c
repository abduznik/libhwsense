/*
 * amd_zen.c — AMD Zen (Family 17h+) temperature reading via SMN.
 *
 * Reads SMN register 0x00059800 through PCI config space on
 * Bus 0 / Device 0 / Function 0 (Root Complex).
 *
 * SMN access protocol:
 *   1. Write SMN address to PCI config offset 0x60
 *   2. Read data from PCI config offset 0x64
 *
 * AMD reports a single Tctl (package temperature), not per-core.
 */

#include "../common/hwsense_internal.h"
#include "../common/ioctl_codes.h"
#include <stdio.h>
#include <intrin.h>

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
 *
 * SMN is accessed through PCI config space on Bus 0 / Dev 0 / Func 0:
 *   1. Write SMN address to config offset 0x60
 *   2. Read data from config offset 0x64
 */
static BOOL smn_read(HANDLE dev, DWORD smn_addr, DWORD *out_val)
{
    DWORD nb = pci_address(0, 0, 0);  /* Root Complex */

    if (!pci_write_dword(dev, nb, SMN_INDEX_OFFSET, smn_addr))
        return FALSE;

    return pci_read_dword(dev, nb, SMN_DATA_OFFSET, out_val);
}

/*
 * Read AMD package temperature (Tctl) via SMN register 0x00059800.
 *
 * Bit layout of SMN 0x00059800:
 *   [31:21]  — CUR_TEMP (current temperature in 0.125C units)
 *   [19]     — RANGE_SEL (if set, apply -49C offset)
 *   [17:16]  — TJ_SEL (if == 0b11, also signals -49C offset)
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

    /* Check offset flags */
    offset_flag = ((raw & 0x80000) != 0) || ((raw & 0x30000) == 0x30000);

    /* Extract temperature: bits [31:21], scaled by 0.125C per unit */
    temp = ((double)(raw >> 21) * 125.0) / 1000.0;
    if (offset_flag)
        temp -= 49.0;

    r.ok = 1;
    r.celsius = temp;
    return r;
}

/*
 * AMD CCD (Core Complex Die) temperature registers.
 *
 * Documented for Matisse (Family 17h Model 0x71):
 *   SMN 0x00059954 = CCD0 temperature
 *   SMN 0x00059958 = CCD1 temperature
 *   SMN 0x0005995C = CCD2 temperature (if present)
 *   SMN 0x00059960 = CCD3 temperature (if present)
 *
 * Formula:
 *   raw12 = raw & 0xFFF          (12-bit value)
 *   temp  = ((raw12 * 125) - 305000) / 1000.0
 *
 * For Renoir (Family 17h Model 0x60): these registers may or may not be
 * present.  We read them and check if the value is sane (0 < temp < 150)
 * to determine availability.
 */
#define AMD_CCD_TEMP_BASE    0x00059954
#define AMD_CCD_TEMP_STRIDE  0x4   /* +4 per CCD */

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

        /* Extract 12-bit temperature value */
        DWORD raw12 = raw & 0xFFF;

        /* Sanity check: if raw12 is 0 or the full register is 0, sensor not present */
        if (raw12 == 0 && raw == 0)
            continue;

        /* Formula: ((raw12 * 125) - 305000) / 1000.0 */
        double temp = ((double)(raw12 * 125) - 305000.0) / 1000.0;

        /* Sanity check: temperature should be between -40 and 150C */
        if (temp < -40.0 || temp > 150.0)
            continue;

        result.celsius[i] = temp;
        result.available[i] = 1;
        result.count++;
    }

    return result;
}

/*
 * Read AMD core voltage (VDDCR_CPU) via SVI2 Plane0 register.
 *
 * SMN 0x0005A010 bit layout:
 *   [24:16]  — SVI2_VID: 9-bit voltage ID
 *              Encoding: voltage = VID * 0.00625 V  (6.25 mV per LSB)
 *              VID = 0x1FF typically means the plane is off / not present.
 *   [7:0]    — SVI2_TEF: current telemetry (amps)
 */
hwsense_voltage_result_t hwsense_amd_core_voltage(HANDLE driver_handle)
{
    hwsense_voltage_result_t r = {0};
    DWORD raw = 0;

    if (!smn_read(driver_handle, AMD_SVI2_PLANE0, &raw)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SMN read of SVI2 Plane0 (0x%08X) failed (error %lu)",
                    AMD_SVI2_PLANE0, GetLastError());
        return r;
    }

    /* Extract SVI2 VID: bits [24:16] — 9 bits */
    DWORD vid = (raw >> 16) & 0x1FF;

    /* VID = 0x1FF means the plane is off / not present */
    if (vid == 0x1FF) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SVI2 Plane0 VID = 0x1FF (plane off / not present)");
        return r;
    }

    /* Extract current telemetry: bits [7:0] */
    DWORD current_raw = raw & 0xFF;

    r.ok = 1;
    r.volts = AMD_SVI2_VID_TO_V(vid);
    r.amps = (double)current_raw;
    return r;
}

/*
 * Read AMD SoC voltage (VDDCR_SOC) via SVI2 Plane1 register.
 *
 * SMN 0x0005A00C — same bit layout as Plane0:
 *   [24:16]  — SVI2_VID: SoC voltage ID (same encoding: VID * 0.00625 V)
 *   [7:0]    — SVI2_TEF: SoC current telemetry
 */
hwsense_voltage_result_t hwsense_amd_soc_voltage(HANDLE driver_handle)
{
    hwsense_voltage_result_t r = {0};
    DWORD raw = 0;

    if (!smn_read(driver_handle, AMD_SVI2_PLANE1, &raw)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SMN read of SVI2 Plane1 (0x%08X) failed (error %lu)",
                    AMD_SVI2_PLANE1, GetLastError());
        return r;
    }

    DWORD vid = (raw >> 16) & 0x1FF;

    if (vid == 0x1FF) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SVI2 Plane1 VID = 0x1FF (plane off / not present)");
        return r;
    }

    DWORD current_raw = raw & 0xFF;

    r.ok = 1;
    r.volts = AMD_SVI2_VID_TO_V(vid);
    r.amps = (double)current_raw;
    return r;
}

/* ── SMU Mailbox Communication ──────────────────────────────────────── */

static BOOL smn_write_dword(HANDLE driver_handle, DWORD smn_addr, DWORD value)
{
    DWORD nb = pci_address(0, 0, 0);
    if (!pci_write_dword(driver_handle, nb, SMN_INDEX_OFFSET, smn_addr))
        return FALSE;
    return pci_write_dword(driver_handle, nb, SMN_DATA_OFFSET, value);
}

/*
 * AMD CPU codenames detected via CPUID family/model.
 */
typedef enum {
    AMD_CODENAME_UNKNOWN = 0,
    AMD_CODENAME_MATISSE,      /* Family 17h Model 0x71 */
    AMD_CODENAME_RENOIR,       /* Family 17h Model 0x60 */
    AMD_CODENAME_VERMEER,      /* Family 19h Model 0x20/0x21 */
    AMD_CODENAME_CEZANNE,      /* Family 19h Model 0x50 */
    AMD_CODENAME_MILAN,        /* Family 19h Model 0x01 */
    AMD_CODENAME_CASTLEPEAK,   /* Family 17h Model 0x31 */
} amd_codename_t;

/*
 * Detect AMD CPU codename from CPUID leaf 0x80000001.
 * CPUID.EAX[27:20] = extended model, CPUID.EAX[11:8] = base model.
 * CPUID.EAX[31:28] = extended family, CPUID.EAX[11:8] = base family.
 */
static amd_codename_t detect_codename(void)
{
    int cpuinfo[4] = {0};
    __cpuid(cpuinfo, 0x80000001);
    unsigned int ext_family = (cpuinfo[0] >> 20) & 0xFF;
    unsigned int base_family = (cpuinfo[0] >> 8) & 0xF;
    unsigned int ext_model = (cpuinfo[0] >> 16) & 0xF;
    unsigned int base_model = (cpuinfo[0] >> 4) & 0xF;

    unsigned int family = base_family + ext_family;
    unsigned int model = (ext_model << 4) | base_model;

    if (family == 0x17) {
        switch (model) {
        case 0x71: return AMD_CODENAME_MATISSE;
        case 0x60: return AMD_CODENAME_RENOIR;
        case 0x31: return AMD_CODENAME_CASTLEPEAK;
        default:   return AMD_CODENAME_UNKNOWN;
        }
    } else if (family == 0x19) {
        switch (model) {
        case 0x21:
        case 0x20: return AMD_CODENAME_VERMEER;
        case 0x50: return AMD_CODENAME_CEZANNE;
        case 0x01: return AMD_CODENAME_MILAN;
        default:   return AMD_CODENAME_UNKNOWN;
        }
    }
    return AMD_CODENAME_UNKNOWN;
}

/*
 * Per-codename MP1 and RSMU mailbox register addresses.
 *
 * MP1 mailbox is used for general SMU queries (SMU version, etc.)
 * RSMU mailbox is used for PM table commands.
 */
typedef struct {
    DWORD mp1_msg;
    DWORD mp1_rsp;
    DWORD mp1_arg;
    DWORD rsmu_msg;
    DWORD rsmu_rsp;
    DWORD rsmu_arg;
    DWORD cmd_get_version;
    DWORD cmd_get_dram_base;
    DWORD cmd_transfer_to_dram;
    DWORD cmd_transfer_arg0;
    DWORD cpuid_family;
    DWORD cpuid_model;
    const char *name;
} smu_config_t;

static const smu_config_t smu_configs[] = {
    /* Matisse (Family 17h Model 0x71) */
    { .mp1_msg = 0x3B10530, .mp1_rsp = 0x3B1057C, .mp1_arg = 0x3B109C4,
      .rsmu_msg = 0x3B10524, .rsmu_rsp = 0x3B10570, .rsmu_arg = 0x3B10A40,
      .cmd_get_version = 0x08, .cmd_get_dram_base = 0x06,
      .cmd_transfer_to_dram = 0x05, .cmd_transfer_arg0 = 0,
      .cpuid_family = 0x17, .cpuid_model = 0x71, .name = "Matisse" },
    /* Renoir (Family 17h Model 0x60) */
    { .mp1_msg = 0x3B10528, .mp1_rsp = 0x3B10564, .mp1_arg = 0x3B10998,
      .rsmu_msg = 0x3B10A20, .rsmu_rsp = 0x3B10A80, .rsmu_arg = 0x3B10A88,
      .cmd_get_version = 0x06, .cmd_get_dram_base = 0x66,
      .cmd_transfer_to_dram = 0x65, .cmd_transfer_arg0 = 3,
      .cpuid_family = 0x17, .cpuid_model = 0x60, .name = "Renoir" },
    /* Vermeer (Family 19h Model 0x21) */
    { .mp1_msg = 0x3B10530, .mp1_rsp = 0x3B1057C, .mp1_arg = 0x3B109C4,
      .rsmu_msg = 0x3B10524, .rsmu_rsp = 0x3B10570, .rsmu_arg = 0x3B10A40,
      .cmd_get_version = 0x08, .cmd_get_dram_base = 0x06,
      .cmd_transfer_to_dram = 0x05, .cmd_transfer_arg0 = 0,
      .cpuid_family = 0x19, .cpuid_model = 0x21, .name = "Vermeer" },
    /* Cezanne (Family 19h Model 0x50) */
    { .mp1_msg = 0x3B10528, .mp1_rsp = 0x3B10564, .mp1_arg = 0x3B10998,
      .rsmu_msg = 0x3B10A20, .rsmu_rsp = 0x3B10A80, .rsmu_arg = 0x3B10A88,
      .cmd_get_version = 0x06, .cmd_get_dram_base = 0x66,
      .cmd_transfer_to_dram = 0x65, .cmd_transfer_arg0 = 0,
      .cpuid_family = 0x19, .cpuid_model = 0x50, .name = "Cezanne" },
    /* Milan (Family 19h Model 0x01) */
    { .mp1_msg = 0x3B10530, .mp1_rsp = 0x3B1057C, .mp1_arg = 0x3B109C4,
      .rsmu_msg = 0x3B10524, .rsmu_rsp = 0x3B10570, .rsmu_arg = 0x3B10A40,
      .cmd_get_version = 0x08, .cmd_get_dram_base = 0x06,
      .cmd_transfer_to_dram = 0x05, .cmd_transfer_arg0 = 0,
      .cpuid_family = 0x19, .cpuid_model = 0x01, .name = "Milan" },
};

#define SMU_CONFIG_COUNT (sizeof(smu_configs) / sizeof(smu_configs[0]))

static const smu_config_t *get_smu_config(void)
{
    int cpuinfo[4] = {0};
    unsigned int ext_family, base_family, ext_model, base_model;
    unsigned int family, model;
    int i;

    __cpuid(cpuinfo, 0x80000001);
    ext_family = (cpuinfo[0] >> 20) & 0xFF;
    base_family = (cpuinfo[0] >> 8) & 0xF;
    ext_model = (cpuinfo[0] >> 16) & 0xF;
    base_model = (cpuinfo[0] >> 4) & 0xF;
    family = base_family + ext_family;
    model = (ext_model << 4) | base_model;

    for (i = 0; i < (int)SMU_CONFIG_COUNT; i++) {
        if ((unsigned int)smu_configs[i].cpuid_family == family &&
            (unsigned int)smu_configs[i].cpuid_model == model)
            return &smu_configs[i];
    }
    return NULL;
}

#define SMU_TIMEOUT 10000

/*
 * Send a command to an SMU mailbox and wait for response.
 *
 * Protocol:
 *   1. Poll RSP until non-zero (previous request done), then write RSP = 0 to clear.
 *   2. Write ARG0 (and further args if needed) to the ARG registers.
 *   3. Write the command ID to MSG.
 *   4. Poll RSP until non-zero:
 *      0x1 = OK, 0x2 = rejected/busy, 0xFE = unsupported, 0xFF = in progress/bad arg
 *   5. Read ARG0 back — that's the response payload.
 *
 * Returns the RSP value (check for 0x01 = OK), or 0 on timeout.
 */
static DWORD smu_send_command(HANDLE driver_handle, DWORD msg_addr, DWORD rsp_addr,
                              DWORD arg_addr, DWORD cmd,
                              const DWORD *args, int arg_count)
{
    DWORD rsp;
    int i, timeout;

    /* Step 1: Wait for previous command to finish (RSP becomes non-zero) */
    for (timeout = 0; timeout < SMU_TIMEOUT; timeout++) {
        if (!smn_read(driver_handle, rsp_addr, &rsp))
            return 0;
        if (rsp != 0)
            break;
        SwitchToThread();
    }
    if (timeout >= SMU_TIMEOUT)
        return 0;

    /* Step 2: Clear RSP by writing 0 */
    smn_write_dword(driver_handle, rsp_addr, 0);

    /* Step 3: Write arguments to ARG registers */
    for (i = 0; i < arg_count && i < 6; i++) {
        if (!smn_write_dword(driver_handle, arg_addr + (i * 4), args[i]))
            return 0;
    }

    /* Step 4: Send command by writing to MSG register */
    if (!smn_write_dword(driver_handle, msg_addr, cmd))
        return 0;

    /* Step 5: Wait for response (RSP becomes non-zero) */
    for (timeout = 0; timeout < SMU_TIMEOUT; timeout++) {
        if (!smn_read(driver_handle, rsp_addr, &rsp))
            return 0;
        if (rsp != 0)
            break;
        SwitchToThread();
    }
    if (timeout >= SMU_TIMEOUT)
        return 0;

    return rsp;
}

/*
 * Read the SMU firmware version via MP1 mailbox command 0x02.
 */
static DWORD smu_get_version(HANDLE driver_handle, const smu_config_t *cfg)
{
    DWORD args[1] = {0};
    DWORD rsp = smu_send_command(driver_handle, cfg->mp1_msg, cfg->mp1_rsp,
                                 cfg->mp1_arg, 0x02, args, 0);
    if (rsp != 0x01)
        return 0;
    DWORD version = 0;
    smn_read(driver_handle, cfg->mp1_arg, &version);
    return version;
}

/*
 * Read the PM table version via RSMU mailbox.
 */
static DWORD smu_get_pm_table_version(HANDLE driver_handle, const smu_config_t *cfg)
{
    DWORD args[1] = {0};
    DWORD rsp = smu_send_command(driver_handle, cfg->rsmu_msg, cfg->rsmu_rsp,
                                 cfg->rsmu_arg, cfg->cmd_get_version, args, 0);
    if (rsp != 0x01)
        return 0;
    DWORD version = 0;
    smn_read(driver_handle, cfg->rsmu_arg, &version);
    return version;
}

/*
 * Package power via SVI2 telemetry:
 * P = V_core * I_core + V_soc * I_soc
 */
hwsense_voltage_result_t hwsense_amd_package_power(HANDLE driver_handle)
{
    hwsense_voltage_result_t r = {0};
    DWORD raw0 = 0, raw1 = 0;

    if (!smn_read(driver_handle, AMD_SVI2_PLANE0, &raw0)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SMN read of SVI2 Plane0 failed (error %lu)", GetLastError());
        return r;
    }

    if (!smn_read(driver_handle, AMD_SVI2_PLANE1, &raw1)) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "SMN read of SVI2 Plane1 failed (error %lu)", GetLastError());
        return r;
    }

    DWORD vid_core = (raw0 >> 16) & 0x1FF;
    DWORD cur_core = raw0 & 0xFF;
    DWORD vid_soc  = (raw1 >> 16) & 0x1FF;
    DWORD cur_soc  = raw1 & 0xFF;

    double v_core = (vid_core != 0x1FF) ? AMD_SVI2_VID_TO_V(vid_core) : 0.0;
    double v_soc  = (vid_soc  != 0x1FF) ? AMD_SVI2_VID_TO_V(vid_soc)  : 0.0;

    double power = (v_core * (double)cur_core) + (v_soc * (double)cur_soc);

    r.ok = 1;
    r.volts = power;
    r.amps = (double)cur_core + (double)cur_soc;
    return r;
}

/*
 * Diagnostic: read raw SMN register value.
 */
int hwsense_amd_read_smn(HANDLE driver_handle, DWORD smn_addr, DWORD *out_value)
{
    return smn_read(driver_handle, smn_addr, out_value) ? 1 : 0;
}

/*
 * Diagnostic: get SMU info (version, PM table version).
 * Returns 1 on success, fills output strings.
 */
int hwsense_amd_smu_diag(HANDLE driver_handle,
                         char *out_name, int name_len,
                         DWORD *out_smu_ver, DWORD *out_pm_ver)
{
    const smu_config_t *cfg = get_smu_config();
    if (!cfg) {
        if (out_name && name_len > 0)
            _snprintf_s(out_name, name_len, _TRUNCATE, "Unknown CPU");
        return 0;
    }

    if (out_name && name_len > 0)
        _snprintf_s(out_name, name_len, _TRUNCATE, "%s", cfg->name);

    if (out_smu_ver)
        *out_smu_ver = smu_get_version(driver_handle, cfg);

    if (out_pm_ver)
        *out_pm_ver = smu_get_pm_table_version(driver_handle, cfg);

    return 1;
}
