/*
 * ride_limits.h - plausibility windows, freshness windows and scale factors for
 * the five ride values.
 *
 * The ST7789 screen and the BLE dashboard frame both derive power, torque, pack
 * voltage, session energy and state of health from the same signals. These rules
 * live here so the two can never disagree about whether a sample was real or
 * about when it went stale.
 *
 * Pure C and header-only; the only dependency is the generated sdkconfig.h.
 */
#pragma once

#include <stdbool.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Instantaneous values go stale fast; slower frames are given longer. */
#define RIDE_STALE_FAST_US     2000000LL
#define RIDE_STALE_ENERGY_US   5000000LL     /* 0x186 runs at ~1.8 Hz */
#define RIDE_STALE_SOH_US    300000000LL     /* UDS-polled, minutes apart */
#define RIDE_STALE_SLOW_US     5000000LL     /* cells, charge, pack temperatures */
#define RIDE_STALE_MUX_US     10000000LL     /* the 0x163 temperature mux, latched per selector */
#define RIDE_STALE_TYRE_US   300000000LL     /* TPMS reports rarely, and only when rolling */
#define RIDE_STALE_GPS_US    900000000LL     /* the TCU position refreshes every few minutes */

/*
 * The window a 0x181 pair must fall in to be believable, from the database's
 * notes: ~400 V nominal, sagging to ~300 V at the 240 A peak, and charging never
 * more than a few tens of amps. Outside this is a decode error, not a reading.
 */
#define RIDE_V_MIN 200.0
#define RIDE_V_MAX 430.0
#define RIDE_I_MIN (-400.0)
#define RIDE_I_MAX 50.0

/* Torque outside this is a decode error, not a reading (observed -280..+1249). */
#define RIDE_TORQUE_ABS_MAX 1500.0

/* 0x161 reports torque in counts; the database's Nm estimate is TENTATIVE. */
#define RIDE_TORQUE_PER_NM ((double)CONFIG_S2_TORQUE_COUNTS_PER_NM_X100 / 100.0)

/*
 * Freshness of one field. The numbering is shared by field_state_t in
 * display/screens.h and s2_ble_freshness_t in ble_proto.h, so a caller can
 * assign the result straight into either.
 */
#define RIDE_FS_MISSING 0
#define RIDE_FS_LIVE    1
#define RIDE_FS_STALE   2

static inline int ride_freshness(bool valid, long long ts_us, long long now_us, long long window_us)
{
    if (!valid) {
        return RIDE_FS_MISSING;
    }
    return (now_us - ts_us) > window_us ? RIDE_FS_STALE : RIDE_FS_LIVE;
}

/* One rule for every consumer, so the gauge and the readouts always agree. */
static inline bool ride_pack_sample_plausible(double volts, double amps)
{
    return volts >= RIDE_V_MIN && volts <= RIDE_V_MAX && amps >= RIDE_I_MIN && amps <= RIDE_I_MAX;
}

static inline bool ride_torque_plausible(double counts)
{
    return counts >= -RIDE_TORQUE_ABS_MAX && counts <= RIDE_TORQUE_ABS_MAX;
}

/* Pack current is charge-positive, so power OUT of the pack is -(V * I). */
static inline double ride_power_kw(double volts, double amps)
{
    return -(volts * amps) / 1000.0;
}

static inline double ride_torque_nm(double counts)
{
    return counts / RIDE_TORQUE_PER_NM;
}

/* ------------------------------------------------------------- extremes --- */

/*
 * The largest values seen since boot.
 *
 * These are fed one CAN frame at a time from the decoder, NOT from the display.
 * The display reads vehicle state ten times a second while the battery frame
 * arrives far faster, so four of every five voltage and current pairs are
 * overwritten before the display sees them, and the one it does see is an
 * arbitrary instant rather than the peak. The ride screen's smoothing then
 * takes another bite: a 0.2 s time constant shows barely half of a step after
 * 0.2 s. Together that is why a glanced power figure reads low on a short
 * burst, and why a peak worth trusting has to be captured per frame.
 *
 * Pure and header-only, so the host tests can drive it directly.
 */
typedef struct {
    bool pack_seen;
    double power_max_kw;    /* most power out of the pack */
    double power_min_kw;    /* most regeneration, so the most negative */
    double amps_max;        /* most charging, since current is charge-positive */
    double amps_min;        /* most discharging */

    bool torque_seen;
    double torque_max_counts;

    bool accel_seen;
    double accel_max, accel_min;
} ride_extremes_t;

static inline void ride_extremes_reset(ride_extremes_t *e)
{
    if (e) {
        e->pack_seen = false;
        e->power_max_kw = 0.0;
        e->power_min_kw = 0.0;
        e->amps_max = 0.0;
        e->amps_min = 0.0;
        e->torque_seen = false;
        e->torque_max_counts = 0.0;
        e->accel_seen = false;
        e->accel_max = 0.0;
        e->accel_min = 0.0;
    }
}

/*
 * One voltage and current pair from a single frame. Gated on the same
 * plausibility rule the gauge uses: without it one glitched decode would stick
 * as a permanent false peak, which is worse than missing one.
 */
static inline void ride_extremes_pack(ride_extremes_t *e, double volts, double amps)
{
    if (!e || !ride_pack_sample_plausible(volts, amps)) {
        return;
    }
    double kw = ride_power_kw(volts, amps);
    if (!e->pack_seen) {
        e->pack_seen = true;
        e->power_max_kw = e->power_min_kw = kw;
        e->amps_max = e->amps_min = amps;
        return;
    }
    if (kw > e->power_max_kw) {
        e->power_max_kw = kw;
    }
    if (kw < e->power_min_kw) {
        e->power_min_kw = kw;
    }
    if (amps > e->amps_max) {
        e->amps_max = amps;
    }
    if (amps < e->amps_min) {
        e->amps_min = amps;
    }
}

/* Only the maximum is interesting: the negative end is regeneration braking. */
static inline void ride_extremes_torque(ride_extremes_t *e, double counts)
{
    if (!e || !ride_torque_plausible(counts)) {
        return;
    }
    if (!e->torque_seen || counts > e->torque_max_counts) {
        e->torque_seen = true;
        e->torque_max_counts = counts;
    }
}

/*
 * Longitudinal acceleration, in the raw counts the IMU reports. No plausibility
 * rule exists for this axis, so the caller gates on the frame's own end-to-end
 * check instead.
 */
static inline void ride_extremes_accel(ride_extremes_t *e, double counts)
{
    if (!e) {
        return;
    }
    if (!e->accel_seen) {
        e->accel_seen = true;
        e->accel_max = e->accel_min = counts;
        return;
    }
    if (counts > e->accel_max) {
        e->accel_max = counts;
    }
    if (counts < e->accel_min) {
        e->accel_min = counts;
    }
}

#ifdef __cplusplus
}
#endif
