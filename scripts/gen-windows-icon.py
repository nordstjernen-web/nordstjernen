#!/usr/bin/env python3
"""Render Nordstjernen's SVG logo into a multi-resolution Windows .ico.

Regenerate after editing data/icons/hicolor/scalable/apps/nordstjernen.svg:

    python scripts/gen-windows-icon.py

Requires rsvg-convert in PATH (provided by MSYS2's librsvg package on Windows,
or librsvg2-bin on Debian/Ubuntu).
"""

import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SVG = ROOT / "data" / "icons" / "hicolor" / "scalable" / "apps" / "nordstjernen.svg"
OUT = ROOT / "data" / "icons" / "nordstjernen.ico"
SIZES = (16, 24, 32, 48, 64, 128, 256)


def render_png(size: int) -> bytes:
    result = subprocess.run(
        ["rsvg-convert", "-w", str(size), "-h", str(size),
         "-f", "png", str(SVG)],
        check=True, capture_output=True,
    )
    return result.stdout


def main() -> int:
    if not SVG.is_file():
        print(f"missing source: {SVG}", file=sys.stderr)
        return 1
    images = [(size, render_png(size)) for size in SIZES]

    header = struct.pack("<HHH", 0, 1, len(images))
    dir_size = 16 * len(images)
    offset = len(header) + dir_size

    directory = bytearray()
    payload = bytearray()
    for size, png in images:
        w = 0 if size >= 256 else size
        h = 0 if size >= 256 else size
        directory += struct.pack(
            "<BBBBHHII",
            w, h, 0, 0, 1, 32, len(png), offset,
        )
        payload += png
        offset += len(png)

    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open("wb") as f:
        f.write(header)
        f.write(directory)
        f.write(payload)
    print(f"wrote {OUT} ({OUT.stat().st_size} bytes, {len(images)} sizes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
