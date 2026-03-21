#!/usr/bin/env python3
"""
把 Monaco.ttf 转成内核可直接使用的 8x16 点阵。

这里选择在构建期完成字体栅格化，这样内核侧仍然保持很小的字模表，
但字符外观可以直接来自真正的 TTF 字体。
"""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


FONT_WIDTH = 8
FONT_HEIGHT = 16


def _pick_font_size(ttf_path: Path) -> int:
    sample = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789@#%&()_+-=[]{}<>?/\\|!$"
    best = 8
    for size in range(8, 32):
        font = ImageFont.truetype(str(ttf_path), size=size)
        ok = True
        for ch in sample:
            bbox = font.getbbox(ch)
            if bbox is None:
                continue
            width = bbox[2] - bbox[0]
            height = bbox[3] - bbox[1]
            if width > FONT_WIDTH or height > FONT_HEIGHT:
                ok = False
                break
        if ok:
            best = size
    return best


def _render_glyph(font: ImageFont.FreeTypeFont, ch: str) -> list[int]:
    image = Image.new("L", (FONT_WIDTH, FONT_HEIGHT), 0)
    draw = ImageDraw.Draw(image)
    bbox = draw.textbbox((0, 0), ch, font=font)
    if bbox is None:
        return [0] * FONT_HEIGHT

    width = bbox[2] - bbox[0]
    height = bbox[3] - bbox[1]
    x = max(0, (FONT_WIDTH - width) // 2 - bbox[0])
    y = max(0, (FONT_HEIGHT - height) // 2 - bbox[1])
    draw.text((x, y), ch, font=font, fill=255)

    rows = []
    for row in range(FONT_HEIGHT):
        bits = 0
        for col in range(FONT_WIDTH):
            if image.getpixel((col, row)) > 128:
                bits |= 1 << (7 - col)
        rows.append(bits)
    return rows


def _glyph_for_code(font: ImageFont.FreeTypeFont, code: int) -> list[int]:
    if code < 32 or code == 127:
        return [0] * FONT_HEIGHT

    try:
        ch = chr(code)
    except ValueError:
        return [0] * FONT_HEIGHT

    return _render_glyph(font, ch)


def _emit_c_array(out_path: Path, glyphs: list[list[int]]) -> None:
    with out_path.open("w", encoding="utf-8") as fp:
        fp.write('#include "font.h"\n\n')
        fp.write("/*\n")
        fp.write(" * 由 tools/gen_font.py 基于 Monaco.ttf 在构建期生成。\n")
        fp.write(" * 这样内核仍然使用固定 8x16 字模，但字形来源是 TTF。\n")
        fp.write(" */\n")
        fp.write("uint8_t font_ascii[256][16] =\n{\n")
        for code, glyph in enumerate(glyphs):
            if code % 16 == 0:
                fp.write(f"\t/*\t{code:04x}\t*/\n")
            row_str = ",".join(f"0x{row:02x}" for row in glyph)
            fp.write(f"\t{{{row_str}}},\n")
        fp.write("};\n")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate 8x16 ASCII font from a TTF file.")
    parser.add_argument("--ttf", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()

    if not args.ttf.is_file():
        raise SystemExit(f"missing font file: {args.ttf}")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    font_size = _pick_font_size(args.ttf)
    font = ImageFont.truetype(str(args.ttf), size=font_size)
    glyphs = [_glyph_for_code(font, code) for code in range(256)]
    _emit_c_array(args.out, glyphs)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
