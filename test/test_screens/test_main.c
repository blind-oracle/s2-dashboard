/*
 * Host-side layout tests for the 240x280 display (pio test -e native).
 *
 * The risk is a value string that grows off an edge or
 * collides with the gauge ring. These tests render the actual screens with
 * worst-case data and assert that everything lands inside the frame and clear
 * of the ring, plus that the gauge and the degraded-data placeholders behave.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "screens.h"

#define W 240
#define H 280
#define CX 120
#define CY 120
/*
 * The gauge ring legitimately reaches 2 px from the edge, so the check is that
 * nothing touches the outermost row or column - which is what clipping looks
 * like when a string has grown too wide for the panel.
 */
#define MARGIN 1
#define R_RING_IN 100   /* the text rows must not reach into the gauge band */

static uint16_t fb[W * H];
static gfx_t g;

#define C_BG      GFX_RGB(0x00, 0x00, 0x00)
#define C_VALUE   GFX_RGB(0xFF, 0xFF, 0xFF)
#define C_REGEN   GFX_RGB(0x00, 0xE0, 0x00)
#define C_POWER   GFX_RGB(0xFF, 0xB0, 0x00)
#define C_PEAK    GFX_RGB(0xFF, 0x30, 0x00)

static void render(unsigned screen, const dash_data_t *d)
{
    gfx_init(&g, fb, W, H);
    screens_render(&g, screen, 4, d);
}

static unsigned count_colour(uint16_t c)
{
    unsigned n = 0;
    for (int i = 0; i < W * H; i++) {
        if (fb[i] == c) {
            n++;
        }
    }
    return n;
}

/* Pixels of one colour inside a radius band, for looking at the ring alone. */
static unsigned count_colour_in_band(uint16_t c, double r_min, double r_max)
{
    unsigned n = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (fb[y * W + x] != c) {
                continue;
            }
            double dx = x - CX, dy = y - CY;
            double r = sqrt(dx * dx + dy * dy);
            if (r >= r_min && r <= r_max) {
                n++;
            }
        }
    }
    return n;
}

/* True when every lit pixel is inside the frame margin. */
static bool inside_frame(void)
{
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (fb[y * W + x] == C_BG) {
                continue;
            }
            if (x < MARGIN || y < MARGIN || x >= W - MARGIN || y >= H - MARGIN) {
                return false;
            }
        }
    }
    return true;
}

/*
 * True when no text pixel intrudes into the gauge band. Text is anything that is
 * neither background nor one of the ring's own colours.
 */
static bool text_clear_of_ring(void)
{
    const uint16_t ring_colours[] = {
        GFX_RGB(0x30, 0x30, 0x30), GFX_RGB(0x40, 0x00, 0x00), C_REGEN, C_POWER, C_PEAK,
        GFX_RGB(0xA0, 0xA0, 0xA0), GFX_RGB(0x60, 0x60, 0x60), C_VALUE,
    };
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t c = fb[y * W + x];
            if (c == C_BG) {
                continue;
            }
            double dx = x - CX, dy = y - CY;
            if (dx * dx + dy * dy < (double)R_RING_IN * R_RING_IN) {
                continue;               /* inside the ring's inner radius: fine */
            }
            bool is_ring = false;
            for (unsigned i = 0; i < sizeof(ring_colours) / sizeof(ring_colours[0]); i++) {
                if (c == ring_colours[i]) {
                    is_ring = true;
                }
            }
            if (!is_ring) {
                return false;
            }
        }
    }
    return true;
}

static dash_data_t live_data(void)
{
    dash_data_t d;
    memset(&d, 0, sizeof(d));
    d.power_state = FIELD_LIVE;
    d.power_kw = 45.3;
    d.torque_state = FIELD_LIVE;
    d.torque_counts = 879;
    d.torque_nm = 879.0 / 3.35;
    d.volts_state = FIELD_LIVE;
    d.volts = 398.1;
    d.energy_state = FIELD_LIVE;
    d.used_kwh = 12.34;
    d.soh_state = FIELD_LIVE;
    d.soh_pct = 97;
    d.uds_enabled = true;
    return d;
}

void test_nothing_is_drawn_outside_the_frame(void)
{
    dash_data_t d = live_data();
    render(0, &d);
    TEST_ASSERT_TRUE_MESSAGE(inside_frame(), "screen 1 draws outside the panel");
    TEST_ASSERT_TRUE_MESSAGE(text_clear_of_ring(), "screen 1 text collides with the gauge ring");

    for (unsigned s = 1; s < 4; s++) {
        render(s, &d);
        TEST_ASSERT_TRUE_MESSAGE(inside_frame(), "an empty screen draws outside the panel");
    }
}

