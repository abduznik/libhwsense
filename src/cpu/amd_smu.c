/*
 * amd_smu.c — AMD SMU mailbox protocol and PM table reading.
 *
 * SMU (System Management Unit) communication via MP1 and RSMU mailboxes.
 * CPU codename detection, PM table transfer, DRAM readback.
 */

#include "../core/hwsense_internal.h"
#include "../core/ioctl_codes.h"
#include <stdio.h>
#include <string.h>
#include <intrin.h>

extern BOOL smn_read(HANDLE dev, DWORD smn_addr, DWORD *out_val);
extern BOOL smn_write_dword(HANDLE driver_handle, DWORD smn_addr, DWORD value);

/* ── CPU Codename Detection ────────────────────────────────────────── */

typedef enum {
    AMD_CODENAME_UNKNOWN = 0,
    AMD_CODENAME_MATISSE,
    AMD_CODENAME_RENOIR,
    AMD_CODENAME_VERMEER,
    AMD_CODENAME_CEZANNE,
    AMD_CODENAME_MILAN,
    AMD_CODENAME_CASTLEPEAK,
} amd_codename_t;

static amd_codename_t detect_codename(void)
{
    int cpuinfo[4] = {0};
    unsigned int ext_family, base_family, ext_model, base_model;
    unsigned int family, model;

    __cpuid(cpuinfo, 0x80000001);
    ext_family = (cpuinfo[0] >> 20) & 0xFF;
    base_family = (cpuinfo[0] >> 8) & 0xF;
    ext_model = (cpuinfo[0] >> 16) & 0xF;
    base_model = (cpuinfo[0] >> 4) & 0xF;

    family = base_family + ext_family;
    model = (ext_model << 4) | base_model;

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

/* ── SMU Config Table ──────────────────────────────────────────────── */

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
    { .mp1_msg = 0x3B10530, .mp1_rsp = 0x3B1057C, .mp1_arg = 0x3B109C4,
      .rsmu_msg = 0x3B10524, .rsmu_rsp = 0x3B10570, .rsmu_arg = 0x3B10A40,
      .cmd_get_version = 0x08, .cmd_get_dram_base = 0x06,
      .cmd_transfer_to_dram = 0x05, .cmd_transfer_arg0 = 0,
      .cpuid_family = 0x17, .cpuid_model = 0x71, .name = "Matisse" },
    { .mp1_msg = 0x3B10528, .mp1_rsp = 0x3B10564, .mp1_arg = 0x3B10998,
      .rsmu_msg = 0x3B10A20, .rsmu_rsp = 0x3B10A80, .rsmu_arg = 0x3B10A88,
      .cmd_get_version = 0x06, .cmd_get_dram_base = 0x66,
      .cmd_transfer_to_dram = 0x65, .cmd_transfer_arg0 = 3,
      .cpuid_family = 0x17, .cpuid_model = 0x60, .name = "Renoir" },
    { .mp1_msg = 0x3B10530, .mp1_rsp = 0x3B1057C, .mp1_arg = 0x3B109C4,
      .rsmu_msg = 0x3B10524, .rsmu_rsp = 0x3B10570, .rsmu_arg = 0x3B10A40,
      .cmd_get_version = 0x08, .cmd_get_dram_base = 0x06,
      .cmd_transfer_to_dram = 0x05, .cmd_transfer_arg0 = 0,
      .cpuid_family = 0x19, .cpuid_model = 0x21, .name = "Vermeer" },
    { .mp1_msg = 0x3B10528, .mp1_rsp = 0x3B10564, .mp1_arg = 0x3B10998,
      .rsmu_msg = 0x3B10A20, .rsmu_rsp = 0x3B10A80, .rsmu_arg = 0x3B10A88,
      .cmd_get_version = 0x06, .cmd_get_dram_base = 0x66,
      .cmd_transfer_to_dram = 0x65, .cmd_transfer_arg0 = 0,
      .cpuid_family = 0x19, .cpuid_model = 0x50, .name = "Cezanne" },
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

/* ── SMU Mailbox Protocol ──────────────────────────────────────────── */

#define SMU_TIMEOUT 10000

static DWORD smu_send_command(HANDLE driver_handle, DWORD msg_addr, DWORD rsp_addr,
                              DWORD arg_addr, DWORD cmd,
                              const DWORD *args, int arg_count)
{
    DWORD rsp;
    int i, timeout;

    for (timeout = 0; timeout < SMU_TIMEOUT; timeout++) {
        if (!smn_read(driver_handle, rsp_addr, &rsp))
            return 0;
        if (rsp != 0)
            break;
        SwitchToThread();
    }
    if (timeout >= SMU_TIMEOUT)
        return 0;

    smn_write_dword(driver_handle, rsp_addr, 0);

    for (i = 0; i < arg_count && i < 6; i++) {
        if (!smn_write_dword(driver_handle, arg_addr + (i * 4), args[i]))
            return 0;
    }

    if (!smn_write_dword(driver_handle, msg_addr, cmd))
        return 0;

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

static DWORD64 smu_get_dram_base(HANDLE driver_handle, const smu_config_t *cfg)
{
    DWORD args[1] = {0};
    DWORD rsp = smu_send_command(driver_handle, cfg->rsmu_msg, cfg->rsmu_rsp,
                                 cfg->rsmu_arg, cfg->cmd_get_dram_base, args, 0);
    if (rsp != 0x01)
        return 0;
    DWORD lo = 0, hi = 0;
    smn_read(driver_handle, cfg->rsmu_arg, &lo);
    smn_read(driver_handle, cfg->rsmu_arg + 4, &hi);
    return ((DWORD64)hi << 32) | lo;
}

static int smu_transfer_table_to_dram(HANDLE driver_handle, const smu_config_t *cfg)
{
    DWORD args[1] = {cfg->cmd_transfer_arg0};
    DWORD rsp = smu_send_command(driver_handle, cfg->rsmu_msg, cfg->rsmu_rsp,
                                 cfg->rsmu_arg, cfg->cmd_transfer_to_dram, args, 1);
    return (rsp == 0x01);
}

/* ── Physical Memory Read ──────────────────────────────────────────── */

static DWORD read_physical_memory(HANDLE driver_handle, DWORD64 phys_addr,
                                  void *buffer, DWORD size)
{
    BYTE inp[16];
    DWORD bytes_ret = 0;

    *(DWORD64*)&inp[0] = phys_addr;
    *(DWORD*)&inp[8] = 1;
    *(DWORD*)&inp[12] = size;

    BOOL ok = DeviceIoControl(
        driver_handle, IOCTL_OLS_READ_MEMORY,
        inp, 16,
        buffer, size,
        &bytes_ret, NULL
    );

    extern DWORD g_last_dram_error;
    g_last_dram_error = ok ? 0 : GetLastError();
    return bytes_ret;
}

DWORD g_last_dram_error = 0;

/* ── Public Diagnostic API ─────────────────────────────────────────── */

int hwsense_amd_smu_diag(HANDLE driver_handle,
                         char *out_name, int name_len,
                         DWORD *out_smu_ver, DWORD *out_pm_ver,
                         DWORD64 *out_dram_base)
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

    if (out_dram_base)
        *out_dram_base = smu_get_dram_base(driver_handle, cfg);

    return 1;
}

float hwsense_amd_pmtable_power_raw(HANDLE driver_handle)
{
    const smu_config_t *cfg = get_smu_config();
    if (!cfg)
        return -1.0f;

    DWORD64 dram_base = smu_get_dram_base(driver_handle, cfg);
    if (dram_base == 0)
        return -1.0f;

    if (!smu_transfer_table_to_dram(driver_handle, cfg))
        return -1.0f;

    Sleep(10);

    float power = -1.0f;

    DWORD test_buf[4] = {0xDEADBEEF, 0, 0, 0};
    read_physical_memory(driver_handle, 0x10000, test_buf, sizeof(test_buf));

    {
        float header[4] = {0};
        DWORD hdr_bytes = read_physical_memory(driver_handle, dram_base,
                                               header, sizeof(header));
        if (hdr_bytes >= 4 && header[0] > 0.0f && header[0] < 10000.0f) {
            DWORD pwr_bytes = read_physical_memory(driver_handle, dram_base + 0x10C,
                                                    &power, sizeof(power));
            if (pwr_bytes == sizeof(power) && power > 0.0f && power < 1000.0f)
                return power;
        }
    }

    DWORD bytes = read_physical_memory(driver_handle, dram_base + 0x10C,
                                       &power, sizeof(power));
    if (bytes == sizeof(power) && power > 0.0f && power < 1000.0f)
        return power;

    {
        OLS_READ_MEMORY_INPUT inp2;
        DWORD bytes_ret2 = 0;
        BYTE raw[4] = {0};
        inp2.Address = (long long)(dram_base + 0x10C);
        inp2.UnitSize = 1;
        inp2.Count = 4;
        BOOL ok2 = DeviceIoControl(
            driver_handle, IOCTL_OLS_READ_MEMORY,
            &inp2, sizeof(inp2),
            raw, sizeof(raw),
            &bytes_ret2, NULL
        );
        if (ok2 && bytes_ret2 == 4) {
            memcpy(&power, raw, 4);
            if (power > 0.0f && power < 1000.0f)
                return power;
        }
    }

    return -1.0f;
}
