/*
 * s2_dbc.h - data model for the LiveWire S2 secondary-CAN signal database.
 *
 * The tables that instantiate these types live in src/gen/s2_dbc_gen.c and are
 * generated from can-db/livewire_s2_delmar_secondary.dbc by tools/dbc2c.py.
 * Pure C, no ESP-IDF dependencies: this header is shared with the host unit tests.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Byte order of a multi-byte signal, as encoded in the DBC ("@0" / "@1"). */
typedef enum {
    S2_ORDER_BIG_ENDIAN = 0,    /* Motorola, DBC "@0" - the S2 uses this everywhere */
    S2_ORDER_LITTLE_ENDIAN = 1, /* Intel, DBC "@1" */
} s2_byte_order_t;

/* Reverse-engineering confidence, mirrored from the DBC comment tags. */
typedef enum {
    S2_CONF_UNSPECIFIED = 0,
    S2_CONF_CONFIRMED,   /* cross-verified >= 2 ways */
    S2_CONF_STRONG,      /* one clean capture, physically consistent */
    S2_CONF_TENTATIVE,   /* needs more data - may be wrong */
} s2_confidence_t;

/* Message flags. */
#define S2_MSG_E2E        0x01u  /* D7 = 6-bit alive counter, D8 = CRC-8/SAE-J1850 over D1..D7 */
#define S2_MSG_NO_E2E     0x02u  /* known to carry constant padding in D7/D8 (no integrity) */

/* One raw value -> meaning pair (enumerations, state machines, bit meanings). */
typedef struct {
    int32_t raw;
    const char *meaning;
} s2_value_desc_t;

typedef struct {
    const char *name;           /* DBC signal name */
    const char *unit;           /* DBC unit string ("" if none) */
    uint8_t start_bit;          /* DBC start bit (Motorola numbering for big-endian) */
    uint8_t length;             /* bit length, 1..64 */
    uint8_t byte_order;         /* s2_byte_order_t */
    uint8_t is_signed;          /* DBC "-" sign */
    double factor;              /* double: 0.1f * 1000 is not 100.0 */
    double offset;
    float min;
    float max;
} s2_signal_def_t;

typedef struct {
    uint16_t id;                /* 11-bit CAN identifier */
    uint8_t dlc;                /* payload length from the DBC */
    uint8_t flags;              /* S2_MSG_* */
    uint16_t first_signal;      /* index of the first signal in s2_signal_id_t numbering */
    uint8_t signal_count;
    const char *name;           /* DBC message name */
    const char *sender;         /* DBC transmitter node ("Vector__XXX" when unknown) */
    const s2_signal_def_t *signals;
} s2_message_def_t;

/* Tables (generated, sorted by ascending id). */
extern const s2_message_def_t s2_dbc_messages[];
extern const size_t s2_dbc_message_count;
extern const size_t s2_dbc_signal_count;   /* total number of signals across all messages */

/* Binary search by CAN id; NULL when the id is not in the database. */
const s2_message_def_t *s2_dbc_find_message(uint32_t can_id);

/* Message definition for a global signal index, or NULL. */
const s2_message_def_t *s2_dbc_message_of_signal(uint16_t signal_index);

/* Signal definition for a global signal index, or NULL. */
const s2_signal_def_t *s2_dbc_signal(uint16_t signal_index);

#ifdef __cplusplus
}
#endif
