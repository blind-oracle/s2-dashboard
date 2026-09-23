#include "screens.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#if CONFIG_S2_DISPLAY_ENABLE   /* the whole file is optional */
/*
 * Palette. Light-on-black throughout: an IPS panel in sunlight gains a
 * reflected luminance floor that hurts dark glyphs far more than bright ones,
 * and nothing here leans on blue, which is both the coarsest RGB565 channel
 * and the dimmest to the eye.
 */
#define C_BG        GFX_RGB(0x00, 0x00, 0x00)
#define C_VALUE     GFX_RGB(0xFF, 0xFF, 0xFF)
#define C_LABEL     GFX_RGB(0xA0, 0xA0, 0xA0)
#define C_DIM       GFX_RGB(0x60, 0x60, 0x60)
#define C_TRACK     GFX_RGB(0x30, 0x30, 0x30)
#define C_REDZONE   GFX_RGB(0x40, 0x00, 0x00)
#define C_REGEN     GFX_RGB(0x00, 0xE0, 0x00)
#define C_POWER     GFX_RGB(0xFF, 0xB0, 0x00)
#define C_PEAK      GFX_RGB(0xFF, 0x30, 0x00)
#define C_STALE     GFX_RGB(0x80, 0x80, 0x80)

#define CX 120
#define CY 120       /* the gauge sits in the upper part of the 240x280 panel */

/* Ring geometry. The panel is square, so the ring is a motif rather than a
 * boundary: only the arc itself constrains where the text rows can start. */
#define R_IN        100
#define R_OUT       118
#define R_TICK_MAJ  111
#define R_TICK_MIN  106

/*
 * Text rows. The panel is 40 px taller than it is wide, and the gauge's open
 * bottom sector plus that extra height leave room for three large value rows
 * below the hero number.
 */
#define Y_HEADER    47
#define Y_HERO      66
#define Y_DIVIDER   130
#define Y_TORQUE    140
#define Y_VOLTS     172
#define Y_ENERGY    204
#define Y_RPM       236
#define Y_DOTS      266
/*
 * Fixed anchors so a value changing length never shifts the row. Chosen so the
 * widest "398.1 Vdc" block sits centred on the panel.
 */
#define X_VALUE     142   /* values are right-aligned here */
#define X_UNIT      150   /* units start here */
#define VALUE_SCALE 3     /* the three value rows */
#define UNIT_SCALE  2

/*
 * Angles are gauge angles: degrees clockwise from 12 o'clock. Zero power sits
 * at -60 (about 10 o'clock); the scale runs 225 degrees clockwise from there,
 * leaving the bottom 135 degrees free for the text rows.
 */
#define A_ZERO      (-60.0f)
#define SWEEP_DEG   225.0                      /* the arc budget, leaving the bottom clear */
#define REGEN_FS    ((double)CONFIG_S2_DISPLAY_REGEN_FULL_SCALE_KW)
#define POWER_FS    ((double)CONFIG_S2_DISPLAY_POWER_FULL_SCALE_KW)
/* Derived, so retuning either full scale keeps the arc inside its budget. */
#define DEG_PER_KW  ((float)(SWEEP_DEG / (POWER_FS + REGEN_FS)))
#define A_REGEN_END ((float)(A_ZERO - DEG_PER_KW * REGEN_FS))
#define A_POWER_END ((float)(A_ZERO + DEG_PER_KW * POWER_FS))
#define PEAK_KW     (POWER_FS * 0.64)          /* amber below, red above (45 of 70 kW) */
#define DEADBAND_KW 0.30                       /* below this the bike is neither driving nor regenerating */

