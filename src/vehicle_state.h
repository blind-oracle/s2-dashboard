/*
 * vehicle_state.h - latest value of every database signal plus per-message and
 * unknown-id statistics. Written by the decoder task, read by the summary task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "e2e.h"
#include "s2_dbc_gen.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double value;           /* physical value */
    uint64_t raw;
    int64_t ts_us;          /* time of last update */
    int64_t change_ts_us;   /* time the value last changed */
    uint32_t updates;
    uint32_t changes;
    bool valid;
} vs_signal_t;

typedef struct {
    uint32_t count;
    uint32_t crc_fail;
    uint32_t dlc_mismatch;
    int64_t first_ts_us;
    int64_t last_ts_us;
    uint8_t last_data[8];
    uint8_t last_dlc;
    s2_e2e_tracker_t alive;
} vs_message_t;

#define VS_UNKNOWN_MAX 48

typedef struct {
    uint32_t id;
    uint32_t count;
    uint32_t changes;       /* payload differed from the previous frame */
    int64_t last_ts_us;
    uint8_t dlc;
    uint8_t data[8];
} vs_unknown_t;

typedef struct {
    char vin[18];           /* assembled from 0x56D/0x56E/0x56F, "" until complete */
    uint8_t vin_parts;      /* bitmask of received parts */
} vs_derived_t;

void vs_init(void);
void vs_lock(void);
void vs_unlock(void);

/* The caller holds the lock for all accessors below. */
vs_signal_t *vs_signal(uint16_t index);            /* index < S2_SIG__COUNT */
vs_message_t *vs_message(uint16_t message_index);  /* index < s2_dbc_message_count */
vs_derived_t *vs_derived(void);

/* Record a decoded signal; returns true when the value changed. */
bool vs_update_signal(uint16_t index, uint64_t raw, double value, int64_t ts_us);

/* Track a frame whose id is not in the database. Returns the slot (or NULL when the table is full). */
vs_unknown_t *vs_note_unknown(uint32_t id, const uint8_t *data, uint8_t dlc, int64_t ts_us, bool *changed);
const vs_unknown_t *vs_unknown_table(unsigned *count);
uint32_t vs_unknown_overflow(void);

#ifdef __cplusplus
}
#endif
