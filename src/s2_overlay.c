/*
 * s2_overlay.c - curated decoding knowledge from the DBC comment blocks.
 *
 * Everything here is comment-only in can-db/livewire_s2_delmar_secondary.dbc
 * (value tables, nibble fields, multiplexers, sentinels, retractions). Line
 * references point into that file at the vendored revision. When the database
 * is updated, review these tables against the new comments.
 */
#include "s2_overlay.h"

#include <stdio.h>
#include <string.h>

#include "dbc_decode.h"
#include "log_writer.h"
#include "ride_limits.h"
#include "s2_dbc_gen.h"
#include "vehicle_state.h"

/* ------------------------------------------------------------------------- */
/* Value tables                                                               */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint16_t signal;
    const s2_value_desc_t *values;
    uint8_t count;
} value_table_t;

#define VT(sig, arr) { sig, arr, (uint8_t)(sizeof(arr) / sizeof((arr)[0])) }

/* 0x134 D1 (DBC L225-235): mode id lives in the high nibble, byte repeats it. */
static const s2_value_desc_t v_ride_mode[] = {
    { 0x22, "Sport" }, { 0x11, "Road" }, { 0x44, "Range" }, { 0x33, "Rain" },
    { 0x55, "Flat Track" }, { 0x88, "Custom A" }, { 0x99, "Custom B" },
};
/* 0x134 D3 per-mode config byte (L240-260). */
static const s2_value_desc_t v_mode_cfg_d3[] = {
    { 0x13, "Sport" }, { 0x11, "Road" }, { 0x1D, "Range" }, { 0x2C, "Rain" },
    { 0x18, "Custom A" }, { 0x19, "Custom B" }, { 0x43, "Flat Track (activated Sport)" },
    { 0x48, "Purple A (activated Custom A)" }, { 0x49, "Purple B (activated Custom B)" },
};
static const s2_value_desc_t v_mode_cfg_d4[] = {
    { 0x30, "Sport / Flat Track" }, { 0x10, "Road" }, { 0xD0, "Range" }, { 0xC0, "Rain" },
    { 0x80, "Custom A" }, { 0x90, "Custom B" },
};
/* 0x260 D2: mode id in the high nibble, low nibble 0. */
static const s2_value_desc_t v_mode_hi_nibble[] = {
    { 0x20, "Sport" }, { 0x10, "Road" }, { 0x40, "Range" }, { 0x30, "Rain" },
    { 0x50, "Flat Track" }, { 0x80, "Custom A" }, { 0x90, "Custom B" },
};
/* 0x133 D1 system state machine (L320-330). */
static const s2_value_desc_t v_system_state[] = {
    { 0x20, "OFF" }, { 0x60, "IGNITION ON, motor off" }, { 0x80, "ENABLING" },
    { 0xE0, "MOTOR ENABLED" }, { 0x50, "CHARGING" },
};
static const s2_value_desc_t v_rear_brake[] = { { 0x00, "released" }, { 0x10, "pressed" } };
static const s2_value_desc_t v_wake_state[] = { { 0x10, "awake" }, { 0x20, "deep-off" } };
static const s2_value_desc_t v_front_brake[] = { { 0x00, "released" }, { 0x40, "pressed" } };
static const s2_value_desc_t v_front_brake_alt[] = { { 0x10, "released" }, { 0x01, "pressed" } };
static const s2_value_desc_t v_hazards[] = { { 0x00, "off" }, { 0x10, "on" } };
static const s2_value_desc_t v_highbeam[] = { { 0x00, "off" }, { 0x04, "on" } };
static const s2_value_desc_t v_ign_152_d2[] = {
    { 0x00, "normal" }, { 0x0E, "WARNING TELLTALE latched" }, { 0x10, "ignition sequence" }, { 0xE0, "ignition sequence" },
};
static const s2_value_desc_t v_lamp_left[] = { { 0x15, "off" }, { 0x19, "LIT" } };
static const s2_value_desc_t v_lamp_right[] = { { 0x50, "off" }, { 0x90, "LIT" } };
static const s2_value_desc_t v_repeater_lr[] = { { 0x00, "idle" }, { 0x10, "left pulse" }, { 0x01, "right pulse" } };
static const s2_value_desc_t v_repeater_off[] = { { 0x00, "idle" }, { 0x10, "off-phase edge" } };
static const s2_value_desc_t v_drive_ready[] = { { 0x44, "RUN (propulsion enabled)" }, { 0x33, "READY (standby)" } };
static const s2_value_desc_t v_drive_state_184[] = { { 0x30, "coast/light" }, { 0x20, "moving" }, { 0x10, "hard propulsion" } };
static const s2_value_desc_t v_wheelspin[] = { { 0, "none" }, { 1, "WHEELSPIN" } };
static const s2_value_desc_t v_im_health[] = { { 0xA2, "operational" }, { 0x90, "init / just reset" } };
static const s2_value_desc_t v_odo_valid[] = { { 0x30, "valid" }, { 0x00, "not yet valid (boot)" } };
static const s2_value_desc_t v_fault_latch[] = {
    { 0x7D, "'normal' code (semantics unresolved)" }, { 0x7E, "'latched' code (semantics unresolved)" },
};
static const s2_value_desc_t v_kickstand[] = { { 0xC8, "DOWN" }, { 0xA8, "up" }, { 0x28, "up + motor ready" } };
static const s2_value_desc_t v_pump[] = { { 0, "off" }, { 1, "RUNNING" } };
static const s2_value_desc_t v_temp_mux_sel[] = {
    { 0, "inverter/winding-class (live)" }, { 1, "motor/coolant-class (live)" },
    { 2, "constant limit 130 C" }, { 3, "constant limit 150 C" }, { 4, "constant limit 130 C" }, { 5, "constant limit 140 C" },
};
static const s2_value_desc_t v_standstill[] = { { 0x00, "moving / drive engaged" }, { 0x40, "STANDSTILL" }, { 0x80, "mode-hold (3rd state)" } };
static const s2_value_desc_t v_brake_applied_128[] = { { 0x01, "off" }, { 0x02, "APPLIED" } };
static const s2_value_desc_t v_charge_conn[] = { { 0x00, "unplugged" }, { 0x02, "CHARGING" }, { 0x01, "latch pressed" } };
static const s2_value_desc_t v_charge_latch[] = { { 0x00, "unplugged" }, { 0x60, "CHARGING" }, { 0x03, "latch pressed" } };
static const s2_value_desc_t v_cooling_speed[] = { { 0x00, "off" }, { 0x70, "low" }, { 0xB0, "medium" }, { 0xE0, "high" } };
static const s2_value_desc_t v_cooling_status[] = { { 0x25, "no charge cable" }, { 0x49, "charge cable connected" }, { 0x29, "plug-in transition" } };
static const s2_value_desc_t v_slow_mode[] = { { 0, "full power" }, { 1, "SLOW MODE (low-SoC derate)" } };
static const s2_value_desc_t v_lamp_telltale[] = {
    { 0x49, "left turn" }, { 0x4A, "left turn" }, { 0x4B, "right turn" }, { 0x4C, "right turn" },
    { 0x50, "high beam" }, { 0x51, "high beam" }, { 0x55, "hazards" }, { 0x5D, "none" },
};
static const s2_value_desc_t v_tpms_warning[] = { { 0x80, "normal" }, { 0x82, "LOW TYRE PRESSURE" } };
static const s2_value_desc_t v_rear_abs[] = { { 0, "active" }, { 1, "REAR ABS DISABLED" } };
static const s2_value_desc_t v_button[] = { { 0, "released" }, { 1, "PRESSED" } };
static const s2_value_desc_t v_tcu_reg[] = {
    { 0x00, "NOT registered" }, { 0x1A, "registered" }, { 0x1B, "registered" }, { 0x1C, "registered" }, { 0x1D, "registered" },
};
static const s2_value_desc_t v_auth_pulse[] = { { 0x00, "pulse 0x00" }, { 0x90, "pulse 0x90" }, { 0xA0, "pulse 0xA0" } };
static const s2_value_desc_t v_energy_mux_sel[] = {
    { 0x00, "phase 0 (rotating diag payload)" }, { 0x20, "energy register A" }, { 0x40, "energy register B" }, { 0x60, "energy register C (inverted?)" },
};
static const s2_value_desc_t v_power_state_322[] = { { 0xFF, "system OFF / asleep" } };

