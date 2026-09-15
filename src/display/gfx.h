/*
 * gfx.h - minimal RGB565 software renderer for the 240x280 panel.
 *
 * Pure C (math.h only), so the primitives are covered by the host unit tests.
 * All primitives clip to the framebuffer; nothing here knows about SPI.
 *
 * Angles are in degrees measured CLOCKWISE FROM 12 O'CLOCK, which is the
 * natural convention for a gauge: 0 = top, +90 = right, -90 = left.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pack 8-8-8 into a natural (host-order) RGB565 word. */
#define GFX_RGB565(r, g, b) ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

/*
 * Pixels are STORED BYTE-SWAPPED so the framebuffer can be handed to SPI DMA
 * with no conversion pass: the ST7789 is left in its big-endian pixel mode, so
 * it latches the most-significant byte of each RGB565 pixel first, while the
 * ESP32 is little-endian. The renderer never interprets a
 * colour value, it only copies it, so swapping once here is enough - as long as
 * every colour comes from GFX_RGB() (or from another value produced by it).
 *
 * If a future panel wants host order instead, this macro is the only change.
 */
#define GFX_RGB(r, g, b) ((uint16_t)((GFX_RGB565(r, g, b) >> 8) | (GFX_RGB565(r, g, b) << 8)))

#define GFX_BLACK   GFX_RGB(0, 0, 0)
#define GFX_WHITE   GFX_RGB(255, 255, 255)

typedef struct {
    uint16_t *fb;
    int w;
    int h;
} gfx_t;

void gfx_init(gfx_t *g, uint16_t *fb, int w, int h);
void gfx_fill(gfx_t *g, uint16_t c);
void gfx_pixel(gfx_t *g, int x, int y, uint16_t c);
uint16_t gfx_get_pixel(const gfx_t *g, int x, int y);   /* 0 outside the framebuffer */
void gfx_hline(gfx_t *g, int x, int y, int w, uint16_t c);
void gfx_vline(gfx_t *g, int x, int y, int h, uint16_t c);
void gfx_fill_rect(gfx_t *g, int x, int y, int w, int h, uint16_t c);
void gfx_rect(gfx_t *g, int x, int y, int w, int h, uint16_t c);
void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, uint16_t c);
void gfx_circle(gfx_t *g, int cx, int cy, int r, uint16_t c);
void gfx_fill_circle(gfx_t *g, int cx, int cy, int r, uint16_t c);

/*
 * Annulus sector from a0 to a1 (degrees clockwise from the top), drawn as
 * radial spokes fine enough that no gaps appear at r_out. a1 may be less than
 * a0; the sector is drawn in the direction of increasing angle from a0 to a1.
 */
void gfx_fill_ring(gfx_t *g, int cx, int cy, int r_in, int r_out, float a0_deg, float a1_deg, uint16_t c);

/* Single radial tick between two radii. */
void gfx_ring_tick(gfx_t *g, int cx, int cy, int r_in, int r_out, float a_deg, int width_px, uint16_t c);

/* Point on a circle, using the gauge angle convention. */
void gfx_polar(int cx, int cy, float r, float a_deg, int *x, int *y);

/* ---- text: 5x7 monospace font, integer scaling, advance 6 px at scale 1 ---- */

int gfx_text_width(const char *s, int scale);
int gfx_text_height(int scale);
int gfx_text(gfx_t *g, int x, int y, const char *s, int scale, uint16_t fg);
int gfx_text_center(gfx_t *g, int cx, int y, const char *s, int scale, uint16_t fg);
int gfx_text_right(gfx_t *g, int x_right, int y, const char *s, int scale, uint16_t fg);

/* ---- seven-segment numerals, for the large readouts ---- */

typedef struct {
    int height;      /* cell height in px */
    int width;       /* cell width in px */
    int thickness;   /* segment thickness */
    int gap;         /* space between cells */
} gfx_seg_style_t;

/* Fill in sensible proportions for the requested digit height. */
void gfx_seg_style(gfx_seg_style_t *st, int height);

/* Accepts '0'..'9', '-', '.', ':', ' '; anything else advances like a space. */
int gfx_seg_width(const char *s, const gfx_seg_style_t *st);
int gfx_seg_text(gfx_t *g, int x, int y, const char *s, const gfx_seg_style_t *st, uint16_t fg);
int gfx_seg_text_center(gfx_t *g, int cx, int y, const char *s, const gfx_seg_style_t *st, uint16_t fg);
int gfx_seg_text_right(gfx_t *g, int x_right, int y, const char *s, const gfx_seg_style_t *st, uint16_t fg);

#ifdef __cplusplus
}
#endif
