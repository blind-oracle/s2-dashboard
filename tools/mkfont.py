#!/usr/bin/env python3
"""
mkfont.py - generate the 5x7 bitmap font tables for the dashboard display.

Input : the hand-authored ASCII-art alphabet in the ART block below.  There is
        no external font file and no third-party glyph data: every glyph in
        this file was drawn pixel by pixel for this project, so the firmware
        carries no font licence obligations.
Output: src/gen/s2_font5x7.h  (s2_font_t type, the s2_font5x7 descriptor, the
        geometry macros and a static-inline glyph lookup)
        src/gen/s2_font5x7.c  (95 * 5 column bytes, one line per glyph)

Cell geometry (7 rows, row 0 = top):
        row 0 .. 5   cap / ascender band       (capitals and digits are 6 tall)
        row 2 .. 5   x-height band             (lowercase is 4 tall)
        row 5        baseline (the last row an unaccented glyph rests on)
        row 6        the single descender row   (g j p q y , ; _ | $)
Glyphs are 5 columns wide and are blitted with a 6-column advance, so the 6th
column is the inter-character gap and is never stored.

Usage: python3 tools/mkfont.py [--out-dir DIR] [--check]
                               [--preview TEXT ...] [--png PATH]
  --check     exit 1 if the generated files differ from what is on disk
  --preview   print TEXT as ASCII art on stdout (repeatable), generate nothing
  --png       also write a PNG proof sheet of the whole alphabet (needs PIL;
              PIL is only ever a PNG writer here, never a glyph source)
"""
import argparse
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(ROOT, "src", "gen")

FIRST = 0x20
LAST = 0x7E
WIDTH = 5
HEIGHT = 7
ADVANCE = 6
BASELINE = 5

