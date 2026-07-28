/*
 * read_homelab_sensors.c — Linux hardware sensor reader.
 *
 * Reads: CPU temps (thermal zones), RAPL power, CPU frequency,
 *        drive SMART stats (SATA + NVMe), fan speeds, network stats.
 *
 * Compile: gcc -o read_homelab_sensors read_homelab_sensors.c -lm
 * Run:     sudo ./read_homelab_sensors  (sudo needed for RAPL + smartctl)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>
#include <ctype.h>

#define MAX_PATH 512
#define MAX_DRIVES 8
#define MAX_NET_IF 8

/* ── Thermal Zone Reading ──────────────────────────────────────────── */

typedef struct {
    char name[64];
    int temp_milli;  /* millidegrees Celsius */
} thermal_zone_t;

static int read_thermal_zones(thermal_zone_t *zones, int max_zones)
{
    char path[MAX_PATH];
    DIR *dir;
    struct dirent *ent;
    int count = 0;

    dir = opendir("/sys/class/thermal");
    if (!dir)
        return 0;

    while ((ent = readdir(dir)) != NULL && count < max_zones) {
        if (strncmp(ent->d_name, "thermal_zone", 12) != 0)
            continue;

        /* Read type */
        snprintf(path, sizeof(path), "/sys/class/thermal/%s/type", ent->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            if (fgets(zones[count].name, sizeof(zones[count].name), f)) {
                /* Remove trailing newline */
                char *nl = strchr(zones[count].name, '\n');
                if (nl) *nl = '\0';
            }
            fclose(f);
        } else {
            strcpy(zones[count].name, ent->d_name);
        }

        /* Read temperature */
        snprintf(path, sizeof(path), "/sys/class/thermal/%s/temp", ent->d_name);
        f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%d", &zones[count].temp_milli) != 1)
                zones[count].temp_milli = -1;
            fclose(f);
        } else {
            zones[count].temp_milli = -1;
        }

        count++;
    }

    closedir(dir);
    return count;
}

/* ── RAPL Power Reading ────────────────────────────────────────────── */

typedef struct {
    unsigned long long energy_uj;      /* current energy in microjoules */
    unsigned long long max_energy_uj;  /* max range in microjoules */
    int available;
} rapl_reading_t;

static rapl_reading_t read_rapl(void)
{
    rapl_reading_t r = {0};
    FILE *f;

    f = fopen("/sys/class/powercap/intel-rapl:0/energy_uj", "r");
    if (f) {
        if (fscanf(f, "%llu", &r.energy_uj) == 1)
            r.available = 1;
        fclose(f);
    }

    f = fopen("/sys/class/powercap/intel-rapl:0/max_energy_range_uj", "r");
    if (f) {
        fscanf(f, "%llu", &r.max_energy_uj);
        fclose(f);
    }

    return r;
}

static double compute_rapl_power(rapl_reading_t *prev, rapl_reading_t *curr)
{
    if (!prev->available || !curr->available)
        return -1.0;

    unsigned long long energy_delta = curr->energy_uj - prev->energy_uj;

    /* Handle wraparound */
    if (curr->energy_uj < prev->energy_uj)
        energy_delta += curr->max_energy_uj;

    /* Convert microjoules to joules, assume ~1 second between readings */
    return (double)energy_delta / 1000000.0;
}

/* ── CPU Frequency ─────────────────────────────────────────────────── */

static int read_cpu_freq_mhz(int core)
{
    char path[MAX_PATH];
    FILE *f;
    int freq_khz = 0;

    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_cur_freq", core);
    f = fopen(path, "r");
    if (!f)
        return -1;

    if (fscanf(f, "%d", &freq_khz) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);

    return freq_khz / 1000;
}

static int get_cpu_count(void)
{
    int count = 0;
    char path[MAX_PATH];
    FILE *f;

    snprintf(path, sizeof(path), "/sys/devices/system/cpu/online");
    f = fopen(path, "r");
    if (f) {
        int first, last;
        if (fscanf(f, "%d-%d", &first, &last) == 2)
            count = last - first + 1;
        else if (fscanf(f, "%d", &first) == 1)
            count = 1;
        fclose(f);
    }
    return count;
}

