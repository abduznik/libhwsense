/*
 * alert.c — Threshold alerts over sensor readings.
 *
 * The decision of whether a reading should fire an alert is kept separate
 * from any hardware access (hwsense_alert_step), so it is unit tested in
 * tests/test_alert.c without a driver or privileges.
 *
 * Two behaviours matter here:
 *
 *   Edge, not level. Firing on every poll while a reading sits above the
 *   threshold buries the caller in duplicates. An alert fires once when a
 *   reading crosses into breach, and again only after it has recovered.
 *
 *   Hysteresis. A reading hovering exactly at the threshold would
 *   otherwise flap between states on sensor noise alone, so recovery
 *   requires dropping a margin below the threshold rather than merely
 *   touching it.
 */

#include "../../include/hwsense_unified.h"
#include <string.h>

void hwsense_alert_init(hwsense_alert_t *alert,
                        double threshold,
                        double hysteresis)
{
    if (!alert)
        return;

    memset(alert, 0, sizeof(*alert));
    alert->threshold = threshold;
    alert->hysteresis = (hysteresis > 0.0) ? hysteresis : 0.0;
    alert->state = HWSENSE_ALERT_OK;
}

int hwsense_alert_step(hwsense_alert_t *alert, double value)
{
    if (!alert)
        return HWSENSE_ALERT_EVENT_NONE;

    alert->last_value = value;

    if (alert->state == HWSENSE_ALERT_OK) {
        /* Strictly above: a reading exactly at the threshold has not
         * exceeded it. */
        if (value > alert->threshold) {
            alert->state = HWSENSE_ALERT_BREACHED;
            alert->breach_count++;
            return HWSENSE_ALERT_EVENT_BREACH;
        }
        return HWSENSE_ALERT_EVENT_NONE;
    }

    /* Breached: recover only after dropping clear of the hysteresis band,
     * so noise around the threshold does not produce repeated events. */
    if (value <= alert->threshold - alert->hysteresis) {
        alert->state = HWSENSE_ALERT_OK;
        return HWSENSE_ALERT_EVENT_RECOVERED;
    }

    return HWSENSE_ALERT_EVENT_NONE;
}

int hwsense_alert_state(const hwsense_alert_t *alert)
{
    return alert ? alert->state : HWSENSE_ALERT_OK;
}

/*
 * Defined out when building the file on its own for tests, where there is
 * no context to read a temperature from. The logic above is what the tests
 * exercise; this is just the glue that feeds it a live reading.
 */
#ifndef HWSENSE_ALERT_LOGIC_ONLY

int hwsense_check_temp_alert(hwsense_ctx_t *ctx,
                             hwsense_alert_t *alert,
                             hwsense_alert_fn callback,
                             void *user_data)
{
    hwsense_temp_result_t temp;
    int event;

    if (!ctx || !alert)
        return HWSENSE_ALERT_EVENT_NONE;

    /* Call the core reader rather than the unified wrapper, which is only
     * built on Windows. */
    temp = hwsense_cpu_package_temp(ctx);

    /* A failed read is not a recovery — leaving the state untouched means
     * a sensor that drops out mid-breach does not silently clear. */
    if (!temp.ok)
        return HWSENSE_ALERT_EVENT_NONE;

    event = hwsense_alert_step(alert, temp.celsius);

    if (event != HWSENSE_ALERT_EVENT_NONE && callback)
        callback(event, temp.celsius, user_data);

    return event;
}

#endif /* HWSENSE_ALERT_LOGIC_ONLY */
