/*
 * test_net.c — sanity checks for win_get_net_io.
 *
 * Live adapter query: no driver, no admin, no hardware assumptions. Verifies
 * the struct is populated with sane values and that the cumulative counters
 * do not rewind between two snapshots.
 */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "hwsense.h"

#define MAX_NETS 32

static int check_snapshot(const win_net_io_t *nets, int count)
{
    int failures = 0;

    for (int i = 0; i < count; i++) {
        if (nets[i].name[0] == '\0') {
            printf("FAIL: adapter %d has an empty name\n", i);
            failures++;
        }
        if (nets[i].rx_bytes < 0 || nets[i].tx_bytes < 0) {
            printf("FAIL: %s has a negative byte counter\n", nets[i].name);
            failures++;
        }
        if (nets[i].rx_errors < 0 || nets[i].tx_errors < 0) {
            printf("FAIL: %s has a negative error counter\n", nets[i].name);
            failures++;
        }
        if (nets[i].link_speed_bps < 0) {
            printf("FAIL: %s has a negative link speed\n", nets[i].name);
            failures++;
        }
        if (nets[i].up != 0 && nets[i].up != 1) {
            printf("FAIL: %s has up=%d\n", nets[i].name, nets[i].up);
            failures++;
        }
    }
    return failures;
}

int main(void)
{
    win_net_io_t first[MAX_NETS], second[MAX_NETS];
    int failures = 0;
    int n1 = win_get_net_io(first, MAX_NETS);

    if (n1 <= 0) {
        printf("FAIL: no adapters returned (GetIfTable2 failed)\n");
        return 1;
    }
    failures += check_snapshot(first, n1);

    Sleep(200);

    int n2 = win_get_net_io(second, MAX_NETS);
    failures += check_snapshot(second, n2);

    /*
     * Counters must not rewind over a 200 ms window. Adapters are matched by
     * name because table order is not guaranteed to be stable.
     */
    int matched = 0;
    for (int i = 0; i < n1; i++) {
        for (int j = 0; j < n2; j++) {
            if (strcmp(first[i].name, second[j].name) != 0) continue;
            matched++;
            if (second[j].rx_bytes < first[i].rx_bytes ||
                second[j].tx_bytes < first[i].tx_bytes) {
                printf("FAIL: %s counters went backwards\n", first[i].name);
                failures++;
            }
            break;
        }
    }
    if (matched == 0) {
        printf("FAIL: no adapter name matched between snapshots\n");
        failures++;
    }

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }

    printf("PASS: %d adapters, %d matched between snapshots\n", n1, matched);
    for (int i = 0; i < n1; i++) {
        printf("  %-28s %-4s rx=%lld tx=%lld speed=%lld Mbps\n",
               first[i].name,
               first[i].up ? "up" : "down",
               first[i].rx_bytes,
               first[i].tx_bytes,
               first[i].link_speed_bps / 1000000);
    }
    return 0;
}