static const value_table_t value_tables[] = {
    VT(S2_SIG_RIDER_SETTINGS_134_ride_mode, v_ride_mode),
    VT(S2_SIG_RIDER_SETTINGS_134_mode_config_d3, v_mode_cfg_d3),
    VT(S2_SIG_RIDER_SETTINGS_134_mode_config_d4, v_mode_cfg_d4),
    VT(S2_SIG_MODE_MIRROR_260_ride_mode_mirror, v_ride_mode),
    VT(S2_SIG_MODE_MIRROR_260_ride_mode_mirror_d2, v_mode_hi_nibble),
    VT(S2_SIG_IGNITION_REAR_BRAKE_133_ignition_d1, v_system_state),
    VT(S2_SIG_IGNITION_REAR_BRAKE_133_rear_brake, v_rear_brake),
    VT(S2_SIG_IGNITION_REAR_BRAKE_133_wake_state, v_wake_state),
    VT(S2_SIG_FRONT_BRAKE_HAZARD_15A_front_brake, v_front_brake),
    VT(S2_SIG_FRONT_BRAKE_HAZARD_15A_front_brake_alt, v_front_brake_alt),
    VT(S2_SIG_FRONT_BRAKE_HAZARD_15A_hazards, v_hazards),
    VT(S2_SIG_LIGHTING_152_highbeam, v_highbeam),
    VT(S2_SIG_LIGHTING_152_ign_152_d2, v_ign_152_d2),
    VT(S2_SIG_TURN_LAMP_131_lamp_left, v_lamp_left),
    VT(S2_SIG_TURN_LAMP_131_lamp_right, v_lamp_right),
    VT(S2_SIG_TURN_REPEATER_154_turn_repeater_lr, v_repeater_lr),
    VT(S2_SIG_TURN_REPEATER_154_turn_repeater_offphase, v_repeater_off),
    VT(S2_SIG_DRIVE_READY_136_drive_ready_state, v_drive_ready),
    VT(S2_SIG_PROPULSION_STATUS_184_drive_state_184, v_drive_state_184),
    VT(S2_SIG_IM_TELLTALE_189_wheelspin_event, v_wheelspin),
    VT(S2_SIG_IM_HEALTH_187_im_health, v_im_health),
    VT(S2_SIG_ODOMETER_ECHO_3C6_odo_valid, v_odo_valid),
    VT(S2_SIG_FAULT_LATCH_3CC_fault_latch, v_fault_latch),
    VT(S2_SIG_DRIVETRAIN_162_kickstand_162, v_kickstand),
    VT(S2_SIG_BATTERY_POWER_163_cooling_pump_163, v_pump),
    VT(S2_SIG_BATTERY_POWER_163_temp_mux_sel, v_temp_mux_sel),
    VT(S2_SIG_STANDSTILL_FLAG_12A_standstill, v_standstill),
    VT(S2_SIG_PROPULSION_ABS_128_front_brake_applied_128, v_brake_applied_128),
    VT(S2_SIG_CHARGE_PORT_1A7_charge_connector_state, v_charge_conn),
    VT(S2_SIG_CHARGE_PORT_1A7_charge_latch_state, v_charge_latch),
    VT(S2_SIG_COOLING_1A6_cooling_speed, v_cooling_speed),
    VT(S2_SIG_COOLING_1A6_cooling_status, v_cooling_status),
    VT(S2_SIG_TEMPERATURES_183_slow_mode_flag, v_slow_mode),
    VT(S2_SIG_LAMP_TELLTALE_188_lamp_telltale, v_lamp_telltale),
    VT(S2_SIG_TPMS_WARNING_334_tyre_pressure_warning, v_tpms_warning),
    VT(S2_SIG_ABS_STATUS_3C8_rear_abs_disabled, v_rear_abs),
    VT(S2_SIG_LEFT_SWITCH_354_horn, v_button),
    VT(S2_SIG_LEFT_SWITCH_354_info_scroll_button, v_button),
    VT(S2_SIG_TCU_NETWORK_392_tcu_signal_candidate, v_tcu_reg),
    VT(S2_SIG_IGNITION_AUTH_137_auth_pulse, v_auth_pulse),
    VT(S2_SIG_BATTERY_ENERGY_MUX_164_energy_mux_sel, v_energy_mux_sel),
    VT(S2_SIG_DRIVE_STATE_322_power_state, v_power_state_322),
};

