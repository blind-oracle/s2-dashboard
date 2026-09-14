#include "uds_decode.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "log_writer.h"
#include "sdkconfig.h"

static uint16_t be16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

static const char *status_tag(uint8_t status)
{
    switch (status) {
    case S2_UDS_CONFIRMED: return "";
    case S2_UDS_CANDIDATE: return "? ";
    default:               return "REFUTED ";
    }
}

#if CONFIG_S2_UDS_LOG_RAW
static void hexdump(const char *prefix, const uint8_t *p, size_t n)
{
    char line[LOG_LINE_MAX];
    for (size_t off = 0; off < n; off += 32) {
        size_t pos = (size_t)snprintf(line, sizeof(line), "%s[%3u]", prefix, (unsigned)off);
        for (size_t i = off; i < n && i < off + 32 && pos + 3 < sizeof(line); i++) {
            pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " %02X", p[i]);
        }
        log_line("%s", line);
    }
}
#endif

static bool is_printable_ascii(const uint8_t *p, size_t n)
{
    size_t printable = 0;
    for (size_t i = 0; i < n; i++) {
        if (p[i] >= 0x20 && p[i] < 0x7F) {
            printable++;
        } else if (p[i] != 0) {
            return false;
        }
    }
    return n > 0 && printable >= (n + 1) / 2;
}

static void ascii_line(char *out, size_t outlen, const uint8_t *p, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; i < n && j + 1 < outlen; i++) {
        if (p[i] >= 0x20 && p[i] < 0x7F) {
            out[j++] = (char)p[i];
        }
    }
    out[j] = '\0';
}

static const char *ride_mode_ordinal_name(uint8_t ordinal)
{
    switch (ordinal) {
    case 1: return "Road";
    case 2: return "Sport";
    case 3: return "Rain";
    case 4: return "Range";
    case 5: return "Flat Track";
    case 8: return "Custom A";
    case 9: return "Custom B";
    default: return "?";
    }
}

/* Array statistics helpers for the RESS per-cell DIDs. */
static void u16_array_stats(const uint8_t *p, size_t count, unsigned *min, unsigned *max, double *mean, unsigned *argmin, unsigned *argmax)
{
    *min = 0xFFFF;
    *max = 0;
    *mean = 0;
    *argmin = *argmax = 0;
    for (size_t i = 0; i < count; i++) {
        unsigned v = be16(p + 2 * i);
        if (v < *min) { *min = v; *argmin = (unsigned)i; }
        if (v > *max) { *max = v; *argmax = (unsigned)i; }
        *mean += v;
    }
    if (count) {
        *mean /= (double)count;
    }
}

static void log_u16_array(const char *prefix, const uint8_t *p, size_t count, int offset)
{
    char line[LOG_LINE_MAX];
    for (size_t start = 0; start < count; start += 16) {
        size_t pos = (size_t)snprintf(line, sizeof(line), "%s[%2u..%2u]", prefix, (unsigned)start,
                                      (unsigned)((start + 16 < count ? start + 16 : count) - 1));
        for (size_t i = start; i < count && i < start + 16 && pos + 6 < sizeof(line); i++) {
            pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " %d", (int)be16(p + 2 * i) + offset);
        }
        log_line("%s", line);
    }
}

static void log_u8_array(const char *prefix, const uint8_t *p, size_t count, int offset)
{
    char line[LOG_LINE_MAX];
    for (size_t start = 0; start < count; start += 24) {
        size_t pos = (size_t)snprintf(line, sizeof(line), "%s[%2u..%2u]", prefix, (unsigned)start,
                                      (unsigned)((start + 24 < count ? start + 24 : count) - 1));
        for (size_t i = start; i < count && i < start + 24 && pos + 5 < sizeof(line); i++) {
            pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " %d", (int)p[i] + offset);
        }
        log_line("%s", line);
    }
}

