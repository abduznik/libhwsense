/*
 * win_sysstats.h — Windows system stats API.
 */

#ifndef WIN_SYSSTATS_H
#define WIN_SYSSTATS_H

#define MAX_WIN_DISKS 26
#define MAX_WIN_DISK_IO 8

typedef struct {
    double total_gb;
    double used_gb;
    double free_gb;
    double available_gb;
    double swap_used_gb;
    int percent_used;
} win_mem_stats_t;

typedef struct {
    double percent;
    int num_cpus;
} win_cpu_load_t;

typedef struct {
    char name[64];
    long long total_size_gb;
    long long free_gb;
    int percent_used;
} win_disk_stats_t;

typedef struct {
    char name[64];
    long long read_bytes;
    long long write_bytes;
} win_disk_io_t;

typedef struct {
    int days;
    int hours;
    int minutes;
    int seconds;
} win_uptime_t;

int win_get_mem_stats(win_mem_stats_t *mem);
int win_get_cpu_load(win_cpu_load_t *load);
int win_get_cpu_load_sampled(win_cpu_load_t *load);
int win_get_disk_stats(win_disk_stats_t *disks, int max_disks);
int win_get_disk_io(win_disk_io_t *disks, int max_disks);
win_uptime_t win_get_uptime(void);

#endif /* WIN_SYSSTATS_H */
