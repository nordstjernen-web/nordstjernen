#!/usr/bin/env bash
# Nordstjernen dev helper: smoke-tests reading-list.txt through the headless engine.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${ND_BIN:-$ROOT/builddir/src/nordstjernen}
export ND_ALLOW_ROOT=${ND_ALLOW_ROOT:-1}
LIST=${ND_LIST:-$ROOT/reading-list.txt}
BASE=${ND_BASE:-$ROOT/data/baseline}

usage() {
    cat <<EOF
Usage: ./scripts/dev.sh <command> [args]

Commands:
  build              Run 'meson setup builddir' (only if needed) and
                     'meson compile -C builddir'. Picks up ccache
                     automatically when installed.
  smoke              For each URL in $LIST, render via '--headless --dump=text'
                     and diff against $BASE/<slug>.txt. Reports drift.
  baseline <url>     Render <url> and write $BASE/<slug>.txt.
  baselines          Refresh $BASE/ from every URL in $LIST.

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

render() {
    "$BIN" --headless --dump=text --settle-ms=300 "$1"
}

cmd_smoke() {
    local fail=0
    local tmp
    tmp=$(mktemp)
    trap 'rm -f -- "${tmp-}"' EXIT
    mkdir -p "$BASE"
    while IFS= read -r line; do
        url="${line%%#*}"; url="${url%"${url##*[![:space:]]}"}"; url="${url#"${url%%[![:space:]]*}"}"
        [ -z "$url" ] && continue
        slug=$(slugify "$url")
        base="$BASE/$slug.txt"
        render "$url" >"$tmp" 2>/dev/null || true
        if [ ! -f "$base" ]; then
            printf 'NEW   %s (no baseline; run: ./scripts/dev.sh baseline %s)\n' "$url" "$url"
            fail=1
            continue
        fi
        if diff -q "$base" "$tmp" >/dev/null 2>&1; then
            printf 'OK    %s\n' "$url"
        else
            printf 'DRIFT %s\n' "$url"
            diff "$base" "$tmp" | head -8 | sed 's/^/      /'
            fail=1
        fi
    done < "$LIST"
    return $fail
}

cmd_baseline() {
    local url="$1"
    mkdir -p "$BASE"
    slug=$(slugify "$url")
    render "$url" > "$BASE/$slug.txt"
    printf 'wrote %s/%s.txt\n' "$BASE" "$slug"
}

cmd_baselines() {
    while IFS= read -r line; do
        url="${line%%#*}"; url="${url%"${url##*[![:space:]]}"}"; url="${url#"${url%%[![:space:]]*}"}"
        [ -z "$url" ] && continue
        cmd_baseline "$url"
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
