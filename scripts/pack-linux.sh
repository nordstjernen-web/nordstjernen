#!/usr/bin/env bash
# Build a redistributable Nordstjernen Linux x86_64 release: the stripped,
# LTO-optimised nordstjernen binary plus the data files it needs at runtime,
# zipped under dist/.
set -euo pipefail

log() { printf '[pack-linux] %s\n' "$*" >&2; }
trap 'rc=$?; printf "[pack-linux] FAILED (exit %s) at line %s: %s\n" \
    "$rc" "$LINENO" "$BASH_COMMAND" >&2; exit $rc' ERR
[ -n "${ND_DEBUG:-}" ] && set -x

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILDDIR=${BUILDDIR:-$ROOT/release-build}
VERSION=${VERSION:-$(awk -F"'" \
    '/^[[:space:]]*version[[:space:]]*:/ { print $2; exit }' "$ROOT/meson.build")}
ARCH=$(uname -m)
FSVERSION=${VERSION//\~/-}
FSVERSION=${FSVERSION//\//-}
SLUG="nordstjernen-${FSVERSION}-linux-${ARCH}"
STAGE="$ROOT/dist/${SLUG}"
ZIP="$ROOT/dist/${SLUG}.zip"

if [ ! -d "$BUILDDIR" ]; then
    meson setup "$BUILDDIR" --buildtype=release -Db_lto="${ND_BUILD_LTO:-true}" \
        -Db_ndebug=true --strip \
        ${ND_BUILD_DATE:+-Dbuild_date="$ND_BUILD_DATE"}
fi
meson compile -C "$BUILDDIR" ${ND_BUILD_JOBS:+-j "$ND_BUILD_JOBS"}
strip --strip-all "$BUILDDIR/src/nordstjernen"

LOADER=$(ldd "$BUILDDIR/src/nordstjernen" 2>/dev/null \
    | grep -m1 -oE 'ld-(musl|linux)[^ ]*' || true)
if printf '%s' "$LOADER" | grep -q 'ld-musl'; then
    LIBC=musl
else
    LIBC=glibc
fi
[ -n "$LOADER" ] || LOADER="ld-$LIBC"

if [ "$LIBC" = musl ]; then
    LIBC_REQ='- musl libc (Alpine 3.19+ era and later)'
    RUNTIME_INSTALL='    sudo apk add gtk4.0 libcurl uchardet librsvg ca-certificates \
        font-dejavu                                                    # Alpine (musl)'
else
    LIBC_REQ='- glibc 2.31+ (Ubuntu 20.04 / Fedora 34 / Debian 11 era and later)'
    RUNTIME_INSTALL='    sudo apt   install libgtk-4-1 libcurl4 libuchardet0 librsvg2-2     # Debian/Ubuntu
    sudo dnf   install gtk4 libcurl libuchardet librsvg2               # Fedora/RHEL
    sudo zypper install libgtk-4-1 libcurl4 libuchardet0 librsvg-2-2   # openSUSE'
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/data/icons/hicolor/scalable/apps"
cp "$BUILDDIR/src/nordstjernen" "$STAGE/"
cp "$ROOT"/data/icons/hicolor/scalable/apps/nordstjernen*.svg \
   "$ROOT"/data/icons/hicolor/scalable/apps/nordstjernen.gif \
   "$STAGE/data/icons/hicolor/scalable/apps/"
cp "$ROOT/data/nordstjernen.desktop" "$STAGE/data/"
cp "$ROOT/README.md" "$STAGE/"
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$STAGE/"
cp "$ROOT/License.md" "$STAGE/"

cat > "$STAGE/INSTALL.md" <<EOF
# Nordstjernen ${VERSION} — Linux ${ARCH} binary

Stripped, LTO-optimised build. The browser engine itself (lexbor,
gumbo, quickjs, uchardet wrapper) is statically linked
into the binary. The GTK 4 desktop stack stays dynamic because it
expects to find pixbuf loaders, IM modules and font/theme data on
the host at runtime — fully-static GTK isn't practical.

## Runtime requirements

This is a ${LIBC} build (linked against ${LOADER}). It will not run
against the other C library — pick the matching download.

${LIBC_REQ}
- GTK 4.6+, with gio, gobject, pango, cairo, gdk-pixbuf
- libcurl with a TLS backend
- libuchardet
- librsvg (SVG rendering)
- fontconfig + a font set; harfbuzz; freetype; libstdc++
- ca-certificates (TLS trust store)
- An X11 or Wayland session

Distro install commands:

${RUNTIME_INSTALL}

For Linux distros without modern GTK 4, build an AppImage instead
(future work).

## Run

    ./nordstjernen https://example.com

## Install on user path

    install -Dm755 nordstjernen ~/.local/bin/nordstjernen
    install -Dm644 data/icons/hicolor/scalable/apps/nordstjernen.svg \\
        ~/.local/share/icons/hicolor/scalable/apps/nordstjernen.svg

## License

Source-available; redistribution / commercial use require a license.
Copyright 2026 Andreas Røsdal. See README.md for details.
EOF

log "staged: $(cd "$STAGE" && find . -type f | sort | tr '\n' ' ')"
log "zip -> $ZIP"
rm -f "$ZIP"
( cd "$ROOT/dist" && zip -r "$(basename "$ZIP")" "$(basename "$STAGE")" >/dev/null )
[ -s "$ZIP" ] || { log "ERROR: $ZIP missing or empty after zip"; exit 1; }

zip_size=$(du -h "$ZIP" 2>/dev/null | cut -f1 || echo '?')
bin_size=$(du -h "$STAGE/nordstjernen" 2>/dev/null | cut -f1 || echo '?')
echo "Built: $ZIP ($zip_size)"
echo "Binary size: $bin_size"
echo
echo "Smoke test: ./dist/${SLUG}/nordstjernen --headless --url=https://example.com --dump=text"
