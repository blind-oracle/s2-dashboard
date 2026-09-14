#include "status_led.h"

#include "sdkconfig.h"

#if CONFIG_S2_STATUS_LED_ENABLE

#include "driver/rmt_encoder.h"
#include "driver/rmt_tx.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "led";

#define RMT_RESOLUTION_HZ 10000000  /* 10 MHz -> 0.1 us per tick */

static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_encoder;
static led_state_t s_state = LED_STATE_BOOT;
static unsigned s_phase;
static volatile bool s_busy;

static bool IRAM_ATTR on_trans_done(rmt_channel_handle_t chan, const rmt_tx_done_event_data_t *edata, void *user_ctx)
{
    (void)chan;
    (void)edata;
    (void)user_ctx;
    s_busy = false;
    return false;
}

static void write_grb(uint8_t r, uint8_t g, uint8_t b)
{
    /* The RMT driver encodes from this buffer asynchronously: it must outlive the call. */
    static uint8_t grb[3];
    if (!s_chan || s_busy) {
        return;   /* previous 24-bit burst still in flight: skip this update */
    }
    grb[0] = g;
    grb[1] = r;
    grb[2] = b;
    const rmt_transmit_config_t tx_cfg = { .loop_count = 0 };
    s_busy = true;
    if (rmt_transmit(s_chan, s_encoder, grb, sizeof(grb), &tx_cfg) != ESP_OK) {
        s_busy = false;
    }
}

esp_err_t status_led_init(void)
{
    rmt_tx_channel_config_t chan_cfg = {
        .gpio_num = CONFIG_S2_STATUS_LED_GPIO,
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = RMT_RESOLUTION_HZ,
        .mem_block_symbols = 64,
        .trans_queue_depth = 2,
    };
    ESP_RETURN_ON_ERROR(rmt_new_tx_channel(&chan_cfg, &s_chan), TAG, "rmt channel");

    /* WS2812: T0H 0.3 us / T0L 0.9 us, T1H 0.9 us / T1L 0.3 us (+/-150 ns). */
    rmt_bytes_encoder_config_t enc_cfg = {
        .bit0 = { .level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9 },
        .bit1 = { .level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3 },
        .flags.msb_first = 1,
    };
    ESP_RETURN_ON_ERROR(rmt_new_bytes_encoder(&enc_cfg, &s_encoder), TAG, "rmt encoder");
    const rmt_tx_event_callbacks_t cbs = { .on_trans_done = on_trans_done };
    ESP_RETURN_ON_ERROR(rmt_tx_register_event_callbacks(s_chan, &cbs, NULL), TAG, "rmt callbacks");
    ESP_RETURN_ON_ERROR(rmt_enable(s_chan), TAG, "rmt enable");
    write_grb(0, 0, 8);
    return ESP_OK;
}

void status_led_set_state(led_state_t state)
{
    s_state = state;
}

void status_led_tick(void)
{
    s_phase++;
    switch (s_state) {
    case LED_STATE_BOOT:
        write_grb(0, 0, 8);
        break;
    case LED_STATE_IDLE:
        /* slow blue breathing: 0.5 Hz */
        write_grb(0, 0, (s_phase % 20) < 10 ? 10 : 2);
        break;
    case LED_STATE_RX_OK:
        write_grb(0, (s_phase % 4) < 2 ? 24 : 2, 0);
        break;
    case LED_STATE_RX_DEGRADED:
        write_grb((s_phase % 4) < 2 ? 24 : 4, (s_phase % 4) < 2 ? 16 : 2, 0);
        break;
    case LED_STATE_BUS_ERROR:
        write_grb((s_phase % 10) < 5 ? 40 : 6, 0, 0);
        break;
    }
}

#else /* !CONFIG_S2_STATUS_LED_ENABLE */

esp_err_t status_led_init(void) { return ESP_OK; }
void status_led_set_state(led_state_t state) { (void)state; }
void status_led_tick(void) {}

#endif
