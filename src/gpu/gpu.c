/*
 * gpu.c — GPU temperature, VRAM, and power reading via NVML (NVIDIA) and ADL (AMD).
 *
 * Dynamically loads NVML.dll or ADL SDK at runtime.
 * No compile-time dependency — uses LoadLibrary/GetProcAddress.
 */

#include "../core/hwsense_internal.h"
#include <stdio.h>

/* ── NVIDIA NVML ────────────────────────────────────────────────────── */

typedef int (*fn_nvmlInit)(void);
typedef int (*fn_nvmlShutdown)(void);
typedef int (*fn_nvmlDeviceGetHandleByIndex)(int index, void **device);
typedef int (*fn_nvmlDeviceGetTemperature)(void *device, int sensorType, unsigned int *temp);
typedef int (*fn_nvmlDeviceGetCount)(unsigned int *count);
typedef int (*fn_nvmlDeviceGetName)(void *device, char *name, unsigned int length);
typedef int (*fn_nvmlDeviceGetMemoryInfo)(void *device, void *info);
typedef int (*fn_nvmlDeviceGetPowerUsage)(void *device, unsigned int *power);
typedef int (*fn_nvmlDeviceGetPowerManagementLimit)(void *device, unsigned int *min, unsigned int *max);
typedef int (*fn_nvmlDeviceGetUtilizationRates)(void *device, void *util);
typedef int (*fn_nvmlDeviceGetClockInfo)(void *device, int clockType, unsigned int *clock);

#define NVML_TEMPERATURE_GPU 0
#define NVML_CLOCK_GRAPHICS  0
#define NVML_CLOCK_MEM       2

typedef struct { unsigned long long total, free, used; } nvmlMemory_t;
typedef struct { unsigned int gpu, memory; } nvmlUtilization_t;

static HMODULE g_nvml_dll = NULL;
static fn_nvmlInit g_nvmlInit = NULL;
static fn_nvmlShutdown g_nvmlShutdown = NULL;
static fn_nvmlDeviceGetHandleByIndex g_nvmlDeviceGetHandleByIndex = NULL;
static fn_nvmlDeviceGetTemperature g_nvmlDeviceGetTemperature = NULL;
static fn_nvmlDeviceGetName g_nvmlDeviceGetName = NULL;
static fn_nvmlDeviceGetMemoryInfo g_nvmlDeviceGetMemoryInfo = NULL;
static fn_nvmlDeviceGetPowerUsage g_nvmlDeviceGetPowerUsage = NULL;
static fn_nvmlDeviceGetPowerManagementLimit g_nvmlDeviceGetPowerManagementLimit = NULL;
static fn_nvmlDeviceGetUtilizationRates g_nvmlDeviceGetUtilizationRates = NULL;
static fn_nvmlDeviceGetClockInfo g_nvmlDeviceGetClockInfo = NULL;

