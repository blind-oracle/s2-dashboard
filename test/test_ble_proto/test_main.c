/*
 * Host tests for the BLE wire formats and the command parser.
 *
 * ble_proto.c is deliberately free of NimBLE and ESP-IDF, so the fragment
 * arithmetic, the fixed layouts and the command grammar are all testable here.
 * The transport in ble_telemetry.c is not covered: it is on-target only.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ble_proto.h"
#include "s2_dbc_gen.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

/* Read back a little-endian field the way an Android client would. */
static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static float get_f32(const uint8_t *p)
{
    uint32_t bits = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    float f;
    memcpy(&f, &bits, sizeof(f));
    return f;
}

/* ------------------------------------------------------------ dashboard --- */

static void test_dash_layout(void)
{
    s2_ble_dash_t d = {
        .power_kw = 12.5f,
        .torque_nm = -34.25f,
        .volts = 398.0f,
        .session_kwh = 1.75f,
        .soh_pct = 97.5f,
        .power_fs = S2_BLE_FS_LIVE,
        .torque_fs = S2_BLE_FS_STALE,
        .volts_fs = S2_BLE_FS_LIVE,
        .energy_fs = S2_BLE_FS_MISSING,
        .soh_fs = S2_BLE_FS_STALE,
        .uds_enabled = true,
    };
    uint8_t buf[64];
    memset(buf, 0xAA, sizeof(buf));
    TEST_ASSERT_EQUAL_UINT(S2_BLE_DASH_SIZE, s2_ble_pack_dash(buf, sizeof(buf), &d));

    TEST_ASSERT_EQUAL_UINT8(S2_BLE_PROTO_VERSION, buf[0]);
    TEST_ASSERT_EQUAL_UINT8(S2_BLE_DASH_FLAG_UDS, buf[1]);

    /* Two bits per field, power in the lowest pair. */
    uint16_t fs = get_u16(buf + 2);
    TEST_ASSERT_EQUAL_UINT(S2_BLE_FS_LIVE, fs & 3u);
    TEST_ASSERT_EQUAL_UINT(S2_BLE_FS_STALE, (fs >> 2) & 3u);
    TEST_ASSERT_EQUAL_UINT(S2_BLE_FS_LIVE, (fs >> 4) & 3u);
    TEST_ASSERT_EQUAL_UINT(S2_BLE_FS_MISSING, (fs >> 6) & 3u);
    TEST_ASSERT_EQUAL_UINT(S2_BLE_FS_STALE, (fs >> 8) & 3u);

    TEST_ASSERT_EQUAL_FLOAT(12.5f, get_f32(buf + 4));
    TEST_ASSERT_EQUAL_FLOAT(-34.25f, get_f32(buf + 8));
    TEST_ASSERT_EQUAL_FLOAT(398.0f, get_f32(buf + 12));
    TEST_ASSERT_EQUAL_FLOAT(1.75f, get_f32(buf + 16));
    TEST_ASSERT_EQUAL_FLOAT(97.5f, get_f32(buf + 20));

    /* Nothing written past the frame. */
    TEST_ASSERT_EQUAL_UINT8(0xAA, buf[S2_BLE_DASH_SIZE]);
}

