/*
 * test_processes.c — sanity checks for win_get_top_processes.
 *
 * Live query, no driver or admin needed (some processes are unreadable and
 * come back zeroed, which is why the checks are structural).
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hwsense.h"

#define MAX_PROCS 32

static int check_rows(const win_process_t *procs, int count, int sort_by)
{
    int failures = 0;

    if (count <= 0) {
        printf("FAIL: no processes returned\n");
        return 1;
    }
    if (count > MAX_PROCS) {
        printf("FAIL: %d processes returned, asked for at most %d\n", count, MAX_PROCS);
        failures++;
    }

    for (int i = 0; i < count; i++) {
        if (procs[i].pid == 0) {
            printf("FAIL: row %d has pid 0\n", i);
            failures++;
        }
        if (procs[i].name[0] == '\0') {
            printf("FAIL: pid %lu has an empty name\n", procs[i].pid);
            failures++;
        }
        if (procs[i].cpu_seconds < 0.0 || procs[i].working_set_mb < 0.0) {
            printf("FAIL: %s has a negative counter\n", procs[i].name);
            failures++;
        }
        if (i > 0) {
            double prev = sort_by == HWSENSE_SORT_CPU ? procs[i - 1].cpu_seconds
                                                      : procs[i - 1].working_set_mb;
            double curr = sort_by == HWSENSE_SORT_CPU ? procs[i].cpu_seconds
                                                      : procs[i].working_set_mb;
            if (curr > prev) {
                printf("FAIL: not sorted at row %d (%s %.2f after %.2f)\n",
                       i, procs[i].name, curr, prev);
                failures++;
            }
        }
    }
    return failures;
}

int main(void)
{
    win_process_t procs[MAX_PROCS];
    int failures = 0;
    int n;

    n = win_get_top_processes(procs, MAX_PROCS, HWSENSE_SORT_MEMORY);
    failures += check_rows(procs, n, HWSENSE_SORT_MEMORY);

    printf("Top %d by working set:\n", n);
    for (int i = 0; i < n && i < 5; i++)
        printf("  %-28s pid=%-6lu %8.1f MB  cpu=%.1fs\n",
               procs[i].name, procs[i].pid, procs[i].working_set_mb, procs[i].cpu_seconds);

    n = win_get_top_processes(procs, MAX_PROCS, HWSENSE_SORT_CPU);
    failures += check_rows(procs, n, HWSENSE_SORT_CPU);

    /* Both orderings must see the same process table. */
    if (n < 2) {
        printf("FAIL: expected several processes, got %d\n", n);
        failures++;
    }

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: %d processes in both sort orders\n", n);
    return 0;
}
