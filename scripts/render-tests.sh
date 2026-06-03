#!/bin/sh
# Render the visual test fixtures in data/render-tests/ to PNGs for manual
# inspection. These are not automated assertions — they are reference
# renderings that exercise HTML/CSS/JS features so regressions are easy to
# spot by eye. Build the browser first (scripts/dev.sh build).
#
# Usage: scripts/render-tests.sh [output-dir]
#   output-dir defaults to ./render-tests-out
set -eu

root=$(cd "$(dirname "$0")/.." && pwd)
bin="$root/builddir/src/nordstjernen"
src="$root/data/render-tests"
out="${1:-$root/render-tests-out}"
port=8137
vw="${ND_TEST_VIEWPORT:-800}"

[ -x "$bin" ] || { echo "build first: scripts/dev.sh build" >&2; exit 1; }
mkdir -p "$out"

python3 -m http.server "$port" --directory "$src" >/dev/null 2>&1 &
srv=$!
trap 'kill $srv 2>/dev/null || true' EXIT
sleep 1

for f in "$src"/*.html; do
    name=$(basename "$f" .html)
    "$bin" --headless --url="http://127.0.0.1:$port/$name.html" \
           --dump="png:$out/$name.png" --viewport="$vw" >/dev/null 2>&1 \
        && echo "  $name.png" \
        || echo "  $name FAILED" >&2
done
echo "Rendered into $out"
