/*
 * s2_overlay.h - hand-curated knowledge that the machine-readable DBC lines do
 * not carry: enumerations / state names, multiplexed sub-frames, comment-only
 * fields and derived values (VIN assembly). Source: the comment blocks of the
 * DBC (see can-db/) - keep the file citations in s2_overlay.c up to date.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "can_bus.h"
#include "s2_dbc_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Human-readable meaning of a raw signal value, or NULL when none is known. */
const char *s2_overlay_describe(uint16_t signal_index, uint64_t raw);

/* Confidence tag for a signal as stated in the DBC comments. */
s2_confidence_t s2_overlay_confidence(uint16_t signal_index);

/*
 * True for signals whose per-frame change lines would only be noise because the
 * overlay logs their demultiplexed meaning instead (mux selectors / payloads).
 */
bool s2_overlay_suppress_change_log(uint16_t signal_index);

/*
 * Frame-level hook, called by the decoder (holding the vehicle-state lock)
 * after the plain DBC signals were extracted. Handles multiplexed payloads,
 * comment-only fields and derived values; logs its own change lines.
 *
 * e2e_ok is false when the frame is E2E-protected and failed its CRC. Derived
 * values that accumulate (the energy meter) must never take an unverified
 * frame, whatever the logging options say about decoding one.
 */
void s2_overlay_on_frame(const s2_message_def_t *msg, const can_frame_t *frame, bool e2e_ok);

/* Formats the overlay's summary lines (mux temperatures, VIN, ...). Lock held. */
void s2_overlay_summary(int64_t now_us);

#ifdef __cplusplus
}
#endif
