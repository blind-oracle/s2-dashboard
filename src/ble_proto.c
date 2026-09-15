#include "ble_proto.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dbc_decode.h"
#include "s2_dbc_gen.h"

_Static_assert(sizeof(float) == 4, "the wire format stores IEEE-754 binary32");

/*
 * A text line is marked stale on the same 5 s rule the serial summary uses, so
 * the two outputs never disagree about whether a value is current.
 */
#define TEXT_STALE_DS 50u

/* Longest possible line: 24-char name, 31-char value, 30-char unit, marker, NL. */
#define TEXT_LINE_MAX 128

/* ------------------------------------------------------- little-endian --- */

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put_f32(uint8_t *p, float v)
{
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    put_u32(p, bits);
}

/* Copy a NUL-padded fixed-width field, truncating silently. */
static void put_str(uint8_t *p, size_t width, const char *s)
{
    memset(p, 0, width);
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    if (n > width - 1) {
        n = width - 1;          /* always leave room for the terminator */
    }
    memcpy(p, s, n);
}

/* ------------------------------------------------------------ dashboard --- */

size_t s2_ble_pack_dash(uint8_t *out, size_t outlen, const s2_ble_dash_t *d)
{
    if (!out || !d || outlen < S2_BLE_DASH_SIZE) {
        return 0;
    }
    uint16_t fs = (uint16_t)((d->power_fs & 3u) | ((d->torque_fs & 3u) << 2) | ((d->volts_fs & 3u) << 4) |
                             ((d->energy_fs & 3u) << 6) | ((d->soh_fs & 3u) << 8));
    out[0] = (uint8_t)S2_BLE_PROTO_VERSION;
    out[1] = d->uds_enabled ? S2_BLE_DASH_FLAG_UDS : 0u;
    put_u16(out + 2, fs);
    put_f32(out + 4, d->power_kw);
    put_f32(out + 8, d->torque_nm);
    put_f32(out + 12, d->volts);
    put_f32(out + 16, d->session_kwh);
    put_f32(out + 20, d->soh_pct);
    return S2_BLE_DASH_SIZE;
}

/* ------------------------------------------------------- fragmentation --- */

unsigned s2_ble_records_per_fragment(size_t record_size, size_t mtu_payload)
{
    if (record_size == 0 || mtu_payload <= S2_BLE_FRAG_HEADER) {
        return 0;
    }
    return (unsigned)((mtu_payload - S2_BLE_FRAG_HEADER) / record_size);
}

unsigned s2_ble_frag_count(unsigned records, size_t record_size, size_t mtu_payload)
{
    unsigned per = s2_ble_records_per_fragment(record_size, mtu_payload);
    if (per == 0) {
        return 0;
    }
    if (records == 0) {
        return 1;               /* an empty message still says "complete, nothing in it" */
    }
    return (records + per - 1) / per;
}

/*
 * Shared prologue: validate the fragment index, work out which records it spans
 * and write the header. Returns the write offset for the first record, or 0 when
 * this fragment cannot be produced.
 */
static size_t frag_header(uint8_t *out, size_t outlen, size_t mtu_payload, uint8_t msg_type,
                          uint16_t seq, unsigned frag, unsigned total_records, size_t record_size,
                          unsigned *first, unsigned *count)
{
    unsigned per = s2_ble_records_per_fragment(record_size, mtu_payload);
    if (!out || per == 0) {
        return 0;
    }
    unsigned total = s2_ble_frag_count(total_records, record_size, mtu_payload);
    if (frag >= total || total > 255u) {
        return 0;
    }
    unsigned f = frag * per;
    unsigned c = 0;
    if (f < total_records) {
        c = total_records - f;
        if (c > per) {
            c = per;
        }
    }
    if (outlen < S2_BLE_FRAG_HEADER + (size_t)c * record_size) {
        return 0;
    }
    out[0] = (uint8_t)S2_BLE_PROTO_VERSION;
    out[1] = msg_type;
    out[2] = (uint8_t)frag;
    out[3] = (uint8_t)total;
    put_u16(out + 4, (uint16_t)c);
    put_u16(out + 6, seq);
    *first = f;
    *count = c;
    return S2_BLE_FRAG_HEADER;
}

