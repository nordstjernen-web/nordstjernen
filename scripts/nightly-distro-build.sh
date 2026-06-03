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
        build-essential clang pkg-config ninja-build cmake git zip \
        python3-pip dpkg-dev patchelf ca-certificates \
        libgtk-4-dev libcurl4-openssl-dev libuchardet-dev libpsl-dev \
        libsqlite3-dev librsvg2-dev libseccomp-dev
    apt-get install -y --no-install-recommends \
        libpoppler-glib-dev \
        libfontconfig-dev libpango1.0-dev libavif-dev || true
    pip3 install --break-system-packages --upgrade 'meson>=1.3' \
        || pip3 install --upgrade 'meson>=1.3'
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
        gcc gcc-c++ clang pkgconf-pkg-config meson ninja cmake git zip \
        rpm-build ca-certificates \
        gtk4-devel libcurl-devel libuchardet-devel libpsl-devel \
        sqlite3-devel librsvg-devel libseccomp-devel
    zypper --non-interactive --gpg-auto-import-keys install --no-recommends \
        libpoppler-glib-devel \
        fontconfig-devel pango-devel libavif-devel || true
}

install_apk() {
    apk update -q
    apk add --no-cache \
        build-base clang pkgconf meson ninja cmake git zip alpine-sdk \
        linux-headers gtk4.0-dev curl-dev uchardet-dev libpsl-dev sqlite-dev \
        librsvg-dev libseccomp-dev
    apk add --no-cache \
        poppler-dev \
        fontconfig-dev pango-dev libavif-dev || true
}

case "$DISTRO" in
    debian|ubuntu) install_apt ;;
    opensuse)      install_zypper ;;
    alpine)        install_apk ;;
    *) echo "unknown distro: $DISTRO" >&2; exit 2 ;;
esac

git config --global --add safe.directory "$(pwd)" || true

if [ -z "${ND_BUILD_JOBS:-}" ]; then
    mem_gb=$(awk '/MemTotal/{print int($2/1024/1024)}' /proc/meminfo)
    cores=$(nproc)
    jobs=$(( mem_gb / 2 ))
    [ "$jobs" -lt 2 ] && jobs=2
    [ "$jobs" -gt "$cores" ] && jobs=$cores
    export ND_BUILD_JOBS=$jobs
fi
export ND_BUILD_LTO=${ND_BUILD_LTO:-false}
echo "nightly-distro-build($DISTRO): building with -j${ND_BUILD_JOBS} lto=${ND_BUILD_LTO} (mem-bounded)"

./scripts/pack-linux.sh

case "$DISTRO" in
    debian|ubuntu) ./scripts/pack-deb.sh ;;
    opensuse)      ./scripts/pack-rpm.sh ;;
    alpine)        ./scripts/pack-apk.sh ;;
esac

echo
echo "nightly-distro-build($DISTRO): artifacts in dist/:"
ls -1 dist/*.zip dist/*.deb dist/*.rpm dist/*.apk 2>/dev/null || true
