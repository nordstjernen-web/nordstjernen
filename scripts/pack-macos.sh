#!/usr/bin/env bash
# Build a macOS .app bundle + .dmg for Nordstjernen. Vendors the
# Homebrew GTK 4 dylibs with dylibbundler so the bundle is portable
# to Macs without Homebrew installed. Runs on macOS only.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(grep -E "^[[:space:]]*version" "$ROOT/meson.build" | head -1 \
          | sed -E "s/.*version: '([^']+)'.*/\1/")
ARCH=$(uname -m)
NAME=Nordstjernen
BUILDDIR=${BUILDDIR:-$ROOT/build-macos}
STAGE="$ROOT/dist/${NAME}.app"
DMG="$ROOT/dist/nordstjernen-${VERSION}-macos-${ARCH}.dmg"

mkdir -p "$ROOT/dist"

if [ "$(uname)" != "Darwin" ]; then
    echo "pack-macos.sh runs on macOS only." >&2
    exit 1
fi

if ! command -v dylibbundler >/dev/null 2>&1; then
    echo "dylibbundler not found. brew install dylibbundler" >&2
    exit 1
fi

if [ ! -d "$BUILDDIR" ]; then
    meson setup "$BUILDDIR" \
        --prefix=/usr/local \
        --buildtype=release \
        -Db_lto=true \
        -Db_ndebug=true \
        --strip
fi
meson compile -C "$BUILDDIR"

rm -rf "$STAGE"
mkdir -p "$STAGE/Contents/MacOS"
mkdir -p "$STAGE/Contents/Resources/share/nordstjernen"
mkdir -p "$STAGE/Contents/Frameworks"

install -m755 "$BUILDDIR/src/nordstjernen" \
    "$STAGE/Contents/MacOS/nordstjernen-bin"

cp "$ROOT/License.md" "$STAGE/Contents/Resources/share/nordstjernen/"
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$STAGE/Contents/Resources/share/nordstjernen/"
cp "$ROOT/README.md" "$STAGE/Contents/Resources/share/nordstjernen/"

cat > "$STAGE/Contents/MacOS/Nordstjernen" <<'LAUNCHER_EOF'
#!/bin/bash
DIR=$(cd "$(dirname "$0")" && pwd)
BUNDLE=$(cd "$DIR/../.." && pwd)
export DYLD_LIBRARY_PATH="$BUNDLE/Contents/Frameworks:${DYLD_LIBRARY_PATH:-}"
exec "$DIR/nordstjernen-bin" "$@"
LAUNCHER_EOF
chmod +x "$STAGE/Contents/MacOS/Nordstjernen"

ICONSET=$(mktemp -d)
trap 'rm -rf "$ICONSET"' EXIT
ICON_GIF="$ROOT/data/icons/hicolor/scalable/apps/nordstjernen.gif"
if command -v sips >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
    mkdir -p "$ICONSET/nordstjernen.iconset"
    for sz in 16 32 64 128 256 512; do
        sips -s format png -z "$sz" "$sz" "$ICON_GIF" \
            --out "$ICONSET/nordstjernen.iconset/icon_${sz}x${sz}.png" \
            >/dev/null 2>&1 || true
    done
    iconutil -c icns "$ICONSET/nordstjernen.iconset" \
        -o "$STAGE/Contents/Resources/nordstjernen.icns" >/dev/null 2>&1 || true
fi

cat > "$STAGE/Contents/Info.plist" <<PLIST_EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>
    <string>Nordstjernen</string>
    <key>CFBundleDisplayName</key>
    <string>Nordstjernen</string>
    <key>CFBundleIdentifier</key>
    <string>org.nordstjernen.Nordstjernen</string>
    <key>CFBundleVersion</key>
    <string>${VERSION}</string>
    <key>CFBundleShortVersionString</key>
    <string>${VERSION}</string>
    <key>CFBundleExecutable</key>
    <string>Nordstjernen</string>
    <key>CFBundlePackageType</key>
    <string>APPL</string>
    <key>CFBundleSignature</key>
    <string>NORD</string>
    <key>CFBundleIconFile</key>
    <string>nordstjernen.icns</string>
    <key>LSMinimumSystemVersion</key>
    <string>11.0</string>
    <key>NSHighResolutionCapable</key>
    <true/>
    <key>NSPrincipalClass</key>
    <string>NSApplication</string>
    <key>LSApplicationCategoryType</key>
    <string>public.app-category.utilities</string>
    <key>CFBundleURLTypes</key>
    <array>
        <dict>
            <key>CFBundleURLName</key>
            <string>HTTP URL</string>
            <key>CFBundleURLSchemes</key>
            <array>
                <string>http</string>
                <string>https</string>
            </array>
        </dict>
    </array>
</dict>
</plist>
PLIST_EOF

if ! dylibbundler -of -cd -b \
    -x "$STAGE/Contents/MacOS/nordstjernen-bin" \
    -d "$STAGE/Contents/Frameworks/" \
    -p "@executable_path/../Frameworks/"; then
    echo "pack-macos.sh: dylibbundler failed; listing binary dylibs and continuing" >&2
    otool -L "$STAGE/Contents/MacOS/nordstjernen-bin" || true
    exit 1
fi

rm -f "$DMG"
if ! hdiutil create -volname "Nordstjernen ${VERSION}" \
    -srcfolder "$STAGE" \
    -ov -format UDZO \
    "$DMG"; then
    echo "pack-macos.sh: hdiutil create failed" >&2
    ls -la "$STAGE" || true
    exit 1
fi

echo
echo "Built: $DMG ($(du -h "$DMG" | cut -f1))"
echo "Bundle: $STAGE ($(du -sh "$STAGE" | cut -f1))"
echo
echo "Test:  open '$STAGE'"
echo "       '$STAGE/Contents/MacOS/Nordstjernen' --headless --dump=text about:start"
