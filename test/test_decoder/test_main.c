/*
 * Host-side unit tests (pio test -e native) for the pure-C decoding core:
 * CRC-8/SAE-J1850, alive-counter tracking, DBC bit extraction and scaling.
 * Expected values come from worked examples in the DBC comments.
 */
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include "dbc_decode.h"
#include "e2e.h"
#include "s2_dbc_gen.h"

static const s2_signal_def_t *sig(uint16_t id)
{
    const s2_signal_def_t *s = s2_dbc_signal(id);
    TEST_ASSERT_NOT_NULL(s);
    return s;
}

/* ---- CRC-8 / SAE-J1850 ---------------------------------------------------- */

void test_crc8_j1850_check_value(void)
{
    /* Standard check value for CRC-8/SAE-J1850 ("123456789") is 0x4B. */
    const uint8_t s[] = "123456789";
    TEST_ASSERT_EQUAL_HEX8(0x4B, s2_crc8_j1850(s, 9));
}

void test_crc8_j1850_empty_is_xorout(void)
{
    /* init 0xFF ^ xorout 0xFF */
    TEST_ASSERT_EQUAL_HEX8(0x00, s2_crc8_j1850(NULL, 0));
}

void test_e2e_crc_ok_roundtrip(void)
{
    uint8_t frame[8] = { 0x00, 0x00, 0x03, 0xE8, 0x00, 0x00, 0x2A, 0x00 };
    frame[7] = s2_crc8_j1850(frame, 7);
    TEST_ASSERT_TRUE(s2_e2e_crc_ok(frame, 8));
    frame[3] ^= 0x10;   /* a mis-aligned/bit-flipped payload fails the CRC */
    TEST_ASSERT_FALSE(s2_e2e_crc_ok(frame, 8));
    TEST_ASSERT_FALSE(s2_e2e_crc_ok(frame, 7));
}

/* ---- alive counter --------------------------------------------------------- */

void test_alive_tracker_counts_gaps(void)
{
    s2_e2e_tracker_t t = { 0 };
    TEST_ASSERT_EQUAL_UINT32(0, s2_e2e_track_alive(&t, 5));
    TEST_ASSERT_EQUAL_UINT32(0, s2_e2e_track_alive(&t, 6));
    TEST_ASSERT_EQUAL_UINT32(2, s2_e2e_track_alive(&t, 9));    /* 7, 8 missed */
    TEST_ASSERT_EQUAL_UINT32(1, t.gaps);
    TEST_ASSERT_EQUAL_UINT32(2, t.frames_lost);
    for (uint8_t a = 10; a < 64; a++) {
        TEST_ASSERT_EQUAL_UINT32(0, s2_e2e_track_alive(&t, a));
    }
    TEST_ASSERT_EQUAL_UINT32(0, s2_e2e_track_alive(&t, 0));    /* 63 -> 0 wraps cleanly */
    TEST_ASSERT_EQUAL_UINT32(63, s2_e2e_track_alive(&t, 0));   /* repeat = worst case */
    TEST_ASSERT_EQUAL_UINT32(0, s2_e2e_track_alive(&t, 0x41)); /* upper bits ignored: 0x41 & 0x3F = 1 */
}

/* ---- bit extraction ---------------------------------------------------------- */

void test_soc_nibble_shifted_12bit(void)
{
    /* 0x185 D3:D4 upper 12 bits; DBC: 0x0645 -> 100 %, 0x0615 -> 97 % */
    const s2_signal_def_t *s = sig(S2_SIG_SOC_185_state_of_charge);
    uint8_t d[8] = { 0, 0, 0x06, 0x45, 0, 0, 0, 0 };
    TEST_ASSERT_TRUE(s2_signal_fits(s, 8));
    TEST_ASSERT_EQUAL_UINT64(100, s2_extract_raw(d, s));
    d[2] = 0x06; d[3] = 0x15;
    TEST_ASSERT_EQUAL_DOUBLE(97.0, s2_raw_to_physical(s, s2_extract_raw(d, s)));
}

