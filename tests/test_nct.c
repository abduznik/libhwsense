/*
 * test_nct.c — Nuvoton Super I/O decode, against a simulated register file.
 *
 * The register addresses and expected values come from the Linux nct6775
 * driver tables; there is no Nuvoton hardware behind this, so it checks the
 * decoding logic (word registers, source de-duplication, voltage scaling,
 * invalid-value filtering), not that a real chip behaves this way.
 */

#include "../src/motherboard/nct_linux.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static void check_int(const char *what, int got, int want)
{
    checks++;
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

static void check_dbl(const char *what, double got, double want)
{
    checks++;
    if (fabs(got - want) > 1e-9) {
        printf("FAIL %s: got %.6f, want %.6f\n", what, got, want);
        failures++;
    }
}

static unsigned char regs[0x1000];
static int reads_out_of_range;

static int fake_rd(void *ctx, unsigned reg)
{
    (void)ctx;
    if (reg >= sizeof(regs)) {
        reads_out_of_range++;
        return -1;
    }
    return regs[reg];
}

static int dead_rd(void *ctx, unsigned reg) { (void)ctx; (void)reg; return -1; }

static void setup_common(void)
{
    memset(regs, 0, sizeof(regs));
    /* SYSTIN(1) on 0x27, CPUTIN(2) on 0x150 (16-bit, high byte = degrees) */
    regs[0x621] = 1; regs[0x27] = 38;
    regs[0x622] = 2; regs[0x150] = 45; regs[0x151] = 0x80;
    /* fan 1 = 1200 RPM, fan 2 = 0xffff (no signal), fan 3 = 840 RPM */
    regs[0x4c0] = 0x04; regs[0x4c1] = 0xb0;
    regs[0x4c2] = 0xff; regs[0x4c3] = 0xff;
    regs[0x4c4] = 0x03; regs[0x4c5] = 0x48;
    /* in0 = 0x80 * 8 mV = 1.024 V; in2 (divided) = 0xc8 * 16 mV = 3.2 V */
    regs[0x480] = 0x80;
    regs[0x482] = 0xc8;
}

static void test_chip_ids(void)
{
    check_int("6779 id", hwsense_nct_chip_name(0xc562) != NULL, 1);
    check_int("6791 id", strcmp(hwsense_nct_chip_name(0xc803), "nct6791"), 0);
    check_int("6798 id", strcmp(hwsense_nct_chip_name(0xd428), "nct6798"), 0);
    check_int("6796 vs 6798 distinct", strcmp(hwsense_nct_chip_name(0xd420), "nct6796"), 0);
    check_int("6799 id", strcmp(hwsense_nct_chip_name(0xd802), "nct6799"), 0);
    check_int("ITE id rejected", hwsense_nct_chip_name(0x8655) == NULL, 1);
    check_int("zero id rejected", hwsense_nct_chip_name(0x0000) == NULL, 1);
}

static void test_nct6791(void)
{
    hwsense_superio_result_t r;

    setup_common();
    memset(&r, 0, sizeof(r));
    check_int("6791 ok", hwsense_nct_decode(0xc803, fake_rd, NULL, &r), 1);
    check_int("6791 name", strcmp(r.chip_name, "nct6791"), 0);

    check_int("temp count", r.count, 2);
    check_int("temp SYSTIN", r.temperatures[0], 38);
    check_int("temp CPUTIN", r.temperatures[1], 45);

    check_int("fan count (0xffff skipped)", r.fan_count, 2);
    check_int("fan1", r.fan_rpms[0], 1200);
    check_int("fan3", r.fan_rpms[1], 840);

    check_int("voltage count", r.voltage_count, 15);
    check_dbl("in0 8mV", r.voltages[0], 1.024);
    check_dbl("in2 16mV", r.voltages[2], 3.2);
    check_dbl("in1 unused", r.voltages[1], 0.0);
    check_int("no OOB reads", reads_out_of_range, 0);
}

static void test_duplicate_sources(void)
{
    hwsense_superio_result_t r;

    setup_common();
    /* A monitor register pointing at SYSTIN again must not add a 3rd temp. */
    regs[0x100] = 1; regs[0x73] = 40; regs[0x74] = 0;
    /* ...while one selecting a new source (AUXTIN0 = 3) must. */
    regs[0x200] = 3; regs[0x75] = 33; regs[0x76] = 0;
    memset(&r, 0, sizeof(r));
    hwsense_nct_decode(0xc803, fake_rd, NULL, &r);
    check_int("dup source dropped", r.count, 3);
    check_int("new source added", r.temperatures[2], 33);
}

static void test_invalid_temps(void)
{
    hwsense_superio_result_t r;

    setup_common();
    regs[0x27] = 0x80;          /* -128: unconnected */
    regs[0x150] = 0x7f;         /* 127: invalid */
    memset(&r, 0, sizeof(r));
    hwsense_nct_decode(0xc803, fake_rd, NULL, &r);
    check_int("invalid temps filtered", r.count, 0);
}

static void test_nct6798_layout(void)
{
    hwsense_superio_result_t r;

    setup_common();
    /* extra fixed temp register on 6798: source 3 at 0x670 selected by 0xc26 */
    regs[0xc26] = 3; regs[0x670] = 29;
    /* in9 is divided on 6798 (16 mV) but not on 6791 (8 mV) */
    regs[0x489] = 0x64;
    memset(&r, 0, sizeof(r));
    check_int("6798 ok", hwsense_nct_decode(0xd428, fake_rd, NULL, &r), 1);
    check_int("6798 extra temp", r.count, 3);
    check_int("6798 temp3", r.temperatures[2], 29);
    check_dbl("6798 in9 16mV", r.voltages[9], 1.6);

    memset(&r, 0, sizeof(r));
    hwsense_nct_decode(0xc803, fake_rd, NULL, &r);
    check_dbl("6791 in9 8mV", r.voltages[9], 0.8);
}

static void test_nct6799_inputs(void)
{
    hwsense_superio_result_t r;

    setup_common();
    memset(&r, 0, sizeof(r));
    hwsense_nct_decode(0xd802, fake_rd, NULL, &r);
    check_int("6799 has 16 voltages (API cap)", r.voltage_count, HWSENSE_MAX_SUPERIO_VOLTAGES);
}

static void test_failures(void)
{
    hwsense_superio_result_t r;

    memset(&r, 0, sizeof(r));
    check_int("dead bus", hwsense_nct_decode(0xc803, dead_rd, NULL, &r), 0);
    memset(&r, 0, sizeof(r));
    check_int("unknown chip", hwsense_nct_decode(0x8655, fake_rd, NULL, &r), 0);
}

int main(void)
{
    test_chip_ids();
    test_nct6791();
    test_duplicate_sources();
    test_invalid_temps();
    test_nct6798_layout();
    test_nct6799_inputs();
    test_failures();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
