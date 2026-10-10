/*
 * test_nvme.c — NVMe SMART log decode against a hand-built 512-byte log laid
 * out per the NVMe Base Specification, "SMART / Health Information" (LID 02h).
 */

#include "../src/storage/nvme_linux.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void check_u64(const char *what, unsigned long long got, unsigned long long want)
{
    checks++;
    if (got != want) {
        printf("FAIL %s: got %llu, want %llu\n", what, got, want);
        failures++;
    }
}

static void check_dbl(const char *what, double got, double want)
{
    checks++;
    if (fabs(got - want) > 1e-6) {
        printf("FAIL %s: got %.4f, want %.4f\n", what, got, want);
        failures++;
    }
}

static void put(unsigned char *log, int off, unsigned long long v, int bytes)
{
    int i;
    for (i = 0; i < bytes; i++)
        log[off + i] = (unsigned char)(v >> (8 * i));
}

int main(void)
{
    unsigned char log[512];
    hwsense_nvme_smart_t s;

    memset(log, 0, sizeof(log));
    log[0] = 0x04;                       /* critical warning: reliability */
    put(log, 1, 323, 2);                 /* 323 K = 49.85 C */
    log[3] = 100; log[4] = 10; log[5] = 3;
    put(log, 32, 123456789ULL, 8);       /* data units read */
    put(log, 48, 5000000000ULL, 8);      /* > 32 bits: check 64-bit decode */
    put(log, 64, 111, 8);
    put(log, 80, 222, 8);
    put(log, 96, 333, 8);
    put(log, 112, 444, 8);
    put(log, 128, 555, 8);
    put(log, 144, 6, 8);
    put(log, 160, 7, 8);
    put(log, 176, 8, 8);
    put(log, 192, 12, 4);
    put(log, 196, 3, 4);
    put(log, 200, 330, 2);               /* sensor 1 = 56.85 C */
    put(log, 204, 310, 2);               /* sensor 3 (sensor 2 not implemented) */

    hwsense_nvme_smart_decode(log, &s);

    check_u64("ok", s.ok, 1);
    check_u64("critical warning", s.critical_warning, 4);
    check_dbl("composite", s.composite_celsius, 323 - 273.15);
    check_u64("spare", s.available_spare_pct, 100);
    check_u64("spare threshold", s.available_spare_threshold_pct, 10);
    check_u64("used", s.percentage_used, 3);
    check_u64("units read", s.data_units_read, 123456789ULL);
    check_u64("units written >32bit", s.data_units_written, 5000000000ULL);
    check_u64("host reads", s.host_read_commands, 111);
    check_u64("host writes", s.host_write_commands, 222);
    check_u64("busy min", s.controller_busy_minutes, 333);
    check_u64("cycles", s.power_cycles, 444);
    check_u64("hours", s.power_on_hours, 555);
    check_u64("unsafe", s.unsafe_shutdowns, 6);
    check_u64("media errors", s.media_errors, 7);
    check_u64("error entries", s.error_log_entries, 8);
    check_u64("warn temp min", s.warning_temp_minutes, 12);
    check_u64("crit temp min", s.critical_temp_minutes, 3);
    check_u64("sensor count (unimplemented skipped)", s.sensor_count, 2);
    check_dbl("sensor 1", s.sensor_celsius[0], 330 - 273.15);
    check_dbl("sensor 3", s.sensor_celsius[1], 310 - 273.15);

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
