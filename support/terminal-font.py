#!/usr/bin/env python3
import argparse
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Rasterize a monospaced TrueType font into Tunix terminal glyph data."
    )
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, default=8)
    parser.add_argument("--height", type=int, default=18)
    parser.add_argument("--size", type=int, default=13)
    return parser.parse_args()


def glyph_repertoire() -> list[int]:
    codepoints = set(range(0x20, 0x7F))
    codepoints.update(range(0x00A0, 0x0180))
    codepoints.add(0x20AC)
    codepoints.update((0x2190, 0x2191, 0x2192, 0x2193))
    codepoints.update(range(0x2500, 0x2580))
    codepoints.update(range(0x2580, 0x25A0))
    codepoints.add(0xFFFD)
    return sorted(codepoints)


# U+2580..U+259F, drawn rather than rasterised.
#
# These are the glyphs whose whole job is to tile: a column of full blocks has
# to come out as one unbroken bar, and two cells side by side have to meet with
# no seam. A TrueType outline cannot do that here. JetBrains Mono's full block
# covers its em box, which at 13px inside an 18px cell leaves two rows of
# background between one cell and the next -- so every vertical stroke drawn out
# of them came out dashed.
#
# Each entry is the fraction of the cell to fill, as (left, top, right, bottom)
# in eighths, or a single alpha for the three shades. Eighths rather than
# pixels, so the rectangles land on the same boundaries whatever the cell is.
EIGHTHS = 8
BLOCK_RECTS = {
    0x2580: (0, 0, 8, 4),   # upper half
    0x2588: (0, 0, 8, 8),   # full
    0x2590: (4, 0, 8, 8),   # right half
    0x2594: (0, 0, 8, 1),   # upper one eighth
    0x2595: (7, 0, 8, 8),   # right one eighth
}
# Lower one eighth up to lower seven eighths, and then left seven down to left one.
BLOCK_RECTS.update({0x2581 + n: (0, 7 - n, 8, 8) for n in range(7)})
BLOCK_RECTS.update({0x2589 + n: (0, 0, 7 - n, 8) for n in range(7)})
# The shades, as an alpha over the whole cell rather than as a dither: the
# console blends what it is given, so a flat alpha is both truer and cheaper.
BLOCK_SHADES = {0x2591: 64, 0x2592: 128, 0x2593: 191}
# The quadrants, each named by the corners it fills.
UPPER_LEFT, UPPER_RIGHT, LOWER_LEFT, LOWER_RIGHT = range(4)
BLOCK_QUADRANTS = {
    0x2596: (LOWER_LEFT,),
    0x2597: (LOWER_RIGHT,),
    0x2598: (UPPER_LEFT,),
    0x2599: (UPPER_LEFT, LOWER_LEFT, LOWER_RIGHT),
    0x259A: (UPPER_LEFT, LOWER_RIGHT),
    0x259B: (UPPER_LEFT, UPPER_RIGHT, LOWER_LEFT),
    0x259C: (UPPER_LEFT, UPPER_RIGHT, LOWER_RIGHT),
    0x259D: (UPPER_RIGHT,),
    0x259E: (UPPER_RIGHT, LOWER_LEFT),
    0x259F: (UPPER_RIGHT, LOWER_LEFT, LOWER_RIGHT),
}
QUADRANT_RECTS = {
    UPPER_LEFT: (0, 0, 4, 4),
    UPPER_RIGHT: (4, 0, 8, 4),
    LOWER_LEFT: (0, 4, 4, 8),
    LOWER_RIGHT: (4, 4, 8, 8),
}