void test_worst_case_strings_stay_inside(void)
{
    /* Every field at its longest printable form, both signs. */
    dash_data_t d = live_data();
    d.power_kw = -99.9;
    d.torque_counts = -1499;
    d.torque_nm = -9999;
    d.volts = 999.9;
    d.used_kwh = 999.9;
    d.soh_pct = 100;
    render(0, &d);
    TEST_ASSERT_TRUE_MESSAGE(inside_frame(), "worst-case strings overflow the panel");
    TEST_ASSERT_TRUE_MESSAGE(text_clear_of_ring(), "worst-case strings collide with the gauge ring");

    d.power_kw = 99.9;
    d.used_kwh = 999.9;
    render(0, &d);
    TEST_ASSERT_TRUE_MESSAGE(inside_frame(), "worst-case strings overflow the panel");
    TEST_ASSERT_TRUE_MESSAGE(text_clear_of_ring(), "worst-case strings collide with the gauge ring");
}

void test_placeholders_when_nothing_has_been_seen(void)
{
    dash_data_t d;
    memset(&d, 0, sizeof(d));   /* every field FIELD_MISSING */
    render(0, &d);
    TEST_ASSERT_TRUE(inside_frame());
    /* no live-white values and no gauge fill at all */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, count_colour(C_POWER), "power fill drawn with no data");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, count_colour(C_REGEN), "regen fill drawn with no data");
    /* the white zero mark and the active dot are still there, so some white exists */
    TEST_ASSERT_TRUE(count_colour(C_VALUE) > 0);
}

void test_gauge_fill_grows_with_power(void)
{
    dash_data_t d = live_data();
    d.power_kw = 5.0;
    render(0, &d);
    unsigned small = count_colour(C_POWER);
    d.power_kw = 40.0;
    render(0, &d);
    unsigned big = count_colour(C_POWER);
    TEST_ASSERT_TRUE_MESSAGE(small > 0, "no amber fill at 5 kW");
    TEST_ASSERT_TRUE_MESSAGE(big > small * 3, "the amber fill does not grow with power");

    /* above the peak threshold the tip turns red */
    d.power_kw = 65.0;
    render(0, &d);
    TEST_ASSERT_TRUE_MESSAGE(count_colour(C_PEAK) > 0, "no red zone above the peak threshold");
}

void test_regen_fills_the_other_way(void)
{
    dash_data_t d = live_data();
    d.power_kw = -8.0;
    render(0, &d);
    TEST_ASSERT_TRUE_MESSAGE(count_colour(C_REGEN) > 0, "no green fill on regen");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, count_colour_in_band(C_POWER, 99, 119),
                                   "amber fill drawn in the ring while regenerating");

    /* the green must sit left of the vertical centre line, the amber right of it */
    unsigned left = 0, right = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (fb[y * W + x] == C_REGEN) {
                if (x < CX) {
                    left++;
                } else {
                    right++;
                }
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(left > right, "regen should fill counter-clockwise from the zero mark");
}

void test_gauge_clamps_beyond_full_scale(void)
{
    double regen_fs, power_fs;
    screens_gauge_range(&regen_fs, &power_fs);
    TEST_ASSERT_TRUE(power_fs > 0 && regen_fs < 0);

    dash_data_t d = live_data();
    d.power_kw = power_fs;
    render(0, &d);
    unsigned at_fs = count_colour(C_POWER) + count_colour(C_PEAK);
    d.power_kw = power_fs * 10.0;      /* absurd value must not wrap round the dial */
    render(0, &d);
    unsigned beyond = count_colour(C_POWER) + count_colour(C_PEAK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(at_fs, beyond, "the gauge does not clamp at full scale");
    TEST_ASSERT_TRUE(inside_frame());
}

void test_stale_values_are_dimmed_not_hidden(void)
{
    dash_data_t d = live_data();
    render(0, &d);
    unsigned live_white = count_colour(C_VALUE);

    d.power_state = FIELD_STALE;
    d.torque_state = FIELD_STALE;
    d.volts_state = FIELD_STALE;
    d.energy_state = FIELD_STALE;
    render(0, &d);
    /* the numbers are still drawn, just no longer in the live colour */
    TEST_ASSERT_TRUE_MESSAGE(count_colour(C_VALUE) < live_white, "stale values kept the live colour");
    /*
     * The ring fill drains to grey so it cannot be mistaken for a live reading.
     * The header word stays amber on purpose: it is the escalation signal, and
     * it reads STALE rather than POWER.
     */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, count_colour_in_band(C_POWER, 99, 119),
                                   "stale power still filled the ring in amber");
    TEST_ASSERT_TRUE_MESSAGE(count_colour(C_POWER) > 0, "the stale header should still be amber");
}

