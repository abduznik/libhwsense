/*
 * driver.c — WinRing0 kernel driver lifecycle management.
 *
 * Installs WinRing0x64.sys as a kernel service via the Service Control Manager,
 * opens a device handle for DeviceIoControl calls, and tears everything down
 * on shutdown.
 */

#include "../common/hwsense_internal.h"
#include "../common/ioctl_codes.h"
#include <stdio.h>
#include <stdlib.h>

/* ── Helpers ───────────────────────────────────────────────────────── */

/*
 * Locate WinRing0x64.sys next to the running .exe, then in CWD.
 * Returns TRUE and writes the full path into `out` on success.
 */
static BOOL find_driver_file(WCHAR *out, DWORD out_chars)
{
    /* 1) next to the executable */
    DWORD n = GetModuleFileNameW(NULL, out, out_chars);
    if (n > 0 && n < out_chars) {
        WCHAR *slash = wcsrchr(out, L'\\');
        if (slash) {
            slash[1] = L'\0';
            if (wcslen(out) + wcslen(WINRING0_SYS_FILENAME) < out_chars) {
                wcscat_s(out, out_chars, WINRING0_SYS_FILENAME);
                if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
                    return TRUE;
            }
        }
    }

    /* 2) current working directory */
    if (GetCurrentDirectoryW(out_chars, out) > 0) {
        size_t len = wcslen(out);
        if (len > 0 && out[len - 1] != L'\\') {
            out[len] = L'\\';
            out[len + 1] = L'\0';
        }
        if (wcslen(out) + wcslen(WINRING0_SYS_FILENAME) < out_chars) {
            wcscat_s(out, out_chars, WINRING0_SYS_FILENAME);
            if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES)
                return TRUE;
        }
    }

    return FALSE;
}

/*
 * Install and start the WinRing0 driver service.
 * Handles ERROR_SERVICE_EXISTS (open existing) and
 * ERROR_SERVICE_MARKED_FOR_DELETE (retry up to 3 times with 2s sleep).
 */
static SC_HANDLE install_and_start_driver(SC_HANDLE scm, const WCHAR *driver_path)
{
    SC_HANDLE svc = NULL;
    int retries = 3;
    int i;

    for (i = 0; i < retries; i++) {
        svc = CreateServiceW(
            scm,
            WINRING0_SERVICE_NAME,
            WINRING0_SERVICE_NAME,
            SERVICE_ALL_ACCESS,
            SERVICE_KERNEL_DRIVER,
            SERVICE_DEMAND_START,
            SERVICE_ERROR_NORMAL,
            driver_path,
            NULL, NULL, NULL, NULL, NULL
        );

        if (svc)
            break;

        DWORD err = GetLastError();

        if (err == ERROR_SERVICE_EXISTS) {
            svc = OpenServiceW(scm, WINRING0_SERVICE_NAME, SERVICE_ALL_ACCESS);
            if (!svc) {
                fprintf(stderr, "OpenService failed (error %lu)\n", GetLastError());
                return NULL;
            }
            break;
        }

        if (err == ERROR_SERVICE_MARKED_FOR_DELETE) {
            fprintf(stderr, "  [retry] Service marked for delete, forcing cleanup... (%d/%d)\n",
                    i + 1, retries);

            SC_HANDLE stale = OpenServiceW(scm, WINRING0_SERVICE_NAME, SERVICE_ALL_ACCESS);
            if (stale) {
                SERVICE_STATUS ss;
                ControlService(stale, SERVICE_CONTROL_STOP, &ss);
                CloseServiceHandle(stale);
            }

            Sleep(2000);
            continue;
        }

        fprintf(stderr, "CreateService failed (error %lu)\n", err);
        return NULL;
    }

    if (!svc) {
        fprintf(stderr, "Service stuck in MARKED_FOR_DELETE after %d retries.\n"
                        "Reboot to clear stale services.\n", retries);
        return NULL;
    }

    if (!StartServiceW(svc, 0, NULL)) {
        DWORD err = GetLastError();
        if (err != ERROR_SERVICE_ALREADY_RUNNING) {
            fprintf(stderr, "StartService failed (error %lu)\n", err);
            CloseServiceHandle(svc);
            return NULL;
        }
    }

    return svc;
}

static HANDLE open_driver_handle(void)
{
    HANDLE h = CreateFileW(
        WINRING0_DEVICE_PATH,
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "CreateFile(%ls) failed (error %lu)\n",
                WINRING0_DEVICE_PATH, GetLastError());
    }
    return h;
}

/* ── Public API ────────────────────────────────────────────────────────── */

hwsense_ctx_t *hwsense_init(void)
{
    WCHAR driver_path[MAX_PATH];

    if (!find_driver_file(driver_path, MAX_PATH)) {
        fprintf(stderr, "ERROR: " WINRING0_SYS_FILENAME_A " not found.\n"
                        "Place it next to the executable or in CWD.\n"
                        "Download: https://github.com/openhardwaremonitor/openhardwaremonitor\n"
                        "          (file: Hardware/WinRing0x64.sys)\n");
        return NULL;
    }

    fprintf(stderr, "Driver: %ls\n", driver_path);

    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!scm) {
        fprintf(stderr, "OpenSCManager failed (error %lu). Run as Administrator.\n",
                GetLastError());
        return NULL;
    }

    SC_HANDLE svc = install_and_start_driver(scm, driver_path);
    if (!svc) {
        CloseServiceHandle(scm);
        return NULL;
    }

    HANDLE dev = open_driver_handle();
    if (dev == INVALID_HANDLE_VALUE) {
        ControlService(svc, SERVICE_CONTROL_STOP, NULL);
        DeleteService(svc);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return NULL;
    }

    hwsense_ctx_t *ctx = (hwsense_ctx_t *)calloc(1, sizeof(*ctx));
    if (!ctx) {
        CloseHandle(dev);
        ControlService(svc, SERVICE_CONTROL_STOP, NULL);
        DeleteService(svc);
        CloseServiceHandle(svc);
        CloseServiceHandle(scm);
        return NULL;
    }

    ctx->driver_handle = dev;
    ctx->scm_handle    = scm;
    ctx->svc_handle    = svc;

    fprintf(stderr, "WinRing0 driver loaded.\n");

    return ctx;
}

