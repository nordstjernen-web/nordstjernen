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

run_dylibbundler() {
    local secs=$1; shift
    ( "$@" </dev/null ) &
    local pid=$! i=0
    while kill -0 "$pid" 2>/dev/null; do
        sleep 1
        i=$((i + 1))
        if [ "$i" -ge "$secs" ]; then
            echo "pack-macos.sh: dylibbundler exceeded ${secs}s, killing" >&2
            kill -9 "$pid" 2>/dev/null || true
            wait "$pid" 2>/dev/null || true
            return 124
        fi
    done
    wait "$pid"
}

# Homebrew's curl is keg-only, so a plain pkg-config resolves the macOS SDK's
# system libcurl (LibreSSL/SecureTransport) rather than the OpenSSL build this
# bundle is designed around — the vendored CA bundle (CURL_CA_BUNDLE) and the
# modern TLS curve list both assume OpenSSL. Put the curl keg on PKG_CONFIG_PATH
# so meson links it.
CURL_PC="$(brew --prefix curl 2>/dev/null)/lib/pkgconfig"
[ -d "$CURL_PC" ] && export PKG_CONFIG_PATH="$CURL_PC:${PKG_CONFIG_PATH:-}"

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

# The bundle executable is the real Mach-O, not a wrapper script: dylibbundler
# rewrites every dependency to @executable_path/../Frameworks, so no
# DYLD_LIBRARY_PATH shim is needed, and a Mach-O entry point is what lets the
# whole .app be code-signed (a shell script cannot carry a signature).
install -m755 "$BUILDDIR/src/gtk/nordstjernen" \
    "$STAGE/Contents/MacOS/Nordstjernen"
# The GUI spawns one sandboxed renderer process per tab; ship it alongside.
install -m755 "$BUILDDIR/src/nordstjernen-renderer" \
    "$STAGE/Contents/MacOS/nordstjernen-renderer"
# The audio playback helper (MP2/MP3 + optional Opus/Vorbis decode, SDL2
# output). Built only when SDL2 was present at configure time; ship it beside
# the main binary so the shell can spawn it for <video>/<audio> sound.
AUDIO_BIN="$BUILDDIR/src/nordstjernen-audio"
if [ -f "$AUDIO_BIN" ]; then
    install -m755 "$AUDIO_BIN" "$STAGE/Contents/MacOS/nordstjernen-audio"
else
    echo "pack-macos.sh: warning: $AUDIO_BIN missing; <video>/<audio> sound will not play" >&2
fi
# The isolated MSE video-decode helper (libav demux/decode → BGRA frames in a
# shared-memory ring the shell composites over the page). Built only when libav
# (FFmpeg) was present at configure time; ship it beside the main binary so the
# shell spawns it instead of decoding MSE video in-process. Without it here,
# ns_proc_video_helper_available() returns false and NS_VIDEO_HELPER is never
# set, so a released .app silently loses the out-of-process video path.
VIDEO_BIN="$BUILDDIR/src/nordstjernen-video"
if [ -f "$VIDEO_BIN" ]; then
    install -m755 "$VIDEO_BIN" "$STAGE/Contents/MacOS/nordstjernen-video"
else
    echo "pack-macos.sh: warning: $VIDEO_BIN missing; MSE video decodes in-process" >&2
fi

cp "$ROOT/License.md" "$STAGE/Contents/Resources/share/nordstjernen/"
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$STAGE/Contents/Resources/share/nordstjernen/"
cp "$ROOT/README.md" "$STAGE/Contents/Resources/share/nordstjernen/"

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
if [ ! -f "$STAGE/Contents/Resources/nordstjernen.icns" ]; then
    if command -v sips >/dev/null 2>&1 && command -v iconutil >/dev/null 2>&1; then
        echo "pack-macos.sh: ERROR: icon generation failed" >&2
        exit 1
    fi
    echo "pack-macos.sh: warning: sips/iconutil unavailable; app uses a generic icon" >&2
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

# Bundle the main binary, the renderer, and the audio/video helpers together:
# each links the same image-codec / SDL / libav dylibs via the shared engine, so
# they all need their references rewritten to the bundled Frameworks, or they
# fail to start with "Library not loaded: …/lib….dylib".
audio_bundle_args=()
if [ -f "$STAGE/Contents/MacOS/nordstjernen-audio" ]; then
    audio_bundle_args=(-x "$STAGE/Contents/MacOS/nordstjernen-audio")
fi
video_bundle_args=()
if [ -f "$STAGE/Contents/MacOS/nordstjernen-video" ]; then
    video_bundle_args=(-x "$STAGE/Contents/MacOS/nordstjernen-video")
