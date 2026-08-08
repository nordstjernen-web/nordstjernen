# build-splash-version.py — restamps the painted splash artwork with the project version.
from __future__ import annotations

import math
from pathlib import Path
import re

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parent.parent
BASE_VERSION = "1.0.22"
INK_BOX = (419, 48, 567, 87)
SHADOW_OFFSET = (2, 2)
SHADOW_RGB = (16, 8, 0)
GOLD_TOP = (247, 183, 6)
GOLD_BOTTOM = (226, 152, 3)
SUPERSAMPLE = 8

FONT_CANDIDATES = (
    "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/liberation/LiberationSans-Bold.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
    "/Library/Fonts/Arial Bold.ttf",
    "C:/Windows/Fonts/arialbd.ttf",
)


def project_version() -> str:
    text = (ROOT / "meson.build").read_text(encoding="utf-8")
    match = re.search(r"^\s*version:\s*'([^']+)'", text, re.MULTILINE)
    if match is None:
        raise RuntimeError("could not read version from meson.build")
    return match.group(1).split("-", 1)[0]


def load_font(size: int) -> ImageFont.FreeTypeFont:
    for path in FONT_CANDIDATES:
        if Path(path).is_file():
            return ImageFont.truetype(path, size)
    raise FileNotFoundError("no bold sans font found for the splash version")


def ink_bounds(mask: Image.Image):
    box = mask.getbbox()
    if box is None:
        raise RuntimeError("the version text rendered empty")
    return box


def stamp_mask(version: str, origin: tuple[int, int], size: int) -> Image.Image:
    canvas = Image.new("L", (2600, 900), 0)
    ImageDraw.Draw(canvas).text(origin, version, font=load_font(size), fill=255)
    return canvas


def rendered_digits(version: str):
    size = 64 * SUPERSAMPLE
    origin = (200, 200)
    reference = stamp_mask(BASE_VERSION, origin, size)
    x0, y0, x1, y1 = ink_bounds(reference)
    scale_x = (INK_BOX[2] - INK_BOX[0] + 1) / (x1 - x0)
    scale_y = (INK_BOX[3] - INK_BOX[1] + 1) / (y1 - y0)

    target = stamp_mask(version, origin, size)
    placed = target.resize(
        (max(1, round(target.width * scale_x)), max(1, round(target.height * scale_y))),
        Image.Resampling.LANCZOS,
    )
    offset_x = INK_BOX[0] - x0 * scale_x
    offset_y = INK_BOX[1] - y0 * scale_y
    return placed, offset_x, offset_y


def differing_span(base: str, version: str) -> tuple[int, int]:
    if len(base) != len(version):
        return 0, len(version)
    lead = 0
    while lead < len(base) and base[lead] == version[lead]:
        lead += 1
    trail = len(base)
    while trail > lead and base[trail - 1] == version[trail - 1]:
        trail -= 1
    return lead, trail


def glyph_slots(version: str, placed_offset) -> list[tuple[int, int]]:
    placed, offset_x, offset_y = placed_offset
    columns = np.asarray(placed, np.float32).max(axis=0) > 24
    slots = []
    start = None
    for index, value in enumerate(columns):
        if value and start is None:
            start = index
        if not value and start is not None:
            slots.append((start, index - 1))
            start = None
    if start is not None:
        slots.append((start, len(columns) - 1))
    return [(round(a + offset_x), round(b + offset_x)) for a, b in slots]


def fill_background(image: Image.Image, box: tuple[int, int, int, int]) -> None:
    left, top, right, bottom = box
    pixels = np.asarray(image, np.float32).copy()
    edge_left = pixels[top:bottom + 1, left - 1]
    edge_right = pixels[top:bottom + 1, right + 1]
    width = right - left + 1
    ramp = np.linspace(0.0, 1.0, width, dtype=np.float32)[None, :, None]
    patch = edge_left[:, None, :] * (1.0 - ramp) + edge_right[:, None, :] * ramp
    pixels[top:bottom + 1, left:right + 1] = patch
    image.paste(Image.fromarray(pixels.astype(np.uint8), "RGB"), (0, 0))


def gold_fill(height: int) -> np.ndarray:
    ramp = np.linspace(0.0, 1.0, height, dtype=np.float32)[:, None]
    top = np.array(GOLD_TOP, np.float32)[None, :]
    bottom = np.array(GOLD_BOTTOM, np.float32)[None, :]
    return top * (1.0 - ramp) + bottom * ramp


def restamp(version: str) -> Image.Image:
    source = Image.open(ROOT / "data" / f"about-splash-{BASE_VERSION}.png").convert("RGB")
    if version == BASE_VERSION:
        return source

    placed_offset = rendered_digits(version)
    lead, trail = differing_span(BASE_VERSION, version)
    slots = glyph_slots(BASE_VERSION, rendered_digits(BASE_VERSION))
    if trail > len(slots):
        raise RuntimeError("the version has more glyphs than the artwork carries")

    left = slots[lead][0] - 3
    right = slots[trail - 1][1] + SHADOW_OFFSET[0] + 3
    top = INK_BOX[1] - 4
    bottom = INK_BOX[3] + SHADOW_OFFSET[1] + 3
    fill_background(source, (left, top, right, bottom))

    placed, offset_x, offset_y = placed_offset
    stamp = Image.new("L", source.size, 0)
    stamp.paste(placed, (round(offset_x), round(offset_y)))
    keep = Image.new("L", source.size, 0)
    ImageDraw.Draw(keep).rectangle((left, top, right, bottom), fill=255)
    alpha = np.asarray(stamp, np.float32) / 255.0 * (np.asarray(keep, np.float32) / 255.0)

    shadow = np.asarray(
        Image.fromarray((alpha * 255.0).astype(np.uint8), "L")
        .transform(
            source.size,
            Image.Transform.AFFINE,
            (1, 0, -SHADOW_OFFSET[0], 0, 1, -SHADOW_OFFSET[1]),
        )
        .filter(ImageFilter.GaussianBlur(0.6)),
        np.float32,
    ) / 255.0

    canvas = np.asarray(source, np.float32).copy()
    shadow_rgb = np.array(SHADOW_RGB, np.float32)[None, None, :]
    canvas = canvas * (1.0 - shadow[:, :, None]) + shadow_rgb * shadow[:, :, None]

    gradient = np.zeros_like(canvas)
    gradient[INK_BOX[1]:INK_BOX[3] + 1] = gold_fill(INK_BOX[3] - INK_BOX[1] + 1)[:, None, :]
    gradient[:INK_BOX[1]] = np.array(GOLD_TOP, np.float32)
    gradient[INK_BOX[3] + 1:] = np.array(GOLD_BOTTOM, np.float32)
    canvas = canvas * (1.0 - alpha[:, :, None]) + gradient * alpha[:, :, None]
    return Image.fromarray(np.clip(canvas, 0, 255).astype(np.uint8), "RGB")


def main() -> None:
    version = project_version()
    output = ROOT / "data" / f"about-splash-{version}.png"
    restamp(version).save(output, optimize=True)
    print(f"wrote {output}")


if __name__ == "__main__":
    main()