/* Returns true when a specific decoder produced output. */
static bool decode_specific(const s2_uds_module_t *m, const s2_uds_did_t *d, const uint8_t *p, size_t n, char *out, size_t outlen)
{
    char pfx[40];
    snprintf(pfx, sizeof(pfx), "UDS %s %04X", m->name, d->did);
    out[0] = '\0';

    switch (d->module) {
    case S2_UDS_MOD_BCM:
        if (d->did == 0x0226 && n >= 8) {
            snprintf(out, outlen, "uptime tick=%lu  wall-clock unix=%lu (RTC never synced, use as uptime anchor only)",
                     (unsigned long)be32(p), (unsigned long)be32(p + 4));
            return true;
        }
        if (d->did == 0x0202 && n >= 9) {
            size_t words = n / 2;
            size_t pos = (size_t)snprintf(out, outlen, "ADC snapshot b8=%u (long-accel, rest ~106): words", p[8]);
            for (size_t i = 0; i < words && pos + 7 < outlen; i++) {
                pos += (size_t)snprintf(out + pos, outlen - pos, " %u", be16(p + 2 * i));
            }
            return true;
        }
        if (d->did == 0x0204 && n >= 8) {
            snprintf(out, outlen, "rails mV: %u %u %u %u", be16(p), be16(p + 2), be16(p + 4), be16(p + 6));
            return true;
        }
        break;

    case S2_UDS_MOD_VSC:
        if (d->did == 0x020C && n >= 3) {
            int rpm = (int)be16(p) - 0x8000;
            snprintf(out, outlen, "motor %d rpm (signed, <0 = reverse)  drive-state 0x%02X (%s)", rpm, p[2],
                     p[2] == 0x01 ? "stop" : p[2] == 0x02 ? "drive" : "?");
            return true;
        }
        if (d->did == 0x0207 && n >= 10) {
            snprintf(out, outlen, "torque counts (offset 5000): drive ceiling %d  regen limit %d  requested %d  applied? %d  delivered %d",
                     (int)be16(p) - 5000, (int)be16(p + 2) - 5000, (int)be16(p + 4) - 5000,
                     (int)be16(p + 6) - 5000, (int)be16(p + 8) - 5000);
            return true;
        }
        if (d->did == 0x0205 && n >= 1) {
            snprintf(out, outlen, "twist-grip raw %u (~%.0f %% throttle, scale ~2.7 ct/%%; b1:b2 %u/%u bidirectional?)",
                     p[0], p[0] / 2.7, n > 1 ? p[1] : 0, n > 2 ? p[2] : 0);
            return true;
        }
        if (d->did == 0x0220 && n >= 6) {
            snprintf(out, outlen, "pack terminal %u V (live)  nominal %u V  b8=0x%02X (%s)", be16(p + 4), be16(p),
                     n > 8 ? p[8] : 0, n > 8 ? (p[8] == 1 ? "ride/park" : p[8] == 0 ? "charge" : "?") : "-");
            return true;
        }
        if (d->did == 0x0203 && n >= 3) {
            snprintf(out, outlen, "coarse speed b2=%u (~ broadcast speed raw / 10.1)", p[2]);
            return true;
        }
        if (d->did == 0x020A && n >= 1) {
            snprintf(out, outlen, "active ride mode ordinal %u (%s)", p[0], ride_mode_ordinal_name(p[0]));
            return true;
        }
        if (d->did == 0x020B && n >= 6) {
            snprintf(out, outlen, "mode calibration P1 power=%u  P2 regen?=%u  P3 throttle-response=%u (0-100; hi bytes 0x%02X%02X%02X)",
                     le16(p) & 0xFF, le16(p + 2) & 0xFF, le16(p + 4) & 0xFF, p[1], p[3], p[5]);
            return true;
        }
        break;

    case S2_UDS_MOD_RHCM:
        if (d->did == 0x029A && n >= 3) {
            snprintf(out, outlen, "brake applied (front OR rear) = %u", p[2]);
            return true;
        }
        break;

    case S2_UDS_MOD_RESS:
        if (d->did == 0x0205 && n >= 2) {
            size_t cells = n / 2;
            unsigned mn, mx, imn, imx;
            double mean;
            u16_array_stats(p, cells, &mn, &mx, &mean, &imn, &imx);
            double sum_v = 0;
            for (size_t i = 0; i < cells; i++) {
                sum_v += be16(p + 2 * i);
            }
            snprintf(out, outlen, "%u cells: min %u mV (#%u)  max %u mV (#%u)  avg %.1f mV  spread %u mV  sum %.1f V",
                     (unsigned)cells, mn, imn, mx, imx, mean, mx - mn, sum_v / 1000.0);
            log_tline("%s %s", pfx, out);
            log_u16_array("   cell mV ", p, cells, 0);
            out[0] = '\0';
            return true;
        }
        if (d->did == 0x0206 && n >= 1) {
            unsigned mn = 255, mx = 0;
            double mean = 0;
            for (size_t i = 0; i < n; i++) {
                if (p[i] < mn) mn = p[i];
                if (p[i] > mx) mx = p[i];
                mean += p[i];
            }
            mean /= (double)n;
            snprintf(out, outlen, "%u temp sensors: min %d C  max %d C  mean %.1f C (raw-40)", (unsigned)n,
                     (int)mn - 40, (int)mx - 40, mean - 40.0);
            log_tline("%s %s", pfx, out);
            log_u8_array("   temp C ", p, n, -40);
            out[0] = '\0';
            return true;
        }
        if (d->did == 0x0209 && n >= 5) {
            double ia = ((double)be16(p) - 1562.0) / 3.888;
            double ib = ((double)be16(p + 3) - 1562.0) / 3.888;
            snprintf(out, outlen, "pack current A=%.1f A (raw %u)  B=%.1f A (raw %u, saturates ~-33 A)  (+charge / -discharge)",
                     ia, be16(p), ib, be16(p + 3));
            return true;
        }
        if (d->did == 0x021F && n >= 2) {
            snprintf(out, outlen, "pack terminal voltage ref: word0 %u (~1 V/count)", be16(p));
            return true;
        }
        if (d->did == 0x0213 && n >= 2) {
            snprintf(out, outlen, "fine SoC/SoE %.2f %% (voltage-instantaneous, ~18-22 pts above display SoC)", be16(p) / 100.0);
            return true;
        }
        if (d->did == 0x020E && n >= 1) {
            snprintf(out, outlen, "SoH-like %u %%", p[0]);
            return true;
        }
        if (d->did == 0x020D && n >= 9) {
            snprintf(out, outlen, "display/coulomb SoC %u %%  temps: mean %d C  min ~%d C  max ~%d C", p[0],
                     (int)p[4] - 40, (int)p[3] - 40, (int)p[6] - 40);
            return true;
        }
        if (d->did == 0x020C && n >= 2) {
            snprintf(out, outlen, "FF-window over current low byte: 0x%02X 0x%02X", p[0], p[1]);
            return true;
        }
        if ((d->did == 0x025A || d->did == 0x025B || d->did == 0x025C) && n >= 4) {
            size_t words = n / 4;
            size_t pos = (size_t)snprintf(out, outlen, "%u x u32 accumulators:", (unsigned)words);
            for (size_t i = 0; i < words && pos + 12 < outlen; i++) {
                pos += (size_t)snprintf(out + pos, outlen - pos, " %lu", (unsigned long)be32(p + 4 * i));
            }
            return true;
        }
        if (d->did == 0x0204 && n >= 8) {
            snprintf(out, outlen, "live estimate w0=%lu w1=%lu trailing=%u", (unsigned long)be32(p),
                     (unsigned long)be32(p + 4), n > 8 ? p[8] : 0);
            return true;
        }
        break;

    case S2_UDS_MOD_EHCU:
        if (d->did == 0x020F && n >= 4) {
            snprintf(out, outlen, "wheel speed front %.1f km/h (raw %u)  rear %.1f km/h (raw %u)  (~250 ct/km/h, unsigned)",
                     be16(p) / 250.0, be16(p), be16(p + 2) / 250.0, be16(p + 2));
            return true;
        }
        if (d->did == 0x0212 && n >= 6) {
            snprintf(out, outlen, "front brake pressure raw %u (~0.79 x broadcast 0x324)  lever nibble %u",
                     be16(p + 4), n > 6 ? (p[6] & 0x0F) : (p[5] & 0x0F));
            return true;
        }
        if (d->did == 0x0210 && n >= 2) {
            snprintf(out, outlen, "brake active flag = %u", p[1]);
            return true;
        }
        if (d->did == 0x0200 && n >= 4) {
            snprintf(out, outlen, "coarse speed b0:b1 %u/%u  aux rail b3=%u (~%.1f V @ 0.08 V/ct)", p[0], p[1], p[3], p[3] * 0.08);
            return true;
        }
        if (d->did == 0x0211 && n >= 1) {
            snprintf(out, outlen, "12 V aux rail raw %u (~%.1f V)", p[0], p[0] * 0.08);
            return true;
        }
        if (d->did == 0x021D && n >= 2) {
            snprintf(out, outlen, "use counter %u (resets on power loss; not a temperature)", be16(p));
            return true;
        }
        break;

    case S2_UDS_MOD_OBC_HV:
        if (d->did == 0x0232 && n >= 4) {
            bool charging = p[0] == 0x02 && p[1] == 0x01;
            snprintf(out, outlen, "plug/charger state %02X %02X %02X %02X -> %s", p[0], p[1], p[2], p[3],
                     charging ? "CHARGING" : (p[0] == 0 && p[1] == 0) ? "not charging" : "?");
            return true;
        }
        if (d->did == 0x0209 && n >= 2) {
            snprintf(out, outlen, "charge current indicator %u (0 = idle, ~79 @ 11 A CC, ~36 @ 4 A CV)", p[1]);
            return true;
        }
        if (d->did == 0x0201 && n >= 6) {
            snprintf(out, outlen, "charger heatsink raw %u (candidate x0.1 C - 40 = %.1f C)", be16(p + 4), be16(p + 4) / 10.0 - 40.0);
            return true;
        }
        if (d->did == 0x0235 && n >= 6) {
            snprintf(out, outlen, "DC-link %.1f V (nominal under drive)  coarse pack byte %u (~%.0f V)", be16(p + 3) / 10.0,
                     p[5], p[5] * 2.55);
            return true;
        }
        if (d->did == 0x0200 && n >= 2) {
            snprintf(out, outlen, "nominal pack/DC-link %.1f V%s", be16(p) / 10.0,
                     n >= 12 ? (p[8] == 0x02 && p[9] == 0x01 ? "  plug-state: charging" : "  plug-state: not charging") : "");
            return true;
        }
        break;

    case S2_UDS_MOD_TCU:
        if (d->did == 0x0200 && n >= 8) {
            uint32_t a = be32(p), b = be32(p + 4);
            float lat, lon;
            memcpy(&lat, &a, 4);
            memcpy(&lon, &b, 4);
            snprintf(out, outlen, "GPS lat %.6f lon %.6f (coarse refresh, minutes)", (double)lat, (double)lon);
            return true;
        }
        if (d->did == 0x0297 && n >= 1) {
            snprintf(out, outlen, "signal/service presence %u (0 = de-registered, 141 = reacquire)", p[0]);
            return true;
        }
        break;

    default:
        break;
    }

    if (is_printable_ascii(p, n) && n >= 3) {
        char txt[128];
        ascii_line(txt, sizeof(txt), p, n);
        snprintf(out, outlen, "\"%s\"", txt);
        return true;
    }
    return false;
}

void uds_decode_log(const s2_uds_module_t *m, const s2_uds_did_t *d, const uint8_t *p, size_t n)
{
    char text[LOG_LINE_MAX - 64];
    bool decoded = decode_specific(m, d, p, n, text, sizeof(text));
    if (decoded && text[0]) {
        log_tline("UDS %s %04X %s%s: %s", m->name, d->did, status_tag(d->status), d->name, text);
    } else if (!decoded) {
        log_tline("UDS %s %04X %s%s: %u bytes", m->name, d->did, status_tag(d->status), d->name, (unsigned)n);
    }
#if CONFIG_S2_UDS_LOG_RAW
    if (!decoded || n <= 16) {
        char pfx[24];
        snprintf(pfx, sizeof(pfx), "   %s %04X", m->name, d->did);
        hexdump(pfx, p, n);
    }
#endif
}
