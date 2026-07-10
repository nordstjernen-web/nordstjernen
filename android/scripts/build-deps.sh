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
#       libcurl, sqlite3, uchardet, libpsl, libwebp.
#     All are plain C and cross-build with meson against the NDK — no Rust
#     toolchain is needed (librsvg, the only Rust dependency, is gone).
#
# Building that dependency sysroot is the bulk of the porting work and is out
# of scope for this script; see android/README.md for the current status.
#
# The generated cross-file links everything with
# -Wl,-z,max-page-size=16384 so the engine .so satisfies Google Play's
# 16 KB page-size requirement (mandatory for apps targeting Android 15+;
# NDK r28+ does this by default, r27 and older need the explicit flag).
# Cross-build the dependency sysroot with the same flag.
#
# The second argument is the NDK platform API level (default 34 = Android 14).
# It must be <= the app's minSdk (android/app/build.gradle); a .so built at a
# higher level can bind bionic symbols the device lacks, so dlopen fails at
# load. Build the dependency sysroot at the same API level for the same reason.
#
# Usage:
#   ANDROID_NDK_HOME=~/Android/Sdk/ndk/27.3.13750724 \
#   NORDSTJERNEN_ANDROID_SYSROOT=~/.cache/nordstjernen-android-sysroot \
#   android/scripts/build-deps.sh x86_64 34

set -euo pipefail

ABI="${1:-arm64-v8a}"
API="${2:-34}"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
JNILIBS="${REPO_ROOT}/android/app/src/main/jniLibs/${ABI}"
WORK="${REPO_ROOT}/android/.build/${ABI}"
LOGDIR="${REPO_ROOT}/android/.build/logs"

: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME to your NDK path}"

case "${ABI}" in
    arm64-v8a)   TRIPLE=aarch64-linux-android;     CPU_FAMILY=aarch64; CPU=aarch64 ;;
    armeabi-v7a) TRIPLE=armv7a-linux-androideabi;  CPU_FAMILY=arm;     CPU=armv7 ;;
    x86_64)      TRIPLE=x86_64-linux-android;       CPU_FAMILY=x86_64;  CPU=x86_64 ;;
    x86)         TRIPLE=i686-linux-android;         CPU_FAMILY=x86;     CPU=i686 ;;
    *) echo "unknown ABI: ${ABI}" >&2; exit 2 ;;
esac

HOST_TAG="linux-x86_64"
TOOL_EXT=""
EXE_EXT=""
case "$(uname -s)" in
    Darwin) HOST_TAG="darwin-x86_64" ;;
    MINGW*|MSYS*|CYGWIN*) HOST_TAG="windows-x86_64"; TOOL_EXT=".cmd"; EXE_EXT=".exe" ;;
esac
TOOLCHAIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/${HOST_TAG}"
CC="${TOOLCHAIN}/bin/${TRIPLE}${API}-clang${TOOL_EXT}"
CXX="${TOOLCHAIN}/bin/${TRIPLE}${API}-clang++${TOOL_EXT}"
AR="${TOOLCHAIN}/bin/llvm-ar${EXE_EXT}"
STRIP="${TOOLCHAIN}/bin/llvm-strip${EXE_EXT}"

if [ ! -f "${CC}" ]; then
    echo "compiler not found: ${CC}" >&2
    exit 2
fi
if [ ! -f "${CXX}" ]; then
    echo "compiler not found: ${CXX}" >&2
    exit 2
fi

mkdir -p "${WORK}" "${JNILIBS}" "${LOGDIR}"
CROSS="${WORK}/android-${ABI}.cross"

SYSROOT_PREFIX=""
SYSROOT_PKGCONFIG=""
if [ -n "${NORDSTJERNEN_ANDROID_SYSROOT:-}" ]; then
    if [ -d "${NORDSTJERNEN_ANDROID_SYSROOT}/${ABI}/lib/pkgconfig" ]; then
        SYSROOT_PREFIX="${NORDSTJERNEN_ANDROID_SYSROOT}/${ABI}"
    else
        SYSROOT_PREFIX="${NORDSTJERNEN_ANDROID_SYSROOT}"
    fi
    SYSROOT_PKGCONFIG="${SYSROOT_PREFIX}/lib/pkgconfig"
fi

CC_MESON="${CC}"
CXX_MESON="${CXX}"
AR_MESON="${AR}"
STRIP_MESON="${STRIP}"
TOOLCHAIN_SYSROOT="${TOOLCHAIN}/sysroot"
TOOLCHAIN_SYSROOT_MESON="${TOOLCHAIN_SYSROOT}"
SYSROOT_PKGCONFIG_MESON="${SYSROOT_PKGCONFIG}"
case "$(uname -s)" in
    MINGW*|MSYS*|CYGWIN*)
        CC_MESON="$(cygpath -m "${CC}")"
        CXX_MESON="$(cygpath -m "${CXX}")"
        AR_MESON="$(cygpath -m "${AR}")"
        STRIP_MESON="$(cygpath -m "${STRIP}")"
        TOOLCHAIN_SYSROOT_MESON="$(cygpath -m "${TOOLCHAIN_SYSROOT}")"
        [ -n "${SYSROOT_PKGCONFIG}" ] &&
            SYSROOT_PKGCONFIG_MESON="$(cygpath -m "${SYSROOT_PKGCONFIG}")"
        ;;
