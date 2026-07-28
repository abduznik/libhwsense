#ifndef WIN_SYSSTATS_H
#define WIN_SYSSTATS_H

#include "../../include/hwsense.h"

HWSENSE_API int win_get_mem_stats(win_mem_stats_t *mem);
HWSENSE_API int win_get_cpu_load_sampled(win_cpu_load_t *load);
HWSENSE_API int win_get_disk_stats(win_disk_stats_t *disks, int max_disks);
HWSENSE_API int win_get_disk_io(win_disk_io_t *disks, int max_disks);
HWSENSE_API win_uptime_t win_get_uptime(void);

#endif /* WIN_SYSSTATS_H */
