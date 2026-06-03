#!/usr/bin/env bash
# Build a Nordstjernen AppImage. Uses linuxdeploy + its GTK plugin to
# bundle the GTK 4 stack (libs, typelibs, pixbuf loaders, GIO modules)
# so the AppImage runs on distros without modern GTK preinstalled.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(grep -E "^[[:space:]]*version" "$ROOT/meson.build" | head -1 \
          | sed -E "s/.*version: '([^']+)'.*/\1/")
ARCH=$(uname -m)
NAME=nordstjernen
BUILDDIR="$ROOT/build-appimage"
APPDIR="$ROOT/dist/AppDir"
TOOLS="$ROOT/dist/appimage-tools"

mkdir -p "$ROOT/dist" "$TOOLS"

LINUXDEPLOY="$TOOLS/linuxdeploy-${ARCH}.AppImage"
GTKPLUGIN="$TOOLS/linuxdeploy-plugin-gtk.sh"
APPIMAGETOOL="$TOOLS/appimagetool-${ARCH}.AppImage"
RUNTIME="$TOOLS/runtime-${ARCH}"

if [ ! -x "$LINUXDEPLOY" ]; then
    echo "Fetching linuxdeploy..."
    curl -L --fail -o "$LINUXDEPLOY" \
        "https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage"
    chmod +x "$LINUXDEPLOY"
fi

if [ ! -x "$GTKPLUGIN" ]; then
    echo "Fetching linuxdeploy-plugin-gtk..."
    curl -L --fail -o "$GTKPLUGIN" \
        "https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh"
    chmod +x "$GTKPLUGIN"
fi

if [ ! -x "$APPIMAGETOOL" ]; then
    echo "Fetching appimagetool..."
    curl -L --fail -o "$APPIMAGETOOL" \
        "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-${ARCH}.AppImage"
    chmod +x "$APPIMAGETOOL"
fi

if [ ! -s "$RUNTIME" ]; then
    echo "Fetching type2-runtime..."
    curl -L --fail -o "$RUNTIME" \
        "https://github.com/AppImage/type2-runtime/releases/download/continuous/runtime-${ARCH}"
fi

if [ ! -d "$BUILDDIR" ]; then
    meson setup "$BUILDDIR" \
        --prefix=/usr \
        --buildtype=release \
        -Db_lto=true \
        -Db_ndebug=true \
        --strip
fi
meson compile -C "$BUILDDIR"

rm -rf "$APPDIR"
DESTDIR="$APPDIR" meson install -C "$BUILDDIR"

rm -f "$APPDIR/usr/bin/qjs" "$APPDIR/usr/bin/qjsc"
rm -rf "$APPDIR/usr/include" "$APPDIR/usr/lib" "$APPDIR/usr/lib64" \
       "$APPDIR/usr/lib32"

install -dm755 "$APPDIR/usr/share/applications"
install -m644 "$ROOT/data/nordstjernen.desktop" \
    "$APPDIR/usr/share/applications/nordstjernen.desktop"

install -dm755 "$APPDIR/usr/share/icons/hicolor/256x256/apps"
ICON_SVG="$ROOT/data/icons/hicolor/scalable/apps/nordstjernen.svg"
ICON_GIF="$ROOT/data/icons/hicolor/scalable/apps/nordstjernen.gif"
PNG_ICON="$APPDIR/usr/share/icons/hicolor/256x256/apps/nordstjernen.png"
if command -v rsvg-convert >/dev/null 2>&1; then
    rsvg-convert -w 256 -h 256 -o "$PNG_ICON" "$ICON_SVG"
elif command -v magick >/dev/null 2>&1; then
    magick "$ICON_GIF[0]" -resize 256x256 "$PNG_ICON"
elif command -v convert >/dev/null 2>&1; then
    convert "$ICON_GIF[0]" -resize 256x256 "$PNG_ICON"
else
    echo "Need rsvg-convert or ImageMagick to rasterise the icon." >&2
    exit 1
fi

cd "$ROOT/dist"
rm -f "${NAME}-${VERSION}-${ARCH}.AppImage" "Nordstjernen"*.AppImage
DEPLOY_GTK_VERSION=4 \
"$LINUXDEPLOY" --appimage-extract-and-run \
    --appdir "$APPDIR" \
    --executable "$APPDIR/usr/bin/nordstjernen" \
    --desktop-file "$APPDIR/usr/share/applications/nordstjernen.desktop" \
    --icon-file "$PNG_ICON" \
    --plugin gtk

FINAL="$ROOT/dist/${NAME}-${VERSION}-${ARCH}.AppImage"
ARCH="${ARCH}" "$APPIMAGETOOL" --appimage-extract-and-run \
    --runtime-file "$RUNTIME" \
    --no-appstream \
    "$APPDIR" "$FINAL"

chmod +x "$FINAL"

echo
echo "Built: $FINAL ($(du -h "$FINAL" | cut -f1))"
echo
echo "Run:    $FINAL https://example.com"
echo "Smoke:  $FINAL --headless --url=https://example.com --dump=text"
