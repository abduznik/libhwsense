/*
 * intel_sensors_linux.c - Intel CPU sensors on Linux via /dev/cpu/*/msr + sysfs.
 *
 * Reads:
 *   MSR 0x198 (IA32_PERF_STATUS) - CPU Core Voltage (EDX[15:0] = VID)
 *   /sys/class/powercap/intel-rapl:0/energy_uj - Package Power (RAPL)
 *
 * Note: Direct MSR reads for RAPL (0x610) are blocked when the intel_rapl_msr
 * kernel module is loaded. Use the sysfs energy_uj counter instead.
 *
 * Compile: gcc -O2 -o intel_sensors intel_sensors_linux.c
 * Run:     sudo ./intel_sensors
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>

#define MSR_IA32_PERF_STATUS 0x198

static int read_msr(int core, uint32_t msr, uint64_t *value)
{
    char path[64];
    int fd;
    ssize_t n;
    snprintf(path, sizeof(path), "/dev/cpu/%d/msr", core);
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    n = pread(fd, value, sizeof(*value), msr);
    close(fd);
    return (n == sizeof(*value)) ? 0 : -1;
}

static double get_core_voltage(int core)
{
    uint64_t msr_val;
    if (read_msr(core, MSR_IA32_PERF_STATUS, &msr_val) < 0)
        return -1.0;
    uint32_t vid = (msr_val >> 32) & 0xFFFF;
    return (double)vid / 8192.0;
}

static int64_t read_energy_uj(void)
{
    char buf[32];
    int fd = open("/sys/class/powercap/intel-rapl:0/energy_uj", O_RDONLY);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return (int64_t)strtoll(buf, NULL, 10);
}

int main(void)
{
    int num_cores, i;

    printf("Intel CPU Sensors (Linux MSR + sysfs)\n");
    printf("=====================================\n\n");

    num_cores = sysconf(_SC_NPROCESSORS_ONLN);
    printf("Logical cores: %d\n\n", num_cores);

    /* CPU Core Voltage via MSR 0x198 */
    printf("--- CPU Core Voltage (MSR 0x198) ---\n");
    printf("  All cores:\n");
    for (i = 0; i < num_cores; i++) {
        double v = get_core_voltage(i);
        if (v > 0)
            printf("    Core %2d: %.4f V\n", i, v);
        else
            printf("    Core %2d: ERROR\n", i);
    }

    /* CPU Package Power via sysfs energy_uj */
    printf("\n--- CPU Package Power (RAPL sysfs) ---\n");
    int64_t e1 = read_energy_uj();
    if (e1 < 0) {
        fprintf(stderr, "  ERROR: Cannot read energy_uj\n");
        fprintf(stderr, "  Make sure intel_rapl module is loaded.\n");
    } else {
        struct timespec t1, t2;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        usleep(1000000);
        int64_t e2 = read_energy_uj();
        clock_gettime(CLOCK_MONOTONIC, &t2);

        if (e2 < 0) {
            fprintf(stderr, "  ERROR: Second energy read failed\n");
        } else {
            double dt = (t2.tv_sec - t1.tv_sec) + (t2.tv_nsec - t1.tv_nsec) / 1e9;
            double de_uj = (double)(e2 - e1);
            double power_w = de_uj / (dt * 1e6);
            printf("  Energy start:  %ld uJ\n", (long)e1);
            printf("  Energy end:    %ld uJ\n", (long)e2);
            printf("  Delta:         %.0f uJ over %.3f s\n", de_uj, dt);
            printf("  Package power: %.2f W\n", power_w);
        }
    }

    printf("\nDone.\n");
    return 0;
}
