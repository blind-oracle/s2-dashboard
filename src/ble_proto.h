/*
 * ble_proto.h - wire formats for the BLE telemetry service.
 *
 * Pure C: no NimBLE, no ESP-IDF, no vehicle_state. Every encoder and the command
 * parser are therefore exercised by the host unit tests; the transport and the
 * locked read of vehicle state live in ble_telemetry.c.
 *
 * All multi-byte fields are little-endian, and floats are IEEE-754 binary32.
 * Both the ESP32-S3 and every Android device are little-endian, but the stores
 * here are explicit so the contract does not depend on that.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "s2_dbc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever a layout below changes in a way an old client would misread. */
#define S2_BLE_PROTO_VERSION 1u

/* ------------------------------------------------------------ freshness --- */

/*
 * Deliberately numbered to match field_state_t in display/screens.h, so the two
 * snapshot paths can converge later without a translation table.
 */
typedef enum {
    S2_BLE_FS_MISSING = 0,   /* never received */
    S2_BLE_FS_LIVE = 1,
    S2_BLE_FS_STALE = 2,     /* last known value, older than the signal's window */
} s2_ble_freshness_t;

/* ------------------------------------------- dashboard characteristic --- */

/*
 * The five values the ST7789 screen shows, as one fixed 24-byte frame. Fixed
 * layout rather than records because this is the characteristic a live gauge
 * subscribes to: a client parses it with five loads and no loop.
 *
 *   [0]  u8   protocol version
 *   [1]  u8   flags: bit 0 = UDS polling is enabled in this build
 *   [2]  u16  freshness, 2 bits per field, s2_ble_freshness_t values,
 *             in the order power, torque, volts, energy, soh (bits 0-1 = power)
 *   [4]  f32  power out of the pack, kW (negative = regen)
 *   [8]  f32  torque delivered, Nm (an estimate; see S2_TORQUE_COUNTS_PER_NM_X100)
 *   [12] f32  pack voltage, V
 *   [16] f32  energy used this key-on cycle, kWh (the bike's own trip meter)
 *   [20] f32  state of health, % (UDS-only, so missing in a passive build)
 */
#define S2_BLE_DASH_SIZE 24u

#define S2_BLE_DASH_FLAG_UDS 0x01u

typedef struct {
    float power_kw;
    float torque_nm;
    float volts;
    float session_kwh;
    float soh_pct;
    uint8_t power_fs;        /* s2_ble_freshness_t */
    uint8_t torque_fs;
    uint8_t volts_fs;
    uint8_t energy_fs;
    uint8_t soh_fs;
    bool uds_enabled;
} s2_ble_dash_t;

/* Writes exactly S2_BLE_DASH_SIZE bytes. Returns 0 when outlen is too small. */
size_t s2_ble_pack_dash(uint8_t *out, size_t outlen, const s2_ble_dash_t *d);

/* ---------------------------------------------- stream characteristic --- */

/*
 * Record messages are fragmented across notifications, because a full snapshot
 * of all 113 signals exceeds any ATT MTU. Every fragment carries the same
 * 8-byte header:
 *
 *   [0] u8   protocol version
 *   [1] u8   message type (S2_BLE_MSG_*)
 *   [2] u8   fragment index, 0-based
 *   [3] u8   fragment count
 *   [4] u16  number of records in this fragment
 *   [6] u16  sequence number, incremented once per complete message
 *
 * A client reassembles by collecting fragments 0..count-1 that share a sequence
 * number, and discards the partial message if the sequence number changes.
 */
#define S2_BLE_FRAG_HEADER 8u

#define S2_BLE_MSG_SNAPSHOT 1u   /* S2_BLE_SNAP_RECORD-sized records */
#define S2_BLE_MSG_CATALOG  2u   /* S2_BLE_CAT_RECORD-sized records */

/*
 * Snapshot record, 8 bytes:
 *   [0] u16  signal index - the DBC global signal index, stable for a database
 *            version and the same number the catalog describes
 *   [2] u16  age in deciseconds since the value last updated, S2_BLE_AGE_NEVER
 *            when the signal has not been seen at all
 *   [4] f32  physical value (raw * factor + offset)
 */
#define S2_BLE_SNAP_RECORD 8u

