/*
 * Host tests for using a handlebar button as an input.
 *
 * vehicle_button.c is pure, so the button table, the press detector and the
 * hold detector are all covered here. The vehicle_state read that feeds it
 * lives in display.c and is on-target only.
 *
 * Note the semantics under test: a short press is reported on RELEASE, so that
 * a long hold cannot report both a short and a long press.
 */
#include <stdio.h>
#include <string.h>

#include "s2_dbc_gen.h"
#include "unity.h"
#include "vehicle_button.h"

void setUp(void) {}
void tearDown(void) {}

/* Released and held raw values for the info/scroll button. */
#define UP 0x00
#define DOWN 0x01

#define LONG_US 2000000LL

static const vbtn_def_t *info_def(void)
{
    return vbtn_get(VBTN_INFO_SCROLL);
}

/* Drives the detector the way the decoder and the render loop would. */
typedef struct {
    vbtn_state_t st;
    uint32_t changes;
    uint64_t raw;
    int64_t now;
} sim_t;

/* Set the raw value, bumping the change counter only when it actually changes. */
static void sim_set(sim_t *s, uint64_t raw)
{
    if (raw != s->raw) {
        s->raw = raw;
        s->changes++;
    }
}

static vbtn_event_t sim_poll_after(sim_t *s, int64_t advance_us)
{
    s->now += advance_us;
    return vbtn_feed(&s->st, info_def(), true, s->raw, s->changes, s->now, LONG_US);
}

/* One poll, 10 ms later, which is the real render-loop tick. */
static vbtn_event_t sim_poll(sim_t *s)
{
    return sim_poll_after(s, 10000);
}

/* Start released and armed, which is the normal steady state. */
static void sim_start(sim_t *s)
{
    memset(s, 0, sizeof(*s));
    sim_set(s, UP);
    vbtn_event_t ev = sim_poll(s);
    TEST_ASSERT_EQUAL_UINT(0, ev.presses);
    TEST_ASSERT_FALSE(ev.long_press);
}

/* ---------------------------------------------------------------- table --- */

static void test_every_button_is_defined(void)
{
    for (int i = 0; i < VBTN__COUNT; i++) {
        const vbtn_def_t *d = vbtn_get((vbtn_id_t)i);
        TEST_ASSERT_NOT_NULL(d);
        TEST_ASSERT_NOT_NULL(d->label);
        TEST_ASSERT_NOT_NULL(d->note);
        TEST_ASSERT_TRUE(d->label[0] != '\0');
        /* The signal must exist in the database. */
        TEST_ASSERT_LESS_THAN_UINT(S2_SIG__COUNT, d->signal);
        TEST_ASSERT_NOT_NULL(s2_dbc_signal(d->signal));
        /* A mask of zero would make every value read as pressed. */
        TEST_ASSERT_NOT_EQUAL_UINT64(0, d->mask);
        /* The held value has to be reachable through the mask. */
        TEST_ASSERT_EQUAL_UINT64(d->pressed, d->pressed & d->mask);
    }
}

static void test_out_of_range_ids(void)
{
    TEST_ASSERT_NULL(vbtn_get(VBTN__COUNT));
    TEST_ASSERT_NULL(vbtn_get((vbtn_id_t)99));
}

static void test_the_default_is_the_info_button(void)
{
    const vbtn_def_t *sel = vbtn_selected();
    TEST_ASSERT_NOT_NULL(sel);
    TEST_ASSERT_EQUAL_PTR(vbtn_get(VBTN_INFO_SCROLL), sel);
    TEST_ASSERT_EQUAL_UINT16(S2_SIG_LEFT_SWITCH_354_info_scroll_button, sel->signal);
}

/* Feed released -> held -> released for an arbitrary button, expect one press. */
static unsigned cycle(const vbtn_def_t *def, uint64_t up, uint64_t down)
{
    vbtn_state_t st;
    memset(&st, 0, sizeof(st));
    uint32_t c = 0;
    int64_t t = 0;
    vbtn_feed(&st, def, true, up, c, t += 10000, LONG_US);
    vbtn_feed(&st, def, true, down, ++c, t += 10000, LONG_US);
    return vbtn_feed(&st, def, true, up, ++c, t += 10000, LONG_US).presses;
}

/*
 * The masked controls are the reason a plain "raw != 0" test would not do: the
 * cruise arm bit shares a byte whose other bits move on their own.
 */
