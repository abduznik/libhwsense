/*
 * wmi.c — WMI sensor access using pure C COM interfaces.
 *
 * Queries WMI namespaces for hardware sensor data:
 *   - ROOT\WMI: MSAcpi_ThermalZoneTemperature
 *   - ROOT\CIMV2: Win32_Processor, Win32_Fan, Win32_VoltageProbe
 *
 * Uses raw COM interfaces (no C++ wrappers).
 */

#define COBJMACROS
#include <windows.h>
#include <stdio.h>
#include <oleauto.h>
#include <wbemidl.h>

/* WMI initialization state */
static BOOL g_wmi_initialized = FALSE;
static IWbemLocator *g_pLoc = NULL;
static IWbemServices *g_pSvc = NULL;

/*
 * Initialize WMI connection.
 */
int wmi_init(void)
{
    HRESULT hres;

    if (g_wmi_initialized)
        return 1;

    /* Initialize COM */
    hres = CoInitializeEx(0, COINIT_MULTITHREADED);
    if (FAILED(hres) && hres != RPC_E_CHANGED_MODE)
        return 0;

    /* Set COM security */
    hres = CoInitializeSecurity(
        NULL, -1, NULL, NULL,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE, NULL);
    if (FAILED(hres) && hres != RPC_E_TOO_LATE) {
        CoUninitialize();
        return 0;
    }

    /* Create IWbemLocator */
    hres = CoCreateInstance(
        &CLSID_WbemLocator, 0, CLSCTX_INPROC_SERVER,
        &IID_IWbemLocator, (LPVOID *)&g_pLoc);
    if (FAILED(hres)) {
        CoUninitialize();
        return 0;
    }

    /* Connect to ROOT\WMI namespace */
    BSTR namespace_str = SysAllocString(L"ROOT\\WMI");
    hres = IWbemLocator_ConnectServer(
        g_pLoc, namespace_str, NULL, NULL, NULL, 0, 0, NULL, &g_pSvc);
    SysFreeString(namespace_str);

    if (FAILED(hres)) {
        IWbemLocator_Release(g_pLoc);
        g_pLoc = NULL;
        CoUninitialize();
        return 0;
    }

    /* Set proxy security */
    hres = CoSetProxyBlanket(
        (IUnknown *)g_pSvc,
        RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, NULL,
        RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
        NULL, EOAC_NONE);
    if (FAILED(hres)) {
        IWbemServices_Release(g_pSvc);
        g_pSvc = NULL;
        IWbemLocator_Release(g_pLoc);
        g_pLoc = NULL;
        CoUninitialize();
        return 0;
    }

    g_wmi_initialized = TRUE;
    return 1;
}

/*
 * Shutdown WMI connection.
 */
void wmi_shutdown(void)
{
    if (g_pSvc) {
        IWbemServices_Release(g_pSvc);
        g_pSvc = NULL;
    }
    if (g_pLoc) {
        IWbemLocator_Release(g_pLoc);
        g_pLoc = NULL;
    }
    g_wmi_initialized = FALSE;
    CoUninitialize();
}

/*
 * Helper: Get BSTR property from WMI object.
 */
static BOOL wmi_get_string_property(IWbemClassObject *obj, LPCWSTR name, char *out, int out_size)
{
    VARIANT vt;
    VariantInit(&vt);

    HRESULT hres = IWbemClassObject_Get(obj, (BSTR)name, 0, &vt, NULL, NULL);
    if (FAILED(hres))
        return FALSE;

    if (vt.vt == VT_BSTR && vt.bstrVal) {
        WideCharToMultiByte(CP_ACP, 0, vt.bstrVal, -1, out, out_size, NULL, NULL);
        VariantClear(&vt);
        return TRUE;
    }

    VariantClear(&vt);
    return FALSE;
}

/*
 * Helper: Get INT32 property from WMI object.
 */
