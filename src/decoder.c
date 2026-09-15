#include "decoder.h"

#include <stdio.h>
#include <string.h>

#include "dbc_decode.h"
#include "e2e.h"
#include "log_writer.h"
#include "s2_overlay.h"
#include "sdkconfig.h"
#include "uds_client.h"
#include "vehicle_state.h"

static decoder_stats_t s_stats;
static int64_t s_last_event_log_us[S2_DBC_MESSAGE_COUNT];
#if CONFIG_S2_LOG_SIGNAL_CHANGES
static int64_t s_last_change_log_us[S2_SIG__COUNT];
static uint64_t s_last_logged_raw[S2_SIG__COUNT];
static bool s_logged_once[S2_SIG__COUNT];
#endif

#define UDS_ID_MIN 0x7D0u
#define UDS_ID_MAX 0x7FFu
/* Room left for the "[ssss.mmm] 0xIII NAME:" prefix in a change line. */
#define CHANGE_LINE_MAX (LOG_LINE_MAX - 64)

static void hex_bytes(char *out, size_t outlen, const uint8_t *d, uint8_t n, bool spaced)
{
    size_t pos = 0;
    for (uint8_t i = 0; i < n && pos + 3 < outlen; i++) {
        pos += (size_t)snprintf(out + pos, outlen - pos, spaced ? "%02X " : "%02X", d[i]);
    }
    if (spaced && pos > 0) {
        out[pos - 1] = '\0';
    } else {
        out[pos] = '\0';
    }
}

#if CONFIG_S2_LOG_RAW_FRAMES
static void log_raw(const can_frame_t *f)
{
    char hex[24];
    hex_bytes(hex, sizeof(hex), f->data, f->dlc, false);
    log_line("(%lld.%06lld) can0 %0*lX%s#%s", (long long)(f->timestamp_us / 1000000),
             (long long)(f->timestamp_us % 1000000), f->extended ? 8 : 3, (unsigned long)f->id,
             f->rtr ? "R" : "", hex);
}
#endif

static void handle_unknown(const can_frame_t *f, bool diag)
{
    /* Key extended ids separately from standard ids that share the same number. */
    uint32_t key = f->id | (f->extended ? 0x80000000u : 0u);
    bool changed = false;
    vs_lock();
    unsigned table_len = 0;
    const vs_unknown_t *table = vs_unknown_table(&table_len);
    vs_unknown_t *slot = vs_note_unknown(key, f->data, f->dlc, f->timestamp_us, &changed);
    uint32_t count = slot ? slot->count : 0;
    unsigned idx = slot ? (unsigned)(slot - table) : VS_UNKNOWN_MAX;
    vs_unlock();
#if CONFIG_S2_LOG_UNKNOWN_IDS
    /* First sighting always; afterwards only when the payload changed, at most ~1/s per id. */
    static int64_t last_us[VS_UNKNOWN_MAX];
    if (slot && changed && idx < VS_UNKNOWN_MAX &&
        (count == 1 || f->timestamp_us - last_us[idx] > 1000000)) {
        last_us[idx] = f->timestamp_us;
        char hex[32];
        hex_bytes(hex, sizeof(hex), f->data, f->dlc, true);
        log_tline("%s 0x%0*lX%s%s [%u] %s", diag ? "DIAG" : "UNKNOWN", f->extended ? 8 : 3, (unsigned long)f->id,
                  f->rtr ? " RTR" : "", count == 1 ? " first seen" : "", f->dlc, hex);
    }
#else
    (void)count;
    (void)idx;
    (void)diag;
#endif
}

