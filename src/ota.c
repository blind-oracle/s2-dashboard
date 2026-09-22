#include "ota.h"

#include <string.h>

#include "sdkconfig.h"

#if !CONFIG_S2_OTA_ENABLE

/* OTA disabled at build time: keep the API linkable and inert. */
bool ota_boot_is_update_mode(void) { return false; }
void ota_log_boot_state(void) {}
esp_err_t ota_update_mode_start(void) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t ota_request_update_mode(void) { return ESP_ERR_NOT_SUPPORTED; }
void ota_leave_update_mode(void) {}
bool ota_update_mode_active(void) { return false; }
void ota_tick(void) {}
void ota_get_update_data(update_data_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
}

#else

#include <stdarg.h>
#include <stdio.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "log_writer.h"
#include "s2_dbc_gen.h"
#include "vehicle_state.h"

static const char *TAG = "ota";

/*
 * The request to enter update mode is carried across the reboot in RTC memory,
 * so deciding to write flash involves no flash write of its own.
 *
 * RTC_NOINIT_ATTR, not RTC_DATA_ATTR: the startup code clears .rtc.bss on every
 * reset except a deep-sleep wake (esp_system/port/cpu_start.c), so a
 * RTC_DATA_ATTR variable would be zeroed by the very reboot that is supposed to
 * carry the request. The .rtc_noinit section is never cleared, which is why the
 * value needs a magic: on a cold boot it holds whatever the RTC RAM held, and
 * only this exact word counts as a request. It is cleared as soon as it is read,
 * so even a one-in-four-billion false match self-corrects on the next boot.
 */
#define OTA_REQUEST_MAGIC 0x52455551u   /* "REQU" */
static RTC_NOINIT_ATTR uint32_t s_boot_request;

/* Bytes read from the socket per iteration. */
#define RECV_CHUNK 2048

/* Anything smaller than this cannot be a valid app image. */
#define MIN_IMAGE_BYTES 4096

/* Consecutive socket timeouts before an upload is given up on. */
#define MAX_RECV_STALLS 3

/* Stationary means at or below this. */
#define MOVING_KMH 1.0

static bool s_update_mode;
static httpd_handle_t s_httpd;
static esp_netif_t *s_netif;

/* Written by the HTTP task, read by the display task. */
static update_data_t s_status;
static SemaphoreHandle_t s_status_lock;

static int64_t s_last_activity_us;
static int64_t s_boot_us;
static bool s_confirmed;

/* ------------------------------------------------------------- status --- */

static void status_lock(void)
{
    if (s_status_lock) {
        xSemaphoreTake(s_status_lock, portMAX_DELAY);
    }
}

static void status_unlock(void)
{
    if (s_status_lock) {
        xSemaphoreGive(s_status_lock);
    }
}

static void status_set_phase(update_phase_t phase)
{
    status_lock();
    s_status.phase = phase;
    status_unlock();
}

static void status_progress(uint32_t received, uint32_t total)
{
    status_lock();
    s_status.received = received;
    s_status.total = total;
    status_unlock();
}

