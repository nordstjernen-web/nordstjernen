#!/usr/bin/env bash
# Build a source RPM (.src.rpm) of Nordstjernen. Every subproject the build
# needs (Wuffs, pl_mpeg) is vendored in the tarball, and the one remaining
# wrap — ns-pango, cloned with git — is switched off in the spec, so
# `rpmbuild --rebuild` resolves offline against the system Pango.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(grep -E "^[[:space:]]*version" "$ROOT/meson.build" | head -1 \
          | sed -E "s/.*version: '([^']+)'.*/\1/")
FSVERSION=${VERSION//\~/-}
FSVERSION=${FSVERSION//\//-}
RPMVERSION=${VERSION//[!A-Za-z0-9.]/.}
NAME=nordstjernen
SLUG="${NAME}-${FSVERSION}"

if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "rpmbuild not found. Install it first:" >&2
    echo "    sudo zypper install rpm-build       # openSUSE" >&2
    echo "    sudo dnf install rpm-build          # Fedora/RHEL" >&2
    echo "    sudo apt install rpm                # Debian/Ubuntu" >&2
    exit 1
fi

RPMTOP="$ROOT/dist/rpmbuild-src"
rm -rf "$RPMTOP"
mkdir -p "$RPMTOP"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

TARBALL="$RPMTOP/SOURCES/${SLUG}.tar.gz"
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

git -C "$ROOT" archive --format=tar --prefix="${SLUG}/" HEAD \
    | tar -x -C "$WORK"

if [ -d "$ROOT/subprojects/packagecache" ]; then
    mkdir -p "$WORK/${SLUG}/subprojects/packagecache"
    cp -a "$ROOT/subprojects/packagecache/." \
          "$WORK/${SLUG}/subprojects/packagecache/"
fi

tar -czf "$TARBALL" -C "$WORK" "${SLUG}"

SPEC="$RPMTOP/SPECS/${NAME}.spec"
cat > "$SPEC" <<SPEC_EOF
Name:           ${NAME}
Version:        ${RPMVERSION}
Release:        1%{?dist}
Summary:        Nordstjernen Web Navigator — a small, hand-written web browser

License:        LicenseRef-NSL-1.0
URL:            https://nordstjernen.org
Source0:        ${SLUG}.tar.gz

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  meson >= 1.0
BuildRequires:  ninja-build
BuildRequires:  cmake
BuildRequires:  pkgconf-pkg-config
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(epoxy)
BuildRequires:  pkgconfig(libcurl)
BuildRequires:  pkgconfig(libcrypto)
BuildRequires:  pkgconfig(uchardet)
BuildRequires:  pkgconfig(libpsl)
BuildRequires:  pkgconfig(libseccomp)
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  pkgconfig(libwebp)
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  pkgconfig(libavcodec) >= 60
BuildRequires:  pkgconfig(libavformat) >= 60
BuildRequires:  pkgconfig(libavutil) >= 58
BuildRequires:  pkgconfig(libswresample) >= 4
BuildRequires:  pkgconfig(libswscale) >= 7
BuildRequires:  pkgconfig(fontconfig)
BuildRequires:  pkgconfig(pango)
BuildRequires:  pkgconfig(pangocairo)
BuildRequires:  pkgconfig(pangoft2)

Requires:       gtk4
Requires:       libcurl
Requires:       uchardet

%description
Nordstjernen is a small, source-available web browser written in C with
GTK 4 and libcurl. The HTML parser, CSS engine, layout, paint and
JavaScript glue are written from scratch — no third-party browser
engine is used. SVG images are rendered in-engine.

%prep
%setup -q -n ${SLUG}

%build
# A rebuild has no network: the vendored subprojects are in the tarball and
# ns-pango, the one git wrap, gives way to the system Pango.
meson setup builddir \\
    --prefix=%{_prefix} \\
    --libdir=%{_libdir} \\
    --buildtype=release \\
    -Db_lto=true \\
    -Db_ndebug=true \\
    -Dns-pango=disabled \\
    -Dwebgpu=disabled \\
    --wrap-mode=nodownload
meson compile -C builddir

%install
rm -rf %{buildroot}
DESTDIR=%{buildroot} meson install --no-rebuild -C builddir

# The browser statically compiles the engine; the embedding shared library
# and its header serve external embedders only.
rm -f %{buildroot}%{_libdir}/libnordstjernen.so
rm -f %{buildroot}%{_includedir}/nordstjernen/libnordstjernen.h
rmdir %{buildroot}%{_includedir}/nordstjernen 2>/dev/null || :

%files
%license %{_datadir}/%{name}/License.md
%doc README.md
%{_bindir}/nordstjernen
%{_bindir}/nordstjernen-renderer
%{_bindir}/nordstjernen-audio
%{_bindir}/nordstjernen-video
%{_datadir}/applications/org.nordstjernen.WebBrowser.desktop
%{_datadir}/metainfo/org.nordstjernen.WebBrowser.metainfo.xml
%{_datadir}/%{name}/
%{_datadir}/icons/hicolor/scalable/apps/nordstjernen.gif
%{_datadir}/icons/hicolor/scalable/apps/nordstjernen*.svg

%post
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t %{_datadir}/icons/hicolor || :
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q %{_datadir}/applications || :
fi

%postun
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -q -t %{_datadir}/icons/hicolor || :
fi
if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database -q %{_datadir}/applications || :
fi

%changelog
* $(LC_ALL=C date "+%a %b %d %Y") Andreas Røsdal <andreas.rosdal@gmail.com> - ${RPMVERSION}-1
- Source RPM release of Nordstjernen ${VERSION}.
SPEC_EOF

rpmbuild --define "_topdir $RPMTOP" -bs "$SPEC"

SRPM=$(find "$RPMTOP/SRPMS" -name "${NAME}-${RPMVERSION}-*.src.rpm" | head -1)
if [ -z "$SRPM" ]; then
    echo "rpmbuild produced no .src.rpm — see $RPMTOP for details." >&2
    exit 1
fi

cp "$SRPM" "$ROOT/dist/"
FINAL="$ROOT/dist/$(basename "$SRPM")"

echo
echo "Built: $FINAL ($(du -h "$FINAL" | cut -f1))"
echo
echo "Inspect: rpm -qpi $FINAL"
echo "Sources: rpm -qpl $FINAL"
echo "Rebuild: rpmbuild --rebuild $FINAL"
