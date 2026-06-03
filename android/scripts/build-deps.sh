#!/usr/bin/env bash
#
# Cross-compile the Nordstjernen engine (and stage its shared-library
# dependencies) for Android, producing per-ABI libnordstjernen.so under
# android/app/src/main/jniLibs/<abi>/. Once that file exists for an ABI, the
# Gradle/CMake build links the real JNI bridge against it; otherwise the build
# falls back to the stub bridge (engine reported unavailable).
#
# Prerequisites
# -------------
#   * Android NDK r26+         -> ANDROID_NDK_HOME
#   * meson + ninja on PATH
#   * A "sysroot" prefix holding the engine's native dependencies already
#     cross-built for the target ABI, with working .pc files. Point
#     NORDSTJERNEN_ANDROID_SYSROOT at it. Since the engine drops GTK 4,
#     librsvg and gdk-pixbuf on Android (see meson.build / src/texture.c),
#     the required set is just the GLib/cairo/pango stack plus the network
#     and storage libraries:
#       glib-2.0, gobject-2.0, gio-2.0, gmodule-2.0,
#       cairo, pango, pangocairo (+ harfbuzz, freetype2, fontconfig,
#       pixman, libffi, pcre2, expat, zlib, libpng),
#       libcurl, sqlite3, uchardet, libpsl.
#     All are plain C and cross-build with meson against the NDK — no Rust
#     toolchain is needed (librsvg, the only Rust dependency, is gone).
#
# Building that dependency sysroot is the bulk of the porting work and is out
# of scope for this script; see android/README.md for the current status.
#
# Usage:
#   ANDROID_NDK_HOME=~/Android/Sdk/ndk/26.3.11579264 \
#   NORDSTJERNEN_ANDROID_SYSROOT=~/nd-android-sysroot/arm64-v8a \
#   android/scripts/build-deps.sh arm64-v8a 26

set -euo pipefail

ABI="${1:-arm64-v8a}"
API="${2:-26}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JNILIBS="${REPO_ROOT}/android/app/src/main/jniLibs/${ABI}"
WORK="${REPO_ROOT}/android/.build/${ABI}"

: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to your NDK path}"

case "${ABI}" in
    arm64-v8a)   TRIPLE=aarch64-linux-android;     CPU_FAMILY=aarch64; CPU=aarch64 ;;
    armeabi-v7a) TRIPLE=armv7a-linux-androideabi;  CPU_FAMILY=arm;     CPU=armv7 ;;
    x86_64)      TRIPLE=x86_64-linux-android;       CPU_FAMILY=x86_64;  CPU=x86_64 ;;
    x86)         TRIPLE=i686-linux-android;         CPU_FAMILY=x86;     CPU=i686 ;;
    *) echo "unknown ABI: ${ABI}" >&2; exit 2 ;;
esac

HOST_TAG="linux-x86_64"
case "$(uname -s)" in
    Darwin) HOST_TAG="darwin-x86_64" ;;
esac
TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}"
CC="${TOOLCHAIN}/bin/${TRIPLE}${API}-clang"
AR="${TOOLCHAIN}/bin/llvm-ar"
STRIP="${TOOLCHAIN}/bin/llvm-strip"

if [ ! -x "${CC}" ]; then
    echo "compiler not found: ${CC}" >&2
    exit 2
fi

mkdir -p "${WORK}" "${JNILIBS}"
CROSS="${WORK}/android-${ABI}.cross"

SYSROOT_PKGCONFIG=""
if [ -n "${NORDSTJERNEN_ANDROID_SYSROOT:-}" ]; then
    SYSROOT_PKGCONFIG="${NORDSTJERNEN_ANDROID_SYSROOT}/lib/pkgconfig"
fi

cat > "${CROSS}" <<EOF
[binaries]
c = '${CC}'
ar = '${AR}'
strip = '${STRIP}'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-fPIC', '--sysroot=${TOOLCHAIN}/sysroot']
c_link_args = ['--sysroot=${TOOLCHAIN}/sysroot']

[properties]
needs_exe_wrapper = true
sys_root = '${TOOLCHAIN}/sysroot'
pkg_config_libdir = '${SYSROOT_PKGCONFIG}'

[host_machine]
system = 'android'
cpu_family = '${CPU_FAMILY}'
cpu = '${CPU}'
endian = 'little'
EOF

echo "Wrote cross file: ${CROSS}"

if [ -z "${NORDSTJERNEN_ANDROID_SYSROOT:-}" ] || [ ! -d "${SYSROOT_PKGCONFIG}" ]; then
    cat >&2 <<MSG

NORDSTJERNEN_ANDROID_SYSROOT is not set (or has no lib/pkgconfig). The engine
cannot be cross-compiled without the dependency sysroot. Cross file has been
generated at:
  ${CROSS}
Build the dependency stack for ${ABI}, point NORDSTJERNEN_ANDROID_SYSROOT at
its prefix, and re-run. The APK will build with the stub engine until then.
MSG
    exit 0
fi

BUILDDIR="${WORK}/builddir"
rm -rf "${BUILDDIR}"

export PKG_CONFIG_LIBDIR="${SYSROOT_PKGCONFIG}"
export PKG_CONFIG_SYSROOT_DIR="${NORDSTJERNEN_ANDROID_SYSROOT}"

meson setup "${BUILDDIR}" "${REPO_ROOT}" \
    --cross-file "${CROSS}" \
    --buildtype release \
    -Ddefault_library=shared

meson compile -C "${BUILDDIR}"

ENGINE_SO="$(find "${BUILDDIR}/src" -name 'libnordstjernen.so*' -type f | head -1)"
if [ -z "${ENGINE_SO}" ]; then
    echo "engine .so not produced" >&2
    exit 1
fi

cp -v "${ENGINE_SO}" "${JNILIBS}/libnordstjernen.so"
"${STRIP}" "${JNILIBS}/libnordstjernen.so" || true

for so in "${NORDSTJERNEN_ANDROID_SYSROOT}"/lib/*.so; do
    [ -e "${so}" ] || continue
    cp -v "${so}" "${JNILIBS}/"
done

echo "Staged engine + deps into ${JNILIBS}"
