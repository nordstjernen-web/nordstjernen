#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

ver=$(sed -n "s/^[[:space:]]*version:[[:space:]]*'\([^']*\)'.*/\1/p" meson.build | head -n1)
[ -n "$ver" ] || { echo "could not read version from meson.build" >&2; exit 1; }
ver=${ver%%-*}
codename='Nordstjernen Web Browser is not Firefox or Chrome.
Possibly the best web browser in the world'

FRAMES=${NS_SPLASH_FRAMES:-56}
DELAY=${NS_SPLASH_DELAY:-12}
LOSSY=${NS_SPLASH_LOSSY:-28}
NOISE=${NS_SPLASH_NOISE:-0.20}

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
fr=$(find_font 'DejaVu Sans' \
    /usr/share/fonts/truetype/dejavu/DejaVuSans.ttf \
    /usr/share/fonts/dejavu/DejaVuSans.ttf /usr/share/fonts/TTF/DejaVuSans.ttf)

if [ -n "${NS_SPLASH_WORKDIR:-}" ]; then
    w=$NS_SPLASH_WORKDIR; mkdir -p "$w"
else
    w=$(mktemp -d)
    trap 'rm -rf "$w"' EXIT
fi

# the scene is composed at 3x and downscaled once for clean anti-aliased edges
S=3
W=$((940 * S)); H=$((320 * S))

# ---------------------------------------------------------------------------
# static background: sky, glow, clouds — rendered once for every frame
# ---------------------------------------------------------------------------

# golden-age sky: deep blue at the top warming to pale gold at the horizon
convert -size ${W}x${H} gradient:'#2b7ac8'-'#f6ecd2' "$w/sky.png"
convert -size ${W}x${H} gradient:none-'#fbeecb' "$w/warm.png"
convert "$w/sky.png" "$w/warm.png" -compose over -composite "$w/sky1.png"

# a soft warm halo behind the world globe (upper right)
convert -size ${W}x${H} xc:none -fill '#cfe9ff' \
    -draw "ellipse $((760*S)),$((72*S)) $((150*S)),$((120*S)) 0,360" -blur 0x$((70*S)) "$w/glow.png"
convert "$w/sky1.png" "$w/glow.png" -compose screen -composite "$w/sky2.png"

# soft, voluminous clouds with shaded undersides and sunlit tops
clouddraw=$(python3 - "$W" "$H" "$S" <<'PY'
import sys
W,H=int(sys.argv[1]),int(sys.argv[2]); S=float(sys.argv[3])
out=[]
def ell(rgba,cx,cy,rx,ry): out.append("fill %s stroke none ellipse %.2f,%.2f %.2f,%.2f 0,360"%(rgba,cx,cy,rx,ry))
def cloud(cx,cy,sc):
    puffs=[(-1.05,0.06,0.46),(-0.55,-0.10,0.66),(0.05,-0.20,0.82),(0.62,-0.08,0.66),(1.12,0.08,0.44),(0.15,0.14,0.95)]
    ell("rgba(178,196,220,0.88)", cx+0.10*sc, cy+sc*0.32, 1.55*sc, sc*0.34)
    for dx,dy,r in puffs:
        ell("rgba(172,192,218,0.88)", cx+dx*sc, cy+dy*sc+sc*0.22, r*sc, r*sc*0.72)
    for dx,dy,r in puffs:
        ell("rgba(222,231,243,0.94)", cx+dx*sc, cy+dy*sc+sc*0.10, r*sc*0.97, r*sc*0.70)
    for dx,dy,r in puffs:
        ell("rgba(251,250,247,0.97)", cx+dx*sc, cy+dy*sc, r*sc, r*sc*0.74)
    for dx,dy,r in puffs[:4]:
        ell("rgba(255,246,222,0.95)", cx+dx*sc-r*sc*0.18, cy+dy*sc-r*sc*0.34, r*sc*0.52, r*sc*0.36)
    for dx,dy,r in puffs[1:4]:
        ell("rgba(255,253,244,0.92)", cx+dx*sc-r*sc*0.30, cy+dy*sc-r*sc*0.44, r*sc*0.28, r*sc*0.18)
cloud(W*0.305, H*0.095, H*0.050)
cloud(W*0.605, H*0.070, H*0.044)
cloud(W*0.175, H*0.335, H*0.040)
cloud(W*0.470, H*0.320, H*0.034)
cloud(W*0.720, H*0.360, H*0.036)
cloud(W*0.055, H*0.180, H*0.038)
sys.stdout.write(" ".join(out))
PY
)
# the clouds live on their own transparent layer so the render loop can drift
# them gently across the sky each frame
convert -size ${W}x${H} xc:none -draw "$clouddraw" -blur 0x$((1*S)) "$w/cloudlayer.png"

