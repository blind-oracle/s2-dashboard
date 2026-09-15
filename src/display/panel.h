/*
 * panel.h - SPI bus, ST7789 panel and backlight for the 240x280 display.
 *
 * Owns the framebuffer (RGB565, byte-swapped for the panel; see gfx.h) and
 * pushes it in horizontal bands so a single DMA transfer stays well inside the
 * SPI hardware limit. panel_flush() returns only once the last band has left
 * the wire, so the caller may re-render immediately afterwards.
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "gfx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PANEL_W 240
#define PANEL_H 280

/* Creates the SPI bus, the panel and the framebuffer, and turns the panel on. */
esp_err_t panel_init(void);

/* The framebuffer, wrapped for the renderer. Valid after panel_init(). */
gfx_t *panel_gfx(void);

/* Send the whole framebuffer to the panel. Blocks until the DMA has finished. */
esp_err_t panel_flush(void);

/* 0..100; a no-op unless the backlight is on a PWM-capable pin. */
esp_err_t panel_set_brightness(int percent);

#ifdef __cplusplus
}
#endif
