#!/usr/bin/env bash
# Build a redistributable Nordstjernen Linux x86_64 release: the stripped,
# LTO-optimised nordstjernen binary plus the data files it needs at runtime,
# zipped under dist/.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
BUILDDIR=${BUILDDIR:-$ROOT/release-build}
VERSION=$(grep -E "^[[:space:]]*version" "$ROOT/meson.build" | head -1 \
          | sed -E "s/.*version: '([^']+)'.*/\1/")
ARCH=$(uname -m)
SLUG="nordstjernen-${VERSION}-linux-${ARCH}"
STAGE="$ROOT/dist/${SLUG}"
ZIP="$ROOT/dist/${SLUG}.zip"

if [ ! -d "$BUILDDIR" ]; then
    meson setup "$BUILDDIR" --buildtype=release -Db_lto=true -Db_ndebug=true \
        --strip
fi
meson compile -C "$BUILDDIR"
strip --strip-all "$BUILDDIR/src/nordstjernen"

rm -rf "$STAGE"
mkdir -p "$STAGE/data/icons/hicolor/scalable/apps"
cp "$BUILDDIR/src/nordstjernen" "$STAGE/"
cp -r "$ROOT/compatibility-css" "$STAGE/"
cp "$ROOT/data/icons/hicolor/scalable/apps/nordstjernen.svg" \
   "$STAGE/data/icons/hicolor/scalable/apps/"
cp "$ROOT/README.md" "$STAGE/"

cat > "$STAGE/INSTALL.md" <<EOF
# Nordstjernen ${VERSION} — Linux ${ARCH} binary

Stripped, LTO-optimised build. The browser engine itself (lexbor,
gumbo, quickjs, ada-url, uchardet wrapper) is statically linked
into the binary. The GTK 4 desktop stack stays dynamic because it
expects to find pixbuf loaders, IM modules and font/theme data on
the host at runtime — fully-static GTK isn't practical.

## Runtime requirements

- glibc 2.31+ (Ubuntu 20.04 / Fedora 34 / Debian 11 era and later)
- GTK 4.6+, with gio, gobject, pango, cairo, gdk-pixbuf
- libcurl with a TLS backend
- libuchardet
- fontconfig + a font set; harfbuzz; freetype; libstdc++
- An X11 or Wayland session

Distro install commands:

    sudo apt   install libgtk-4-1 libcurl4 libuchardet0           # Debian/Ubuntu
    sudo dnf   install gtk4 libcurl libuchardet                   # Fedora/RHEL
    sudo zypper install libgtk-4-1 libcurl4 libuchardet0          # openSUSE

For Linux distros without modern GTK 4, build an AppImage instead
(future work).

## Run

    ./nordstjernen https://example.com

## Install on user path

    install -Dm755 nordstjernen ~/.local/bin/nordstjernen
    install -Dm644 data/icons/hicolor/scalable/apps/nordstjernen.svg \\
        ~/.local/share/icons/hicolor/scalable/apps/nordstjernen.svg

The compatibility-css/ directory is found at runtime in (first match wins):
  ./compatibility-css/  (next to the binary)
  ./data/compatibility-css/
  ../share/nordstjernen/compatibility-css/
  /usr/local/share/nordstjernen/compatibility-css/
  /usr/share/nordstjernen/compatibility-css/
  \$XDG_DATA_HOME/nordstjernen/compatibility-css/  (per-user override)

## License

Source-available; redistribution / commercial use require a license.
Copyright 2026 Andreas Røsdal. See README.md for details.
EOF

rm -f "$ZIP"
( cd "$ROOT/dist" && zip -r "$(basename "$ZIP")" "$(basename "$STAGE")" >/dev/null )

echo "Built: $ZIP ($(du -h "$ZIP" | cut -f1))"
echo "Binary size: $(du -h "$STAGE/nordstjernen" | cut -f1)"
echo
echo "Smoke test: ./dist/${SLUG}/nordstjernen --headless --url=https://example.com --dump=text"