/* State of health lives on the battery screen now, not the ride screen. */
void test_soh_distinguishes_uds_off_from_no_answer(void)
{
    dash_data_t d = live_data();
    d.soh_state = FIELD_MISSING;
    d.uds_enabled = false;
    render(1, &d);
    unsigned a = count_colour(GFX_RGB(0x60, 0x60, 0x60));
    d.uds_enabled = true;
    render(1, &d);
    unsigned b = count_colour(GFX_RGB(0x60, 0x60, 0x60));
    /* "n/a" plus the explanatory line, versus a bare "--" */
    TEST_ASSERT_TRUE_MESSAGE(a != b, "the SoH placeholder does not say why it is missing");
}

/* The ride screen shows motor speed where state of health used to be. */
void test_ride_screen_shows_rpm_not_soh(void)
{
    dash_data_t d = live_data();
    d.rpm_state = FIELD_LIVE;
    d.motor_rpm = 4210;
    render(0, &d);
    unsigned with_rpm = count_colour(GFX_RGB(0xFF, 0xFF, 0xFF));

    d.rpm_state = FIELD_MISSING;
    render(0, &d);
    unsigned without = count_colour(GFX_RGB(0xFF, 0xFF, 0xFF));
    TEST_ASSERT_NOT_EQUAL_UINT(with_rpm, without);

    /* Changing state of health must no longer alter the ride screen at all. */
    d.rpm_state = FIELD_LIVE;
    render(0, &d);
    unsigned base = count_colour(GFX_RGB(0x60, 0x60, 0x60));
    d.soh_state = FIELD_MISSING;
    d.uds_enabled = !d.uds_enabled;
    render(0, &d);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(base, count_colour(GFX_RGB(0x60, 0x60, 0x60)),
                                   "state of health still affects the ride screen");
}


/* --------------------------------------------------------- detail screens --- */

/* Every field populated with a plausible but extreme value. */
static dash_data_t detail_data(void)
{
    dash_data_t d = live_data();
    d.rpm_state = FIELD_LIVE;          d.motor_rpm = -9999;
    d.cell_state = FIELD_LIVE;         d.cell_mv_min = 2988; d.cell_mv_avg = 3901; d.cell_mv_max = 4211;
    d.current_state = FIELD_LIVE;      d.pack_amps = -238.7;
    d.soc_state = FIELD_LIVE;          d.soc_pct = 100;
    d.soh_state = FIELD_LIVE;          d.soh_pct = 97;
    d.pack_temp_state = FIELD_LIVE;    d.pack_t_min = -20; d.pack_t_avg = 31; d.pack_t_max = 102;
    d.coolant_state = FIELD_LIVE;      d.coolant_c = 60;
    d.inverter_state = FIELD_LIVE;     d.inverter_c = 102;
    d.ambient_state = FIELD_LIVE;      d.ambient_c = -18;
    d.accel_state = FIELD_LIVE;        d.accel_now = -7865; d.accel_max_pos = 8123; d.accel_max_neg = -8877;
    d.tyre_state = FIELD_LIVE;         d.tyre_front_kpa = 510; d.tyre_rear_kpa = 248;
    d.gps_state = FIELD_LIVE;          d.gps_lat = -33.86785; d.gps_lon = -151.20732;
    d.cell_signal_state = FIELD_LIVE;  d.cell_signal = 141;
    snprintf(d.plmn, sizeof(d.plmn), "310 410");
    d.uds_enabled = true;
    return d;
}

void test_every_screen_stays_inside_the_panel(void)
{
    dash_data_t d = detail_data();
    for (unsigned sc = 0; sc < SCREENS_IMPLEMENTED; sc++) {
        render(sc, &d);
        char msg[48];
        snprintf(msg, sizeof(msg), "screen %u draws outside the panel", sc + 1);
        TEST_ASSERT_TRUE_MESSAGE(inside_frame(), msg);
    }
}

