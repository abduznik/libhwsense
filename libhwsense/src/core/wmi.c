/*
 * wmi.c — WMI sensor access using pure C COM interfaces.
 *
 * Queries multiple WMI namespaces for hardware sensor data:
 *   - ROOT\WMI: MSAcpi_ThermalZoneTemperature
 *   - ROOT\CIMV2: Win32_Processor, Win32_TemperatureProbe, Win32_Fan
 *
 * Uses raw COM interfaces (no C++ wrappers).
 */

#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <oleauto.h>
#include <wbemidl.h>
#include "wmi.h"

/* WMI namespaces */
#define WMI_NAMESPACE_WMI     L"ROOT\\WMI"
#define WMI_NAMESPACE_CIMV2   L"ROOT\\CIMV2"

/* WMI state */
static BOOL g_wmi_initialized = FALSE;
static IWbemLocator *g_pLoc = NULL;
static IWbemServices *g_pSvcWmi = NULL;
static IWbemServices *g_pSvcCim = NULL;

static IWbemServices *wmi_connect(IWbemLocator *loc, const wchar_t *ns)
{
    IWbemServices *svc = NULL;
    BSTR bstr_ns = SysAllocString(ns);
    HRESULT hres = IWbemLocator_ConnectServer(loc, bstr_ns, NULL, NULL, NULL, 0, 0, NULL, &svc);
    SysFreeString(bstr_ns);
    if (FAILED(hres)) return NULL;
    CoSetProxyBlanket((IUnknown *)svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
                      RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE);
    return svc;
}

int wmi_init(void)
{
    if (g_wmi_initialized) return 1;
    HRESULT hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres) && hres != RPC_E_CHANGED_MODE) return 0;
    hres = CoInitializeSecurity(NULL, -1, NULL, NULL, RPC_C_AUTHN_LEVEL_DEFAULT,
                                RPC_C_IMP_LEVEL_IMPERSONATE, NULL, EOAC_NONE, NULL);
    if (FAILED(hres) && hres != RPC_E_TOO_LATE) { CoUninitialize(); return 0; }
    hres = CoCreateInstance(&CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER, &IID_IWbemLocator, (LPVOID *)&g_pLoc);
    if (FAILED(hres)) { CoUninitialize(); return 0; }
    g_pSvcWmi = wmi_connect(g_pLoc, WMI_NAMESPACE_WMI);
    g_pSvcCim = wmi_connect(g_pLoc, WMI_NAMESPACE_CIMV2);
    g_wmi_initialized = TRUE;
    return 1;
}

void wmi_shutdown(void)
{
    if (g_pSvcWmi) { IWbemServices_Release(g_pSvcWmi); g_pSvcWmi = NULL; }
    if (g_pSvcCim) { IWbemServices_Release(g_pSvcCim); g_pSvcCim = NULL; }
    if (g_pLoc) { IWbemLocator_Release(g_pLoc); g_pLoc = NULL; }
    g_wmi_initialized = FALSE;
    CoUninitialize();
}

static BOOL wmi_get_str(IWbemClassObject *obj, LPCWSTR name, char *out, int size)
{
    VARIANT vt; VariantInit(&vt);
    if (FAILED(IWbemClassObject_Get(obj, (BSTR)name, 0, &vt, NULL, NULL))) return FALSE;
    if (vt.vt == VT_BSTR && vt.bstrVal) {
        WideCharToMultiByte(CP_ACP, 0, vt.bstrVal, -1, out, size, NULL, NULL);
        VariantClear(&vt); return TRUE;
    }
    VariantClear(&vt); return FALSE;
}

static BOOL wmi_get_int(IWbemClassObject *obj, LPCWSTR name, int *out)
{
    VARIANT vt; VariantInit(&vt);
    if (FAILED(IWbemClassObject_Get(obj, (BSTR)name, 0, &vt, NULL, NULL))) return FALSE;
    if (vt.vt == VT_I4) { *out = vt.iVal; VariantClear(&vt); return TRUE; }
    VariantClear(&vt); return FALSE;
}

static IEnumWbemClassObject *wmi_query(IWbemServices *svc, const wchar_t *q)
{
    IEnumWbemClassObject *en = NULL;
    BSTR wql = SysAllocString(L"WQL"); BSTR query = SysAllocString(q);
    HRESULT hres = IWbemServices_ExecQuery(svc, wql, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, NULL, &en);
    SysFreeString(wql); SysFreeString(query);
    return SUCCEEDED(hres) ? en : NULL;
}

