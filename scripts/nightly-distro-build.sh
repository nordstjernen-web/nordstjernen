#!/usr/bin/env bash
# Build and package Nordstjernen inside a distro container. Invoked by
# nightly.sh via `docker run` with the source tree at the current
# directory; installs that distro's deps, builds a release binary, and
# emits a portable tarball plus a native package (.deb or .rpm) under
# dist/. Argument 1 selects the distro: debian | ubuntu | opensuse.
set -euo pipefail

DISTRO=${1:?usage: nightly-distro-build.sh <debian|ubuntu|opensuse>}
export VERSION=${VERSION:-}
export DEBIAN_FRONTEND=noninteractive
export CC=${CC:-cc}

trap 'rc=$?; echo "nightly-distro-build($DISTRO): FAILED (exit $rc) at line $LINENO: $BASH_COMMAND" >&2; exit $rc' ERR

install_apt() {
    apt-get update -qq
    apt-get install -y --no-install-recommends \
        build-essential clang pkg-config ninja-build cmake git zip unzip curl \
        python3-pip dpkg-dev patchelf ca-certificates \
        libgtk-4-dev libepoxy-dev libcurl4-openssl-dev libssl-dev libuchardet-dev libpsl-dev \
        libsqlite3-dev librsvg2-dev libseccomp-dev libwebp-dev libavif-dev libsdl2-dev
    apt-get install -y --no-install-recommends \
        libpoppler-glib-dev \
        libfontconfig-dev libpango1.0-dev libavif-dev || true
    # FFmpeg libav* enables the auto-detected inline WebM path (VP9/VP8 video +
    # Opus/Vorbis audio). Optional and on its own line so its absence skips
    # WebM rather than dropping the other optional dev packages.
    apt-get install -y --no-install-recommends \
        libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
        || echo "nightly-distro-build($DISTRO): FFmpeg dev libs unavailable; WebM skipped" >&2
    pip3 install --break-system-packages --upgrade 'meson>=1.4' \
        || pip3 install --upgrade 'meson>=1.4'
}

install_zypper() {
    # Tumbleweed is a rolling release and its mirrors are frequently caught
    # mid-sync, so a repo's repomd.xml can transiently reference metadata
    # files (e.g. appdata.xml.gz) not yet present on the mirror, making
    # `zypper refresh` fail. Retry a few times — the sync window resolves in
    # a minute or two and download.opensuse.org rotates mirrors on retry —
    # then fall through: `zypper install` auto-refreshes, so a still-stale
    # repo fails there with a real error instead of a single flake aborting
    # the whole nightly stage.
    local i
    for i in 1 2 3 4 5; do
        zypper --non-interactive --gpg-auto-import-keys refresh && break
        echo "zypper refresh attempt $i failed (mirror likely mid-sync); retrying in $((i * 15))s..." >&2
        sleep $((i * 15))
    done
    zypper --non-interactive --gpg-auto-import-keys install --no-recommends \
        gcc gcc-c++ clang pkgconf-pkg-config meson ninja cmake git zip unzip curl \
        rpm-build patchelf ca-certificates \
        gtk4-devel libepoxy-devel libcurl-devel libopenssl-devel libuchardet-devel libpsl-devel \
        sqlite3-devel librsvg-devel libseccomp-devel libwebp-devel libavif-devel
    # SDL2 backs the auto-detected audio helper; keep it out of the required
    # set so an unavailable/mid-sync package degrades to no audio, not a failed
    # nightly. Its own line (not the optional group) so it is independent of
    # poppler availability.
    zypper --non-interactive --gpg-auto-import-keys install --no-recommends \
        libSDL2-devel \
        || echo "nightly-distro-build(opensuse): SDL2 unavailable; audio helper skipped" >&2
    zypper --non-interactive --gpg-auto-import-keys install --no-recommends \
        libpoppler-glib-devel \
        fontconfig-devel pango-devel libavif-devel || true
    # FFmpeg libav* (inline WebM: VP9/VP8 + Opus/Vorbis). openSUSE ships these
    # via Packman, not the default repos, so this commonly degrades to no WebM.
    zypper --non-interactive --gpg-auto-import-keys install --no-recommends \
        libavformat-devel libavcodec-devel libavutil-devel libswscale-devel libswresample-devel \
        || echo "nightly-distro-build(opensuse): FFmpeg dev libs unavailable (Packman not enabled?); WebM skipped" >&2
}

install_apk() {
    apk update -q
    apk add --no-cache \
        build-base clang pkgconf meson ninja cmake git zip alpine-sdk \
        linux-headers gtk4.0-dev libepoxy-dev curl-dev openssl-dev uchardet-dev libpsl-dev sqlite-dev \
        librsvg-dev libseccomp-dev libwebp-dev sdl2-dev
    apk add --no-cache \
        poppler-dev \
        fontconfig-dev pango-dev libavif-dev || true
    # FFmpeg libav* (inline WebM: VP9/VP8 + Opus/Vorbis). ffmpeg-dev provides
    # all of libavformat/libavcodec/libavutil/libswscale/libswresample.
    apk add --no-cache ffmpeg-dev \
        || echo "nightly-distro-build(alpine): ffmpeg-dev unavailable; WebM skipped" >&2
}

case "$DISTRO" in
    debian|ubuntu) install_apt ;;
    opensuse)      install_zypper ;;
    alpine)        install_apk ;;
    *) echo "unknown distro: $DISTRO" >&2; exit 2 ;;
esac

git config --global --add safe.directory "$(pwd)" || true

if [ -z "${NS_BUILD_JOBS:-}" ]; then
    mem_gb=$(awk '/MemTotal/{print int($2/1024/1024)}' /proc/meminfo)
    cores=$(nproc)
    jobs=$(( mem_gb / 2 ))
    [ "$jobs" -lt 2 ] && jobs=2
    [ "$jobs" -gt "$cores" ] && jobs=$cores
    export NS_BUILD_JOBS=$jobs
fi
export NS_BUILD_LTO=${NS_BUILD_LTO:-false}
echo "nightly-distro-build($DISTRO): building with -j${NS_BUILD_JOBS} lto=${NS_BUILD_LTO} (mem-bounded)"

./scripts/pack-linux.sh

case "$DISTRO" in
    debian|ubuntu) ./scripts/pack-deb.sh ;;
    opensuse)      ./scripts/pack-rpm.sh ;;
    alpine)        ./scripts/pack-apk.sh ;;
esac

echo
echo "nightly-distro-build($DISTRO): artifacts in dist/:"
ls -1 dist/*.zip dist/*.deb dist/*.rpm dist/*.apk 2>/dev/null || true