# ---------------------------------------------------------------------------
# static scene: hills, land, wonders, vegetation, road, and the calm sea —
# everything the animated pass draws over. Ships, cars, the sun-glitter, the
# globe, the aircraft, the balloon and the palm fronds are added per frame.
# ---------------------------------------------------------------------------
scene=$(python3 - "$W" "$H" "$S" <<'PY'
import sys, random, math
W, H = int(sys.argv[1]), int(sys.argv[2])
S = float(sys.argv[3]) if len(sys.argv) > 3 else 1.0
out = []
random.seed(20)

def hx(c): return "#%02x%02x%02x" % (int(c[0]), int(c[1]), int(c[2]))
def dk(c, f=0.8): return tuple(max(0, int(v*f)) for v in c)
def lt(c, f=1.15): return tuple(min(255, int(v*f)) for v in c)
def poly(col, pts):
    out.append("fill %s stroke none polygon %s" % (hx(col), " ".join("%.2f,%.2f" % p for p in pts)))
def rgba_poly(rgba, pts):
    out.append("fill %s stroke none polygon %s" % (rgba, " ".join("%.2f,%.2f" % p for p in pts)))
def ell(col, cx, cy, rx, ry):
    out.append("fill %s stroke none ellipse %.2f,%.2f %.2f,%.2f 0,360" % (hx(col), cx, cy, rx, ry))
def rgba_ell(rgba, cx, cy, rx, ry, a0=0, a1=360):
    out.append("fill %s stroke none ellipse %.2f,%.2f %.2f,%.2f %d,%d" % (rgba, cx, cy, rx, ry, a0, a1))
def arc(col, wid, cx, cy, rx, ry, a0, a1):
    out.append("fill none stroke %s stroke-width %.2f ellipse %.2f,%.2f %.2f,%.2f %d,%d" % (col, wid, cx, cy, rx, ry, a0, a1))
    out.append("stroke none")
def line(col, wid, a, b):
    out.append("stroke %s stroke-width %.2f stroke-linecap round line %.2f,%.2f %.2f,%.2f" % (hx(col), wid, a[0], a[1], b[0], b[1]))
    out.append("stroke none")
def rect(col, x0, y0, x1, y1):
    out.append("fill %s stroke none rectangle %.2f,%.2f %.2f,%.2f" % (hx(col), x0, y0, x1, y1))

WL = H*0.705

def ridge(base_y, color, amp, seed, x0):
    random.seed(seed)
    pts = [(x0, H), (x0, base_y)]
    x = x0
    while x < W + 30*S:
        y = base_y - random.uniform(amp*0.3, amp)
        pts.append((x, y)); x += (70 + random.uniform(-18, 18))*S
    pts += [(W+30*S, base_y), (W+30*S, H)]
    poly(color, pts)

ridge(H*0.50, (150,178,196), H*0.09, 3, W*0.30)
ridge(H*0.56, (128,168,150), H*0.08, 6, W*0.28)

land_pts = [(W*0.285, WL), (W*0.34, H*0.66), (W*0.42, H*0.615), (W*0.52, H*0.60),
            (W*0.60, H*0.58), (W*0.70, H*0.585), (W*0.80, H*0.55),
            (W*0.90, H*0.505), (W, H*0.47), (W, WL)]
poly((150,196,120), land_pts)
poly(dk((150,196,120),0.94), [(W*0.285, WL), (W*0.34, H*0.66), (W*0.42, H*0.615),
    (W*0.52, H*0.60), (W*0.60, H*0.58), (W*0.60, WL)])

def gshadow(cx, by, rx, ry, a=0.20):
    rgba_ell("rgba(46,58,44,%.2f)" % a, cx, by, rx, ry)

def tri_band(apex, bl, br, ct, cb, n=8):
    for i in range(n):
        t0=i/n; t1=(i+1)/n
        y0=apex[1]+(bl[1]-apex[1])*t0; y1=apex[1]+(bl[1]-apex[1])*t1
        xl0=apex[0]+(bl[0]-apex[0])*t0; xl1=apex[0]+(bl[0]-apex[0])*t1
        xr0=apex[0]+(br[0]-apex[0])*t0; xr1=apex[0]+(br[0]-apex[0])*t1
        tm=(t0+t1)/2
        c=tuple(int(ct[k]+(cb[k]-ct[k])*tm) for k in range(3))
        poly(c, [(xl0,y0),(xr0,y0),(xr1,y1),(xl1,y1)])

_lt=[(0.285,0.705),(0.34,0.66),(0.42,0.615),(0.52,0.60),(0.60,0.58),
     (0.70,0.585),(0.80,0.55),(0.90,0.505),(1.0,0.47)]
def land_top(xf):
    for i in range(len(_lt)-1):
        x0,y0=_lt[i]; x1,y1=_lt[i+1]
        if xf<=x1:
            t=(xf-x0)/(x1-x0) if x1>x0 else 0.0
            return H*(y0+(y1-y0)*max(0.0,min(1.0,t)))
    return H*_lt[-1][1]

HAZE=(198,211,214)
def hz(c, f=0.58): return tuple(int(round(c[k]*(1-f)+HAZE[k]*f)) for k in range(3))
def sky_ell(cx, cy, rx, ry, a0, a1): rgba_ell("rgba(%d,%d,%d,1.0)"%HAZE, cx, cy, rx, ry, a0, a1)

def bg_col_temple(cx, base, wd, h):
    c=hz((230,226,212)); cs=hz((196,190,172)); rf=hz((214,208,190))
    gshadow(cx, base, wd*0.80, h*0.045, 0.14)
    rect(cs, cx-wd*0.58, base-h*0.10, cx+wd*0.58, base)
    for i in range(6):
        x=cx-wd*0.46+(wd*0.92)*i/5
        rect(c, x-wd*0.045, base-h*0.66, x+wd*0.045, base-h*0.10)
        rect(cs, x+wd*0.012, base-h*0.66, x+wd*0.045, base-h*0.10)
    rect(cs, cx-wd*0.52, base-h*0.78, cx+wd*0.52, base-h*0.66)
    poly(rf, [(cx-wd*0.56, base-h*0.78),(cx+wd*0.56, base-h*0.78),(cx, base-h*0.98)])
    poly(hz((196,190,172)), [(cx, base-h*0.78),(cx+wd*0.56, base-h*0.78),(cx, base-h*0.98)])

def bg_aqueduct(x0, x1, base, h):
    c=hz((216,206,185)); cs=hz((190,180,158))
    n=max(3,int((x1-x0)/(H*0.030)))
    seg=(x1-x0)/n
    gshadow((x0+x1)/2, base, (x1-x0)*0.58, h*0.10, 0.13)
    rect(c, x0, base-h, x1, base)
    for i in range(n):
        px=x0+seg*(i+0.5)
        sky_ell(px, base, seg*0.34, h*0.66, 180, 360)
    rect(cs, x0, base-h, x1, base-h*0.86)
    rect(c, x0-seg*0.1, base-h*1.02, x1+seg*0.1, base-h*0.86)

def bg_rotunda(cx, base, wd, h):
    c=hz((228,222,206)); cs=hz((198,192,174)); dm=hz((216,208,190))
    gshadow(cx, base, wd*0.68, h*0.05, 0.14)
    rect(cs, cx-wd*0.5, base-h*0.46, cx+wd*0.5, base)
    sky_ell(cx, base-h*0.46, wd*0.5, h*0.46, 180, 360)
    for i in range(4):
        x=cx-wd*0.34+wd*0.68*i/3
        rect(c, x-wd*0.05, base-h*0.46, x+wd*0.05, base)
    poly(dm, [(cx-wd*0.46, base-h*0.46),(cx+wd*0.46, base-h*0.46),(cx, base-h*0.66)])

def bg_arch(cx, base, wd, h):
    c=hz((220,210,188)); cs=hz((192,182,160))
    gshadow(cx, base, wd*0.72, h*0.06, 0.14)
    rect(c, cx-wd*0.5, base-h, cx+wd*0.5, base)
    sky_ell(cx, base, wd*0.24, h*0.52, 180, 360)
    rect(cs, cx-wd*0.6, base-h*1.06, cx+wd*0.6, base-h*0.88)

def bg_column(cx, base, h):
    c=hz((226,220,204)); cs=hz((196,190,172))
    gshadow(cx, base+h*0.04, h*0.16, h*0.03, 0.14)
    rect(c, cx-h*0.045, base-h, cx+h*0.045, base)
    rect(cs, cx+h*0.012, base-h, cx+h*0.045, base)
    rect(cs, cx-h*0.075, base-h*1.08, cx+h*0.075, base-h)
    rect(c, cx-h*0.09, base-h*0.02, cx+h*0.09, base+h*0.04)

def bg_tholos(cx, base, wd, h):
    c=hz((230,224,208)); cs=hz((198,192,174)); rf=hz((214,206,188))
    gshadow(cx, base, wd*0.62, h*0.05, 0.14)
    for i in range(6):
        x=cx-wd*0.4+wd*0.8*i/5
        rect(c, x-wd*0.04, base-h*0.6, x+wd*0.04, base)
    sky_ell(cx, base-h*0.6, wd*0.5, h*0.36, 180, 360)
    rect(cs, cx-wd*0.46, base-h*0.6, cx+wd*0.46, base-h*0.52)
    poly(rf, [(cx-wd*0.48, base-h*0.6),(cx+wd*0.48, base-h*0.6),(cx, base-h*0.82)])

NSEG=64
for i in range(NSEG):
    xf0=0.295+ (1.0-0.295)*i/NSEG; xf1=0.295+ (1.0-0.295)*(i+1)/NSEG
    x0,y0=W*xf0, land_top(xf0); x1,y1=W*xf1, land_top(xf1)
    line(lt((162,206,128),1.10), max(1.2,1.5*S), (x0, y0+0.8*S), (x1, y1+0.8*S))
    rgba_poly("rgba(34,62,42,0.12)", [(x0, y0+2.0*S),(x1, y1+2.0*S),(x1, y1+9.0*S),(x0, y0+9.0*S)])
    rgba_poly("rgba(34,62,42,0.06)", [(x0, y0+9.0*S),(x1, y1+9.0*S),(x1, y1+18.0*S),(x0, y0+18.0*S)])

bg_col_temple(W*0.470, land_top(0.470)+H*0.050, H*0.072, H*0.120)
bg_aqueduct(W*0.548, W*0.636, land_top(0.59)+H*0.048, H*0.082)
bg_rotunda(W*0.680, land_top(0.680)+H*0.044, H*0.078, H*0.112)
bg_arch(W*0.775, land_top(0.775)+H*0.040, H*0.052, H*0.082)
bg_column(W*0.900, land_top(0.900)+H*0.030, H*0.120)
bg_tholos(W*0.955, land_top(0.955)+H*0.028, H*0.058, H*0.095)

poly((236,214,160), [(W*0.36, WL), (W*0.585, WL), (W*0.55, H*0.63), (W*0.40, H*0.645)])

rgba_poly("rgba(250,236,150,0.16)", [(W*0.30, WL), (W, H*0.475), (W, H*0.545),
    (W*0.60, H*0.62), (W*0.42, H*0.65), (W*0.30, WL)])
rgba_poly("rgba(30,70,60,0.10)", [(W*0.285, WL), (W, WL-H*0.02), (W, WL), (W*0.285, WL)])

def cypress(bx, by, h):
    c1=(52,104,64); c2=(72,132,80)
    gshadow(bx+h*0.12, by, h*0.22, h*0.05, 0.16)
    line((104,78,50), max(1.0,1.2*S), (bx, by), (bx, by-h*0.14))
    poly(dk(c1,0.9), [(bx,by-h),(bx+h*0.13,by-h*0.04),(bx-h*0.11,by-h*0.04)])
    poly(c1, [(bx-h*0.01,by-h*0.99),(bx+h*0.10,by-h*0.05),(bx-h*0.10,by-h*0.05)])
    poly(c2, [(bx-h*0.03,by-h*0.9),(bx+h*0.03,by-h*0.2),(bx-h*0.06,by-h*0.2)])

def pine(bx, by, h):
    base=(58,122,66)
    gshadow(bx+h*0.14, by, h*0.4, h*0.06, 0.16)
    line((116,84,54), max(1.2,1.6*S), (bx, by), (bx, by-h*0.66))
    ell(dk(base,0.86), bx+h*0.10, by-h*0.70, h*0.36, h*0.20)
    ell(base, bx, by-h*0.78, h*0.38, h*0.23)
    ell(lt(base,1.14), bx-h*0.12, by-h*0.86, h*0.16, h*0.10)

def olive(bx, by, h):
    base=(126,156,102)
    gshadow(bx+h*0.10, by, h*0.34, h*0.05, 0.16)
    line((126,108,82), max(1.0,1.3*S), (bx, by), (bx, by-h*0.46))
    blobs=[(-0.17,-0.58,0.24),(0.18,-0.55,0.23),(0.0,-0.74,0.26)]
    for dx,dy,r in blobs:
        ell(dk(base,0.88), bx+dx*h+r*h*0.12, by+dy*h+r*h*0.10, r*h, r*h*0.9)
    for dx,dy,r in blobs:
        ell(base, bx+dx*h, by+dy*h, r*h, r*h*0.9)
    ell(lt(base,1.12), bx-0.14*h, by-0.78*h, 0.12*h, 0.09*h)

def place_tree(fn, xf, sf, h):
    ty0=land_top(xf); by=ty0+(WL-ty0)*sf
    fn(W*xf, by, h)

for kind,xf,sf,hh in [
    (cypress,0.355,0.55,0.090),(pine,0.500,0.62,0.086),(olive,0.535,0.74,0.070),
    (cypress,0.605,0.5,0.096),(cypress,0.628,0.62,0.082),(pine,0.688,0.66,0.088),
    (olive,0.742,0.55,0.072),(cypress,0.772,0.72,0.078),(pine,0.862,0.5,0.090),
    (cypress,0.905,0.42,0.092),(olive,0.952,0.66,0.070),(cypress,0.978,0.5,0.082)]:
    place_tree(kind, xf, sf, H*hh)

random.seed(63)
for _ in range(60):
    gx=random.uniform(W*0.30, W); gt=land_top(gx/W)
    gy=random.uniform(gt+H*0.02, WL-2*S)
    gh=random.uniform(3,7)*S
    line(dk((92,168,84),0.92), max(0.7,0.9*S), (gx, gy), (gx+random.uniform(-2,2)*S, gy-gh))
    line((120,192,100), max(0.7,0.9*S), (gx+1.6*S, gy), (gx+1.6*S+random.uniform(-2,2)*S, gy-gh*0.8))

rgba_poly("rgba(255,244,196,0.30)", [(W*0.36, WL-2.4*S),(W*0.585, WL-2.4*S),(W*0.585, WL-0.8*S),(W*0.36, WL-0.8*S)])
rgba_poly("rgba(150,116,72,0.35)", [(W*0.355, WL-0.8*S),(W*0.59, WL-0.8*S),(W*0.59, WL),(W*0.355, WL)])

def pyramid(cx, by, hw, hh):
    gshadow(cx+hw*0.35, by, hw*1.2, hh*0.05, 0.20)
    litT=(248,228,180); litB=(228,200,142); shdT=(212,186,134); shdB=(184,156,104)
    apex=(cx, by-hh)
    tri_band(apex,(cx-hw,by),(cx,by), litT, litB, 9)
    tri_band(apex,(cx,by),(cx+hw,by), shdT, shdB, 9)
    poly(lt(litT,1.05), [apex,(cx-hw*0.11,by-hh*0.86),(cx+hw*0.11,by-hh*0.86)])
    line(lt(litT,1.08), max(0.8,1.0*S), apex, (cx, by))
    for k in (0.32,0.55,0.78):
        y=by-hh*k
        line(dk(litB,0.9), max(0.6,0.7*S), (cx-hw*(1-k), y), (cx, y))
        line(dk(shdB,0.9), max(0.6,0.7*S), (cx, y), (cx+hw*(1-k), y))

def palm_trunk(bx, by, h):
    gshadow(bx+h*0.06, by, h*0.30, h*0.045, 0.16)
    line((120,86,52), max(1.4,2.0*S), (bx, by), (bx-h*0.06, by-h))

pyramid(W*0.470, WL, H*0.085, H*0.150)
pyramid(W*0.420, WL, H*0.058, H*0.098)
pyramid(W*0.520, WL, H*0.045, H*0.078)
palm_trunk(W*0.392, WL, H*0.115)

def temple(cx, by, wd, ht):
    marble=(242,238,226); shade=(210,204,186); dark=(150,146,132)
    roof=(230,224,206); cap=(232,226,208)
    gshadow(cx, by, wd*0.64, ht*0.05, 0.20)
    rect(dk(marble,0.86), cx-wd*0.60, by-ht*0.05, cx+wd*0.60, by)
    rect(shade, cx-wd*0.56, by-ht*0.10, cx+wd*0.56, by-ht*0.05)
    rect(marble, cx-wd*0.52, by-ht*0.14, cx+wd*0.52, by-ht*0.10)
    n=7
    cw=wd*0.036
    for i in range(n):
        x=cx-wd*0.46 + (wd*0.92)*i/(n-1)
        rect(marble, x-cw, by-ht*0.70, x+cw, by-ht*0.14)
        rect(shade, x+cw*0.30, by-ht*0.70, x+cw, by-ht*0.14)
        for f in (-0.55,0.0,0.55):
            line(dk(marble,0.88), max(0.5,0.6*S), (x+cw*f, by-ht*0.68), (x+cw*f, by-ht*0.16))
        rect(cap, x-cw*1.3, by-ht*0.74, x+cw*1.3, by-ht*0.70)
    rect(marble, cx-wd*0.54, by-ht*0.80, cx+wd*0.54, by-ht*0.74)
    rect(shade, cx-wd*0.54, by-ht*0.84, cx+wd*0.54, by-ht*0.80)
    for i in range(9):
        gx=cx-wd*0.48+wd*0.96*i/8
        rect(dark, gx-wd*0.012, by-ht*0.836, gx+wd*0.012, by-ht*0.804)
    poly(roof, [(cx-wd*0.58, by-ht*0.84), (cx+wd*0.58, by-ht*0.84), (cx, by-ht*1.14)])
    poly(dk(roof,0.9), [(cx, by-ht*0.84), (cx+wd*0.58, by-ht*0.84), (cx, by-ht*1.14)])
    line(dk(roof,0.8), max(0.6,0.8*S), (cx-wd*0.58, by-ht*0.84), (cx+wd*0.58, by-ht*0.84))
    for ax,ay in [(cx, by-ht*1.14),(cx-wd*0.58, by-ht*0.84),(cx+wd*0.58, by-ht*0.84)]:
        ell(cap, ax, ay-ht*0.02, wd*0.02, ht*0.028)

temple(W*0.560, H*0.62, H*0.150, H*0.205)

def eiffel(cx, by, ht):
    iron=(104,82,60); irl=(148,122,92); ird=(68,54,42); irh=(200,172,132)
    y1=by-ht*0.30; y2=by-ht*0.575; y3=by-ht*0.885
    w0=ht*0.200; w1=ht*0.088; w2=ht*0.046; w3=ht*0.017
    gshadow(cx, by, w0*1.35, ht*0.030, 0.20)
    n=10
    outL=[]; outR=[]; inn=[]
    for k in range(n+1):
        t=k/float(n)
        y=by-(by-y1)*t
        wo=w0+(w1-w0)*(t**1.6)
        a=min(1.0, t/0.78)
        wi=w0*0.55*max(0.0, 1.0-a**2.2)
        outL.append((cx-wo, y)); outR.append((cx+wo, y)); inn.append((wi, y))
    poly(iron, outL + [(cx-inn[k][0], inn[k][1]) for k in range(n, -1, -1)])
    poly(dk(iron,0.80), outR + [(cx+inn[k][0], inn[k][1]) for k in range(n, -1, -1)])
    for k in range(n):
        line(irl, max(0.6,0.7*S), outL[k], (cx-inn[k+1][0], inn[k+1][1]))
        line(irl, max(0.6,0.7*S), (cx-inn[k][0], inn[k][1]), outL[k+1])
        line(dk(irl,0.82), max(0.6,0.7*S), outR[k], (cx+inn[k+1][0], inn[k+1][1]))
        line(dk(irl,0.82), max(0.6,0.7*S), (cx+inn[k][0], inn[k][1]), outR[k+1])
    for k in range(n):
        line(irh, max(0.7,0.9*S), outL[k], outL[k+1])
        line(ird, max(0.7,0.9*S), outR[k], outR[k+1])
        if inn[k][0] > 0.5 and inn[k+1][0] > 0.1:
            line(irh, max(0.7,0.9*S), (cx-inn[k][0], inn[k][1]), (cx-inn[k+1][0], inn[k+1][1]))
            line(irl, max(0.7,0.9*S), (cx+inn[k][0], inn[k][1]), (cx+inn[k+1][0], inn[k+1][1]))
    def section(ya, wa, yb, wb, p, rungs):
        m=8
        L=[]; R=[]
        for k in range(m+1):
            t=k/float(m)
            y=ya-(ya-yb)*t
            wcur=wa+(wb-wa)*(t**p)
            L.append((cx-wcur, y)); R.append((cx+wcur, y))
        rgba_poly("rgba(%d,%d,%d,0.30)" % iron, L + list(reversed(R)))
        for r in range(rungs):
            s0=r/float(rungs); s1=(r+1)/float(rungs)
            yA=ya-(ya-yb)*s0; yB=ya-(ya-yb)*s1
            wA=wa+(wb-wa)*(s0**p); wB=wa+(wb-wa)*(s1**p)
            line(irl, max(0.5,0.65*S), (cx-wA, yA), (cx+wB, yB))
            line(irl, max(0.5,0.65*S), (cx+wA, yA), (cx-wB, yB))
            line(ird, max(0.4,0.55*S), (cx-wB, yB), (cx+wB, yB))
        for k in range(m):
            line(irh, max(0.6,0.8*S), L[k], L[k+1])
            line(ird, max(0.6,0.8*S), R[k], R[k+1])
    section(y1, w1, y2, w2, 1.25, 5)
    section(y2, w2, y3, w3, 1.15, 6)
    def deck(y, wdd, hgt):
        rect(ird, cx-wdd, y-hgt, cx+wdd, y)
        line(irh, max(0.6,0.8*S), (cx-wdd, y-hgt), (cx+wdd, y-hgt))
        line(dk(ird,0.8), max(0.5,0.6*S), (cx-wdd, y), (cx+wdd, y))
    deck(y1+ht*0.012, w1*1.38, ht*0.026)
    deck(y2+ht*0.008, w2*1.52, ht*0.018)
    deck(y3+ht*0.006, w3*2.4, ht*0.013)
    rect(iron, cx-ht*0.013, y3-ht*0.032, cx+ht*0.013, y3)
    line(irh, max(0.5,0.7*S), (cx-ht*0.013, y3-ht*0.032), (cx+ht*0.013, y3-ht*0.032))
    poly(iron, [(cx-w3, y3-ht*0.030), (cx-ht*0.004, by-ht*0.968), (cx+ht*0.004, by-ht*0.968), (cx+w3, y3-ht*0.030)])
    line(ird, max(1.0,1.3*S), (cx, by-ht*0.968), (cx, by-ht))
    ell(irh, cx, by-ht*0.998, ht*0.006, ht*0.006)

eiffel(W*0.640, WL, H*0.44)

def colosseum(cx, by, wd, ht):
    litc=(238,224,188); stone=(220,204,164); shd=(190,172,132); dark=(112,96,72); grass=(150,182,120)
    gshadow(cx, by, wd*1.15, ht*0.06, 0.20)
    top=by-ht
    ell(stone, cx, top+ht*0.20, wd, ht*0.20)
    ell(dk(stone,0.9), cx, top+ht*0.18, wd*0.72, ht*0.15)
    ell(grass, cx, top+ht*0.17, wd*0.66, ht*0.12)
    rect(stone, cx-wd, top+ht*0.19, cx+wd, by)
    poly(litc, [(cx-wd, top+ht*0.19),(cx-wd*0.46, top+ht*0.19),(cx-wd*0.46, by),(cx-wd,by)])
    poly(shd, [(cx+wd, top+ht*0.19),(cx+wd*0.5, top+ht*0.19),(cx+wd*0.5, by),(cx+wd,by)])
    ell(shd, cx, by, wd, ht*0.15)
    ell(stone, cx, by-ht*0.055, wd*0.99, ht*0.12)
    na=9
    for i in range(na):
        t=i/(na-1.0)
        ax=cx-wd*0.9+wd*1.8*t
        aw=wd*0.045*(1-abs(t-0.5)*0.5)
        rect(dark, ax-aw, top+ht*0.19, ax+aw, top+ht*0.29)
    for ya,yb in [(top+ht*0.34, top+ht*0.55),(top+ht*0.60, top+ht*0.81)]:
        for i in range(na):
            t=i/(na-1.0)
            ax=cx-wd*0.9+wd*1.8*t
            aw=wd*0.05*(1-abs(t-0.5)*0.45)
            if aw<=0.3*S: continue
            rect(dark, ax-aw, ya+(yb-ya)*0.4, ax+aw, yb)
            rgba_ell("rgba(112,96,72,1.0)", ax, ya+(yb-ya)*0.4, aw, (yb-ya)*0.4, 180, 360)
    line(dk(stone,0.9), max(0.5,0.6*S), (cx-wd, top+ht*0.32), (cx+wd, top+ht*0.32))
    line(dk(stone,0.9), max(0.5,0.6*S), (cx-wd, top+ht*0.575), (cx+wd, top+ht*0.575))

colosseum(W*0.718, WL, H*0.072, H*0.140)

def pagoda(cx, by, wd, ht):
    red=(198,74,60); rdk=(150,52,44); roof=(120,66,58); gold=(240,200,90); wood=(150,96,60)
    gshadow(cx, by, wd*0.62, ht*0.05, 0.20)
    tiers=[(0.0,0.30,1.00),(0.28,0.26,0.80),(0.52,0.22,0.62),(0.72,0.0,0.42)]
    for i,(yb,bh,rw) in enumerate(tiers):
        y0=by-ht*yb
        if bh>0:
            rect(red, cx-wd*rw*0.5, y0-ht*bh, cx+wd*rw*0.5, y0)
            rect(rdk, cx+wd*rw*0.18, y0-ht*bh, cx+wd*rw*0.5, y0)
            rect(wood, cx-wd*rw*0.08, y0-ht*bh, cx+wd*rw*0.08, y0)
        ry=y0-ht*bh; rw2=wd*rw*0.84
        out.append("fill %s stroke none path 'M %.2f,%.2f Q %.2f,%.2f %.2f,%.2f Q %.2f,%.2f %.2f,%.2f Q %.2f,%.2f %.2f,%.2f Z'" % (
            hx(roof), cx-rw2, ry-ht*0.045, cx-rw2*0.42, ry-ht*0.135, cx, ry-ht*0.105,
            cx+rw2*0.42, ry-ht*0.135, cx+rw2, ry-ht*0.045,
            cx, ry+ht*0.055, cx-rw2, ry-ht*0.045))
        line(gold, max(0.8,1.0*S), (cx-rw2, ry-ht*0.045), (cx, ry-ht*0.10))
        line(gold, max(0.8,1.0*S), (cx, ry-ht*0.10), (cx+rw2, ry-ht*0.045))
    line(gold, max(1.4,2.0*S), (cx, by-ht*1.02), (cx, by-ht*1.16))
    ell(gold, cx, by-ht*1.16, wd*0.05, wd*0.05)

pagoda(W*0.820, H*0.585, H*0.115, H*0.185)

def great_wall():
    stone=(196,180,150); shd=(168,152,124); top=(214,200,172)
    pts=[(W*0.86, H*0.505),(W*0.90, H*0.470),(W*0.935, H*0.500),(W*0.965, H*0.455),(W, H*0.478)]
    for i in range(len(pts)-1):
        a,b=pts[i],pts[i+1]
        dx,dy=b[0]-a[0],b[1]-a[1]; L=math.hypot(dx,dy) or 1
        nx,ny=-dy/L,dx/L; th=H*0.055
        poly(stone, [(a[0],a[1]),(b[0],b[1]),(b[0]+nx*th,b[1]+ny*th),(a[0]+nx*th,a[1]+ny*th)])
        line(top, max(1.4,2.0*S), a, b)
        m=int(L/(10*S))+1
        for k in range(m):
            t=k/max(1,m-1)
            mx=a[0]+dx*t; my=a[1]+dy*t
            rect(top, mx-2*S, my-4*S, mx+2*S, my)
    for tx,ty in [(W*0.90, H*0.470),(W*0.965, H*0.455)]:
        gshadow(tx, ty+H*0.024, H*0.024, H*0.007, 0.15)
        rect(stone, tx-4*S, ty-H*0.05, tx+4*S, ty+H*0.02)
        rect(shd, tx+1*S, ty-H*0.05, tx+4*S, ty+H*0.02)
        rect(top, tx-5*S, ty-H*0.06, tx+5*S, ty-H*0.05)

great_wall()

road_y = WL - H*0.028
poly((92,94,104), [(W*0.30, WL+H*0.004), (W, WL-H*0.02), (W, road_y-H*0.030), (W*0.315, road_y-H*0.010)])
for i in range(11):
    t=i/10.0
    x=W*0.34+ (W*0.62)*t
    y=road_y-H*0.016 - t*H*0.006
    rect((236,214,120), x-6*S, y-1.4*S, x+6*S, y+1.4*S)

seaT=(100,180,220); seaB=(24,94,164)
NB=18
for i in range(NB):
    t0=i/NB; t1=(i+1)/NB
    y0=WL+(H-WL)*t0; y1=WL+(H-WL)*t1
    tm=((t0+t1)/2)**0.85
    c=tuple(int(round(seaT[k]+(seaB[k]-seaT[k])*tm)) for k in range(3))
    poly(c, [(-30*S, y0), (W+30*S, y0), (W+30*S, y1), (-30*S, y1)])
rgba_poly("rgba(214,236,238,0.55)", [(-30*S, WL), (W+30*S, WL), (W+30*S, WL+H*0.018), (-30*S, WL+H*0.018)])
random.seed(51)
for _ in range(78):
    t=random.random()
    sy=WL+H*0.02+(H-WL)*t
    sx=random.uniform(-20*S, W+20*S)
    sw=(4+30*t)*S
    rgba_ell("rgba(214,238,246,%.2f)"%(0.14+0.22*t), sx, sy, sw, max(0.8,1.0*S))
random.seed(9)
fx=W*0.285
while fx<W+10*S:
    rgba_ell("rgba(242,249,250,0.55)", fx, WL+random.uniform(-1.0,2.0)*S, random.uniform(7,13)*S, max(1.2,1.5*S))
    fx+=random.uniform(11,19)*S

sys.stdout.write(" ".join(out))
PY
)
# the scene is kept on its own transparent layer so the animated sun can ride
# between the sky and the terrain: it rises out of the sea and sets behind the
# eastern hills, occluded by whatever land or water is in front of it
convert -size ${W}x${H} xc:none -draw "$scene" "$w/scenelayer.png"

