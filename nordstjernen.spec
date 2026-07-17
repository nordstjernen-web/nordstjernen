#
# spec file for package nordstjernen
#
# Copyright 2026 Andreas Røsdal
#
# The Nordstjernen Source License v1.0 (NSL-1.0) is source-available but
# NOT an OSI-approved / free license: it forbids "Competing Use" and limits
# some purposes to non-commercial use. It is therefore tagged SUSE-NonFree
# and this package belongs in a non-free repository (a home: project or
# openSUSE's non-free area), never in openSUSE:Factory. NSL-1.0 converts to
# the MIT license ten years after each release.
#


Name:           nordstjernen
Version:        1.0.17
Release:        0
Summary:        Small, hand-written GTK web browser
License:        SUSE-NonFree
Group:          Productivity/Networking/Web/Browsers
URL:            https://nordstjernen.org

BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  meson >= 1.0
BuildRequires:  ninja
BuildRequires:  pkgconfig
BuildRequires:  update-desktop-files
BuildRequires:  pkgconfig(epoxy)
BuildRequires:  pkgconfig(enchant-2)
BuildRequires:  pkgconfig(gtk4)
BuildRequires:  pkgconfig(libcrypto)
BuildRequires:  pkgconfig(libcurl) >= 7.85
BuildRequires:  pkgconfig(libpsl)
BuildRequires:  pkgconfig(librsvg-2.0) >= 2.46
BuildRequires:  pkgconfig(libseccomp)
BuildRequires:  pkgconfig(libavif)
BuildRequires:  pkgconfig(libwebp)
BuildRequires:  pkgconfig(sdl2)
BuildRequires:  pkgconfig(sqlite3)
BuildRequires:  pkgconfig(uchardet)
Requires:       hicolor-icon-theme
Recommends:     mpv
Recommends:     myspell-en_US

%description
Nordstjernen is a clean-room web browser written from scratch in C, with a
GTK 4 user interface and a libcurl network stack. It is built to be small,
secure, and readable by a single person end to end.

  * A from-scratch HTML5, CSS, and JavaScript engine — no forked browser engine.
  * Each tab's engine runs in its own sandboxed process (seccomp + Landlock on
    Linux) behind an IPC and shared-memory framebuffer boundary.
  * No JIT, which keeps the JavaScript attack surface small.
  * Opt-in WebGL, gated by a per-site trust prompt.
  * No telemetry: it does not phone home and does not track the user.

%prep
%setup -q -c -T
top=$(find "%{_sourcedir}" -name meson.build 2>/dev/null \
      | awk '{ print length, $0 }' | sort -n | head -1 | cut -d' ' -f2-)
echo "DIAG: top meson.build = ${top:-<none>}"
echo "DIAG: %{_sourcedir} ="; ls -la "%{_sourcedir}"
if [ -z "$top" ]; then
    echo "DIAG: no meson.build under SOURCES; listing %{_builddir}:"
    ls -laR "%{_builddir}" | head -80
    exit 1
fi
cp -a "$(dirname "$top")"/. .
test -f meson.build

%build
%ifarch i386 i486 i586 i686
%global extra_meson -Dwasm=disabled
%endif
%meson \
    -Dai=disabled \
    -Dwebgpu=disabled \
    %{?extra_meson}
%meson_build

%install
%meson_install

# The GTK browser statically compiles the engine; the embedding shared
# library and its development header are only needed by external embedders,
# not the browser app. Drop them so the package is a clean application,
# not a -devel library.
rm -f %{buildroot}%{_libdir}/libnordstjernen.so
rm -f %{buildroot}%{_includedir}/nordstjernen/libnordstjernen.h
rmdir %{buildroot}%{_includedir}/nordstjernen 2>/dev/null || :

%suse_update_desktop_file org.nordstjernen.WebBrowser

%files
%doc README.md
%license %{_datadir}/nordstjernen/License.md
%{_bindir}/nordstjernen
%{_bindir}/nordstjernen-renderer
%{_bindir}/nordstjernen-audio
%{_datadir}/applications/org.nordstjernen.WebBrowser.desktop
%{_datadir}/metainfo/org.nordstjernen.WebBrowser.metainfo.xml
%{_datadir}/nordstjernen/
%{_datadir}/icons/hicolor/scalable/apps/nordstjernen.gif
%{_datadir}/icons/hicolor/scalable/apps/nordstjernen*.svg

%changelog
