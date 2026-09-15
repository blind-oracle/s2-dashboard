#include "ble_telemetry.h"

#include "sdkconfig.h"

#if !CONFIG_S2_BLE_ENABLE

/* BLE disabled at build time: keep the API linkable and inert. */
esp_err_t ble_telemetry_start(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ble_telemetry_connected(void) { return false; }

#else

#include <stdio.h>
#include <string.h>

#include "ble_proto.h"
#include "can_bus.h"
#include "decoder.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "log_writer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "ride_limits.h"
#include "s2_dbc_gen.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "vehicle_state.h"

static const char *TAG = "ble";

/* Boolean Kconfig symbols are undefined (not 0) when disabled. */
#ifdef CONFIG_S2_BLE_NUS_ENABLE
#define OPT_NUS 1
#else
#define OPT_NUS 0
#endif
#ifdef CONFIG_S2_UDS_ENABLE
#define OPT_UDS 1
#else
#define OPT_UDS 0
#endif

/*
 * Kconfig drops an int symbol entirely when its "depends on" is unmet, so the
 * text period only exists when the UART service is compiled in.
 */
#ifdef CONFIG_S2_BLE_TEXT_PERIOD_MS
#define OPT_TEXT_MS CONFIG_S2_BLE_TEXT_PERIOD_MS
#else
#define OPT_TEXT_MS 1000
#endif

/* The publisher polls this often; it bounds the jitter on every period below. */
#define TICK_MS 10

/* ATT overhead on a notification: opcode plus handle. */
#define ATT_NOTIFY_OVERHEAD 3

/* Cap one tick's work so a long text dump cannot monopolise the radio. */
#define MAX_CHUNKS_PER_TICK 4

/* ------------------------------------------------------------------ UUIDs --- */

/*
 * Nordic UART Service. Not a Bluetooth SIG service, but the de-facto contract
 * that nRF Toolbox, nRF Connect and Serial Bluetooth Terminal all understand.
 * BLE_UUID128_INIT takes the bytes reversed from the printed form.
 */
#define NUS_UUID(b) BLE_UUID128_INIT(0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
                                     0x93, 0xf3, 0xa3, 0xb5, (b), 0x00, 0x40, 0x6e)

#if OPT_NUS
static const ble_uuid128_t nus_svc_uuid = NUS_UUID(0x01);   /* 6E400001-... */
static const ble_uuid128_t nus_rx_uuid = NUS_UUID(0x02);    /* phone -> device */
static const ble_uuid128_t nus_tx_uuid = NUS_UUID(0x03);    /* device -> phone */
#endif

/* Custom telemetry service, 5332da0N-1b2c-4f3e-8a9d-1e0f2a3b4c5d. */
#define S2_UUID(n) BLE_UUID128_INIT(0x5d, 0x4c, 0x3b, 0x2a, 0x0f, 0x1e, 0x9d, 0x8a, \
                                    0x3e, 0x4f, 0x2c, 0x1b, (n), 0xda, 0x32, 0x53)

static const ble_uuid128_t s2_svc_uuid = S2_UUID(0x00);
static const ble_uuid128_t s2_dash_uuid = S2_UUID(0x01);
static const ble_uuid128_t s2_stream_uuid = S2_UUID(0x02);
static const ble_uuid128_t s2_cmd_uuid = S2_UUID(0x03);
static const ble_uuid128_t s2_info_uuid = S2_UUID(0x04);

/* Characteristic User Description: nRF Connect shows it instead of the UUID. */
static const ble_uuid16_t dsc_cud_uuid = BLE_UUID16_INIT(0x2901);

/* ------------------------------------------------------------------ state --- */

static uint8_t s_own_addr_type;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;

static uint16_t s_dash_handle;
static uint16_t s_stream_handle;
static uint16_t s_nus_tx_handle;

static volatile bool s_dash_sub;
static volatile bool s_stream_sub;
static volatile bool s_nus_sub;

/* Pending one-shot replies, set from the host task and drained by the publisher. */
#define REQ_HELP    0x01u
#define REQ_INFO    0x02u
#define REQ_ALL     0x04u
#define REQ_CATALOG 0x08u
#define REQ_BAD_ARG 0x10u
#define REQ_UNKNOWN 0x20u
static uint32_t s_req;

