#!/usr/bin/env bash
# Build a redistributable Nordstjernen Windows bundle: nordstjernen.exe plus the
# mingw64 DLLs and GTK runtime data it needs to run outside MSYS2.
#
# Builds (or reuses) a separate --buildtype=release tree in $BUILDDIR so the
# shipped binary has NDEBUG defined — third-party assertions in vendored deps
# like quickjs-ng are compiled out, and the optimiser runs at -O3.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILDDIR=${BUILDDIR:-$ROOT/builddir-release}
OUT=${OUT:-$ROOT/dist/nordstjernen-win64}
BIN_SRC=$BUILDDIR/src/nordstjernen.exe

resolve_mingw_prefix() {
    for cand in "${MINGW_PREFIX:-}" /c/msys64/mingw64 "C:/msys64/mingw64" \
                /mingw64; do
        [ -n "$cand" ] || continue
        [ -d "$cand/bin" ] || continue
        [ -f "$cand/bin/libgtk-4-1.dll" ] || continue
        echo "$cand"; return
    done
    return 1
}

MINGW_PREFIX=$(resolve_mingw_prefix) || {
    echo "pack-windows: could not find mingw64/bin; set MINGW_PREFIX." >&2
    exit 1
}

if [ ! -d "$BUILDDIR" ]; then
    meson setup "$BUILDDIR" --buildtype=release
fi
meson compile -C "$BUILDDIR"

if [ ! -x "$BIN_SRC" ]; then
    echo "pack-windows: build did not produce $BIN_SRC" >&2
    exit 1
fi

rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$BIN_SRC" "$OUT/nordstjernen.exe"

# GLib settings schemas (compiled). Apps that GSettings-look-up a key crash without these.
mkdir -p "$OUT/share/glib-2.0/schemas"
if [ -f "$MINGW_PREFIX/share/glib-2.0/schemas/gschemas.compiled" ]; then
    cp "$MINGW_PREFIX/share/glib-2.0/schemas/gschemas.compiled" "$OUT/share/glib-2.0/schemas/"
fi

# GDK-PixBuf loader cache + loader DLLs (image decode for <img>). Copied
# *before* the DLL chase so the loaders' transitive deps (notably
# librsvg-2-2.dll, pulled in only by pixbufloader_svg.dll) get bundled too.
# GdkPixbuf loads these dynamically via loaders.cache, so their import edges
# don't appear in nordstjernen.exe's static-import graph.
if [ -d "$MINGW_PREFIX/lib/gdk-pixbuf-2.0" ]; then
    mkdir -p "$OUT/lib"
    cp -r "$MINGW_PREFIX/lib/gdk-pixbuf-2.0" "$OUT/lib/"
fi

# Transitively resolve DLL dependencies starting from the exe and every
# pixbuf loader DLL. objdump reports import names; we look them up in the
# mingw bin dir and skip anything that resolves to a Windows system DLL.
declare -A seen
queue=("$OUT/nordstjernen.exe")
for loader in "$OUT"/lib/gdk-pixbuf-2.0/*/loaders/*.dll; do
    [ -f "$loader" ] && queue+=("$loader")
done
while [ ${#queue[@]} -gt 0 ]; do
    cur=${queue[0]}
    queue=("${queue[@]:1}")
    deps=$(objdump -p "$cur" 2>/dev/null | awk '/DLL Name:/ {print $3}') || true
    for dep in $deps; do
        key=$(printf '%s' "$dep" | tr '[:upper:]' '[:lower:]')
        if [ -n "${seen[$key]:-}" ]; then continue; fi
        seen[$key]=1
        src=$MINGW_PREFIX/bin/$dep
        if [ ! -f "$src" ]; then
            # case-insensitive fallback for DLLs whose import name capitalisation
            # differs from the on-disk file name
            alt=$(find "$MINGW_PREFIX/bin" -maxdepth 1 -iname "$dep" -print -quit 2>/dev/null || true)
            [ -n "$alt" ] && src=$alt
        fi
        if [ -f "$src" ]; then
            cp "$src" "$OUT/"
            queue+=("$OUT/$(basename "$src")")
        fi
    done
done

# Adwaita + hicolor icons for default GTK widget glyphs (back/forward arrows, etc.).
mkdir -p "$OUT/share/icons"
for theme in Adwaita hicolor; do
    if [ -d "$MINGW_PREFIX/share/icons/$theme" ]; then
        cp -r "$MINGW_PREFIX/share/icons/$theme" "$OUT/share/icons/"
    fi
done

# Nordstjernen's own application + toolbar icons (drop into the hicolor theme
# so gtk_image_new_from_icon_name("nordstjernen-back") and friends resolve at
# runtime, and about: pages can read the svg/gif as a data URI). The dev tree
# finds these via the ../../data/icons search path; the bundle only has
# share/icons, so every nordstjernen-*.svg the toolbar references must be
# copied here or the header-bar buttons render blank. Refresh the hicolor
# cache so the bundled icons show up without a filesystem scan.
mkdir -p "$OUT/share/icons/hicolor/scalable/apps"
cp "$ROOT"/data/icons/hicolor/scalable/apps/nordstjernen*.svg \
   "$ROOT/data/icons/hicolor/scalable/apps/nordstjernen.gif" \
   "$OUT/share/icons/hicolor/scalable/apps/"
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache --force --ignore-theme-index \
        "$OUT/share/icons/hicolor" >/dev/null 2>&1 || true
fi

# Per-application data: license text. The browser reads it relative to
# the exe at runtime (see src/net.c::about_read_first).
mkdir -p "$OUT/share/nordstjernen"
cp "$ROOT/License.md" "$OUT/share/nordstjernen/"

# Third-party copyright + license notices required by the libraries we ship.
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$OUT/"

# CA certificate bundle for libcurl HTTPS verification.
mkdir -p "$OUT/etc/ssl/certs"
for ca in \
    "$MINGW_PREFIX/etc/ssl/certs/ca-bundle.crt" \
    "$MINGW_PREFIX/etc/ssl/cert.pem" \
    "$MINGW_PREFIX/ssl/certs/ca-bundle.crt"; do
    if [ -f "$ca" ]; then
        cp "$ca" "$OUT/etc/ssl/certs/ca-bundle.crt"
        break
    fi
done

bundled=$(find "$OUT" -maxdepth 1 -name '*.dll' | wc -l)
size=$(du -sh "$OUT" | awk '{print $1}')
printf 'pack-windows: bundled %s DLLs, total size %s, output: %s\n' "$bundled" "$size" "$OUT"
