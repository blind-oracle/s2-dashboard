/*
 * Host-side unit tests for the display renderer primitives (pio test -e native).
 */
#include <math.h>
#include <string.h>
#include <unity.h>

#include "gfx.h"
#include "s2_font5x7.h"

#define W 64
#define H 48

static uint16_t fb[W * H];
static gfx_t g;

static void fresh(void)
{
    memset(fb, 0, sizeof(fb));
    gfx_init(&g, fb, W, H);
}

static unsigned count_colour(uint16_t c)
{
    unsigned n = 0;
    for (int i = 0; i < W * H; i++) {
        if (fb[i] == c) {
            n++;
        }
    }
    return n;
}

void test_rgb565_is_stored_byte_swapped(void)
{
    /* The panel latches the high byte first and the ESP32 is little-endian. */
    TEST_ASSERT_EQUAL_HEX16(0xF800, GFX_RGB565(255, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(0x00F8, GFX_RGB(255, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(0x07E0, GFX_RGB565(0, 255, 0));
    TEST_ASSERT_EQUAL_HEX16(0xE007, GFX_RGB(0, 255, 0));
    TEST_ASSERT_EQUAL_HEX16(0x1F00, GFX_RGB(0, 0, 255));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, GFX_RGB(255, 255, 255));
    TEST_ASSERT_EQUAL_HEX16(0x0000, GFX_RGB(0, 0, 0));
}

void test_pixel_clipping(void)
{
    fresh();
    gfx_pixel(&g, -1, 5, 0xFFFF);
    gfx_pixel(&g, 5, -1, 0xFFFF);
    gfx_pixel(&g, W, 5, 0xFFFF);
    gfx_pixel(&g, 5, H, 0xFFFF);
    TEST_ASSERT_EQUAL_UINT(0, count_colour(0xFFFF));
    gfx_pixel(&g, 0, 0, 0xFFFF);
    gfx_pixel(&g, W - 1, H - 1, 0xFFFF);
    TEST_ASSERT_EQUAL_UINT(2, count_colour(0xFFFF));
    TEST_ASSERT_EQUAL_HEX16(0xFFFF, gfx_get_pixel(&g, 0, 0));
    TEST_ASSERT_EQUAL_HEX16(0, gfx_get_pixel(&g, -1, 0));
}

void test_lines_and_rects_clip(void)
{
    fresh();
    gfx_hline(&g, -10, 3, 200, 0x1234);       /* clipped to the full row */
    TEST_ASSERT_EQUAL_UINT(W, count_colour(0x1234));
    fresh();
    gfx_vline(&g, 7, -5, 500, 0x1234);
    TEST_ASSERT_EQUAL_UINT(H, count_colour(0x1234));
    fresh();
    gfx_fill_rect(&g, 2, 2, 4, 3, 0x1234);
    TEST_ASSERT_EQUAL_UINT(12, count_colour(0x1234));
    fresh();
    gfx_rect(&g, 0, 0, 10, 10, 0x1234);
    TEST_ASSERT_EQUAL_UINT(36, count_colour(0x1234));   /* 4*10 - 4 corners */
}

void test_fill_and_circle(void)
{
    fresh();
    gfx_fill(&g, 0xBEEF);
    TEST_ASSERT_EQUAL_UINT(W * H, count_colour(0xBEEF));

    fresh();
    gfx_fill_circle(&g, 20, 20, 10, 0x1234);
    /* area is about pi*r^2 = 314; the scanline fill is within a few percent */
    unsigned n = count_colour(0x1234);
    TEST_ASSERT_TRUE(n > 290 && n < 350);
    /* the centre and the cardinal edges are inside, one pixel beyond is not */
    TEST_ASSERT_EQUAL_HEX16(0x1234, gfx_get_pixel(&g, 20, 20));
    TEST_ASSERT_EQUAL_HEX16(0x1234, gfx_get_pixel(&g, 30, 20));
    TEST_ASSERT_EQUAL_HEX16(0, gfx_get_pixel(&g, 31, 20));
    TEST_ASSERT_EQUAL_HEX16(0, gfx_get_pixel(&g, 20, 31));
}

void test_polar_angle_convention(void)
{
    int x, y;
    gfx_polar(100, 100, 10, 0.0f, &x, &y);        /* 0 degrees = 12 o'clock */
    TEST_ASSERT_EQUAL_INT(100, x);
    TEST_ASSERT_EQUAL_INT(90, y);
    gfx_polar(100, 100, 10, 90.0f, &x, &y);       /* +90 = 3 o'clock */
    TEST_ASSERT_EQUAL_INT(110, x);
    TEST_ASSERT_EQUAL_INT(100, y);
    gfx_polar(100, 100, 10, 180.0f, &x, &y);      /* 180 = 6 o'clock */
    TEST_ASSERT_EQUAL_INT(100, x);
    TEST_ASSERT_EQUAL_INT(110, y);
    gfx_polar(100, 100, 10, -90.0f, &x, &y);      /* -90 = 9 o'clock */
    TEST_ASSERT_EQUAL_INT(90, x);
    TEST_ASSERT_EQUAL_INT(100, y);
}

void test_ring_sector_covers_only_its_sweep(void)
{
    fresh();
    /* a quarter ring from 12 o'clock to 3 o'clock, radii 15..20 */
    gfx_fill_ring(&g, 32, 24, 15, 20, 0.0f, 90.0f, 0x1234);

    /* inside the sweep at r=17: set */
    int x, y;
    gfx_polar(32, 24, 17, 45.0f, &x, &y);
    TEST_ASSERT_EQUAL_HEX16(0x1234, gfx_get_pixel(&g, x, y));
    gfx_polar(32, 24, 17, 2.0f, &x, &y);
    TEST_ASSERT_EQUAL_HEX16(0x1234, gfx_get_pixel(&g, x, y));
    /* outside the sweep: clear */
    gfx_polar(32, 24, 17, -45.0f, &x, &y);
    TEST_ASSERT_EQUAL_HEX16(0, gfx_get_pixel(&g, x, y));
    gfx_polar(32, 24, 17, 135.0f, &x, &y);
    TEST_ASSERT_EQUAL_HEX16(0, gfx_get_pixel(&g, x, y));
    /* inside the inner radius: clear */
    gfx_polar(32, 24, 10, 45.0f, &x, &y);
    TEST_ASSERT_EQUAL_HEX16(0, gfx_get_pixel(&g, x, y));

    /* a wider sweep must cover strictly more pixels */
    unsigned quarter = count_colour(0x1234);
    fresh();
    gfx_fill_ring(&g, 32, 24, 15, 20, 0.0f, 180.0f, 0x1234);
    TEST_ASSERT_TRUE(count_colour(0x1234) > quarter);
}

void test_ring_is_a_solid_annulus(void)
{
    /*
     * The gauge ring is drawn as radial spokes, so the angular step has to be
     * fine enough to leave no holes. Check the true property: every pixel
     * strictly inside the band and inside the sweep is painted.
     */
    static uint16_t big[240 * 240];
    gfx_t b;
    memset(big, 0, sizeof(big));
    gfx_init(&b, big, 240, 240);
    gfx_fill_ring(&b, 120, 120, 100, 118, -110.0f, 115.0f, 0x1234);

    unsigned checked = 0, holes = 0;
    for (int y = 0; y < 240; y++) {
        for (int x = 0; x < 240; x++) {
            double dx = x - 120, dy = y - 120;
            double r = sqrt(dx * dx + dy * dy);
            if (r < 101.5 || r > 116.5) {
                continue;
            }
            double a = atan2(dx, -dy) * 180.0 / M_PI;   /* gauge angle: 0 = up, clockwise */
            if (a < -108.0 || a > 113.0) {
                continue;
            }
            checked++;
            if (big[y * 240 + x] != 0x1234) {
                holes++;
            }
        }
    }
    TEST_ASSERT_TRUE(checked > 5000);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0, holes, "holes inside the gauge ring");

    /* And the outer edge is continuous within one pixel all the way round. */
    for (float a = -110.0f; a <= 115.0f; a += 0.1f) {
        int x, y;
        gfx_polar(120, 120, 117, a, &x, &y);
        bool found = false;
        for (int oy = -1; oy <= 1 && !found; oy++) {
            for (int ox = -1; ox <= 1 && !found; ox++) {
                if (gfx_get_pixel(&b, x + ox, y + oy) == 0x1234) {
                    found = true;
                }
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(found, "break in the ring's outer edge");
    }
}

void test_font_metrics_and_glyphs(void)
{
    TEST_ASSERT_EQUAL_UINT8(0x20, s2_font5x7.first);
    TEST_ASSERT_EQUAL_UINT8(0x7E, s2_font5x7.last);
    TEST_ASSERT_EQUAL_UINT8(5, s2_font5x7.width);
    TEST_ASSERT_EQUAL_UINT8(7, s2_font5x7.height);
    TEST_ASSERT_EQUAL_UINT8(6, s2_font5x7.advance);

    TEST_ASSERT_EQUAL_INT(7, gfx_text_height(1));
    TEST_ASSERT_EQUAL_INT(14, gfx_text_height(2));
    /* one glyph is 5 wide; n glyphs are 6n-1 */
    TEST_ASSERT_EQUAL_INT(5, gfx_text_width("A", 1));
    TEST_ASSERT_EQUAL_INT(11, gfx_text_width("AB", 1));
    TEST_ASSERT_EQUAL_INT(22, gfx_text_width("AB", 2));
    TEST_ASSERT_EQUAL_INT(0, gfx_text_width("", 1));

    /* a space paints nothing, a glyph paints something */
    fresh();
    gfx_text(&g, 1, 1, " ", 1, 0x1234);
    TEST_ASSERT_EQUAL_UINT(0, count_colour(0x1234));
    gfx_text(&g, 1, 1, "A", 1, 0x1234);
    TEST_ASSERT_TRUE(count_colour(0x1234) > 5);

    /* scale 2 paints exactly four times the pixels of scale 1 */
    fresh();
    gfx_text(&g, 0, 0, "8", 1, 0x1234);
    unsigned one = count_colour(0x1234);
    fresh();
    gfx_text(&g, 0, 0, "8", 2, 0x1234);
    TEST_ASSERT_EQUAL_UINT(one * 4, count_colour(0x1234));
}

void test_text_alignment_helpers(void)
{
    fresh();
    gfx_text_right(&g, 40, 0, "AB", 1, 0x1234);
    /* the last glyph column must land at x=39 */
    bool found = false;
    for (int y = 0; y < 7; y++) {
        if (gfx_get_pixel(&g, 39, y) == 0x1234) {
            found = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found, "right-aligned text does not end at the anchor");

    fresh();
    gfx_text_center(&g, 32, 0, "AB", 1, 0x1234);
    int min_x = W, max_x = -1;
    for (int y = 0; y < 7; y++) {
        for (int x = 0; x < W; x++) {
            if (gfx_get_pixel(&g, x, y) == 0x1234) {
                if (x < min_x) { min_x = x; }
                if (x > max_x) { max_x = x; }
            }
        }
    }
    int mid = (min_x + max_x) / 2;
    TEST_ASSERT_INT_WITHIN(2, 32, mid);
}

void test_seven_segment_digits(void)
{
    gfx_seg_style_t st = { .height = 52, .width = 28, .thickness = 7, .gap = 5 };
    /* the spec's worst-case hero string must be 129 px wide */
    TEST_ASSERT_EQUAL_INT(129, gfx_seg_width("-99.9", &st));
    TEST_ASSERT_EQUAL_INT(28, gfx_seg_width("8", &st));
    TEST_ASSERT_EQUAL_INT(61, gfx_seg_width("88", &st));

    static uint16_t big[240 * 240];
    gfx_t b;
    memset(big, 0, sizeof(big));
    gfx_init(&b, big, 240, 240);
    gfx_seg_text(&b, 10, 10, "8", &st, 0x1234);
    unsigned eight = 0;
    for (int i = 0; i < 240 * 240; i++) {
        if (big[i] == 0x1234) { eight++; }
    }
    memset(big, 0, sizeof(big));
    gfx_seg_text(&b, 10, 10, "1", &st, 0x1234);
    unsigned one = 0;
    for (int i = 0; i < 240 * 240; i++) {
        if (big[i] == 0x1234) { one++; }
    }
    /* '8' lights all seven segments, '1' only two, so it must be much bigger */
    TEST_ASSERT_TRUE(one > 0);
    TEST_ASSERT_TRUE_MESSAGE(eight > one * 2, "'8' should paint far more than '1'");

    /* a minus sign sits on the middle row, nothing near the top or bottom */
    memset(big, 0, sizeof(big));
    gfx_seg_text(&b, 10, 10, "-", &st, 0x1234);
    TEST_ASSERT_EQUAL_HEX16(0, gfx_get_pixel(&b, 18, 11));
    TEST_ASSERT_EQUAL_HEX16(0x1234, gfx_get_pixel(&b, 18, 10 + 52 / 2));
    TEST_ASSERT_EQUAL_HEX16(0, gfx_get_pixel(&b, 18, 10 + 50));
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rgb565_is_stored_byte_swapped);
    RUN_TEST(test_pixel_clipping);
    RUN_TEST(test_lines_and_rects_clip);
    RUN_TEST(test_fill_and_circle);
    RUN_TEST(test_polar_angle_convention);
    RUN_TEST(test_ring_sector_covers_only_its_sweep);
    RUN_TEST(test_ring_is_a_solid_annulus);
    RUN_TEST(test_font_metrics_and_glyphs);
    RUN_TEST(test_text_alignment_helpers);
    RUN_TEST(test_seven_segment_digits);
    return UNITY_END();
}
