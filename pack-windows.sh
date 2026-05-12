#!/usr/bin/env bash
# Build a redistributable Nordstjernen Windows bundle: nordstjernen.exe plus the
# mingw64 DLLs and GTK runtime data it needs to run outside MSYS2.
#
# Builds (or reuses) a separate --buildtype=release tree in $BUILDDIR so the
# shipped binary has NDEBUG defined — third-party assertions in vendored deps
# like quickjs-ng are compiled out, and the optimiser runs at -O3.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
MINGW_PREFIX=${MINGW_PREFIX:-/mingw64}
BUILDDIR=${BUILDDIR:-$ROOT/builddir-release}
OUT=${OUT:-$ROOT/dist/nordstjernen-win64}
BIN_SRC=$BUILDDIR/src/nordstjernen.exe

if [ ! -d "$MINGW_PREFIX/bin" ]; then
    echo "pack-windows: MINGW_PREFIX=$MINGW_PREFIX/bin not found" >&2
    exit 1
fi

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

# Transitively resolve DLL dependencies starting from the exe and any DLL we
# already copied. objdump reports import names; we look them up in the mingw
# bin dir and skip anything that resolves to a Windows system DLL.
declare -A seen
queue=("$OUT/nordstjernen.exe")
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

# GLib settings schemas (compiled). Apps that GSettings-look-up a key crash without these.
mkdir -p "$OUT/share/glib-2.0/schemas"
if [ -f "$MINGW_PREFIX/share/glib-2.0/schemas/gschemas.compiled" ]; then
    cp "$MINGW_PREFIX/share/glib-2.0/schemas/gschemas.compiled" "$OUT/share/glib-2.0/schemas/"
fi

# GDK-PixBuf loader cache + loader DLLs (image decode for <img>).
if [ -d "$MINGW_PREFIX/lib/gdk-pixbuf-2.0" ]; then
    mkdir -p "$OUT/lib"
    cp -r "$MINGW_PREFIX/lib/gdk-pixbuf-2.0" "$OUT/lib/"
fi

# Adwaita + hicolor icons for default GTK widget glyphs (back/forward arrows, etc.).
mkdir -p "$OUT/share/icons"
for theme in Adwaita hicolor; do
    if [ -d "$MINGW_PREFIX/share/icons/$theme" ]; then
        cp -r "$MINGW_PREFIX/share/icons/$theme" "$OUT/share/icons/"
    fi
done

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

# Tiny launcher batch file that sets GTK_DATA_PREFIX / cert env so the bundle
# is self-contained even when run from outside the directory.
cat > "$OUT/nordstjernen.cmd" <<'EOF'
@echo off
setlocal
set "ND_DIR=%~dp0"
set "GTK_DATA_PREFIX=%ND_DIR%"
set "GDK_PIXBUF_MODULE_FILE=%ND_DIR%lib\gdk-pixbuf-2.0\2.10.0\loaders.cache"
set "CURL_CA_BUNDLE=%ND_DIR%etc\ssl\certs\ca-bundle.crt"
set "SSL_CERT_FILE=%ND_DIR%etc\ssl\certs\ca-bundle.crt"
set "PATH=%ND_DIR%;%PATH%"
start "" "%ND_DIR%nordstjernen.exe" %*
EOF

bundled=$(find "$OUT" -maxdepth 1 -name '*.dll' | wc -l)
size=$(du -sh "$OUT" | awk '{print $1}')
printf 'pack-windows: bundled %s DLLs, total size %s, output: %s\n' "$bundled" "$size" "$OUT"