def scale(value: int, size: int) -> int:
    """An eighth boundary in pixels, rounded so that n and 8-n still add up."""
    return (value * size + EIGHTHS // 2) // EIGHTHS


def flatten(image) -> list[int]:
    """The alpha bytes, row by row, under either Pillow's name for it."""
    if hasattr(image, "get_flattened_data"):
        return list(image.get_flattened_data())
    return list(image.getdata())


def draw_block(image, codepoint: int, width: int, height: int) -> bool:
    """Fill in one of the block elements. False if this is not one."""
    from PIL import ImageDraw

    if codepoint in BLOCK_SHADES:
        image.paste(BLOCK_SHADES[codepoint], (0, 0, width, height))
        return True
    rectangles = []
    if codepoint in BLOCK_RECTS:
        rectangles.append(BLOCK_RECTS[codepoint])
    elif codepoint in BLOCK_QUADRANTS:
        rectangles.extend(QUADRANT_RECTS[q] for q in BLOCK_QUADRANTS[codepoint])
    else:
        return False
    draw = ImageDraw.Draw(image)
    for left, top, right, bottom in rectangles:
        box = (scale(left, width), scale(top, height),
               scale(right, width) - 1, scale(bottom, height) - 1)
        if box[2] >= box[0] and box[3] >= box[1]:
            draw.rectangle(box, fill=255)
    return True


def write_wrapped_values(handle, values: list[str], indent: str = "    ", width: int = 96) -> None:
    line = indent
    for value in values:
        token = value + ","
        if len(line) + len(token) + 1 > width:
            handle.write(line.rstrip() + "\n")
            line = indent
        line += token + " "
    if line.strip():
        handle.write(line.rstrip() + "\n")


def main() -> None:
    args = parse_args()
    if args.width <= 0 or args.height <= 0 or args.size <= 0:
        raise SystemExit("width, height and size must be positive")
    if not args.source.is_file():
        raise SystemExit(f"font source not found: {args.source}")

    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError as exc:
        raise SystemExit("Pillow is required: python3 -m pip install Pillow") from exc

    font = ImageFont.truetype(str(args.source), args.size)
    ascent, _descent = font.getmetrics()
    baseline = min(ascent, args.height - 1)
    codepoints = glyph_repertoire()
    bitmaps: list[int] = []

    for codepoint in codepoints:
        character = chr(codepoint)
        image = Image.new("L", (args.width, args.height), 0)
        if draw_block(image, codepoint, args.width, args.height):
            bitmaps.extend(flatten(image))
            continue
        draw = ImageDraw.Draw(image)
        left, top, right, bottom = draw.textbbox(
            (0, baseline), character, font=font, anchor="ls"
        )
        glyph_width = right - left
        x = (args.width - glyph_width) // 2 - left
        draw.text((x, baseline), character, fill=255, font=font, anchor="ls")
        bitmaps.extend(flatten(image))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8", newline="\n") as output:
        output.write("/* Generated by scripts/generate-terminal-font.py. */\n")
        output.write(f"#define TUNIX_TERMINAL_FONT_GLYPH_COUNT {len(codepoints)}U\n\n")
        output.write(
            "static const uint32_t tunix_terminal_font_codepoints"
            "[TUNIX_TERMINAL_FONT_GLYPH_COUNT] = {\n"
        )
        write_wrapped_values(output, [f"UINT32_C(0x{value:04X})" for value in codepoints])
        output.write("};\n\n")
        output.write(
            "static const uint8_t tunix_terminal_font_alpha"
            "[TUNIX_TERMINAL_FONT_GLYPH_COUNT * TUNIX_TERMINAL_FONT_PIXELS_PER_GLYPH] = {\n"
        )
        write_wrapped_values(output, [f"0x{value:02X}" for value in bitmaps])
        output.write("};\n\n")
        output.write(
            "_Static_assert(sizeof(tunix_terminal_font_alpha) ==\n"
            "               TUNIX_TERMINAL_FONT_GLYPH_COUNT *\n"
            "               TUNIX_TERMINAL_FONT_PIXELS_PER_GLYPH,\n"
            "               \"terminal font alpha size mismatch\");\n"
        )

    print(
        f"terminal font: {args.output} "
        f"({len(codepoints)} glyphs, {args.width}x{args.height}, {len(bitmaps)} alpha bytes)"
    )


if __name__ == "__main__":
    main()
