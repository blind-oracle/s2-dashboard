/*
 * screens.h - what the round display actually draws.
 *
 * Screen 1 is the ride screen: a bidirectional power ring, the power value as
 * large seven-segment digits, then torque, pack voltage, session energy and
 * state of health. The remaining screens are placeholders.
 *
 * Rendering is pure: it takes a snapshot and a framebuffer and touches nothing
 * else, so the layout can be exercised on the host.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-field freshness. Placeholders differ for "never seen" and "went stale". */
typedef enum {
    FIELD_MISSING = 0,   /* never received: dashes in the dim colour */
    FIELD_LIVE,
    FIELD_STALE,         /* last known value, recoloured, header says STALE */
} field_state_t;

#define DASH_PLMN_MAX 12

typedef struct {
    /* --- screen 1, the ride screen --- */
    field_state_t power_state;
    double power_kw;        /* positive = out of the pack, negative = regen */

    field_state_t torque_state;
    double torque_counts;   /* 0x161 torque_delivered, zero at raw 5000 */
    double torque_nm;       /* estimate: counts / (counts-per-Nm); marked with ~ */

    field_state_t volts_state;
    double volts;

    field_state_t energy_state;
    double used_kwh;        /* the bike's own key-on trip meter, 0x186, not integrated here */

    field_state_t rpm_state;
    double motor_rpm;       /* 0x160 motor_rpm */

    /* --- screen 2, battery --- */
    field_state_t cell_state;
    double cell_mv_min, cell_mv_avg, cell_mv_max;   /* 0x182 */

    field_state_t current_state;
    double pack_amps;       /* 0x181, charge-positive as the bike reports it */

    field_state_t soc_state;
    double soc_pct;

    field_state_t soh_state;
    double soh_pct;
    bool uds_enabled;       /* explains a missing SoH, GPS or cellular: they are UDS-only */

    /* --- screen 3, thermal --- */
    field_state_t pack_temp_state;
    double pack_t_min, pack_t_avg, pack_t_max;      /* across the three 0x183 sensors */

    field_state_t coolant_state;
    double coolant_c;       /* 0x163 temperature mux, selector 1 */

    field_state_t inverter_state;
    double inverter_c;      /* 0x163 temperature mux, selector 0 */

    field_state_t ambient_state;
    double ambient_c;

    /* --- screen 4, chassis --- */
    field_state_t accel_state;
    double accel_now;       /* 0x122 longitudinal, raw counts */
    double accel_max_pos;   /* extremes observed since boot */
    double accel_max_neg;

    field_state_t tyre_state;
    double tyre_front_kpa, tyre_rear_kpa;           /* converted for display */

    /* --- screen 5, telematics (all UDS-only) --- */
    field_state_t gps_state;
    double gps_lat, gps_lon;

    field_state_t cell_signal_state;
    double cell_signal;     /* TCU DID 0297 byte 0 */
    char plmn[DASH_PLMN_MAX];                       /* TCU DID 0296, "MCC MNC" */
} dash_data_t;

/*
 * Update mode. Kept as a plain struct with no ESP-IDF types so screens.c stays
 * host-testable; src/ota.c fills it in.
 */
typedef enum {
    UPDATE_WAITING = 0,   /* access point up, nothing uploaded yet */
    UPDATE_RECEIVING,
    UPDATE_DONE,          /* image accepted, about to reboot */
    UPDATE_FAILED,
} update_phase_t;

#define UPDATE_SSID_MAX 33
#define UPDATE_PASS_MAX 16
#define UPDATE_IP_MAX 16
#define UPDATE_DETAIL_MAX 48

typedef struct {
    update_phase_t phase;
    char ssid[UPDATE_SSID_MAX];
    char pass[UPDATE_PASS_MAX];
    char ip[UPDATE_IP_MAX];
    uint32_t received;          /* bytes written so far */
    uint32_t total;             /* 0 when the size is not known */
    char detail[UPDATE_DETAIL_MAX];  /* version transition, or why it failed */
} update_data_t;

/* How many screens the renderer actually draws; the rest are placeholders. */
#define SCREENS_IMPLEMENTED 5u

/*
 * Convert a tyre pressure from the kPa the bus reports into the unit chosen in
 * menuconfig, and return that unit's label. Pure, so the conversion is covered
 * by the host tests.
 */
const char *screens_pressure(double kpa, double *out);

/* Draw one screen into g. screen is 0-based; count sets the indicator dots. */
void screens_render(gfx_t *g, unsigned screen, unsigned count, const dash_data_t *d);

/* Draw the update-mode screen: how to connect, then upload progress. */
void screens_render_update(gfx_t *g, const update_data_t *d);

/* Full-scale ends of the power ring, in kW (regen side is negative). */
void screens_gauge_range(double *regen_kw, double *power_kw);

#ifdef __cplusplus
}
#endif
