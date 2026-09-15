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
#include "s2_dbc_gen.h"
#include "s2_uds_gen.h"
#include "sdkconfig.h"
#include "status_led.h"
#include "summary.h"
#include "uds_client.h"
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

void app_main(void)
{
    ESP_ERROR_CHECK(log_writer_start());
    vs_init();
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
    log_line("display:  ST7789 240x280, SCLK%d MOSI%d CS%d DC%d RST%d BL%d, %d Hz refresh, %d screens, button GPIO%d",
             CONFIG_S2_DISPLAY_SCLK_GPIO, CONFIG_S2_DISPLAY_MOSI_GPIO, CONFIG_S2_DISPLAY_CS_GPIO,
             CONFIG_S2_DISPLAY_DC_GPIO, CONFIG_S2_DISPLAY_RST_GPIO, CONFIG_S2_DISPLAY_BL_GPIO,
             CONFIG_S2_DISPLAY_REFRESH_HZ, CONFIG_S2_DISPLAY_SCREENS, CONFIG_S2_DISPLAY_BUTTON_GPIO);
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
    if (ble_telemetry_start() != ESP_OK) {
        ESP_LOGE(TAG, "BLE not started - the CAN log and the screen carry on without it");
    }
    log_line("heap:     %u bytes free after BLE init",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif

    ESP_ERROR_CHECK(can_bus_start());

    xTaskCreatePinnedToCore(decoder_task, "can_decode", 8192, NULL, 10, NULL, 1);
    xTaskCreate(summary_task, "summary", 8192, NULL, 3, NULL);
#if CONFIG_S2_UDS_ENABLE
    if (uds_client_start() != ESP_OK) {
        ESP_LOGE(TAG, "UDS poller not started");
    }
#endif

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(100));
        can_bus_service();
        update_led();
        status_led_tick();
    }
}