static void test_masked_controls_ignore_the_rest_of_the_byte(void)
{
    /* 0x09 and 0x05 are the documented base values with cruise disarmed. */
    TEST_ASSERT_EQUAL_UINT(1, cycle(vbtn_get(VBTN_CRUISE), 0x09, 0x89));
    TEST_ASSERT_EQUAL_UINT(1, cycle(vbtn_get(VBTN_CRUISE), 0x05, 0x85));
    /* The front brake is 0x40 in a byte that is otherwise 0x00. */
    TEST_ASSERT_EQUAL_UINT(1, cycle(vbtn_get(VBTN_FRONT_BRAKE), 0x00, 0x40));
    TEST_ASSERT_EQUAL_UINT(1, cycle(vbtn_get(VBTN_REAR_BRAKE), 0x00, 0x10));
    TEST_ASSERT_EQUAL_UINT(1, cycle(vbtn_get(VBTN_HIGHBEAM), 0x00, 0x04));

    /* A value that shares no masked bit must not read as pressed. */
    vbtn_state_t st;
    memset(&st, 0, sizeof(st));
    const vbtn_def_t *hb = vbtn_get(VBTN_HIGHBEAM);
    int64_t t = 0;
    vbtn_feed(&st, hb, true, 0x00, 0, t += 10000, LONG_US);
    /* 0x01 is not 0x04: not a press, so releasing to 0x00 completes nothing. */
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, hb, true, 0x01, 1, t += 10000, LONG_US).presses);
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, hb, true, 0x00, 2, t += 10000, LONG_US).presses);

    /* The cruise base byte moving on its own is not a press either. */
    memset(&st, 0, sizeof(st));
    const vbtn_def_t *cr = vbtn_get(VBTN_CRUISE);
    t = 0;
    vbtn_feed(&st, cr, true, 0x09, 0, t += 10000, LONG_US);
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, cr, true, 0x05, 1, t += 10000, LONG_US).presses);
}

/* ------------------------------------------------ short press detection --- */

static void test_a_press_reports_on_release(void)
{
    sim_t s;
    sim_start(&s);

    sim_set(&s, DOWN);
    vbtn_event_t ev = sim_poll(&s);
    TEST_ASSERT_EQUAL_UINT(0, ev.presses);      /* not yet: still held */
    TEST_ASSERT_FALSE(ev.long_press);

    sim_set(&s, UP);
    ev = sim_poll(&s);
    TEST_ASSERT_EQUAL_UINT(1, ev.presses);      /* released: one press */
    TEST_ASSERT_FALSE(ev.long_press);

    TEST_ASSERT_EQUAL_UINT(0, sim_poll(&s).presses);
}

static void test_holding_does_not_repeat(void)
{
    sim_t s;
    sim_start(&s);
    sim_set(&s, DOWN);
    /* Hold for well under the long-press threshold, polled many times. */
    for (int i = 0; i < 50; i++) {
        vbtn_event_t ev = vbtn_feed(&s.st, info_def(), true, s.raw, s.changes,
                                    s.now += 10000, 0 /* long press disabled */);
        TEST_ASSERT_EQUAL_UINT(0, ev.presses);
        TEST_ASSERT_FALSE(ev.long_press);
    }
    sim_set(&s, UP);
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&s.st, info_def(), true, s.raw, s.changes,
                                        s.now += 10000, 0).presses);
}

static void test_a_press_that_fell_between_polls_is_not_lost(void)
{
    sim_t s;
    sim_start(&s);
    /* Pressed and released with no poll in between: two changes, same level. */
    sim_set(&s, DOWN);
    sim_set(&s, UP);
    TEST_ASSERT_EQUAL_UINT(1, sim_poll(&s).presses);
}

static void test_several_missed_presses_are_all_counted(void)
{
    sim_t s;
    sim_start(&s);
    for (int i = 0; i < 3; i++) {
        sim_set(&s, DOWN);
        sim_set(&s, UP);
    }
    TEST_ASSERT_EQUAL_UINT(3, sim_poll(&s).presses);

    /* Ending held: the trailing press is not complete, so it is not counted yet. */
    for (int i = 0; i < 2; i++) {
        sim_set(&s, DOWN);
        sim_set(&s, UP);
    }
    sim_set(&s, DOWN);
    TEST_ASSERT_EQUAL_UINT(2, sim_poll(&s).presses);
    sim_set(&s, UP);
    TEST_ASSERT_EQUAL_UINT(1, sim_poll(&s).presses);
}

static void test_a_button_already_held_at_startup_reports_nothing(void)
{
    sim_t s;
    memset(&s, 0, sizeof(s));
    sim_set(&s, DOWN);                           /* held before the first poll */

    /* Hold it well past the long-press threshold: still nothing. */
    for (int i = 0; i < 500; i++) {
        vbtn_event_t ev = sim_poll(&s);
        TEST_ASSERT_EQUAL_UINT(0, ev.presses);
        TEST_ASSERT_FALSE(ev.long_press);
    }
    /* Releasing that initial hold reports nothing either. */
    sim_set(&s, UP);
    vbtn_event_t ev = sim_poll(&s);
    TEST_ASSERT_EQUAL_UINT(0, ev.presses);
    TEST_ASSERT_FALSE(ev.long_press);

    /* A genuine press afterwards works normally. */
    sim_set(&s, DOWN);
    sim_poll(&s);
    sim_set(&s, UP);
    TEST_ASSERT_EQUAL_UINT(1, sim_poll(&s).presses);
}

