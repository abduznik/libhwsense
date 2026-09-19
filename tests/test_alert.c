/*
 * test_alert.c — Unit tests for threshold alert logic.
 *
 * hwsense_alert_step takes a reading rather than fetching one, so all of
 * this runs without hardware or privileges.
 */

#include "../include/hwsense_unified.h"
#include <stdio.h>
#include <string.h>

static int failures = 0;
static int checks = 0;

static const char *event_name(int e)
{
    switch (e) {
    case HWSENSE_ALERT_EVENT_NONE:      return "NONE";
    case HWSENSE_ALERT_EVENT_BREACH:    return "BREACH";
    case HWSENSE_ALERT_EVENT_RECOVERED: return "RECOVERED";
    default:                            return "?";
    }
}

static void check_event(const char *what, int got, int want)
{
    checks++;
    if (got != want) {
        printf("FAIL %s: got %s, want %s\n", what, event_name(got), event_name(want));
        failures++;
    }
}

static void check_int(const char *what, int got, int want)
{
    checks++;
    if (got != want) {
        printf("FAIL %s: got %d, want %d\n", what, got, want);
        failures++;
    }
}

static void check(const char *what, int cond)
{
    checks++;
    if (!cond) {
        printf("FAIL %s\n", what);
        failures++;
    }
}

static void test_init(void)
{
    hwsense_alert_t a;

    hwsense_alert_init(&a, 90.0, 5.0);
    check("init state is OK", hwsense_alert_state(&a) == HWSENSE_ALERT_OK);
    check_int("init breach count", a.breach_count, 0);

    /* A negative hysteresis would invert the recovery test, so it is
     * clamped rather than trusted. */
    hwsense_alert_init(&a, 90.0, -5.0);
    check("negative hysteresis clamped to 0", a.hysteresis == 0.0);
}

static void test_edge_triggered(void)
{
    hwsense_alert_t a;
    hwsense_alert_init(&a, 90.0, 0.0);

    check_event("below threshold", hwsense_alert_step(&a, 50.0),
                HWSENSE_ALERT_EVENT_NONE);

    check_event("crossing fires breach", hwsense_alert_step(&a, 95.0),
                HWSENSE_ALERT_EVENT_BREACH);

    /* The point of edge-triggering: staying above must stay quiet. */
    check_event("staying above is quiet (1)", hwsense_alert_step(&a, 96.0),
                HWSENSE_ALERT_EVENT_NONE);
    check_event("staying above is quiet (2)", hwsense_alert_step(&a, 99.0),
                HWSENSE_ALERT_EVENT_NONE);
    check_event("staying above is quiet (3)", hwsense_alert_step(&a, 91.0),
                HWSENSE_ALERT_EVENT_NONE);

    check_event("dropping fires recovery", hwsense_alert_step(&a, 80.0),
                HWSENSE_ALERT_EVENT_RECOVERED);

    check_event("staying below is quiet", hwsense_alert_step(&a, 70.0),
                HWSENSE_ALERT_EVENT_NONE);

    check_event("re-crossing fires again", hwsense_alert_step(&a, 95.0),
                HWSENSE_ALERT_EVENT_BREACH);

    check_int("breach count counts both", a.breach_count, 2);
}

static void test_threshold_boundary(void)
{
    hwsense_alert_t a;
    hwsense_alert_init(&a, 90.0, 0.0);

    /* Exactly at the threshold has not exceeded it. */
    check_event("exactly at threshold does not breach",
                hwsense_alert_step(&a, 90.0), HWSENSE_ALERT_EVENT_NONE);

    /* A hair above does. */
    check_event("just above threshold breaches",
                hwsense_alert_step(&a, 90.001), HWSENSE_ALERT_EVENT_BREACH);

    /* With zero hysteresis, returning to exactly the threshold recovers. */
    check_event("back to threshold recovers with no hysteresis",
                hwsense_alert_step(&a, 90.0), HWSENSE_ALERT_EVENT_RECOVERED);
}

