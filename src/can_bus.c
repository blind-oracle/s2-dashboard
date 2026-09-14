#include "can_bus.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/queue.h"
#include "sdkconfig.h"

static const char *TAG = "can";

static twai_node_handle_t s_node;
static QueueHandle_t s_rx_queue;
static volatile can_bus_stats_t s_stats;
static volatile bool s_bus_off;
static volatile bool s_recovering;

/* ---- ISR callbacks (no blocking calls, no logging) ------------------------ */

static bool IRAM_ATTR on_rx_done(twai_node_handle_t node, const twai_rx_done_event_data_t *edata, void *user_ctx)
{
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    uint8_t buf[TWAI_FRAME_MAX_LEN];
    twai_frame_t rx = {
        .buffer = buf,
        .buffer_len = sizeof(buf),
    };
    if (twai_node_receive_from_isr(node, &rx) != ESP_OK) {
        s_stats.rx_isr_errors++;
        return false;
    }
    can_frame_t f;
    f.timestamp_us = esp_timer_get_time();
    f.id = rx.header.id;
    f.extended = rx.header.ide;
    f.rtr = rx.header.rtr;
    uint16_t len = twaifd_dlc2len(rx.header.dlc);
    /* Remote frames carry no data (the HAL leaves the buffer untouched); no_receive_rtr is not honoured on S3. */
    if (f.rtr) {
        len = 0;
    }
    f.dlc = (uint8_t)(len > TWAI_FRAME_MAX_LEN ? TWAI_FRAME_MAX_LEN : len);
    memcpy(f.data, buf, f.dlc);
    if (f.dlc < TWAI_FRAME_MAX_LEN) {
        memset(f.data + f.dlc, 0, TWAI_FRAME_MAX_LEN - f.dlc);
    }
    if (xQueueSendFromISR(s_rx_queue, &f, &woken) == pdTRUE) {
        s_stats.rx_frames++;
    } else {
        s_stats.rx_queue_full++;
    }
    return woken == pdTRUE;
}

static bool IRAM_ATTR on_state_change(twai_node_handle_t node, const twai_state_change_event_data_t *edata, void *user_ctx)
{
    (void)node;
    (void)user_ctx;
    s_stats.state_changes++;
    s_stats.state = edata->new_sta;
    if (edata->new_sta == TWAI_ERROR_BUS_OFF) {
        s_stats.bus_off_events++;
        s_bus_off = true;
    } else if (edata->old_sta == TWAI_ERROR_BUS_OFF) {
        /* The driver models "disabled" as BUS_OFF too, so only count real recoveries. */
        if (s_bus_off) {
            s_stats.recoveries++;
        }
        s_bus_off = false;
        s_recovering = false;
    }
    return false;
}

static bool IRAM_ATTR on_error(twai_node_handle_t node, const twai_error_event_data_t *edata, void *user_ctx)
{
    (void)node;
    (void)user_ctx;
    s_stats.bus_errors++;
    if (edata->err_flags.arb_lost) {
        s_stats.err_arb_lost++;
    }
    if (edata->err_flags.bit_err) {
        s_stats.err_bit++;
    }
    if (edata->err_flags.form_err) {
        s_stats.err_form++;
    }
    if (edata->err_flags.stuff_err) {
        s_stats.err_stuff++;
    }
    if (edata->err_flags.ack_err) {
        s_stats.err_ack++;
    }
    return false;
}

static bool IRAM_ATTR on_tx_done(twai_node_handle_t node, const twai_tx_done_event_data_t *edata, void *user_ctx)
{
    (void)node;
    (void)user_ctx;
    if (edata->is_tx_success) {
        s_stats.tx_frames++;
    } else {
        s_stats.tx_failed++;
    }
    return false;
}

/* ---- API --------------------------------------------------------------------- */