static volatile int s_dash_hz = CONFIG_S2_BLE_DASH_HZ;
static volatile bool s_stream_text = true;

static uint32_t s_tx_dropped;       /* notifications the stack or the pool refused */
static uint16_t s_snap_seq;
static uint16_t s_cat_seq;

/* Big enough for the largest notification we ever build. */
#define TX_BUF_MAX 517
static uint8_t s_tx[TX_BUF_MAX];
static char s_text[TX_BUF_MAX];

/* Owned by the publisher task only. */
static s2_ble_record_t s_recs[S2_SIG__COUNT];

/*
 * In-progress multi-notification dumps. Each one pins the payload size it started
 * with: an MTU change part-way through a message would otherwise move the
 * fragment boundaries and the client would reassemble garbage.
 */
static bool s_all_active;
static unsigned s_all_next;

static bool s_snap_active;
static unsigned s_snap_frag;
static unsigned s_snap_total;
static size_t s_snap_payload;

static bool s_cat_active;
static unsigned s_cat_frag;
static unsigned s_cat_total;
static size_t s_cat_payload;

/*
 * The periodic text digest: the handful of values worth watching on a phone.
 * The full set goes out on request ("all") and continuously as binary records,
 * because 113 lines a second would swamp a terminal app.
 */
static const uint16_t s_digest[] = {
    S2_SIG_SPEED_160_vehicle_speed,
    S2_SIG_SOC_185_state_of_charge,
    S2_SIG_BATTERY_STATUS_181_pack_voltage,
    S2_SIG_BATTERY_STATUS_181_pack_current,
    S2_SIG_MOTOR_POWER_161_torque_delivered,
    S2_SIG_HV_ENERGY_186_trip_energy_consumed_wh,
    S2_SIG_HV_ENERGY_186_pack_energy_remaining_wh,
    S2_SIG_TEMPERATURES_183_batt_temp_1,
    S2_SIG_AUX_VOLTAGE_332_aux_battery_voltage,
    S2_SIG_ODOMETER_562_odometer_m,
};
#define DIGEST_COUNT (sizeof(s_digest) / sizeof(s_digest[0]))

bool ble_telemetry_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

static void req_set(uint32_t bits)
{
    __atomic_fetch_or(&s_req, bits, __ATOMIC_RELAXED);
}

static uint32_t req_take(void)
{
    return __atomic_exchange_n(&s_req, 0u, __ATOMIC_RELAXED);
}

/* --------------------------------------------------------- reading state --- */

/*
 * The five ride values, using the same plausibility and freshness rules as the
 * screen (ride_limits.h) so the two displays can never disagree.
 *
 * The lock is held only to copy scalars out; every derivation happens after it
 * is released, which is the pattern display.c:snapshot() established.
 */
static void read_dash(s2_ble_dash_t *d, int64_t now)
{
    memset(d, 0, sizeof(*d));
    d->uds_enabled = OPT_UDS;

    double volts = 0, amps = 0, torque_counts = 0, used_wh = 0, soh = 0;
    bool v_valid = false, i_valid = false, t_valid = false, e_valid = false, soh_valid = false;
    int64_t v_ts = 0, i_ts = 0, t_ts = 0, e_ts = 0, soh_ts = 0;

    vs_lock();
    const vs_signal_t *sv = vs_signal(S2_SIG_BATTERY_STATUS_181_pack_voltage);
    const vs_signal_t *si = vs_signal(S2_SIG_BATTERY_STATUS_181_pack_current);
    const vs_signal_t *st = vs_signal(S2_SIG_MOTOR_POWER_161_torque_delivered);
    const vs_signal_t *se = vs_signal(S2_SIG_HV_ENERGY_186_trip_energy_consumed_wh);
    if (sv && sv->valid) {
        volts = sv->value;
        v_ts = sv->ts_us;
        v_valid = true;
    }
    if (si && si->valid) {
        amps = si->value;
        i_ts = si->ts_us;
        i_valid = true;
    }
    if (st && st->valid) {
        torque_counts = st->value;
        t_ts = st->ts_us;
        t_valid = true;
    }
    if (se && se->valid) {
        used_wh = se->value;            /* the bike's own key-on trip meter */
        e_ts = se->ts_us;
        e_valid = true;
    }
    const vs_uds_t *u = vs_uds();
    soh_valid = u->soh_valid;
    soh = u->soh_pct;
    soh_ts = u->soh_ts_us;
    vs_unlock();

    /* One rule for the pair, so voltage and power never disagree about validity. */
    bool pair_ok = v_valid && i_valid && ride_pack_sample_plausible(volts, amps);
    int64_t power_ts = v_ts < i_ts ? v_ts : i_ts;

    d->power_fs = (uint8_t)ride_freshness(pair_ok, power_ts, now, RIDE_STALE_FAST_US);
    d->power_kw = pair_ok ? (float)ride_power_kw(volts, amps) : 0.0f;

    bool t_ok = t_valid && ride_torque_plausible(torque_counts);
    d->torque_fs = (uint8_t)ride_freshness(t_ok, t_ts, now, RIDE_STALE_FAST_US);
    d->torque_nm = t_ok ? (float)ride_torque_nm(torque_counts) : 0.0f;

    d->volts_fs = (uint8_t)ride_freshness(pair_ok, v_ts, now, RIDE_STALE_FAST_US);
    d->volts = (float)volts;

    d->energy_fs = (uint8_t)ride_freshness(e_valid, e_ts, now, RIDE_STALE_ENERGY_US);
    d->session_kwh = (float)(used_wh / 1000.0);

    d->soh_fs = (uint8_t)ride_freshness(soh_valid, soh_ts, now, RIDE_STALE_SOH_US);
    d->soh_pct = (float)soh;
}

