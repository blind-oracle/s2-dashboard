#include "display.h"

#include <math.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_writer.h"
#include "panel.h"
#include "s2_dbc_gen.h"
#include "screens.h"
#include "sdkconfig.h"
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

/* Freshness windows: instantaneous values go stale fast, slower frames later. */
#define STALE_FAST_US    2000000LL
#define STALE_ENERGY_US  5000000LL     /* 0x186 runs at ~1.8 Hz */
#define STALE_SOH_US   300000000LL

/*
 * The window a 0x181 pair must fall in to be believable, from the database's
 * notes: ~400 V nominal, sagging to ~300 V at the 240 A peak, and charging never
 * more than a few tens of amps. Outside this is a decode error, not a reading.
 */
#define V_MIN 200.0
#define V_MAX 430.0
#define I_MIN (-400.0)
#define I_MAX 50.0

static inline bool pack_sample_plausible(double volts, double amps)
{
    return volts >= V_MIN && volts <= V_MAX && amps >= I_MIN && amps <= I_MAX;
}

/* Torque outside this is a decode error, not a reading (observed -280..+1249). */
#define TORQUE_ABS_MAX 1500.0

/*
 * The button is polled from the render loop, whose period is the tick plus
 * however long rendering and the (synchronous) flush took. All button timing is
 * therefore wall-clock, never a count of iterations.
 */
#define TICK_MS 10
#define DEBOUNCE_US 30000LL

#define TORQUE_PER_NM ((double)CONFIG_S2_TORQUE_COUNTS_PER_NM_X100 / 100.0)

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

#if CONFIG_S2_DISPLAY_BUTTON_GPIO >= 0
static esp_err_t button_init(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << CONFIG_S2_DISPLAY_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
#if CONFIG_S2_DISPLAY_BUTTON_ACTIVE_LOW
        .pull_up_en = GPIO_PULLUP_ENABLE,
#else
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
#endif
    };
    return gpio_config(&cfg);
}

static bool button_is_down(void)
{
    int level = gpio_get_level(CONFIG_S2_DISPLAY_BUTTON_GPIO);
#if CONFIG_S2_DISPLAY_BUTTON_ACTIVE_LOW
    return level == 0;
#else
    return level != 0;
#endif
}

/*
 * Polled debounce: a state only counts after DEBOUNCE_TICKS consecutive equal
 * samples. A long press fires the moment the threshold is crossed while the
 * button is still down; a short press fires on release if no long press did.
 * A button already held at boot is handled correctly because the debounced
 * state starts at "up" only after the first stable samples.
 */
static void button_poll(void)
{
    static bool stable_down;
    static bool last_sample;
    static int64_t sample_since_us;

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
        if (!stable_down) {
            display_next_screen();      /* act on release, so a held button does not repeat */
            log_tline("display: screen %u/%d", s_screen + 1, CONFIG_S2_DISPLAY_SCREENS);
        }
    }
}
#else
static esp_err_t button_init(void) { return ESP_OK; }
static void button_poll(void) {}
#endif

/* ---------------------------------------------------------------- sampling --- */

static field_state_t freshness(bool valid, int64_t ts_us, int64_t now, int64_t window)
{
    if (!valid) {
        return FIELD_MISSING;
    }
    return (now - ts_us) > window ? FIELD_STALE : FIELD_LIVE;
}

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

static void snapshot(dash_data_t *d, int64_t now, double dt_s)
{
    memset(d, 0, sizeof(*d));
    d->uds_enabled = OPT_UDS;

    double volts = 0, amps = 0, torque_counts = 0;
    bool v_ok = false, i_ok = false, t_ok = false;
    int64_t v_ts = 0, i_ts = 0, t_ts = 0;
    double used_wh = 0;
    bool energy_seen = false;
    int64_t energy_ts = 0;
    bool soh_ok = false;
    double soh = 0;
    int64_t soh_ts = 0;

    vs_lock();
    const vs_signal_t *sv = vs_signal(S2_SIG_BATTERY_STATUS_181_pack_voltage);
    const vs_signal_t *si = vs_signal(S2_SIG_BATTERY_STATUS_181_pack_current);
    const vs_signal_t *st = vs_signal(S2_SIG_MOTOR_POWER_161_torque_delivered);
    const vs_signal_t *se = vs_signal(S2_SIG_HV_ENERGY_186_trip_energy_consumed_wh);
    if (sv && sv->valid) {
        volts = sv->value;
        v_ts = sv->ts_us;
    }
    if (si && si->valid) {
        amps = si->value;
        i_ts = si->ts_us;
    }
    /*
     * One plausibility rule for the gauge and the accumulator, so they can never
     * disagree about whether a sample was real.
     */
    bool pair_ok = sv && si && sv->valid && si->valid && pack_sample_plausible(volts, amps);
    v_ok = pair_ok;
    i_ok = pair_ok;
    if (st && st->valid) {
        torque_counts = st->value;
        t_ts = st->ts_us;
        t_ok = torque_counts >= -TORQUE_ABS_MAX && torque_counts <= TORQUE_ABS_MAX;
    }
    if (se && se->valid) {
        used_wh = se->value;          /* the bike's own trip meter, ~1 Wh/count */
        energy_ts = se->ts_us;
        energy_seen = true;
    }
    const vs_uds_t *u = vs_uds();
    soh_ok = u->soh_valid;
    soh = u->soh_pct;
    soh_ts = u->soh_ts_us;
    vs_unlock();

    /* Power: the pack current is charge-positive, so power out is -(V * I). */
    bool power_ok = v_ok && i_ok;
    int64_t power_ts = v_ts < i_ts ? v_ts : i_ts;
    d->power_state = freshness(power_ok, power_ts, now, STALE_FAST_US);
    if (power_ok) {
        double raw_kw = -(volts * amps) / 1000.0;
        d->power_kw = filter(&s_power_filt, &s_power_filt_valid, raw_kw, dt_s, 0.2);
    }

    d->torque_state = freshness(t_ok, t_ts, now, STALE_FAST_US);
    if (t_ok) {
        d->torque_counts = filter(&s_torque_filt, &s_torque_filt_valid, torque_counts, dt_s, 0.1);
        d->torque_nm = d->torque_counts / TORQUE_PER_NM;
    }

    d->volts_state = freshness(v_ok, v_ts, now, STALE_FAST_US);
    d->volts = volts;

    d->energy_state = freshness(energy_seen, energy_ts, now, STALE_ENERGY_US);
    d->used_kwh = used_wh / 1000.0;

    d->soh_state = freshness(soh_ok, soh_ts, now, STALE_SOH_US);
    d->soh_pct = soh;
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

            dash_data_t data;
            snapshot(&data, now, dt_s);
            screens_render(panel_gfx(), s_screen, (unsigned)CONFIG_S2_DISPLAY_SCREENS, &data);
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
