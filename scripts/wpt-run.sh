#!/usr/bin/env bash
# Nordstjernen web-platform-tests (WPT) runner.
#
# Drives testharness.js tests from a web-platform-tests checkout through the
# browser's headless --wpt mode, which injects a completion hook before any
# document script runs and reports per-subtest results on stdout. This script
# enumerates tests, serves them with WPT's own ./wpt serve, runs each through
# the browser, and aggregates the results.
#
# Only testharness.js tests are supported (no reftests, no wdspec, no
# *.worker.js). See docs/wpt.md for details and setup.
#
# Build the browser first (scripts/dev.sh build), and clone WPT:
#   git clone --depth 1 https://github.com/web-platform-tests/wpt.git ~/wpt
#
# Usage: scripts/wpt-run.sh --wpt-root=DIR TEST-PATH...
#   TEST-PATH is a file or directory relative to the WPT checkout, e.g.
#   "dom/events" or "url/url-constructor.any.js".
#
# Options:
#   --wpt-root=DIR    WPT checkout directory (or set NS_WPT_ROOT)
#   --base=URL        server base URL (default http://web-platform.test:8000)
#   --no-serve        assume a WPT server is already running at --base
#   --timeout-ms=N    per-test timeout in ms (default 15000)
#   --results=FILE    append one JSON line per test to FILE
#   --list            print the enumerated test URLs and exit
#
# Environment overrides:
#   NS_BIN       browser binary (default builddir/src/gtk/nordstjernen)
#   NS_WPT_ROOT  default for --wpt-root
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${NS_BIN:-$ROOT/builddir/src/gtk/nordstjernen}
export NS_ALLOW_ROOT=${NS_ALLOW_ROOT:-1}
WPT_ROOT=${NS_WPT_ROOT:-}
BASE="http://web-platform.test:8000"
SERVE=1
TIMEOUT_MS=15000
RESULTS=""
LIST=0
PATHS=()

for arg in "$@"; do
    case "$arg" in
        --wpt-root=*)   WPT_ROOT=${arg#*=} ;;
        --base=*)       BASE=${arg#*=} ;;
        --no-serve)     SERVE=0 ;;
        --timeout-ms=*) TIMEOUT_MS=${arg#*=} ;;
        --results=*)    RESULTS=${arg#*=} ;;
        --list)         LIST=1 ;;
        --*)            echo "error: unknown option $arg" >&2; exit 2 ;;
        *)              PATHS+=("$arg") ;;
    esac
done

if [ -z "$WPT_ROOT" ] || [ ! -d "$WPT_ROOT" ]; then
    echo "error: --wpt-root=DIR (or NS_WPT_ROOT) must point at a web-platform-tests checkout" >&2
    exit 2
fi
WPT_ROOT=$(cd "$WPT_ROOT" && pwd)
if [ "${#PATHS[@]}" -eq 0 ]; then
    echo "error: no test paths given (e.g. scripts/wpt-run.sh --wpt-root=~/wpt dom/events)" >&2
    exit 2
fi
if [ "$LIST" -eq 0 ] && [ ! -x "$BIN" ]; then
    echo "error: browser binary not found at $BIN — run scripts/dev.sh build" >&2
    exit 1
fi

host=$(echo "$BASE" | sed -E 's#^[a-z]+://##; s#[:/].*$##')
case "$host" in
    localhost|127.0.0.1|::1|*.*.*.*) host_ok=1 ;;
    *) command -v getent >/dev/null 2>&1 && getent hosts "$host" >/dev/null 2>&1 && host_ok=1 || host_ok=0 ;;
esac
if [ "$LIST" -eq 0 ] && [ "$host_ok" -eq 0 ]; then
    echo "error: '$host' does not resolve. WPT needs its hosts entries:" >&2
    echo "  cd $WPT_ROOT && ./wpt make-hosts-file | sudo tee -a /etc/hosts" >&2
    echo "(or pass --base=http://localhost:8000 for origin-insensitive tests)" >&2
    exit 1
fi

collect_tests() {
    local p="$1" abs="$WPT_ROOT/$1"
    if [ -f "$abs" ]; then
        printf '%s\n' "$p"
        return
    fi
    if [ ! -d "$abs" ]; then
        echo "warning: $p not found under $WPT_ROOT — skipped" >&2
        return
    fi
    (cd "$WPT_ROOT" && find "$p" -type f \
        \( -name '*.html' -o -name '*.htm' -o -name '*.any.js' -o -name '*.window.js' \) \
        ! -path '*/resources/*' ! -path '*/support/*' ! -path '*/tools/*' \
        ! -path '*/crashtests/*' ! -name '*-ref.html' ! -name '*-manual.html' \
        | sort)
}

to_url() {
    local rel="$1"
    case "$rel" in
        *.any.js)    printf '/%s.any.html'    "${rel%.any.js}" ;;
        *.window.js) printf '/%s.window.html' "${rel%.window.js}" ;;
        *)           printf '/%s' "$rel" ;;
    esac
}

urls_for() {
    local rel="$1"
    local base
    base=$(to_url "$rel")
    local variants=""
    case "$rel" in
        *.html|*.htm)
            variants=$(grep -oE '<meta[[:space:]]+name="variant"[[:space:]]+content="[^"]*"' \
                       "$WPT_ROOT/$rel" 2>/dev/null \
                       | sed -E 's/.*content="([^"]*)".*/\1/' || true)
            ;;
    esac
    if [ -n "$variants" ]; then
        while IFS= read -r v; do
            [ -n "$v" ] || continue
            printf '%s%s\n' "$base" "$v"
        done <<< "$variants"
    else
        printf '%s\n' "$base"
    fi
}