/*
 * Every database signal, as records. The age arithmetic is done inside the lock
 * on purpose: it is one subtract and one divide per signal, far cheaper than the
 * ~900 bytes a parallel array of timestamps would cost to defer it, and nothing
 * here can block the way summary.c's logging under the lock can.
 */
static void read_records(s2_ble_record_t *recs, int64_t now)
{
    vs_lock();
    for (uint16_t i = 0; i < (uint16_t)S2_SIG__COUNT; i++) {
        const vs_signal_t *s = vs_signal(i);
        recs[i].index = i;
        if (s && s->valid) {
            recs[i].age_ds = s2_ble_age_ds(now, s->ts_us, true);
            recs[i].value = (float)s->value;
        } else {
            recs[i].age_ds = S2_BLE_AGE_NEVER;
            recs[i].value = 0.0f;
        }
    }
    vs_unlock();
}

static void read_vin(char *out, size_t outlen)
{
    vs_lock();
    const vs_derived_t *d = vs_derived();
    snprintf(out, outlen, "%s", d->vin[0] ? d->vin : "unknown");
    vs_unlock();
}

/* ------------------------------------------------------------ notifying --- */

static size_t mtu_payload(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return 23 - ATT_NOTIFY_OVERHEAD;
    }
    uint16_t mtu = ble_att_mtu(s_conn);
    if (mtu < 23) {
        mtu = 23;               /* the spec floor, before any exchange completes */
    }
    size_t payload = (size_t)mtu - ATT_NOTIFY_OVERHEAD;
    if (payload > TX_BUF_MAX) {
        payload = TX_BUF_MAX;
    }
    return payload;
}

static bool notify_flat(uint16_t handle, const void *data, size_t len)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE || len == 0) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)len);
    if (!om) {
        s_tx_dropped++;         /* msys pool exhausted: drop, never block the task */
        return false;
    }
    /* Consumes the mbuf whether or not it succeeds. */
    if (ble_gatts_notify_custom(s_conn, handle, om) != 0) {
        s_tx_dropped++;
        return false;
    }
    return true;
}

static bool notify_text(const char *s, size_t len)
{
    if (!OPT_NUS || !s_nus_sub || len == 0) {
        return false;
    }
    /* Split at the MTU so a long reply arrives as several notifications. */
    size_t chunk = mtu_payload();
    size_t sent = 0;
    while (sent < len) {
        size_t n = len - sent;
        if (n > chunk) {
            n = chunk;
        }
        if (!notify_flat(s_nus_tx_handle, s + sent, n)) {
            return false;
        }
        sent += n;
    }
    return true;
}

static void notify_str(const char *s)
{
    notify_text(s, strlen(s));
}

/* ------------------------------------------------------- GATT callbacks --- */