# ---------------------------------------------------------------------------
# The alphabet.  One block per code point: a ':<hex> <label>' line followed by
# exactly HEIGHT rows of exactly WIDTH cells, '#' = pixel on, '.' = off.
# Drawn by hand for this project - do not paste glyph data from other fonts.
# ---------------------------------------------------------------------------
ART = """
:20 space
.....
.....
.....
.....
.....
.....
.....
:21 exclam
..#..
..#..
..#..
..#..
.....
..#..
.....
:22 quotedbl
.#.#.
.#.#.
.....
.....
.....
.....
.....
:23 numbersign
.....
.#.#.
#####
.#.#.
#####
.#.#.
.....
:24 dollar
..#..
.####
#.#..
.###.
..#.#
####.
..#..
:25 percent
##..#
##.#.
..#..
.#...
#..##
...##
.....
:26 ampersand
.##..
#..#.
.##..
#.#..
#..#.
.##.#
.....
:27 quotesingle
..#..
..#..
.....
.....
.....
.....
.....
:28 parenleft
...#.
..#..
.#...
.#...
..#..
...#.
.....
:29 parenright
.#...
..#..
...#.
...#.
..#..
.#...
.....
:2A asterisk
.....
#.#.#
.###.
#.#.#
.....
.....
.....
:2B plus
.....
.....
..#..
#####
..#..
.....
.....
:2C comma
.....
.....
.....
.....
.....
..#..
.#...
:2D hyphen
.....
.....
.....
.###.
.....
.....
.....
:2E period
.....
.....
.....
.....
.....
..#..
.....
:2F slash
....#
...#.
..#..
..#..
.#...
#....
.....
:30 zero
.###.
#..##
#.#.#
##..#
#...#
.###.
.....
:31 one
..#..
.##..
..#..
..#..
..#..
.###.
.....
:32 two
.###.
#...#
...#.
..#..
.#...
#####
.....
:33 three
####.
....#
.###.
....#
....#
####.
.....
:34 four
...#.
..##.
.#.#.
#..#.
#####
...#.
.....
:35 five
#####
#....
####.
....#
....#
####.
.....
:36 six
..##.
.#...
#....
####.
#...#
.###.
.....
:37 seven
#####
....#
...#.
..#..
.#...
.#...
.....
:38 eight
.###.
#...#
.###.
#...#
#...#
.###.
.....
:39 nine
.###.
#...#
.####
....#
...#.
.##..
.....
:3A colon
.....
.....
.....
..#..
.....
..#..
.....
:3B semicolon
.....
.....
.....
..#..
.....
..#..
.#...
:3C less
.....
...#.
..#..
.#...
..#..
...#.
.....
:3D equal
.....
.....
.###.
.....
.###.
.....
.....
:3E greater
.....
.#...
..#..
...#.
..#..
.#...
.....
:3F question
.###.
#...#
....#
..##.
.....
..#..
.....
:40 at
.###.
#...#
#.###
#.#.#
#.###
.###.
.....
:41 A
.###.
#...#
#...#
#####
#...#
#...#
.....
:42 B
####.
#...#
####.
#...#
#...#
####.
.....
:43 C
.####
#....
#....
#....
#....
.####
.....
:44 D
####.
#...#
#...#
#...#
#...#
####.
.....
:45 E
#####
#....
####.
#....
#....
#####
.....
:46 F
#####
#....
####.
#....
#....
#....
.....
:47 G
.####
#....
#....
#..##
#...#
.###.
.....
:48 H
#...#
#...#
#####
#...#
#...#
#...#
.....
:49 I
.###.
..#..
..#..
..#..
..#..
.###.
.....
:4A J
..###
...#.
...#.
...#.
#..#.
.##..
.....
:4B K
#...#
#..#.
##...
#.#..
#..#.
#...#
.....
:4C L
#....
#....
#....
#....
#....
#####
.....
:4D M
#...#
##.##
#.#.#
#...#
#...#
#...#
.....
:4E N
#...#
##..#
#.#.#
#..##
#...#
#...#
.....
:4F O
.###.
#...#
#...#
#...#
#...#
.###.
.....
:50 P
####.
#...#
#...#
####.
#....
#....
.....
:51 Q
.###.
#...#
#...#
#.#.#
.###.
....#
.....
:52 R
####.
#...#
#...#
####.
#..#.
#...#
.....
:53 S
.####
#....
.###.
....#
....#
####.
.....
:54 T
#####
..#..
..#..
..#..
..#..
..#..
.....
:55 U
#...#
#...#
#...#
#...#
#...#
.###.
.....
:56 V
#...#
#...#
#...#
#...#
.#.#.
..#..
.....
:57 W
#...#
#...#
#...#
#.#.#
#.#.#
.#.#.
.....
:58 X
#...#
.#.#.
..#..
..#..
.#.#.
#...#
.....
:59 Y
#...#
#...#
.#.#.
..#..
..#..
..#..
.....
:5A Z
#####
....#
...#.
..#..
.#...
#####
.....
:5B bracketleft
.###.
.#...
.#...
.#...
.#...
.###.
.....
:5C backslash
#....
.#...
..#..
..#..
...#.
....#
.....
:5D bracketright
.###.
...#.
...#.
...#.
...#.
.###.
.....
:5E asciicircum
..#..
.#.#.
#...#
.....
.....
.....
.....
:5F underscore
.....
.....
.....
.....
.....
.....
#####
:60 grave
.#...
..#..
.....
.....
.....
.....
.....
:61 a
.....
.....
####.
#..#.
#..#.
.###.
.....
:62 b
#....
#....
###..
#..#.
#..#.
###..
.....
:63 c
.....
.....
.###.
#....
#....
.###.
.....
:64 d
...#.
...#.
.###.
#..#.
#..#.
.###.
.....
:65 e
.....
.....
.##..
#..#.
###..
.##..
.....
:66 f
..##.
.#...
###..
.#...
.#...
.#...
.....
:67 g
.....
.....
.###.
#..#.
#..#.
.###.
###..
:68 h
#....
#....
###..
#..#.
#..#.
#..#.
.....
:69 i
.#...
.....
.#...
.#...
.#...
.#...
.....
:6A j
..#..
.....
..#..
..#..
..#..
..#..
##...
:6B k
#....
#....
#..#.
###..
#.#..
#..#.
.....
:6C l
##...
.#...
.#...
.#...
.#...
.###.
.....
:6D m
.....
.....
#####
#.#.#
#.#.#
#.#.#
.....
:6E n
.....
.....
###..
#..#.
#..#.
#..#.
.....
:6F o
.....
.....
.##..
#..#.
#..#.
.##..
.....
:70 p
.....
.....
###..
#..#.
#..#.
###..
#....
:71 q
.....
.....
.###.
#..#.
#..#.
.###.
...#.
:72 r
.....
.....
#.##.
##...
#....
#....
.....
:73 s
.....
.....
.###.
##...
..##.
###..
.....
:74 t
.#...
.#...
####.
.#...
.#...
..##.
.....
:75 u
.....
.....
#..#.
#..#.
#..#.
.###.
.....
:76 v
.....
.....
#...#
#...#
.#.#.
..#..
.....
:77 w
.....
.....
#...#
#.#.#
#.#.#
.#.#.
.....
:78 x
.....
.....
#..#.
.##..
.##..
#..#.
.....
:79 y
.....
.....
#..#.
#..#.
#..#.
.###.
...#.
:7A z
.....
.....
####.
..#..
.#...
####.
.....
:7B braceleft
..###
..#..
.#...
.#...
..#..
..###
.....
:7C bar
..#..
..#..
..#..
..#..
..#..
..#..
..#..
:7D braceright
###..
..#..
...#.
...#.
..#..
###..
.....
:7E asciitilde
.....
.....
.#...
#.#.#
...#.
.....
.....
"""


