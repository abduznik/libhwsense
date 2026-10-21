/*
 * test_soc.c — Unit tests for the phone/ARM sensor sources.
 *
 * The decode math is tested directly. The sysfs readers are tested against
 * a fake tree built in a temporary directory, with the zone names, sentinels
 * and unit quirks seen on a real Snapdragon 8 Gen 2 (SM8550) running Android.
 * No hardware, kernel driver or privileges needed.
 * Build via CTest: cmake --build build && ctest --test-dir build
 */

/* nftw and mkdtemp need POSIX.1-2008 on glibc. */
#define _XOPEN_SOURCE 700

#include "../src/core/soc_math.h"
#include "../src/core/soc_linux.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures = 0;
static int checks = 0;

static void check_double(const char *what, double got, double want)
{
    checks++;
    if (fabs(got - want) > 1e-6) {
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

static void check_str(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) != 0) {
        printf("FAIL %s: got \"%s\", want \"%s\"\n", what, got, want);
        failures++;
    }
}

/* ── fake sysfs helpers ───────────────────────────────────────────── */

static char root[256];

static void put(const char *rel, const char *content)
{
    char path[512], dir[512], *slash;
    FILE *f;

    snprintf(path, sizeof(path), "%s/%s", root, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    for (slash = dir + strlen(root) + 1; (slash = strchr(slash, '/')); slash++) {
        *slash = '\0';
        mkdir(dir, 0700);
        *slash = '/';
    }
    f = fopen(path, "w");
    if (!f) {
        printf("cannot create %s\n", path);
        exit(2);
    }
    fputs(content, f);
    fclose(f);
}

static void zone(int idx, const char *type, const char *temp)
{
    char rel[64];

    snprintf(rel, sizeof(rel), "thermal_zone%d/type", idx);
    put(rel, type);
    if (temp) {
        snprintf(rel, sizeof(rel), "thermal_zone%d/temp", idx);
        put(rel, temp);
    }
}

/* ── decode math ──────────────────────────────────────────────────── */

static void test_thermal_decode(void)
{
    double c = 0;

    /* Normal millidegrees. */
    check_int("33800 valid", hwsense_thermal_decode(33800, &c), 1);
    check_double("33800 -> 33.8", c, 33.8);

    /* Whole degrees, as the "socd" zone reports them. */
    check_int("27 valid", hwsense_thermal_decode(27, &c), 1);
    check_double("27 -> 27", c, 27.0);

    /* Sentinels: inactive sensor and not-reporting zone. */
    check_int("-273000 rejected", hwsense_thermal_decode(-273000, &c), 0);
    check_int("0 rejected", hwsense_thermal_decode(0, &c), 0);

    /* Out of range either way. */
    check_int("200000 rejected", hwsense_thermal_decode(200000, &c), 0);
    check_int("999 (999 C) rejected", hwsense_thermal_decode(999, &c), 0);

    /* Boundaries: exactly 1 C in millidegrees, and the range limits. */
    check_int("1000 valid", hwsense_thermal_decode(1000, &c), 1);
    check_double("1000 -> 1.0", c, 1.0);
    check_int("150000 valid", hwsense_thermal_decode(150000, &c), 1);
    check_int("150001 rejected", hwsense_thermal_decode(150001, &c), 0);
    check_int("-40000 valid", hwsense_thermal_decode(-40000, &c), 1);
    check_int("-40001 rejected", hwsense_thermal_decode(-40001, &c), 0);

    /* A rejected value must not clobber the caller's output. */
    c = 12.5;
    hwsense_thermal_decode(-273000, &c);
    check_double("rejected leaves output alone", c, 12.5);
}

static void test_zone_classify(void)
{
    check_int("cpu-0-0", hwsense_thermal_is_cpu_zone("cpu-0-0"), 1);
    check_int("cpuss-3", hwsense_thermal_is_cpu_zone("cpuss-3"), 1);
    check_int("cpu-thermal (Pi)", hwsense_thermal_is_cpu_zone("cpu-thermal"), 1);
    check_int("mtktscpu", hwsense_thermal_is_cpu_zone("mtktscpu"), 1);
    check_int("x86_pkg_temp", hwsense_thermal_is_cpu_zone("x86_pkg_temp"), 1);
    check_int("soc-thermal", hwsense_thermal_is_cpu_zone("soc-thermal"), 1);
    check_int("gpuss-0 is not cpu", hwsense_thermal_is_cpu_zone("gpuss-0"), 0);
    check_int("battery is not cpu", hwsense_thermal_is_cpu_zone("battery"), 0);
    check_int("ddr is not cpu", hwsense_thermal_is_cpu_zone("ddr"), 0);
    check_int("empty is not cpu", hwsense_thermal_is_cpu_zone(""), 0);

    check_int("gpuss-0", hwsense_thermal_is_gpu_zone("gpuss-0"), 1);
    check_int("gpu-thermal", hwsense_thermal_is_gpu_zone("gpu-thermal"), 1);
    check_int("cpu-0-0 is not gpu", hwsense_thermal_is_gpu_zone("cpu-0-0"), 0);
}

/* ── readers against a fake tree ──────────────────────────────────── */

static void test_thermal_readers(void)
{
    hwsense_thermal_zone_t z[16];
    hwsense_temp_result_t t;
    double g = 0;
    int n;

    zone(0, "aoss-0\n",     "33300\n");
    zone(1, "cpuss-0\n",    "36400\n");
    zone(2, "cpu-1-5\n",    "38000\n");   /* hottest CPU zone */
    zone(3, "cpu-0-0\n",    "33800\n");
    zone(4, "gpuss-0\n",    "32700\n");
    zone(5, "gpuss-4\n",    "33900\n");   /* hottest GPU zone */
    zone(6, "bcl_warn\n",   "-273000\n"); /* inactive sentinel */
    zone(7, "pm8550b_tz\n", NULL);        /* temp unreadable */
    zone(8, "socd\n",       "27\n");      /* whole degrees */
    zone(9, "cpu-1-1\n",    "-273000\n"); /* CPU zone, but inactive */
    /* index 10 absent: scanning must not stop at a gap */
    zone(11, "battery\n",   "26400\n");

    n = hwsense_thermal_scan(root, z, 16);
    check_int("scan keeps valid zones only", n, 8);
    check_str("first zone", z[0].type, "aoss-0");
    check_double("first temp", z[0].celsius, 33.3);

    {
        int i, saw_socd = 0, saw_battery = 0, saw_sentinel = 0;
        for (i = 0; i < n; i++) {
            if (!strcmp(z[i].type, "socd")) {
                saw_socd = 1;
                check_double("socd in degrees", z[i].celsius, 27.0);
            }
            if (!strcmp(z[i].type, "battery"))
                saw_battery = 1;   /* found past the gap at index 10 */
            if (!strcmp(z[i].type, "bcl_warn") || !strcmp(z[i].type, "cpu-1-1"))
                saw_sentinel = 1;
        }
        check_int("socd listed", saw_socd, 1);
        check_int("battery listed past gap", saw_battery, 1);
        check_int("sentinel zones dropped", saw_sentinel, 0);
    }

    /* max caps the output. */
    check_int("scan honours max", hwsense_thermal_scan(root, z, 3), 3);
    check_int("scan max 0", hwsense_thermal_scan(root, z, 0), 0);
    check_int("scan NULL out", hwsense_thermal_scan(root, NULL, 4), 0);

    t = hwsense_thermal_cpu_temp(root);
    check_int("cpu temp ok", t.ok, 1);
    check_double("cpu temp is hottest valid CPU zone", t.celsius, 38.0);

    check_int("gpu temp ok", hwsense_thermal_gpu_temp(root, &g), 1);
    check_double("gpu temp is hottest GPU zone", g, 33.9);

    /* A root with no zones fails cleanly. */
    t = hwsense_thermal_cpu_temp("/nonexistent/hwsense-test");
    check_int("missing root fails", t.ok, 0);
    check_int("missing root explains itself", t.error[0] != '\0', 1);
    check_int("missing root: no gpu", hwsense_thermal_gpu_temp("/nonexistent/hwsense-test", &g), 0);
}

static void test_kgsl(void)
{
    hwsense_gpu_info_t g;

    memset(&g, 0, sizeof(g));
    check_int("kgsl absent", hwsense_kgsl_info("/nonexistent/hwsense-test", &g), 0);

    put("gpu_model", "Adreno740v2\n");
    put("clock_mhz", "220\n");
    put("gpu_busy_percentage", "37 %\n");
    check_int("kgsl present", hwsense_kgsl_info(root, &g), 1);
    check_str("kgsl model", g.name, "Adreno740v2");
    check_int("kgsl clock", g.clock_mhz, 220);
    check_int("kgsl load parses '37 %'", g.gpu_load, 37);
}

static void test_soc_name(void)
{
    char name[64] = "";
    char path[300];

    /* ARM cpuinfo has "Hardware", not "model name". */
    put("cpuinfo_arm",
        "processor\t: 0\nBogoMIPS\t: 38.40\nCPU implementer\t: 0x41\n"
        "Hardware\t: Qualcomm Technologies, Inc SM8550\n");
    snprintf(path, sizeof(path), "%s/cpuinfo_arm", root);
    check_int("hardware line found", hwsense_soc_name_cpuinfo(path, name, sizeof(name)), 1);
    check_str("hardware value", name, "Qualcomm Technologies, Inc SM8550");

    /* x86 cpuinfo has none; that is "not found", not a crash. */
    put("cpuinfo_x86", "processor\t: 0\nmodel name\t: AMD Ryzen 5 4500\n");
    snprintf(path, sizeof(path), "%s/cpuinfo_x86", root);
    name[0] = '\0';
    check_int("no hardware line", hwsense_soc_name_cpuinfo(path, name, sizeof(name)), 0);
    check_str("name untouched", name, "");

    check_int("missing file", hwsense_soc_name_cpuinfo("/nonexistent/hwsense-test", name, sizeof(name)), 0);
}

static int remove_entry(const char *path, const struct stat *sb, int type,
                        struct FTW *ftw)
{
    (void)sb; (void)type; (void)ftw;
    return remove(path);
}

int main(void)
{
    /* Termux has no /tmp; honour TMPDIR. */
    const char *tmp = getenv("TMPDIR");
    char tmpl[256];

    snprintf(tmpl, sizeof(tmpl), "%s/hwsense-soc-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(tmpl)) {
        printf("cannot create temp dir %s\n", tmpl);
        return 2;
    }
    snprintf(root, sizeof(root), "%s", tmpl);

    test_thermal_decode();
    test_zone_classify();

    /* The reader tests share one fake tree: thermal zones, KGSL files and
     * cpuinfo fixtures live side by side under the same root. */
    test_thermal_readers();
    test_kgsl();
    test_soc_name();

    if (nftw(root, remove_entry, 16, FTW_DEPTH | FTW_PHYS) != 0)
        printf("note: could not remove %s\n", root);

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