/* The same, with nothing ever received: every field is a placeholder. */
void test_every_screen_stays_inside_when_nothing_is_known(void)
{
    dash_data_t d;
    memset(&d, 0, sizeof(d));   /* all states FIELD_MISSING, uds off */
    for (unsigned sc = 0; sc < SCREENS_IMPLEMENTED; sc++) {
        render(sc, &d);
        char msg[48];
        snprintf(msg, sizeof(msg), "empty screen %u draws outside the panel", sc + 1);
        TEST_ASSERT_TRUE_MESSAGE(inside_frame(), msg);
    }
}

/* A screen past the implemented ones must still be a safe placeholder. */
void test_screens_beyond_the_implemented_ones(void)
{
    dash_data_t d = detail_data();
    render(SCREENS_IMPLEMENTED, &d);
    TEST_ASSERT_TRUE(inside_frame());
    render(7, &d);
    TEST_ASSERT_TRUE(inside_frame());
}

void test_detail_screens_show_placeholders_not_zeros(void)
{
    dash_data_t live = detail_data();
    dash_data_t none;
    memset(&none, 0, sizeof(none));
    /* Each detail screen must look different with and without data; a screen
     * that rendered zeros for missing values would look identical to 0.0. */
    for (unsigned sc = 1; sc < SCREENS_IMPLEMENTED; sc++) {
        render(sc, &live);
        unsigned a = count_colour(GFX_RGB(0xFF, 0xFF, 0xFF));
        render(sc, &none);
        unsigned b = count_colour(GFX_RGB(0xFF, 0xFF, 0xFF));
        char msg[56];
        snprintf(msg, sizeof(msg), "screen %u looks the same with no data", sc + 1);
        TEST_ASSERT_TRUE_MESSAGE(a != b, msg);
    }
}

void test_pressure_conversion(void)
{
    double out = 0;
    const char *unit = screens_pressure(248.2, &out);
    TEST_ASSERT_NOT_NULL(unit);
    /* The host build selects PSI, matching the Kconfig default. */
    TEST_ASSERT_EQUAL_STRING("PSI", unit);
    TEST_ASSERT_FLOAT_WITHIN(0.05, 36.0, out);

    /* Zero and a big value must stay finite and correctly scaled. */
    screens_pressure(0.0, &out);
    TEST_ASSERT_EQUAL_FLOAT(0.0, out);
    screens_pressure(510.0, &out);
    TEST_ASSERT_FLOAT_WITHIN(0.05, 73.97, out);
}

/* The telematics screen has to explain itself when UDS polling is off, because
 * that is the default build and every value on it would otherwise be dashes. */
void test_telematics_screen_explains_a_missing_uds(void)
{
    dash_data_t d;
    memset(&d, 0, sizeof(d));
    d.uds_enabled = false;
    render(4, &d);
    unsigned off = count_colour(GFX_RGB(0x60, 0x60, 0x60));
    d.uds_enabled = true;
    render(4, &d);
    unsigned on = count_colour(GFX_RGB(0x60, 0x60, 0x60));
    TEST_ASSERT_TRUE_MESSAGE(off > on, "no hint that the telematics screen needs UDS");
}

void setUp(void) {}
void tearDown(void) {}


/* ------------------------------------------------------------ update mode --- */

static void render_update(const update_data_t *d)
{
    gfx_init(&g, fb, W, H);
    screens_render_update(&g, d);
}

static update_data_t waiting_data(void)
{
    update_data_t d;
    memset(&d, 0, sizeof(d));
    d.phase = UPDATE_WAITING;
    snprintf(d.ssid, sizeof(d.ssid), "S2-DASH-OTA");
    snprintf(d.pass, sizeof(d.pass), "48210736");
    snprintf(d.ip, sizeof(d.ip), "192.168.4.1");
    return d;
}

static void test_update_screen_stays_inside_the_panel(void)
{
    update_data_t d = waiting_data();
    render_update(&d);
    TEST_ASSERT_TRUE_MESSAGE(inside_frame(), "update screen draws outside the panel");

    /* Every phase, including the widest possible strings. */
    d.phase = UPDATE_RECEIVING;
    d.received = 1219419;
    d.total = 1740800;
    snprintf(d.detail, sizeof(d.detail), "0.1.0 to 9.99.99-rc1");
    render_update(&d);
    TEST_ASSERT_TRUE_MESSAGE(inside_frame(), "receiving screen draws outside the panel");

    d.phase = UPDATE_DONE;
    render_update(&d);
    TEST_ASSERT_TRUE_MESSAGE(inside_frame(), "done screen draws outside the panel");

    d.phase = UPDATE_FAILED;
    snprintf(d.detail, sizeof(d.detail), "image is larger than the slot");
    render_update(&d);
    TEST_ASSERT_TRUE_MESSAGE(inside_frame(), "failed screen draws outside the panel");
}

