/*
 * export.c — Serialize a sensor snapshot to JSON or CSV.
 *
 * These take an already-populated hwsense_sensor_data_t and touch no
 * hardware, so they work identically on every platform and are unit
 * tested in tests/test_export.c.
 *
 * Both writers take a caller-supplied buffer and return the number of
 * characters the full output needs, excluding the terminator — the same
 * contract as snprintf. A return value >= buf_size means the output was
 * truncated and the caller should retry with a larger buffer.
 */

#include "../../include/hwsense_unified.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

/*
 * Append to a buffer, tracking the total length the output would need
 * even after the buffer fills up.
 */
typedef struct {
    char  *buf;
    size_t size;   /* capacity of buf, may be 0 */
    size_t needed; /* chars required so far, excluding terminator */
} appender_t;

static void append(appender_t *a, const char *fmt, ...)
{
    va_list args;
    size_t remaining;
    int n;

    /* Where the next write lands, and how much room is left for it. */
    remaining = (a->needed < a->size) ? (a->size - a->needed) : 0;

    va_start(args, fmt);
    if (remaining > 0)
        n = vsnprintf(a->buf + a->needed, remaining, fmt, args);
    else
        n = vsnprintf(NULL, 0, fmt, args);
    va_end(args);

    if (n > 0)
        a->needed += (size_t)n;
}

/*
 * Write a string as a JSON string value, escaping what RFC 8259 requires.
 * Control characters below 0x20 become \u00XX.
 */
static void append_json_string(appender_t *a, const char *s)
{
    append(a, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        switch (c) {
        case '"':  append(a, "\\\""); break;
        case '\\': append(a, "\\\\"); break;
        case '\b': append(a, "\\b");  break;
        case '\f': append(a, "\\f");  break;
        case '\n': append(a, "\\n");  break;
        case '\r': append(a, "\\r");  break;
        case '\t': append(a, "\\t");  break;
        default:
            if (c < 0x20)
                append(a, "\\u%04x", c);
            else
                append(a, "%c", c);
            break;
        }
    }
    append(a, "\"");
}

/*
 * Write a string as a CSV field per RFC 4180: quote it when it contains a
 * comma, quote, or newline, and double any embedded quotes.
 */
static void append_csv_field(appender_t *a, const char *s)
{
    const char *p;
    int needs_quotes = 0;

    for (p = s; *p; p++) {
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') {
            needs_quotes = 1;
            break;
        }
    }

    if (!needs_quotes) {
        append(a, "%s", s);
        return;
    }

    append(a, "\"");
    for (p = s; *p; p++) {
        if (*p == '"')
            append(a, "\"\"");
        else
            append(a, "%c", *p);
    }
    append(a, "\"");
}