void decoder_handle_frame(const can_frame_t *f)
{
    s_stats.frames++;
#if CONFIG_S2_LOG_RAW_FRAMES
    log_raw(f);
#endif
    if (f->extended || f->rtr) {
        handle_unknown(f, false);
        s_stats.unknown++;
        return;
    }
    if (f->id >= UDS_ID_MIN && f->id <= UDS_ID_MAX) {
        s_stats.diag++;
        uds_client_on_frame(f);
        handle_unknown(f, true);
        return;
    }

    const s2_message_def_t *msg = s2_dbc_find_message(f->id);
    if (!msg) {
        s_stats.unknown++;
        handle_unknown(f, false);
        return;
    }
    s_stats.known++;
    uint16_t msg_index = (uint16_t)(msg - s2_dbc_messages);

    char line[CHANGE_LINE_MAX];
    size_t pos = 0;
    uint32_t lost = 0;
    bool crc_bad = false;

    vs_lock();
    vs_message_t *ms = vs_message(msg_index);
    ms->count++;
    if (ms->count == 1) {
        ms->first_ts_us = f->timestamp_us;
    }
    ms->last_ts_us = f->timestamp_us;
    ms->last_dlc = f->dlc;
    memcpy(ms->last_data, f->data, 8);
    if (f->dlc != msg->dlc) {
        ms->dlc_mismatch++;
        s_stats.dlc_mismatch++;
    }

    if (msg->flags & S2_MSG_E2E) {
        if (f->dlc == 8 && s2_e2e_crc_ok(f->data, 8)) {
            lost = s2_e2e_track_alive(&ms->alive, s2_e2e_alive(f->data));
            if (lost) {
                s_stats.alive_gaps++;
                s_stats.frames_lost += lost;
            }
        } else {
            ms->crc_fail++;
            s_stats.crc_fail++;
            crc_bad = true;
            /* The counter byte of a corrupted frame is not trustworthy: restart gap accounting. */
            ms->alive.have_last = false;
        }
    }

    bool decode = true;
#if CONFIG_S2_E2E_SKIP_BAD_CRC
    decode = !crc_bad;
#endif
    if (decode) {
        for (unsigned i = 0; i < msg->signal_count; i++) {
            const s2_signal_def_t *sig = &msg->signals[i];
            if (!s2_signal_fits(sig, f->dlc)) {
                continue;
            }
            uint16_t sidx = (uint16_t)(msg->first_signal + i);
            uint64_t raw = s2_extract_raw(f->data, sig);
            double value = s2_raw_to_physical(sig, raw);
            (void)vs_update_signal(sidx, raw, value, f->timestamp_us);   /* state counts every change */
#if CONFIG_S2_LOG_SIGNAL_CHANGES
            /*
             * Log when the value differs from what was last PRINTED (not from the
             * previous frame), so a change suppressed by the rate limit is emitted
             * as soon as the interval has elapsed and the final state is never lost.
             */
            bool pending = !s_logged_once[sidx] || s_last_logged_raw[sidx] != raw;
            if (pending && !s2_overlay_suppress_change_log(sidx) &&
                (f->timestamp_us - s_last_change_log_us[sidx]) >= (int64_t)CONFIG_S2_LOG_CHANGE_MIN_INTERVAL_MS * 1000) {
                s_last_change_log_us[sidx] = f->timestamp_us;
                s_last_logged_raw[sidx] = raw;
                s_logged_once[sidx] = true;
                char val[32];
                s2_format_value(sig, value, val, sizeof(val));
                const char *desc = s2_overlay_describe(sidx, raw);
                s2_confidence_t conf = s2_overlay_confidence(sidx);
                const char *mark = conf == S2_CONF_TENTATIVE ? "?" : "";
                int n;
                if (desc) {
                    n = snprintf(line + pos, sizeof(line) - pos, " %s=%s%s(%s)", sig->name, val, mark, desc);
                } else if (sig->unit[0]) {
                    n = snprintf(line + pos, sizeof(line) - pos, " %s=%s%s %s", sig->name, val, mark, sig->unit);
                } else if (sig->length <= 8 && sig->factor == 1.0 && sig->offset == 0.0) {
                    n = snprintf(line + pos, sizeof(line) - pos, " %s=%s%s(0x%02llX)", sig->name, val, mark,
                                 (unsigned long long)raw);
                } else {
                    n = snprintf(line + pos, sizeof(line) - pos, " %s=%s%s", sig->name, val, mark);
                }
                if (n > 0 && pos + (size_t)n < sizeof(line)) {
                    pos += (size_t)n;
                } else if (n > 0) {
                    pos = sizeof(line) - 1;   /* truncated: keep what fits */
                }
            }
#endif
        }
        s2_overlay_on_frame(msg, f, !crc_bad);
    }
    uint32_t crc_fail_total = ms->crc_fail;
    uint32_t gaps_total = ms->alive.gaps;
    vs_unlock();

    if (pos > 0) {
        log_tline("0x%03X %s:%s", msg->id, msg->name, line);
    }
    if (crc_bad && (f->timestamp_us - s_last_event_log_us[msg_index]) > 2000000) {
        s_last_event_log_us[msg_index] = f->timestamp_us;
        char hex[32];
        hex_bytes(hex, sizeof(hex), f->data, f->dlc, true);
        log_tline("0x%03X %s: E2E CRC FAIL (%lu so far) %s", msg->id, msg->name, (unsigned long)crc_fail_total, hex);
    } else if (lost && (f->timestamp_us - s_last_event_log_us[msg_index]) > 2000000) {
        s_last_event_log_us[msg_index] = f->timestamp_us;
        log_tline("0x%03X %s: alive counter gap, ~%lu frame(s) missed (%lu gaps total)", msg->id, msg->name,
                  (unsigned long)lost, (unsigned long)gaps_total);
    }
}

void decoder_task(void *arg)
{
    (void)arg;
    can_frame_t f;
    for (;;) {
        if (can_bus_receive(&f, pdMS_TO_TICKS(500))) {
            decoder_handle_frame(&f);
        }
    }
}

void decoder_get_stats(decoder_stats_t *out)
{
    *out = s_stats;
}
