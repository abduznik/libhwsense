/*
 * test_sensor_math.c — Unit tests for the pure register-decode math.
 *
 * These run anywhere: no hardware, no kernel driver, no privileges.
 * Build via CTest: cmake --build build && ctest --test-dir build
 */

#include "../src/core/sensor_math.h"
#include <stdio.h>
#include <math.h>

static int failures = 0;
static int checks = 0;

static void check_double(const char *what, double got, double want)
{
    checks++;
    if (fabs(got - want) > 1e-9) {
        printf("FAIL %s: got %.6f, want %.6f\n", what, got, want);
        failures++;
    }
}

static void check_int(const char *what, long got, long want)
{
    checks++;
    if (got != want) {
        printf("FAIL %s: got %ld, want %ld\n", what, got, want);
        failures++;
    }
}

static void test_intel_tjmax(void)
{
    /* TjMax lives in bits [23:16]. */
    check_int("tjmax 100C", hwsense_intel_tjmax(100u << 16), 100);
    check_int("tjmax 105C", hwsense_intel_tjmax(105u << 16), 105);

    /* Out-of-range values fall back to 100. */
    check_int("tjmax 0 -> fallback", hwsense_intel_tjmax(0), 100);
    check_int("tjmax 49 -> fallback", hwsense_intel_tjmax(49u << 16), 100);
    check_int("tjmax 151 -> fallback", hwsense_intel_tjmax(151u << 16), 100);

    /* Boundaries are accepted as-is. */
    check_int("tjmax 50 boundary", hwsense_intel_tjmax(50u << 16), 50);
    check_int("tjmax 150 boundary", hwsense_intel_tjmax(150u << 16), 150);

    /* Bits outside [23:16] must not leak in. */
    check_int("tjmax ignores other bits",
              hwsense_intel_tjmax(0xFF00FFFFu | (100u << 16)), 100);
}

static void test_intel_therm_valid(void)
{
    check_int("therm valid set", hwsense_intel_therm_valid(0x80000000u), 1);
    check_int("therm valid clear", hwsense_intel_therm_valid(0x7FFFFFFFu), 0);
}

static void test_intel_temp(void)
{
    /* TjMax 100, readout 40 -> 60 C */
    check_double("intel 100-40", hwsense_intel_temp_c(100u << 16, 40u << 16), 60.0);

    /* Readout 0 means the core is at TjMax. */
    check_double("intel at tjmax", hwsense_intel_temp_c(100u << 16, 0), 100.0);

    /* Readout is 7 bits: 0x7F = 127 below TjMax. */
    check_double("intel max readout",
                 hwsense_intel_temp_c(100u << 16, 0x7Fu << 16), -27.0);

    /* The valid bit must not bleed into the readout. */
    check_double("intel valid bit ignored",
                 hwsense_intel_temp_c(100u << 16, 0x80000000u | (40u << 16)), 60.0);
}

static void test_amd_tctl(void)
{
    /* CUR_TEMP is bits [31:21] in 0.125 C units. 400 units = 50 C. */
    check_double("amd tctl 50C", hwsense_amd_tctl_c(400u << 21), 50.0);
    check_double("amd tctl 0C", hwsense_amd_tctl_c(0), 0.0);

    /* RANGE_SEL (bit 19) applies a -49 C offset. */
    check_double("amd tctl range_sel",
                 hwsense_amd_tctl_c((400u << 21) | 0x80000u), 1.0);

    /* TJ_SEL == 0b11 (bits [17:16]) also applies the offset. */
    check_double("amd tctl tj_sel",
                 hwsense_amd_tctl_c((400u << 21) | 0x30000u), 1.0);

    /* A single TJ_SEL bit is not enough to trigger the offset. */
    check_double("amd tctl tj_sel partial",
                 hwsense_amd_tctl_c((400u << 21) | 0x10000u), 50.0);

    /* Both offset conditions together still apply -49 once, not twice. */
    check_double("amd tctl both flags",
                 hwsense_amd_tctl_c((400u << 21) | 0x80000u | 0x30000u), 1.0);
}

static void test_amd_ccd(void)
{
    /* temp = (raw12 * 125 - 305000) / 1000; raw12 = 2440 -> 0 C */
    check_double("amd ccd 0C", hwsense_amd_ccd_temp_c(2440), 0.0);
    check_double("amd ccd 50C", hwsense_amd_ccd_temp_c(2840), 50.0);

    /* Only the low 12 bits participate. */
    check_double("amd ccd ignores high bits",
                 hwsense_amd_ccd_temp_c(0xFFFFF000u | 2440u), 0.0);

    /* Absent sensors read back as zero. */
    check_int("amd ccd zero implausible", hwsense_amd_ccd_temp_plausible(0), 0);

    /* raw12 = 0 decodes to -305 C, far outside the plausible range. */
    check_int("amd ccd raw12 zero implausible",
              hwsense_amd_ccd_temp_plausible(0xFFFFF000u), 0);

    check_int("amd ccd 50C plausible", hwsense_amd_ccd_temp_plausible(2840), 1);

    /* Boundaries: -40 C and 150 C are accepted. */
    check_int("amd ccd -40C plausible", hwsense_amd_ccd_temp_plausible(2120), 1);
    check_int("amd ccd 150C plausible", hwsense_amd_ccd_temp_plausible(3640), 1);

    /* Just past the upper boundary is rejected. */
    check_int("amd ccd 150.125C implausible",
              hwsense_amd_ccd_temp_plausible(3641), 0);
}

