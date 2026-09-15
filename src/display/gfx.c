#include "gfx.h"

#include <math.h>
#include <string.h>

#include "s2_font5x7.h"

#define DEG2RAD (3.14159265358979323846f / 180.0f)

void gfx_init(gfx_t *g, uint16_t *fb, int w, int h)
{
    g->fb = fb;
    g->w = w;
    g->h = h;
}

void gfx_fill(gfx_t *g, uint16_t c)
{
    size_t n = (size_t)g->w * (size_t)g->h;
    for (size_t i = 0; i < n; i++) {
        g->fb[i] = c;
    }
}

void gfx_pixel(gfx_t *g, int x, int y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= g->w || y >= g->h) {
        return;
    }
    g->fb[(size_t)y * (size_t)g->w + (size_t)x] = c;
}

uint16_t gfx_get_pixel(const gfx_t *g, int x, int y)
{
    if (x < 0 || y < 0 || x >= g->w || y >= g->h) {
        return 0;
    }
    return g->fb[(size_t)y * (size_t)g->w + (size_t)x];
}

void gfx_hline(gfx_t *g, int x, int y, int w, uint16_t c)
{
    if (y < 0 || y >= g->h || w <= 0) {
        return;
    }
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (x + w > g->w) {
        w = g->w - x;
    }
    if (w <= 0) {
        return;
    }
    uint16_t *row = &g->fb[(size_t)y * (size_t)g->w + (size_t)x];
    for (int i = 0; i < w; i++) {
        row[i] = c;
    }
}