size_t s2_ble_pack_snapshot(uint8_t *out, size_t outlen, size_t mtu_payload, uint16_t seq,
                            unsigned frag, const s2_ble_record_t *recs, unsigned n)
{
    unsigned first = 0, count = 0;
    size_t pos = frag_header(out, outlen, mtu_payload, S2_BLE_MSG_SNAPSHOT, seq, frag, n,
                             S2_BLE_SNAP_RECORD, &first, &count);
    if (pos == 0) {
        return 0;
    }
    if (count && !recs) {
        return 0;
    }
    for (unsigned i = 0; i < count; i++) {
        const s2_ble_record_t *r = &recs[first + i];
        put_u16(out + pos, r->index);
        put_u16(out + pos + 2, r->age_ds);
        put_f32(out + pos + 4, r->value);
        pos += S2_BLE_SNAP_RECORD;
    }
    return pos;
}

size_t s2_ble_pack_catalog(uint8_t *out, size_t outlen, size_t mtu_payload, uint16_t seq,
                           unsigned frag)
{
    unsigned first = 0, count = 0;
    size_t pos = frag_header(out, outlen, mtu_payload, S2_BLE_MSG_CATALOG, seq, frag,
                             (unsigned)S2_SIG__COUNT, S2_BLE_CAT_RECORD, &first, &count);
    if (pos == 0) {
        return 0;
    }
    for (unsigned i = 0; i < count; i++) {
        uint16_t idx = (uint16_t)(first + i);
        const s2_signal_def_t *def = s2_dbc_signal(idx);
        const s2_message_def_t *msg = s2_dbc_message_of_signal(idx);
        put_u16(out + pos, idx);
        put_u16(out + pos + 2, msg ? msg->id : 0u);
        put_f32(out + pos + 4, def ? def->min : 0.0f);
        put_f32(out + pos + 8, def ? def->max : 0.0f);
        put_str(out + pos + 12, S2_BLE_CAT_NAME, def ? def->name : "");
        put_str(out + pos + 40, S2_BLE_CAT_UNIT, def ? def->unit : "");
        pos += S2_BLE_CAT_RECORD;
    }
    return pos;
}

/* ------------------------------------------------------------ text form --- */

unsigned s2_ble_render_text(char *out, size_t outlen, size_t *out_len, const s2_ble_record_t *recs,
                            unsigned n, unsigned start)
{
    size_t pos = 0;
    unsigned i = start;

    if (!out || outlen == 0) {
        if (out_len) {
            *out_len = 0;
        }
        return start;
    }
    out[0] = '\0';
    if (!recs) {
        n = 0;
    }

    for (; i < n; i++) {
        const s2_ble_record_t *r = &recs[i];
        if (r->age_ds == S2_BLE_AGE_NEVER) {
            continue;           /* never seen: omitted, exactly as the summary omits it */
        }
        const s2_signal_def_t *def = s2_dbc_signal(r->index);
        if (!def) {
            continue;
        }
        char val[32];
        s2_format_value(def, (double)r->value, val, sizeof(val));
        const char *stale = r->age_ds > TEXT_STALE_DS ? "*" : "";
        char line[TEXT_LINE_MAX];
        int m;
        if (def->unit[0]) {
            m = snprintf(line, sizeof(line), "%s=%s %s%s\n", def->name, val, def->unit, stale);
        } else {
            m = snprintf(line, sizeof(line), "%s=%s%s\n", def->name, val, stale);
        }
        if (m <= 0) {
            continue;
        }
        size_t len = (size_t)m;
        if (len >= sizeof(line)) {
            len = sizeof(line) - 1;
        }
        if (pos + len >= outlen) {
            if (pos == 0) {
                /*
                 * The buffer cannot even hold one line. Emit a truncated one and
                 * consume the record anyway, so a caller looping on the return
                 * value always makes progress.
                 */
                size_t fit = outlen - 1;
                memcpy(out, line, fit);
                out[fit] = '\0';
                pos = fit;
                i++;
            }
            break;
        }
        memcpy(out + pos, line, len);
        pos += len;
        out[pos] = '\0';
    }

    if (out_len) {
        *out_len = pos;
    }
    return i;
}

