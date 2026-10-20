#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601   /* GetIfTable2 needs Vista or newer */
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
/* ws2ipdef.h defines _WS2IPDEF_, which netioapi.h needs before MIB_IF_ROW2.
   WIN32_LEAN_AND_MEAN keeps the old winsock.h out, so this is safe. */
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include "../../include/hwsense.h"

/* Internal CPU times structure */
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

int win_get_mem_stats(win_mem_stats_t *mem)
{
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return 0;
    mem->total_gb = ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    mem->free_gb = ms.ullAvailPhys / (1024.0 * 1024.0 * 1024.0);
    mem->available_gb = mem->free_gb;
    mem->used_gb = mem->total_gb - mem->available_gb;
    mem->percent_used = ms.dwMemoryLoad;
    mem->swap_used_gb = (ms.ullTotalPageFile - ms.ullAvailPageFile) / (1024.0 * 1024.0 * 1024.0);
    return 1;
}

int win_get_cpu_load(win_cpu_load_t *load)
{
    static cpu_times_t prev = {0};
    static int first_call = 1;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    load->num_cpus = si.dwNumberOfProcessors;
    cpu_times_t curr;
    if (!get_cpu_times(&curr)) return 0;
    if (first_call) { prev = curr; first_call = 0; load->percent = 0.0; return 1; }
    long long idle_delta = filetime_to_100ns(&curr.idle_time) - filetime_to_100ns(&prev.idle_time);
    long long kernel_delta = filetime_to_100ns(&curr.kernel_time) - filetime_to_100ns(&prev.kernel_time);
    long long user_delta = filetime_to_100ns(&curr.user_time) - filetime_to_100ns(&prev.user_time);
    long long total = kernel_delta + user_delta;
    load->percent = (total > 0) ? (100.0 * (1.0 - (double)idle_delta / (double)total)) : 0.0;
    prev = curr;
    return 1;
}

int win_get_cpu_load_sampled(win_cpu_load_t *load)
{
    cpu_times_t start, end;
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    load->num_cpus = si.dwNumberOfProcessors;
    if (!get_cpu_times(&start)) return 0;
    Sleep(1000);
    if (!get_cpu_times(&end)) return 0;
    long long idle_delta = filetime_to_100ns(&end.idle_time) - filetime_to_100ns(&start.idle_time);
    long long kernel_delta = filetime_to_100ns(&end.kernel_time) - filetime_to_100ns(&start.kernel_time);
    long long user_delta = filetime_to_100ns(&end.user_time) - filetime_to_100ns(&start.user_time);
    long long total = kernel_delta + user_delta;
    load->percent = (total > 0) ? (100.0 * (1.0 - (double)idle_delta / (double)total)) : 0.0;
    return 1;
}

