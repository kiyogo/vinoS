#!/usr/bin/env python3
"""Rasterize the bundled Inter UI font to the compact proportional GUI font."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


GLYPH_FIRST = 32
GLYPH_LAST = 126
CELL_WIDTH = 12
CELL_HEIGHT = 14


def render_glyphs(font_path: Path) -> list[tuple[int, list[int]]]:
    font = ImageFont.truetype(str(font_path), 11)
    result: list[tuple[int, list[int]]] = []
    for codepoint in range(GLYPH_FIRST, GLYPH_LAST + 1):
        character = chr(codepoint)
        bounds = font.getbbox(character)
        ink_width = max(1, bounds[2] - bounds[0])
        ink_height = max(1, bounds[3] - bounds[1])
        advance = max(3, min(CELL_WIDTH,
                             round(font.getlength(character)) + 1))
        advance = max(advance, min(CELL_WIDTH, ink_width))
        x = (advance - ink_width) // 2 - bounds[0]
        y = (CELL_HEIGHT - ink_height) // 2 - bounds[1]
        image = Image.new("L", (advance, CELL_HEIGHT), 0)
        ImageDraw.Draw(image).text((x, y), character, font=font, fill=255)
        pixels = image.load()
        rows = []
        for row in range(CELL_HEIGHT):
            bits = 0
            for column in range(advance):
                if pixels[column, row] >= 128:
                    bits |= 1 << column
            rows.append(bits)
        result.append((advance, rows))
    return result


def write_header(path: Path, glyphs: list[tuple[int, list[int]]]) -> None:
    lines = [
        "/* Generated from Inter under the SIL Open Font License. */",
        "#ifndef KOS_GUI_FONT_INTER_8X14_H",
        "#define KOS_GUI_FONT_INTER_8X14_H",
        "#include <stdint.h>",
        "enum { KOS_GUI_FONT_INTER_FIRST = 32, KOS_GUI_FONT_INTER_COUNT = 95,",
        "       KOS_GUI_FONT_INTER_MAX_WIDTH = 12, KOS_GUI_FONT_INTER_HEIGHT = 14 };",
        "struct kos_gui_font_inter_glyph {",
        "    uint8_t advance;",
        "    uint16_t rows[KOS_GUI_FONT_INTER_HEIGHT];",
        "};",
        "static const struct kos_gui_font_inter_glyph",
        "kos_gui_font_inter_8x14[KOS_GUI_FONT_INTER_COUNT] = {",
    ]
    for codepoint, (advance, rows) in enumerate(glyphs, start=GLYPH_FIRST):
        printable = chr(codepoint)
        label = printable if printable not in "\\'" else f"\\x{codepoint:02x}"
        lines.append(f"    /* U+{codepoint:04X} {label} */ {{ {advance}, {{")
        lines.append("        " + ", ".join(f"0x{bits:03x}" for bits in rows) + "} },")
    lines.extend(["};", "#endif /* KOS_GUI_FONT_INTER_8X14_H */", ""])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines), encoding="ascii", newline="\n")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--font", type=Path, default=Path("assets/gui/fonts/Inter-Variable.ttf"))
    parser.add_argument("--output", type=Path,
                        default=Path("include/kos/gui_font_inter_8x14.h"))
    arguments = parser.parse_args()
    write_header(arguments.output, render_glyphs(arguments.font))
    print(f"Wrote {arguments.output} ({GLYPH_COUNT} glyphs)")
    return 0


GLYPH_COUNT = GLYPH_LAST - GLYPH_FIRST + 1

if __name__ == "__main__":
    raise SystemExit(main())
