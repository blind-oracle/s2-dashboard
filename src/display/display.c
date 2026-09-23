#include "display.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_writer.h"
#include "ota.h"
#include "panel.h"
#include "ride_limits.h"
#include "s2_dbc_gen.h"
#include "screens.h"
#include "sdkconfig.h"
#include "vehicle_button.h"
#include "vehicle_state.h"

#if !CONFIG_S2_DISPLAY_ENABLE

/* Display disabled at build time: keep the API linkable and inert. */
esp_err_t display_start(void) { return ESP_ERR_NOT_SUPPORTED; }
unsigned display_current_screen(void) { return 0; }
void display_next_screen(void) {}

#else

static const char *TAG = "display";

#ifdef CONFIG_S2_UDS_ENABLE
#define OPT_UDS 1
#else
#define OPT_UDS 0
#endif

/*
 * Kconfig drops an int symbol entirely when its "depends on" is unmet, so the
 * brightness is only defined when the backlight is PWM-driven.
 */
#ifdef CONFIG_S2_DISPLAY_BRIGHTNESS
#define OPT_BRIGHTNESS CONFIG_S2_DISPLAY_BRIGHTNESS
#else
#define OPT_BRIGHTNESS 100
#endif
#ifdef CONFIG_S2_DISPLAY_BUTTON_GPIO
#define OPT_BUTTON_GPIO CONFIG_S2_DISPLAY_BUTTON_GPIO
#else
#define OPT_BUTTON_GPIO (-1)   /* the int is not emitted unless the pin is selected */
#endif
#ifdef CONFIG_S2_DISPLAY_BUTTON_ACTIVE_LOW
#define OPT_BUTTON_ACTIVE_LOW 1
#else
#define OPT_BUTTON_ACTIVE_LOW 0
#endif
#ifdef CONFIG_S2_DISPLAY_BUTTON_VEHICLE
#define OPT_BUTTON_VEHICLE 1
#else
#define OPT_BUTTON_VEHICLE 0
#endif
/*
 * Hold time that enters update mode. The int is not emitted when OTA is off, and
 * a threshold of 0 disables long-press detection entirely.
 */
#ifdef CONFIG_S2_OTA_LONG_PRESS_MS
#define OPT_LONG_PRESS_US ((int64_t)CONFIG_S2_OTA_LONG_PRESS_MS * 1000)
#else
#define OPT_LONG_PRESS_US 0
#endif

/*
 * The button is polled from the render loop, whose period is the tick plus
 * however long rendering and the (synchronous) flush took. All button timing is
 * therefore wall-clock, never a count of iterations.
 */
#define TICK_MS 10
#define DEBOUNCE_US 30000LL

static unsigned s_screen;
static double s_power_filt;
static bool s_power_filt_valid;
static double s_torque_filt;
static bool s_torque_filt_valid;

unsigned display_current_screen(void)
{
    return s_screen;
}

void display_next_screen(void)
{
    s_screen = (s_screen + 1) % (unsigned)CONFIG_S2_DISPLAY_SCREENS;
}

/* ------------------------------------------------------------------ button --- */

#if OPT_BUTTON_VEHICLE || OPT_BUTTON_GPIO >= 0
/*
 * A long hold toggles update mode. Entering it reboots, so this does not return
 * on success; ota_request_update_mode() logs its own reason when it refuses,
 * which it does while the bike is moving or while a new image is unconfirmed.
 */
static void button_long_press(void)
{
    if (ota_update_mode_active()) {
        log_tline("display: leaving update mode");
        ota_leave_update_mode();
    } else {
        ota_request_update_mode();
    }
}
#endif

/*
 * Screens are cycled either by one of the motorcycle's own handlebar buttons,
 * seen on the broadcast bus, or by a switch wired to a GPIO pin. The vehicle
 * button is the default: it needs no wiring and the dash info/scroll button is
 * the one the rider already uses to page through the instrument cluster.
 */

#if OPT_BUTTON_VEHICLE

static vbtn_state_t s_vbtn;

/*
 * Bound on how many screens one poll may advance. A genuine press yields one,
 * and a press that fell entirely between two polls also yields one, so this only
 * ever trims a signal that is flapping far faster than a human can press, such
 * as a selected control that turns out to follow a blink rather than a switch.
 */
#define MAX_ADVANCE_PER_POLL 4

/* Nothing to set up: the button arrives on the bus. main.c logs which one. */
static esp_err_t button_init(void) { return ESP_OK; }

