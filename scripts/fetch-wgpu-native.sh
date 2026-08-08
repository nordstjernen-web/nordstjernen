#!/usr/bin/env bash
# Fetch a pinned wgpu-native release for the host platform and print the
# extracted root (containing lib/ and include/webgpu/) on stdout, so a build
# can pass it to meson as -Dwgpu_native_root. Idempotent and cached; exits
# non-zero (without printing a root) when wgpu-native is unavailable for the
# platform or the download fails, so callers degrade to a WebGPU-free build.
set -uo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)

VERSION=${WGPU_NATIVE_VERSION:-v29.0.1.1}
BASEURL=${WGPU_NATIVE_BASEURL:-https://github.com/gfx-rs/wgpu-native/releases/download}
CACHE=${WGPU_NATIVE_CACHE:-$ROOT/third_party/wgpu-native/dl}

log() { printf '[fetch-wgpu-native] %s\n' "$*" >&2; }

os=$(uname -s 2>/dev/null || echo unknown)
arch=$(uname -m 2>/dev/null || echo unknown)

case "$arch" in
    x86_64|amd64)   arch=x86_64 ;;
    arm64|aarch64)  arch=aarch64 ;;
    *) log "unsupported arch '$arch'"; exit 2 ;;
esac

case "$os" in
    Linux)
        if [ -n "${WGPU_NATIVE_FORCE_GLIBC:-}" ]; then
            :
        elif ldd --version 2>&1 | grep -qi musl \
             || [ -e /lib/ld-musl-"$arch".so.1 ]; then
            log "musl libc detected; wgpu-native ships no musl build — skipping WebGPU"
            exit 2
        fi
        asset="wgpu-linux-${arch}-release.zip" ;;
    Darwin)
        asset="wgpu-macos-${arch}-release.zip" ;;
    MINGW*|MSYS*|CYGWIN*|Windows_NT)
        asset="wgpu-windows-${arch}-gnu-release.zip" ;;
    *) log "unsupported OS '$os'"; exit 2 ;;
esac

dest="$CACHE/$VERSION/${asset%.zip}"
if [ -f "$dest/lib/libwgpu_native.so" ] || \
   [ -f "$dest/lib/libwgpu_native.dylib" ] || \
   [ -f "$dest/lib/wgpu_native.dll" ] || \
   [ -f "$dest/lib/libwgpu_native.a" ]; then
    log "using cached $dest"
    printf '%s\n' "$dest"
    exit 0
fi

if ! command -v unzip >/dev/null 2>&1; then
    log "unzip not found; cannot extract wgpu-native release"
    exit 3
fi
fetch() {
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL --retry 3 -o "$1" "$2"
    elif command -v wget >/dev/null 2>&1; then
        wget -q -O "$1" "$2"
    else
        log "neither curl nor wget available"; return 3
    fi
}

url="$BASEURL/$VERSION/$asset"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
log "downloading $url"
if ! fetch "$tmp/wgpu.zip" "$url"; then
    log "download failed: $url"
    exit 3
fi
rm -rf "$dest"
mkdir -p "$dest"
if ! unzip -q -o "$tmp/wgpu.zip" -d "$dest"; then
    log "extract failed"
    rm -rf "$dest"
    exit 3
fi
if [ ! -e "$dest/include/webgpu/webgpu.h" ]; then
    log "extracted release missing include/webgpu/webgpu.h (unexpected layout)"
    rm -rf "$dest"
    exit 3
fi
log "extracted $VERSION -> $dest"
printf '%s\n' "$dest"