void gfx_vline(gfx_t *g, int x, int y, int h, uint16_t c)
{
    if (x < 0 || x >= g->w || h <= 0) {
        return;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (y + h > g->h) {
        h = g->h - y;
    }
    for (int i = 0; i < h; i++) {
        g->fb[(size_t)(y + i) * (size_t)g->w + (size_t)x] = c;
    }
}

void gfx_fill_rect(gfx_t *g, int x, int y, int w, int h, uint16_t c)
{
    for (int i = 0; i < h; i++) {
        gfx_hline(g, x, y + i, w, c);
    }
}

void gfx_rect(gfx_t *g, int x, int y, int w, int h, uint16_t c)
{
    if (w <= 0 || h <= 0) {
        return;
    }
    gfx_hline(g, x, y, w, c);
    gfx_hline(g, x, y + h - 1, w, c);
    gfx_vline(g, x, y, h, c);
    gfx_vline(g, x + w - 1, y, h, c);
}

void gfx_line(gfx_t *g, int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        gfx_pixel(g, x0, y0, c);
        if (x0 == x1 && y0 == y1) {
            return;
        }
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void gfx_circle(gfx_t *g, int cx, int cy, int r, uint16_t c)
{
    if (r < 0) {
        return;
    }
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(g, cx + x, cy + y, c);
        gfx_pixel(g, cx + y, cy + x, c);
        gfx_pixel(g, cx - y, cy + x, c);
        gfx_pixel(g, cx - x, cy + y, c);
        gfx_pixel(g, cx - x, cy - y, c);
        gfx_pixel(g, cx - y, cy - x, c);
        gfx_pixel(g, cx + y, cy - x, c);
        gfx_pixel(g, cx + x, cy - y, c);
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
}

void gfx_fill_circle(gfx_t *g, int cx, int cy, int r, uint16_t c)
{
    if (r < 0) {
        return;
    }
    for (int dy = -r; dy <= r; dy++) {
        /* Truncate: rounding up makes the disc a pixel wider than its radius. */
        int dx = (int)sqrtf((float)(r * r - dy * dy));
        gfx_hline(g, cx - dx, cy + dy, 2 * dx + 1, c);
    }
}

void gfx_polar(int cx, int cy, float r, float a_deg, int *x, int *y)
{
    float a = a_deg * DEG2RAD;
    *x = cx + (int)lroundf(r * sinf(a));
    *y = cy - (int)lroundf(r * cosf(a));
}

/* One radial spoke from r_in to r_out at a fixed angle. */
static void spoke(gfx_t *g, int cx, int cy, int r_in, int r_out, float a_deg, uint16_t c)
{
    float a = a_deg * DEG2RAD;
    float s = sinf(a), k = cosf(a);
    for (int r = r_in; r <= r_out; r++) {
        int x = cx + (int)lroundf((float)r * s);
        int y = cy - (int)lroundf((float)r * k);
        gfx_pixel(g, x, y, c);
    }
}

void gfx_fill_ring(gfx_t *g, int cx, int cy, int r_in, int r_out, float a0_deg, float a1_deg, uint16_t c)
{
    if (r_out < r_in) {
        int t = r_in;
        r_in = r_out;
        r_out = t;
    }
    if (r_out <= 0) {
        return;
    }
    if (r_in < 0) {
        r_in = 0;
    }
    /*
     * Step small enough that consecutive spokes touch at r_out: the arc length
     * per step is r_out * step_rad, so step_deg <= 57.3 / r_out. Halve it for
     * margin (overdraw is harmless).
     */
    float step = 28.6f / (float)r_out;
    if (step > 1.0f) {
        step = 1.0f;
    }
    if (step < 0.02f) {
        step = 0.02f;
    }
    float span = a1_deg - a0_deg;
    if (span < 0.0f) {
        step = -step;
    }
    int steps = (int)(span / step);
    if (steps < 0) {
        steps = 0;
    }
    for (int i = 0; i <= steps; i++) {
        spoke(g, cx, cy, r_in, r_out, a0_deg + step * (float)i, c);
    }
    spoke(g, cx, cy, r_in, r_out, a1_deg, c);   /* exact end, whatever the rounding did */
}

void gfx_ring_tick(gfx_t *g, int cx, int cy, int r_in, int r_out, float a_deg, int width_px, uint16_t c)
{
    if (width_px < 1) {
        width_px = 1;
    }
    /* Widen by sweeping a few spokes either side of the target angle. */
    float half_deg = (float)(width_px - 1) * 0.5f * 57.3f / (float)(r_out > 0 ? r_out : 1);
    gfx_fill_ring(g, cx, cy, r_in, r_out, a_deg - half_deg, a_deg + half_deg, c);
}

/* ------------------------------------------------------------------ text --- */

int gfx_text_height(int scale)
{
    return s2_font5x7.height * (scale < 1 ? 1 : scale);
}

int gfx_text_width(const char *s, int scale)
{
    if (scale < 1) {
        scale = 1;
    }
    size_t n = strlen(s);
    if (n == 0) {
        return 0;
    }
    /* advance per glyph, minus the trailing inter-glyph gap */
    return (int)n * s2_font5x7.advance * scale - (s2_font5x7.advance - s2_font5x7.width) * scale;
}

int gfx_text(gfx_t *g, int x, int y, const char *s, int scale, uint16_t fg)
{
    if (scale < 1) {
        scale = 1;
    }
    int x0 = x;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char ch = *p;
        if (ch < s2_font5x7.first || ch > s2_font5x7.last) {
            ch = '?';
        }
        const uint8_t *glyph = &s2_font5x7.data[(size_t)(ch - s2_font5x7.first) * s2_font5x7.width];
        for (int col = 0; col < s2_font5x7.width; col++) {
            uint8_t bits = glyph[col];
            for (int row = 0; row < s2_font5x7.height; row++) {
                if (bits & (1u << row)) {
                    if (scale == 1) {
                        gfx_pixel(g, x + col, y + row, fg);
                    } else {
                        gfx_fill_rect(g, x + col * scale, y + row * scale, scale, scale, fg);
                    }
                }
            }
        }
        x += s2_font5x7.advance * scale;
    }
    return x - x0;
}

int gfx_text_center(gfx_t *g, int cx, int y, const char *s, int scale, uint16_t fg)
{
    return gfx_text(g, cx - gfx_text_width(s, scale) / 2, y, s, scale, fg);
}

int gfx_text_right(gfx_t *g, int x_right, int y, const char *s, int scale, uint16_t fg)
{
    return gfx_text(g, x_right - gfx_text_width(s, scale), y, s, scale, fg);
}

/* --------------------------------------------------- seven-segment digits --- */

void gfx_seg_style(gfx_seg_style_t *st, int height)
{
    if (height < 7) {
        height = 7;
    }
    /* Proportions chosen so gfx_seg_style(&st, 52) reproduces the hero style. */
    st->height = height;
    st->width = (height * 11) / 20;          /* ~0.55 aspect, like a classic LCD digit */
    st->thickness = (height * 2 + 7) / 15;
    st->gap = height / 10 > 0 ? height / 10 : 1;
}

/* Per-character cell width, excluding the inter-cell gap. */
static int seg_cell_width(char ch, const gfx_seg_style_t *st)
{
    switch (ch) {
    case '.':
    case ':':
        return st->thickness + 2;
    case '-':
        return (st->width * 4) / 7;
    case ' ':
        return st->width / 2;
    default:
        return st->width;
    }
}

int gfx_seg_width(const char *s, const gfx_seg_style_t *st)
{
    int w = 0;
    for (const char *p = s; *p; p++) {
        w += seg_cell_width(*p, st);
        if (p[1]) {
            w += st->gap;
        }
    }
    return w;
}

/*
 * Segment order: a top, b top-right, c bottom-right, d bottom,
 * e bottom-left, f top-left, g middle.
 */
static const uint8_t seg_digits[10] = {
    /* 0 */ 0x3F, /* 1 */ 0x06, /* 2 */ 0x5B, /* 3 */ 0x4F, /* 4 */ 0x66,
    /* 5 */ 0x6D, /* 6 */ 0x7D, /* 7 */ 0x07, /* 8 */ 0x7F, /* 9 */ 0x6F,
};

/* Horizontal bar with bevelled ends, so adjacent segments meet cleanly. */
static void seg_h(gfx_t *g, int x, int y, int w, int t, uint16_t c)
{
    for (int i = 0; i < t; i++) {
        int off = (i < (t + 1) / 2) ? i : (t - 1 - i);
        gfx_hline(g, x + off, y + i, w - 2 * off, c);
    }
}

static void seg_v(gfx_t *g, int x, int y, int h, int t, uint16_t c)
{
    for (int i = 0; i < t; i++) {
        int off = (i < (t + 1) / 2) ? i : (t - 1 - i);
        gfx_vline(g, x + i, y + off, h - 2 * off, c);
    }
}

static void draw_digit(gfx_t *g, int x, int y, uint8_t mask, const gfx_seg_style_t *st, uint16_t c)
{
    int w = st->width, h = st->height, t = st->thickness;
    int half = (h - t) / 2;
    if (mask & 0x01) { seg_h(g, x, y, w, t, c); }                            /* a */
    if (mask & 0x02) { seg_v(g, x + w - t, y, half + t, t, c); }             /* b */
    if (mask & 0x04) { seg_v(g, x + w - t, y + half, h - half, t, c); }      /* c */
    if (mask & 0x08) { seg_h(g, x, y + h - t, w, t, c); }                    /* d */
    if (mask & 0x10) { seg_v(g, x, y + half, h - half, t, c); }              /* e */
    if (mask & 0x20) { seg_v(g, x, y, half + t, t, c); }                     /* f */
    if (mask & 0x40) { seg_h(g, x, y + half, w, t, c); }                     /* g */
}

int gfx_seg_text(gfx_t *g, int x, int y, const char *s, const gfx_seg_style_t *st, uint16_t fg)
{
    int x0 = x;
    for (const char *p = s; *p; p++) {
        char ch = *p;
        int cw = seg_cell_width(ch, st);
        if (ch >= '0' && ch <= '9') {
            draw_digit(g, x, y, seg_digits[ch - '0'], st, fg);
        } else if (ch == '-') {
            seg_h(g, x, y + (st->height - st->thickness) / 2, cw, st->thickness, fg);
        } else if (ch == '.') {
            /* A square dot centred in the cell, not the whole cell filled. */
            gfx_fill_rect(g, x + (cw - st->thickness) / 2, y + st->height - st->thickness,
                          st->thickness, st->thickness, fg);
        } else if (ch == ':') {
            int q = st->height / 4;
            int ox = x + (cw - st->thickness) / 2;
            gfx_fill_rect(g, ox, y + q, st->thickness, st->thickness, fg);
            gfx_fill_rect(g, ox, y + st->height - q - st->thickness, st->thickness, st->thickness, fg);
        }
        x += cw;
        if (p[1]) {
            x += st->gap;
        }
    }
    return x - x0;
}

int gfx_seg_text_center(gfx_t *g, int cx, int y, const char *s, const gfx_seg_style_t *st, uint16_t fg)
{
    return gfx_seg_text(g, cx - gfx_seg_width(s, st) / 2, y, s, st, fg);
}

int gfx_seg_text_right(gfx_t *g, int x_right, int y, const char *s, const gfx_seg_style_t *st, uint16_t fg)
{
    return gfx_seg_text(g, x_right - gfx_seg_width(s, st), y, s, st, fg);
}
