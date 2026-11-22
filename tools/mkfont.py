#!/usr/bin/env python3
"""把 TrueType 字形烘成 1bit 点阵 C 头文件。

墨水屏只有黑白红三色、没有抗锯齿，所以字体必须是纯点阵。
只烘固定文案用到的字符（数字、月份缩写、周数），中文标题交给 U8g2 的 GB2312 字体。

用法:
  python3 tools/mkfont.py --name wk_num80 --size 80 --chars 0123456789 \
      --out firmware/src/font_num80.h
"""
import argparse
import os

from PIL import Image, ImageDraw, ImageFont

FONT_DEFAULT = "/System/Library/Fonts/Hiragino Sans GB.ttc"


def render_glyph(font, ch, size, stroke=0):
    """返回 (bitmap_rows, w, h, xoff, yoff, advance)。yoff 相对基线，向上为负。"""
    pad = size
    img = Image.new("L", (size * 3, size * 3), 255)
    d = ImageDraw.Draw(img)
    d.text((pad, pad), ch, font=font, fill=0, anchor="ls",
           stroke_width=stroke, stroke_fill=0)
    ink = img.point(lambda p: 255 if p < 128 else 0).getbbox()
    adv = int(round(font.getlength(ch)))
    if ink is None:
        return [], 0, 0, 0, 0, adv
    l, t, r, b = ink
    w, h = r - l, b - t
    rows = []
    px = img.load()
    for y in range(t, b):
        bits = bytearray((w + 7) // 8)
        for x in range(l, r):
            if px[x, y] < 128:
                bits[(x - l) >> 3] |= 0x80 >> ((x - l) & 7)
        rows.extend(bits)
    return rows, w, h, l - pad, t - pad, adv


def emit(name, size, chars, rows_all, glyphs):
    lines = [
        "#pragma once",
        "#include <stdint.h>",
        '#include "wkfont.h"',
        "",
        "// clang-format off",
        "static const uint8_t %s_bits[] PROGMEM = {" % name,
    ]
    for i in range(0, len(rows_all), 16):
        lines.append("  " + ",".join("0x%02X" % b for b in rows_all[i:i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("static const WkGlyph %s_glyphs[] PROGMEM = {" % name)
    for cp, w, h, xoff, yoff, adv, off in glyphs:
        lines.append("  {0x%04X, %d, %d, %d, %d, %d, %d}," % (cp, w, h, xoff, yoff, adv, off))
    lines.append("};")
    lines.append("")
    lines.append("static const WkFont %s = { %s_glyphs, %s_bits, %d, %d };"
                 % (name, name, name, len(glyphs), size))
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--name", required=True)
    ap.add_argument("--size", type=int, required=True)
    ap.add_argument("--chars", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--font", default=FONT_DEFAULT)
    ap.add_argument("--index", type=int, default=1)
    ap.add_argument("--stroke", type=int, default=0,
                    help="合成加粗的描边像素数，烘进点阵后是纯像素加粗，不会糊")
    args = ap.parse_args()

    font = ImageFont.truetype(args.font, args.size, index=args.index)
    rows_all, glyphs = [], []
    for ch in args.chars:
        rows, w, h, xoff, yoff, adv = render_glyph(font, ch, args.size, args.stroke)
        glyphs.append((ord(ch), w, h, xoff, yoff, adv, len(rows_all)))
        rows_all.extend(rows)

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w") as f:
        f.write(emit(args.name, args.size, args.chars, rows_all, glyphs))
    print("%-12s %2d glyphs  %5d bytes  -> %s" % (args.name, len(glyphs), len(rows_all), args.out))


if __name__ == "__main__":
    main()