static BOOL wmi_get_int_property(IWbemClassObject *obj, LPCWSTR name, int *out)
{
    VARIANT vt;
    VariantInit(&vt);

    HRESULT hres = IWbemClassObject_Get(obj, (BSTR)name, 0, &vt, NULL, NULL);
    if (FAILED(hres))
        return FALSE;

    if (vt.vt == VT_I4) {
        *out = vt.iVal;
        VariantClear(&vt);
        return TRUE;
    }

    VariantClear(&vt);
    return FALSE;
}

/*
 * Read thermal zone temperatures via WMI.
 */
int wmi_read_thermal_zones(double *temps, int max_temps, char names[][64], int max_names)
{
    IEnumWbemClassObject *pEnumerator = NULL;
    HRESULT hres;
    int count = 0;

    if (!g_wmi_initialized)
        return 0;

    BSTR wql = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT * FROM MSAcpi_ThermalZoneTemperature");

    hres = IWbemServices_ExecQuery(
        g_pSvc, wql, query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        NULL, &pEnumerator);

    SysFreeString(wql);
    SysFreeString(query);

    if (FAILED(hres))
        return 0;

    IWbemClassObject *pclsObj = NULL;
    ULONG uReturn = 0;

    while (count < max_temps) {
        hres = IEnumWbemClassObject_Next(pEnumerator, WBEM_INFINITE, 1, &pclsObj, &uReturn);
        if (FAILED(hres) || uReturn == 0)
            break;

        /* Get instance name */
        if (names && count < max_names) {
            VARIANT vtName;
            VariantInit(&vtName);
            if (SUCCEEDED(IWbemClassObject_Get(pclsObj, L"__INSTANCE", 0, &vtName, 0, 0))) {
                if (vtName.vt == VT_BSTR)
                    WideCharToMultiByte(CP_ACP, 0, vtName.bstrVal, -1,
                                        names[count], 64, NULL, NULL);
            }
            VariantClear(&vtName);
        }

        /* Get temperature (in tenths of Kelvin) */
        VARIANT vtTemp;
        VariantInit(&vtTemp);
        hres = IWbemClassObject_Get(pclsObj, L"CurrentTemperature", 0, &vtTemp, 0, 0);
        if (SUCCEEDED(hres) && vtTemp.vt == VT_I4) {
            temps[count] = (vtTemp.iVal - 2732) / 10.0;
            count++;
        }
        VariantClear(&vtTemp);

        IWbemClassObject_Release(pclsObj);
    }

    IEnumWbemClassObject_Release(pEnumerator);
    return count;
}

/*
 * Read CPU information via WMI.
 */
typedef struct {
    char name[128];
    int max_clock_mhz;
    int current_clock_mhz;
    int voltage_mv;
    int load_percent;
    int temperature;
} wmi_cpu_info_t;

int wmi_read_cpu_info(wmi_cpu_info_t *info)
{
    IEnumWbemClassObject *pEnumerator = NULL;
    HRESULT hres;
    int count = 0;

    if (!g_wmi_initialized || !info)
        return 0;

    BSTR wql = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT * FROM Win32_Processor");

    hres = IWbemServices_ExecQuery(
        g_pSvc, wql, query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        NULL, &pEnumerator);

    SysFreeString(wql);
    SysFreeString(query);

    if (FAILED(hres))
        return 0;

    IWbemClassObject *pclsObj = NULL;
    ULONG uReturn = 0;

    hres = IEnumWbemClassObject_Next(pEnumerator, WBEM_INFINITE, 1, &pclsObj, &uReturn);
    if (SUCCEEDED(hres) && uReturn > 0) {
        wmi_get_string_property(pclsObj, L"Name", info->name, sizeof(info->name));
        wmi_get_int_property(pclsObj, L"MaxClockSpeed", &info->max_clock_mhz);
        wmi_get_int_property(pclsObj, L"CurrentClockSpeed", &info->current_clock_mhz);
        wmi_get_int_property(pclsObj, L"CurrentVoltage", &info->voltage_mv);
        info->voltage_mv *= 100;  /* Convert to millivolts */
        wmi_get_int_property(pclsObj, L"LoadPercentage", &info->load_percent);
        count = 1;
        IWbemClassObject_Release(pclsObj);
    }

    IEnumWbemClassObject_Release(pEnumerator);
    return count;
}

