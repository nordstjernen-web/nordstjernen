#!/usr/bin/env python3
"""Compose the Nordstjernen architecture poster from graphviz pieces."""

import math
from PIL import Image, ImageDraw, ImageFont

FD = "/usr/share/fonts/truetype/dejavu/"
BG = "#FDFDFB"

COL = {
    "spawn": "#C0392B",
    "ipc": "#2C5FA8",
    "px": "#1E7A3C",
    "media": "#B8860B",
    "thin": "#777777",
}


class Piece:
    def __init__(self, name):
        self.name = name
        self.img = Image.open("piece_%s.png" % name).convert("RGBA")
        self.w, self.h = self.img.size
        self.nodes = {}
        gw = gh = 1.0
        for line in open("piece_%s.plain" % name):
            f = line.split()
            if not f:
                continue
            if f[0] == "graph":
                gw, gh = float(f[2]), float(f[3])
            elif f[0] == "node":
                self.nodes[f[1]] = (float(f[2]), float(f[3]),
                                    float(f[4]), float(f[5]))
        self.fx = self.w / gw
        self.fy = self.h / gh
        self.gh = gh
        self.x = 0
        self.y = 0

    def rect(self, node):
        cx, cy, w, h = self.nodes[node]
        px = self.x + cx * self.fx
        py = self.y + (self.gh - cy) * self.fy
        return (px - w * self.fx / 2, py - h * self.fy / 2,
                px + w * self.fx / 2, py + h * self.fy / 2)

    def pt(self, node, side, frac=0.5):
        x0, y0, x1, y1 = self.rect(node)
        if side == "top":
            return (x0 + (x1 - x0) * frac, y0)
        if side == "bottom":
            return (x0 + (x1 - x0) * frac, y1)
        if side == "left":
            return (x0, y0 + (y1 - y0) * frac)
        return (x1, y0 + (y1 - y0) * frac)


def fillet(points, radius=45, step=6):
    pts = [tuple(map(float, p)) for p in points]
    out = [pts[0]]
    for i in range(1, len(pts) - 1):
        p0, p1, p2 = out[-1], pts[i], pts[i + 1]
        d0 = math.hypot(p1[0] - p0[0], p1[1] - p0[1])
        d2 = math.hypot(p2[0] - p1[0], p2[1] - p1[1])
        r = min(radius, d0 / 2, d2 / 2)
        if r < 2:
            out.append(p1)
            continue
        a = (p1[0] - (p1[0] - p0[0]) / d0 * r, p1[1] - (p1[1] - p0[1]) / d0 * r)
        b = (p1[0] + (p2[0] - p1[0]) / d2 * r, p1[1] + (p2[1] - p1[1]) / d2 * r)
        out.append(a)
        n = max(3, int(r / step))
        for t in (j / n for j in range(1, n)):
            x = ((1 - t) ** 2 * a[0] + 2 * (1 - t) * t * p1[0] + t * t * b[0])
            y = ((1 - t) ** 2 * a[1] + 2 * (1 - t) * t * p1[1] + t * t * b[1])
            out.append((x, y))
        out.append(b)
    out.append(pts[-1])
    return out


def densify(pts, step=4.0):
    out = [pts[0]]
    for p in pts[1:]:
        q = out[-1]
        d = math.hypot(p[0] - q[0], p[1] - q[1])
        n = max(1, int(d / step))
        for j in range(1, n + 1):
            out.append((q[0] + (p[0] - q[0]) * j / n,
                        q[1] + (p[1] - q[1]) * j / n))
    return out


def arrow(draw, waypoints, color, width=4, dash=None, head=22):
    pts = densify(fillet(waypoints))
    if dash:
        on, off = dash
        dist = 0.0
        pen = True
        seg = []
        for i in range(1, len(pts)):
            seg.append(pts[i - 1])
            dist += math.hypot(pts[i][0] - pts[i - 1][0],
                               pts[i][1] - pts[i - 1][1])
            lim = on if pen else off
            if dist >= lim:
                seg.append(pts[i])
                if pen:
                    draw.line(seg, fill=color, width=width, joint="curve")
                seg = []
                dist = 0.0
                pen = not pen
        if pen and seg:
            seg.append(pts[-1])
            draw.line(seg, fill=color, width=width, joint="curve")
    else:
        draw.line(pts, fill=color, width=width, joint="curve")
    tip = pts[-1]
    back = pts[max(0, len(pts) - 8)]
    ang = math.atan2(tip[1] - back[1], tip[0] - back[0])
    hw = head * 0.42
    p1 = (tip[0] - head * math.cos(ang) + hw * math.sin(ang),
          tip[1] - head * math.sin(ang) - hw * math.cos(ang))
    p2 = (tip[0] - head * math.cos(ang) - hw * math.sin(ang),
          tip[1] - head * math.sin(ang) + hw * math.cos(ang))
    draw.polygon([tip, p1, p2], fill=color)


FONTS = {}


