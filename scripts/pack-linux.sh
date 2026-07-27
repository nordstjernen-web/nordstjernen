#!/usr/bin/env bash
# Build a redistributable Nordstjernen Linux x86_64 release: the stripped,
# LTO-optimised nordstjernen binary plus the data files it needs at runtime,
# zipped under dist/.
set -euo pipefail

log() { printf '[pack-linux] %s\n' "$*" >&2; }
trap 'rc=$?; printf "[pack-linux] FAILED (exit %s) at line %s: %s\n" \
    "$rc" "$LINENO" "$BASH_COMMAND" >&2; exit $rc' ERR
[ -n "${NS_DEBUG:-}" ] && set -x

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

# WebGPU (experimental): the meson feature is `auto`, so it is compiled in
# whenever wgpu-native is reachable. For a redistributable build we fetch the
# pinned wgpu-native release, point meson at it, and bundle the shared library
# beside the binaries with an $ORIGIN rpath. NS_WEBGPU=0/disabled forces it off;
# NS_WEBGPU=1/enabled makes a missing wgpu-native fatal; anything else (the
# default) degrades to a WebGPU-free build when wgpu-native cannot be fetched.
WEBGPU_MODE=${NS_WEBGPU:-auto}
WGPU_ROOT=""
WEBGPU_SETUP_ARGS=()
case "$WEBGPU_MODE" in
    0|off|disabled|no) log "WebGPU: disabled (NS_WEBGPU=$WEBGPU_MODE)" ;;
    *)
        if WGPU_ROOT=$("$ROOT/scripts/fetch-wgpu-native.sh"); then
            WEBGPU_SETUP_ARGS=( -Dwebgpu=enabled -Dwgpu_native_root="$WGPU_ROOT" )
            log "WebGPU: enabled (wgpu-native at $WGPU_ROOT)"
        else
            WGPU_ROOT=""
            if [ "$WEBGPU_MODE" = 1 ] || [ "$WEBGPU_MODE" = enabled ] || [ "$WEBGPU_MODE" = on ]; then
                log "ERROR: NS_WEBGPU=$WEBGPU_MODE but wgpu-native could not be fetched"
                exit 1
            fi
            log "WebGPU: not bundled (wgpu-native unavailable for this platform); building without it"
        fi ;;
esac

if [ ! -d "$BUILDDIR" ]; then
    meson setup "$BUILDDIR" --buildtype=release -Db_lto="${NS_BUILD_LTO:-true}" \
        -Db_ndebug=true --strip \
        ${NS_BUILD_DATE:+-Dbuild_date="$NS_BUILD_DATE"} \
        ${WEBGPU_SETUP_ARGS[@]+"${WEBGPU_SETUP_ARGS[@]}"}
fi
meson compile -C "$BUILDDIR" ${NS_BUILD_JOBS:+-j "$NS_BUILD_JOBS"}
strip --strip-all "$BUILDDIR/src/gtk/nordstjernen"
# The GUI is a thin shell that spawns one sandboxed nordstjernen-renderer
# process per tab; it must ship alongside the main binary.
strip --strip-all "$BUILDDIR/src/nordstjernen-renderer"
# The audio playback helper (MP2/MP3 decode + SDL2 output). Built only when
# SDL2 was present at configure time (meson 'audio' feature is auto); ship it
# beside the main binary so the shell can spawn it for <video>/<audio> sound.
AUDIO_BIN="$BUILDDIR/src/nordstjernen-audio"
if [ -f "$AUDIO_BIN" ]; then
    strip --strip-all "$AUDIO_BIN"
fi
# Inline WebM (VP9/VP8 + Opus/Vorbis) is compiled in only when FFmpeg's libav*
# was present at configure time (meson auto-detect). When it is, the engine
# links libavformat directly, so the FFmpeg runtime libraries become a hard
# requirement documented below.
WEBM=0
if ldd "$BUILDDIR/src/nordstjernen-renderer" 2>/dev/null | grep -q 'libavformat'; then
    WEBM=1
    log "WebM: built (libav linked) — FFmpeg runtime libs required at install"