fi
if ! run_dylibbundler 300 dylibbundler -of -cd -b \
    -x "$STAGE/Contents/MacOS/Nordstjernen" \
    -x "$STAGE/Contents/MacOS/nordstjernen-renderer" \
    "${audio_bundle_args[@]+"${audio_bundle_args[@]}"}" \
    "${video_bundle_args[@]+"${video_bundle_args[@]}"}" \
    -d "$STAGE/Contents/Frameworks/" \
    -p "@executable_path/../Frameworks/"; then
    echo "pack-macos.sh: dylibbundler failed; listing binary dylibs and continuing" >&2
    otool -L "$STAGE/Contents/MacOS/Nordstjernen" || true
    otool -L "$STAGE/Contents/MacOS/nordstjernen-renderer" || true
    exit 1
fi

# The build binaries carry one LC_RPATH per Homebrew dependency directory.
# dylibbundler adds a "@executable_path/../Frameworks/" rpath for each of them,
# leaving many identical LC_RPATH entries — which modern macOS dyld rejects as
# a fatal "duplicate LC_RPATH" error, so the bundled app refuses to launch.
# Collapse them to a single entry on each Mach-O executable.
dedup_frameworks_rpath() {
    local bin="$1" rp="@executable_path/../Frameworks/"
    [ -f "$bin" ] || return 0
    while install_name_tool -delete_rpath "$rp" "$bin" 2>/dev/null; do :; done
    install_name_tool -add_rpath "$rp" "$bin"
}
for exe in Nordstjernen nordstjernen-renderer nordstjernen-audio nordstjernen-video; do
    dedup_frameworks_rpath "$STAGE/Contents/MacOS/$exe"
done

RES="$STAGE/Contents/Resources"
FW="$STAGE/Contents/Frameworks"