static void button_poll(void)
{
    const vbtn_def_t *def = vbtn_selected();
    if (!def) {
        return;
    }

    /* A short lock that only copies scalars out, as everywhere else here. */
    bool valid;
    uint64_t raw;
    uint32_t changes;
    vs_lock();
    const vs_signal_t *sig = vs_signal(def->signal);
    valid = sig && sig->valid;
    raw = valid ? sig->raw : 0;
    changes = valid ? sig->changes : 0;
    vs_unlock();

    vbtn_event_t ev = vbtn_feed(&s_vbtn, def, valid, raw, changes, esp_timer_get_time(),
                                OPT_LONG_PRESS_US);
    if (ev.long_press) {
        button_long_press();
        return;
    }
    /* While update mode is up, only the hold gesture is live. */
    if (ota_update_mode_active()) {
        return;
    }
    unsigned presses = ev.presses;
    if (presses > MAX_ADVANCE_PER_POLL) {
        presses = MAX_ADVANCE_PER_POLL;
    }
    for (unsigned i = 0; i < presses; i++) {
        display_next_screen();
    }
    if (presses) {
        log_tline("display: screen %u/%d (%s)", s_screen + 1, CONFIG_S2_DISPLAY_SCREENS, def->label);
    }
}

#elif OPT_BUTTON_GPIO >= 0

static esp_err_t button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << OPT_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
#if OPT_BUTTON_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
#else
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
    };
    return gpio_config(&cfg);
}

static bool button_is_down(void)
{
    int level = gpio_get_level(OPT_BUTTON_GPIO);
#if OPT_BUTTON_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

/*
 * Polled debounce: a state only counts once it has held for DEBOUNCE_US. The
 * screen advances on release, so a held button does not repeat. A button already
 * down at boot does not fire, because the debounced state starts released and
 * only a release transition acts.
 */
static void button_poll(void)
{
    static bool stable_down;
    static bool last_sample;
    static int64_t sample_since_us;
    static int64_t down_since_us;
    static bool long_fired;

    /*
     * Debounced on wall-clock time, not on a count of polls: this runs from the
     * render loop, whose period is the tick plus however long the frame took.
     */
    int64_t now = esp_timer_get_time();
    bool sample = button_is_down();
    if (sample != last_sample) {
        last_sample = sample;
        sample_since_us = now;
    }
    if (stable_down != sample && (now - sample_since_us) >= DEBOUNCE_US) {
        stable_down = sample;
        if (stable_down) {
            down_since_us = now;
            long_fired = false;
        } else if (long_fired) {
            long_fired = false;         /* the hold already acted; not a screen change */
        } else if (!ota_update_mode_active()) {
            display_next_screen();
            log_tline("display: screen %u/%d", s_screen + 1, CONFIG_S2_DISPLAY_SCREENS);
        }
    }

    /* Same hold gesture as the handlebar button, fired once per hold. */
    if (stable_down && !long_fired && OPT_LONG_PRESS_US > 0 &&
        (now - down_since_us) >= OPT_LONG_PRESS_US) {
        long_fired = true;
        button_long_press();
    }
}

#else

static esp_err_t button_init(void) { return ESP_OK; }
static void button_poll(void) {}

#endif

/* ---------------------------------------------------------------- sampling --- */

/* First-order low-pass, so the digits do not churn on sensor noise. */
static double filter(double *state, bool *have, double raw, double dt_s, double tau_s)
{
    if (!*have) {
        *have = true;
        *state = raw;
    } else {
        double alpha = dt_s / (tau_s + dt_s);
        *state += (raw - *state) * alpha;
    }
    return *state;
}

/*
 * The 0x163 temperature mux cycles one selector per frame at 50 Hz, and the
 * decoder only ever holds the most recent pair. Polling it from here at the
 * refresh rate therefore catches each channel now and then rather than every
 * time, so the two channels worth reading are latched with their own
 * timestamps. RIDE_STALE_MUX_US is generous for that reason.
 */
static double s_coolant_c, s_inverter_c;
static int64_t s_coolant_ts, s_inverter_ts;
static bool s_coolant_seen, s_inverter_seen;

/* Longitudinal extremes since boot. */
static double s_accel_max_pos, s_accel_max_neg;
static bool s_accel_seen;

/* min/avg/max across however many of the three pack sensors are reporting. */
static unsigned temp_stats(const double *v, const bool *ok, unsigned n, double *mn, double *av,
                           double *mx)
{
    unsigned count = 0;
    double sum = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!ok[i]) {
            continue;
        }
        if (count == 0 || v[i] < *mn) {
            *mn = v[i];
        }
        if (count == 0 || v[i] > *mx) {
            *mx = v[i];
        }
        sum += v[i];
        count++;
    }
    if (count) {
        *av = sum / count;
    }
    return count;
}

