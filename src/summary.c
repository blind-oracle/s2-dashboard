#include "summary.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "can_bus.h"
#include "dbc_decode.h"
#include "decoder.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_writer.h"
#include "ride_limits.h"
#include "s2_overlay.h"
#include "sdkconfig.h"
#include "uds_client.h"
#include "vehicle_state.h"

#define STALE_US 5000000LL

typedef struct {
    char buf[LOG_LINE_MAX];
    size_t pos;
    int64_t now;
} line_t;

static void line_reset(line_t *l, int64_t now)
{
    l->buf[0] = '\0';
    l->pos = 0;
    l->now = now;
}

static void line_add(line_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void line_add(line_t *l, const char *fmt, ...)
{
    if (l->pos >= sizeof(l->buf) - 1) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(l->buf + l->pos, sizeof(l->buf) - l->pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        l->pos += (size_t)n;
        if (l->pos > sizeof(l->buf) - 1) {
            l->pos = sizeof(l->buf) - 1;
        }
    }
}

static void line_flush(line_t *l)
{
    if (l->pos) {
        log_line("%s", l->buf);
    }
    l->pos = 0;
    l->buf[0] = '\0';
}

/* Append " label=value unit[*]" for a valid signal; nothing when never seen. */
static void add_sig(line_t *l, const char *label, uint16_t idx)
{
    const vs_signal_t *s = vs_signal(idx);
    const s2_signal_def_t *def = s2_dbc_signal(idx);
    if (!s || !def || !s->valid) {
        return;
    }
    char val[32];
    s2_format_value(def, s->value, val, sizeof(val));
    const char *stale = (l->now - s->ts_us) > STALE_US ? "*" : "";
    const char *desc = s2_overlay_describe(idx, s->raw);
    const char *mark = s2_overlay_confidence(idx) == S2_CONF_TENTATIVE ? "?" : "";
    if (desc) {
        line_add(l, "  %s=%s%s(%s)%s", label, val, mark, desc, stale);
    } else if (def->unit[0]) {
        line_add(l, "  %s=%s%s %s%s", label, val, mark, def->unit, stale);
    } else if (def->length <= 8 && def->factor == 1.0 && def->offset == 0.0) {
        line_add(l, "  %s=0x%02llX%s%s", label, (unsigned long long)s->raw, mark, stale);
    } else {
        line_add(l, "  %s=%s%s%s", label, val, mark, stale);
    }
}

static double sig_value(uint16_t idx, bool *valid)
{
    const vs_signal_t *s = vs_signal(idx);
    *valid = s && s->valid;
    return *valid ? s->value : 0.0;
}

static void print_summary(int64_t now)
{
    can_bus_stats_t bus;
    decoder_stats_t dec;
    can_bus_get_stats(&bus);
    decoder_get_stats(&dec);

    static uint32_t prev_frames;
    static int64_t prev_now;
    double fps = 0;
    if (prev_now) {
        fps = (double)(dec.frames - prev_frames) * 1e6 / (double)(now - prev_now);
    }
    prev_frames = dec.frames;
    prev_now = now;

    line_t l;
    line_reset(&l, now);
    line_add(&l, "=== t=%lld.%01llds  bus %.0f fps (%lu frames, %lu unknown-id, %lu diag)  state %s  bus-err %lu/%lu  rxq-drop %lu  log-drop %lu",
             (long long)(now / 1000000), (long long)((now / 100000) % 10), fps, (unsigned long)dec.frames,
             (unsigned long)dec.unknown, (unsigned long)dec.diag, can_bus_state_name(bus.state),
             (unsigned long)bus.bus_errors, (unsigned long)bus.hal_bus_errors,
             (unsigned long)bus.rx_queue_full, (unsigned long)log_writer_dropped());
    line_flush(&l);
    line_add(&l, "    E2E: crc-fail %lu  alive-gaps %lu (~%lu frames lost)  dlc-mismatch %lu",
             (unsigned long)dec.crc_fail, (unsigned long)dec.alive_gaps, (unsigned long)dec.frames_lost,
             (unsigned long)dec.dlc_mismatch);
#if CONFIG_S2_UDS_ENABLE
    uds_stats_t uds;
    uds_client_get_stats(&uds);
    line_add(&l, "  UDS: req %lu ok %lu neg %lu(%lu?) t/o %lu tx-err %lu tp-err %lu backoff %lu rounds %lu",
             (unsigned long)uds.requests, (unsigned long)uds.positive, (unsigned long)uds.negative,
             (unsigned long)uds.unattributed_nrc, (unsigned long)uds.timeouts, (unsigned long)uds.tx_errors,
             (unsigned long)uds.transport_errors, (unsigned long)uds.backoffs, (unsigned long)uds.rounds);
#endif
    line_flush(&l);

    vs_lock();
    if (dec.known == 0) {
        vs_unlock();
        log_line("    (no database frames decoded yet - check wiring, bitrate and that the bike is awake)");
        return;
    }

    line_add(&l, " motion:");
    add_sig(&l, "speed", S2_SIG_SPEED_160_vehicle_speed);
    add_sig(&l, "rpm", S2_SIG_SPEED_160_motor_rpm);
    add_sig(&l, "wheelF", S2_SIG_WHEEL_SPEEDS_326_wheel_speed_front);
    add_sig(&l, "wheelR", S2_SIG_WHEEL_SPEEDS_326_wheel_speed_rear);
    add_sig(&l, "standstill", S2_SIG_STANDSTILL_FLAG_12A_standstill);
    add_sig(&l, "cruise", S2_SIG_SPEED_160_cruise_setpoint);
    add_sig(&l, "cruise_armed", S2_SIG_SPEED_160_cruise_armed);
    line_flush(&l);

    line_add(&l, " drive:");
    add_sig(&l, "throttle", S2_SIG_MOTOR_POWER_161_throttle_request);
    add_sig(&l, "torque_req", S2_SIG_MOTOR_POWER_161_torque_request);
    add_sig(&l, "torque_del", S2_SIG_MOTOR_POWER_161_torque_delivered);
    add_sig(&l, "motor_status", S2_SIG_MOTOR_POWER_161_motor_status_d6);
    add_sig(&l, "energy_dir", S2_SIG_DRIVETRAIN_162_energy_direction);
    add_sig(&l, "drive_state", S2_SIG_PROPULSION_STATUS_184_drive_state_184);
    add_sig(&l, "tc", S2_SIG_PROPULSION_STATUS_184_tc_intervention_184);
    line_flush(&l);

    line_add(&l, " battery:");
    add_sig(&l, "soc", S2_SIG_SOC_185_state_of_charge);
    bool vv, iv;
    double v = sig_value(S2_SIG_BATTERY_STATUS_181_pack_voltage, &vv);
    double i = sig_value(S2_SIG_BATTERY_STATUS_181_pack_current, &iv);
    add_sig(&l, "pack", S2_SIG_BATTERY_STATUS_181_pack_voltage);
    add_sig(&l, "current", S2_SIG_BATTERY_STATUS_181_pack_current);
    /* Same helper and same plausibility rule as the screen and BLE, so the log
     * cannot print a power figure the screen refuses to show. */
    if (vv && iv && ride_pack_sample_plausible(v, i)) {
        line_add(&l, "  power_out=%.2f kW", ride_power_kw(v, i));
    }
    add_sig(&l, "packV_163", S2_SIG_BATTERY_POWER_163_pack_voltage_163);
    add_sig(&l, "cell_min", S2_SIG_CELL_VOLTAGE_182_cell_v_min);
    add_sig(&l, "cell_avg", S2_SIG_CELL_VOLTAGE_182_cell_v_avg);
    add_sig(&l, "cell_max", S2_SIG_CELL_VOLTAGE_182_cell_v_max);
    line_flush(&l);

    line_add(&l, " energy:");
    add_sig(&l, "trip_used", S2_SIG_HV_ENERGY_186_trip_energy_consumed_wh);
    add_sig(&l, "remaining", S2_SIG_HV_ENERGY_186_pack_energy_remaining_wh);
    add_sig(&l, "charge_conn", S2_SIG_CHARGE_PORT_1A7_charge_connector_state);
    add_sig(&l, "charge_pwr_raw", S2_SIG_CHARGE_PORT_1A7_charge_power_raw);
    add_sig(&l, "charge_latch", S2_SIG_CHARGE_PORT_1A7_charge_latch_state);
    line_flush(&l);

    line_add(&l, " thermal:");
    add_sig(&l, "batt1", S2_SIG_TEMPERATURES_183_batt_temp_1);
    add_sig(&l, "batt2", S2_SIG_TEMPERATURES_183_batt_temp_2);
    add_sig(&l, "batt3", S2_SIG_TEMPERATURES_183_batt_temp_3);
    add_sig(&l, "ambient", S2_SIG_TEMPERATURES_183_ambient_temp);
    add_sig(&l, "slow_mode", S2_SIG_TEMPERATURES_183_slow_mode_flag);
    add_sig(&l, "pump", S2_SIG_BATTERY_POWER_163_cooling_pump_163);
    add_sig(&l, "cooling_speed", S2_SIG_COOLING_1A6_cooling_speed);
    add_sig(&l, "cooling_status", S2_SIG_COOLING_1A6_cooling_status);
    line_flush(&l);

    line_add(&l, " chassis:");
    add_sig(&l, "aux", S2_SIG_AUX_VOLTAGE_332_aux_battery_voltage);
    add_sig(&l, "tyreF", S2_SIG_TYRE_PRESSURE_33A_tyre_pressure_front);
    add_sig(&l, "tyreR", S2_SIG_TYRE_PRESSURE_33A_tyre_pressure_rear);
    add_sig(&l, "tpms_warn", S2_SIG_TPMS_WARNING_334_tyre_pressure_warning);
    add_sig(&l, "brakeF", S2_SIG_FRONT_BRAKE_HAZARD_15A_front_brake);
    add_sig(&l, "brakeR", S2_SIG_IGNITION_REAR_BRAKE_133_rear_brake);
    add_sig(&l, "brake_press", S2_SIG_FRONT_BRAKE_PRESSURE_324_front_brake_pressure);
    add_sig(&l, "abs_rear_off", S2_SIG_ABS_STATUS_3C8_rear_abs_disabled);
    add_sig(&l, "kickstand", S2_SIG_DRIVETRAIN_162_kickstand_162);
    line_flush(&l);

    line_add(&l, " state:");
    add_sig(&l, "mode", S2_SIG_RIDER_SETTINGS_134_ride_mode);
    add_sig(&l, "tc_cfg", S2_SIG_RIDER_SETTINGS_134_mode_tc_status);
    add_sig(&l, "drive_ready", S2_SIG_DRIVE_READY_136_drive_ready_state);
    add_sig(&l, "power", S2_SIG_POWER_AVAIL_261_power_state);
    add_sig(&l, "motion", S2_SIG_DRIVE_STATE_322_motion_state);
    add_sig(&l, "ignition", S2_SIG_IGNITION_REAR_BRAKE_133_ignition_d1);
    add_sig(&l, "wake", S2_SIG_IGNITION_REAR_BRAKE_133_wake_state);
    add_sig(&l, "inhibit", S2_SIG_DRIVETRAIN_162_start_inhibit);
    add_sig(&l, "fault", S2_SIG_FAULT_LATCH_3CC_fault_latch);
    line_flush(&l);

    line_add(&l, " lamps:");
    add_sig(&l, "left", S2_SIG_TURN_LAMP_131_lamp_left);
    add_sig(&l, "right", S2_SIG_TURN_LAMP_131_lamp_right);
    add_sig(&l, "hazards", S2_SIG_FRONT_BRAKE_HAZARD_15A_hazards);
    add_sig(&l, "highbeam", S2_SIG_LIGHTING_152_highbeam);
    add_sig(&l, "horn", S2_SIG_LEFT_SWITCH_354_horn);
    add_sig(&l, "info_btn", S2_SIG_LEFT_SWITCH_354_info_scroll_button);
    line_flush(&l);

    line_add(&l, " misc:");
    {
        bool ov;
        double odo_m = sig_value(S2_SIG_ODOMETER_562_odometer_m, &ov);
        if (ov) {
            line_add(&l, "  odo=%.1f km", odo_m / 1000.0);
        }
    }
    {
        bool hv, mv, sv;
        double h = sig_value(S2_SIG_WALL_CLOCK_3C4_clock_hours, &hv);
        double m = sig_value(S2_SIG_WALL_CLOCK_3C4_clock_minutes, &mv);
        double s = sig_value(S2_SIG_WALL_CLOCK_3C4_clock_seconds, &sv);
        if (hv && mv && sv) {
            line_add(&l, "  clock=%02d:%02d:%02d", (int)h, (int)m, (int)s);
        }
    }
    add_sig(&l, "tcu_signal", S2_SIG_TCU_PAIRING_395_cellular_signal_395);
    add_sig(&l, "tcu_state", S2_SIG_TCU_NETWORK_392_tcu_signal_candidate);
    const vs_derived_t *dv = vs_derived();
    if (dv->vin[0]) {
        line_add(&l, "  VIN=%s", dv->vin);
    }
    line_flush(&l);

    line_add(&l, " imu:");
    add_sig(&l, "acc_lat", S2_SIG_IMU_ACCEL_122_accel_lateral);
    add_sig(&l, "acc_lon", S2_SIG_IMU_ACCEL_122_accel_longitudinal);
    add_sig(&l, "acc_vert", S2_SIG_IMU_ACCEL_122_accel_vertical);
    add_sig(&l, "gyro_roll", S2_SIG_IMU_GYRO_126_gyro_roll_rate);
    add_sig(&l, "gyro_pitch", S2_SIG_IMU_GYRO_126_gyro_pitch_rate);
    add_sig(&l, "gyro_yaw", S2_SIG_IMU_GYRO_126_gyro_yaw_rate);
    line_flush(&l);

    s2_overlay_summary(now);
    vs_unlock();
}

static void print_frame_table(int64_t now)
{
    static uint32_t prev_count[S2_DBC_MESSAGE_COUNT];
    static int64_t prev_now;
    double dt = prev_now ? (double)(now - prev_now) / 1e6 : 0.0;

    log_line("--- frame table (%.0f s window) ---", dt);
    vs_lock();
    for (uint16_t i = 0; i < S2_DBC_MESSAGE_COUNT; i++) {
        const vs_message_t *m = vs_message(i);
        const s2_message_def_t *def = &s2_dbc_messages[i];
        if (m->count == 0) {
            continue;
        }
        double hz = dt > 0 ? (double)(m->count - prev_count[i]) / dt : 0.0;
        prev_count[i] = m->count;
        char hex[32];
        size_t pos = 0;
        for (uint8_t b = 0; b < m->last_dlc && pos + 3 < sizeof(hex); b++) {
            pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%02X ", m->last_data[b]);
        }
        const char *e2e = (def->flags & S2_MSG_E2E) ? "E2E" : (def->flags & S2_MSG_NO_E2E) ? "   " : " ? ";
        log_line(" 0x%03X %-26s %6.1f Hz  n=%-7lu %s crc-fail %-4lu gaps %-4lu age %5.1fs  %s", def->id, def->name, hz,
                 (unsigned long)m->count, e2e, (unsigned long)m->crc_fail, (unsigned long)m->alive.gaps,
                 (double)(now - m->last_ts_us) / 1e6, hex);
    }
    unsigned n_unknown = 0;
    const vs_unknown_t *unk = vs_unknown_table(&n_unknown);
    for (unsigned i = 0; i < n_unknown; i++) {
        char hex[32];
        size_t pos = 0;
        for (uint8_t b = 0; b < unk[i].dlc && pos + 3 < sizeof(hex); b++) {
            pos += (size_t)snprintf(hex + pos, sizeof(hex) - pos, "%02X ", unk[i].data[b]);
        }
        const char *kind = (unk[i].id >= 0x7D0 && unk[i].id <= 0x7FF) ? "diag" : "unknown";
        log_line(" 0x%03lX %-26s           n=%-7lu changes %-6lu age %5.1fs  %s", (unsigned long)unk[i].id, kind,
                 (unsigned long)unk[i].count, (unsigned long)unk[i].changes,
                 (double)(now - unk[i].last_ts_us) / 1e6, hex);
    }
    if (vs_unknown_overflow()) {
        log_line(" (%lu frames from further unknown ids not tracked - table full)", (unsigned long)vs_unknown_overflow());
    }
    vs_unlock();
    prev_now = now;
}

void summary_task(void *arg)
{
    (void)arg;
    int64_t next_summary = esp_timer_get_time() + 3000000;
    int64_t next_table = next_summary + 2000000;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        int64_t now = esp_timer_get_time();
#if CONFIG_S2_SUMMARY_PERIOD_MS > 0
        if (now >= next_summary) {
            print_summary(now);
            next_summary = now + (int64_t)CONFIG_S2_SUMMARY_PERIOD_MS * 1000;
        }
#endif
#if CONFIG_S2_FRAME_TABLE_PERIOD_S > 0
        if (now >= next_table) {
            print_frame_table(now);
            next_table = now + (int64_t)CONFIG_S2_FRAME_TABLE_PERIOD_S * 1000000;
        }
#endif
    }
}