else
    log "WebM: not built (libav absent at configure time)"
fi

LOADER=$(ldd "$BUILDDIR/src/gtk/nordstjernen" 2>/dev/null \
    | grep -m1 -oE 'ld-(musl|linux)[^ ]*' || true)
if printf '%s' "$LOADER" | grep -q 'ld-musl'; then
    LIBC=musl
else
    LIBC=glibc
fi
[ -n "$LOADER" ] || LOADER="ld-$LIBC"

if [ "$LIBC" = musl ]; then
    LIBC_REQ='- musl libc (Alpine 3.19+ era and later)'
    RUNTIME_INSTALL='    sudo apk add gtk4.0 libcurl uchardet librsvg poppler-glib libavif libwebp \
        libseccomp libpsl sqlite-libs ca-certificates font-dejavu sdl2 # Alpine (musl)'
else
    # Don't guess the glibc floor — read it from the binary we just built.
    GLIBC_MIN=$(objdump -T "$BUILDDIR/src/gtk/nordstjernen" 2>/dev/null \
        | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -t. -k1,1V -k2,2n -u \
        | tail -1 | cut -d_ -f2)
    LIBC_REQ="- glibc ${GLIBC_MIN:-2.38}+ (the build container's generation; check with: ldd --version)"
    RUNTIME_INSTALL='    sudo apt   install libgtk-4-1 libcurl4 libuchardet0 librsvg2-2 libwebp7 \
        libpoppler-glib8 libavif16 libpsl5 libseccomp2 libsqlite3-0 libsdl2-2.0-0   # Debian/Ubuntu
    sudo dnf   install gtk4 libcurl libuchardet librsvg2 poppler-glib libwebp \
        libavif libpsl libseccomp sqlite-libs SDL2                     # Fedora/RHEL
    sudo zypper install libgtk-4-1 libcurl4 libuchardet0 librsvg-2-2 libwebp7 \
        libpoppler-glib8 libavif16 libpsl5 libseccomp2 libsqlite3-0 libSDL2-2_0-0   # openSUSE'
fi

rm -rf "$STAGE"
mkdir -p "$STAGE/data/icons/hicolor/scalable/apps"
cp "$BUILDDIR/src/gtk/nordstjernen" "$STAGE/"
cp "$BUILDDIR/src/nordstjernen-renderer" "$STAGE/"
if [ -f "$AUDIO_BIN" ]; then
    cp "$AUDIO_BIN" "$STAGE/"
fi
cp "$ROOT"/data/icons/hicolor/scalable/apps/nordstjernen*.svg \
   "$ROOT"/data/icons/hicolor/scalable/apps/nordstjernen.gif \
   "$STAGE/data/icons/hicolor/scalable/apps/"
cp "$ROOT/data/nordstjernen.desktop" "$STAGE/data/"