void screens_gauge_range(double *regen_kw, double *power_kw)
{
    if (regen_kw) {
        *regen_kw = -REGEN_FS;
    }
    if (power_kw) {
        *power_kw = POWER_FS;
    }
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static float angle_for_kw(double kw)
{
    return (float)(A_ZERO + DEG_PER_KW * clampd(kw, -REGEN_FS, POWER_FS));
}

static uint16_t value_colour(field_state_t st)
{
    switch (st) {
    case FIELD_LIVE:  return C_VALUE;
    case FIELD_STALE: return C_STALE;
    default:          return C_DIM;
    }
}

static uint16_t unit_colour(field_state_t st)
{
    /* Units stay bright when a value merely froze, so "missing" and "frozen" read differently. */
    return st == FIELD_MISSING ? C_DIM : C_LABEL;
}

static void draw_ring(gfx_t *g, const dash_data_t *d)
{
    gfx_fill_ring(g, CX, CY, R_IN, R_OUT, A_REGEN_END, A_POWER_END, C_TRACK);
    gfx_fill_ring(g, CX, CY, R_IN, R_OUT, angle_for_kw(PEAK_KW), A_POWER_END, C_REDZONE);

    if (d->power_state != FIELD_MISSING && (d->power_state == FIELD_STALE || fabs(d->power_kw) >= DEADBAND_KW)) {
        float a = angle_for_kw(d->power_kw);
        if (d->power_state == FIELD_STALE) {
            /* The ring never lies: keep the last angle but drain the colour. */
            gfx_fill_ring(g, CX, CY, R_IN, R_OUT, A_ZERO, a, C_DIM);
        } else if (d->power_kw < 0.0) {
            gfx_fill_ring(g, CX, CY, R_IN, R_OUT, a, A_ZERO, C_REGEN);
        } else {
            float a_peak = angle_for_kw(PEAK_KW);
            if (d->power_kw > PEAK_KW) {
                gfx_fill_ring(g, CX, CY, R_IN, R_OUT, A_ZERO, a_peak, C_POWER);
                gfx_fill_ring(g, CX, CY, R_IN, R_OUT, a_peak, a, C_PEAK);
            } else {
                gfx_fill_ring(g, CX, CY, R_IN, R_OUT, A_ZERO, a, C_POWER);
            }
        }
    }

    /* Ticks after the fill, so the fill cannot bury them, and the zero mark last. */
    for (double kw = -REGEN_FS; kw <= POWER_FS + 0.01; kw += 10.0) {
        gfx_ring_tick(g, CX, CY, R_IN, R_TICK_MAJ, angle_for_kw(kw), 5, C_LABEL);
    }
    for (double kw = -REGEN_FS + 5.0; kw < POWER_FS; kw += 10.0) {
        gfx_ring_tick(g, CX, CY, R_IN, R_TICK_MIN, angle_for_kw(kw), 4, C_DIM);
    }
    gfx_fill_ring(g, CX, CY, R_IN, R_OUT, A_ZERO - 1.5f, A_ZERO + 1.5f, C_VALUE);
}

static void draw_dots(gfx_t *g, unsigned screen, unsigned count)
{
    if (count < 2) {
        return;
    }
    const int spacing = 16;
    int x0 = CX - spacing * ((int)count - 1) / 2;
    for (unsigned i = 0; i < count; i++) {
        int x = x0 + spacing * (int)i;
        if (i == screen) {
            gfx_fill_circle(g, x, Y_DOTS, 4, C_VALUE);
        } else {
            gfx_circle(g, x, Y_DOTS, 4, C_DIM);
        }
    }
}

/* A value row: right-aligned number at x=142, unit starting at x=148. */
/*
 * One value row: number right-aligned at X_VALUE, unit starting at X_UNIT with
 * its baseline aligned to the number's (the font's baseline is row 5 of 7, so
 * the smaller unit drops by 5 px per scale step).
 */
static void draw_row(gfx_t *g, int y_top, const char *value, const char *unit, field_state_t st)
{
    gfx_text_right(g, X_VALUE, y_top, value, VALUE_SCALE, value_colour(st));
    if (unit) {
        gfx_text(g, X_UNIT, y_top + (VALUE_SCALE - UNIT_SCALE) * 5, unit, UNIT_SCALE, unit_colour(st));
    }
}

static void render_ride_screen(gfx_t *g, const dash_data_t *d)
{
    char buf[16];

    draw_ring(g, d);

    /* Header word: what the ring is showing, in the ring's own colour. */
    const char *header;
    uint16_t header_colour;
    if (d->power_state == FIELD_MISSING) {
        header = "POWER kW";
        header_colour = C_DIM;
    } else if (d->power_state == FIELD_STALE) {
        header = "STALE kW";
        header_colour = C_POWER;
    } else if (d->power_kw < -DEADBAND_KW) {
        header = "REGEN kW";
        header_colour = C_REGEN;
    } else {
        header = "POWER kW";
        header_colour = d->power_kw > PEAK_KW ? C_PEAK : C_POWER;
    }
    gfx_text_center(g, CX, Y_HEADER, header, 2, header_colour);

    /* Hero value: seven-segment, 52 px tall. */
    gfx_seg_style_t hero = { .height = 52, .width = 28, .thickness = 7, .gap = 5 };
    if (d->power_state == FIELD_MISSING) {
        gfx_seg_text_center(g, CX, Y_HERO, "---", &hero, C_DIM);
    } else {
        double kw = clampd(d->power_kw, -99.9, 99.9);
        snprintf(buf, sizeof(buf), "%.1f", kw);
        gfx_seg_text_center(g, CX, Y_HERO, buf, &hero, value_colour(d->power_state));
    }

    gfx_hline(g, 36, Y_DIVIDER, 168, C_DIM);

    /* Torque, in counts with an estimated Nm figure. */
    if (d->torque_state == FIELD_MISSING) {
        draw_row(g, Y_TORQUE, "----", "~Nm", d->torque_state);
    } else {
        snprintf(buf, sizeof(buf), "%d", (int)(d->torque_nm > 0 ? d->torque_nm + 0.5 : d->torque_nm - 0.5));
        draw_row(g, Y_TORQUE, buf, "~Nm", d->torque_state);
    }

    /* Pack voltage. */
    if (d->volts_state == FIELD_MISSING) {
        draw_row(g, Y_VOLTS, "---.-", "Vdc", d->volts_state);
    } else {
        snprintf(buf, sizeof(buf), "%.1f", clampd(d->volts, 0.0, 999.9));
        draw_row(g, Y_VOLTS, buf, "Vdc", d->volts_state);
    }

    /* Energy consumed since boot. */
    if (d->energy_state == FIELD_MISSING) {
        draw_row(g, Y_ENERGY, "--.--", "kWh", d->energy_state);
    } else {
        double kwh = clampd(d->used_kwh, 0.0, 999.9);
        /* Switch on the rounded magnitude, so 99.999 does not print six glyphs. */
        if (kwh < 99.995) {
            snprintf(buf, sizeof(buf), "%.2f", kwh);
        } else {
            snprintf(buf, sizeof(buf), "%.1f", kwh);
        }
        draw_row(g, Y_ENERGY, buf, "kWh", d->energy_state);
    }

    /* Motor speed. State of health moved to the battery screen: it is UDS-only
     * and changes over months, so it does not belong on the ride screen. */
    if (d->rpm_state == FIELD_MISSING) {
        draw_row(g, Y_RPM, "-----", "rpm", d->rpm_state);
    } else {
        snprintf(buf, sizeof(buf), "%d", (int)clampd(d->motor_rpm, -99999.0, 99999.0));
        draw_row(g, Y_RPM, buf, "rpm", d->rpm_state);
    }
}

/* ------------------------------------------------------- detail screens --- */

/*
 * Screens 2 to 5 are label/value lists rather than gauges, so they share one
 * layout: a title, a rule, then rows 29 px apart. A row is either one value or
 * a min/avg/max triple, which takes the height of two.
 */
#define L_MARGIN     10
#define L_Y_TITLE    14
#define L_Y_RULE     38
#define L_Y_FIRST    50
#define L_ROW_H      29
#define L_LABEL_SC    1
#define L_VALUE_SC    2

static int list_y(int row)
{
    return L_Y_FIRST + row * L_ROW_H;
}

static void list_title(gfx_t *g, const char *title)
{
    gfx_text_center(g, g->w / 2, L_Y_TITLE, title, 2, C_POWER);
    gfx_hline(g, L_MARGIN, L_Y_RULE, g->w - 2 * L_MARGIN, C_TRACK);
}

/* One label on the left, one value right-aligned. */
static void list_row(gfx_t *g, int row, const char *label, const char *value, field_state_t st)
{
    int y = list_y(row);
    gfx_text(g, L_MARGIN, y + 4, label, L_LABEL_SC, C_LABEL);
    gfx_text_right(g, g->w - L_MARGIN, y, value, L_VALUE_SC, value_colour(st));
}

/*
 * A labelled triple, laid out over three lines so each column heading sits
 * directly above its own number. Occupies the height of two rows.
 */
static void list_triple(gfx_t *g, int row, const char *label, const char *ha, const char *hb,
                        const char *hc, const char *a, const char *b, const char *c,
                        field_state_t st)
{
    int y = list_y(row);
    uint16_t col = value_colour(st);

    gfx_text(g, L_MARGIN, y, label, L_LABEL_SC, C_LABEL);

    /* Headings and values share the same three anchors. */
    gfx_text(g, L_MARGIN, y + 12, ha, L_LABEL_SC, C_DIM);
    gfx_text_center(g, g->w / 2, y + 12, hb, L_LABEL_SC, C_DIM);
    gfx_text_right(g, g->w - L_MARGIN, y + 12, hc, L_LABEL_SC, C_DIM);

    gfx_text(g, L_MARGIN, y + 24, a, L_VALUE_SC, col);
    gfx_text_center(g, g->w / 2, y + 24, b, L_VALUE_SC, col);
    gfx_text_right(g, g->w - L_MARGIN, y + 24, c, L_VALUE_SC, col);
}

/* Format a value, or dashes of the same shape when it has never been seen. */
static void fmt(char *buf, size_t len, field_state_t st, const char *dashes, const char *spec,
                double v)
{
    if (st == FIELD_MISSING) {
        snprintf(buf, len, "%s", dashes);
    } else {
        snprintf(buf, len, spec, v);
    }
}

const char *screens_pressure(double kpa, double *out)
{
#if CONFIG_S2_DISPLAY_PRESSURE_BAR
    *out = kpa * 0.01;
    return "bar";
#elif CONFIG_S2_DISPLAY_PRESSURE_KPA
    *out = kpa;
    return "kPa";
#else
    *out = kpa * 0.1450377;   /* PSI */
    return "PSI";
#endif
}

static void render_battery_screen(gfx_t *g, const dash_data_t *d)
{
    char a[12], b[12], c[12];
    list_title(g, "BATTERY");

    fmt(a, sizeof(a), d->cell_state, "----", "%.0f", d->cell_mv_min);
    fmt(b, sizeof(b), d->cell_state, "----", "%.0f", d->cell_mv_avg);
    fmt(c, sizeof(c), d->cell_state, "----", "%.0f", d->cell_mv_max);
    list_triple(g, 0, "CELL mV", "MIN", "AVG", "MAX", a, b, c, d->cell_state);

    /* Charge-positive as the bike reports it, so a discharge reads negative. */
    fmt(a, sizeof(a), d->current_state, "---.-", "%.1f", d->pack_amps);
    list_row(g, 2, "PACK CURRENT  A", a, d->current_state);

    fmt(a, sizeof(a), d->soc_state, "--", "%.0f", d->soc_pct);
    list_row(g, 3, "STATE OF CHARGE  %", a, d->soc_state);

    if (d->soh_state == FIELD_MISSING) {
        list_row(g, 4, "STATE OF HEALTH  %", d->uds_enabled ? "--" : "n/a", d->soh_state);
    } else {
        fmt(a, sizeof(a), d->soh_state, "--", "%.0f", d->soh_pct);
        list_row(g, 4, "STATE OF HEALTH  %", a, d->soh_state);
    }
    if (!d->uds_enabled) {
        gfx_text_center(g, g->w / 2, list_y(5) + 8, "HEALTH NEEDS UDS POLLING", 1, C_DIM);
    }
}

static void render_thermal_screen(gfx_t *g, const dash_data_t *d)
{
    char a[12], b[12], c[12];
    list_title(g, "THERMAL");

    fmt(a, sizeof(a), d->pack_temp_state, "--", "%.0f", d->pack_t_min);
    fmt(b, sizeof(b), d->pack_temp_state, "--", "%.0f", d->pack_t_avg);
    fmt(c, sizeof(c), d->pack_temp_state, "--", "%.0f", d->pack_t_max);
    list_triple(g, 0, "PACK C", "MIN", "AVG", "MAX", a, b, c, d->pack_temp_state);

    fmt(a, sizeof(a), d->coolant_state, "--", "%.0f", d->coolant_c);
    list_row(g, 2, "COOLANT  C", a, d->coolant_state);

    fmt(a, sizeof(a), d->inverter_state, "--", "%.0f", d->inverter_c);
    list_row(g, 3, "INVERTER  C", a, d->inverter_state);

    fmt(a, sizeof(a), d->ambient_state, "--", "%.0f", d->ambient_c);
    list_row(g, 4, "AMBIENT  C", a, d->ambient_state);

    gfx_text_center(g, g->w / 2, list_y(5) + 8, "NO CHARGER TEMP ON THE BUS", 1, C_DIM);
}

static void render_chassis_screen(gfx_t *g, const dash_data_t *d)
{
    char a[12], b[12], c[12];
    double per_g = (double)CONFIG_S2_ACCEL_COUNTS_PER_G;
    list_title(g, "CHASSIS");

    /* Longitudinal axis: the extremes seen since boot either side of now. */
    fmt(a, sizeof(a), d->accel_state, "--.-", "%.2f", d->accel_max_neg / per_g);
    fmt(b, sizeof(b), d->accel_state, "--.-", "%.2f", d->accel_now / per_g);
    fmt(c, sizeof(c), d->accel_state, "--.-", "%.2f", d->accel_max_pos / per_g);
    list_triple(g, 0, "LONGITUDINAL ~g", "-MAX", "NOW", "+MAX", a, b, c, d->accel_state);

    fmt(a, sizeof(a), d->accel_state, "-----", "%.0f", d->accel_now);
    list_row(g, 2, "RAW COUNTS", a, d->accel_state);

    double p;
    const char *unit = screens_pressure(d->tyre_front_kpa, &p);
    char label[24];
    fmt(a, sizeof(a), d->tyre_state, "--.-", "%.1f", p);
    snprintf(label, sizeof(label), "TYRE FRONT  %s", unit);
    list_row(g, 3, label, a, d->tyre_state);

    screens_pressure(d->tyre_rear_kpa, &p);
    fmt(a, sizeof(a), d->tyre_state, "--.-", "%.1f", p);
    snprintf(label, sizeof(label), "TYRE REAR  %s", unit);
    list_row(g, 4, label, a, d->tyre_state);
}

static void render_telematics_screen(gfx_t *g, const dash_data_t *d)
{
    char a[16];
    list_title(g, "TELEMATICS");

    fmt(a, sizeof(a), d->gps_state, "---.-----", "%.5f", d->gps_lat);
    list_row(g, 0, "LATITUDE", a, d->gps_state);

    fmt(a, sizeof(a), d->gps_state, "---.-----", "%.5f", d->gps_lon);
    list_row(g, 1, "LONGITUDE", a, d->gps_state);

    fmt(a, sizeof(a), d->cell_signal_state, "---", "%.0f", d->cell_signal);
    list_row(g, 2, "CELL SIGNAL", a, d->cell_signal_state);

    list_row(g, 3, "NETWORK", d->plmn[0] ? d->plmn : "---", d->cell_signal_state);

    if (!d->uds_enabled) {
        gfx_text_center(g, g->w / 2, list_y(4) + 8, "THIS SCREEN NEEDS UDS", 1, C_DIM);
        gfx_text_center(g, g->w / 2, list_y(5) + 2, "WITH TCU POLLING ON", 1, C_DIM);
    }
}

static void render_empty_screen(gfx_t *g, unsigned screen)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "SCREEN %u", screen + 1);
    gfx_text_center(g, CX, 110, buf, 3, C_LABEL);
    gfx_text_center(g, CX, 153, "(empty)", 2, C_DIM);
    gfx_rect(g, 4, 4, 232, 272, C_TRACK);
    gfx_rect(g, 5, 5, 230, 270, C_TRACK);
}

