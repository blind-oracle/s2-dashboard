#include "panel.h"

#include <stdbool.h>
#include <string.h>

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !CONFIG_S2_DISPLAY_ENABLE

/* Display disabled: nothing in this file is built. */

#else

#if CONFIG_S2_DISPLAY_BL_PWM
#include "driver/ledc.h"
#endif

static const char *TAG = "panel";

/*
 * spi_host_device_t is SPI1_HOST = 0, SPI2_HOST = 1, SPI3_HOST = 2, so the
 * human-facing option (2 = SPI2, 3 = SPI3) must be shifted, not cast.
 */
/*
 * Boolean Kconfig symbols are undefined, not 0, when off, so they cannot be
 * passed as function arguments without a shim.
 *
 * A 180 degree rotation is MY|MX. Extra mirroring is XORed on top for modules
 * whose native orientation is the mirror image of the usual one.
 */
#ifdef CONFIG_S2_DISPLAY_ROTATE_180
#define OPT_ROT180 true
#else
#define OPT_ROT180 false
#endif
#ifdef CONFIG_S2_DISPLAY_MIRROR_X
#define OPT_MIRROR_X true
#else
#define OPT_MIRROR_X false
#endif
#ifdef CONFIG_S2_DISPLAY_MIRROR_Y
#define OPT_MIRROR_Y true
#else
#define OPT_MIRROR_Y false
#endif

#define DISP_SPI_HOST ((spi_host_device_t)(CONFIG_S2_DISPLAY_SPI_HOST - 1))
_Static_assert(CONFIG_S2_DISPLAY_SPI_HOST != 2 || DISP_SPI_HOST == SPI2_HOST, "SPI host mapping is wrong");
_Static_assert(CONFIG_S2_DISPLAY_SPI_HOST != 3 || DISP_SPI_HOST == SPI3_HOST, "SPI host mapping is wrong");

/* Catch a pin that collides with something already wired on this board. */
_Static_assert(CONFIG_S2_DISPLAY_SCLK_GPIO != CONFIG_S2_CAN_TX_GPIO &&
               CONFIG_S2_DISPLAY_SCLK_GPIO != CONFIG_S2_CAN_RX_GPIO &&
               CONFIG_S2_DISPLAY_MOSI_GPIO != CONFIG_S2_CAN_TX_GPIO &&
               CONFIG_S2_DISPLAY_MOSI_GPIO != CONFIG_S2_CAN_RX_GPIO &&
               CONFIG_S2_DISPLAY_DC_GPIO != CONFIG_S2_CAN_TX_GPIO &&
               CONFIG_S2_DISPLAY_DC_GPIO != CONFIG_S2_CAN_RX_GPIO,
               "a display pin collides with a CAN pin");

#define BAND_ROWS  CONFIG_S2_DISPLAY_BAND_ROWS
#define BAND_BYTES ((size_t)PANEL_W * BAND_ROWS * 2u)
/*
 * Per-transaction ceiling on the ESP32-S3 is 32768 bytes; esp_lcd would split
 * anything larger (holding CS active), but sizing the bus for the band keeps
 * one band = one transaction.
 */
#define MAX_TRANSFER_SZ (BAND_BYTES > 32768u ? 32768u : BAND_BYTES)

/*
 * How long one band should take on the wire, doubled and with a floor, so the
 * wait scales with the band size and the clock instead of being a fixed guess.
 */
#define BAND_MS ((BAND_BYTES * 8ull * 1000ull) / (unsigned long long)CONFIG_S2_DISPLAY_SPI_HZ)
#define FLUSH_WAIT_MS ((uint32_t)(100ull + 2ull * BAND_MS))

static esp_lcd_panel_io_handle_t s_io;
static esp_lcd_panel_handle_t s_panel;
static uint16_t *s_fb;
static gfx_t s_gfx;
static SemaphoreHandle_t s_flush_done;

static bool on_color_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    (void)io;
    (void)edata;
    (void)user_ctx;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_flush_done, &woken);
    /* esp_lcd ignores the return value here, so ask for the yield directly. */
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
    return woken == pdTRUE;
}

