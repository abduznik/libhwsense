/*
 * win_sysstats.c — Windows system stats: memory, CPU load, disk usage.
 *
 * Uses Win32 APIs: GlobalMemoryStatusEx, GetSystemTimes, GetDiskFreeSpaceEx.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

/* ── Memory Stats ──────────────────────────────────────────────────── */

typedef struct {
    double total_gb;
    double used_gb;
    double free_gb;
    double available_gb;
    double swap_used_gb;
    int percent_used;
} win_mem_stats_t;

int win_get_mem_stats(win_mem_stats_t *mem)
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);

    if (!GlobalMemoryStatusEx(&ms))
        return 0;

    mem->total_gb = ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    mem->free_gb = ms.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
    mem->available_gb = mem->free_gb;
    mem->used_gb = mem->total_gb - mem->available_gb;
    mem->percent_used = ms.dwMemoryLoad;

    mem->swap_used_gb = (ms.ullTotalPageFile - ms.ullAvailPageFile) / (1024.0 * 1024.0 * 1024.0);

    return 1;
}

/* ── CPU Load ──────────────────────────────────────────────────────── */

typedef struct {
    FILETIME idle_time;
    FILETIME kernel_time;
    FILETIME user_time;
} cpu_times_t;

static int get_cpu_times(cpu_times_t *times)
{
    return GetSystemTimes(&times->idle_time, &times->kernel_time, &times->user_time);
}

static long long filetime_to_100ns(const FILETIME *ft)
{
    ULARGE_INTEGER ul;
    ul.LowPart = ft->dwLowDateTime;
    ul.HighPart = ft->dwHighDateTime;
    return (long long)ul.QuadPart;
}

typedef struct {
    double percent;
    int num_cpus;
} win_cpu_load_t;

int win_get_cpu_load(win_cpu_load_t *load)
{
    static cpu_times_t prev = {0};
    static int first_call = 1;

    SYSTEM_INFO si;
    GetSystemInfo(&si);
    load->num_cpus = si.dwNumberOfProcessors;

    cpu_times_t curr;
    if (!get_cpu_times(&curr))
        return 0;

    if (first_call) {
        prev = curr;
        first_call = 0;
        load->percent = 0.0;
        return 1;
    }

    long long idle_delta = filetime_to_100ns(&curr.idle_time) - filetime_to_100ns(&prev.idle_time);
    long long kernel_delta = filetime_to_100ns(&curr.kernel_time) - filetime_to_100ns(&prev.kernel_time);
    long long user_delta = filetime_to_100ns(&curr.user_time) - filetime_to_100ns(&prev.user_time);

    long long total = kernel_delta + user_delta;
    if (total > 0)
        load->percent = 100.0 * (1.0 - (double)idle_delta / (double)total);
    else
        load->percent = 0.0;

    prev = curr;
    return 1;
}

/* ── Disk Stats ────────────────────────────────────────────────────── */

typedef struct {
    char name[64];
    long long total_size_gb;
    long long free_gb;
    int percent_used;
} win_disk_stats_t;

int win_get_disk_stats(win_disk_stats_t *disks, int max_disks)
{
    int count = 0;
    char drives[] = "CDEFGHIJKLMNOPQRSTUVWXYZ";
    int i;

    for (i = 0; i < (int)strlen(drives) && count < max_disks; i++) {
        char root[4];
        ULARGE_INTEGER free_bytes, total_bytes;
        snprintf(root, sizeof(root), "%c:\\", drives[i]);

        if (GetDiskFreeSpaceExA(root, NULL, &total_bytes, &free_bytes)) {
            disks[count].total_size_gb = total_bytes.QuadPart / (1024ULL * 1024 * 1024);
            disks[count].free_gb = free_bytes.QuadPart / (1024ULL * 1024 * 1024);
            disks[count].percent_used = (int)(100.0 * (1.0 - (double)free_bytes.QuadPart / (double)total_bytes.QuadPart));
            snprintf(disks[count].name, sizeof(disks[count].name), "%c:", drives[i]);
            count++;
        }
    }

    return count;
}

/* ── Disk I/O Stats ────────────────────────────────────────────────── */

#define MAX_WIN_DISK_IO 8

typedef struct {
    char name[64];
    long long read_bytes;
    long long write_bytes;
} win_disk_io_t;

int win_get_disk_io(win_disk_io_t *disks, int max_disks)
{
    int count = 0;
    int i;

    for (i = 0; i < max_disks && count < max_disks; i++) {
        char device_path[64];
        HANDLE hDev;
        DWORD bytes_returned;
        DISK_PERFORMANCE perf;

        snprintf(device_path, sizeof(device_path), "\\\\.\\PhysicalDrive%d", i);

        hDev = CreateFileA(
            device_path,
            0,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL,
            OPEN_EXISTING,
            0,
            NULL
        );

        if (hDev == INVALID_HANDLE_VALUE)
            continue;

        if (DeviceIoControl(hDev, IOCTL_DISK_PERFORMANCE, NULL, 0,
                           &perf, sizeof(perf), &bytes_returned, NULL)) {
            snprintf(disks[count].name, sizeof(disks[count].name), "PhysicalDrive%d", i);
            disks[count].read_bytes = perf.BytesRead.QuadPart;
            disks[count].write_bytes = perf.BytesWritten.QuadPart;
            count++;
        }

        CloseHandle(hDev);
    }

    return count;
}

/* ── Uptime ────────────────────────────────────────────────────────── */

typedef struct {
    int days;
    int hours;
    int minutes;
    int seconds;
} win_uptime_t;

win_uptime_t win_get_uptime(void)
{
    win_uptime_t uptime = {0};
    DWORD ticks = GetTickCount64() / 1000;

    uptime.seconds = ticks % 60;
    ticks /= 60;
    uptime.minutes = ticks % 60;
    ticks /= 60;
    uptime.hours = ticks % 24;
    uptime.days = ticks / 24;

    return uptime;
}
