/*
 * Host tests for the since-restart extremes.
 *
 * These are fed one CAN frame at a time from the decoder rather than sampled at
 * the display's refresh rate, which is the whole point: the battery frame
 * arrives far faster than the screen redraws, so a peak captured here is real
 * where a peak sampled by the display is whatever it happened to catch.
 *
 * The logic is pure, so it is driven directly.
 */
#include <string.h>

#include "ride_limits.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* A plausible resting pair: 390 V, drawing nothing. */
#define V_REST 390.0

static ride_extremes_t fresh(void)
{
    ride_extremes_t e;
    memset(&e, 0xAA, sizeof(e));   /* reset must not rely on zeroed memory */
    ride_extremes_reset(&e);
    return e;
}

static void test_reset_clears_everything(void)
{
    ride_extremes_t e = fresh();
    TEST_ASSERT_FALSE(e.pack_seen);
    TEST_ASSERT_FALSE(e.torque_seen);
    TEST_ASSERT_FALSE(e.accel_seen);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, e.power_max_kw);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, e.power_min_kw);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, e.amps_max);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, e.amps_min);
    TEST_ASSERT_EQUAL_DOUBLE(0.0, e.torque_max_counts);
}

/* The first valid sample has to seed both ends, or one end stays at zero and
 * a ride that never charges would report a charging peak of 0 A. */
static void test_the_first_sample_seeds_both_ends(void)
{
    ride_extremes_t e = fresh();
    ride_extremes_pack(&e, 380.0, -100.0);      /* discharging 38 kW */
    TEST_ASSERT_TRUE(e.pack_seen);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 38.0, e.power_max_kw);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 38.0, e.power_min_kw);
    TEST_ASSERT_FLOAT_WITHIN(0.01, -100.0, e.amps_max);
    TEST_ASSERT_FLOAT_WITHIN(0.01, -100.0, e.amps_min);
}

static void test_both_directions_are_kept(void)
{
    ride_extremes_t e = fresh();
    ride_extremes_pack(&e, V_REST, 0.0);        /* at rest */
    ride_extremes_pack(&e, 304.0, -189.5);      /* wide open throttle */
    ride_extremes_pack(&e, 400.0, 30.0);        /* regenerating */
    ride_extremes_pack(&e, 385.0, -20.0);       /* cruising */

    /* Discharge is charge-positive negative, so driving is the positive kW end. */
    TEST_ASSERT_FLOAT_WITHIN(0.1, 57.6, e.power_max_kw);
    TEST_ASSERT_FLOAT_WITHIN(0.1, -12.0, e.power_min_kw);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 30.0, e.amps_max);
    TEST_ASSERT_FLOAT_WITHIN(0.01, -189.5, e.amps_min);
}

static void test_extremes_only_grow(void)
{
    ride_extremes_t e = fresh();
    ride_extremes_pack(&e, 304.0, -189.5);
    double hi = e.power_max_kw, lo = e.power_min_kw;
    for (int i = 0; i < 100; i++) {
        ride_extremes_pack(&e, V_REST, -1.0);   /* long gentle cruise afterwards */
    }
    TEST_ASSERT_EQUAL_DOUBLE(hi, e.power_max_kw);
    TEST_ASSERT_TRUE(e.power_min_kw <= lo);
}

/*
 * The important one. A single glitched decode must not leave a peak that never
 * clears, so the same plausibility rule the gauge uses gates the extremes too.
 */
static void test_an_implausible_sample_cannot_set_a_peak(void)
{
    ride_extremes_t e = fresh();
    ride_extremes_pack(&e, 380.0, -100.0);
    double hi = e.power_max_kw;

    ride_extremes_pack(&e, 380.0, -4000.0);     /* current way past the floor */
    ride_extremes_pack(&e, 0.0, -300.0);        /* voltage collapsed to zero */
    ride_extremes_pack(&e, 9000.0, -300.0);     /* voltage absurdly high */
    ride_extremes_pack(&e, 380.0, 500.0);       /* charging far past the ceiling */
    TEST_ASSERT_EQUAL_DOUBLE(hi, e.power_max_kw);
    TEST_ASSERT_FLOAT_WITHIN(0.01, -100.0, e.amps_min);

    /* A plausible sample still lands afterwards. */
    ride_extremes_pack(&e, 304.0, -189.5);
    TEST_ASSERT_FLOAT_WITHIN(0.1, 57.6, e.power_max_kw);
}

/* The very first sample being implausible must not arm the record either. */
static void test_an_implausible_first_sample_does_not_arm(void)
{
    ride_extremes_t e = fresh();
    ride_extremes_pack(&e, 0.0, 0.0);
    TEST_ASSERT_FALSE(e.pack_seen);
    ride_extremes_pack(&e, 380.0, -50.0);
    TEST_ASSERT_TRUE(e.pack_seen);
    TEST_ASSERT_FLOAT_WITHIN(0.01, 19.0, e.power_max_kw);
}

