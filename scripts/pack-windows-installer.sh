#!/usr/bin/env bash
# Build the Windows .exe installer for Nordstjernen.
#
# Runs pack-windows.sh first to produce the redistributable bundle, then
# wraps it with NSIS (Modern UI 2) into a single per-user installer.
# Per-user means $LOCALAPPDATA\Programs\Nordstjernen — no Administrator
# rights are required, which matches the binary's refuse-to-run-elevated
# policy (src/security.c::nd_security_refuse_root).
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$SCRIPT_DIR/.." && pwd)
"$SCRIPT_DIR/pack-windows.sh"

VERSION=$(grep -E "^[[:space:]]*version:" "$ROOT/meson.build" \
          | head -1 | sed -E "s/.*'([^']+)'.*/\\1/")
BUNDLE=$ROOT/dist/nordstjernen-win64
INSTALLER=$ROOT/dist/nordstjernen-${VERSION}-win64-setup.exe
NSI=$ROOT/data/installer/nordstjernen.nsi

if [ ! -d "$BUNDLE" ]; then
    echo "pack-windows-installer: bundle dir missing: $BUNDLE" >&2
    exit 1
fi
if [ ! -f "$NSI" ]; then
    echo "pack-windows-installer: nsi script missing: $NSI" >&2
    exit 1
fi

resolve_makensis() {
    for cand in "${MAKENSIS:-}" \
                /c/msys64/mingw64/bin/makensis.exe \
                /mingw64/bin/makensis.exe \
                "/c/Program Files (x86)/NSIS/makensis.exe" \
                "/c/Program Files/NSIS/makensis.exe"; do
        [ -n "$cand" ] || continue
        [ -f "$cand" ] || continue
        echo "$cand"; return
    done
    return 1
}

MAKENSIS=$(resolve_makensis) || {
    echo "pack-windows-installer: makensis not found." >&2
    echo "Install with: pacman -S mingw-w64-x86_64-nsis" >&2
    exit 1
}

winpath() {
    if command -v cygpath >/dev/null 2>&1; then
        cygpath -w "$1"
    else
        printf '%s' "$1"
    fi
}

mkdir -p "$ROOT/dist"
"$MAKENSIS" -V2 \
    "-DVERSION=$VERSION" \
    "-DSRCDIR=$(winpath "$BUNDLE")" \
    "-DOUTFILE=$(winpath "$INSTALLER")" \
    "$(winpath "$NSI")"

if [ ! -f "$INSTALLER" ]; then
    echo "pack-windows-installer: makensis ran but installer not produced" >&2
    exit 1
fi

size=$(du -h "$INSTALLER" | awk '{print $1}')
printf 'pack-windows-installer: built %s (%s)\n' "$INSTALLER" "$size"
