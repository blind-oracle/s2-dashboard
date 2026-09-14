/*
 * status_led.h - WS2812 RGB status LED (GPIO21 on the ESP32-S3-Zero).
 *
 *   off            : LED disabled or not initialised
 *   dim blue       : booted, no CAN traffic yet
 *   green blink    : frames flowing (blink rate follows traffic)
 *   yellow         : frames flowing but CRC failures / counter gaps
 *   red            : bus-off or error-passive
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_STATE_BOOT,
    LED_STATE_IDLE,        /* no frames in the last second */
    LED_STATE_RX_OK,
    LED_STATE_RX_DEGRADED,
    LED_STATE_BUS_ERROR,
} led_state_t;

esp_err_t status_led_init(void);
void status_led_set_state(led_state_t state);
/* Call at ~10 Hz from a low-priority task to animate. */
void status_led_tick(void);

#ifdef __cplusplus
}
#endif