# ---------------------------------------------------------------------------
# static lighting washes and the wordmark, composited identically every frame
# ---------------------------------------------------------------------------
convert -size ${W}x${H} gradient:'rgba(255,255,255,0)'-'rgba(18,38,66,0.15)' "$w/botshade.png"
convert -size ${H}x${W} gradient:black-white -rotate 90 \
    -evaluate pow 2.0 -evaluate multiply 0.46 "$w/lmask.png"
convert -size ${W}x${H} xc:'#f8f2e4' "$w/lmask.png" \
    -alpha off -compose CopyOpacity -composite "$w/lwash.png"

P() { echo $(( $1 * S )); }
convert -background none -font "$fr" -pointsize $(P 54) -kerning $((1*S)) -fill '#28344f' label:'Nordstjernen ' "$w/t1.png"
convert -background none -font "$fr" -pointsize $(P 54) -fill '#b96a12' label:"$ver" "$w/t2.png"
convert -background none -font "$fr" -pointsize $(P 25) -fill '#295169' label:'Nordstjernen Web Browser' "$w/ts.png"
convert -background none -font "$fr" -pointsize $(P 20) -fill '#000000' -size $((700*S))x caption:"$codename" "$w/tc.png"

for n in t1 t2; do
    convert "$w/$n.png" -channel A -blur 0x$((3*S)) -level 0,55% +channel \
        -fill '#f6f1e4' -colorize 100 -channel A -evaluate multiply 0.55 +channel "$w/${n}g.png"