static int dsc_label_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    const char *label = (const char *)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_DSC || !label) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    return os_mbuf_append(ctxt->om, label, strlen(label)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/* One labelled 0x2901 descriptor, so a generic browser shows a name. */
#define LABEL_DSC(sym, text)                                             \
    static struct ble_gatt_dsc_def sym[] = {                             \
        {                                                                \
            .uuid = &dsc_cud_uuid.u,                                     \
            .att_flags = BLE_ATT_F_READ,                                 \
            .access_cb = dsc_label_cb,                                   \
            .arg = (void *)(text),                                       \
        },                                                               \
        { 0 },                                                           \
    }

LABEL_DSC(dsc_dash, "Ride dashboard (24-byte frame)");
LABEL_DSC(dsc_stream, "Telemetry stream (fragmented records)");
LABEL_DSC(dsc_cmd, "Command (ASCII, read-only device control)");
LABEL_DSC(dsc_info, "Device info (text)");
#if OPT_NUS
LABEL_DSC(dsc_nus_rx, "UART RX (commands to the device)");
LABEL_DSC(dsc_nus_tx, "UART TX (key=value lines)");
#endif

static int dash_access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    /* Built into local storage: this runs on the host task, not the publisher. */
    s2_ble_dash_t d;
    uint8_t buf[S2_BLE_DASH_SIZE];
    read_dash(&d, esp_timer_get_time());
    size_t len = s2_ble_pack_dash(buf, sizeof(buf), &d);
    return os_mbuf_append(ctxt->om, buf, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

static int stream_access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)ctxt;
    (void)arg;
    /* Notify-only: a snapshot is far larger than one attribute value. */
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static int info_access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) {
        return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    decoder_stats_t dec;
    can_bus_stats_t bus;
    decoder_get_stats(&dec);
    can_bus_get_stats(&bus);
    char vin[18];
    read_vin(vin, sizeof(vin));

    char buf[416];
    int n = snprintf(buf, sizeof(buf),
                     "fw=%s\nbuilt=%s %s\nproto=%u\ndb=%s\nsignals=%u\nvin=%s\n"
                     "uptime_s=%lld\nframes=%lu\ncrc_fail=%lu\nbus=%s\nble_tx_drop=%lu\n",
                     app->version, app->date, app->time, (unsigned)S2_BLE_PROTO_VERSION, S2_DBC_VERSION,
                     (unsigned)S2_SIG__COUNT, vin, (long long)(esp_timer_get_time() / 1000000),
                     (unsigned long)dec.frames, (unsigned long)dec.crc_fail, can_bus_state_name(bus.state),
                     (unsigned long)s_tx_dropped);
    if (n < 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    size_t len = (size_t)n < sizeof(buf) ? (size_t)n : sizeof(buf) - 1;
    return os_mbuf_append(ctxt->om, buf, len) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

/*
 * The command channel. Every verb only changes what this device reports; none
 * can leave listen-only mode, transmit a frame or start a UDS request. Anyone in
 * radio range can connect, so that boundary is deliberate.
 */
static int cmd_access_cb(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn;
    (void)attr;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_READ_NOT_PERMITTED;
    }
    char in[64];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, in, sizeof(in) - 1, &len) != 0) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    in[len] = '\0';

    s2_ble_cmd_result_t r = s2_ble_parse_cmd(in, len);
    switch (r.cmd) {
    case S2_BLE_CMD_EMPTY:
        break;
    case S2_BLE_CMD_HELP:
        req_set(REQ_HELP);
        break;
    case S2_BLE_CMD_INFO:
        req_set(REQ_INFO);
        break;
    case S2_BLE_CMD_ALL:
        req_set(REQ_ALL);
        break;
    case S2_BLE_CMD_CATALOG:
        req_set(REQ_CATALOG);
        break;
    case S2_BLE_CMD_STREAM_ON:
        s_stream_text = true;
        break;
    case S2_BLE_CMD_STREAM_OFF:
        s_stream_text = false;
        break;
    case S2_BLE_CMD_RATE:
        s_dash_hz = r.arg;
        break;
    case S2_BLE_CMD_BAD_ARG:
        req_set(REQ_BAD_ARG);
        break;
    case S2_BLE_CMD_UNKNOWN:
    default:
        req_set(REQ_UNKNOWN);
        break;
    }
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s2_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &s2_dash_uuid.u,
                .access_cb = dash_access_cb,
                .descriptors = dsc_dash,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_dash_handle,
            },
            {
                .uuid = &s2_stream_uuid.u,
                .access_cb = stream_access_cb,
                .descriptors = dsc_stream,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_stream_handle,
            },
            {
                .uuid = &s2_cmd_uuid.u,
                .access_cb = cmd_access_cb,
                .descriptors = dsc_cmd,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s2_info_uuid.u,
                .access_cb = info_access_cb,
                .descriptors = dsc_info,
                .flags = BLE_GATT_CHR_F_READ,
            },
            { 0 },
        },
    },