static int load_nvml(void)
{
    if (g_nvml_dll) return 1;
    g_nvml_dll = LoadLibraryA("nvml.dll");
    if (!g_nvml_dll) g_nvml_dll = LoadLibraryA("C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvml.dll");
    if (!g_nvml_dll) return 0;
    g_nvmlInit = (fn_nvmlInit)GetProcAddress(g_nvml_dll, "nvmlInit_v2");
    if (!g_nvmlInit) g_nvmlInit = (fn_nvmlInit)GetProcAddress(g_nvml_dll, "nvmlInit");
    g_nvmlShutdown = (fn_nvmlShutdown)GetProcAddress(g_nvml_dll, "nvmlShutdown");
    g_nvmlDeviceGetHandleByIndex = (fn_nvmlDeviceGetHandleByIndex)GetProcAddress(g_nvml_dll, "nvmlDeviceGetHandleByIndex_v2");
    if (!g_nvmlDeviceGetHandleByIndex) g_nvmlDeviceGetHandleByIndex = (fn_nvmlDeviceGetHandleByIndex)GetProcAddress(g_nvml_dll, "nvmlDeviceGetHandleByIndex");
    g_nvmlDeviceGetTemperature = (fn_nvmlDeviceGetTemperature)GetProcAddress(g_nvml_dll, "nvmlDeviceGetTemperature");
    g_nvmlDeviceGetName = (fn_nvmlDeviceGetName)GetProcAddress(g_nvml_dll, "nvmlDeviceGetName_v2");
    if (!g_nvmlDeviceGetName) g_nvmlDeviceGetName = (fn_nvmlDeviceGetName)GetProcAddress(g_nvml_dll, "nvmlDeviceGetName");
    g_nvmlDeviceGetMemoryInfo = (fn_nvmlDeviceGetMemoryInfo)GetProcAddress(g_nvml_dll, "nvmlDeviceGetMemoryInfo");
    g_nvmlDeviceGetPowerUsage = (fn_nvmlDeviceGetPowerUsage)GetProcAddress(g_nvml_dll, "nvmlDeviceGetPowerUsage");
    g_nvmlDeviceGetPowerManagementLimit = (fn_nvmlDeviceGetPowerManagementLimit)GetProcAddress(g_nvml_dll, "nvmlDeviceGetPowerManagementLimit");
    g_nvmlDeviceGetUtilizationRates = (fn_nvmlDeviceGetUtilizationRates)GetProcAddress(g_nvml_dll, "nvmlDeviceGetUtilizationRates");
    g_nvmlDeviceGetClockInfo = (fn_nvmlDeviceGetClockInfo)GetProcAddress(g_nvml_dll, "nvmlDeviceGetClockInfo");
    if (!g_nvmlInit || !g_nvmlShutdown || !g_nvmlDeviceGetHandleByIndex || !g_nvmlDeviceGetTemperature) {
        FreeLibrary(g_nvml_dll); g_nvml_dll = NULL; return 0;
    }
    return 1;
}

/* ── AMD ADL ────────────────────────────────────────────────────────── */

typedef int (*fn_ADL_Main_Control_Create)(void *cb, int iEnum, void **ctx);
typedef int (*fn_ADL_Main_Control_Destroy)(void *ctx);
typedef int (*fn_ADL_Adapter_NumberOfAdapters_Get)(void *ctx, int *count);
typedef int (*fn_ADL_Overdrive5_Temperature_Get)(void *ctx, int adapter, int *temp);

static HMODULE g_adl_dll = NULL;
static fn_ADL_Main_Control_Create g_adlCreate = NULL;
static fn_ADL_Main_Control_Destroy g_adlDestroy = NULL;
static fn_ADL_Adapter_NumberOfAdapters_Get g_adlAdapterCount = NULL;
static fn_ADL_Overdrive5_Temperature_Get g_adlTemp = NULL;
static void *g_adl_ctx = NULL;

static int load_adl(void)
{
    if (g_adl_dll) return 1;
    g_adl_dll = LoadLibraryA("atiadlxx.dll");
    if (!g_adl_dll) g_adl_dll = LoadLibraryA("atiadlxy.dll");
    if (!g_adl_dll) return 0;
    g_adlCreate = (fn_ADL_Main_Control_Create)GetProcAddress(g_adl_dll, "ADL_Main_Control_Create");
    g_adlDestroy = (fn_ADL_Main_Control_Destroy)GetProcAddress(g_adl_dll, "ADL_Main_Control_Destroy");
    g_adlAdapterCount = (fn_ADL_Adapter_NumberOfAdapters_Get)GetProcAddress(g_adl_dll, "ADL_Adapter_NumberOfAdapters_Get");
    g_adlTemp = (fn_ADL_Overdrive5_Temperature_Get)GetProcAddress(g_adl_dll, "ADL_Overdrive5_Temperature_Get");
    if (!g_adlCreate || !g_adlDestroy || !g_adlAdapterCount || !g_adlTemp) {
        FreeLibrary(g_adl_dll); g_adl_dll = NULL; return 0;
    }
    if (g_adlCreate(NULL, 0, &g_adl_ctx) != 0 || !g_adl_ctx) {
        FreeLibrary(g_adl_dll); g_adl_dll = NULL; return 0;
    }
    return 1;
}