esp_err_t can_bus_start(void)
{
    if (s_node) {
        return ESP_ERR_INVALID_STATE;
    }
    s_rx_queue = xQueueCreate(CONFIG_S2_CAN_RX_QUEUE_LEN, sizeof(can_frame_t));
    ESP_RETURN_ON_FALSE(s_rx_queue, ESP_ERR_NO_MEM, TAG, "rx queue");

    twai_onchip_node_config_t cfg = {
        .io_cfg = {
            .tx = CONFIG_S2_CAN_TX_GPIO,
            .rx = CONFIG_S2_CAN_RX_GPIO,
            .quanta_clk_out = -1,
            .bus_off_indicator = -1,
        },
        .bit_timing = {
            .bitrate = CONFIG_S2_CAN_BITRATE,
            .sp_permill = 800,   /* 80 % sample point, the CiA/vehicle-bus norm for 500 kbit/s */
        },
        /*
         * On the SJA1000-class controller of the ESP32-S3 this is binary: -1 = normal
         * CAN retransmission (arbitration loss, errors), anything else = single-shot.
         * Normal retransmission is what a UDS tester needs on a busy bus.
         */
        .fail_retry_cnt = -1,
        .tx_queue_depth = CONFIG_S2_CAN_TX_QUEUE_DEPTH,
        .intr_priority = 0,
        .flags = {
#if CONFIG_S2_CAN_LISTEN_ONLY
            .enable_listen_only = 1,
#endif
            .no_receive_rtr = 1,
        },
    };
    ESP_RETURN_ON_ERROR(twai_new_node_onchip(&cfg, &s_node), TAG, "twai_new_node_onchip");

    const twai_event_callbacks_t cbs = {
        .on_rx_done = on_rx_done,
        .on_state_change = on_state_change,
        .on_error = on_error,
        .on_tx_done = on_tx_done,
    };
    ESP_RETURN_ON_ERROR(twai_node_register_event_callbacks(s_node, &cbs, NULL), TAG, "register callbacks");

    /*
     * Accept everything: id 0 / mask 0 is the driver's "full open" filter (hardware
     * mask all-don't-care, software id-type check off) - identical to the default.
     */
    twai_mask_filter_config_t accept_all = {
        .id = 0,
        .mask = 0,
        .is_ext = 0,
    };
    ESP_RETURN_ON_ERROR(twai_node_config_mask_filter(s_node, 0, &accept_all), TAG, "filter");

    ESP_RETURN_ON_ERROR(twai_node_enable(s_node), TAG, "twai_node_enable");
    s_stats.state = TWAI_ERROR_ACTIVE;
    ESP_LOGI(TAG, "TWAI up: %d kbit/s, TX GPIO%d, RX GPIO%d, %s",
             CONFIG_S2_CAN_BITRATE / 1000, CONFIG_S2_CAN_TX_GPIO, CONFIG_S2_CAN_RX_GPIO,
             can_bus_is_listen_only() ? "LISTEN-ONLY (passive tap)" : "NORMAL mode (can transmit/ACK)");
    return ESP_OK;
}

bool can_bus_receive(can_frame_t *out, TickType_t timeout)
{
    return s_rx_queue && xQueueReceive(s_rx_queue, out, timeout) == pdTRUE;
}

esp_err_t can_bus_transmit(uint32_t id, const uint8_t *data, uint8_t len, uint32_t timeout_ms)
{
    if (!s_node) {
        return ESP_ERR_INVALID_STATE;
    }
    if (can_bus_is_listen_only()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (len > TWAI_FRAME_MAX_LEN || (id & ~TWAI_STD_ID_MASK)) {
        return ESP_ERR_INVALID_ARG;
    }
    /*
     * The driver keeps a pointer to the frame until on_tx_done fires, so the
     * frame and its payload must outlive this call: use a small static ring.
     */
    static twai_frame_t frames[CONFIG_S2_CAN_TX_QUEUE_DEPTH + 1];
    static uint8_t payloads[CONFIG_S2_CAN_TX_QUEUE_DEPTH + 1][TWAI_FRAME_MAX_LEN];
    static unsigned next;
    unsigned slot = next;

    memcpy(payloads[slot], data, len);
    memset(&frames[slot], 0, sizeof(frames[slot]));
    frames[slot].header.id = id;
    frames[slot].header.dlc = twaifd_len2dlc(len);
    frames[slot].buffer = payloads[slot];
    frames[slot].buffer_len = len;
    esp_err_t err = twai_node_transmit(s_node, &frames[slot], (int)timeout_ms);
    if (err == ESP_OK) {
        /*
         * Advance only on success: with depth + 1 slots and one slot consumed per
         * accepted frame, the slot being rewritten is always older than every frame
         * the driver can still hold (depth queued + 1 in hardware).
         */
        next = (next + 1) % (CONFIG_S2_CAN_TX_QUEUE_DEPTH + 1);
    }
    return err;
}

void can_bus_get_stats(can_bus_stats_t *out)
{
    twai_node_status_t st;
    twai_node_record_t rec;
    if (s_node && twai_node_get_info(s_node, &st, &rec) == ESP_OK) {
        /* Note: TEC/REC are not populated by esp_driver_twai 5.5.1 on this target (always 0). */
        s_stats.tx_error_count = st.tx_error_count;
        s_stats.rx_error_count = st.rx_error_count;
        s_stats.hal_bus_errors = rec.bus_err_num;
        /*
         * In listen-only mode the driver's cached state is the errata preset
         * (REC forced to 128 -> "error passive", CONFIG_TWAI_ERRATA_FIX_LISTEN_ONLY_DOM)
         * sampled once at enable and never updated; bus-off cannot occur. Keep the
         * state tracked by on_state_change() instead.
         */
        if (!can_bus_is_listen_only()) {
            s_stats.state = st.state;
        }
    }
    memcpy(out, (const void *)&s_stats, sizeof(*out));
}

void can_bus_service(void)
{
    if (s_node && s_bus_off && !s_recovering) {
        ESP_LOGW(TAG, "bus-off: starting recovery");
        if (twai_node_recover(s_node) == ESP_OK) {
            s_recovering = true;
        }
    }
}

bool can_bus_is_listen_only(void)
{
#if CONFIG_S2_CAN_LISTEN_ONLY
    return true;
#else
    return false;
#endif
}

const char *can_bus_state_name(twai_error_state_t state)
{
    switch (state) {
    case TWAI_ERROR_ACTIVE:  return "active";
    case TWAI_ERROR_WARNING: return "warning";
    case TWAI_ERROR_PASSIVE: return "passive";
    case TWAI_ERROR_BUS_OFF: return "BUS-OFF";
    default:                 return "?";
    }
}