static void snapshot(dash_data_t *d, int64_t now, double dt_s)
{
    memset(d, 0, sizeof(*d));
    d->uds_enabled = OPT_UDS;

    /* ---- one short locked region: copy scalars out, derive nothing here ---- */
    double volts = 0, amps = 0, torque_counts = 0, used_wh = 0, soh = 0, rpm = 0;
    bool v_ok = false, i_ok = false, t_ok = false;
    int64_t v_ts = 0, i_ts = 0, t_ts = 0, rpm_ts = 0;
    bool energy_seen = false;
    int64_t energy_ts = 0;
    bool rpm_seen = false;
    bool soh_ok = false;
    int64_t soh_ts = 0;

    double cmin = 0, cavg = 0, cmax = 0;
    bool cell_ok = false;
    int64_t cell_ts = 0;
    double soc = 0;
    bool soc_ok = false;
    int64_t soc_ts = 0;

    double bt[3] = { 0, 0, 0 };
    bool bt_ok[3] = { false, false, false };
    int64_t bt_ts = 0;
    double ambient = 0;
    bool amb_ok = false;
    int64_t amb_ts = 0;
    double mux_sel = 0, mux_raw = 0;
    bool mux_ok = false;
    int64_t mux_ts = 0;

    double accel = 0;
    bool acc_ok = false;
    int64_t acc_ts = 0;
    double tyre_f = 0, tyre_r = 0;
    bool tyre_ok = false;
    int64_t tyre_ts = 0;

    bool gps_ok = false;
    double lat = 0, lon = 0;
    int64_t gps_ts = 0;
    bool sig_ok = false;
    double sig = 0;
    int64_t sig_ts = 0;
    char plmn[VS_PLMN_MAX] = { 0 };

    vs_lock();
    const vs_signal_t *sv = vs_signal(S2_SIG_BATTERY_STATUS_181_pack_voltage);
    const vs_signal_t *si = vs_signal(S2_SIG_BATTERY_STATUS_181_pack_current);
    const vs_signal_t *st = vs_signal(S2_SIG_MOTOR_POWER_161_torque_delivered);
    const vs_signal_t *se = vs_signal(S2_SIG_HV_ENERGY_186_trip_energy_consumed_wh);
    const vs_signal_t *sr = vs_signal(S2_SIG_SPEED_160_motor_rpm);
    if (sv && sv->valid) {
        volts = sv->value;
        v_ts = sv->ts_us;
    }
    if (si && si->valid) {
        amps = si->value;
        i_ts = si->ts_us;
    }
    bool pair_ok = sv && si && sv->valid && si->valid && ride_pack_sample_plausible(volts, amps);
    v_ok = pair_ok;
    i_ok = pair_ok;
    if (st && st->valid) {
        torque_counts = st->value;
        t_ts = st->ts_us;
        t_ok = ride_torque_plausible(torque_counts);
    }
    if (se && se->valid) {
        used_wh = se->value;
        energy_ts = se->ts_us;
        energy_seen = true;
    }
    if (sr && sr->valid) {
        rpm = sr->value;
        rpm_ts = sr->ts_us;
        rpm_seen = true;
    }

    const vs_signal_t *c1 = vs_signal(S2_SIG_CELL_VOLTAGE_182_cell_v_min);
    const vs_signal_t *c2 = vs_signal(S2_SIG_CELL_VOLTAGE_182_cell_v_avg);
    const vs_signal_t *c3 = vs_signal(S2_SIG_CELL_VOLTAGE_182_cell_v_max);
    if (c1 && c2 && c3 && c1->valid && c2->valid && c3->valid) {
        cmin = c1->value;
        cavg = c2->value;
        cmax = c3->value;
        cell_ok = true;
        cell_ts = c1->ts_us < c3->ts_us ? c1->ts_us : c3->ts_us;
    }
    const vs_signal_t *ss = vs_signal(S2_SIG_SOC_185_state_of_charge);
    if (ss && ss->valid) {
        soc = ss->value;
        soc_ts = ss->ts_us;
        soc_ok = true;
    }

    const uint16_t temp_ids[3] = { S2_SIG_TEMPERATURES_183_batt_temp_1,
                                   S2_SIG_TEMPERATURES_183_batt_temp_2,
                                   S2_SIG_TEMPERATURES_183_batt_temp_3 };
    for (unsigned k = 0; k < 3; k++) {
        const vs_signal_t *tp = vs_signal(temp_ids[k]);
        if (tp && tp->valid) {
            bt[k] = tp->value;
            bt_ok[k] = true;
            if (tp->ts_us > bt_ts) {
                bt_ts = tp->ts_us;
            }
        }
    }
    const vs_signal_t *sa = vs_signal(S2_SIG_TEMPERATURES_183_ambient_temp);
    if (sa && sa->valid) {
        ambient = sa->value;
        amb_ts = sa->ts_us;
        amb_ok = true;
    }
    const vs_signal_t *ms = vs_signal(S2_SIG_BATTERY_POWER_163_temp_mux_sel);
    const vs_signal_t *mr = vs_signal(S2_SIG_BATTERY_POWER_163_temp_mux_raw);
    if (ms && mr && ms->valid && mr->valid) {
        mux_sel = ms->value;
        mux_raw = mr->value;
        mux_ts = mr->ts_us;
        mux_ok = true;
    }

    const vs_signal_t *ax = vs_signal(S2_SIG_IMU_ACCEL_122_accel_longitudinal);
    if (ax && ax->valid) {
        accel = ax->value;
        acc_ts = ax->ts_us;
        acc_ok = true;
    }
    const vs_signal_t *tf = vs_signal(S2_SIG_TYRE_PRESSURE_33A_tyre_pressure_front);
    const vs_signal_t *tr = vs_signal(S2_SIG_TYRE_PRESSURE_33A_tyre_pressure_rear);
    if (tf && tr && tf->valid && tr->valid) {
        tyre_f = tf->value;
        tyre_r = tr->value;
        tyre_ok = true;
        tyre_ts = tf->ts_us < tr->ts_us ? tf->ts_us : tr->ts_us;
    }

    const vs_uds_t *u = vs_uds();
    soh_ok = u->soh_valid;
    soh = u->soh_pct;
    soh_ts = u->soh_ts_us;
    gps_ok = u->gps_valid;
    lat = u->gps_lat;
    lon = u->gps_lon;
    gps_ts = u->gps_ts_us;
    sig_ok = u->cell_signal_valid;
    sig = u->cell_signal;
    sig_ts = u->cell_signal_ts_us;
    memcpy(plmn, u->plmn, sizeof(plmn));
    vs_unlock();

    /* ------------------------ everything below is derivation ------------------------ */

    bool power_ok = v_ok && i_ok;
    int64_t power_ts = v_ts < i_ts ? v_ts : i_ts;
    d->power_state = ride_freshness(power_ok, power_ts, now, RIDE_STALE_FAST_US);
    if (power_ok) {
        double raw_kw = ride_power_kw(volts, amps);
        d->power_kw = filter(&s_power_filt, &s_power_filt_valid, raw_kw, dt_s, 0.2);
    }

    d->torque_state = ride_freshness(t_ok, t_ts, now, RIDE_STALE_FAST_US);
    if (t_ok) {
        d->torque_counts = filter(&s_torque_filt, &s_torque_filt_valid, torque_counts, dt_s, 0.1);
        d->torque_nm = ride_torque_nm(d->torque_counts);
    }

    d->volts_state = ride_freshness(v_ok, v_ts, now, RIDE_STALE_FAST_US);
    d->volts = volts;

    d->energy_state = ride_freshness(energy_seen, energy_ts, now, RIDE_STALE_ENERGY_US);
    d->used_kwh = used_wh / 1000.0;

    d->rpm_state = ride_freshness(rpm_seen, rpm_ts, now, RIDE_STALE_FAST_US);
    d->motor_rpm = rpm;

    /* ---- battery ---- */
    d->cell_state = ride_freshness(cell_ok, cell_ts, now, RIDE_STALE_SLOW_US);
    d->cell_mv_min = cmin;
    d->cell_mv_avg = cavg;
    d->cell_mv_max = cmax;

    d->current_state = ride_freshness(i_ok, i_ts, now, RIDE_STALE_FAST_US);
    d->pack_amps = amps;

    d->soc_state = ride_freshness(soc_ok, soc_ts, now, RIDE_STALE_SLOW_US);
    d->soc_pct = soc;

    d->soh_state = ride_freshness(soh_ok, soh_ts, now, RIDE_STALE_SOH_US);
    d->soh_pct = soh;

    /* ---- thermal ---- */
    double tmin = 0, tavg = 0, tmax = 0;
    unsigned n_temps = temp_stats(bt, bt_ok, 3, &tmin, &tavg, &tmax);
    d->pack_temp_state = ride_freshness(n_temps > 0, bt_ts, now, RIDE_STALE_SLOW_US);
    d->pack_t_min = tmin;
    d->pack_t_avg = tavg;
    d->pack_t_max = tmax;

    /*
     * Latch the two mux channels that carry real readings. Selectors 2 to 5 are
     * a constant limit table in every capture, so they are ignored rather than
     * shown as temperatures.
     */
    if (mux_ok) {
        int sel = (int)(mux_sel + 0.5);
        if (sel == 0) {
            s_inverter_c = mux_raw;
            s_inverter_ts = mux_ts;
            s_inverter_seen = true;
        } else if (sel == 1) {
            s_coolant_c = mux_raw;
            s_coolant_ts = mux_ts;
            s_coolant_seen = true;
        }
    }
    d->coolant_state = ride_freshness(s_coolant_seen, s_coolant_ts, now, RIDE_STALE_MUX_US);
    d->coolant_c = s_coolant_c;
    d->inverter_state = ride_freshness(s_inverter_seen, s_inverter_ts, now, RIDE_STALE_MUX_US);
    d->inverter_c = s_inverter_c;

    d->ambient_state = ride_freshness(amb_ok, amb_ts, now, RIDE_STALE_SLOW_US);
    d->ambient_c = ambient;

    /* ---- chassis ---- */
    if (acc_ok) {
        if (!s_accel_seen) {
            s_accel_seen = true;
            s_accel_max_pos = accel;
            s_accel_max_neg = accel;
        }
        if (accel > s_accel_max_pos) {
            s_accel_max_pos = accel;
        }
        if (accel < s_accel_max_neg) {
            s_accel_max_neg = accel;
        }
    }
    d->accel_state = ride_freshness(acc_ok, acc_ts, now, RIDE_STALE_FAST_US);
    d->accel_now = accel;
    d->accel_max_pos = s_accel_max_pos;
    d->accel_max_neg = s_accel_max_neg;

    d->tyre_state = ride_freshness(tyre_ok, tyre_ts, now, RIDE_STALE_TYRE_US);
    d->tyre_front_kpa = tyre_f;
    d->tyre_rear_kpa = tyre_r;

    /* ---- telematics ---- */
    d->gps_state = ride_freshness(gps_ok, gps_ts, now, RIDE_STALE_GPS_US);
    d->gps_lat = lat;
    d->gps_lon = lon;
    d->cell_signal_state = ride_freshness(sig_ok, sig_ts, now, RIDE_STALE_SOH_US);
    d->cell_signal = sig;
    snprintf(d->plmn, sizeof(d->plmn), "%s", plmn);
    d->plmn[sizeof(d->plmn) - 1] = '\0';
}

