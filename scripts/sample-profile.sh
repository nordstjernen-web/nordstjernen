#!/usr/bin/env bash
# Nordstjernen poor-man's sampling profiler.
#
# Attaches gdb to a running process repeatedly, grabs the top of the stack
# each time, and aggregates where the engine spends wall-clock time. Useful
# for attributing cost in a long-running headless run (e.g. a Speedometer
# workload) without a perf/instrumented build — a debugoptimized build with
# frame pointers is enough:
#
#   meson setup builddir-prof --buildtype=debugoptimized -Db_lto=false
#   meson compile -C builddir-prof
#
# Usage: scripts/sample-profile.sh <pid> [num-samples]
#   num-samples defaults to 120.
#
# Prints two tables: the leaf function (where the CPU actually is) and the
# inclusive frequency of Nordstjernen (nd_*) frames across all stacks.
set -euo pipefail

PID=${1:?usage: sample-profile.sh <pid> [num-samples]}
N=${2:-120}
SAMPLES=$(mktemp)
trap 'rm -f "$SAMPLES"' EXIT

if ! command -v gdb >/dev/null 2>&1; then
    echo "error: gdb not found" >&2
    exit 1
fi
if ! kill -0 "$PID" 2>/dev/null; then
    echo "error: no process with pid $PID" >&2
    exit 1
fi

echo "Sampling pid $PID, $N times ..." >&2
for _ in $(seq 1 "$N"); do
    kill -0 "$PID" 2>/dev/null || break
    gdb -p "$PID" --batch -nx \
        -ex "set pagination off" -ex "thread 1" -ex "bt 12" 2>/dev/null \
        | grep -E "^#" >> "$SAMPLES" || true
    echo "---SAMPLE---" >> "$SAMPLES"
done

echo
echo "=== Leaf function (innermost non-waiting frame), top 25 ==="
awk '
/^---SAMPLE---/ { if (leaf!="") print leaf; leaf=""; next }
/^#/ {
    if (leaf=="") {
        if (match($0, / in [A-Za-z_][A-Za-z0-9_]*/)) fn=substr($0, RSTART+4, RLENGTH-4)
        else { n=split($0,a," "); fn=a[2] }
        if (fn ~ /^(poll|__poll|ppoll|g_main_context|g_main_loop|epoll|read|__libc_(read|poll))/) { fn=""; next }
        if (fn!="") leaf=fn
    }
}
END { if (leaf!="") print leaf }
' "$SAMPLES" | sort | uniq -c | sort -rn | head -25

echo
echo "=== Inclusive nd_* frames across all stacks, top 25 ==="
grep -oE " in nd_[A-Za-z0-9_]+| nd_[A-Za-z0-9_]+ \(" "$SAMPLES" \
    | sed -E 's/ in //; s/ \(//; s/^ //' | grep -E "^nd_" \
    | sort | uniq -c | sort -rn | head -25