static void status_detail(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void status_detail(const char *fmt, ...)
{
    char buf[UPDATE_DETAIL_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    status_lock();
    snprintf(s_status.detail, sizeof(s_status.detail), "%s", buf);
    status_unlock();
}

void ota_get_update_data(update_data_t *out)
{
    if (!out) {
        return;
    }
    if (!s_update_mode) {
        memset(out, 0, sizeof(*out));
        return;
    }
    status_lock();
    *out = s_status;
    status_unlock();
}

bool ota_update_mode_active(void)
{
    return s_update_mode;
}

/* ------------------------------------------------------- boot handling --- */

bool ota_boot_is_update_mode(void)
{
    bool requested = (s_boot_request == OTA_REQUEST_MAGIC);
    s_boot_request = 0;         /* one shot: a power cycle comes up normally */
    return requested;
}

static const char *img_state_name(esp_ota_img_states_t st)
{
    switch (st) {
    case ESP_OTA_IMG_NEW: return "new";
    case ESP_OTA_IMG_PENDING_VERIFY: return "pending-verify";
    case ESP_OTA_IMG_VALID: return "valid";
    case ESP_OTA_IMG_INVALID: return "invalid";
    case ESP_OTA_IMG_ABORTED: return "aborted";
    default: return "undefined";
    }
}

/* True while this image still has to prove itself or the bootloader reverts. */
static bool image_on_probation(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!run || esp_ota_get_state_partition(run, &st) != ESP_OK) {
        return false;
    }
    return st == ESP_OTA_IMG_PENDING_VERIFY;
}

void ota_log_boot_state(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (!run) {
        log_line("ota:      no running partition reported");
        return;
    }
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    esp_ota_get_state_partition(run, &st);
    log_line("ota:      running %s at 0x%06lx (%lu KB slot), image %s, %u app slots",
             run->label, (unsigned long)run->address, (unsigned long)(run->size / 1024),
             img_state_name(st), (unsigned)esp_ota_get_app_partition_count());
}

/* --------------------------------------------------- entering / leaving --- */

/*
 * Only vehicle_speed is consulted. The database also carries a standstill flag
 * on 0x12A, but its polarity is not established well enough to gate a reboot
 * on, and a wrong guess there would either refuse updates while parked or allow
 * one while rolling. A speed that has never been seen counts as stationary, so
 * this still works on a bench with no bus attached.
 */
static bool bike_is_moving(void)
{
    bool moving = false;
    vs_lock();
    const vs_signal_t *sp = vs_signal(S2_SIG_SPEED_160_vehicle_speed);
    if (sp && sp->valid) {
        moving = sp->value > MOVING_KMH;
    }
    vs_unlock();
    return moving;
}

esp_err_t ota_request_update_mode(void)
{
    if (s_update_mode) {
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_ota_get_app_partition_count() < 2) {
        log_tline("ota: refused, this firmware was flashed to a single-slot table");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (image_on_probation()) {
        log_tline("ota: refused, the running image has not been confirmed yet");
        return ESP_ERR_INVALID_STATE;
    }
    if (bike_is_moving()) {
        log_tline("ota: refused, the bike is moving");
        return ESP_ERR_INVALID_STATE;
    }
    log_tline("ota: entering update mode, rebooting");
    s_boot_request = OTA_REQUEST_MAGIC;
    vTaskDelay(pdMS_TO_TICKS(150));     /* let the log drain */
    esp_restart();
}

void ota_leave_update_mode(void)
{
    s_boot_request = 0;
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
}

void ota_tick(void)
{
    int64_t now = esp_timer_get_time();
    if (!s_boot_us) {
        s_boot_us = now;
    }

    /*
     * Confirm a probationary image once it has stayed up long enough with every
     * subsystem started. A panic or a watchdog before this point means the next
     * boot reverts to the slot that was working, which is the whole point.
     */
    if (!s_confirmed && (now - s_boot_us) >= (int64_t)CONFIG_S2_OTA_SELF_TEST_S * 1000000) {
        s_confirmed = true;
        if (image_on_probation()) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                log_tline("ota: image confirmed after %d s, rollback cancelled",
                          CONFIG_S2_OTA_SELF_TEST_S);
            } else {
                ESP_LOGE(TAG, "failed to confirm the running image");
            }
        }
    }

    if (!s_update_mode) {
        return;
    }

    /* Never leave the access point up by accident. */
    update_data_t d;
    ota_get_update_data(&d);
    if (d.phase == UPDATE_RECEIVING || d.phase == UPDATE_DONE) {
        s_last_activity_us = now;
        return;
    }
    if ((now - s_last_activity_us) >= (int64_t)CONFIG_S2_OTA_IDLE_TIMEOUT_S * 1000000) {
        log_tline("ota: idle for %d s, leaving update mode", CONFIG_S2_OTA_IDLE_TIMEOUT_S);
        ota_leave_update_mode();
    }
}

/* --------------------------------------------------------- HTTP server --- */