esac

cat > "${CROSS}" <<EOF
[binaries]
c = '${CC_MESON}'
cpp = '${CXX_MESON}'
ar = '${AR_MESON}'
strip = '${STRIP_MESON}'
pkg-config = 'pkg-config'

[built-in options]
c_args = ['-fPIC', '--sysroot=${TOOLCHAIN_SYSROOT_MESON}']
cpp_args = ['-fPIC', '--sysroot=${TOOLCHAIN_SYSROOT_MESON}']
c_link_args = ['--sysroot=${TOOLCHAIN_SYSROOT_MESON}', '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384']
cpp_link_args = ['--sysroot=${TOOLCHAIN_SYSROOT_MESON}', '-Wl,-z,max-page-size=16384', '-Wl,-z,common-page-size=16384']

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '${SYSROOT_PKGCONFIG_MESON}'

[host_machine]
system = 'android'
cpu_family = '${CPU_FAMILY}'
cpu = '${CPU}'
endian = 'little'
EOF

echo "Wrote cross file: ${CROSS}"
echo "Android ABI: ${ABI}"
echo "NDK toolchain: ${TOOLCHAIN}"
echo "Dependency sysroot: ${SYSROOT_PREFIX:-not set}"
echo "pkg-config dir: ${SYSROOT_PKGCONFIG:-not set}"

if [ -z "${NORDSTJERNEN_ANDROID_SYSROOT:-}" ] || [ ! -d "${SYSROOT_PKGCONFIG}" ]; then
    cat >&2 <<MSG

NORDSTJERNEN_ANDROID_SYSROOT is not set (or has no lib/pkgconfig). The engine
cannot be cross-compiled without the dependency sysroot. Cross file has been
generated at:
  ${CROSS}
Build the dependency stack for ${ABI}, point NORDSTJERNEN_ANDROID_SYSROOT at
the sysroot base or ABI prefix, and re-run. The APK will build with the stub
engine until then.
MSG
    exit 0
fi

BUILDDIR="${WORK}/builddir"
rm -rf "${BUILDDIR}"

export PKG_CONFIG_LIBDIR="${SYSROOT_PKGCONFIG}"
unset PKG_CONFIG_SYSROOT_DIR || true
unset PKG_CONFIG_PATH || true
echo "Using pkg-config: $(command -v pkg-config || echo missing)"
pkg-config --list-all | sort > "${LOGDIR}/pkg-config-${ABI}.txt" || true
echo "Wrote pkg-config module list: ${LOGDIR}/pkg-config-${ABI}.txt"

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

python3 - "${JNILIBS}/libnordstjernen.so" <<'PY'
import re
import sys

path = sys.argv[1]
data = bytearray(open(path, "rb").read())
for match in list(re.finditer(rb"[^\0]+", data)):
    raw = bytes(match.group(0))
    if b"nordstjernen-android-sysroot" not in raw and b"$ORIGIN" not in raw:
        continue
    if b".so" in raw:
        cut = max(raw.rfind(b"/"), raw.rfind(b"\\"))
        new = raw[cut + 1:] if cut >= 0 else raw
    else:
        new = b"$ORIGIN"
    if len(new) > len(raw):
        raise SystemExit(f"replacement longer than original in {path}: {raw!r}")
    data[match.start():match.end()] = new + (b"\0" * (len(raw) - len(new)))
open(path, "wb").write(data)
PY

for so in "${SYSROOT_PREFIX}"/lib/*.so; do
    [ -e "${so}" ] || continue
    cp -v "${so}" "${JNILIBS}/"
done

CMAKE_CACHE_ROOT="${REPO_ROOT}/android/app/.cxx"
if [ -d "${CMAKE_CACHE_ROOT}" ]; then
    find "${CMAKE_CACHE_ROOT}" -mindepth 3 -maxdepth 3 -type d -name "${ABI}" -exec rm -rf {} +
fi

CMAKE_OBJ_ROOT="${REPO_ROOT}/android/app/build/intermediates/cxx"
if [ -d "${CMAKE_OBJ_ROOT}" ]; then
    find "${CMAKE_OBJ_ROOT}" -path "*/obj/${ABI}" -type d -exec rm -rf {} +
fi

echo "Invalidated Android CMake cache for ${ABI}"
echo "Staged engine + deps into ${JNILIBS}"