is_testharness() {
    local rel="$1"
    case "$rel" in
        *.any.js|*.window.js) return 0 ;;
    esac
    grep -lq 'testharness\.js' "$WPT_ROOT/$rel" 2>/dev/null
}

TESTS=()
for p in "${PATHS[@]}"; do
    while IFS= read -r rel; do
        [ -n "$rel" ] || continue
        is_testharness "$rel" || continue
        TESTS+=("$rel")
    done < <(collect_tests "$p")
done
if [ "${#TESTS[@]}" -eq 0 ]; then
    echo "error: no testharness.js tests found under the given paths" >&2
    exit 1
fi

if [ "$LIST" -eq 1 ]; then
    for rel in "${TESTS[@]}"; do urls_for "$rel"; done
    exit 0
fi

SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill -- -"$SERVER_PID" 2>/dev/null || kill "$SERVER_PID" 2>/dev/null || true
    fi
}
trap cleanup EXIT

if ! curl -sf --max-time 3 -o /dev/null "$BASE/"; then
    if [ "$SERVE" -eq 0 ]; then
        echo "error: no WPT server reachable at $BASE (and --no-serve given)" >&2
        exit 1
    fi
    echo "Starting WPT server in $WPT_ROOT ..."
    SERVE_ARGS=()
    base_serve_host=${BASE#*://}; base_serve_host=${base_serve_host%%:*}; base_serve_host=${base_serve_host%%/*}
    if [ "$base_serve_host" = "localhost" ] || [ "$base_serve_host" = "127.0.0.1" ]; then
        printf '{"browser_host": "localhost", "alternate_hosts": {"alt": "alt.localhost"}}\n' \
            > /tmp/ns-wpt-serve-config.json
        SERVE_ARGS=(--config /tmp/ns-wpt-serve-config.json)
    fi
    (cd "$WPT_ROOT" && setsid ./wpt serve "${SERVE_ARGS[@]}" >/tmp/ns-wpt-serve.log 2>&1) &
    SERVER_PID=$!
    for _ in $(seq 1 60); do
        curl -sf --max-time 3 -o /dev/null "$BASE/" && break
        sleep 1
    done
    if ! curl -sf --max-time 3 -o /dev/null "$BASE/"; then
        echo "error: WPT server did not come up at $BASE — see /tmp/ns-wpt-serve.log" >&2
        exit 1
    fi
fi

total_files=0; ok_files=0; fail_files=0; timeout_files=0
total_sub=0; pass_sub=0; fail_sub=0; timeout_sub=0; notrun_sub=0; pf_sub=0

for rel in "${TESTS[@]}"; do
  while IFS= read -r url; do
    [ -n "$url" ] || continue
    total_files=$((total_files + 1))
    set +e
    out=$(timeout -k 5 "$(( TIMEOUT_MS / 1000 + 10 ))" \
          "$BIN" --wpt --wpt-timeout-ms="$TIMEOUT_MS" "$BASE$url" 2>/dev/null)
    rc=$?
    set -e
    if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then rc=2; fi
    summary=$(printf '%s\n' "$out" | sed -n 's/^WPT SUMMARY //p' | head -1)
    json=$(printf '%s\n' "$out" | sed -n 's/^WPT JSON //p' | head -1)
    case "$rc" in
        0) ok_files=$((ok_files + 1));           status="ok     " ;;
        2) timeout_files=$((timeout_files + 1)); status="TIMEOUT" ;;
        *) fail_files=$((fail_files + 1));       status="FAIL   " ;;
    esac
    echo "$status $url  ${summary:-no result}"
    if [ "$rc" -ne 0 ] && [ -n "$out" ]; then
        printf '%s\n' "$out" | grep -E '^WPT (FAIL|TIMEOUT|NOTRUN|HARNESS (ERROR|TIMEOUT|PRECONDITION_FAILED))' \
            | sed 's/^/    /' || true
    fi
    for kv in $summary; do
        n=${kv#*=}
        case "$kv" in
            total=*)   total_sub=$((total_sub + n)) ;;
            pass=*)    pass_sub=$((pass_sub + n)) ;;
            fail=*)    fail_sub=$((fail_sub + n)) ;;
            timeout=*) timeout_sub=$((timeout_sub + n)) ;;
            notrun=*)  notrun_sub=$((notrun_sub + n)) ;;
            precondition_failed=*) pf_sub=$((pf_sub + n)) ;;
        esac
    done
    if [ -n "$RESULTS" ]; then
        printf '{"test":"%s","exit":%d,"result":%s}\n' \
               "$url" "$rc" "${json:-null}" >> "$RESULTS"
    fi
  done < <(urls_for "$rel")
done

echo
echo "files: $total_files total, $ok_files ok, $fail_files fail, $timeout_files timeout"
echo "subtests: $total_sub total, $pass_sub pass, $fail_sub fail, $timeout_sub timeout, $notrun_sub notrun, $pf_sub precondition_failed"
[ -n "$RESULTS" ] && echo "results: $RESULTS"

[ "$fail_files" -eq 0 ] && [ "$timeout_files" -eq 0 ]