/* ── Drive SMART Stats ─────────────────────────────────────────────── */

typedef struct {
    char model[128];
    char serial[64];
    char type[16];       /* "SATA" or "NVMe" */
    int temperature;     /* Celsius */
    int power_on_hours;
    int percentage_used;  /* NVMe only */
    int wear_leveling;    /* SATA SSD only */
    long long total_lbas_written;  /* sectors */
    long long total_lbas_read;
    int reallocated_sectors;
    int pending_sectors;
    int available;
} drive_stats_t;

static int read_drive_stats(const char *device, drive_stats_t *stats)
{
    char cmd[MAX_PATH];
    FILE *f;
    char line[1024];
    int is_nvme = 0;

    memset(stats, 0, sizeof(*stats));
    stats->temperature = -1;
    stats->power_on_hours = -1;

    /* Determine if NVMe or SATA */
    if (strncmp(device, "nvme", 4) == 0)
        is_nvme = 1;

    /* Get model name first */
    snprintf(cmd, sizeof(cmd), "smartctl -i /dev/%s 2>/dev/null", device);
    f = popen(cmd, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *p;
            if ((p = strstr(line, "Device Model:")) || (p = strstr(line, "Model Number:"))) {
                p = strchr(p, ':');
                if (p) {
                    p += 2;
                    char *nl = strchr(p, '\n');
                    if (nl) *nl = '\0';
                    while (*p == ' ') p++;
                    strncpy(stats->model, p, sizeof(stats->model) - 1);
                }
            }
        }
        pclose(f);
    }

    /* Get SMART attributes */
    snprintf(cmd, sizeof(cmd), "smartctl -A /dev/%s 2>/dev/null", device);
    f = popen(cmd, "r");
    if (!f)
        return 0;

    while (fgets(line, sizeof(line), f)) {
        /* Temperature (SATA: RAW_VALUE is last field) */
        if (strstr(line, "Temperature_Celsius")) {
            /* Format: 194 Temperature_Celsius 0x0022 100 100 050 Old_age Always - 40 */
            char *p = line + strlen(line) - 1;
            while (p > line && (*p == '\n' || *p == ' ' || *p == '\t')) *p-- = '\0';
            p = strrchr(line, ' ');
            if (p) stats->temperature = atoi(p + 1);
        }

        /* NVMe Temperature */
        if (strstr(line, "Temperature:") && !strstr(line, "Warning") && !strstr(line, "Sensor")) {
            int temp;
            if (sscanf(line, "Temperature: %d Celsius", &temp) == 1)
                stats->temperature = temp;
        }

        /* Power On Hours */
        if (strstr(line, "Power_On_Hours")) {
            char *p = line + strlen(line) - 1;
            while (p > line && (*p == '\n' || *p == ' ' || *p == '\t')) *p-- = '\0';
            p = strrchr(line, ' ');
            if (p) stats->power_on_hours = atoi(p + 1);
        }

        /* NVMe Percentage Used */
        if (strstr(line, "Percentage Used:")) {
            sscanf(line, "Percentage Used: %d%%", &stats->percentage_used);
        }

        /* SATA Wear Leveling */
        if (strstr(line, "Wear_Leveling_Count")) {
            char *p = line + strlen(line) - 1;
            while (p > line && (*p == '\n' || *p == ' ' || *p == '\t')) *p-- = '\0';
            p = strrchr(line, ' ');
            if (p) stats->wear_leveling = atoi(p + 1);
        }

        /* Reallocated Sectors */
        if (strstr(line, "Reallocated_Sector_Ct")) {
            char *p = line + strlen(line) - 1;
            while (p > line && (*p == '\n' || *p == ' ' || *p == '\t')) *p-- = '\0';
            p = strrchr(line, ' ');
            if (p) stats->reallocated_sectors = atoi(p + 1);
        }

        /* Pending Sectors */
        if (strstr(line, "Current_Pending_Sector")) {
            char *p = line + strlen(line) - 1;
            while (p > line && (*p == '\n' || *p == ' ' || *p == '\t')) *p-- = '\0';
            p = strrchr(line, ' ');
            if (p) stats->pending_sectors = atoi(p + 1);
        }

        /* SATA Total LBAs Written */
        if (strstr(line, "Total_LBAs_Written")) {
            char *p = line + strlen(line) - 1;
            while (p > line && (*p == '\n' || *p == ' ' || *p == '\t')) *p-- = '\0';
            p = strrchr(line, ' ');
            if (p) stats->total_lbas_written = atoll(p + 1);
        }

        /* SATA Total LBAs Read */
        if (strstr(line, "Total_LBAs_Read")) {
            char *p = line + strlen(line) - 1;
            while (p > line && (*p == '\n' || *p == ' ' || *p == '\t')) *p-- = '\0';
            p = strrchr(line, ' ');
            if (p) stats->total_lbas_read = atoll(p + 1);
        }

        /* NVMe Data Units Read (with commas) */
        if (strstr(line, "Data Units Read:")) {
            long long units;
            char *p = strstr(line, "Data Units Read:");
            p += 17;
            /* Skip spaces, parse number with commas */
            units = 0;
            while (*p && *p != '[') {
                if (isdigit(*p)) units = units * 10 + (*p - '0');
                p++;
            }
            stats->total_lbas_read = units * 512;
        }

        /* NVMe Data Units Written (with commas) */
        if (strstr(line, "Data Units Written:")) {
            long long units;
            char *p = strstr(line, "Data Units Written:");
            p += 20;
            units = 0;
            while (*p && *p != '[') {
                if (isdigit(*p)) units = units * 10 + (*p - '0');
                p++;
            }
            stats->total_lbas_written = units * 512;
        }
    }

    pclose(f);

    strncpy(stats->type, is_nvme ? "NVMe" : "SATA", sizeof(stats->type));
    stats->available = 1;
    return 1;
}

