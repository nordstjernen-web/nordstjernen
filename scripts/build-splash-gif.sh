#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

ver=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" meson.build | head -n1)
[ -n "$ver" ] || { echo "could not read version from meson.build" >&2; exit 1; }
ver=${ver%%-*}
codename='Nordstjernen Web Browser is not Firefox or Chrome.
Possibly the best web browser in the world'

FRAMES=${NS_SPLASH_FRAMES:-48}
DELAY=${NS_SPLASH_DELAY:-8}
LEVELS=${NS_SPLASH_LEVELS:-6}

find_font() {
    local q=$1; shift
    if command -v fc-match >/dev/null 2>&1; then
        local f; f=$(fc-match -f '%{file}' "$q" 2>/dev/null || true)
        [ -n "$f" ] && [ -f "$f" ] && { echo "$f"; return 0; }
    fi
    local p
    for p in "$@"; do [ -f "$p" ] && { echo "$p"; return 0; }; done
    echo "missing font: $q" >&2; return 1
}
fr=$(find_font 'Liberation Sans' \
    /usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf \
    /usr/share/fonts/liberation-sans/LiberationSans-Regular.ttf \
    /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf)
fb=$(find_font 'Liberation Sans:bold' \
    /usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf \
    /usr/share/fonts/liberation-sans/LiberationSans-Bold.ttf \
    /usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf)

if [ -n "${NS_SPLASH_WORKDIR:-}" ]; then
    w=$NS_SPLASH_WORKDIR; mkdir -p "$w"
else
    w=$(mktemp -d)
    trap 'rm -rf "$w"' EXIT
fi

S=3
W=$((940 * S)); H=$((320 * S))
HORIZON=$(python3 -c "print(int($H*0.680))")

convert -size ${W}x${H} gradient:'#000007'-'#071747' "$w/sky0.png"
convert -size ${W}x${H} xc:none -fill '#123a8e' \
    -draw "ellipse $((W/2)),${HORIZON} $((W)),$((62*S)) 0,360" -blur 0x$((46*S)) "$w/skyglow.png"
convert "$w/sky0.png" "$w/skyglow.png" -compose screen -composite "$w/sky.png"

python3 - "$W" "$H" "$S" > "$w/stars.mvg" <<'PY'
import sys, math, random
W, H, S = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
rnd = random.Random(1997)
out = []
for _ in range(340):
    x = rnd.uniform(0, W); y = rnd.uniform(0, H*0.66)
    if y > H*0.52 and rnd.random() < 0.55:
        continue
    r = S*rnd.choice([0.5, 0.5, 0.5, 1.0, 1.0, 1.5])
    c = rnd.choice(["#8fa6d8", "#6d84b8", "#b9c8ec", "#9aaee0", "#7b93c8"])
    out.append("fill %s stroke none rectangle %.0f,%.0f %.0f,%.0f" % (c, x, y, x+r*2, y+r*2))
sys.stdout.write(" ".join(out))
PY

PLANET_CY=$(python3 -c "print(int($HORIZON + $H*2.05))")
PLANET_RX=$(python3 -c "print(int($W*1.28))")
PLANET_RY=$(python3 -c "print(int($H*2.05))")

convert -size ${W}x${H} xc:black -fill white \
    -draw "ellipse $((W/2)),${PLANET_CY} ${PLANET_RX},${PLANET_RY} 0,360" \
    -alpha off "$w/planetmask.png"

python3 - "$W" "$H" "$S" "$HORIZON" > "$w/grid.mvg" <<'PY'
import sys, math
W, H, S = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
HZ = int(sys.argv[4])
out = []
vx, vy = W*0.5, HZ

def shade(t, base):
    v = 0.16 + 0.84*t**1.5
    return "#%02x%02x%02x" % (int(base[0]*v), int(base[1]*v), int(base[2]*v))

NSEG = 14
for i in range(-16, 17):
    x1 = vx + i*W*0.118
    x0 = vx + i*W*0.006
    for k in range(NSEG):
        t0 = k/NSEG; t1 = (k + 1)/NSEG
        out.append("stroke %s stroke-width %.1f fill none line %.1f,%.1f %.1f,%.1f" % (
            shade((t0 + t1)/2, (36, 92, 190)), 1.0*S,
            x0 + (x1 - x0)*t0, vy + (H*1.02 - vy)*t0,
            x0 + (x1 - x0)*t1, vy + (H*1.02 - vy)*t1))
