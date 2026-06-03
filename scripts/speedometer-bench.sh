#!/usr/bin/env bash
# Nordstjernen Speedometer 3.1 benchmark harness.
#
# Speedometer's own runner loads every workload inside an <iframe>, and
# Nordstjernen renders iframes as display:none and runs no nested browsing
# context, so the official aggregate harness cannot drive the engine. This
# script instead loads each Speedometer 3.1 TodoMVC workload directly and
# replays Speedometer's *own* per-suite interaction steps (Adding100Items,
# CompletingAllItems, DeletingAllItems) against the top-level document, timing
# each phase with the same sync + forced-layout-async method the real runner
# uses.
#
# Speedometer's release/3.1 sources are fetched at runtime into a scratch
# directory; nothing third-party is vendored into this repository. The page
# driver is generated from those sources on the fly.
#
# Build the browser first (scripts/dev.sh build).
#
# Usage: scripts/speedometer-bench.sh [suite-substring]
#   With no argument every loadable TodoMVC suite is run. With an argument
#   only suites whose path contains the substring run (e.g. "react", "complex").
#
# Environment overrides:
#   ND_BIN              browser binary (default builddir/src/nordstjernen)
#   ND_SPEEDOMETER_DIR  scratch checkout (default $TMPDIR/nd-speedometer31)
#   ND_ITERS            iterations per suite, median reported (default 3)
#   ND_SETTLE           headless settle budget in ms (default 8000)
#   ND_PORT             local HTTP port (default 8123)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${ND_BIN:-$ROOT/builddir/src/nordstjernen}
export ND_ALLOW_ROOT=${ND_ALLOW_ROOT:-1}
WORK=${ND_SPEEDOMETER_DIR:-${TMPDIR:-/tmp}/nd-speedometer31}
PORT=${ND_PORT:-8123}
ITERS=${ND_ITERS:-3}
SETTLE=${ND_SETTLE:-8000}
REPO=${ND_SPEEDOMETER_REPO:-https://github.com/WebKit/Speedometer.git}
FILTER=${1:-}

if [ ! -x "$BIN" ]; then
    echo "error: browser binary not found at $BIN — run scripts/dev.sh build" >&2
    exit 1
fi

if [ ! -d "$WORK/resources" ]; then
    echo "Fetching Speedometer release/3.1 into $WORK ..."
    rm -rf "$WORK"
    git clone --depth 1 --branch release/3.1 "$REPO" "$WORK"
fi

echo "Generating page driver from Speedometer sources ..."
python3 - "$WORK" <<'PY'
import re, sys
work = sys.argv[1]
tr = re.sub(r'^\s*export\s+', '', open(f'{work}/resources/translations.mjs', encoding='utf-8').read(), flags=re.M)
tests = open(f'{work}/resources/tests.mjs', encoding='utf-8').read()
tests = re.sub(r'^\s*import .*?;\s*$', '', tests, flags=re.M)
tests = re.sub(r'^\s*export\s+', '', tests, flags=re.M)

harness = r'''
"use strict";
var params = { warmupBeforeSync: 0 };
function NDLOG(s){ try { console.log(s); } catch(e){} }
class BenchmarkTestStep { constructor(name, run){ this.name = name; this.run = run; } }
function getParent(start, path){
    let root = start;
    for (const sel of path){ const n = root.querySelector(sel); root = (n && n.shadowRoot) ? n.shadowRoot : n; }
    return root;
}
const NATIVE_OPTIONS = { bubbles: true, cancellable: true };
class PageElement {
    constructor(node){ this._node = node; }
    setValue(v){ this._node.value = v; }
    click(){ this._node.click(); }
    focus(){ this._node.focus(); }
    getElementByMethod(name){ return new PageElement(this._node[name]()); }
    dispatchEvent(name, options = NATIVE_OPTIONS, type = Event){
        if (name === "submit") this._dispatchSubmitEvent();
        else this._node.dispatchEvent(new type(name, options));
    }
    _dispatchSubmitEvent(){ const e = document.createEvent("Event"); e.initEvent("submit", true, true); this._node.dispatchEvent(e); }
    enter(type, options){ return this.dispatchKeyEvent(type, 13, "Enter", options); }
    dispatchKeyEvent(type, keyCode, key, options){
        let o = { bubbles: true, cancelable: true, keyCode, which: keyCode, key };
        if (options !== undefined) o = Object.assign(o, options);
        this._node.dispatchEvent(new KeyboardEvent(type, o));
    }
    dispatchMouseEvent(type, ox, oy, options){
        const r = this._node.getBoundingClientRect(), cw = this._node.ownerDocument.defaultView;
        const cx = ox + r.x, cy = oy + r.y;
        let o = { bubbles: true, cancelable: true, clientX: cx, clientY: cy, screenX: cx + cw.screenX, screenY: cy + cw.screenY };
        if (options !== undefined) o = Object.assign(o, options);
        this._node.dispatchEvent(new cw.MouseEvent(type, o));
    }
    querySelectorInShadowRoot(sel, path = []){
        const start = this._node.shadowRoot ? this._node.shadowRoot : this._node;
        const el = getParent(start, path).querySelector(sel);
        return el === null ? null : new PageElement(el);
    }
    querySelector(sel){ const el = this._node.querySelector(sel); return el === null ? null : new PageElement(el); }
    querySelectorAll(sel){ return Array.from(this._node.querySelectorAll(sel)).map((e) => new PageElement(e)); }
}
class Page {
    layoutFlush(){ return (document.body || document.documentElement).getBoundingClientRect().height; }
    waitForElement(sel, path = []){
        return new Promise((resolve) => {
            let tries = 0;
            const poll = () => {
                const el = getParent(document, path).querySelector(sel);
                if (el) { resolve(new PageElement(el)); return; }
                if (++tries > 1500) { resolve(null); return; }
                requestAnimationFrame(poll);
            };
            poll();
        });
    }
    querySelector(sel, path = []){ const el = getParent(document, path).querySelector(sel); return el === null ? null : new PageElement(el); }
    querySelectorAll(sel, path = []){ return Array.from(getParent(document, path).querySelectorAll(sel)).map((e) => new PageElement(e)); }
    getElementById(id){ const el = document.getElementById(id); return el === null ? null : new PageElement(el); }
}
'''

bootstrap = r'''
const page = new Page();
function runStep(test){
    return new Promise((resolve) => {
        let syncTime, asyncStart, asyncTime;
        const runSync = () => { const t0 = performance.now(); test.run(page); syncTime = performance.now() - t0; asyncStart = performance.now(); };
        const measureAsync = () => { const h = page.layoutFlush(); asyncTime = performance.now() - asyncStart; window._unusedHeightValue = h; };
        requestAnimationFrame(() => runSync());
        requestAnimationFrame(() => { setTimeout(() => { measureAsync(); setTimeout(() => resolve({ name: test.name, sync: syncTime, async: asyncTime, total: syncTime + asyncTime }), 0); }, 0); });
    });
}
async function ndMain(){
    const here = location.pathname;
    const norm = (u) => u.split('#')[0].split('?')[0];
    const suite = Suites.find((s) => here.endsWith(norm(s.url)));
    if (!suite){ NDLOG("ND_BENCH ERROR no-suite-match " + here); return; }
    NDLOG("ND_BENCH SUITE " + suite.name);
    try { await suite.prepare(page); }
    catch (e){ NDLOG("ND_BENCH ERROR prepare " + (e && e.message ? e.message : e)); return; }
    let suiteTotal = 0; const parts = [];
    for (const test of suite.tests){
        let r;
        try { r = await runStep(test); }
        catch (e){ NDLOG("ND_BENCH ERROR step " + test.name + " " + (e && e.message ? e.message : e)); return; }
        suiteTotal += r.total;
        parts.push(test.name + "=" + r.total.toFixed(2) + "(s" + r.sync.toFixed(2) + "/a" + r.async.toFixed(2) + ")");
    }
    NDLOG("ND_BENCH DONE " + suite.name + " | total=" + suiteTotal.toFixed(2) + " | " + parts.join(" ") + " | domNodes=" + document.getElementsByTagName('*').length);
}
if (document.readyState === "complete" || document.readyState === "interactive") requestAnimationFrame(() => ndMain());
else window.addEventListener("DOMContentLoaded", () => requestAnimationFrame(() => ndMain()));
'''

out = "(function(){\n" + harness + "\n/* translations */\n" + tr + "\n/* suites (from Speedometer tests.mjs) */\n" + tests + "\n/* bootstrap */\n" + bootstrap + "\n})();\n"
open(f'{work}/nd-driver.js', 'w', encoding='utf-8').write(out)

import glob
tag = '<script src="/nd-driver.js"></script>'
n = 0
for p in glob.glob(f'{work}/resources/todomvc/**/dist/index.html', recursive=True):
    h = open(p, encoding='utf-8').read()
    if 'nd-driver.js' in h: continue
    h = h.replace('</body>', tag + '</body>', 1) if '</body>' in h else h + tag
    open(p, 'w', encoding='utf-8').write(h)
    n += 1
print(f"  driver injected into {n} suite pages")
PY

cd "$WORK"
python3 -m http.server "$PORT" >/dev/null 2>&1 &
HTTPD=$!
trap 'kill "$HTTPD" 2>/dev/null || true' EXIT
sleep 1

mapfile -t PATHS < <(cd "$WORK" && ls resources/todomvc/*/*/dist/index.html | sed 's#resources/todomvc/##')

printf "%-44s %10s %10s %10s %10s %8s\n" "SUITE" "total(ms)" "add" "complete" "delete" "dom"
for p in "${PATHS[@]}"; do
    [ -n "$FILTER" ] && case "$p" in *"$FILTER"*) ;; *) continue ;; esac
    url="http://localhost:$PORT/resources/todomvc/$p"
    case "$p" in *react*|*preact*) url="$url#/home" ;; esac
    tots=(); add=""; comp=""; del=""; dom=""; name=""
    for _ in $(seq 1 "$ITERS"); do
        line=$(ND_ALLOW_ROOT=1 "$BIN" --headless --dump=none --settle-ms="$SETTLE" "$url" 2>&1 \
               | grep -oE "ND_BENCH DONE.*" | head -1 || true)
        [ -z "$line" ] && continue
        name=$(echo "$line" | sed -E 's/ND_BENCH DONE ([^ ]+) .*/\1/')
        tots+=("$(echo "$line" | grep -oE 'total=[0-9.]+' | head -1 | cut -d= -f2)")
        add=$(echo "$line"  | grep -oE 'Adding[0-9]+Items=[0-9.]+' | grep -oE '[0-9.]+$')
        comp=$(echo "$line" | grep -oE 'CompletingAllItems=[0-9.]+' | grep -oE '[0-9.]+$')
        del=$(echo "$line"  | grep -oE 'DeletingAllItems=[0-9.]+'   | grep -oE '[0-9.]+$')
        dom=$(echo "$line"  | grep -oE 'domNodes=[0-9]+' | cut -d= -f2)
    done
    if [ "${#tots[@]}" -gt 0 ]; then
        med=$(printf "%s\n" "${tots[@]}" | sort -n | awk '{a[NR]=$1} END{print (NR%2)?a[(NR+1)/2]:(a[NR/2]+a[NR/2+1])/2}')
        printf "%-44s %10.1f %10.1f %10.1f %10.1f %8s\n" "${name}" "$med" "${add:-0}" "${comp:-0}" "${del:-0}" "${dom:-?}"
    else
        short=$(echo "$p" | sed -E 's#/dist/index.html##; s#.*/##')
        printf "%-44s %10s   (does not load — see docs/Benchmarking.md)\n" "$short" "n/a"
    fi
done