static const char k_page[] =
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>s2-dashboard update</title>"
    "<style>body{font:16px system-ui;margin:0;padding:24px;background:#111;color:#eee}"
    "h1{font-size:20px;margin:0 0 4px}p{color:#aaa;margin:4px 0 20px}"
    "input,button{font:inherit;width:100%;box-sizing:border-box;padding:12px;margin:6px 0}"
    "button{background:#fb0;border:0;border-radius:6px;font-weight:600}"
    "progress{width:100%;height:24px}#s{margin-top:14px}</style>"
    "<h1>s2-dashboard</h1><p id=v></p>"
    "<input id=f type=file accept='.bin'><button onclick='go()'>Flash firmware</button>"
    "<progress id=p value=0 max=100 hidden></progress><div id=s></div>"
    "<script>"
    "fetch('/info').then(r=>r.text()).then(t=>v.textContent=t);"
    "function go(){var fl=f.files[0];if(!fl){s.textContent='Pick a .bin first';return;}"
    "p.hidden=false;var x=new XMLHttpRequest();x.open('POST','/update');"
    "x.upload.onprogress=function(e){if(e.lengthComputable)p.value=e.loaded*100/e.total;};"
    "x.onload=function(){s.textContent=x.responseText;};"
    "x.onerror=function(){s.textContent='Upload failed';};"
    "x.send(fl);}"
    "</script>";

static esp_err_t page_get(httpd_req_t *req)
{
    s_last_activity_us = esp_timer_get_time();
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, k_page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t info_get(httpd_req_t *req)
{
    s_last_activity_us = esp_timer_get_time();
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *run = esp_ota_get_running_partition();
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    char buf[160];
    int n = snprintf(buf, sizeof(buf), "running %s from %s, will write %s (%lu KB slot)",
                     app->version, run ? run->label : "?", next ? next->label : "?",
                     (unsigned long)(next ? next->size / 1024 : 0));
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, buf, n > 0 ? n : 0);
}

static esp_err_t fail(httpd_req_t *req, const char *why)
{
    status_set_phase(UPDATE_FAILED);
    status_detail("%s", why);
    log_tline("ota: upload failed: %s", why);
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, why, HTTPD_RESP_USE_STRLEN);
}

/*
 * Static, not on the stack: the HTTP task's stack is modest and only one upload
 * can be in flight, because the server is configured with a single worker.
 */
static char s_recv_buf[RECV_CHUNK];

static esp_err_t update_post(httpd_req_t *req)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        return fail(req, "no spare app slot");
    }
    if (req->content_len <= 0) {
        return fail(req, "empty upload");
    }
    uint32_t total = (uint32_t)req->content_len;
    if (total > target->size) {
        return fail(req, "image is larger than the slot");
    }
    if (total < MIN_IMAGE_BYTES) {
        return fail(req, "file is too small to be firmware");
    }

    /*
     * OTA_WITH_SEQUENTIAL_WRITES, never OTA_SIZE_UNKNOWN: the latter erases the
     * whole 1700 KB slot in one blocking call, and this firmware's CAN ISR lives
     * in flash. Erasing a sector at a time inside esp_ota_write keeps every
     * cache-off window short.
     */
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &h);
    if (err != ESP_OK) {
        return fail(req, esp_err_to_name(err));
    }

    log_tline("ota: receiving %lu KB into %s", (unsigned long)(total / 1024), target->label);
    status_set_phase(UPDATE_RECEIVING);
    status_progress(0, total);
    status_detail("%s", "");

    uint32_t written = 0;
    int remaining = req->content_len;
    bool described = false;
    int stalls = 0;

    while (remaining > 0) {
        int want = remaining < RECV_CHUNK ? remaining : RECV_CHUNK;
        int n = httpd_req_recv(req, s_recv_buf, (size_t)want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            /*
             * Each call already waits recv_wait_timeout seconds, so a bounded
             * number of retries keeps a phone that walked out of range from
             * holding the handler, and update mode, open forever.
             */
            if (++stalls > MAX_RECV_STALLS) {
                esp_ota_abort(h);
                return fail(req, "upload stalled");
            }
            continue;
        }
        stalls = 0;
        if (n <= 0) {
            esp_ota_abort(h);
            return fail(req, "connection dropped");
        }
        err = esp_ota_write(h, s_recv_buf, (size_t)n);
        if (err != ESP_OK) {
            esp_ota_abort(h);
            /* esp_ota_write rejects a first byte that is not the 0xE9 magic. */
            return fail(req, err == ESP_ERR_OTA_VALIDATE_FAILED ? "not a firmware image"
                                                                : esp_err_to_name(err));
        }
        written += (uint32_t)n;
        remaining -= n;
        status_progress(written, total);
        s_last_activity_us = esp_timer_get_time();

        /* Once the header has landed, show which version is arriving. */
        if (!described && written >= sizeof(esp_image_header_t)
                                     + sizeof(esp_image_segment_header_t)
                                     + sizeof(esp_app_desc_t)) {
            described = true;
            esp_app_desc_t incoming;
            if (esp_ota_get_partition_description(target, &incoming) == ESP_OK) {
                const esp_app_desc_t *cur = esp_app_get_description();
                status_detail("%s to %s", cur->version, incoming.version);
                log_tline("ota: image is %s (built %s %s)", incoming.version, incoming.date,
                          incoming.time);
            }
        }
    }

    err = esp_ota_end(h);       /* validates the image checksum */
    if (err != ESP_OK) {
        return fail(req, err == ESP_ERR_OTA_VALIDATE_FAILED ? "image failed validation"
                                                            : esp_err_to_name(err));
    }
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        return fail(req, esp_err_to_name(err));
    }

    status_set_phase(UPDATE_DONE);
    status_progress(written, written);
    log_tline("ota: wrote %lu KB to %s, rebooting into it",
              (unsigned long)(written / 1024), target->label);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_send(req, "Flashed. The dashboard is rebooting into the new firmware.",
                    HTTPD_RESP_USE_STRLEN);

    /* Let the response and the log reach the phone before the reset. */
    vTaskDelay(pdMS_TO_TICKS(800));
    s_boot_request = 0;         /* come back up in normal mode */
    esp_restart();
}