y = HZ
step = H*0.005
while y < H*1.02:
    y += step
    step *= 1.44
    t = (y - HZ)/(H*1.02 - HZ)
    out.append("stroke %s stroke-width %.1f fill none line 0,%.1f %d,%.1f" % (
        shade(t, (32, 84, 176), ), 1.0*S, y, W, y))
sys.stdout.write(" ".join(out))
PY

convert -size ${W}x${H} xc:black -draw "$(cat "$w/grid.mvg")" "$w/ground0.png"
convert "$w/ground0.png" "$w/planetmask.png" -alpha off -compose CopyOpacity -composite "$w/ground.png"

convert -size ${W}x${H} xc:none -fill none -stroke '#7fe0ff' -strokewidth $((5*S)) \
    -draw "ellipse $((W/2)),${PLANET_CY} ${PLANET_RX},${PLANET_RY} 0,360" \
    -blur 0x$((3*S)) "$w/limb.png"
convert -size ${W}x${H} xc:none -fill none -stroke '#2ea8ff' -strokewidth $((16*S)) \
    -draw "ellipse $((W/2)),${PLANET_CY} ${PLANET_RX},${PLANET_RY} 0,360" \
    -blur 0x$((26*S)) "$w/limbglow.png"

convert "$w/sky.png" -draw "$(cat "$w/stars.mvg")" \
    "$w/ground.png" -compose over -composite \
    "$w/limbglow.png" -compose screen -composite \
    "$w/limb.png" -compose screen -composite "$w/bg.png"

SX=$(python3 -c "print(int($W*0.800))"); SY=$(python3 -c "print(int($H*0.360))")
convert -size ${W}x${H} xc:none -fill '#4f9ce8' \
    -draw "ellipse ${SX},${SY} $((30*S)),$((30*S)) 0,360" -blur 0x$((30*S)) "$w/halo.png"
convert -size ${W}x${H} xc:none -fill '#dcefff' \
    -draw "ellipse ${SX},${SY} $((15*S)),$((15*S)) 0,360" -blur 0x$((10*S)) "$w/halo2.png"
convert "$w/halo.png" "$w/halo2.png" -compose over -composite "$w/starhalo.png"

P() { echo $(( $1 * S )); }
convert -background none -font "$fb" -pointsize $(P 52) -kerning $((1*S)) -fill '#ffffff' label:'Nordstjernen ' "$w/t1.png"
convert -background none -font "$fb" -pointsize $(P 52) -fill '#ffcc33' label:"$ver" "$w/t2.png"
convert -background none -font "$fb" -pointsize $(P 24) -fill '#7fd4ff' label:'Nordstjernen Web Browser' "$w/ts.png"
convert -background none -font "$fr" -pointsize $(P 20) -fill '#dfe9f7' -size $((700*S))x caption:"$codename" "$w/tc.png"

for n in t1 t2 ts tc; do
    convert "$w/$n.png" -fill '#000616' -colorize 100 "$w/${n}k.png"
done
for n in t1 t2; do
    convert "$w/$n.png" -fill '#5f86c8' -colorize 100 "$w/${n}h.png"
done

w1=$(identify -format '%w' "$w/t1.png"); h1=$(identify -format '%h' "$w/t1.png")
hc=$(identify -format '%h' "$w/tc.png")
ty=$((42*S)); textleft=$((70*S))
sy=$((ty + h1 + 10*S)); cy=$(( H - hc - 106*S ))
sh=$((2*S))

