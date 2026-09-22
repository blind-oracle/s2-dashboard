/*
 * Host tests for using a handlebar button as the screen-cycling input.
 *
 * vehicle_button.c is pure, so the button table and the edge detector are both
 * covered here. The vehicle_state read that feeds it lives in display.c and is
 * on-target only.
 */
#include <string.h>

#include "s2_dbc_gen.h"
#include "unity.h"
#include "vehicle_button.h"

void setUp(void) {}
void tearDown(void) {}

/* Released and held raw values for the info/scroll button. */
#define UP 0x00
#define DOWN 0x01

/* Feed a sample, keeping a change counter the way the decoder would. */
typedef struct {
    vbtn_state_t st;
    uint32_t changes;
    uint64_t raw;
} sim_t;

static const vbtn_def_t *info_def(void)
{
    return vbtn_get(VBTN_INFO_SCROLL);
}

/* Set the raw value, bumping the change counter only when it actually changes. */
static void sim_set(sim_t *s, uint64_t raw)
{
    if (raw != s->raw) {
        s->raw = raw;
        s->changes++;
    }
}

static unsigned sim_poll(sim_t *s)
{
    return vbtn_feed(&s->st, info_def(), true, s->raw, s->changes);
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

/*
 * The masked controls are the reason a plain "raw != 0" test would not do: the
 * cruise arm bit shares a byte whose other bits move on their own.
 */
static void test_masked_controls_ignore_the_rest_of_the_byte(void)
{
    vbtn_state_t st;
    const vbtn_def_t *cruise = vbtn_get(VBTN_CRUISE);

    /* 0x09 and 0x05 are the documented base values with cruise disarmed. */
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, cruise, true, 0x09, 0));   /* arms */
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, cruise, true, 0x05, 1));   /* base changed only */
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, cruise, true, 0x85, 2));   /* bit 7 set: armed */
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, cruise, true, 0x89, 3));   /* still armed */
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, cruise, true, 0x09, 4));   /* disarmed */

    /* The front brake is 0x40 in a byte that is otherwise 0x00. */
    const vbtn_def_t *fb = vbtn_get(VBTN_FRONT_BRAKE);
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, fb, true, 0x00, 0));
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, fb, true, 0x40, 1));

    /* The high beam is 0x04, so 0x01 must not read as pressed. */
    const vbtn_def_t *hb = vbtn_get(VBTN_HIGHBEAM);
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, hb, true, 0x00, 0));
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, hb, true, 0x01, 1));
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, hb, true, 0x04, 2));
}

/* ------------------------------------------------------ edge detection --- */

static void test_a_press_fires_once(void)
{
    sim_t s = { 0 };
    sim_set(&s, UP);
    TEST_ASSERT_EQUAL_UINT(0, sim_poll(&s));     /* first sample only arms */

    sim_set(&s, DOWN);
    TEST_ASSERT_EQUAL_UINT(1, sim_poll(&s));
    sim_set(&s, UP);
    TEST_ASSERT_EQUAL_UINT(0, sim_poll(&s));     /* release does not fire */
}

static void test_holding_does_not_repeat(void)
{
    sim_t s = { 0 };
    sim_set(&s, UP);
    sim_poll(&s);
    sim_set(&s, DOWN);
    TEST_ASSERT_EQUAL_UINT(1, sim_poll(&s));
    for (int i = 0; i < 50; i++) {
        TEST_ASSERT_EQUAL_UINT(0, sim_poll(&s));   /* still held, no new presses */
    }
    sim_set(&s, UP);
    TEST_ASSERT_EQUAL_UINT(0, sim_poll(&s));
}

static void test_a_button_already_held_at_startup_does_not_fire(void)
{
    sim_t s = { 0 };
    sim_set(&s, DOWN);                            /* held before the first poll */
    TEST_ASSERT_EQUAL_UINT(0, sim_poll(&s));
    TEST_ASSERT_EQUAL_UINT(0, sim_poll(&s));
    sim_set(&s, UP);
    TEST_ASSERT_EQUAL_UINT(0, sim_poll(&s));
    sim_set(&s, DOWN);
    TEST_ASSERT_EQUAL_UINT(1, sim_poll(&s));      /* only a real new press counts */
}

