#include "uds_client.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "log_writer.h"
#include "s2_uds_gen.h"
#include "sdkconfig.h"
#include "uds_decode.h"

static uds_stats_t s_stats;

const char *uds_nrc_name(uint8_t nrc)
{
    switch (nrc) {
    case 0x10: return "generalReject";
    case 0x11: return "serviceNotSupported";
    case 0x12: return "subFunctionNotSupported";
    case 0x13: return "incorrectMessageLength";
    case 0x14: return "responseTooLong";
    case 0x21: return "busyRepeatRequest";
    case 0x22: return "conditionsNotCorrect";
    case 0x24: return "requestSequenceError";
    case 0x31: return "requestOutOfRange";
    case 0x33: return "securityAccessDenied";
    case 0x35: return "invalidKey";
    case 0x36: return "exceededNumberOfAttempts";
    case 0x37: return "requiredTimeDelayNotExpired";
    case 0x78: return "responsePending";
    case 0x7E: return "subFunctionNotSupportedInActiveSession";
    case 0x7F: return "serviceNotSupportedInActiveSession";
    default:   return "?";
    }
}

void uds_client_get_stats(uds_stats_t *out)
{
    *out = s_stats;
}

#if CONFIG_S2_UDS_ENABLE

static const char *TAG = "uds";

#define P2_TIMEOUT_MS        CONFIG_S2_UDS_REQUEST_TIMEOUT_MS
#define P2_STAR_TIMEOUT_MS   5000
#define N_CR_TIMEOUT_MS      1000
#define FC_BLOCK_SIZE        0x00   /* send everything */
#define FC_STMIN             0x00   /* ms between consecutive frames */
#define FOREIGN_BACKOFF_MS   2000   /* stay quiet this long after another tester's request */
#define FOREIGN_BACKOFF_MAX_MS 15000

typedef enum {
    ISOTP_OK,
    ISOTP_TIMEOUT,
    ISOTP_TRANSPORT_ERROR,
} isotp_status_t;

static QueueHandle_t s_rx_queue;
static SemaphoreHandle_t s_lock;
static volatile uint32_t s_expected_resp_id;
static volatile uint32_t s_expected_req_id;
static volatile bool s_foreign_request_seen;      /* someone else used our request id */
static volatile int64_t s_last_foreign_request_us; /* any tester request seen on the bus */

static bool is_module_request_id(uint32_t id)
{
    for (size_t i = 0; i < s2_uds_module_count; i++) {
        if (s2_uds_modules[i].req_id == id) {
            return true;
        }
    }
    return false;
}

void uds_client_on_frame(const can_frame_t *f)
{
    /* Our own transmissions are not looped back, so a request id on RX is another tester. */
    if (is_module_request_id(f->id)) {
        s_last_foreign_request_us = f->timestamp_us;
        if (f->id == s_expected_req_id) {
            s_foreign_request_seen = true;
        }
        return;
    }
    if (s_rx_queue && f->id == s_expected_resp_id) {
        xQueueSend(s_rx_queue, f, 0);
    }
}

static esp_err_t send_padded(uint32_t id, const uint8_t *payload, uint8_t len)
{
    uint8_t frame[8];
    memset(frame, UDS_PADDING_BYTE, sizeof(frame));
    memcpy(frame, payload, len);
    esp_err_t err = can_bus_transmit(id, frame, 8, 100);
    if (err != ESP_OK) {
        s_stats.tx_errors++;
    }
    return err;
}

static bool wait_frame(can_frame_t *f, int64_t deadline_us)
{
    int64_t now = esp_timer_get_time();
    if (now >= deadline_us) {
        return false;
    }
    TickType_t ticks = pdMS_TO_TICKS((deadline_us - now) / 1000) + 1;
    return xQueueReceive(s_rx_queue, f, ticks) == pdTRUE;
}

/*
 * Receive one complete ISO-TP message that answers our request for `did`.
 * Single frames are returned as they come (the caller matches SID/DID); a
 * multi-frame transfer is only claimed - and flow-controlled - when its first
 * bytes are `62 <did>`; other testers' transfers on the same response id are
 * left alone (their consecutive frames are ignored).
 */