def parse_art(text=ART):
    """ASCII-art blocks -> {code point: [WIDTH-char row, ...] * HEIGHT}."""
    glyphs = {}
    labels = {}
    code = None
    rows = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip()
        if not line:
            continue
        if line.startswith(":"):
            if code is not None and len(rows) != HEIGHT:
                sys.exit("ART:%d: glyph 0x%02X has %d rows, want %d" % (lineno, code, len(rows), HEIGHT))
            parts = line[1:].split(None, 1)
            try:
                code = int(parts[0], 16)
            except ValueError:
                sys.exit("ART:%d: bad code point %r" % (lineno, line))
            if code in glyphs:
                sys.exit("ART:%d: duplicate glyph 0x%02X" % (lineno, code))
            if code < FIRST or code > LAST:
                sys.exit("ART:%d: code point 0x%02X outside 0x%02X..0x%02X" % (lineno, code, FIRST, LAST))
            labels[code] = parts[1] if len(parts) > 1 else ""
            rows = []
            glyphs[code] = rows
            continue
        if code is None:
            sys.exit("ART:%d: pixel row before any ':<hex>' header" % lineno)
        if len(line) != WIDTH or any(c not in ".#" for c in line):
            sys.exit("ART:%d: row %r is not %d cells of '.' or '#'" % (lineno, line, WIDTH))
        if len(rows) == HEIGHT:
            sys.exit("ART:%d: glyph 0x%02X has more than %d rows" % (lineno, code, HEIGHT))
        rows.append(line)
    if code is not None and len(rows) != HEIGHT:
        sys.exit("ART: glyph 0x%02X has %d rows, want %d" % (code, len(rows), HEIGHT))
    missing = [c for c in range(FIRST, LAST + 1) if c not in glyphs]
    if missing:
        sys.exit("ART: missing glyphs: " + " ".join("0x%02X" % c for c in missing))
    return glyphs, labels


def encode(rows):
    """7 art rows -> WIDTH column bytes, bit 0 = top row, bit 6 = descender row."""
    cols = []
    for x in range(WIDTH):
        byte = 0
        for y in range(HEIGHT):
            if rows[y][x] == "#":
                byte |= 1 << y
        assert byte < 0x80, "bit 7 must stay clear"
        cols.append(byte)
    return cols


def c_char_comment(code):
    """The glyph's own character, safe to drop inside a /* */ comment."""
    ch = chr(code)
    if ch in ("\\",):
        return "'\\\\'"
    return "'%s'" % ch