static void test_a_press_that_fell_between_polls_is_not_lost(void)
{
    sim_t s = { 0 };
    sim_set(&s, UP);
    sim_poll(&s);

    /* Pressed and released with no poll in between: two changes, same level. */
    sim_set(&s, DOWN);
    sim_set(&s, UP);
    TEST_ASSERT_EQUAL_UINT(1, sim_poll(&s));
}

static void test_several_missed_presses_are_all_counted(void)
{
    sim_t s = { 0 };
    sim_set(&s, UP);
    sim_poll(&s);

    for (int i = 0; i < 3; i++) {
        sim_set(&s, DOWN);
        sim_set(&s, UP);
    }
    TEST_ASSERT_EQUAL_UINT(3, sim_poll(&s));

    /* Ending held: the trailing press counts too. */
    for (int i = 0; i < 2; i++) {
        sim_set(&s, DOWN);
        sim_set(&s, UP);
    }
    sim_set(&s, DOWN);
    TEST_ASSERT_EQUAL_UINT(3, sim_poll(&s));
}

static void test_a_missed_release_does_not_invent_a_press(void)
{
    sim_t s = { 0 };
    sim_set(&s, UP);
    sim_poll(&s);
    sim_set(&s, DOWN);
    TEST_ASSERT_EQUAL_UINT(1, sim_poll(&s));

    /* Released, pressed, released with no poll between: one new press. */
    sim_set(&s, UP);
    sim_set(&s, DOWN);
    sim_set(&s, UP);
    TEST_ASSERT_EQUAL_UINT(1, sim_poll(&s));
}

static void test_an_unseen_signal_never_fires(void)
{
    vbtn_state_t st;
    memset(&st, 0, sizeof(st));
    for (int i = 0; i < 10; i++) {
        TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), false, DOWN, 7));
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
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, UP, 0));
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, info_def(), true, DOWN, 1));

    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), false, 0, 0));    /* gone */
    /* Back, already held, and with a change counter that jumped a long way. */
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, DOWN, 900));
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, UP, 901));
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, info_def(), true, DOWN, 902));
}

static void test_change_counter_wrap(void)
{
    vbtn_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, UP, 0xFFFFFFFEu));
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, info_def(), true, DOWN, 0xFFFFFFFFu));
    /* The counter wraps to 0 on the release and 1 on the next press. */
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, UP, 0x00000000u));
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, info_def(), true, DOWN, 0x00000001u));
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
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, UP, 5));
    TEST_ASSERT_EQUAL_UINT(1, vbtn_feed(&st, info_def(), true, DOWN, 5));
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, info_def(), true, UP, 5));
}

static void test_null_arguments(void)
{
    vbtn_state_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(NULL, info_def(), true, DOWN, 1));
    TEST_ASSERT_EQUAL_UINT(0, vbtn_feed(&st, NULL, true, DOWN, 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_button_is_defined);
    RUN_TEST(test_out_of_range_ids);
    RUN_TEST(test_the_default_is_the_info_button);
    RUN_TEST(test_masked_controls_ignore_the_rest_of_the_byte);
    RUN_TEST(test_a_press_fires_once);
    RUN_TEST(test_holding_does_not_repeat);
    RUN_TEST(test_a_button_already_held_at_startup_does_not_fire);
    RUN_TEST(test_a_press_that_fell_between_polls_is_not_lost);
    RUN_TEST(test_several_missed_presses_are_all_counted);
    RUN_TEST(test_a_missed_release_does_not_invent_a_press);
    RUN_TEST(test_an_unseen_signal_never_fires);
    RUN_TEST(test_a_signal_going_away_and_returning_does_not_fire);
    RUN_TEST(test_change_counter_wrap);
    RUN_TEST(test_a_level_change_without_a_counter_change);
    RUN_TEST(test_null_arguments);
    return UNITY_END();
}