done
for n in ts tc; do
    convert "$w/$n.png" -channel A -blur 0x$((3*S)) -level 0,40% +channel \
        -fill '#f8f3e7' -colorize 100 "$w/${n}g.png"
done

w1=$(identify -format '%w' "$w/t1.png"); h1=$(identify -format '%h' "$w/t1.png")
hs=$(identify -format '%h' "$w/ts.png"); hc=$(identify -format '%h' "$w/tc.png")
g1=$((14*S))
ty=$((46*S)); textleft=$((80*S))
sy=$((ty + h1 + g1)); cy=$(( H - hc - 30*S ))

convert -size ${W}x${H} xc:none \
    "$w/t1g.png" -gravity NorthWest -geometry +${textleft}+${ty} -compose over -composite \
    "$w/t2g.png" -gravity NorthWest -geometry +$((textleft + w1))+${ty} -compose over -composite \
    "$w/tsg.png" -gravity NorthWest -geometry +${textleft}+${sy} -compose over -composite \
    "$w/tcg.png" -gravity NorthWest -geometry +${textleft}+${cy} -compose over -composite \
    "$w/t1.png" -gravity NorthWest -geometry +${textleft}+${ty} -compose over -composite \
    "$w/t2.png" -gravity NorthWest -geometry +$((textleft + w1))+${ty} -compose over -composite \
    "$w/ts.png" -gravity NorthWest -geometry +${textleft}+${sy} -compose over -composite \
    "$w/tc.png" -gravity NorthWest -geometry +${textleft}+${cy} -compose over -composite \
    "$w/textlayer.png"

# ---------------------------------------------------------------------------
# the world globe is raytraced when POV-Ray is available: a texture-mapped
# sphere lit each frame from the sun's current direction, composited exactly
# where the drawn globe otherwise goes; without povray the drawn globe stays
# ---------------------------------------------------------------------------
GLOBEMODE=drawn
if command -v povray >/dev/null 2>&1; then
    GLOBEMODE=pov
    mapdraw=$(python3 - <<'PY'
import math
Wm, Hm = 1024, 512
out = []
blobs = [(10,18,0.34),(52,-12,0.30),(96,30,0.24),(140,-28,0.30),(186,12,0.26),
         (232,-24,0.24),(280,32,0.28),(324,-14,0.24),(60,58,0.20),(200,-56,0.18)]
for lon, lat, sz in blobs:
    x = lon/360.0*Wm
    y = (0.5 - lat/180.0)*Hm
    rx = sz*163.0/max(0.35, math.cos(math.radians(lat)))
    ry = sz*163.0*0.8
    for wrap in (-Wm, 0, Wm):
        out.append("fill #56aa60 stroke none ellipse %.1f,%.1f %.1f,%.1f 0,360" % (x+wrap, y, rx, ry))
grat = "rgba(255,255,255,0.30)"
for k in range(12):
    x = k/12.0*Wm
    out.append("stroke %s stroke-width 2.2 line %.1f,0 %.1f,%d" % (grat, x, x, Hm))
for lat in (-60,-30,0,30,60):
    y = (0.5 - lat/180.0)*Hm
    out.append("stroke %s stroke-width 2.2 line 0,%.1f %d,%.1f" % (grat, y, Wm, y))
print(" ".join(out))
PY
)
    convert -size 1024x512 xc:'#2e78b4' -draw "$mapdraw" "$w/globemap.png"
    cat > "$w/globe.pov" <<'POV'
#version 3.7;
global_settings { assumed_gamma 2.2 }
background { rgbt <0,0,0,1> }
camera { orthographic location <0,0,-2.2> right <2.06,0,0> up <0,2.06,0> look_at <0,0,0> }
light_source { <LX,LY,-55> rgb <LR,LG,LB> }
light_source { <-30,45,-90> rgb <0.24,0.28,0.34> shadowless }
sphere { <0,0,0>, 1
  texture {
    pigment { image_map { png "globemap.png" map_type 1 interpolate 2 } }
    finish { ambient 0.34 diffuse 0.82 specular 0.30 roughness 0.045 }
  }
  rotate <0, SPIN, 0>
}
POV
    read -r GX GY GD <<<"$(python3 -c "W=$W;H=$H;R=H*0.185;print(round(W*0.80-R), round(H*0.235-R), round(2*R))")"
    convert -size ${W}x${H} xc:none -draw "$(python3 -c "W=$W;H=$H;R=H*0.185;print('fill rgba(20,60,110,0.35) stroke none ellipse %.1f,%.1f %.1f,%.1f 0,360'%(W*0.80+R*0.10, H*0.235+R*0.12, R*1.02, R*1.02))")" "$w/globeshadow.png"
fi

# ---------------------------------------------------------------------------
# per-frame animated layers
# ---------------------------------------------------------------------------

# the sun's arc, colour and visibility are shared between the sun layer (behind
# the terrain) and the lighting pass in gen_moving (over it) — keep in sync
SUNPATH='
WL = H*0.705
_lt=[(0.285,0.705),(0.34,0.66),(0.42,0.615),(0.52,0.60),(0.60,0.58),
     (0.70,0.585),(0.80,0.55),(0.90,0.505),(1.0,0.47)]
def land_top(xf):
    for i in range(len(_lt)-1):
        x0,y0=_lt[i]; x1,y1=_lt[i+1]
        if xf<=x1:
            t=(xf-x0)/(x1-x0) if x1>x0 else 0.0
            return H*(y0+(y1-y0)*max(0.0,min(1.0,t)))
    return H*_lt[-1][1]
def clamp01(v): return max(0.0, min(1.0, v))
def lerp3(a, b, t): return tuple(a[k]+(b[k]-a[k])*t for k in range(3))
su = clamp01((T - 0.05)/0.90)
sun_e = math.sin(math.pi*su)
sun_x = W*(0.055 + 0.885*su)
sun_y = H*(0.84 - 0.16*su) - H*0.50*sun_e
sun_r = H*0.050*(1.0 + 0.45*(1.0-sun_e))
horizon_y = WL if sun_x < W*0.285 else land_top(sun_x/W)
sun_vis = clamp01((horizon_y - (sun_y - sun_r))/(sun_r*2.6)) * clamp01(min(T, 1.0-T)/0.03)
sun_low = (1.0-sun_e)*sun_vis
'

