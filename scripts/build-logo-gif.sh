#!/usr/bin/env bash
set -euo pipefail

cd "$(dirname "$0")/.."

out="data/icons/hicolor/scalable/apps/nordstjernen.gif"
size=128
frames=24
trail=6

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

emit_frame() {
    local frame=$1 path=$2
    local cx cy
    local i ax ay aop ar

    {
        cat <<'HEADER'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256" width="256" height="256">
  <defs>
    <radialGradient id="sky" cx="50%" cy="42%" r="78%">
      <stop offset="0%"   stop-color="#1a2b6b"/>
      <stop offset="55%"  stop-color="#070d3a"/>
      <stop offset="100%" stop-color="#01030f"/>
    </radialGradient>
    <linearGradient id="nfill" x1="0" y1="0" x2="0" y2="1">
      <stop offset="0%"   stop-color="#ffffff"/>
      <stop offset="55%"  stop-color="#eef2fb"/>
      <stop offset="100%" stop-color="#aab5cc"/>
    </linearGradient>
    <radialGradient id="halo" cx="50%" cy="50%" r="50%">
      <stop offset="0%"   stop-color="#fff7c2" stop-opacity="0.85"/>
      <stop offset="35%"  stop-color="#fff2a0" stop-opacity="0.35"/>
      <stop offset="70%"  stop-color="#fff2a0" stop-opacity="0.05"/>
      <stop offset="100%" stop-color="#fff2a0" stop-opacity="0"/>
    </radialGradient>
    <radialGradient id="starGrad" cx="50%" cy="40%" r="65%">
      <stop offset="0%"   stop-color="#ffffff"/>
      <stop offset="60%"  stop-color="#f0f4ff"/>
      <stop offset="100%" stop-color="#b9c4e8"/>
    </radialGradient>
    <radialGradient id="core" cx="50%" cy="50%" r="50%">
      <stop offset="0%"   stop-color="#ffffff"/>
      <stop offset="40%"  stop-color="#fff4b8"/>
      <stop offset="100%" stop-color="#ffba2f"/>
    </radialGradient>
    <radialGradient id="comet" cx="50%" cy="50%" r="50%">
      <stop offset="0%"   stop-color="#ffffff"/>
      <stop offset="35%"  stop-color="#fff5b0"/>
      <stop offset="100%" stop-color="#ffba2f" stop-opacity="0"/>
    </radialGradient>
    <clipPath id="card">
      <rect x="0" y="0" width="256" height="256" rx="36" ry="36"/>
    </clipPath>
  </defs>
  <g clip-path="url(#card)">
    <rect width="256" height="256" fill="url(#sky)"/>
HEADER

        local stars=(
            "22 34 1.2" "44 18 0.8" "62 58 1.5" "86 26 0.9"
            "14 92 1.0" "36 118 0.7" "18 156 1.3" "42 200 0.9"
            "70 232 1.1" "106 222 0.8" "148 238 1.4" "188 218 0.9"
            "216 240 1.0" "234 196 1.3" "242 148 0.9" "222 112 1.1"
            "238 74 0.8" "212 40 1.4" "176 22 0.9" "148 42 1.0"
            "120 18 0.7" "92 200 0.8" "200 170 0.9" "60 170 0.7"
        )
        local idx=0
        for star in "${stars[@]}"; do
            read -r sx sy sr <<<"$star"
            local op
            op=$(awk -v f="$frame" -v i="$idx" -v F="$frames" '
                BEGIN {
                  phase = (i * 1.97) % F;
                  v = 0.55 + 0.4 * (0.5 + 0.5 * sin(2*3.14159265 * (f + phase) / F));
                  printf "%.2f", v
                }')
            printf '    <circle cx="%s" cy="%s" r="%s" fill="#ffffff" opacity="%s"/>\n' \
                "$sx" "$sy" "$sr" "$op"
            idx=$((idx + 1))
        done

        cat <<'NFIX'
    <text x="128" y="196" text-anchor="middle"
          font-family="Georgia, 'Times New Roman', 'DejaVu Serif', 'Liberation Serif', serif"
          font-size="184" font-weight="bold" fill="url(#nfill)" opacity="0.92">N</text>
    <circle cx="128" cy="128" r="86" fill="url(#halo)"/>
    <polygon points="128,18 138,118 238,128 138,138 128,238 118,138 18,128 118,118" fill="url(#starGrad)" opacity="0.55"/>
    <polygon points="128,62 132,124 194,128 132,132 128,194 124,132 62,128 124,124" fill="#ffffff" opacity="0.30"/>
    <circle cx="128" cy="128" r="9" fill="url(#core)"/>
    <circle cx="125" cy="125" r="2.6" fill="#ffffff" opacity="0.9"/>
NFIX

        local active=24
        if (( frame < active )); then
            for (( i = trail; i >= 0; i-- )); do
                local tframe=$((frame - i))
                if (( tframe < 0 )); then continue; fi
                local read_coords
                read_coords=$(awk -v f="$tframe" -v A="$active" '
                    BEGIN {
                      t = f / (A - 1);
                      x = 240 - 224 * t;
                      y = 30 + 196 * t + 28 * sin(3.14159265 * t);
                      printf "%.1f %.1f", x, y
                    }')
                read -r ax ay <<<"$read_coords"
                aop=$(awk -v i="$i" -v T="$trail" '
                    BEGIN {
                      printf "%.2f", 1.0 - (i / (T + 1))
                    }')
                ar=$(awk -v i="$i" -v T="$trail" '
                    BEGIN {
                      printf "%.1f", 10 - 7 * (i / T)
                    }')
                printf '    <circle cx="%s" cy="%s" r="%s" fill="url(#comet)" opacity="%s"/>\n' \
                    "$ax" "$ay" "$ar" "$aop"
            done
        fi

        cat <<'FOOTER'
  </g>
</svg>
FOOTER
    } > "$path"
}

for (( f = 0; f < frames; f++ )); do
    emit_frame "$f" "$work/f$(printf '%02d' "$f").svg"
    rsvg-convert -w "$size" -h "$size" "$work/f$(printf '%02d' "$f").svg" \
        -o "$work/f$(printf '%02d' "$f").png"
done

convert -delay 6 -loop 0 "$work"/f*.png "$work/raw.gif"
gifsicle -O3 --colors 64 "$work/raw.gif" -o "$out"
echo "wrote $out ($(stat -c%s "$out") bytes)"