void hwsense_shutdown(hwsense_ctx_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->driver_handle && ctx->driver_handle != INVALID_HANDLE_VALUE)
        CloseHandle(ctx->driver_handle);

    if (ctx->svc_handle) {
        ControlService(ctx->svc_handle, SERVICE_CONTROL_STOP, NULL);
        DeleteService(ctx->svc_handle);
        CloseServiceHandle(ctx->svc_handle);
    }

    if (ctx->scm_handle)
        CloseServiceHandle(ctx->scm_handle);

    free(ctx);
}

/*
 * Dispatch to Intel or AMD temperature reading based on CPU vendor.
 * Declared in amd/amd_zen.c and intel/intel.c.
 */
extern int   hwsense_detect_vendor(void);
extern hwsense_temp_result_t hwsense_amd_package_temp(HANDLE driver_handle);
extern hwsense_temp_result_t hwsense_intel_core_temp(HANDLE driver_handle);
extern hwsense_voltage_result_t hwsense_amd_core_voltage(HANDLE driver_handle);
extern hwsense_voltage_result_t hwsense_amd_soc_voltage(HANDLE driver_handle);
extern hwsense_voltage_result_t hwsense_amd_package_power(HANDLE driver_handle);
extern int hwsense_amd_read_smn(HANDLE driver_handle, DWORD smn_addr, DWORD *out_value);
extern int hwsense_amd_smu_diag(HANDLE driver_handle,
                               char *out_name, int name_len,
                               DWORD *out_smu_ver, DWORD *out_pm_ver);

hwsense_temp_result_t hwsense_cpu_package_temp(hwsense_ctx_t *ctx)
{
    hwsense_temp_result_t r = {0};

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE, "Invalid context or driver handle");
        return r;
    }

    int vendor = hwsense_detect_vendor();

    if (vendor == 'I')
        return hwsense_intel_core_temp(ctx->driver_handle);

    if (vendor == 'A')
        return hwsense_amd_package_temp(ctx->driver_handle);

    r.ok = 0;
    _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                "Unknown CPU vendor (registry VendorIdentifier not Intel/AMD)");
    return r;
}

hwsense_voltage_result_t hwsense_cpu_core_voltage(hwsense_ctx_t *ctx)
{
    hwsense_voltage_result_t r = {0};

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE, "Invalid context or driver handle");
        return r;
    }

    int vendor = hwsense_detect_vendor();

    if (vendor == 'A')
        return hwsense_amd_core_voltage(ctx->driver_handle);

    if (vendor == 'I') {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                    "Intel core voltage not yet implemented (MSR 0x198)");
        return r;
    }

    r.ok = 0;
    _snprintf_s(r.error, sizeof(r.error), _TRUNCATE, "Unknown CPU vendor");
    return r;
}

hwsense_voltage_result_t hwsense_amd_soc_voltage_dispatch(hwsense_ctx_t *ctx)
{
    hwsense_voltage_result_t r = {0};

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE, "Invalid context or driver handle");
        return r;
    }

    int vendor = hwsense_detect_vendor();

    if (vendor == 'A')
        return hwsense_amd_soc_voltage(ctx->driver_handle);

    r.ok = 0;
    _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                "SoC voltage is AMD-only (SVI2 Plane1)");
    return r;
}

hwsense_voltage_result_t hwsense_cpu_package_power(hwsense_ctx_t *ctx)
{
    hwsense_voltage_result_t r = {0};

    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE) {
        r.ok = 0;
        _snprintf_s(r.error, sizeof(r.error), _TRUNCATE, "Invalid context or driver handle");
        return r;
    }

    int vendor = hwsense_detect_vendor();

    if (vendor == 'A')
        return hwsense_amd_package_power(ctx->driver_handle);

    r.ok = 0;
    _snprintf_s(r.error, sizeof(r.error), _TRUNCATE,
                "Package power via SVI2 is AMD-only");
    return r;
}

int hwsense_read_smn_diag(hwsense_ctx_t *ctx, unsigned int smn_addr, unsigned int *out_value)
{
    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE)
        return 0;
    return hwsense_amd_read_smn(ctx->driver_handle, (DWORD)smn_addr, (DWORD *)out_value);
}

int hwsense_smu_diag(hwsense_ctx_t *ctx,
                     char *out_name, int name_len,
                     unsigned int *out_smu_ver, unsigned int *out_pm_ver)
{
    if (!ctx || !ctx->driver_handle || ctx->driver_handle == INVALID_HANDLE_VALUE)
        return 0;
    return hwsense_amd_smu_diag(ctx->driver_handle, out_name, name_len,
                                (DWORD *)out_smu_ver, (DWORD *)out_pm_ver);
}