convert -size ${W}x${H} xc:none \
    "$w/t1k.png" -gravity NorthWest -geometry +$((textleft + sh))+$((ty + sh)) -compose over -composite \
    "$w/t2k.png" -gravity NorthWest -geometry +$((textleft + w1 + sh))+$((ty + sh)) -compose over -composite \
    "$w/tsk.png" -gravity NorthWest -geometry +$((textleft + sh))+$((sy + sh)) -compose over -composite \
    "$w/tck.png" -gravity NorthWest -geometry +$((textleft + sh))+$((cy + sh)) -compose over -composite \
    "$w/t1h.png" -gravity NorthWest -geometry +$((textleft - S))+$((ty - S)) -compose over -composite \
    "$w/t2h.png" -gravity NorthWest -geometry +$((textleft + w1 - S))+$((ty - S)) -compose over -composite \
    "$w/t1.png"  -gravity NorthWest -geometry +${textleft}+${ty} -compose over -composite \
    "$w/t2.png"  -gravity NorthWest -geometry +$((textleft + w1))+${ty} -compose over -composite \
    "$w/ts.png"  -gravity NorthWest -geometry +${textleft}+${sy} -compose over -composite \
    "$w/tc.png"  -gravity NorthWest -geometry +${textleft}+${cy} -compose over -composite \
    "$w/textlayer.png"

python3 - "$W" "$H" "$S" > "$w/frame.mvg" <<'PY'
import sys
W, H, S = int(sys.argv[1]), int(sys.argv[2]), int(sys.argv[3])
out = []
def band(i, col):
    out.append("fill none stroke %s stroke-width %d rectangle %.1f,%.1f %.1f,%.1f" % (
        col, S, i*S + S*0.5, i*S + S*0.5, W - i*S - S*0.5, H - i*S - S*0.5))
def edge(i, tl, br):
    o = i*S
    out.append("fill %s stroke none polygon %d,%d %d,%d %d,%d %d,%d %d,%d %d,%d" % (
        tl, o, o, W-o, o, W-o-S, o+S, o+S, o+S, o+S, H-o-S, o, H-o))
    out.append("fill %s stroke none polygon %d,%d %d,%d %d,%d %d,%d %d,%d %d,%d" % (
        br, W-o, o, W-o, H-o, o, H-o, o+S, H-o-S, W-o-S, H-o-S, W-o-S, o+S))
band(0, "#000000")
edge(1, "#f4f4f4", "#5a5a5a")
band(2, "#c6c6c6")
band(3, "#c6c6c6")
band(4, "#c6c6c6")
edge(5, "#5a5a5a", "#f4f4f4")
band(7, "#000000")
sys.stdout.write(" ".join(out))
PY

cat > "$w/anim.py" <<'PY'
import sys, math, random

W, H, S = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
T = float(sys.argv[4]); wd = sys.argv[5]; idx = int(sys.argv[6])
SX, SY = W*0.800, H*0.360
TAU = 2*math.pi
out = []
chrome = []

def rect(col, x0, y0, x1, y1, dst=None):
    (out if dst is None else dst).append(
        "fill %s stroke none rectangle %.1f,%.1f %.1f,%.1f" % (col, x0, y0, x1, y1))

def ell(col, cx, cy, rx, ry):
    out.append("fill %s stroke none ellipse %.1f,%.1f %.1f,%.1f 0,360" % (col, cx, cy, rx, ry))

def poly(col, pts):
    out.append("fill %s stroke none polygon %s" % (col, " ".join("%.1f,%.1f" % p for p in pts)))

def hx(c):
    return "#%02x%02x%02x" % tuple(int(max(0, min(255, v))) for v in c)

rnd = random.Random(4242)
for _ in range(34):
    x = rnd.uniform(W*0.02, W*0.98); y = rnd.uniform(H*0.03, H*0.55)
    ph = rnd.uniform(0, TAU); sp = rnd.choice([1.0, 1.0, 2.0, 3.0])
    tw = 0.5 + 0.5*math.sin(TAU*T*sp + ph)
    v = 0.30 + 0.70*tw
    c = hx((255*v, 255*v, 235*v))
    r = S*(0.5 + 1.0*tw)
    rect(c, x, y, x + r*2, y + r*2)
    if tw > 0.86:
        rect(c, x - r*2, y + r*0.6, x + r*4, y + r*1.4)
        rect(c, x + r*0.6, y - r*2, x + r*1.4, y + r*4)

def rgba(c, a):
    return "rgba(%d,%d,%d,%.3f)" % (c[0], c[1], c[2], max(0.0, min(1.0, a)))