gen_sun() {  # $1 = phase t in [0,1) -> the sun itself, drawn behind the scene
python3 - "$W" "$H" "$S" "$1" <<PY
import sys, math
W, H = int(sys.argv[1]), int(sys.argv[2])
S = float(sys.argv[3]); T = float(sys.argv[4])
TAU = 2*math.pi
$SUNPATH
out = []
def hx(c): return "#%02x%02x%02x" % tuple(int(max(0,min(255,v))) for v in c)
def rgba_ell(rgba, cx, cy, rx, ry):
    out.append("fill %s stroke none ellipse %.2f,%.2f %.2f,%.2f 0,360" % (rgba, cx, cy, rx, ry))
def ell(col, cx, cy, rx, ry):
    out.append("fill %s stroke none ellipse %.2f,%.2f %.2f,%.2f 0,360" % (hx(col), cx, cy, rx, ry))
core = lerp3((255,212,128), (255,251,238), sun_e)
mid  = lerp3((255,166,88),  (255,238,192), sun_e)
glow = lerp3((255,138,66),  (255,226,168), sun_e)
if sun_vis > 0.004:
    for i in range(18):
        t = i/17.0
        r = sun_r*(1.25 + 6.0*t*t)
        a = 0.115*sun_vis*(1.0-t)**2*(0.72+0.42*(1.0-sun_e))
        if a <= 0.008: continue
        rgba_ell("rgba(%d,%d,%d,%.3f)" % (glow[0],glow[1],glow[2],a),
                 sun_x, sun_y, r*(1.0+0.55*(1.0-sun_e)), r*(1.0-0.16*(1.0-sun_e)))
    band = 0.24*sun_vis*max(0.0, 1.0-sun_e*1.35)
    if band > 0.01:
        rgba_ell("rgba(255,176,96,%.3f)" % band, sun_x, horizon_y, W*0.24, H*0.030)
        rgba_ell("rgba(255,208,140,%.3f)" % (band*0.7), sun_x, horizon_y, W*0.13, H*0.018)
    rot = TAU*T*0.25
    for k in range(8):
        a = rot + k*math.pi/4
        L = sun_r*(2.25 + 0.25*math.sin(TAU*2*T + k))
        hw = sun_r*0.075
        tip = (sun_x+math.cos(a)*L, sun_y+math.sin(a)*L)
        bl = (sun_x+math.cos(a+math.pi/2)*hw, sun_y+math.sin(a+math.pi/2)*hw)
        br = (sun_x+math.cos(a-math.pi/2)*hw, sun_y+math.sin(a-math.pi/2)*hw)
        out.append("fill rgba(%d,%d,%d,%.3f) stroke none polygon %.2f,%.2f %.2f,%.2f %.2f,%.2f" % (
            mid[0], mid[1], mid[2], 0.34*sun_vis, tip[0],tip[1], bl[0],bl[1], br[0],br[1]))
ell(glow, sun_x, sun_y, sun_r*1.16, sun_r*1.16)
ell(mid,  sun_x, sun_y, sun_r*1.04, sun_r*1.04)
ell(core, sun_x, sun_y, sun_r*0.92, sun_r*0.92)
ell(lerp3(core,(255,255,250),0.55), sun_x - sun_r*0.16, sun_y - sun_r*0.20, sun_r*0.52, sun_r*0.50)
sys.stdout.write(" ".join(out))
PY
}

gen_globelight() {  # $1 = phase t -> povray Declare= arguments for the sun-lit globe
python3 - "$W" "$H" "$S" "$1" <<PY
import sys, math
W, H = int(sys.argv[1]), int(sys.argv[2])
S = float(sys.argv[3]); T = float(sys.argv[4])
$SUNPATH
cx, cy = W*0.80, H*0.235
dx, dy = sun_x-cx, sun_y-cy
L = math.hypot(dx, dy) or 1.0
nx = (dx/L)*sun_vis - 0.707*(1.0-sun_vis)
ny = min((dy/L)*sun_vis - 0.707*(1.0-sun_vis), -0.22)
Ln = math.hypot(nx, ny) or 1.0
nx, ny = nx/Ln, ny/Ln
lc = lerp3((1.0,0.97,0.93), (1.0,0.80,0.62), min(1.0, sun_low*1.2))
print("Declare=SPIN=%.2f Declare=LX=%.1f Declare=LY=%.1f Declare=LR=%.3f Declare=LG=%.3f Declare=LB=%.3f"
      % (T*360.0, nx*80.0, -ny*80.0, lc[0], lc[1], lc[2]))
PY
}