def font(size, bold=False):
    key = (size, bold)
    if key not in FONTS:
        FONTS[key] = ImageFont.truetype(
            FD + ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"), size)
    return FONTS[key]


def label(draw, xy, text, color, size=24, anchor="la", bold=False, bg=True):
    fnt = font(size, bold)
    box = draw.multiline_textbbox(xy, text, font=fnt, anchor=anchor, spacing=5)
    if bg:
        draw.rectangle((box[0] - 7, box[1] - 4, box[2] + 7, box[3] + 4),
                       fill=(253, 253, 251, 235))
    draw.multiline_text(xy, text, font=fnt, fill=color, anchor=anchor,
                        spacing=5)


def main():
    P = {n: Piece(n) for n in ["watchdog", "shell", "renderer", "audio",
                               "video", "shmfb", "legend", "syslibs"]}
    M = 200
    TH = 230
    LW = max(P[n].w for n in ["watchdog", "shell", "audio", "video"])

    def place(p, x, y):
        p.x, p.y = x, y

    place(P["watchdog"], M + (LW - P["watchdog"].w) / 2, TH)
    place(P["shell"], M + (LW - P["shell"].w) / 2, TH + P["watchdog"].h + 150)
    sh = P["shell"]
    shell_bot = sh.y + sh.h
    place(P["shmfb"], M + 376, shell_bot + 170)
    place(P["audio"], M + (LW - P["audio"].w) / 2,
          P["shmfb"].y + P["shmfb"].h + 170)
    place(P["video"], M + (LW - P["video"].w) / 2,
          P["audio"].y + P["audio"].h + 90)
    place(P["legend"], M, P["video"].y + P["video"].h + 120)
    place(P["syslibs"], M, P["legend"].y + P["legend"].h + 40)

    GAP = 560
    XR = M + LW + GAP
    place(P["renderer"], XR, TH + 30)
    rend = P["renderer"]

    W = int(XR + rend.w + 100)
    H = int(max(P["syslibs"].y + P["syslibs"].h,
                rend.y + rend.h + 170) + 120)

    img = Image.new("RGBA", (W, H), BG)
    draw = ImageDraw.Draw(img)

    label(draw, (W / 2, 60),
          "Nordstjernen Web Navigator — Software Architecture",
          "#111111", size=64, anchor="mm", bold=True, bg=False)
    label(draw, (W / 2, 130),
          "processes · source modules · dependencies · "
          "information flows    (snapshot 1.0.21-dev, 2026-07-27)",
          "#444444", size=30, anchor="mm", bg=False)

    for p in P.values():
        img.paste(p.img, (int(p.x), int(p.y)), p.img)
    draw = ImageDraw.Draw(img)

    wd = P["watchdog"]
    au = P["audio"]
    vi = P["video"]
    fb = P["shmfb"]

    LABELS = []

    def lab(*a, **kw):
        LABELS.append((a, kw))

    # A: watchdog -> shell (spawn)
    a0 = wd.pt("wd_sup", "bottom", 0.35)
    a1 = sh.pt("appmain", "top", 0.5)
    arrow(draw, [a0, (a1[0], a0[1] + 60), a1], COL["spawn"], 4, dash=(20, 13))
    lab((a1[0] + 26, (a0[1] + a1[1]) / 2 + 4),
        "fork/exec the GUI shell; restart on\ncrash or hang (max 5 / 60 s)",
        COL["spawn"], anchor="lm")

    # B: heartbeat back
    b0 = sh.pt("wd_hb", "top", 0.5)
    b1 = wd.pt("wd_sup", "bottom", 0.8)
    arrow(draw, [b0, (b1[0], b0[1] - 60), b1], COL["spawn"], 3, dash=(6, 9))
    lab((b0[0] + 34, b0[1] - 76), "heartbeat → non-zero\nexit on wedge",
        COL["spawn"], size=21, anchor="lm")

    # gap lanes: climbs hug the right side of the gap
    xc, xd, xe = XR - 96, XR - 61, XR - 26

    # C: procview -> renderer (spawn)
    c0 = sh.pt("procview", "right", 0.25)
    c1 = rend.pt("renderer_http", "left", 0.3)
    arrow(draw, [c0, (xc, c0[1]), (xc, c1[1]), c1],
          COL["spawn"], 4, dash=(20, 13))
    lab((sh.x + sh.w + 10, c0[1] - 180),
        "fork+exec one renderer\nper tab; transparent\nrestart on crash",
        COL["spawn"], anchor="lm")

    # D: ipc_http -> renderer_http (IPC control)
    d0 = sh.pt("ipc_http", "right", 0.5)
    d1 = rend.pt("renderer_http", "left", 0.75)
    arrow(draw, [d0, (xd, d0[1]), (xd, d1[1]), d1], COL["ipc"], 7)
    lab((sh.x + sh.w + 10, (c0[1] + d0[1]) / 2 - 55),
        "HTTP/1.1 + JSON control\nchannel — AF_UNIX\nsocketpair (fd 3)\n"
        "POST /open /render\n/click /key /scroll\n/eval /find /media …",
        COL["ipc"], size=24, anchor="lm", bold=True)

    # E: renderer_serve reply -> rproc_http
    e0 = rend.pt("renderer_serve", "left", 0.5)
    e1 = sh.pt("rproc_http", "right", 0.4)
    arrow(draw, [e0, (xe, e0[1]), (xe, e1[1]), e1],
          COL["ipc"], 4, dash=(18, 12))
    lab((sh.x + sh.w + 10, e1[1] + 118),
        "replies: X-W X-H X-Stride\nX-Anim geometry +\nside-channels "
        "X-Nav\nX-Audio X-WebGL\nX-Camera X-Download",
        COL["ipc"], size=23, anchor="lm")

    # G: paint -> shm framebuffer
    g0 = rend.pt("paint", "bottom", 0.5)
    below = rend.y + rend.h + 90
    g1 = fb.pt("shmfb", "right", 0.55)
    arrow(draw, [g0, (g0[0], below), (XR - 260, below),
                 (XR - 260, g1[1]), g1], COL["px"], 7)
    lab((g0[0] + 26, below - 40),
        "Cairo ARGB32 frame painted straight into the shared buffer "
        "(ns_browser_render_argb32)", COL["px"], size=25, bold=True,
        anchor="lm")

    # H: shm framebuffer -> procview (blit)
    h0 = fb.pt("shmfb", "top", 0.8)
    h1 = sh.pt("procview", "right", 0.75)
    hx = sh.x + sh.w - 90
    arrow(draw, [(h0[0], h0[1]), (h0[0] + 210, h0[1] - 90),
                 (hx + 160, h1[1] + 120), (h1[0] + 90, h1[1]), h1],
          COL["px"], 7)
    lab((h0[0] + 110, h0[1] - 128), "blit to the GTK\nwidget each frame",
        COL["px"], size=23, anchor="lm", bold=True)

    # I/K: spawn lanes to helpers (left margin)
    i0 = sh.pt("procview", "left", 0.35)
    i1 = (au.x, au.y + au.h * 0.45)
    k1 = (vi.x, vi.y + vi.h * 0.35)
    arrow(draw, [i0, (M - 90, i0[1]), (M - 90, i1[1] - 40), i1],
          COL["spawn"], 4, dash=(20, 13))
    arrow(draw, [(M - 90, i1[1] - 40), (M - 90, k1[1] - 40), k1],
          COL["spawn"], 4, dash=(20, 13))
    # J/L: media command lanes
    j0 = sh.pt("procview", "left", 0.7)
    j1 = (au.x, au.y + au.h * 0.72)
    l1 = (vi.x, vi.y + vi.h * 0.55)
    arrow(draw, [j0, (M - 150, j0[1]), (M - 150, j1[1] - 40), j1],
          COL["media"], 5)
    arrow(draw, [(M - 150, j1[1] - 40), (M - 150, l1[1] - 40), l1],
          COL["media"], 5)
    lab((M - 130, au.y - 118),
        "spawn (lazy, per tab)  +  stdin/stdout text protocol:\n"
        "open · play · pause · seek · loop · volume",
        "#8B6508", size=22, anchor="la")

    # N: vshm -> shell (composite)
    n0 = vi.pt("vshm", "right", 0.5)
    n1 = (sh.x + sh.w * 0.62, sh.y + sh.h)
    arrow(draw, [n0, (M + LW + 150, n0[1]), (M + LW + 150, n1[1] + 210),
                 (n1[0], n1[1] + 90), n1], COL["media"], 5)
    lab((M + LW + 40, n1[1] + 330),
        "video frames composited\nover the page surface\neach tick "
        "(procview.c)", COL["media"], size=23, anchor="lm")

    # O/P: audio helper -> vendored decoders
    o0 = au.pt("audio_main", "right", 0.75)
    pl = rend.rect("plmpeg")
    mp = rend.rect("minimp3")
    laneY = rend.y + rend.h + 150
    arrow(draw, [o0, (M + LW + 260, o0[1]), (M + LW + 260, laneY),
                 ((pl[0] + pl[2]) / 2, laneY),
                 ((pl[0] + pl[2]) / 2, pl[3])], COL["thin"], 3)
    arrow(draw, [(M + LW + 260, laneY), ((mp[0] + mp[2]) / 2, laneY),
                 ((mp[0] + mp[2]) / 2, mp[3])], COL["thin"], 3)
    lab((M + LW + 320, laneY - 36),
        "audio helper decodes with the same vendored codecs: "
        "pl_mpeg (MPEG-1/MP2) · minimp3 (MP3)",
        "#666666", size=23, anchor="lm")

    for a, kw in LABELS:
        label(draw, *a, **kw)

    img = img.convert("RGB")
    img.save("nordstjernen-architecture.png", optimize=True)
    print("poster:", img.size)


if __name__ == "__main__":
    main()
