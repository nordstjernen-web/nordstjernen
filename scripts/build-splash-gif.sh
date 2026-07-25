#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

ver=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" meson.build | head -n1)
[ -n "$ver" ] || { echo "could not read version from meson.build" >&2; exit 1; }
ver=${ver%%-*}
codename='Nordstjernen Web Browser is not Firefox or Chrome.
Possibly the best web browser in the world'

FRAMES=${NS_SPLASH_FRAMES:-36}
DELAY=${NS_SPLASH_DELAY:-11}
LEVELS=${NS_SPLASH_LEVELS:-14}
LOSSY=${NS_SPLASH_LOSSY:-60}

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

python3 - "$W" "$H" > "$w/sky.mvg" <<'PY'
import sys
W, H = int(sys.argv[1]), int(sys.argv[2])
ZENITH = (8, 58, 140)
HAZE = (228, 245, 254)
out = []
for y in range(H):
    e = ((y + 0.5)/H)**1.75
    c = tuple(int(ZENITH[i] + (HAZE[i] - ZENITH[i])*e) for i in range(3))
    out.append("fill #%02x%02x%02x stroke none rectangle 0,%.1f %d,%.1f" % (c + (y, W, y + 1.2)))
sys.stdout.write(" ".join(out))
PY

convert -size ${W}x${H} xc:black -draw "$(cat "$w/sky.mvg")" "$w/sky0.png"
convert -size ${W}x${H} xc:none -fill '#b6d9f4' \
    -draw "ellipse $((W/2)),${HORIZON} $((W)),$((62*S)) 0,360" -blur 0x$((46*S)) "$w/skyglow.png"
convert "$w/sky0.png" "$w/skyglow.png" -compose screen -composite "$w/sky.png"

PLANET_CY=$(python3 -c "print(int($HORIZON + $H*2.05))")
PLANET_RX=$(python3 -c "print(int($W*1.28))")
PLANET_RY=$(python3 -c "print(int($H*2.05))")

convert -size ${W}x${H} xc:black -fill white \
    -draw "ellipse $((W/2)),${PLANET_CY} ${PLANET_RX},${PLANET_RY} 0,360" \
    -alpha off "$w/planetmask.png"

GH=$((H - HORIZON + 6))
convert -size ${W}x${GH} gradient:'#eaf6ff'-'#8cc3e8' "$w/gbase.png"
convert -size ${W}x${H} xc:'#eaf6ff' "$w/gbase.png" \
    -gravity South -compose over -composite "$w/ground0.png"

python3 - "$W" "$H" "$S" "$HORIZON" > "$w/grid.mvg" <<'PY'
import sys
W, H, S = int(sys.argv[1]), int(sys.argv[2]), float(sys.argv[3])
HZ = int(sys.argv[4])
SURFACE_HORIZON = (234, 246, 255)
SURFACE_FOOT = (140, 195, 232)
GRID = (14, 62, 118)
out = []
vx, vy = W*0.5, HZ

def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i])*t for i in range(3))

def depth(y):
    return max(0.0, min(1.0, (y - HZ)/(H - HZ)))

def gridcol(y):
    t = depth(y)
    return "#%02x%02x%02x" % tuple(int(v) for v in mix(
        mix(SURFACE_HORIZON, SURFACE_FOOT, t), GRID, 0.15 + 0.85*t**0.95))

NSEG = 14
for i in range(-16, 17):
    x1 = vx + i*W*0.118
    x0 = vx + i*W*0.006
    for k in range(NSEG):
        t0 = k/NSEG; t1 = (k + 1)/NSEG
        y0 = vy + (H*1.02 - vy)*t0; y1 = vy + (H*1.02 - vy)*t1
        out.append("stroke %s stroke-width %.1f fill none line %.1f,%.1f %.1f,%.1f" % (
            gridcol((y0 + y1)/2), 1.0*S,
            x0 + (x1 - x0)*t0, y0, x0 + (x1 - x0)*t1, y1))
y = HZ
step = H*0.005
while y < H*1.02:
    y += step
    step *= 1.44
    out.append("stroke %s stroke-width %.1f fill none line 0,%.1f %d,%.1f" % (
        gridcol(y), 1.0*S, y, W, y))
sys.stdout.write(" ".join(out))
PY

convert "$w/ground0.png" -draw "$(cat "$w/grid.mvg")" "$w/ground1.png"
convert "$w/ground1.png" "$w/planetmask.png" -alpha off -compose CopyOpacity -composite "$w/ground.png"

convert -size ${W}x${H} xc:none -fill none -stroke '#f2fbff' -strokewidth $((5*S)) \
    -draw "ellipse $((W/2)),${PLANET_CY} ${PLANET_RX},${PLANET_RY} 0,360" \
    -blur 0x$((3*S)) "$w/limb.png"