# Engine runtime data the binary looks up relative to itself (the i18n.c and
# safebrowsing.c "../Resources/share/nordstjernen/..." search arm): the UI
# translation catalogues and the safe-browsing blocklist. Without these the
# .app shows an English-only UI and silently disables safe-browsing.
mkdir -p "$RES/share/nordstjernen/i18n"
cp "$ROOT"/data/i18n/*.lang "$RES/share/nordstjernen/i18n/" 2>/dev/null || true
cp "$ROOT/data/safebrowsing.list" "$RES/share/nordstjernen/" 2>/dev/null || true

# Homebrew's "sdl2" is the sdl2-compat shim: the audio helper links
# libSDL2-2.0.0.dylib, but that shim dlopen's the real SDL3 at runtime
# (it probes @loader_path/libSDL3.dylib first). dylibbundler cannot follow a
# dlopen, so it bundles the shim but not SDL3 — and playback then dies with a
# "Failed loading SDL3 library" popup. Ship SDL3 next to the shim.
if [ -f "$FW/libSDL2-2.0.0.dylib" ]; then
    sdl3_prefix=$(brew --prefix sdl3 2>/dev/null || true)
    sdl3_lib=""
    for cand in "$sdl3_prefix/lib/libSDL3.0.dylib" "$sdl3_prefix/lib/libSDL3.dylib"; do
        [ -f "$cand" ] && { sdl3_lib="$cand"; break; }
    done
    [ -z "$sdl3_lib" ] && sdl3_lib=$(find "$(brew --prefix 2>/dev/null)" \
        -name 'libSDL3.0.dylib' -print -quit 2>/dev/null || true)
    if [ -n "$sdl3_lib" ] && [ -f "$sdl3_lib" ]; then
        cp "$sdl3_lib" "$FW/libSDL3.dylib"
        run_dylibbundler 120 dylibbundler -of -cd -b -x "$FW/libSDL3.dylib" \
            -d "$FW/" -p "@executable_path/../Frameworks/" >/dev/null 2>&1 || true
        dedup_frameworks_rpath "$FW/libSDL3.dylib"
    else
        echo "pack-macos.sh: warning: sdl2-compat bundled but real SDL3 not found; audio will fail" >&2
    fi
fi

mkdir -p "$RES/share/icons"
cp -R "$ROOT/data/icons/hicolor" "$RES/share/icons/"
GTK_PREFIX=$(pkg-config --variable=prefix gtk4 2>/dev/null || true)
if [ -n "$GTK_PREFIX" ] && [ -f "$GTK_PREFIX/share/icons/hicolor/index.theme" ]; then
    cp "$GTK_PREFIX/share/icons/hicolor/index.theme" \
        "$RES/share/icons/hicolor/" 2>/dev/null || true
fi

# GTK 4 aborts at runtime if its org.gtk.gtk4.* schemas are missing, so
# collect schemas from every prefix that may carry them (glib's own
# schemasdir points into glib's Cellar and does NOT contain gtk4's
# schemas on Homebrew — the linked opt prefix does) and hard-fail if
# the compiled cache doesn't materialise.
SCHEMADIR=$(pkg-config --variable=schemasdir gio-2.0 2>/dev/null || true)
BREW_PREFIX=$(brew --prefix 2>/dev/null || true)
mkdir -p "$RES/share/glib-2.0/schemas"
for sd in "$SCHEMADIR" \
          "$GTK_PREFIX/share/glib-2.0/schemas" \
          "$BREW_PREFIX/share/glib-2.0/schemas"; do
    [ -n "$sd" ] && [ -d "$sd" ] || continue
    cp "$sd"/*.xml "$RES/share/glib-2.0/schemas/" 2>/dev/null || true
    cp "$sd"/gschema.dtd "$RES/share/glib-2.0/schemas/" 2>/dev/null || true
done
glib-compile-schemas "$RES/share/glib-2.0/schemas"
if [ ! -f "$RES/share/glib-2.0/schemas/gschemas.compiled" ]; then
    echo "pack-macos.sh: ERROR: gschemas.compiled was not produced" >&2
    exit 1
fi

PIXBUF_MODDIR=$(pkg-config --variable=gdk_pixbuf_moduledir gdk-pixbuf-2.0 2>/dev/null || true)
if [ -n "$PIXBUF_MODDIR" ] && [ -d "$PIXBUF_MODDIR" ]; then
    LOADERS="$RES/lib/gdk-pixbuf-2.0/2.10.0/loaders"
    mkdir -p "$LOADERS"
    cp "$PIXBUF_MODDIR"/*.so "$LOADERS/" 2>/dev/null || true
    for so in "$LOADERS"/*.so; do
        [ -e "$so" ] || continue
        run_dylibbundler 180 dylibbundler -of -cd -b -x "$so" -d "$FW/" \
            -p "@executable_path/../Frameworks/" >/dev/null 2>&1 || true
    done
    # Query against the ORIGINAL Homebrew loaders — the bundled copies
    # have been rewritten to @executable_path/../Frameworks refs that
    # only resolve inside the .app, so dlopen'ing them from here fails
    # and yields a header-only (useless) cache. Then strip the absolute
    # module dir prefix: the app sets GDK_PIXBUF_MODULEDIR at runtime
    # and relative entries are resolved against it.
    CACHE="$RES/lib/gdk-pixbuf-2.0/2.10.0/loaders.cache"
    GDK_PIXBUF_MODULEDIR="$PIXBUF_MODDIR" gdk-pixbuf-query-loaders \
        > "$CACHE"
    sed -i '' "s|\"${PIXBUF_MODDIR%/}/|\"|" "$CACHE"
    if ! grep -q '^"' "$CACHE"; then
        echo "pack-macos.sh: ERROR: loaders.cache has no loader entries" >&2
        exit 1
    fi
fi

# Homebrew's libcurl links OpenSSL, which has no built-in trust store and (on
# macOS) no Keychain bridge. ns_net_resolve_ca_bundle() only finds a CA bundle
# on a machine that has Homebrew or a Unix-style /etc/ssl/cert.pem — neither
# exists on a clean Mac — so vendor one and point the app at it (the macOS
# anchor sets CURL_CA_BUNDLE from Contents/Resources/etc/ssl/certs), or every
# HTTPS page fails certificate verification.
mkdir -p "$RES/etc/ssl/certs"
OPENSSL_PREFIX=$(brew --prefix openssl@3 2>/dev/null || true)
CA_DST="$RES/etc/ssl/certs/ca-bundle.crt"
for ca in \
    "$GTK_PREFIX/etc/ssl/certs/ca-bundle.crt" \
    "$BREW_PREFIX/etc/ca-certificates/cert.pem" \
    "$OPENSSL_PREFIX/etc/openssl@3/cert.pem" \
    "$BREW_PREFIX/etc/openssl@3/cert.pem" \
    "/etc/ssl/cert.pem"; do
    if [ -n "$ca" ] && [ -f "$ca" ]; then
        cp "$ca" "$CA_DST"
        break
    fi
done
if [ ! -f "$CA_DST" ]; then
    echo "pack-macos.sh: ERROR: no CA bundle found to vendor; HTTPS would fail on a clean Mac" >&2
    exit 1
fi

# Sign the bundle inside-out — nested code first, the .app last — so the seal
# stays valid. Without an Apple Developer ID the project signs ad-hoc ("-"),
# which seals the bundle so it launches once the user clears the download
# quarantine (see docs/macOS.md). Set MACOS_SIGN_IDENTITY to a
# "Developer ID Application: …" identity for a notarisation-ready bundle.
# This must be the final mutation before hdiutil: anything written into the
# bundle afterwards would invalidate the signature.
IDENTITY="${MACOS_SIGN_IDENTITY:--}"
if command -v codesign >/dev/null 2>&1; then
    sign_opts=(--force --sign "$IDENTITY")
    if [ "$IDENTITY" != "-" ]; then
        sign_opts+=(--options runtime --timestamp)
        ENTITLEMENTS="$ROOT/packaging/macos/entitlements.plist"
        [ -f "$ENTITLEMENTS" ] && sign_opts+=(--entitlements "$ENTITLEMENTS")
    fi
    sign_ok=1
    while IFS= read -r -d '' lib; do
        codesign "${sign_opts[@]}" "$lib" || sign_ok=0
    done < <(find "$STAGE/Contents" -type f \( -name '*.dylib' -o -name '*.so' \) -print0)
    for helper in nordstjernen-renderer nordstjernen-audio nordstjernen-video; do
        [ -f "$STAGE/Contents/MacOS/$helper" ] || continue
        codesign "${sign_opts[@]}" "$STAGE/Contents/MacOS/$helper" || sign_ok=0
    done
    codesign "${sign_opts[@]}" "$STAGE" || sign_ok=0
    if [ "$sign_ok" = 1 ] && ! codesign --verify --deep --strict "$STAGE"; then
        sign_ok=0
    fi
    # An ad-hoc signature can't satisfy Gatekeeper for a downloaded build
    # anyway (only notarisation does), so a hiccup there must not block the
    # .dmg — it still carries the real fixes. A real Developer ID identity is
    # different: shipping it half-signed would be a silent failure, so fail.
    if [ "$sign_ok" != 1 ]; then
        if [ "$IDENTITY" != "-" ]; then
            echo "pack-macos.sh: ERROR: signing with '$IDENTITY' failed" >&2
            exit 1
        fi
        echo "pack-macos.sh: warning: ad-hoc signing incomplete; bundle may need 'xattr -dr com.apple.quarantine'" >&2
    fi
else
    echo "pack-macos.sh: warning: codesign not found; shipping an unsigned bundle" >&2
fi

rm -f "$DMG"
if ! hdiutil create -volname "Nordstjernen ${VERSION}" \
    -srcfolder "$STAGE" \
    -ov -format UDZO -imagekey zlib-level=1 \
    "$DMG"; then
    echo "pack-macos.sh: hdiutil create failed" >&2
    ls -la "$STAGE" || true
    exit 1
fi

# Notarise and staple the .dmg when a stored notarytool credential profile is
# supplied (created once with `xcrun notarytool store-credentials <name>`). This
# only runs for a real Developer ID identity: an ad-hoc signature can never
# notarise, so the step is skipped and the .dmg still ships (its quarantine is
# cleared with xattr, see docs/macOS.md). Submitting the .dmg notarises the
# signed .app inside it, and stapling attaches the ticket so a downloaded build
# opens with no Gatekeeper prompt. A failure on a real identity is fatal —
# publishing a build that looks notarised but isn't is worse than shipping none.
NOTARY_PROFILE="${MACOS_NOTARY_PROFILE:-}"
if [ -n "$NOTARY_PROFILE" ] && [ "$IDENTITY" != "-" ]; then
    echo "pack-macos.sh: submitting $DMG to Apple notary service (profile: $NOTARY_PROFILE)..."
    if ! xcrun notarytool submit "$DMG" \
            --keychain-profile "$NOTARY_PROFILE" --wait; then
        echo "pack-macos.sh: ERROR: notarisation submission failed" >&2
        echo "pack-macos.sh:   inspect a rejection with: xcrun notarytool log <submission-id> --keychain-profile '$NOTARY_PROFILE'" >&2
        exit 1
    fi
    if ! xcrun stapler staple "$DMG"; then
        echo "pack-macos.sh: ERROR: stapling the notarisation ticket to $DMG failed" >&2
        exit 1
    fi
    echo "pack-macos.sh: notarised and stapled $DMG"
    spctl -a -vv -t open "$DMG" 2>&1 | sed 's/^/pack-macos.sh: spctl: /' || true
elif [ "$IDENTITY" != "-" ]; then
    echo "pack-macos.sh: note: signed with '$IDENTITY' but MACOS_NOTARY_PROFILE is unset — skipping notarisation." >&2
    echo "pack-macos.sh:   notarise manually: xcrun notarytool submit '$DMG' --keychain-profile <profile> --wait && xcrun stapler staple '$DMG'" >&2
fi

echo
echo "Built: $DMG ($(du -h "$DMG" | cut -f1))"
echo "Bundle: $STAGE ($(du -sh "$STAGE" | cut -f1))"
echo
echo "Test:  open '$STAGE'"
echo "       '$STAGE/Contents/MacOS/Nordstjernen' --headless --dump=text about:start"
