/*
 * intel_linux.c — Intel CPU temperature via /dev/cpu/N/msr on Linux.
 *
 * Same registers and formula as the Windows path in intel.c:
 *   MSR 0x1A2 (IA32_TEMPERATURE_TARGET) -> TjMax from bits [23:16]
 *   MSR 0x19C (IA32_THERM_STATUS)       -> digital readout from bits [22:16],
 *                                          valid flag at bit 31
 *   temperature = TjMax - digital_readout
 *
 * No thread pinning needed: /dev/cpu/N/msr already selects the target
 * logical core by path, unlike the Windows DeviceIoControl path where
 * RDMSR runs on whichever core the calling thread happens to be on.
 */

#ifndef _WIN32

#include "../core/hwsense_internal.h"
#include "../core/sensor_math.h"
#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>

#define MSR_IA32_TEMPERATURE_TARGET  0x1A2
#define MSR_IA32_THERM_STATUS        0x19C

static int read_msr(int core, unsigned int msr_index, uint64_t *out_value)
{
    char path[64];
    int fd;
    ssize_t n;

    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", core);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return 0;

    n = pread(fd, out_value, sizeof(*out_value), msr_index);
    close(fd);

    return n == (ssize_t)sizeof(*out_value);
}

hwsense_temp_result_t hwsense_intel_core_temp_linux(void)
{
    hwsense_temp_result_t r = {0};
    uint64_t msr_val;
    uint32_t temp_target_eax;
    uint32_t therm_status_eax;

    if (!read_msr(0, MSR_IA32_TEMPERATURE_TARGET, &msr_val)) {
        snprintf(r.error, sizeof(r.error),
                 "RDMSR 0x%X failed on /dev/cpu/0/msr (need root/CAP_SYS_RAWIO "
                 "and the msr kernel module loaded)", MSR_IA32_TEMPERATURE_TARGET);
        return r;
    }
    temp_target_eax = (uint32_t)(msr_val & 0xFFFFFFFF);

    if (!read_msr(0, MSR_IA32_THERM_STATUS, &msr_val)) {
        snprintf(r.error, sizeof(r.error),
                 "RDMSR 0x%X failed on /dev/cpu/0/msr", MSR_IA32_THERM_STATUS);
        return r;
    }
    therm_status_eax = (uint32_t)(msr_val & 0xFFFFFFFF);

    if (!hwsense_intel_therm_valid(therm_status_eax)) {
        snprintf(r.error, sizeof(r.error), "Thermal reading not valid (bit 31 = 0)");
        return r;
    }

    r.ok = 1;
    r.celsius = hwsense_intel_temp_c(temp_target_eax, therm_status_eax);
    return r;
}

#endif /* !_WIN32 */