convert -size ${W}x${H} xc:none -fill none -stroke '#9ed3f4' -strokewidth $((16*S)) \
    -draw "ellipse $((W/2)),${PLANET_CY} ${PLANET_RX},${PLANET_RY} 0,360" \
    -blur 0x$((26*S)) "$w/limbglow.png"

convert "$w/sky.png" \
    "$w/ground.png" -compose over -composite \
    "$w/limbglow.png" -compose screen -composite \
    "$w/limb.png" -compose screen -composite "$w/bg.png"

SX=$(python3 -c "print(int($W*0.800))"); SY=$(python3 -c "print(int($H*0.360))")
convert -size ${W}x${H} xc:none -fill '#ffc65a' \
    -draw "ellipse ${SX},${SY} $((44*S)),$((44*S)) 0,360" -blur 0x$((34*S)) "$w/halo.png"
convert -size ${W}x${H} xc:none -fill '#fff3d2' \
    -draw "ellipse ${SX},${SY} $((18*S)),$((18*S)) 0,360" -blur 0x$((12*S)) "$w/halo2.png"
convert "$w/halo.png" "$w/halo2.png" -compose over -composite "$w/sunhalo.png"

P() { echo $(( $1 * S )); }
convert -background none -font "$fb" -pointsize $(P 52) -kerning $((1*S)) -fill '#ffffff' label:'Nordstjernen ' "$w/t1.png"
convert -background none -font "$fb" -pointsize $(P 52) -fill '#ffd23f' label:"$ver" "$w/t2.png"
convert -background none -font "$fb" -pointsize $(P 24) -fill '#d9eeff' label:'Nordstjernen Web Browser' "$w/ts.png"
convert -background none -font "$fr" -pointsize $(P 20) -fill '#0a2b5c' -size $((700*S))x caption:"$codename" "$w/tc.png"

for n in t1 t2 ts; do
    convert "$w/$n.png" -fill '#04203f' -colorize 100 "$w/${n}k.png"
done
convert "$w/tc.png" -fill '#eaf5ff' -colorize 100 "$w/tck.png"
for n in t1 t2; do
    convert "$w/$n.png" -fill '#0d3f86' -colorize 100 "$w/${n}h.png"
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
SKY = (170, 208, 238)
out = []
clouds = []

def ell(col, cx, cy, rx, ry, dst=None):
    (out if dst is None else dst).append(
        "fill %s stroke none ellipse %.1f,%.1f %.1f,%.1f 0,360" % (col, cx, cy, rx, ry))

def poly(col, pts):
    out.append("fill %s stroke none polygon %s" % (col, " ".join("%.1f,%.1f" % p for p in pts)))

def hx(c):
    return "#%02x%02x%02x" % tuple(int(max(0, min(255, v))) for v in c)

def rgba(c, a):
    return "rgba(%d,%d,%d,%.3f)" % (c[0], c[1], c[2], max(0.0, min(1.0, a)))

def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i])*t for i in range(3))

CLOUDS = [
    (0.108, 0.505, 0.079, 0.00, 11),
    (0.302, 0.462, 0.050, 1.90, 23),
    (0.556, 0.500, 0.082, 3.40, 37),
    (0.735, 0.470, 0.036, 0.40, 29),
    (0.822, 0.536, 0.061, 2.30, 51),
    (0.976, 0.462, 0.047, 4.70, 67),
    (0.215, 0.577, 0.024, 2.70, 71),
    (0.668, 0.592, 0.029, 5.60, 83),
    (0.892, 0.618, 0.023, 0.90, 97),
    (0.404, 0.627, 0.019, 3.10, 113),
]

for fx, fy, sc, phase, seed in CLOUDS:
    rnd = random.Random(seed)
    R = H*sc
    cx = W*fx + math.sin(TAU*T + phase)*W*0.0045*(sc/0.05)
    cy = H*fy + math.sin(TAU*T + phase*1.7)*H*0.0035
    haze = max(0.0, min(0.85, (0.056 - sc)/0.042))
    under = mix((120, 162, 206), SKY, haze*0.80)
    body = mix((219, 236, 251), SKY, haze*0.90)
    lit = mix((255, 255, 255), SKY, haze*0.85)
    sunward = 1.0 if cx < SX else -1.0
    puffs = []
    n = rnd.randint(4, 6)
    for i in range(n):
        u = (i + 0.5)/n
        b = math.sin(math.pi*u)**0.62
        puffs.append((cx + (u - 0.5)*R*2.0 + rnd.uniform(-0.07, 0.07)*R,
                      cy - R*0.55*b - rnd.uniform(0.0, 0.10)*R,
                      R*(0.34 + 0.62*b)*rnd.uniform(0.86, 1.12)))
    for i in range(rnd.randint(1, 2)):
        u = rnd.uniform(0.25, 0.75)
        b = math.sin(math.pi*u)**0.62
        puffs.append((cx + (u - 0.5)*R*1.7,
                      cy - R*1.05*b*rnd.uniform(0.72, 1.00),
                      R*(0.24 + 0.34*b)*rnd.uniform(0.82, 1.10)))
    ell(hx(under), cx, cy + R*0.10, R*1.32, R*0.30, clouds)
    for px, py, pr in puffs:
        ell(hx(under), px, py + R*0.17, pr*1.05, pr*1.05, clouds)
    ell(hx(body), cx, cy - R*0.02, R*1.28, R*0.26, clouds)
    for px, py, pr in puffs:
        ell(hx(body), px, py, pr, pr, clouds)
    for px, py, pr in puffs:
        ell(hx(lit), px + sunward*pr*0.14, py - pr*0.22, pr*0.76, pr*0.76, clouds)