#if OPT_NUS
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]){
            {
                .uuid = &nus_rx_uuid.u,
                .access_cb = cmd_access_cb,
                .descriptors = dsc_nus_rx,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &nus_tx_uuid.u,
                .access_cb = stream_access_cb,
                .descriptors = dsc_nus_tx,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_nus_tx_handle,
            },
            { 0 },
        },
    },
#endif
    { 0 },
};

/* --------------------------------------------------------------- GAP --- */

static void advertise(void);

static void clear_subscriptions(void)
{
    s_dash_sub = false;
    s_stream_sub = false;
    s_nus_sub = false;
    s_all_active = false;
    s_snap_active = false;
    s_cat_active = false;
    s_stream_text = true;
    s_dash_hz = CONFIG_S2_BLE_DASH_HZ;
}

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            log_tline("ble: client connected");
        } else {
            log_tline("ble: connect failed (status %d), advertising again", event->connect.status);
            advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        log_tline("ble: client disconnected (reason 0x%02x)", event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        clear_subscriptions();
        advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_dash_handle) {
            s_dash_sub = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_stream_handle) {
            s_stream_sub = event->subscribe.cur_notify;
        } else if (event->subscribe.attr_handle == s_nus_tx_handle) {
            s_nus_sub = event->subscribe.cur_notify;
            if (s_nus_sub) {
                req_set(REQ_HELP);      /* so a terminal app shows something at once */
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU now %d", event->mtu.value);
        return 0;

    default:
        return 0;
    }
}

static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_hs_adv_fields rsp;
    struct ble_gap_adv_params params;
    const char *name = ble_svc_gap_device_name();

    /*
     * The name goes in the advertisement so a phone's scan list shows it; the
     * 128-bit service UUID goes in the scan response, because one alone would
     * leave no room for a name in the 31-byte advertisement.
     */
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)name;
    fields.name_len = (uint8_t)strlen(name);
    fields.name_is_complete = 1;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields failed: %d", rc);
        return;
    }

    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = (ble_uuid128_t *)&s2_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = !OPT_NUS;    /* the UART service is present but unlisted */
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_rsp_set_fields failed: %d", rc);
        return;
    }

    memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "adv_start failed: %d", rc);
    }
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    if (ble_hs_id_infer_auto(0, &s_own_addr_type) != 0) {
        ESP_LOGE(TAG, "address type inference failed");
        return;
    }
    log_tline("ble: advertising as \"%s\"", CONFIG_S2_BLE_DEVICE_NAME);
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset, reason %d", reason);
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    clear_subscriptions();
}

/* --------------------------------------------------------- publishing --- */

static void publish_dash(int64_t now)
{
    s2_ble_dash_t d;
    read_dash(&d, now);
    size_t len = s2_ble_pack_dash(s_tx, sizeof(s_tx), &d);
    if (len) {
        notify_flat(s_dash_handle, s_tx, len);
    }
}

/*
 * Start a snapshot. It is then sent a few fragments per tick rather than all at
 * once: at the negotiated 247-byte MTU that is four fragments and finishes in one
 * tick, but a client that never enlarges the MTU from the 23-byte default would
 * otherwise get a burst of 113 notifications every period.
 */
static void snapshot_begin(int64_t now)
{
    read_records(s_recs, now);
    s_snap_payload = mtu_payload();
    s_snap_total = s2_ble_frag_count((unsigned)S2_SIG__COUNT, S2_BLE_SNAP_RECORD, s_snap_payload);
    if (s_snap_total == 0) {
        return;                 /* the MTU cannot carry even one record */
    }
    s_snap_seq++;
    s_snap_frag = 0;
    s_snap_active = true;
}

