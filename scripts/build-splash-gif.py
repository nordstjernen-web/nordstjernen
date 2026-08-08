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
CRAFT_SS = 4


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


def draw_cruiser(draw: ImageDraw.ImageDraw, x: float, y: float, length: float, facing: int) -> None:
    unit = length / 10.0
    step = facing * unit
    draw.polygon(
        [
            (x + step * 5.0, y - unit * 0.10),
            (x + step * 3.4, y - unit * 0.68),
            (x - step * 1.2, y - unit * 0.92),
            (x - step * 4.3, y - unit * 0.80),
            (x - step * 4.7, y + unit * 0.20),
            (x - step * 4.0, y + unit * 0.88),
            (x + step * 1.2, y + unit * 0.90),
            (x + step * 3.6, y + unit * 0.46),
        ],
        fill=(46, 57, 78, 255),
    )
    draw.polygon(
        [
            (x - step * 0.4, y - unit * 0.88),
            (x - step * 1.9, y - unit * 1.52),
            (x - step * 3.6, y - unit * 1.48),
            (x - step * 4.2, y - unit * 0.80),
        ],
        fill=(68, 84, 112, 255),
    )
    draw.polygon(
        [
            (x - step * 4.0, y - unit * 0.72),
            (x - step * 4.9, y - unit * 0.56),
            (x - step * 4.9, y + unit * 0.62),
            (x - step * 4.0, y + unit * 0.74),
        ],
        fill=(68, 84, 112, 255),
    )
    draw.line(
        [(x + step * 3.2, y - unit * 0.74), (x - step * 4.1, y - unit * 0.92)],
        fill=(148, 186, 232, 255),
        width=max(1, int(unit * 0.30)),
    )
    draw.line(
        [(x + step * 3.7, y - unit * 0.30), (x + step * 2.2, y - unit * 0.56)],
        fill=(168, 236, 255, 255),
        width=max(1, int(unit * 0.40)),
    )


def draw_saucer(draw: ImageDraw.ImageDraw, x: float, y: float, width: float) -> None:
    rim = width * 0.30
    dome = width * 0.44
    draw.ellipse((x - dome, y - rim * 1.85, x + dome, y + rim * 0.35), fill=(58, 72, 98, 255))
    draw.ellipse((x - width, y - rim * 0.62, x + width, y + rim * 0.72), fill=(38, 47, 65, 255))
    draw.ellipse((x - width * 0.72, y + rim * 0.10, x + width * 0.72, y + rim * 1.05), fill=(27, 34, 48, 255))
    draw.line((x - width * 0.92, y - rim * 0.18, x + width * 0.92, y - rim * 0.18), fill=(142, 178, 224, 220), width=max(1, int(rim * 0.22)))


def draw_aircraft(draw: ImageDraw.ImageDraw, x: float, y: float, length: float, facing: int) -> None:
    unit = length / 10.0
    step = facing * unit
    draw.polygon(
        [
            (x + step * 5.0, y),
            (x + step * 1.0, y - unit * 0.62),
            (x - step * 4.6, y - unit * 0.52),
            (x - step * 4.6, y + unit * 0.58),
            (x + step * 1.0, y + unit * 0.66),
        ],
        fill=(54, 66, 88, 255),
    )
    draw.polygon(
        [
            (x + step * 0.6, y - unit * 0.30),
            (x - step * 1.8, y - unit * 3.0),
            (x - step * 2.9, y - unit * 2.9),
            (x - step * 1.4, y - unit * 0.20),
        ],
        fill=(70, 86, 114, 255),
    )
    draw.polygon(
        [
            (x + step * 0.6, y + unit * 0.34),
            (x - step * 1.8, y + unit * 3.0),
            (x - step * 2.9, y + unit * 2.9),
            (x - step * 1.4, y + unit * 0.24),
        ],
        fill=(70, 86, 114, 255),
    )
    draw.polygon(
        [
            (x - step * 3.6, y - unit * 0.40),
            (x - step * 4.6, y - unit * 1.85),
            (x - step * 5.2, y - unit * 1.80),
            (x - step * 4.8, y - unit * 0.34),
        ],
        fill=(70, 86, 114, 255),
    )


def cruiser_x(index: int) -> float:
    return -92.0 + 1128.0 * index / FRAME_COUNT


def airliner_x(index: int) -> float:
    return 1012.0 - 1130.0 * index / FRAME_COUNT


def cargo_x(index: int) -> float:
    return -64.0 + 1096.0 * ((index + 13) % FRAME_COUNT) / FRAME_COUNT