/* ── Public API ─────────────────────────────────────────────────────── */

hwsense_gpu_result_t hwsense_gpu_temperature(int gpu_index)
{
    hwsense_gpu_result_t result = {0};
    if (load_nvml()) {
        if (g_nvmlInit() == 0) {
            void *device = NULL;
            if (g_nvmlDeviceGetHandleByIndex(gpu_index, &device) == 0) {
                unsigned int temp = 0;
                if (g_nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temp) == 0) {
                    result.ok = 1; result.temperature = (int)temp;
                    if (g_nvmlDeviceGetName) g_nvmlDeviceGetName(device, result.name, sizeof(result.name));
                    else _snprintf_s(result.name, sizeof(result.name), _TRUNCATE, "NVIDIA GPU");
                    g_nvmlShutdown(); return result;
                }
            }
            g_nvmlShutdown();
        }
    }
    if (load_adl()) {
        int count = 0;
        if (g_adlAdapterCount(g_adl_ctx, &count) == 0 && count > 0) {
            int temp = 0;
            if (g_adlTemp(g_adl_ctx, gpu_index, &temp) == 0) {
                result.ok = 1; result.temperature = temp / 10;
                _snprintf_s(result.name, sizeof(result.name), _TRUNCATE, "AMD GPU");
                return result;
            }
        }
    }
    result.ok = 0;
    _snprintf_s(result.error, sizeof(result.error), _TRUNCATE, "No GPU temperature available");
    return result;
}

hwsense_gpu_info_t hwsense_gpu_info(int gpu_index)
{
    hwsense_gpu_info_t info = {0};
    if (load_nvml()) {
        if (g_nvmlInit() == 0) {
            void *device = NULL;
            if (g_nvmlDeviceGetHandleByIndex(gpu_index, &device) == 0) {
                if (g_nvmlDeviceGetName) g_nvmlDeviceGetName(device, info.name, sizeof(info.name));
                else _snprintf_s(info.name, sizeof(info.name), _TRUNCATE, "NVIDIA GPU");
                unsigned int temp = 0;
                if (g_nvmlDeviceGetTemperature(device, NVML_TEMPERATURE_GPU, &temp) == 0)
                    info.temperature = (int)temp;
                if (g_nvmlDeviceGetMemoryInfo) {
                    nvmlMemory_t mem = {0};
                    if (g_nvmlDeviceGetMemoryInfo(device, &mem) == 0) {
                        info.vram_total_mb = (int)(mem.total / (1024 * 1024));
                        info.vram_used_mb = (int)(mem.used / (1024 * 1024));
                    }
                }
                if (g_nvmlDeviceGetPowerUsage) {
                    unsigned int power = 0;
                    if (g_nvmlDeviceGetPowerUsage(device, &power) == 0)
                        info.power_usage_w = (int)(power / 1000);
                }
                if (g_nvmlDeviceGetPowerManagementLimit) {
                    unsigned int min_l = 0, max_l = 0;
                    if (g_nvmlDeviceGetPowerManagementLimit(device, &min_l, &max_l) == 0)
                        info.power_limit_w = (int)(max_l / 1000);
                }
                if (g_nvmlDeviceGetUtilizationRates) {
                    nvmlUtilization_t util = {0};
                    if (g_nvmlDeviceGetUtilizationRates(device, &util) == 0) {
                        info.gpu_load = util.gpu; info.mem_load = util.memory;
                    }
                }
                if (g_nvmlDeviceGetClockInfo) {
                    unsigned int gfx = 0, mem = 0;
                    if (g_nvmlDeviceGetClockInfo(device, NVML_CLOCK_GRAPHICS, &gfx) == 0)
                        info.clock_mhz = (int)gfx;
                    if (g_nvmlDeviceGetClockInfo(device, NVML_CLOCK_MEM, &mem) == 0)
                        info.mem_clock_mhz = (int)mem;
                }
                info.ok = 1;
            }
            g_nvmlShutdown();
        }
    }
    return info;
}