#if CONFIG_S2_DISPLAY_BL_PWM
#define BL_TIMER      LEDC_TIMER_0
#define BL_CHANNEL    LEDC_CHANNEL_0
#define BL_RES        LEDC_TIMER_10_BIT
#define BL_MAX_DUTY   ((1 << 10) - 1)

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,     /* the ESP32-S3 has no high-speed mode */
        .duty_resolution = BL_RES,
        .timer_num = BL_TIMER,
        .freq_hz = 20000,                      /* above hearing, well inside the 10-bit limit */
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer), TAG, "ledc timer");
    ledc_channel_config_t ch = {
        .gpio_num = CONFIG_S2_DISPLAY_BL_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_CHANNEL,
        .timer_sel = BL_TIMER,
        .duty = 0,                             /* stay dark until the first frame is up */
        .hpoint = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ch), TAG, "ledc channel");
    return ESP_OK;
}

esp_err_t panel_set_brightness(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    uint32_t duty = (uint32_t)((BL_MAX_DUTY * percent) / 100);
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL, duty), TAG, "ledc duty");
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_CHANNEL);
}
#else /* plain GPIO or not connected */
#include "driver/gpio.h"

static esp_err_t backlight_init(void)
{
#if CONFIG_S2_DISPLAY_BL_GPIO >= 0
    gpio_config_t cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << CONFIG_S2_DISPLAY_BL_GPIO,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&cfg), TAG, "bl gpio");
    gpio_set_level(CONFIG_S2_DISPLAY_BL_GPIO, 0);
#endif
    return ESP_OK;
}

esp_err_t panel_set_brightness(int percent)
{
#if CONFIG_S2_DISPLAY_BL_GPIO >= 0
    gpio_set_level(CONFIG_S2_DISPLAY_BL_GPIO, percent > 0 ? 1 : 0);
#else
    (void)percent;
#endif
    return ESP_OK;
}
#endif

esp_err_t panel_init(void)
{
    ESP_RETURN_ON_FALSE(!s_panel, ESP_ERR_INVALID_STATE, TAG, "already initialised");

    s_flush_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_flush_done, ESP_ERR_NO_MEM, TAG, "no mem for flush semaphore");

    s_fb = heap_caps_malloc((size_t)PANEL_W * PANEL_H * 2u, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    ESP_RETURN_ON_FALSE(s_fb, ESP_ERR_NO_MEM, TAG, "no mem for the %d x %d framebuffer (%u bytes)",
                        PANEL_W, PANEL_H, (unsigned)((size_t)PANEL_W * PANEL_H * 2u));
    gfx_init(&s_gfx, s_fb, PANEL_W, PANEL_H);
    gfx_fill(&s_gfx, GFX_BLACK);

    ESP_RETURN_ON_ERROR(backlight_init(), TAG, "backlight");

    spi_bus_config_t bus = {
        .sclk_io_num = CONFIG_S2_DISPLAY_SCLK_GPIO,
        .mosi_io_num = CONFIG_S2_DISPLAY_MOSI_GPIO,
        .miso_io_num = -1,                     /* write-only panel */
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = MAX_TRANSFER_SZ,
        /* Keep SPI completion interrupts off core 1, where the CAN decoder runs. */
        .isr_cpu_id = ESP_INTR_CPU_AFFINITY_0,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(DISP_SPI_HOST, &bus, SPI_DMA_CH_AUTO), TAG, "spi bus");

    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = CONFIG_S2_DISPLAY_CS_GPIO,
        .dc_gpio_num = CONFIG_S2_DISPLAY_DC_GPIO,
        .spi_mode = 0,
        .pclk_hz = CONFIG_S2_DISPLAY_SPI_HZ,
        .trans_queue_depth = 6,
        .on_color_trans_done = on_color_trans_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)DISP_SPI_HOST, &io_cfg, &s_io),
                        TAG, "panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {
        /* ESP-IDF 6 narrowed this from int to gpio_num_t; the Kconfig value is an
         * int, and -1 for "no reset pin" is GPIO_NUM_NC. */
        .reset_gpio_num = (gpio_num_t)CONFIG_S2_DISPLAY_RST_GPIO,