/* ------------------------------------------------- hold (long) detection --- */

static void test_a_long_hold_fires_once_and_suppresses_the_short_press(void)
{
    sim_t s;
    sim_start(&s);
    sim_set(&s, DOWN);

    /* Poll every 10 ms; nothing until the threshold. */
    int64_t held = 0;
    bool fired = false;
    for (int i = 0; i < 400 && !fired; i++) {
        vbtn_event_t ev = sim_poll(&s);
        held += 10000;
        TEST_ASSERT_EQUAL_UINT(0, ev.presses);
        if (ev.long_press) {
            fired = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(fired, "long press never fired");
    TEST_ASSERT_GREATER_OR_EQUAL_INT64(LONG_US, held);

    /* It must not fire again while the hold continues. */
    for (int i = 0; i < 200; i++) {
        vbtn_event_t ev = sim_poll(&s);
        TEST_ASSERT_FALSE(ev.long_press);
        TEST_ASSERT_EQUAL_UINT(0, ev.presses);
    }

    /* And the release must NOT also report a short press. */
    sim_set(&s, UP);
    vbtn_event_t ev = sim_poll(&s);
    TEST_ASSERT_EQUAL_UINT(0, ev.presses);
    TEST_ASSERT_FALSE(ev.long_press);
}

static void test_a_hold_just_under_the_threshold_is_a_short_press(void)
{
    sim_t s;
    sim_start(&s);
    sim_set(&s, DOWN);
    /* One poll 10 us short of the threshold. */
    vbtn_event_t ev = sim_poll_after(&s, LONG_US - 10);
    TEST_ASSERT_FALSE(ev.long_press);
    TEST_ASSERT_EQUAL_UINT(0, ev.presses);

    sim_set(&s, UP);
    ev = sim_poll(&s);
    TEST_ASSERT_EQUAL_UINT(1, ev.presses);
    TEST_ASSERT_FALSE(ev.long_press);
}

static void test_two_holds_in_a_row_each_fire(void)
{
    sim_t s;
    sim_start(&s);
    for (int n = 0; n < 2; n++) {
        sim_set(&s, DOWN);
        /* The hold timer starts when the press is first observed, so the poll
         * that sees it cannot also cross the threshold. */
        vbtn_event_t ev = sim_poll(&s);
        TEST_ASSERT_FALSE(ev.long_press);
        ev = sim_poll_after(&s, LONG_US);
        TEST_ASSERT_TRUE_MESSAGE(ev.long_press, "hold did not fire");
        TEST_ASSERT_EQUAL_UINT(0, ev.presses);
        sim_set(&s, UP);
        ev = sim_poll(&s);
        TEST_ASSERT_EQUAL_UINT(0, ev.presses);
    }
}

/*
 * A release and re-press that both fell between two polls must restart the hold
 * timer, or the new hold would inherit the old one's start time and fire
 * immediately.
 */
static void test_a_re_press_between_polls_restarts_the_hold_timer(void)
{
    sim_t s;
    sim_start(&s);
    sim_set(&s, DOWN);
    sim_poll(&s);                                /* press observed, timer starts */
    vbtn_event_t ev = sim_poll_after(&s, LONG_US);
    TEST_ASSERT_TRUE(ev.long_press);

    /* Released and pressed again, both between polls. */
    sim_set(&s, UP);
    sim_set(&s, DOWN);
    ev = sim_poll(&s);
    TEST_ASSERT_EQUAL_UINT(0, ev.presses);       /* the long hold's release is suppressed */
    TEST_ASSERT_FALSE_MESSAGE(ev.long_press, "new hold fired immediately");

    /* The new hold fires only after its own threshold, measured from the poll
     * that observed it. */
    ev = sim_poll_after(&s, LONG_US);
    TEST_ASSERT_TRUE(ev.long_press);
}

static void test_long_press_can_be_disabled(void)
{
    sim_t s;
    memset(&s, 0, sizeof(s));
    sim_set(&s, UP);
    vbtn_feed(&s.st, info_def(), true, s.raw, s.changes, s.now += 10000, 0);
    sim_set(&s, DOWN);
    for (int i = 0; i < 500; i++) {
        vbtn_event_t ev = vbtn_feed(&s.st, info_def(), true, s.raw, s.changes,
                                    s.now += 10000, 0);
        TEST_ASSERT_FALSE(ev.long_press);
    }
    /* With long press off, the release is an ordinary short press. */
    sim_set(&s, UP);
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&s.st, info_def(), true, s.raw, s.changes,
                                        s.now += 10000, 0).presses);
}

