/*
 * gpu.c — GPU temperature reading via NVML (NVIDIA) and ADL (AMD).
 *
 * Dynamically loads NVML.dll or ADL SDK at runtime.
 * No compile-time dependency — uses LoadLibrary/GetProcAddress.
 */

#include "../core/hwsense_internal.h"
#include <stdio.h>

/* ── NVIDIA NVML ────────────────────────────────────────────────────── */

/* NVML function pointer types */
typedef int (*fn_nvmlInit)(void);
typedef int (*fn_nvmlShutdown)(void);
typedef int (*fn_nvmlDeviceGetHandleByIndex)(int index, void **device);
typedef int (*fn_nvmlDeviceGetTemperature)(void *device, int sensorType, unsigned int *temp);
typedef int (*fn_nvmlDeviceGetCount)(unsigned int *count);
typedef int (*fn_nvmlDeviceGetName)(void *device, char *name, unsigned int length);

#define NVML_TEMPERATURE_GPU 0

static HMODULE g_nvml_dll = NULL;
static fn_nvmlInit g_nvmlInit = NULL;
static fn_nvmlShutdown g_nvmlShutdown = NULL;
static fn_nvmlDeviceGetHandleByIndex g_nvmlDeviceGetHandleByIndex = NULL;
static fn_nvmlDeviceGetTemperature g_nvmlDeviceGetTemperature = NULL;
static fn_nvmlDeviceGetCount g_nvmlDeviceGetCount = NULL;
static fn_nvmlDeviceGetName g_nvmlDeviceGetName = NULL;

static int load_nvml(void)
{
    if (g_nvml_dll)
        return 1;

    g_nvml_dll = LoadLibraryA("nvml.dll");
    if (!g_nvml_dll)
        g_nvml_dll = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!g_nvml_dll)
        return 0;

    g_nvmlInit = (fn_nvmlInit)GetProcAddress(g_nvml_dll, "nvmlInit_v2");
    if (!g_nvmlInit) g_nvmlInit = (fn_nvmlInit)GetProcAddress(g_nvml_dll, "nvmlInit");
    g_nvmlShutdown = (fn_nvmlShutdown)GetProcAddress(g_nvml_dll, "nvmlShutdown");
    g_nvmlDeviceGetHandleByIndex = (fn_nvmlDeviceGetHandleByIndex)GetProcAddress(g_nvml_dll, "nvmlDeviceGetHandleByIndex_v2");
    if (!g_nvmlDeviceGetHandleByIndex) g_nvmlDeviceGetHandleByIndex = (fn_nvmlDeviceGetHandleByIndex)GetProcAddress(g_nvml_dll, "nvmlDeviceGetHandleByIndex");
    g_nvmlDeviceGetTemperature = (fn_nvmlDeviceGetTemperature)GetProcAddress(g_nvml_dll, "nvmlDeviceGetTemperature");
    g_nvmlDeviceGetCount = (fn_nvmlDeviceGetCount)GetProcAddress(g_nvml_dll, "nvmlDeviceGetCount");
    g_nvmlDeviceGetName = (fn_nvmlDeviceGetName)GetProcAddress(g_nvml_dll, "nvmlDeviceGetName_v2");
    if (!g_nvmlDeviceGetName) g_nvmlDeviceGetName = (fn_nvmlDeviceGetName)GetProcAddress(g_nvml_dll, "nvmlDeviceGetName");

    if (!g_nvmlInit || !g_nvmlShutdown || !g_nvmlDeviceGetHandleByIndex ||
        !g_nvmlDeviceGetTemperature || !g_nvmlDeviceGetCount) {
        FreeLibrary(g_nvml_dll);
        g_nvml_dll = NULL;
        return 0;
    }

    return 1;
}

/* ── AMD ADL ────────────────────────────────────────────────────────── */