static size_t isotp_receive(uint16_t req_id, uint16_t did, uint8_t *buf, size_t buflen, int64_t deadline_us,
                            isotp_status_t *status)
{
    can_frame_t f;
    for (;;) {
        if (!wait_frame(&f, deadline_us)) {
            *status = ISOTP_TIMEOUT;
            return 0;
        }
        if (f.dlc < 1) {
            continue;
        }
        uint8_t pci = f.data[0] >> 4;
        if (pci == 0) {                       /* single frame */
            size_t len = f.data[0] & 0x0F;
            if (len == 0 || len > 7 || len + 1 > f.dlc) {
                s_stats.transport_errors++;
                continue;
            }
            memcpy(buf, &f.data[1], len);
            *status = ISOTP_OK;
            return len;
        }
        if (pci == 1) {                       /* first frame */
            if (f.dlc < 8) {
                s_stats.transport_errors++;
                continue;
            }
            bool ours = f.data[2] == 0x62 && f.data[3] == (uint8_t)(did >> 8) && f.data[4] == (uint8_t)did;
            if (!ours) {
                continue;                     /* another tester's transfer: no flow control from us */
            }
            size_t total = ((size_t)(f.data[0] & 0x0F) << 8) | f.data[1];
            if (total > buflen) {
                s_stats.transport_errors++;
                const uint8_t fc[3] = { 0x32, 0, 0 };   /* flow control: overflow */
                send_padded(req_id, fc, sizeof(fc));
                *status = ISOTP_TRANSPORT_ERROR;
                return 0;
            }
            memcpy(buf, &f.data[2], 6);
            size_t got = 6;
            const uint8_t fc[3] = { 0x30, FC_BLOCK_SIZE, FC_STMIN };
            if (send_padded(req_id, fc, sizeof(fc)) != ESP_OK) {
                *status = ISOTP_TRANSPORT_ERROR;
                return 0;
            }
            uint8_t seq = 1;
            int64_t cf_deadline = esp_timer_get_time() + (int64_t)N_CR_TIMEOUT_MS * 1000;
            while (got < total) {
                if (!wait_frame(&f, cf_deadline)) {
                    s_stats.transport_errors++;
                    *status = ISOTP_TRANSPORT_ERROR;
                    return 0;
                }
                if ((f.data[0] >> 4) != 2) {
                    continue;                 /* stray frame on this id while our CFs are flowing */
                }
                if ((f.data[0] & 0x0F) != seq) {
                    s_stats.transport_errors++;
                    *status = ISOTP_TRANSPORT_ERROR;
                    return 0;
                }
                seq = (uint8_t)((seq + 1) & 0x0F);
                size_t chunk = total - got;
                if (chunk > 7) {
                    chunk = 7;
                }
                if (f.dlc < chunk + 1) {
                    s_stats.transport_errors++;
                    *status = ISOTP_TRANSPORT_ERROR;
                    return 0;
                }
                memcpy(buf + got, &f.data[1], chunk);
                got += chunk;
                cf_deadline = esp_timer_get_time() + (int64_t)N_CR_TIMEOUT_MS * 1000;
            }
            *status = ISOTP_OK;
            return total;
        }
        /* pci 2 (consecutive frame of a transfer we did not claim) or 3 (flow control): ignore */
    }
}

esp_err_t uds_read_did(uint16_t req_id, uint16_t resp_id, uint16_t did, uint8_t *buf, size_t *len, uint8_t *nrc)
{
    static uint8_t msg[UDS_MAX_PAYLOAD];
    *len = 0;
    *nrc = 0;
    if (!s_rx_queue) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_stats.requests++;

    s_foreign_request_seen = false;
    s_expected_req_id = req_id;
    s_expected_resp_id = resp_id;
    xQueueReset(s_rx_queue);
    const uint8_t req[4] = { 0x03, 0x22, (uint8_t)(did >> 8), (uint8_t)did };
    esp_err_t err = send_padded(req_id, req, sizeof(req));
    if (err != ESP_OK) {
        goto out;
    }

    int64_t deadline = esp_timer_get_time() + (int64_t)P2_TIMEOUT_MS * 1000;
    uint8_t pending_nrc = 0;
    for (;;) {
        isotp_status_t status;
        size_t n = isotp_receive(req_id, did, msg, sizeof(msg), deadline, &status);
        if (n == 0) {
            if (status == ISOTP_TRANSPORT_ERROR) {
                err = ESP_ERR_INVALID_SIZE;
            } else if (pending_nrc) {
                /* No positive response arrived either: report the NRC we saw. */
                *nrc = pending_nrc;
                s_stats.negative++;
                err = ESP_ERR_INVALID_RESPONSE;
            } else {
                s_stats.timeouts++;
                err = ESP_ERR_TIMEOUT;
            }
            break;
        }
        if (msg[0] == 0x7F && n >= 3) {
            if (msg[1] != 0x22) {
                continue;                     /* negative response to someone else's service */
            }
            if (msg[2] == 0x78) {             /* response pending: extend to P2* */
                deadline = esp_timer_get_time() + (int64_t)P2_STAR_TIMEOUT_MS * 1000;
                continue;
            }
            if (!s_foreign_request_seen) {
                *nrc = msg[2];
                s_stats.negative++;
                err = ESP_ERR_INVALID_RESPONSE;
                break;
            }
            /*
             * Another tester used our request id since we sent: an NRC carries no DID
             * echo, so it may be theirs. Remember it and keep waiting for `62 <did>`.
             */
            pending_nrc = msg[2];
            s_stats.unattributed_nrc++;
            continue;
        }
        if (msg[0] == 0x62 && n >= 3) {
            uint16_t got_did = (uint16_t)((msg[1] << 8) | msg[2]);
            if (got_did != did) {
                continue;                     /* stale / foreign response, keep waiting */
            }
            *len = n - 3;
            memcpy(buf, &msg[3], *len);
            s_stats.positive++;
            err = ESP_OK;
            break;
        }
        /* anything else on this id (e.g. the cluster's own 0x19 traffic): ignore */
    }

out:
    s_expected_resp_id = 0;
    s_expected_req_id = 0;
    xSemaphoreGive(s_lock);
    return err;
}