/* ------------------------------------------------------------- commands --- */

const char *s2_ble_help_text(void)
{
    return "commands: help, info, all, catalog, stream on|off, rate <1-20>\n"
           "this channel only changes what is reported; it never touches the CAN bus\n";
}

s2_ble_cmd_result_t s2_ble_parse_cmd(const char *in, size_t len)
{
    s2_ble_cmd_result_t r = { S2_BLE_CMD_EMPTY, 0 };
    if (!in) {
        return r;
    }

    size_t b = 0, e = len;
    while (b < e && isspace((unsigned char)in[b])) {
        b++;
    }
    while (e > b && isspace((unsigned char)in[e - 1])) {
        e--;
    }
    if (b == e) {
        return r;               /* blank line, or a bare CR/LF */
    }

    char buf[64];
    size_t n = e - b;
    if (n >= sizeof(buf)) {
        n = sizeof(buf) - 1;
    }
    for (size_t i = 0; i < n; i++) {
        buf[i] = (char)tolower((unsigned char)in[b + i]);
    }
    buf[n] = '\0';

    const char *arg = NULL;
    char *sp = strchr(buf, ' ');
    if (sp) {
        *sp = '\0';
        arg = sp + 1;
        while (*arg == ' ') {
            arg++;
        }
    }

    if (!strcmp(buf, "help") || !strcmp(buf, "?")) {
        r.cmd = S2_BLE_CMD_HELP;
    } else if (!strcmp(buf, "all")) {
        r.cmd = S2_BLE_CMD_ALL;
    } else if (!strcmp(buf, "info")) {
        r.cmd = S2_BLE_CMD_INFO;
    } else if (!strcmp(buf, "catalog")) {
        r.cmd = S2_BLE_CMD_CATALOG;
    } else if (!strcmp(buf, "stream")) {
        if (arg && !strcmp(arg, "on")) {
            r.cmd = S2_BLE_CMD_STREAM_ON;
        } else if (arg && !strcmp(arg, "off")) {
            r.cmd = S2_BLE_CMD_STREAM_OFF;
        } else {
            r.cmd = S2_BLE_CMD_BAD_ARG;
        }
    } else if (!strcmp(buf, "rate")) {
        if (!arg || !*arg) {
            r.cmd = S2_BLE_CMD_BAD_ARG;
        } else {
            char *end = NULL;
            long v = strtol(arg, &end, 10);
            if (!end || *end != '\0' || v < S2_BLE_RATE_MIN || v > S2_BLE_RATE_MAX) {
                r.cmd = S2_BLE_CMD_BAD_ARG;
            } else {
                r.cmd = S2_BLE_CMD_RATE;
                r.arg = (int)v;
            }
        }
    } else {
        r.cmd = S2_BLE_CMD_UNKNOWN;
    }
    return r;
}

/* ------------------------------------------------------------- helpers --- */

uint16_t s2_ble_age_ds(int64_t now_us, int64_t ts_us, bool valid)
{
    if (!valid) {
        return S2_BLE_AGE_NEVER;
    }
    int64_t age = now_us - ts_us;
    if (age < 0) {
        age = 0;                /* a clock that appears to run backwards reads as fresh */
    }
    age /= 100000;              /* microseconds to deciseconds */
    if (age > (int64_t)S2_BLE_AGE_MAX) {
        return S2_BLE_AGE_MAX;
    }
    return (uint16_t)age;
}