def generate(glyphs, labels):
    rel = os.path.relpath(os.path.abspath(__file__), ROOT)
    count = LAST - FIRST + 1

    h = []
    h.append("/* Generated by %s - DO NOT EDIT.  Glyph artwork is original to this project. */" % rel)
    h.append("#ifndef S2_GEN_S2_FONT5X7_H")
    h.append("#define S2_GEN_S2_FONT5X7_H")
    h.append("")
    h.append("#include <stdint.h>")
    h.append("")
    h.append("#define S2_FONT5X7_FIRST 0x%02Xu       /* space */" % FIRST)
    h.append("#define S2_FONT5X7_LAST 0x%02Xu        /* tilde */" % LAST)
    h.append("#define S2_FONT5X7_COUNT %du" % count)
    h.append("#define S2_FONT5X7_WIDTH %du         /* stored column bytes per glyph */" % WIDTH)
    h.append("#define S2_FONT5X7_HEIGHT %du        /* rows per column byte, bit 0 = top */" % HEIGHT)
    h.append("#define S2_FONT5X7_ADVANCE %du       /* pen step: WIDTH + 1 blank gap column */" % ADVANCE)
    h.append("#define S2_FONT5X7_BASELINE %du      /* row the unaccented glyphs sit on */" % BASELINE)
    h.append("")
    h.append("/* Monospace bitmap font, column-major.  glyph[x] holds column x (left to")
    h.append(" * right); bit y of that byte is row y (bit 0 top, bit 6 the descender row),")
    h.append(" * bit 7 always 0.  So the pixel at (x, y) is (glyph[x] >> y) & 1u. */")
    h.append("typedef struct {")
    h.append("    const uint8_t *data;   /* COUNT * WIDTH column bytes, ASCII order from 'first' */")
    h.append("    uint8_t first;         /* first code point present */")
    h.append("    uint8_t last;          /* last code point present */")
    h.append("    uint8_t width;         /* column bytes per glyph */")
    h.append("    uint8_t height;        /* rows used inside each column byte */")
    h.append("    uint8_t advance;       /* pen advance per character, in columns */")
    h.append("} s2_font_t;")
    h.append("")
    h.append("extern const s2_font_t s2_font5x7;")
    h.append("")
    h.append("/* Column bytes of 'c', or of '?' when 'c' is outside the covered range. */")
    h.append("static inline const uint8_t *s2_font5x7_glyph(char c)")
    h.append("{")
    h.append("    unsigned code = (unsigned char)c;")
    h.append("    if (code < S2_FONT5X7_FIRST || code > S2_FONT5X7_LAST) {")
    h.append("        code = (unsigned)'?';")
    h.append("    }")
    h.append("    return &s2_font5x7.data[(code - S2_FONT5X7_FIRST) * S2_FONT5X7_WIDTH];")
    h.append("}")
    h.append("")
    h.append("#endif /* S2_GEN_S2_FONT5X7_H */")
    h.append("")

    c = []
    c.append("/* Generated by %s - DO NOT EDIT.  Glyph artwork is original to this project. */" % rel)
    c.append('#include "s2_font5x7.h"')
    c.append("")
    c.append("/* %d glyphs in ASCII order, 0x%02X..0x%02X, %d column bytes each:" % (count, FIRST, LAST, WIDTH))
    c.append(" * glyph n starts at (n - 0x%02X) * %d.  Within a byte bit 0 is the top row" % (FIRST, WIDTH))
    c.append(" * and bit %d the descender row; bit 7 is unused.  One source line per glyph," % (HEIGHT - 1))
    c.append(" * commented with its code point and character. */")
    c.append("static const uint8_t s2_font5x7_data[%d * %d] = {" % (count, WIDTH))
    for code in range(FIRST, LAST + 1):
        cols = encode(glyphs[code])
        c.append("    %s, /* 0x%02X %-4s %s */" % (
            ", ".join("0x%02X" % b for b in cols), code, c_char_comment(code), labels[code]))
    c.append("};")
    c.append("")
    c.append("const s2_font_t s2_font5x7 = {")
    c.append("    .data = s2_font5x7_data,")
    c.append("    .first = 0x%02Xu," % FIRST)
    c.append("    .last = 0x%02Xu," % LAST)
    c.append("    .width = %du," % WIDTH)
    c.append("    .height = %du," % HEIGHT)
    c.append("    .advance = %du," % ADVANCE)
    c.append("};")
    c.append("")
    return "\n".join(h) + "\n", "\n".join(c) + "\n"