gen_moving() {  # $1 = phase t in [0,1)
python3 - "$W" "$H" "$S" "$1" "$GLOBEMODE" <<PY
import sys, math
W, H = int(sys.argv[1]), int(sys.argv[2])
S = float(sys.argv[3]); T = float(sys.argv[4])
GM = sys.argv[5] if len(sys.argv) > 5 else "drawn"
TAU = 2*math.pi
$SUNPATH
def warm(c, f): return lerp3(c, (255,204,148), clamp01(f))
def shadow_shift(cx, s): return max(-1.0, min(1.0, (cx-sun_x)/(W*0.35)))*s*0.4*sun_vis
wf = 0.38*sun_low
out = []
def hx(c): return "#%02x%02x%02x" % (int(max(0,min(255,c[0]))), int(max(0,min(255,c[1]))), int(max(0,min(255,c[2]))))
def dk(c, f=0.8): return tuple(max(0, int(v*f)) for v in c)
def lt(c, f=1.15): return tuple(min(255, int(v*f)) for v in c)
def poly(col, pts):
    out.append("fill %s stroke none polygon %s" % (hx(col), " ".join("%.2f,%.2f" % p for p in pts)))
def rgba_poly(rgba, pts):
    out.append("fill %s stroke none polygon %s" % (rgba, " ".join("%.2f,%.2f" % p for p in pts)))
def ell(col, cx, cy, rx, ry):
    out.append("fill %s stroke none ellipse %.2f,%.2f %.2f,%.2f 0,360" % (hx(col), cx, cy, rx, ry))
def rgba_ell(rgba, cx, cy, rx, ry, a0=0, a1=360):
    out.append("fill %s stroke none ellipse %.2f,%.2f %.2f,%.2f %d,%d" % (rgba, cx, cy, rx, ry, a0, a1))
def arc(col, wid, cx, cy, rx, ry, a0, a1):
    out.append("fill none stroke %s stroke-width %.2f ellipse %.2f,%.2f %.2f,%.2f %d,%d" % (col, wid, cx, cy, rx, ry, a0, a1))
    out.append("stroke none")
def line(col, wid, a, b):
    out.append("stroke %s stroke-width %.2f stroke-linecap round line %.2f,%.2f %.2f,%.2f" % (hx(col), wid, a[0], a[1], b[0], b[1]))
    out.append("stroke none")
def rect(col, x0, y0, x1, y1):
    out.append("fill %s stroke none rectangle %.2f,%.2f %.2f,%.2f" % (hx(col), x0, y0, x1, y1))

WL = H*0.705
road_y = WL - H*0.028

# ---- the world globe, slowly turning ----
def globe(cx, cy, R, spin):
    dx, dy = sun_x-cx, sun_y-cy
    L = math.hypot(dx, dy) or 1.0
    nx = (dx/L)*sun_vis - 0.707*(1.0-sun_vis)
    ny = min((dy/L)*sun_vis - 0.707*(1.0-sun_vis), -0.22)
    Ln = math.hypot(nx, ny) or 1.0
    nx, ny = nx/Ln, ny/Ln
    adeg = math.degrees(math.atan2(ny, nx)) % 360.0
    if GM == "pov":
        if sun_low > 0.05:
            rgba_ell("rgba(255,178,102,%.3f)" % (0.12*sun_low), cx+nx*R*0.30, cy+ny*R*0.30, R*0.80, R*0.80)
        arc("rgba(210,236,255,0.50)", max(1.0,1.4*S), cx, cy, R*0.99, R*0.99, int(adeg-55), int(adeg+55))
        return
    rgba_ell("rgba(20,60,110,0.35)", cx+R*0.10, cy+R*0.12, R*1.02, R*1.02)
    ell((46,120,180), cx, cy, R, R)
    ell((58,140,198), cx+nx*R*0.12, cy+ny*R*0.12, R*0.86, R*0.86)
    ell((70,155,210), cx+nx*R*0.24, cy+ny*R*0.24, R*0.66, R*0.66)
    land = (86,170,96)
    blobs = [(10,18,0.34),(52,-12,0.30),(96,30,0.24),(140,-28,0.30),(186,12,0.26),
             (232,-24,0.24),(280,32,0.28),(324,-14,0.24),(60,58,0.20),(200,-56,0.18)]
    order = sorted(blobs, key=lambda b: math.cos(math.radians(b[1]))*math.cos(math.radians(b[0]+spin)))
    for lon, lat, sz in order:
        lam = math.radians(lon + spin); phi = math.radians(lat)
        z = math.cos(phi)*math.cos(lam)
        if z <= 0.06: continue
        x3 = math.cos(phi)*math.sin(lam); y3 = math.sin(phi)
        px = cx + R*x3*0.985; py = cy - R*y3*0.985
        rx = sz*R*(0.35+0.65*z); ry = sz*R*(0.62+0.30*z)
        fade = min(1.0, (z-0.06)/0.26)
        c = tuple(int(round(land[k]*fade + (66,150,206)[k]*(1-fade))) for k in range(3))
        ell(dk(c,0.9), px+R*0.03, py+R*0.03, rx, ry)
        ell(c, px, py, rx, ry)
    grat = "rgba(255,255,255,0.22)"
    for k in (0.85, 0.55, 0.0):
        arc(grat, max(1.0,1.0*S), cx, cy, R*0.985, R*0.985*k, 0, 360)
    for mo in range(6):
        lam = math.radians(mo*30 + spin)
        z = math.cos(lam)
        if z <= 0.02: continue
        mrx = abs(R*0.985*math.sin(lam))
        arc(grat, max(0.8,0.9*S), cx, cy, mrx, R*0.985, 90, 270)
    ta = (adeg + 180.0) % 360.0
    rgba_ell("rgba(6,30,66,0.30)", cx, cy, R*0.985, R*0.985, int(ta-66), int(ta+66))
    rgba_ell("rgba(6,30,66,0.16)", cx, cy, R*0.985, R*0.985, int(ta-92), int(ta+92))
    if sun_low > 0.05:
        rgba_ell("rgba(255,178,102,%.3f)" % (0.14*sun_low), cx+nx*R*0.30, cy+ny*R*0.30, R*0.72, R*0.72)
    rgba_ell("rgba(255,255,255,0.60)", cx+nx*R*0.42, cy+ny*R*0.42, R*0.22, R*0.16)
    rgba_ell("rgba(255,255,255,0.30)", cx+nx*R*0.34, cy+ny*R*0.34, R*0.36, R*0.28)
    arc("rgba(210,236,255,0.55)", max(1.0,1.4*S), cx, cy, R*0.97, R*0.97, int(adeg-55), int(adeg+55))

globe(W*0.80, H*0.235, H*0.185, T*360.0)

# ---- airliner tracing a long contrail across the sky, wrapping seamlessly ----
def airliner(cx, cy, s, tilt=0.0):
    body = (238,241,247); trim = (70,120,210); dark = (52,62,86)
    poly(dk(body,0.9), [(cx-s*2.0, cy+s*0.20), (cx+s*1.5, cy+s*0.24), (cx+s*2.05, cy), (cx+s*1.5, cy-s*0.16)])
    ell(body, cx, cy, s*1.9, s*0.34)
    poly(body, [(cx+s*1.4, cy), (cx+s*2.35, cy-s*0.05), (cx+s*1.5, cy+s*0.05)])
    poly(dk(body,0.82), [(cx-s*1.7, cy-s*0.10), (cx-s*2.5, cy-s*0.72), (cx-s*2.05, cy-s*0.72), (cx-s*1.15, cy-s*0.08)])
    poly(dk(body,0.86), [(cx+s*0.55, cy+s*0.14), (cx-s*0.35, cy+s*1.05), (cx-s*0.95, cy+s*1.05), (cx-s*0.30, cy+s*0.18)])
    poly(lt(body,1.02), [(cx+s*0.7, cy-s*0.12), (cx-s*0.2, cy-s*0.98), (cx-s*0.75, cy-s*0.98), (cx-s*0.1, cy-s*0.16)])
    out.append("fill %s stroke none rectangle %.2f,%.2f %.2f,%.2f" % (hx(trim), cx-s*1.6, cy-s*0.06, cx+s*1.4, cy+s*0.04))
    for i in range(6):
        ell(dark, cx+s*(1.0-i*0.42), cy-s*0.02, s*0.06, s*0.06)

ax0, ay0 = W*0.285, H*0.078
travel = W*0.34
fp = (T*travel) % travel
plane_x = ax0 + fp
def edge_fade(x):
    d = min(x-ax0, (ax0+travel)-x)
    return max(0.0, min(1.0, d/(W*0.05)))
env = edge_fade(plane_x)
n_puff = 15
for i in range(n_puff):
    u = i/(n_puff-1.0)
    px = ax0 + fp*u
    py = ay0 - math.sin(u*fp/travel*math.pi)*H*0.020 + (fp/travel)*H*0.010*u
    a = (0.06 + 0.42*u) * env
    if a <= 0.02: continue
    rgba_ell("rgba(255,255,255,%.2f)" % a, px, py, W*0.012*(0.4+u), H*0.010*(0.4+u))
py = ay0 + (fp/travel)*H*0.010
if env > 0.02:
    airliner(plane_x, py, H*0.032)

# ---- the rocket: flickering flame, billowing exhaust, a gentle climb ----
def rocket(cx, by, s, flick, climb):
    body = (238,240,246); nose = (220,72,66); fin = (70,120,210)
    puffs = 8
    for i in range(puffs):
        age = (T*puffs + i) % puffs
        rise = age/puffs
        pa = 0.42*(1-rise)
        if pa <= 0.02: continue
        sway = math.sin(i*1.3 + rise*2.0)*s*0.6
        rgba_ell("rgba(236,240,248,%.2f)" % pa, cx+sway, by+s*1.8+rise*s*11.0,
                 s*(0.7+rise*2.4), s*(0.5+rise*2.0))
    fl = 1.0 + 0.35*flick
    poly((255,196,74), [(cx-s*0.30, by+s*1.6), (cx+s*0.30, by+s*1.6), (cx, by+s*(1.6+1.4*fl))])
    poly((255,140,52), [(cx-s*0.18, by+s*1.6), (cx+s*0.18, by+s*1.6), (cx, by+s*(1.6+0.8*fl))])
    poly((255,238,180), [(cx-s*0.10, by+s*1.6), (cx+s*0.10, by+s*1.6), (cx, by+s*(1.6+0.4*fl))])
    poly(fin, [(cx-s*0.42, by+s*1.7), (cx-s*0.42, by+s*1.0), (cx-s*0.18, by+s*1.55)])
    poly(fin, [(cx+s*0.42, by+s*1.7), (cx+s*0.42, by+s*1.0), (cx+s*0.18, by+s*1.55)])
    out.append("fill %s stroke none path 'M %.2f,%.2f L %.2f,%.2f Q %.2f,%.2f %.2f,%.2f Q %.2f,%.2f %.2f,%.2f Z'" % (
        hx(body), cx-s*0.28, by+s*1.7, cx-s*0.28, by+s*0.55,
        cx-s*0.28, by-s*0.5, cx, by-s*0.5,
        cx+s*0.28, by-s*0.5, cx+s*0.28, by+s*0.55, ))
    poly(nose, [(cx-s*0.28, by+s*0.4), (cx+s*0.28, by+s*0.4), (cx+s*0.28, by+s*0.62), (cx-s*0.28, by+s*0.62)])
    ell((120,170,220), cx, by+s*0.95, s*0.14, s*0.14)

rk = H*0.030
rocket(W*0.955, H*0.150 - math.sin(TAU*T)*rk*0.9, rk,
       math.sin(TAU*3*T)+0.32*math.sin(TAU*6*T+1.0), T)

# ---- a small flock, wings beating ----
def bird(cx, cy, s, beat):
    d = 0.10 + 0.32*(0.5+0.5*beat)
    line((58,72,92), max(1.0,1.6*S), (cx-s, cy+s*d), (cx, cy))
    line((58,72,92), max(1.0,1.6*S), (cx, cy), (cx+s, cy+s*d))

bx, by = W*0.360 + math.sin(TAU*T)*W*0.006, H*0.120 + math.sin(TAU*T+1.0)*H*0.004
for i in range(3):
    beat = math.sin(TAU*5*T - i*0.7)
    bird(bx + i*W*0.018, by + i*H*0.013, H*0.013, beat)
    bird(bx - i*W*0.018, by + i*H*0.013, H*0.013, math.sin(TAU*5*T - i*0.7 + 0.4))

# ---- the biplane, airborne now: drifting, bobbing, propeller a-blur ----
def biplane(cx, cy, s, prop):
    body = (226,86,72); cream = (240,228,196); dark = (58,44,40)
    gt = land_top(cx/W)
    sdx = max(-1.0, min(1.0, (cx-sun_x)/(W*0.45)))*H*0.05*sun_vis
    rgba_ell("rgba(30,42,34,%.3f)" % (0.08+0.05*sun_vis), cx+sdx, gt + (WL-gt)*0.55, s*1.15, s*0.16)
    line(dark, max(1.0,1.2*S), (cx-s*1.2, cy-s*0.55), (cx-s*1.2, cy+s*0.55))
    line(dark, max(1.0,1.2*S), (cx-s*0.5, cy-s*0.55), (cx-s*0.5, cy+s*0.55))
    poly(cream, [(cx-s*1.9, cy-s*0.62), (cx+s*0.6, cy-s*0.62), (cx+s*0.6, cy-s*0.48), (cx-s*1.9, cy-s*0.48)])
    ell(body, cx, cy, s*1.5, s*0.3)
    poly(cream, [(cx-s*1.9, cy+s*0.48), (cx+s*0.6, cy+s*0.48), (cx+s*0.6, cy+s*0.62), (cx-s*1.9, cy+s*0.62)])
    poly(body, [(cx-s*1.35, cy-s*0.06), (cx-s*2.0, cy-s*0.5), (cx-s*1.75, cy-s*0.5), (cx-s*1.0, cy-s*0.04)])
    rgba_ell("rgba(58,46,40,0.26)", cx+s*1.5, cy, s*0.16, s*0.62)
    bw = abs(math.cos(prop))*s*0.58
    line(dark, max(1.2,1.6*S), (cx+s*1.5, cy-bw), (cx+s*1.5, cy+bw))
    ell(lt(cream,1.05), cx-s*0.2, cy-s*0.05, s*0.28, s*0.16)

biplane(W*0.614 + math.sin(TAU*T)*W*0.009, H*0.156 + math.sin(TAU*T+2.0)*H*0.010,
        H*0.031, TAU*9*T)

# ---- a hot-air balloon drifting above the far coast ----
def balloon(cx, cy, r, c1, c2):
    bdx = max(-1.0, min(1.0, (sun_x-cx)/(W*0.30)))
    gt = land_top(cx/W)
    sdx = max(-1.0, min(1.0, (cx-sun_x)/(W*0.45)))*H*0.06*sun_vis
    rgba_ell("rgba(30,42,34,%.3f)" % (0.09+0.06*sun_vis), cx+sdx, gt + (WL-gt)*0.60, r*0.62, r*0.11)
    poly(dk(c1,0.9), [(cx-r*0.55, cy+r*0.55), (cx+r*0.55, cy+r*0.55), (cx+r*0.16, cy+r*1.05), (cx-r*0.16, cy+r*1.05)])
    ell(c1, cx, cy, r, r*1.12)
    ell(c2, cx-r*0.33, cy, r*0.34, r*1.12)
    ell(c2, cx+r*0.33, cy, r*0.34, r*1.12)
    rgba_ell("rgba(66,38,26,0.12)", cx-bdx*r*0.44, cy+r*0.14, r*0.38, r*0.78)
    ell(lt(c1,1.18), cx+bdx*r*0.30, cy-r*0.45, r*0.22, r*0.30)
    if wf > 0.02:
        rgba_ell("rgba(255,196,124,%.3f)" % (0.5*wf), cx+bdx*r*0.34, cy-r*0.30, r*0.34, r*0.52)
    line((90,64,40), max(1.0,1.2*S), (cx-r*0.32, cy+r*1.05), (cx-r*0.18, cy+r*1.45))
    line((90,64,40), max(1.0,1.2*S), (cx+r*0.32, cy+r*1.05), (cx+r*0.18, cy+r*1.45))
    poly((120,84,48), [(cx-r*0.20, cy+r*1.45), (cx+r*0.20, cy+r*1.45), (cx+r*0.16, cy+r*1.66), (cx-r*0.16, cy+r*1.66)])

balloon(W*0.690 + math.sin(TAU*T+0.7)*W*0.004, H*0.405 + math.sin(TAU*T)*H*0.016,
        H*0.050, (210,86,74), (238,200,96))

# ---- palm fronds swaying in the sea breeze ----
def palm_fronds(bx, by, h, sway):
    top=(bx-h*0.06, by-h)
    for k,a in enumerate((-0.9,-0.4,0.1,0.6,1.1)):
        aa = a + sway*(0.12 + 0.05*k)
        ex=top[0]+math.cos(aa)*h*0.5; ey=top[1]-abs(math.sin(aa+0.4))*h*0.10 - h*0.02 + aa*h*0.06
        poly((70,150,80), [top, (top[0]+math.cos(aa-0.12)*h*0.5, ey), (ex+math.cos(aa)*h*0.04, ey+h*0.05)])

palm_fronds(W*0.392, WL, H*0.115, math.sin(TAU*T))

# ---- a shooting star gliding the upper sky once each loop ----
sp = (T - 0.05) / 0.40
if 0.0 < sp < 1.0:
    hx0, hy0 = W*0.100, H*0.030
    hx1, hy1 = W*0.452, H*0.120
    hxp = hx0 + (hx1-hx0)*sp; hyp = hy0 + (hy1-hy0)*sp
    env = math.sin(math.pi*sp)
    L = math.hypot(hx1-hx0, hy1-hy0); ux, uy = (hx1-hx0)/L, (hy1-hy0)/L
    for j in range(14):
        tj = j/14.0
        a = env*0.5*(1-tj)
        if a <= 0.02: continue
        rr = W*0.006*(1-tj*0.7)
        rgba_ell("rgba(255,250,224,%.2f)" % a, hxp-ux*tj*W*0.10, hyp-uy*tj*W*0.10, rr, rr)
    rgba_ell("rgba(255,255,255,%.2f)" % env, hxp, hyp, W*0.007, W*0.007)

# ---- rolling swell: crests slide across the sea, shaded troughs behind them,
# ---- near rows drifting faster than far ones; phases advance whole cycles per
# ---- loop so the water wraps seamlessly ----
NR = 8
def swell(rows):
    for r in rows:
        fr = r/(NR-1.0)
        ry = WL + (H-WL)*(0.085 + 0.84*fr)
        amp = (1.8 + 5.2*fr)*S
        wl_ = 0.015/(0.5+0.85*fr)
        ph = TAU*(1 + (r % 2))*T + r*1.9
        npt = 30
        ew = (7+10*fr)*S
        la = 0.10 + 0.13*fr
        da = 0.06 + 0.09*fr
        for i in range(npt):
            xx = -26*S + (W+52*S)*i/(npt-1)
            yy = ry + amp*math.sin(xx*wl_ + ph) + amp*0.35*math.sin(xx*wl_*2.17 + ph*2.0)
            rgba_ell("rgba(232,245,250,%.2f)" % la, xx, yy, ew, max(0.9,1.1*S))
            rgba_ell("rgba(18,52,94,%.2f)" % da, xx + ew*0.55, yy + amp*0.75 + 1.2*S, ew*0.92, max(0.9,1.1*S))

swell(range(0, 6))

# ---- sun-glitter on the sea, trailing beneath the sun as it crosses ----
import random as _r
_r.seed(77)
gx = min(max(sun_x, W*0.06), W*0.94)
gstr = sun_vis*(0.42 + 0.58*(1.0-sun_e))
gcol = lerp3((255,246,212), (255,204,128), 1.0-sun_e)
if gstr > 0.03:
    rgba_ell("rgba(%d,%d,%d,%.3f)" % (gcol[0],gcol[1],gcol[2],0.10*gstr), gx, WL+(H-WL)*0.42, W*0.026, (H-WL)*0.46)
    rgba_ell("rgba(%d,%d,%d,%.3f)" % (gcol[0],gcol[1],gcol[2],0.07*gstr), gx, WL+(H-WL)*0.50, W*0.055, (H-WL)*0.52)
glints=[]
for i in range(30):
    t=i/30.0
    yy=WL+H*0.02+(H-WL)*t
    spread=(8+70*t)*S
    glints.append((gx+_r.uniform(-spread,spread), yy, _r.uniform(4,11)*S, 0.34*(1-t*0.55), _r.uniform(0,TAU)))
for gx2,gy2,gw,ga,ph in glints:
    tw = 0.45 + 0.55*(0.5+0.5*math.sin(TAU*3*T + ph))
    ga2 = ga*tw*gstr
    if ga2 <= 0.02: continue
    rgba_ell("rgba(%d,%d,%d,%.2f)" % (gcol[0],gcol[1],gcol[2],ga2), gx2, gy2, gw*(0.7+0.5*tw), max(1.0,1.1*S))

# ---- traffic on the shore road ----
def car(cx, by, L, col, kind="coupe"):
    dark=(40,44,54); glass=(160,202,228); tire=(38,38,44); hub=(150,150,158)
    h=L*0.42
    rgba_ell("rgba(28,32,38,0.24)", cx + max(-1.0,min(1.0,(cx-sun_x)/(W*0.35)))*L*0.22*sun_vis, by+L*0.02, L*0.56, L*0.11)
    if kind=="bus":
        h=L*0.62
        rect(dk(col,0.9), cx-L*0.5, by-h, cx+L*0.5, by-L*0.14)
        rect(col, cx-L*0.5, by-h, cx+L*0.5, by-h*0.5)
        for i in range(4):
            x=cx-L*0.4+ i*L*0.24
            rect(glass, x, by-h*0.86, x+L*0.14, by-h*0.55)
    else:
        rect(col, cx-L*0.5, by-h*0.55, cx+L*0.5, by-L*0.14)
        out.append("fill %s stroke none path 'M %.2f,%.2f Q %.2f,%.2f %.2f,%.2f L %.2f,%.2f Q %.2f,%.2f %.2f,%.2f Z'" % (
            hx(col), cx-L*0.24, by-h*0.55, cx-L*0.16, by-h, cx+L*0.06, by-h,
            cx+L*0.30, by-h, cx+L*0.30, by-h*0.55, cx-L*0.24, by-h*0.55))
        rect(glass, cx-L*0.18, by-h*0.92, cx+L*0.02, by-h*0.6)
        rect(glass, cx+L*0.05, by-h*0.92, cx+L*0.26, by-h*0.6)
        line(lt(col,1.22), max(0.6,0.8*S), (cx-L*0.44, by-h*0.5), (cx+L*0.28, by-h*0.5))
        rect(dk(col,0.86), cx-L*0.5, by-L*0.2, cx+L*0.5, by-L*0.14)
        rect((252,236,156), cx+L*0.46, by-h*0.42, cx+L*0.5, by-h*0.24)
        ell((220,70,60), cx-L*0.49, by-L*0.28, L*0.03, L*0.05)
    for wx in (cx-L*0.30, cx+L*0.30):
        ell(tire, wx, by-L*0.10, L*0.14, L*0.14)
        ell(hub, wx, by-L*0.10, L*0.06, L*0.06)
        ell(lt(hub,1.2), wx, by-L*0.10, L*0.025, L*0.025)

rx0, rx1 = W*0.335, W*1.02
rspan = rx1 - rx0
def road_pt(x):
    u = (x - W*0.34)/(W*0.62)
    return road_y - H*0.016 - u*H*0.006
cars = [(0.00, H*0.052, (216,72,66), "coupe"),
        (0.28, H*0.048, (244,196,72), "coupe"),
        (0.55, H*0.058, (74,150,196), "bus"),
        (0.78, H*0.050, (90,182,120), "coupe")]
for off, L, col, kind in cars:
    cx = rx0 + ((T + off) % 1.0)*rspan
    cf = max(0.0, min(1.0, min(cx-rx0, rx1-cx)/(W*0.04)))
    if cf <= 0.02: continue
    car(cx, road_pt(cx)+H*0.004, L, col, kind)

# ---- ships riding a gentle swell ----
def galleon(cx, wl, s):
    hull=(120,80,48); hdk=(92,60,36); flag=(212,72,66); rig=(74,54,38)
    sail=warm((246,242,232), wf); sdk=warm((216,210,194), wf*0.8)
    rgba_ell("rgba(40,36,30,0.16)", cx+s*0.1+shadow_shift(cx, s), wl+s*0.5, s*1.2, s*0.7)
    for i in range(7):
        wa = 0.20 - i*0.026
        if wa <= 0.02: break
        rgba_ell("rgba(236,244,247,%.2f)" % wa, cx - s*(1.6+i*0.78), wl + s*(0.26+0.05*i), s*(0.5+0.12*i), max(1.0,1.1*S)*(1.0+0.05*i))
    rgba_ell("rgba(244,250,252,0.55)", cx+s*1.32, wl+s*0.08, s*0.28, max(1.2,1.5*S))
    poly(hull, [(cx-s*1.3, wl), (cx+s*1.35, wl), (cx+s*1.0, wl+s*0.42), (cx-s*0.95, wl+s*0.42)])
    poly(hdk, [(cx-s*1.3, wl), (cx+s*1.35, wl), (cx+s*1.2, wl+s*0.16), (cx-s*1.15, wl+s*0.16)])
    poly((238,226,198), [(cx-s*1.3, wl-s*0.24), (cx+s*1.35, wl-s*0.24), (cx+s*1.35, wl), (cx-s*1.3, wl)])
    line((216,182,112), max(0.6,0.8*S), (cx-s*1.28, wl-s*0.12), (cx+s*1.33, wl-s*0.12))
    line(rig, max(1.0,1.2*S), (cx+s*1.18, wl-s*0.18), (cx+s*1.85, wl-s*0.52))
    tops=[]
    for mi,(mx,mh) in enumerate([(cx-s*0.6, s*1.9),(cx+s*0.15, s*2.3),(cx+s*0.82, s*1.7)]):
        line(rig, max(1.2,1.6*S), (mx, wl-s*0.24), (mx, wl-s*0.24-mh))
        top=wl-s*0.24-mh; tops.append((mx,top))
        poly(sail, [(mx-s*0.5, top+mh*0.18), (mx+s*0.5, top+mh*0.18), (mx+s*0.42, top+mh*0.52), (mx-s*0.42, top+mh*0.52)])
        poly(sdk, [(mx-s*0.44, top+mh*0.56), (mx+s*0.44, top+mh*0.56), (mx+s*0.34, top+mh*0.88), (mx-s*0.34, top+mh*0.88)])
        line(dk(sdk,0.9), max(0.4,0.5*S), (mx, top+mh*0.18), (mx, top+mh*0.88))
        wv=math.sin(TAU*5*T + mi*1.3)
        poly(flag, [(mx, top), (mx+s*(0.42+0.12*wv), top+s*(0.10+0.05*wv)),
                    (mx+s*0.05*wv, top+s*0.20)])
    line(rig, max(0.4,0.5*S), (cx+s*1.85, wl-s*0.52), tops[2])
    line(rig, max(0.4,0.5*S), tops[0], (cx-s*1.12, wl-s*0.08))

def steamer(cx, wl, s, puff):
    hull=(58,72,96); hdk=(40,52,72); stack=(196,80,64); dark=(52,54,62); gold=(224,190,120)
    cabin=warm((240,238,232), wf); csh=warm((210,208,202), wf*0.8)
    rgba_ell("rgba(40,44,56,0.16)", cx+shadow_shift(cx, s), wl+s*0.6, s*1.5, s*0.7)
    for i in range(8):
        wa = 0.22 - i*0.026
        if wa <= 0.02: break
        rgba_ell("rgba(234,242,246,%.2f)" % wa, cx - s*(2.0+i*0.85), wl + s*(0.28+0.045*i), s*(0.55+0.13*i), max(1.0,1.1*S)*(1.0+0.05*i))
    rgba_ell("rgba(244,250,252,0.55)", cx+s*1.62, wl+s*0.10, s*0.30, max(1.2,1.5*S))
    poly(hull, [(cx-s*1.7, wl), (cx+s*1.7, wl), (cx+s*1.3, wl+s*0.5), (cx-s*1.5, wl+s*0.5)])
    poly(hdk, [(cx-s*1.7, wl), (cx+s*1.7, wl), (cx+s*1.55, wl+s*0.18), (cx-s*1.6, wl+s*0.18)])
    line(gold, max(0.5,0.7*S), (cx-s*1.64, wl-s*0.02), (cx+s*1.64, wl-s*0.02))
    rect(cabin, cx-s*1.0, wl-s*0.7, cx+s*0.9, wl)
    rect(csh, cx+s*0.2, wl-s*0.7, cx+s*0.9, wl)
    for i in range(5):
        ell(dark, cx-s*0.8+ i*s*0.4, wl-s*0.4, s*0.075, s*0.075)
    rect(stack, cx-s*0.15, wl-s*1.5, cx+s*0.28, wl-s*0.7)
    rect(dark, cx-s*0.15, wl-s*1.5, cx+s*0.28, wl-s*1.4)
    line((60,44,34), max(1.0,1.2*S), (cx+s*1.48, wl-s*0.02), (cx+s*1.48, wl-s*1.0))
    pw=math.sin(TAU*5*T+0.8)
    poly((70,150,210), [(cx+s*1.48, wl-s*1.0),(cx+s*(1.98+0.14*pw), wl-s*(0.9-0.05*pw)),(cx+s*1.48, wl-s*0.78)])
    for i in range(5):
        age = (puff + i) % 5
        rise = age/5.0
        pa = (0.5-rise*0.42)
        if pa <= 0.02: continue
        rgba_ell("rgba(120,124,134,%.2f)" % pa, cx+s*0.05 - rise*s*2.4 + math.sin(i*1.4+rise)*s*0.3,
                 wl-s*1.7-rise*s*2.6, s*(0.3+rise*0.9), s*(0.26+rise*0.8))

def sailboat(cx, wl, s, dr=1):
    hull=(120,80,48); sail=warm((246,242,232), wf)
    rgba_ell("rgba(40,36,30,0.14)", cx+shadow_shift(cx, s), wl+s*0.42, s*0.8, s*0.4)
    for i in range(5):
        wa = 0.16 - i*0.03
        if wa <= 0.02: break
        rgba_ell("rgba(236,244,247,%.2f)" % wa, cx - dr*s*(1.0+i*0.62), wl + s*(0.20+0.05*i), s*(0.34+0.09*i), max(0.9,1.0*S))
    rgba_ell("rgba(244,250,252,0.45)", cx+dr*s*0.72, wl+s*0.06, s*0.20, max(1.0,1.2*S))
    poly(hull, [(cx-dr*s*0.8, wl), (cx+dr*s*0.8, wl), (cx+dr*s*0.55, wl+s*0.34), (cx-dr*s*0.55, wl+s*0.34)])
    line((70,50,34), max(1.0,1.2*S), (cx, wl), (cx, wl-s*1.5))
    poly(sail, [(cx+dr*s*0.06, wl-s*1.45), (cx+dr*s*0.06, wl-s*0.1), (cx+dr*s*0.7, wl-s*0.1)])
    poly(dk(sail,0.92), [(cx-dr*s*0.06, wl-s*1.2), (cx-dr*s*0.06, wl-s*0.1), (cx-dr*s*0.55, wl-s*0.1)])

def ship_x(off, dr):
    u = (T + off) % 1.0
    return W*(-0.16 + u*1.32) if dr > 0 else W*(1.16 - u*1.32)

galleon(ship_x(0.62, 1), WL+H*0.085 + math.sin(TAU*T)*H*0.006, H*0.062)
sailboat(ship_x(0.15, -1), WL+H*0.120 + math.sin(TAU*T+0.6)*H*0.006, H*0.050, -1)
steamer(ship_x(0.05, 1), WL+H*0.150 + math.sin(TAU*T+2.1)*H*0.005, H*0.052, T*5)
sailboat(ship_x(0.70, -1), WL+H*0.210 + math.sin(TAU*T+3.4)*H*0.005, H*0.044, -1)
swell(range(6, NR))

# ---- warm light spilling from the sun over land, sea and sky ----
spillc = lerp3((255,232,180), (255,184,110), 1.0-sun_e)
for i in range(9):
    t = i/8.0
    r = sun_r*(2.2 + 10.5*t*t)
    a = 0.050*sun_vis*(1.0-t)**2*(0.75+0.55*(1.0-sun_e))
    if a <= 0.006: continue
    rgba_ell("rgba(%d,%d,%d,%.3f)" % (spillc[0],spillc[1],spillc[2],a), sun_x, sun_y, r*1.3, r)

sys.stdout.write(" ".join(out))
PY
}