/* ----------------------------------------------------- bringing it up --- */

/*
 * A fresh passphrase per session, shown on the screen and never stored. Digits
 * only: it has to be read off a small display and typed on a phone. WPA2 needs
 * at least eight characters, which is exactly what this produces.
 */
static void make_passphrase(char *out, size_t len)
{
    for (size_t i = 0; i + 1 < len; i++) {
        out[i] = (char)('0' + (esp_random() % 10u));
    }
    out[len - 1] = '\0';
}

esp_err_t ota_update_mode_start(void)
{
    s_status_lock = xSemaphoreCreateMutex();
    if (!s_status_lock) {
        return ESP_ERR_NO_MEM;
    }
    memset(&s_status, 0, sizeof(s_status));
    s_status.phase = UPDATE_WAITING;

    char pass[9];
    make_passphrase(pass, sizeof(pass));

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_netif = esp_netif_create_default_wifi_ap();
    if (!s_netif) {
        return ESP_FAIL;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");

    wifi_config_t wc = { 0 };
    snprintf((char *)wc.ap.ssid, sizeof(wc.ap.ssid), "%s", CONFIG_S2_OTA_AP_SSID);
    wc.ap.ssid_len = (uint8_t)strlen((char *)wc.ap.ssid);
    snprintf((char *)wc.ap.password, sizeof(wc.ap.password), "%s", pass);
    wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    wc.ap.max_connection = 1;
    wc.ap.channel = 1;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "wifi mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &wc), TAG, "wifi config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");

    esp_netif_ip_info_t ip;
    memset(&ip, 0, sizeof(ip));
    esp_netif_get_ip_info(s_netif, &ip);

    httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
    hc.lru_purge_enable = true;
    hc.recv_wait_timeout = 20;
    hc.send_wait_timeout = 20;
    hc.max_uri_handlers = 4;
    hc.stack_size = 6144;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &hc), TAG, "httpd start");

    static const httpd_uri_t u_page = { .uri = "/", .method = HTTP_GET, .handler = page_get };
    static const httpd_uri_t u_info = { .uri = "/info", .method = HTTP_GET, .handler = info_get };
    static const httpd_uri_t u_post = { .uri = "/update", .method = HTTP_POST, .handler = update_post };
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &u_page), TAG, "uri /");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &u_info), TAG, "uri /info");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &u_post), TAG, "uri /update");

    status_lock();
    snprintf(s_status.ssid, sizeof(s_status.ssid), "%s", CONFIG_S2_OTA_AP_SSID);
    snprintf(s_status.pass, sizeof(s_status.pass), "%s", pass);
    snprintf(s_status.ip, sizeof(s_status.ip), IPSTR, IP2STR(&ip.ip));
    status_unlock();

    s_update_mode = true;
    s_last_activity_us = esp_timer_get_time();

    log_line("ota:      update mode - join \"%s\", pass %s, then open http://" IPSTR,
             CONFIG_S2_OTA_AP_SSID, pass, IP2STR(&ip.ip));
    return ESP_OK;
}

#endif /* CONFIG_S2_OTA_ENABLE */