# When WebGPU was built, ship libwgpu_native.so beside the binaries and point
# their rpath at $ORIGIN so the renderer (and the GTK shell, which links
# the engine) load it from the bundle rather than a system path. WebGPU still
# stays dormant at runtime until the browser is launched with --enable-webgpu.
WEBGPU_BUNDLED=0
if [ -n "$WGPU_ROOT" ] && [ -e "$WGPU_ROOT/lib/libwgpu_native.so" ]; then
    if command -v patchelf >/dev/null 2>&1; then
        cp -L "$WGPU_ROOT/lib/libwgpu_native.so" "$STAGE/libwgpu_native.so"
        chmod 644 "$STAGE/libwgpu_native.so"
        # wgpu-native ships the cdylib with no SONAME, so meson recorded the
        # full build-tree path as the binaries' DT_NEEDED — an $ORIGIN rpath
        # alone would be ignored. Give the bundled copy a plain soname and
        # rewrite each binary's NEEDED to the bare name so $ORIGIN resolves it.
        patchelf --set-soname libwgpu_native.so "$STAGE/libwgpu_native.so"
        for bin in nordstjernen nordstjernen-renderer; do
            [ -e "$STAGE/$bin" ] || continue
            cur=$(patchelf --print-needed "$STAGE/$bin" \
                  | grep -E '(^|/)libwgpu_native\.so$' | head -1)
            if [ -n "$cur" ] && [ "$cur" != libwgpu_native.so ]; then
                patchelf --replace-needed "$cur" libwgpu_native.so "$STAGE/$bin"
            fi
            patchelf --set-rpath '$ORIGIN' "$STAGE/$bin"
        done
        WEBGPU_BUNDLED=1
        log "WebGPU: bundled libwgpu_native.so (rpath \$ORIGIN)"
    else
        log "WebGPU: built but patchelf is missing; not bundling libwgpu_native.so"
    fi
fi

cp "$ROOT/README.md" "$STAGE/"
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$STAGE/"
cp "$ROOT/License.md" "$STAGE/"

if [ "$WEBM" = 1 ]; then
    WEBM_REQ_NOTE='- FFmpeg libav* (libavformat / libavcodec / libavutil / libswscale /
  libswresample) — **inline WebM playback** (VP9/VP8 video + Opus/Vorbis
  audio). This build links them, so the binary will not start unless they
  are installed:

      sudo apt    install ffmpeg         # Debian/Ubuntu
      sudo dnf    install ffmpeg-libs    # Fedora/RHEL (RPM Fusion)
      sudo zypper install ffmpeg         # openSUSE (Packman)
      sudo apk add ffmpeg-libs           # Alpine (musl)
'
else
    WEBM_REQ_NOTE=''
fi

if [ "$WEBGPU_BUNDLED" = 1 ]; then
    WEBGPU_RUN_NOTE='
### Experimental WebGPU

This build includes experimental WebGPU (`navigator.gpu`) over the bundled
`libwgpu_native.so` (loaded from beside the binary via an `$ORIGIN` rpath). It
is **off by default**; start the browser with `--enable-webgpu` to turn it on:

    ./nordstjernen --enable-webgpu https://example.com
'
else
    WEBGPU_RUN_NOTE=''
fi

cat > "$STAGE/INSTALL.md" <<EOF
# Nordstjernen ${VERSION} — Linux ${ARCH} binary

Stripped, LTO-optimised build. The browser engine itself (lexbor for
HTML, quickjs for JavaScript, wuffs for image decoding, wamr for
WebAssembly) is statically linked into the binary. The GTK 4 desktop
stack stays dynamic because it expects to find pixbuf loaders, IM
modules and font/theme data on the host at runtime — fully-static GTK
isn't practical.

## Runtime requirements

This is a ${LIBC} build (linked against ${LOADER}). It will not run
against the other C library — pick the matching download.

${LIBC_REQ}
- GTK 4.6+, with gio, gobject, pango, cairo, gdk-pixbuf
- libepoxy (usually pulled in by GTK 4; WebGL dispatch)
- libcurl with a TLS backend; OpenSSL 3 (libcrypto)
- libuchardet
- librsvg (SVG rendering)
- libpoppler-glib (PDF rendering)
- libavif 16 (AVIF images — only in recent distro releases)
- libwebp (WebP images)
${WEBM_REQ_NOTE}- libpsl, libseccomp, libsqlite3
- fontconfig + a font set; harfbuzz; freetype; libstdc++
- ca-certificates (TLS trust store)
- An X11 or Wayland session

Distro install commands:

${RUNTIME_INSTALL}

For Linux distros without modern GTK 4, build an AppImage instead
(future work).

## Run

    ./nordstjernen https://example.com
${WEBGPU_RUN_NOTE}

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