/* --------------------------------------------------------- robustness --- */

static void test_an_unseen_signal_never_fires(void)
{
    vbtn_state_t st;
    memset(&st, 0, sizeof(st));
    for (int i = 0; i < 10; i++) {
        vbtn_event_t ev = vbtn_feed(&st, info_def(), false, DOWN, 7, i * 10000, LONG_US);
        TEST_ASSERT_EQUAL_UINT(0, ev.presses);
        TEST_ASSERT_FALSE(ev.long_press);
    }
}

/*
 * A frame that stops and later resumes must not fire. This is the case that
 * matters when the ignition goes off and on: the signal goes invalid, and the
 * first sample after it returns only re-arms the detector.
 */
static void test_a_signal_going_away_and_returning_does_not_fire(void)
{
    vbtn_state_t st;
    memset(&st, 0, sizeof(st));
    int64_t t = 0;
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, UP, 0, t += 10000, LONG_US).presses);
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, DOWN, 1, t += 10000, LONG_US).presses);
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, info_def(), true, UP, 2, t += 10000, LONG_US).presses);

    vbtn_feed(&st, info_def(), false, 0, 0, t += 10000, LONG_US);   /* gone */

    /* Back, already held, with a change counter that jumped a long way. */
    vbtn_event_t ev = vbtn_feed(&st, info_def(), true, DOWN, 900, t += 10000, LONG_US);
    TEST_ASSERT_EQUAL_UINT(0, ev.presses);
    TEST_ASSERT_FALSE(ev.long_press);
    /* And that resumed hold does not become a long press either. */
    ev = vbtn_feed(&st, info_def(), true, DOWN, 900, t += LONG_US * 2, LONG_US);
    TEST_ASSERT_FALSE(ev.long_press);
    /* Releasing it reports nothing; the next real press works. */
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, UP, 901, t += 10000, LONG_US).presses);
    vbtn_feed(&st, info_def(), true, DOWN, 902, t += 10000, LONG_US);
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, info_def(), true, UP, 903, t += 10000, LONG_US).presses);
}

static void test_change_counter_wrap(void)
{
    vbtn_state_t st;
    memset(&st, 0, sizeof(st));
    int64_t t = 0;
    vbtn_feed(&st, info_def(), true, UP, 0xFFFFFFFEu, t += 10000, LONG_US);
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, DOWN, 0xFFFFFFFFu,
                                        t += 10000, LONG_US).presses);
    /* The counter wraps to 0 on the release. */
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, info_def(), true, UP, 0x00000000u,
                                        t += 10000, LONG_US).presses);
}

/*
 * Inconsistent input: the level changed but the decoder's counter did not. The
 * detector must still behave, because the alternative was an unsigned underflow
 * that reported billions of presses and spun the screens.
 */
static void test_a_level_change_without_a_counter_change(void)
{
    vbtn_state_t st;
    memset(&st, 0, sizeof(st));
    int64_t t = 0;
    vbtn_feed(&st, info_def(), true, UP, 5, t += 10000, LONG_US);
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, DOWN, 5, t += 10000, LONG_US).presses);
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, info_def(), true, UP, 5, t += 10000, LONG_US).presses);
}

static void test_null_arguments(void)
{
    vbtn_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(NULL, info_def(), true, DOWN, 1, 0, LONG_US).presses);
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, NULL, true, DOWN, 1, 0, LONG_US).presses);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_button_is_defined);
    RUN_TEST(test_out_of_range_ids);
    RUN_TEST(test_the_default_is_the_info_button);
    RUN_TEST(test_masked_controls_ignore_the_rest_of_the_byte);
    RUN_TEST(test_a_press_reports_on_release);
    RUN_TEST(test_holding_does_not_repeat);
    RUN_TEST(test_a_press_that_fell_between_polls_is_not_lost);
    RUN_TEST(test_several_missed_presses_are_all_counted);
    RUN_TEST(test_a_button_already_held_at_startup_reports_nothing);
    RUN_TEST(test_a_long_hold_fires_once_and_suppresses_the_short_press);
    RUN_TEST(test_a_hold_just_under_the_threshold_is_a_short_press);
    RUN_TEST(test_two_holds_in_a_row_each_fire);
    RUN_TEST(test_a_re_press_between_polls_restarts_the_hold_timer);
    RUN_TEST(test_long_press_can_be_disabled);
    RUN_TEST(test_an_unseen_signal_never_fires);
    RUN_TEST(test_a_signal_going_away_and_returning_does_not_fire);
    RUN_TEST(test_change_counter_wrap);
    RUN_TEST(test_a_level_change_without_a_counter_change);
    RUN_TEST(test_null_arguments);
    return UNITY_END();
}
