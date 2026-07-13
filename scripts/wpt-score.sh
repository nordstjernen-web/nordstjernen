#!/usr/bin/env bash
# Nordstjernen WPT score generator.
#
# Runs all of the fixed web-platform-tests slice tracked in
# docs/wpt-scores.md, or just the given parts of it, through
# scripts/wpt-run.sh. Results merge into docs/wpt-subtests.tsv (the
# canonical per-subtest state; untouched areas carry over), a full
# per-file snapshot is written to docs/wpt-runs/DATE-REV.tsv, and the
# history row plus per-area table for docs/wpt-scores.md are printed.
#
# Usage: scripts/wpt-score.sh --wpt-root=DIR [PATH...]
#   PATH is a tracked area or a sub-path of one (e.g. "dom/nodes" or
#   "url/url-tojson.any.js"). No PATHs means a full run.
# Options are passed through to wpt-run.sh (--base, --no-serve,
# --timeout-ms). Build the browser first (scripts/dev.sh build).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

SLICES=(
    dom/nodes
    dom/events
    dom/traversal
    dom/ranges
    dom/lists
    dom/collections
    url
    console
    hr-time
    html/webappapis/atob
    html/webappapis/timers
    html/dom/elements
    WebCryptoAPI/digest
    xhr/formdata
    html/semantics/forms/the-form-element
)

WPT_ROOT=${NS_WPT_ROOT:-}
OPTS=()
PATHS=()
for arg in "$@"; do
    case "$arg" in
        --wpt-root=*) WPT_ROOT=${arg#*=}; OPTS+=("$arg") ;;
        --*)          OPTS+=("$arg") ;;
        *)            PATHS+=("${arg%/}") ;;
    esac
done
if [ -z "$WPT_ROOT" ] || [ ! -d "$WPT_ROOT" ]; then
    echo "error: --wpt-root=DIR (or NS_WPT_ROOT) must point at a web-platform-tests checkout" >&2
    exit 2
fi