void test_speed_and_rpm_big_endian_16(void)
{
    const s2_signal_def_t *speed = sig(S2_SIG_SPEED_160_vehicle_speed);
    const s2_signal_def_t *rpm = sig(S2_SIG_SPEED_160_motor_rpm);
    /* speed raw 0x03E8 = 1000 -> 100.0 km/h ; rpm raw 0x0BB8 = 3000 -> 3000-1000 = 2000 rpm */
    uint8_t d[8] = { 0x03, 0xE8, 0x0B, 0xB8, 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_DOUBLE(100.0, s2_raw_to_physical(speed, s2_extract_raw(d, speed)));
    TEST_ASSERT_EQUAL_DOUBLE(2000.0, s2_raw_to_physical(rpm, s2_extract_raw(d, rpm)));
}

void test_pack_voltage_and_current_share_bytes(void)
{
    /* 0x181: pack_voltage 7|12 (D1 + hi nibble D2), pack_current 11|16 (lo nibble D2, D3, hi nibble D4) */
    const s2_signal_def_t *v = sig(S2_SIG_BATTERY_STATUS_181_pack_voltage);
    const s2_signal_def_t *i = sig(S2_SIG_BATTERY_STATUS_181_pack_current);
    /* voltage 0x18E = 398 V ; current raw 0x0FA0 = 4000 -> 4000*0.1 - 400 = 0 A */
    uint8_t d[8] = { 0x18, 0xE0, 0xFA, 0x00, 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_UINT64(0x18E, s2_extract_raw(d, v));
    TEST_ASSERT_EQUAL_UINT64(0x0FA0, s2_extract_raw(d, i));
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 0.0, s2_raw_to_physical(i, s2_extract_raw(d, i)));
    /* -161 A -> raw = (-161+400)/0.1 = 2390 = 0x0956 -> D2 lo nibble 0, D3 0x95, D4 hi nibble 6 */
    d[1] = 0xE0; d[2] = 0x95; d[3] = 0x60;
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, -161.0, s2_raw_to_physical(i, s2_extract_raw(d, i)));
}

void test_offset_binary_imu(void)
{
    /* 0x122 vertical axis ~ -7770 counts at rest: raw = 32768 - 7770 = 24998 = 0x61A6 in D5:D6 */
    const s2_signal_def_t *z = sig(S2_SIG_IMU_ACCEL_122_accel_vertical);
    uint8_t d[8] = { 0x80, 0x00, 0x80, 0xAF, 0x61, 0xA6, 0, 0 };
    TEST_ASSERT_EQUAL_DOUBLE(-7770.0, s2_raw_to_physical(z, s2_extract_raw(d, z)));
    const s2_signal_def_t *y = sig(S2_SIG_IMU_ACCEL_122_accel_longitudinal);
    TEST_ASSERT_EQUAL_DOUBLE(175.0, s2_raw_to_physical(y, s2_extract_raw(d, y)));
}