/* ADL function pointer types */
typedef int (*fn_ADL_Main_Control_Create)(void *callback, int iEnumAdapters, void **context);
typedef int (*fn_ADL_Main_Control_Destroy)(void *context);
typedef int (*fn_ADL_Adapter_NumberOfAdapters_Get)(void *context, int *count);
typedef int (*fn_ADL_Overdrive5_Temperature_Get)(void *context, int adapter, int *temperature);

static HMODULE g_adl_dll = NULL;
static fn_ADL_Main_Control_Create g_adlCreate = NULL;
static fn_ADL_Main_Control_Destroy g_adlDestroy = NULL;
static fn_ADL_Adapter_NumberOfAdapters_Get g_adlAdapterCount = NULL;
static fn_ADL_Overdrive5_Temperature_Get g_adlTemp = NULL;
static void *g_adl_context = NULL;

static int load_adl(void)
{
    if (g_adl_dll)
        return 1;

    g_adl_dll = LoadLibraryA("atiadlxx.dll");
    if (!g_adl_dll)
        g_adl_dll = LoadLibraryA("atiadlxy.dll");
    if (!g_adl_dll)
        return 0;

    g_adlCreate = (fn_ADL_Main_Control_Create)GetProcAddress(g_adl_dll, "ADL_Main_Control_Create");
    g_adlDestroy = (fn_ADL_Main_Control_Destroy)GetProcAddress(g_adl_dll, "ADL_Main_Control_Destroy");
    g_adlAdapterCount = (fn_ADL_Adapter_NumberOfAdapters_Get)GetProcAddress(g_adl_dll, "ADL_Adapter_NumberOfAdapters_Get");
    g_adlTemp = (fn_ADL_Overdrive5_Temperature_Get)GetProcAddress(g_adl_dll, "ADL_Overdrive5_Temperature_Get");

    if (!g_adlCreate || !g_adlDestroy || !g_adlAdapterCount || !g_adlTemp) {
        FreeLibrary(g_adl_dll);
        g_adl_dll = NULL;
        return 0;
    }

    if (g_adlCreate(NULL, 0, &g_adl_context) != 0 || !g_adl_context) {
        FreeLibrary(g_adl_dll);
        g_adl_dll = NULL;
        return 0;
    }

    return 1;
}

/* ── Public API ─────────────────────────────────────────────────────── */

#define HWSENSE_MAX_GPU 4

/*
 * Read GPU temperature.
 * Tries NVIDIA NVML first, then AMD ADL.
 * gpu_index: which GPU to read (0 = first).
 */
hwsense_gpu_result_t hwsense_gpu_temperature(int gpu_index)
{
    hwsense_gpu_result_t result = {0};

    /* Try NVIDIA NVML */
    if (load_nvml()) {
        if (g_nvmlInit() == 0) {
            void *device = NULL;
            if (g_nvmlDeviceGetHandleByIndex(gpu_index, &device) == 0) {
                unsigned int temp = 0;
                if (g_nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temp) == 0) {
                    result.ok = 1;
                    result.temperature = (int)temp;
                    if (g_nvmlDeviceGetName)
                        g_nvmlDeviceGetName(device, result.name, sizeof(result.name));
                    else
                        _snprintf_s(result.name, sizeof(result.name), _TRUNCATE, "NVIDIA GPU");
                    g_nvmlShutdown();
                    return result;
                }
            }
            g_nvmlShutdown();
        }
    }

    /* Try AMD ADL */
    if (load_adl()) {
        int adapter_count = 0;
        if (g_adlAdapterCount(g_adl_context, &adapter_count) == 0 && adapter_count > 0) {
            int temp = 0;
            if (g_adlTemp(g_adl_context, gpu_index, &temp) == 0) {
                /* ADL returns temperature in tenths of degrees */
                result.ok = 1;
                result.temperature = temp / 10;
                _snprintf_s(result.name, sizeof(result.name), _TRUNCATE, "AMD GPU");
                return result;
            }
        }
    }

    result.ok = 0;
    _snprintf_s(result.error, sizeof(result.error), _TRUNCATE,
                "No GPU temperature available (NVML/ADL not loaded)");
    return result;
}
