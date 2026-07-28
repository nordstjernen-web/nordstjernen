#!/usr/bin/env bash
# Stage a portable Nordstjernen BSD release zip: the nordstjernen binary, the
# sandboxed renderer, runtime data, and an INSTALL.md naming the pkg runtime
# dependencies, zipped under dist/. OS label via $1 or NS_PACK_OS.
set -euo pipefail

log() { printf '[pack-bsd] %s\n' "$*" >&2; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
OS=${1:-${NS_PACK_OS:-bsd}}
BUILDDIR=${BUILDDIR:-$ROOT/builddir}
VERSION=${VERSION:-$(awk -F"'" \
    '/^[[:space:]]*version[[:space:]]*:/ { print $2; exit }' "$ROOT/meson.build")}
FSVERSION=${VERSION//\~/-}
FSVERSION=${FSVERSION//\//-}
SLUG="nordstjernen-${FSVERSION}-${OS}-x86_64"
STAGE="$ROOT/dist/$SLUG"
ZIP="$ROOT/dist/${SLUG}.zip"

if [ ! -x "$BUILDDIR/src/gtk/nordstjernen" ]; then
    log "ERROR: $BUILDDIR/src/gtk/nordstjernen not found — build first"
    exit 1
fi

strip "$BUILDDIR/src/gtk/nordstjernen" 2>/dev/null || true
strip "$BUILDDIR/src/nordstjernen-renderer" 2>/dev/null || true

rm -rf "$STAGE"
mkdir -p "$STAGE/data/icons/hicolor/scalable/apps"
cp "$BUILDDIR/src/gtk/nordstjernen" "$STAGE/"
[ -f "$BUILDDIR/src/nordstjernen-renderer" ] && \
    cp "$BUILDDIR/src/nordstjernen-renderer" "$STAGE/"
cp "$ROOT"/data/icons/hicolor/scalable/apps/nordstjernen*.svg \
   "$STAGE/data/icons/hicolor/scalable/apps/" 2>/dev/null || true
cp "$ROOT"/data/icons/hicolor/scalable/apps/nordstjernen.gif \
   "$STAGE/data/icons/hicolor/scalable/apps/" 2>/dev/null || true
cp "$ROOT/data/nordstjernen.desktop" "$STAGE/data/" 2>/dev/null || true
cp "$ROOT/README.md" "$STAGE/" 2>/dev/null || true
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$STAGE/" 2>/dev/null || true
cp "$ROOT/License.md" "$STAGE/" 2>/dev/null || true

case "$OS" in
    freebsd)
        RUNTIME='    pkg install gtk4 libcurl uchardet webp sqlite3 libpsl libepoxy' ;;
    netbsd)
        RUNTIME='    pkgin install gtk4 curl uchardet sqlite3 libwebp libpsl libepoxy' ;;
    *)
        RUNTIME='    install gtk4 and the other runtime libraries with your package manager' ;;
esac

cat > "$STAGE/INSTALL.md" <<EOF
# Nordstjernen ${VERSION} — ${OS} x86_64

Portable build. The browser engine (lexbor for HTML, quickjs for
JavaScript, wuffs for image decoding, wamr for WebAssembly) is statically
linked into the binary; GTK 4 and the other shared runtime libraries come
from your system. The Linux syscall sandbox (seccomp) does not exist on
${OS}, so it is compiled out — process isolation relies on the OS.

## Runtime dependencies

${RUNTIME}

## Run

    ./nordstjernen https://example.com
EOF

( cd "$ROOT/dist" && rm -f "${SLUG}.zip" && zip -r "${SLUG}.zip" "$SLUG" >/dev/null )
log "wrote $ZIP"
printf '%s\n' "$ZIP"