def meteor(phase, x0, y0, x1, y1, ln, col):
    p = phase % 1.0
    x = x0 + (x1 - x0)*p; y = y0 + (y1 - y0)*p
    dx = x1 - x0; dy = y1 - y0
    L = math.hypot(dx, dy) or 1.0
    ux, uy = dx/L, dy/L
    fade = min(1.0, min(p, 1.0 - p)/0.14)
    if fade <= 0.02:
        return
    for k in range(9):
        t0 = k/9.0; t1 = (k + 1)/9.0
        a = (1.0 - t0)**2.0*fade*0.85
        hwid = S*2.2*(1.0 - t0)**0.6
        ax, ay = x - ux*ln*t0, y - uy*ln*t0
        bx, by = x - ux*ln*t1, y - uy*ln*t1
        poly(rgba(col, a), [(ax - uy*hwid, ay + ux*hwid), (ax + uy*hwid, ay - ux*hwid),
                            (bx + uy*hwid*0.5, by - ux*hwid*0.5),
                            (bx - uy*hwid*0.5, by + ux*hwid*0.5)])
    ell(rgba((150, 200, 255), fade*0.45), x, y, S*5.0, S*5.0)
    ell(rgba((235, 245, 255), fade*0.85), x, y, S*2.6, S*2.6)
    ell(rgba((255, 255, 255), fade), x, y, S*1.3, S*1.3)

meteor(T*1.00 + 0.05, W*1.06, -H*0.10, W*0.34, H*0.52, H*0.30, (150, 200, 255))
meteor(T*1.00 + 0.48, W*1.14, H*0.10, W*0.52, H*0.62, H*0.24, (190, 215, 255))
meteor(T*0.75 + 0.30, W*0.62, -H*0.14, W*0.06, H*0.34, H*0.22, (255, 230, 180))
meteor(T*1.30 + 0.72, W*1.02, H*0.26, W*0.60, H*0.66, H*0.18, (160, 235, 255))

pulse = 0.72 + 0.28*math.sin(TAU*T)
fx, fy = W*0.5, H*0.5
for t, rad, col, a in [(0.42, 0.020, (120, 220, 255), 0.20), (0.70, 0.034, (170, 130, 255), 0.16),
                       (1.16, 0.026, (255, 190, 120), 0.18), (1.42, 0.052, (90, 170, 255), 0.11),
                       (1.78, 0.016, (255, 240, 190), 0.22)]:
    px = SX + (fx - SX)*t; py = SY + (fy - SY)*t
    ell(rgba(col, a*(0.6 + 0.4*pulse)), px, py, H*rad*1.05, H*rad)

def spike(ang, ln, hwid, col):
    a = math.radians(ang)
    tip = (SX + math.cos(a)*ln, SY + math.sin(a)*ln)
    px, py = math.cos(a + math.pi/2)*hwid, math.sin(a + math.pi/2)*hwid
    poly(col, [tip, (SX + px, SY + py), (SX - px, SY - py)])

for ang in (0, 90, 180, 270):
    spike(ang, H*0.190*pulse, S*3.6, "#cfe6ff")
for ang in (45, 135, 225, 315):
    spike(ang, H*0.078*pulse, S*2.6, "#8fbdf0")
for ang in (0, 90, 180, 270):
    spike(ang, H*0.132*pulse, S*1.7, "#ffffff")
ell("#eaf4ff", SX, SY, S*8.6*pulse, S*8.6*pulse)
ell("#ffffff", SX, SY, S*5.0*pulse, S*5.0*pulse)

bx0 = W*0.075; bx1 = W*0.925
by0 = H - 52.0*S; by1 = H - 32.0*S
rect("#05070f", bx0, by0, bx1, by1, chrome)
rect("#141d33", bx0, by0, bx1, by0 + S, chrome)
rect("#141d33", bx0, by0, bx0 + S, by1, chrome)
rect("#9fb4d8", bx0, by1 - S, bx1, by1, chrome)
rect("#9fb4d8", bx1 - S, by0, bx1, by1, chrome)

