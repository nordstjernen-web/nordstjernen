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

apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential clang pkg-config ninja-build cmake git zip \
    python3-pip ca-certificates openjdk-21-jdk-headless \
    libgtk-4-dev libcurl4-openssl-dev libuchardet-dev libpsl-dev \
    libsqlite3-dev librsvg2-dev libseccomp-dev
apt-get install -y --no-install-recommends \
    libpoppler-glib-dev libfontconfig-dev libpango1.0-dev libavif-dev || true
pip3 install --break-system-packages --upgrade 'meson>=1.3' \
    || pip3 install --upgrade 'meson>=1.3'

git config --global --add safe.directory "$(pwd)" || true

if [ -z "${JAVA_HOME:-}" ]; then
    JAVA_HOME=$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")
fi
export JAVA_HOME

exec bash java/scripts/build-native.sh