void test_single_bit_and_24bit_fields(void)
{
    /* 0x163 cooling_pump 22|1 -> start bit 22 = byte 2 (D3), bit 6 */
    const s2_signal_def_t *pump = sig(S2_SIG_BATTERY_POWER_163_cooling_pump_163);
    uint8_t d[8] = { 0, 0, 0x40, 0, 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_UINT64(1, s2_extract_raw(d, pump));
    d[2] = 0xBF;
    TEST_ASSERT_EQUAL_UINT64(0, s2_extract_raw(d, pump));

    /* 0x562 odometer 15|24 -> D2:D3:D4 big-endian metres */
    const s2_signal_def_t *odo = sig(S2_SIG_ODOMETER_562_odometer_m);
    uint8_t o[8] = { 0x00, 0x3D, 0x9B, 0x2C, 0, 0, 0, 0 };  /* 0x3D9B2C = 4037420 m */
    TEST_ASSERT_EQUAL_UINT64(4037420, s2_extract_raw(o, odo));
}

void test_vin_ascii_48bit(void)
{
    const s2_signal_def_t *vin = sig(S2_SIG_VIN_PART1_56D_vin_chars_1_6);
    uint8_t d[8] = { '1', 'H', 'D', '1', 'A', 'B', 0, 0 };
    uint64_t raw = s2_extract_raw(d, vin);
    TEST_ASSERT_EQUAL_HEX64(0x314844314142ull, raw);
}

void test_little_endian_and_signed_extraction(void)
{
    /* synthetic Intel signal: start 4, length 12, signed */
    s2_signal_def_t le = { .name = "le", .unit = "", .start_bit = 4, .length = 12,
                           .byte_order = S2_ORDER_LITTLE_ENDIAN, .is_signed = 1,
                           .factor = 0.5f, .offset = 0, .min = 0, .max = 0 };
    /* bits 4..15: byte0 hi nibble = low 4 bits, byte1 = high 8 bits => raw 0xFFF = -1 */
    uint8_t d[8] = { 0xF0, 0xFF, 0, 0, 0, 0, 0, 0 };
    TEST_ASSERT_TRUE(s2_signal_fits(&le, 2));
    TEST_ASSERT_EQUAL_UINT64(0xFFF, s2_extract_raw(d, &le));
    TEST_ASSERT_EQUAL_INT64(-1, s2_raw_to_int(&le, 0xFFF));
    TEST_ASSERT_EQUAL_DOUBLE(-0.5, s2_raw_to_physical(&le, 0xFFF));
    d[0] = 0x50; d[1] = 0x00;   /* raw 0x005 = 5 */
    TEST_ASSERT_EQUAL_DOUBLE(2.5, s2_raw_to_physical(&le, s2_extract_raw(d, &le)));
}

void test_signal_fits_rejects_short_payload(void)
{
    const s2_signal_def_t *odo = sig(S2_SIG_ODOMETER_562_odometer_m);   /* needs bytes 1..3 */
    TEST_ASSERT_TRUE(s2_signal_fits(odo, 4));
    TEST_ASSERT_FALSE(s2_signal_fits(odo, 3));
    const s2_signal_def_t *ctr = sig(S2_SIG_ODOMETER_562_msg_counter_562); /* 55|8 -> byte 6 */
    TEST_ASSERT_TRUE(s2_signal_fits(ctr, 7));
    TEST_ASSERT_FALSE(s2_signal_fits(ctr, 6));
}

void test_every_dbc_signal_fits_its_message(void)
{
    for (size_t m = 0; m < s2_dbc_message_count; m++) {
        const s2_message_def_t *msg = &s2_dbc_messages[m];
        for (unsigned s = 0; s < msg->signal_count; s++) {
            char why[64];
            snprintf(why, sizeof why, "0x%03X %s", msg->id, msg->signals[s].name);
            TEST_ASSERT_TRUE_MESSAGE(s2_signal_fits(&msg->signals[s], msg->dlc), why);
        }
        TEST_ASSERT_EQUAL_PTR(msg, s2_dbc_find_message(msg->id));
        if (m > 0) {
            TEST_ASSERT_TRUE(s2_dbc_messages[m - 1].id < msg->id);   /* sorted for bsearch */
        }
    }
    TEST_ASSERT_NULL(s2_dbc_find_message(0x7FF));
    TEST_ASSERT_EQUAL_UINT(S2_SIG__COUNT, s2_dbc_signal_count);
}

void test_signal_index_mapping(void)
{
    const s2_message_def_t *m = s2_dbc_message_of_signal(S2_SIG_SPEED_160_motor_rpm);
    TEST_ASSERT_NOT_NULL(m);
    TEST_ASSERT_EQUAL_HEX16(0x160, m->id);
    TEST_ASSERT_EQUAL_STRING("motor_rpm", s2_dbc_signal(S2_SIG_SPEED_160_motor_rpm)->name);
    TEST_ASSERT_NULL(s2_dbc_signal(S2_SIG__COUNT));
}

void test_e2e_flags_from_header(void)
{
    TEST_ASSERT_TRUE(s2_dbc_find_message(0x160)->flags & S2_MSG_E2E);
    TEST_ASSERT_TRUE(s2_dbc_find_message(0x562)->flags & S2_MSG_E2E);
    TEST_ASSERT_TRUE(s2_dbc_find_message(0x326)->flags & S2_MSG_NO_E2E);
    TEST_ASSERT_FALSE(s2_dbc_find_message(0x326)->flags & S2_MSG_E2E);
    TEST_ASSERT_TRUE(s2_dbc_find_message(0x138)->flags & S2_MSG_NO_E2E);
}

void test_value_formatting(void)
{
    char buf[32];
    s2_format_value(sig(S2_SIG_SPEED_160_vehicle_speed), 12.34, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("12.3", buf);
    s2_format_value(sig(S2_SIG_SPEED_160_motor_rpm), 2000.0, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("2000", buf);
    s2_format_value(sig(S2_SIG_BATTERY_POWER_163_pack_voltage_163), 398.0625, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("398.0625", buf);
    s2_format_value(sig(S2_SIG_TEMPERATURES_183_ambient_temp), 21.5, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("21.5", buf);
    s2_format_value(sig(S2_SIG_TYRE_PRESSURE_33A_tyre_pressure_front), 250.0, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("250", buf);
    s2_format_value(sig(S2_SIG_MEAN_WHEEL_SPEED_124_wheel_rate_front), 12.195, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("12.1950", buf);
}

void test_packed_12bit_wheel_rates_0x124(void)
{
    /* wheel_rate_front = D3<<4 | D4>>4 (23|12), wheel_rate_rear = (D4&0x0F)<<8 | D5 (27|12) */
    const s2_signal_def_t *fr = sig(S2_SIG_MEAN_WHEEL_SPEED_124_wheel_rate_front);
    const s2_signal_def_t *rr = sig(S2_SIG_MEAN_WHEEL_SPEED_124_wheel_rate_rear);
    uint8_t d[8] = { 0, 0, 0xAB, 0xC1, 0x23, 0, 0, 0 };
    TEST_ASSERT_EQUAL_UINT64(0xABC, s2_extract_raw(d, fr));
    TEST_ASSERT_EQUAL_UINT64(0x123, s2_extract_raw(d, rr));
    /* rear crossing raw 512 carries into the D4 low nibble */
    uint8_t e[8] = { 0, 0, 0x00, 0x02, 0x00, 0, 0, 0 };
    TEST_ASSERT_EQUAL_UINT64(512, s2_extract_raw(e, rr));
}

void test_temperature_scales_0x183(void)
{
    /* D1..D3 degC = raw - 40 ; D4 ambient degC = raw * 0.5 - 40 (DBC: 147 -> 33.5 degC) */
    const s2_signal_def_t *t1 = sig(S2_SIG_TEMPERATURES_183_batt_temp_1);
    const s2_signal_def_t *amb = sig(S2_SIG_TEMPERATURES_183_ambient_temp);
    uint8_t d[8] = { 73, 74, 73, 147, 0, 0, 0, 0 };
    TEST_ASSERT_EQUAL_DOUBLE(33.0, s2_raw_to_physical(t1, s2_extract_raw(d, t1)));
    TEST_ASSERT_EQUAL_DOUBLE(33.5, s2_raw_to_physical(amb, s2_extract_raw(d, amb)));
}

void test_torque_offset_0x161_and_voltage_copy_0x181(void)
{
    /* torque words: zero = 5000 ; 0x1388 = 5000 -> 0 ; 5989 -> +989 (DBC ride1 WOT peak) */
    const s2_signal_def_t *req = sig(S2_SIG_MOTOR_POWER_161_torque_request);
    const s2_signal_def_t *thr = sig(S2_SIG_MOTOR_POWER_161_throttle_request);
    uint8_t d[8] = { 0x17, 0x65, 0x13, 0x88, 0xC8, 0x04, 0, 0 };
    TEST_ASSERT_EQUAL_DOUBLE(989.0, s2_raw_to_physical(req, s2_extract_raw(d, req)));
    TEST_ASSERT_EQUAL_DOUBLE(100.0, s2_raw_to_physical(thr, s2_extract_raw(d, thr)));
    /* 0x181 D5 legacy copy: volts = D5 + 256 (DBC: 135 -> 391 V) */
    const s2_signal_def_t *vc = sig(S2_SIG_BATTERY_STATUS_181_pack_voltage_d5copy);
    uint8_t e[8] = { 0, 0, 0, 0, 135, 0, 0, 0 };
    TEST_ASSERT_EQUAL_DOUBLE(391.0, s2_raw_to_physical(vc, s2_extract_raw(e, vc)));
}

void test_header_only_ids_are_unknown(void)
{
    TEST_ASSERT_NULL(s2_dbc_find_message(0x180));
    TEST_ASSERT_NULL(s2_dbc_find_message(0x1C7));
    TEST_ASSERT_NULL(s2_dbc_find_message(0x1C9));
    TEST_ASSERT_NULL(s2_dbc_find_message(0x35A));
}

void test_negative_value_formatting(void)
{
    char buf[32];
    s2_format_value(sig(S2_SIG_BATTERY_STATUS_181_pack_current), -161.0, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("-161.0", buf);
    s2_format_value(sig(S2_SIG_SPEED_160_motor_rpm), -88.0, buf, sizeof buf);
    TEST_ASSERT_EQUAL_STRING("-88", buf);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_crc8_j1850_check_value);
    RUN_TEST(test_crc8_j1850_empty_is_xorout);
    RUN_TEST(test_e2e_crc_ok_roundtrip);
    RUN_TEST(test_alive_tracker_counts_gaps);
    RUN_TEST(test_soc_nibble_shifted_12bit);
    RUN_TEST(test_speed_and_rpm_big_endian_16);
    RUN_TEST(test_pack_voltage_and_current_share_bytes);
    RUN_TEST(test_offset_binary_imu);
    RUN_TEST(test_single_bit_and_24bit_fields);
    RUN_TEST(test_vin_ascii_48bit);
    RUN_TEST(test_little_endian_and_signed_extraction);
    RUN_TEST(test_signal_fits_rejects_short_payload);
    RUN_TEST(test_every_dbc_signal_fits_its_message);
    RUN_TEST(test_signal_index_mapping);
    RUN_TEST(test_e2e_flags_from_header);
    RUN_TEST(test_value_formatting);
    RUN_TEST(test_packed_12bit_wheel_rates_0x124);
    RUN_TEST(test_temperature_scales_0x183);
    RUN_TEST(test_torque_offset_0x161_and_voltage_copy_0x181);
    RUN_TEST(test_header_only_ids_are_unknown);
    RUN_TEST(test_negative_value_formatting);
    return UNITY_END();
}