static void test_torque_tracks_only_the_maximum(void)
{
    ride_extremes_t e = fresh();
    ride_extremes_torque(&e, 400.0);
    TEST_ASSERT_TRUE(e.torque_seen);
    TEST_ASSERT_EQUAL_DOUBLE(400.0, e.torque_max_counts);

    ride_extremes_torque(&e, 900.0);
    TEST_ASSERT_EQUAL_DOUBLE(900.0, e.torque_max_counts);
    ride_extremes_torque(&e, 100.0);
    TEST_ASSERT_EQUAL_DOUBLE(900.0, e.torque_max_counts);

    /* Regeneration drives torque negative; that must not become the maximum. */
    ride_extremes_torque(&e, -280.0);
    TEST_ASSERT_EQUAL_DOUBLE(900.0, e.torque_max_counts);

    /* And an implausible count is rejected the same way. */
    ride_extremes_torque(&e, 50000.0);
    TEST_ASSERT_EQUAL_DOUBLE(900.0, e.torque_max_counts);
}

/* A first torque sample below zero still arms the record at that value. */
static void test_torque_first_sample_may_be_negative(void)
{
    ride_extremes_t e = fresh();
    ride_extremes_torque(&e, -200.0);
    TEST_ASSERT_TRUE(e.torque_seen);
    TEST_ASSERT_EQUAL_DOUBLE(-200.0, e.torque_max_counts);
    ride_extremes_torque(&e, -50.0);
    TEST_ASSERT_EQUAL_DOUBLE(-50.0, e.torque_max_counts);
}

static void test_acceleration_keeps_both_ends(void)
{
    ride_extremes_t e = fresh();
    ride_extremes_accel(&e, -1420.0);
    TEST_ASSERT_TRUE(e.accel_seen);
    TEST_ASSERT_EQUAL_DOUBLE(-1420.0, e.accel_max);
    TEST_ASSERT_EQUAL_DOUBLE(-1420.0, e.accel_min);

    ride_extremes_accel(&e, 5310.0);
    ride_extremes_accel(&e, -6180.0);
    ride_extremes_accel(&e, 0.0);
    TEST_ASSERT_EQUAL_DOUBLE(5310.0, e.accel_max);
    TEST_ASSERT_EQUAL_DOUBLE(-6180.0, e.accel_min);
}

static void test_null_is_harmless(void)
{
    ride_extremes_reset(NULL);
    ride_extremes_pack(NULL, 380.0, -100.0);
    ride_extremes_torque(NULL, 900.0);
    ride_extremes_accel(NULL, 1.0);
    TEST_PASS();
}

/*
 * The peak a per-frame tracker sees versus what point-sampling at the display
 * rate would have seen. This is the bug being fixed, expressed as a test: a
 * burst whose peak falls between two display samples is invisible to the
 * display and caught here.
 */
static void test_a_peak_between_display_samples_is_still_caught(void)
{
    ride_extremes_t e = fresh();
    /* Fifty frames, the way the bus delivers them; the display would read the
     * first of every five. The peak deliberately sits on a frame it skips. */
    double display_best = 0.0;
    for (int i = 0; i < 50; i++) {
        double amps = (i == 12) ? -189.5 : -20.0;   /* frame 12 is the peak */
        double volts = (i == 12) ? 304.0 : 385.0;
        ride_extremes_pack(&e, volts, amps);
        if (i % 5 == 0) {                            /* what the display samples */
            double kw = ride_power_kw(volts, amps);
            if (kw > display_best) {
                display_best = kw;
            }
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(0.1, 57.6, e.power_max_kw);
    TEST_ASSERT_FLOAT_WITHIN(0.1, 7.7, display_best);
    TEST_ASSERT_TRUE_MESSAGE(e.power_max_kw > display_best * 2.0,
                             "per-frame tracking should beat display sampling here");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reset_clears_everything);
    RUN_TEST(test_the_first_sample_seeds_both_ends);
    RUN_TEST(test_both_directions_are_kept);
    RUN_TEST(test_extremes_only_grow);
    RUN_TEST(test_an_implausible_sample_cannot_set_a_peak);
    RUN_TEST(test_an_implausible_first_sample_does_not_arm);
    RUN_TEST(test_torque_tracks_only_the_maximum);
    RUN_TEST(test_torque_first_sample_may_be_negative);
    RUN_TEST(test_acceleration_keeps_both_ends);
    RUN_TEST(test_null_is_harmless);
    RUN_TEST(test_a_peak_between_display_samples_is_still_caught);
    return UNITY_END();
}