int wmi_read_thermal_zones(double *temps, int max_temps, char names[][64], int max_names)
{
    int count = 0;
    if (!g_wmi_initialized) return 0;

    /* Try ROOT\WMI first */
    if (g_pSvcWmi) {
        IEnumWbemClassObject *en = wmi_query(g_pSvcWmi, L"SELECT * FROM MSAcpi_ThermalZoneTemperature");
        if (en) {
            IWbemClassObject *obj; ULONG ret;
            while (count < max_temps && IEnumWbemClassObject_Next(en, WBEM_INFINITE, 1, &obj, &ret) == S_OK && ret > 0) {
                if (names && count < max_names) {
                    VARIANT vn; VariantInit(&vn);
                    if (SUCCEEDED(IWbemClassObject_Get(obj, L"__INSTANCE", 0, &vn, 0, 0)) && vn.vt == VT_BSTR)
                        WideCharToMultiByte(CP_ACP, 0, vn.bstrVal, -1, names[count], 64, NULL, NULL);
                    VariantClear(&vn);
                }
                VARIANT vt; VariantInit(&vt);
                if (SUCCEEDED(IWbemClassObject_Get(obj, L"CurrentTemperature", 0, &vt, 0, 0)) && vt.vt == VT_I4)
                { temps[count] = (vt.iVal - 2732) / 10.0; count++; }
                VariantClear(&vt); IWbemClassObject_Release(obj);
            }
            IEnumWbemClassObject_Release(en);
        }
    }

    /* Try ROOT\CIMV2 if no results */
    if (count == 0 && g_pSvcCim) {
        IEnumWbemClassObject *en = wmi_query(g_pSvcCim, L"SELECT * FROM Win32_TemperatureProbe");
        if (en) {
            IWbemClassObject *obj; ULONG ret;
            while (count < max_temps && IEnumWbemClassObject_Next(en, WBEM_INFINITE, 1, &obj, &ret) == S_OK && ret > 0) {
                wmi_get_str(obj, L"Name", names[count], 64);
                int r = 0;
                if (wmi_get_int(obj, L"CurrentReading", &r)) { temps[count] = r / 10.0; count++; }
                IWbemClassObject_Release(obj);
            }
            IEnumWbemClassObject_Release(en);
        }
    }
    return count;
}

int wmi_read_cpu_info(wmi_cpu_info_t *info)
{
    if (!g_wmi_initialized || !info || !g_pSvcCim) return 0;
    IEnumWbemClassObject *en = wmi_query(g_pSvcCim, L"SELECT * FROM Win32_Processor");
    if (!en) return 0;
    IWbemClassObject *obj; ULONG ret; int count = 0;
    if (IEnumWbemClassObject_Next(en, WBEM_INFINITE, 1, &obj, &ret) == S_OK && ret > 0) {
        wmi_get_str(obj, L"Name", info->name, sizeof(info->name));
        wmi_get_int(obj, L"MaxClockSpeed", &info->max_clock_mhz);
        wmi_get_int(obj, L"CurrentClockSpeed", &info->current_clock_mhz);
        wmi_get_int(obj, L"CurrentVoltage", &info->voltage_mv);
        info->voltage_mv *= 100;
        wmi_get_int(obj, L"LoadPercentage", &info->load_percent);
        count = 1; IWbemClassObject_Release(obj);
    }
    IEnumWbemClassObject_Release(en);
    return count;
}

int wmi_read_fans(wmi_fan_info_t *fans, int max_fans)
{
    if (!g_wmi_initialized || !fans || !g_pSvcCim) return 0;
    IEnumWbemClassObject *en = wmi_query(g_pSvcCim, L"SELECT * FROM Win32_Fan");
    if (!en) return 0;
    IWbemClassObject *obj; ULONG ret; int count = 0;
    while (count < max_fans && IEnumWbemClassObject_Next(en, WBEM_INFINITE, 1, &obj, &ret) == S_OK && ret > 0) {
        wmi_get_str(obj, L"Name", fans[count].name, sizeof(fans[count].name));
        VARIANT vt; VariantInit(&vt);
        if (SUCCEEDED(IWbemClassObject_Get(obj, L"ActiveCooling", 0, &vt, 0, 0)))
        { fans[count].active_cooling = (vt.vt == VT_BOOL && vt.boolVal) ? 1 : 0; VariantClear(&vt); }
        VariantInit(&vt);
        if (SUCCEEDED(IWbemClassObject_Get(obj, L"DesiredSpeed", 0, &vt, 0, 0)))
        { fans[count].desired_speed = vt.iVal; VariantClear(&vt); }
        count++; IWbemClassObject_Release(obj);
    }
    IEnumWbemClassObject_Release(en);
    return count;
}

int wmi_read_voltages(wmi_voltage_info_t *voltages, int max_voltages)
{
    if (!g_wmi_initialized || !voltages || !g_pSvcCim) return 0;
    IEnumWbemClassObject *en = wmi_query(g_pSvcCim, L"SELECT * FROM Win32_VoltageProbe");
    if (!en) return 0;
    IWbemClassObject *obj; ULONG ret; int count = 0;
    while (count < max_voltages && IEnumWbemClassObject_Next(en, WBEM_INFINITE, 1, &obj, &ret) == S_OK && ret > 0) {
        wmi_get_str(obj, L"Name", voltages[count].name, sizeof(voltages[count].name));
        int v;
        if (wmi_get_int(obj, L"CurrentReading", &v)) voltages[count].voltage = v / 10.0;
        if (wmi_get_int(obj, L"MaxReading", &v)) voltages[count].max_voltage = v / 10.0;
        if (wmi_get_int(obj, L"MinReading", &v)) voltages[count].min_voltage = v / 10.0;
        count++; IWbemClassObject_Release(obj);
    }
    IEnumWbemClassObject_Release(en);
    return count;
}
