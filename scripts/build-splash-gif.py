# build-splash-gif.py — animates and embeds the indexed about:start splash.
from __future__ import annotations

import base64
import math
import os
from pathlib import Path
import random
import re
import textwrap

from PIL import Image, ImageDraw, ImageFilter


ROOT = Path(__file__).resolve().parent.parent
WIDTH = 940
HEIGHT = 320
FRAME_COUNT = int(os.environ.get("NS_SPLASH_FRAMES", "32"))
FRAME_DELAY = int(os.environ.get("NS_SPLASH_DELAY", "80"))


def project_version() -> str:
    text = (ROOT / "meson.build").read_text(encoding="utf-8")
    match = re.search(r"^\s*version:\s*'([^']+)'", text, re.MULTILINE)
    if match is None:
        raise RuntimeError("could not read version from meson.build")
    return match.group(1).split("-", 1)[0]


def composite_glow(frame: Image.Image, layer: Image.Image, radius: float) -> None:
    blurred = layer.filter(ImageFilter.GaussianBlur(radius))
    frame.alpha_composite(blurred)
    frame.alpha_composite(layer)


def aurora_layer(index: int) -> Image.Image:
    layer = Image.new("RGBA", (WIDTH, HEIGHT))
    draw = ImageDraw.Draw(layer)
    cycle = 2.0 * math.pi * index / FRAME_COUNT
    for band in range(23):
        x = 55 + band * 38
        phase = band * 1.31 + cycle
        height = 28 + 34 * (0.5 + 0.5 * math.sin(phase))
        sway = 8 * math.sin(cycle * 1.4 + band * 0.72)
        opacity = int(9 + 13 * (0.5 + 0.5 * math.sin(phase * 0.83)))
        draw.line(
            [(x + sway, 238), (x + sway * 0.45, 210), (x, 202 - height)],
            fill=(42, 241, 204, opacity),
            width=5,
        )
    return layer.filter(ImageFilter.GaussianBlur(8.0))


def north_star_layer(index: int) -> Image.Image:
    layer = Image.new("RGBA", (WIDTH, HEIGHT))
    draw = ImageDraw.Draw(layer)
    cycle = 2.0 * math.pi * index / FRAME_COUNT
    pulse = 0.5 + 0.5 * math.sin(cycle)
    cx, cy = 750, 112
    core = 3 + int(2 * pulse)
    long_ray = 22 + int(10 * pulse)
    short_ray = 9 + int(5 * pulse)
    alpha = 90 + int(80 * pulse)
    draw.ellipse((cx - core, cy - core, cx + core, cy + core), fill=(255, 255, 255, 220))
    draw.line((cx - long_ray, cy, cx + long_ray, cy), fill=(204, 231, 255, alpha), width=2)
    draw.line((cx, cy - long_ray, cx, cy + long_ray), fill=(204, 231, 255, alpha), width=2)
    draw.line((cx - short_ray, cy - short_ray, cx + short_ray, cy + short_ray), fill=(177, 218, 255, alpha), width=1)
    draw.line((cx - short_ray, cy + short_ray, cx + short_ray, cy - short_ray), fill=(177, 218, 255, alpha), width=1)
    return layer


def twinkle_layer(index: int) -> Image.Image:
    layer = Image.new("RGBA", (WIDTH, HEIGHT))
    draw = ImageDraw.Draw(layer)
    cycle = 2.0 * math.pi * index / FRAME_COUNT
    stars = [
        (44, 41), (167, 34), (225, 52), (326, 36), (407, 58), (477, 39),
        (568, 48), (619, 27), (676, 66), (832, 37), (901, 68), (525, 96),
        (603, 119), (854, 116), (469, 135), (660, 149), (875, 156), (341, 158),
    ]
    for number, (x, y) in enumerate(stars):
        pulse = 0.5 + 0.5 * math.sin(cycle * (1 + number % 3) + number * 1.73)
        if pulse < 0.58:
            continue
        alpha = int(55 + pulse * 120)
        radius = 1 + int(pulse * 1.4)
        draw.ellipse((x - radius, y - radius, x + radius, y + radius), fill=(214, 235, 255, alpha))
        if pulse > 0.87:
            draw.line((x - 4, y, x + 4, y), fill=(166, 211, 255, alpha), width=1)
            draw.line((x, y - 4, x, y + 4), fill=(166, 211, 255, alpha), width=1)
    lights = [
        (238, 278), (286, 287), (313, 271), (357, 282), (392, 270), (528, 286),
        (555, 275), (617, 289), (678, 275), (705, 291), (738, 282), (770, 299),
    ]
    for number, (x, y) in enumerate(lights):
        pulse = 0.5 + 0.5 * math.sin(cycle * 2.0 + number * 2.17)
        if pulse > 0.62:
            alpha = int(70 + pulse * 145)
            draw.ellipse((x - 1, y - 1, x + 1, y + 1), fill=(255, 205, 104, alpha))
    return layer


def make_frames(source: Path) -> list[Image.Image]:
    with Image.open(source) as opened:
        base = opened.convert("RGBA").resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    frames = []
    for index in range(FRAME_COUNT):
        frame = base.copy()
        frame.alpha_composite(aurora_layer(index))
        composite_glow(frame, north_star_layer(index), 5.0)
        composite_glow(frame, twinkle_layer(index), 1.2)
        frames.append(frame.convert("RGB"))
    return frames


def indexed_frames(frames: list[Image.Image]) -> list[Image.Image]:
    rng = random.Random(1997)
    sample = Image.new("RGB", (WIDTH, HEIGHT))
    pixels = sample.load()
    source_pixels = [frame.load() for frame in frames]
    for y in range(HEIGHT):
        for x in range(WIDTH):
            pixels[x, y] = source_pixels[rng.randrange(len(frames))][x, y]
    palette = sample.quantize(colors=256, method=Image.Quantize.MEDIANCUT)
    return [
        frame.quantize(palette=palette, dither=Image.Dither.FLOYDSTEINBERG)
        for frame in frames
    ]


def write_header(gif_path: Path) -> None:
    encoded = base64.b64encode(gif_path.read_bytes()).decode("ascii")
    lines = textwrap.wrap(encoded, 96)
    output = [
        "/* about_splash_gif.h — the animated 256-colour about:start splash, embedded. */",
        "#ifndef NS_ABOUT_SPLASH_GIF_H",
        "#define NS_ABOUT_SPLASH_GIF_H",
        "",
        "static const char about_splash_gif_b64[] =",
    ]
    output.extend(
        f'    "{line}"{";" if number == len(lines) - 1 else ""}'
        for number, line in enumerate(lines)
    )
    output.extend(["", "#endif", ""])
    (ROOT / "src" / "about_splash_gif.h").write_text(
        "\n".join(output), encoding="utf-8", newline="\n"
    )


def main() -> None:
    version = project_version()
    source = ROOT / "data" / f"about-splash-{version}.png"
    if not source.is_file():
        raise FileNotFoundError(f"missing versioned splash source: {source}")
    output = ROOT / "data" / "about-splash.gif"
    frames = indexed_frames(make_frames(source))
    frames[0].save(
        output,
        save_all=True,
        append_images=frames[1:],
        duration=FRAME_DELAY,
        loop=0,
        disposal=1,
        optimize=True,
    )
    write_header(output)
    print(f"wrote {output} ({len(frames)} frames, 256 colours)")


if __name__ == "__main__":
    main()