int hwsense_export_json(const hwsense_sensor_data_t *data, char *buf, int buf_size)
{
    appender_t a;
    int i;

    if (!data || buf_size < 0 || (buf_size > 0 && !buf))
        return -1;

    a.buf = buf;
    a.size = (size_t)buf_size;
    a.needed = 0;

    append(&a, "{");

    append(&a, "\"cpu\":{");
    append(&a, "\"vendor\":");
    append_json_string(&a, data->cpu_vendor);
    append(&a, ",\"name\":");
    append_json_string(&a, data->cpu_name);
    append(&a, ",\"cores\":%d", data->cpu_cores);
    append(&a, ",\"threads\":%d", data->cpu_threads);
    append(&a, ",\"temp_c\":%.2f", data->cpu_temp);
    append(&a, ",\"temp_min_c\":%.2f", data->cpu_temp_min);
    append(&a, ",\"temp_max_c\":%.2f", data->cpu_temp_max);
    append(&a, ",\"temp_avg_c\":%.2f", data->cpu_temp_avg);
    append(&a, ",\"clock_mhz\":%d", data->cpu_clock_mhz);
    append(&a, ",\"clock_max_mhz\":%d", data->cpu_clock_max_mhz);
    append(&a, ",\"voltage_v\":%.4f", data->cpu_voltage);
    append(&a, ",\"soc_voltage_v\":%.4f", data->soc_voltage);
    append(&a, ",\"power_w\":%.2f", data->cpu_power);
    append(&a, ",\"power_cores_w\":%.2f", data->cpu_power_cores);
    append(&a, ",\"power_uncore_w\":%.2f", data->cpu_power_uncore);
    append(&a, ",\"load_percent\":%.1f", data->cpu_load);
    append(&a, "}");

    append(&a, ",\"gpu\":{\"temp_c\":%.2f}", data->gpu_temp);

    append(&a, ",\"memory\":{");
    append(&a, "\"used_gb\":%.2f", data->memory_used_gb);
    append(&a, ",\"total_gb\":%.2f", data->memory_total_gb);
    append(&a, ",\"percent\":%d", data->memory_percent);
    append(&a, ",\"dram_power_w\":%.2f", data->dram_power);
    append(&a, "}");

    append(&a, ",\"fans\":[");
    for (i = 0; i < data->fan_count && i < 8; i++)
        append(&a, "%s%d", i ? "," : "", data->fan_rpms[i]);
    append(&a, "]");

    append(&a, ",\"drives\":[");
    for (i = 0; i < data->drive_count && i < 8; i++) {
        append(&a, "%s{\"name\":", i ? "," : "");
        append_json_string(&a, data->drives[i].name);
        append(&a, ",\"temp_c\":%d", data->drives[i].temperature);
        append(&a, ",\"size_gb\":%lld", data->drives[i].size_gb);
        append(&a, ",\"used_gb\":%lld", data->drives[i].used_gb);
        append(&a, "}");
    }
    append(&a, "]");

    append(&a, ",\"uptime\":{\"days\":%d,\"hours\":%d}",
           data->uptime_days, data->uptime_hours);

    append(&a, "}");

    return (int)a.needed;
}

/* Column order must match hwsense_export_csv_row. */
int hwsense_export_csv_header(char *buf, int buf_size)
{
    appender_t a;

    if (buf_size < 0 || (buf_size > 0 && !buf))
        return -1;

    a.buf = buf;
    a.size = (size_t)buf_size;
    a.needed = 0;

    append(&a,
           "cpu_vendor,cpu_name,cpu_cores,cpu_threads,"
           "cpu_temp_c,cpu_temp_min_c,cpu_temp_max_c,cpu_temp_avg_c,"
           "cpu_clock_mhz,cpu_clock_max_mhz,cpu_voltage_v,soc_voltage_v,"
           "cpu_power_w,cpu_power_cores_w,cpu_power_uncore_w,cpu_load_percent,"
           "gpu_temp_c,memory_used_gb,memory_total_gb,memory_percent,"
           "dram_power_w,uptime_days,uptime_hours");

    return (int)a.needed;
}

int hwsense_export_csv_row(const hwsense_sensor_data_t *data, char *buf, int buf_size)
{
    appender_t a;

    if (!data || buf_size < 0 || (buf_size > 0 && !buf))
        return -1;

    a.buf = buf;
    a.size = (size_t)buf_size;
    a.needed = 0;

    append_csv_field(&a, data->cpu_vendor);
    append(&a, ",");
    append_csv_field(&a, data->cpu_name);
    append(&a, ",%d,%d", data->cpu_cores, data->cpu_threads);
    append(&a, ",%.2f,%.2f,%.2f,%.2f",
           data->cpu_temp, data->cpu_temp_min,
           data->cpu_temp_max, data->cpu_temp_avg);
    append(&a, ",%d,%d", data->cpu_clock_mhz, data->cpu_clock_max_mhz);
    append(&a, ",%.4f,%.4f", data->cpu_voltage, data->soc_voltage);
    append(&a, ",%.2f,%.2f,%.2f",
           data->cpu_power, data->cpu_power_cores, data->cpu_power_uncore);
    append(&a, ",%.1f", data->cpu_load);
    append(&a, ",%.2f", data->gpu_temp);
    append(&a, ",%.2f,%.2f,%d",
           data->memory_used_gb, data->memory_total_gb, data->memory_percent);
    append(&a, ",%.2f", data->dram_power);
    append(&a, ",%d,%d", data->uptime_days, data->uptime_hours);

    return (int)a.needed;
}