void screens_render(gfx_t *g, unsigned screen, unsigned count, const dash_data_t *d)
{
    gfx_fill(g, C_BG);
    switch (screen) {
    case 0: render_ride_screen(g, d); break;
    case 1: render_battery_screen(g, d); break;
    case 2: render_thermal_screen(g, d); break;
    case 3: render_chassis_screen(g, d); break;
    case 4: render_telematics_screen(g, d); break;
    default: render_empty_screen(g, screen); break;
    }
    draw_dots(g, screen, count);
}


/* ------------------------------------------------------------ update mode --- */

#define C_OTA_HEAD  GFX_RGB(0xFF, 0xB0, 0x00)
#define C_OTA_PASS  GFX_RGB(0x00, 0xE0, 0xE0)
#define C_OTA_FAIL  GFX_RGB(0xFF, 0x30, 0x00)
#define C_OTA_OK    GFX_RGB(0x00, 0xE0, 0x00)

/* Update-mode rows. Full-width text, no gauge, so the layout is its own. */
#define OU_Y_TITLE   14
#define OU_Y_RULE    40
#define OU_Y_L1      54
#define OU_Y_V1      74
#define OU_Y_L2     108
#define OU_Y_V2     128
#define OU_Y_L3     166
#define OU_Y_V3     186
#define OU_Y_BAR    212
#define OU_BAR_H     22
#define OU_Y_DETAIL 246
#define OU_Y_HINT   266
#define OU_MARGIN    10

