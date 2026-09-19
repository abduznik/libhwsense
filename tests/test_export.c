/*
 * test_export.c — Unit tests for the JSON/CSV serializers.
 *
 * These operate on a struct we fill in ourselves, so no hardware,
 * driver, or privileges are involved.
 */

#include "../include/hwsense_unified.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int failures = 0;
static int checks = 0;

static void check(const char *what, int cond)
{
    checks++;
    if (!cond) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void check_str_eq(const char *what, const char *got, const char *want)
{
    checks++;
    if (strcmp(got, want) != 0) {
        printf("FAIL %s:\n  got  [%s]\n  want [%s]\n", what, got, want);
        failures++;
    }
}

static void check_int_eq(const char *what, int got, int want)
{
    checks++;
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

/* Does `hay` contain `needle`? */
static int has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/*
 * Count CSV fields the way a parser does: commas inside a quoted field
 * are data, not separators.
 */
static int count_csv_fields(const char *row)
{
    int fields = 1;
    int in_quotes = 0;
    const char *p;

    for (p = row; *p; p++) {
        if (*p == '"') {
            if (in_quotes && p[1] == '"')
                p++;            /* escaped quote inside a quoted field */
            else
                in_quotes = !in_quotes;
        } else if (*p == ',' && !in_quotes) {
            fields++;
        }
    }
    return fields;
}

static hwsense_sensor_data_t sample(void)
{
    hwsense_sensor_data_t d;
    memset(&d, 0, sizeof(d));

    strcpy(d.cpu_vendor, "Intel");
    strcpy(d.cpu_name, "Core i5-1235U");
    d.cpu_cores = 10;
    d.cpu_threads = 12;
    d.cpu_temp = 51.5;
    d.cpu_temp_min = 48.0;
    d.cpu_temp_max = 55.0;
    d.cpu_temp_avg = 51.0;
    d.cpu_clock_mhz = 2400;
    d.cpu_clock_max_mhz = 4400;
    d.cpu_voltage = 1.0625;
    d.soc_voltage = 0.85;
    d.cpu_power = 15.25;
    d.cpu_power_cores = 10.5;
    d.cpu_power_uncore = 3.0;
    d.cpu_load = 12.5;
    d.gpu_temp = 42.0;
    d.memory_used_gb = 7.5;
    d.memory_total_gb = 16.0;
    d.memory_percent = 47;
    d.dram_power = 2.25;
    d.uptime_days = 3;
    d.uptime_hours = 7;

    d.fan_count = 2;
    d.fan_rpms[0] = 1200;
    d.fan_rpms[1] = 900;

    d.drive_count = 1;
    strcpy(d.drives[0].name, "Samsung SSD 980");
    d.drives[0].temperature = 38;
    d.drives[0].size_gb = 1000;
    d.drives[0].used_gb = 420;

    return d;
}

static void test_json_content(void)
{
    hwsense_sensor_data_t d = sample();
    char buf[4096];
    int n = hwsense_export_json(&d, buf, sizeof(buf));

    check("json fits in 4096", n > 0 && n < (int)sizeof(buf));
    check("json return matches strlen", n == (int)strlen(buf));

    check("json opens/closes as object",
          buf[0] == '{' && buf[strlen(buf) - 1] == '}');

    check("json has vendor", has(buf, "\"vendor\":\"Intel\""));
    check("json has cpu name", has(buf, "\"name\":\"Core i5-1235U\""));
    check("json has cores", has(buf, "\"cores\":10"));
    check("json has temp", has(buf, "\"temp_c\":51.50"));
    check("json has clock", has(buf, "\"clock_mhz\":2400"));
    check("json has voltage", has(buf, "\"voltage_v\":1.0625"));
    check("json has gpu temp", has(buf, "\"gpu\":{\"temp_c\":42.00}"));
    check("json has memory percent", has(buf, "\"percent\":47"));
    check("json has fan array", has(buf, "\"fans\":[1200,900]"));
    check("json has drive name", has(buf, "\"name\":\"Samsung SSD 980\""));
    check("json has drive size", has(buf, "\"size_gb\":1000"));
    check("json has uptime", has(buf, "\"days\":3,\"hours\":7"));
}

static void test_json_empty_arrays(void)
{
    hwsense_sensor_data_t d;
    char buf[4096];
    memset(&d, 0, sizeof(d));

    hwsense_export_json(&d, buf, sizeof(buf));

    /* No fans or drives must still produce valid empty arrays, not [,] */
    check("json empty fans", has(buf, "\"fans\":[]"));
    check("json empty drives", has(buf, "\"drives\":[]"));
}

static void test_json_escaping(void)
{
    hwsense_sensor_data_t d;
    char buf[4096];
    memset(&d, 0, sizeof(d));

    /* A name with a quote and a backslash must not break the JSON. */
    strcpy(d.cpu_vendor, "A\"B\\C");
    hwsense_export_json(&d, buf, sizeof(buf));
    check("json escapes quote and backslash",
          has(buf, "\"vendor\":\"A\\\"B\\\\C\""));

    /* Control characters become \u00XX. */
    memset(&d, 0, sizeof(d));
    d.cpu_vendor[0] = 'X';
    d.cpu_vendor[1] = 0x01;
    d.cpu_vendor[2] = '\0';
    hwsense_export_json(&d, buf, sizeof(buf));
    check("json escapes control char", has(buf, "\"vendor\":\"X\\u0001\""));

    /* Tab and newline use their short forms. */
    memset(&d, 0, sizeof(d));
    strcpy(d.cpu_vendor, "X\tY\nZ");
    hwsense_export_json(&d, buf, sizeof(buf));
    check("json escapes tab/newline", has(buf, "\"vendor\":\"X\\tY\\nZ\""));
}

static void test_json_truncation(void)
{
    hwsense_sensor_data_t d = sample();
    char big[4096];
    char tiny[16];
    int full, got;

    full = hwsense_export_json(&d, big, sizeof(big));

    /* A short buffer must report the full required size, not what it wrote. */
    got = hwsense_export_json(&d, tiny, (int)sizeof(tiny));
    check_int_eq("truncated json reports full size", got, full);

    /* And it must still be NUL-terminated within the buffer. */
    check("truncated json is terminated", strlen(tiny) < sizeof(tiny));

    /* What it did write must be a prefix of the full output. */
    check("truncated json is a prefix",
          strncmp(tiny, big, strlen(tiny)) == 0);

    /* Size-probing with a zero-length buffer must not write. */
    got = hwsense_export_json(&d, NULL, 0);
    check_int_eq("zero-size probe returns full size", got, full);

    /* Allocating exactly (n + 1) must hold the whole thing. */
    {
        char *exact = malloc((size_t)full + 1);
        int n2 = hwsense_export_json(&d, exact, full + 1);
        check_int_eq("exact buffer returns same size", n2, full);
        check_str_eq("exact buffer matches", exact, big);
        free(exact);
    }
}

static void test_json_bad_args(void)
{
    hwsense_sensor_data_t d = sample();
    char buf[64];

    check_int_eq("null data rejected", hwsense_export_json(NULL, buf, sizeof(buf)), -1);
    check_int_eq("negative size rejected", hwsense_export_json(&d, buf, -1), -1);
    check_int_eq("null buf with size rejected", hwsense_export_json(&d, NULL, 10), -1);
}

static void test_csv(void)
{
    hwsense_sensor_data_t d = sample();
    char header[1024];
    char row[1024];
    int hn, rn;

    hn = hwsense_export_csv_header(header, sizeof(header));
    rn = hwsense_export_csv_row(&d, row, sizeof(row));

    check("csv header fits", hn > 0 && hn < (int)sizeof(header));
    check("csv row fits", rn > 0 && rn < (int)sizeof(row));

    check("csv header starts with cpu_vendor",
          strncmp(header, "cpu_vendor,", 11) == 0);
    check("csv row starts with Intel", strncmp(row, "Intel,", 6) == 0);

    /* The row must have exactly as many fields as the header. */
    check_int_eq("csv field count matches header",
                 count_csv_fields(row), count_csv_fields(header));

    check("csv has temp", has(row, "51.50"));
    check("csv has clock", has(row, "2400"));
    check("csv has uptime", has(row, ",3,7"));

    /* Neither may contain a raw newline — one record per line. */
    check("csv header has no newline", strchr(header, '\n') == NULL);
    check("csv row has no newline", strchr(row, '\n') == NULL);
}

static void test_csv_quoting(void)
{
    hwsense_sensor_data_t d;
    char row[1024];
    char header[1024];

    hwsense_export_csv_header(header, sizeof(header));

    /* A comma in a field must force quoting, or the column count breaks. */
    memset(&d, 0, sizeof(d));
    strcpy(d.cpu_name, "Core i5, 12th Gen");
    hwsense_export_csv_row(&d, row, sizeof(row));
    check("csv quotes field with comma", has(row, "\"Core i5, 12th Gen\""));
    check_int_eq("csv with embedded comma keeps field count",
                 count_csv_fields(row), count_csv_fields(header));

    /* An embedded quote is doubled, per RFC 4180. */
    memset(&d, 0, sizeof(d));
    strcpy(d.cpu_name, "A\"B");
    hwsense_export_csv_row(&d, row, sizeof(row));
    check("csv doubles embedded quote", has(row, "\"A\"\"B\""));

    /* A plain field is not quoted. */
    memset(&d, 0, sizeof(d));
    strcpy(d.cpu_name, "PlainName");
    hwsense_export_csv_row(&d, row, sizeof(row));
    check("csv leaves plain field unquoted", has(row, ",PlainName,"));
}

static void test_csv_truncation(void)
{
    hwsense_sensor_data_t d = sample();
    char big[1024];
    char tiny[8];
    int full, got;

    full = hwsense_export_csv_row(&d, big, sizeof(big));
    got = hwsense_export_csv_row(&d, tiny, (int)sizeof(tiny));

    check_int_eq("truncated csv reports full size", got, full);
    check("truncated csv is terminated", strlen(tiny) < sizeof(tiny));
    check("truncated csv is a prefix", strncmp(tiny, big, strlen(tiny)) == 0);
}

int main(void)
{
    test_json_content();
    test_json_empty_arrays();
    test_json_escaping();
    test_json_truncation();
    test_json_bad_args();
    test_csv();
    test_csv_quoting();
    test_csv_truncation();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