pulse = 0.80 + 0.20*math.sin(TAU*T)

def ray(ang, ln, hw, col, a0, segs=6):
    ux, uy = math.cos(ang), math.sin(ang)
    px, py = -uy, ux
    for k in range(segs):
        t0 = k/segs; t1 = (k + 1)/segs
        a = a0*(1.0 - t0)**1.7
        w0 = hw*(1.0 - t0*0.85); w1 = hw*(1.0 - t1*0.85)
        ax, ay = SX + ux*ln*t0, SY + uy*ln*t0
        bx, by = SX + ux*ln*t1, SY + uy*ln*t1
        poly(rgba(col, a), [(ax + px*w0, ay + py*w0), (ax - px*w0, ay - py*w0),
                            (bx - px*w1, by - py*w1), (bx + px*w1, by + py*w1)])

RAYS = 16
for k in range(RAYS):
    q = (k - T)/RAYS
    m = 0.5 + 0.5*math.sin(TAU*3*q + 0.7)
    ray(TAU*q, H*(0.14 + 0.36*m)*pulse, S*(2.2 + 5.4*m),
        (255, 246, 214), 0.055 + 0.080*m)

fx, fy = W*0.5, H*0.5
for t, rad, col, a in [(0.42, 0.020, (255, 232, 170), 0.20), (0.70, 0.034, (255, 214, 150), 0.15),
                       (1.16, 0.026, (255, 208, 130), 0.18), (1.42, 0.052, (190, 226, 255), 0.12),
                       (1.78, 0.016, (255, 250, 210), 0.22)]:
    px = SX + (fx - SX)*t; py = SY + (fy - SY)*t
    ell(rgba(col, a*(0.6 + 0.4*pulse)), px, py, H*rad*1.05, H*rad)

def spike(ang, ln, hwid, col):
    a = math.radians(ang)
    tip = (SX + math.cos(a)*ln, SY + math.sin(a)*ln)
    px, py = math.cos(a + math.pi/2)*hwid, math.sin(a + math.pi/2)*hwid
    poly(col, [tip, (SX + px, SY + py), (SX - px, SY - py)])

for ang in (0, 90, 180, 270):
    spike(ang, H*0.200*pulse, S*3.8, "#ffe9b0")
for ang in (45, 135, 225, 315):
    spike(ang, H*0.082*pulse, S*2.8, "#ffd88a")
for ang in (0, 90, 180, 270):
    spike(ang, H*0.138*pulse, S*1.8, "#fffbee")
ell("#fff2c8", SX, SY, S*9.2*pulse, S*9.2*pulse)
ell("#ffffff", SX, SY, S*5.4*pulse, S*5.4*pulse)

open("%s/glow_%03d.mvg" % (wd, idx), "w").write(" ".join(out))
open("%s/clouds_%03d.mvg" % (wd, idx), "w").write(" ".join(clouds))
PY

render_frame() {
    local i=$1 t n out halo
    t=$(python3 -c "print(f'{$i/$FRAMES:.6f}')")
    n=$(printf '%03d' "$i")
    out="$w/frame_${n}.png"
    halo=$(python3 -c "import math;print('%.3f'%(0.80+0.20*math.sin(2*math.pi*$t)))")
    python3 "$w/anim.py" "$W" "$H" "$S" "$t" "$w" "$i"
    convert -size ${W}x${H} xc:black -draw "$(cat "$w/glow_${n}.mvg")" "$w/glow_${n}.png"
    convert "$w/bg.png" -draw "$(cat "$w/clouds_${n}.mvg")" \
        \( "$w/sunhalo.png" -channel A -evaluate multiply "$halo" +channel \) \
        -compose screen -composite \
        "$w/glow_${n}.png" -compose screen -composite \
        "$w/textlayer.png" -compose over -composite \
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
gifsicle -O3 --lossy="$LOSSY" --colors 256 "$w/splash_pre.gif" -o "$w/splash.gif"
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
