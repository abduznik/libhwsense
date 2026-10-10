/*
 * test_ite.c — ITE Super I/O decode against a simulated register file.
 *
 * The register values mirror a real IT8655E reading (ASUS PRIME B450M-K), but
 * they are derived from the decoded numbers rather than dumped from the
 * chip. The test checks the decoding logic, bank handling and filtering.
 */

#include "../src/motherboard/ite_decode.h"
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

static unsigned char regs[0x100];
static int writes;

static int fake_rd(void *ctx, unsigned reg)
{
    (void)ctx;
    return reg < sizeof(regs) ? regs[reg] : -1;
}

static void fake_wr(void *ctx, unsigned reg, int val)
{
    (void)ctx;
    writes++;
    if (reg < sizeof(regs))
        regs[reg] = (unsigned char)val;
}

static int dead_rd(void *ctx, unsigned reg) { (void)ctx; (void)reg; return -1; }

static void setup(void)
{
    memset(regs, 0, sizeof(regs));
    writes = 0;
    /* temps: 51, 43, 40, 40; the other two inputs unconnected */
    regs[0x29] = 51; regs[0x2a] = 43; regs[0x2b] = 40; regs[0x2c] = 40;
    regs[0x2d] = 0x80; regs[0x2e] = 0;
    /* fan1 = 1809 RPM -> count 373 = 0x0175; fan2 = 1004 RPM -> 672 = 0x02a0;
     * fan3 reads 0xffff (no signal) */
    regs[0x0d] = 0x75; regs[0x18] = 0x01;
    regs[0x0e] = 0xa0; regs[0x19] = 0x02;
    regs[0x0f] = 0xff; regs[0x1a] = 0xff;
    /* voltages: in0 = 127 * 10.9 mV, in7 = 153 * 21.8 mV (divided input) */
    regs[0x20] = 127;
    regs[0x27] = 153;
    regs[0x28] = 150;
    regs[0x2f] = 153;
}

static void test_names(void)
{
    check_int("8655 known", hwsense_ite_chip_name(0x8655) != NULL, 1);
    check_int("8665 known", hwsense_ite_chip_name(0x8665) != NULL, 1);
    check_int("8686 known", hwsense_ite_chip_name(0x8686) != NULL, 1);
    check_int("8728 known", hwsense_ite_chip_name(0x8728) != NULL, 1);
    check_int("8792 (unsupported) rejected", hwsense_ite_chip_name(0x8733) == NULL, 1);
    check_int("nuvoton id rejected", hwsense_ite_chip_name(0xd802) == NULL, 1);
    check_int("zero id rejected", hwsense_ite_chip_name(0) == NULL, 1);
}

static void test_it8655(void)
{
    hwsense_superio_result_t r;

    setup();
    memset(&r, 0, sizeof(r));
    check_int("ok", hwsense_ite_decode(0x8655, fake_rd, fake_wr, NULL, &r), 1);
    check_int("name", strcmp(r.chip_name, "it8655"), 0);
    check_int("chip id", r.chip_id, 0x8655);

    check_int("temp count (unconnected skipped)", r.count, 4);
    check_int("temp1", r.temperatures[0], 51);
    check_int("temp4", r.temperatures[3], 40);

    check_int("fan count (no-signal skipped)", r.fan_count, 2);
    check_int("fan1", r.fan_rpms[0], 1809);
    check_int("fan2", r.fan_rpms[1], 1004);

    check_int("voltage count", r.voltage_count, 10);
    check_dbl("in0 10.9mV", r.voltages[0], 127 * 109 / 10000.0);
    check_dbl("in7 divided", r.voltages[7], 153 * 218 / 10000.0);
    check_dbl("in9 divided", r.voltages[9], 153 * 218 / 10000.0);
    check_int("no bank writes when bank 0", writes, 0);
}

static void test_adc_lsb(void)
{
    hwsense_superio_result_t r;

    setup();
    memset(&r, 0, sizeof(r));
    hwsense_ite_decode(0x8686, fake_rd, fake_wr, NULL, &r);
    check_dbl("it8686 in0 12mV", r.voltages[0], 127 * 120 / 10000.0);
    check_dbl("it8686 in7 24mV", r.voltages[7], 153 * 240 / 10000.0);
}

static void test_fan_counts(void)
{
    hwsense_superio_result_t r;

    setup();
    /* fan4 lives at 0x80/0x81 on chips with more than three fans */
    regs[0x80] = 0x4a; regs[0x81] = 0x01;           /* count 330 -> 2045 RPM */
    memset(&r, 0, sizeof(r));
    hwsense_ite_decode(0x8655, fake_rd, fake_wr, NULL, &r);
    check_int("it8655 has only 3 fans", r.fan_count, 2);

    memset(&r, 0, sizeof(r));
    hwsense_ite_decode(0x8665, fake_rd, fake_wr, NULL, &r);
    check_int("it8665 has fan4", r.fan_count, 3);
    check_int("it8665 fan4 rpm", r.fan_rpms[2], 1350000 / (330 * 2));
}

static void test_temp_counts(void)
{
    hwsense_superio_result_t r;

    setup();
    regs[0x29] = 51; regs[0x2a] = 43; regs[0x2b] = 40; regs[0x2c] = 41; regs[0x2d] = 42;
    memset(&r, 0, sizeof(r));
    hwsense_ite_decode(0x8728, fake_rd, fake_wr, NULL, &r);
    check_int("it8728 reads 3 temps", r.count, 3);
    memset(&r, 0, sizeof(r));
    hwsense_ite_decode(0x8655, fake_rd, fake_wr, NULL, &r);
    check_int("it8655 reads 5 connected temps", r.count, 5);
}

static void test_bank_restore(void)
{
    hwsense_superio_result_t r;

    setup();
    regs[0x06] = 0x40;                 /* chip left in bank 2 by another driver */
    memset(&r, 0, sizeof(r));
    check_int("ok in bank 2", hwsense_ite_decode(0x8655, fake_rd, fake_wr, NULL, &r), 1);
    check_int("bank restored", regs[0x06], 0x40);
    check_int("two bank writes (to 0, back)", writes, 2);

    setup();
    regs[0x06] = 0x07;                 /* low bits set, bank already 0 */
    memset(&r, 0, sizeof(r));
    hwsense_ite_decode(0x8655, fake_rd, fake_wr, NULL, &r);
    check_int("low bits preserved, no writes", writes, 0);
}

static void test_failures(void)
{
    hwsense_superio_result_t r;

    memset(&r, 0, sizeof(r));
    check_int("dead bus", hwsense_ite_decode(0x8655, dead_rd, fake_wr, NULL, &r), 0);
    memset(&r, 0, sizeof(r));
    check_int("unknown chip", hwsense_ite_decode(0x1234, fake_rd, fake_wr, NULL, &r), 0);

    setup();
    memset(regs + 0x20, 0, 0x10);       /* everything reads zero */
    memset(regs + 0x29, 0, 6);
    regs[0x0d] = regs[0x18] = 0; regs[0x0e] = regs[0x19] = 0; regs[0x0f] = regs[0x1a] = 0;
    memset(&r, 0, sizeof(r));
    /* voltages read as 0.000 V (still counted), so this still reports ok */
    check_int("all-zero chip still returns voltages", hwsense_ite_decode(0x8655, fake_rd, fake_wr, NULL, &r), 1);
}

int main(void)
{
    test_names();
    test_it8655();
    test_adc_lsb();
    test_fan_counts();
    test_temp_counts();
    test_bank_restore();
    test_failures();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
