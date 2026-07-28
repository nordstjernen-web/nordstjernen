#!/usr/bin/env bash
# Build the Java native libraries (engine + JNI bridge) inside a distro
# container so the nightly host needs no C toolchain or GTK stack. Invoked by
# nightly.sh via `docker run` with the source tree mounted at the working
# directory; installs the engine build deps plus a JDK, then runs
# java/scripts/build-native.sh, which stages the libraries into
# java/src/main/resources/native/<os>-<arch>/.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
export CC=${CC:-cc}
JDK_PKG=${NS_JDK_PKG:-openjdk-21-jdk-headless}

apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential clang pkg-config ninja-build cmake git zip \
    python3-pip ca-certificates "$JDK_PKG" \
    libgtk-4-dev libepoxy-dev libcurl4-openssl-dev libuchardet-dev libpsl-dev \
    libsqlite3-dev libseccomp-dev
apt-get install -y --no-install-recommends \
    libpoppler-glib-dev libfontconfig-dev libpango1.0-dev libavif-dev || true
# FFmpeg libav* enables inline WebM (VP9/VP8 + Opus/Vorbis); optional, like
# libavif. The native lib then expects the FFmpeg runtime on the host.
apt-get install -y --no-install-recommends \
    libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
    || echo "nightly-java-native: FFmpeg dev libs unavailable; WebM skipped" >&2
pip3 install --break-system-packages --upgrade 'meson>=1.4' \
    || pip3 install --upgrade 'meson>=1.4'

git config --global --add safe.directory "$(pwd)" || true

if [ -z "${JAVA_HOME:-}" ]; then
    JAVA_HOME=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")
fi
export JAVA_HOME

exec bash java/scripts/build-native.sh