gen_star() {  # $1 = phase t in [0,1) -> spike/core primitives for a black canvas
python3 - "$W" "$H" "$S" "$1" <<'PY'
import sys, math
W, H = int(sys.argv[1]), int(sys.argv[2])
S = float(sys.argv[3]); T = float(sys.argv[4])
TAU = 2*math.pi
out = []
cx, cy = W*0.658, H*0.128
tw = 0.82 + 0.18*math.sin(TAU*T) + 0.06*math.sin(TAU*3*T)
rot = math.radians(5*math.sin(TAU*T))
Lr = H*0.120*(0.92+0.10*math.sin(TAU*2*T))
def hx(c): return "#%02x%02x%02x" % (int(max(0,min(255,c[0]))), int(max(0,min(255,c[1]))), int(max(0,min(255,c[2]))))
def spike(ang, length, halfw, col):
    a = ang+rot
    tip=(cx+math.cos(a)*length, cy+math.sin(a)*length)
    bl=(cx+math.cos(a+math.pi/2)*halfw, cy+math.sin(a+math.pi/2)*halfw)
    br=(cx+math.cos(a-math.pi/2)*halfw, cy+math.sin(a-math.pi/2)*halfw)
    out.append("fill %s stroke none polygon %.2f,%.2f %.2f,%.2f %.2f,%.2f" % (hx(col), tip[0],tip[1], bl[0],bl[1], br[0],br[1]))
warm=(255, int(236*tw+18), int(190*tw+34))
core=(int(255*tw), int(251*tw), int(236*tw))
ray=(int(255*tw), int(238*tw), int(168*tw))
dray=(int(252*tw), int(228*tw), int(160*tw))
# long thin lens-flare streaks beyond the star for a unique beacon
spike(0.0,        Lr*1.55, H*0.0018, dray); spike(math.pi,      Lr*1.55, H*0.0018, dray)
spike(math.pi/2,  Lr*1.7,  H*0.0018, dray); spike(-math.pi/2,   Lr*1.7,  H*0.0018, dray)
for k in range(4):
    spike(k*math.pi/2, Lr, H*0.013, ray)
for k in range(4):
    spike(k*math.pi/2 + math.pi/4, Lr*0.46, H*0.009, dray)
out.append("fill %s stroke none ellipse %.2f,%.2f %.2f,%.2f 0,360" % (hx(warm), cx, cy, H*0.033, H*0.033))
out.append("fill %s stroke none ellipse %.2f,%.2f %.2f,%.2f 0,360" % (hx(core), cx, cy, H*0.019, H*0.019))
out.append("fill #fffdf6 stroke none ellipse %.2f,%.2f %.2f,%.2f 0,360" % (cx, cy, H*0.010, H*0.010))

# companion stars — a quiet constellation twinkling around the North Star
comps = [(0.586,0.066,0.7),(0.704,0.070,1.9),(0.560,0.150,2.7),
         (0.694,0.166,3.6),(0.628,0.049,4.5),(0.542,0.104,5.3)]
for cxf,cyf,ph in comps:
    px, py = W*cxf, H*cyf
    ct = 0.40 + 0.60*(0.5+0.5*math.sin(TAU*T + ph))
    lr = H*0.024*(0.65+0.5*ct)
    cc = (int(255*ct), int(238*ct), int(172*ct))
    for k in range(4):
        a = k*math.pi/2
        tip=(px+math.cos(a)*lr, py+math.sin(a)*lr)
        bl=(px+math.cos(a+math.pi/2)*H*0.0035, py+math.sin(a+math.pi/2)*H*0.0035)
        br=(px+math.cos(a-math.pi/2)*H*0.0035, py+math.sin(a-math.pi/2)*H*0.0035)
        out.append("fill %s stroke none polygon %.2f,%.2f %.2f,%.2f %.2f,%.2f" % (hx(cc), tip[0],tip[1], bl[0],bl[1], br[0],br[1]))
    out.append("fill %s stroke none ellipse %.2f,%.2f %.2f,%.2f 0,360" % (hx((int(255*ct),int(246*ct),int(210*ct))), px, py, H*0.0055, H*0.0055))

sys.stdout.write(" ".join(out))
PY
}

