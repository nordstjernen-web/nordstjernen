#!/usr/bin/env bash
#
# Build the engine (libnordstjernen) and the JNI bridge (libnordstjernenjni),
# staging both into java/src/main/resources/native/<os>-<arch>/ so the Gradle
# jar bundles them and NativeLoader can extract them at runtime.
#
# Requires: JAVA_HOME (JDK 21), meson + ninja, and the engine's build deps
# (gtk4/cairo/pango/curl/...). See the repo README.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILDDIR="${BUILDDIR:-$REPO_ROOT/builddir}"
: "${JAVA_HOME:?set JAVA_HOME to a JDK 21}"

if [ ! -e "$BUILDDIR/build.ninja" ]; then
    meson setup "$BUILDDIR" "$REPO_ROOT" -Ddefault_library=shared
fi
meson compile -C "$BUILDDIR"

OS="$(uname -s)"
ARCH="$(uname -m)"
case "$OS" in
    Linux)  P_OS=linux  ; SOEXT=so    ; RPATH='-Wl,-rpath,$ORIGIN'          ;;
    Darwin) P_OS=macos  ; SOEXT=dylib ; RPATH='-Wl,-rpath,@loader_path'     ;;
    *)      P_OS=linux  ; SOEXT=so    ; RPATH='-Wl,-rpath,$ORIGIN'          ;;
esac
case "$ARCH" in
    x86_64|amd64)  P_ARCH=x86_64  ;;
    aarch64|arm64) P_ARCH=aarch64 ;;
    *)             P_ARCH="$ARCH" ;;
esac
PLAT="$P_OS-$P_ARCH"

ENGINE="$(ls "$BUILDDIR"/src/libnordstjernen.$SOEXT 2>/dev/null | head -1 || true)"
if [ -z "$ENGINE" ]; then
    echo "engine library not found in $BUILDDIR/src" >&2
    exit 1
fi

JNI_MD_DIR="$JAVA_HOME/include/$P_OS"
[ -d "$JNI_MD_DIR" ] || JNI_MD_DIR="$JAVA_HOME/include/linux"

OUTDIR="$REPO_ROOT/java/src/main/resources/native/$PLAT"
mkdir -p "$OUTDIR"

CC="${CC:-cc}"
$CC -shared -fPIC -O2 \
    -I"$JAVA_HOME/include" -I"$JNI_MD_DIR" -I"$REPO_ROOT/src" \
    "$REPO_ROOT/java/src/main/native/nd_java_jni.c" \
    -L"$(dirname "$ENGINE")" -lnordstjernen \
    $RPATH \
    -o "$OUTDIR/libnordstjernenjni.$SOEXT"

cp -f "$ENGINE" "$OUTDIR/libnordstjernen.$SOEXT"

echo "Staged libnordstjernen.$SOEXT + libnordstjernenjni.$SOEXT into $OUTDIR"