static const char *lookup(const s2_value_desc_t *tab, uint8_t n, int64_t raw)
{
    for (uint8_t i = 0; i < n; i++) {
        if (tab[i].raw == raw) {
            return tab[i].meaning;
        }
    }
    return NULL;
}

static const char *mode_name_from_id(unsigned id)
{
    switch (id) {
    case 2: return "Sport";
    case 1: return "Road";
    case 4: return "Range";
    case 3: return "Rain";
    case 5: return "Flat Track";
    case 8: return "Custom A";
    case 9: return "Custom B";
    default: return "?";
    }
}

static uint8_t s_last_3c8_d4;   /* last 0x3C8 D4, for the rear-ABS self-test gate */

const char *s2_overlay_describe(uint16_t signal_index, uint64_t raw)
{
    static char buf[96];

    /* Composite / bitfield descriptions first. */
    switch (signal_index) {
    case S2_SIG_RIDER_SETTINGS_134_mode_tc_status: {
        /* L265-275: high nibble = mode id, low nibble 3/4 = TC on, 9 = TC off */
        unsigned lo = raw & 0x0F, hi = (raw >> 4) & 0x0F;
        /* 3 = TC on (Sport), 4 = TC on only verified as the Flat Track default, 9 = TC off */
        const char *tc = lo == 3 ? "TC on" : (lo == 4 && hi == 5) ? "TC on" : lo == 9 ? "TC OFF" : "TC ?";
        snprintf(buf, sizeof(buf), "%s, %s", mode_name_from_id(hi), tc);
        return buf;
    }
    case S2_SIG_SPEED_160_cruise_armed:
        /* L600-640: only bit7 is decoded; the cruise state is the pair (bit7, setpoint) */
        return (raw & 0x80) ? "bit7: standby/cancelled" : "bit7 clear: off or engaged";
    case S2_SIG_DRIVETRAIN_162_energy_direction: {
        /* L660-690: high nibble 4 = enabled, 8 = not enabled; low nibble 1 rest, 2 driving, 6 regen */
        unsigned hi = (raw >> 4) & 0x0F, lo = raw & 0x0F;
        const char *en = hi == 4 ? "motor enabled" : hi == 8 ? "not enabled" : "enable ?";
        const char *flow = lo == 1 ? "rest" : lo == 2 ? "driving" : lo == 6 ? "regen" : "flow ?";
        snprintf(buf, sizeof(buf), "%s, %s", en, flow);
        return buf;
    }
    case S2_SIG_DRIVETRAIN_162_start_inhibit: {
        /* L695-750: independent reason bits */
        if (raw == 0) {
            return "no inhibit";
        }
        buf[0] = '\0';
        if (raw & 0x01) strncat(buf, "throttle-not-zero ", sizeof(buf) - strlen(buf) - 1);
        if (raw & 0x02) strncat(buf, "KICKSTAND ", sizeof(buf) - strlen(buf) - 1);
        if (raw & 0x04) strncat(buf, "FORK-LOCK ", sizeof(buf) - strlen(buf) - 1);
        if (raw & 0xF8) {
            char tmp[24];
            snprintf(tmp, sizeof(tmp), "unknown-bits:0x%02X", (unsigned)(raw & 0xF8));
            strncat(buf, tmp, sizeof(buf) - strlen(buf) - 1);
        }
        return buf;
    }
    case S2_SIG_PROPULSION_STATUS_184_tc_intervention_184:
        /* L440-455: high nibble fixed 0xF, low nibble = intervention intensity 0..12 */
        if ((raw & 0xF0) == 0xF0) {
            snprintf(buf, sizeof(buf), (raw & 0x0F) ? "TC intervention %u/12" : "no intervention", (unsigned)(raw & 0x0F));
            return buf;
        }
        return NULL;
    case S2_SIG_MOTOR_POWER_161_motor_status_d6: {
        /* L1830-1870: bit 0x10 limiter active, bits 0x60 derate/limp markers, 0x04 normal base */
        buf[0] = '\0';
        if (raw & 0x10) strncat(buf, "LIMITER ", sizeof(buf) - strlen(buf) - 1);
        if (raw & 0x60) strncat(buf, "DERATE?(candidate) ", sizeof(buf) - strlen(buf) - 1);
        if (buf[0] == '\0') {
            return (raw == 0x04 || raw == 0x00) ? "normal" : NULL;
        }
        return buf;
    }
    case S2_SIG_POWER_AVAIL_261_power_state:
    case S2_SIG_POWER_AVAIL_262_power_state_mirror: {
        /* L500-512: high nibble = power domain, low nibble = availability level */
        unsigned hi = (raw >> 4) & 0x0F, lo = raw & 0x0F;
        const char *dom = hi == 0xA ? "fault/init" : hi == 0xD ? "drive-ready" : hi == 0xE ? "charge/accessory" : "domain ?";
        snprintf(buf, sizeof(buf), "%s, avail 0x%X", dom, lo);
        return buf;
    }
    case S2_SIG_DRIVE_STATE_322_yaw_rate_coarse:
        return raw == 0xFF ? "OFF (not a yaw value)" : NULL;
    case S2_SIG_ABS_STATUS_3C8_rear_abs_disabled:
        /* bit7 is not meaningful while D4 == 0x0F (power-up self-test) */
        if (s_last_3c8_d4 == 0x0F) {
            return "n/a during ABS self-test";
        }
        break;
    case S2_SIG_DRIVE_STATE_322_motion_state:
        return "SUSPECT decode - do not trust";
    case S2_SIG_TYRE_PRESSURE_33A_unk_33a_b3:
    case S2_SIG_TYRE_PRESSURE_33A_unk_33a_b4:
        return "unknown (tyre-temp reading retracted)";
    case S2_SIG_TEMPERATURES_183_temp_6_unused:
        return raw == 0 ? "reserved slot (not a temperature)" : "reserved slot became non-zero!";
    case S2_SIG_BOOT_STEP_1C3_boot_step_1c3:
    case S2_SIG_BOOT_STEP_1C5_boot_step_1c5:
        snprintf(buf, sizeof(buf), "boot step %u/13", (unsigned)raw);
        return buf;
    case S2_SIG_TCU_PAIRING_395_pairing_status:
        return raw == 0 ? "not account-linked" : "account-linked";
    case S2_SIG_TCU_NETWORK_392_tcu_operator_id:
        return raw == 0x000001 ? "not registered" : "home network id";
    case S2_SIG_IMU_GYRO_126_gyro_roll_rate:
    case S2_SIG_IMU_GYRO_126_gyro_pitch_rate:
    case S2_SIG_IMU_GYRO_126_gyro_yaw_rate:
        return raw == 0xFFFE ? "N/A sentinel" : NULL;
    case S2_SIG_WHEEL_SPEEDS_326_wheel_speed_front:
    case S2_SIG_WHEEL_SPEEDS_326_wheel_speed_rear:
    case S2_SIG_MEAN_WHEEL_SPEED_124_mean_wheel_speed:
        return raw == 0xFFFF ? "asleep/invalid" : NULL;
    case S2_SIG_FRONT_BRAKE_PRESSURE_324_front_brake_pressure:
        return raw == 0xFFFF ? "asleep/invalid" : NULL;
    case S2_SIG_AUX_VOLTAGE_332_aux_battery_voltage:
        return raw == 0xFF ? "invalid (boot/asleep)" : NULL;
    case S2_SIG_CELL_VOLTAGE_182_cell_v_min:
    case S2_SIG_CELL_VOLTAGE_182_cell_v_max:
    case S2_SIG_CELL_VOLTAGE_182_cell_v_avg:
        return raw == 0 ? "BMS not initialised" : NULL;
    case S2_SIG_CHARGE_PORT_1A7_charge_power_raw:
        /* L1680-1700: approx kW = (D2 - 20) / 24.5, noisy, 19-20 = not charging */
        if (raw <= 20) {
            return "not charging";
        }
        snprintf(buf, sizeof(buf), "~%.1f kW AC", ((double)raw - 20.0) / 24.5);
        return buf;
    default:
        break;
    }

    for (size_t i = 0; i < sizeof(value_tables) / sizeof(value_tables[0]); i++) {
        if (value_tables[i].signal == signal_index) {
            return lookup(value_tables[i].values, value_tables[i].count, (int64_t)raw);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* Confidence tags                                                            */
/* ------------------------------------------------------------------------- */

s2_confidence_t s2_overlay_confidence(uint16_t signal_index)
{
    switch (signal_index) {
    /* explicitly retracted / suspect / unresolved / candidate in the DBC */
    case S2_SIG_DRIVE_STATE_322_motion_state:
    case S2_SIG_TYRE_PRESSURE_33A_unk_33a_b3:
    case S2_SIG_TYRE_PRESSURE_33A_unk_33a_b4:
    case S2_SIG_TEMPERATURES_183_temp_6_unused:
    case S2_SIG_FAULT_LATCH_3CC_fault_latch:
    case S2_SIG_COOLING_1A6_cooling_d2:
    case S2_SIG_TCU_PAIRING_395_cellular_signal_395:
    case S2_SIG_MEAN_WHEEL_SPEED_124_wheel_rate_front:   /* km/h scale inferential */
    case S2_SIG_MEAN_WHEEL_SPEED_124_wheel_rate_rear:
    case S2_SIG_RIDER_SETTINGS_134_mode_config_d3:       /* deterministic per mode, semantics open */
    case S2_SIG_RIDER_SETTINGS_134_mode_config_d4:
    case S2_SIG_LIGHTING_152_ign_152_d2:                 /* only 0x0E is ride-confirmed */
    case S2_SIG_TCU_NETWORK_392_tcu_signal_candidate:    /* registration part confirmed, RSSI part not */
        return S2_CONF_TENTATIVE;
    /* single clean capture / physically consistent */
    case S2_SIG_SPEED_160_motor_rpm:
    case S2_SIG_LIGHTING_152_highbeam:
    case S2_SIG_TURN_REPEATER_154_turn_repeater_lr:
    case S2_SIG_TURN_REPEATER_154_turn_repeater_offphase:
    case S2_SIG_DRIVE_READY_136_drive_ready_state:
    case S2_SIG_PROPULSION_STATUS_184_drive_state_184:
    case S2_SIG_PROPULSION_STATUS_184_tc_intervention_184:
    case S2_SIG_IM_HEALTH_187_im_health:
    case S2_SIG_POWER_AVAIL_261_power_state:
    case S2_SIG_POWER_AVAIL_261_avail_level_261_d3:
    case S2_SIG_POWER_AVAIL_261_avail_level_261_d5:
    case S2_SIG_POWER_AVAIL_262_power_state_mirror:
    case S2_SIG_FAULT_LATCH_3CC_config_gen_3cc:
    case S2_SIG_LEFT_SWITCH_354_info_scroll_button:
    case S2_SIG_DRIVE_STATE_322_yaw_rate_coarse:
    case S2_SIG_DRIVE_STATE_322_roll_angle_integral_322:
    case S2_SIG_TEMPERATURES_183_batt_temp_1:
    case S2_SIG_TEMPERATURES_183_batt_temp_2:
    case S2_SIG_TEMPERATURES_183_batt_temp_3:
    case S2_SIG_CHARGE_PORT_1A7_charge_power_raw:
    case S2_SIG_COOLING_1A6_cooling_speed:
    case S2_SIG_COOLING_1A6_cooling_status:
    case S2_SIG_BATTERY_POWER_163_temp_mux_raw:
    case S2_SIG_IMU_GYRO_126_gyro_yaw_rate:
    case S2_SIG_TCU_PAIRING_395_pairing_status:
    case S2_SIG_TCU_NETWORK_392_tcu_operator_id:
        return S2_CONF_STRONG;
    default:
        return S2_CONF_CONFIRMED;
    }
}

bool s2_overlay_suppress_change_log(uint16_t signal_index)
{
    switch (signal_index) {
    case S2_SIG_BATTERY_POWER_163_temp_mux_sel:      /* 6-phase selector cycles every frame */
    case S2_SIG_BATTERY_POWER_163_temp_mux_raw:      /* meaning depends on the selector */
    case S2_SIG_BATTERY_ENERGY_MUX_164_energy_mux_sel:
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------------- */
/* Frame-level decoding: multiplexers, derived states, uncovered bytes       */
/* ------------------------------------------------------------------------- */

typedef struct {
    /* 0x163 temperature mux (L753-842) */
    int temp_inverter_c;
    int temp_motor_coolant_c;
    int64_t temp_inverter_ts;
    int64_t temp_motor_coolant_ts;
    uint8_t temp_limits[4];      /* sel 2..5 raw */
    bool temp_valid[2];
    /* 0x164 energy mux (L2200-2211) */
    uint32_t energy_reg[3];      /* sel 0x20, 0x40, 0x60 */
    bool energy_valid[3];
    int64_t energy_ts[3];
    /* 0x160 cruise state */
    uint8_t cruise_state;        /* 0 off, 1 armed, 2 engaged, 3 cancelled-retained */
    bool cruise_known;
    /* 0x181 contactor */
    bool contactor_open;
    bool contactor_known;
    /* 0x3C8 ABS state */
    uint8_t abs_state;           /* 0 unknown, 1 init/self-test, 2 ready */
    /* 0x134 activated variant */
    bool activated_variant;
    bool activated_known;
    /* 0x1A6 charge cable */
    bool charge_cable;
    bool charge_cable_known;
    /* VIN assembly scratch (published to vs_derived only when complete) */
    char vin_scratch[18];
    uint8_t vin_parts;
} overlay_state_t;

static overlay_state_t st;

/* Per-message: bytes that no SG_ covers; tracked to surface undocumented state. */
static uint8_t s_uncovered_mask[S2_DBC_MESSAGE_COUNT];
static uint8_t s_last_uncovered[S2_DBC_MESSAGE_COUNT][8];
static bool s_have_uncovered[S2_DBC_MESSAGE_COUNT];
static int64_t s_last_uncovered_log[S2_DBC_MESSAGE_COUNT];
static bool s_masks_ready;

static const char *cruise_name(uint8_t s)
{
    switch (s) {
    case 0: return "OFF";
    case 1: return "ARMED (no setpoint)";
    case 2: return "ENGAGED";
    default: return "CANCELLED, setpoint retained";
    }
}

static void build_masks(void)
{
    for (size_t m = 0; m < s2_dbc_message_count; m++) {
        const s2_message_def_t *msg = &s2_dbc_messages[m];
        uint8_t covered = 0;
        for (unsigned s = 0; s < msg->signal_count; s++) {
            const s2_signal_def_t *sig = &msg->signals[s];
            /* bytes touched by this signal */
            if (sig->byte_order == S2_ORDER_BIG_ENDIAN) {
                unsigned pos = (sig->start_bit / 8u) * 8u + (7u - (sig->start_bit % 8u));
                for (unsigned b = 0; b < sig->length; b++, pos++) {
                    covered |= (uint8_t)(1u << (pos / 8u));
                }
            } else {
                for (unsigned b = 0; b < sig->length; b++) {
                    covered |= (uint8_t)(1u << ((sig->start_bit + b) / 8u));
                }
            }
        }
        if (msg->flags & S2_MSG_E2E) {
            covered |= 0xC0;   /* D7 alive counter, D8 CRC */
        }
        uint8_t mask = (uint8_t)(~covered & ((msg->dlc >= 8) ? 0xFF : ((1u << msg->dlc) - 1u)));
        switch (msg->id) {
        case 0x12A: mask &= (uint8_t)~0x01; break;  /* D1 toggles every moving frame (L181-214) */
        case 0x189: mask &= (uint8_t)~0x15; break;  /* D1/D3/D5 blink/PWM aggregates */
        case 0x163: mask &= (uint8_t)~0x20; break;  /* D6 handled by the temperature mux */
        case 0x164: mask &= (uint8_t)~0x3E; break;  /* D2..D6 handled by the energy mux */
        case 0x3C6: mask &= (uint8_t)~0x10; break;  /* D5 internal counter */
        case 0x324: mask &= (uint8_t)~0x10; break;  /* D5 documented 0x51/0x92 toggle (unresolved) */
        case 0x33C: case 0x3CE:                     /* two-phase heartbeats, checked separately */
        case 0x138: case 0x328: case 0x33E: case 0x3CA: case 0x3CF: case 0x304: case 0x320:
            mask = 0; break;                        /* constant payloads, checked by const_frames */
        default: break;
        }
        s_uncovered_mask[m] = mask;
    }
    s_masks_ready = true;
}

static void hex_masked(char *out, size_t n, const uint8_t *d, uint8_t mask)
{
    size_t pos = 0;
    for (unsigned b = 0; b < 8 && pos + 6 < n; b++) {
        if (mask & (1u << b)) {
            pos += (size_t)snprintf(out + pos, n - pos, "D%u=%02X ", b + 1, d[b]);
        }
    }
    if (pos) {
        out[pos - 1] = '\0';
    } else {
        out[0] = '\0';
    }
}

/* Constant-payload frames: any deviation is news (heartbeats, reserved, init). */
typedef struct {
    uint16_t id;
    uint8_t payload[8];
    uint8_t mask;   /* bytes to compare */
} const_frame_t;

static const const_frame_t const_frames[] = {
    { 0x138, { 0x00, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 }, 0xFF },
    { 0x328, { 0x00, 0xFF, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00 }, 0xFF },
    { 0x33E, { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x60, 0x00 }, 0xFF },
    { 0x3CA, { 0 }, 0xFF },
    { 0x3CF, { 0 }, 0xFF },
    { 0x304, { 0x0C, 0x18, 0x00, 0x00, 0x0C, 0x20, 0x00, 0x00 }, 0xFF },
    { 0x320, { 0x0C, 0x88, 0x00, 0x00, 0x0C, 0x90, 0x00, 0x00 }, 0xFF },
};

static void check_constant_frame(const s2_message_def_t *msg, const can_frame_t *f)
{
    for (size_t i = 0; i < sizeof(const_frames) / sizeof(const_frames[0]); i++) {
        if (const_frames[i].id != msg->id) {
            continue;
        }
        bool same = f->dlc == 8;
        for (unsigned b = 0; same && b < 8; b++) {
            if ((const_frames[i].mask & (1u << b)) && f->data[b] != const_frames[i].payload[b]) {
                same = false;
            }
        }
        uint16_t mi = (uint16_t)(msg - s2_dbc_messages);
        if (!same && f->timestamp_us - s_last_uncovered_log[mi] > 5000000) {
            s_last_uncovered_log[mi] = f->timestamp_us;
            log_tline("0x%03X %s: payload deviates from the documented constant: %02X %02X %02X %02X %02X %02X %02X %02X",
                      msg->id, msg->name, f->data[0], f->data[1], f->data[2], f->data[3], f->data[4], f->data[5],
                      f->data[6], f->data[7]);
        }
        return;
    }
    if (msg->id == 0x33C || msg->id == 0x3CE) {
        /* two-phase heartbeat: D1 alternates 0x40 / 0x50 */
        if (f->data[0] != 0x40 && f->data[0] != 0x50) {
            uint16_t mi = (uint16_t)(msg - s2_dbc_messages);
            if (f->timestamp_us - s_last_uncovered_log[mi] > 5000000) {
                s_last_uncovered_log[mi] = f->timestamp_us;
                log_tline("0x%03X %s: unexpected phase byte D1=0x%02X", msg->id, msg->name, f->data[0]);
            }
        }
    }
}

static void track_uncovered(const s2_message_def_t *msg, const can_frame_t *f)
{
    uint16_t mi = (uint16_t)(msg - s2_dbc_messages);
    uint8_t mask = s_uncovered_mask[mi];
    if (!mask || f->dlc < 8) {
        return;
    }
    bool changed = !s_have_uncovered[mi];
    for (unsigned b = 0; b < 8 && !changed; b++) {
        if ((mask & (1u << b)) && s_last_uncovered[mi][b] != f->data[b]) {
            changed = true;
        }
    }
    if (!changed) {
        return;
    }
    bool first = !s_have_uncovered[mi];
    s_have_uncovered[mi] = true;
    memcpy(s_last_uncovered[mi], f->data, 8);
    /* Rate limit: at most one line per 2 s per message so counters cannot flood the log. */
    if (!first && f->timestamp_us - s_last_uncovered_log[mi] < 2000000) {
        return;
    }
    s_last_uncovered_log[mi] = f->timestamp_us;
    char txt[64];
    hex_masked(txt, sizeof(txt), f->data, mask);
    log_tline("0x%03X %s: undocumented bytes %s%s", msg->id, msg->name, txt, first ? " (first)" : "");
}

/*
 * Record the since-boot extremes, once per frame.
 *
 * This has to happen here rather than in the display, because the display reads
 * vehicle state ten times a second while these frames arrive far faster: most
 * samples are overwritten before it ever looks, and the one it sees is an
 * arbitrary instant rather than the peak. That is the whole reason a glanced
 * power figure reads low on a short burst.
 *
 * Runs on the decoder task with the vehicle-state lock already held, after
 * every signal of this frame has been written, so reading them back here gives
 * a consistent set from this one frame.
 *
 * Gated on the frame's end-to-end check: a corrupt frame that slipped through
 * would otherwise leave a false peak that never clears.
 */
static void feed_extremes(const s2_message_def_t *msg, bool e2e_ok)
{
    if (!e2e_ok) {
        return;
    }
    ride_extremes_t *e = vs_extremes();
    if (!e) {
        return;
    }

    switch (msg->id) {
    case 0x181: {
        const vs_signal_t *v = vs_signal(S2_SIG_BATTERY_STATUS_181_pack_voltage);
        const vs_signal_t *i = vs_signal(S2_SIG_BATTERY_STATUS_181_pack_current);
        if (v && i && v->valid && i->valid) {
            ride_extremes_pack(e, v->value, i->value);
        }
        break;
    }
    case 0x161: {
        const vs_signal_t *t = vs_signal(S2_SIG_MOTOR_POWER_161_torque_delivered);
        if (t && t->valid) {
            ride_extremes_torque(e, t->value);
        }
        break;
    }
    case 0x122: {
        const vs_signal_t *a = vs_signal(S2_SIG_IMU_ACCEL_122_accel_longitudinal);
        if (a && a->valid) {
            ride_extremes_accel(e, a->value);
        }
        break;
    }
    default:
        break;
    }
}

void s2_overlay_on_frame(const s2_message_def_t *msg, const can_frame_t *f, bool e2e_ok)
{
    if (!s_masks_ready) {
        build_masks();
    }
    feed_extremes(msg, e2e_ok);
    const uint8_t *d = f->data;
    int64_t now = f->timestamp_us;

    switch (msg->id) {
    case 0x163: {
        /* 6-phase temperature multiplexer: D5 selector, D6 = degC + 40 (L780-842) */
        if (f->dlc < 6) {
            break;
        }
        uint8_t sel = d[4];
        int temp_c = (int)d[5] - 40;
        if (sel == 0 && (!st.temp_valid[0] || st.temp_inverter_c != temp_c)) {
            st.temp_inverter_c = temp_c;
            st.temp_valid[0] = true;
            if (now - st.temp_inverter_ts > 1000000) {
                st.temp_inverter_ts = now;
                log_tline("0x163 mux sel0: inverter/winding-class temp=%d degC", temp_c);
            }
        } else if (sel == 1 && (!st.temp_valid[1] || st.temp_motor_coolant_c != temp_c)) {
            st.temp_motor_coolant_c = temp_c;
            st.temp_valid[1] = true;
            if (now - st.temp_motor_coolant_ts > 1000000) {
                st.temp_motor_coolant_ts = now;
                log_tline("0x163 mux sel1: motor/coolant-class temp=%d degC", temp_c);
            }
        } else if (sel >= 2 && sel <= 5) {
            if (st.temp_limits[sel - 2] != d[5]) {
                if (st.temp_limits[sel - 2] != 0) {
                    log_tline("0x163 mux sel%u: constant limit changed %u -> %u (%d degC)", sel,
                              st.temp_limits[sel - 2], d[5], temp_c);
                }
                st.temp_limits[sel - 2] = d[5];
            }
        }
        break;
    }
    case 0x164: {
        /* 4-phase battery-energy mux: gate on D1 first (L2200-2211) */
        if (f->dlc < 6) {
            break;
        }
        uint8_t sel = d[0];
        if (sel == 0x20 || sel == 0x40 || sel == 0x60) {
            unsigned idx = sel / 0x20 - 1;
            uint32_t reg = ((uint32_t)d[1] << 24) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 8) | d[4];
            if (!st.energy_valid[idx] || st.energy_reg[idx] != reg) {
                st.energy_reg[idx] = reg;
                st.energy_valid[idx] = true;
                if (now - st.energy_ts[idx] > 2000000) {
                    st.energy_ts[idx] = now;
                    log_tline("0x164 mux sel0x%02X: energy register=%lu (0x%08lX, units open)", sel,
                              (unsigned long)reg, (unsigned long)reg);
                }
            }
        }
        break;
    }
    case 0x56D:
    case 0x56E:
    case 0x56F: {
        /* VIN in ASCII, 6 + 6 + 5 chars (L1401-1416) */
        vs_derived_t *dv = vs_derived();
        unsigned part = msg->id - 0x56D;
        unsigned n = part == 2 ? 5 : 6;
        if (f->dlc < n) {
            break;
        }
        bool changed = memcmp(&st.vin_scratch[part * 6], d, n) != 0;
        memcpy(&st.vin_scratch[part * 6], d, n);
        st.vin_scratch[17] = '\0';
        uint8_t before = st.vin_parts;
        st.vin_parts |= (uint8_t)(1u << part);
        if (st.vin_parts == 0x07 && (before != 0x07 || changed)) {
            memcpy(dv->vin, st.vin_scratch, sizeof(dv->vin));
            dv->vin_parts = st.vin_parts;
            log_tline("VIN: %s", dv->vin);
        }
        break;
    }
    case 0x160: {
        /* cruise state = (D6 bit7, D5 setpoint) (L600-645) */
        if (f->dlc < 6) {
            break;
        }
        bool flag = (d[5] & 0x80) != 0;
        bool sp = d[4] != 0;
        uint8_t state = (!flag && !sp) ? 0 : (flag && !sp) ? 1 : (!flag && sp) ? 2 : 3;
        if (!st.cruise_known || state != st.cruise_state) {
            st.cruise_known = true;
            st.cruise_state = state;
            if (sp) {
                log_tline("0x160 cruise control: %s, setpoint %u km/h", cruise_name(state), d[4]);
            } else {
                log_tline("0x160 cruise control: %s", cruise_name(state));
            }
        }
        break;
    }
    case 0x181: {
        /* contactor open: current raw == 4000 with validity nibble 0 (L843-963) */
        if (f->dlc < 4) {
            break;
        }
        unsigned raw_i = ((d[1] & 0x0Fu) << 12) | ((unsigned)d[2] << 4) | (d[3] >> 4);
        bool open = raw_i == 4000 && (d[3] & 0x0F) == 0;
        if (!st.contactor_known || open != st.contactor_open) {
            st.contactor_known = true;
            st.contactor_open = open;
            log_tline("0x181 HV contactor: %s (validity nibble %u)", open ? "OPEN (no current measurement)" : "closed / measuring", d[3] & 0x0F);
        }
        break;
    }
    case 0x3C8: {
        /* ABS init -> ready handshake (L1367-1400) */
        if (f->dlc < 5) {
            break;
        }
        s_last_3c8_d4 = d[3];
        uint8_t state = (d[0] == 0x04 && d[1] == 0x24 && d[4] == 0x10) ? 2 : (d[0] == 0 && d[1] == 0 && d[4] == 0) ? 1 : 0;
        if (state != st.abs_state) {
            st.abs_state = state;
            log_tline("0x3C8 ABS: %s (D3=0x%02X D4=0x%02X)", state == 2 ? "READY" : state == 1 ? "INIT / self-test" : "unknown header",
                      d[2], d[3]);
        }
        break;
    }
    case 0x134: {
        /* activated-variant flag D6 bit2 (L280-300) */
        if (f->dlc < 6) {
            break;
        }
        bool act = (d[5] & 0x04) != 0;
        if (!st.activated_known || act != st.activated_variant) {
            st.activated_known = true;
            st.activated_variant = act;
            log_tline("0x134 activated mode variant (Flat Track / Purple A / Purple B): %s", act ? "ENGAGED" : "off");
        }
        break;
    }
    case 0x1A6: {
        /* charge cable connected = D3 bit6 (L1936-1975) */
        if (f->dlc < 3) {
            break;
        }
        bool cable = (d[2] & 0x40) != 0;
        if (!st.charge_cable_known || cable != st.charge_cable) {
            st.charge_cable_known = true;
            st.charge_cable = cable;
            log_tline("0x1A6 charge cable: %s", cable ? "CONNECTED" : "not connected");
        }
        break;
    }
    default:
        break;
    }

    check_constant_frame(msg, f);
    track_uncovered(msg, f);
}

void s2_overlay_summary(int64_t now_us)
{
    char line[LOG_LINE_MAX];
    size_t pos = 0;
    pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " derived:");
    if (st.temp_valid[0]) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "  inverter=%d degC%s", st.temp_inverter_c,
                                (now_us - st.temp_inverter_ts) > 5000000 ? "*" : "");
    }
    if (st.temp_valid[1]) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "  motor/coolant=%d degC%s", st.temp_motor_coolant_c,
                                (now_us - st.temp_motor_coolant_ts) > 5000000 ? "*" : "");
    }
    if (st.cruise_known) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "  cruise=%s", cruise_name(st.cruise_state));
    }
    if (st.contactor_known) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "  contactor=%s", st.contactor_open ? "open" : "closed");
    }
    if (st.abs_state) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "  abs=%s", st.abs_state == 2 ? "ready" : "init");
    }
    if (st.charge_cable_known) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "  cable=%s", st.charge_cable ? "connected" : "no");
    }
    if (st.activated_known && st.activated_variant) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "  activated-variant");
    }
    const vs_signal_t *rem = vs_signal(S2_SIG_HV_ENERGY_186_pack_energy_remaining_wh);
    if (rem && rem->valid) {
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "  soc_est=%.1f%% (Wh/103)", rem->value / 103.0);
    }
    for (unsigned i = 0; i < 3 && pos < sizeof(line) - 40; i++) {
        if (st.energy_valid[i]) {
            pos += (size_t)snprintf(line + pos, sizeof(line) - pos, "  e%02X=%lu", 0x20 * (i + 1), (unsigned long)st.energy_reg[i]);
        }
    }
    if (pos > 9) {
        log_line("%s", line);
    }
}
