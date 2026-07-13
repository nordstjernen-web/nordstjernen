#!/usr/bin/env bash
# Nordstjernen wpt-fast runner.
#
# Drives the wpt-fast checkout (https://github.com/nordstjernen-web/wpt-fast)
# through the browser's headless --wpt mode in parallel, writes a
# wptreport.json, and prints the per-standard compliance scores via the
# checkout's ./wpt-score. Only testharness.js tests run (reftests and
# crashtests are not supported by the headless harness and do not appear
# in the report).
#
# Usage: scripts/wpt-fast.sh [--fast-root=DIR] [--jobs=N] [--out=FILE] [PATH...]
#   PATH limits the run to subtrees of the checkout (default: all scored
#   directories).
#
# Options:
#   --fast-root=DIR   wpt-fast checkout (default ~/wpt-fast, or NS_WPT_FAST_ROOT)
#   --port=N          HTTP port for ./wpt serve (default 8100)
#   --jobs=N          parallel browser processes (default min(nproc, 8))
#   --timeout-ms=N    per-test timeout in ms (default 10000)
#   --out=FILE        wptreport output (default wptreport.json in CWD)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${NS_BIN:-$ROOT/builddir/src/gtk/nordstjernen}
export NS_ALLOW_ROOT=${NS_ALLOW_ROOT:-1}
FAST_ROOT=${NS_WPT_FAST_ROOT:-$HOME/wpt-fast}
PORT=8100
JOBS=$(nproc 2>/dev/null || echo 4)
[ "$JOBS" -gt 8 ] && JOBS=8
TIMEOUT_MS=10000
OUT=$PWD/wptreport.json
PATHS=()
for arg in "$@"; do
    case "$arg" in
        --fast-root=*)  FAST_ROOT=${arg#*=} ;;
        --port=*)       PORT=${arg#*=} ;;
        --jobs=*)       JOBS=${arg#*=} ;;
        --timeout-ms=*) TIMEOUT_MS=${arg#*=} ;;
        --out=*)        OUT=${arg#*=} ;;
        *)              PATHS+=("$arg") ;;
    esac
done
if [ ! -d "$FAST_ROOT" ]; then
    echo "error: wpt-fast checkout not found at $FAST_ROOT" >&2
    echo "  git clone --depth 1 https://github.com/nordstjernen-web/wpt-fast $FAST_ROOT" >&2
    exit 2
fi
FAST_ROOT=$(cd "$FAST_ROOT" && pwd)
if [ ! -x "$BIN" ]; then
    echo "error: browser binary not found at $BIN — run scripts/dev.sh build" >&2
    exit 1
fi
if [ "${#PATHS[@]}" -eq 0 ]; then
    PATHS=(dom domparsing shadow-dom url webstorage html css js ecmascript
           webidl wasm)
fi

BASE="http://localhost:$PORT"
list=$(mktemp)
resdir=$(mktemp -d)
SERVER_PID=""
cleanup() {
    rm -rf "$list" "$resdir"
    if [ -n "$SERVER_PID" ]; then
        kill -- -"$SERVER_PID" 2>/dev/null || kill "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

existing=()
for p in "${PATHS[@]}"; do
    [ -e "$FAST_ROOT/$p" ] && existing+=("$p")
done
"$ROOT/scripts/wpt-run.sh" --wpt-root="$FAST_ROOT" --list "${existing[@]}" > "$list"
total=$(wc -l < "$list")
echo "wpt-fast: $total test URLs, $JOBS jobs, base $BASE" >&2

if ! curl -sf --max-time 3 -o /dev/null "$BASE/"; then
    cat > /tmp/ns-wpt-fast-serve-config.json <<EOF
{"browser_host": "localhost",
 "alternate_hosts": {"alt": "alt.localhost"},
 "ports": {"http": [$PORT, $((PORT + 1))],
           "https": [null, null], "ws": [null], "wss": [null],
           "h2": [null]}}
EOF
    (cd "$FAST_ROOT" && setsid ./wpt serve --config /tmp/ns-wpt-fast-serve-config.json \
        >/tmp/ns-wpt-fast-serve.log 2>&1) &
    SERVER_PID=$!
    for _ in $(seq 1 60); do
        curl -sf --max-time 3 -o /dev/null "$BASE/" && break
        sleep 1
    done
    if ! curl -sf --max-time 3 -o /dev/null "$BASE/"; then
        echo "error: wpt-fast server did not come up at $BASE — see /tmp/ns-wpt-fast-serve.log" >&2
        exit 1
    fi
fi

export NS_FAST_BIN="$BIN" NS_FAST_BASE="$BASE" NS_FAST_RESDIR="$resdir"
export NS_FAST_TIMEOUT_MS="$TIMEOUT_MS"
export NS_FAST_TSEC=$(( TIMEOUT_MS / 1000 + 10 ))

xargs -P "$JOBS" -n 1 -a "$list" bash -c '
    url="$0"
    out=$(timeout -k 5 "$NS_FAST_TSEC" "$NS_FAST_BIN" --wpt \
          --wpt-timeout-ms="$NS_FAST_TIMEOUT_MS" "$NS_FAST_BASE$url" \
          2>/dev/null)
    rc=$?
    json=$(printf "%s\n" "$out" | sed -n "s/^WPT JSON //p" | head -1)
    f="$NS_FAST_RESDIR/$(printf "%s" "$url" | md5sum | cut -d" " -f1)"
    printf "%s\n%s\n%s\n" "$url" "$rc" "$json" > "$f"
' || true

python3 - "$resdir" "$OUT" <<'EOF'
import json, os, sys

resdir, out = sys.argv[1], sys.argv[2]
results = []
for name in os.listdir(resdir):
    with open(os.path.join(resdir, name), encoding='utf-8') as f:
        lines = f.read().split('\n')
    url = lines[0]
    rc = lines[1] if len(lines) > 1 else ''
    raw = lines[2] if len(lines) > 2 else ''
    entry = {"test": url, "status": "ERROR", "subtests": []}
    parsed = None
    if raw:
        try:
            parsed = json.loads(raw)
        except ValueError:
            parsed = None
    if parsed:
        harness = parsed.get("harness") or "ERROR"
        entry["status"] = "OK" if harness == "OK" else harness
        if parsed.get("message"):
            entry["message"] = parsed["message"]
        for s in parsed.get("subtests") or []:
            entry["subtests"].append({
                "name": s.get("name", ""),
                "status": s.get("status", "FAIL"),
                "message": s.get("message"),
            })
    elif rc in ("2", "124", "137"):
        entry["status"] = "TIMEOUT"
    results.append(entry)

with open(out, "w", encoding="utf-8") as f:
    json.dump({"results": results}, f)
print("wptreport: %s (%d tests)" % (out, len(results)))
EOF

"$FAST_ROOT/wpt-score" "$OUT"
