/*
 * gpu_linux.c — GPU sensors on Linux.
 *
 * NVIDIA: NVML, loaded at runtime with dlopen so the library has no link-time
 * dependency on the NVIDIA driver. AMD: amdgpu hwmon (see sensors_linux.c).
 */

#ifndef _WIN32

#include "../core/hwsense_internal.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

extern int hwsense_amdgpu_info_linux(int gpu_index, hwsense_gpu_info_t *info);

typedef void *nvml_device_t;
typedef struct { unsigned long long total, free, used; } nvml_memory_t;
typedef struct { unsigned int gpu, memory; } nvml_util_t;

static struct {
    int tried;
    void *lib;
    int (*init)(void);
    int (*by_index)(unsigned int, nvml_device_t *);
    int (*get_name)(nvml_device_t, char *, unsigned int);
    int (*get_temp)(nvml_device_t, int, unsigned int *);
    int (*get_mem)(nvml_device_t, nvml_memory_t *);
    int (*get_power)(nvml_device_t, unsigned int *);
    int (*get_limit)(nvml_device_t, unsigned int *);
    int (*get_util)(nvml_device_t, nvml_util_t *);
    int (*get_clock)(nvml_device_t, int, unsigned int *);
} nv;

static int nvml_load(void)
{
    if (nv.tried)
        return nv.lib != NULL;
    nv.tried = 1;

    nv.lib = dlopen("libnvidia-ml.so.1", RTLD_LAZY);
    if (!nv.lib)
        return 0;

    nv.init     = (int (*)(void))dlsym(nv.lib, "nvmlInit_v2");
    nv.by_index = (int (*)(unsigned int, nvml_device_t *))dlsym(nv.lib, "nvmlDeviceGetHandleByIndex_v2");
    nv.get_name = (int (*)(nvml_device_t, char *, unsigned int))dlsym(nv.lib, "nvmlDeviceGetName");
    nv.get_temp = (int (*)(nvml_device_t, int, unsigned int *))dlsym(nv.lib, "nvmlDeviceGetTemperature");
    nv.get_mem  = (int (*)(nvml_device_t, nvml_memory_t *))dlsym(nv.lib, "nvmlDeviceGetMemoryInfo");
    nv.get_power = (int (*)(nvml_device_t, unsigned int *))dlsym(nv.lib, "nvmlDeviceGetPowerUsage");
    nv.get_limit = (int (*)(nvml_device_t, unsigned int *))dlsym(nv.lib, "nvmlDeviceGetPowerManagementLimit");
    nv.get_util = (int (*)(nvml_device_t, nvml_util_t *))dlsym(nv.lib, "nvmlDeviceGetUtilizationRates");
    nv.get_clock = (int (*)(nvml_device_t, int, unsigned int *))dlsym(nv.lib, "nvmlDeviceGetClockInfo");

    if (!nv.init || !nv.by_index || !nv.get_temp || nv.init() != 0) {
        dlclose(nv.lib);
        nv.lib = NULL;
        return 0;
    }
    return 1;
}

hwsense_gpu_info_t hwsense_gpu_info(int gpu_index)
{
    hwsense_gpu_info_t g;
    nvml_device_t dev;
    unsigned int u;
    nvml_memory_t mem;
    nvml_util_t util;

    memset(&g, 0, sizeof(g));

    if (gpu_index >= 0 && nvml_load() && nv.by_index((unsigned int)gpu_index, &dev) == 0) {
        if (nv.get_name)
            nv.get_name(dev, g.name, sizeof(g.name));
        if (nv.get_temp(dev, 0, &u) == 0)
            g.temperature = (int)u;
        if (nv.get_mem && nv.get_mem(dev, &mem) == 0) {
            g.vram_used_mb = (int)(mem.used >> 20);
            g.vram_total_mb = (int)(mem.total >> 20);
        }
        if (nv.get_power && nv.get_power(dev, &u) == 0)
            g.power_usage_w = (int)(u / 1000);
        if (nv.get_limit && nv.get_limit(dev, &u) == 0)
            g.power_limit_w = (int)(u / 1000);
        if (nv.get_util && nv.get_util(dev, &util) == 0) {
            g.gpu_load = (int)util.gpu;
            g.mem_load = (int)util.memory;
        }
        if (nv.get_clock) {
            if (nv.get_clock(dev, 0, &u) == 0)
                g.clock_mhz = (int)u;
            if (nv.get_clock(dev, 2, &u) == 0)
                g.mem_clock_mhz = (int)u;
        }
        g.ok = 1;
        return g;
    }

    if (gpu_index >= 0 && hwsense_amdgpu_info_linux(gpu_index, &g))
        return g;

    snprintf(g.error, sizeof(g.error),
             "No GPU found (NVML libnvidia-ml.so.1 not loadable and no amdgpu hwmon device)");
    return g;
}

hwsense_gpu_result_t hwsense_gpu_temperature(int gpu_index)
{
    hwsense_gpu_result_t r;
    hwsense_gpu_info_t g = hwsense_gpu_info(gpu_index);

    memset(&r, 0, sizeof(r));
    r.ok = g.ok;
    r.temperature = g.temperature;
    memcpy(r.name, g.name, sizeof(r.name));
    memcpy(r.error, g.error, sizeof(r.error));
    return r;
}

#endif /* !_WIN32 */
