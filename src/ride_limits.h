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

#ifdef __cplusplus
}
#endif