static void test_svi2(void)
{
    /* VID is bits [24:16], 9 bits wide. */
    check_int("svi2 vid extract", (long)hwsense_svi2_vid(0x00240000u), 0x24);
    check_int("svi2 vid masks to 9 bits",
              (long)hwsense_svi2_vid(0xFFFF0000u), 0x1FF);

    /* 6.25 mV per LSB. */
    check_double("svi2 vid 0", hwsense_svi2_vid_to_volts(0), 0.0);
    check_double("svi2 vid 1", hwsense_svi2_vid_to_volts(1), 0.00625);
    check_double("svi2 vid 200", hwsense_svi2_vid_to_volts(200), 1.25);

    /* The off marker. */
    check_int("svi2 off marker", HWSENSE_SVI2_VID_OFF, 0x1FF);
}

static void test_family_known(void)
{
    /* Intel Core/Atom. */
    check_int("intel family 6 known",
              hwsense_cpu_family_known("GenuineIntel", 0x6), 1);
    check_int("intel family 5 unknown",
              hwsense_cpu_family_known("GenuineIntel", 0x5), 0);

    /*
     * These are the values CPUID actually reports for shipping AMD parts.
     * Writing the table in decimal instead of hex makes every one of these
     * miss, which is the bug this guards against — a Ryzen 5 4500 reports
     * family 0x17 (decimal 23), so a `family == 17` test never fires.
     */
    check_int("amd zen2 renoir (0x17) known",
              hwsense_cpu_family_known("AuthenticAMD", 0x17), 1);
    check_int("amd bulldozer (0x15) known",
              hwsense_cpu_family_known("AuthenticAMD", 0x15), 1);
    check_int("amd zen3/zen4 (0x19) known",
              hwsense_cpu_family_known("AuthenticAMD", 0x19), 1);
    check_int("amd zen5 (0x1A) known",
              hwsense_cpu_family_known("AuthenticAMD", 0x1A), 1);

    /* Decimal 17 is family 0x11, which is not a Zen part. */
    check_int("amd decimal 17 is not a known family",
              hwsense_cpu_family_known("AuthenticAMD", 17), 0);

    /* Unknown vendors and NULL must not match anything. */
    check_int("unknown vendor", hwsense_cpu_family_known("SomeOtherCPU", 0x17), 0);
    check_int("null vendor", hwsense_cpu_family_known(NULL, 0x17), 0);

    /* A vendor must not match the other vendor's family table. */
    check_int("intel does not match amd family",
              hwsense_cpu_family_known("GenuineIntel", 0x17), 0);
    check_int("amd does not match intel family",
              hwsense_cpu_family_known("AuthenticAMD", 0x6), 0);
}

static void test_rapl(void)
{
    /* Energy unit is 1/2^bits[12:8]. Unit 16 is the common Intel value. */
    check_double("rapl unit 16", hwsense_rapl_energy_unit(16u << 8), 1.0 / 65536.0);
    check_double("rapl unit 0", hwsense_rapl_energy_unit(0), 1.0);

    /* 65536 raw units at 1/65536 J per unit = 1 J; over 1 s = 1 W. */
    check_double("rapl 1W", hwsense_rapl_power_w(0, 65536, 1.0 / 65536.0, 1.0), 1.0);

    /* Halving the interval doubles the power. */
    check_double("rapl 2W", hwsense_rapl_power_w(0, 65536, 1.0 / 65536.0, 0.5), 2.0);

    /* The 32-bit counter wraps; the delta must still come out right. */
    check_double("rapl wraparound",
                 hwsense_rapl_power_w(0xFFFFFFFFu, 0xFFFF, 1.0 / 65536.0, 1.0),
                 (double)0x10000 / 65536.0);

    /* A non-positive interval is an error, not a divide-by-zero. */
    check_double("rapl zero interval",
                 hwsense_rapl_power_w(0, 65536, 1.0 / 65536.0, 0.0), -1.0);
}

int main(void)
{
    test_intel_tjmax();
    test_intel_therm_valid();
    test_intel_temp();
    test_amd_tctl();
    test_amd_ccd();
    test_svi2();
    test_family_known();
    test_rapl();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
