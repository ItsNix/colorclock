#!/usr/bin/env python3
"""
Generate anti-aliased VLW fonts for the clock and write them to ../fonts.h.

LovyanGFX draws VLW ("smooth") fonts with 8-bit alpha, so text stays crisp at
any size, unlike the built-in bitmap fonts which turn blocky when scaled. Each
font is rendered at the exact pixel size it's drawn at on the 800x480 screen.

Time font sizes are derived from the layout: the largest size at which the
widest string for each display format fits TIME_MAX_W, leaving room for an
AM/PM suffix drawn in the 56 px text font.

Usage:  python3 tools/make_fonts.py [path/to/DejaVuSans-Bold.ttf]
Needs Pillow with FreeType.

VLW layout (all integers big-endian int32), as parsed by lgfx::VLWfont:
  header: glyph_count, version(11), font_size, 0, ascent, descent
  per glyph, sorted by code point: unicode, height, width, x_advance, dY, dX, 0
      dY = rows above the baseline, dX = left offset from the cursor
  then each glyph's width*height alpha bitmap, in the same order.
LovyanGFX keeps width / x_advance as uint8 and dX as int8, so those are checked.
It also alloca()s each glyph's whole bitmap on the stack while drawing, so the
largest glyph is exported as FONTS_MAX_GLYPH_BYTES to size the loop task stack.
"""
import os
import struct
import sys

from PIL import Image, ImageDraw, ImageFont

TTF = sys.argv[1] if len(sys.argv) > 1 else "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "fonts.h")

ASCII = "".join(chr(c) for c in range(0x20, 0x7F))
TIME_CHARS = " 0123456789:"

TIME_MAX_W = 760          # usable width for the time line (20 px margins)
TIME_MAX_DIGIT_H = 200    # tallest digits that fit between hex code and date
SUFFIX_GAP = 16           # space between digits and AM/PM
SUFFIX_FONT_SIZE = 56


def digit_advance(font):
    return max(font.getlength(d) for d in "0123456789")


def time_width(font, pattern):
    """Width of a time pattern like '88:88' with tabular digits."""
    adv = digit_advance(font)
    return sum(adv if ch == "8" else font.getlength(ch) for ch in pattern)


def fit_time_size(pattern, suffix_w):
    size = 400
    while size > 20:
        font = ImageFont.truetype(TTF, size)
        digit_h = -font.getbbox("8", anchor="ls")[1]
        if time_width(font, pattern) + suffix_w <= TIME_MAX_W and digit_h <= TIME_MAX_DIGIT_H:
            return size
        size -= 1
    raise SystemExit(f"no size fits {pattern!r}")


def make_vlw(size, chars, tabular_digits=False):
    font = ImageFont.truetype(TTF, size)
    ascent, descent = font.getmetrics()
    tab_adv = round(digit_advance(font))
    headers, bitmaps, max_glyph = [], [], 0
    for ch in sorted(set(chars), key=ord):
        adv = round(font.getlength(ch))
        if ch == " ":
            w = h = dy = dx = 0
            bitmap = b""
        else:
            x0, y0, x1, y1 = font.getbbox(ch, anchor="ls")
            w, h = x1 - x0, y1 - y0
            img = Image.new("L", (w, h), 0)
            ImageDraw.Draw(img).text((-x0, -y0), ch, font=font, fill=255, anchor="ls")
            bitmap = img.tobytes()
            dy, dx = -y0, x0
        if tabular_digits and ch.isdigit():
            dx += (tab_adv - adv) // 2   # center the glyph in a fixed-width cell
            adv = tab_adv
        if not (0 <= w <= 255 and 0 <= adv <= 255 and -128 <= dx <= 127):
            raise SystemExit(f"glyph {ch!r} at {size}px exceeds VLW limits (w={w} adv={adv} dx={dx})")
        max_glyph = max(max_glyph, w * h)
        headers.append(struct.pack(">7i", ord(ch), h, w, adv, dy, dx, 0))
        bitmaps.append(bitmap)
    head = struct.pack(">6i", len(headers), 11, size, 0, ascent, descent)
    return head + b"".join(headers) + b"".join(bitmaps), max_glyph


def c_array(name, data, comment):
    lines = [f"// {comment} — {len(data)} bytes", f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 32):
        lines.append("  " + ",".join(str(b) for b in data[i:i + 32]) + ",")
    lines.append("};")
    return "\n".join(lines)


def main():
    suffix_font = ImageFont.truetype(TTF, SUFFIX_FONT_SIZE)
    suffix_w = max(suffix_font.getlength(s) for s in ("AM", "PM")) + SUFFIX_GAP

    # Time sizes: L = no seconds (12h worst case, also used for 24h),
    # M = 24h with seconds, S = 12h with seconds (needs room for AM/PM).
    time_l = fit_time_size("88:88", suffix_w)
    time_m = fit_time_size("88:88:88", 0)
    time_s = fit_time_size("88:88:88", suffix_w)

    fonts = [
        ("FONT_TEXT_24", 24, ASCII, False, "DejaVu Sans Bold 24px, ASCII"),
        ("FONT_TEXT_40", 40, ASCII, False, "DejaVu Sans Bold 40px, ASCII"),
        ("FONT_TEXT_56", 56, ASCII, False, "DejaVu Sans Bold 56px, ASCII"),
        ("FONT_TIME_L", time_l, TIME_CHARS, True, f"DejaVu Sans Bold {time_l}px, time digits (no seconds)"),
        ("FONT_TIME_M", time_m, TIME_CHARS, True, f"DejaVu Sans Bold {time_m}px, time digits (24h + seconds)"),
        ("FONT_TIME_S", time_s, TIME_CHARS, True, f"DejaVu Sans Bold {time_s}px, time digits (12h + seconds)"),
    ]

    parts = [
        "// Generated by tools/make_fonts.py — do not edit by hand.",
        f"// Source: {os.path.basename(TTF)} (DejaVu fonts license, Bitstream Vera derivative)",
        "#pragma once",
        "#include <stdint.h>",
        "",
    ]
    total = max_glyph = 0
    body = []
    for name, size, chars, tabular, comment in fonts:
        data, glyph = make_vlw(size, chars, tabular)
        total += len(data)
        max_glyph = max(max_glyph, glyph)
        body.append(c_array(name, data, comment))
        body.append("")
        print(f"{name:13s} {size:3d}px  {len(data):7d} bytes")
    parts.append("// Largest glyph bitmap; LovyanGFX alloca()s one while drawing it.")
    parts.append(f"#define FONTS_MAX_GLYPH_BYTES {max_glyph}")
    parts.append("")
    parts.extend(body)
    print(f"total {total} bytes, largest glyph {max_glyph} bytes -> {os.path.normpath(OUT)}")
    with open(OUT, "w") as f:
        f.write("\n".join(parts))


if __name__ == "__main__":
    main()
