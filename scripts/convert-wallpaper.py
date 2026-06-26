#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path

WALLPAPER_MAGIC = 0x4C415754
WALLPAPER_VERSION = 1
WALLPAPER_RGB565 = 1


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert an image into the Tunix RGB565 framebuffer wallpaper format."
    )
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--width", type=int, default=960)
    parser.add_argument("--height", type=int, default=540)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    if args.width <= 0 or args.height <= 0:
        raise SystemExit("width and height must be positive")

    try:
        from PIL import Image, ImageOps
    except ImportError as exc:
        raise SystemExit("Pillow is required: python3 -m pip install Pillow") from exc

    with Image.open(args.source) as source:
        image = ImageOps.fit(
            source.convert("RGB"),
            (args.width, args.height),
            method=Image.Resampling.LANCZOS,
            centering=(0.5, 0.5),
        )

    source_bytes = image.tobytes()
    payload = bytearray(args.width * args.height * 2)
    offset = 0
    for source_offset in range(0, len(source_bytes), 3):
        red = source_bytes[source_offset]
        green = source_bytes[source_offset + 1]
        blue = source_bytes[source_offset + 2]
        pixel = ((red >> 3) << 11) | ((green >> 2) << 5) | (blue >> 3)
        struct.pack_into("<H", payload, offset, pixel)
        offset += 2

    header = struct.pack(
        "<IHHIIII",
        WALLPAPER_MAGIC,
        WALLPAPER_VERSION,
        WALLPAPER_RGB565,
        args.width,
        args.height,
        args.width,
        len(payload),
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + payload)
    print(f"wallpaper: {args.output} ({args.width}x{args.height}, {len(header) + len(payload)} bytes)")


if __name__ == "__main__":
    main()