/* ── Network Stats ─────────────────────────────────────────────────── */

typedef struct {
    char name[32];
    long long rx_bytes;
    long long tx_bytes;
    long long rx_packets;
    long long tx_packets;
    long long rx_errors;
    long long tx_errors;
} net_stats_t;

static int read_net_stats(net_stats_t *stats, int max_if)
{
    FILE *f;
    char line[1024];
    int count = 0;

    f = fopen("/proc/net/dev", "r");
    if (!f)
        return 0;

    /* Skip header lines */
    fgets(line, sizeof(line), f);
    fgets(line, sizeof(line), f);

    while (fgets(line, sizeof(line), f) && count < max_if) {
        char *p = strchr(line, ':');
        if (!p) continue;

        *p = '\0';
        char *name = line;
        while (*name == ' ') name++;

        /* Skip loopback */
        if (strcmp(name, "lo") == 0)
            continue;

        strncpy(stats[count].name, name, sizeof(stats[count].name) - 1);

        p++;
        sscanf(p, "%lld %lld %lld 0 0 0 0 0 %lld %lld %lld",
               &stats[count].rx_bytes, &stats[count].rx_packets,
               &stats[count].rx_errors,
               &stats[count].tx_bytes, &stats[count].tx_packets,
               &stats[count].tx_errors);

        count++;
    }

    fclose(f);
    return count;
}

/* ── Fan Speeds (via lm-sensors) ───────────────────────────────────── */

typedef struct {
    char name[64];
    int rpm;
} fan_speed_t;