static void test_dash_rejects_short_buffer(void)
{
    s2_ble_dash_t d = { 0 };
    uint8_t buf[S2_BLE_DASH_SIZE];
    TEST_ASSERT_EQUAL_UINT(0, s2_ble_pack_dash(buf, S2_BLE_DASH_SIZE - 1, &d));
    TEST_ASSERT_EQUAL_UINT(0, s2_ble_pack_dash(NULL, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT(0, s2_ble_pack_dash(buf, sizeof(buf), NULL));
}

static void test_dash_clears_uds_flag(void)
{
    s2_ble_dash_t d = { .uds_enabled = false };
    uint8_t buf[S2_BLE_DASH_SIZE];
    TEST_ASSERT_EQUAL_UINT(S2_BLE_DASH_SIZE, s2_ble_pack_dash(buf, sizeof(buf), &d));
    TEST_ASSERT_EQUAL_UINT8(0, buf[1]);
}

/* ------------------------------------------------------- fragmentation --- */

static void test_records_per_fragment(void)
{
    /* A 247-byte ATT payload minus the 8-byte header is 239, so 29 records. */
    TEST_ASSERT_EQUAL_UINT(29, s2_ble_records_per_fragment(S2_BLE_SNAP_RECORD, 247));
    TEST_ASSERT_EQUAL_UINT(4, s2_ble_records_per_fragment(S2_BLE_CAT_RECORD, 247));

    /* No room for the header, or for a single record after it. */
    TEST_ASSERT_EQUAL_UINT(0, s2_ble_records_per_fragment(S2_BLE_SNAP_RECORD, S2_BLE_FRAG_HEADER));
    TEST_ASSERT_EQUAL_UINT(0, s2_ble_records_per_fragment(S2_BLE_SNAP_RECORD, S2_BLE_FRAG_HEADER + 1));
    TEST_ASSERT_EQUAL_UINT(1, s2_ble_records_per_fragment(S2_BLE_SNAP_RECORD,
                                                          S2_BLE_FRAG_HEADER + S2_BLE_SNAP_RECORD));
    TEST_ASSERT_EQUAL_UINT(0, s2_ble_records_per_fragment(0, 247));
}

static void test_frag_count_boundaries(void)
{
    const size_t mtu = S2_BLE_FRAG_HEADER + 4 * S2_BLE_SNAP_RECORD;   /* exactly 4 per fragment */

    /* Zero records still produces one fragment, so a client sees a complete message. */
    TEST_ASSERT_EQUAL_UINT(1, s2_ble_frag_count(0, S2_BLE_SNAP_RECORD, mtu));
    TEST_ASSERT_EQUAL_UINT(1, s2_ble_frag_count(1, S2_BLE_SNAP_RECORD, mtu));
    /* Exactly filling a fragment must not add an empty one. */
    TEST_ASSERT_EQUAL_UINT(1, s2_ble_frag_count(4, S2_BLE_SNAP_RECORD, mtu));
    /* One record over rolls to a second fragment. */
    TEST_ASSERT_EQUAL_UINT(2, s2_ble_frag_count(5, S2_BLE_SNAP_RECORD, mtu));
    TEST_ASSERT_EQUAL_UINT(2, s2_ble_frag_count(8, S2_BLE_SNAP_RECORD, mtu));
    TEST_ASSERT_EQUAL_UINT(3, s2_ble_frag_count(9, S2_BLE_SNAP_RECORD, mtu));

    /* An unusable MTU yields no fragments rather than a division by zero. */
    TEST_ASSERT_EQUAL_UINT(0, s2_ble_frag_count(10, S2_BLE_SNAP_RECORD, 4));
}

static void fill_records(s2_ble_record_t *r, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        r[i].index = (uint16_t)i;
        r[i].age_ds = (uint16_t)i;
        r[i].value = (float)i * 1.5f;
    }
}

static void test_snapshot_fragments_cover_every_record(void)
{
    const size_t mtu = S2_BLE_FRAG_HEADER + 4 * S2_BLE_SNAP_RECORD;
    s2_ble_record_t recs[9];
    fill_records(recs, 9);
    uint8_t buf[256];

    unsigned total = s2_ble_frag_count(9, S2_BLE_SNAP_RECORD, mtu);
    TEST_ASSERT_EQUAL_UINT(3, total);

    unsigned seen = 0;
    for (unsigned f = 0; f < total; f++) {
        size_t len = s2_ble_pack_snapshot(buf, sizeof(buf), mtu, 0x1234, f, recs, 9);
        TEST_ASSERT_GREATER_THAN_UINT(0, len);
        TEST_ASSERT_EQUAL_UINT8(S2_BLE_PROTO_VERSION, buf[0]);
        TEST_ASSERT_EQUAL_UINT8(S2_BLE_MSG_SNAPSHOT, buf[1]);
        TEST_ASSERT_EQUAL_UINT8(f, buf[2]);
        TEST_ASSERT_EQUAL_UINT8(total, buf[3]);
        TEST_ASSERT_EQUAL_UINT16(0x1234, get_u16(buf + 6));

        unsigned count = get_u16(buf + 4);
        TEST_ASSERT_EQUAL_UINT(S2_BLE_FRAG_HEADER + count * S2_BLE_SNAP_RECORD, len);
        TEST_ASSERT_LESS_OR_EQUAL_UINT(mtu, len);

        for (unsigned i = 0; i < count; i++) {
            const uint8_t *rec = buf + S2_BLE_FRAG_HEADER + i * S2_BLE_SNAP_RECORD;
            TEST_ASSERT_EQUAL_UINT16(seen, get_u16(rec));
            TEST_ASSERT_EQUAL_UINT16(seen, get_u16(rec + 2));
            TEST_ASSERT_EQUAL_FLOAT((float)seen * 1.5f, get_f32(rec + 4));
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_UINT(9, seen);

    /* One past the last fragment is refused rather than returning an empty one. */
    TEST_ASSERT_EQUAL_UINT(0, s2_ble_pack_snapshot(buf, sizeof(buf), mtu, 0x1234, total, recs, 9));
}

static void test_snapshot_exact_fill_and_one_over(void)
{
    const size_t mtu = S2_BLE_FRAG_HEADER + 4 * S2_BLE_SNAP_RECORD;
    s2_ble_record_t recs[5];
    fill_records(recs, 5);
    uint8_t buf[256];

    /* Exactly one fragment's worth: a single fragment, completely full. */
    TEST_ASSERT_EQUAL_UINT(1, s2_ble_frag_count(4, S2_BLE_SNAP_RECORD, mtu));
    size_t len = s2_ble_pack_snapshot(buf, sizeof(buf), mtu, 1, 0, recs, 4);
    TEST_ASSERT_EQUAL_UINT(mtu, len);
    TEST_ASSERT_EQUAL_UINT8(1, buf[3]);
    TEST_ASSERT_EQUAL_UINT16(4, get_u16(buf + 4));

    /* One more record: two fragments, the second holding just that record. */
    len = s2_ble_pack_snapshot(buf, sizeof(buf), mtu, 1, 1, recs, 5);
    TEST_ASSERT_EQUAL_UINT(S2_BLE_FRAG_HEADER + S2_BLE_SNAP_RECORD, len);
    TEST_ASSERT_EQUAL_UINT8(2, buf[3]);
    TEST_ASSERT_EQUAL_UINT16(1, get_u16(buf + 4));
    TEST_ASSERT_EQUAL_UINT16(4, get_u16(buf + S2_BLE_FRAG_HEADER));
}

static void test_snapshot_empty_message(void)
{
    uint8_t buf[64];
    size_t len = s2_ble_pack_snapshot(buf, sizeof(buf), 247, 7, 0, NULL, 0);
    TEST_ASSERT_EQUAL_UINT(S2_BLE_FRAG_HEADER, len);
    TEST_ASSERT_EQUAL_UINT8(1, buf[3]);
    TEST_ASSERT_EQUAL_UINT16(0, get_u16(buf + 4));
    TEST_ASSERT_EQUAL_UINT16(7, get_u16(buf + 6));
}

static void test_snapshot_rejects_short_output(void)
{
    s2_ble_record_t recs[4];
    fill_records(recs, 4);
    uint8_t buf[256];
    const size_t mtu = S2_BLE_FRAG_HEADER + 4 * S2_BLE_SNAP_RECORD;
    /* The output buffer, not just the MTU, has to hold the fragment. */
    TEST_ASSERT_EQUAL_UINT(0, s2_ble_pack_snapshot(buf, mtu - 1, mtu, 0, 0, recs, 4));
}

/* --------------------------------------------------------------- catalog --- */

static void test_catalog_describes_every_signal(void)
{
    const size_t mtu = 247;
    uint8_t buf[256];
    unsigned total = s2_ble_frag_count((unsigned)S2_SIG__COUNT, S2_BLE_CAT_RECORD, mtu);
    TEST_ASSERT_GREATER_THAN_UINT(0, total);

    unsigned seen = 0;
    for (unsigned f = 0; f < total; f++) {
        size_t len = s2_ble_pack_catalog(buf, sizeof(buf), mtu, 9, f);
        TEST_ASSERT_GREATER_THAN_UINT(0, len);
        TEST_ASSERT_EQUAL_UINT8(S2_BLE_MSG_CATALOG, buf[1]);
        unsigned count = get_u16(buf + 4);
        for (unsigned i = 0; i < count; i++) {
            const uint8_t *rec = buf + S2_BLE_FRAG_HEADER + i * S2_BLE_CAT_RECORD;
            uint16_t idx = get_u16(rec);
            TEST_ASSERT_EQUAL_UINT16(seen, idx);

            const s2_signal_def_t *def = s2_dbc_signal(idx);
            const s2_message_def_t *msg = s2_dbc_message_of_signal(idx);
            TEST_ASSERT_NOT_NULL(def);
            TEST_ASSERT_NOT_NULL(msg);
            TEST_ASSERT_EQUAL_UINT16(msg->id, get_u16(rec + 2));

            /* Names and units are NUL-terminated even when truncated. */
            const char *name = (const char *)(rec + 12);
            const char *unit = (const char *)(rec + 40);
            TEST_ASSERT_EQUAL_UINT8(0, rec[12 + S2_BLE_CAT_NAME - 1]);
            TEST_ASSERT_EQUAL_UINT8(0, rec[40 + S2_BLE_CAT_UNIT - 1]);
            TEST_ASSERT_EQUAL_STRING_LEN(def->name, name, strlen(name));
            TEST_ASSERT_EQUAL_STRING_LEN(def->unit, unit, strlen(unit));
            seen++;
        }
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)S2_SIG__COUNT, seen);
}

static void test_catalog_truncates_a_long_unit(void)
{
    /*
     * At least one database unit is a descriptive note longer than the field
     * ("coarse yaw (~x0.0018 of 0x126)"), so truncation is a real case and not a
     * hypothetical one.
     */
    bool found_long = false;
    for (uint16_t i = 0; i < (uint16_t)S2_SIG__COUNT; i++) {
        const s2_signal_def_t *def = s2_dbc_signal(i);
        if (def && strlen(def->unit) >= S2_BLE_CAT_UNIT) {
            found_long = true;
            /* One record per fragment makes the arithmetic easy to follow. */
            const size_t mtu = S2_BLE_FRAG_HEADER + S2_BLE_CAT_RECORD;
            uint8_t buf[128];
            size_t len = s2_ble_pack_catalog(buf, sizeof(buf), mtu, 0, i);
            TEST_ASSERT_EQUAL_UINT(mtu, len);
            const uint8_t *rec = buf + S2_BLE_FRAG_HEADER;
            TEST_ASSERT_EQUAL_UINT16(i, get_u16(rec));
            const char *unit = (const char *)(rec + 40);
            TEST_ASSERT_EQUAL_UINT(S2_BLE_CAT_UNIT - 1, strlen(unit));
            TEST_ASSERT_EQUAL_STRING_LEN(def->unit, unit, S2_BLE_CAT_UNIT - 1);
            break;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_long, "expected a database unit longer than the catalog field");
}

/* ------------------------------------------------------------ text form --- */

static void test_text_skips_never_seen(void)
{
    s2_ble_record_t recs[3] = {
        { .index = 0, .age_ds = S2_BLE_AGE_NEVER, .value = 1.0f },
        { .index = 1, .age_ds = 0, .value = 2.0f },
        { .index = 2, .age_ds = S2_BLE_AGE_NEVER, .value = 3.0f },
    };
    char out[256];
    size_t len = 0;
    unsigned next = s2_ble_render_text(out, sizeof(out), &len, recs, 3, 0);

    TEST_ASSERT_EQUAL_UINT(3, next);
    TEST_ASSERT_EQUAL_UINT(len, strlen(out));
    /* Exactly one line, for the one record that has been seen. */
    const char *nl = strchr(out, '\n');
    TEST_ASSERT_NOT_NULL(nl);
    TEST_ASSERT_EQUAL_UINT(len, (size_t)(nl - out) + 1);

    const s2_signal_def_t *def = s2_dbc_signal(1);
    TEST_ASSERT_EQUAL_STRING_LEN(def->name, out, strlen(def->name));
    TEST_ASSERT_EQUAL_CHAR('=', out[strlen(def->name)]);
}

static void test_text_marks_stale(void)
{
    s2_ble_record_t fresh = { .index = 0, .age_ds = 1, .value = 0.0f };
    s2_ble_record_t old = { .index = 0, .age_ds = 600, .value = 0.0f };   /* 60 s */
    char out[128];
    size_t len = 0;

    s2_ble_render_text(out, sizeof(out), &len, &fresh, 1, 0);
    TEST_ASSERT_NULL(strchr(out, '*'));

    s2_ble_render_text(out, sizeof(out), &len, &old, 1, 0);
    TEST_ASSERT_NOT_NULL(strchr(out, '*'));
}

static void test_text_includes_the_unit(void)
{
    /* Signal 0 is an IMU axis, whose unit is "count". */
    const s2_signal_def_t *def = s2_dbc_signal(0);
    TEST_ASSERT_NOT_NULL(def);
    TEST_ASSERT_TRUE(def->unit[0] != '\0');

    s2_ble_record_t r = { .index = 0, .age_ds = 0, .value = 1.0f };
    char out[128];
    size_t len = 0;
    s2_ble_render_text(out, sizeof(out), &len, &r, 1, 0);
    TEST_ASSERT_NOT_NULL(strstr(out, def->unit));
}

static void test_text_resumes_where_it_stopped(void)
{
    s2_ble_record_t recs[20];
    for (unsigned i = 0; i < 20; i++) {
        recs[i].index = (uint16_t)i;
        recs[i].age_ds = 0;
        recs[i].value = 1.0f;
    }
    /* Small enough that a single call cannot render all 20 lines. */
    char out[64];
    size_t len = 0;
    unsigned lines = 0;
    unsigned next = 0;
    unsigned guard = 0;

    while (next < 20 && guard++ < 100) {
        unsigned prev = next;
        next = s2_ble_render_text(out, sizeof(out), &len, recs, 20, next);
        TEST_ASSERT_GREATER_THAN_UINT(prev, next);   /* always makes progress */
        TEST_ASSERT_LESS_THAN_UINT(sizeof(out), len);
        for (const char *p = out; *p; p++) {
            if (*p == '\n') {
                lines++;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT(20, next);
    TEST_ASSERT_EQUAL_UINT(20, lines);
}

static void test_text_makes_progress_in_a_tiny_buffer(void)
{
    s2_ble_record_t r = { .index = 0, .age_ds = 0, .value = 1.0f };
    char out[4];
    size_t len = 0;
    unsigned next = s2_ble_render_text(out, sizeof(out), &len, &r, 1, 0);
    TEST_ASSERT_EQUAL_UINT(1, next);             /* consumed, truncated, not looping */
    TEST_ASSERT_EQUAL_UINT(sizeof(out) - 1, len);
    TEST_ASSERT_EQUAL_UINT(len, strlen(out));
}

/* ------------------------------------------------------------- commands --- */

static s2_ble_cmd_t parse(const char *s)
{
    return s2_ble_parse_cmd(s, strlen(s)).cmd;
}

static void test_commands_recognised(void)
{
    TEST_ASSERT_EQUAL(S2_BLE_CMD_HELP, parse("help"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_HELP, parse("?"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_ALL, parse("all"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_INFO, parse("info"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_CATALOG, parse("catalog"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_STREAM_ON, parse("stream on"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_STREAM_OFF, parse("stream off"));
}

static void test_commands_tolerate_terminal_framing(void)
{
    /* Terminal apps send CR, LF or both, and users add spaces and capitals. */
    TEST_ASSERT_EQUAL(S2_BLE_CMD_HELP, parse("help\r\n"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_HELP, parse("HELP"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_HELP, parse("  Help  \n"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_STREAM_ON, parse("Stream   On\r"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_EMPTY, parse(""));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_EMPTY, parse("\r\n"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_EMPTY, parse("   "));
}

static void test_rate_argument_is_range_checked(void)
{
    s2_ble_cmd_result_t r = s2_ble_parse_cmd("rate 10", 7);
    TEST_ASSERT_EQUAL(S2_BLE_CMD_RATE, r.cmd);
    TEST_ASSERT_EQUAL_INT(10, r.arg);

    r = s2_ble_parse_cmd("rate 1", 6);
    TEST_ASSERT_EQUAL(S2_BLE_CMD_RATE, r.cmd);
    TEST_ASSERT_EQUAL_INT(S2_BLE_RATE_MIN, r.arg);

    char max[16];
    snprintf(max, sizeof(max), "rate %d", S2_BLE_RATE_MAX);
    r = s2_ble_parse_cmd(max, strlen(max));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_RATE, r.cmd);
    TEST_ASSERT_EQUAL_INT(S2_BLE_RATE_MAX, r.arg);

    /* Out of range, non-numeric, partly numeric and missing are all bad args. */
    TEST_ASSERT_EQUAL(S2_BLE_CMD_BAD_ARG, parse("rate 0"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_BAD_ARG, parse("rate 21"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_BAD_ARG, parse("rate 999"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_BAD_ARG, parse("rate -5"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_BAD_ARG, parse("rate abc"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_BAD_ARG, parse("rate 5x"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_BAD_ARG, parse("rate"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_BAD_ARG, parse("stream"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_BAD_ARG, parse("stream maybe"));
}

static void test_unknown_commands(void)
{
    TEST_ASSERT_EQUAL(S2_BLE_CMD_UNKNOWN, parse("bogus"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_UNKNOWN, parse("reset"));
    /* Nothing in the grammar can reach the bus; these must not be commands. */
    TEST_ASSERT_EQUAL(S2_BLE_CMD_UNKNOWN, parse("tx 0x161 0011223344556677"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_UNKNOWN, parse("uds"));
    TEST_ASSERT_EQUAL(S2_BLE_CMD_UNKNOWN, parse("normal"));
}

static void test_help_text_is_present(void)
{
    const char *h = s2_ble_help_text();
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_NOT_NULL(strstr(h, "rate"));
    TEST_ASSERT_NOT_NULL(strstr(h, "catalog"));
}

/* --------------------------------------------------------------- helpers --- */

static void test_age_in_deciseconds(void)
{
    TEST_ASSERT_EQUAL_UINT16(S2_BLE_AGE_NEVER, s2_ble_age_ds(1000000, 0, false));
    TEST_ASSERT_EQUAL_UINT16(0, s2_ble_age_ds(1000000, 1000000, true));
    TEST_ASSERT_EQUAL_UINT16(15, s2_ble_age_ds(2500000, 1000000, true));
    /* Truncates rather than rounds, so a value never reads fresher than it is. */
    TEST_ASSERT_EQUAL_UINT16(1, s2_ble_age_ds(199999, 0, true));
    /* A timestamp in the future reads as fresh instead of wrapping. */
    TEST_ASSERT_EQUAL_UINT16(0, s2_ble_age_ds(0, 5000000, true));
    /* Saturates instead of aliasing onto the never-seen sentinel. */
    TEST_ASSERT_EQUAL_UINT16(S2_BLE_AGE_MAX, s2_ble_age_ds(100000000000LL, 0, true));
    TEST_ASSERT_NOT_EQUAL_UINT16(S2_BLE_AGE_NEVER, s2_ble_age_ds(100000000000LL, 0, true));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dash_layout);
    RUN_TEST(test_dash_rejects_short_buffer);
    RUN_TEST(test_dash_clears_uds_flag);
    RUN_TEST(test_records_per_fragment);
    RUN_TEST(test_frag_count_boundaries);
    RUN_TEST(test_snapshot_fragments_cover_every_record);
    RUN_TEST(test_snapshot_exact_fill_and_one_over);
    RUN_TEST(test_snapshot_empty_message);
    RUN_TEST(test_snapshot_rejects_short_output);
    RUN_TEST(test_catalog_describes_every_signal);
    RUN_TEST(test_catalog_truncates_a_long_unit);
    RUN_TEST(test_text_skips_never_seen);
    RUN_TEST(test_text_marks_stale);
    RUN_TEST(test_text_includes_the_unit);
    RUN_TEST(test_text_resumes_where_it_stopped);
    RUN_TEST(test_text_makes_progress_in_a_tiny_buffer);
    RUN_TEST(test_commands_recognised);
    RUN_TEST(test_commands_tolerate_terminal_framing);
    RUN_TEST(test_rate_argument_is_range_checked);
    RUN_TEST(test_unknown_commands);
    RUN_TEST(test_help_text_is_present);
    RUN_TEST(test_age_in_deciseconds);
    return UNITY_END();
}
