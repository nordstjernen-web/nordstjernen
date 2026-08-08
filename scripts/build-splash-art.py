# build-splash-art.py — renders the versioned about:start splash artwork.
from __future__ import annotations

import math
from pathlib import Path
import re

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont


ROOT = Path(__file__).resolve().parent.parent
WIDTH = 940
HEIGHT = 320
SS = 3
SEED = 20260808

EARTH_CX = 470.0
EARTH_TOP = 212.0
EARTH_R = 2400.0
EARTH_TILT = 68.0
STAR_X = 750.0
STAR_Y = 112.0

FONT_CANDIDATES = (
    "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/Library/Fonts/DejaVuSans-Bold.ttf",
    "C:/Windows/Fonts/segoeuib.ttf",
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
    raise FileNotFoundError("no bold sans font found for the splash title")


def blur(channel: np.ndarray, radius: float) -> np.ndarray:
    peak = float(np.max(channel))
    if peak <= 0.0:
        return np.zeros_like(channel, np.float32)
    scaled = np.clip(channel / peak * 255.0, 0.0, 255.0).astype(np.uint8)
    image = Image.fromarray(scaled, "L").filter(ImageFilter.GaussianBlur(radius))
    return np.asarray(image, np.float32) / 255.0 * peak


def blur_rgb(layer: np.ndarray, radius: float) -> np.ndarray:
    return np.stack([blur(layer[:, :, c], radius) for c in range(3)], axis=2)


def fractal_noise(width: int, height: int, octaves: int, rng, cells: int = 4) -> np.ndarray:
    total = np.zeros((height, width), np.float32)
    amplitude = 1.0
    norm = 0.0
    for octave in range(octaves):
        grid_w = max(2, cells * (2 ** octave))
        grid_h = max(2, int(round(grid_w * height / width)))
        grid = rng.random((grid_h, grid_w)).astype(np.float32)
        scaled = Image.fromarray(grid * 255.0).resize((width, height), Image.Resampling.BICUBIC)
        total += amplitude * (np.asarray(scaled, np.float32) / 255.0)
        norm += amplitude
        amplitude *= 0.5
    return total / norm


def ridged_noise(width: int, height: int, octaves: int, rng, cells: int = 4) -> np.ndarray:
    return 1.0 - np.abs(fractal_noise(width, height, octaves, rng, cells) * 2.0 - 1.0)


def splat(canvas: np.ndarray, x: float, y: float, colour: np.ndarray, weight: float) -> None:
    height, width = canvas.shape[:2]
    x0, y0 = int(math.floor(x)), int(math.floor(y))
    fx, fy = x - x0, y - y0
    for dy in (0, 1):
        for dx in (0, 1):
            px, py = x0 + dx, y0 + dy
            if 0 <= px < width and 0 <= py < height:
                share = (fx if dx else 1.0 - fx) * (fy if dy else 1.0 - fy)
                canvas[py, px] += colour * (weight * share)


def limb_y(x: np.ndarray) -> np.ndarray:
    cx = EARTH_CX * SS
    cy = (EARTH_TOP + EARTH_R) * SS
    radius = EARTH_R * SS
    return cy - np.sqrt(np.clip(radius ** 2 - (x - cx) ** 2, 0.0, None))


def sample_equirect(texture: np.ndarray, lon: np.ndarray, lat: np.ndarray) -> np.ndarray:
    rows, columns = texture.shape
    tx = (lon / (2.0 * math.pi) + 0.5) * columns
    ty = (lat / math.pi + 0.5) * rows
    x0 = np.floor(tx).astype(np.int64)
    y0 = np.floor(ty).astype(np.int64)
    fx = (tx - x0).astype(np.float32)
    fy = (ty - y0).astype(np.float32)
    xa = np.mod(x0, columns)
    xb = np.mod(x0 + 1, columns)
    ya = np.clip(y0, 0, rows - 1)
    yb = np.clip(y0 + 1, 0, rows - 1)
    top = texture[ya, xa] * (1.0 - fx) + texture[ya, xb] * fx
    bottom = texture[yb, xa] * (1.0 - fx) + texture[yb, xb] * fx
    return top * (1.0 - fy) + bottom * fy


def globe(width: int, height: int):
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    cx = EARTH_CX * SS
    cy = (EARTH_TOP + EARTH_R) * SS
    radius = EARTH_R * SS
    u = (xx - cx) / radius
    v = (yy - cy) / radius
    span = u * u + v * v
    inside = (span <= 1.0).astype(np.float32)
    facing = np.sqrt(np.clip(1.0 - span, 0.0, None))

    tilt = math.radians(EARTH_TILT)
    cos_t, sin_t = math.cos(tilt), math.sin(tilt)
    v_rot = v * cos_t + facing * sin_t
    w_rot = facing * cos_t - v * sin_t

    lat = np.arcsin(np.clip(-v_rot, -1.0, 1.0))
    lon = np.arctan2(u, np.clip(w_rot, 1.0e-5, None))
    return inside, facing, lat, lon, np.hypot(xx - cx, yy - cy), radius


def sky(width: int, height: int, rng) -> np.ndarray:
    rows = np.linspace(0.0, 1.0, height, dtype=np.float32)[:, None]
    columns = np.linspace(0.0, 1.0, width, dtype=np.float32)[None, :]
    top = np.array([0.003, 0.006, 0.020], np.float32)
    bottom = np.array([0.008, 0.024, 0.068], np.float32)
    canvas = top[None, None, :] + (bottom - top)[None, None, :] * (rows ** 1.8)[:, :, None]
    canvas = np.repeat(canvas, width, axis=1)

    axis = rows - (0.22 + 0.26 * columns)
    core = np.exp(-((axis / 0.085) ** 2))
    wings = np.exp(-((axis / 0.24) ** 2))
    clumps = fractal_noise(width, height, 6, rng, cells=4)
    rift = fractal_noise(width, height, 5, rng, cells=5)
    dust = np.clip(1.0 - np.clip(rift * 2.1 - 0.62, 0.0, 1.0) * 1.35, 0.0, 1.0)

    milky = (core * 0.52 + wings * 0.20) * (0.34 + 0.66 * clumps) * dust
    canvas += milky[:, :, None] * np.array([0.30, 0.34, 0.52], np.float32)
    canvas += (milky ** 2.2)[:, :, None] * np.array([0.26, 0.22, 0.18], np.float32) * 0.5

    haze = fractal_noise(width, height, 4, rng, cells=2)
    canvas += (haze[:, :, None] - 0.5) * np.array([0.005, 0.008, 0.018], np.float32)
    return np.clip(canvas, 0.0, 1.0)


def star_colour(temperature: float) -> np.ndarray:
    warm = np.array([1.0, 0.66, 0.38], np.float32)
    sun = np.array([1.0, 0.94, 0.86], np.float32)
    hot = np.array([0.71, 0.81, 1.0], np.float32)
    if temperature < 0.5:
        blend = temperature / 0.5
        return warm * (1.0 - blend) + sun * blend
    blend = (temperature - 0.5) / 0.5
    return sun * (1.0 - blend) + hot * blend


def starfield(width: int, height: int, rng) -> np.ndarray:
    canvas = np.zeros((height, width, 3), np.float32)
    bright = np.zeros((height, width, 3), np.float32)
    horizon = limb_y(np.arange(width, dtype=np.float32))
    drift = fractal_noise(width, height, 5, rng, cells=6)

    placed = 0
    while placed < 6400:
        x = rng.random() * width
        y = rng.random() * height
        if drift[int(y), int(x)] < 0.36 and rng.random() < 0.70:
            continue
        placed += 1
        above = horizon[int(x)] - y
        if above <= 0.0:
            continue
        extinction = float(np.clip(above / (100.0 * SS), 0.05, 1.0))
        magnitude = rng.random() ** 3.0
        tint = star_colour(rng.random())
        reddened = tint * np.array(
            [1.0, 0.60 + 0.40 * extinction, 0.26 + 0.74 * extinction], np.float32
        )
        splat(canvas, x, y, reddened, (0.07 + 2.0 * magnitude) * extinction)

    for _ in range(26):
        x = rng.random() * width
        y = rng.random() * height
        if horizon[int(x)] - y <= 12.0 * SS:
            continue
        power = 0.45 + rng.random() * 0.9
        tint = star_colour(0.35 + rng.random() * 0.65)
        splat(bright, x, y, tint, 5.4 * power)
        arm = int((5 + 8 * power) * SS)
        for step in range(1, arm):
            fade = (1.0 - step / arm) ** 2.3 * 0.38 * power
            splat(bright, x + step, y, tint, fade)
            splat(bright, x - step, y, tint, fade)
            splat(bright, x, y + step, tint, fade)
            splat(bright, x, y - step, tint, fade)

    return canvas + blur_rgb(canvas, 1.1 * SS) * 0.30 + bright + blur_rgb(bright, 2.0 * SS) * 0.75


def terrain(inside, lat, lon, rng):
    land_tex = fractal_noise(2048, 1024, 7, rng, cells=5)
    warp_tex = fractal_noise(2048, 1024, 4, rng, cells=3)
    relief_tex = ridged_noise(2048, 1024, 6, rng, cells=14)
    cloud_tex = fractal_noise(2048, 1024, 6, rng, cells=7)

    field = sample_equirect(land_tex, lon, lat) * 0.74 + sample_equirect(warp_tex, lon, lat) * 0.26
    land = np.clip((field - 0.474) * 15.0, 0.0, 1.0)
    relief = sample_equirect(relief_tex, lon, lat)

    banding = 0.52 + 0.48 * np.sin(lat * 7.4 + 0.6)
    cloud = np.clip((sample_equirect(cloud_tex, lon, lat) * banding - 0.30) * 4.6, 0.0, 1.0) * inside

    ocean = np.array([0.011, 0.024, 0.054], np.float32)
    ground = np.array([0.066, 0.068, 0.072], np.float32)
    surface = ocean[None, None, :] * (1.0 - land)[:, :, None] + ground[None, None, :] * land[:, :, None]
    surface += ((relief - 0.5) * 0.040)[:, :, None] * land[:, :, None]
    return surface, land, cloud


def cities(width: int, height: int, inside, land, facing, cloud, rng):
    canvas = np.zeros((height, width), np.float32)
    view = canvas[:, :, None]
    unit = np.array([1.0], np.float32)
    peak = float(np.max(facing * inside))
    squash_map = np.clip(facing / max(peak, 1.0e-4), 0.16, 1.0) ** 0.55

    centres = []
    attempts = 0
    while len(centres) < 300 and attempts < 240000:
        attempts += 1
        x = rng.random() * width
        y = (EARTH_TOP * SS) + rng.random() * (height - EARTH_TOP * SS)
        ix, iy = int(x), int(y)
        if not (0 <= ix < width and 0 <= iy < height) or inside[iy, ix] < 0.5:
            continue
        if land[iy, ix] < 0.12:
            continue
        centres.append((x, y, 0.12 + rng.random() ** 3.4 * 1.5))

    for x, y, power in centres:
        squash = float(np.clip(squash_map[int(y), int(x)], 0.16, 1.0))
        spread = (4.0 + 15.0 * power) * SS
        splat(view, x, y, unit, 2.4 * power + 0.5)
        for _ in range(int(50 + 380 * power ** 1.35)):
            angle = rng.random() * math.tau
            reach = rng.random() ** 1.8
            px = x + math.cos(angle) * reach * spread
            py = y + math.sin(angle) * reach * spread * squash
            ix, iy = int(px), int(py)
            if not (0 <= ix < width and 0 <= iy < height) or inside[iy, ix] < 0.5:
                continue
            splat(view, px, py, unit, (1.0 - reach) ** 2 * 1.6)

    for index, (x, y, power) in enumerate(centres):
        neighbours = sorted(
            (
                (math.hypot(other[0] - x, other[1] - y), other)
                for other in centres[index + 1:]
            ),
            key=lambda pair: pair[0],
        )[:3]
        for span, other in neighbours:
            if span > 74.0 * SS or span < 5.0 * SS:
                continue
            weight = 0.30 * min(power, other[2]) + 0.10
            steps = int(span * 2.0)
            for step in range(steps):
                t = step / steps
                wobble = math.sin(t * math.pi) * (rng.random() - 0.5) * 3.0 * SS
                px = x + (other[0] - x) * t
                py = y + (other[1] - y) * t + wobble * squash_map[int(y), int(x)]
                ix, iy = int(px), int(py)
                if not (0 <= ix < width and 0 <= iy < height) or inside[iy, ix] < 0.5:
                    continue
                splat(view, px, py, unit, weight * 0.5)

    canvas *= 1.0 - cloud * 0.78
    warm = np.array([1.0, 0.60, 0.19], np.float32)
    pale = np.array([1.0, 0.87, 0.64], np.float32)
    lit = canvas[:, :, None] * pale * 1.10
    lit += blur(canvas, 2.0 * SS)[:, :, None] * warm * 0.72
    lit += blur(canvas, 9.0 * SS)[:, :, None] * warm * 0.26
    return lit * inside[:, :, None], canvas


def cloud_deck(cloud, city_field, facing, inside) -> np.ndarray:
    under = blur(city_field, 7.0 * SS)
    moon = np.array([0.30, 0.38, 0.55], np.float32)
    ember = np.array([1.0, 0.62, 0.26], np.float32)
    rim = np.array([0.16, 0.30, 0.52], np.float32)
    lit = cloud[:, :, None] * moon * 0.105
    lit += (cloud * np.clip(under * 0.85, 0.0, 1.4))[:, :, None] * ember * 0.72
    lit += (cloud * np.clip(1.0 - facing * 3.2, 0.0, 1.0) ** 2)[:, :, None] * rim * 0.30
    return lit * inside[:, :, None]


def atmosphere(width: int, height: int, distance, radius) -> np.ndarray:
    offset = distance - radius
    outside = (offset > 0.0).astype(np.float32)
    within = (offset <= 0.0).astype(np.float32)
    columns = np.arange(width, dtype=np.float32)[None, :]
    arc = 0.52 + 0.48 * np.clip(1.0 - np.abs(columns - 0.46 * width) / (0.60 * width), 0.0, 1.0) ** 0.8

    shell = np.exp(-((offset / (1.15 * SS)) ** 2)) * arc
    inward = np.exp(-np.clip(-offset, 0.0, None) / (6.5 * SS)) * within
    outward = np.exp(-np.clip(offset, 0.0, None) / (7.0 * SS)) * outside * arc
    halo = np.exp(-np.clip(offset, 0.0, None) / (30.0 * SS)) * outside
    airglow = np.exp(-(((offset - 4.6 * SS) / (2.3 * SS)) ** 2)) * outside
    sodium = np.exp(-(((offset - 11.0 * SS) / (4.2 * SS)) ** 2)) * outside

    core = np.array([0.86, 0.94, 1.0], np.float32)
    sky_blue = np.array([0.36, 0.66, 1.0], np.float32)
    deep = np.array([0.14, 0.36, 0.92], np.float32)
    oxygen = np.array([0.26, 1.0, 0.66], np.float32)
    amber = np.array([1.0, 0.66, 0.34], np.float32)

    glow = shell[:, :, None] * core * 0.95
    glow += inward[:, :, None] * sky_blue * 0.55
    glow += outward[:, :, None] * sky_blue * 0.60
    glow += halo[:, :, None] * deep * 0.34
    glow += airglow[:, :, None] * oxygen * 0.26
    glow += sodium[:, :, None] * amber * 0.05
    return glow


def aurora(width: int, height: int, rng) -> np.ndarray:
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    base = limb_y(xx)

    envelope = fractal_noise(width, height, 3, rng, cells=4)[0:1, :]
    rays = fractal_noise(width, height, 3, rng, cells=150)[0:1, :]
    drape = fractal_noise(width, height, 3, rng, cells=26)[0:1, :]

    presence = np.clip(envelope * 2.8 - 0.80, 0.0, 1.0) ** 0.65
    texture = np.clip(rays * 1.5 - 0.32, 0.0, 1.0) ** 0.9
    strength = presence * (0.30 + 0.70 * texture) * (0.55 + 0.45 * drape)

    reach = (38.0 + 48.0 * drape) * SS
    above = np.clip((base - yy) / reach, 0.0, 1.0)
    profile = (1.0 - above) ** 2.0 * np.clip((base - yy) / (3.0 * SS), 0.0, 1.0)
    profile *= (yy < base).astype(np.float32)

    band = profile * strength
    green = np.array([0.20, 0.94, 0.46], np.float32)
    violet = np.array([0.44, 0.34, 0.90], np.float32)
    glow = band[:, :, None] * green
    glow += (band * above ** 2.2)[:, :, None] * violet * 0.34

    return glow * 0.38 + blur_rgb(glow, 1.6 * SS) * 0.66 + blur_rgb(glow, 7.0 * SS) * 0.26


def north_star(width: int, height: int) -> np.ndarray:
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    x = xx - STAR_X * SS
    y = yy - STAR_Y * SS
    reach = np.hypot(x, y)

    core = np.exp(-((reach / (2.6 * SS)) ** 2))
    vertical = np.exp(-(np.abs(x) / (0.95 * SS))) * np.clip(1.0 - np.abs(y) / (96.0 * SS), 0.0, 1.0) ** 2.0
    horizontal = np.exp(-(np.abs(y) / (0.95 * SS))) * np.clip(1.0 - np.abs(x) / (58.0 * SS), 0.0, 1.0) ** 2.0
    minor_a = np.exp(-(np.abs(x - y) / (1.7 * SS))) * np.clip(1.0 - reach / (22.0 * SS), 0.0, 1.0) ** 2.2
    minor_b = np.exp(-(np.abs(x + y) / (1.7 * SS))) * np.clip(1.0 - reach / (22.0 * SS), 0.0, 1.0) ** 2.2

    white = np.array([1.0, 1.0, 1.0], np.float32)
    pale = np.array([0.80, 0.90, 1.0], np.float32)

    glow = core[:, :, None] * white * 2.6
    glow += (vertical + horizontal)[:, :, None] * pale * 1.15
    glow += (minor_a + minor_b)[:, :, None] * pale * 0.55
    glow += np.exp(-(reach / (13.0 * SS)))[:, :, None] * np.array([0.52, 0.70, 1.0], np.float32) * 0.62
    glow += np.exp(-(reach / (46.0 * SS)))[:, :, None] * np.array([0.20, 0.40, 0.90], np.float32) * 0.30
    return glow


def cruiser(draw: ImageDraw.ImageDraw, x: float, y: float, length: float, facing: int) -> None:
    unit = length / 10.0
    step = facing * unit
    hull = (26, 33, 46, 255)
    deck = (44, 55, 74, 255)

    draw.polygon(
        [
            (x + step * 5.0, y - unit * 0.10),
            (x + step * 3.4, y - unit * 0.66),
            (x - step * 1.2, y - unit * 0.90),
            (x - step * 4.3, y - unit * 0.78),
            (x - step * 4.7, y + unit * 0.20),
            (x - step * 4.0, y + unit * 0.86),
            (x + step * 1.2, y + unit * 0.88),
            (x + step * 3.6, y + unit * 0.44),
        ],
        fill=hull,
    )
    draw.polygon(
        [
            (x - step * 0.4, y - unit * 0.86),
            (x - step * 1.9, y - unit * 1.46),
            (x - step * 3.6, y - unit * 1.42),
            (x - step * 4.2, y - unit * 0.78),
        ],
        fill=deck,
    )
    draw.polygon(
        [
            (x - step * 4.0, y - unit * 0.70),
            (x - step * 4.9, y - unit * 0.55),
            (x - step * 4.9, y + unit * 0.60),
            (x - step * 4.0, y + unit * 0.72),
        ],
        fill=deck,
    )
    draw.line(
        [(x + step * 3.7, y - unit * 0.28), (x + step * 2.3, y - unit * 0.52)],
        fill=(160, 232, 255, 255),
        width=max(1, int(unit * 0.36)),
    )


def spaceships(width: int, height: int):
    layer = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    emit = np.zeros((height, width, 3), np.float32)

    fleet = (
        (208.0, 170.0, 40.0, 1),
        (614.0, 146.0, 21.0, 1),
        (886.0, 194.0, 14.0, -1),
    )
    for x, y, length, facing in fleet:
        px, py, plen = x * SS, y * SS, length * SS
        cruiser(draw, px, py, plen, facing)
        unit = plen / 10.0
        scale = (plen / (40.0 * SS)) ** 0.6
        for offset in (-unit * 0.34, unit * 0.42):
            ex = px - facing * unit * 5.0
            ey = py + offset
            splat(emit, ex, ey, np.array([0.86, 0.95, 1.0], np.float32), 4.2 * scale)
            trail = int(unit * 11.0)
            for step in range(1, trail):
                fade = (1.0 - step / trail) ** 2.8
                splat(emit, ex - facing * step, ey, np.array([0.40, 0.72, 1.0], np.float32), fade * 0.75 * scale)
        splat(emit, px + facing * unit * 4.4, py - unit * 0.36, np.array([0.55, 0.95, 1.0], np.float32), 1.2 * scale)
        splat(emit, px - facing * unit * 2.7, py - unit * 2.0, np.array([1.0, 0.30, 0.28], np.float32), 0.9 * scale)
        splat(emit, px + facing * unit * 1.2, py + unit * 0.9, np.array([0.32, 1.0, 0.42], np.float32), 0.9 * scale)

    body = np.asarray(layer, np.float32) / 255.0
    alpha = body[:, :, 3:4]
    edges = np.asarray(layer.split()[3].filter(ImageFilter.FIND_EDGES), np.float32) / 255.0
    rim = (edges * alpha[:, :, 0])[:, :, None] * np.array([0.34, 0.50, 0.78], np.float32) * 0.85
    return body[:, :, :3], alpha, rim + emit + blur_rgb(emit, 3.0 * SS) * 0.85


def titles(width: int, height: int, version: str):
    layer = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    draw = ImageDraw.Draw(layer)
    title_font = load_font(int(48 * SS))
    sub_font = load_font(int(25 * SS))

    name = "Nordstjernen"
    x = 62.0 * SS
    y = 38.0 * SS
    draw.text((x, y), name, font=title_font, fill=(240, 246, 253, 255))
    draw.text(
        (x + draw.textlength(name + " ", font=title_font), y),
        version,
        font=title_font,
        fill=(255, 172, 24, 255),
    )
    draw.text((64.0 * SS, 104.0 * SS), "Nordstjernen Navigator", font=sub_font, fill=(122, 172, 232, 255))

    text = np.asarray(layer, np.float32) / 255.0
    alpha = text[:, :, 3:4]
    shadow = blur(alpha[:, :, 0], 3.4 * SS)[:, :, None]
    return text[:, :, :3], alpha, shadow


def frame(canvas: np.ndarray) -> np.ndarray:
    height, width = canvas.shape[:2]
    yy, xx = np.mgrid[0:height, 0:width].astype(np.float32)
    nx = (xx - width / 2.0) / (width / 2.0)
    ny = (yy - height / 2.0) / (height / 2.0)
    canvas *= np.clip(1.0 - 0.26 * (nx ** 2 + ny ** 2) ** 1.5, 0.0, 1.0)[:, :, None]

    edge = int(round(2 * SS))
    inner = int(round(1 * SS))
    canvas[:edge, :, :] = 0.04
    canvas[-edge:, :, :] = 0.04
    canvas[:, :edge, :] = 0.04
    canvas[:, -edge:, :] = 0.04
    rail = np.array([0.30, 0.38, 0.50], np.float32)
    canvas[edge:edge + inner, edge:-edge, :] = rail
    canvas[-edge - inner:-edge, edge:-edge, :] = rail
    canvas[edge:-edge, edge:edge + inner, :] = rail
    canvas[edge:-edge, -edge - inner:-edge, :] = rail
    return canvas


def render(version: str) -> Image.Image:
    width, height = WIDTH * SS, HEIGHT * SS
    rng = np.random.default_rng(SEED)

    canvas = sky(width, height, rng)
    canvas += starfield(width, height, rng)

    inside, facing, lat, lon, distance, radius = globe(width, height)
    surface, land, cloud = terrain(inside, lat, lon, rng)
    canvas = canvas * (1.0 - inside[:, :, None]) + surface * inside[:, :, None]

    lights, city_field = cities(width, height, inside, land, facing, cloud, rng)
    canvas += cloud_deck(cloud, city_field, facing, inside)
    canvas += lights
    canvas += atmosphere(width, height, distance, radius)
    canvas += aurora(width, height, rng)
    canvas += north_star(width, height)

    hull, ship_alpha, ship_light = spaceships(width, height)
    canvas = canvas * (1.0 - ship_alpha) + hull * ship_alpha
    canvas += ship_light

    text, text_alpha, shadow = titles(width, height, version)
    canvas *= 1.0 - shadow * 0.80
    canvas = canvas * (1.0 - text_alpha) + text * text_alpha
    canvas += blur(text_alpha[:, :, 0], 2.6 * SS)[:, :, None] * np.array([0.14, 0.22, 0.40], np.float32)

    canvas = frame(np.clip(canvas, 0.0, 1.0))
    image = Image.fromarray((np.clip(canvas, 0.0, 1.0) * 255.0 + 0.5).astype(np.uint8), "RGB")
    return image.resize((WIDTH, HEIGHT), Image.Resampling.LANCZOS)


def main() -> None:
    version = project_version()
    output = ROOT / "data" / f"about-splash-{version}.png"
    render(version).save(output, optimize=True)
    print(f"wrote {output}")


if __name__ == "__main__":
    main()