static int read_fan_speeds(fan_speed_t *fans, int max_fans)
{
    FILE *f;
    char line[1024];
    int count = 0;

    f = popen("sensors -f 2>/dev/null", "r");
    if (!f)
        return 0;

    while (fgets(line, sizeof(line), f) && count < max_fans) {
        char *p = strstr(line, "fan");
        if (!p) continue;

        char name[64];
        int rpm;
        if (sscanf(p, "%63[^:]: %d RPM", name, &rpm) == 2) {
            strncpy(fans[count].name, name, sizeof(fans[count].name) - 1);
            fans[count].rpm = rpm;
            count++;
        }
    }

    pclose(f);
    return count;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int main(void)
{
    thermal_zone_t zones[32];
    int zone_count;
    rapl_reading_t rapl_prev, rapl_curr;
    double rapl_power;
    int cpu_count;
    drive_stats_t drives[MAX_DRIVES];
    char *drive_devices[] = {"sda", "sdc", "nvme0n1", NULL};
    net_stats_t net[MAX_NET_IF];
    int net_count;
    fan_speed_t fans[8];
    int fan_count;
    int i;

    printf("=== Homelab Sensor Report ===\n");
    printf("Timestamp: ");
    fflush(stdout);
    system("date '+%Y-%m-%d %H:%M:%S'");
    printf("\n");

    /* ── CPU Temperatures ── */
    printf("--- CPU Temperatures (Thermal Zones) ---\n");
    zone_count = read_thermal_zones(zones, 32);
    for (i = 0; i < zone_count; i++) {
        if (zones[i].temp_milli > 0)
            printf("  %-20s %5.1f C\n", zones[i].name, zones[i].temp_milli / 1000.0);
    }
    printf("\n");

    /* ── RAPL Power ── */
    printf("--- RAPL Package Power ---\n");
    rapl_prev = read_rapl();
    if (rapl_prev.available) {
        printf("  Energy: %llu uJ (max range: %llu uJ)\n",
               rapl_prev.energy_uj, rapl_prev.max_energy_uj);
        printf("  (Take two readings ~1s apart to compute power)\n");
    } else {
        printf("  RAPL not available (need sudo)\n");
    }
    printf("\n");

    /* ── CPU Frequency ── */
    printf("--- CPU Frequencies ---\n");
    cpu_count = get_cpu_count();
    printf("  Online CPUs: %d\n", cpu_count);
    for (i = 0; i < cpu_count && i < 16; i++) {
        int freq = read_cpu_freq_mhz(i);
        if (freq > 0)
            printf("  CPU%-3d %5d MHz\n", i, freq);
    }
    printf("\n");

    /* ── Drive SMART Stats ── */
    printf("--- Drive SMART Stats ---\n");
    for (i = 0; drive_devices[i]; i++) {
        if (read_drive_stats(drive_devices[i], &drives[i])) {
            printf("  [%s] %s\n", drive_devices[i], drives[i].model[0] ? drives[i].model : "Unknown");
            printf("    Type: %s", drives[i].type);
            if (drives[i].temperature >= 0)
                printf("  Temp: %d C", drives[i].temperature);
            if (drives[i].power_on_hours >= 0)
                printf("  Power: %d hrs", drives[i].power_on_hours);
            if (drives[i].percentage_used > 0)
                printf("  Used: %d%%", drives[i].percentage_used);
            printf("\n");
            if (drives[i].total_lbas_written > 0)
                printf("    Written: %.2f TB  Read: %.2f TB\n",
                       drives[i].total_lbas_written * 512.0 / 1e12,
                       drives[i].total_lbas_read * 512.0 / 1e12);
            if (drives[i].reallocated_sectors > 0 || drives[i].pending_sectors > 0)
                printf("    WARNING: Reallocated: %d  Pending: %d\n",
                       drives[i].reallocated_sectors, drives[i].pending_sectors);
        }
    }
    printf("\n");

    /* ── Fan Speeds ── */
    printf("--- Fan Speeds ---\n");
    fan_count = read_fan_speeds(fans, 8);
    if (fan_count > 0) {
        for (i = 0; i < fan_count; i++)
            printf("  %-10s %d RPM\n", fans[i].name, fans[i].rpm);
    } else {
        printf("  No fan data available\n");
    }
    printf("\n");

    /* ── Network Stats ── */
    printf("--- Network Interfaces ---\n");
    net_count = read_net_stats(net, MAX_NET_IF);
    for (i = 0; i < net_count; i++) {
        printf("  %-16s RX: %7.2f GB  TX: %7.2f GB\n",
               net[i].name,
               net[i].rx_bytes / 1e9,
               net[i].tx_bytes / 1e9);
    }
    printf("\n");

    /* ── RAPL Power Delta ── */
    if (rapl_prev.available) {
        printf("--- RAPL Power Delta ---\n");
        printf("  Reading power (1 second)...\n");
        sleep(1);
        rapl_curr = read_rapl();
        rapl_power = compute_rapl_power(&rapl_prev, &rapl_curr);
        if (rapl_power > 0)
            printf("  Package Power: %.2f W\n", rapl_power);
        else
            printf("  Could not compute power delta\n");
        printf("\n");
    }

    printf("=== End Report ===\n");
    return 0;
}
