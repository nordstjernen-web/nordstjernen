#!/usr/bin/env bash
set -uo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${ND_BIN:-$ROOT/builddir/src/nordstjernen.exe}
OUT=${OUT:-$ROOT/data/screenshots}
LIST=${LIST:-$OUT/sites.txt}

mkdir -p "$OUT"

slugify() {
    printf '%s' "$1" | tr -cs 'A-Za-z0-9.-' '_' | sed 's/__*/_/g; s/^_//; s/_$//'
}

while IFS= read -r url; do
    case "$url" in ''|\#*) continue;; esac
    slug=$(slugify "$url")
    target="$OUT/$slug.png"
    printf 'render: %-60s ' "$url"
    rel_target="data/screenshots/$slug.png"
    ( cd "$ROOT" && "$BIN" --headless --dump=png:"$rel_target" --settle-ms=400 "$url" ) >/dev/null 2>&1 || true
    if [ -f "$target" ] && [ -s "$target" ]; then
        size=$(stat -c%s "$target" 2>/dev/null || echo 0)
        printf 'OK %sb\n' "$size"
    else
        printf 'NO OUTPUT\n'
    fi
done < "$LIST"
echo "--- done ---"
ls -la "$OUT"/*.png 2>/dev/null | awk '{print $5, $9}'