def saucer_y(index: int) -> float:
    return 62.0 + 3.6 * math.sin(2.0 * math.pi * index / FRAME_COUNT)


def craft_hulls(index: int) -> Image.Image:
    layer = Image.new("RGBA", (WIDTH * CRAFT_SS, HEIGHT * CRAFT_SS), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    draw_cruiser(draw, cruiser_x(index) * CRAFT_SS, 161.0 * CRAFT_SS, 62.0 * CRAFT_SS, 1)
    draw_saucer(draw, 862.0 * CRAFT_SS, saucer_y(index) * CRAFT_SS, 20.0 * CRAFT_SS)
    draw_aircraft(draw, airliner_x(index) * CRAFT_SS, 188.0 * CRAFT_SS, 22.0 * CRAFT_SS, -1)
    draw_aircraft(draw, cargo_x(index) * CRAFT_SS, 201.0 * CRAFT_SS, 16.0 * CRAFT_SS, 1)
    return layer.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)


def craft_lights(index: int) -> Image.Image:
    layer = Image.new("RGBA", (WIDTH * CRAFT_SS, HEIGHT * CRAFT_SS), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)

    x = cruiser_x(index) * CRAFT_SS
    y = 163.0 * CRAFT_SS
    unit = 6.2 * CRAFT_SS
    for offset in (-unit * 0.36, unit * 0.44):
        draw.ellipse(
            (x - unit * 5.4, y + offset - unit * 0.30, x - unit * 4.5, y + offset + unit * 0.30),
            fill=(214, 240, 255, 255),
        )
    for tail in range(10):
        near = x - unit * (5.0 + tail * 0.9)
        far = near - unit * 0.95
        fade = int(150 * (1.0 - tail / 10.0) ** 2.2)
        draw.line(
            [(near, y), (far, y)],
            fill=(104, 180, 255, fade),
            width=max(1, int(unit * (0.62 - 0.045 * tail))),
        )
    draw.ellipse(
        (x + unit * 4.1, y - unit * 0.62, x + unit * 4.8, y - unit * 0.05), fill=(120, 232, 255, 255)
    )

    sx = 858.0 * CRAFT_SS
    sy = saucer_y(index) * CRAFT_SS
    span = 15.0 * CRAFT_SS
    for light in range(7):
        phase = 2.0 * math.pi * (light / 7.0 + 2.0 * index / FRAME_COUNT)
        glow = 0.5 + 0.5 * math.sin(phase)
        lx = sx + math.cos(2.0 * math.pi * light / 7.0) * span * 0.82
        ly = sy + span * 0.16
        radius = span * 0.10
        draw.ellipse(
            (lx - radius, ly - radius, lx + radius, ly + radius),
            fill=(255, 214, 122, int(70 + 170 * glow)),
        )
    draw.ellipse(
        (sx - span * 0.34, sy - span * 0.62, sx + span * 0.34, sy - span * 0.24),
        fill=(150, 236, 255, 190),
    )

    for x_of, y_at, size, facing in (
        (airliner_x(index), 188.0, 22.0, -1),
        (cargo_x(index), 201.0, 16.0, 1),
    ):
        ax = x_of * CRAFT_SS
        ay = y_at * CRAFT_SS
        unit = size * CRAFT_SS / 10.0
        strobe = 255 if index % 8 == 0 else 40
        draw.ellipse(
            (ax - unit * 3.1, ay - unit * 3.2, ax - unit * 2.5, ay - unit * 2.6), fill=(255, 70, 60, 235)
        )
        draw.ellipse(
            (ax - unit * 3.1, ay + unit * 2.6, ax - unit * 2.5, ay + unit * 3.2), fill=(80, 255, 110, 235)
        )
        draw.ellipse(
            (ax + unit * 4.4, ay - unit * 0.3, ax + unit * 5.0, ay + unit * 0.3),
            fill=(255, 255, 255, strobe),
        )
        draw.line(
            [(ax - facing * unit * 4.8, ay), (ax - facing * unit * 15.0, ay)],
            fill=(150, 190, 230, 58),
            width=max(1, int(unit * 0.55)),
        )
    return layer.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)


def make_frames(source: Path) -> list[Image.Image]:
    with Image.open(source) as opened:
        base = opened.convert("RGBA").resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)
    frames = []
    for index in range(FRAME_COUNT):
        frame = base.copy()
        frame.alpha_composite(aurora_layer(index))
        frame.alpha_composite(craft_hulls(index))
        composite_glow(frame, craft_lights(index), 2.6)
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