/* One fragment of the snapshot. Returns false when the message is finished. */
static bool publish_snapshot_chunk(void)
{
    size_t len = s2_ble_pack_snapshot(s_tx, sizeof(s_tx), s_snap_payload, s_snap_seq, s_snap_frag,
                                      s_recs, (unsigned)S2_SIG__COUNT);
    if (!len || !notify_flat(s_stream_handle, s_tx, len)) {
        return false;           /* the client discards the partial sequence */
    }
    s_snap_frag++;
    return s_snap_frag < s_snap_total;
}

/* The periodic digest: the computed values first, then the curated signals. */
static void publish_digest(int64_t now)
{
    s2_ble_dash_t d;
    read_dash(&d, now);

    size_t pos = 0;
    int n = snprintf(s_text, sizeof(s_text), "t=%lld\n", (long long)(now / 1000000));
    if (n > 0) {
        pos = (size_t)n;
    }
    if (d.power_fs != S2_BLE_FS_MISSING) {
        n = snprintf(s_text + pos, sizeof(s_text) - pos, "power_out=%.2f kW%s\n", (double)d.power_kw,
                     d.power_fs == S2_BLE_FS_STALE ? "*" : "");
        if (n > 0) {
            pos += (size_t)n;
        }
    }
    if (d.torque_fs != S2_BLE_FS_MISSING) {
        n = snprintf(s_text + pos, sizeof(s_text) - pos, "torque_est=%.1f Nm%s\n", (double)d.torque_nm,
                     d.torque_fs == S2_BLE_FS_STALE ? "*" : "");
        if (n > 0) {
            pos += (size_t)n;
        }
    }
    if (d.energy_fs != S2_BLE_FS_MISSING) {
        n = snprintf(s_text + pos, sizeof(s_text) - pos, "session=%.3f kWh%s\n", (double)d.session_kwh,
                     d.energy_fs == S2_BLE_FS_STALE ? "*" : "");
        if (n > 0) {
            pos += (size_t)n;
        }
    }
    if (d.soh_fs != S2_BLE_FS_MISSING) {
        n = snprintf(s_text + pos, sizeof(s_text) - pos, "soh=%.1f %%%s\n", (double)d.soh_pct,
                     d.soh_fs == S2_BLE_FS_STALE ? "*" : "");
        if (n > 0) {
            pos += (size_t)n;
        }
    }

    /* Then the curated signals, rendered by the same formatter the summary uses. */
    s2_ble_record_t digest[DIGEST_COUNT];
    int64_t stamp = now;
    vs_lock();
    for (unsigned i = 0; i < DIGEST_COUNT; i++) {
        const vs_signal_t *s = vs_signal(s_digest[i]);
        digest[i].index = s_digest[i];
        if (s && s->valid) {
            digest[i].age_ds = s2_ble_age_ds(stamp, s->ts_us, true);
            digest[i].value = (float)s->value;
        } else {
            digest[i].age_ds = S2_BLE_AGE_NEVER;
            digest[i].value = 0.0f;
        }
    }
    vs_unlock();

    size_t body = 0;
    s2_ble_render_text(s_text + pos, sizeof(s_text) - pos, &body, digest, DIGEST_COUNT, 0);
    pos += body;

    notify_text(s_text, pos);
}

/* One chunk of the "all" dump. Returns false when the dump is finished. */
static bool publish_all_chunk(void)
{
    size_t len = 0;
    unsigned next = s2_ble_render_text(s_text, sizeof(s_text), &len, s_recs, (unsigned)S2_SIG__COUNT,
                                       s_all_next);
    if (len) {
        notify_text(s_text, len);
    }
    s_all_next = next;
    if (next >= (unsigned)S2_SIG__COUNT) {
        notify_str("--- end ---\n");
        return false;
    }
    return true;
}

/* One fragment of the catalog. Returns false when the catalog is finished. */
static bool publish_catalog_chunk(void)
{
    size_t len = s2_ble_pack_catalog(s_tx, sizeof(s_tx), s_cat_payload, s_cat_seq, s_cat_frag);
    if (!len || !notify_flat(s_stream_handle, s_tx, len)) {
        return false;
    }
    s_cat_frag++;
    return s_cat_frag < s_cat_total;
}