static void ota_label(gfx_t *g, int y, const char *s)
{
    gfx_text(g, OU_MARGIN, y, s, 1, C_LABEL);
}

/*
 * Draw centred text that always fits: shrink the scale first, then truncate.
 * These strings come from Kconfig and from the runtime, so they can be longer
 * than any layout assumed, and drawing off the edge is the one outcome that must
 * not happen.
 */
static void ota_value(gfx_t *g, int y, const char *s, int max_scale, uint16_t c)
{
    int avail = g->w - 2 * OU_MARGIN;
    int sc = max_scale;
    while (sc > 1 && gfx_text_width(s, sc) > avail) {
        sc--;
    }
    if (gfx_text_width(s, sc) <= avail) {
        gfx_text_center(g, g->w / 2, y, s, sc, c);
        return;
    }

    char buf[64];
    size_t n = 0;
    while (s[n] != '\0' && n + 1 < sizeof(buf)) {
        buf[n] = s[n];
        buf[n + 1] = '\0';
        if (gfx_text_width(buf, sc) > avail) {
            buf[n] = '\0';
            break;
        }
        n++;
    }
    gfx_text_center(g, g->w / 2, y, buf, sc, c);
}

void screens_render_update(gfx_t *g, const update_data_t *d)
{
    if (!g || !d) {
        return;
    }
    gfx_fill(g, C_BG);

    const char *title = "UPDATE MODE";
    uint16_t title_c = C_OTA_HEAD;
    if (d->phase == UPDATE_DONE) {
        title = "UPDATED";
        title_c = C_OTA_OK;
    } else if (d->phase == UPDATE_FAILED) {
        title = "FAILED";
        title_c = C_OTA_FAIL;
    } else if (d->phase == UPDATE_RECEIVING) {
        title = "RECEIVING";
    }
    gfx_text_center(g, g->w / 2, OU_Y_TITLE, title, 2, title_c);
    gfx_hline(g, OU_MARGIN, OU_Y_RULE, g->w - 2 * OU_MARGIN, C_TRACK);

    if (d->phase == UPDATE_WAITING || d->phase == UPDATE_FAILED) {
        /*
         * How to connect. The passphrase is generated fresh for this session and
         * never stored, so showing it here is what restricts the upload to
         * somebody standing at the bike.
         */
        ota_label(g, OU_Y_L1, "WI-FI NETWORK");
        ota_value(g, OU_Y_V1, d->ssid[0] ? d->ssid : "-", 3, C_VALUE);
        ota_label(g, OU_Y_L2, "PASSWORD");
        ota_value(g, OU_Y_V2, d->pass[0] ? d->pass : "-", 4, C_OTA_PASS);
        ota_label(g, OU_Y_L3, "THEN OPEN");
        ota_value(g, OU_Y_V3, d->ip[0] ? d->ip : "-", 3, C_VALUE);
    } else {
        /* Progress. A bar plus a percentage, or bytes when the size is unknown. */
        char line[32];
        int pct = -1;
        if (d->total && d->received <= d->total) {
            pct = (int)((uint64_t)d->received * 100u / d->total);
        }
        if (pct >= 0) {
            snprintf(line, sizeof(line), "%d%%", pct);
        } else {
            snprintf(line, sizeof(line), "%lu KB", (unsigned long)(d->received / 1024u));
        }
        ota_value(g, OU_Y_V1, line, 5, C_VALUE);

        snprintf(line, sizeof(line), "%lu of %lu KB", (unsigned long)(d->received / 1024u),
                 (unsigned long)(d->total / 1024u));
        if (d->total) {
            ota_value(g, OU_Y_V2 + 18, line, 1, C_LABEL);
        }

        int bw = g->w - 2 * OU_MARGIN;
        gfx_rect(g, OU_MARGIN, OU_Y_BAR, bw, OU_BAR_H, C_TRACK);
        if (pct > 0) {
            int fill = (bw - 4) * pct / 100;
            if (fill > 0) {
                gfx_fill_rect(g, OU_MARGIN + 2, OU_Y_BAR + 2, fill, OU_BAR_H - 4,
                              d->phase == UPDATE_DONE ? C_OTA_OK : C_OTA_HEAD);
            }
        }
    }

    if (d->detail[0]) {
        ota_value(g, OU_Y_DETAIL, d->detail, 1,
                  d->phase == UPDATE_FAILED ? C_OTA_FAIL : C_LABEL);
    }

    const char *hint = "HOLD BUTTON TO CANCEL";
    if (d->phase == UPDATE_DONE) {
        hint = "REBOOTING";
    } else if (d->phase == UPDATE_RECEIVING) {
        hint = "DO NOT POWER OFF";
    }
    gfx_text_center(g, g->w / 2, OU_Y_HINT, hint, 1, C_DIM);
}

#endif /* CONFIG_S2_DISPLAY_ENABLE */