in_slice() {
    local p="$1" a
    for a in "${SLICES[@]}"; do
        case "$p" in "$a"|"$a"/*) return 0 ;; esac
    done
    return 1
}

note="full"
if [ "${#PATHS[@]}" -eq 0 ]; then
    PATHS=("${SLICES[@]}")
else
    for p in "${PATHS[@]}"; do
        if ! in_slice "$p"; then
            echo "error: $p is not within the tracked slice (see docs/wpt-scores.md)" >&2
            exit 2
        fi
    done
    note="partial: ${PATHS[*]}"
fi

ns_rev=$(git -C "$ROOT" rev-parse --short HEAD)
wpt_rev=$(git -C "$WPT_ROOT" rev-parse --short HEAD)
date=$(date -u +%Y-%m-%d)
jsonl=$(mktemp)
trap 'rm -f "$jsonl"' EXIT

prefixes=""
for p in "${PATHS[@]}"; do
    [ -d "$WPT_ROOT/$p" ] && prefixes="$prefixes,/$p/"
done

for p in "${PATHS[@]}"; do
    "$ROOT/scripts/wpt-run.sh" --results="$jsonl" "${OPTS[@]}" "$p" >&2 || true
    echo "done: $p" >&2
done

mkdir -p "$ROOT/docs/wpt-runs"
python3 - "$ROOT/docs/wpt-subtests.tsv" "$jsonl" \
    "$ROOT/docs/wpt-runs/$date-$ns_rev.tsv" "$ROOT/docs/wpt-scores.md" \
    "$date" "$ns_rev" "$wpt_rev" "$note" "${prefixes#,}" "${SLICES[@]}" <<'EOF'
import json, os, sys

subtsv, jsonl, filetsv, doc, date, ns_rev, wpt_rev, note, prefixes = sys.argv[1:10]
areas = sys.argv[10:]
prefixes = [p for p in prefixes.split(',') if p]

def read_doc(path):
    try:
        return open(path, encoding='utf-8').read().splitlines()
    except UnicodeDecodeError:
        return open(path, encoding='cp1252').read().splitlines()

old = []
if os.path.exists(subtsv):
    with open(subtsv, encoding='utf-8') as f:
        next(f)
        old = [tuple(line.rstrip('\n').split('\t')) for line in f]

new = []
new_tests = set()
for line in open(jsonl, encoding='utf-8'):
    d = json.loads(line)
    r = d.get('result') or {}
    new_tests.add(d['test'])
    new.append((d['test'], '<harness>', r.get('harness') or 'NONE'))
    for s in (r.get('subtests') or []):
        name = (s.get('name') or '')
        name = name.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ')
        new.append((d['test'], name, s.get('status', 'FAIL')))

for p in prefixes:
    if (not any(t.startswith(p) for t in new_tests)
            and any(r[0].startswith(p) for r in old)):
        sys.exit("error: rerun of %s produced no results but %s has data "
                 "for it — refusing to erase it (server down?)" % (p, subtsv))

def dropped(row):
    t = row[0]
    return t in new_tests or any(t.startswith(p) for p in prefixes)

merged = sorted([r for r in old if not dropped(r)] + new)
with open(subtsv, 'w', encoding='utf-8') as f:
    f.write("test\tsubtest\tstatus\n")
    for r in merged:
        f.write('\t'.join(r) + '\n')

files = {}
for test, sub, status in merged:
    e = files.setdefault(test, {'harness': 'NONE', 'PASS': 0, 'FAIL': 0,
                                'TIMEOUT': 0, 'NOTRUN': 0,
                                'PRECONDITION_FAILED': 0})
    if sub == '<harness>':
        e['harness'] = status
    else:
        e[status] = e.get(status, 0) + 1

with open(filetsv, 'w', encoding='utf-8') as f:
    f.write("test\tharness\ttotal\tpass\tfail\ttimeout\tnotrun\tprecondition_failed\n")
    for test in sorted(files):
        e = files[test]
        total = sum(e[k] for k in ('PASS', 'FAIL', 'TIMEOUT', 'NOTRUN',
                                   'PRECONDITION_FAILED'))
        f.write('\t'.join(str(x) for x in (test, e['harness'], total,
                e['PASS'], e['FAIL'], e['TIMEOUT'], e['NOTRUN'],
                e['PRECONDITION_FAILED'])) + '\n')

def area_of(test):
    for a in areas:
        if test.startswith('/' + a + '/'):
            return a
    return None

stats = {a: [0, 0, 0, 0, 0, 0, 0, 0] for a in areas}
for test, e in files.items():
    a = area_of(test)
    if a is None:
        continue
    s = stats[a]
    ok = (e['harness'] == 'OK' and e['FAIL'] == 0 and e['TIMEOUT'] == 0
          and e['NOTRUN'] == 0 and e['PRECONDITION_FAILED'] == 0)
    s[0] += 1
    s[1] += 1 if ok else 0
    s[2] += sum(e[k] for k in ('PASS', 'FAIL', 'TIMEOUT', 'NOTRUN',
                               'PRECONDITION_FAILED'))
    s[3] += e['PASS']
    s[4] += e['FAIL']
    s[5] += e['TIMEOUT']
    s[6] += e['NOTRUN']
    s[7] += e['PRECONDITION_FAILED']

tot = [sum(stats[a][i] for a in areas) for i in range(8)]
def pct(n, d):
    return 100 * n // d if d else 0

hist_row = "| %s | %s | %s | %d/%d (%d%%) | %d/%d (%d%%) | %s |" % (
    date, ns_rev, wpt_rev, tot[1], tot[0], pct(tot[1], tot[0]),
    tot[3], tot[2], pct(tot[3], tot[2]), note)

section = [
    "## Per-area results — %s" % date,
    "",
    "Per-file detail for this run: `docs/wpt-runs/%s-%s.tsv`." % (date, ns_rev),
    "",
    "| Area | Files ok | Subtests passing | Fail | Timeout | Notrun | Precondition failed |",
    "|------|----------|------------------|------|---------|--------|---------------------|",
]
for a in areas:
    s = stats[a]
    section.append("| `%s` | %d/%d | %d/%d | %d | %d | %d | %d |" % (
        a, s[1], s[0], s[3], s[2], s[4], s[5], s[6], s[7]))
section.append("| **Total** | **%d/%d** | **%d/%d** | **%d** | **%d** | **%d** | **%d** |" % (
    tot[1], tot[0], tot[3], tot[2], tot[4], tot[5], tot[6], tot[7]))
section.append("")

roi = {a: [0, 0, 0, 0] for a in areas}
for test, e in files.items():
    a = area_of(test)
    if a is None:
        continue
    r = roi[a]
    nonpass = sum(e[k] for k in ('FAIL', 'TIMEOUT', 'NOTRUN',
                                 'PRECONDITION_FAILED'))
    broken = e['harness'] != 'OK'
    r[0] += nonpass
    r[1] += 1 if (nonpass or broken) else 0
    r[2] += 1 if broken else 0
    r[3] += 1 if (not broken and 0 < nonpass <= 2) else 0

roi_section = [
    "## ROI by area — %s" % date,
    "",
    "Where score is cheapest to win, from the same data. Available",
    "gain is the non-passing subtest count (sorted descending);",
    "gain per affected file is its density — high values mean one",
    "root cause likely flips many subtests. Harness-broken files",
    "never report (usually one missing API hangs the page) and their",
    "real gain is understated, since their unreported subtests count",
    "zero. Near-ok files are at most two subtests away from a clean",
    "file.",
    "",
    "| Area | Available gain | Affected files | Gain/file | Harness-broken | Near-ok |",
    "|------|----------------|----------------|-----------|----------------|---------|",
]
for a in sorted(areas, key=lambda a: -roi[a][0]):
    r = roi[a]
    density = "%.1f" % (r[0] / r[1]) if r[1] else "-"
    roi_section.append("| `%s` | %d | %d | %s | %d | %d |" % (
        a, r[0], r[1], density, r[2], r[3]))
roi_section.append("")

lines = read_doc(doc)

hi = lines.index("## History")
last = None
for i in range(hi + 1, len(lines)):
    if lines[i].startswith("## "):
        break
    if lines[i].startswith("|"):
        last = i
lines.insert(last + 1, hist_row)

pi = next(i for i, l in enumerate(lines) if l.startswith("## Per-area results"))
pj = next(i for i in range(pi + 1, len(lines)) if lines[i].startswith("## "))
lines[pi:pj] = section

ri = next((i for i, l in enumerate(lines) if l.startswith("## ROI by area")), None)
if ri is None:
    ri = pi + len(section)
    lines[ri:ri] = roi_section
else:
    rj = next(i for i in range(ri + 1, len(lines)) if lines[i].startswith("## "))
    lines[ri:rj] = roi_section

with open(doc, 'w', encoding='utf-8') as f:
    f.write('\n'.join(lines) + '\n')

print(hist_row)
print("updated: %s" % doc)
EOF
echo "per-file snapshot: $ROOT/docs/wpt-runs/$date-$ns_rev.tsv" >&2
echo "per-subtest state: $ROOT/docs/wpt-subtests.tsv" >&2
