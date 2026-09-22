/*
 * s2-dashboard - LiveWire S2 Del Mar CAN statistics logger (Waveshare ESP32-S3-Zero).
 *
 * Passive tap on the secondary CAN bus at the Data Link Connector, decoding
 * every frame of the community DBC (can-db/) and logging over the USB serial
 * console. Optional UDS polling of the diagnostic modules through the BCM gateway.
 */
#include <stdio.h>

#include "ble_telemetry.h"
#include "can_bus.h"
#include "decoder.h"
#include "display.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "log_writer.h"
#include "nvs_flash.h"
#include "ota.h"
#include "s2_dbc_gen.h"
#include "s2_uds_gen.h"
#include "sdkconfig.h"
#include "status_led.h"
#include "summary.h"
#include "uds_client.h"
#include "vehicle_button.h"
#include "vehicle_state.h"

static const char *TAG = "main";

/* Boolean Kconfig symbols are undefined (not 0) when disabled. */
#ifdef CONFIG_S2_LOG_RAW_FRAMES
#define OPT_RAW_LOG 1
#else
#define OPT_RAW_LOG 0
#endif
#ifdef CONFIG_S2_LOG_SIGNAL_CHANGES
#define OPT_CHANGE_LOG 1
#define OPT_CHANGE_MIN_MS CONFIG_S2_LOG_CHANGE_MIN_INTERVAL_MS
#else
#define OPT_CHANGE_LOG 0
#define OPT_CHANGE_MIN_MS 0   /* the int is not emitted when its dependency is off */
#endif
#ifdef CONFIG_S2_UDS_ENABLE
#define OPT_UDS 1
#else
#define OPT_UDS 0
#endif
#ifdef CONFIG_S2_BLE_NUS_ENABLE
#define OPT_BLE_NUS 1
#else
#define OPT_BLE_NUS 0
#endif
#ifdef CONFIG_S2_DISPLAY_BUTTON_GPIO
#define OPT_BUTTON_GPIO CONFIG_S2_DISPLAY_BUTTON_GPIO
#else
#define OPT_BUTTON_GPIO (-1)   /* the int is not emitted unless the pin is selected */
#endif

static void update_led(void)
{
    static uint32_t prev_frames, prev_crc_fail;
    static int64_t last_eval_us;
    static led_state_t state = LED_STATE_BOOT;

    int64_t now = esp_timer_get_time();
    if (now - last_eval_us < 1000000) {
        return;
    }
    last_eval_us = now;

    can_bus_stats_t bus;
    decoder_stats_t dec;
    can_bus_get_stats(&bus);
    decoder_get_stats(&dec);

    bool traffic = dec.frames != prev_frames;
    bool degraded = dec.crc_fail != prev_crc_fail;
    prev_frames = dec.frames;
    prev_crc_fail = dec.crc_fail;

    if (bus.state == TWAI_ERROR_BUS_OFF || bus.state == TWAI_ERROR_PASSIVE) {
        state = LED_STATE_BUS_ERROR;
    } else if (!traffic) {
        state = LED_STATE_IDLE;
    } else {
        state = degraded ? LED_STATE_RX_DEGRADED : LED_STATE_RX_OK;
    }
    status_led_set_state(state);
}

/*
 * NVS is initialised here rather than inside ble_telemetry_start(), because the
 * Wi-Fi stack needs it too and a BLE-off build would otherwise never call it.
 */
static void nvs_bring_up(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
    }
}

void app_main(void)
{
    ESP_ERROR_CHECK(log_writer_start());
    vs_init();
    nvs_bring_up();

    /*
     * Asked for by a long press before the reboot that landed here. Read once
     * and cleared, so a power cycle always comes up in normal operation.
     */
    bool update_mode = ota_boot_is_update_mode();
    if (status_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "status LED unavailable");
    }

    const esp_app_desc_t *app = esp_app_get_description();
    log_line("%s", "");
    log_line("s2-dashboard %s (%s %s) - LiveWire S2 CAN logger", app->version, app->date, app->time);
    log_line("database: %s", S2_DBC_VERSION);
    log_line("          %u broadcast messages / %u signals, %u UDS modules / %u DIDs", S2_DBC_MESSAGE_COUNT,
             S2_DBC_SIGNAL_COUNT, (unsigned)s2_uds_module_count, (unsigned)s2_uds_did_count);
    log_line("config:   TX GPIO%d, RX GPIO%d, %d kbit/s, %s, raw-log %s, change-log %s (>= %d ms), summary %d ms, UDS %s",
             CONFIG_S2_CAN_TX_GPIO, CONFIG_S2_CAN_RX_GPIO, CONFIG_S2_CAN_BITRATE / 1000,
             can_bus_is_listen_only() ? "listen-only" : "NORMAL mode",
             OPT_RAW_LOG ? "on" : "off", OPT_CHANGE_LOG ? "on" : "off",
             OPT_CHANGE_MIN_MS, CONFIG_S2_SUMMARY_PERIOD_MS,
             OPT_UDS ? "ENABLED" : "off");