def preview(glyphs, text, on="#", off=" "):
    """Render 'text' as HEIGHT lines of ASCII art (round trip through the bytes)."""
    lines = []
    for y in range(HEIGHT):
        row = []
        for ch in text:
            code = ord(ch)
            if code < FIRST or code > LAST:
                code = ord("?")
            cols = encode(glyphs[code])
            row.append("".join(on if (b >> y) & 1 else off for b in cols))
            row.append(off * (ADVANCE - WIDTH))
        lines.append("".join(row).rstrip() if off == " " else "".join(row))
    return lines


def write_png(glyphs, path, scale=3):
    try:
        from PIL import Image
    except ImportError:
        print("note: PIL not installed - skipping %s (PNG preview is optional)" % path)
        return
    per_row = 16
    codes = list(range(FIRST, LAST + 1))
    rows = (len(codes) + per_row - 1) // per_row
    w, hgt = per_row * ADVANCE, rows * (HEIGHT + 1)
    img = Image.new("1", (w, hgt), 0)
    px = img.load()
    for i, code in enumerate(codes):
        ox, oy = (i % per_row) * ADVANCE, (i // per_row) * (HEIGHT + 1)
        for x, byte in enumerate(encode(glyphs[code])):
            for y in range(HEIGHT):
                if (byte >> y) & 1:
                    px[ox + x, oy + y] = 1
    img.resize((w * scale, hgt * scale), Image.NEAREST).save(path)
    print("wrote %s (%dx%d, scale %d)" % (path, w, hgt, scale))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out-dir", default=DEFAULT_OUT)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--preview", action="append", metavar="TEXT",
                    help="render TEXT as ASCII art instead of generating (repeatable)")
    ap.add_argument("--png", metavar="PATH", help="also write a PNG proof sheet (needs PIL)")
    args = ap.parse_args()

    glyphs, labels = parse_art()

    if args.preview:
        for text in args.preview:
            print("+" + "-" * (len(text) * ADVANCE) + "+")
            print("| " + text)
            print("+" + "-" * (len(text) * ADVANCE) + "+")
            for y, line in enumerate(preview(glyphs, text)):
                mark = "<- baseline" if y == BASELINE else ""
                print("  %s" % line.ljust(len(text) * ADVANCE) + ("  " + mark if mark else ""))
            print()
        return 0

    header, source = generate(glyphs, labels)
    outputs = {
        os.path.join(args.out_dir, "s2_font5x7.h"): header,
        os.path.join(args.out_dir, "s2_font5x7.c"): source,
    }
    if args.check:
        stale = [p for p, content in outputs.items()
                 if not os.path.exists(p) or open(p, encoding="utf-8").read() != content]
        if stale:
            print("stale generated files: " + ", ".join(os.path.relpath(p, ROOT) for p in stale))
            return 1
        print("generated files are up to date")
        return 0
    os.makedirs(args.out_dir, exist_ok=True)
    for path, content in outputs.items():
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
    ink = sum(bin(b).count("1") for c in range(FIRST, LAST + 1) for b in encode(glyphs[c]))
    print("%d glyphs 0x%02X..0x%02X, %dx%d cell, advance %d, %d bytes -> %s"
          % (LAST - FIRST + 1, FIRST, LAST, WIDTH, HEIGHT, ADVANCE,
             (LAST - FIRST + 1) * WIDTH, os.path.relpath(args.out_dir, ROOT)))
    print("%d lit pixels, %.1f%% ink" % (ink, 100.0 * ink / ((LAST - FIRST + 1) * WIDTH * HEIGHT)))
    if args.png:
        write_png(glyphs, args.png)
    return 0


if __name__ == "__main__":
    sys.exit(main())
