#!/usr/bin/env bash
# Build a portable Nordstjernen RPM by repackaging the bundle that
# pack-linux.sh produces. The binary is already statically linked
# against the in-tree engine (lexbor, gumbo, quickjs); the RPM only
# needs to declare the dynamic GTK / curl / rsvg system deps, and rpm's
# auto-Requires picks those up from the binary's SONAMEs.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
VERSION=${VERSION:-$(grep -E "^[[:space:]]*version" "$ROOT/meson.build" | head -1 \
          | sed -E "s/.*version: '([^']+)'.*/\1/")}
ARCH=$(uname -m)
FSVERSION=${VERSION//\~/-}
FSVERSION=${FSVERSION//\//-}
RPMVERSION=${VERSION//[!A-Za-z0-9.]/.}
SLUG="nordstjernen-${FSVERSION}-linux-${ARCH}"
STAGE="$ROOT/dist/${SLUG}"

if ! command -v rpmbuild >/dev/null 2>&1; then
    echo "rpmbuild not found. Install it first:" >&2
    echo "    sudo zypper install rpm-build       # openSUSE" >&2
    echo "    sudo dnf install rpm-build          # Fedora/RHEL" >&2
    echo "    sudo apt install rpm                # Debian/Ubuntu (provides rpmbuild)" >&2
    exit 1
fi

if [ ! -x "$STAGE/nordstjernen" ]; then
    echo "Bundle not staged yet — running pack-linux.sh first."
    "$ROOT/scripts/pack-linux.sh"
fi

RPMTOP="$ROOT/dist/rpmbuild"
rm -rf "$RPMTOP"
mkdir -p "$RPMTOP"/{BUILD,BUILDROOT,RPMS,SOURCES,SPECS,SRPMS}

cp "$STAGE/nordstjernen" "$RPMTOP/SOURCES/"
cp "$ROOT/data/icons/hicolor/scalable/apps/nordstjernen.svg" \
   "$RPMTOP/SOURCES/"
cp "$ROOT/data/nordstjernen.desktop" "$RPMTOP/SOURCES/"
cp "$ROOT/README.md" "$RPMTOP/SOURCES/"
cp "$ROOT/THIRD-PARTY-LICENSES.md" "$RPMTOP/SOURCES/"

SPEC="$RPMTOP/SPECS/nordstjernen.spec"
cat > "$SPEC" <<SPEC_EOF
Name:           nordstjernen
Version:        ${RPMVERSION}
Release:        1%{?dist}
Summary:        Nordstjernen Web Navigator — a small, hand-written web browser

License:        Proprietary
URL:            https://nordstjernen.org
BuildArch:      ${ARCH}

Source0:        nordstjernen
Source1:        nordstjernen.svg
Source2:        nordstjernen.desktop
Source3:        README.md
Source4:        THIRD-PARTY-LICENSES.md

AutoReqProv:    yes

%define debug_package %{nil}
%global __strip /bin/true
%global __os_install_post %{nil}

%description
Nordstjernen is a small, source-available web browser written in C with
GTK 4 and libcurl. The HTML parser, CSS engine, layout, paint and
JavaScript glue are written from scratch — no third-party browser
engine is used. SVG images are rendered with librsvg.

%prep
# nothing to prep; binary is prebuilt

%build
# nothing to build; binary is prebuilt

%install
rm -rf %{buildroot}
install -dm755 %{buildroot}%{_bindir}
install -dm755 %{buildroot}%{_datadir}/icons/hicolor/scalable/apps
install -dm755 %{buildroot}%{_datadir}/applications
install -dm755 %{buildroot}%{_docdir}/nordstjernen

install -m755 %{SOURCE0} %{buildroot}%{_bindir}/nordstjernen
install -m644 %{SOURCE1} %{buildroot}%{_datadir}/icons/hicolor/scalable/apps/nordstjernen.svg
install -m644 %{SOURCE2} %{buildroot}%{_datadir}/applications/nordstjernen.desktop
install -m644 %{SOURCE3} %{buildroot}%{_docdir}/nordstjernen/
install -m644 %{SOURCE4} %{buildroot}%{_docdir}/nordstjernen/

%files
%{_bindir}/nordstjernen
%{_datadir}/icons/hicolor/scalable/apps/nordstjernen.svg
%{_datadir}/applications/nordstjernen.desktop
%{_docdir}/nordstjernen/

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
* $(date "+%a %b %d %Y") Andreas Røsdal <andreas.rosdal@gmail.com> - ${RPMVERSION}-1
- Prebuilt binary repackaged as an RPM.
SPEC_EOF

rpmbuild --define "_topdir $RPMTOP" -bb "$SPEC"

RPMFILE=$(find "$RPMTOP/RPMS" -name "nordstjernen-${RPMVERSION}-*.rpm" -print -quit)
if [ -z "$RPMFILE" ]; then
    echo "rpmbuild produced no .rpm — see $RPMTOP/BUILD for details." >&2
    exit 1
fi

cp "$RPMFILE" "$ROOT/dist/"
FINAL="$ROOT/dist/$(basename "$RPMFILE")"

echo
echo "Built: $FINAL ($(du -h "$FINAL" | cut -f1))"
echo
echo "Inspect: rpm -qpi $FINAL"
echo "Deps:    rpm -qpR $FINAL"
echo "Install: sudo rpm -i $FINAL    (or: sudo dnf install $FINAL)"