/*
 * The strings come from Kconfig and from the runtime, so they can be longer than
 * the designer assumed. The renderer shrinks the scale to fit; what must never
 * happen is drawing off the edge.
 */
static void test_update_screen_survives_overlong_strings(void)
{
    update_data_t d;
    memset(&d, 0, sizeof(d));
    d.phase = UPDATE_WAITING;
    memset(d.ssid, 'W', sizeof(d.ssid) - 1);
    memset(d.pass, '8', sizeof(d.pass) - 1);
    memset(d.ip, '2', sizeof(d.ip) - 1);
    memset(d.detail, 'X', sizeof(d.detail) - 1);
    render_update(&d);
    TEST_ASSERT_TRUE_MESSAGE(inside_frame(), "overlong strings overflow the panel");
}

static void test_update_screen_shows_the_passphrase_while_waiting(void)
{
    /* The passphrase is what restricts the upload to somebody at the bike, so
     * it has to actually be drawn, and in its own colour. */
    const uint16_t c_pass = GFX_RGB(0x00, 0xE0, 0xE0);
    update_data_t d = waiting_data();
    render_update(&d);
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0, count_colour(c_pass), "passphrase not drawn");

    /* Once receiving, the connection details give way to progress. */
    d.phase = UPDATE_RECEIVING;
    d.received = 100;
    d.total = 1000;
    render_update(&d);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, count_colour(c_pass),
                                   "passphrase still on screen while receiving");
}

static void test_update_progress_bar_tracks_the_byte_count(void)
{
    const uint16_t c_bar = GFX_RGB(0xFF, 0xB0, 0x00);
    update_data_t d = waiting_data();
    d.phase = UPDATE_RECEIVING;
    d.total = 1000000;

    d.received = 0;
    render_update(&d);
    unsigned at_zero = count_colour(c_bar);

    d.received = 500000;
    render_update(&d);
    unsigned at_half = count_colour(c_bar);

    d.received = 1000000;
    render_update(&d);
    unsigned at_full = count_colour(c_bar);

    TEST_ASSERT_GREATER_THAN_UINT(at_zero, at_half);
    TEST_ASSERT_GREATER_THAN_UINT(at_half, at_full);
}

/* A total of zero means the size is unknown; it must not divide by it. */
static void test_update_progress_without_a_known_total(void)
{
    update_data_t d = waiting_data();
    d.phase = UPDATE_RECEIVING;
    d.total = 0;
    d.received = 65536;
    render_update(&d);
    TEST_ASSERT_TRUE(inside_frame());
}

static void test_update_screen_handles_null_and_empty(void)
{
    update_data_t d;
    memset(&d, 0, sizeof(d));
    gfx_init(&g, fb, W, H);
    screens_render_update(&g, NULL);     /* must not crash */
    screens_render_update(NULL, &d);
    render_update(&d);                   /* all-empty strings */
    TEST_ASSERT_TRUE(inside_frame());
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_nothing_is_drawn_outside_the_frame);
    RUN_TEST(test_worst_case_strings_stay_inside);
    RUN_TEST(test_placeholders_when_nothing_has_been_seen);
    RUN_TEST(test_gauge_fill_grows_with_power);
    RUN_TEST(test_regen_fills_the_other_way);
    RUN_TEST(test_gauge_clamps_beyond_full_scale);
    RUN_TEST(test_stale_values_are_dimmed_not_hidden);
    RUN_TEST(test_soh_distinguishes_uds_off_from_no_answer);
    RUN_TEST(test_ride_screen_shows_rpm_not_soh);
    RUN_TEST(test_every_screen_stays_inside_the_panel);
    RUN_TEST(test_every_screen_stays_inside_when_nothing_is_known);
    RUN_TEST(test_screens_beyond_the_implemented_ones);
    RUN_TEST(test_detail_screens_show_placeholders_not_zeros);
    RUN_TEST(test_pressure_conversion);
    RUN_TEST(test_telematics_screen_explains_a_missing_uds);
    RUN_TEST(test_update_screen_stays_inside_the_panel);
    RUN_TEST(test_update_screen_survives_overlong_strings);
    RUN_TEST(test_update_screen_shows_the_passphrase_while_waiting);
    RUN_TEST(test_update_progress_bar_tracks_the_byte_count);
    RUN_TEST(test_update_progress_without_a_known_total);
    RUN_TEST(test_update_screen_handles_null_and_empty);
    return UNITY_END();
}