static bool should_poll(const s2_uds_did_t *d)
{
    if (d->locked || d->status == S2_UDS_REFUTED) {
        return false;
    }
#if !CONFIG_S2_UDS_POLL_TCU
    if (d->module == S2_UDS_MOD_TCU) {
        return false;
    }
#endif
    return true;
}

/* Wait until the bus has been free of other testers' requests for a while. */
static void back_off_from_other_testers(void)
{
    int64_t start = esp_timer_get_time();
    for (;;) {
        int64_t since = (esp_timer_get_time() - s_last_foreign_request_us) / 1000;
        if (s_last_foreign_request_us == 0 || since >= FOREIGN_BACKOFF_MS) {
            return;
        }
        if ((esp_timer_get_time() - start) / 1000 >= FOREIGN_BACKOFF_MAX_MS) {
            return;                           /* the cluster is polling continuously: share the bus */
        }
        s_stats.backoffs++;
        vTaskDelay(pdMS_TO_TICKS(FOREIGN_BACKOFF_MS - since + 10));
    }
}

static void uds_task(void *arg)
{
    (void)arg;
    static uint8_t data[UDS_MAX_PAYLOAD];
    vTaskDelay(pdMS_TO_TICKS(3000));         /* let the passive decoder settle first */
    unsigned polled = 0;
    for (size_t i = 0; i < s2_uds_did_count; i++) {
        polled += should_poll(&s2_uds_dids[i]) ? 1 : 0;
    }
    log_tline("UDS poller: %u of %u catalogued DIDs, round >= %d ms, timeout %d ms", polled,
              (unsigned)s2_uds_did_count, CONFIG_S2_UDS_ROUND_PERIOD_MS, P2_TIMEOUT_MS);

    for (;;) {
        int64_t round_start = esp_timer_get_time();
        for (size_t i = 0; i < s2_uds_did_count; i++) {
            const s2_uds_did_t *d = &s2_uds_dids[i];
            if (!should_poll(d)) {
                continue;
            }
            back_off_from_other_testers();
            const s2_uds_module_t *m = &s2_uds_modules[d->module];
            size_t len = 0;
            uint8_t nrc = 0;
            esp_err_t err = uds_read_did(m->req_id, m->resp_id, d->did, data, &len, &nrc);
            if (err == ESP_OK) {
                uds_decode_log(m, d, data, len);
            } else if (err == ESP_ERR_INVALID_RESPONSE) {
                log_tline("UDS %s %04X: NRC 0x%02X %s%s", m->name, d->did, nrc, uds_nrc_name(nrc),
                          s_foreign_request_seen ? " (another tester was active - attribution uncertain)" : "");
            } else if (err == ESP_ERR_TIMEOUT) {
                log_tline("UDS %s %04X: no response", m->name, d->did);
            } else if (err == ESP_ERR_INVALID_SIZE) {
                log_tline("UDS %s %04X: ISO-TP transport error (sequence/length/flow control)", m->name, d->did);
            } else {
                log_tline("UDS %s %04X: tx error %s", m->name, d->did, esp_err_to_name(err));
            }
            vTaskDelay(pdMS_TO_TICKS(CONFIG_S2_UDS_INTER_REQUEST_GAP_MS));
        }
        s_stats.rounds++;
        int64_t elapsed_ms = (esp_timer_get_time() - round_start) / 1000;
        if (elapsed_ms < CONFIG_S2_UDS_ROUND_PERIOD_MS) {
            vTaskDelay(pdMS_TO_TICKS(CONFIG_S2_UDS_ROUND_PERIOD_MS - elapsed_ms));
        }
    }
}

esp_err_t uds_client_start(void)
{
    if (can_bus_is_listen_only()) {
        ESP_LOGE(TAG, "UDS polling needs normal CAN mode (disable CONFIG_S2_CAN_LISTEN_ONLY)");
        return ESP_ERR_NOT_SUPPORTED;
    }
    s_lock = xSemaphoreCreateMutex();
    s_rx_queue = xQueueCreate(64, sizeof(can_frame_t));
    if (!s_lock || !s_rx_queue) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(uds_task, "uds", 8192, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

#else /* !CONFIG_S2_UDS_ENABLE */

void uds_client_on_frame(const can_frame_t *f) { (void)f; }
esp_err_t uds_client_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t uds_read_did(uint16_t req_id, uint16_t resp_id, uint16_t did, uint8_t *buf, size_t *len, uint8_t *nrc)
{
    (void)req_id; (void)resp_id; (void)did; (void)buf; (void)nrc;
    *len = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