/* -------------------------------------------------------------------- task --- */

static void display_task(void *arg)
{
    (void)arg;
    const int64_t period_us = 1000000LL / CONFIG_S2_DISPLAY_REFRESH_HZ;
    int64_t next_render = esp_timer_get_time();
    int64_t last_render = next_render;
    bool first_frame = true;

    for (;;) {
        button_poll();

        int64_t now = esp_timer_get_time();
        if (now >= next_render) {
            double dt_s = (double)(now - last_render) / 1e6;
            if (dt_s <= 0.0 || dt_s > 1.0) {
                dt_s = (double)period_us / 1e6;
            }
            last_render = now;
            next_render = now + period_us;

            if (ota_update_mode_active()) {
                update_data_t ud;
                ota_get_update_data(&ud);
                screens_render_update(panel_gfx(), &ud);
            } else {
                dash_data_t data;
                snapshot(&data, now, dt_s);
                screens_render(panel_gfx(), s_screen, (unsigned)CONFIG_S2_DISPLAY_SCREENS, &data);
            }
            esp_err_t err = panel_flush();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "flush failed: %s", esp_err_to_name(err));
            } else if (first_frame) {
                first_frame = false;
                panel_set_brightness(OPT_BRIGHTNESS);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));
    }
}

esp_err_t display_start(void)
{
    ESP_RETURN_ON_ERROR(panel_init(), TAG, "panel init failed");
    ESP_RETURN_ON_ERROR(button_init(), TAG, "button init failed");
    /* Core 0: the CAN decoder owns core 1, and the SPI ISR is pinned here too. */
    if (xTaskCreatePinnedToCore(display_task, "display", 5120, NULL, 4, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#endif /* CONFIG_S2_DISPLAY_ENABLE */
