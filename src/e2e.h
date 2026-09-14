/*
 * e2e.h - AUTOSAR-style end-to-end protection used on ~36 LiveWire S2 frames.
 *
 *   D7 (data[6]) = 6-bit alive counter, 0..63 wrapping
 *   D8 (data[7]) = CRC-8 / SAE-J1850 (poly 0x1D, init 0xFF, xor-out 0xFF, no reflection)
 *                  computed over D1..D7 (data[0..6]), no Data-ID prefix.
 *
 * Pure C, shared with the host unit tests.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define S2_E2E_ALIVE_MASK 0x3Fu
#define S2_E2E_ALIVE_MOD  64u

/* CRC-8/SAE-J1850 over len bytes. */
uint8_t s2_crc8_j1850(const uint8_t *data, size_t len);

/* True when data[7] equals the CRC of data[0..6]. Requires dlc == 8. */
bool s2_e2e_crc_ok(const uint8_t *data, size_t dlc);

/* Alive counter of a protected frame (low 6 bits of D7). */
static inline uint8_t s2_e2e_alive(const uint8_t *data)
{
    return data[6] & S2_E2E_ALIVE_MASK;
}

/* Per-message alive-counter tracker. */
typedef struct {
    bool have_last;
    uint8_t last;
    uint32_t gaps;          /* number of times the counter did not advance by exactly 1 */
    uint32_t frames_lost;   /* sum of (gap size - 1) over all gaps, modulo 64 per gap */
} s2_e2e_tracker_t;

/*
 * Feed the alive counter of a new frame. Returns the number of frames that appear
 * to have been missed since the previous one (0 when consecutive, 0 for the first).
 * A repeated counter (delta 0) counts as a gap with 63 lost - it should not happen.
 */
uint32_t s2_e2e_track_alive(s2_e2e_tracker_t *t, uint8_t alive);

#ifdef __cplusplus
}
#endif