prog = min(1.0, T/0.90)
bw = 14.0*S; gap = 4.0*S
inner0 = bx0 + 2*S; inner1 = bx1 - 2*S
n = int((inner1 - inner0)/(bw + gap))
filled = int(round(n*prog))
for k in range(filled):
    x = inner0 + k*(bw + gap)
    rect("#2f5fd0", x, by0 + 2*S, x + bw, by1 - 2*S, chrome)
    rect("#7ba4ff", x, by0 + 2*S, x + bw, by0 + 4*S, chrome)
    rect("#16368e", x, by1 - 4*S, x + bw, by1 - 2*S, chrome)
    if k == filled - 1:
        rect("#c8dcff", x, by0 + 2*S, x + bw, by1 - 2*S, chrome)

open("%s/glow_%03d.mvg" % (wd, idx), "w").write(" ".join(out))
open("%s/chrome_%03d.mvg" % (wd, idx), "w").write(" ".join(chrome))
PY

render_frame() {
    local i=$1 t n out halo
    t=$(python3 -c "print(f'{$i/$FRAMES:.6f}')")
    n=$(printf '%03d' "$i")
    out="$w/frame_${n}.png"
    halo=$(python3 -c "import math;print('%.3f'%(0.55+0.45*math.sin(2*math.pi*$t)))")
    python3 "$w/anim.py" "$W" "$H" "$S" "$t" "$w" "$i"
    convert -size ${W}x${H} xc:black -draw "$(cat "$w/glow_${n}.mvg")" "$w/glow_${n}.png"
    convert "$w/bg.png" \
        \( "$w/starhalo.png" -channel A -evaluate multiply "$halo" +channel \) \
        -compose screen -composite \
        "$w/glow_${n}.png" -compose screen -composite \
        "$w/textlayer.png" -compose over -composite \
        -draw "$(cat "$w/chrome_${n}.mvg")" \
        -draw "$(cat "$w/frame.mvg")" \
        -filter Lanczos -resize 940x320 \
        -ordered-dither o8x8,"$LEVELS" -strip "$out"
    rm -f "$w/glow_${n}.png"
}

echo "rendering $FRAMES frames for $ver ..."
maxjobs=$(nproc 2>/dev/null || echo 4)
for ((i=0; i<FRAMES; i++)); do
    render_frame "$i" &
    while [ "$(jobs -r | wc -l)" -ge "$maxjobs" ]; do wait -n; done
done
wait
frames=()
for ((i=0; i<FRAMES; i++)); do frames+=("$w/frame_$(printf '%03d' "$i").png"); done
echo "rendered ${#frames[@]} frames"

convert "${frames[@]}" -append -colors 256 -unique-colors "$w/pal.gif"
echo "palette: $(identify -format '%w' "$w/pal.gif") colours"
convert -delay "$DELAY" -loop 0 \
    $(for f in "${frames[@]}"; do printf ' ( %q -dither None -remap %q ) ' "$f" "$w/pal.gif"; done) \
    "$w/splash_pre.gif"
gifsicle -O3 --colors 256 "$w/splash_pre.gif" -o "$w/splash.gif"
sz=$(stat -c%s "$w/splash.gif")
echo "assembled splash.gif ${FRAMES}f $(identify -format '%wx%h' "$w/splash.gif[0]") ($sz bytes)"

if [ -n "${OUTGIF:-}" ]; then cp "$w/splash.gif" "$OUTGIF"; fi

header="src/about_splash_gif.h"
python3 - "$w/splash.gif" "$header" <<'PY'
import base64, sys, textwrap
gif, header = sys.argv[1], sys.argv[2]
b64 = base64.b64encode(open(gif, "rb").read()).decode()
lines = textwrap.wrap(b64, 96)
out = ["/* about_splash_gif.h — the about:start release splash animation, embedded. */",
       "#ifndef NS_ABOUT_SPLASH_GIF_H", "#define NS_ABOUT_SPLASH_GIF_H", "",
       "static const char about_splash_gif_b64[] ="]
out += ['    "%s"%s' % (ln, ";" if i == len(lines) - 1 else "")
        for i, ln in enumerate(lines)]
out += ["", "#endif", ""]
open(header, "w", newline="\n").write("\n".join(out))
print("wrote %s (%d b64 chars)" % (header, len(b64)))
PY
