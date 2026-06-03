#!/usr/bin/env bash
# Build a source RPM (.src.rpm) of Nordstjernen. The SRPM contains a
# clean source tarball plus the meson wrap packagecache so rpmbuild can
# resolve the remaining wrap-based subproject (Wuffs) offline when the
# SRPM is rebuilt with `rpmbuild --rebuild`.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(grep -E "^[[:space:]]*version" "$ROOT/meson.build" | head -1 \
          | sed -E "s/.*version: '([^']+)'.*/\1/")
NAME=nordstjernen
SLUG="${NAME}-${VERSION}"

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
Version:        ${VERSION}
Release:        1%{?dist}
Summary:        Nordstjernen Web Navigator — a small, hand-written web browser

License:        Proprietary
URL:            https://nordstjernen.org
Source0:        ${SLUG}.tar.gz

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  meson >= 1.0
BuildRequires:  ninja-build
BuildRequires:  cmake
BuildRequires:  pkgconf-pkg-config
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(libcurl)
BuildRequires:  pkgconfig(uchardet)
BuildRequires:  pkgconfig(librsvg-2.0)
BuildRequires:  pkgconfig(libpsl)
BuildRequires:  pkgconfig(libseccomp)

Requires:       gtk4
Requires:       libcurl
Requires:       uchardet
Requires:       librsvg2

%description
Nordstjernen is a small, source-available web browser written in C with
GTK 4 and libcurl. The HTML parser, CSS engine, layout, paint and
JavaScript glue are written from scratch — no third-party browser
engine is used. SVG images are rendered with librsvg.

%prep
%setup -q

%build
meson setup builddir \\
    --prefix=%{_prefix} \\
    --libdir=%{_libdir} \\
    --buildtype=release \\
    -Db_lto=true \\
    -Db_ndebug=true \\
    --wrap-mode=default
meson compile -C builddir

%install
rm -rf %{buildroot}
install -dm755 %{buildroot}%{_bindir}
install -dm755 %{buildroot}%{_datadir}/icons/hicolor/scalable/apps
install -dm755 %{buildroot}%{_datadir}/applications
install -dm755 %{buildroot}%{_docdir}/%{name}

install -m755 builddir/src/nordstjernen %{buildroot}%{_bindir}/nordstjernen
install -m644 data/icons/hicolor/scalable/apps/nordstjernen.svg \\
    %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/nordstjernen.svg
install -m644 data/nordstjernen.desktop \\
    %{buildroot}%{_datadir}/applications/nordstjernen.desktop
install -m644 README.md %{buildroot}%{_docdir}/%{name}/
install -m644 THIRD-PARTY-LICENSES.md %{buildroot}%{_docdir}/%{name}/
install -m644 License.md %{buildroot}%{_docdir}/%{name}/

%files
%{_bindir}/nordstjernen
%{_datadir}/icons/hicolor/scalable/apps/nordstjernen.svg
%{_datadir}/applications/nordstjernen.desktop
%doc %{_docdir}/%{name}/README.md
%doc %{_docdir}/%{name}/THIRD-PARTY-LICENSES.md
%license %{_docdir}/%{name}/License.md

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
* $(LC_ALL=C date "+%a %b %d %Y") Andreas Røsdal <andreas.rosdal@gmail.com> - ${VERSION}-1
- Source RPM release of Nordstjernen ${VERSION}.
SPEC_EOF

rpmbuild --define "_topdir $RPMTOP" -bs "$SPEC"

SRPM=$(find "$RPMTOP/SRPMS" -name "${NAME}-${VERSION}-*.src.rpm" | head -1)
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