#if CONFIG_S2_DISPLAY_BGR
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR,
#else
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
#endif
        .bits_per_pixel = 16,
        /*
         * data_endian left at its default: the driver then programs RAMCTRL for
         * big-endian pixels, which is what the byte-swapped framebuffer emits
         * (see gfx.h). Asking for little-endian here garbles the colours.
         */
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(s_io, &panel_cfg, &s_panel), TAG, "st7789 panel");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel), TAG, "panel init");
#if CONFIG_S2_DISPLAY_INVERT_COLOR
    /* ST7789 IPS glass ships needing inversion; esp_lcd_panel_init does not send it. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_panel, true), TAG, "invert colour");
#endif
    /* A 180 degree turn is both mirrors; the extra options XOR on top of it. */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_panel, OPT_ROT180 != OPT_MIRROR_X, OPT_ROT180 != OPT_MIRROR_Y),
                        TAG, "mirror");
    /*
     * The 280 visible rows sit in the MIDDLE of the controller's 320-row memory
     * (rows 20..299), so the offset is 20 at 0 and at 180 degrees alike - unlike
     * the end-justified 240x240 panels, where it moves between 80 and 0. The gap
     * is applied in application coordinates, after the MADCTL transform.
     */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, CONFIG_S2_DISPLAY_X_GAP, CONFIG_S2_DISPLAY_Y_GAP),
                        TAG, "set gap");
    /*
     * Clear the panel's power-on RAM before switching it on: with the backlight
     * hardwired (BL = -1) there is no other way to stop the garbage being seen.
     */
    ESP_RETURN_ON_ERROR(panel_flush(), TAG, "initial clear");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), TAG, "display on");

    ESP_LOGI(TAG, "ST7789 %dx%d up: SCLK%d MOSI%d CS%d DC%d RST%d BL%d, %d MHz, gap %d/%d, %d-row bands",
             PANEL_W, PANEL_H, CONFIG_S2_DISPLAY_SCLK_GPIO, CONFIG_S2_DISPLAY_MOSI_GPIO,
             CONFIG_S2_DISPLAY_CS_GPIO, CONFIG_S2_DISPLAY_DC_GPIO, CONFIG_S2_DISPLAY_RST_GPIO,
             CONFIG_S2_DISPLAY_BL_GPIO, CONFIG_S2_DISPLAY_SPI_HZ / 1000000,
             CONFIG_S2_DISPLAY_X_GAP, CONFIG_S2_DISPLAY_Y_GAP, BAND_ROWS);
    return ESP_OK;
}

gfx_t *panel_gfx(void)
{
    return &s_gfx;
}

esp_err_t panel_flush(void)
{
    if (!s_panel) {
        return ESP_ERR_INVALID_STATE;
    }
    /*
     * Drop any completion left over from a previous flush that timed out or
     * failed mid-way. Without this a stale give would satisfy the first band's
     * wait below, and the renderer would be free to overwrite the framebuffer
     * while DMA was still reading it.
     */
    while (xSemaphoreTake(s_flush_done, 0) == pdTRUE) {
    }
    for (int y = 0; y < PANEL_H; y += BAND_ROWS) {
        int rows = PANEL_H - y < BAND_ROWS ? PANEL_H - y : BAND_ROWS;
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, PANEL_W, y + rows,
                                                  &s_fb[(size_t)y * PANEL_W]);
        if (err != ESP_OK) {
            return err;
        }
        /*
         * Wait for this band's DMA before touching the framebuffer again. The
         * transfer reads straight out of the framebuffer, so overlapping a
         * re-render with it would tear.
         */
        if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(FLUSH_WAIT_MS)) != pdTRUE) {
            ESP_LOGW(TAG, "flush timed out at row %d after %u ms", y, (unsigned)FLUSH_WAIT_MS);
            /*
             * The transfer cannot be cancelled and still owns the framebuffer,
             * so give it a bounded grace period rather than letting the renderer
             * race it. The drain at the top of the next flush covers the case
             * where it completes even later than this.
             */
            if (xSemaphoreTake(s_flush_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
                ESP_LOGE(TAG, "band at row %d still in flight; the display bus looks stuck", y);
            }
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

#endif /* CONFIG_S2_DISPLAY_ENABLE */