#if CONFIG_S2_DISPLAY_ENABLE
    log_line("display:  ST7789 240x280, SCLK%d MOSI%d CS%d DC%d RST%d BL%d, %d Hz refresh, %d screens",
             CONFIG_S2_DISPLAY_SCLK_GPIO, CONFIG_S2_DISPLAY_MOSI_GPIO, CONFIG_S2_DISPLAY_CS_GPIO,
             CONFIG_S2_DISPLAY_DC_GPIO, CONFIG_S2_DISPLAY_RST_GPIO, CONFIG_S2_DISPLAY_BL_GPIO,
             CONFIG_S2_DISPLAY_REFRESH_HZ, CONFIG_S2_DISPLAY_SCREENS);
    {
        const vbtn_def_t *btn = vbtn_selected();
        if (btn) {
            log_line("screens:  cycled by the %s button on the bars (%s)", btn->label, btn->note);
        } else if (OPT_BUTTON_GPIO >= 0) {
            log_line("screens:  cycled by a switch on GPIO%d", OPT_BUTTON_GPIO);
        } else {
            log_line("screens:  fixed on screen 1 (no button configured)");
        }
    }
#else
    log_line("display:  disabled");
#endif
#if CONFIG_S2_BLE_ENABLE
    log_line("ble:      \"%s\", dashboard %d Hz, snapshot %d ms, Nordic UART %s",
             CONFIG_S2_BLE_DEVICE_NAME, CONFIG_S2_BLE_DASH_HZ, CONFIG_S2_BLE_SNAPSHOT_MS,
             OPT_BLE_NUS ? "on" : "off");
#else
    log_line("ble:      disabled");
#endif
    ota_log_boot_state();
#if CONFIG_S2_OTA_ENABLE && CONFIG_S2_DISPLAY_ENABLE
    log_line("update:   hold the screen button %d ms; access point \"%s\"",
             CONFIG_S2_OTA_LONG_PRESS_MS, CONFIG_S2_OTA_AP_SSID);
#elif CONFIG_S2_OTA_ENABLE
    /*
     * The only way into update mode is the screen button, and the passphrase is
     * only ever shown on the screen, so a display-less build cannot use it.
     */
    log_line("update:   built in but unreachable - update mode needs the display");
#else
    log_line("update:   over-the-air updates disabled");
#endif
    if (update_mode) {
        log_line("%s", "*** UPDATE MODE - BLE stays off, waiting for a firmware upload ***");
    }

#if CONFIG_S2_DISPLAY_ENABLE
    /*
     * Before the CAN queues and task stacks, so the 134 KB framebuffer gets the
     * least fragmented heap. The screen shows its no-data placeholders until the
     * bus comes up.
     */
    log_line("heap:     %u bytes free, largest DMA-capable block %u bytes",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (display_start() != ESP_OK) {
        ESP_LOGE(TAG, "display not started - check the wiring and the S2_DISPLAY_* options");
    }
#endif

#if CONFIG_S2_BLE_ENABLE
    /*
     * Skipped in update mode. The BLE controller executes from flash, and
     * erasing flash stalls the cache, so the safe thing is for it never to have
     * started. That also leaves the heap free for the Wi-Fi stack.
     */
    if (!update_mode) {
        if (ble_telemetry_start() != ESP_OK) {
            ESP_LOGE(TAG, "BLE not started - the CAN log and the screen carry on without it");
        }
        log_line("heap:     %u bytes free after BLE init",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
#endif

    /*
     * CAN runs in update mode too, so the handlebar button can still cancel and
     * the standstill check stays live. It is listen-only, so losing frames to a
     * flash erase costs nothing and cannot disturb a bus we never acknowledge.
     */
    ESP_ERROR_CHECK(can_bus_start());

    xTaskCreatePinnedToCore(decoder_task, "can_decode", 8192, NULL, 10, NULL, 1);
    if (!update_mode) {
        xTaskCreate(summary_task, "summary", 8192, NULL, 3, NULL);
    }
#if CONFIG_S2_UDS_ENABLE
    if (!update_mode && uds_client_start() != ESP_OK) {
        ESP_LOGE(TAG, "UDS poller not started");
    }
#endif

#if CONFIG_S2_OTA_ENABLE
    if (update_mode) {
        esp_err_t err = ota_update_mode_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "update mode failed to start: %s", esp_err_to_name(err));
            log_line("update:   could not start, rebooting to normal operation");
            ota_leave_update_mode();
        }
        log_line("heap:     %u bytes free with the access point up",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    }
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        can_bus_service();
        ota_tick();
        update_led();
        status_led_tick();
    }
}