/*
 * Read fan speeds via WMI.
 */
typedef struct {
    char name[64];
    int active_cooling;
    int desired_speed;
} wmi_fan_info_t;

int wmi_read_fans(wmi_fan_info_t *fans, int max_fans)
{
    IEnumWbemClassObject *pEnumerator = NULL;
    HRESULT hres;
    int count = 0;

    if (!g_wmi_initialized || !fans)
        return 0;

    BSTR wql = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT * FROM Win32_Fan");

    hres = IWbemServices_ExecQuery(
        g_pSvc, wql, query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        NULL, &pEnumerator);

    SysFreeString(wql);
    SysFreeString(query);

    if (FAILED(hres))
        return 0;

    IWbemClassObject *pclsObj = NULL;
    ULONG uReturn = 0;

    while (count < max_fans) {
        hres = IEnumWbemClassObject_Next(pEnumerator, WBEM_INFINITE, 1, &pclsObj, &uReturn);
        if (FAILED(hres) || uReturn == 0)
            break;

        wmi_get_string_property(pclsObj, L"Name", fans[count].name, sizeof(fans[count].name));

        VARIANT vt;
        VariantInit(&vt);
        if (SUCCEEDED(IWbemClassObject_Get(pclsObj, L"ActiveCooling", 0, &vt, 0, 0))) {
            fans[count].active_cooling = (vt.vt == VT_BOOL && vt.boolVal) ? 1 : 0;
            VariantClear(&vt);
        }
        VariantInit(&vt);
        if (SUCCEEDED(IWbemClassObject_Get(pclsObj, L"DesiredSpeed", 0, &vt, 0, 0))) {
            fans[count].desired_speed = vt.iVal;
            VariantClear(&vt);
        }

        count++;
        IWbemClassObject_Release(pclsObj);
    }

    IEnumWbemClassObject_Release(pEnumerator);
    return count;
}

/*
 * Read voltage information via WMI.
 */
typedef struct {
    char name[64];
    double voltage;
    double max_voltage;
    double min_voltage;
} wmi_voltage_info_t;

int wmi_read_voltages(wmi_voltage_info_t *voltages, int max_voltages)
{
    IEnumWbemClassObject *pEnumerator = NULL;
    HRESULT hres;
    int count = 0;

    if (!g_wmi_initialized || !voltages)
        return 0;

    BSTR wql = SysAllocString(L"WQL");
    BSTR query = SysAllocString(L"SELECT * FROM Win32_VoltageProbe");

    hres = IWbemServices_ExecQuery(
        g_pSvc, wql, query,
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        NULL, &pEnumerator);

    SysFreeString(wql);
    SysFreeString(query);

    if (FAILED(hres))
        return 0;

    IWbemClassObject *pclsObj = NULL;
    ULONG uReturn = 0;

    while (count < max_voltages) {
        hres = IEnumWbemClassObject_Next(pEnumerator, WBEM_INFINITE, 1, &pclsObj, &uReturn);
        if (FAILED(hres) || uReturn == 0)
            break;

        wmi_get_string_property(pclsObj, L"Name", voltages[count].name, sizeof(voltages[count].name));

        int val;
        if (wmi_get_int_property(pclsObj, L"CurrentReading", &val))
            voltages[count].voltage = val / 10.0;
        if (wmi_get_int_property(pclsObj, L"MaxReading", &val))
            voltages[count].max_voltage = val / 10.0;
        if (wmi_get_int_property(pclsObj, L"MinReading", &val))
            voltages[count].min_voltage = val / 10.0;

        count++;
        IWbemClassObject_Release(pclsObj);
    }

    IEnumWbemClassObject_Release(pEnumerator);
    return count;
}
