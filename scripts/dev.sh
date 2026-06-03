#!/usr/bin/env bash
# Nordstjernen dev helper: smoke-tests reading-list.txt through the headless engine.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${ND_BIN:-$ROOT/builddir/src/nordstjernen}
export ND_ALLOW_ROOT=${ND_ALLOW_ROOT:-1}
LIST=${ND_LIST:-$ROOT/reading-list.txt}
BASE=${ND_BASE:-$ROOT/data/baseline}

cd "$ROOT"

usage() {
    cat <<EOF
Usage: ./scripts/dev.sh <command> [args]

Commands:
  build              Run 'meson setup builddir' (only if needed) and
                     'meson compile -C builddir'. Picks up ccache
                     automatically when installed.
  smoke              For each target in $LIST, render headless and diff
                     against its baseline. Reports drift.
  baseline <target>  Render one target and (re)write its baseline.
  baselines          Refresh baselines from every target in $LIST.

Each target in the list is '[mode ]url', where mode is text (default),
layout, or dom. text/dom baselines are <slug>.txt/<slug>.dom.txt;
layout baselines are <slug>.layout.txt. Use layout targets for
geometry (text-free, fixed-size fixtures) so the diff is font-stable.

Env:
  ND_BIN   path to nordstjernen binary (default: $BIN)
  ND_LIST  reading-list path           (default: $LIST)
  ND_BASE  baseline dir                (default: $BASE)
EOF
}

slugify() {
    printf '%s' "$1" | tr -cs 'A-Za-z0-9._-' '_' \
        | sed 's/__*/_/g; s/^_//; s/_$//'
}

parse_target() {
    local spec="$1"
    T_MODE=text
    T_URL="$spec"
    case "$spec" in
        "text "*|"layout "*|"dom "*)
            T_MODE="${spec%% *}"
            T_URL="${spec#* }"
            ;;
    esac
}

baseline_path() {
    if [ "$1" = text ]; then printf '%s/%s.txt' "$BASE" "$2"
    else printf '%s/%s.%s.txt' "$BASE" "$2" "$1"; fi
}

render() {
    "$BIN" --headless --dump="$1" --settle-ms=300 "$2"
}

trim() {
    local s="${1%%#*}"
    s="${s%"${s##*[![:space:]]}"}"
    printf '%s' "${s#"${s%%[![:space:]]*}"}"
}

cmd_smoke() {
    local fail=0
    local tmp
    tmp=$(mktemp)
    trap 'rm -f -- "${tmp-}"' EXIT
    mkdir -p "$BASE"
    while IFS= read -r line; do
        spec=$(trim "$line")
        [ -z "$spec" ] && continue
        parse_target "$spec"
        slug=$(slugify "$T_URL")
        base=$(baseline_path "$T_MODE" "$slug")
        render "$T_MODE" "$T_URL" >"$tmp" 2>/dev/null || true
        if [ ! -f "$base" ]; then
            printf 'NEW   %s [%s] (no baseline; run: ./scripts/dev.sh baselines)\n' "$T_URL" "$T_MODE"
            fail=1
            continue
        fi
        if diff -q "$base" "$tmp" >/dev/null 2>&1; then
            printf 'OK    %s [%s]\n' "$T_URL" "$T_MODE"
        else
            printf 'DRIFT %s [%s]\n' "$T_URL" "$T_MODE"
            diff "$base" "$tmp" | head -8 | sed 's/^/      /'
            fail=1
        fi
    done < "$LIST"
    return $fail
}

cmd_baseline() {
    parse_target "$1"
    mkdir -p "$BASE"
    slug=$(slugify "$T_URL")
    local base
    base=$(baseline_path "$T_MODE" "$slug")
    render "$T_MODE" "$T_URL" > "$base"
    printf 'wrote %s\n' "$base"
}

cmd_baselines() {
    while IFS= read -r line; do
        spec=$(trim "$line")
        [ -z "$spec" ] && continue
        cmd_baseline "$spec"
    done < "$LIST"
}

cmd_build() {
    cd "$ROOT"
    if [ ! -f builddir/build.ninja ]; then
        meson setup builddir
    fi
    meson compile -C builddir
}

case "${1:-}" in
    build)     cmd_build ;;
    smoke)     cmd_smoke ;;
    baseline)  shift; cmd_baseline "${1:?url required}" ;;
    baselines) cmd_baselines ;;
    *)         usage ; exit 2 ;;
esac