#define S2_BLE_AGE_NEVER 0xFFFFu
#define S2_BLE_AGE_MAX   0xFFFEu   /* 6553.4 s, about 109 minutes */

typedef struct {
    uint16_t index;
    uint16_t age_ds;
    float value;
} s2_ble_record_t;

/*
 * Catalog record, 56 bytes. Sent once on request so a client can label and
 * range any signal without compiling the database in.
 *   [0]  u16     signal index
 *   [2]  u16     CAN identifier of the message carrying it
 *   [4]  f32     database minimum
 *   [8]  f32     database maximum
 *   [12] char[28] signal name, NUL-padded, truncated if longer
 *   [40] char[16] unit, NUL-padded, truncated if longer
 */
#define S2_BLE_CAT_RECORD 56u
#define S2_BLE_CAT_NAME   28u
#define S2_BLE_CAT_UNIT   16u

/* Records of `record_size` that fit one notification of `mtu_payload` bytes. */
unsigned s2_ble_records_per_fragment(size_t record_size, size_t mtu_payload);

/* Fragments needed for `records` records. Zero records still needs one fragment. */
unsigned s2_ble_frag_count(unsigned records, size_t record_size, size_t mtu_payload);

/*
 * Pack one fragment. Returns the bytes written, or 0 when `frag` is past the end
 * of the message or `out` is too small for the header plus one record.
 */
size_t s2_ble_pack_snapshot(uint8_t *out, size_t outlen, size_t mtu_payload, uint16_t seq,
                            unsigned frag, const s2_ble_record_t *recs, unsigned n);

/*
 * The catalog is generated straight from the DBC tables rather than from an
 * array, so it needs no caller-side storage.
 */
size_t s2_ble_pack_catalog(uint8_t *out, size_t outlen, size_t mtu_payload, uint16_t seq,
                           unsigned frag);

/* ------------------------------------------------------------ text form --- */

/*
 * Render records as "name=value unit" lines separated by '\n', for the Nordic
 * UART Service. A stale value gets a trailing '*' and a never-seen one is
 * skipped, matching what summary.c prints.
 *
 * Writes a NUL-terminated string of at most outlen-1 bytes and reports its
 * length in *out_len. Returns the index of the first record it did NOT render,
 * so a caller streams a long dump by passing that back as `start`. Always makes
 * progress when outlen holds one line, so a caller cannot loop forever.
 */
unsigned s2_ble_render_text(char *out, size_t outlen, size_t *out_len, const s2_ble_record_t *recs,
                            unsigned n, unsigned start);

/* ------------------------------------------------------------- commands --- */

typedef enum {
    S2_BLE_CMD_EMPTY = 0,    /* blank line: ignore */
    S2_BLE_CMD_HELP,
    S2_BLE_CMD_ALL,          /* one-shot dump of every signal as text */
    S2_BLE_CMD_STREAM_ON,
    S2_BLE_CMD_STREAM_OFF,
    S2_BLE_CMD_RATE,         /* arg = Hz */
    S2_BLE_CMD_INFO,
    S2_BLE_CMD_CATALOG,      /* one-shot binary catalog on the stream characteristic */
    S2_BLE_CMD_BAD_ARG,      /* recognised verb, unusable argument */
    S2_BLE_CMD_UNKNOWN,
} s2_ble_cmd_t;

#define S2_BLE_RATE_MIN 1
#define S2_BLE_RATE_MAX 20

typedef struct {
    s2_ble_cmd_t cmd;
    int arg;
} s2_ble_cmd_result_t;

/*
 * Parse one command. Leading and trailing whitespace and a trailing CR or LF are
 * ignored, and the verb is case-insensitive.
 *
 * Every command only changes what this device reports. None of them can leave
 * listen-only mode, transmit a CAN frame or start a UDS request: anyone in radio
 * range can connect, so the command channel must never reach the bus.
 */
s2_ble_cmd_result_t s2_ble_parse_cmd(const char *in, size_t len);

/* The text `help` replies with. */
const char *s2_ble_help_text(void);

/* ------------------------------------------------------------- helpers --- */

/* Age in deciseconds, clamped to S2_BLE_AGE_MAX; S2_BLE_AGE_NEVER when !valid. */
uint16_t s2_ble_age_ds(int64_t now_us, int64_t ts_us, bool valid);

#ifdef __cplusplus
}
#endif