static void test_hysteresis(void)
{
    hwsense_alert_t a;
    hwsense_alert_init(&a, 90.0, 5.0);

    check_event("breach", hwsense_alert_step(&a, 95.0),
                HWSENSE_ALERT_EVENT_BREACH);

    /* Inside the hysteresis band: below the threshold, but not far enough
     * to count as recovered. This is the flapping case. */
    check_event("inside band does not recover (89)",
                hwsense_alert_step(&a, 89.0), HWSENSE_ALERT_EVENT_NONE);
    check_event("inside band does not recover (86)",
                hwsense_alert_step(&a, 86.0), HWSENSE_ALERT_EVENT_NONE);

    /* Exactly at the recovery point (threshold - hysteresis) recovers. */
    check_event("at recovery point recovers",
                hwsense_alert_step(&a, 85.0), HWSENSE_ALERT_EVENT_RECOVERED);
}

static void test_no_flapping(void)
{
    hwsense_alert_t a;
    int i;
    int events = 0;

    hwsense_alert_init(&a, 90.0, 5.0);
    hwsense_alert_step(&a, 95.0);   /* breach */

    /* A reading oscillating either side of the threshold, as a real sensor
     * does, must not produce an event on every sample. */
    for (i = 0; i < 20; i++) {
        double v = (i % 2) ? 89.5 : 90.5;
        if (hwsense_alert_step(&a, v) != HWSENSE_ALERT_EVENT_NONE)
            events++;
    }

    check_int("noise around threshold produces no events", events, 0);
    check("still breached after noise",
          hwsense_alert_state(&a) == HWSENSE_ALERT_BREACHED);
}

static void test_first_reading_above(void)
{
    hwsense_alert_t a;
    hwsense_alert_init(&a, 90.0, 5.0);

    /* Starting already hot must report the breach, not assume a prior
     * state. */
    check_event("first reading above fires",
                hwsense_alert_step(&a, 99.0), HWSENSE_ALERT_EVENT_BREACH);
}

static void test_last_value(void)
{
    hwsense_alert_t a;
    hwsense_alert_init(&a, 90.0, 0.0);

    hwsense_alert_step(&a, 42.5);
    check("last_value records reading", a.last_value == 42.5);

    hwsense_alert_step(&a, 95.5);
    check("last_value updates on breach", a.last_value == 95.5);
}

static void test_null_safety(void)
{
    check_event("null alert returns NONE",
                hwsense_alert_step(NULL, 100.0), HWSENSE_ALERT_EVENT_NONE);
    check_int("null state reports OK",
              hwsense_alert_state(NULL), HWSENSE_ALERT_OK);

    /* Must not crash. */
    hwsense_alert_init(NULL, 90.0, 5.0);
    checks++;
}

/* --- callback plumbing --- */

typedef struct {
    int calls;
    int last_event;
    double last_value;
    void *seen_user_data;
} cb_record_t;

static void record_cb(int event, double value, void *user_data)
{
    cb_record_t *r = (cb_record_t *)user_data;
    r->calls++;
    r->last_event = event;
    r->last_value = value;
    r->seen_user_data = user_data;
}

static void test_callback_contract(void)
{
    /*
     * hwsense_check_temp_alert needs a live context, so drive the callback
     * the same way it does to confirm the plumbing: fire only on a state
     * change, and pass user_data through untouched.
     */
    hwsense_alert_t a;
    cb_record_t rec;
    int event;

    memset(&rec, 0, sizeof(rec));
    hwsense_alert_init(&a, 90.0, 5.0);

    event = hwsense_alert_step(&a, 50.0);
    if (event != HWSENSE_ALERT_EVENT_NONE)
        record_cb(event, 50.0, &rec);
    check_int("no callback when nothing changed", rec.calls, 0);

    event = hwsense_alert_step(&a, 95.0);
    if (event != HWSENSE_ALERT_EVENT_NONE)
        record_cb(event, 95.0, &rec);
    check_int("callback fired on breach", rec.calls, 1);
    check_event("callback got breach event", rec.last_event,
                HWSENSE_ALERT_EVENT_BREACH);
    check("callback got the value", rec.last_value == 95.0);
    check("user_data passed through", rec.seen_user_data == &rec);

    event = hwsense_alert_step(&a, 96.0);
    if (event != HWSENSE_ALERT_EVENT_NONE)
        record_cb(event, 96.0, &rec);
    check_int("no second callback while still breached", rec.calls, 1);
}

int main(void)
{
    test_init();
    test_edge_triggered();
    test_threshold_boundary();
    test_hysteresis();
    test_no_flapping();
    test_first_reading_above();
    test_last_value();
    test_null_safety();
    test_callback_contract();

    printf("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