int win_get_disk_stats(win_disk_stats_t *disks, int max_disks)
{
    int count = 0;
    char drives[] = "CDEFGHIJKLMNOPQRSTUVWXYZ";
    for (int i = 0; i < (int)strlen(drives) && count < max_disks; i++) {
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

int win_get_disk_io(win_disk_io_t *disks, int max_disks)
{
    int count = 0;
    for (int i = 0; i < max_disks && count < max_disks; i++) {
        char device_path[64];
        HANDLE hDev;
        DWORD bytes_returned;
        DISK_PERFORMANCE perf;
        snprintf(device_path, sizeof(device_path), "\\\\.\\PhysicalDrive%d", i);
        hDev = CreateFileA(device_path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (hDev == INVALID_HANDLE_VALUE) continue;
        if (DeviceIoControl(hDev, IOCTL_DISK_PERFORMANCE, NULL, 0, &perf, sizeof(perf), &bytes_returned, NULL)) {
            snprintf(disks[count].name, sizeof(disks[count].name), "PhysicalDrive%d", i);
            disks[count].read_bytes = perf.BytesRead.QuadPart;
            disks[count].write_bytes = perf.BytesWritten.QuadPart;
            count++;
        }
        CloseHandle(hDev);
    }
    return count;
}

/*
 * Convert a UTF-16 Windows string to UTF-8. Writes an empty string on failure
 * so callers can supply their own fallback.
 */
static void wcs_to_utf8(const WCHAR *src, char *out, int out_size)
{
    out[0] = '\0';
    WideCharToMultiByte(CP_UTF8, 0, src, -1, out, out_size, NULL, NULL);
}

/*
 * MIB_IF_ROW2 carries the adapter name as UTF-16. Convert to UTF-8 here so
 * callers do not have to care; fall back to the interface index if both the
 * alias and the description are empty.
 */
static void net_row_name(const MIB_IF_ROW2 *row, char *out, int out_size)
{
    const WCHAR *src = row->Alias[0] ? row->Alias : row->Description;

    wcs_to_utf8(src, out, out_size);
    if (out[0] == '\0')
        snprintf(out, (size_t)out_size, "adapter%lu", (unsigned long)row->InterfaceIndex);
}

int win_get_net_io(win_net_io_t *nets, int max_nets)
{
    MIB_IF_TABLE2 *table = NULL;
    int count = 0;

    if (!nets || max_nets <= 0) return 0;
    if (GetIfTable2(&table) != NO_ERROR || !table) return 0;

    for (ULONG i = 0; i < table->NumEntries && count < max_nets; i++) {
        const MIB_IF_ROW2 *row = &table->Table[i];
        win_net_io_t *out = &nets[count];

        /* Loopback has no useful throughput; the counters just mirror traffic.
           Filter interfaces (WFP, QoS, ...) mirror their parent's counters, so
           reporting them would double-count every byte. */
        if (row->Type == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (row->InterfaceAndOperStatusFlags.FilterInterface) continue;

        net_row_name(row, out->name, (int)sizeof(out->name));
        out->rx_bytes = (long long)row->InOctets;
        out->tx_bytes = (long long)row->OutOctets;
        out->rx_errors = (long long)row->InErrors;
        out->tx_errors = (long long)row->OutErrors;
        out->link_speed_bps = (long long)row->ReceiveLinkSpeed;
        out->up = (row->OperStatus == IfOperStatusUp) ? 1 : 0;
        count++;
    }

    FreeMibTable(table);
    return count;
}

/* Descending sort; qsort wants an int, not a double. */
static int cmp_by_memory(const void *a, const void *b)
{
    double da = ((const win_process_t *)a)->working_set_mb;
    double db = ((const win_process_t *)b)->working_set_mb;
    return (da < db) - (da > db);
}

static int cmp_by_cpu(const void *a, const void *b)
{
    double da = ((const win_process_t *)a)->cpu_seconds;
    double db = ((const win_process_t *)b)->cpu_seconds;
    return (da < db) - (da > db);
}

/*
 * Collect every process, sort, then hand back the top max_procs. The buffer
 * grows on demand because the process count is not known up front, and there
 * is no upper bound worth hardcoding.
 *
 * Protected processes cannot be opened; they are still listed with zeroed
 * counters rather than dropped, so a missing entry is not mistaken for an
 * absent process.
 */
int win_get_top_processes(win_process_t *procs, int max_procs, int sort_by)
{
    HANDLE snap;
    PROCESSENTRY32W entry;
    win_process_t *all = NULL;
    size_t count = 0, capacity = 0;
    int out_count;

    if (!procs || max_procs <= 0) return 0;

    snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;

    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
        do {
            win_process_t row;
            HANDLE h;

            memset(&row, 0, sizeof(row));
            row.pid = entry.th32ProcessID;
            wcs_to_utf8(entry.szExeFile, row.name, (int)sizeof(row.name));

            h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, entry.th32ProcessID);
            if (h) {
                FILETIME created, exited, kernel, user;
                PROCESS_MEMORY_COUNTERS mem;

                if (GetProcessTimes(h, &created, &exited, &kernel, &user)) {
                    ULARGE_INTEGER k, u;
                    k.LowPart = kernel.dwLowDateTime;  k.HighPart = kernel.dwHighDateTime;
                    u.LowPart = user.dwLowDateTime;    u.HighPart = user.dwHighDateTime;
                    row.cpu_seconds = (double)(k.QuadPart + u.QuadPart) / 10000000.0;
                }
                if (GetProcessMemoryInfo(h, &mem, sizeof(mem)))
                    row.working_set_mb = (double)mem.WorkingSetSize / (1024.0 * 1024.0);
                CloseHandle(h);
            }

            if (count == capacity) {
                size_t grown_capacity = capacity ? capacity * 2 : 128;
                win_process_t *grown = realloc(all, grown_capacity * sizeof(*all));
                if (!grown) break;
                all = grown;
                capacity = grown_capacity;
            }
            all[count++] = row;
        } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);

    if (count == 0) {
        free(all);
        return 0;
    }

    qsort(all, count, sizeof(*all),
          sort_by == HWSENSE_SORT_CPU ? cmp_by_cpu : cmp_by_memory);

    out_count = (count < (size_t)max_procs) ? (int)count : max_procs;
    memcpy(procs, all, (size_t)out_count * sizeof(*all));
    free(all);
    return out_count;
}

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
