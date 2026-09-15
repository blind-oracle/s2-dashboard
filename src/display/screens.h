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

typedef struct {
    field_state_t power_state;
    double power_kw;        /* positive = out of the pack, negative = regen */

    field_state_t torque_state;
    double torque_counts;   /* 0x161 torque_delivered, zero at raw 5000 */
    double torque_nm;       /* estimate: counts / (counts-per-Nm); marked with ~ */

    field_state_t volts_state;
    double volts;

    field_state_t energy_state;
    double used_kwh;        /* the bike's own key-on trip meter, 0x186, not integrated here */

    field_state_t soh_state;
    double soh_pct;
    bool uds_enabled;       /* explains a missing SoH: it is UDS-only */
} dash_data_t;

/* Draw one screen into g. screen is 0-based; count sets the indicator dots. */
void screens_render(gfx_t *g, unsigned screen, unsigned count, const dash_data_t *d);

/* Full-scale ends of the power ring, in kW (regen side is negative). */
void screens_gauge_range(double *regen_kw, double *power_kw);

#ifdef __cplusplus
}
#endif