static void handle_requests(int64_t now)
{
    uint32_t req = req_take();
    if (req & REQ_HELP) {
        notify_str(s2_ble_help_text());
    }
    if (req & REQ_BAD_ARG) {
        notify_str("error: bad argument\n");
    }
    if (req & REQ_UNKNOWN) {
        notify_str("error: unknown command, try help\n");
    }
    if (req & REQ_INFO) {
        const esp_app_desc_t *app = esp_app_get_description();
        char vin[18];
        read_vin(vin, sizeof(vin));
        snprintf(s_text, sizeof(s_text), "fw=%s\nproto=%u\nsignals=%u\nvin=%s\nuptime_s=%lld\n",
                 app->version, (unsigned)S2_BLE_PROTO_VERSION, (unsigned)S2_SIG__COUNT, vin,
                 (long long)(now / 1000000));
        notify_str(s_text);
    }
    if (req & REQ_ALL) {
        if (s_snap_active) {
            /*
             * A snapshot in flight owns s_recs, and every fragment of one
             * sequence number has to describe the same instant. Retry next tick;
             * a snapshot always completes.
             */
            req_set(REQ_ALL);
        } else {
            read_records(s_recs, now);
            s_all_next = 0;
            s_all_active = true;
        }
    }
    if (req & REQ_CATALOG) {
        s_cat_payload = mtu_payload();
        s_cat_total = s2_ble_frag_count((unsigned)S2_SIG__COUNT, S2_BLE_CAT_RECORD, s_cat_payload);
        if (s_cat_total) {
            s_cat_seq++;
            s_cat_frag = 0;
            s_cat_active = true;
        }
    }
}

static void pub_task(void *arg)
{
    (void)arg;
    int64_t next_dash = 0, next_snap = 0, next_text = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(TICK_MS));

        if (s_conn == BLE_HS_CONN_HANDLE_NONE) {
            next_dash = next_snap = next_text = 0;
            continue;           /* nothing to do while nobody is listening */
        }

        int64_t now = esp_timer_get_time();
        handle_requests(now);

        if (s_dash_sub && now >= next_dash) {
            int hz = s_dash_hz;
            if (hz < S2_BLE_RATE_MIN) {
                hz = S2_BLE_RATE_MIN;
            }
            next_dash = now + 1000000LL / hz;
            publish_dash(now);
        }

        /* Not while a text dump is walking s_recs, for the same reason. */
        if (s_stream_sub && !s_snap_active && !s_cat_active && !s_all_active && now >= next_snap) {
            next_snap = now + (int64_t)CONFIG_S2_BLE_SNAPSHOT_MS * 1000;
            snapshot_begin(now);
        }

        if (OPT_NUS && s_nus_sub && s_stream_text && !s_all_active && now >= next_text) {
            next_text = now + (int64_t)OPT_TEXT_MS * 1000;
            publish_digest(now);
        }

        /* Bounded, so no multi-fragment dump can starve the periodic frames. */
        for (unsigned i = 0; i < MAX_CHUNKS_PER_TICK && s_snap_active; i++) {
            s_snap_active = publish_snapshot_chunk();
        }
        for (unsigned i = 0; i < MAX_CHUNKS_PER_TICK && s_all_active; i++) {
            s_all_active = publish_all_chunk();
        }
        for (unsigned i = 0; i < MAX_CHUNKS_PER_TICK && s_cat_active; i++) {
            s_cat_active = publish_catalog_chunk();
        }
    }
}

/* --------------------------------------------------------------- start --- */

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();          /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

esp_err_t ble_telemetry_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_count_cfg failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_gatts_add_svcs(gatt_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "gatts_add_svcs failed: %d", rc);
        return ESP_FAIL;
    }
    rc = ble_svc_gap_device_name_set(CONFIG_S2_BLE_DEVICE_NAME);
    if (rc != 0) {
        ESP_LOGE(TAG, "device_name_set failed: %d", rc);
        return ESP_FAIL;
    }

    /* Core 0: core 1 is reserved for the CAN decoder task. */
    if (xTaskCreatePinnedToCore(pub_task, "ble_pub", 4096, NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "publisher task not created");
        return ESP_ERR_NO_MEM;
    }

    nimble_port_freertos_init(host_task);
    return ESP_OK;
}

#endif /* CONFIG_S2_BLE_ENABLE */