# the North Star sits at a fixed point, so its soft bloom is blurred just once;
# only the crisp spikes and core are redrawn per frame
scx=$(python3 -c "print(int($W*0.658))"); scy=$(python3 -c "print(int($H*0.128))")
convert -size ${W}x${H} xc:black -fill '#9c7838' \
    -draw "ellipse ${scx},${scy} $((54*S)),$((54*S)) 0,360" -blur 0x$((30*S)) "$w/starglow.png"

# a single fixed grain field, blended identically into every frame: it breaks
# the sky/sea banding a 256-colour palette would otherwise show, yet stays
# byte-stable frame to frame so the static background still compresses away
convert -size 940x320 xc:gray50 -attenuate "$NOISE" +noise Gaussian \
    -colorspace Gray -blur 0x0.3 "$w/grain.png"

render_frame() {
    local i=$1
    local t sun moving star out cgeo povdec
    t=$(python3 -c "print(f'{$i/$FRAMES:.6f}')")
    sun=$(gen_sun "$t")
    moving=$(gen_moving "$t")
    star=$(gen_star "$t")
    cgeo=$(python3 -c "import math;T=$i/$FRAMES;print('%+d%+d'%(round(15*$S*math.sin(2*math.pi*T)), round(4*$S*math.sin(2*math.pi*T+1.1))))")
    out="$w/frame_$(printf '%03d' "$i").png"
    convert "$w/starglow.png" -draw "$star" "$w/star_${i}.png"
    if [ "$GLOBEMODE" = pov ]; then
        povdec=$(gen_globelight "$t")
        (cd "$w" && povray +Iglobe.pov +Og_${i}.png +W720 +H720 +UA -D +A0.3 +AM2 +Q9 $povdec >/dev/null 2>&1)
        convert "$w/sky2.png" -draw "$sun" \
            "$w/scenelayer.png" -compose over -composite \
            "$w/cloudlayer.png" -geometry "$cgeo" -compose over -composite \
            "$w/globeshadow.png" -geometry +0+0 -compose over -composite \
            \( "$w/g_${i}.png" -resize ${GD}x${GD} \) -geometry +${GX}+${GY} -compose over -composite \
            "$w/fc_${i}.png"
    else
        convert "$w/sky2.png" -draw "$sun" \
            "$w/scenelayer.png" -compose over -composite \
            "$w/cloudlayer.png" -geometry "$cgeo" -compose over -composite "$w/fc_${i}.png"
    fi
    convert "$w/fc_${i}.png" -draw "$moving" \
        "$w/botshade.png" -compose over   -composite \
        "$w/lwash.png"    -compose over   -composite \
        "$w/star_${i}.png" -compose screen -composite \
        "$w/textlayer.png" -compose over  -composite \
        -filter Lanczos -resize 940x320 \
        -unsharp 0x0.9+0.5+0.004 -modulate 101,108,100 -sigmoidal-contrast 1.8x50% \
        "$w/grain.png" -compose SoftLight -composite -strip "$out"
    rm -f "$w/star_${i}.png" "$w/fc_${i}.png" "$w/g_${i}.png"
}

echo "rendering $FRAMES frames for $ver $codename ..."
maxjobs=$(nproc 2>/dev/null || echo 4)
for ((i=0; i<FRAMES; i++)); do
    render_frame "$i" &
    while [ "$(jobs -r | wc -l)" -ge "$maxjobs" ]; do wait -n; done
done
wait
frames=()
for ((i=0; i<FRAMES; i++)); do frames+=("$w/frame_$(printf '%03d' "$i").png"); done
echo "rendered ${#frames[@]} frames"

# a single shared 256-colour palette keeps the static background byte-identical
# across frames, so gifsicle can diff away everything that does not move. The
# grain baked into the frames stands in for dithering, so the remap is
# dither-free — that keeps the unchanging pixels bit-for-bit equal frame to
# frame, which is what lets the animation stay small
convert "${frames[@]}" -append -colors 256 -unique-colors "$w/pal.gif"
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
